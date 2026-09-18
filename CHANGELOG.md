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
- **Single-entry CI pipeline** (`.github/workflows/ci.yml` + reusable
  `_build-test.yml` / `_sdk-smoke.yml`): FAST (`static-analysis`, parallel
  `clang-tidy`) → MAIN (`build-test` matrix over linux/windows/macos) →
  RELEASE (`sdk-smoke` matrix over linux/windows), chained by `needs`, with
  `concurrency` cancelling superseded runs. The three required check names
  (`linux-build-and-test`, `windows-build-and-test`, `macos-build`) are
  unchanged and now come from `matrix.check_name`. Actions are pinned to
  full commit SHAs. The pre-Conan build-Drogon-from-source jobs moved to
  dispatch-only `legacy-source-build.yml`. The old `ci-linux.yml` /
  `ci-windows.yml` / `ci-macos.yml` / `conan-create.yml` still run beside
  the new pipeline for one verification cycle and are deleted afterwards.
- **macOS CI runs the test suite instead of building only.** The arm64 leg
  provisions a throwaway Postgres cluster (`initdb -A trust` under
  `$RUNNER_TEMP`) and a daemonized `redis-server`, applies the migration chain
  and runs the same `PayBackendTests` ctest entry as the other platforms.
  Previously a platform-specific regression could only be caught by hand.
- **Linux CI applies the whole migration chain** (`sql/001`–`004`): the
  per-platform workflow it replaces hardcoded only `001` and `002`, and its
  Postgres readiness loop fell through to a green step when the probe never
  succeeded. Readiness now probes `SELECT 1`, hard-fails on timeout and dumps
  the container log.
