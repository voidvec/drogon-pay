# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Docs/AI-config drift guard** (`scripts/check_docs_drift.py`, CI hard
  gate): keeps the `AGENTS.md` asset inventory in sync with `.claude/`,
  rejects backticked paths that don't exist in governance docs, bans
  gtest vocabulary outside archived history, refuses migration versions the
  `sql/` chain does not have, and (rule 5) requires a file held by both
  `.claude/` and `.codex/` to be byte-identical — see Fixed.
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
  verbal percentage. A SEED run writes its baseline into the runner's working
  copy and loses it there, so the file this ships with is the first green
  `coverage.yml` run's measured numbers copied into a reviewed commit (overall
  40.21%, 3012/7490 lines, 2026-09-19) — without it every run re-SEEDs and the
  ratchet has no floor to hold.
- **Drift guard rules 6 and 7** (`scripts/check_docs_drift.py`), which turn
  this round of documentation fixes into something that cannot silently rot
  again. Rule 6 (`no-version-stamps`) rejects a `**版本：**` /
  `**Last updated:**` line in any live governance document: a stamp is a
  second copy of a fact CI already checks elsewhere, so 18 of them went away
  (four header/footer pairs in each of the four operations and deployment
  guides, plus the `CLAUDE.md` / `TECH_SPECS.md` footers, which now defer to
  `git log`). Verified by pointing the rule at `git show HEAD:` of the four
  stamped documents (it reports exactly the 16 deleted guide lines) and at the
  cleaned tree (it reports nothing). Rule 7 (`twin-scripts`) requires the five
  entry points (`build`/`test`/`setup_database`/`deploy`/`check_config`) to
  exist as a `.sh` + `.bat` pair, refuses an undeclared orphan script in
  `examples/pay-server/scripts/` (declare it single-platform with a reason and
  a `TECH_SPECS.md` row instead), drops a stale declaration whose file is gone,
  and fails when a `.sh` is indexed `100644` because a clone could not `./` it.
  Each of the three failure modes was exercised against a temporary scripts
  directory that is then removed.
- **Single-entry CI pipeline** (`.github/workflows/ci.yml` + reusable
  `_build-test.yml` / `_sdk-smoke.yml`): FAST (`static-analysis`, parallel
  `clang-tidy`) → MAIN (`build-test` matrix over linux/windows/macos) →
  RELEASE (`sdk-smoke` matrix over linux/windows), chained by `needs`, with
  `concurrency` cancelling superseded runs. The three required check names
  (`linux-build-and-test`, `windows-build-and-test`, `macos-build`) are
  unchanged and now come from `matrix.check_name`. Actions are pinned to
  full commit SHAs. The pre-Conan build-Drogon-from-source jobs moved to
  dispatch-only `legacy-source-build.yml`. The old `ci-linux.yml` /
  `ci-windows.yml` / `ci-macos.yml` / `conan-create.yml` ran beside the new
  pipeline for exactly one verification cycle and are deleted in this stack: the
  same commit carried both chains to green (`ci.yml` FAST → MAIN → both
  sdk-smoke legs, plus all four legacy checks), and the new RELEASE gate
  smoke-tests `conan create` on Linux as well as Windows at PR time, which the
  Windows-only job it replaces never did.
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

- **Two dead idempotency helpers survived the service refactor until GCC
  pointed at them.** `storeIdempotencySnapshot` existed as a file-local
  function in both `PaymentService.cc` and `RefundService.cc`, with no caller
  in either: snapshot persistence had moved to
  `IdempotencyService::updateResult` (the fix that made the write land before
  the response), and the `RefundService` copy had even lost its own exception
  messages to the copy-paste, logging "Mapper construction failed" where an
  insert failure would appear. `PaymentService.cc` also carried a second
  orphan, `toRfc3339Utc`, whose `RefundService` twin is live. MSVC's `/W4 /WX`
  does not report an unreferenced internal-linkage function, so the
  `DROGON_PAY_WERROR` gate passed on Windows and on the macOS build lane while
  GCC's `-Wunused-function` was right. All three are deleted, along with the
  `PayIdempotency` include and model alias that only they used. The behaviour
  was already correct; nothing that wrote a snapshot was removed.

