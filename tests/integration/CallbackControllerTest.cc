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

namespace
{

struct CtrlResult
{
    drogon::HttpStatusCode status{drogon::k500InternalServerError};
    Json::Value body;
    bool called{false};
};

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
    CHECK(r.body["message"].asString() != "OK");
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
}
