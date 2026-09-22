# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **`sql/006_ledger_payment_income_unique.sql`**: a partial unique index
  `uq_pay_ledger_payment_income` on `pay_ledger(payment_no)` where
  `entry_type = 'PAYMENT' AND payment_no IS NOT NULL` — the DB-layer backstop
  for income double-booking (audit round 17). A payment collects money once,
  so it owns at most one income ledger row, but today the only thing enforcing
  that is the application CAS: all five income writers —
  `CallbackService.cc:1465` and the four settle branches in
  `PaymentService.cc` (`:2627/:2811/:3123/:3293`) — insert only on the
  branch where the payment-status `UPDATE` actually hit. If that gate ever regresses,
  the append-only ledger would book the duplicate silently; now the insert
  fails loudly inside the settling transaction. `REFUND` entries are
  deliberately outside the index (partial refunds are legitimate repeats per
  payment, and the ledger has no `refund_no` column to key them by), matching
  the defense-in-depth pattern of `003_refund_unique_constraint.sql`.
  Enforcement itself is DB-backed, so it is verified by the CI legs only.
- **`platform_ca_cert_path` on the WeChat channel** (optional): a PEM bundle of
  trust anchors. When set, every downloaded platform certificate must chain to
  one of them before it is cached or used to verify a notification; when unset
  the channel behaves as before, so the key is an upgrade step rather than a
  breaking default. Without an anchor, whoever answers for `api_base` decides
  which certificate later validates payments.
- **`docs/review/2026-09-20-wechat-pay-api-audit.md`**: code-level audit of the
  whole WeChat Pay V3 flow against the official API — inbound notification
  path, outbound transaction/refund/certificate path — with each defect cited
  at `file:line`, the fix batch it landed in, and the items deliberately left
  out (the Alipay timeout unit mix-up). Three of the items an earlier batch
  listed as left out have since been implemented below: the idempotency-
  reservation owner token, the bare-`this` capture in the certificate-download
  callback, and the `time_expire` format and window check.
- **`tests/integration/QrPaymentBookingTest.cc`**: drives the service behind
  `/api/qrpay/create` (with the channel injected through
  `PayPlugin::setTestChannels()`) so the QR booking contract is pinned by a test
  instead of by reading the service — payment row present, per-attempt rows on
  retry, the amount and currency the channel is offered, what the service does
  with the fields the handler passes through (`user_id` on the order row, a
  public `notify_url` in the channel payload), and what is refused *before* the
  channel is asked at all.
- **`tests/integration/RequestBodyShapeTest.cc`**: one handler-level case per
  body member whose JSON type used to fault the process, calling the controllers
  directly, plus the cases pinning what a process with no `PayPlugin` answers.
  None of them reaches a database or a channel, which is what makes the suite
  runnable where no Postgres exists — but several do reach the plugin, and whether
  the test process has one depends on the start directory (`tests/main.cc`
  registers `PayPlugin` only when `./config.json` opens from there), so those
  assertions are gated on the process state and the file is verified in both.
- **A `findOne` caveat in the DB rule** (`rules/db-operations.md`, mirrored to
  the other agent config): `Mapper::findOne` sends "no row" *and* "more than one
  row" down the error callback, so the rule now states that it is only for
  unique keys and points at `orderBy(col, DESC).limit(1).findBy(...)` for
  "the newest of several" — the shape two callback lookups had to be moved to.
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
  `concurrency` cancelling superseded runs. The three required checks keep the
  names the legacy files used, but not as bare contexts: a job calling a
  reusable workflow reports `<caller job name> / <name the called workflow gives
  its own job>`, so the ruleset now requires `linux-build-and-test / build-test`,
  `windows-build-and-test / build-test` and `macos-build / build-test`, whose
  first half comes from `matrix.check_name` and second half from the unnamed
  `jobs: build-test:` in `_build-test.yml`. Actions are pinned to
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
- **Release gate waits for the pipeline that certifies the tag**
  (`.github/workflows/_tag-gate.yml`, called by release.yml's `ci-gate` job
  between `version-check` and `sdk-smoke`, and by deploy.yml's `tag-gate` before
  it pushes an image — see the Fixed entry for why one file serves both).
  Branch protection cannot cover a tag: the ruleset's required contexts gate
  merges, and `git tag v1.1.0 <commit> && git push --tags` is not a merge, so any
  commit could be released without ever having gone through the pipeline —
  v1.0.0 was, with `windows-build-and-test` reported as `failure` on that very
  commit. The job resolves the tag to its commit (the API dereferences
  annotated tags, whose own object SHA carries no check runs), refuses anything
  that `compare/master...<sha>` does not place inside master's history — which
  also excludes a tag on an unmerged branch commit, whose PR checks may look
  green — and then polls `commits/<sha>/check-runs` for the five contexts the
  merge pipeline reports (three `* / build-test`, two `* / sdk-smoke`, FAST
  excluded because MAIN `needs` it). A non-success conclusion fails immediately,
  a check that has not finished (or not yet appeared) polls for
  `DEADLINE_MINUTES: 120` minutes and then fails with the names that never
  arrived — measured end to end, one green pass of the merge pipeline takes 32
  minutes, so the window has to clear that with room left for a cold Conan cache,
  and `timeout-minutes: 135` sits above it deliberately so the script's own
  "which context is missing" message beats a bare runner cancel. When a name was
  reported more than once (a re-run, a second dispatch over the same commit) the
  verdict comes from the highest check-run id, so a re-run supersedes its own
  predecessor; requiring every sibling to be green would let one stale
  `cancelled` lock the release with nothing able to clear it. An empty check-run
  listing is *not* the "nothing is coming" signal — this same workflow reports
  check runs against the tagged commit, so the listing is never empty — and the
  five contexts only appear once FAST has finished, a quarter of an hour in. The
  evidence that a pipeline reached this commit at all is ci.yml's own entry jobs,
  so `ENTRY_CHECKS: static-analysis,clang-tidy` is polled for and fifteen minutes
  without them (`EARLY_BAIL_SECONDS: 900`, a window wide enough to survive runner
  queueing) exits with the reason instead of waiting two hours. That bail is a
  backstop, not a merge test: those two names are reported on `pull_request` runs
  too, and only the containment check above says the commit is on `master`. The tag
  name is shape-checked against semver before it reaches an API path, the three
  timing knobs are rejected unless they are numbers *and before anything does
  arithmetic with them*, every context name is rejected unless it is made of the
  characters the real five use (one containing a backslash could never match, since
  `awk -v` unescapes it, and the release would stall for two hours over it), and
  the list is rejected unless it holds exactly five entries — a truncated `env:`
  block would otherwise leave nothing pending and print "green" having inspected
  nothing. `scripts/ci/tag_gate_scenarios.py` replays that table by extracting the
  workflow's own `run:` bytes and driving them against a stand-in `gh` (23 cases:
  the entry bail in both directions, a listing that is pending on the first poll
  and green on the second, and each of the three API reads failing, so a transport
  error is never read as a verdict), and with `--live` against the real read-only
  API. The `static-analysis` job of `ci.yml` runs the stubbed mode on every pull
  request into master, so a verdict that drifts fails a pull request instead of a
  release. Replayed
  live it reproduces the incident it exists
  for: pointed at the v1.0.0 tag it dereferences the annotated tag to its
  commit, accepts that the commit is inside master, and — judged by the check
  names that commit's own pipeline reported, since the five current contexts
  postdate it — refuses with `windows-build-and-test: failure`. `sdk-smoke`
  still re-runs afterwards:
  "green on master" and "installs the way a consumer builds it" are claims about
  different artifacts.

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

