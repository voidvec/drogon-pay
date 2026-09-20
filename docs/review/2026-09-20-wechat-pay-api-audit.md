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
| C2 | native/jsapi 分支按契约要求**非空** `code_url` / `prepay_id`，缺失即失败 | 部分覆盖：服务层把 `code_url` 交回调用方由 `PayPlugin_QrBooking_WechatQrCreatesAPaymentRow` 证明；`channelResultError` 自身的"缺字段/字段为空"分支仍无直达用例（`CreatePaymentIntegrationTest.cc` 的两个微信用例在更早的 `missing appid/mchid/notify_url` 处就返回，见第七节） |
| C3 | `createQRPayment` 自建按通道 payload（与 `createPayment` 同形、各自独立，未抽公共函数） | 微信分支已随 C5 放开；`PayPlugin_QrBooking_WechatQrCreatesAPaymentRow` 断言通道收到 `amount.total=999`（分）与 `out_trade_no` |
| C4 | 退款响应 `status` 白名单（`SUCCESS`/`CLOSED`/`PROCESSING`），否则走 `updateRefundWithError`；复审发现 `CLOSED` 仍漏进成功路径，见第六节 | `RefundQueryTest.cc` |
| C5 | QR 入口改为**先落库再问通道**：`pay_order`(`CREATED`) + `pay_payment`(`INIT`，带请求 payload) → 通道受理后晋升 `PAYING`/`PROCESSING`（含通道响应），通道拒绝只关闭该条 payment(`FAIL`)；`order_no` 唯一，重试复用订单并追加 payment；已结算、或金额/币种/通道/属主不符时以 400 拒绝复用（币种与属主的比对见第九节） | `PayPlugin_QrBooking_WechatQrCreatesAPaymentRow`、`PayPlugin_QrBooking_ChannelRefusalClosesThePaymentAndAllowsRetry`；多尝试订单连带打破的重复通知审计分支见第八节，"被拒绝的尝试不得遮蔽可付尝试"见第九节 |
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

- **C5（新增，本轮已修）**：`/api/qrpay/create` 只插 `pay_order`，从不写 `pay_payment`（两个通道都如此，属既有缺口）。
  C3 把微信扫码的 payload 修对之后，这条路径的净效果从"必然 400"变成"能建真交易但无法入账"——
  回调找不到 payment 就回 FAIL，钱收了订单悬在 PAYING。第一轮的做法是在 QR 入口对微信加 501 闸门（资金安全优先），
  本轮把入账补齐后闸门已删除：订单与 payment 在问通道之前落库，成功后晋升 `PAYING`/`PROCESSING`，
  通道拒绝只关闭该条 payment。回调侧无需改动即可结算——`CallbackService` 按 `order_no` 找 payment，
  只跳过 `SUCCESS`/`REFUNDED`，且 native 建单不带 `channel_trade_no`，`transaction_id` 比对对空值天然放行。
  顺带修掉两个由此暴露的问题：`pay_order.order_no` 唯一导致"失败后重试"必然撞库（现按订单复用 + 追加 payment），
  以及该端点在 `dbClient_` 缺失时会在 Mapper 里崩溃（现回 1003）。
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
  全落到 HTTP 500。第一轮把微信 QR 路径上的三处改成 `makePayError(400|501)` 并在映射表补 400/501；
  C5 落地后 501 不再由任何路径产生，映射表里的 501 分支与 `openapi.yaml` 的 501 响应已一并删除，
  QR 路径统一走 `failQr(code, message, extra)`：4xx 由业务码决定状态，5xx 仍回 HTTP 500。
  其余通道/查询路径**未改**，`openapi.yaml` 的总述已改成如实描述这一限制。把服务层错误统一成业务码
  是下一轮的独立改动。
- **第五节的证据列有三处夸大，已就地更正**：C2（`code_url` 契约没有直达用例——两个微信建单用例在更早的
  `missing appid/mchid/notify_url` 检查处就返回，补用例需要先给测试注入可用的微信配置）、C3（payload
  构造是重复而非复用，且微信分支当时处于 501 之后不可达——C5 落地后已可达并由 QR 用例覆盖）、M4（`verifyMessageWithCert` 的过期分支无直达
  用例）、H1（四个用例不是三个）。
- **配置文档的默认值与代码相反**：`reconcile.enabled` 与 `channels.<name>.enabled` 实际默认 `true`
  （`PayPlugin.cc:114,128,205,286`），`configuration_guide.md` 原写 false，已改。


## 八、C5 落地补记（本轮）

