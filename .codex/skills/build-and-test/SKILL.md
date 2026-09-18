---
name: build-and-test
description: Build the drogon-pay C++ project and run its Drogon DROGON_TEST suite on Windows, Linux or macOS.
---

# Build & Test

Both platforms have one script with the same flags; pick the twin that matches
your OS. Each wraps `conan install` + `cmake --preset` + `cmake --build`, then
copies `config.json`, `.env` and `certs/` next to the binaries.

| Platform | Script | Presets it selects |
|----------|--------|--------------------|
| Windows | `examples/pay-server/scripts/build.bat` / `test.bat` | `windows-msvc` (Release), `windows-msvc-debug` |
| Linux | `examples/pay-server/scripts/build.sh` / `test.sh` | `linux-release`, `linux-debug` |
| macOS | same as Linux | `macos-arm64` (Release), `macos-debug` |

Flags are identical in both twins: `-debug` / `-release` (default Release).
`test.bat` / `test.sh` additionally take `-l` (list), `-r <pattern>`,
`-v` (verbose), `-o` (also write `test_results.log`).

```powershell
examples\pay-server\scripts\build.bat            REM Release
examples\pay-server\scripts\build.bat -debug     REM Debug
```

```bash
bash examples/pay-server/scripts/build.sh        # Release
bash examples/pay-server/scripts/test.sh -r Idempotency
```

Output paths differ only because MSVC is a multi-config generator:

| | Server | Tests |
|---|--------|-------|
| Windows | `build/windows-msvc/examples/pay-server/Release/PayServer.exe` | `build/windows-msvc/tests/Release/PayBackendTests.exe` |
| Linux/macOS | `build/linux-release/examples/pay-server/PayServer` | `build/linux-release/tests/PayBackendTests` |

## Test

Preferred entry point is ctest — it is what CI runs, and the whole suite is
registered as a single `PayBackendTests` entry:

```bash
ctest --test-dir build/linux-release --output-on-failure            # Linux
ctest --test-dir build\windows-msvc -C Release --output-on-failure  # Windows
```

Requirements: PostgreSQL and Redis on `127.0.0.1`, database `pay_test` built by
`examples/pay-server/scripts/setup_database.{sh,bat}` (see `/db-reset`). The
binary reads its credentials from the `.env` copied beside it, so no script or
command line carries a password; the test listener runs on 5567
(`PAY_TEST_PORT`) to avoid hijacking a running PayServer on 5566.

Do not assume a gtest-style filter flag: this project tests with Drogon
`DROGON_TEST`, and filtering happens through ctest (`-R`).

## Manual configure (only when a preset does not fit)

`conan install` must run first for the target preset folder, because the preset's
`CMAKE_TOOLCHAIN_FILE` points inside it:

```bash
conan install . --output-folder=build/linux-release -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset linux-release && cmake --build --preset linux-release
```

C++ standard: C++17 (`CMakeLists.txt`).

## Run the server

From the directory holding the binary, so `config.json` and `.env` resolve:

```bash
cd build/linux-release/examples/pay-server && ./PayServer
```

## Error recovery

| Symptom | Fix |
|---------|-----|
| `Error: Conan install failed` | Re-run `conan install . --output-folder=build/<preset> --build=missing -s build_type=<cfg> -s compiler.cppstd=17` and read the recipe output |
| `Error: CMake configuration failed` | Check `build/<preset>/build/generators/conan_toolchain.cmake` exists (that is the conan step above); otherwise delete `build/<preset>` and rebuild |
| `fatal error C1083` (missing header) | Conan packages missing for that preset; re-run the install for the same `--output-folder` |
| Tests hang on `!!!Pg connection failed` | Postgres is unreachable or the `test` role / `pay_test` db is missing — infinite retry, not a code bug; fix the database (`/db-reset`) |
| Port 5566/5567 conflict | Kill the running server: `taskkill /F /IM PayServer.exe` or `pkill -x PayServer` |

## Guard rails

- A `git commit` runs `build/windows-msvc/tests/Release/PayBackendTests.exe`
  through a PreToolUse hook; a failing suite blocks the commit.
- Edits to `libs/drogon-pay/src/models/**` are blocked — they are generated
  (`/orm-gen`).
- `.claude/settings.json` denies reading `examples/pay-server/.env*`; keep
  secrets out of command lines and logs.
- PostToolUse runs `python scripts/clang_format.py --fix` on `Edit`.
