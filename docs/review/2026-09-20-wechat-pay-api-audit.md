# 微信支付 V3 全流程代码级审计报告

- 日期：2026-09-20
- 范围：`libs/drogon-pay/src/channels/WechatChannel.cc`、`services/{Payment,Refund,Callback,Reconciliation}Service.cc`、
  `utils/PayUtils.cc`、`handlers/{Pay,Callback}Handlers.cc`
- 基线：微信支付 V3 官方规范（`/v3/pay/transactions/native`、`/v3/pay/transactions/out-trade-no/{no}`、
  `/v3/refund/domestic/refunds`、`/v3/certificates`、支付/退款结果通知的验签与 `AEAD_AES_256_GCM` 解密）

## 一、结论摘要

回调（入站通知）链路是**已加固**的：验签 → 时间戳新鲜度 → nonce 防重放 → `resource_type`/`algorithm`/
`associated_data` 校验 → 解密 → `appid`/`mchid` 比对 → **金额与币种与库内订单逐分核对** → `FOR UPDATE` +
CAS 式状态迁移 + 幂等表预留。这条路径未发现可伪造入账的通路。

问题集中在**出站（商户→微信）链路**：通道层把"HTTP 有响应"等同于"业务成功"，导致微信侧的失败被上层记成成功或
记成一种错误的失败，进而产生资金状态漂移。以下每条均有代码证据。

## 二、缺陷清单

### Critical

**C1 — 通道层完全忽略 HTTP 状态码，4xx/5xx 以"成功"上抛**
`WechatChannel.cc:353-378`，响应回调里只判断 `result != ReqResult::Ok || !resp` 和"能否解析成 JSON"，
从头到尾没读过 `resp->statusCode()`。微信 V3 用 HTTP 状态码表达失败（400 `PARAM_ERROR`、401 `SIGN_ERROR`、
403 `NO_AUTH`、404 `ORDER_NOT_EXIST`、500 `SYSTEM_ERROR`），这些响应体是合法 JSON，于是以 `error == ""`
返回给上层。
对照组：`AlipayChannel.cc:360-365` 明确检查了 `getStatusCode() != k200OK`。同一仓库两个通道语义不一致，
微信通道是错的这一侧。
修复：`sendWechatRequest` 对非 2xx 生成 `HTTP <status>: <code> <message>` 形式的 error，并保留 body 供排查。

**C2 — 下单以 `error.empty()` 判定成功，失败订单被写成 PAYING**
`PaymentService.cc:570-574` 的 `paymentCallback` 只在 `!error.empty()` 时走失败分支；`PaymentService.cc:813`
注释直接写 `// Success - update payment and order status`，随后 payment 置 `PROCESSING`、order 置 `PAYING`、
对外返回 `code: 0 "Payment created successfully"`。叠加 C1，微信返回 400 参数错误时，本系统仍报告下单成功。
后果：库里存在一批永远不会被支付的 `PAYING` 订单，对账任务（`ReconciliationService.cc:192`）再去查单，
`ORDER_NOT_EXIST` 响应没有 `trade_state`（`PaymentService.cc:1783-1791` 直接 return），状态永久悬置。
修复：微信分支按 V3 契约校验响应必须含 `code_url`（native）/`prepay_id`（jsapi），缺失即失败；配合 C1。

**C3 — `/pay/qr` 对微信发送支付宝格式的请求体，功能整条不可用**
`PaymentService.cc:1356-1359` 无条件构造 `out_trade_no` + `total_amount`（元）+ `subject`，这是支付宝
`alipay.trade.precreate` 的字段；1383 行把它交给 `channelImpl->createQRPayment()`，微信通道原样 POST 到
`/v3/pay/transactions/native`。V3 native 必填 `amount.total`（**分**、整数）与 `description`，此请求两者皆无 →
必然 400。返回处理同样按支付宝契约：`PaymentService.cc:1406-1407` 要求 `result["code"] == "10000"`，
`1432-1434` 只读 `qr_code`，全文没有微信的 `code_url` 分支（对比 `createPayment` 的
`PaymentService.cc:907-975` 是按通道分流的、正确的）。
净效果：`channel: wechat` 的扫码下单永远失败，且因 1397 行 `clearReservation` 而静默。
修复：`createQRPayment` 复用 `createPayment` 已有的按通道 payload 构造，并按通道解析成功标志与二维码字段。