- **A verified Alipay notification could still confirm the wrong order.** The
  async notification path ran the signature check and then trusted
  `trade_status` alone: `syncOrderStatusFromAlipay()` wrote `PAID` without
  comparing the notification's `total_amount` against the amount stored on the
  order, and the controller never re-checked the notification's `app_id`
  against our own configured one. Either gap turns a correctly signed message
  into credited money that was never charged for that order — a
  mis-configured sandbox, a replayed notification from another app, or a body
  whose amount was altered before the notification was composed (Alipay's own
  integration contract requires re-checking `out_trade_no`, `total_amount`,
  `app_id`/`seller_id` and `trade_status`, not just the signature). The service
  now compares the notification amount and the order amount in fen (the stored
  amount is a string, so `"0.5"` and `"0.50"` compare equal) and rolls the
  transaction back on a mismatch or an unparseable amount; the controller
  rejects a notification whose `app_id` differs from the configured one, and
  only when we know our own id, so an unconfigured sandbox cannot reject every
  callback. `seller_id` is deliberately *not* compared: the configured value
  may be the seller email (the sandbox quickstart documents either form) while
  the notification carries the `2088…` PID, so a check there would reject every
  genuine notification. `CallbackController_Alipay_ForgedSignature_Rejected`
  and `..._MissingSignature_Rejected` pin both reject reasons and so prove the
  notification never reached the order-sync path; neither reaches the `app_id`
  guard, which sits behind a signature the suite cannot mint. Their assertions
  are literal (`"signature verification failed"`) rather than "either reject
  reason", so the cases fail loudly if a future config lets a forged
  notification be refused by the "client not configured" branch instead.
  `PayPlugin_SyncOrderStatusFromAlipay_AmountMismatch_RefusesCredit` and its
  positive control `..._AmountMatches_CreditsOrder` drive the gate against a
  real transaction through `setTestClients`: the mismatch case requires the
  service to report `""` and the order and payment to stay `PAYING` /
  `PROCESSING` after the rollback, the match case requires `PAID` / `SUCCESS`,
  so a guard that refused everything could not pass the suite. The gate is
  written twice, once per payment state, so
  `PayPlugin_SyncOrderStatusFromAlipay_SettledPaymentAmountMismatch_RefusesCredit`
  covers the copy that a `PROCESSING` fixture never reaches.
- **A refused order sync was acknowledged as handled.** The notification handler
  called `syncOrderStatusFromAlipay()` and answered `{"code":"SUCCESS"}` for
  every outcome, including the empty status the service returns when it rolls
  the transaction back (amount mismatch, a failed update, a missing order), and
  logged that refusal at `LOG_INFO` as a completed sync. Whoever reads the
  response or the log — an operator reconciling a stuck order, a channel tool
  echoing the body — was told the payment was booked when the service had just
  refused to book it. The handler now answers
  `{"code":"FAIL","message":"order sync rejected"}` at `LOG_ERROR`, matching the
  level the `app_id` reject already uses. This changes what *we* report, not
  what Alipay does with it: our success body is already JSON rather than the
  plain-text `success` Alipay expects (recorded in `TECH_SPECS.md` "回调响应"),
  so the retry schedule is unaffected either way and that conformance stays a
  separate follow-up.
- **Alipay's own notifications could fail our verifier.** `verifyCallback()`
  built the signed payload from every parameter except `sign`/`sign_type`, but
  the official rule also drops parameters whose value is *empty*. A genuine
  `TRADE_SUCCESS` carrying a blank `refund_amount` or `gmt_refund` therefore
  hashed a different string than the one Alipay signed, verification failed,
  and the order stayed unconfirmed while Alipay kept retrying a notification we
  kept refusing. The verifier now skips empty-valued members, guarded by
  `AlipayVerifyCallbackTest`, which mints a throwaway keypair and signs a
  notification containing empty fields three ways: under the official rule (must
  verify), under the rule this verifier used to implement, which folded the empty
  fields in (must be rejected — the pre-fix code accepted exactly this one, so it
  is the assertion that pins which rule is in force), and a wrong signature (must
  be rejected). The fixture's temporary PEMs are named per invocation, because two
  test processes running at once would otherwise truncate each other's key files
  mid-verification. The same build path called the non-reentrant
  `std::localtime()` while composing the common request parameters; Drogon can
  serve from several IO-loop threads, so the timestamp is now formatted through
  `localtime_s`/`localtime_r`.
