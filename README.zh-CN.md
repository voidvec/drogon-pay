# drogon-pay

**Drogon 框架的可复用支付插件。**

[English](README.md) | **简体中文**

[![CI](https://github.com/voidvec/drogon-pay/actions/workflows/ci.yml/badge.svg)](https://github.com/voidvec/drogon-pay/actions/workflows/ci.yml)
[![Coverage](https://github.com/voidvec/drogon-pay/actions/workflows/coverage.yml/badge.svg)](https://github.com/voidvec/drogon-pay/actions/workflows/coverage.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

`drogon-pay` 把完整的支付能力（创建/扫码/查询/退款/回调验签/对账/幂等）封装为
一个标准 `drogon::Plugin`。任意 Drogon 宿主应用通过 **Conan 依赖 +
`find_package(DrogonPay)` + config.json 插件块** 三步接入，无需拷贝业务代码；
新支付渠道通过实现 `PaymentChannel` SPI 即插即用。

```text
宿主 app ──find_package(DrogonPay)──▶ DrogonPay::DrogonPay (STATIC)
config.json plugins: [PayPlugin] ──▶ 渠道装配 → 服务构造 → 路由程序化注册
                                        │
                          ChannelRegistry（启动期冻结，运行期无锁）
                                        │
                    ┌───────────────────┼──────────────────┐
              WechatChannel       AlipayChannel      宿主自定义渠道
              （内置，SPI 实现）  （内置，SPI 实现）  （registerFactory 注册）
```

## 特性

- **插件即库**：`drogon-pay/1.1.0` Conan 包（`static-library`），CMake 目标
  `DrogonPay::DrogonPay`，公共 API 仅 4 个头文件
- **渠道 SPI**：`drogon_pay::PaymentChannel` 抽象接口 + `ChannelRegistry`
  启动期注册/冻结；未知渠道显式 `CHANNEL_NOT_AVAILABLE`，无隐式兜底
- **内置微信/支付宝渠道**：HttpClient 每 IO loop 复用（`IOThreadStorage` +
  keep-alive）、微信平台证书原子快照热刷新
- **路由全部程序化注册**：无 `ADD_METHOD_TO` 静态注册，静态库链接不丢符号，
  路由前缀 `base_path` 可配置（默认 `/api/pay`）
- **生产化基座**：API Key + scope 鉴权、幂等键（Redis 可选 / 数据库兜底）、
  回调状态机 + 台账、定时对账（独立 worker 线程）、Prometheus 指标
- **消费者验证**：`conan create` 走 `test_package/` 最小宿主端到端验证
  （find_package → 插件加载 → 路由可达 → 干净退出）

## 快速开始（宿主三步接入）

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

再执行 [sql/](sql/) 建表脚本并启动即可。完整五步接入、配置键说明、路由表、
自定义渠道开发指南与 v1.0 破坏性变更映射表见
**[docs/development/plugin_integration.md](docs/development/plugin_integration.md)**。

## 版本兼容

| drogon-pay | Drogon | C++ |
|---|---|---|
| 1.0.x | 1.9.13（钉死；库与宿主必须同版本） | C++17 |

静态库 + C++ ABI 决定了 Drogon 版本是硬约束；Drogon 升级随本库 minor 版本发布。

## 仓库布局

```
├── libs/drogon-pay/        # ★ 可复用插件库（唯一发布物）
│   ├── include/drogon_pay/ #   公共 API：PayPlugin / PaymentChannel / ChannelRegistry / PayErrorCategory
│   └── src/                #   内部实现：handlers / services / channels / models（不安装）
├── examples/pay-server/    # 示例宿主（.env/CORS/healthz 等宿主关注点）
├── examples/pay-admin/     # 示例宿主配套的 Vue 3 管理控制台（演示层）
├── tests/                  # DROGON_TEST 单元/集成测试（ctest）
├── test_package/           # Conan 消费者视角端到端验证
├── sql/                    # PostgreSQL 迁移脚本 000-004
├── cmake/                  # find_package 导出模板
└── docs/                   # 集成指南 / API / 部署 / 运维文档
```

架构守卫（依赖方向单向）：库不依赖 `examples/` 任何代码；公共头不泄漏
`src/` 内部实现；服务层只依赖 SPI，不 include 具体渠道头。

## 从源码构建

前置：CMake 3.15+、Conan 2、MSVC 2022 / GCC / Clang（C++17）。

```bash
conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset windows-msvc         # 其他平台用 linux-release / macos-arm64
cmake --build --preset windows-msvc
ctest --test-dir build/windows-msvc -C Release        # 需要 PostgreSQL + Redis，见 tests/
conan create . --build=missing -s build_type=Release -s compiler.cppstd=17   # 打包 + test_package 验证
```

CMake 开关：`DROGON_PAY_BUILD_EXAMPLES`（默认 ON）、`DROGON_PAY_BUILD_TESTS`
（默认 ON，CI 经 `BUILD_TESTS` 兼容开关控制）。

## 文档

- **[宿主集成指南](docs/development/plugin_integration.md)** — 五步接入 + 自定义渠道 SPI + 旧→新配置映射
- [API 示例](docs/api/pay-api-examples.md) · [API Key 配置](docs/api/api_key_configuration.md)
- [示例宿主说明](examples/pay-server/README.md) · [管理控制台说明](examples/pay-admin/README.md)
- [架构总览](docs/architecture/architecture_overview.md) · [部署指南](docs/deployment/deployment_guide.md)
- [测试指南](docs/testing/testing_guide.md) · [运维手册](docs/operations/operations_manual.md)
- 完整索引：[docs/README.md](docs/README.md)

## 贡献

1. Fork → 特性分支 → 提交 PR（要求三平台 CI 绿）
2. 新渠道贡献请先阅读集成指南的 SPI 契约（线程安全 + HttpClient 复用）
3. 遵循 `.clang-format`；新代码零告警准入

## License

[MIT](LICENSE)