- **OpenAPI 3.0 contract** (`examples/pay-server/openapi.yaml`): all 11 plugin
  routes and 4 host routes documented with request/response schemas, the
  business-code → HTTP-status mapping, scope requirements and the two
  channel-facing notify bodies (marked as channel conventions, not this
  service's contract). `docs/api/pay-api-examples.md` gains the two endpoints
  it never documented (`/api/pay/orders`, `/api/pay/reconcile/summary`) plus
  the Alipay callback.
- **OpenAPI route gate** (`scripts/check_openapi_routes.py`, two
  `static-analysis` steps): parses the `registerHandler`/`ADD_METHOD_TO`
  call sites — including `basePath_ + "/x"` concatenation, the `qrPath`
  variable and the ternary that pins `/api/qrpay/create` — and diffs the
  resulting `METHOD /path` set against the spec paths in both directions,
  then checks the auth posture of each pair (`authed()` routes may not be
  documented as public, `OPTIONS` must be). A contract that merely omits a
  route fails; so does an `EXCLUSIONS` entry without a reason and a `$ref`
  with no definition. Stdlib-only, because the FAST gate must not depend on
  PyYAML being present on the runner. The `openapi-update` skill was
  rewritten around this gate (both mirrors).
- **Migration executor** (`scripts/migrate_db.py`, stdlib-only, shells out to
  `psql`): the only code that knows which files exist. It discovers
  `sql/NNN_*.sql`, applies what is missing in version order, commits each
  migration together with its `schema_migrations` row (version / filename /
  sha256 / applied_at) inside one transaction, refuses to run when an applied
  version's bytes changed, warns when recorded tables were dropped out-of-band,
  and adds `--status` / `--dry-run` / `--baseline` (adopt a database an
  `initdb.d` mount already provisioned, rejected unless the tables are really
  there) / `--reset-schema --confirm-drop <db>`. `--reset-schema` accepts a
  loopback host only: `setup_database.{sh,bat}` fills in `--confirm-drop` for
  the operator, so the repeated name is not a human decision and a remote host
  is where staging and production live. Creating or dropping the
  *database* stayed out of it on purpose — the app role has no `CREATEDB`, so a
  failed `DROP DATABASE` cannot be undone by the same connection; the executor
  only probes `pg_database` and prints the superuser command.
- **Migration hygiene guard** (`scripts/check_migrations.py`, CI
  `static-analysis` step): naming, an unbroken version chain, idempotence and
  non-destruction, plus a sha256 pin of history in
  `scripts/migrations_baseline.json`. Content rules apply only to migrations
  that are not yet baselined — `001`–`004` predate the guard and are pinned as
  they are, while a new file must pass. `--write-missing` pins new versions,
  never rewrites an existing entry, and refuses a candidate that breaks the
  content rules (pinning is a permanent exemption, so it cannot double as a
  waiver).
- **`examples/pay-server/scripts/setup_database.sh`**, the POSIX twin of
  `setup_database.bat`, and the `.bat` lost its embedded default password: both
  now reset the schema, replay the chain through the executor and read
  credentials from the environment or `examples/pay-server/.env`.
- **`examples/pay-server/scripts/build.sh` and `test.sh`**, the POSIX twins of
  `build.bat` / `test.bat`. Same flags (`-debug` / `-release`; `-l`,
  `-r <pattern>`, `-v`, `-o` on the test side), same exit codes, and they
  resolve `uname` to the matching preset instead of asking the reader to paste a
  four-line conan+cmake incantation. New `linux-debug` and `macos-debug` CMake
  presets back the `-debug` flag on Unix — previously only Windows had a debug
  preset, so the documented `-debug` was a Windows-only option. `test.bat` also
  dropped its `DB_HOST/DB_PORT/DB_NAME/DB_USER/DB_PASS=123456` block: no test
  reads those names (the suite loads `.env` through `ConfigLoader`), so it was a
  plausible-looking plaintext credential that did nothing. Every tracked
  `examples/pay-server/**/*.sh` is now mode 100755 — the docs (and these scripts'
  own headers) have always shown a bare `examples/pay-server/scripts/setup_database.sh`
  invocation, which a 0644 checkout rejects.
- **Version sync guard** (`scripts/check_version_sync.py`): the version is
  declared in `CMakeLists.txt`, `conanfile.py` and `examples/pay-admin/package.json`,
  and nothing else may restate it. Bare mode asserts the three agree; `--tag
  vX.Y.Z` additionally requires the tag to equal them and `CHANGELOG.md` to
  already carry that section. Six `# Version: 1.0.0` comment lines in
  `examples/pay-server/deploy/` were deleted as the drift they had already
  caused.

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

- **The `v*` release pipeline is now a gate, not a formality.** `release.yml`
  opens with a `version-check` job (five minutes, no compiler) that fails a tag
  whose version is not what the tree declares or whose `CHANGELOG.md` section was
  never written, replaces its Windows-only `conan create` step with the same
  `_sdk-smoke.yml` the RELEASE gate of `ci.yml` uses — so the tag path exercises
  the plugin routes on Linux and Windows instead of only building on one — and
  `publish` now depends on that. The hand-run `gh release create` recipe is gone
  with it: `/release` describes the tag-and-watch-CI flow instead of telling you
  to `git log > CHANGELOG.md`, which would have thrown away the changelog and
  left the release body blank. The workflow asks for `contents: read` and grants
  `write` to `publish` alone, and its changelog extraction fails the job when it
  yields nothing instead of publishing a release with a blank body.
- **`build-test` waits for `clang-tidy`, not only `static-analysis`.** MAIN
  depended on one FAST job, so the promoted tidy batch was advisory in
  practice: red on a check nothing depends on still merges. The `needs` edge
  makes it a blocker regardless of which contexts the branch ruleset requires.
- **Every workflow now states what its token may do, and no workflow runs an
  action that is not frozen.** `ci.yml`, `_build-test.yml`, `_sdk-smoke.yml`,
  `coverage.yml` and `deploy.yml` declare `permissions: contents: read` at
  workflow level instead of inheriting the repository default, and the
  remaining floating refs (`coverage.yml`, `deploy.yml`, `secrets-scan.yml`)
  are pinned to full commit SHAs with their tag in a comment — the same rule
  the `ci.yml` pipeline already followed. Each SHA was resolved through the
  tag ref API and cross-checked against the pins already in use, so the two
  spellings of `actions/checkout` in this repository name one commit.
  `secrets-scan.yml` keeps its inherited grants on purpose: gitleaks posts a
  commit status, and narrowing it without a run to observe is how a security
  gate goes quiet.

### Fixed

- **Documentation contradicted the code on money, statuses and routes.**
  Writing the contract surfaced four stale claims, now corrected against the
  implementation:
  - Amounts were documented as `BIGINT` cents/fen
    (`libs/drogon-pay/src/models/README.md`, the `openapi-update` skill and
    `.claude/agents/api-documenter.md`). They are `VARCHAR(32)` decimal
    **strings in yuan units**, validated by the controller regex
    `^\d+(\.\d{1,2})?$`.
  - `TECH_SPECS.md` 「订单状态机」 named states the code never writes
    (`SUCCESS` for orders, `REFUND_PROCESSING` / `REFUND_FAILED`). The tables
    now list the values produced by `PayUtils.cc` and `services/*.cc`, and flag
    the one real inconsistency left in code: WeChat maps a failed payment to
    `FAIL` while the Alipay `TRADE_CLOSED` branch maps it to `FAILED`.
  - `.claude/agents/api-documenter.md` and the `docker-integration-test` skill
    (both mirrors, including `scripts/pay_e2e_test.py`) drove a fictional
    `/api/v1/payments` surface with numeric amounts and no `user_id`; every one
    of those requests would have been a 404. They now use the real routes and
    payload shapes.
  - `updated_at` was documented as caller-maintained; the
    `update_*_modtime` triggers in `sql/001_init_pay_tables.sql` set it.
- **The deploy scripts never applied a migration.** `deploy.bat` and
  `deploy.sh` looped over `%PROJECT_ROOT%\sql\*.sql`, but `sql/` moved to the
  repository root at the plugin refactor and `PROJECT_ROOT` is
  `examples/pay-server`, so the glob matched nothing and the step reported
  success while applying zero schema. (Before the move the same loop had run
  `000_drop_pay_tables.sql` as if it were a version — the 2026-07-07
  production-readiness gap analysis called that "not zero-downtime,
  zero-data".) Both now call the executor, which cannot silently find nothing.
  Adjacent typos in the same routines: `pg_isquiet` (not a program) made every
  `deploy.bat` run abort at the connectivity check, and a required-dependency
  named `pgredis` made every `deploy.sh` run abort before building.
- **The Linux CI migration step had no password.** `_build-test.yml` applied
  the chain with `psql -h 127.0.0.1 -U test` inside a step that never set
  `PGPASSWORD`, while the readiness probe above it passed
  `PGPASSWORD=123456` inline; on a password-authenticated container the step
  could only fail once it stopped being the first psql call.
- **A Windows checkout could not pass the guards that hash committed bytes.**
  No `.gitattributes` existed, so `core.autocrlf=true` delivered CRLF working
  copies — and `scripts/migrations_baseline.json` had been pinned from one of
  them: `001`/`003`/`004` carried CRLF digests, which pass on the machine that
  wrote them and fail rule 3 on a Linux runner that checks out LF. The pins now
  hold the committed bytes and `.gitattributes` declares them (`eol=lf` for
  text, `eol=crlf` for the `.bat`/`.cmd` files cmd.exe needs CRLF for), so
  disk, index and runner agree. `check_migrations.py` and `migrate_db.py`
  additionally separate "applied history was edited" from "your working copy
  has CRLF", because those two need opposite fixes. The other half of the same
  bug: a CRLF `#!/bin/bash` shebang is not executable, which is exactly how the
  `.sh` twins of the dev scripts failed on this machine.
- **`.env.production` was not ignored.** The ignore list covered `.env`,
  `.env.local` and `.env.*.local` but not the rest of the family, while
  `docs/development/environment_setup.md` tells operators to put real
  credentials in `.env.production` — one `git add .` away from committing a
  production password. `.env.*` is ignored now, with only the `.example`
  templates re-included.

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