- **Three dropped test assertions, and the compiler that was missing them.**
  `PayPlugin_QueryOrder_WechatQueryError`, `PayPlugin_QueryRefund_WechatQueryError`
  and `PayPlugin_WechatCallback_WechatClientNotReady` each fetched the
  callback's `std::error_code` into a local and never asserted on it, so the
  contract those cases exist to pin (a degraded channel query still reports
  *no service error*; the not-ready path *does* report 1400) went unwatched.
  GCC's `-Wunused-but-set-variable`, which MSVC does not implement, caught all
  three — the Linux log named one, and a local `g++ -fsyntax-only -Werror` sweep
  over the whole compile database (compiling needs no database, so WSL is a
  usable stand-in for this) named the other two, whose translation units the CI
  build had never reached before it stopped. All three now
  carry the `CHECK` their sibling cases in the same file have always had, and
  each one passes on the first run — the behaviour was right, only the watch
  was missing. A CI log stops at the first failing translation unit, so the
  same class was hunted rather than waited for: counting occurrences of every
  file-local function name turned up two more orphans, `writeTempPrivateKey` in
  `RefundQueryTest.cc` and `writePrivateKey` in `WechatPayClientTest.cc` — three
  copies of an OpenSSL RSA key-minting helper across the `CreatePayment`,
  `RefundQuery` and `WechatPayClient` tests, none of them called by anything;
  the live key minting is `generateKeyAndCert` in the two WeChat suites. The
  sweep then reported no remaining `-Wunused-function` in any of the 41
  first-party translation units (the six generated `src/models/*.cc` files live
  in their own OBJECT library, deliberately advisory-only, where
  `-Wunused-parameter` is reported but not fatal). Deleting the helpers also
  removed the
  `<openssl/*>`, `<filesystem>`,
  `<fstream>` and `<atomic>` includes that only they used; the target-level
  OpenSSL 3.0 deprecation suppression stays, because the live minting still
  calls the legacy `RSA_*` API.
- **The first clang-only class this gate produced: fifteen dead `this`
  captures.** `AlipaySandboxClient::sendRequest`'s response lambda was the one
  the macOS log named — it captured `this` and never used it (the `timeoutMs_`
  read sits in the *argument list after* the lambda, which belongs to the
  enclosing function, not to the capture). `-Wunused-lambda-capture` is clang
  only, part of `-Wall`, and GCC and MSVC both let it pass, so the same local
  compile database was replayed through `clang++ -fsyntax-only -Wall -Wextra`
  over every translation unit. That found fourteen more, all in the service
  layer: three commit callbacks in `CallbackService`, eight in `PaymentService`
  (`proceedCreatePayment`, `createQRPayment`, and the WeChat/Alipay status-sync
  pairs), three in `RefundService`. None of the fifteen lambdas touched a
  member — where a transaction is involved the `dbClient_` read happens *before*
  `newTransactionAsync`, outside the callback — so each capture was a bare
  `this` alias with nothing behind it. Removing one is also a small lifetime
  win: an uncaptured `this` cannot be dereferenced by a later edit by accident.

  The sweep had to be re-run to a fixpoint, which is the part worth remembering:
  after the first ten were stripped, clang reported four *new* sites
  (`CallbackService:2565`, `PaymentService:1841`/`:2283`,
  `RefundService:1913`) that the same command had not printed a minute earlier.
  A `-Wunused-lambda-capture` report is therefore not a complete inventory of a
  file in one pass — treat the first run as a work queue, not a verdict. All
  three compilers then agreed on the result: MSVC rebuilt clean, the WSL
  `g++ -fsyntax-only -Werror` sweep over all 47 units came back with nothing but
  the six generated models' advisory `-Wunused-parameter`, and the local clang
  sweep ended at zero. Test counts were unchanged at 149 cases / 1455 assertions,
  confirming no behaviour moved.

  That asymmetry is the reason the `DROGON_PAY_WERROR` bar runs on three
  compilers rather than one: each of the three owns a class of defect the other
  two cannot see.

