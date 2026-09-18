# Pay Plugin Example - Claude Code 项目规范

> 本文档为 Claude Code 提供 Pay Plugin 项目的开发指导，技术规范请参考 [TECH_SPECS.md](TECH_SPECS.md)。

---

## 项目概述

**Drogon Payment Processing Plugin & Vue Admin Dashboard** - 企业级支付处理系统，支持支付宝沙箱、微信支付等多种支付平台，采用服务导向架构（SOA）。

**技术栈**: Drogon C++17 | PostgreSQL 13+ | Redis 6.0+ | Vue 3 + Element Plus | CMake 3.21+ | Conan 2

**项目结构**: `libs/drogon-pay/`（发布库：`src/handlers/` → `src/services/` → `src/channels/` → `src/models/`，ORM禁止修改）| `examples/pay-server/`（示例宿主）| `examples/pay-admin/`（Vue 管理台）

**关键现状**: 149 个 `DROGON_TEST` 用例 | 三平台 CI（linux/windows/macos）+ 静态门禁 | Conan 发布级包（`test_package` 消费方验证）| tag 驱动 Release + Docker 部署

---

## 技术规范

遵循 [Drogon Pay Plugin 技术规范](TECH_SPECS.md)：禁止修改 ORM 类 | 禁止 raw SQL | 优先异步回调 | Lambda 捕获 `[sharedCb]` | 使用 Service API 而非 Plugin API

---

## 构建规则

| 命令 | 说明 |
|------|------|
| `examples\pay-server\scripts\build.bat` | Release 模式（默认，脚本自动切到仓库根） |
| `examples\pay-server\scripts\build.bat -debug` | Debug 模式 |

**注意**: 使用 build.bat 确保 Conan 依赖正确配置，避免直接使用 CMake 或 Visual Studio 构建。

---

## 配置管理

| 配置项 | 文件/变量 |
|--------|-----------|
| 主配置 | `examples/pay-server/config.json` |
| 敏感信息 | `PAY_DB_PASSWORD`, `PAY_REDIS_PASSWORD`, `PAY_API_KEY`, `ALIPAY_PRIVATE_KEY`, `WECHAT_PAY_KEY` |

---

## 测试规范

| 测试类型 | 命令 |
|----------|------|
| 全量测试（ctest） | `ctest --test-dir build\windows-msvc -C Release --output-on-failure` |
| 直接运行测试可执行 | `build\windows-msvc\tests\Release\PayBackendTests.exe` |
| 完整测试脚本 | `examples\pay-server\scripts\test.bat` |

测试框架为 Drogon 自带 `DROGON_TEST`（非 gtest）；测试目标: `PayBackendTests.exe` | 运行位置: 仓库根目录

---

## 服务架构

| 服务 | 文件 | 职责 |
|------|------|------|
| PaymentService | `libs/drogon-pay/src/services/PaymentService.{h,cc}` | 支付创建和查询 |
| RefundService | `libs/drogon-pay/src/services/RefundService.{h,cc}` | 退款处理和查询 |
| CallbackService | `libs/drogon-pay/src/services/CallbackService.{h,cc}` | 支付回调处理 |
| IdempotencyService | `libs/drogon-pay/src/services/IdempotencyService.{h,cc}` | 幂等性管理 |
| ReconciliationService | `libs/drogon-pay/src/services/ReconciliationService.{h,cc}` | 对账和报表 |

**Service API**: `service->method(request, apiKey, callback)` | **禁止**: 旧 Plugin API

---

## 项目架构要点

### 支付流程

| 流程 | 端点 | 步骤 |
|------|------|------|
| 创建支付 | `POST {base}/create` | 验证 API Key → 检查幂等性 → 创建记录 → 调用第三方 |
| 支付回调 | `POST {base}/notify/{wechat\|alipay}` | 验证签名 → 更新状态 → 触发业务逻辑 |
| 创建退款 | `POST {base}/refund` | 验证 API Key → 检查支付状态 → 创建退款 → 调用第三方 |
| 查询订单 | `GET {base}/query` | 验证 API Key → 查询记录 → 返回详情 |

