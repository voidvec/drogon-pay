---
name: test-writer
description: Generates Drogon-compatible C++ tests for the Pay Plugin project. Focuses on payment processing coverage gaps and regression protection.
---

# Test Writer Agent

Generates C++ tests for the Pay Plugin project using the Drogon-native
`DROGON_TEST` framework. Focuses on payment coverage gaps and regression
protection.

## When to Use

Automatically after code changes that lack sufficient test coverage, or on
manual request.

## Test Framework & Patterns

### Framework

- Header: `#include <drogon/drogon_test.h>` (NOT gtest — there is no
  `TEST_F`/`EXPECT_EQ` in this repo)
- Test macro: `DROGON_TEST(Module_Scenario)` — one macro per case, no fixture
  classes
- Assertions: `CHECK(expr)` (non-fatal), `REQUIRE(expr)` (fatal),
  `CHECK_FALSE`, `CHECK_EQ`/`CHECK_THROWS` also exist but this repo mostly
  uses `CHECK`/`REQUIRE` with explicit comparisons
- Runner: single binary `PayBackendTests` built from `tests/`
  (`tests/main.cc` boots the Drogon app; tests run against the configured
  test port, see `TestConfigHelper.h`). New pure-logic files go to
  `tests/unit/`; anything touching the HTTP server, DB or Redis goes to
  `tests/integration/`.

### Async callback pattern (the house style)

Services are callback-based; tests bridge with `std::promise`/`future` and a
timeout, mirroring `tests/integration/CreatePaymentIntegrationTest.cc`:

```cpp
DROGON_TEST(PayPlugin_CreatePayment_WechatSuccess)
{
    // ... build request, wire test doubles (plugin.setTestClients(...)) ...

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->createPayment(
      request,
      apiKey,
      [&resultPromise, &errorPromise](const Json::Value &result,
                                      const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    CHECK(!errorFuture.get());
}
```

In production code (not tests) callbacks are captured as
`auto sharedCb = std::make_shared<CallbackType>(std::move(cb));` and lambdas
capture `[sharedCb]` — never `[this]` or `[&var]`.

### Service API

Tests use the Service API, never the legacy Plugin API:

```cpp
auto paymentService = plugin.paymentService();
auto refundService = plugin.refundService();
```

## Checklist

Before writing tests, verify:
- [ ] Existing tests in the same module for pattern consistency
- [ ] Success AND error paths (invalid API key, payment not found, refund
      amount exceeds paid)
- [ ] Every async callback path is bounded by a `wait_for` timeout +
      `REQUIRE(... == ready)` so a dropped callback fails, never hangs
- [ ] DB rows inserted by the test are deleted at the end of the case
- [ ] No hardcoded credentials or environment-specific values (use
      `PAY_API_KEY=test_key_123456` style env defaults already in CI)
- [ ] Idempotency behavior tested for payment create and refund
- [ ] `tests/CMakeLists.txt` updated if a new test file is added
      (explicit list; file registered under the unit or integration section
      matching its dependencies)

## Naming Convention

`DROGON_TEST({Module}_{Scenario})` — module is the class/service under test,
scenario describes the expectation:

- `PayUtils_ParseAmountToFen`
- `PayPlugin_CreatePayment_IdempotencySnapshot`
- `RefundQuery_AmountExceedsPaid_ReturnsError`
- `PayIdempotency_DbUniqueKey`
- `RouteRegistration_PayEndpointsRespond`

## Quick Reference

```bash
# Run the whole suite (ctest, cross-platform)
ctest --test-dir build/windows-msvc -C Release --output-on-failure

# Run one case by name
build/windows-msvc/tests/Release/PayBackendTests.exe -r PayUtils_ParseAmountToFen

# Linux/macOS
ctest --test-dir build/linux-release --output-on-failure
```
