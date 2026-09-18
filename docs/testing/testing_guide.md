# Pay Plugin Testing Guide

Tests live in the repository-root `tests/` directory and use Drogon's own
`DROGON_TEST` framework (not gtest). The test binary is built by
`tests/CMakeLists.txt` and registered with CTest.

## Test Structure

```
tests/
├── main.cc                       # DROGON_TEST_MAIN entry; overrides listener port
├── TestConfigHelper.h            # testPort() / baseUrl() helpers (PAY_TEST_PORT, default 5567)
├── CMakeLists.txt                # builds the test binary + ctest registration
├── unit/                         # pure logic — no HTTP/DB round-trip
│   ├── AuthCheckTest.cc          # auth / scope enforcement
│   ├── ConfigLoaderTest.cc
│   ├── ControllerMetricsTest.cc  # /metrics + auth metrics endpoints
│   ├── OnceCallbackTest.cc
│   ├── PayAuthMetricsTest.cc     # auth metrics counters + Prometheus format
│   ├── PayUtilsTest.cc
│   └── StartupValidatorTest.cc
└── integration/                  # exercise the running Drogon app, DB or HTTP surface
    ├── CallbackControllerTest.cc
    ├── CreatePaymentIntegrationTest.cc
    ├── HealthProbeTest.cc        # /healthz, /readyz probes
    ├── HttpResponseHeadersTest.cc # CORS preflight + security headers
    ├── IdempotencyIntegrationTest.cc
    ├── PayErrorCategoryTest.cc   # error-code → HTTP status mapping
    ├── QueryOrderListAndReconcileTest.cc
    ├── QueryOrderTest.cc
    ├── ReconcileSummaryTest.cc
    ├── RefundQueryTest.cc
    ├── RouteRegistrationSmokeTest.cc
    ├── WechatCallbackIntegrationTest.cc
    └── WechatPayClientTest.cc
```

HTTP-level e2e smoke scripts live with the host they exercise:
`examples/pay-server/scripts/e2e_test.sh` / `e2e_test.ps1`.

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

Or through the dev wrappers, which pick the preset for you, refuse to run
before a build exists, and refuse to call an empty suite a pass:
`examples/pay-server/scripts/test.sh` (Windows:
`examples\pay-server\scripts\test.bat`), with `-l` to list case names,
`-r <ExactName>` to run one case, `-v` for per-test output and `-o` to also
keep `test_results.log` in the build directory.

### A single test

`tests/CMakeLists.txt` registers **one** CTest case (`PayBackendTests`) that
runs every `DROGON_TEST` in a single process from the binary's own output
directory, so the `./config.json` and `./.env` resolve and one process exit code
propagates to CTest unchanged. CTest therefore cannot select an individual
case: `ctest -R RefundQuery` matches nothing, prints "No tests were found!!!"
and exits **0**, which reads as a pass. Filter through the test binary instead —
its `-r` takes an exact case name and exits 1 when no case matches:

```bash
examples\pay-server\scripts\test.bat -l                    # list exact names
examples\pay-server\scripts\test.bat -r PayIdempotency_RedisSetNx
./build/windows-msvc/tests/Release/PayBackendTests -r PayIdempotency_RedisSetNx   # direct
```

The direct form must run from the binary's own directory, which is where
`build.bat` / `build.sh` put the `config.json` and `.env` the suite loads; the
wrappers already `cd` there for you.

### End-to-end HTTP smoke scripts

```bash
cd examples/pay-server/scripts
./e2e_test.sh        # Bash
./e2e_test.ps1       # PowerShell
```

## Writing Tests

Place a new file under `tests/unit/` when it only calls functions/classes
directly, and under `tests/integration/` when it talks to the in-process
Drogon server, the database or Redis. Register it in the matching section of
`tests/CMakeLists.txt` (explicit list, no glob).

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

- Build the test schema with the migration executor, not by hand:
  `examples/pay-server/scripts/setup_database.sh` (or `.bat`) resets the schema
  and replays `sql/NNN_*.sql` through `scripts/migrate_db.py`; add `--keep-data`
  to apply only what is missing
- The test config points at a throwaway database; isolate per CI run
- External provider calls are stubbed via test channels (no real network)
