---
name: drogon-build
description: Build Drogon C++ application (drogon-pay library + PayServer example host) using the project's Conan + CMake presets + MSVC toolchain via examples\pay-server\scripts\build.bat.
---

# Drogon Build

Build the drogon-pay library and the PayServer example host using the
project's Conan 2 + CMake presets + MSVC toolchain via
`examples/pay-server/scripts/build.bat` (the script switches to the
repository root itself, so it can be invoked from anywhere).

## Quick Build

```powershell
examples\pay-server\scripts\build.bat
```

### Debug Build
```powershell
examples\pay-server\scripts\build.bat -debug
```

### What the build script does (step by step)

1. Kills any running `PayServer.exe` processes (avoids file-lock conflicts)
2. `cd` to the repository root (where `conanfile.py` / `CMakePresets.json` live)
3. `conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing`
4. `cmake --preset windows-msvc`
5. `cmake --build --preset windows-msvc`
6. Copies `config.json`, `.env`, and `certs/` from `examples/pay-server/` into the output dirs

### Build outputs

| Artifact | Path (from repository root) |
|----------|------|
| Server executable | `build/windows-msvc/examples/pay-server/Release/PayServer.exe` |
| Test executable | `build/windows-msvc/tests/Release/PayBackendTests.exe` |
| Config | `build/windows-msvc/examples/pay-server/Release/config.json` |
| Certs | `build/windows-msvc/examples/pay-server/Release/certs/` |

## Manual build (what build.bat runs, from the repository root)

```powershell
conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

Presets: `windows-msvc` (Release) and `windows-msvc-debug` (Debug), defined
in the root `CMakePresets.json`.

## Running After Build

```powershell
# From the repository root
cd build\windows-msvc\examples\pay-server\Release
.\PayServer.exe
```

The binary takes **no** command line arguments (`examples/pay-server/main.cc`
declares `main()` with no `argc/argv`); it always reads `./config.json` and
`./.env` from the current directory, which is why the `cd` above is required.
`build.bat` copies both files next to the executable, so running
`PayServer.exe -c <path>` from anywhere else silently loads the wrong config.

## Build Troubleshooting

| Symptom | Fix |
|---------|-----|
| `error MSB1009: Project file does not exist` | Run `conan install` + `cmake --preset` from the repository root |
| `conan: command not found` | `pip install conan` or add Conan to PATH |
| `cmake: command not found` | Install CMake and add to PATH |
| `Error: Conan install failed` | Re-run `conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing` |
| `fatal error C1083` (drogon headers) | Drogon not installed via Conan — check `conanfile.py` |
| `LNK1104: cannot open file 'PayServer.exe'` | `taskkill /F /IM PayServer.exe` then retry |

## Project Structure

```
pay-plugin/                          # repository root (build from here)
├── conanfile.py                     # Conan 2 recipe (drogon-pay package)
├── CMakePresets.json                # windows-msvc / windows-msvc-debug presets
├── CMakeLists.txt                   # top level: options + subdirectories
├── libs/drogon-pay/                 # ★ the reusable plugin library
│   ├── include/drogon_pay/          # public API headers (4 headers)
│   └── src/                         # internal impl (channels/handlers/services/models/utils)
├── examples/pay-server/             # example host (PayServer target)
│   ├── config.json / .env / certs/  # host runtime configuration
│   ├── controllers/ · utils/        # host-only code
│   └── scripts/build.bat            # ← THE build script
├── tests/                           # DROGON_TEST files (PayBackendTests target)
└── sql/                             # database migrations
```

## Context

- **C++ Standard**: C++17 (set in `CMakeLists.txt`)
- **Build System**: CMake presets + Conan 2.x
- **Compiler**: MSVC (Windows), GCC/Clang (Linux) — supports both
- **Framework**: Drogon C++ web framework (1.9.13, pinned)
- **Dependencies**: Drogon, OpenSSL (via Conan)
- **ORM**: drogon_ctl generated models from PostgreSQL schema
