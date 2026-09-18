#include <drogon/drogon_test.h>
#include "utils/ConfigLoader.h"

DROGON_TEST(ConfigLoader_MaskSensitive_Normal)
{
    std::string result = ConfigLoader::maskSensitive("abcdefghijkl");
    CHECK(result == "abcd***ijkl");
}

DROGON_TEST(ConfigLoader_MaskSensitive_Short)
{
    std::string result = ConfigLoader::maskSensitive("abc");
    CHECK(result == "***");
}

DROGON_TEST(ConfigLoader_MaskSensitive_Empty)
{
    std::string result = ConfigLoader::maskSensitive("");
    CHECK(result == "***");
}

DROGON_TEST(ConfigLoader_MaskSensitive_ExactBoundary)
{
    std::string result = ConfigLoader::maskSensitive("12345678");
    CHECK(result == "***");
}

DROGON_TEST(ConfigLoader_MaskSensitive_LongKey)
{
    std::string result = ConfigLoader::maskSensitive("sk_live_abcdef123456");
    CHECK(result == "sk_l***3456");
}