**C4 — 退款响应缺 `status` 时按"成功受理"落库**
`RefundService.cc:1405-1426`：`refundStatus` 先默认 `"REFUNDING"`，只有 `wechatStatus` 命中
`SUCCESS`/`CLOSED` 才改写；随后无条件调用 `updateRefundWithSuccess(...)`。因 C1，微信返回 400/401 时
`result` 是 `{"code":"PARAM_ERROR",...}`，`status` 为空 → 退款单被写成 `REFUNDING` 并带上空的 `refund_id`，
对外报成功。实际微信侧根本没有这笔退款，用户收不到退款，且 `REFUNDING` 会挡住重试。
修复：`status` 不属于微信合法的 `SUCCESS`/`CLOSED`/`PROCESSING` 集合时走 `updateRefundWithError`。

### High

**H1 — `/v3/certificates` 响应未验签，且下载到的证书不做任何校验**
`WechatChannel.cc:486-518`：拿到 `data[]` 后直接用 APIv3 key 解密并 `setPlatformCert(serial_no, 明文)`，
既没验证响应头的 `Wechatpay-Signature`（官方要求：证书下载响应本身要用**当前已信任的平台证书**验签），
也没校验解密出的 PEM 是否真是 X.509、其序列号是否等于所声称的 `serial_no`、是否在有效期内。
`api_base` 是配置项（`WechatChannel.cc:390`），一旦配置被指向攻击者主机或发生 TLS 终止型中间人，
攻击者可注入自己的"平台证书"，此后 `verifyCallback` 就用它验签 → **伪造支付成功通知**。
修复：安装证书前强制 X509 解析成功 + 序列号自洽 + 有效期覆盖当前时间；响应验签在有可信引导证书时执行。

**H2 — 平台证书回退逻辑用错了序列号字段，且无轮换刷新**
`WechatChannel.cc:672-691`：缓存里没有该 `Wechatpay-Serial` 时，回退读静态 `platform_cert_path`，
并用 `serialNo_`（这是配置里的**商户API证书**序列号，见 `WechatChannel.cc:386`）与通知的**平台证书**序列号比较。
两个不同命名空间的序列号必然不等 → 除非常年命中缓存，否则静态回退路径把所有合法通知判为
`serial number mismatch`；若 `serial_no` 未配置则跳过比较，任何序列号都改用同一张静态证书验签（语义上错位）。
并且缺少官方要求的轮换处理：遇到未知序列号应先下载证书再重试验签（`onStart` 只在进程启动时预热一次，
`WechatChannel.cc:841-855`，还有 `cert_download_min_interval_seconds` 节流）。
修复：新增 `platform_cert_serial` 配置表达静态平台证书序列号并据此比对；未知序列号时触发证书拉取并返回失败，
让微信按其重试策略再投（不阻塞 IO 循环）。

**H3 — 订单号/退款号未做 URL 编码即拼进请求路径**
`WechatChannel.cc:553` 与 `602`：`"/v3/pay/transactions/out-trade-no/" + orderNo + "?mchid=" + mchId_`。
`orderNo` 来自 `pay_order.order_no`，而其上游来自客户端 `order_no`。含 `?`/`#`/`&`/`/` 的订单号可以改写实际
请求的路径与查询串；因为签名串（`WechatChannel.cc:633-634`）用的是同一个字符串，签名依然"正确"，
微信侧会照单执行 —— 相当于用合法签名去访问被篡改的资源位置。
修复：对路径段做百分号编码（签名与请求共用编码后的串）。

### Medium

- **M0**：`timeout_ms` 与 `cert_refresh_interval_seconds` 两个配置项都无人读取。前者被文档承诺
  （默认 5000）、示例配置里也写着，但 `sendWechatRequest` 调用 `HttpClient::sendRequest` 时没有传第三个参数，
  而 Drogon 的默认值是 `0` = 永不超时，微信端挂起时下单/查单/退款请求会永久悬置，幂等预留一直被占住。
  后者承诺了一个代码里并不存在的定时刷新。
