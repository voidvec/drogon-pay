---
name: openapi-update
description: 支付 API 端点发生变化时同步 OpenAPI 契约（examples/pay-server/openapi.yaml）与端点文档，并通过 stdlib 路由门禁校验一致性
---

# API 契约同步技能

`examples/pay-server/openapi.yaml`（OpenAPI 3.0.3）是本服务 HTTP 表面的唯一
契约来源。它与代码的对应关系由 `scripts/check_openapi_routes.py` 在 CI
FAST 门强制，不再靠人记得去改文档。

## 事实来源三件套

| 层 | 位置 | 角色 |
|----|------|------|
| 代码 | `libs/drogon-pay/src/PayPlugin.cc`（`registerHandler`）+ `examples/pay-server/controllers/*.h`（`ADD_METHOD_TO`） | 路由与鉴权姿态的真实定义 |
| 契约 | `examples/pay-server/openapi.yaml` | schema、参数、响应、状态码语义 |
| 人类文档 | `docs/api/pay-api-examples.md` | curl 示例与调用语境 |

`TECH_SPECS.md` "API 契约" 一节列的是不变量（金额表示、响应信封、状态码
语义），不是端点清单；端点清单只看上表。

## 门禁做什么

```
python scripts/check_openapi_routes.py --lint-only     # 契约自洽
python scripts/check_openapi_routes.py --routes-only   # 代码 <-> 契约双向 diff
python scripts/check_openapi_routes.py --print-routes  # 只看代码侧解析结果
python scripts/check_openapi_routes.py                 # 两半都跑
```

`--lint-only`：openapi 版本声明、`info.version`、operationId 唯一、每个
operation 有 summary 与 responses、`securitySchemes` 已声明、`$ref` 目标
存在、`EXCLUSIONS` 每条带 reason。

`--routes-only`：从代码解析 `METHOD /path` 集合（含 `basePath_ + "/query"`
这类拼接、`qrPath` 变量、三元表达式），与 spec paths 双向比对；同时比对
鉴权姿态 —— `authed(...)` 注册的路由不得在 spec 里声明为公开，
`open(...)`/宿主路由必须显式 `security: []`，所有 `OPTIONS` 必须公开。

两侧路径都能解析到，才算通过。脚本只用标准库（CI runner 不保证有
PyYAML），因此新增 spec 语法时保持它在脚本支持的 YAML 子集内：块映射、
块序列、`|`/`>` 块标量、2 空格层级缩进。

## 工作流程

1. **改代码路由**（新增/删除/改 `base_path`、改 `authed`→`open`）：
   - 先跑 `--print-routes` 确认代码侧解析出的路径与鉴权姿态符合预期
   - 在 `openapi.yaml` 补/删对应 path 与 operation
   - 若某条路由刻意不写进契约，在 `check_openapi_routes.py` 的
     `EXCLUSIONS` 加条目并写 reason —— 无 reason 的门禁失败是故意的
2. **只改契约语义**（加字段、改 enum、调状态码）：不必动代码，但
   `docs/api/pay-api-examples.md` 的示例必须同步，否则文档又是第二份真相
3. 复跑门禁 + `python scripts/check_docs_drift.py`（文档里的反引号路径必须
   存在，`sql/NNN_` 版本号必须与磁盘一致）
4. `CHANGELOG.md` `[Unreleased]` 记录，提交前缀 `docs(api)`（纯契约）或
   `feat(api)`/`fix(api)`（伴随代码变更）

## 硬性约定

- **金额**：`components.schemas.Amount`，`type: string` +
  `pattern: "^\d+(\.\d{1,2})?$"`，单位**元**、十进制字符串。DB 侧同样是
  `VARCHAR(32)`，不是整数分。
- **响应信封**：`{code, message, data?}`；成功 `code: 0`（`/orders` 用
  `200`），渠道降级 `code: 1` 并在 `data` 里带 `*_query_error`。
- **鉴权失败**：401/403/503 返回**纯文本**，不是信封 JSON。
- **状态枚举**：大写。`OrderStatus` =
  `CREATED|PAYING|PAID|REFUNDED|CLOSED|FAILED`；`RefundStatus` =
  `REFUND_INIT|REFUNDING|REFUND_SUCCESS|REFUND_FAIL`。
- **已知缺陷照实写**：微信 payment 状态是 `FAIL`、支付宝是 `FAILED`；
  `reconcile/summary` 的 `date` 参数已被服务层丢弃。契约记录现状，不粉饰
  成理想设计 —— 修复要单独提 PR，顺手改 enum 会让线上对账文档说谎。
- **notify 回调**：`/api/pay/notify/wechat`、`/api/pay/notify/alipay` 的
  响应体是**渠道约定的格式**（微信 JSON `{"code":"SUCCESS"}`、支付宝期望
  `success` 文本而当前回 JSON 且拒绝也返 200），不是自有契约，二者都无需
  `X-API-Key`。
- `/api/qrpay/create` 前缀固定为 `/api/qrpay`，不随 `base_path` 变化；
  失败时以 HTTP 200 在体内报 `500`/`1005`。

## 禁止

- 不要复活 `.claude/agents/api-documenter.md` 里曾出现过的 `/api/v1/*`
  端点映射 —— 那些路径在本仓库从未注册过。
- 不要在文档里写"金额一律整数分"。
- 不要引入 `pip install pyyaml` 到 FAST 门来简化脚本。
