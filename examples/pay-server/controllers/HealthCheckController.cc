#include "HealthCheckController.h"
#include <drogon/drogon.h>
#include <json/json.h>

void HealthCheckController::healthz(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    Json::Value response;

    bool loopAlive = (drogon::app().getLoop() != nullptr);
    bool isRunning = drogon::app().isRunning();

    if (loopAlive && isRunning)
    {
        response["status"] = "alive";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(k200OK);
        callback(resp);
    }
    else
    {
        response["status"] = "dead";
        if (!loopAlive)
            response["reason"] = "event_loop_null";
        else
            response["reason"] = "not_running";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(k503ServiceUnavailable);
        callback(resp);
    }
}

void HealthCheckController::readyz(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    auto redisClient = drogon::app().getRedisClient();

    struct ReadyState
    {
        std::mutex mtx;
        std::vector<std::string> failed;
        int pending = 0;
    };

    auto state = std::make_shared<ReadyState>();

    state->pending = 2;  // DB + Redis
    if (redisClient == nullptr)
        state->pending = 1;  // Only DB

    // DB check
    if (dbClient)
    {
        // Connectivity probe (raw-SQL exemption): touches no table.
        dbClient->execSqlAsync(
          "SELECT 1",
          [state](const drogon::orm::Result &) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->pending--;
          },
          [state](const drogon::orm::DrogonDbException & /*e*/) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->failed.push_back("db");
              state->pending--;
          }
        );
    }
    else
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->failed.push_back("db");
        state->pending--;
    }

    // Redis check: PING
    if (redisClient)
    {
        redisClient->execCommandAsync(
          [state](const drogon::nosql::RedisResult &) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->pending--;
          },
          [state](const std::exception & /*e*/) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->failed.push_back("redis");
              state->pending--;
          },
          "PING"
        );
    }

    // 1-second deadline
    auto *loop = drogon::app().getLoop();
    loop->runAfter(1.0, [state, callback, this]() {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (state->pending > 0)
        {
            state->failed.push_back("timeout");
        }

        Json::Value response;
        bool ready = state->failed.empty();

        if (ready)
        {
            consecutiveFailures_.store(0);
            lastReadyState_.store(true);
        }
        else
        {
            int failures = consecutiveFailures_.fetch_add(1) + 1;
            if (failures >= FAILURE_THRESHOLD)
            {
                lastReadyState_.store(false);
            }
        }

        bool reportReady = lastReadyState_.load();

        if (reportReady)
        {
            response["status"] = "ready";
            auto resp = HttpResponse::newHttpJsonResponse(response);
            resp->setStatusCode(k200OK);
            callback(resp);
        }
        else
        {
            response["status"] = "not_ready";
            Json::Value failed(Json::arrayValue);
            for (const auto &f : state->failed)
            {
                failed.append(f);
            }
            response["failed"] = failed;
            auto resp = HttpResponse::newHttpJsonResponse(response);
            resp->setStatusCode(k503ServiceUnavailable);
            callback(resp);
        }
    });
}