- **入账先于问通道**：`createQRPayment` 现在按 `pay_order`(`CREATED`) → `pay_payment`(`INIT`) →
  通道 → 晋升（payment `PROCESSING` + 通道响应、order `PAYING`）→ 响应 的顺序走，响应仍晚于
  幂等快照写入。入账分支不需要改：`CallbackService` 是按 `order_no` 取 payment，只把
  `SUCCESS`/`REFUNDED` 视为已终态，`INIT`/`PROCESSING` 都可入账，而 native 建单不回
  `transaction_id`，第五节 M2 的比对对空值放行。重复通知分支需要，见下一条。
- **重复通知的审计分支被 C5 打破，已一并修**：两个分支（交易 `CallbackService.cc:590`、退款
  `CallbackService.cc:2295`，行号按第十节那一轮重挂）都用 `findOne(order_no)` 找 payment 来写 `pay_callback`，而 Drogon 的
  `findOne` 在命中 0 行**或多行**时都走异常回调（`UnexpectedRows`，Drogon 的 Mapper 头文件里
  `r.size() > 1` 那一支）。一个订单只有一条 payment 时它没事；一旦"拒绝后重试"让同一订单带上两条
  payment，重复通知就会得到 `FAIL`/1400，而 WeChat 收到 FAIL 会一直重发，审计行反而永远写不下。
  现在两处都改成 `created_at DESC LIMIT 1` 的 `findBy`，与入账分支（`CallbackService.cc:760`）取的
  是同一条 payment，空结果仍按原行为回 1400（`UnexpectedRows("0 rows found")`）。正向对照：
  `PayPlugin_WechatCallback_IdempotencyHitRecordsCallback` 与
  `PayPlugin_WechatCallback_RefundIdempotencyHitRecordsCallback` 现在各多插一条 60 秒前的 `FAIL`
  尝试——旧代码在断言 `!error` 处就会失败，新代码另外断言审计行落在最新那条尝试上。
  其余按 `order_no` 取 payment 的位置（`PaymentService.cc:2464,2916`、`RefundService.cc` 的选单）
  本来就是有序的 `findBy`；它们与上面三处对"最新"的口径直到第九节才真正统一（状态白名单），
  而退款那条在第十节又被细化成"已结算的尝试优先"。
- **通道拒绝不回滚订单**：只关闭这一条 payment。同一订单可能已有前一次尝试留下的可用二维码，
  把订单写成 `FAILED` 会掩盖真实可付的单子。
- **重试可用**：`pay_order.order_no` 唯一（`sql/001_init_pay_tables.sql`），`pay_payment.order_no`
  只是索引，因此复用订单 + 每次尝试一条 payment 是让"通道故障后重试"仍然可走的最小改动；
  复用被限制在同一金额同一通道，且已结算的订单一律以 400 拒绝再次发码。
- **覆盖与本地限制**：`tests/integration/QrPaymentBookingTest.cc` 用 `PayPlugin::setTestChannels()`
  注入桩通道，其中凡是要断言库内那一行的用例都需要 PostgreSQL，本机（WSL 的 Postgres 无 `test` 角色）
  跑不了，由 CI 的 linux/windows 两腿执行——与 AGENTS.md 的说明一致，不是代码问题。
- **仍未做（不在本轮范围）**：把服务层残余的 `std::errc::*` 统一成业务码（第七节）；
  `AlipayChannel.cc:428` 把毫秒当秒用；`WechatChannel::downloadCertificates` 回调里裸 `this`
  的生命周期假设（第六节）。

## 九、第三轮复审补记（1042f9a 之后，本轮）

本轮从"两个评审子代理 + 逐条对撞文档声明"起步，先修存活与归属，再对撞文档。守卫方向与正向对照
按 `feedback-audit-claim-verification` 的口径逐条核对。

- **一个类型错的 JSON 成员就能打死进程（Critical，本轮修）**：所有写接口都用 jsoncpp 的
  `asString()`/`asInt64()` 直接读成员，而它对类型不符的成员**抛异常**；`HttpControllersRouter` 到
  trantor 之间没人接，`EventLoop::loop()` 捕获后停循环、展开时再抛，`app().run()` 随之返回。
  回调端点无需任何凭据，一个匿名 `{"event_type":{}` 即可；写路由在 `checkAuth` 之后，一把泄漏的
  API key 就够了。现在四个 POST 面先校验形状（`validateBodyTypes`、`CallbackHandlers` 的
  `event_type` 显式判定）再读，回 400；`registerHttpHandlers` 的 `authed`/`open` 两条注册路径统一
  套 `guarded()` 异常屏障，只在"响应之前抛出"时补一个 500，屏障内的回调被 `atomic` 收成至多一次，
  异步完成不会被二次应答。`tests/integration/RequestBodyShapeTest.cc` 按成员逐一对着 handler 打；
  这一族不碰库与通道，所以不需要 PostgreSQL —— 但**它碰插件**，而插件在不在取决于启动目录能读到
  `config.json`（详见 §十 那条更正：原先写"14 条因此本机可跑"既少算了用例，也没说出真正的分档条件）。
  **未做**：`guarded()` 自身没有"故意让 handler 抛出"的直达用例（要起一个真在注册后被打、且 handler
  内抛的 HTTP 服务）。它确实被间接穿过：`RouteRegistrationSmoke`、`HttpHeaders_*`、`HealthProbe_*`
  共 8 条用例打的都是注册后的路由，因此屏障在正常应答路径上是绿的——缺的只是那条故障分支。
