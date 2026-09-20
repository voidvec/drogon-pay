# Contributing to drogon-pay

Thanks for considering a contribution! This document covers the local
workflow, commit conventions and the extra rules that apply to a payment
library.

## Build & test

Prerequisites: CMake ≥ 3.21, Conan 2, a C++17 toolchain
(MSVC 2022 / GCC / Clang), PostgreSQL 13+ and Redis 6+ for the test suite.

```bash
# 1+2. Dependencies, configure and build in one step. The dev scripts are
#      parameter-for-parameter twins: build.sh on Linux/macOS, build.bat on
#      Windows (add -debug for the *-debug presets). They run
#      `conan install` + `cmake --preset` + `cmake --build --preset` and copy
#      config.json/.env/certs next to the binaries.
examples/pay-server/scripts/build.sh            # Linux / macOS
examples\pay-server\scripts\build.bat           # Windows

# 3. Provision the test database (superuser: CREATE ROLE test / CREATE DATABASE
#    pay_test OWNER test), build its schema with
#    examples/pay-server/scripts/setup_database.{sh,bat}, then run tests
#    (same twins: test.sh / test.bat, or ctest directly)
ctest --test-dir build/windows-msvc -C Release --output-on-failure

# 4. Consumer-view verification (recipe + test_package)
conan create . --build=missing -s build_type=Release -s compiler.cppstd=17
```

Presets: `windows-msvc` / `windows-msvc-debug` (multi-config, binaries under
`build/<preset>/.../Release|Debug`) and `linux-release` / `linux-debug` /
`macos-arm64` / `macos-debug` / `linux-coverage` (single-config).

CMake options: `DROGON_PAY_BUILD_EXAMPLES` / `DROGON_PAY_BUILD_TESTS`
(both default ON; `PAY_BUILD_TESTS`/`BUILD_TESTS` kept as CI-compatible aliases).

Line endings come from `.gitattributes`, not from your `core.autocrlf`: text is
LF in the working tree as well as in the repository, `.bat`/`.cmd` stay CRLF.
This is not cosmetic — `scripts/check_migrations.py` and `migrate_db.py` hash
file bytes, so a CRLF working copy disagrees with the pins and with the Linux
runner. If a clone predates the attributes file, re-materialise it with
`git rm --cached -r -q . && git reset --hard` (or re-clone).

## Pull request checklist

- [ ] Three-platform CI green (Linux / macOS / Windows) — required
- [ ] New/changed behavior covered by `DROGON_TEST` tests in `tests/`
- [ ] `clang-format` clean (config in `.clang-format`; run
      `pre-commit run --all-files` locally)
