#pragma once

#include <drogon/HttpController.h>
#include <atomic>

using namespace drogon;

class HealthCheckController : public drogon::HttpController<HealthCheckController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthCheckController::healthz, "/healthz", Get, Options);
    ADD_METHOD_TO(HealthCheckController::readyz, "/readyz", Get, Options);
    METHOD_LIST_END

    void healthz(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void readyz(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);

  private:
    std::atomic<int> consecutiveFailures_{0};
    // Start "not ready" so traffic is not routed before the first probe has
    // actually verified dependencies. Hysteresis still prevents flapping once a
    // successful probe has flipped this to true.
    std::atomic<bool> lastReadyState_{false};
    static constexpr int FAILURE_THRESHOLD = 3;
};