- **钱可以记在一个查询再也认不出的属主名下（Critical，本轮修）**：`/api/pay/create` 文档承诺的 401
  分支是死代码 —— `Attributes::get` 从不抛（缺键与类型不符都回默认构造的 `0`，只 `LOG_ERROR` 一句
  "Bad type"，见 Drogon `Attribute.h`），而全仓库没有任何地方写 `user_id` 属性，于是所有不带 body
  `user_id` 的下单都以属主 `0` 入账，而 `0` 正是 `queryOrderList` 的"不加属主过滤"哨兵值。
  现在存在性用 `find()` 判、属主必须 `> 0`。QR 侧同族还有两刀：handler 用 `FieldType::Int` + `asInt()`
  读属主，把只 fit 64 位的真实租户当"类型错误"拒掉（`pay_order.user_id` 是 BIGINT），服务层
  `createQRPayment` 则用 `request.get("user_id", "1").asInt64()` —— 默认值是**字符串**，对直接调用
  服务的一方会抛，不抛时又把订单和幂等哈希记到租户 `1` 名下。三处统一到"解析一次、必须为正"。
  正向对照：`PayHandlers_CreatePayment_MissingUserId_StillAnswers401`（类型守卫没吞掉 401）、
  `PayHandlers_CreateQRPayment_OwnerAboveInt32Range_NotRefusedAsMistyped`（合法 int64 必须放行）、
  `PayPlugin_QrBooking_CallerOwnerIsBookedOnTheOrder`（落库属主，CI-only）。
- **`/api/qrpay/create` 把决定钱落在哪的字段全丢了（High，本轮修）**：handler 从零重建服务请求，
  `currency`/`notify_url`/`buyer_id`/`idempotency_key` 无人复制 —— 于是每一笔 QR 单都按 CNY 计价、
  绑定全局回调地址、不带 buyer，而文档承诺的 `X-Idempotency-Key` 对这条路由**完全无效**（只剩派生的
  `QR_<order_no>_<channel>`）。现在四个字段透传（`idempotency_key` 另加请求头兜底），`notify_url` 过
  `/api/pay/create` 同一条 SSRF 闸（P1-3 的 QR 缺口；仅 wechat 分支会把它发给通道，支付宝的回调地址由
  服务端配置决定，成员在那条分支不生效），`currency` 归一成大写三字母并同时写进 `amount.currency` 与订单行
  （回调按币种比对，记错即永不可结算），`amount` 也过与 `/api/pay/create` 相同的格式校验（原先 Alipay
  收到原样的 `total_amount` 并把同一个串记在订单上）。
- **第二个调用者可以重放别人的二维码（High，本轮修）**：QR 的幂等请求哈希原先只覆盖
  `order_no`/`amount`/`channel`/`subject`，同一单号换一个 `user_id`/`currency`/`notify_url`/`buyer_id`
  哈希不变，于是被判成"重放"并把**第一次那笔的二维码**回给它 —— 别人可付的码 + 别人的回调地址。
  四个字段现已入哈希，冲突以 1004/404 呈现；订单复用侧另补了币种与属主两项 400 比对。
- **被拒绝的尝试可以遮蔽可付的那条（High，本轮修）**：入账、两条审计分支、退款选单、两处
  通道查单回写，全部按"该订单 `created_at DESC LIMIT 1`"取 payment。QR 一次尝试一行之后，"最新"
  完全可能是一条已被拒绝关闭的 `FAIL`：状态 CAS 打不中任何行，通知被 ACK 成 SUCCESS 而钱没入账；
  退款则去退一笔从未存在的交易，而真正已付那条永远退不掉。三处回调/审计与两处同步统一走
  `openAttemptsOfOrder()` 白名单（`INIT`/`PROCESSING`/`SUCCESS`/`REFUNDED`），尝试全闭合的订单
  按 1404/FAIL 报出来而不是静默确认。用例 `PayPlugin_WechatCallback_ClosedAttemptDoesNotShadowThePayableOne`（CI-only）。
