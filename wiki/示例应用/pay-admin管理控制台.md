# pay-admin 管理控制台

<cite>
**Referenced Files**
- [package.json](file://examples/pay-admin/package.json)
- [vite.config.js](file://examples/pay-admin/vite.config.js)
</cite>

## 技术栈

Vue 3.5 + Vite 7 + Element Plus 2.11 + Pinia + vue-router + axios + qrcode（package.json）。版本顶层 `version` 是四处版本声明源之一，受 check_version_sync.py 约束。

**Sources** · [package.json](file://examples/pay-admin/package.json)

## API Key 注入与响应归一化

- Vite 代理 `/api` → localhost:5566（vite.config.js）；
- axios 拦截器注入 `X-Api-Key`；Key 存 Pinia user store（sessionStorage 持久化，开发态 VITE_DEFAULT_API_KEY 回填）；
- 响应归一化：code 0/200 成功、code 1 降级标记（渠道查询失败附 *_query_error）、401/403/503 友好文案。

**Sources** · [vite.config.js](file://examples/pay-admin/vite.config.js)

## 五个页面

| 页面 | 功能 |
|------|------|
| 创建支付 | 渠道表单 + 二维码渲染 + 状态轮询 |
| 订单管理 | 筛选 + limit+1 探测分页 |
| 订单详情 | PAID 可跳退款 |
| 退款管理 | 发起 + 进度轮询 |
| 对账与指标 | reconcileSummary + authMetrics 轮询 |

页面与后端路由一一对应（见「HTTP契约与安全」路由表）；金额展示遵循元单位字符串约定。

**Sources** · [README.md](file://examples/pay-admin/README.md)
