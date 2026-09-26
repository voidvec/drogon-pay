#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <map>
#include <memory>
#include <string>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Date.h>
#include "PaymentChannel.h"
#include "ChannelRegistry.h"

namespace drogon_pay
{
// Static-library linking safety net: hosts whose linker drops the PayPlugin
// DrObject auto-registration symbol (getPlugin<PayPlugin>() returns null)
// can call this from main() to force the object file to be kept.
void ensureLinked();
}  // namespace drogon_pay

// Forward declarations (public header must not include internal src/ headers)
class PaymentService;
class RefundService;
class CallbackService;
class IdempotencyService;
class ReconciliationService;
class WechatPayClient;
class AlipaySandboxClient;
class PayController;
class WechatCallbackController;
class AlipayCallbackController;
class PayMetricsController;

class PayPlugin : public drogon::Plugin<PayPlugin>
{
  public:
    // Out-of-line ctor/dtor: members are unique_ptr/shared_ptr of forward-
    // declared internal types that are only complete inside src/PayPlugin.cc.
    PayPlugin();
    ~PayPlugin() override;
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    // Service accessors. All services are fully constructed inside
    // initAndStart() and are immutable afterwards (no lazy initialization -
    // the previous callbackService() lazy-init was a data race).
    std::shared_ptr<PaymentService> paymentService();
    std::shared_ptr<RefundService> refundService();
    std::shared_ptr<CallbackService> callbackService();
    std::shared_ptr<IdempotencyService> idempotencyService();

    // Channel accessors
    drogon_pay::PaymentChannelPtr channel(const std::string &name) const;

    // SPI whitelist exception: AlipayCallbackController needs the concrete
    // client for channel-specific callback verification helpers.
    std::shared_ptr<AlipaySandboxClient> alipayClient();

    // Configured route prefix for the pay API (default "/api/pay").
    const std::string &basePath() const
    {
        return basePath_;
    }

    // Test support: initialize services with test channels.
    // NOTE: This method is for integration testing only.
    void setTestChannels(
      std::map<std::string, drogon_pay::PaymentChannelPtr> channels,
      std::shared_ptr<drogon::orm::DbClient> dbClient
    );

    // Legacy adapter kept so existing tests keep compiling; wraps the clients
    // into a channel map and delegates to setTestChannels().
    void setTestClients(
      std::shared_ptr<WechatPayClient> wechatClient,
      std::shared_ptr<AlipaySandboxClient> alipayClient,
      std::shared_ptr<drogon::orm::DbClient> dbClient
    );

  private:
    // Channels (frozen after initAndStart)
    drogon_pay::ChannelRegistry registry_;

    // Services
    std::shared_ptr<PaymentService> paymentService_;
    std::shared_ptr<RefundService> refundService_;
    std::shared_ptr<CallbackService> callbackService_;
    std::unique_ptr<ReconciliationService> reconciliationService_;
    std::shared_ptr<IdempotencyService> idempotencyService_;

    // Infrastructure
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    drogon::nosql::RedisClientPtr redisClient_;
    std::string basePath_{"/api/pay"};

    // HTTP handler objects (plain classes, no HttpController macros); routes
    // are registered programmatically in registerHttpHandlers().
    std::shared_ptr<PayController> payController_;
    std::shared_ptr<WechatCallbackController> wechatCallbackController_;
    std::shared_ptr<AlipayCallbackController> alipayCallbackController_;
    std::shared_ptr<PayMetricsController> payMetricsController_;

    // Dedicated worker loop for maintenance timers (reconcile sweeps,
    // certificate refresh) so they never compete with IO loops.
    std::unique_ptr<trantor::EventLoopThread> workerLoopThread_;
    trantor::TimerId certRefreshTimerId_{0};

    void registerBuiltinChannels(const Json::Value &channelsConfig);
    std::map<std::string, drogon_pay::PaymentChannelPtr> channelMap() const;
    void registerHttpHandlers();
    void startCertRefreshTimer(trantor::EventLoop *loop, double intervalSeconds);
};
