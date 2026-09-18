#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <algorithm>
#include <string>
#include <vector>

// Phase 3 smoke assertion: PayPlugin registers all pay routes
// programmatically in initAndStart (no ADD_METHOD_TO macros). This guards
// against the classic static-library failure mode where route registration
// symbols are silently dropped by the linker (builds fine, 404 at runtime).
DROGON_TEST(RouteRegistrationSmoke)
{
    std::vector<std::string> registeredPaths;
    for (const auto &info : drogon::app().getHandlersInfo())
    {
        registeredPaths.push_back(std::get<0>(info));
    }

    const auto isRegistered = [&](const std::string &path) {
        return std::find(registeredPaths.begin(), registeredPaths.end(), path) !=
               registeredPaths.end();
    };

    // Routes registered by PayPlugin::registerHttpHandlers under base_path
    CHECK(isRegistered("/api/pay/create"));
    CHECK(isRegistered("/api/qrpay/create"));
    CHECK(isRegistered("/api/pay/query"));
    CHECK(isRegistered("/api/pay/refund"));
    CHECK(isRegistered("/api/pay/refund/query"));
    CHECK(isRegistered("/api/pay/orders"));
    CHECK(isRegistered("/api/pay/reconcile/summary"));
    CHECK(isRegistered("/api/pay/metrics/auth"));
    CHECK(isRegistered("/api/pay/metrics/auth.prom"));
    CHECK(isRegistered("/api/pay/notify/wechat"));
    CHECK(isRegistered("/api/pay/notify/alipay"));
}
