/// =============================================================================
/// @file   CallbackControllerTest.cc
/// @brief  P0 controller-level validation tests (C2-1 fix and C1-2 fix).
///
/// Calls WechatCallbackController::notify() / AlipayCallbackController::notify()
/// directly with crafted HTTP requests. PayPlugin is auto-registered via
/// config.json → app().loadConfig() in tests/main.cc, so the controller can
/// resolve the plugin via drogon::app().getPlugin<PayPlugin>().
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <chrono>
#include <future>

#include "handlers/CallbackHandlers.h"
#include "channels/AlipayChannel.h"
#include "services/PaymentService.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

struct CtrlResult
{
    drogon::HttpStatusCode status{drogon::k500InternalServerError};
    Json::Value body;
    std::string bodyText;
    bool isJson{false};
    bool called{false};
};

// ---- Local Alipay keypair fixture -------------------------------------------
// processAlipayNotify is exercised with a locally-built client + service so the
// success/seller_id paths can be pinned without the frozen global registry or a
// database. The service is constructed with a null DbClient: for TRADE_SUCCESS
// syncOrderStatusFromAlipay then returns the mapped order status synchronously,
// which is exactly the branch that reaches the acknowledgement.
struct KeyFiles
{
    std::string privatePath;
    std::string publicPath;
    EVP_PKEY *pkey = nullptr;

    KeyFiles() = default;

    // RAII cleanup survives an early REQUIRE return (Drogon REQUIRE aborts the
    // test on failure), so the key and temp PEMs never leak.
    ~KeyFiles()
    {
        if (pkey)
        {
            EVP_PKEY_free(pkey);
        }
        std::error_code ec;
        if (!privatePath.empty())
        {
            std::filesystem::remove(privatePath, ec);
        }
        if (!publicPath.empty())
        {
            std::filesystem::remove(publicPath, ec);
        }
    }

    KeyFiles(const KeyFiles &) = delete;
    KeyFiles &operator=(const KeyFiles &) = delete;

    KeyFiles(KeyFiles &&other) noexcept
        : privatePath(std::move(other.privatePath)),
          publicPath(std::move(other.publicPath)),
          pkey(other.pkey)
    {
        other.pkey = nullptr;
    }

    KeyFiles &operator=(KeyFiles &&other) noexcept
    {
        if (this != &other)
        {
            if (pkey)
            {
                EVP_PKEY_free(pkey);
            }
            privatePath = std::move(other.privatePath);
            publicPath = std::move(other.publicPath);
            pkey = other.pkey;
            other.pkey = nullptr;
        }
        return *this;
    }
};

KeyFiles makeTempKeypair()
{
    KeyFiles kf;
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    if (pkey == nullptr)
    {
        return kf;
    }
    kf.pkey = pkey;

    auto dir = std::filesystem::temp_directory_path();
    static int invocation = 0;
    const std::string tag =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
      std::to_string(invocation++);
    kf.privatePath = (dir / ("alipay_ctrl_" + tag + "_private.pem")).string();
    kf.publicPath = (dir / ("alipay_ctrl_" + tag + "_public.pem")).string();

    {
        BIO *bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        BUF_MEM *mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        std::ofstream f(kf.privatePath, std::ios::binary);
        f.write(mem->data, static_cast<std::streamsize>(mem->length));
        BIO_free(bio);
    }
    {
        BIO *bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(bio, pkey);
        BUF_MEM *mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        std::ofstream f(kf.publicPath, std::ios::binary);
        f.write(mem->data, static_cast<std::streamsize>(mem->length));
        BIO_free(bio);
    }
    return kf;
}

