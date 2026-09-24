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

### [MUST] 迁移工程化

迁移链住在仓库根 `sql/`，**只有** `scripts/migrate_db.py` 知道有哪些文件。

| 规范项 | 要求 |
|--------|------|
| 唯一执行器 | 任何工作流/脚本都不得自己写 `psql -f sql/...` 或 `for f in sql/*.sql`。历史口径（`git show 043c5ed^` 可复核）：这样的清单曾有**六**份（`ci-linux.yml`、`ci-windows.yml`、旧 `coverage.yml`、`setup_database.bat`、`deploy.bat`、`deploy.sh`），其中 CI 两份只列了 001+002（四条版本静默漏两条），deploy 两份 glob 的是插件化改造后已搬走的 `sql/` 路径，于是一条都没应用却打印成功 |
| 版本记账 | `schema_migrations(version, filename, sha256, applied_at)`；每条迁移与它的记账行在**同一事务**内提交，失败即整体回滚 |
| 不可变 | 已应用的版本内容一旦被改：`migrate_db.py`（比对 `sha256`）与 `scripts/check_migrations.py`（比对 `scripts/migrations_baseline.json`）双双 exit 1；要改历史只能同一 PR 里手改基线 JSON。基线条目永久豁免内容规则，所以 `--write-missing` 钉新文件前会先跑这些规则，不合格就拒绝写入 |
| 重置助手 | `sql/000_*.sql` 不属于版本链，执行器跳过它；开发库重置走 `setup_database.{sh,bat}`（`--reset-schema --confirm-drop <db>`），且**仅允许 loopback 主机**——远端主机是 staging/生产所在地，`--confirm-drop` 由脚本自动填写，真正拦住误用的是这条主机规则 |
| 建库 = provisioning | 应用角色无 `CREATEDB`，所以执行器只探测、只提示，不建库也不删库；`CREATE DATABASE pay_test OWNER test` 由超级用户在部署前置步骤里做 |
| 钉的是提交字节 | sha256 比对原始文件，因此检出必须是 LF——`.gitattributes` 把 `eol=lf` 钉死（`.bat`/`.cmd` 例外为 `eol=crlf`）。从 CRLF 工作副本 `--write-missing` 出来的基线在本地绿、在 Linux runner 上必红；两个脚本遇到这种差异会明确报"仅行尾不同"而不是"历史被改" |
| 幂等/非破坏 | 新迁移必须 `IF NOT EXISTS` 风格，`ADD CONSTRAINT x` 与 `DROP CONSTRAINT IF EXISTS x` 同文件配对；禁止 `DROP TABLE/COLUMN`、`TRUNCATE`、裸 `DELETE FROM` |
| compose 路径 | `docker-entrypoint-initdb.d` 只在新卷上以超户跑全部 `*.sql` 且不记账，因此那种库第一次必须 `migrate_db.py --baseline` 采纳（缺表会拒绝） |

```bash
python3 scripts/check_migrations.py            # 门禁（CI static-analysis 步骤）
python3 scripts/migrate_db.py --status         # 数据库认为应用了什么
python3 scripts/migrate_db.py --dry-run        # 下次会应用什么
python3 scripts/migrate_db.py                  # 应用缺口
```

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

### [MUST] 开发脚本跨平台对齐

面向开发者的**入口脚本**必须成对：`build` / `test` / `setup_database` /
`deploy` / `check_config` 各有 `.sh` 与 `.bat` 两个孪生体，二者**逐参数对齐**：
同样的选项、同样的默认值、同样的退出码语义。`check_docs_drift.py` 的
`twin-scripts` 规则守着这个集合，少一边就红。

其余脚本**有意**不成对，别按上面的规则去找它们：

| 脚本 | 平台 | 原因 |
|------|------|------|
| `full_test.bat` | 仅 Windows | 它只是依次 `call` 另外四个 `.bat`（setup_database → generate_models → build → test）的编排壳；跨平台编排由 CI 承担 |
| `generate_models.bat` | 仅 Windows | `drogon_ctl create model` 的交互确认壳，尚未移植；模型生成规范见 `/orm-gen` |
| `run_server.bat` | 仅 Windows | 便利启动器（`cd` 到 `build\windows-msvc\...\Release` 再跑）；POSIX 侧一行 `cd && ./PayServer` 就够，见 `/build-and-test` |
| `healthcheck.sh` | 仅 POSIX | 手工 curl 探针，**当前无任何文档/脚本引用它**；容器侧健康检查由 `docker-compose.yml` 的 `curl -f http://localhost:5566/healthz` 承担。要恢复使用就先在 `/docs` 里给出调用场景，否则删除 |
| `e2e_test.sh` + `e2e_test.ps1` | 两端 | 刻意是 bash 与 **PowerShell**，不是 bash 与 cmd：脚本要用到数组与 `[[ ]]`，cmd 表达不了 |

