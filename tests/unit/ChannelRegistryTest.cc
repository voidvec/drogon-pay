/// =============================================================================
/// @file   ChannelRegistryTest.cc
/// @brief  The contract the two-channel routing stands on, measured at the
///         registry itself: an unknown name answers nullptr (there is never a
///         fallback channel), a null channel is refused, freeze() really ends
///         the registration phase, and a channel that owns no close API answers
///         an explicit refusal rather than silence.
/// =============================================================================

#include <drogon/drogon_test.h>
#include <drogon_pay/ChannelRegistry.h>
#include <string>
#include <vector>
#include "StubChannel.h"

using namespace drogon_pay;
using pay::test_util::StubChannel;

DROGON_TEST(ChannelRegistry_UnknownNameAnswersNullptr)
{
    ChannelRegistry registry;
    const auto alpha = std::make_shared<StubChannel>("alpha");
    const auto beta = std::make_shared<StubChannel>("beta");
    registry.add("alpha", alpha);
    registry.add("beta", beta);

    CHECK(registry.find("alpha") == alpha);
    CHECK(registry.find("beta") == beta);
    // The routing fact every service depends on: an unregistered name is a
    // miss, not the first channel that happens to exist.
    CHECK(registry.find("gamma") == nullptr);
    // Hoisted out of the macro: a braced list inside CHECK's argument would
    // split on its comma, and the preprocessor error is a wall of C2220 noise.
    const std::vector<std::string> registered{"alpha", "beta"};
    CHECK(registry.names() == registered);
    CHECK(registry.size() == 2);
}

DROGON_TEST(ChannelRegistry_NullChannelIsRefused)
{
    ChannelRegistry registry;
    registry.add("ghost", nullptr);

    CHECK(registry.size() == 0);
    CHECK(registry.find("ghost") == nullptr);
    CHECK(registry.names().empty());

    registry.add("real", std::make_shared<StubChannel>("real"));
    CHECK(registry.size() == 1);
}

DROGON_TEST(ChannelRegistry_AddAfterFreezeIsIgnored)
{
    ChannelRegistry registry;
    registry.add("before", std::make_shared<StubChannel>("before"));
    registry.freeze();
    CHECK(registry.frozen());

    registry.add("after", std::make_shared<StubChannel>("after"));

    // freeze() is what makes runtime find() lock-free, so a write that landed
    // after it would be a data race the type system can no longer catch.
    CHECK(registry.find("after") == nullptr);
    CHECK(registry.size() == 1);
    CHECK(registry.find("before") != nullptr);
}

DROGON_TEST(ChannelRegistry_StartAndStopReachEveryChannel)
{
    ChannelRegistry registry;
    const auto first = std::make_shared<StubChannel>("first");
    const auto second = std::make_shared<StubChannel>("second");
    registry.add("first", first);
    registry.add("second", second);

    registry.startAll();
    CHECK(first->starts() == 1);
    CHECK(second->starts() == 1);
    CHECK(first->stops() == 0);

    registry.stopAll();
    CHECK(first->stops() == 1);
    CHECK(second->stops() == 1);
}

DROGON_TEST(ChannelRegistry_HostFactoryBecomesAChannelOnDemand)
{
    // The factory map is process-wide and outlives this case, so the registered
    // lambda must not capture anything from the stack: fromConfig() is the same
    // body tests/main.cc installs for the host channel.
    ChannelRegistry::registerFactory("unit-test-channel", [](const Json::Value &config) {
        return StubChannel::fromConfig(config);
    });

    const auto &factories = ChannelRegistry::factories();
    CHECK(factories.count("unit-test-channel") == 1);
    CHECK(factories.count("no-such-channel") == 0);

    Json::Value config;
    config["name"] = "made-by-factory";
    config["marker"] = "config-block-passthrough";
    const auto created =
      std::dynamic_pointer_cast<StubChannel>(factories.at("unit-test-channel")(config));
    REQUIRE(created != nullptr);
    CHECK(created->name() == "made-by-factory");
    // The docs promise initAndStart calls the factory "with that channel's JSON
    // config block"; the marker only survives if the block really was passed.
    CHECK(created->marker() == "config-block-passthrough");
}

DROGON_TEST(ChannelRegistry_CloseOrderWithoutSupportIsAnExplicitRefusal)
{
    StubChannel channel("no-close-api");
    Json::Value result;
    std::string error = "unset";
    channel.closeOrder("order-1", [&](const Json::Value &answer, const std::string &e) {
        result = answer;
        error = e;
    });

    // The reconciliation sweeper casts to the concrete WeChat client for its
    // close call; a channel that never implemented the SPI must still answer,
    // because a silent callback would leave the caller waiting on a close that
    // was never attempted. The refusal rides on `error` alone -- the default
    // implementation carries no payload, which matches the one call site that
    // discards the result and branches on the error string.
    CHECK(error == "channel does not support closing orders");
    CHECK(result.isNull());
}
