#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include "TestConfigHelper.h"

using namespace drogon;

// These are integration tests that hit the in-process test server started by
// tests/main.cc (port from pay::test_util::testPort(), default 5567).

DROGON_TEST(HealthProbe_LivenessEndpoint)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/healthz");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);

        auto json = resp->getJsonObject();
        REQUIRE(json != nullptr);
        CHECK((*json)["status"].asString() == "alive");
    });
}

DROGON_TEST(HealthProbe_ReadinessEndpoint)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/readyz");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);

        auto json = resp->getJsonObject();
        REQUIRE(json != nullptr);

        // With DB running: 200 + "ready"; without DB: 503 + "not_ready"
        auto status = (*json)["status"].asString();
        if (resp->getStatusCode() == k200OK)
        {
            CHECK(status == "ready");
        }
        else
        {
            CHECK(resp->getStatusCode() == k503ServiceUnavailable);
            CHECK(status == "not_ready");
            CHECK(json->isMember("failed"));
        }
    });
}

DROGON_TEST(HealthProbe_CompatEndpoint_DeprecationHeader)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/health");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);

        auto deprecation = resp->getHeader("Deprecation");
        CHECK(deprecation == "true");

        auto sunset = resp->getHeader("Sunset");
        CHECK(!sunset.empty());
    });
}