std::string rsaSignBase64(EVP_PKEY *pkey, const std::string &data)
{
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(mdctx, data.c_str(), data.length());
    size_t sigLen = 0;
    EVP_DigestSignFinal(mdctx, nullptr, &sigLen);
    std::vector<unsigned char> sig(sigLen);
    EVP_DigestSignFinal(mdctx, sig.data(), &sigLen);
    EVP_MD_CTX_free(mdctx);

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    BIO_write(b64, sig.data(), static_cast<int>(sigLen));
    BIO_flush(b64);
    BUF_MEM *bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

// Notification string-to-sign: exclude sign AND sign_type, drop empty values.
std::string alipaySignString(const Json::Value &params)
{
    std::vector<std::string> keys;
    for (const auto &key : params.getMemberNames())
    {
        if (key != "sign" && key != "sign_type")
        {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    std::string data;
    for (const auto &key : keys)
    {
        const std::string value = params[key].asString();
        if (value.empty())
        {
            continue;
        }
        if (!data.empty())
        {
            data += "&";
        }
        data += key + "=" + value;
    }
    return data;
}

// Build a client from the local keypair. app_id/seller_id are passed explicitly
// so a test can make the client unconfigured (empty) while still loading real
// keys — verifyCallback then succeeds and the isConfigured gate is what rejects.
std::shared_ptr<AlipaySandboxClient> makeAlipayClient(
  const KeyFiles &kf,
  const std::string &appId,
  const std::string &sellerId
)
{
    Json::Value config;
    config["app_id"] = appId;
    config["seller_id"] = sellerId;
    config["private_key_path"] = kf.privatePath;
    config["alipay_public_key_path"] = kf.publicPath;
    config["gateway_url"] = "https://openapi.alipaydev.com/gateway.do";
    return std::make_shared<AlipaySandboxClient>(config);
}

// Null-DB service: syncOrderStatusFromAlipay maps TRADE_SUCCESS to a non-empty
// status synchronously, reaching the ack branch without Postgres.
std::shared_ptr<PaymentService> makeNullDbService()
{
    std::map<std::string, drogon_pay::PaymentChannelPtr> channels;
    return std::make_shared<PaymentService>(channels, nullptr, nullptr, nullptr);
}

// A notification signed over the params AS SENT (app_id/sellerId are the values
// carried in the notification, independent of the client's own configuration).
std::unordered_map<std::string, std::string> signedNotifyParams(
  EVP_PKEY *pkey,
  const std::string &outTradeNo,
  const std::string &appId,
  const std::string &sellerId,
  const std::string &tradeStatus = "TRADE_SUCCESS"
)
{
    std::unordered_map<std::string, std::string> params;
    params["out_trade_no"] = outTradeNo;
    params["trade_no"] = "20260919220010000099";
    params["trade_status"] = tradeStatus;
    params["total_amount"] = "88.88";
    params["app_id"] = appId;
    params["seller_id"] = sellerId;
    params["sign_type"] = "RSA2";

    Json::Value jp;
    for (const auto &kv : params)
    {
        jp[kv.first] = kv.second;
    }
    params["sign"] = rsaSignBase64(pkey, alipaySignString(jp));
    return params;
}

}  // namespace

// =============================================================================
// P0-4.3: WeChat callback controller — event_type routing (C2-1 fix)
// =============================================================================

DROGON_TEST(CallbackController_Wechat_InvalidJson_Rejected)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody("{");

    auto controller = std::make_shared<WechatCallbackController>();

    std::promise<CtrlResult> promise;
    controller->notify(req, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        auto json = resp->getJsonObject();
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // C2-1: invalid JSON → HTTP 400 + code=40002
    CHECK(r.status == drogon::k400BadRequest);
    CHECK(r.body["code"].asInt() == 40002);
}

DROGON_TEST(CallbackController_Wechat_MissingEventType_Rejected)
{
    Json::Value notify;
    notify["id"] = "no_et_test";
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string body = Json::writeString(builder, notify);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);

    auto controller = std::make_shared<WechatCallbackController>();

    std::promise<CtrlResult> promise;
    controller->notify(req, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        auto json = resp->getJsonObject();
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // C2-1: missing event_type → HTTP 400 + code=40003
    CHECK(r.status == drogon::k400BadRequest);
    CHECK(r.body["code"].asInt() == 40003);
}

DROGON_TEST(CallbackController_Wechat_UnknownEventType_Rejected)
{
    Json::Value notify;
    notify["id"] = "unknown_et_test";
    notify["event_type"] = "UNKNOWN.EVENT";
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string body = Json::writeString(builder, notify);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);

    auto controller = std::make_shared<WechatCallbackController>();

    std::promise<CtrlResult> promise;
    controller->notify(req, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        auto json = resp->getJsonObject();
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // C2-1: unknown event_type → HTTP 400 + code=40004
    CHECK(r.status == drogon::k400BadRequest);
    CHECK(r.body["code"].asInt() == 40004);
}

// =============================================================================
// P0-1: Alipay callback controller — an unauthenticated notification is
// rejected before it can advance any order state.
//
// Both cases assert the "signature verification failed" reason literally: the
// shared config registers the Alipay channel (config.json channels.alipay
// enabled, and the client tolerates missing key files), so the only reject
// branch a forged notification can reach is verification. The assertion fails
// loudly if that assumption ever stops holding, instead of quietly accepting
// the "client not configured" reject.
// =============================================================================

DROGON_TEST(CallbackController_Alipay_ForgedSignature_Rejected)
{
    // A plausible-looking TRADE_SUCCESS notification whose signature was not
    // produced by the Alipay key. Accepting it would credit a payment that
    // never happened, so the controller must fail it at verification.
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeString("application/x-www-form-urlencoded");
    req->setBody(
      "out_trade_no=FORGED_SIG_ORDER_001&trade_no=2026092000000000000001"
      "&trade_status=TRADE_SUCCESS&total_amount=88.88"
      "&app_id=2021000000000000&sign_type=RSA2"
      "&sign=bm90LWEtcmVhbC1zaWduYXR1cmU"
    );

    auto controller = std::make_shared<AlipayCallbackController>();

    std::promise<CtrlResult> promise;
    controller->notify(req, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        auto json = resp->getJsonObject();
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // P0-1: the notification is acknowledged as FAIL, never SUCCESS.
    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "signature verification failed");
}

DROGON_TEST(CallbackController_Alipay_MissingSignature_Rejected)
{
    // The same request with no sign= member at all: the empty-signature branch
    // must not be treated as "nothing to check".
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeString("application/x-www-form-urlencoded");
    req->setBody(
      "out_trade_no=FORGED_NOSIG_ORDER_001&trade_status=TRADE_SUCCESS"
      "&total_amount=88.88&app_id=2021000000000000&sign_type=RSA2"
    );

    auto controller = std::make_shared<AlipayCallbackController>();

    std::promise<CtrlResult> promise;
    controller->notify(req, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        auto json = resp->getJsonObject();
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "signature verification failed");
}

// =============================================================================
// H1: a verified, booked notification is acknowledged with the EXACT plain-text
// body "success" (not JSON), so Alipay stops redelivering.
// M2 (positive control): a matching seller_id passes the merchant re-check.
// =============================================================================
DROGON_TEST(CallbackController_Alipay_ValidNotify_PlainTextSuccess)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    Json::Value config;
    config["app_id"] = "2021000000000000";
    config["seller_id"] = "2088000000000000";
    config["private_key_path"] = kf.privatePath;
    config["alipay_public_key_path"] = kf.publicPath;
    config["gateway_url"] = "https://openapi.alipaydev.com/gateway.do";
    auto client = std::make_shared<AlipaySandboxClient>(config);

    std::map<std::string, drogon_pay::PaymentChannelPtr> channels;
    auto service = std::make_shared<PaymentService>(channels, nullptr, nullptr, nullptr);

    std::unordered_map<std::string, std::string> params;
    params["out_trade_no"] = "ORDER-H1-0001";
    params["trade_no"] = "20260919220010000005";
    params["trade_status"] = "TRADE_SUCCESS";
    params["total_amount"] = "88.88";
    params["app_id"] = "2021000000000000";
    params["seller_id"] = "2088000000000000";
    params["sign_type"] = "RSA2";

    Json::Value jp;
    for (const auto &kv : params)
    {
        jp[kv.first] = kv.second;
    }
    params["sign"] = rsaSignBase64(kf.pkey, alipaySignString(jp));

    std::promise<CtrlResult> promise;
    processAlipayNotify(params, client, service, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        r.bodyText = std::string(resp->body());
        auto json = resp->getJsonObject();
        r.isJson = static_cast<bool>(json);
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // H1: HTTP 200 with the literal 7-byte body "success", and NOT a JSON object.
    CHECK(r.status == drogon::k200OK);
    CHECK(r.bodyText == "success");
    CHECK(r.isJson == false);
}

// =============================================================================
// M2: a validly-signed notification whose seller_id differs from the configured
// merchant is rejected before any order state advances.
// =============================================================================
DROGON_TEST(CallbackController_Alipay_SellerIdMismatch_Rejected)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    Json::Value config;
    config["app_id"] = "2021000000000000";
    config["seller_id"] = "2088000000000000";
    config["private_key_path"] = kf.privatePath;
    config["alipay_public_key_path"] = kf.publicPath;
    config["gateway_url"] = "https://openapi.alipaydev.com/gateway.do";
    auto client = std::make_shared<AlipaySandboxClient>(config);

    std::map<std::string, drogon_pay::PaymentChannelPtr> channels;
    auto service = std::make_shared<PaymentService>(channels, nullptr, nullptr, nullptr);

    std::unordered_map<std::string, std::string> params;
    params["out_trade_no"] = "ORDER-M2-0001";
    params["trade_no"] = "20260919220010000006";
    params["trade_status"] = "TRADE_SUCCESS";
    params["total_amount"] = "88.88";
    params["app_id"] = "2021000000000000";
    params["seller_id"] = "2088999999999999";  // foreign seller
    params["sign_type"] = "RSA2";

    // Sign over the params AS SENT (foreign seller included): the signature is
    // genuine, so this isolates the seller_id re-check from verification.
    Json::Value jp;
    for (const auto &kv : params)
    {
        jp[kv.first] = kv.second;
    }
    params["sign"] = rsaSignBase64(kf.pkey, alipaySignString(jp));

    std::promise<CtrlResult> promise;
    processAlipayNotify(params, client, service, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        r.bodyText = std::string(resp->body());
        auto json = resp->getJsonObject();
        r.isJson = static_cast<bool>(json);
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // M2: rejected as FAIL with the seller_id reason, never a "success" ack.
    CHECK(r.bodyText != "success");
    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "seller_id mismatch");
}

// =============================================================================
// #7: an enabled-but-unconfigured client (empty app_id/seller_id) must refuse a
// GENUINELY-signed notification. The signature verifies (real keys are loaded),
// so this isolates the isConfigured gate from verification: without it the
// app_id/seller_id re-checks would be silently skipped and a foreign merchant's
// valid notification could credit a colliding order.
// =============================================================================
DROGON_TEST(CallbackController_Alipay_UnconfiguredClient_RejectsValidSignature)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    // Keys loaded, but merchant identity empty -> isConfigured() == false.
    auto client = makeAlipayClient(kf, "", "");
    auto service = makeNullDbService();
    REQUIRE(!client->isConfigured());

    // A foreign merchant's notification, validly signed against the loaded key.
    auto params =
      signedNotifyParams(kf.pkey, "ORDER-UNCONFIG-0001", "2021000000000000", "2088000000000000");

    std::promise<CtrlResult> promise;
    processAlipayNotify(params, client, service, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        r.bodyText = std::string(resp->body());
        auto json = resp->getJsonObject();
        r.isJson = static_cast<bool>(json);
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    // Refused at the configuration gate, never acknowledged as success.
    CHECK(r.bodyText != "success");
    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "Alipay client not configured");
}