- **The amount gate could compare two amounts to the same wrapped number.**
  `parseAmountToFen()` parsed the yuan part with `std::stoll`, which accepts
  anything up to `int64_t`'s own limit, and then scaled it by 100 — signed
  overflow is undefined behaviour, and on the wrapping arithmetic every build
  actually ships, `92233720368547758` yuan lands on a fen value that collides
  with a small legitimate one. The gate that exists to catch a low-value payment
  confirming a high-value order compares those fen with `!=`, so the collision is
  the exact failure it cannot afford. It now refuses to return a fen value it
  cannot represent (`PayUtils_ParseAmountToFen_RejectsOverflow`), which also makes
  the unparseable-amount path fail closed. Both gate sites in
  `syncOrderStatusFromAlipay()` had meanwhile grown the same parse-and-compare
  with the same rollback, so the comparison lives once as
  `pay::utils::amountEqualsFen()` — negative expectations (the caller's "could not
  resolve" sentinel) never match, pinned by `PayUtils_AmountEqualsFen`.
- **An enabled Alipay channel with no `app_id` booted silently.** The
  merchant-identity re-check enforces only when the client knows its own `app_id`,
  so a deployment that forgot `ALIPAY_SANDBOX_APP_ID` — where the placeholder
  resolves to an empty string rather than failing — kept accepting notifications
  with that check disabled, and signed every gateway request with an empty
  `app_id`. Nothing said so until a callback from another app arrived, or the
  gateway rejected a request. `StartupValidator::validateChannelReadiness()` now
  walks the resolved config for enabled channels and reports each missing
  `app_id` as a `LOG_WARN` at startup; it stays a warning because a partial
  rollout (one channel configured, another not) has to keep booting.

- **A refused QR order answered before the writes that refuse it had landed**
  (audit round 20). The QR branch closed the attempt and released its
  idempotency reservation with two fire-and-forget writes and called the
  response callback immediately after issuing them, so the client was told
  "this failed, retry" while the database still said the opposite: the
  `pay_payment` row remained `INIT`, which every recovery filter reads as an
  attempt still in flight, and the reservation row was still there for the
  retry to trip over. A corrected retry — the case the reservation exists to
  protect — could therefore be refused for a payment the channel never saw.
  Both writes are now chained ahead of the answer: `failQr`
  (`PaymentService.cc:1538`) responds from the `clearReservation` callback, and
  `markQrPaymentFailed` (`:1695`) takes the response as its `afterClose`
  continuation, invoked exactly once through either the write's own callback or
  the `catch`. This was the outlier, not a new rule: the success branch already
  answered from `respondQr` after its writes settled, and the jsapi path has
  passed the answer into `bookRefusedAttempt` as `done`
  (`PaymentService.cc:360`) since round 3. CI caught what local runs could not:
  `PayPlugin_QrBooking_ChannelRefusalClosesThePaymentAndAllowsRetry`
  (`QrPaymentBookingTest.cc:360`) read `INIT` where it required `FAIL` on both
  the Linux and Windows legs, and the Linux leg — the slower one — also failed
  the retry half of the same case for the reservation reason. The assertion was
  sound; only the code's ordering was not, so no test changed.
  The same round cleared two compile errors that only non-MSVC compilers can
  see, both introduced here: `static WinsockGuard winsock` is an unused
  variable under GCC once the type collapses to an empty struct off Windows
  (`WechatPayClientTest.cc:521`, now `[[maybe_unused]]`), and `this` was a dead
  capture in the refund SUM lambda, which clang reports as
  `-Wunused-lambda-capture` (`RefundService.cc:1720`).

- **The outbound answer-verification gate sat after the two answers an
  attacker finds easiest to forge** (audit round 19). `sendWechatRequest`
  verified only `2xx && status != 204`, and answered `204 No Content` as
  success *before* that check. Per the official response-verification guide
  the exemption list is the file/image download interfaces only: a 204 is
  signed over an empty body (`应答时间戳\n应答随机串\n\n`), and error answers
  are standard responses like any other. So an unsigned 204 — the cheapest
  forged answer there is, no body to construct — read as "call succeeded" to
  every consumer that keys success on an empty error, and an unsigned 4xx
  envelope fed `RefundService`'s terminal-failure decision. Verification now
  runs first for every answer that has a verifier, leaving the certificate
  download bootstrap (whose answer is self-authenticating via its GCM tag) as
  the only skip. The same round fixed what that made reachable:
  `refundCertainlyDidNotHappen` infers "the request never left" from the shape
  of the error string, and the verification-failure text added in round 16 is
  not `HTTP …`-prefixed, so a refund WeChat may have *accepted* was booked
  `REFUND_FAIL` — inviting the retry-under-a-new-`out_refund_no` that becomes
  a second refund. An unreadable answer is now an unknown outcome
  (`REFUNDING`, warn, reconciliation decides), while a genuine pre-send fault
  still books terminal FAIL. Not changed, and stated as a limitation: the
  signed message binds neither the request path nor our own nonce, so the
  remaining replay control is matching the answer's `out_trade_no` /
  `out_refund_no` against what was asked, which no read site does today
  (§二十五 of the audit doc). A clock-skew window on answers is deliberately
  not added — the guide requires none, matching the inbound conclusion of
  round 15.

- **A tag no pipeline had ever run could deploy production.** `deploy.yml`
  triggers on the same `push: tags: v*` as `release.yml`, and its `build-and-push`
  and `deploy-production` legs depended only on `preflight` — which checks whether
  secrets exist, not whether the commit is sound — so `git tag v9.9.9 <commit>
  && git push --tags` would push a semver image and roll the ECS service for any
  commit at all, including one the tests never ran, wherever those credentials are
  configured. A red release did not stop it:
  `jobs.*.needs` cannot cross workflows, so the two tag consumers shared no gate.
  The check now lives once, in `.github/workflows/_tag-gate.yml`, and both call it
  — `release.yml` as `ci-gate`, `deploy.yml` as `tag-gate`, which `build-and-push`
  `needs`. `deploy.yml` is also dispatchable from a branch, and there is no tag to
  certify there (its production leg already keys off a tag ref), so a non-tag ref
  passes the gate through instead of failing a manual deploy that has nothing to
  check. Two limits of the fix are worth stating: Actions runs the workflow
  definition *stored in the tagged commit*, so a tag aimed at a commit older than
  this change is still built by the ungated file and the hole closes going
  forward rather than retroactively; and a `v*` tag outside `v` + three numeric
  segments is now refused by the shape check, where such a tag previously reached
  the image-push and ECS-roll legs with nothing saying otherwise (in this
  repository those legs then stopped on their own missing-credential conditions,
  which is why no `v*` tag has actually rolled production that way — the absence
  of an incident is a secret gap, not a gate).

- **`time_expire` had no HTTP entry point, which made the whole order-expiry
  chain dead code (audit round 18)**: no handler ever assigned
  `CreatePaymentRequest::timeExpire`, so the round-10 pre-validation
  (`PaymentService.cc:497`), the channel forwarding (`:712`) and the single
  `setExpireAt` writer (`:650`) never ran — `pay_order.expire_at` was always
  NULL, and the round-14 close sweep, whose gate is `NOTPAID ∧ expire_at
  passed`, could therefore never fire in production. Both create routes now
  accept the field: shape-guarded (`PayHandlers.cc:177/:338`), wired on the
  struct path (`:210`) and passed through plus hashed on the QR path
  (`:410`; service read `:1404`, replay-hash `:1461`, 400-before-booking
  validation `:1545`, WeChat forwarding `:1612`, QR `expire_at` booking
  `:2017`), documented in the OpenAPI request schemas of both routes, and
  pinned locally by two handler mistype cases
  (`RequestBodyShapeTest.cc:199/:278`); the QR end-to-end booking is DB-family
  and adjudicated by CI.

- **Outbound APIv3 answers are now verified before the caller may read them.**
  The official signing doctrine is that the merchant must verify WeChat's
  answer on every request that carries one -- the channel had enforced that
  for callbacks only, so a man-in-the-middle or a hostile `api_base` could
  answer a query, create, close or refund with a well-formed `200` JSON
  claiming any state and the service layer would book it. `sendWechatRequest`
  now runs a per-client answer verifier over every 2xx answer that carries a
  body (`WechatChannel.cc:520`): the `Wechatpay-Timestamp/Nonce/Signature`
  set is checked first (an unsigned answer is refused before it can spend the
  shared certificate-refresh window), the signature is validated over
  `timestamp\nnonce\nbody\n` against the trusted platform certificate named
  by `Wechatpay-Serial`, and a failed answer is dropped without echoing the
  forged body. Documented carve-outs, each with its reason in the code: the
  headerless `204 No Content` success, non-2xx failure envelopes, and
  `/v3/certificates` itself (bootstrapping the trust anchors cannot verify
  against what it is fetching; that answer is self-authenticating under the
  AES-GCM `api_v3_key` and every cert still passes the serial/validity/CA
  binding in `setPlatformCert`). Pinned by four cases in
  `WechatPayClientTest.cc`: a signed `200` end-to-end accepted
  (`:1084`), an unsigned `200`-SUCCESS forgery dropped (`:1157`), the
  header/body/serial binding with its rejections (`:1224`), and the
  empty-verifier certificate-download path answering through the shared
  request path without dereferencing an empty `std::function` (`:1295`,
  a pre-release review catch -- as was holding the weak pin across the
  verification call rather than null-checking a temporary)
  (`docs/review/2026-09-20-wechat-pay-api-audit.md` §二十二).
- **An amount that overflows the money type is now refused instead of
  silently wrapped.** The controller's amount regex caps the shape but not the
  digit count, and `parseAmountToFen` fed 17-19-digit yuan values through
  `stoll` unharmed into `yuan * 100`, which wraps signed (so
  "184467440737095517.99" booked fen 183 — 1.83, a self-consistent small
  positive number that every downstream amount-reconciliation gate then
  accepted). The parse now rejects anything that cannot scale to fen without
  overflow (`PayUtils.cc:370`, exact `INT64_MAX`-derived ceiling, shared by
  both channels), and the existing service-level rejections turn it into the
  400 the request always deserved. Pinned at the exact representable ceiling
  as well as the wrap values in `PayUtilsTest.cc:25` (19 assertions).
  The round's other candidate — a timestamp tolerance window on callback
  verification — was falsified against five official pages and deliberately
  not implemented: the documented 5-second clause is a processing deadline,
  duplicates are delegated to merchant-side idempotency the CAS/ledger gates
  already provide, and a hard window would false-reject WeChat's legitimate
  multi-hour retries (`docs/review/2026-09-20-wechat-pay-api-audit.md`
  §二十一).
- **An expired, still-unpaid WeChat trade is now closed on the channel.**
  Nothing in the codebase ever called the close API, so an order whose deadline
  passed without payment stayed pay-able on WeChat's side until the channel's
  own lazy expiry, and the reconcile sweep — the one component that looks at
  exactly these orders — reported them unpaid every pass without ending them.
  The channel SPI gained `closeOrder` (`PaymentChannel.h:78`, defaulting to an
  explicit "unsupported" answer), `WechatPayClient::closeTransaction` implements
  the V3 contract (`WechatChannel.cc:941`: POST to
  `/v3/pay/transactions/out-trade-no/{out_trade_no}/close`, body only `mchid`),
  and the shared request path now treats the documented `204 No Content` answer
  as success instead of "invalid json response" (`WechatChannel.cc:500`) — the
  one proof-of-close answer would otherwise have surfaced as a failure. The
  sweep fires the close only when the channel itself still says `NOTPAY` *and*
  the order row's own `expire_at` has passed
  (`ReconciliationService.cc:223`); refusals are logged, a paid trade is
  answered by the channel's refusal, and local rows converge to `CLOSED` on the
  next pass. Pinned by the channel-shape cases in `WechatPayClientTest.cc:954`
  and the sweep gate — three negative controls included — in
  `WechatCloseOrderReconcileTest.cc:339`.
- **A trade in `REFUND` state keeps the payment it collected.** `mapTradeState`
  answered `REFUND` together with `CLOSED`/`REVOKED`, so a dropped notification
  plus one status query booked money that *had arrived* as money that never did:
  the payment row fell to `FAIL`, no `PAYMENT` ledger entry was written, and the
  order read `CLOSED` as if it had expired unpaid — while the channel bill shows
  the payment and its refund. `REFUND` is a state a trade reaches only after the
  money arrives, so it now settles to order `REFUNDED` / payment `SUCCESS`
  (`PayUtils.cc:410`), and both the "is this answer evidence about *this*
  payment" amount proof and the ledger gate accept the `REFUNDED` landing
  (`PaymentService.cc:2474`, `:2552`, `:2717`, `CallbackService.cc:1091`). The
  collection still needs the amount it asked for: a `REFUND` answer reporting a
  different total settles nothing. `CLOSED`, `REVOKED`, `PAYERROR` and the
  unknown default keep their old direction, and the notification-path case that
  had pinned the wrong answer now pins the right one.
- **An order's `time_expire` is checked before it is booked, and stored as the
  instant it names.** The field was forwarded to WeChat verbatim while the local
  `expire_at` was parsed with `trantor::Date::fromDbStringLocal`, which splits on
  a *space*: a correct `2026-05-20T13:29:35+08:00` reached the day field as
  `20T13:29:35+08:00`, where `std::stol` stopped at the `T` without throwing, so
  the order was booked at local midnight with the whole time-of-day silently
  dropped — while the space-separated form the same parser *did* accept is the one
  WeChat answers with a 400, after the row already exists. `pay::utils::parseRfc3339`
  now reads the strict form itself (offset applied once, calendar-checked, no
  dependency on the machine's zone — `fromISOString` is not usable either, it adds
  the runner's offset on top of the string's own), `validateTimeExpire` refuses a
  deadline that has already passed or that exceeds the channel's own seven-day
  window, and `PaymentService::createPayment` refuses with 1001 before the order
  row is written (`PaymentService.cc:497`) and books `expire_at` from the same
  reading (`:650`).
- **An idempotency reservation is now finalized only by the delivery that took
  it (owner token).** The WeChat callback chains reserve a `pay_idempotency`
  row with a NULL snapshot and finalize it after the settlement commits, but
  nothing recorded WHO held the reservation: the read path's stale-reservation
  delete matches by key, so a retry can drop the reservation of a live-but-slow
  delivery, which then keeps running and finalizes through a key-only UPDATE —
  stamping a snapshot onto a row it no longer owns, or committing a settlement
  whose idempotency proof was deleted underneath it and ACKing SUCCESS with no
  row left. `sql/005` adds `owner_token`; each callback delivery writes a fresh
  random token at reserve time, and both chains' finalizes go through
  `finalizeReservation`, an ownership-guarded `UPDATE ... RETURNING` (the
  generated ORM model predates the column and models are drogon_ctl-only).
  A zero-row match inside the business transaction rolls the whole delivery
  back and answers FAIL — the channel retries and the next delivery reads the
  true state; after the transaction already committed, the settlement is the
  truth, so the snapshot loss warns and still ACKs. The read-path delete stays
  key-scoped deliberately (a crashed holder's reservation must be clearable);
  the takeover is what the guard neutralizes. Test fixtures create the new
  column, and the payment-callback stale-reservation case now asserts the
  winner's row carries both snapshot and owner token. The real race needs two
  concurrent deliveries against one Postgres, so locally the guard is
  compile-only evidence and CI adjudicates.

- **WeChat bookings are now checked against the official field window before
  they are booked.** `out_trade_no` is capped by the channel at 6-32 characters
  of `[0-9a-zA-Z_|*-]` and `description` must be non-empty and at most 127
  characters, but `/api/pay/create` and `/api/qrpay/create` forwarded whatever
  they were given. An order number outside the window was written to
  `pay_order`, failed every channel call with WeChat's own 400, and then sat in
  the reconciliation sweep forever against a trade that could never exist — and
  a number over 64 characters could not even be refunded afterwards. Both
  service entries now share `pay::utils::validateWechatOrderFields` and answer
  400 (1001 on `/api/pay/create`, releasing the idempotency reservation) before
  the booking. `openapi.yaml` documents the window and the example WeChat
  response now shows a compliant order number; the QR booking suite's
  `ord_qr_<full-uuid>` fixture numbers (43 characters) moved to a unique
  in-window generator, and the service-level `CreatePaymentIntegrationTest`
  cases that relied on the old empty-order-number default now pass one.

- **The certificate-download response handler captured the client as a raw
  `this`.** `downloadCertificates` issued an async HTTP request whose completion
  lambda reached back into the client to call `decryptResource`/`setPlatformCert`,
  but nothing kept the client alive across the boundary. Every production owner
  holds it through a `shared_ptr` (the registry `make_shared`s it; the services
  `dynamic_pointer_cast` copies), so if the process tore the client down during
  shutdown while a refresh was still in flight, the late response dereferenced a
  dangling pointer. The class now derives from `enable_shared_from_this`, the
  handler captures a `weak_ptr` and locks it before touching any member, dropping
  the answer if the client is gone. The stack-constructed path used by the unit
  test still fails synchronously at auth-header build (before the async boundary),
  so it is unaffected; the actual race needs a live HTTP response plus teardown and
  is compile-only locally.

  acknowledged as handled even though nothing proved the winner finished.** The
  payment-callback reserve path (`CallbackService.cc`) and its refund twin used to
  answer `SUCCESS` whenever `ON CONFLICT DO NOTHING RETURNING` inserted 0 rows —
  i.e. whenever a concurrent delivery already held the key. But a reservation is
  written with `response_snapshot = NULL` and only finalized after the business
  transaction commits, so an empty insert proves *contention*, not *completion*.
  The read path had already decided the opposite for the same state (a NULL
  snapshot means "still in flight — drop and answer FAIL so the channel retries");
  the two paths disagreed on identical input. If the winning delivery then died
  before finalizing, its row stayed NULL, the loser had already returned 2xx,
  WeChat stopped retrying, and the settlement was stranded. Both reserve-race
  branches now answer `FAIL` (retry) and let the next delivery take the read
  path, which distinguishes a finalized snapshot from a stale reservation; the
  loser never touches business logic, so there is no double-settle risk. Needs two
  concurrent deliveries against PostgreSQL, so this is compile-only locally and
  waits on CI.
- **The certificate-download throttle spent its window on requests that never
  reached the network.** `downloadCertificates` stamped `lastCertDownloadAt_`
  before building the signed request, so a signing/config failure (a missing key)
  consumed the shared interval without issuing anything, starving the next genuine
  rotation refresh. The stamp now happens only after the auth header builds and
  the request is about to dispatch, keeping the check-and-stamp atomic. The
  round's "unknown-serial flood starves rotation" security claim was checked and
  refuted: `/v3/certificates` returns the entire current set and the loop installs
  every cert that validates regardless of which serial triggered the fetch, so an
  attacker can at most induce one signed GET per interval — the throttle is that
  rate-limiter by design, not a starvation vector.
- **The three-argument SPI `verifyCallback` accepted a signed body with no
  `resource`.** A WeChat V3 notification always carries the AES-GCM `resource`
  object that holds the transaction fields; a body without one is malformed, yet
  the method returned `true` with an empty `out_trade_no` and a null payload.
  It now returns `false`. This overload has no production caller (the live flow
  uses the six-argument signature-only variant), so the change is interface
  hardening with compile-only evidence.
- **Refund success moved the order to `REFUNDED` too early and via unguarded
  writes.** `updateRefundWithError`/`updateRefundWithSuccess` read the row then
  wrote the whole model back, so a dirty `status` column could overwrite state a
  fast notification had already settled, and a single successful refund flipped
  the parent order to `REFUNDED`. Both now use guarded `Mapper::updateBy` CAS
  (`WHERE status IN ('REFUND_INIT','REFUNDING')`), and the order moves to
  `REFUNDED` only on `REFUND_SUCCESS` and only under `WHERE status='PAID'`; the
  notification path gained the matching in-transaction order write. In-flight
  refund conflicts now report business code `1409` (HTTP 409), which is what
  `openapi.yaml` had promised all along.
- **The reconcile path settled an order without checking the amount.** When
  `queryOrder` pulled a live channel answer and the local order already read
  `PAID`, it synced the status without comparing the channel's amount against the
  booked `pay_payment.amount`. A new `reconcileAmountProblem` helper gates both
  doors — WeChat's `amount.total` (fen) and Alipay's `total_amount` (yuan, via
  `parseAmountToFen`) — so a mismatch or an answer carrying no amount logs an
  error and leaves the reported status unchanged rather than silently settling.
  `pay_payment` has no `currency` column (it lives on `pay_order`), so this guard
  compares amounts only; the currency check stays on the notification path.
- **`validateNotifyUrl` let an SSRF payload through on a `#` fragment, and had
  three other spelling gaps.** A fragment never terminated the host, so
  `http://127.0.0.1#x.com` parsed as an unrecognized host, passed the check, and
  the client connected to `127.0.0.1`. The host now runs to the first of
  `/:?#`. Also completed: userinfo is stripped at the *last* `@`; non-canonical
  IPv4 literals (`127.1`, `2130706433`, `0x7f.1`, `010.1.1.1`, ...) are refused
  via `isNumericAddressShape`; IPv6 uses a `2000::/3` whitelist rather than the
  old textual blocklist that any alternate spelling evaded. Positive controls
  (`host42.example.com`, a path `@`, a trailing `/#cb`) guard against the checks
  over-rejecting. Covered by `PayUtils_ValidateNotifyUrl` (42 assertions, green
  locally).
- **An uncertain WeChat answer on `/api/pay/create` closed the attempt that the
  callback needs to find.** Last round's rule — only an answer that *proves* the
  channel refused may close a booked `pay_payment` row — was wired into
  `createQRPayment` only, so the JSAPI failure branch still wrote `FAIL` (and the
  order `FAILED`) unconditionally, including after a timeout or a transport fault.
  `FAIL` is exactly the status `openAttemptsOfOrder()` hides, so a `prepay_id` that
  did get created on WeChat's side produced a paid order no callback or reconcile
  pass could locate: money landed and stayed unclaimed. Both create paths now
  share one predicate (`attemptCertainlyNotCreated`, renamed from the QR-local
  `qrAttemptCertainlyNotCreated`); an uncertain outcome logs a warning and leaves
  the attempt in flight, and the response to the caller is unchanged (`1002`).
  The predicate's two branches need PostgreSQL, so the local evidence here is the
  compile plus the existing QR case that pins the predicate itself; the JSAPI
  wiring waits on CI.
- **The Alipay notify route could still fault the process after verifying a
  signature.** `593813d` folded a missing plugin into the route's existing "no
  client to verify with" refusal, but past verification it dereferenced
  `plugin->paymentService()` unguarded — a different state: the plugin is
  registered, the service is not. That is the one place in the flow where an
  *already-verified* notification is about to be acknowledged, on a process that
  cannot book it. The lookup is now a presence test answered the same way as the
  client-missing branch (`{"code":"FAIL"}`, never an acknowledgement), so Alipay
  redelivers on a process that can. Neither this branch nor the one above it is
  reachable from a handler-level test — a test process that starts the plugin has
  a service — so this is compile-only evidence, stated as such in the audit.
- **A request that reached a handler with no plugin in the process stopped it.**
  Every route resolves its service through `drogon::app().getPlugin<PayPlugin>()`
  and then calls a member on what it gets back — ten places across
  `PayHandlers.cc` and `CallbackHandlers.cc` — on a pointer this project's own
  header documents may be null (`PayPlugin.h`: a host whose linker drops the
  DrObject self-registration symbol sees exactly that, which is why
  `ensureLinked()` exists; a config that registers the routes without a
  `PayPlugin` entry gets the same state). Calling a member on it is not a lost
  request but an access violation inside the handler — the one fault the new
  exception barrier cannot contain, since the barrier is only where the routes are
  registered. On the WeChat notify route the dereference also sat *before* body
  validation, so a mistyped field there answered nothing at all: last round's
  shape guard was unreachable in precisely that state. Each lookup is now a
  presence test (`plugin ? plugin->paymentService() : nullptr`) answered with 1501
  over HTTP 503 by `respondPluginUnavailable()` in the new
  `src/handlers/PluginGuard.h` — the code this contract already uses for one of our
  own dependencies being missing. The WeChat route resolves its service *after*
  validating the envelope, so a body it cannot route is still refused for its own
  reason, and the Alipay route folds a missing plugin into the branch it already
  had for "no client to verify with" (the same fault to a signature verifier, and
  never an acknowledgement, so no notification is consumed by a process that
  cannot book it). `openapi.yaml` carries the new 503 on the notify route and
  names the trigger in the shared `ServiceUnavailable` description. Two claims
  there were wrong and are corrected this round: that 503 carries the numeric
  error shape, not `CallbackAck` (whose `code` is the `SUCCESS`/`FAIL` string
  enum, and a refusal to handle is not an acknowledgement of anything); and the
  shared description said all three 503 faults carry business code 1501, when the
  auth layer answers plain text with no code at all and a channel this process has
  no client for answers `1002` on `/api/pay/create` and `1005` on
  `/api/qrpay/create`, both over HTTP 500 — only the refund routes report that
  fault as `1501`. The route's own refusal count was also overstated in the audit:
  eight lookups answer `1501`/503 (seven in `PayHandlers.cc`, one on the WeChat
  notify route), the Alipay route answers `FAIL`. This also
  corrects the local-evidence note in
  `docs/review/2026-09-20-wechat-pay-api-audit.md`: `RequestBodyShapeTest.cc` was
  *not* green at `4f028d0` — the two cases that reach the dereference died with
  `0xC0000005`, reproducibly:
  `PayHandlers_CreateQRPayment_OwnerAboveInt32Range_NotRefusedAsMistyped`, whose
  body is legitimate, and
  `CallbackHandlers_WechatNotify_EventTypeObject_Answers400InsteadOfThrowing`,
  which the pre-validation lookup refused the shape guard the chance to answer. It
  is the *test* that was right. Four
  cases now assert the answered fault (three 1501 over 503, one the Alipay `FAIL`
  the route already had), and the file is verified in both states: no
  plugin (run from the repo root) and the plugin the test config registers (run
  beside `config.json`, which is how ctest starts it) — 18/18 in each.
- **A request body whose member had the wrong JSON type stopped the process.**
  Every write route read its body through jsoncpp's `asString()`/`asInt64()`
  directly, and jsoncpp *throws* on a member it cannot convert — `{"amount":{}}`
  where a string was expected. The fault is narrower than "any wrong type", and
  since a reviewer's BLOCKER claimed otherwise, the pinned 1.9.5 was probed
  directly: `asString()` converts int, unsigned, int64, bool and null (a numeric
  `2200` reads back as `"2200"`) and throws only for an object or an array, while
  `asInt64()` throws for a string as well as for an object. So a mistyped body
  stops the process through exactly two doors — an object/array under a string
  read, and a string/object under an int64 read — and those are the shapes the
  guard refuses. Nothing caught it between the handler and the
  event loop: trantor's `EventLoop::loop()` catches an escaping exception, stops
  the loop and rethrows it as the stack unwinds, which returns from
  `app().run()` and takes the gateway down with it. The notify endpoint is
  anonymous, so that body came straight from the internet; the write routes sit
  behind an API key, so one leaked key was enough. The four POST surfaces now
  check the shape of every field they read before reading it
  (`validateBodyTypes` in `PayHandlers.cc`, an explicit `event_type` test in
  `CallbackHandlers.cc`) and answer 400, and `registerHttpHandlers` wraps every
  registration in a `guarded()` barrier that answers 500 only when a handler
  threw *before* responding — the wrapped callback fires at most once, so an
  asynchronous completion is never answered twice. The closure handed to a handler
  is a *copy*: handing over the original moved its target away, and the fault path
  then called an empty `std::function`, which threw out of the `catch` and escaped
  the barrier it was inside. Its answer also has to carry the status, not only a
  body saying `code: 500` — callers that branch on the HTTP status were being
  handed a 200.
- **Money could be booked under an owner no query can name.** `/api/pay/create`
  documents a 401 when no `user_id` is available, and that branch was dead on
  arrival: it wrapped `req->attributes()->get<int64_t>("user_id")` in a
  try/catch, but `Attributes::get` never throws — a missing key, and a key stored
  under another type, both read back as a default-constructed `0` and only log
  "Bad type". The request therefore continued into the database with
  `userId = 0`, which `queryOrderList` reads as "no owner filter" — an order
  booked under it has no owner: only the unfiltered listing shows it (any valid
  API key may ask for that), and no owner-scoped query can name it. Nothing in
  this repository ever sets that attribute, so no deployment path could reach the
  intended 401. The presence test is now `find()` and the owner has to be
  positive on both create paths; the QR route additionally read `user_id` with
  `asInt()` and a `FieldType::Int` gate, which refused (and would have truncated)
  any tenant id above 2^31−1, and
  `PaymentService::createQRPayment` resolved a missing buyer from the *string*
  default `"1"` — `asInt64()` on which throws for a direct service caller, and
  which silently attributed the order to tenant 1 otherwise.
- **`/api/qrpay/create` dropped the fields that decide where the money lands.**
  The handler rebuilt the service request from scratch and copied four members
  into it, so `currency`, `notify_url`, `buyer_id` and `idempotency_key` never
  reached the service: every QR order was priced in CNY, bound to the globally
  configured callback URL, never scoped to a buyer, and guarded only by the
  derived `QR_<order_no>_<channel>` key — the documented `X-Idempotency-Key`
  header had no effect on this route at all. The four now pass through (with the
  header as fallback for `idempotency_key`), the currency is upper-cased and
  validated as three letters before the channel is offered it, `notify_url`
  through the same SSRF gate `/api/pay/create` applies, and the amount through the
  same format check the other route has always run — without it an unrepresentable
  `total_amount` went to Alipay verbatim and was booked on the order row.
- **A second caller could replay another tenant's QR code.** The QR idempotency
  request hash covered `order_no`, `amount`, `channel` and `subject` only, so a
  request naming the same order number with a different `user_id`, `currency`,
  `notify_url` or `buyer_id` hashed identically and was answered as a *replay* of
  the first caller's code — one tenant paying into another's order, with the
  callback URL of the first. Those four fields are now part of the hash, so the
  collision surfaces as 1004 over HTTP 404 instead. The currency enters the hash
  as the value the booking actually uses rather than the string the caller typed:
  WeChat takes an upper-case ISO code and the service normalises lowercase input,
  so hashing the raw field made `"cny"` and `"CNY"` — and an absent field and an
  explicit `"CNY"` — two different requests for one identical order.
- **A refused attempt could shadow the payable one.** `pay_payment` carries one
  row per QR precreate attempt, and the callback and refund lookups both took
  "the newest row for this order" — which, once a channel refusal had closed a
  later attempt, was a row that can never settle. The settlement CAS then matched
  nothing, the notification was ACKed as SUCCESS with no money booked, and a
  refund asked WeChat to refund a transaction that never existed while the paid
  attempt stayed unrefunded. Both paths now filter to the attempts that can carry
  money (`INIT`/`PROCESSING`/`SUCCESS`/`REFUNDED`), matching the row the
  settlement branch settles; an order whose attempts are all closed is reported
  rather than acknowledged
  (`PayPlugin_WechatCallback_ClosedAttemptDoesNotShadowThePayableOne`). The
  refund lookup goes one step further than the filter: among those rows it takes
  the newest *settled* attempt before any open one, because a newer attempt that
  never got an answer still holds no money, and picking it answered
  "payment not successful" on an order that had genuinely been paid
  (`PayPlugin_Refund_SettledAttemptIsPickedOverANewerOpenOne`).
- **An unfinalized idempotency reservation was acknowledged as a handled
  callback.** Both callback branches answered the duplicate path — record the
  delivery, return SUCCESS — as soon as a `pay_idempotency` row existed, ignoring
  whether it had a response snapshot. A reservation with no snapshot is not
  evidence the callback was handled: the delivery that took it is either still
  running or died before its transaction committed, and acking it stops WeChat's
  retries on money that is booked nowhere. Both branches now drop the stale
  reservation and answer FAIL/1400, so the next delivery runs the full path —
  where the settlement CAS leaves a concurrent winner's work intact
  (`PayPlugin_WechatCallback_UnfinalizedReservationIsReprocessedOnRetry`). The
  delete carries the condition the read checked: the delivery that took the
  reservation can finalize its snapshot in between, and removing *that* row would
  erase the only evidence the callback was handled and let a later delivery settle
  it a second time. A delete that matches nothing answers the retry the same way.
- **A QR answer that proved nothing was booked as a refusal, twice over.** The
  gate that decides whether a failed QR attempt may be closed read
  "did this string come through HTTP?" — so a 2xx body carrying neither
  `code_url` nor `prepay_id` closed the payment row as `FAIL`, against the
  direction its own comment, the `LOG_WARN` beside it and `TECH_SPECS.md` all
  state: an answer that names no channel error proves nothing, and closing it
  hides a code the buyer may still pay from the notification and from
  reconciliation. That one case is now recognised by name and left in flight
  (`PayPlugin_QrBooking_AnswerWithoutCodeUrlKeepsTheAttemptInFlight`). The same
  row was also being spelled two different ways in one codebase: the Alipay
  status-sync branch wrote `FAILED` into `pay_payment.status`, where every other
  writer — `mapTradeState`, the QR close path — writes `FAIL` (`FAILED` is an
  *order* status), and a row spelled the other way matched no `FAIL`-keyed query
  and no open-attempt filter either.
- **The WeChat channel sent every outbound request with no timeout, and one of
  its certificate keys was read by nobody.** `timeout_ms` was documented
  (default 5000) and set in the example config, but `sendWechatRequest` never
  passed a timeout to `drogon::HttpClient::sendRequest`, whose default is `0` =
  disabled, so a stalled `api.mch.weixin.qq.com` left create/query/refund calls
  pending forever with the idempotency reservation held. The value is now read
  in the constructor, converted to Drogon's seconds on the way down, and a
  timed-out call reports `http request timed out after <n>ms` rather than the
  generic `http request failed`. `cert_refresh_interval_seconds` was the mirror
  image of the same carelessness: `PayPlugin::startCertRefreshTimer` did run a
  periodic refresh, on a `43200.0` literal it never read from config, so the key
  could neither lengthen nor shorten it. The timer now takes the configured
  value with a 300-second floor (below that it warns and keeps the default),
  which is what the example config and the two guides describe. `AlipayChannel.cc:29,428`
  passes its `timeout_ms` (30000) straight into the same seconds parameter, an
  eight-hour timeout in the same family; it is left to the Alipay track rather
  than changed beside this fix.
- **"WeChat answered" was being read as "WeChat succeeded" on the outbound
  path.** `sendWechatRequest` parsed the body and only reported an error when
  the transport failed, so a V3 `400/404` carrying `{"code":"ORDER_NOT_EXIST",
  "message":"..."}` came back as a successful call with a JSON payload nobody
  checked. Every caller that trusts the empty error string — `queryTransaction`
  during reconciliation, `refund`, `createTransactionNative` — then treated a
  rejected request as an accepted one. Non-2xx now yields
  `HTTP <status>: <code> <message>` (body text bounded at 200 chars, and
  unparseable bodies still fail). The raw body stays out of that string when no
  error envelope is present: the text is reflected into responses to our own API
  callers, and `api_base` is configuration, so whatever answers there must not
  be echoed through us — it goes to `LOG_TRACE` instead.
- **`createQRPayment` spoke Alipay to WeChat and then reported success.** The
  QR endpoint built one payload for every channel (`total_amount`/`subject`,
  yuan as a string), which `/v3/pay/transactions/native` rejects outright
  (`description` plus integer-fen `amount.total` are required), and its success
  gate tested the Alipay `code == "10000"` for all channels — so the WeChat
  branch never matched, and an error body produced `code: 0` with no `code_url`
  for the client to render. The payload is now built per channel and
  `channelResultError` decides success per channel (`code_url`/`prepay_id` for
  WeChat, and a *non-empty* one — `{"code_url": null}` satisfied an `isMember`
  test), with a bad amount returning 400 and clearing the reservation. The QR
  endpoint used to insert a `pay_order` but never a `pay_payment`, on either
  channel, so a fixed WeChat payload would have made money collectable on an
  order no callback can settle; the booking now happens before the channel is
  asked (see the next entry), which is what let the WeChat branch open instead
  of being refused outright.
- **`/api/qrpay/create` booked nothing the callback could settle.** Both channels
  ran the whole flow on a `pay_order` row alone: no `pay_payment` was ever
  inserted, so `CallbackService`, which resolves a notification by payment row,
  answered `FAIL` to a paid WeChat QR order and the money sat on an order that
  could not settle. The endpoint now writes the order (`CREATED`) and one payment
  row (`INIT`, with the channel request payload) *before* calling the channel,
  promotes them to `PAYING`/`PROCESSING` with the channel response once it
  accepts, and closes only the payment row (`FAIL`) when it refuses — a refusal
  leaves the order alone, because an earlier attempt's code may still be live.
  `pay_order.order_no` is unique, so a retry after a failure reuses that order
  and appends a new payment row rather than colliding; reuse is refused with 400
  when the order is already settled or describes a different amount or channel,
  which would otherwise settle the wrong charge. A row update that faults after
  the channel accepted the order still answers with the code rather than
  withholding a payable QR.
- **A duplicate notification on a multi-attempt order was rejected, not
  recorded.** Both idempotency-hit branches (transaction and refund) looked the
  payment up with `findOne`, which reports "Found more than one row" through its
  *error* callback — so on the order shape the booking above makes reachable (a
  refused attempt plus its retry) the audit row was never written, and the
  service answered `FAIL`/1400 to a notification it had already settled, which
  is precisely what tells WeChat to keep retrying. Both lookups now take
  `created_at DESC LIMIT 1`, the same row the settlement branch settles, so the
  `pay_callback` entry names the attempt the money belongs to instead of an
  arbitrary one, and an empty result still answers 1400 as before.
- **A business code in the body could ride an unrelated HTTP status.**
  `mapErrorToHttpStatus` decides the status from `error.value()`, but several
  `PaymentService` failures handed it `std::make_error_code(std::errc::...)`,
  whose value is an `errno` (22, 5) matching no case — so a body saying `400`,
  `1001` or `1005` arrived as HTTP 500, while the contract text claimed the two
  were paired. The WeChat QR path now carries its business code through
  `makePayError`, and 400 has a mapping; the remaining paths still land
  on 500, which `openapi.yaml` now states instead of over-promising. Classifying
  every service error by business code is a separate change.
- **An unmappable refund status defaulted to `REFUNDING`.** `RefundService`
  read `status` from the refund response and fell through to `REFUNDING` for
  anything it did not recognise — including an absent field, which is what a
  WeChat error body looks like. A refund that never started was therefore
  booked as in-flight. Unknown values now take the failure branch of that
  response — `1502` over HTTP 502, which it shares with the uncertain outcomes
  below, so the body's `data.status` is what distinguishes them — and so do
  `CLOSED`/`ABNORMAL`, which
  `mapRefundStatus` had already turned into `REFUND_FAIL` before the success
  path stored them anyway — the order ended up `REFUNDED` with `code: 0` on a
  refund WeChat refused. Conversely a *transport* failure is not a refusal:
  timeouts, 5xx and empty 2xx bodies no longer write the terminal
  `REFUND_FAIL` (which invites a retry under a fresh `out_refund_no`, i.e. a
  double refund) but keep the record `REFUNDING` for reconciliation, and only
  the response the channel explicitly rejected reports `REFUND_FAIL`. A channel
  fault that never sent a request at all (missing config, client not ready) is
  certainly not a refund and still books terminal, as it did before.
- **The platform certificate trusted whoever said so.** `verifyCallback`
  accepted the statically configured platform certificate for any notification
  whose `Wechatpay-Serial` equalled the merchant's own `serial_no` — two
  unrelated numbering spaces — and `setPlatformCert` cached a downloaded
  certificate under whatever serial the response body claimed. A signed
  notification could thus be verified against a certificate bound to the wrong
  name, and a spoofed `/v3/certificates` response could poison the cache. The
  header must now name the serial *inside* the certificate (compared on the
  normalised form, since WeChat writes uppercase hex without padding), the
  cache is keyed by that same serial, and each certificate is parsed, checked
  against its validity window and optionally chained to
  `platform_ca_cert_path` before it is stored. An unseen serial triggers a
  throttled refresh and rejects the notification — WeChat retries, by which
  time the rotation is cached. The throttle floor is one second: the callback
  endpoint is public, so a configurable `0` turned every notification naming an
  unknown serial into an outbound signed request.
- **Merchant identifiers were interpolated into the signed URL unencoded.**
  `/v3/pay/transactions/out-trade-no/{no}?mchid=` and
  `/v3/refund/domestic/refunds/{no}` took the order/refund number as it came,
  and the signature covers exactly that string: a number carrying a literal
  `?`, `#`, `&` or `/` moved a validly signed request to another resource. Both
  path segments now go through `pay::utils::urlEncodePathSegment`.
- **Callback amount guards.** A notification whose `amount.payer_total` exceeds
  the order `total` is refused; the other direction has to pass, because a
  coupon legitimately puts it below `total` and a fully covered order at exactly
  0, so the gap alone only logs (the ledger books `total`). One whose
  `transaction_id` differs from the `channel_trade_no` already booked on that
  payment is refused — otherwise a second WeChat transaction could be settled
  under another order.
- **The AEAD IV length came from the payload.** `decryptAesGcm` set the GCM IV
  size from the notification's `nonce` field, letting a malformed resource
  choose the cipher parameters. WeChat fixes it at 12 bytes, so any other
  length is now rejected, and the final tag write no longer lands one past the
  end of the plaintext buffer.
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
  implementation still is: the job is named `macos-build` because an earlier
  honest-naming pass renamed it off `macos-build-and-test` precisely for having
  no databases — in the standalone `ci-macos.yml` that name was also the whole
  context, which is why the drift below was invisible for so long. What the leg
  does gate — the arm64 clang `-Werror` compile, sole reporter of
  `-Wunused-lambda-capture` — needs no services, and the runtime suite stays
  covered on Linux and Windows.

- **Deleting the legacy workflows deleted the only reporters of three required
  status checks.** The ruleset required the bare contexts
  `linux-build-and-test`, `windows-build-and-test` and `macos-build`, and the
  legacy per-platform copies were the only real reporters of those strings.
  `ci.yml` looked like a second reporter because its matrix carries the same
  three names, but a job that calls a reusable workflow with `uses:` reports its
  check as `<caller job name> / <name the called workflow gives its own job>`, so
  `matrix.check_name` had only ever produced the first half. Removing the copies
  left the ruleset requiring three contexts nothing would ever report — which
  does not lift merge protection, it inverts it into a stall: the PR that carried
  the deletion sat at `mergeStateStatus: BLOCKED` with those three checks pending,
  and every later PR would have too, since no workflow could report them again.
  The cause was hidden by the tooling, because `gh pr checks` lists check runs
  that exist and never a required check that has none, so the run read all-green
  while the merge was blocked. The ruleset now requires the three
  contexts that `ci.yml` genuinely reports — `linux-build-and-test /
  build-test`, `windows-build-and-test / build-test`, `macos-build / build-test`
  — which keeps the protection it was meant to enforce and makes it match
  reality; `AGENTS.md` "CI", the `ci.yml` header and the `ci-monitor` agent
  document carry the mechanism plus the two-command `gh api` diff that catches a
  recurrence, and `TECH_SPECS.md` states the rule in its CI governance table.

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
- **One settled refund booked the whole order as `REFUNDED`.** WeChat accepts
  up to fifty partial refunds per order and `trade_state=REFUND` answers for a
  partly refunded trade too, but both settlement sites — the refund-success
  path in `libs/drogon-pay/src/services/RefundService.cc` and the refund
  notification path in `libs/drogon-pay/src/services/CallbackService.cc` —
  wrote `pay_order.status = 'REFUNDED'` on any refund that reached
  `REFUND_SUCCESS`. Returning 3.00 of a 10.00 order told every consumer keying
  on `REFUNDED` that the money was back while 7.00 was still with the
  merchant. Both sites now gate on `pay::utils::refundsCoverOrderAmount`: the
  order flips only when the sum of its `REFUND_SUCCESS` refund rows covers
  `pay_order.amount` (the callback-path read runs inside the notification's own
  transaction, so it sees the row that settlement just wrote), and an amount
  nobody measured leaves the order as it is. `PayUtils_RefundsCoverOrderAmount`
  pins the predicate; the pairs `PayPlugin_Refund_PartialRefundKeepsOrderPaid`
  / `PayPlugin_Refund_CumulativeRefundsSettleOrder` and the matching pair in
  `tests/integration/WechatCallbackIntegrationTest.cc` pin both sites through
  real settlement flows — a partial refund leaves the order `PAID` while its
  refund row still reads `REFUND_SUCCESS`, and refunds that do cover the total
  flip it (a positive control, so the gate cannot pass by never writing). The
  two remaining `REFUNDED`-adjacent landings named in the audit — the
  `trade_state=REFUND` sync paths — are deliberately left to the next batch.
- **A `REFUND` answer from the query or the transaction notification booked
  `REFUNDED` ungated.** The previous batch closed the two refund settlement
  sites but left the doors the audit had named: the trade notification maps
  `trade_state=REFUND` straight onto `pay_order.status`, and so does the order
  query sync in `libs/drogon-pay/src/services/PaymentService.cc` — yet
  `REFUND` only says the trade entered refunding, which one settled partial
  refund of an order is enough to produce. A dropped refund notification plus
  one query therefore still recorded 3.00 back on a 10.00 order as the whole
  order returned. All three remaining write points now pass the claim through
  `pay::utils::resolveRefundedOrderStatus` against the same settled-refund sum
  (read inside each path's own transaction, queued ahead of the write): an
  uncovered `REFUNDED` lands as `PAID` — which is what a REFUND trade has
  nonetheless proven — and a covered one stands. `syncRefundStatusFromWechat`
  in `libs/drogon-pay/src/services/RefundService.cc` also gained the settlement
  its name promises: when a queried refund has settled and the refunds on the
  order together cover its total, the order is moved to `REFUNDED` under the
  same PAID-guarded CAS, recovering the concurrent case where two settlements
  each counted without the other. `PayUtils_ResolveRefundedOrderStatus` pins
  the predicate; `PayPlugin_QueryOrder_WechatRefundSettlesOrderOnlyWhenCovered`
  and `PayPlugin_WechatCallback_TransactionRefundStateCoveredSettlesOrder` pin
  both new doors with their positive controls, and the round-11/round-12
  cases flipped to the ledger-backed expectation. What a single channel answer
  can still never prove — that the refund it mentions exists at all — remains
  guarded only by the amount check, as before.

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
