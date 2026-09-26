/// =============================================================================
/// @file   PayRouteBarrierTest.cc
/// @brief  The exception barrier around every registered route, driven through
///         the router rather than through a controller.
///
/// The handler-level shape cases call the controllers directly, which proves a
/// mistyped body is answered but never proves the thing the barrier exists for:
/// that one anonymous request cannot take the gateway down. Only a request that
/// really travels `registerHandler -> guarded()` reaches the barrier, so these
/// cases send it over HTTP and then ask the same process for something ordinary.
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon_pay/PayPlugin.h>
#include <string>
#include "TestConfigHelper.h"

using namespace drogon;

DROGON_TEST(PayRouteBarrier_MistypedNotificationBodyIsAnsweredNotFatal)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/api/pay/notify/wechat");
    req->setContentTypeCode(CT_APPLICATION_JSON);
    // The exact body the barrier's own comment names: jsoncpp converts an
    // object where a string was asked for by throwing, and before the shape
    // gate that throw escaped the handler into trantor's loop.
    req->setBody(R"({"amount":{}})");

    client->sendRequest(req, [TEST_CTX, client](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k400BadRequest);
        const auto body = resp->getJsonObject();
        REQUIRE(body != nullptr);
        CHECK((*body)["code"] == 40003);
        CHECK((*body)["message"] == "Missing event_type");
    });
}

// Runs after the case above, which is the point: a process whose event loop was
// stopped by an escaping exception answers nothing here. Kept as its own case
// rather than a nested request because a failure has to name the request that
// produced it.
DROGON_TEST(PayRouteBarrier_GatewayStillServesAfterTheRefusal)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/healthz");

    client->sendRequest(req, [TEST_CTX, client](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);
    });
}

DROGON_TEST(PayRouteBarrier_ForgedAlipayNotificationRefusedAtTheHttpSurface)
{
    // Which refusal this request earns depends on whether this process owns an
    // Alipay client, so read that state instead of assuming it: pinning one
    // literal regardless would let the case pass on the wrong branch.
    const auto plugin = app().getPlugin<PayPlugin>();
    const bool hasClient = plugin && static_cast<bool>(plugin->alipayClient());
    const std::string expectedMessage =
      hasClient ? "signature verification failed" : "Alipay client not configured";

    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/api/pay/notify/alipay");
    req->setContentTypeCode(CT_APPLICATION_X_FORM);
    req->setBody(
      "app_id=2021000000000000&out_trade_no=pay_route_barrier_forged&total_amount=0.01"
      "&trade_status=TRADE_SUCCESS&sign=bm90LWEtc2lnbmF0dXJl&sign_type=RSA2"
    );

    client->sendRequest(
      req, [TEST_CTX, client, expectedMessage](ReqResult result, const HttpResponsePtr &resp) {
          REQUIRE(result == ReqResult::Ok);
          REQUIRE(resp != nullptr);
          // Refusals here are a 200 carrying FAIL, which is what this project's
          // callback contract documents; a 5xx would mean the barrier faulted.
          CHECK(resp->getStatusCode() == k200OK);
          const auto body = resp->getJsonObject();
          REQUIRE(body != nullptr);
          CHECK((*body)["code"] == "FAIL");
          CHECK((*body)["message"] == expectedMessage);
      }
    );
}