// =============================================================================
// #8: a validly-signed notification carrying a foreign app_id is rejected by the
// app_id re-check (mirrors the seller_id case). The signature is genuine over
// the params as sent, so verification passes and the app_id gate is what fires.
// =============================================================================
DROGON_TEST(CallbackController_Alipay_AppIdMismatch_Rejected)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    auto client = makeAlipayClient(kf, "2021000000000000", "2088000000000000");
    auto service = makeNullDbService();
    REQUIRE(client->isConfigured());

    // Foreign app_id, matching seller_id, signed over the params as sent.
    auto params =
      signedNotifyParams(kf.pkey, "ORDER-APPID-0001", "2021999999999999", "2088000000000000");

    std::promise<CtrlResult> promise;
    processAlipayNotify(params, client, service, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        r.bodyText = std::string(resp->body());
        auto json = resp->getJsonObject();
        r.isJson = static_cast<bool>(json);
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    CHECK(r.bodyText != "success");
    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "app_id mismatch");
}

// =============================================================================
// #8: a real-key client rejects a genuinely invalid signature. Unlike the
// global-client ForgedSignature test (which fails on an empty public key), this
// loads a real keypair and sends a real RSA signature over DIFFERENT bytes, so
// EVP_DigestVerifyFinal returns a 0 mismatch — isolating the crypto comparison.
// =============================================================================
DROGON_TEST(CallbackController_Alipay_RealKeyForgedSignature_Rejected)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    auto client = makeAlipayClient(kf, "2021000000000000", "2088000000000000");
    auto service = makeNullDbService();
    REQUIRE(client->isConfigured());

    auto params =
      signedNotifyParams(kf.pkey, "ORDER-REALFORGE-0001", "2021000000000000", "2088000000000000");

    // Re-sign a tampered payload (amount 0.01) but send the original 88.88: a
    // structurally valid RSA-2048 signature that does not match the sent bytes.
    Json::Value tampered;
    for (const auto &kv : params)
    {
        tampered[kv.first] = kv.second;
    }
    tampered["total_amount"] = "0.01";
    params["sign"] = rsaSignBase64(kf.pkey, alipaySignString(tampered));

    std::promise<CtrlResult> promise;
    processAlipayNotify(params, client, service, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        r.bodyText = std::string(resp->body());
        auto json = resp->getJsonObject();
        r.isJson = static_cast<bool>(json);
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    CHECK(r.bodyText != "success");
    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "signature verification failed");
}

