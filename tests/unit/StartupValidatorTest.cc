#include <drogon/drogon_test.h>
#include "utils/StartupValidator.h"
#include <cstdlib>

DROGON_TEST(StartupValidator_IsPlaceholder_EnvVarSyntax)
{
    CHECK(StartupValidator::isPlaceholder("__env_var:PAY_DB_PASSWORD__"));
    CHECK(StartupValidator::isPlaceholder("__env_var:FOO__"));
}

DROGON_TEST(StartupValidator_IsPlaceholder_ShellSyntax)
{
    CHECK(StartupValidator::isPlaceholder("${PAY_DB_PASSWORD}"));
    CHECK(StartupValidator::isPlaceholder("${FOO}"));
}

DROGON_TEST(StartupValidator_IsPlaceholder_Empty)
{
    CHECK(StartupValidator::isPlaceholder(""));
}

DROGON_TEST(StartupValidator_IsPlaceholder_NormalValue)
{
    CHECK(!StartupValidator::isPlaceholder("actual_password_123"));
    CHECK(!StartupValidator::isPlaceholder("123456"));
}

DROGON_TEST(StartupValidator_ValidateRequired_MissingVar)
{
#ifdef _WIN32
    _putenv_s("PAY_TEST_MISSING_VAR", "");
#else
    unsetenv("PAY_TEST_MISSING_VAR");
#endif

    auto result = StartupValidator::validateRequired({"PAY_TEST_MISSING_VAR"});
    CHECK(!result.ok);
    CHECK(result.missingVars.size() == 1);
    CHECK(result.missingVars[0] == "PAY_TEST_MISSING_VAR");
}

DROGON_TEST(StartupValidator_ValidateRequired_PresentVar)
{
#ifdef _WIN32
    _putenv_s("PAY_TEST_PRESENT_VAR", "some_value");
#else
    setenv("PAY_TEST_PRESENT_VAR", "some_value", 1);
#endif

    auto result = StartupValidator::validateRequired({"PAY_TEST_PRESENT_VAR"});
    CHECK(result.ok);
    CHECK(result.missingVars.empty());
}

DROGON_TEST(StartupValidator_ValidateRequired_PlaceholderVar)
{
#ifdef _WIN32
    _putenv_s("PAY_TEST_PLACEHOLDER_VAR", "__env_var:STILL_PLACEHOLDER__");
#else
    setenv("PAY_TEST_PLACEHOLDER_VAR", "__env_var:STILL_PLACEHOLDER__", 1);
#endif

    auto result = StartupValidator::validateRequired({"PAY_TEST_PLACEHOLDER_VAR"});
    CHECK(!result.ok);
    CHECK(result.missingVars.size() == 1);
}

namespace
{
// The config shape PayPlugin reads: a top-level plugins array whose PayPlugin
// entry carries the channels map.
Json::Value configWithAlipayChannel(bool enabled, const std::string &app_id)
{
    Json::Value channel;
    channel["enabled"] = enabled;
    channel["app_id"] = app_id;

    Json::Value payConfig;
    payConfig["channels"]["alipay"] = channel;

    Json::Value plugin;
    plugin["name"] = "PayPlugin";
    plugin["config"] = payConfig;

    Json::Value root;
    root["plugins"].append(plugin);
    return root;
}
}  // namespace

DROGON_TEST(StartupValidator_ChannelReadiness_WarnsWhenAppIdMissing)
{
    // An unset environment variable resolves to an empty string, and an
    // unresolved placeholder is equally unusable; both have to be reported.
    for (const char *app_id : {"", "__env_var:ALIPAY_SANDBOX_APP_ID__"})
    {
        const auto warnings =
          StartupValidator::validateChannelReadiness(configWithAlipayChannel(true, app_id));
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].find("alipay") != std::string::npos);
        CHECK(warnings[0].find("app_id") != std::string::npos);
    }
}

DROGON_TEST(StartupValidator_ChannelReadiness_SilentWhenUsableOrDisabled)
{
    CHECK(
      StartupValidator::validateChannelReadiness(configWithAlipayChannel(true, "2021000000000000"))
        .empty()
    );
    CHECK(StartupValidator::validateChannelReadiness(configWithAlipayChannel(false, "")).empty());

    // A config without the plugin (or without channels) is not a warning case:
    // the plugin itself already reports a missing channels block.
    CHECK(StartupValidator::validateChannelReadiness(Json::Value(Json::objectValue)).empty());
}
