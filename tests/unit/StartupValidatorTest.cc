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