- [ ] No secrets in the diff (gitleaks runs in CI; install the local hook via
      [pre-commit](https://pre-commit.com): `pre-commit install`)
- [ ] Architecture guard passes (`python scripts/check_architecture.py`)
- [ ] All CI static gates pass locally — each gate is a stdlib-only script
      under `scripts/check_*.py`; run them all before pushing
- [ ] `CHANGELOG.md` updated under `[Unreleased]` for user-visible changes
- [ ] Docs updated when config keys, routes or public headers change

## Commit conventions

Conventional Commits: `<type>(<scope>): <imperative subject>` (≤ 72 chars).

- **type**: `feat`, `fix`, `refactor`, `docs`, `test`, `build`, `ci`,
  `chore`, `perf`, `style`
- **scope**: the affected area, matching existing history — e.g. `linux`,
  `windows`, `macos`, `tests`, `logs`, `ci-gate`, `idempotency`, `alipay`,
  `wechat`, `handlers`, `services`, `channels`, `cmake`, `pay-admin`,
  `docs`, `agents`
- **Breaking changes**: append `!` after the scope (`feat(spi)!: ...`) and
  say `BREAKING:` in the body; update the migration table in
  `docs/development/plugin_integration.md`.

Examples from history: `refactor(logs): address review feedback on six-tier
standardization`, `ci(linux): drop diagnostic diff output from clang-format
check`, `fix(idempotency): persist snapshot before responding on refund`.

## Architecture rules (CI-enforced)

Dependency direction is one-way and guarded by
`scripts/check_architecture.py`:

1. `libs/drogon-pay/src/**` must not include anything from `examples/`
   (the library never depends on a host).
2. Public headers `libs/drogon-pay/include/drogon_pay/**` must not include
   internal `src/` headers (no implementation leakage).
3. `libs/drogon-pay/src/services/**` must not include concrete channel
   headers from `src/channels/**` — services talk to the
   `PaymentChannel` SPI only. (The `dynamic_pointer_cast` exception for
   channel-specific capabilities lives in the handlers layer.)

The public API surface is a frozen whitelist (4 headers). Adding a header to
`include/drogon_pay/` requires updating the whitelist in
`scripts/check_architecture.py` in the same PR — deliberate friction to keep
the API surface small.

## Schema changes

Migrations live in `sql/` as `NNN_snake_case.sql` and are applied by exactly
one program, `scripts/migrate_db.py`, which records each version in
`schema_migrations` and refuses to run when a file already applied has changed
bytes. The practical rules (see `TECH_SPECS.md` "迁移工程化" and the
`/create-migration` skill for the full checklist):

- Never write `psql -f sql/...` in a workflow or script — add the file to `sql/`
  and the executor picks it up. Six copies of that list used to exist and they
  had already drifted: the two in the CI workflows ran only `001`+`002` and
  silently skipped the other two versions, while the deploy scripts globbed a
  `sql/` path the plugin refactor had moved, so they applied nothing and still
  logged success.
- New migrations must be idempotent and non-destructive;
  `python scripts/check_migrations.py` enforces that plus naming and an
  unbroken version chain, and CI runs it as a hard gate.
- `sql/000_*.sql` is a dev reset helper, not a version; it is never applied by
  the executor (a deploy that ran it dropped every table on redeploy).
- Once a migration has shipped, pin it with
  `python scripts/check_migrations.py --write-missing`. It adds entries for new
  files only, never rewrites an existing one, and refuses a candidate that
  breaks the content rules — a pinned file is exempt from them for good.
  Changing a baselined file means editing `scripts/migrations_baseline.json` in
  the same PR.
- Creating/dropping the *database* is provisioning, not a migration: the app
  role has no `CREATEDB`, so the executor only probes and prints the superuser
  command instead of running it.

## Releasing

The version lives in three places and nowhere else: `project(drogon-pay VERSION …)`
in `CMakeLists.txt`, `version = …` in `conanfile.py`, and the top-level `"version"`
in `examples/pay-admin/package.json`. `scripts/check_version_sync.py` keeps them
honest, and `static-analysis` runs it on every pull request.

To release: move `[Unreleased]` into a dated `## [x.y.z]` section, bump the three
declarations in the same commit, then `git tag v1.2.3 && git push --tags`.
Do not restate the version in config or deploy comments; that is how it drifted before
the checker existed.

Pushing a `v*` tag runs `.github/workflows/release.yml`: `version-check` (the same
script with `--tag "$GITHUB_REF_NAME"`, so a tag whose version is not in the tree or
whose CHANGELOG section is missing fails in seconds), then `ci-gate` (the tagged
commit must already be on `master` *and* the merge pipeline must have gone green on
it — a tag is never merged, so the branch ruleset's required checks cannot be
trusted to have run), then `sdk-smoke` — the consumer
`conan create` + `test_package` gate from `_sdk-smoke.yml`, on Linux and Windows —
and only then `publish`, which uses that CHANGELOG section as the release body. Use
`/release` for the runbook; do not create the release by hand.

`ci-gate` is `.github/workflows/_tag-gate.yml` called through `workflow_call`, and
`.github/workflows/deploy.yml` calls the same file before it pushes an image or
touches ECS: `jobs.*.needs` cannot cross workflows, so a red release would never
have stopped that pipeline's production leg. A tag that fails the gate ships
nothing, in either workflow. The gate closes that path going forward rather than
retroactively, though — Actions runs the definition *stored in the tagged commit*,
so a tag aimed at a commit older than the gate is still built by the file that
commit carries. It also now rejects a `v*` tag outside `v` + three numeric
segments, where such a tag previously reached the image-push and ECS-roll legs with
nothing saying otherwise.

## Contributing a payment channel

New channels are host-side plugins, not library edits, in most cases:
implement `drogon_pay::PaymentChannel`, register a factory, enable it in
config — see the
[custom channel guide](docs/development/plugin_integration.md#四自定义渠道开发指南).

For a channel to be accepted **into** the library (`src/channels/`):

- Must satisfy the SPI contract: thread-safe, HttpClient reused per IO loop
  (`drogon::IOThreadStorage`), callbacks normalized via `verifyCallback`
- Asymmetric capabilities stay on the concrete class (no SPI bloat)
- Registered in `PayPlugin::registerBuiltinChannels()` (no self-registration
  macros — static-library builds drop those symbols)
- Integration tests included; no test may hit a real payment endpoint

## Reporting security issues

See [SECURITY.md](SECURITY.md) — never open public issues for
vulnerabilities.
