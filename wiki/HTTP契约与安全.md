# HTTP 契约与安全

<cite>
**Referenced Files**
- [openapi.yaml](file://examples/pay-server/openapi.yaml)
- [check_openapi_routes.py](file://scripts/check_openapi_routes.py)
- [AuthCheck.cc](file://libs/drogon-pay/src/handlers/AuthCheck.cc)
- [PayHandlers.cc](file://libs/drogon-pay/src/handlers/PayHandlers.cc)
- [PayAuthMetrics.cc](file://libs/drogon-pay/src/handlers/PayAuthMetrics.cc)
</cite>

## 契约治理

`examples/pay-server/openapi.yaml` 是 HTTP 表面的**唯一契约来源**；`scripts/check_openapi_routes.py` 双向比对代码与契约，任一侧缺失即 FAST 门禁失败。路由全部程序化注册（PayPlugin.cc:357-417），`base` 默认 `/api/pay`。

## 路由表

| 方法 | 路径 | 鉴权 |
|------|------|------|
| POST | base/create | API Key |
| POST | /api/qrpay/create（base 非默认时 base/qrpay/create） | API Key |
| GET | base/query | API Key + scope `order_query` |
| POST | base/refund | API Key + scope `refund` |
| GET | base/refund/query | API Key + scope `refund_query` |
| GET | base/orders | API Key |
| GET | base/reconcile/summary | API Key + scope `reconcile` |
| GET | base/metrics/auth | API Key |
| GET | base/metrics/auth.prom | API Key |
| POST | base/notify/wechat | 开放（渠道签名验证） |
| POST | base/notify/alipay | 开放（渠道签名验证） |

所有业务路由带 OPTIONS（CORS 预检直通）。

**Sources** · [PayPlugin.cc:357-417](file://libs/drogon-pay/src/PayPlugin.cc#L357-L417) · [openapi.yaml](file://examples/pay-server/openapi.yaml)

## 鉴权流程（fail-closed）

AuthCheck.cc：

- 密钥源：`custom_config.pay.api_keys[]` + 环境 `PAY_API_KEY` / `PAY_API_KEYS`（逗号分隔）；**未配置任何密钥 → 503**；
- 取 `X-Api-Key` 或 `Authorization: Bearer`；缺失或不匹配 → 401；比较用 `CRYPTO_memcmp` 常量时间、全量扫描不提前返回，日志不记密钥内容；
- scope 由 base 相对后缀解析；仅当配置了 api_key_scopes / api_key_default_scopes 才校验，不符 → 403；
- 四个 atomic 计数器（missing_key / invalid_key / scope_denied / not_configured）导出为 `pay_auth_*_total`。

**Sources** · [AuthCheck.cc:119-220](file://libs/drogon-pay/src/handlers/AuthCheck.cc#L119-L220) · [PayAuthMetrics.cc:50-53](file://libs/drogon-pay/src/handlers/PayAuthMetrics.cc#L50-L53)

## 错误码 → HTTP 映射

PayHandlers.cc:17-39：

| 业务码 | HTTP | 语义 |
|--------|------|------|
| 1404 / 1004 | 404 | 资源不存在 |
| 1001 / 1400 / 400 | 400 | 参数错误 |
| 1409 | 409 | 幂等冲突 |
| 1501 | 503 | 自身依赖/渠道未就绪 |
| 1502 | 502 | 上游业务失败 |
| 1002 / 1003 / 其他 | 500 | 内部错误 |

回调路由不走此表：微信错误恒 500；支付宝用字符串 FAIL/成功 JSON。响应信封 `{code, message, data?}`；金额一律元单位十进制字符串（`^\d+(\.\d{1,2})?$`），落库 VARCHAR(32)。

**Sources** · [PayHandlers.cc:17-39](file://libs/drogon-pay/src/handlers/PayHandlers.cc#L17-L39)

## 敏感数据保护

- 密钥永不进源码：config.json 用 `__env_var:NAME__` 占位符，真值走环境变量（.env gitignored）；
- notifyUrl SSRF 防护（PayUtils.cc:498-528）：拦截私网/环回/链路本地；DNS rebinding 为已知局限；
- 回调验签先于任何改库；微信应答验签覆盖所有响应；日志不记密钥与原始签名材料（LOG_TRACE 才允许原始 body）。

**Sources** · [PayUtils.cc:498-528](file://libs/drogon-pay/src/utils/PayUtils.cc#L498-L528) · [SECURITY.md](file://SECURITY.md)