| 规范项 | 要求 |
|--------|------|
| 唯一入口 | 平台差异只写在脚本里，不写进文档/工作流；文档给的是 `build.sh` / `build.bat`，不是展开的 conan+cmake 长命令 |
| 预设映射 | 脚本内部映射到 CMakePresets：`windows-msvc{,-debug}`、`linux-{release,debug}`、`macos-{arm64,debug}`；新增构建类型先加 preset 再加脚本分支 |
| 凭据来源 | 脚本不写口令。`test.sh`/`test.bat` 不设任何 `DB_*` 变量，真实测试凭据由 `build.*` 复制到二进制旁边的 `.env` 提供，进程环境优先 |
| 变更要求 | 改一个孪生体必须在同一提交里改另一个；只加 `.bat` 会让 Linux/macOS 开发者按文档抄命令时踩到未覆盖路径 |

### [MUST] CI 工作流治理

| 规范项 | 要求 |
|--------|------|
| 单一入口 | `ci.yml` 是唯一入口，FAST(`static-analysis`+`clang-tidy`) → MAIN(`_build-test.yml`) → RELEASE(`_sdk-smoke.yml`) 全部用 `needs` 串联；缺 `needs` 边的检查只是建议性信号，它红了也照样能合进去 |
| 检查名契约 | 三个 required context 是 `linux-build-and-test / build-test`、`windows-build-and-test / build-test`、`macos-build / build-test`：`uses:` 调可复用工作流的 job 报名为「调用方 job 名 / 被调用方工作流给它自己 job 的名字」，`matrix.check_name` 只供前一半；后缀之所以就是 `build-test` 这个键名，是因为 `_build-test.yml` 的 job 没写 `name:` 键，给它补一个 `name:` 同样会改掉三条 context。改任何一处或删掉曾上报某名的工作流文件，ruleset 会继续要求一个无人上报的 context——保护不是解除而是反转成永久 pending 卡死，且 `gh pr checks` 不列出缺位的必需检查，全绿列表会掩盖它；用 ruleset context 与 `commits/<sha>/check-runs` 名做差集核验（命令见 AGENTS.md "CI"） |
| 行动固定 | 所有 `uses:` 钉到完整 commit SHA，并注释该 SHA 对应的 tag；浮动 major tag 让未经评审的上游变更决定门禁结论 |
| 最小权限 | 工作流级显式声明 `permissions: contents: read`，只有 `release.yml` 的 `publish` job 拿 `write`；`.github/workflows/secrets-scan.yml` 因 gitleaks 需要回写 commit status 暂未收窄 |

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
| 缺基线 | 视为首次 SEED：写入基线并通过，基线提交本身即审计线索。注意 runner 写的那份随作业工作副本一起丢弃，所以基线要由人按那次全绿运行的报表数字提交进仓库，否则每次运行都在重新 SEED，棘轮永远没有地板 |

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

### [MUST] 版本号一致性

版本号只**声明**、不派生，**声明源只有四处**：`CMakeLists.txt` 的
`project(drogon-pay VERSION x.y.z …)`、`conanfile.py` 的 `version = "x.y.z"`、
`examples/pay-admin/package.json` 的顶层 `"version"`、
`examples/pay-server/openapi.yaml` 的 `info: version:`。`check_version_sync.py`
只读这四处；文档里出现的版本号一律不受它管辖，因此把它们另立两类并各自动作：

| 类别 | 位置 | 处理 |
|------|------|------|
| 声明源 | 上面四处 | 同一提交里一起改；任一处不一致门禁就红 |
| 已发布包引用 | `README.md`、`README.zh-CN.md`、`docs/development/plugin_integration.md` 里的 6 处 `drogon-pay/<已发布版本>` | 指的是**已 tag** 的那一版，不是开发中的下一版；下一次发布 PR 必须同步改这三份文件，`check_version_sync.py` 挡不住它 |
| 禁止复述 | 其余一切：配置/部署/告警文件的注释、文档页眉页脚的"版本/最后更新"戳 | 不得出现版本号或手工日期。历史 6 处 `# Version: 1.0.0` 注释已删；`docs/deployment/*`、`docs/operations/*` 四份文档的 16 行版本/日期戳（每份头尾各 4 行）也已删，git 才是真相，`check_docs_drift.py` 的 `no-version-stamps` 规则挡住回潮 |

