#include "drogon_pay/PayPlugin.h"
#include "services/PaymentService.h"
#include "services/RefundService.h"
#include "services/CallbackService.h"
#include "services/ReconciliationService.h"
#include "services/IdempotencyService.h"
#include "channels/WechatChannel.h"
#include "channels/AlipayChannel.h"
#include "handlers/PayHandlers.h"
#include "handlers/CallbackHandlers.h"
#include "handlers/PayMetricsHandlers.h"
#include "handlers/AuthCheck.h"
#include <drogon/drogon.h>
#include <future>
#include <stdexcept>

PayPlugin::PayPlugin() = default;
PayPlugin::~PayPlugin() = default;

namespace drogon_pay
{
// Referencing PayPlugin from a function with external linkage forces the
// linker to keep this object file (and with it the DrObject registration)
// when drogon-pay is consumed as a static library.
void ensureLinked()
{
    static PayPlugin *volatile anchor = nullptr;
    (void)anchor;
}
}  // namespace drogon_pay

namespace
{
// Breaking change (v1.0): the old per-channel top-level config blocks were
// replaced by the "channels" map. Detect them and refuse to start with a
// clear migration message instead of silently ignoring credentials.
const std::map<std::string, std::string> kLegacyKeyMigration = {
  {"wechat_pay", "channels.wechat"},
  {"alipay_sandbox", "channels.alipay"},
};
}  // namespace

void PayPlugin::registerBuiltinChannels(const Json::Value &channelsConfig)
{
    // Built-in channels are registered through this explicit call (no
    // self-registration macros: static-library builds would drop the symbols).
    const Json::Value &wechatCfg = channelsConfig["wechat"];
    if (wechatCfg.isObject() && wechatCfg.get("enabled", true).asBool())
    {
        try
        {
            registry_.add("wechat", std::make_shared<WechatPayClient>(wechatCfg));
            LOG_INFO << "WeChat channel registered";
        }
        catch (const std::exception &e)
        {
            LOG_ERROR << "Failed to create WeChat channel: " << e.what();
        }
    }

    const Json::Value &alipayCfg = channelsConfig["alipay"];
    if (alipayCfg.isObject() && alipayCfg.get("enabled", true).asBool())
    {
        try
        {
            registry_.add("alipay", std::make_shared<AlipaySandboxClient>(alipayCfg));
            LOG_INFO << "Alipay channel registered";
        }
        catch (const std::exception &e)
        {
            LOG_ERROR << "Failed to create Alipay channel: " << e.what();
        }
    }
}

std::map<std::string, drogon_pay::PaymentChannelPtr> PayPlugin::channelMap() const
{
    std::map<std::string, drogon_pay::PaymentChannelPtr> channels;
    for (const auto &name : registry_.names())
    {
        channels[name] = registry_.find(name);
    }
    return channels;
}

