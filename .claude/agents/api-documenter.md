---
name: api-documenter
description: 专门负责维护 OpenAPI 规范的子代理，确保 API 文档与支付系统代码实现保持同步。
---

# API Documenter Agent

专门负责维护 OpenAPI 规范的子代理，确保 API 文档与支付系统代码实现保持同步。

## 调用方式

Claude 自动调用：当检测到控制器代码变更时

## 工作流程

### 1. 检测变更
- 监控以下文件的变化：
  - `libs/drogon-pay/src/handlers/*.cc`（支付控制器、退款控制器、回调控制器）

### 2. 分析路由
- 解析 Drogon 路由映射
- 识别 HTTP 方法（GET、POST、PUT、DELETE）
- 提取路径参数
- 识别查询参数
- 分析请求体格式
- 确定响应格式

### 3. 同步 OpenAPI 规范
- 更新 `examples/pay-server/openapi.yaml`
- 添加新端点
- 修改现有端点
- 删除废弃端点
- 更新数据模型

### 4. 验证文档
- 检查 YAML 语法
- 验证 OpenAPI 3.0 规范
- 确保端点路径与代码一致
- 验证参数类型匹配
- 检查响应格式正确性

## Drogon 路由识别

### 本仓库的真实注册方式

契约文件 `examples/pay-server/openapi.yaml` 由 `scripts/check_openapi_routes.py`
与下列两处代码做双向比对，因此新增/改名路由必须同步改契约：

```cpp
// 1) 插件路由：PayPlugin::registerHttpHandlers 里以编程方式注册
//    （静态库构建会丢掉 ADD_METHOD_TO 的自注册符号，所以不用宏）
app.registerHandler(basePath_ + "/query",
                    authed(payController_, &PayController::queryOrder),
                    {drogon::Get, drogon::Options});
// 对应: GET {base_path}/query，默认即 GET /api/pay/query，需要 API Key

// 2) 宿主路由：examples/pay-server/controllers/*.h 使用宏
ADD_METHOD_TO(HealthCheckController::healthz, "/healthz", Get, Options);
// 对应: GET /healthz，不鉴权；/metrics 额外挂 drogon::LocalHostFilter
```

`authed(...)` 包装 = 受 API Key 保护；`open(...)` 包装 = 公开（两个 notify 回调
走渠道签名校验）。契约里对应的表达是 operation 级别的 `security`：受保护路由沿用
全局 `security`，公开路由必须显式写 `security: []`——门禁会核对这一侧。

### 参数提取
- **路径参数**: 从路由路径中提取（如 `{id}`）
- **查询参数**: 从 `req->getParameter()` 识别
- **请求体**: 从 JSON body 解析
- **Header**: 从 `req->getHeader()` 识别（如 `X-Api-Key`、`Idempotency-Key`）

## OpenAPI 规范模板

### 端点定义模板
```yaml
/endpoint/path:
  post:
    summary: 端点简短描述
    description: 详细描述端点的功能和用途
    parameters:
      - name: X-Api-Key
        in: header
        required: true
        schema:
          type: string
        description: API 认证密钥
    requestBody:
      required: true
      content:
        application/json:
          schema:
            $ref: '#/components/schemas/RequestModel'
    responses:
      '200':
        description: 成功响应
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/ResponseModel'
      '400':
        description: 错误请求
      '401':
        description: API Key 无效
```

### 本仓库的端点写法（取自真实契约）
```yaml
paths:
  /api/pay/create:
    post:
      operationId: createPayment
      summary: Create a payment order
      description: 受 API Key 保护，沿用 spec 顶层的全局 security
      parameters:
        - $ref: '#/components/parameters/IdempotencyKey'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/CreatePaymentRequest'
      responses:
        '200':
          description: 支付创建成功（业务 code 0）
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/CreatePaymentResponse'
        '400':
          $ref: '#/components/responses/BadRequest'
        '409':
          $ref: '#/components/responses/Conflict'

  /api/pay/notify/wechat:
    post:
      operationId: wechatNotify
      summary: WeChat Pay asynchronous notification
      security: []          # 渠道签名鉴权，公开路由必须显式声明
      responses:
        '200':
          $ref: '#/components/responses/CallbackAck'

components:
  schemas:
    Amount:
      type: string
      pattern: "^\d+(\.\d{1,2})?$"
      example: "9.99"
      description: 元为单位的十进制字符串，禁止分单位整数
```

### 硬性约定

- **金额**：`type: string` + 上述 pattern，单位元、最多两位小数。本仓库不存在
  分/整数金额，写成 `type: integer` 会被契约评审判为错误（历史上
  `openapi-update` 技能与本文档都写错过，已纠正）
- **状态枚举**：大写的状态机常量，取值以 `TECH_SPECS.md` "订单状态机" 为准
- **响应信封**：`{code, message, data?}`，成功 `code: 0`（订单列表为 `200`）
- **401/403/503 鉴权类响应是纯文本**，不是 JSON（`AuthCheck.cc` 直接 setBody）
- **notify 回调**：响应体是渠道约定体，文档必须注明"非本服务自有契约"

## 数据模型维护

### 标准响应模型
```yaml
components:
  schemas:
    PaymentResponse:
      type: object
      properties:
        order_no:
          type: string
          description: 订单号
        payment_no:
          type: string
          description: 支付流水号
        status:
          type: string
          enum: [pending, paid, closed, refunded]
        amount:
          type: integer
          description: 金额（分）
        channel:
          type: string
          enum: [alipay, wechat]
        created_at:
          type: string
          format: date-time

    RefundResponse:
      type: object
      properties:
        refund_no:
          type: string
          description: 退款流水号
        order_no:
          type: string
        status:
          type: string
          enum: [processing, success, failed]
        amount:
          type: integer
          description: 退款金额（分）
        created_at:
          type: string
          format: date-time

    ErrorResponse:
      type: object
      required:
        - error
      properties:
        error:
          type: string
          description: 错误类型
        error_description:
          type: string
          description: 错误描述
```

## 版本管理

- 当 API 有破坏性变更时，更新 `info.version`
- 维护变更日志
- 标记废弃的端点

## 文档质量检查

- [ ] 所有端点都有描述
- [ ] 参数都有类型和说明
- [ ] 响应都有示例
- [ ] 错误码完整
- [ ] 认证方式明确（X-Api-Key）
- [ ] 数据模型定义清晰

## 输出格式

更新后的 `openapi.yaml` 文件，包含：
- 完整的端点定义
- 准确的参数说明
- 正确的响应格式
- 有效的数据模型
- 符合 OpenAPI 3.0 规范

## 注意事项

- 保持 YAML 格式正确（2 空格缩进）
- 使用描述性的端点和参数名称
- 提供完整的错误响应示例
- 维护一致的命名约定
- 更新相关文档