- **没定稿的幂等预留被当成"已处理"确认（High，本轮修）**：两个回调分支只要看到 `pay_idempotency`
  有行就走重复分支（记审计 + 回 SUCCESS），不看 `response_snapshot` 是否已写。空快照只说明"预留被取走、
  那一刀没跑完"（仍在途，或在事务提交前死掉），确认它正好停掉微信的重试，而钱此刻落在任何地方都没有。
  现在先删掉这条过期预留再回 FAIL/1400，下一次投递走完整路径，并发由入账的 CAS 兜住。
  用例 `PayPlugin_WechatCallback_UnfinalizedReservationIsReprocessedOnRetry`（CI-only）。
- **文档与代码对撞的结果（本轮逐条更正）**：
  - `TECH_SPECS.md` 的「`/api/qrpay/create` 无 `CREATED` 状态，INSERT 即 `PAYING`」在 C5 之后为假，
    状态机一小节已按"订单 `CREATED`→`PAYING`、每次尝试一条 payment、只有可证明的拒绝才关闭该行、
    渠道拒绝不改订单状态"重写。
  - `openapi.yaml`：create/qrpay 两条路由上写着的 409/502/503 不可能产生（1409/1501/1502 只由
    `RefundService` 产生），1004 实际映射 404；`out_trade_no` 原描述成"渠道交易号"，它是**商户**单号
    （渠道自己的是 `trade_no`，预下单不回），`user_id` 补 int64/下界 1；QR 请求体补 `currency`/
    `notify_url`/`buyer_id`/`idempotency_key`，并明写 `description` 接受而不使用。
  - `CHANGELOG.md` 里 `QrPaymentBookingTest.cc` 原写"drives `/api/qrpay/create`"，它驱动的是该路由
    背后的服务方法；`RequestBodyShapeTest.cc` 头注释原写"none of it required an API key"，写路由其实
    在 `checkAuth` 之后 —— 两处都已改成按路由区分；`PaymentService.cc` 里"Alipay precreate 回它自己的
    交易号"的注释同上更正。
  - `docs/api/pay-api-examples.md`：401 的真实触发条件、QR 的拒因表（body 码 vs HTTP）、
    派生幂等键的完整式子（含 `sha256(amount + currency)`）。
- **验证边界**：本轮本机证据 = MSVC Release 构建 + `RequestBodyShapeTest.cc` 14 条（非 DB）+
  穿过屏障的路由级回归 8 条（`RouteRegistrationSmoke`、`HttpHeaders_*`×4、`HealthProbe_*`×3）+
  全部 Python 门禁；`QrPaymentBookingTest.cc` 新增 10 条与 `WechatCallbackIntegrationTest.cc` 新增 2 条都要
  PostgreSQL，只有 CI 证据。
- **仍未做**：`guarded()` 的路由级用例（上一条）；支付宝 QR 入账分支不比对 `total_amount` 与订单金额
  （微信侧比对了 `payer_total`）；服务层残余 `std::errc::*` → 业务码（第七节）；`AlipayChannel.cc:428`
  毫秒当秒（第六节）；`downloadCertificates` 裸 `this`（第六节）；`/api/pay/orders` 不带 `user_id` 时
  返回**全部**属主的单 —— 这是运维语义而非缺陷，但该在契约里明写，免得被当成越权查询的例外。

## 十、第四轮复审补记（4f028d0 之后，本轮）

两个评审子代理对着 `4f028d0` 全量重读，一条一条落到代码上核对。结论：**上一轮修的东西没有一处被推翻，
但它自己带进四个新缺陷**，其中两个正好是 memory 里"核对守卫方向"该抓的形状。

- **异常屏障自己会抛出（BLOCKER，本轮修）**：`guarded()` 用 `handler(req, std::move(onceCb))` 把闭包
  交给 handler，这一步就把 `onceCb` 的目标**移走**了（`authed` 包装层与每个 controller 都按
  `std::function&&` 继续 move）。于是 catch 分支里那句 `onceCb(...)` 调的是一个空的 `std::function` ——
  `std::bad_function_call` 从 catch 里抛出，直接越过屏障，正是屏障要拦的那条"逃出 `app().run()`"路径。
  现在改为交出副本（`PayPlugin.cc:85`），两份副本共享同一个 `answered`，至多一次的语义不变。
  评审给的机制描述并不准确（被移走的是 `onceCb` 里的目标，不是调用方的 `cb`），但缺陷成立。
- **屏障的 500 只有 body 是 500（MAJOR，本轮修）**：`fault` 组了 `{"code":500}` 却没
  `setStatusCode(k500InternalServerError)`，HTTP 层回 200 —— 调用方按状态码分支的都会把这道故障当成功。
  `PayPlugin.cc:74`。
