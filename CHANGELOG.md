# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
  out (the `pay_payment` booking the QR endpoint is missing, the bare-`this`
  capture in the certificate-download callback, the Alipay timeout unit mix-up).
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
  endpoint inserts a `pay_order` but never a `pay_payment`, on either channel,
  so a fixed WeChat payload would have made money collectable on an order no
  callback can settle; WeChat QR creation now answers 501 until that booking
  gap closes, which keeps the endpoint honest rather than silently wrong.
- **A business code in the body could ride an unrelated HTTP status.**
  `mapErrorToHttpStatus` decides the status from `error.value()`, but several
  `PaymentService` failures handed it `std::make_error_code(std::errc::...)`,
  whose value is an `errno` (22, 5) matching no case — so a body saying `400`,
  `1001` or `1005` arrived as HTTP 500, while the contract text claimed the two
  were paired. The WeChat QR path now carries its business code through
  `makePayError`, and 400 and 501 have mappings; the remaining paths still land
  on 500, which `openapi.yaml` now states instead of over-promising. Classifying
  every service error by business code is a separate change.
- **An unmappable refund status defaulted to `REFUNDING`.** `RefundService`
  read `status` from the refund response and fell through to `REFUNDING` for
  anything it did not recognise — including an absent field, which is what a
  WeChat error body looks like. A refund that never started was therefore
  booked as in-flight. Unknown values now map to no status at all and take the
  failure branch (`1502`, `REFUND_FAIL`), and so do `CLOSED`/`ABNORMAL`, which
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