- **An `assert` that two tests exist to violate, visible only in Debug.**
  `CallbackService`'s constructor asserted that `wechatClient_` and `dbClient_`
  were non-null (the comment called it "C2-3 fix: previously missing null
  checks"), while `PayPlugin::setTestChannels` builds that service
  unconditionally — its comment says so: "a missing wechat channel exercises the
  service's own 'wechat client not ready' branch" — and
  `PayPlugin_WechatCallback_WechatClientNotReady` calls `setTestChannels({},
  nullptr)` precisely to pin that branch. The two cannot both be true. `assert`
  evaporates under `NDEBUG`, so every Release lane — which is all `ci.yml`
  builds — has run those tests green, while `coverage.yml`, the one lane that
  builds Debug, aborted 2.4 s into `ctest`: before a single `.gcda` landed,
  which is one reason more the coverage baseline has still never seeded. Both
  asserts are gone; nothing else was, because the null checks were never the
  job of the constructor: every dereference of `wechatClient_` and `dbClient_`
  already sits behind a branch that answers `1400 / "wechat client not ready"`
  or `1003 / "Database client not available"`, and those were the only two
  `assert(...)` calls left in the service layer.

- **The macOS leg was asked to do something its image cannot do.** Its first
  failure was a formula name: `brew install postgresql@15 redis@7` — `redis@7`
  has been removed from homebrew-core, and `brew` exits non-zero on an unknown
  formula. Renaming it to plain `redis` looked like the fix and was the start of
  the real answer: `macos-14` sits outside Homebrew's support window now
  (`You are using macOS 14. ... Homebrew no longer builds bottles for this
  configuration`), so every fresh install compiles from source, and Redis 8
  pulls `llvm@22` and `rust` in as build dependencies. The leg burned its entire
  120-minute timeout inside `brew install` and never reached `initdb`, let alone
  the suite. The provisioning step is deleted and `use_database` is `false` for
  macOS, which is where the repository started and where the reference
  implementation still is: the required check is named `macos-build` because an
  earlier honest-naming pass renamed it off `macos-build-and-test` precisely for
  having no databases. What the leg does gate — the arm64 clang `-Werror`
  compile, sole reporter of `-Wunused-lambda-capture` — needs no services, and
  the runtime suite stays covered on Linux and Windows.

- **The docs described a command line the server does not have.**
  `main()` in `examples/pay-server/main.cc` takes no `argc`/`argv`, yet
  `CLAUDE.md`, `docs/operations/operations_manual.md` and the `drogon-build`
  skill showed `./PayServer --port 5567 --config config2.json`. PayServer
  discards every argument without complaint, so the four-instance recipe in
  the operations manual started four processes all binding 5566 — the exact
  port-hijacking the isolated test port exists to prevent. The manual now runs
  one process per deployment directory, each reading its own `config.json`,
  and the docs state plainly that the working directory (not a flag) decides
  which `config.json` and `.env` are read.
- **The test guide recommended a filter that cannot fail.**
  `tests/CMakeLists.txt` registers a single ctest case (`PayBackendTests`), so
  `ctest -R SomeCase` matches nothing and exits 0: the "selected the one test I
  wanted" workflow was really "ran nothing and called it green". `CLAUDE.md`
  and `docs/testing/testing_guide.md` now send single-case work to the test
  binary's own `-l` / `-r`, and say where the run actually happens — the
  binary's directory, since that is the `WORKING_DIRECTORY` ctest uses and the
  only place `config.json` resolves.
- **Health checks polled an endpoint whose sunset date had passed.**
  `/health` is a deprecated alias of `/readyz` and answers with
  `Deprecation: true` plus `Sunset: 2026-08-28`, a date already behind us.
  `deploy/ops/restart_service.sh` and `deploy/ops/restore_db.sh` gated a
  rollout on it, and `docker-integration-test` probed it too; all three now use
  `/readyz`, and `CLAUDE.md`'s endpoint table spells out the difference
  (`/healthz` = process alive, `/readyz` = dependencies reachable). Removing
  the alias itself is a breaking change and belongs to a version bump, so it
  stays served for now.
- **A Debug build was said to be impossible.** `docs/deployment/deployment_guide.md`
  warned that building in Debug "causes link errors". Each preset directory
  carries its own Conan dependency tree, so Debug links Debug dependencies — and
  `coverage.yml` builds and tests exactly that (Debug + gcov) whenever
  coverage-relevant paths change, which would have been dead on arrival had the
  claim been true.
- **The container test step described a binary that is not in the image.**
  The `docker-integration-test` skill had a step running the test suite inside
  the service container; the runtime image contains `/app/PayServer` and
  `/app/config.json` and nothing else. It now says to exercise the image over
  HTTP and run the suite on the host, and names the script that actually
  produces the report it references. `run_docker_tests.sh` is documented as the
  orphan it is: unreferenced, probing the deprecated `/health`, no exec bit.
- **A test could have read the dev server's counters.** `tests/main.cc`
  rewrote each `listeners[]` entry to the isolated test port but left
  `custom_config.pay.metrics_base_url` at the copied config's `5566`, and
  `/metrics` proxies to *this process's* `/metrics/base` — so any case that
  scraped metrics would have silently collected a locally running PayServer's
  numbers while believing its own. The base URL is rewritten to the test port
  alongside the listeners.
- **Version facts were described as one list, not three kinds.**
  `check_version_sync.py` compares three declarations (`CMakeLists.txt`,
  `conanfile.py`, `pay-admin/package.json`); the six `drogon-pay/1.0.0`
  references in the READMEs and `plugin_integration.md` are *published* package
  versions that only move with a release, and documentation version stamps are
  now banned outright. `TECH_SPECS.md` 「版本号一致性」 tabulates those three
  categories, and the `release` skill lists the six text references as a
  manual checklist item so they stop being mistaken for a guard that broke.
- **The migration-history claim understated itself.** The docs said three
  hard-coded file lists had drifted; `git show 043c5ed^` has six, of which the
  two in the CI workflows applied only `001` + `002`, and the two in the deploy
  scripts globbed a directory that had moved and printed success after applying
  nothing. `CONTRIBUTING.md`, `TECH_SPECS.md` and the `create-migration` skill
  (both mirrors) now state the checked version, with that commit as the
  reference for anyone who wants to re-verify it.
- **The dead-path rule was green on a laptop and red on CI.** R2 of
  `scripts/check_docs_drift.py` resolved backticked paths with
  `Path.exists()`, so on a development machine it accepted documentation
  pointing at `build/`, `examples/pay-server/.env` and
  `examples/pay-server/certs` — three things that exist only as untracked,
  gitignored output. A fresh CI checkout has none of them and the guard failed
  on its first run there. It now resolves each citation against the git index
  (a tracked file or a tracked directory prefix) plus the set `git
  check-ignore` claims, both of which are versioned facts, so the answer no
  longer depends on what happens to be on disk — and each probe is asked about
  twice, bare and with a trailing slash, because `build/` and `certs/` are
  directory-only patterns and git will not apply one to a path it cannot
  establish is a directory, which on a checkout is exactly the case. The
  plumbing had its own laptop/CI split too: passing `encoding=` to
  `subprocess.run` enables text mode, whose newline translation fed CRLF to
  `git check-ignore --stdin` and made every path look unignored, and the
  writer thread's exception left the git child blocked on stdin. That call now
  speaks bytes in both directions. Verified by running the guard in a clean
  clone with no `build/`, `.env` or `certs/` on disk (green, as on CI, where
  the previous version was red) and by injecting two fabricated paths there
  (both reported, then clean again once the file was restored).
- **`YOUR_API_KEY` was flagged as a leaked secret.** The `curl-auth-header`
  rule matches on shape, and every curl sample in
  `docs/api/pay-api-examples.md` writes `-H "X-Api-Key: YOUR_API_KEY"`. The
  placeholder joins the existing allowlist in `.gitleaks.toml` (which already
  carries `test_key_123456`, `PLACEHOLDER_32_CHARACTER_KEY==` and
  `query-only-key`) rather than the documentation being rewritten to dodge a
  shape match.
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
- **The second agent's copy of the rules had drifted, silently and in the
  wrong direction.** Three `.codex/` files that mirror `.claude/` were stale:
  `rules/db-operations.md` still listed three raw-SQL exemptions where the
  source lists six and had no `Mapper`-construction section at all,
  `skills/project-conventions/SKILL.md` still placed core logic in `pay-server`
  (the path the plugin refactor replaced) and still described four log levels,
  and `skills/orm-gen/SKILL.md` still told the operator to `cd` into
  `pay-server` for the model tree. Nothing reads a mirror, so this is invisible
  until an agent enforces a rule the code stopped having — rule 5 of
  `scripts/check_docs_drift.py` now fails CI on it (verified: the guard passes
  on the re-mirrored tree, and reports the byte counts after a one-line
  injected drift, then clears when the file is restored). While re-mirroring,
  the `.claude` source itself lost two authforge leftovers in the same file:
  `StringListCallback` / `AccessTokenCallback` / `RefreshTokenCallback` name
  types this repository does not declare anywhere, and `(*sharedCb)(...)` is
  only half the idiom — the section now describes what
  `libs/drogon-pay/src/services/` actually does (per-construction-site
  `try/catch`, failure reported through the call site's own callback shape,
  `OnceCallback::call` where a service wrapped it).

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