`{base}` 为插件配置 `base_path`（默认 `/api/pay`），完整路由表见 [docs/development/plugin_integration.md](docs/development/plugin_integration.md)。

### 数据模型

| 表名 | 模型文件 | 用途 |
|------|----------|------|
| pay_payment | `libs/drogon-pay/src/models/PayPayment.{h,cc}` | 支付记录 |
| pay_refund | `libs/drogon-pay/src/models/PayRefund.{h,cc}` | 退款记录 |
| pay_callback | `libs/drogon-pay/src/models/PayCallback.{h,cc}` | 回调记录 |
| pay_idempotency | `libs/drogon-pay/src/models/PayIdempotency.{h,cc}` | 幂等性键 |
| pay_ledger | `libs/drogon-pay/src/models/PayLedger.{h,cc}` | 账本记录 |

表结构只由 `scripts/migrate_db.py` 应用（发现 `sql/` 链、与 `schema_migrations`
同事务记账、已应用版本被改即拒绝），CI/部署/本地共用这一个执行器；
`scripts/check_migrations.py` 负责命名、链连续性、幂等与非破坏。规范见
[TECH_SPECS.md](TECH_SPECS.md) 「迁移工程化」，新增迁移用 `/create-migration`。

### 支付渠道（PaymentChannel SPI 实现）

| 渠道 | 文件 |
|--------|------|
| 支付宝沙箱 | `libs/drogon-pay/src/channels/AlipayChannel.{h,cc}` |
| 微信支付 | `libs/drogon-pay/src/channels/WechatChannel.{h,cc}` |

### 前端

位置: `examples/pay-admin/` | 启动: `cd examples/pay-admin && npm run dev` | 技术栈: Vue 3 + Element Plus + Pinia

---

## 开发流程

| 规范项 | 要求 |
|--------|------|
| Git 操作 | 允许 commit，禁止 push（需审核） |
| 调试代码 | 解决后必须移除，使用 `LOG_DEBUG` |
| 完成标准 | 测试通过 + 静态分析通过 + CI 成功 + 文档更新 |

| 分支类型 | 命名规范 |
|----------|----------|
| feature | `feature/service-api-migration` |
| bugfix | `bugfix/fix-idempotency-check` |
| hotfix | `hotfix/critical-payment-fix` |
| refactor | `refactor/optimize-service-layer` |

---

## 部署监控

| 平台 | 构建 | 测试 | 启动 |
|------|------|------|------|
| Windows | `examples\pay-server\scripts\build.bat` | `examples\pay-server\scripts\test.bat` | `.\build\windows-msvc\examples\pay-server\Release\PayServer.exe -c examples\pay-server\config.json` |
| Linux/macOS | `cmake --preset linux-release && cmake --build --preset linux-release` | `ctest --test-dir build/linux-release` | `./build/linux-release/examples/pay-server/PayServer -c examples/pay-server/config.json` |

| 监控端点 | 说明 |
|----------|------|
| `GET /healthz` / `GET /readyz` | 存活/就绪检查（宿主） |
| `GET /health` | 健康检查（宿主） |
| `GET /metrics` | Prometheus 指标（宿主） |
| `GET /api/pay/metrics/auth` | 支付统计（JSON） |
| `GET /api/pay/metrics/auth.prom` | 支付统计（Prometheus 文本） |

---

## 相关文档

| 文档 | 链接 | 说明 |
|------|------|------|
| 技术规范 | [TECH_SPECS.md](TECH_SPECS.md) | 架构、数据访问、安全规范 |
| 项目概述 | [README.md](README.md) | 项目概述和快速开始 |
| 部署文档 | [docs/deployment/](docs/deployment/) | 部署和运维指南 |
| API 契约 | [examples/pay-server/openapi.yaml](examples/pay-server/openapi.yaml) | HTTP 表面唯一契约来源，CI 校验与代码路由一致 |
| API 文档 | [docs/api/pay-api-examples.md](docs/api/pay-api-examples.md) | API 接口文档 |
| 集成指南 | [docs/development/plugin_integration.md](docs/development/plugin_integration.md) | 宿主接入与自定义渠道开发 |

---

**文档版本**: v2.1 | **最后更新**: 2026-09-18 | **维护者**: Pay Plugin 开发团队
