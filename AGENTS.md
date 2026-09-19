<INSTRUCTIONS>
## Scope
This project uses Claude Code configuration stored under `.claude/`. The
operational facts below apply to **any** coding agent working in this repo.

## Quick Start (cross-tool)

### Build

| Platform | Command |
|----------|---------|
| Linux | `examples/pay-server/scripts/build.sh` (Release; `-debug` for `linux-debug`) |
| macOS | `examples/pay-server/scripts/build.sh` (Release; `-debug` for `macos-debug`) |
| Windows | `examples\pay-server\scripts\build.bat` (Release; `-debug` for `windows-msvc-debug`) |

The two scripts take the same flags and do the same thing: `conan install . --output-folder=build/<preset>
-s build_type=<Release|Debug> -s compiler.cppstd=17 --build=missing` (plus `-s arch=armv8` on macOS), then
`cmake --preset <preset>` and `cmake --build --preset <preset>`, then copy `config.json`/`.env`/`certs/`
next to the binaries. The raw Conan+CMake sequence in those scripts is the fallback for custom
configurations.

### Test

| Scope | Command |
|-------|---------|
| Full suite (Linux/macOS) | `examples/pay-server/scripts/test.sh` |
| Full suite (Windows) | `examples\pay-server\scripts\test.bat` |
| Raw ctest (after a build) | `ctest --test-dir build/<preset> --output-on-failure` (add `-C Release` for the MSVC presets) |
| Line coverage (Linux/gcc only) | `cmake --preset linux-coverage` + build + ctest, then `python3 scripts/measure_coverage.py --dir build/linux-coverage --report` — full recipe and ratchet rules in [TECH_SPECS.md](TECH_SPECS.md) "行覆盖率计量" |

Test framework: Drogon `DROGON_TEST` (not gtest). Test target: `PayBackendTests`.
Integration tests need Postgres+Redis on `127.0.0.1` with the `test` role and
`pay_test` database (see `examples/pay-server/.env`, gitignored). The coverage
baseline is *measured* by the first green run of
`.github/workflows/coverage.yml` — that job, not a laptop, is the source of
truth for the numbers — but a run's write is discarded with the runner's working
copy, so the measured numbers have to be committed as
`scripts/coverage_baseline.json` before the ratchet can hold anything. On this
machine WSL is NAT'd with its own (role-less) Postgres 16 on loopback, so a
WSL `ctest` hangs on DB auth rather than failing — do not chase that as a
code bug.

### CI

| Gate | Definition | Checks it produces |
|------|-----------|--------------------|
| FAST | `static-analysis` + `clang-tidy` jobs in `.github/workflows/ci.yml` | source-only guards, no compilation |
| MAIN | `.github/workflows/_build-test.yml` (called per platform by `ci.yml`) | `linux-build-and-test`, `windows-build-and-test`, `macos-build` |
| RELEASE | `.github/workflows/_sdk-smoke.yml` (called per platform by `ci.yml`) | `sdk-smoke-linux`, `sdk-smoke-windows` |

`ci.yml` is the single entry point: the per-platform workflow copies it replaced
(plus the standalone Windows-only Conan-package job) ran beside it until one
commit produced a fully green pass of both chains, and are now deleted. Do not
reintroduce a per-platform workflow file — extend `ci.yml`.

**The three MAIN check names are required status checks in the branch
ruleset.** Never rename them (a rename silently removes merge protection);
they are set by `matrix.check_name` in `ci.yml`, not inside the reusable
workflows. Postgres/Redis for the DB-backed suite is provisioned on two of the
three legs: Docker containers on Linux, and the runner's own PostgreSQL service
plus Memurai on Windows. The macOS leg is **build-only** — `macos-14` is past
Homebrew's support window and has no bottles, so installing a database there
compiles a toolchain instead (a real attempt consumed the job's whole
120-minute timeout in `brew install` and never reached the suite). What that leg
does prove is the arm64 clang `-Werror` compile, which is the only lane that
sees clang-exclusive diagnostics. `.github/workflows/legacy-source-build.yml`
holds the pre-Conan build-Drogon-from-source rollback net and is dispatch-only.

### Database

`scripts/migrate_db.py` is the **only** migration executor: it discovers the
versioned files under `sql/`, applies what is missing in version order, commits
each one together with its `schema_migrations` row, and refuses to run when a
version already applied has changed bytes. CI, the deploy scripts and local dev
all call it — no consumer keeps its own file list any more (the copies that did
had drifted, and the deploy loop pointed at a moved directory so it applied
nothing while reporting success).

| Task | Command |
|------|---------|
| Hygiene guard (CI step) | `python3 scripts/check_migrations.py` |
| What the database has recorded | `python3 scripts/migrate_db.py --status` |
| What the next run would apply | `python3 scripts/migrate_db.py --dry-run` |
| Dev reset + full replay | `examples/pay-server/scripts/setup_database.sh` (Windows: `.bat`) |
| Adopt a volume that initdb.d already built | `python3 scripts/migrate_db.py --baseline` |