- **M1**：回调只比对 `amount.total`（`CallbackService.cc:768-769`），未比对 `payer_total`，个别场景（券/结算差异）
  下实付与订单额不一致不会被发现。
- **M2**：`SUCCESS` 通知带 `transaction_id`，但库内已有 `channel_trade_no` 时不比对，无法发现同一商户单号被
  不同微信交易号重复入账。
- **M3**：`decryptAesGcm` 不校验 IV 长度（微信固定 12 字节，`WechatChannel.cc:228-235` 直接用调用方给的长度），
  且密文恰为 16 字节标签时 `plaintext.resize(0)` 后取 `&plaintext[0]` 是越界取址（`WechatChannel.cc:271-298`）。
- **M4**：`verifyMessageWithCert` 不校验证书有效期（`WechatChannel.cc:106-186`），过期平台证书仍可验签通过。
- **M5**：证书缓存无过期/周期刷新，平台证书轮换后只能靠重启进程。

## 三、修复计划

| 批次 | 内容 | 涉及 |
|------|------|------|
| 1 | C1 通道层 HTTP 状态码 → error；C4 退款响应状态白名单 | WechatChannel.cc, RefundService.cc |
| 2 | C2 下单响应按通道契约校验；C3 `createQRPayment` 通道分流 | PaymentService.cc |
| 3 | H1 证书安装前 X509/序列号/有效期校验；H2 平台证书序列号语义 + 未知序列号触发刷新；M4 有效期 | WechatChannel.cc/.h, config |
| 4 | H3 路径段百分号编码；M3 IV 长度与空明文越界 | WechatChannel.cc |
| 5 | M1 `payer_total`、M2 `transaction_id` 交叉校验 | CallbackService.cc |
| 6 | 单测补齐 + CHANGELOG + openapi/配置文档同步 | tests/, CHANGELOG.md |

不纳入本轮：通道层重构成按通道分派的响应解析器（改动面过大）。M5 的收口方式在复审中被推翻，见第六节。

## 四、验证口径

- `sendWechatRequest` 的失败可被单测覆盖（本地 HTTP 服务或 400 响应）。
- 证书校验、URL 编码、`decryptResource` 加固：`tests/integration/WechatPayClientTest.cc` 已能自签证书 +
  构造密文，直接扩展用例。
- C2/C3/C4 属服务层，走 `tests/integration/CreatePaymentIntegrationTest.cc`、`RefundQueryTest.cc` 既有桩通道。
- 门禁：clang-format、clang-tidy、`scripts/check_openapi_routes.py`、全量 ctest。

## 五、修复落地情况（2026-09-20）