- **删过期预留可以顺手删掉刚定稿的那条（MAJOR，本轮修）**：`findOne` 与 `deleteBy` 之间，取走预留的
  那一次投递完全可能已经写完 `response_snapshot`；原先的删除只按 `idempotency_key`，会把"已处理"的
  证据抹掉，下一次投递于是重跑入账。两条回调分支合并成 `dropUnfinalizedReservation()`
  （`CallbackService.cc:52`，调用点 `550`、`2242`），判据补 `response_snapshot IS NULL`；删除命中 0 行
  仍回 FAIL/1400，微信再投一次就命中定稿快照。
- **退款选单还会被较新的 `INIT` 遮蔽（MAJOR，本轮修）**：上一轮的白名单挡住了 `FAIL`，但"最新"仍可能
  是一条新一点的 `INIT` 僵尸，而真正已付的那条在它下面 —— 于是回 1409，钱在库里、退款被拒。现在两段
  查询：先 `SUCCESS`/`REFUNDED`，空了才看 `INIT`/`PROCESSING`，两边都空仍按原来的 1404
  （`RefundService.cc:501-561`）。用例 `PayPlugin_Refund_SettledAttemptIsPickedOverANewerOpenOne`（CI-only）。
- **没有 `code_url` 的 2xx 被当成明确拒绝（本轮修）**：`qrAttemptCertainlyNotCreated` 末尾那句
  `return !wentThroughHttp` 把"渠道回了但没有可付码"判成关闭 —— 与该函数自己的注释、与
  `TECH_SPECS.md:238,257`、与它上面那条 `LOG_WARN`（"leaving it in flight"）三处都相反。文档这次是对的：
  交易可能在中间层改写的响应背后已经建起来，关闭就把它从回调与对账的视野里删掉。现在用一个文件内常量
  把这条消息认出来并留在途（`PaymentService.cc:190,230,292`）。用例
  `PayPlugin_QrBooking_AnswerWithoutCodeUrlKeepsTheAttemptInFlight`（CI-only）。
- **payment 状态词表少了一个拼写（本轮修）**：支付宝同步分支把 payment 写成 `FAILED`
  （`PaymentService.cc:2875` 现为 `FAIL`）。`FAILED` 是**订单**状态（`mapTradeState` 就是这么分的），
  payment 侧从来只写 `FAIL`；拼错的那一行既不落在任何 `FAIL` 查询里，也不在开放尝试的白名单里。
  同处顺手收紧了 §九 的措辞：payment 没有 `CLOSED` 这个词，被排除的闭合状态只有 `FAIL`。
- **幂等哈希按原样币种算（本轮修）**：币种在入库前才归一化，哈希却用了请求里的原始串，于是同一笔单
  `"cny"` 与 `"CNY"`、缺省与显式 `"CNY"` 都会被判成 1004。现在哈希与订单行、通道 payload 用同一个
  派生值（`PaymentService.cc:1445,1465`），非法币种仍照原样入哈希（它压根进不到建单）。用例
  `PayPlugin_QrBooking_CurrencySpellingIsNotAnIdempotencyConflict`（CI-only）。
- **核对后不改的**：`Attributes::get<T>` 的类型不符确实回默认构造值并 `LOG_ERROR` 一句 "Bad type"
  （Drogon `Attribute.h:39-56`），§九 那条关于 401 死代码的论证按原文成立。
- **文档更正**：§九 的 `RequestBodyShapeTest.cc` 由 13 改 14、`QrPaymentBookingTest.cc` 新增由 6 改 10；
  §七 里指向 `PayPlugin.cc` 的行号在屏障改写后已重挂。
- **两处契约声明与代码对不上（本轮更正）**：
  - `openapi.yaml` 的 `CreatePaymentRequest.required` 里挂着 `user_id`，而 handler 是"body 或
    `user_id` 请求属性"二选一（`PayHandlers.cc:215-227`），只带属性的请求合法且不带这个 body 成员 —— 该
    schema 自己的描述也这么写。现在 `required` 只剩 `[order_no, amount]`，`user_id` 的下界与"缺两者回
    401"的说明留在字段描述里，并点明 QR 那条路由是**必须**写在 body 里（`PayHandlers.cc:306`）；
    `docs/api/pay-api-examples.md:51-52` 早已按"body 或属性"描述，契约与示例从此一致。
  - `CHANGELOG.md` 的退款条目原先写成"未知状态一律走 `1502` + `REFUND_FAIL`"，与同段后半句"只有渠道
    明确拒绝才报 `REFUND_FAIL`"自相矛盾：`RefundService.cc:1476,1505` 的 `definitive` 只决定落库与
    `data.status`，`1502`（HTTP 502，`PayHandlers.cc:32-33`）两种结局共用。现在改为按"整个分支都回
    `1502`，区分只看 body 的 `data.status`"来描述。
