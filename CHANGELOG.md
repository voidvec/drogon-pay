# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Docs/AI-config drift guard** (`scripts/check_docs_drift.py`, CI hard
  gate): keeps the `AGENTS.md` asset inventory in sync with `.claude/`,
  rejects backticked paths that don't exist in governance docs, and bans
  gtest vocabulary outside archived history.
- **`scripts/clang_format.py`**: single pinned clang-format major (22) for
  CI, the agent PostToolUse hook and pre-commit — previously three
  consumers used three different versions (CI 22 / pre-commit 17 / bare
  PATH `clang-format`), which produced spurious formatting drift.
- **`DROGON_PAY_WERROR` build option** (`cmake/Warnings.cmake`,
  `pay_apply_warnings()`): opt-in hard warning bar (/W4 /WX on MSVC,
  -Wall -Wextra -Werror elsewhere) applied to first-party targets only
  (library, example host, tests) and PRIVATE so consumers are unaffected.
  All three CI platforms configure with it ON. The drogon_ctl-generated
  ORM models were split into a `drogon_pay_models` OBJECT library that
  keeps the advisory profile — generated code must not be hand-edited to
  satisfy the gate.
- **clang-tidy two-tier gate** (`scripts/clang_tidy_gate.py`, new CI job
  `clang-tidy` on Linux): `.clang-tidy` stays advisory while a promoted
  subset of bugprone/performance checks runs with `--warnings-as-errors`
  as a hard gate over first-party, non-model translation units. The
  promote list only grows (0-finding checks first; `--report` prints hit
  counts for the next candidates), and unknown check names fail the gate
  instead of being silently dropped by clang-tidy.
- **Test suites split into `tests/unit/` (pure logic) and
  `tests/integration/` (HTTP/DB/Redis surface)**, guarded by
  `scripts/check_test_layout.py` (CI `static-analysis` step): `*Test.cc`
  naming, single `DROGON_TEST_MAIN` (`tests/main.cc`), no DROGON_TEST
  outside `tests/`, explicit CMake registration. HTTP e2e smoke scripts
  moved to `examples/pay-server/scripts/`. `tests/run_all_tests.ps1` and
  `ultra_simple.ps1` were retired — ctest now runs the binary directly on
  all three platforms. A full `DROGON_PAY_WERROR=ON` rebuild also exposed
  (and fixed) pre-existing gate breaks in the test target: one unused
  variable, missing `/utf-8`, and OpenSSL 3.0 deprecation warnings from
  the test RSA fixtures (now suppressed target-wide).
- **Line-coverage pipeline** (`cmake/Coverage.cmake` +
  `DROGON_PAY_COVERAGE` + `linux-coverage` preset +
  `scripts/measure_coverage.py` + `.github/workflows/coverage.yml`):
  Debug+gcov instrumented build (GCC/Clang only, models excluded), ctest
  run against service containers, then per-directory buckets
  (handlers/services/channels/utils/core + host-*) gated by a ratchet
  baseline (`scripts/coverage_baseline.json`, 0.5pp tolerance, small-bucket
  exemption, line-collapse detection, SEED on first run). The
  `TECH_SPECS.md` coverage claim is now backed by the gate instead of a
  verbal percentage.

### Changed

- **Log levels standardized to the six-tier Drogon taxonomy**
  (TRACE/DEBUG/INFO/WARN/ERROR/FATAL); see `TECH_SPECS.md` 「日志分级规范」.
  - `LOG_INFO` is now reserved for lifecycle/milestone events; per-request
    flow steps moved to `LOG_DEBUG`.
  - Fire-and-forget helper failures (ledger insert/lookup, idempotency
    snapshot write) moved from `LOG_ERROR` to `LOG_WARN` — these degrade
    audit/replay but do not fail the request.
  - Startup-exit paths (config load, env-var validation) moved from
    `LOG_ERROR` to `LOG_FATAL`.
  - **Ops impact:** if you alert on `LOG_ERROR` count via log aggregation
    (ELK/Loki), these fire-and-forget failures will no longer trigger that
    alert. Built-in Prometheus metric alerts (`HighErrorRate` in
    `docs/deployment/monitoring_setup.md`) are unaffected. For idempotency-
    snapshot failures (which affect retry correctness), the
    `clearReservation` path remains `LOG_ERROR` and is the recommended
    alert anchor. See `docs/development/logging_standards.md`.