| 规范项 | 要求 |
|--------|------|
| 常规 PR | `python3 scripts/check_version_sync.py`（CI `static-analysis` 步骤）断言四处一致。契约那一处是**逐行**读出来的——FAST 门只依赖 stdlib，不能 import YAML 解析器——所以扫描器与解析器可能各读出一个版本的每一种写法都是这个门的漏洞：`scripts/ci/version_sync_scenarios.py` 把"哪些写法读成版本、哪些必须拒"钉成一张判定表（锚点/标签/别名/块标量/块标量只有头/续行/空行后续行/与值同级的杂项或列表项/第二份文档/tab 或非 ASCII 既当空白又当缩进/值里的 form feed/前导零/未闭合引号/单引号翻倍转义/行尾注释/缩进更深的注释行——条数由脚本自己打印，别抄进文档），与守卫同步在 FAST 每次都跑，并额外断言一件否则可以空转的事：仓库自己的契约读出的值，确实在那四处一致的集合里 |
| 打 tag | `release.yml` 的 `version-check` job 以 `--tag "$GITHUB_REF_NAME"` 再跑一次：tag 必须等于四处声明，且 `CHANGELOG.md` 已有对应 `## [x.y.z]` 段，缺段硬失败（先于任何构建，不浪费一个 Conan 编译周期） |
| tag 也要过 CI | `.github/workflows/_tag-gate.yml`（`on: workflow_call`，被 release.yml 的 `ci-gate` 与 deploy.yml 的 `tag-gate` 调用——两个会因 `v*` push 跑起来的工作流共用一份定义，不再各写一份）：tag 不是被合并进来的，分支 ruleset 的必需检查管不到它，所以该门禁先用 `compare/master...<sha>` 确认该 commit 已在 master 历史上（顺手排掉"未合并分支上的 commit 也带着绿色 PR 检查"这条路），再轮询合并流水线在该 commit 上报的五个 context（三条 `* / build-test` + 两条 `* / sdk-smoke`，FAST 不列是因为 MAIN `needs` 它）。该端点会累计一个 commit 上的历次上报，所以同一名字（重跑、对同一 SHA 再 dispatch）取 id 最大的那一次作为结论，其非 `success` 立即失败；还没出现的 context 计入"未完成"继续等，`DEADLINE_MINUTES: 120` 到限才失败（实测一轮链是 32 分钟，上限必须容得下冷缓存，`timeout-minutes: 135` 又必须大于它，否则作业被 runner 掐掉就只剩"超时"而没有"哪个 context 没绿"）。"清单为空"不能作为"流水线没跑过"的判据——release/deploy 自己也会在被 tag 的 commit 上报 check run，清单永不为空；真正的证据是 ci.yml 的入口作业，即 `ENTRY_CHECKS: static-analysis,clang-tidy`——它们的 check run 在 run 创建时就存在（不必等到有 runner 领任务），十五分钟（`EARLY_BAIL_SECONDS: 900`，窗口要容得下排队）还没出现就说明没有任何 ci.yml run 摸到过这条 commit（多 commit push 只在 tip 触发 CI，master 的内部提交就是这种），此时直接失败并说明原因；这两个名字在 `pull_request` 事件里同样会出现，所以早退只是"别白等两小时"的兜底，不能替代上面的包含性检查。名单被截断或清空、`ENTRY_CHECKS` 为空或含非法字符、单个 context 名带上下不文的字符（会被 `awk -v` 反转义而永远匹配不上）、时长旋钮不是数字时同样直接失败，且旋钮校验排在任何算术之前——宁可拦住发布，也不给出一个没检查过任何事的"全绿"。非 tag 触发（deploy.yml 的 `workflow_dispatch`）没有 tag 可证明，原样放行。`scripts/ci/tag_gate_scenarios.py` 把这张判定表钉在 CI 上：FAST 门每次进 master 的 PR 都会自动跑一遍（从工作流文件里现场抽取 `run:` 正文，用打桩的 `gh` 复演 23 个判定——含入口早退的正反两向、第一轮 pending/第二轮绿、三个 API 读取失败分支），`--live` 再用真实只读 API 复演 v1.0.0 那次的三种判法；改门禁的判定就要同时改这张表 |
| 发布顺序 | `[Unreleased]` 归档为带日期的版本段 → 同一提交改四处声明 + 已发布包引用 → 提交 PR → 合并后打 tag |

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

**维护者**: Pay Plugin 开发团队（版本与改动日期以 `git log -- TECH_SPECS.md` 为准，
不手写戳记——`check_docs_drift.py` 的 `no-version-stamps` 规则会拒掉戳记）
