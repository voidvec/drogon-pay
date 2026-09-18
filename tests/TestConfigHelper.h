#pragma once

// ============================================================================
// Shared test helpers for loading config.json and building a libpq connection
// string. Replaces the per-file duplicates that lived in each integration test.
//
// loadConfig() resolves __env_var:VAR__ placeholders via ConfigLoader, mirroring
// the host's main.cc and tests/main.cc, so credentials in config.json (e.g. the
// DB password) are pulled from the environment that tests/main.cc loaded from
// .env. Without this, the raw placeholder string was passed to libpq and DB
// auth failed.
// ============================================================================

#include <json/json.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <string>
#include <vector>
#include "utils/ConfigLoader.h"

namespace pay::test_util
{

// Test listener port, isolated from the dev server's 5566. config.json is a
// straight copy of examples/pay-server, so without an override the test binary
// and a locally running PayServer would both bind 5566 (drogon reuse_port) and
// hijack each other's requests. Override with PAY_TEST_PORT if 5567 is taken.
inline int testPort()
{
    if (const char *env = std::getenv("PAY_TEST_PORT"))
    {
        const int port = std::atoi(env);
        if (port > 0 && port < 65536)
        {
            return port;
        }
    }
    return 5567;
}

inline std::string testBaseUrl()
{
    return "http://127.0.0.1:" + std::to_string(testPort());
}

inline bool loadConfig(Json::Value &root)
{
    const auto cwd = std::filesystem::current_path();
    const std::vector<std::filesystem::path> candidates =
      {cwd / "config.json",
       cwd / "test" / "Release" / "config.json",
       cwd / "test" / "Debug" / "config.json",
       cwd / "Release" / "config.json",
       cwd / "Debug" / "config.json",
       cwd.parent_path() / "config.json",
       cwd.parent_path() / "test" / "Release" / "config.json",
       cwd.parent_path() / "test" / "Debug" / "config.json",
       cwd.parent_path() / "Release" / "config.json",
       cwd.parent_path() / "Debug" / "config.json"};

    std::filesystem::path configPath;
    for (const auto &candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            configPath = candidate;
            break;
        }
    }

    if (configPath.empty())
    {
        return false;
    }

    std::ifstream in(configPath.string());
    if (!in)
    {
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string errors;
    const bool ok = Json::parseFromStream(builder, in, &root, &errors);
    if (!ok)
    {
        return false;
    }

    // Resolve __env_var:VAR__ placeholders so credentials come from the
    // environment (loaded from .env by tests/main.cc), matching main.cc.
    root = ConfigLoader::loadConfig(root);
    return true;
}

inline std::string buildPgConnInfo(const Json::Value &db)
{
    const std::string host = db.get("host", "").asString();
    const int port = db.get("port", 5432).asInt();
    const std::string dbname = db.get("dbname", "").asString();
    const std::string user = db.get("user", "").asString();
    const std::string passwd = db.get("passwd", "").asString();

    std::string connInfo =
      "host=" + host + " port=" + std::to_string(port) + " dbname=" + dbname + " user=" + user;
    if (!passwd.empty())
    {
        connInfo += " password=" + passwd;
    }
    return connInfo;
}

// ============================================================================
// Future wait helpers
// ============================================================================
// Root-cause defense against the ctest hang. Integration tests drive async
// services through a std::promise and wait on the matching std::future. The old
// idiom was:
//
//     CHECK(fut.wait_for(5s) == std::future_status::ready);
//     auto x = fut.get();   // blocks forever if the callback never fires
//
// Drogon's CHECK does NOT abort on failure, so a 5s timeout fell through to
// fut.get() with no timeout and deadlocked the whole ctest run. The fix below
// pairs REQUIRE-style abort-on-timeout (handled at call sites via the boolean
// return) with a get() that only runs when the future is known ready.
//
// waitForFutureReady(): pure readiness check; does NOT call .get(). Use it as
//   REQUIRE(waitForFutureReady(fut, 5s));  // fails+returns on timeout
//   auto x = fut.get();                    // safe: only reached when ready
//
// tryFutureGet(): readiness check + value extraction in one. For sites that
// previously did `auto x = promise.get_future().get();` with no wait at all.
// Returns false on timeout and leaves `out` untouched so the caller's
// subsequent assertions fail loudly instead of hanging.
// ============================================================================

template <typename T, typename Rep, typename Period>
inline bool waitForFutureReady(
  std::future<T> &fut,
  const std::chrono::duration<Rep, Period> &timeout
)
{
    return fut.wait_for(timeout) == std::future_status::ready;
}

template <typename T, typename Rep, typename Period>
inline bool tryFutureGet(
  std::future<T> &fut,
  T &out,
  const std::chrono::duration<Rep, Period> &timeout
)
{
    if (fut.wait_for(timeout) != std::future_status::ready)
    {
        return false;
    }
    out = fut.get();
    return true;
}

}  // namespace pay::test_util
