# drogon-pay

**A reusable payment plugin for the [Drogon](https://github.com/drogonframework/drogon) framework.**

**English** | [简体中文](README.zh-CN.md)

[![CI](https://github.com/voidvec/drogon-pay/actions/workflows/ci.yml/badge.svg)](https://github.com/voidvec/drogon-pay/actions/workflows/ci.yml)
[![Coverage](https://github.com/voidvec/drogon-pay/actions/workflows/coverage.yml/badge.svg)](https://github.com/voidvec/drogon-pay/actions/workflows/coverage.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

`drogon-pay` packages a complete payment stack (create / QR code / query / refund /
callback signature verification / reconciliation / idempotency) as a standard
`drogon::Plugin`. Any Drogon host application integrates in three steps —
**Conan dependency + `find_package(DrogonPay)` + a config.json plugin block** —
without copying any business code. New payment channels plug in by implementing
the `PaymentChannel` SPI.

```text
host app ──find_package(DrogonPay)──▶ DrogonPay::DrogonPay (STATIC)
config.json plugins: [PayPlugin] ──▶ channel assembly → service wiring → programmatic routes
                                        │
                          ChannelRegistry (frozen at startup, lock-free at runtime)
                                        │
                    ┌───────────────────┼──────────────────┐
              WechatChannel       AlipayChannel      custom host channels
              (built-in SPI impl) (built-in SPI impl) (via registerFactory)
```

## Features

- **Plugin as a library**: `drogon-pay/1.1.0` Conan package (`static-library`),
  CMake target `DrogonPay::DrogonPay`, public API is just 4 headers
- **Channel SPI**: `drogon_pay::PaymentChannel` abstract interface +
  `ChannelRegistry` with startup-time registration/freeze; unknown channels fail
  explicitly with `CHANNEL_NOT_AVAILABLE` — no implicit fallback
- **Built-in WeChat Pay / Alipay channels**: per-IO-loop HttpClient reuse
  (`IOThreadStorage` + keep-alive), atomic-snapshot hot refresh of WeChat
  platform certificates
- **Fully programmatic routing**: no `ADD_METHOD_TO` static registration, so no
  symbols are lost when linking the static library; route prefix `base_path`
  is configurable (default `/api/pay`)
- **Production-grade foundation**: API key + scope auth, idempotency keys
  (Redis optional / database fallback), callback state machine + ledger,
  scheduled reconciliation (dedicated worker thread), Prometheus metrics
- **Consumer-side verification**: `conan create` runs an end-to-end check from
  the consumer's perspective via `test_package/`
  (find_package → plugin loads → routes reachable → clean shutdown)

## Quick Start (three-step host integration)

```python
# conanfile.py
def requirements(self):
    self.requires("drogon-pay/1.1.0")
```

```cmake
find_package(DrogonPay CONFIG REQUIRED)
target_link_libraries(my_server PRIVATE DrogonPay::DrogonPay)
```

```json
{ "plugins": [{ "name": "PayPlugin", "config": {
    "base_path": "/api/pay",
    "db_client": "default",
    "channels": { "wechat": { "enabled": true }, "alipay": { "enabled": true } }
} }] }
```

Then run the [sql/](sql/) migration scripts and start your server. For the full
five-step integration, configuration keys, route table, custom channel
development guide, and the v1.0 breaking-change mapping, see
**[docs/development/plugin_integration.md](docs/development/plugin_integration.md)**.

## Version Compatibility

| drogon-pay | Drogon | C++ |
|---|---|---|
| 1.0.x | 1.9.13 (pinned; library and host must match) | C++17 |

Static library + C++ ABI make the Drogon version a hard constraint; Drogon
upgrades ship as minor releases of this library.

## Repository Layout

```
├── libs/drogon-pay/        # ★ the reusable plugin library (the only published artifact)
│   ├── include/drogon_pay/ #   public API: PayPlugin / PaymentChannel / ChannelRegistry / PayErrorCategory
│   └── src/                #   internals: handlers / services / channels / models (not installed)
├── examples/pay-server/    # example host (host concerns: .env / CORS / healthz ...)
├── examples/pay-admin/     # Vue 3 admin console for the example host (demo layer)
├── tests/                  # DROGON_TEST unit/integration tests (ctest)
├── test_package/           # Conan consumer-side end-to-end verification
├── sql/                    # PostgreSQL migration scripts 000-004
├── cmake/                  # find_package export templates
└── docs/                   # integration / API / deployment / operations docs
```

Architecture guards (one-way dependency direction): the library depends on
nothing under `examples/`; public headers never leak `src/` internals; the
service layer depends only on the SPI and never includes concrete channel
headers.

## Building from Source

Prerequisites: CMake 3.15+, Conan 2, MSVC 2022 / GCC / Clang (C++17).

```bash
conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset windows-msvc         # linux-release / macos-arm64 on other platforms
cmake --build --preset windows-msvc
ctest --test-dir build/windows-msvc -C Release        # requires PostgreSQL + Redis, see tests/
conan create . --build=missing -s build_type=Release -s compiler.cppstd=17   # package + test_package verification
```

CMake options: `DROGON_PAY_BUILD_EXAMPLES` (default ON), `DROGON_PAY_BUILD_TESTS`
(default ON, controlled in CI via the `BUILD_TESTS` compatibility switch).

## Documentation

- **[Host Integration Guide](docs/development/plugin_integration.md)** — five-step setup + custom channel SPI + old→new config mapping
- [API Examples](docs/api/pay-api-examples.md) · [API Key Configuration](docs/api/api_key_configuration.md)
- [Example Host](examples/pay-server/README.md) · [Admin Console](examples/pay-admin/README.md)
- [Architecture Overview](docs/architecture/architecture_overview.md) · [Deployment Guide](docs/deployment/deployment_guide.md)
- [Testing Guide](docs/testing/testing_guide.md) · [Operations Manual](docs/operations/operations_manual.md)
- Full index: [docs/README.md](docs/README.md)

## Contributing

1. Fork → feature branch → open a PR (all three platform CI checks must pass)
2. Before contributing a new channel, read the SPI contract in the integration
   guide (thread safety + HttpClient reuse)
3. Follow `.clang-format`; new code must be warning-free

## License

[MIT](LICENSE)
