---
name: project-conventions
description: Enforces Pay project coding conventions from TECH_SPECS.md during code generation and review
user-invocable: false
---

# Project Conventions Checklist

Apply these rules to ALL C++ code generated or modified in this project. Loaded automatically by Claude as background knowledge.

## Architecture Rules

| Rule | Requirement |
|------|-------------|
| Layer separation | Controllers (HTTP) -> Plugin/Service (business) -> Storage (data) -> Model (ORM) |
| Drogon-first | Use Drogon built-ins over third-party libraries |
| Plugin pattern | Core logic in `libs/drogon-pay`, server wiring in `examples/pay-server` (`PayServer` target) |
| ORM immutable | NEVER edit files in `models/` -- use `drogon_ctl` to regenerate |

## Async Programming

| Pattern | Status |
|---------|--------|
| Async callbacks (`Mapper::findOne`, `execSqlAsync`) | REQUIRED -- always prefer |
| Synchronous (`Mapper::findBy` with future) | RESTRICTED -- only when necessary |
| Coroutines (`CoroMapper`) | FORBIDDEN -- never use |

### Lambda Capture Rules
- `[sharedCb]` -- REQUIRED for callback lifetime
- `[&var]` -- FORBIDDEN unless PR explains lifetime guarantee
- `[this]` -- FORBIDDEN (no PR-exemption); use `shared_from_this()` instead — the
  class must `enable_shared_from_this<T>` and the lambda captures
  `auto sharedCb = shared_from_this()`, holding ownership so `this` stays alive for
  the whole async continuation.

### Callback Pattern
```cpp
auto sharedCb = std::make_shared<std::function<void(const ResultType &)>>(
    std::move(callback));
// Use *sharedCb to invoke
```

## Data Access

| Operation | Allowed | Method |
|-----------|---------|--------|
| SELECT | ORM only | `Mapper::findBy`, `Mapper::findOne` |
| INSERT | ORM only | `Mapper::insert` |
| UPDATE | ORM only | `Mapper::update` |
| JOIN | Forbidden | Split into multiple queries or `Criteria::In` |
| Raw SQL | Exception only | DDL, `UPDATE...RETURNING`, batch ops |

## Error Handling

- Always catch `const DrogonDbException &e` for DB operations
- All async callbacks MUST handle failure path: `(*sharedCb)(errorResult)`
- Log levels (six-tier — full table in `TECH_SPECS.md` "日志分级规范"):
  - `LOG_TRACE` — function I/O args, loop iterations, line-by-line (deep debug only)
  - `LOG_DEBUG` — variable values, branch decisions, internal state, **per-request flow steps**
  - `LOG_INFO` — **lifecycle/milestone events only** (startup/shutdown, channel registration, task completion). Do NOT use for per-request steps or variable dumps.
  - `LOG_WARN` — recoverable/degraded: fire-and-forget helpers (ledger/idempotency snapshot) failing, config using defaults, resource near threshold
  - `LOG_ERROR` — a single operation failed but the service stays up
  - `LOG_FATAL` — process cannot continue (startup-exit); must be rare
- NEVER log passwords, tokens, or secrets

## Code Style

- C++17 standard, Google style, 100 char line limit
- clang-format runs automatically on edit (hook configured)
- ASCII only in code: use `[+]`, `[-]`, `[!]` instead of emoji
- No comments explaining WHAT -- name variables/functions to be self-documenting
- Comments only for WHY: hidden constraints, non-obvious invariants, workarounds

## Security

- Input validation on ALL user input
- ORM Criteria for queries (no string concatenation)
- API Key 通过 `X-Api-Key` Header 传递，环境变量 `PAY_API_KEY` 管理
- 金额使用整数（分），避免浮点精度
- 幂等性通过 `Idempotency-Key` Header + `pay_idempotency` 表保障
- 回调签名验证在业务逻辑之前执行

## Testing

- 框架：Drogon 自带 `DROGON_TEST`（`#include <drogon/drogon_test.h>`）；
  本仓库没有 gtest，`TEST_F`/`EXPECT_EQ` 宏不存在
- 断言：`CHECK`（非致命）/ `REQUIRE`（致命）；异步回调用
  `std::promise`/`future` 桥接，并以 `wait_for` + `REQUIRE(... == ready)` 兜底
- 覆盖率：由 CI 棘轮基线守护，不写口头指标
- 测试命名：`DROGON_TEST({Module}_{Scenario})`，如 `PayUtils_ParseAmountToFen`
- 测试依赖真实 PostgreSQL/Redis（CI service 容器），测试进程监听独立的
  `PAY_TEST_PORT`（默认 5567）