| 项 | 落地 | 证据 |
|----|------|------|
| C1 | `sendWechatRequest` 对非 2xx 生成 `HTTP <status>: <code> <message>`（detail 截断 200 字节），body 仍原样上抛 | `WechatPayClient_QueryTransaction_ReportsHttpErrorAsFailure`（打真实监听器取 404） |
| C2 | native/jsapi 分支按契约要求**非空** `code_url` / `prepay_id`，缺失即失败 | 无直达用例（见第七节）：`CreatePaymentIntegrationTest.cc` 的两个微信用例在更早的 `missing appid/mchid/notify_url` 处就返回 |
| C3 | `createQRPayment` 自建按通道 payload（与 `createPayment` 同形、各自独立，未抽公共函数） | 微信分支现被 501 闸门挡住，见 C5 |
| C4 | 退款响应 `status` 白名单（`SUCCESS`/`CLOSED`/`PROCESSING`），否则走 `updateRefundWithError`；复审发现 `CLOSED` 仍漏进成功路径，见第六节 | `RefundQueryTest.cc` |
| H1 | `setPlatformCert` 安装前强制：X509 可解析 + 有效期覆盖当前 + 证书内序列号与登记者一致 + （可选）链路到 `platform_ca_cert_path` | `WechatPayClient_SetPlatformCert_*` 四个用例 |
| H2 | 没有新增 `platform_cert_serial` 配置：静态兜底证书改用**它自身**的序列号与通知头比对，配置无法与证书漂移；未知序列号触发自节流下载并按失败返回；缓存两侧统一走 `normalizeSerialHex` | `WechatPayClient_VerifyCallback_*`、`SetPlatformCert_BindsCacheKeyToCertificateSerial` |
| H3 | 路径段百分号编码，抽到 `pay::utils::urlEncodePathSegment`（可单测），签名与请求共用编码后的串 | `PayUtils_UrlEncodePathSegment` |
| M0 | `timeout_ms` 真正传入 `HttpClient::sendRequest`（毫秒→秒，0 表示不限时），超时错误为 `http request timed out after <n>ms`；~~`cert_refresh_interval_seconds` 作为无人读取的死配置从示例配置删除~~ 复审推翻，见 M5 行与第六节 | 编译 + 直读；见下方未覆盖项 |
| M1 | `payer_total` 不做等值校验（有券时不等是合法的），~~只拒绝 `<=0` 的非法值~~ 复审改为只拒绝 `> total`（第六节），差额按 `LOG_WARN` 记录 | `PayPlugin_WechatCallback_TransactionIdAndPayerTotalGuards` |
| M2 | `transaction_id` 与库内 `channel_trade_no` 不一致时整笔回滚并返回失败 | 同一用例 |
| M3 | `nonce` 必须 12 字节、`api_v3_key` 必须 32 字节、密文至少要装得下标签（`< 16` 拒绝），空明文不再 `&plaintext[0]` 越界取址 | `DecryptResource_RejectsShortNonce` / `InvalidKey` / `ShortCiphertext` / `InvalidTag` |
| M4 | `verifyMessageWithCert` 校验证书有效期 | 安装期的校验由 `SetPlatformCert_RejectsOutOfValidity` 覆盖；`verifyMessageWithCert` 自己那条过期分支（`WechatChannel.cc:209-215`）无直达用例，见第七节 |
| M5 | ~~删除死配置~~ 复审推翻：定时器一直存在（`PayPlugin::startCertRefreshTimer` 硬编码 43200.0），改为让它读 `cert_refresh_interval_seconds`（下限 300 秒） | PayPlugin.cc/.h, config |

## 六、复审补记（同日，两个子代理评审 84ceaa5 之后）

评审发现的**本轮修复自身**的问题，已一并修掉：

- **C5（新增，未修）**：`/api/qrpay/create` 只插 `pay_order`，从不写 `pay_payment`（两个通道都如此，属既有缺口）。
  C3 把微信扫码的 payload 修对之后，这条路径的净效果从"必然 400"变成"能建真交易但无法入账"——
  回调找不到 payment 就回 FAIL，钱收了订单悬在 PAYING。因此本轮在 QR 入口对微信加了 501 闸门（资金安全优先），
  payload 构造保留待 C5 的入账改动落地后放开。补 `pay_payment` 落库是下一轮的工作。
- **M1 修过头**：`payer_total <= 0` 一律拒绝会杀掉全额代金券（`payer_total` 恰为 0）以及一切合法的低于 `total` 的差额，
  方向反了。改为只拒绝 `payer_total > amount.total`（不可能值），差额仍 `LOG_WARN`；并补了一条正向对照用例。
- **C4 收口不完整**：`CLOSED`/`ABNORMAL` 经 `mapRefundStatus` 得到 `REFUND_FAIL` 后仍走 `updateRefundWithSuccess`
  （订单被置 `REFUNDED`、对外 `code:0`）。现在只有 `SUCCESS`/`PROCESSING` 进成功路径，其余进失败路径。
- **C1 的超时副作用**：传输失败被写成终态 `REFUND_FAIL` 会诱导用新 `out_refund_no` 重试 → 双重退款。
  现在只有微信明确拒绝（`HTTP 4xx` 或响应里的 `code`）才落终态，超时/5xx/2xx 无状态一律保持 `REFUNDING` 交给对账。
