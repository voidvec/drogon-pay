---
name: openapi-update
description: 当支付 API 端点发生变化时同步 API 契约文档（OpenAPI spec 由治理阶段引入，当前以路由表+文档为准）
---

# API 契约同步技能

当支付路由发生变化时，保持 API 契约文档与代码一致。

## 当前状态（重要）

本仓库**尚无** `openapi.yaml`。API 契约的事实来源是：

1. 代码路由表：`libs/drogon-pay/src/PayPlugin.cc`（`registerHandler` 注册）
   与宿主 `examples/pay-server/controllers/*.h`（`ADD_METHOD_TO`）
2. 端点文档：`docs/api/pay-api-examples.md`

OpenAPI 规范文件由 API 治理阶段落地（路径将是
`examples/pay-server/openapi.yaml`，并配 CI 校验门禁）。在它存在之前，
**不要**让任何流程读写一个不存在的 spec 文件。

## 真实端点清单

插件路由（`base_path` 默认 `/api/pay`，鉴权 `X-API-Key` Header）：

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/pay/create` | 创建支付 |
| POST | `/api/qrpay/create` | 二维码支付创建（注意：固定前缀，不随 base_path） |
| GET | `/api/pay/query` | 查询订单 |
| POST | `/api/pay/refund` | 创建退款 |
| GET | `/api/pay/refund/query` | 查询退款 |
| GET | `/api/pay/orders` | 订单列表 |
| GET | `/api/pay/reconcile/summary` | 对账摘要 |
| GET | `/api/pay/metrics/auth` | 支付统计（JSON） |
| GET | `/api/pay/metrics/auth.prom` | 支付统计（Prometheus 文本） |
| POST | `/api/pay/notify/wechat` | 微信支付回调（渠道约定响应） |
| POST | `/api/pay/notify/alipay` | 支付宝回调（渠道约定响应） |

宿主路由：`/healthz`、`/readyz`、`/health`（`HealthCheckController`）、
`/metrics`（`MetricsController`）。

## 工作流程

1. 对比代码与文档：从 `PayPlugin.cc` 与宿主 controllers 提取路由，与
   `docs/api/pay-api-examples.md` 的端点清单比对，双向补齐缺失
2. 状态机语义：`/api/pay/create`（CREATED→PAYING）与 `/api/qrpay/create`
   （直接 PAYING）的差异见 `TECH_SPECS.md` 订单状态机一节，文档变更时同步
3. 若 `docs/api/pay-api-examples.md` 缺少端点（历史上缺
   `/api/pay/orders` 与 `/api/pay/reconcile/summary`），补充请求/响应示例

## 约定

- 金额字段一律整数、单位"分"
- notify 回调的成功响应是**渠道要求的约定体**（微信
  `{"code":"SUCCESS"}` / 支付宝 `success` 文本），不是本服务自有契约，
  文档中必须注明
- 变更提交：`docs(api): ...`，并在 `CHANGELOG.md` `[Unreleased]` 记录
