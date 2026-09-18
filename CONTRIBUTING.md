# Contributing to drogon-pay

Thanks for considering a contribution! This document covers the local
workflow, commit conventions and the extra rules that apply to a payment
library.

## Build & test

Prerequisites: CMake ≥ 3.21, Conan 2, a C++17 toolchain
(MSVC 2022 / GCC / Clang), PostgreSQL 13+ and Redis 6+ for the test suite.

```bash
# 1. Dependencies (per-preset output folder)
conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing

# 2. Configure + build (presets: windows-msvc / linux-release / macos-arm64)
cmake --preset windows-msvc
cmake --build --preset windows-msvc

# 3. Create the test database (user test / db pay_test), then run tests
ctest --test-dir build/windows-msvc -C Release --output-on-failure

# 4. Consumer-view verification (recipe + test_package)
conan create . --build=missing -s build_type=Release -s compiler.cppstd=17
```

CMake options: `DROGON_PAY_BUILD_EXAMPLES` / `DROGON_PAY_BUILD_TESTS`
(both default ON; `PAY_BUILD_TESTS`/`BUILD_TESTS` kept as CI-compatible aliases).

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
