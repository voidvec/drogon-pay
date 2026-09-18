# Drogon Pay Plugin 技术规范

> 本文档定义 Drogon Pay Plugin 项目的技术规范，包括架构设计、编码标准、安全要求等。

---

## 一、架构规范

### [MUST] Drogon 框架优先原则
- 优先使用 Drogon 内置功能，避免引入三方库
- 引入新库必须在 PR 中说明必要性

### [MUST] 分层架构

| 层级 | 目录 | 职责 | 关键要求 |
|------|------|------|----------|
| handlers 层 | `src/handlers/` | HTTP 请求/响应 | 薄层设计，验证格式，调用 Service |
| services 层 | `src/services/` | 核心业务逻辑 | PaymentService, RefundService, CallbackService 等；仅依赖 channels 的 SPI（`PaymentChannel`），禁止 include 具体渠道头文件 |
| channels 层 | `src/channels/` | 第三方支付渠道集成 | WechatChannel, AlipayChannel 实现 `PaymentChannel` SPI |
| models 层 | `src/models/` | ORM 映射 | 禁止修改 ORM 类，用 `drogon_ctl` 重新生成 |

依赖方向：`handlers → services → channels → models`，由 `scripts/check_architecture.py` 在 CI 强制。

### [MUST] 服务架构

项目采用服务导向架构（SOA），核心服务包括：

| 服务 | 职责 | 关键方法 |
|------|------|----------|
| PaymentService | 支付创建和查询 | `createPayment`, `getPaymentById`, `getPaymentsByApiKey` |
| RefundService | 退款处理和查询 | `createRefund`, `getRefundById`, `getRefundsByPaymentId` |
| CallbackService | 支付回调处理 | `handleAlipayCallback`, `handleWechatCallback` |
| IdempotencyService | 幂等性管理 | `checkIdempotency`, `createIdempotencyKey` |
| ReconciliationService | 对账和报表 | `generateDailyReport`, `reconcilePayments` |

### [MUST] 异步编程规范

| 接口类型 | 优先级 | 说明 |
|----------|--------|------|
| 异步回调 | [+] 最高 | `Mapper::findOne`, `execSqlAsync` |
| 同步接口 | [!] 限制 | `Mapper::findBy` with future（非必要禁止） |
| 协程接口 | [-] 禁止 | `CoroMapper`（严格禁止使用） |

**Lambda 捕获规范**:
- [+] 捕获 `sharedCb`: `[sharedCb]`
- [-] 捕获裸指针: `[this]`, `[&var]`
- 如需使用裸指针，必须在 PR 中说明生命周期保障方案 (`shared_from_this`, `weak_ptr`)

**Service API 调用规范**:
- [+] 使用新 Service API: `service->method(request, apiKey, callback)`
- [-] 禁止使用旧 Plugin API: `plugin.method(req, callback)`
- [+] 使用 `std::shared_ptr` 管理 callback 生命周期

---

## 二、数据访问规范

### [MUST] ORM 使用规范

| 操作 | 禁止 | 推荐 |
| :--- | :--- | :--- |
| SELECT | raw SQL | `Mapper::findBy` |
| INSERT | raw INSERT | `Mapper::insert` |
| UPDATE | raw UPDATE | `Mapper::update` |
| JOIN | JOIN 查询 | 拆分查询或 `Criteria::In` |

**允许使用 raw SQL 的特殊情况**:
- [+] PostgreSQL `UPDATE ... RETURNING` (原子操作)
- [+] DDL 操作 (表结构变更，需用 SchemaSetup.cc)
- [+] 批量操作优化 (需说明必要性)
- [+] 测试代码清理

### 关键规范

| 规范项 | 要求 |
|--------|------|
| Callback 生命周期 | 使用 `std::make_shared<CallbackType>(std::move(cb))` |
| 替代 JOIN | 拆分为多个 ORM 查询或使用 `Criteria::In` |
| 错误处理 | 所有异步回调都有错误处理分支 |
| Lambda 捕获 | 捕获 `[sharedCb]` 而非裸指针 |
| 幂等性检查 | 所有写操作必须先检查幂等性键 |

### [MUST] 数据库连接管理
- 读写分离: `dbClientMaster_` (写), `dbClientReader_` (读)
- 连接池配置在 `config.json` 中
- 异步操作使用共享的 DbClientPtr

### [MUST] 数据模型

| 表名 | 用途 | 关键字段 |
|------|------|----------|
| pay_payment | 支付记录 | payment_id, api_key, amount, status |
| pay_refund | 退款记录 | refund_id, payment_id, amount, status |
| pay_callback | 回调记录 | callback_id, payment_id, provider, payload |
| pay_idempotency | 幂等性键 | idempotency_key, operation_result |
| pay_ledger | 账本记录 | ledger_id, payment_id, refund_id, amount |