- **一个"文档里说可能为空"的指针被十处 handler 直接解引用（BLOCKER，本轮修）**：
  `drogon::app().getPlugin<PayPlugin>()` 的返回值在 `4f028d0` 的 `PayHandlers.cc:257,420,463,605,646,743,784`
  与 `CallbackHandlers.cc:27,180,249` 都是拿来就用（`plugin->paymentService()`）。而 `PayPlugin.h:20-23`
  明写这个指针**可能为空**（静态库链接丢掉 DrObject 自注册符号时就是空，`ensureLinked()` 正是为此而
  存在），配置里没有 `PayPlugin` 一项也得到同样的状态。在空指针上调成员函数不是"这一个请求丢了"，而是
  handler 内访问违例 —— 上一轮那道 `guarded()` 屏障拦不到它，因为屏障只套在注册后的路由上，而崩溃发生在
  进程里。更要紧的是 wechat 回调把这次解引用放在**校验之前**：形状守卫在它下面，在这个状态下正好是死代码。
  现在八处统一"先判存在再取用"、缺失时由 `src/handlers/PluginGuard.h` 的 `respondPluginUnavailable()`
  回 1501/503（本契约已用于"我们自己的依赖缺失"的那个码）：`PayHandlers.cc` 七处（`262,430,478,625,671,773,819`）
  加 wechat notify 一处（`CallbackHandlers.cc:107`）—— 本节初稿写"九处统一回 1501/503"是错的，多算的
  那一处是 alipay，它回的不是这个码。第十处（`4f028d0` 的 `CallbackHandlers.cc:249`）
  只能在上一个提前 return 之后到达。wechat 侧把服务解析移到信封校验**之后**，因此不认识的通知仍按自身原因被拒；alipay
  侧把"没有插件"折进它已有的"没有验签客户端"分支 —— 对验签方而言是同一种故障，且永不确认收到（回
  `{"code":"FAIL"}` + HTTP 200，见上面对 alipay 的说明，而不是 1501/503）。
  `openapi.yaml` 给 notify 路由补上 503；本轮第五节再更正它两处：那条 503 的 body 是数值
  `ErrorResponse`（`{"code":1501}`）而不是 `CallbackAck`（它的 `code` 是 `SUCCESS`/`FAIL` 串枚举 ——
  回绝本来就不是确认），共享的 `ServiceUnavailable` 说明也按"auth 明文（无业务码）/ 没有插件回 1501 /
  通道不可用：pay 回 1002、qrpay 回 1005、refund 回 1501"重写，原先那句"三种都带业务码 1501"不成立。
- **上一轮"本机 14 条全绿"是错的（本轮更正，并据此改口）**：`RequestBodyShapeTest.cc` 里两条会走到那次
  解引用的用例 —— `PayHandlers_CreateQRPayment_OwnerAboveInt32Range_NotRefusedAsMistyped`（body 合法，
  过了校验才碰得到插件）与 `CallbackHandlers_WechatNotify_EventTypeObject_Answers400InsteadOfThrowing`
  （body 本该被形状守卫拒掉，但解引用排在校验之前）—— 在 `4f028d0` 上把进程打成 `0xC0000005`
  （连跑两次都复现，同一目录、同一二进制；PowerShell `Start-Process` 拿到原始退出码）。
  原因是 stdout 全缓冲、崩溃时不落盘，所以看起来像"没输出"而不是"崩了"；判据改用退出码而非日志文本才看清。
  崩溃点正是上一条的空指针解引用：用例断言的是"该按自身原因回 400/40003，或者合法地继续往下"，而代码在能答
  之前先死了 —— 按 memory 的口径，这次是**代码错、测试对**。修完后这两种进程状态都跑：无插件（在仓库根跑，
  `./config.json` 打不开）与有插件
  （在 exe 旁边跑，即 ctest 的 `WORKING_DIRECTORY`），各 18/18。
  **踩到的坑记下来**：`tests/main.cc` 只在能从当前目录读到 `config.json` 时才注册 PayPlugin，所以
  "插件不存在"这一族断言天生依赖启动目录；ctest 跑的那个状态里插件是**在**的，因此这类断言必须按状态分档
  （见 `pluginMissing()`），否则会在 CI 上红。另外本机短时间内连排起多个 drogon_test 进程会偶发
  `exit=127` + 0 字节输出的启动失败，不是用例失败 —— 判绿要按"有没有 `All tests passed` + 输出字节数"两条一起看。
- **本轮仍在本机之外才能验的部分**：`QrPaymentBookingTest.cc` 与 `RefundQueryTest.cc` 的用例要连
  PostgreSQL，本机那个角色连不上——直接跑是**挂住**而不是失败（探针跑了 4 分钟没有输出，只能杀掉），所以这两族
  按 §四/§六 已写明的本机边界（§八 记过同一件事）只在 CI 的 linux/windows 两条腿上验；本机这一侧能给的证据只有编译加上一条 `guarded()` 屏障与
  形状/插件守卫（18/18 × 两种进程状态）、`RouteRegistrationSmoke` 1/1、`HttpHeaders_*` 4/4、`HealthProbe_*` 3/3。

