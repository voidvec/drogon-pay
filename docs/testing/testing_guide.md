# Pay Plugin Testing Guide

Tests live in the repository-root `tests/` directory and use Drogon's own
`DROGON_TEST` framework (not gtest). The test binary is built by
`tests/CMakeLists.txt` and registered with CTest.

## Test Structure

```
tests/
├── test_main.cc                  # DROGON_TEST_MAIN entry; overrides listener port
├── TestConfigHelper.h            # testPort() / baseUrl() helpers (PAY_TEST_PORT, default 5567)
├── CMakeLists.txt                # builds the test binary + ctest registration
├── AuthCheckTest.cc              # auth / scope enforcement
├── PayAuthMetricsTest.cc         # auth metrics counters + Prometheus format
├── PayErrorCategoryTest.cc       # error-code → HTTP status mapping
├── CreatePaymentIntegrationTest.cc
├── QueryOrderTest.cc
├── QueryOrderListAndReconcileTest.cc
├── ReconcileSummaryTest.cc
├── RefundQueryTest.cc
├── IdempotencyIntegrationTest.cc
├── CallbackControllerTest.cc
├── WechatCallbackIntegrationTest.cc
├── WechatPayClientTest.cc
├── HealthProbeTest.cc            # /healthz, /readyz probes
├── HttpResponseHeadersTest.cc    # CORS preflight + security headers
├── ControllerMetricsTest.cc      # /metrics + auth metrics endpoints
├── RouteRegistrationSmokeTest.cc
├── ConfigLoaderTest.cc
├── StartupValidatorTest.cc
├── OnceCallbackTest.cc
├── PayUtilsTest.cc
├── e2e_test.sh / e2e_test.ps1    # HTTP-level smoke scripts
└── run_all_tests.ps1
```

## Running Tests

Tests are driven by CTest. They require PostgreSQL (and Redis when the
`redis_client` key is present in the loaded config). The test binary loads a
copy of `examples/pay-server/config.json` but overrides the listener port to
`PAY_TEST_PORT` (default **5567**) so it never collides with a locally running
dev `PayServer` on 5566.

### All tests

```bash
ctest --test-dir build/windows-msvc -C Release        # Windows
ctest --test-dir build/linux-release                  # Linux/macOS
```

### A single test (by CTest name)

```bash
ctest --test-dir build/windows-msvc -C Release -R RefundQuery
```

### Direct binary (verbose)

```bash
./build/windows-msvc/tests/Release/PayBackendTests
```

### End-to-end HTTP smoke scripts

```bash
cd tests
./e2e_test.sh        # Bash
./e2e_test.ps1       # PowerShell
```

## Writing Tests

Tests use the `DROGON_TEST` macros (`TEST`, assertions via the framework).
Inject test channels with `PayPlugin::setTestChannels(...)` (the legacy
`setTestClients(...)` adapter is kept for compatibility). Listener/client base
URLs should be built from `pay::test_util::baseUrl()` rather than hardcoding a
port, so tests follow `PAY_TEST_PORT`.

## Test Coverage Goals

- Service / handler coverage across create / query / refund / callback paths
- Integration tests: all critical paths through the real Drogon HTTP layer
- Edge cases: error handling, validation, idempotency, error-code mapping
- Auth: missing / invalid key, scope denial, not-configured

## CI/CD Integration

Tests run automatically on:
- Every pull request (Windows / Linux / macOS CI)
- The Windows CI gates merges on a green CTest run

## Test Data Management

- Use a test database schema (run `sql/000_drop_pay_tables.sql` reset, then
  `sql/001_init_pay_tables.sql` … `sql/004_ledger_fk.sql` setup)
- The test config points at a throwaway database; isolate per CI run
- External provider calls are stubbed via test channels (no real network)
