/// =============================================================================
/// @file   PayPluginConfigTest.cc
/// @brief  PayPlugin's pre-1.0 config schema guard: a removed top-level key must
///         refuse startup by name, and it must refuse *first* -- before the
///         plugin reads anything else out of the config or reaches for a
///         database client.
///
/// These are the only cases in the suite that construct a PayPlugin directly,
/// which is safe precisely because the refusal is step 0 of initAndStart(): no
/// listener, no DbClient, no route registration is touched. The basePath check is
/// what pins that ordering claim, so a guard that drifted below the config read
/// would show up as an adopted base path rather than as a silent pass.
/// =============================================================================

#include <drogon/drogon_test.h>
#include <drogon_pay/PayPlugin.h>
#include <exception>
#include <json/json.h>
#include <string>

namespace
{
struct Refusal
{
    bool refused{false};
    std::string message;
    std::string basePath;
};

/// A config that is invalid only for carrying one of the removed keys. The
/// base_path is a probe: adopting it would mean the guard ran too late.
Json::Value legacyKeyConfig(const std::string &legacyKey)
{
    Json::Value config;
    config["base_path"] = "/legacy/schema/probe";
    config[legacyKey]["app_id"] = "irrelevant-to-the-guard";
    return config;
}

Refusal refuse(const Json::Value &config)
{
    PayPlugin plugin;
    Refusal refusal;
    try
    {
        plugin.initAndStart(config);
    }
    catch (const std::exception &e)
    {
        refusal.refused = true;
        refusal.message = e.what();
    }
    refusal.basePath = plugin.basePath();
    return refusal;
}
}  // namespace

DROGON_TEST(PayPluginConfig_LegacyWechatKeyRefusedBeforeAnySetup)
{
    const Refusal refusal = refuse(legacyKeyConfig("wechat_pay"));
    REQUIRE(refusal.refused);
    // The exception names the offending key; the old->new mapping is the paired
    // LOG_ERROR's job, so only the key is assertable from here.
    CHECK(refusal.message.find("wechat_pay") != std::string::npos);
    // initAndStart() never got as far as reading base_path.
    CHECK(refusal.basePath == "/api/pay");
}

DROGON_TEST(PayPluginConfig_LegacyAlipayKeyRefusedBeforeAnySetup)
{
    const Refusal refusal = refuse(legacyKeyConfig("alipay_sandbox"));
    REQUIRE(refusal.refused);
    CHECK(refusal.message.find("alipay_sandbox") != std::string::npos);
    CHECK(refusal.basePath == "/api/pay");
}
