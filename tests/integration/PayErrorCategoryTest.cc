/// =============================================================================
/// @file   PayErrorCategoryTest.cc
/// @brief  P3-7.2: Verify PayErrorCategory singleton, name, and message mapping.
/// =============================================================================

#include "drogon_pay/PayErrorCategory.h"
#include <drogon/drogon_test.h>

DROGON_TEST(PayErrorCategory_Singleton)
{
    auto &a = pay::PayErrorCategory::instance();
    auto &b = pay::PayErrorCategory::instance();
    CHECK(&a == &b);
}

DROGON_TEST(PayErrorCategory_Name)
{
    // Error category name should be "pay"
    CHECK(std::string(pay::PayErrorCategory::instance().name()) == std::string("pay"));
}

DROGON_TEST(PayErrorCategory_DefaultMessage)
{
    std::string msg = pay::PayErrorCategory::instance().message(9999);
    CHECK(msg.find("pay error") != std::string::npos);
    CHECK(msg.find("9999") != std::string::npos);
}

DROGON_TEST(PayErrorCategory_RegisteredMessage)
{
    auto ec = pay::makePayError(1501, "WeChat client not ready");
    CHECK(ec.value() == 1501);
    CHECK(ec.message() == "WeChat client not ready");
}
