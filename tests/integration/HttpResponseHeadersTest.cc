/// =============================================================================
/// @file   HttpResponseHeadersTest.cc
/// @brief  P2 tests for CORS and security headers (paths 13-14).
///
/// Verifies that every response includes the mandatory security headers
/// (X-Content-Type-Options, X-Frame-Options, HSTS, CSP, etc.) and that
/// OPTIONS preflight returns correct CORS headers for allowed origins.
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include "TestConfigHelper.h"

using namespace drogon;

// CORS origins allowed in config.json custom_config.cors.allow_origins
static const std::string kAllowedOrigin = "http://localhost:5173";

// =============================================================================
// P2-6.9: Security headers on every response
// =============================================================================

DROGON_TEST(HttpHeaders_SecurityHeaders_Present)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/healthz");  // use liveness probe — no auth required

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);

        CHECK(resp->getHeader("X-Content-Type-Options") == "nosniff");
        CHECK(resp->getHeader("X-Frame-Options") == "DENY");
        CHECK(resp->getHeader("X-XSS-Protection") == "1; mode=block");
        CHECK(
          resp->getHeader("Strict-Transport-Security").find("max-age=31536000") != std::string::npos
        );
        CHECK(resp->getHeader("Content-Security-Policy").find("default-src") != std::string::npos);
        CHECK(resp->getHeader("Referrer-Policy") == "no-referrer");
        CHECK(resp->getHeader("Permissions-Policy").find("geolocation") != std::string::npos);
    });
}

// =============================================================================
// P2-6.8: CORS OPTIONS preflight
// =============================================================================

DROGON_TEST(HttpHeaders_CorsOptions_Preflight_AllowedOrigin)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Options);
    req->setPath("/api/pay/create");
    req->addHeader("Origin", kAllowedOrigin);
    req->addHeader("Access-Control-Request-Method", "POST");
    req->addHeader("Access-Control-Request-Headers", "Content-Type, X-Api-Key");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);

        CHECK(resp->getHeader("Access-Control-Allow-Origin") == kAllowedOrigin);
        CHECK(resp->getHeader("Access-Control-Allow-Methods").find("POST") != std::string::npos);
        CHECK(
          resp->getHeader("Access-Control-Allow-Headers").find("Content-Type") != std::string::npos
        );
        CHECK(resp->getHeader("Access-Control-Allow-Credentials") == "true");
    });
}

DROGON_TEST(HttpHeaders_CorsOptions_DisallowedOrigin_NoCORS)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Options);
    req->setPath("/api/pay/create");
    req->addHeader("Origin", "https://evil.example.com");
    req->addHeader("Access-Control-Request-Method", "POST");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);

        // Non-whitelisted origin: should NOT receive CORS allow headers
        CHECK(resp->getHeader("Access-Control-Allow-Origin").empty());
    });
}

DROGON_TEST(HttpHeaders_CorsPost_SuccessfulOrigin_HasCORS)
{
    auto client = HttpClient::newHttpClient(pay::test_util::testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/healthz");
    req->addHeader("Origin", kAllowedOrigin);

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::Ok);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);

        // CORS headers applied via PostHandlingAdvice for allowed origins
        CHECK(resp->getHeader("Access-Control-Allow-Origin") == kAllowedOrigin);
        CHECK(resp->getHeader("Access-Control-Allow-Methods").find("GET") != std::string::npos);
    });
}