## 十一、第五轮复审补记（593813d 之后，本轮）

两个评审子代理对着 `593813d` 全量重读（包括第四轮那批守卫自己），另一个专查文档与契约。落地两条真缺陷；
一条 BLOCKER 用可执行证据证伪；一条修复建议核对后判定**守卫方向相反、不改**；另有契约与代码对不上的
六处更正。评审报告自己的机制描述也在对撞范围内：本轮一条 BLOCKER 就是这么证伪的（见下第三条）。

- **jsapi 下单把不确定的结果也关成了 `FAIL`（Critical，本轮修）**：第八、九节立的"只有可证明的拒绝才关闭
  该行"只接在 `createQRPayment` 上，`proceedCreatePayment` 的失败分支仍无条件把 `pay_payment` 写成 `FAIL`、
  订单写成 `FAILED` —— 而 `FAIL` 正是 `openAttemptsOfOrder()` 要遮蔽的那个状态。一次超时之后 `prepay_id`
  完全可能已在微信侧建起，用户真付了款，回调与对账就都找不到那行：钱落地而无人认领。现在两条建单路径共用
  同一个判据 `attemptCertainlyNotCreated()`（原 `qrAttemptCertainlyNotCreated`，`PaymentService.cc:277`），
  jsapi 分支只在判据为真时落库（`PaymentService.cc:714`），否则只 `LOG_WARN` 留在途（`967`）；对外应答
  仍是 `1002`/500，没变。判据本身已由 §十 那条 `PayPlugin_QrBooking_AnswerWithoutCodeUrlKeepsTheAttemptInFlight`
  覆盖，jsapi 这条**接线**要连 PostgreSQL，本机只能证明编译，等 CI 两条腿的证据 —— 本轮没有为它新增用例，
  也没有假装它被本机跑过。
- **alipay 回调在"能答之前"还留着一次空解引用（BLOCKER，本轮修）**：`593813d` 把"没有插件"折进了"没有验签
  客户端"分支，但验签通过之后仍写死 `plugin->paymentService()`（`4f028d0` 起如此）。`PayPlugin` 在、而
  `paymentService()` 为空是另一件事：`config.json` 里插件注册了、服务却没起来，这一状态原本会让整个网关在
  一次**已经验过签**的通知上崩溃 —— 也就是在唯一一次"确认收到"即将发出的地方。现在服务取用也判存在
  （`CallbackHandlers.cc:266-280`），回 `{"code":"FAIL"}` + HTTP 200：alipay 拿不到它要的 `success` 串，
  于是按自己的重试策略再投，不会把一笔没入账的款当成已送达。
- **一条 BLOCKER 被证伪（不改）**：评审称 `PayHandlers.cc:485` 对 int 成员调 `asString()` 会抛。对着 conan
  锁定的 jsoncpp **1.9.5** 现编了个探针跑（源码放在 gitignore 的构建目录里，跑完已删）：
  `asString()` 会**转换** int/unsigned/int64/bool/null（回 `"0"`、`"2200"`、`"true"`、`""`），只对 object 与
  array 抛 `Type is not convertible to string`；反过来 `asInt64()` 对字符串抛 `Value is not convertible to
  Int64.`、对 object 也抛。这条表同时说明 §九 那条"类型错的成员能打死进程"的论证只成立于 object/array
  与 `asInt64()` 读字符串两种形状 —— 形状守卫该防的仍然是它们，而不是"任何类型不符"。断言与代码对撞之后，
  本条按**代码对、评审报告错**结案（探针输出的完整表另记于 CHANGELOG 同一条目）。
- **一条建议核对后判定方向相反（不改）**：评审 2 指出快照定稿写命中 0 行时仍会 COMMIT + 确认收到，提议
  "回滚并回 FAIL"。核对着入账代码看：入账本身是 `UPDATE pay_payment ... WHERE status IN ('INIT',
  'PROCESSING') RETURNING 1`（`CallbackService.cc:1073-1082`），命中 0 行的那一支已经先 `rollback()` 再记
  审计、回 SUCCESS（`1102-1280`）。也就是说：幂等行消失只丢掉**去重证据**，钱该记的已经在事务里记完；
  此时再回滚反而把一笔已成立的入账退回去，全指望渠道还肯再投一次 —— 支付宝重试到上限就不来了，那才是真
  把钱晾在外面。故本条不改（守卫方向与代价都相反）。剩下那句诊断仍然成立：`dropUnfinalizedReservation()`
  只按 `idempotency_key` 匹配、不认是谁的预留，所以它确实可以删掉别人在途的那条；要治本得给预留加
  owner token，属独立改造，记在"仍未做"。