## [1.0.0] - 2026-07-31

First release of `drogon-pay` as a reusable Drogon plugin library. The former
`PayBackend/` monolith was refactored into a Conan-distributable STATIC library
(`DrogonPay::DrogonPay`) plus an example host.

### Added

- **Channel SPI**: `drogon_pay::PaymentChannel` abstract interface
  (create/QR-create/query/refund/refund-query/verifyCallback/onStart/onStop)
  with a normalized `CallbackEvent`; contract requires thread safety and
  HttpClient reuse.
- **ChannelRegistry**: registration during `initAndStart`, frozen afterwards
  (lock-free runtime lookup). Hosts extend via
  `ChannelRegistry::registerFactory(name, factory)` before `app().run()`.
- **Configurable route prefix** (`base_path`, default `/api/pay`); all routes
  registered programmatically (safe in static-library hosts, no
  WHOLE_ARCHIVE needed).
- `drogon_pay::ensureLinked()` safety net for hosts whose linker drops the
  PayPlugin DrObject auto-registration symbol.
- **Packaging**: full Conan recipe (`drogon-pay/1.0.0`,
  `package_type=static-library`), CMake install/export
  (`find_package(DrogonPay)`), and `test_package/` consumer verification
  (plugin loads, routes reachable, clean shutdown).
- Host integration guide: `docs/development/plugin_integration.md`.
- CI: Windows ctest gate, gitleaks secret scanning, `conan create` smoke job.

### Changed

- **BREAKING — plugin config schema**: top-level `wechat_pay` /
  `alipay_sandbox` blocks were replaced by the `channels` map
  (`channels.wechat` / `channels.alipay`, each with `enabled`). Legacy keys
  are detected and the plugin refuses to start with a migration error. See
  the mapping table in `docs/development/plugin_integration.md`.
- **BREAKING — Redis is now opt-in**: the `redis_client` key must be set
  explicitly to enable the Redis idempotency cache; omitting it selects the
  database-only path. (Previously the plugin always queried the `default`
  Redis client, which corrupted drogon's RedisClientManager on hosts without
  a Redis config and crashed at shutdown.)
- Unknown/disabled channels now return `CHANNEL_NOT_AVAILABLE` instead of
  silently falling back to wechat.
- Channel HTTP clients are reused per IO loop
  (`drogon::IOThreadStorage<HttpClientPtr>` + keep-alive) instead of being
  created per request; wechat platform-certificate state moved to an atomic
  `shared_ptr` snapshot.
- All services are constructed inside `initAndStart` and immutable afterwards
  (removed the `callbackService()` lazy-init data race). Reconcile and
  certificate-refresh timers moved to a dedicated worker
  `trantor::EventLoopThread`.
- `PayAuthFilter` replaced by the `checkAuth(req)` function applied inside
  handlers (no drogon filter registration).
- Test hook `setTestClients(...)` superseded by
  `setTestChannels(map<string, PaymentChannelPtr>, dbClient)` (legacy adapter
  kept for compatibility).
- Repository layout: library in `libs/drogon-pay/`, example host in
  `examples/pay-server/`, admin console in `examples/pay-admin/`, tests in
  `tests/`, SQL migrations in root `sql/`.

### Removed

- `ADD_METHOD_TO` static route registration and the hardcoded `/api/pay/*`
  paths.
- The `/FI` / `-include` force-include hack for `orm_compat.h` (root cause
  fixed in the generated model headers).
- Dead gtest dependency (tests use Drogon's own `DROGON_TEST` framework).
- Duplicated inline CORS implementation in `main.cc` (single
  `SecurityHeaders.h` implementation, host-side).

[Unreleased]: https://github.com/lucaswang420/drogon-pay/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lucaswang420/drogon-pay/releases/tag/v1.0.0