// =============================================================================
// A genuine, fully-authorized notification whose trade_status the service does
// not recognize maps to an empty synced status. The handler must answer FAIL
// ("order sync rejected"), never "success": acknowledging it would tell Alipay
// the notification was processed and the refusal would never resurface on retry.
// =============================================================================
DROGON_TEST(CallbackController_Alipay_UnknownTradeStatus_Rejected)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    auto client = makeAlipayClient(kf, "2021000000000000", "2088000000000000");
    auto service = makeNullDbService();
    REQUIRE(client->isConfigured());

    // Signed, matching merchant identity, but an unmapped trade_status.
    auto params = signedNotifyParams(
      kf.pkey, "ORDER-UNKNOWN-0001", "2021000000000000", "2088000000000000", "UNKNOWN_STATUS"
    );

    std::promise<CtrlResult> promise;
    processAlipayNotify(params, client, service, [&promise](const drogon::HttpResponsePtr &resp) {
        CtrlResult r;
        r.called = true;
        r.status = resp->getStatusCode();
        r.bodyText = std::string(resp->body());
        auto json = resp->getJsonObject();
        r.isJson = static_cast<bool>(json);
        if (json)
            r.body = *json;
        promise.set_value(r);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto r = future.get();
    CHECK(r.called);

    CHECK(r.bodyText != "success");
    CHECK(r.body["code"].asString() == "FAIL");
    CHECK(r.body["message"].asString() == "order sync rejected");
}