void PayPlugin::initAndStart(const Json::Value &config)
{
    LOG_INFO << "Initializing PayPlugin...";

    // 0. Reject the pre-1.0 config schema with an actionable migration error.
    for (const auto &[oldKey, newKey] : kLegacyKeyMigration)
    {
        if (config.isMember(oldKey))
        {
            LOG_ERROR << "PayPlugin config uses removed key '" << oldKey
                      << "'. Move this block to '" << newKey
                      << "' (see docs/development/plugin_integration.md for the full "
                      << "old->new key mapping). Refusing to start.";
            throw std::runtime_error("PayPlugin: legacy config key '" + oldKey + "' detected");
        }
    }

    // 1. Get infrastructure (client names configurable for multi-DB hosts)
    const std::string dbClientName = config.get("db_client", "default").asString();
    basePath_ = config.get("base_path", "/api/pay").asString();

    dbClient_ = drogon::app().getDbClient(dbClientName);
    if (!dbClient_)
    {
        LOG_ERROR << "Failed to get database client '" << dbClientName << "'";
        return;
    }

    // Redis is opt-in: only look the client up when the host explicitly sets
    // 'redis_client'. Calling getRedisClient() for a name that was never
    // configured corrupts drogon's RedisClientManager map (operator[] inserts
    // a null entry that its destructor dereferences -> crash on shutdown).
    if (config.isMember("redis_client"))
    {
        redisClient_ = drogon::app().getRedisClient(config["redis_client"].asString());
    }
    if (!redisClient_)
    {
        LOG_WARN << "Redis client not configured, idempotency will be database-only";
    }

    // 2. Assemble channels: built-ins first, then host-registered factories.
    const Json::Value &channelsConfig = config["channels"];
    if (!channelsConfig.isObject())
    {
        LOG_ERROR << "Missing 'channels' config block; no payment channel will be available";
    }
    else
    {
        registerBuiltinChannels(channelsConfig);
        for (const auto &[name, factory] : drogon_pay::ChannelRegistry::factories())
        {
            const Json::Value &channelCfg = channelsConfig[name];
            if (!channelCfg.isObject() || !channelCfg.get("enabled", true).asBool())
            {
                continue;
            }
            if (registry_.find(name))
            {
                LOG_WARN << "Custom channel factory '" << name
                         << "' shadows a built-in channel; skipping";
                continue;
            }
            try
            {
                registry_.add(name, factory(channelCfg));
                LOG_INFO << "Custom channel registered: " << name;
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "Failed to create custom channel '" << name << "': " << e.what();
            }
        }
    }
    registry_.freeze();
    {
        std::string names;
        for (const auto &name : registry_.names())
        {
            names += (names.empty() ? "" : ", ") + name;
        }
        LOG_INFO << "Registered payment channels: [" << names << "]";
    }

    // 3. Create IdempotencyService (no dependencies)
    int64_t idempotencyTtl = 604800;  // 7 days default
    if (config.isMember("idempotency_ttl_seconds") && config["idempotency_ttl_seconds"].isInt64())
    {
        idempotencyTtl = config["idempotency_ttl_seconds"].asInt64();
    }
    else
    {
        LOG_WARN << "'idempotency_ttl_seconds' not set, using default " << idempotencyTtl << "s";
    }
    idempotencyService_ =
      std::make_shared<IdempotencyService>(dbClient_, redisClient_, idempotencyTtl);
    LOG_INFO << "IdempotencyService created";

    // 4. Create business services (channel-agnostic; all services are fully
    //    constructed here and immutable afterwards - no lazy init races).
    auto channels = channelMap();
    paymentService_ =
      std::make_shared<PaymentService>(channels, dbClient_, redisClient_, idempotencyService_);
    LOG_INFO << "PaymentService created";

    refundService_ = std::make_shared<RefundService>(channels, dbClient_, idempotencyService_);
    LOG_INFO << "RefundService created";

    if (auto wechatChannel = registry_.find("wechat"))
    {
        callbackService_ =
          std::make_shared<CallbackService>(wechatChannel, dbClient_, redisClient_);
        LOG_INFO << "CallbackService created";
    }
    else
    {
        LOG_WARN << "WeChat channel not registered; CallbackService disabled";
    }

    // 5. Maintenance timers live on a dedicated worker loop so reconcile
    //    sweeps and certificate refresh never block IO loops.
    workerLoopThread_ = std::make_unique<trantor::EventLoopThread>("PayPluginWorker");
    workerLoopThread_->run();
    auto *workerLoop = workerLoopThread_->getLoop();

    reconciliationService_ =
      std::make_unique<ReconciliationService>(paymentService_, refundService_, channels, dbClient_);
    const Json::Value &reconcileCfg = config["reconcile"];
    if (reconcileCfg.isObject())
    {
        reconciliationService_->setReconcileOptions(
          reconcileCfg.get("interval_seconds", 0).asInt(), reconcileCfg.get("batch_size", 0).asInt()
        );
    }
    if (!reconcileCfg.isObject() || reconcileCfg.get("enabled", true).asBool())
    {
        reconciliationService_->startReconcileTimer(workerLoop);
        LOG_INFO << "ReconciliationService created and timer started";
    }
    else
    {
        LOG_INFO << "ReconciliationService created (timer disabled by config)";
    }

    // 6. Channel lifecycle hooks (e.g. WechatPayClient::onStart warms up the
    //    platform certificates) + periodic certificate refresh.
    workerLoop->runInLoop([this]() { registry_.startAll(); });
    double certRefreshSeconds = 43200.0;
    const Json::Value wechatCfg =
      config.get("channels", Json::Value()).get("wechat", Json::Value());
    if (wechatCfg.isMember("cert_refresh_interval_seconds"))
    {
        const int configured = wechatCfg.get("cert_refresh_interval_seconds", 43200).asInt();
        // Floor of five minutes: this timer signs and sends a request to
        // api.mch.weixin.qq.com per tick, and WeChat rate-limits the merchant
        // account, so an aggressive value hurts the integration it serves.
        if (configured >= 300)
        {
            certRefreshSeconds = static_cast<double>(configured);
        }
        else
        {
            LOG_WARN << "'cert_refresh_interval_seconds' must be >= 300, using "
                     << certRefreshSeconds << "s";
        }
    }
    startCertRefreshTimer(workerLoop, certRefreshSeconds);

    // 7. Register HTTP routes programmatically (ADD_METHOD_TO static
    //    registration is gone: static-library builds drop those symbols).
    registerHttpHandlers();

    LOG_INFO << "PayPlugin initialization complete";
}