---

## 三、代码质量规范

### [MUST] 代码风格

| 规范项 | 要求 |
|--------|------|
| 语言标准 | C++17 |
| 风格指南 | Google C++ Style Guide (Drogon 默认) |
| 行长度限制 | 100 字符 |
| 格式化工具 | clang-format 自动格式化 |
| 字符规范 | 禁止 emoji，使用 ASCII 符号如 `[+]`, `[-]`, `[!]`（Windows 兼容性） |

### [MUST] 错误处理

| 错误类型 | 处理要求 |
|----------|----------|
| Drogon 异常 | 必须捕获: `catch (const DrogonDbException &e)` |
| 异步回调失败 | 必须在失败时调用 `(*sharedCb)(errorResult)` |
| 业务逻辑错误 | 使用 `PayErrorCategory` 定义错误码 |
| 日志级别 | 遵循下方 [日志分级规范](#must-日志分级规范) 六等级 |

### [MUST] 性能优化

| 优化项 | 要求 |
|--------|------|
| 接口选择 | 优先使用异步接口，避免阻塞 |
| 缓存策略 | 合理使用 Redis 缓存幂等性键 |
| 数据库优化 | 使用索引，避免 N+1 查询 |
| 连接池配置 | 根据并发需求调整 |
| 幂等性保护 | 所有写操作必须检查幂等性 |

---

## 四、安全规范

### [MUST] 输入验证

| 验证项 | 要求 |
|--------|------|
| 用户输入 | 所有用户输入必须验证 |
| SQL 查询 | 使用 ORM Criteria，禁止字符串拼接 |
| XSS 防护 | 使用 Drogon 内置 CSP 和模板转义 |
| API 认证 | 所有端点必须验证 API Key |

### [MUST] 支付安全

| 规范项 | 要求 |
|--------|------|
| 幂等性保护 | 所有支付和退款操作必须实现幂等性 |
| 回调验证 | 支付回调必须验证签名和来源 |
| 金额验证 | 所有金额字段必须验证格式和范围 |
| 状态机 | 支付和退款状态转换必须遵循状态机规则 |

### [MUST] 订单状态机

状态值以代码为准（`libs/drogon-pay/src/utils/PayUtils.cc` 的渠道状态映射与
`services/*.cc` 的写入点），建表默认值见 `sql/001_init_pay_tables.sql`。
两种支付创建路径使用不同的初始状态，状态转换规则如下：

#### /api/pay/create 路径

```
CREATED ──(channel API call)──> PAYING ──(callback SUCCESS)──> PAID
                                     │
                                     ├──(callback FAIL)───────> FAILED
                                     └──(channel CLOSED/REVOKED)──> CLOSED
```

| 状态 | 含义 | 转换触发 |
|------|------|----------|
| `CREATED` | 订单已创建，支付记录已写入，等待渠道调用 | `PayOrder` INSERT 时设置 |
| `PAYING` | 渠道调用成功，等待用户支付 | 渠道 API return success 时更新 |
| `PAID` | 支付成功 | 回调 `TRANSACTION.SUCCESS` / `TRADE_SUCCESS` |
| `CLOSED` | 渠道侧关闭或撤销 | 微信 `CLOSED`/`REVOKED`/`REFUND` |
| `FAILED` | 支付失败 | 渠道 API return error 或回调失败 |

#### /api/qrpay/create 路径

```
PAYING ──(callback SUCCESS)──> PAID
   │
   └──(callback FAIL)─────────> FAILED
```

| 状态 | 含义 | 转换触发 |
|------|------|----------|
| `PAYING` | 二维码已生成，等待用户扫码支付 | `PayOrder` INSERT 时设置（注意：与 /api/pay/create 不同，无 `CREATED` 状态） |
| `PAID` | 支付成功 | 回调通知 |
| `FAILED` | 支付失败/超时 | 回调失败或订单过期 |

> **设计说明**: `/api/qrpay/create` 在订单创建前已完成渠道调用（生成 QR 码），因此订单创建时即进入 `PAYING` 状态。`/api/pay/create` 先创建订单再调用渠道，因此使用 `CREATED` 作为中间状态。两种路径的状态差异在 `queryOrder`、`queryOrderList` 和 `reconcileSummary` 等查询/对账接口中均已正确处理。

> 订单终态 `REFUNDED` 不属于支付创建路径：只有在退款达到 `REFUND_SUCCESS` 后，
> 退款流程才把父订单置为 `REFUNDED`。

#### 退款状态机

```
REFUND_INIT ──(channel call)──> REFUNDING ──(callback SUCCESS)──> REFUND_SUCCESS
      │                             │
      └──(channel/business error)───┴──────────────────────────> REFUND_FAIL
```

| 状态 | 含义 |
|------|------|
| `REFUND_INIT` | 退款记录已写入，待渠道调用（建表默认值） |
| `REFUNDING` | 渠道受理，等待退款结果 |
| `REFUND_SUCCESS` | 退款成功，父订单随之变为 `REFUNDED` |
| `REFUND_FAIL` | 退款失败，可重新发起 |

#### Payment 记录状态

| 状态 | 含义 |
|------|------|
| `INIT` | 支付记录已写入事务，待渠道调用（建表默认值） |
| `PROCESSING` | 渠道调用成功，等待支付结果 |
| `SUCCESS` | 支付成功（回调确认） |
| `FAIL` | 渠道调用失败 |

> **已知缺陷**: 微信映射产出 `FAIL`（`PayUtils.cc`），而支付宝
> `TRADE_CLOSED` 分支产出 `FAILED`（`PaymentService.cc`）。两者语义相同，
> 消费端暂时只能按"终态失败"处理；修复渠道映射一致性前不要依赖具体拼写。

### [MUST] API 契约

| 要求 | 说明 |
|------|------|
| 契约文件 | `examples/pay-server/openapi.yaml` 是 HTTP 表面的唯一契约来源 |
| 路由一致性 | `scripts/check_openapi_routes.py` 双向比对代码注册的路由与契约，任一侧缺失即 CI 失败；例外必须写进 `EXCLUSIONS` 并给出 reason |
| 金额表示 | 一律为**元为单位的十进制字符串**（`^\d+(\.\d{1,2})?$`），禁止分单位整数与 JSON number；落库列为 `VARCHAR(32)` |
| 响应信封 | `{code, message, data?}`；成功 `code: 0`（订单列表为 `200`），查询类降级为 `code: 1` 并附 `*_query_error` |
| 状态码语义 | 传输层错误用 HTTP 状态表达（400/401/403/404/409/500/502/503），业务细节用 `code` 表达；映射表见 `PayHandlers.cc` `mapErrorToHttpStatus` |
| 回调响应 | `/notify/wechat`、`/notify/alipay` 的响应体是**渠道约定体**（微信 `{"code":"SUCCESS"}`；支付宝预期纯文本 `success`，当前实现返回 JSON，见 `docs/api/pay-api-examples.md`），不受本服务自有契约约束 |
| 幂等 | 写操作接受 `X-Idempotency-Key`（同义 `Idempotency-Key`）；快照落盘先于响应，详见"幂等"相关规范 |
| 变更顺序 | 先改契约，再改代码与文档；`docs/api/pay-api-examples.md` 只做示例，不重复定义字段类型 |

### [MUST] 敏感数据保护

| 数据类型 | 保护要求 |
|----------|----------|
| API Key | 使用环境变量 `PAY_API_KEY` |
| 支付宝密钥 | 使用环境变量 `ALIPAY_PRIVATE_KEY` |
| 微信密钥 | 使用环境变量 `WECHAT_PAY_KEY` |
| 日志输出 | 禁止日志中输出敏感信息 (密钥, token) |

### [MUST] API 认证

| 认证方式 | 说明 |
|----------|------|
| API Key | 请求头 `X-API-Key: {key}` |
| Scope 验证 | 验证 API Key 有权限访问指定资源 |
| 速率限制 | 基于 API Key 的速率限制（可选） |

---

## 五、测试规范

### [MUST] 测试覆盖

| 测试类型 | 要求 | 工具 |
|----------|------|------|
| 单元测试 | 覆盖率由 CI 棘轮基线守护（`scripts/measure_coverage.py` + `scripts/coverage_baseline.json`），禁止口头指标 | Drogon `DROGON_TEST` |
| 集成测试 | API 接口级验证，依赖真实 Postgres/Redis | Drogon `DROGON_TEST` + CI service 容器 |
| 端到端测试 | 完整支付流程验证 | `examples/pay-server/scripts/e2e_test.sh` / `e2e_test.ps1` |

### [MUST] 测试数据管理

| 规范项 | 要求 |
|--------|------|
| 测试数据库 | 使用内存数据库或测试专用数据库 |
| 测试清理 | 每个测试后清理数据 |
| 测试隔离 | 测试之间相互独立 |
| Mock 使用 | 第三方支付服务使用 Mock |

### [MUST] 行覆盖率计量（gcov 棘轮）

覆盖率只在 Linux/gcc 下计量（MSVC 无 gcov），Windows 构建传 `DROGON_PAY_COVERAGE=ON` 时插桩函数为空操作。

```bash
conan install . --output-folder=build/linux-coverage -s build_type=Debug -s compiler.cppstd=17 --build=missing
cmake --preset linux-coverage && cmake --build --preset linux-coverage -j"$(nproc)"
ctest --test-dir build/linux-coverage --output-on-failure     # 跑测试才会落 .gcda
for d in $(find build/linux-coverage -name '*.gcda' -printf '%h\n' | sort -u); do
  (cd "$d" && gcov -i -p -j"$(nproc)" ./*.gcda >/dev/null)
done
python3 scripts/measure_coverage.py --dir build/linux-coverage --report    # 分桶报表
python3 scripts/measure_coverage.py --dir build/linux-coverage --ratchet   # 棘轮门禁
```

| 约束 | 说明 |
|------|------|
| 统计范围 | `handlers` / `services` / `channels` / `utils` / `core` + `host-*`；生成的 ORM models 与 tests 目录**不计入** |
| 基线来源 | `scripts/coverage_baseline.json` 必须由 CI（`.github/workflows/coverage.yml`）一次全绿运行生成；本地半途中断的运行会把虚假的低值钉成地板 |
| 容忍度 | 单桶下降 >0.5pp 判失败；<150 行的小桶只报表不门禁 |
| 塌缩检测 | 某桶可计量行数缩水 >50% 直接判失败（视为覆盖率数据丢失，而非改进） |
| 缺基线 | 视为首次 SEED：写入基线并通过，基线提交本身即审计线索 |

---

## 六、部署规范

### [MUST] 环境配置

| 环境变量 | 用途 |
|----------|------|
| `PAY_DB_PASSWORD` | 数据库密码 |
| `PAY_REDIS_PASSWORD` | Redis 密码 |
| `PAY_API_KEY` | API 认证密钥 |
| `ALIPAY_PRIVATE_KEY` | 支付宝私钥 |
| `WECHAT_PAY_KEY` | 微信支付密钥 |

### [MUST] 日志分级规范

项目统一使用 Drogon 的六个日志等级，按以下语义使用：

| 等级 | 含义 | 典型场景 |
|------|------|----------|
| `LOG_TRACE` | 最细粒度追踪 | 函数入参/出参、循环迭代、逐行执行轨迹，仅深度调试定位时开启 |
| `LOG_DEBUG` | 调试信息 | 变量值、分支走向、内部状态变化、per-request 流程步骤，开发阶段排查用 |
| `LOG_INFO` | 常规信息 | 服务启动/停止、关键流程节点、通道注册、任务完成等**生命周期/里程碑**事件 |
| `LOG_WARN` | 警告 | 可恢复的异常、降级处理（如 fire-and-forget 账本/快照写失败）、配置用默认值、资源接近阈值，系统仍能正常运行 |
| `LOG_ERROR` | 错误 | 功能失败、请求异常、数据库连接失败等，影响**单次操作**但服务整体可用 |
| `LOG_FATAL` | 致命错误 | 导致服务崩溃、无法继续运行的严重故障（如启动期配置/环境变量校验失败进程退出），需立即告警并人工介入 |

约定：
- 生产环境默认开启 `LOG_INFO` 及以上；`LOG_TRACE`/`LOG_DEBUG` 按需动态开启。
- 等级越高输出越少，`LOG_FATAL` 应极少出现（仅进程无法继续时）。
- **`LOG_INFO` 仅用于生命周期/里程碑事件**；per-request 流程步骤、分支决定、变量值一律用 `LOG_DEBUG`。
- **fire-and-forget 的辅助写入**（账本、幂等快照等返回 void、不阻断主流程的路径）失败时用 `LOG_WARN`，而非 `LOG_ERROR`。
- 禁止在日志中输出敏感信息（密钥、token）。

### [MUST] 监控和日志

| 监控项 | 要求 |
|--------|------|
| 应用指标 | Prometheus 指标暴露在 `/metrics` |
| 日志级别 | 生产环境使用 `LOG_INFO` 及以上；`trace`/`debug` 按需动态开启，`fatal` 应极少出现 |
| 错误追踪 | 所有错误必须记录堆栈信息 |
| 性能监控 | 记录 P50, P95, P99 延迟 |

---

**文档版本**: v1.1 | **最后更新**: 2026-09-18 | **维护者**: Pay Plugin 开发团队