- **一个格式化陷阱（本轮踩到，记录判据）**：`CallbackService.cc` 的 `handlePaymentCallback` 整体是**一个**
  嵌套 lambda 表达式，`handleRefundCallback` 同。往它的任一层函数体里插进任何一条语句（哪怕两行），
  pinned clang-format 22 就会从 `dbClient_->newTransactionAsync(` 起把整段重排 —— 本机实测：插入 25 行
  → 2332 行 churn，四种换行写法（参数名另起一行、调用拆行、短参数名）全都躲不掉，且**逐字节验证只是空白
  差异**（`git diff -w` 仍只剩我那一处）。所以这里的规矩是：新逻辑写在命名空间作用域的小函数里
  （`dropUnfinalizedReservation`、`insertLedgerEntry` 就是这个形状），调用点能不插语句就不插；真要插，
  就单独提一个纯格式化 commit 把 churn 与语义分开，别混在一条 commit 里逼评审读两千行空白。
- **文档与契约对撞（本轮更正六处）**：
  - `openapi.yaml` 的 `ServiceUnavailable` 原写"三种情况都带业务码 1501"：auth 层无 key 回的是
    **明文字符串**、根本没有业务码（`src/handlers/AuthCheck.cc:158-165`）；"通道客户端没配"在两条下单路由上也各不相同
    （pay 回 `1002`、qrpay 回 `1005`，都是 500），只有退款路由回 `1501`（`RefundService.cc:1297,1395`）。
    已按三种故障重写。
  - 同文件 wechat notify 的 503 原 `$ref: CallbackAck`，而 `respondPluginUnavailable()` 回的是数值
    `{"code":1501}`（`CallbackAck.code` 是 `SUCCESS`/`FAIL` 串枚举）—— 改成 `ErrorResponse`，并在描述里
    写明"回绝不是确认，所以不用 ack 形状"。
  - `docs/api/pay-api-examples.md`：同一条"通道客户端未配置 → 1501/503"的错误断言出现在正文与 QR 拒因表
    里，已改为按路由区分；另给 `/api/pay/create` 补一段"只有可证明的拒绝才关闭该行"（本轮那条判据）——
    此前这条规则只在 QR 一节写过，读者会以为下单路由仍是无条件关闭。
  - 本文件 §十 的"九处统一回 1501/503"数错：实际八处（`PayHandlers.cc` 七处 + `CallbackHandlers.cc:107`），
    alipay 那两处回的是 `FAIL`；§九 的"14 条因此本机可跑"也补齐了真正的分档条件（插件按启动目录有无）。
  - §十 里对 `TECH_SPECS.md:238,257` 的行号引用**核对后保留**：两处断言确实落在 238 与 257（评审报告说的
    237/256-257 是错的），那次是文档对、代码错。
- **本轮本机证据**（比前几轮多跑了一遍非 DB 的单元测试族）：MSVC Release 构建（三个改动文件重编、日志零 warning）+ 七个
  Python 门禁全过 + `RequestBodyShapeTest.cc` 18/18 × 两种进程状态（有插件/无插件，两种状态各用一条
  "启动日志里有没有 `Initializing PayPlugin`" 的判据确认过，不是靠目录猜的）+ 穿过屏障的路由族
  `RouteRegistrationSmoke` 1/1、`HttpHeaders_*` 4/4、`HealthProbe_*` 3/3 + 非 DB 单元/通道客户端族 55 条
  （`AuthCheck_*` 5、`ConfigLoader_*` 5、`ControllerMetrics_*` 3、`OnceCallback_*` 5、`PayAuthMetrics_*` 1、
  `PayUtils_*` 7、`StartupValidator_*` 7、`PayErrorCategory_*` 4、`WechatPayClient_*` 18）全绿。
  其中 `AuthCheck_OptionsPreflightPassesThrough` 在批量跑里出现一次 90 秒不起进程、单独 0.3 秒即绿 —— 就是
  §十 记的那个启动产物，判绿要把它单独重跑一次再计数。jsapi 判据接线与 alipay 服务为空分支仍只有编译证据
  （前者要 PostgreSQL，后者的状态在测试进程里造不出来 —— 起得来就一定带着 `paymentService`）。
- **仍未做**：幂等预留加 owner token（上一条）；`guarded()` 的故障分支直达用例（§九）；jsapi/alipay 两条
  新分支的用例；服务层残余 `std::errc::*` → 业务码（§七）；`AlipayChannel.cc:428` 毫秒当秒、
  `downloadCertificates` 裸 `this`（§六）；`CallbackService` 两条回调的嵌套压平（本轮实测它是格式化陷阱）。