void PayPlugin::registerHttpHandlers()
{
    payController_ = std::make_shared<PayController>();
    wechatCallbackController_ = std::make_shared<WechatCallbackController>();
    alipayCallbackController_ = std::make_shared<AlipayCallbackController>();
    payMetricsController_ = std::make_shared<PayMetricsController>();

    // Wraps a handler member function with the checkAuth() precheck that
    // replaced the old PayAuthFilter (null result = authorized).
    const auto authed = [this](auto controller, auto memFn) {
        return [this, controller, memFn](
                 const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb
               ) {
            if (auto resp = drogon_pay::checkAuth(req, basePath_))
            {
                cb(resp);
                return;
            }
            ((*controller).*memFn)(req, std::move(cb));
        };
    };
    const auto open = [](auto controller, auto memFn) {
        return [controller, memFn](
                 const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb
               ) { ((*controller).*memFn)(req, std::move(cb)); };
    };

    auto &app = drogon::app();
    app.registerHandler(
      basePath_ + "/create",
      authed(payController_, &PayController::createPayment),
      {drogon::Post, drogon::Options}
    );
    // Historical QR path lives outside the /api/pay prefix; keep it verbatim
    // for the default base path so existing frontends stay unaffected.
    const std::string qrPath =
      basePath_ == "/api/pay" ? std::string("/api/qrpay/create") : basePath_ + "/qrpay/create";
    app.registerHandler(
      qrPath,
      authed(payController_, &PayController::createQRPayment),
      {drogon::Post, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/query",
      authed(payController_, &PayController::queryOrder),
      {drogon::Get, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/refund",
      authed(payController_, &PayController::refund),
      {drogon::Post, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/refund/query",
      authed(payController_, &PayController::queryRefund),
      {drogon::Get, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/orders",
      authed(payController_, &PayController::queryOrderList),
      {drogon::Get, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/reconcile/summary",
      authed(payController_, &PayController::reconcileSummary),
      {drogon::Get, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/metrics/auth",
      authed(payMetricsController_, &PayMetricsController::authMetrics),
      {drogon::Get, drogon::Options}
    );
    app.registerHandler(
      basePath_ + "/metrics/auth.prom",
      authed(payMetricsController_, &PayMetricsController::authMetricsProm),
      {drogon::Get, drogon::Options}
    );

    // Channel callbacks authenticate via signature verification, not API keys.
    app.registerHandler(
      basePath_ + "/notify/wechat",
      open(wechatCallbackController_, &WechatCallbackController::notify),
      {drogon::Post}
    );
    app.registerHandler(
      basePath_ + "/notify/alipay",
      open(alipayCallbackController_, &AlipayCallbackController::notify),
      {drogon::Post}
    );

    LOG_INFO << "PayPlugin routes registered under base path '" << basePath_ << "' (QR create: '"
             << qrPath << "')";
}

void PayPlugin::shutdown()
{
    LOG_INFO << "Shutting down PayPlugin...";

    // Stop reconciliation timer
    if (reconciliationService_)
    {
        reconciliationService_->stopReconcileTimer();
    }

    if (workerLoopThread_)
    {
        auto *workerLoop = workerLoopThread_->getLoop();
        // Stop certificate refresh timer
        if (certRefreshTimerId_)
        {
            workerLoop->invalidateTimer(certRefreshTimerId_);
            certRefreshTimerId_ = 0;
        }
        registry_.stopAll();
        // Drain in-flight worker tasks before tearing the thread down.
        std::promise<void> drained;
        workerLoop->runInLoop([&drained]() { drained.set_value(); });
        drained.get_future().wait();
        workerLoopThread_.reset();
    }
    else
    {
        registry_.stopAll();
    }

    LOG_INFO << "PayPlugin shutdown complete";
}

std::shared_ptr<PaymentService> PayPlugin::paymentService()
{
    return paymentService_;
}

std::shared_ptr<RefundService> PayPlugin::refundService()
{
    return refundService_;
}

std::shared_ptr<CallbackService> PayPlugin::callbackService()
{
    // Constructed in initAndStart (or setTestChannels) and immutable after;
    // may be null when the wechat channel is not registered.
    return callbackService_;
}

std::shared_ptr<IdempotencyService> PayPlugin::idempotencyService()
{
    return idempotencyService_;
}

drogon_pay::PaymentChannelPtr PayPlugin::channel(const std::string &name) const
{
    return registry_.find(name);
}

std::shared_ptr<AlipaySandboxClient> PayPlugin::alipayClient()
{
    // SPI whitelist: concrete type needed for alipay-specific verify helpers.
    return std::dynamic_pointer_cast<AlipaySandboxClient>(registry_.find("alipay"));
}

void PayPlugin::setTestChannels(
  std::map<std::string, drogon_pay::PaymentChannelPtr> channels,
  std::shared_ptr<drogon::orm::DbClient> dbClient
)
{
    LOG_DEBUG << "PayPlugin::setTestChannels called for testing";

    dbClient_ = dbClient;
    for (const auto &[name, channelPtr] : channels)
    {
        registry_.add(name, channelPtr);
    }
    registry_.freeze();

    // Create IdempotencyService with test clients (no Redis for tests)
    idempotencyService_ = std::make_shared<IdempotencyService>(dbClient_, nullptr, 604800);

    // Create business services with test channels
    paymentService_ =
      std::make_shared<PaymentService>(channels, dbClient_, nullptr, idempotencyService_);

    refundService_ = std::make_shared<RefundService>(channels, dbClient_, idempotencyService_);

    // Always construct CallbackService so the accessor never returns null
    // after test initialization; a missing wechat channel exercises the
    // service's own "wechat client not ready" branch.
    auto it = channels.find("wechat");
    auto wechatChannel = (it != channels.end()) ? it->second : drogon_pay::PaymentChannelPtr{};
    callbackService_ = std::make_shared<CallbackService>(wechatChannel, dbClient_, nullptr);

    // Note: ReconciliationService is NOT created for tests
    // (it would start background timers)
}

void PayPlugin::setTestClients(
  std::shared_ptr<WechatPayClient> wechatClient,
  std::shared_ptr<AlipaySandboxClient> alipayClient,
  std::shared_ptr<drogon::orm::DbClient> dbClient
)
{
    std::map<std::string, drogon_pay::PaymentChannelPtr> channels;
    if (wechatClient)
    {
        channels["wechat"] = wechatClient;
    }
    if (alipayClient)
    {
        channels["alipay"] = alipayClient;
    }
    setTestChannels(std::move(channels), std::move(dbClient));
}

void PayPlugin::startCertRefreshTimer(trantor::EventLoop *loop, double intervalSeconds)
{
    // SPI whitelist: periodic certificate refresh is a wechat-only capability;
    // the initial download happens in WechatPayClient::onStart().
    auto wechatClient = std::dynamic_pointer_cast<WechatPayClient>(registry_.find("wechat"));
    if (!wechatClient || !loop)
    {
        return;
    }

    // Set up periodic refresh (channels.wechat.cert_refresh_interval_seconds,
    // 12 hours by default)
    certRefreshTimerId_ = loop->runEvery(intervalSeconds, [wechatClient]() {
        wechatClient->downloadCertificates([](const Json::Value &, const std::string &err) {
            if (!err.empty())
            {
                LOG_WARN << "Wechat certificate refresh failed: " << err;
            }
            else
            {
                LOG_INFO << "Wechat certificates refreshed";
            }
        });
    });
}
