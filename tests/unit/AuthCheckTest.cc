#include <drogon/drogon_test.h>
#include "handlers/AuthCheck.h"
#include "handlers/PayAuthMetrics.h"
#include <cstdlib>

namespace
{
void setEnvVar(const char *key, const char *value)
{
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value && *value)
    {
        setenv(key, value, 1);
    }
    else
    {
        unsetenv(key);
    }
#endif
}

Json::UInt64 metricValue(const Json::Value &snapshot, const char *key)
{
    return snapshot.get(key, 0).asUInt64();
}

constexpr const char *kBasePath = "/api/pay";
}  // namespace

DROGON_TEST(AuthCheck_NotConfigured)
{
    setEnvVar("PAY_API_KEY", "");
    setEnvVar("PAY_API_KEYS", "");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/pay/query");

    const auto before = PayAuthMetrics::snapshot();
    auto resp = drogon_pay::checkAuth(req, kBasePath);

    CHECK(resp != nullptr);
    CHECK(resp->statusCode() == drogon::k503ServiceUnavailable);
    CHECK(resp->body() == "api key not configured");

    const auto after = PayAuthMetrics::snapshot();
    CHECK(metricValue(after, "not_configured") == metricValue(before, "not_configured") + 1);
}

DROGON_TEST(AuthCheck_MissingKey)
{
    setEnvVar("PAY_API_KEY", "secret");
    setEnvVar("PAY_API_KEYS", "");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/pay/query");

    const auto before = PayAuthMetrics::snapshot();
    auto resp = drogon_pay::checkAuth(req, kBasePath);

    CHECK(resp != nullptr);
    CHECK(resp->statusCode() == drogon::k401Unauthorized);
    CHECK(resp->body() == "missing api key");

    const auto after = PayAuthMetrics::snapshot();
    CHECK(metricValue(after, "missing_key") == metricValue(before, "missing_key") + 1);
}

DROGON_TEST(AuthCheck_InvalidKey)
{
    setEnvVar("PAY_API_KEY", "secret");
    setEnvVar("PAY_API_KEYS", "");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/pay/query");
    req->addHeader("X-Api-Key", "wrong");

    const auto before = PayAuthMetrics::snapshot();
    auto resp = drogon_pay::checkAuth(req, kBasePath);

    CHECK(resp != nullptr);
    CHECK(resp->statusCode() == drogon::k401Unauthorized);
    CHECK(resp->body() == "invalid api key");

    const auto after = PayAuthMetrics::snapshot();
    CHECK(metricValue(after, "invalid_key") == metricValue(before, "invalid_key") + 1);
}

DROGON_TEST(AuthCheck_ValidKey)
{
    // test_key_123456 has order_query scope in api_key_scopes config
    // NOTE: reset env after previous tests polluted it to "secret"
    setEnvVar("PAY_API_KEY", "test_key_123456");
    setEnvVar("PAY_API_KEYS", "test_key_123456,performance-test-key,admin-test-key");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/pay/query");
    req->addHeader("X-Api-Key", "test_key_123456");

    // Null response means the request is authorized.
    auto resp = drogon_pay::checkAuth(req, kBasePath);
    CHECK(resp == nullptr);
}

DROGON_TEST(AuthCheck_OptionsPreflightPassesThrough)
{
    setEnvVar("PAY_API_KEY", "");
    setEnvVar("PAY_API_KEYS", "");

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Options);
    req->setPath("/api/pay/create");

    // CORS preflight must pass even without any key configured.
    auto resp = drogon_pay::checkAuth(req, kBasePath);
    CHECK(resp == nullptr);
}
