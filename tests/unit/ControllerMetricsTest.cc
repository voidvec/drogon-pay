#include <drogon/drogon_test.h>

#include "controllers/MetricsController.h"
#include "handlers/PayMetricsHandlers.h"
#include "handlers/PayAuthMetrics.h"

namespace
{
template <typename Controller, typename Method>
drogon::HttpResponsePtr runController(
  Controller &controller,
  Method method,
  const drogon::HttpMethod httpMethod
)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(httpMethod);

    drogon::HttpResponsePtr response;
    (controller.*
     method)(req, [&response](const drogon::HttpResponsePtr &resp) { response = resp; });
    return response;
}
}  // namespace

DROGON_TEST(PayMetricsController_AuthMetricsJson)
{
    const auto before = PayAuthMetrics::snapshot();
    PayAuthMetrics::incMissingKey();
    PayAuthMetrics::incInvalidKey();
    PayAuthMetrics::incScopeDenied();
    PayAuthMetrics::incNotConfigured();

    PayMetricsController controller;
    const auto resp = runController(controller, &PayMetricsController::authMetrics, drogon::Get);
    CHECK(resp != nullptr);
    CHECK(resp->statusCode() == drogon::k200OK);

    const auto json = resp->getJsonObject();
    CHECK(json != nullptr);
    CHECK((*json)["missing_key"].asUInt64() == before["missing_key"].asUInt64() + 1);
    CHECK((*json)["invalid_key"].asUInt64() == before["invalid_key"].asUInt64() + 1);
    CHECK((*json)["scope_denied"].asUInt64() == before["scope_denied"].asUInt64() + 1);
    CHECK((*json)["not_configured"].asUInt64() == before["not_configured"].asUInt64() + 1);
}

DROGON_TEST(PayMetricsController_AuthMetricsProm)
{
    const auto snapshot = PayAuthMetrics::snapshot();

    PayMetricsController controller;
    const auto resp =
      runController(controller, &PayMetricsController::authMetricsProm, drogon::Get);
    CHECK(resp != nullptr);
    CHECK(resp->statusCode() == drogon::k200OK);

    const std::string body = std::string(resp->body());
    CHECK(body.find("pay_auth_missing_key_total") != std::string::npos);
    CHECK(body.find("pay_auth_invalid_key_total") != std::string::npos);
    CHECK(body.find("pay_auth_scope_denied_total") != std::string::npos);
    CHECK(body.find("pay_auth_not_configured_total") != std::string::npos);
    CHECK(body.find(std::to_string(snapshot["missing_key"].asUInt64())) != std::string::npos);
    // P3-7.4: verify Prometheus exposition format compliance
    // Every metric must have HELP and TYPE lines
    CHECK(body.find("# HELP pay_auth_missing_key_total") != std::string::npos);
    CHECK(body.find("# TYPE pay_auth_missing_key_total counter") != std::string::npos);
    CHECK(body.find("# HELP pay_auth_invalid_key_total") != std::string::npos);
    CHECK(body.find("# TYPE pay_auth_invalid_key_total counter") != std::string::npos);
    CHECK(body.find("# HELP pay_auth_scope_denied_total") != std::string::npos);
    CHECK(body.find("# TYPE pay_auth_scope_denied_total counter") != std::string::npos);
    CHECK(body.find("# HELP pay_auth_not_configured_total") != std::string::npos);
    CHECK(body.find("# TYPE pay_auth_not_configured_total counter") != std::string::npos);
    // Metric lines should follow format: metric_name{labels} value
    CHECK(body.find("pay_auth_missing_key_total ") != std::string::npos);
}

DROGON_TEST(MetricsController_Options)
{
    MetricsController controller;
    const auto resp = runController(controller, &MetricsController::metrics, drogon::Options);
    CHECK(resp != nullptr);
    CHECK(resp->statusCode() == drogon::k200OK);
}
