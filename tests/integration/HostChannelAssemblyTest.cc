/// =============================================================================
/// @file   HostChannelAssemblyTest.cc
/// @brief  The host channel SPI, end to end: factories registered before the app
///         starts are consumed by PayPlugin::initAndStart with their own config
///         block, a built-in channel cannot be shadowed by a factory of the same
///         name, and a factory that throws is skipped without taking payment
///         assembly down with it.
///
/// The registration itself lives in tests/main.cc, which is the host here. These
/// cases only read what the plugin ended up with, which is the half no other
/// test could see: everything upstream of assembly is a unit-level registry
/// question (tests/unit/ChannelRegistryTest.cc).
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon_pay/PayPlugin.h>
#include <memory>
#include "StubChannel.h"

using namespace drogon;
using pay::test_util::StubChannel;

DROGON_TEST(HostChannel_AssemblyBuildsTheHostChannelFromItsOwnConfigBlock)
{
    const auto plugin = app().getPlugin<PayPlugin>();
    REQUIRE(plugin != nullptr);

    const auto hostChannel =
      std::dynamic_pointer_cast<StubChannel>(plugin->channel(pay::test_util::kHostChannelName));
    REQUIRE(hostChannel != nullptr);
    CHECK(hostChannel->name() == pay::test_util::kHostChannelName);
    // The docs promise initAndStart calls the factory "with that channel's JSON
    // config block". Only a value that travels through the block proves it.
    CHECK(hostChannel->marker() == pay::test_util::kHostChannelMarker);
}

DROGON_TEST(HostChannel_BuiltInSurvivesAFactoryRegisteredUnderItsName)
{
    const auto plugin = app().getPlugin<PayPlugin>();
    REQUIRE(plugin != nullptr);

    const auto wechat = plugin->channel("wechat");
    REQUIRE(wechat != nullptr);
    // The registry key is the same either way, so the type is what tells the
    // built-in from the host's stub: an unguarded factory would have replaced it.
    CHECK(std::dynamic_pointer_cast<StubChannel>(wechat) == nullptr);
    CHECK(wechat->name() == "wechat");
}

DROGON_TEST(HostChannel_FailingFactoryIsSkippedWithoutUndoingAssembly)
{
    const auto plugin = app().getPlugin<PayPlugin>();
    REQUIRE(plugin != nullptr);

    CHECK(plugin->channel(pay::test_util::kBrokenHostChannelName) == nullptr);
    // Positive control: "skipped" must not mean "assembly aborted". Both
    // built-ins are still routed, and so is the host channel that came before.
    CHECK(plugin->channel("alipay") != nullptr);
    CHECK(plugin->channel("wechat") != nullptr);
    CHECK(plugin->channel(pay::test_util::kHostChannelName) != nullptr);
}
