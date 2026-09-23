#pragma once

// ============================================================================
// Shared PaymentChannel stub.
//
// Two roles, one class:
//  - the registry-level stub in tests/unit/ChannelRegistryTest.cc, where a
//    channel that answers every call with an error IS the point (the registry
//    never looks at what a channel does); and
//  - the host-provided channel that tests/main.cc registers through
//    ChannelRegistry::registerFactory(), because the test binary is a host and
//    the factory SPI otherwise has no end-to-end proof (docs/development/
//    plugin_integration.md, steps 2-3: register before app().run(), enable the
//    block in config, PayPlugin calls the factory with it and freezes).
//
// marker() carries back the config block the factory was handed, so an assembly
// test can tell "PayPlugin passed my channel's config" from "a default was
// used". Reaching it through dynamic_pointer_cast is the SPI's documented way to
// touch channel-only state.
// ============================================================================

#include <drogon_pay/ChannelRegistry.h>
#include <string>
#include <utility>

namespace pay::test_util
{

/// Registry keys the test host installs a channel under (see the
/// registerHostChannelFactories() call in tests/main.cc) and the assembly test
/// looks up. One declaration each side, so they cannot drift apart.
inline const std::string kHostChannelName = "host-test";
inline const std::string kBrokenHostChannelName = "host-broken";
/// Value the assembly test expects back from the channel's own config block.
inline const std::string kHostChannelMarker = "built-by-host-factory";

class StubChannel : public drogon_pay::PaymentChannel
{
  public:
    explicit StubChannel(std::string name, std::string marker = std::string{})
        : name_(std::move(name)), marker_(std::move(marker))
    {
    }

    /// Factory form: the channel's own config block, as PayPlugin passes it.
    static std::shared_ptr<StubChannel> fromConfig(const Json::Value &config)
    {
        return std::make_shared<StubChannel>(
          config.get("name", "stub").asString(), config.get("marker", "").asString()
        );
    }

    const std::string &marker() const
    {
        return marker_;
    }

    const std::string &name() const override
    {
        return name_;
    }

    bool isConfigured() const override
    {
        return false;
    }

    void createPayment(const Json::Value &, JsonCallback &&callback) override
    {
        callback(Json::Value{}, "stub: not implemented");
    }

    void createQRPayment(const Json::Value &, JsonCallback &&callback) override
    {
        callback(Json::Value{}, "stub: not implemented");
    }

    void queryPayment(const std::string &, JsonCallback &&callback) override
    {
        callback(Json::Value{}, "stub: not implemented");
    }

    void refund(const Json::Value &, JsonCallback &&callback) override
    {
        callback(Json::Value{}, "stub: not implemented");
    }

    void queryRefund(const std::string &, JsonCallback &&callback) override
    {
        callback(Json::Value{}, "stub: not implemented");
    }

    bool verifyCallback(
      const drogon::HttpRequestPtr &,
      drogon_pay::CallbackEvent &,
      std::string &error
    ) override
    {
        error = "stub: no verification";
        return false;
    }

    void onStart() override
    {
        ++starts_;
    }

    void onStop() override
    {
        ++stops_;
    }

    int starts() const
    {
        return starts_;
    }

    int stops() const
    {
        return stops_;
    }

  private:
    std::string name_;
    std::string marker_;
    int starts_{0};
    int stops_{0};
};

}  // namespace pay::test_util
