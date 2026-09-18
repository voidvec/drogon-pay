#include <drogon/drogon_test.h>

#include "handlers/PayAuthMetrics.h"

DROGON_TEST(PayAuthMetrics_SnapshotIncrements)
{
    const auto before = PayAuthMetrics::snapshot();

    PayAuthMetrics::incMissingKey();
    PayAuthMetrics::incInvalidKey();
    PayAuthMetrics::incScopeDenied();
    PayAuthMetrics::incNotConfigured();

    const auto after = PayAuthMetrics::snapshot();

    CHECK(after["missing_key"].asUInt64() == before["missing_key"].asUInt64() + 1);
    CHECK(after["invalid_key"].asUInt64() == before["invalid_key"].asUInt64() + 1);
    CHECK(after["scope_denied"].asUInt64() == before["scope_denied"].asUInt64() + 1);
    CHECK(after["not_configured"].asUInt64() == before["not_configured"].asUInt64() + 1);
}