- **C2 的 `isMember` 漏洞**：`{"code_url":null}` 与空串同样算通过，改为必须是非空字符串。
- **H1 副作用**：`cert_download_min_interval_seconds` 配 0/负数会把节流整个关掉，而回调端点是公开的——
  每条带未知序列号的通知都会变成一次签名出站请求。下限改为 1 秒。
- **C1 副作用**：非 2xx 且响应无 `code`/`message` 时把响应体原文塞进 error，而这段文本会被反射进对我们 API 调用方的响应；
  现在只回状态码，响应体进 `LOG_TRACE`。
- **未修，需设计决策**：`WechatPayClient::downloadCertificates` 的 HTTP 回调捕获裸 `this`
  （`onStart` 一直是这个形状，本轮的"未知序列号触发刷新"多了第二个入口）。`PayPlugin::setTestClients`
  替换通道 shared_ptr 后，在途回调会打到已析构对象上。修法要么 `enable_shared_from_this`（但测试里通道是栈对象），
  要么把证书缓存挪到一个 shared_ptr 持有的独立状态里；留给下一轮决定。
- **未修，属支付宝线**：`AlipayChannel.cc:428` 把 `timeoutMs_`（30000）直接交给以秒计的参数，等于 8.3 小时。

本轮**未加自动化测试**的一项：M0 的超时分支。要确定性地复现"连接已建立但对端不回包"，需要在测试里挂一个
静默 `TcpServer`，其回调签名与 trantor 版本耦合；该改动是 4 行直读代码（构造函数取值 → 传参 → 单位换算 →
错误串），故按编译与直读验证，并把它记在这里而不是用一个假断言凑数。服务层对超时的反应等同于已有的
网络失败分支，那条分支已被 C1/C2 的用例覆盖。

验证边界：本机没有 `.env`/Postgres 角色，DB 用例与 `clang-tidy`（需 `compile_commands.json`，只在 Linux
preset 下生成）由 CI 判定；本地已跑通 MSVC 构建、`clang_format.py --check`、全部 Python 门禁与非 DB 用例。

## 七、第二轮复审补记（60d4afc 之后）

- **退款终态判定的第一版太窄，会砸掉既有用例**：只按 `HTTP 4` 前缀认定"微信明确拒绝"，于是
  "配置缺失""客户端未就绪"这类**请求根本没发出去**的失败也被留在 `REFUNDING`，与
  `RefundQueryTest.cc:781,923,953,1087,1115,1257,1284,1456,1485,1660,1803` 的期望相反。
  现在由 `refundCertainlyDidNotHappen()` 判定：本地故障与"带微信错误信封的 4xx"落终态；
  5xx、无信封的 4xx（中间设备替我们回答）、超时、传输失败、2xx 无状态一律保持 `REFUNDING`。
- **`/api/qrpay/create` 的状态码对不上是系统性的**：`mapErrorToHttpStatus` 读的是 `error.value()`，
  而服务层不少地方传的是 `std::errc::*`（`EINVAL`=22、`EIO`=5），因此 body 里的 1001/400/500/1005
  全落到 HTTP 500。本轮只把微信 QR 路径上的三处改成 `makePayError(400|501)`，并在映射表补 400/501；
  其余通道/查询路径**未改**，`openapi.yaml` 的总述已改成如实描述这一限制。把服务层错误统一成业务码
  是下一轮的独立改动。
- **第五节的证据列有三处夸大，已就地更正**：C2（`code_url` 契约没有直达用例——两个微信建单用例在更早的
  `missing appid/mchid/notify_url` 检查处就返回，补用例需要先给测试注入可用的微信配置）、C3（payload
  构造是重复而非复用，且微信分支现处于 501 之后不可达）、M4（`verifyMessageWithCert` 的过期分支无直达
  用例）、H1（四个用例不是三个）。
- **配置文档的默认值与代码相反**：`reconcile.enabled` 与 `channels.<name>.enabled` 实际默认 `true`
  （`PayPlugin.cc:48,62,139,220`），`configuration_guide.md` 原写 false，已改。

