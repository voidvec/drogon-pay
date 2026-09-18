// Minimal Drogon host used by the Conan test_package.
//
// End-to-end consumer verification of the drogon-pay static library:
//   1. drogon_pay::ensureLinked() compiles and links (public API surface).
//   2. PayPlugin loads from a plugins config block, i.e. the DrObject
//      auto-registration symbol survived static-library linking.
//   3. The programmatically registered routes are reachable: a request to
//      {base_path}/query must NOT return 404 (it returns 401 here because no
//      API key is configured - which proves the handler chain is wired up).
//
// Structure mirrors tests/main.cc: the app runs on a worker thread and
// the main thread performs synchronous checks, then asks the loop to quit.
#include <drogon/drogon.h>
#include <drogon_pay/PayPlugin.h>

#include <cstdio>
#include <future>
#include <iostream>
#include <thread>

namespace
{
int fail(const std::string &reason)
{
    std::cerr << "[test_package] FAILED: " << reason << std::endl;
    return 1;
}
}  // namespace

int main()
{
    // Unbuffered stdout so teardown logs survive if the process dies.
    setvbuf(stdout, nullptr, _IONBF, 0);
    drogon_pay::ensureLinked();

    Json::Value config;
    config["listeners"][0]["address"] = "127.0.0.1";
    config["listeners"][0]["port"] = 18848;

    // PayPlugin refuses to start without a db client; an in-memory sqlite3
    // client keeps the test hermetic (no external services).
    Json::Value db;
    db["name"] = "default";
    db["rdbms"] = "sqlite3";
    db["filename"] = ":memory:";
    db["connection_number"] = 1;
    config["db_clients"][0] = db;

    Json::Value plugin;
    plugin["name"] = "PayPlugin";
    plugin["config"]["base_path"] = "/api/pay";
    plugin["config"]["idempotency_ttl_seconds"] = 60;
    plugin["config"]["channels"]["wechat"]["enabled"] = false;
    plugin["config"]["channels"]["alipay"]["enabled"] = false;
    plugin["config"]["reconcile"]["enabled"] = false;
#ifndef PROBE_NO_PLUGIN
    config["plugins"][0] = plugin;
#endif

    drogon::app().loadConfigJson(config);

    std::promise<void> started;
    auto startedFuture = started.get_future();
    std::thread appThread([&started]() {
        drogon::app().getLoop()->queueInLoop([&started]() { started.set_value(); });
        drogon::app().run();
    });
    startedFuture.wait();

    int exitCode = 0;
    {
        auto *payPlugin = drogon::app().getPlugin<PayPlugin>();
        if (!payPlugin)
        {
            exitCode = fail("getPlugin<PayPlugin>() returned null - DrObject "
                            "registration symbol was dropped by the linker");
        }
        else if (payPlugin->basePath() != "/api/pay")
        {
            exitCode = fail("unexpected base path: " + payPlugin->basePath());
        }
        else
        {
            auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:18848");
            auto req = drogon::HttpRequest::newHttpRequest();
            req->setMethod(drogon::Get);
            req->setPath("/api/pay/query");
            auto [result, resp] = client->sendRequest(req, 10.0);
            if (result != drogon::ReqResult::Ok || !resp)
            {
                exitCode = fail("request to /api/pay/query did not complete");
            }
            else if (resp->statusCode() == drogon::k404NotFound)
            {
                exitCode = fail("/api/pay/query returned 404 - plugin routes "
                                "were not registered");
            }
            else
            {
                std::cout << "[test_package] OK: PayPlugin loaded and routes "
                             "reachable (HTTP "
                          << resp->statusCode() << ")" << std::endl;
            }
        }
    }

    drogon::app().getLoop()->queueInLoop([]() { drogon::app().quit(); });
    appThread.join();
    std::cout << "[test_package] clean shutdown, exit=" << exitCode << std::endl;
    return exitCode;
}
