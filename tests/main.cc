#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon_pay/ChannelRegistry.h>
#include <stdexcept>
#include "StubChannel.h"
#include "utils/ConfigLoader.h"
#include "utils/SecurityHeaders.h"
#include "TestConfigHelper.h"
#include <fstream>
#include <json/json.h>

namespace
{
using pay::test_util::StubChannel;

/// Host-side channel factories. PayPlugin consumes them while it assembles, so
/// registration has to happen before the app starts -- the contract
/// docs/development/plugin_integration.md spells out as "register the factory
/// before app().run()". The test binary is a host, so this is the only place
/// that assembly step can be exercised end to end; HostChannelAssemblyTest
/// reads the results back off the plugin.
void registerHostChannelFactories()
{
    // Enabled by the config block enableHostChannel() adds below.
    drogon_pay::ChannelRegistry::registerFactory(
      pay::test_util::kHostChannelName,
      [](const Json::Value &config) { return StubChannel::fromConfig(config); }
    );
    // Deliberately named after a built-in: the assembly loop has to skip it, so
    // the real WeChat channel survives. It answers instead of throwing so the
    // skip is what the assertion detects -- a caught throw would look the same
    // from the outside as a guarded one.
    drogon_pay::ChannelRegistry::registerFactory(
      "wechat", [](const Json::Value &config) -> drogon_pay::PaymentChannelPtr {
          return StubChannel::fromConfig(config);
      }
    );
    // A host factory that fails must be logged and skipped, not fatal.
    drogon_pay::ChannelRegistry::registerFactory(
      pay::test_util::kBrokenHostChannelName,
      [](const Json::Value &) -> drogon_pay::PaymentChannelPtr {
          throw std::runtime_error("host factory fails on purpose");
      }
    );
}

/// Adds an enabled `channels.<name>` block to the PayPlugin entry of the loaded
/// config. Done here rather than in examples/pay-server/config.json because a
/// fake channel belongs to the test host, not to the example anyone can copy.
void enableHostChannel(Json::Value &config, const std::string &name, const std::string &marker)
{
    for (auto &plugin : config["plugins"])
    {
        if (plugin.get("name", "").asString() != "PayPlugin")
        {
            continue;
        }
        Json::Value block;
        block["enabled"] = true;
        // Present so StartupValidator's "enabled but app_id is not set" warning
        // stays a signal about real channels instead of noise about this fixture.
        block["app_id"] = name;
        block["name"] = name;
        block["marker"] = marker;
        plugin["config"]["channels"][name] = block;
    }
}
}  // namespace

int main(int argc, char **argv)
{
    using namespace drogon;

    // Load .env and process config.json placeholders (same as main.cc)
    ConfigLoader::loadEnvFile(".env");

    registerHostChannelFactories();

    std::ifstream configFile("./config.json");
    if (configFile.is_open())
    {
        Json::Value config;
        Json::CharReaderBuilder builder;
        std::string errors;
        if (Json::parseFromStream(builder, configFile, &config, &errors))
        {
            Json::Value processedConfig = ConfigLoader::loadConfig(config);
            // Isolate the test listener from the dev server: config.json is a
            // straight copy of examples/pay-server (port 5566), and with two
            // processes bound to the same port requests get hijacked randomly.
            // Tests listen on PAY_TEST_PORT (default 5567) instead.
            for (auto &listener : processedConfig["listeners"])
            {
                listener["port"] = pay::test_util::testPort();
            }
            // The same isolation has to cover custom_config: /metrics proxies
            // to *this process's* /metrics/base, and the copied config points
            // at 5566, so a case that scraped /metrics would silently read a
            // locally running dev PayServer's counters instead of its own.
            const bool hasCustomMetricsUrl =
              processedConfig.isMember("custom_config") &&
              processedConfig["custom_config"].isMember("pay") &&
              processedConfig["custom_config"]["pay"].isMember("metrics_base_url");
            if (hasCustomMetricsUrl)
            {
                processedConfig["custom_config"]["pay"]["metrics_base_url"] =
                  pay::test_util::testBaseUrl() + "/metrics/base";
            }
            enableHostChannel(
              processedConfig, pay::test_util::kHostChannelName, pay::test_util::kHostChannelMarker
            );
            enableHostChannel(
              processedConfig, pay::test_util::kBrokenHostChannelName, "never built"
            );
            app().loadConfigJson(std::move(processedConfig));
        }
    }

    // Register CORS + security headers (same as production main.cc)
    // P2-6.8/6.9: Without this, HttpResponseHeadersTest tests would fail
    // because the test binary wouldn't apply security headers to responses.
    pay::security::setupCorsAndSecurityHeaders();

    std::promise<void> p1;
    std::future<void> f1 = p1.get_future();

    // Start the main loop on another thread
    std::thread thr([&]() {
        // Queues the promise to be fulfilled after starting the loop
        app().getLoop()->queueInLoop([&p1]() { p1.set_value(); });
        app().run();
    });

    // The future is only satisfied after the event loop started
    f1.get();
    int status = test::run(argc, argv);

    // Ask the event loop to shutdown and wait
    app().getLoop()->queueInLoop([]() { app().quit(); });
    thr.join();
    return status;
}