`sql/000_*.sql` is a dev reset helper, not a version, and the executor skips it.
Creating or dropping the *database* is provisioning: the app role has no
`CREATEDB`, so the executor only probes and prints the superuser command.
Rules in [TECH_SPECS.md](TECH_SPECS.md) "迁移工程化"; new files via
`/create-migration`.

### Versioning

The version is declared three times and never derived: `project(drogon-pay
VERSION …)` in `CMakeLists.txt`, `version = …` in `conanfile.py`, and the top-level
`"version"` in `examples/pay-admin/package.json`. `scripts/check_version_sync.py`
enforces it: the FAST gate runs it with no arguments, which requires the three
declarations to agree, and the `version-check` job that opens
`.github/workflows/release.yml` re-runs it with `--tag "$GITHUB_REF_NAME"`, which
also requires the tag to equal them and `CHANGELOG.md` to carry a `## [x.y.z]`
section. Never hand-write a version into a deploy/config comment: those strings
were the drift source and have been deleted. Bumping = three declarations + a new
CHANGELOG section, then tag.

## Critical Constraints (always enforce)

- **Never modify ORM models** in `libs/drogon-pay/src/models/` (generated by `drogon_ctl`).
- **Never use raw SQL** — use Mapper + Criteria (async callbacks). See `.claude/rules/db-operations.md`.
- **Never hand-write a migration file list** — `sql/NNN_*.sql` is applied by `scripts/migrate_db.py` and policed by `scripts/check_migrations.py`.
- **Prefer async callbacks**; capture shared state as `[sharedCb]` in lambdas.
- **Use Service API** (`service->method(req, apiKey, callback)`), not the legacy Plugin API.
- **Secrets never in source** — use `__env_var:NAME__` placeholders in `config.json`; real values via environment variables (`.env` is gitignored).
- **Log levels follow the six-tier spec** in [TECH_SPECS.md](TECH_SPECS.md) "日志分级规范": `LOG_INFO` = lifecycle/milestone events only (per-request flow steps use `LOG_DEBUG`); fire-and-forget helper failures (ledger/idempotency snapshot) use `LOG_WARN`; process-exit-only failures use `LOG_FATAL` (rare); `LOG_TRACE` for raw bodies/signing internals.

## Architecture (layers, top-down)

`handlers/` → `services/` → `channels/` → `models/` (ORM, read-only)

HTTP surface: `examples/pay-server/openapi.yaml` is the contract, and
`scripts/check_openapi_routes.py` fails the FAST gate when code and spec
disagree. See `.claude/skills/openapi-update/SKILL.md`.

Full technical spec: [TECH_SPECS.md](TECH_SPECS.md) | Claude-specific guide: [CLAUDE.md](CLAUDE.md)

## Git Workflow

- Commit is allowed; **push requires review**.
- Debug code must be removed before commit (use `LOG_DEBUG`).
- Done = tests pass + static analysis pass + CI green + docs updated.

## Claude Code Assets

Tombstoned entries (`status: deprecated` in front matter) are retired
authforge/OAuth2 leftovers kept as placeholders so tooling can detect they
were deliberately removed; never revive their content.

### Agents
- api-documenter (file: .claude/agents/api-documenter.md)
- ci-monitor (file: .claude/agents/ci-monitor.md)
- code-reviewer (file: .claude/agents/code-reviewer.md)
- compliance-checker (file: .claude/agents/compliance-checker.md) [retired: superseded by security-reviewer]
- payment-test-reviewer (file: .claude/agents/payment-test-reviewer.md)
- performance-analyzer (file: .claude/agents/performance-analyzer.md)
- security-reviewer (file: .claude/agents/security-reviewer.md)
- test-writer (file: .claude/agents/test-writer.md)

### Rules
- data-access (file: .claude/rules/data-access.md)
- db-operations (file: .claude/rules/db-operations.md)
- dev-workflow (file: .claude/rules/dev-workflow.md)
- orm-models (file: .claude/rules/orm-models.md)

### Skills
- build-and-test (file: .claude/skills/build-and-test/SKILL.md)
- create-migration (file: .claude/skills/create-migration/SKILL.md)
- db-reset (file: .claude/skills/db-reset/SKILL.md)
- docker-integration-test (file: .claude/skills/docker-integration-test/SKILL.md)
- docker-manage (file: .claude/skills/docker-manage/SKILL.md)
- drogon-build (file: .claude/skills/drogon-build/SKILL.md)
- e2e-test (file: .claude/skills/e2e-test/SKILL.md) [retired: superseded by docker-integration-test]
- openapi-update (file: .claude/skills/openapi-update/SKILL.md)
- orm-gen (file: .claude/skills/orm-gen/SKILL.md)
- production-readiness-docs (file: .claude/skills/production-readiness-docs/SKILL.md)
- project-conventions (file: .claude/skills/project-conventions/SKILL.md)
- release (file: .claude/skills/release/SKILL.md)
</INSTRUCTIONS>
