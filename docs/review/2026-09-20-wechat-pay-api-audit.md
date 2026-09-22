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

## 十二、第六轮复审补记（e9999e6 之后，本轮）

三个评审子代理并行对 `e9999e6` 起：加解密/证书、退款生命周期、并发/幂等。逐条落回代码核对守卫方向，
refuted 也留证据。本轮落地八处（含 §九~§十一 遗留的 SSRF 校验器补全），证伪两条安全断言，判定一处方向相反。

- **退款错误码 1409/409（本轮修）**：退款在途冲突原回笼统码，`openapi.yaml:833` 承诺的 409 一直是空头。
  `RefundService.cc:331-333`、`1075-1077` 改回业务码 `1409`（HTTP 409），`RefundQueryTest.cc:425,2617,3531`
  同步断言 1409 —— 契约第一次与代码相符。
- **退款生命周期两处竞态：过早 `REFUNDED` + 无守卫状态写（Critical，本轮修）**：
  `updateRefundWithError`/`updateRefundWithSuccess` 原以 `findOne` 再 `update(model)` 落库，会把脏列里的
  `status` 一并写回，且一笔退款成功就把父订单无条件推到 `REFUNDED`。改成 `Mapper::updateBy(...)` 的
  CAS：`updateRefundWithError` 只在 `status IN ('REFUND_INIT','REFUNDING')` 时写 `REFUND_FAIL`；
  `updateRefundWithSuccess` 同守卫写 `REFUND_SUCCESS`，且**只有** `settleOrder()` 命中 `REFUND_SUCCESS`
  时、再以 `WHERE status='PAID'` 守卫把订单改 `REFUNDED`（`RefundService.cc:1544-1848`）。通知侧
  （`CallbackService.cc:2712-2802`）补一笔事务内、`PAID` 守卫的等价订单写，保证两条入账路径都不越过先到的
  快速通知。
- **对账入账的金额校验（本轮修）**：`queryOrder` 走渠道实时同步时，把渠道答案金额与本地 `pay_payment.amount`
  对撞后才允许落 `PAID`。新增命名空间函数 `reconcileAmountProblem(answerTotalFen, paymentAmount)`
  （`PaymentService.cc`）：微信读 `amount.total`（分）、支付宝读 `total_amount`（元，经 `parseAmountToFen`），
  仅在 `orderStatus=='PAID'` 分支前校验，不一致或渠道无金额则 `LOG_ERROR` + `callback("")`，
  让 `queryOrder` 报回未变的库状态。注意 `pay_payment` 无 `currency` 列（在 `pay_order` 上），故对账门只比金额，
  币种校验仍留在通知路径。
- **SSRF 校验器补全（本轮修，含 `#` 那个洞）**：`validateNotifyUrl` 补四类 —— userinfo 用**最后一个** `@`
  剥离、非规范 IPv4 拼法（`127.1`/`2130706433`/`0x7f.1`/`010.1.1.1` 等一律经 `isNumericAddressShape` 拒）、
  IPv6 白名单（`isGlobalUnicastIpv6` 只放 `2000::/3`）、host 终止符扩到 `/:?#`。最后一条是我自己新测试
  `PayUtilsTest.cc:176` 抓出来的：`#` 不截断 host 时 `http://127.0.0.1#x.com` 读成非法 host 而放行、客户端却连
  127.0.0.1 —— 这个洞 HEAD 也有。补了正向对照（`host42.example.com`、`127.0.0.1@pub.example`、路径里的 `@`、
  `https://pub.example.com/#cb`）防守卫过宽。
- **证书下载节流：安全断言证伪、真实小缺陷修（本轮）**：评审称未知序列号的伪造回调能"饿死"证书轮换。
  核对 `downloadCertificates` 与 `/v3/certificates`：**该端点返回整套平台证书、循环里把所有通过校验的都装进
  缓存，与触发它的序列号无关**。所以攻击者顶多把签名 GET 限到每周期一次，拦不住真轮换证书被装上 —— 节流
  本身就是防"每通知一次外呼"的限速器，让不可信流量去 stamp 它是**设计如此**。故"饿死=安全问题"证伪。
  但确有一处真缺陷：原 `lastCertDownloadAt_ = now` 在 `buildAuthorizationHeader` 之前，签名/配置失败（根本没
  发请求）也烧掉整个窗口、饿死下次真刷新。已把建头移到前面、只有真要发请求时才在锁内 stamp
  （`WechatChannel.cc:616`），check+stamp 仍原子。
- **证书回调裸 `this` 的生命周期缺陷（§六 遗留，本轮修）**：`downloadCertificates` 的异步 HTTP 完成回调捕获裸
  `this` 去调 `decryptResource`/`setPlatformCert`，但没有任何东西保证客户端活到响应回来。生产侧所有持有者都经
  `shared_ptr`（注册表 `make_shared`、服务 `dynamic_pointer_cast`），故进程关停时若一次刷新仍在途，迟到响应会解引用
  悬垂指针 —— 就是 §六 起一直挂"仍未做"的那条。现让类继承 `enable_shared_from_this`、回调解 `weak_ptr` 并在碰任何成员
  前 `lock()`，客户端已亡则丢弃响应。栈构造的测试路径（`WechatPayClient_DownloadCertificates`）在建签名头时即同步失败、
  在异步边界之前，故不受影响（本机实测仍 3 断言绿）。真正的竞态要"活响应 + 关停"同时发生，本机只有编译证据。
- **SPI 三参 `verifyCallback` 对无 `resource` 的体应拒绝（本轮修，接口加固）**：`:1114` 那版过去对缺
  `resource` 对象的已验签体仍 `return true`、给出空 `order_no` 与 null payload。微信 V3 每条通知必带加密
  `resource`，缺它即畸形 —— 现改为 `else { error=...; return false; }`（`WechatChannel.cc` 资源解密段）。
  **该三参版在生产回调链无调用者**（实链走六参纯验签版 `CallbackService.cc:3294`），故本条只有编译证据。
- **并发评审 F2：预留竞态的幂等 ACK 补守卫（本轮修，两处）**：`CallbackService.cc:710-721`（支付）与其退款孪生
  `:2405-2416`，在 `ON CONFLICT DO NOTHING RETURNING` 抢注失败（0 行）时**无条件回 SUCCESS**，却没读抢占者的
  `response_snapshot`。而读路径（`:540`、`:2242`）恰恰规定"快照为 NULL = 在途，不能当已处理，要 drop+回 FAIL 重试"。
  两条路径对同一语义给出相反答案，就是 F2：抢占者若在其业务事务提交前崩溃，预留行永久停在 NULL，而输家已把
  2xx 回给了微信 → 微信停投、结算搁浅。**修法与读路径对齐**：抢注失败改回 `FAIL`（`makePayError(1400,...)`），
  把"完成 vs 在途"的裁决让给下一次走读路径的投递（它能正确区分）。输家不碰业务逻辑，无重复扣款风险。
  `openapi.yaml` wechat notify 描述补了这一句并发语义。
- **判定方向相反、不改**：F2 里"`dropUnfinalizedReservation` 只按 key 匹配会删别人在途预留"的根因仍需要 owner
  token 才能治本，与 §十一 同条一致，记入"仍未做"，本轮不动。
- **本轮本机证据**：MSVC Release 重建（`WechatChannel.cc`+`CallbackService.cc` 重编、`clang-format --fix` 后四文件
  再编，日志 0 warning）+ 七门禁全过（clang-format check + 六个 `check_*`）+ `PayUtils_ValidateNotifyUrl` 42 断言全绿。
  **证据边界（诚实标注）**：F2 两处改动、SPI 三参严格化、证书节流重排都只有编译证据 —— 预留竞态要两条并发投递
  打同一 key 且连 PostgreSQL，是 CI-only。SSRF 的"红"证据不是真跑出来的：分类器拦了那次临时回退（会撤销 SSRF 修复），
  所以红是靠读 `git show HEAD:` blob 逐输入推演、绿才是真跑；证书重排的旁证是构建日志里那条"配置缺失 warm-up 在
  节流之前返回错误"，恰好演示了新顺序。jsapi/alipay 对账接线、部分退款仍把订单在第一笔成功退款时标 `REFUNDED`
  （既有产品语义，本轮未改，记为已知缺口而非静默重设计）。
- **仍未做（承接 §十一）**：幂等预留 owner token；jsapi/alipay 新分支与 F2/SPI
  的用例；`CallbackService` 两条回调嵌套压平。（`downloadCertificates` 裸 `this` 本轮已修，见上。）

## 十三、第七轮复核（无新增缺陷，三条候选假设均以证据证伪）

本轮按"refuted 也要留证据"的纪律，对三个新的可疑方向逐一撞代码，结论全部为**已防护、不改**：

- **候选一：V3 回调缺时间戳重放窗口 → 证伪**。支付与退款两条回调链都在验签之后、进业务事务之前调用
  `isTimestampFresh`（`CallbackService.cc:191`，`kMaxSkewSeconds=300`），支付链 `:315`、退款链 `:2035`
  各一处；随后 `checkNonce`（`CallbackService.cc:218`）以 Redis `SET NX EX 360`（`:262`）做一次性 nonce。
  重放要同时越过
  5 分钟时戳窗与 360 秒 nonce 缓存，两道门都在，且都在贵操作（DB 事务）之前。无需新增。
- **候选二：累计超额退款缺总量对撞 → 证伪**。退款发起前对同一 `order_no` 做行锁 `SELECT SUM`
  （`RefundService.cc:1032`），`status IN ('REFUND_INIT','REFUNDING','REFUND_SUCCESS')` 显式**排除**
  `REFUND_FAIL`（失败的不占额度），再 `:1069` `if (refundedFen + refundFen > totalFen)` 回 `1409`。
  单笔 `refundFen<=0 || >totalFen` 另有 `:683` 前置门。累计口径与状态集合都对，无需新增。
- **候选三：对账同步绕过金额门 → 证伪**。金额对撞放在共享函数
  `syncOrderStatusFromWechat`/`syncOrderStatusFromAlipay` 内部（`PaymentService.cc:2403`/`:2884`，
  `orderStatus=='PAID'` 分支前），而非只放在 `queryOrder`。`ReconciliationService` 定时扫
  PAYING/CREATED 后调的正是这两个共享函数（`ReconciliationService.cc:201`/`:270`），故对账入账**继承**同一道
  金额门，无旁路。无需新增。

结论：微信 V3 全流程（建单、回调、查询、退款、对账、证书）在本轮可查证据下已系统加固；本轮未发现同类可实现缺陷。
CI-only 分支（F2 预留竞态、SPI 三参严格化、证书下载并发生命周期、对账接线）仍待 `_build-test.yml` 三平台绿。

## 十四、第八轮：出站请求构造对撞官方参数表（一缺陷修复 + 一断言证伪）

本轮换了视角：前七轮都审**入站回调与状态机**，这轮逐字段审**出站请求**与官方 V3 参数表的符合性。

- **建单字段前置校验缺失（P2 功能缺陷，本轮修）**：官方 Native 下单参数表（原句）——`out_trade_no`
  "要求6-32个字符内，只能是数字、大小写字母_-|\* 且在同一个商户号下唯一"，`description` 必填、≤127 字符。
  代码里建单入口对 `order_no` **零校验**（`PaymentService.cc` createPayment 与 QR 分支只查空；handler 的
  `FieldType::String` 只查 JSON 类型），`attach`/`description` 也无上限。后果链：越窗单号 → 本地订单已落库 →
  微信侧建单/查询永远 400/ORDERNOTEXIST → 订单永滞留对账扫表；>64 字符的单号连退款入口都进不去
  （`RefundService.cc:407`）。修法：`PayUtils` 新增纯函数 `validateWechatOrderFields`
  （out_trade_no 6-32 + 字符集；description 非空 ≤127 **码点**、attach ≤128 码点——官方原句按"字符"，
  UTF-8 码点计数不误伤中文单），两处 wechat 分支在建单前调用并回 400/1001、释放幂等预留
  （`PaymentService.cc:476` create、`:1495` QR；函数在 `PayUtils.cc:539`）。
  新单测 `PayUtils_ValidateWechatOrderFields` 17 断言（含中文码点正向对照）。
  **测试夹具随守卫收口**：`QrPaymentBookingTest` 的 `ord_qr_<uuid>`（43 字符，本就在窗外）与
  `CreatePaymentIntegrationTest` 依赖**空 order_no 落库**的两个 service 级用例，改为合规唯一单号生成器；
  `RequestBodyShapeTest` 的 46 字符单号同改。openapi：`CreatePaymentRequest.order_no` 描述写明窗口，
  wechat 响应示例换成 32 字符无连字符单号，QR 端点描述补前置校验语义。
- **"退款缺必填 `req_from`" 证伪**：直连商户「申请退款」现行参数表**没有** `req_from`、
  `user_define_refund` 字段（服务商版同）；`funds_account` 枚举 = `AVAILABLE`/`UNSETTLED`，与
  `RefundService.cc:432-436` 的校验逐字一致；`reason` ≤80 与官方 80 一致（按字节，代码
  `:394` 用 `size()` 即字节数，方向与官方一致）。`payload`（`RefundService.cc:1408-1425`）无需补字段。
- **本轮本机证据与边界（诚实标注）**：MSVC Release 重建 0 warning；`PayUtils_ValidateWechatOrderFields`
  17 断言 + 相邻 `PayUtils_*`/`WechatPayClient_SetPlatformCert_*`/`DownloadCertificates` 全绿。
  `WechatPayClient_QueryTransaction_ReportsHttpErrorAsFailure` 与对照组 `HealthProbe_LivenessEndpoint`
  在 `-r` 单跑时同红（"Bad server address"）：该族依赖测试套件自带 listener，单跑不满足前置 —— 环境约束、
  非本轮回归，CI 全量跑为裁决。守卫对建单入口的行为改变（400 提前）要 QR/shape 集成族连 PostgreSQL 验证，
  属 CI-only。
- **仍未做（承接 §十二/§十三，新增两条）**：`time_expire` 的 RFC3339 格式与"≤15 天"前置校验（现状：
  非法格式照发给渠道，建单必败、订单滞留 —— 与本轮修的同一后果类）；`goods_detail` 透传字段未实现
  （官方可选，缺它不畸形，不算缺陷）。

## 十五、第九轮：幂等预留 owner token（F2 根因治本，一缺陷修复）

§十一 记下的"要治本得给预留加 owner token"、§十二 判定"方向相反、本轮不动"的那条，本轮落地。

- **缺陷链复盘**：回调链两阶段预留（INSERT 空快照 → 业务事务提交后定稿）从不记录**持有者是谁**。
  读路径的 `dropUnfinalizedReservation` 只能按 `key + snapshot IS NULL` 匹配 —— 恢复"崩溃持有者留的死预留"
  必须允许第三方清行，所以删除处加不了 owner 校验；但它因此能删掉**活着但慢**的投递 A 的在途预留。
  A 不知情继续跑：定稿是纯 key 匹配的 UPDATE，结局二皆坏 —— 行已被重抢（C 预留了新行）则 A 把快照
  盖到**不属于自己的行**上；行被删净则 A 提交入账、回给微信 SUCCESS，却**没有任何幂等证据写回**，
  后续重复投递只能靠 CAS 分支重新裁决。
- **治本落点（本轮修）**：`sql/005_pay_idempotency_owner_token.sql` 给 `pay_idempotency` 加可空
  `owner_token VARCHAR(64)`；两条回调链的
  预留 INSERT 写入每次投递新造的随机 token（`CallbackService.cc:764`、`:2637`，去连字符 UUID=32 字符）；
  三处定稿收敛到文件级 helper `finalizeReservation`（`:131`），守卫形态为
  `UPDATE ... SET response_snapshot=$1 WHERE idempotency_key=$2 AND owner_token=$3 RETURNING ...`，
  按返回行数裁决：
  - 事务内两处（支付 PAID 分支 `:1402`、退款链 `:3184`）：0 行 = 主权已失 → **回滚整个投递 + FAIL/1400**
    —— 不提交自己已不持有的证据，入账随事务一起撤销，渠道重试后由下一位投递读到真实状态；
  - 支付 else 分支是**提交后**在 `dbClient_` 上定稿（`:1723`）：事务已 COMMIT，入账即事实，此时 0 行只
    说明快照没盖上 → `LOG_WARN` + 照常 SUCCESS（证据缺失至多换一轮重试，重复投递走读路径 + CAS 兜底）。
- **定稿为何允许裸 SQL**：`UPDATE ... RETURNING` 本就是 `.claude/rules/db-operations.md` 豁免清单里的
  形态（与预留 INSERT 同款确定性行数判定）；且 ORM 模型由 `drogon_ctl` 生成、本轮禁改 —— 新列对模型
  不可见，读路径 `findOne` 与 create 路径均不受影响（`CallbackService.cc:59` 的删注释同步说明"删除处
  不加 owner、接管由定稿守卫中和"的方向）。
- **测试与证据边界（诚实标注）**：39 处夹具的 `CREATE TABLE pay_idempotency` 统一加列（迁移与夹具双路
  径建出的表都有该列）；支付回调"陈旧预留 drop+重试"用例补正向断言 —— 胜者的行必须**同时**带已定稿
  快照与非空 owner_token（`WechatCallbackIntegrationTest.cc` 的
  `PayPlugin_WechatCallback_UnfinalizedReservationIsReprocessedOnRetry` 用例尾部），快照有、token 空 = 走了无守卫
  的旧路径，两者皆空 = 提交了却没写证据。真正的"删活预留 → 定稿 0 行 → 回滚 FAIL"竞态需要两条并发
  投递打同一个 PostgreSQL，本机不可判定：**本轮为编译证据**（MSVC Release 重建 0 warning，token 透传
  靠编译器逐层捕获校验）+ 七门禁全绿；接管裁决分支连 CI 数据库族验证，仍属 CI-only。
- **边界（不在本轮范围，注明理由）**：下单/退款创建链在 `IdempotencyService` 自己的预留（不同 key 域）
  不覆盖 —— 那里没有"读路径第三方清行"的对手机制，`clearReservation` 同样限定 `snapshot IS NULL`，且
  失败面由渠道按 `out_trade_no`/`out_refund_no` 去重 + 微信重试自愈；留作后续统一改造。
- **仍未做（承接）**：`goods_detail` 透传（§十四 那条不变）；`time_expire` 前置校验已在 §十六 落地；
  上文创建路径预留的统一 owner 化；`CallbackService` 两条回调嵌套压平（§十二 起挂着）。



## 十六、第十轮：`time_expire` 前置校验与落库解析（两侧读数相反）

- **缺陷链（同一个字段，两处方向相反的错读）**：
  1. 出站侧：建单把 `request.timeExpire` 原样放进 V3 payload，格式从不校验 —— 空格形
     `2026-05-20 13:29:35` 本地解析成功、渠道判 400，而 `pay_order` 行此时已提交，于是
     留下一条渠道侧根本不存在的交易等着对账清扫（与第八轮同一后果类）。
  2. 落库侧：`trantor::Date::fromDbStringLocal` 以**空格**切分（trantor `Date.cc:293`），
     RFC3339 串于是只剩日期段被切出，日字段变成 `20T13:29:35+08:00`，而 `std::stol` 在
     部分转换成功时不抛（标准语义）—— `expire_at` 落成本地零点、时分秒整段静默丢失，连
     原本的 `LOG_WARN` 都不会触发。**即：合法值被截断，非法值被接受。**
- **官方口径修正（本轮核验，纠正 §十五 记录）**：§十五 写的"≤15 天"出自 H5 下单页；本仓
  两条微信入口都打 `/v3/pay/transactions/native`（`WechatChannel.cc:1121`、`:1127` 收敛到
  同一个 `createTransactionNative`），适用 Native 页原文"支付结束时间需遵循 rfc3339 标准
  格式……需在下单时间的 7 天以内，如超过 7 天，系统将自动调整"。故守卫按 **7 天**，并把
  "系统自动调整"这个调用方观测不到的静默改期改为本地拒绝；字段长度 string(64) 同源。
- **落点**：`pay::utils::parseRfc3339`（`PayUtils.cc:642`）严格 RFC3339 → UTC 秒；
  `validateTimeExpire`（`:702`）在其上叠加"必须晚于当前"与微信 7 天窗（按 channel 分向，
  非微信渠道不受该窗约束）；建单在**写库之前** fail-fast（`PaymentService.cc:497`，1001 +
  `clearReservation`，与第八轮字段守卫同位同处置——锚点为第十八轮重锚，原 :477/:625 已漂），`expire_at` 用同一读数落库（`:650`）。
- **为何不复用 trantor 解析器（实测，不是推断）**：`fromISOString` 把机器时区叠在串自己声明的
  偏移之上，首版单测里"格式化为 UTC 再比对"直接读出整 8 小时的偏差与 1 秒漂移。故日历与偏移
  自算（`daysFromCivil`，`:589`），测试侧的 RFC3339 生成器同样自写 —— 被测与辅助不同源，
  否则helper 的错会替守卫的错背书。
- **本轮自查出的自身缺陷（留证据）**：首版 `daysFromCivil` 的月份三目写反
  （`month + (month>2 ? 9 : 0) - 3`，1/2 月还会 unsigned 回绕），epoch 断言当场红
  （`1970-01-01T00:00:00Z` 读出 `74217003148800`）；改为 `month + (month > 2 ? -3 : 9)` 后
  `1970-01-01 → 0`、`2000-01-01 → 946684800`、`+08:00` 为减偏移、闰日
  `2024-02-29T23:59:59Z → 1709251199` 全绿。守卫自己也被测试推翻过一次，正是"先跑红再改"的价值。
- **测试与证据边界（诚实标注）**：`PayUtilsTest.cc:305` 39 断言本机全绿，含正向对照（合法值
  必须放行、7 天窗只作用于微信）与旧解析钉证（`fromDbStringLocal("…T…")` 等于
  `fromDbStringLocal("2099-05-20")`，与时区无关，只暴露截断）；
  `CreatePaymentIntegrationTest.cc:419` 的"拒绝前不落库 / 合法值过守卫"连 PostgreSQL，本机无
  库（连接按 1 秒重试不止，非代码缺陷）→ **CI-only**。七门禁全绿，69 文件格式干净，MSVC
  Release 重建 0 warning。
- **顺带核实（不改，注明理由）**：HTTP 面 `PayHandlers.cc:202` 起从未给 `request.timeExpire`
  与 `request.attach` 赋值，`openapi.yaml` 也未声明 `time_expire` —— 该字段目前只对直接使用
  Service API 的调用方可达。本轮按"修已存在的字段"处理；是否把 `time_expire` 提升为 HTTP
  契约字段属产品决定，列入下轮候选。
- **仍未做（承接）**：`goods_detail` 透传；创建路径预留的统一 owner 化；`CallbackService` 回调
  嵌套压平；`time_expire` 是否进 HTTP 契约待拍板。

## 十七、第十一轮：`trade_state=REFUND` 的映射方向（把已收的钱记成没收到）

- **缺陷链（一处映射，三条下游后果）**：`pay::utils::mapTradeState` 把 `REFUND` 与
  `CLOSED`/`REVOKED` 写在同一分支，回答"这笔支付失败了"。该函数有两个调用点，都在钱已经
  动过的路径上：通知落库（`CallbackService.cc:1065`）与查单对账
  （`PaymentService.cc:2418`）。于是回调丢失后由查单兜底时：payment 行 CAS 成 `FAIL`、
  `PAYMENT` 流水一分不写（两处补流水的判据都只认 `PAID`：`PaymentService.cc:2553`、
  `:2724`，通知侧 `CallbackService.cc:1363`）、order 落成 `CLOSED` 像是过期未付。渠道账单
  上却同时存在这笔支付和它的退款，本地账面与渠道侧永久对不上——正是第七轮"收款事实不能被
  推断覆盖"那类问题的反面版本。
- **官方口径（本轮逐字取证）**：`https://pay.weixin.qq.com/wiki/doc/apiv3/apis/chapter3_4_2.shtml`
  （微信支付订单号查询订单，`GET /v3/pay/transactions/id/{transaction_id}`）原文枚举：
  "SUCCESS：支付成功 REFUND：转入退款 NOTPAY：未支付 CLOSED：已关闭 REVOKED：已撤销（仅付款码
  支付会返回）USERPAYING：用户支付中（仅付款码支付会返回）PAYERROR：支付失败（仅付款码支付会返
  回）"。两点据此成立：`REFUND` 与"支付失败"（`PAYERROR`）是不同状态，它说的是**已经收款**的
  交易被转入退款；本仓 `FAILED`/`FAIL` 默认分支覆盖 `PAYERROR` 与未知值，方向不变。同页还只把
  业务流转细节指向"开发指引-订单状态流转图"，未给处理步骤，故本仓契约以
  `examples/pay-server/openapi.yaml:958-981`（`OrderStatus` 含 `REFUNDED`，`CLOSED`/`FAILED`
  来自"closed/revoked/expired trade"，`REFUNDED` 来自"completed refund"）为准。取证的另一半
  限制也记录：`pay.weixin.qq.com/docs/...` 域下的页面本轮仍被网关以
  `FORBIDDEN / code 10605` 拒绝，只有 `wiki/doc/apiv3` 老路径可达（与既有踩坑记录一致）。
- **落点**：`PayUtils.cc:435` 拆成 `REFUND → order=REFUNDED / payment=SUCCESS`，
  `CLOSED`/`REVOKED → CLOSED / FAIL` 保持原样；`PayUtils.h` 的声明处补上"两个答案是独立的：
  一笔交易可以同时收了钱、又不再是已支付"。查单侧"这条答案是不是在说本笔支付"的金额证明
  扩到 `REFUNDED`（`PaymentService.cc:2481`），两处补 `PAYMENT` 流水的判据同步
  （`:2553`、`:2724`），通知侧同一判据（`CallbackService.cc:1363`）。支付宝孪生分支
  （`PaymentService.cc:2966`/`:3037`/`:3207`）本轮**故意不动**：它的状态词表与语义不同源，
  不在无证据的情况下跟着改。
- **红验证（先跑红，再改）**：修复前 `PayUtils_MapTradeState` 报
  `16 | 14 passed | 2 failed`，两处展开正是 `"CLOSED" == "REFUNDED"` 与
  `"FAIL" == "SUCCESS"`；修复后 `All tests passed (16 assertions in 1 tests cases)`，
  16 条断言覆盖官方 7 个枚举 + 未知值 + 方向对照。
- **为什么既有测试没抓到（答案比"没覆盖"更难看：它把错方向钉死了）**：
  `tests/integration/WechatCallbackIntegrationTest.cc:2000`
  的 `PayPlugin_WechatCallback_TransactionRefundState` 用 `trade_state=REFUND`、
  金额与订单一致（`9.99` ↔ `total=999`），却断言 payment `FAIL` + order `CLOSED` +
  `pay_ledger` 0 行——把一个应当留痕的收款写成无痕。本轮把它改成正向断言
  （`SUCCESS` / `REFUNDED` / ledger 1 行）并写明理由；同族 `..._TransactionClosed`（`:1542`）、
  `..._TransactionRevoked`（`:1771`）仍断言 `FAIL`/`CLOSED`，方向对照因此不是只靠新用例撑着的。
  另一处原因：`QueryOrder_*` 家族用 `setTestClients(realWechatClient, ...)`，真实 client 卡在配置
  校验，永远走 `wechat_query_error` 分支，名字里的"Success"从未进入映射，所以查单侧的方向错
  向多年无人触碰（本轮新增用例改用 `setTestChannels` 的 SPI 注入，才第一次真正跑通这条链）。
- **新增证据（`tests/integration/QueryOrderTest.cc:901` 起）**：`QueryStubChannel` 注入渠道答案，
  `settleFromChannelAnswer` 记账→查单→回读 `pay_order`/`pay_payment`/`pay_ledger` 计数。两个用例：
  `PayPlugin_QueryOrder_WechatRefundKeepsTheCollectedPayment`（REFUND 收口 + CLOSED 对照）与
  `PayPlugin_QueryOrder_WechatRefundWithForeignAmountSettlesNothing`（同一状态但 `total=5000` ≠
  本笔 `49.90`：不得凭状态落账，订单留 `PAYING`、响应里的 status 保持库值 `PAYING`、流水 0 行）。
  判据从"状态"扩到"状态 + 金额证明"，避免修复把另一个方向的口子打开。
- **证据边界（诚实标注）**：本机无 PostgreSQL，`makeQueryTestClient()` 直接返回 null，
  两个新用例在本地以 `REQUIRE(client != nullptr)` 立即红（这是比既有家族"连不上库挂死"
  更好的失败形态，但不算验证）→ **CI-only**。本轮能给出的本机证据只有：MSVC Release 全量重建
  0 warning、`PayUtils_MapTradeState` 16 断言绿、七门禁全绿、69 文件格式干净。
  因此本轮不宣称完成，判据仍是 `_build-test.yml` 三平台绿。
- **顺带核实、判定为不改（附理由）**：payment 已是 `SUCCESS` 时后到的交易通知会走
  `CallbackService.cc:891` 的"already final"短路（回滚 + 回 `SUCCESS`），order 因而停在
  `PAID`。这不是本轮引入的洞：退款结论另有专责路径 —— 退款通知按 `REFUND.` 前缀路由
  （`CallbackService.cc:2307`），并只把 `status='PAID'` 的订单 CAS 成 `REFUNDED`
  （`:3064-3081`），交易通知不承担退款结论。本轮改动之后两条路径的关系也已核对：先到
  `trade_state=REFUND` 的交易通知自己就把 order 落到 `REFUNDED`，随后那条退款通知的 CAS
  因 WHERE 条件不再命中而空转，两次都不会重复记账：入账 `PAYMENT` 流水由交易侧写
  （`CallbackService.cc:1370`），出账 `REFUND` 流水由退款侧写（`:3464`），两条 `entry_type`
  不同、各自一条。若反过来允许"最终态也向后修正 order"，等于把 CAS 的幂等
  保护换成一条可被重放通知改写的路径，风险大于收益。
- **发现但仍未做（下一批候选，按风险排序）**：
  1. **主动关单缺位**：`libs/drogon-pay/src` 全文无 `.../close` 端点调用（`WechatChannel.cc`
     里唯一含 "close" 的行是解密注释 `:1174`），过期只能等微信侧自动关单；对"用户放弃但订单
     还开着"的回收路径不完整。
  2. 部分退款是否应过早把 order 记成 `REFUNDED`（`RefundService.cc:1675`）需按官方"未全额退款
     时交易仍为 SUCCESS"再对一次口径。
  3. 创建路径预留的统一 owner 化、`goods_detail` 透传、`CallbackService` 回调嵌套压平、
     `time_expire` 是否进 HTTP 契约（第十轮遗留，待拍板）。

## 十八、第十二轮：单笔部分退款不得把整单记成 `REFUNDED`（关闭 §十七 候选 2）

- **官方依据（本轮实测原文）**：
  - 申请退款参数页（`https://pay.weixin.qq.com/doc/v3/merchant/4012587971`，本轮 WebFetch 核验）：
    `amount.total` 为"【原订单金额】原支付交易的订单总金额，单位为分，只能为整数"；退款能力描述为
    "商户可以通过申请退款接口将支付款**全额或部分**还给用户"。两句合起来即：一笔通知里的
    `total` 永远只是订单原总额，它相对单笔 `refund` 的大小关系**证明不了"已退完"**——是否退完
    是"该订单全部已退成功退款之和"对总额的问题，只能由库里的账回答。
  - `trade_state=REFUND` 对部分退款也返回的口径沿引 §十七（查询页枚举 + 第十一轮取证）；本轮
    据此把"REFUND 即整单退完"的推断继续留在未门控路径（见下"仍未门控"），不越权收口。
  - 老路径 `wiki/doc/apiv3/apis/chapter3_4_5.shtml` 本轮实测仍返回"支付成功回调通知"页而非
    退款参数页，与取证地图的踩坑记录一致，未采用。
- **缺陷（两处写点）**：`RefundService.cc` 的 `updateRefundWithSuccess`（现 `:1594`）与
  `CallbackService.cc` 的退款通知落库（CAS 现 `:3227`）此前只看"这一笔退款是否 `REFUND_SUCCESS`"，
  命中即写 `order.status='REFUNDED'`。10.00 的订单退成功 3.00 后，所有以 `REFUNDED` 为键的下游
  （会员回收、对账、客服口径）都在按"整款已退"行动，而 7.00 还在商户手里。这与第十一轮修的
  是同一枚硬币的反面：那次是"把收过的钱记成没收"，这次是"把没收齐的退款记成收齐"。
- **修复（判据下沉为纯函数 + 两处门控）**：
  - `pay::utils::refundsCoverOrderAmount`（`PayUtils.cc:383`）：`orderTotalFen > 0 &&
    settledRefundFen >= orderTotalFen`；总额或和被测为 0/解析失败时一律 false，订单保持原状
    ——没人量过的钱不能当证据。
  - 服务路径：`settleOrder`（`RefundService.cc:1684`）在写 `REFUNDED` 前用聚合 SUM（raw-SQL
    豁免第 3 条，Mapper 表达不了 SUM）读该订单 `REFUND_SUCCESS` 行之和，覆盖总额才走
    `writeRefundedOrder` 的 `PAID→REFUNDED` CAS（`:1671`），否则订单不动、退款记录照常成功返回。
    渠道调用回调补传 `orderTotalFen`（支付宝/微信两条 lambda 同改），判据与渠道无关。
  - 通知路径：SUM 读数放在退款通知自己的事务里、payload 更新回调之后（`CallbackService.cc:3120`
    判、`:3227` 写），因此能看到本事务刚落的这一行；语句排在 `insertCallbackAndFinish` 的显式
    `COMMIT` 之前，不改变"先落库再回渠道 ACK"的既有顺序。重复计数不可能：退款行 CAS 以
    `status IN ('REFUND_INIT','REFUNDING')` 为条件，已被并发写终态时 0 行早退。
- **测试**：
  - `PayUtils_RefundsCoverOrderAmount`（9 断言：恰覆盖/超额为真；999/1000、1/1000、0/1000
    为假；0/0、1000/0、负数两侧均假）。本机绿（`All tests passed (9 assertions in 1 tests
    cases)`）。
  - 服务路径 CI-only 用例（`tests/integration/RefundQueryTest.cc`）：`RefundStubChannel` 经
    `setTestChannels` SPI 注入，种"10.00 已 PAID + 4.00 已退成功"，两例
    `PayPlugin_Refund_PartialRefundKeepsOrderPaid`（再退 3.00：订单仍 `PAID`、已退成功 2 行）与
    `PayPlugin_Refund_CumulativeRefundsSettleOrder`（再退 6.00：订单 `REFUNDED`）。成对互为对照，
    防止"门永远不放行"混过只测拒绝方向的断言。
  - 通知路径同族两例（`tests/integration/WechatCallbackIntegrationTest.cc`：
    `PayPlugin_WechatCallback_PartialRefundKeepsOrderPaid` /
    `PayPlugin_WechatCallback_CumulativeRefundsSettleOrder`），并给既有
    `PayPlugin_WechatCallback_RefundSuccess` 补了它此前缺失的 order 断言（全额通知后必须
    `REFUNDED`）——没有这条正向对照，门可以永远拒写而全绿。
  - 证据边界同 §十七：本机无 PostgreSQL，四个新用例本地以 `REQUIRE(client != nullptr)` 快速红，
    不算验证；本机可给出的证据为 MSVC Release 全量重建 0 error、纯函数单测绿、七门禁全绿、
    69 文件格式干净。判据仍是 `_build-test.yml` 三平台绿。
- **已知并刻意接受的保守失败模式**：两笔部分退款并发落账时，各自事务里的 SUM 可能都读不到对方
  未提交的行 → 双双少算 → 订单停在 `PAID`。方向偏保守（宁可少记退完），但恢复路径目前偏弱：
  `syncRefundStatusFromWechat`（`RefundService.cc:1889`）只补 `REFUND` 流水、从不推进 order，
  下一笔退款或交易通知不来就无人纠正。列第十三轮首位。
- **仍未门控的 `REFUNDED` 落点（第十三轮，与本轮同判据）**：交易通知按 `trade_state=REFUND`
  经 `mapTradeState`（`PayUtils.cc:410`）直接写 order（`CallbackService.cc:1367`），以及查单
  同步路径把映射结果透传落库（`PaymentService.cc:2481/2553/2724`）。REFUND 只说"转入退款"，
  同样证明不了整单退完；本轮刻意不动，避免一次改三处再引入新面。
- **文档同步**：`examples/pay-server/openapi.yaml` 的 OrderStatus/RefundStatus 措辞、
  `TECH_SPECS.md` 状态机三处（`REFUNDED` 行、"只有退款达到 `REFUND_SUCCESS`"导语、
  `REFUND_SUCCESS` 行）已按"已退成功合计覆盖总额才 `REFUNDED`"改口径；CHANGELOG 记 Fixed 一条。

---

## 十九、第十三轮：`REFUND` 答案的三处未门控落点收口 + 退款查单恢复 order

### 官方口径（沿用 §十八 取证）
`trade_state=REFUND` 只表示"转入退款"——单笔订单允许至多 50 次部分退款，
一笔退掉一分钱的交易也会以 REFUND 应答下单/查单/通知三条链。因此
REFUND→order 写 `REFUNDED` 在任何入口都只是"主张"，必须与 §十八 同一判据
（该订单上 `REFUND_SUCCESS` 合计覆盖订单总额）对齐后才能落库。

### 缺陷（§十八 刻意留下的三处）
1. **交易通知**：`mapTradeState`（`PayUtils.cc:410`，REFUND 分支 `:435-437`）把
   REFUND 映射为 `REFUNDED` 后，`CallbackService` 交易通知事务内直写 order
   （`CallbackService.cc:1063-1074` 映射，`:1415` 落库）。
2. **查单同步**：`PaymentService::settleFromChannelAnswer` 微信路径把映射结果
   透传到两处 order 写点（payment 已 SUCCESS 分支与 CAS 成功分支，
   `PaymentService.cc:2595-2604`、`2773-2782`）。
3. **恢复路径缺口**：`syncRefundStatusFromWechat`（`RefundService.cc:1889`）
   退款查单同步只补 `REFUND` 流水，从不推进 order——并发双双少算后订单停在
   `PAID` 时，这条最该纠错的通道自己也不会纠。

### 修复
- 新纯函数 `pay::utils::resolveRefundedOrderStatus`（`PayUtils.cc:388`）：
  仅当映射值为 `REFUNDED` 且 `refundsCoverOrderAmount` 不成立时降档为 `PAID`
  （REFUND 交易毕竟证明了"钱收到过"）；其余状态原样透传。合计不可解析/总额
  不可解析一律按"证明不了"处理，方向保守。
- **通知门**（`CallbackService.cc:1076-1161`）：映射出 REFUNDED 时，在既有
  交易事务上先排队 SUM（豁免 #3 聚合），insert-callback 捕获
  `resolvedOrderStatus` 共享指针并在首行物化为局部 `orderStatus`，下游全部
  写点/流水判据/CAS 透传自动取降档后的值——不重排既有 300 行嵌套。SUM 读失败
  使 Postgres 事务中止，后续语句落入既有错误路径整单回滚重试。非 REFUNDED
  流程零新增 SQL。
- **查单门**（`PaymentService.cc:2542-2577` 事务头 SUM +
  `orderStatusAfterRefundCoverage`（`:82`）两写点应用）：与通知门同一手法；
  "payment 已终态"的纯报告分支（`:2716-2728`）不动。
- **恢复路径**（`RefundService.cc:2063-2235`）：`syncRefundStatusFromWechat`
  在退款行已按 REFUND_SUCCESS 落库的同一事务内读 SUM（本行可见），覆盖总额时
  以 `PAID`→`REFUNDED` CAS 推进 order（guard 防重开已关闭订单），未覆盖/
  不可解析保持现状仅报告退款状态。第十一轮 §十七 与第十二轮 §十八 记录的
  "双双少算停在 PAID"的病，此后只要有一次退款查单同步即可纠正。
- 支付宝查单路径映射不产生 REFUNDED（只产 PAID/PAYING/FAILED），本轮按目标
  范围刻意不动。

### 测试
- 纯函数：`PayUtils_ResolveRefundedOrderStatus`（`tests/unit/PayUtilsTest.cc:72`，
  10 断言，本机绿）：覆盖/超额透传，欠覆盖/零总额/不可解析降档，
  其余状态原样。
- 查单门：`PayPlugin_QueryOrder_WechatRefundKeepsTheCollectedPayment`
  （`tests/integration/QueryOrderTest.cc:1057`）按新语义反转——无退款台账的
  REFUND 答案落 `PAID`（payment 仍 SUCCESS、PAYMENT 流水仍在），并保留
  CLOSED→FAIL/CLOSED 的反向对照；新增
  `PayPlugin_QueryOrder_WechatRefundSettlesOrderOnlyWhenCovered`（`:1085`）
  30.00/49.90 保持 `PAID`、49.90/49.90 落 `REFUNDED`，正向对照防"永不写
  REFUNDED 也能过"。夹具补 `pay_refund` DDL 与种销行、清理。
- 通知门：`PayPlugin_WechatCallback_TransactionRefundState`
  （`tests/integration/WechatCallbackIntegrationTest.cc:2000`）order 断言
  `REFUNDED`→`PAID`（守卫方向复核）；新增
  `PayPlugin_WechatCallback_TransactionRefundStateCoveredSettlesOrder`
  （`:2260`）先种一笔全额已退成功退款再发同一 REFUND 通知，order 必须落
  `REFUNDED`。两案夹具均含 `pay_refund` DDL。
- 本机验证：全量编译绿；七门禁绿（format-clean 69 文件、架构 4、测试布局 4、
  文档漂移 7、迁移 6、版本同步、OpenAPI 15 paths/28 ops）；新增 DB 族用例
  本机 `REQUIRE(client != nullptr)` 快红不挂死，判定属 CI-only。

### 诚实边界与遗留
- 通知门只收"REFUNDED 主张"的降档；`syncRefundStatusFromWechat` 的推进要求
  order 行此刻是 `PAID`，若交易通知先把它写成 `PAID`（降档）再退款同步覆盖，
  方向正确；但若 order 曾被更早路径写成非 PAID 终态（如 CLOSED），恢复不重开，
  与 §十八 同一保守取向。
- 查单"看到已覆盖但行仍 PAID→升档"在 `settleFromChannelAnswer` 刻意未做
  （只降不升，升档留给退款同步与退款通知两条有台账证据的路径），避免把无
  台账写入权的路径变成第二把推进锁。
- 单笔渠道答案仍证明不了"它提到的退款存在"——维持金额对账守卫，未新增。
- 仍未做（后轮候选，按风险排序）：主动关单缺位、`goods_detail` 透传、创建路径
  owner 化统一、`CallbackService` 回调嵌套压平、`time_expire` 是否进 HTTP 契约、
  部分退款是否应过早把 order 记成 `REFUNDED`（§十七 提出的"覆盖判据"本轮已把
  该疑虑消解为"覆盖即终态"，若产品要"部分退款可继续支付/退款"另议）。
- 文档同步：CHANGELOG Fixed 一条；`TECH_SPECS.md` §十八 措辞已覆盖"已退成功
  合计覆盖才 REFUNDED"，本轮三落点同判据无需再改；openapi OrderStatus/
  RefundStatus 描述继续成立。

## 二十、第十四轮：主动关单缺位——超时未支付订单从未在渠道侧关闭

### 官方口径（取证页 4012526915，2026-09-21）
- `POST /v3/pay/transactions/out-trade-no/{out_trade_no}/close`，body 仅
  `mchid`；**成功应答为 HTTP 204 No Content，无应答包体**。
- 使用须知：只有"未支付状态"的订单可关闭；"订单超时未支付……商户需进行关单
  处理"——关单是商户义务，不是渠道兜底。
- 错误码：400 INVALID_REQUEST/MCH_NOT_EXISTS；401 SIGN_ERROR；403
  RULE_LIMIT/TRADE_ERROR（业务原因交易失败——已支付即在此被拒）；429
  FREQUENCY_LIMITED；500 SYSTEM_ERROR（官方注明"请用相同参数重新调用"，天然
  可重试）。
- 取证纪律案例：首轮误抓 `f2f/closeorderinfo`（4012268573，付款码关单，另一个
  接口），页面身份自检发现同名不同物后改页重取，未据错页设计。

### 缺陷
1. **全库没有任何 close 调用点**：所有本地 `CLOSED` 都来自渠道答案的被动读数；
   一笔超时且从未被查到的订单在微信侧保持可支付，直到渠道自己的惰性过期——
   窗口期内用户仍可能付款成功，形成"本地认为未支付、渠道收了钱"的错配。
2. **对账清扫每轮看着这些订单却不结束它们**：`syncPendingWeChatOrders` 扫的
   正是 `PAYING` + wechat 的行，查回 `NOTPAY` 后同步把状态写回 `PAYING`，
   下一轮重复报告，永不收敛。
3. **204 会被当成失败**：共享请求通道把一切 2xx 都按"必须解析出 JSON"处理，
   空 body 的 204 报 `invalid json response`——即便补上调用点，唯一证明
   "已关闭"的应答也会被记成失败。

### 修复
- SPI 新增 `PaymentChannel::closeOrder`（`PaymentChannel.h:78`），带默认实现
  （应答 "channel does not support closing orders"）：调用方 `ReconciliationService`
  只持有 `PaymentChannelPtr`，默认虚函数让无关单能力的渠道不改代码也能编译，
  且错误是显式的而不是沉默。
- `WechatPayClient::closeTransaction`（`WechatChannel.cc:941`）：网络前守卫
  （`missing orderNo` / `missing mch_id`），路径段过 `urlEncodePathSegment`，
  body 仅 `mchid`，POST 签名走既有 `buildAuthorizationHeader`；SPI 入口
  `closeOrder`（`:979`）转发。
- **204 即成功**（现 `WechatChannel.cc:524`）：显式分支返回空对象 + 空错误，
  置于 JSON 解析之前。第十九轮取证后该分支移到验签之后——204 并非验签豁免，
  见 §二十五。
- **清扫门**（`ReconciliationService.cc:195` 读 `row.getExpireAt()` 可空指针，
  `:223-241` 判据）：仅当渠道自己仍答 `NOTPAY` **且**该 order 行的
  `expire_at` 已过，才 `closeOrder`。`NOTPAY` 是唯一"渠道未支付且本地未定"的
  状态——`CLOSED`/`REVOKED` 已被同轮同步落终态，已支付的交易 close 会被渠道
  拒绝且不改本地。拒绝 `LOG_INFO`，成功 `LOG_DEBUG`；本轮刻意不代写本地状态，
  关单成功后微信查单即答 `CLOSED`，下一轮清扫自然落账。
- 支付宝 `closeTrade` 依旧未接（目标范围是微信；SPI 默认实现已就位）。

### 测试
- 通道级（本机绿，无 DB）：
  `WechatPayClient_CloseTransaction_AcceptsSigned204AndSendsCloseShape`
  （`WechatPayClientTest.cc:1051`；第十九轮按取证给 204 补上应答签名并如此更名）
  在测试内起一次性裸 socket 监听，捕获真实
  出网请求并回 204——请求线（POST + 编码路径 + `/close`）、签名头
  （大小写无关匹配 `authorization: wechatpay2-...`）、body 恰含 `mchid` 且无
  `appid`、调用方拿到"空错误 + object"四半同时钉死。
  `WechatPayClient_CloseTransaction_GuardsBeforeNetwork`（`:1017`）：两个网络前
  守卫 + 零出网。`WechatPayClient_CloseOrder_SpiEntryIsSupported`（`:1047`）：
  经 `PaymentChannelPtr` 调用，错误串不得是 "does not support closing"——证明
  SPI 入口转到了真实现而非默认空转。
- 清扫级（DB 族，CI-only）：
  `PayPlugin_Reconcile_ExpiredUnpaidWechatOrderIsClosedOnChannel`
  （`WechatCloseOrderReconcileTest.cc:339`）直接构造
  `PaymentService`/`RefundService`/`ReconciliationService` + 记录型 SPI stub，
  一个正向（过期 3600s + `NOTPAY` → 恰一次 close，本地行保持 `PAYING`）配三个
  反向对照（未来过期不关；`expire_at` NULL 不关；`SUCCESS` 不关且照常落
  `PAID`/`SUCCESS`）。`reconcile` 回调只代表派发完成，等待一律以下游事件
  （stub 的"看过并决定"、行状态轮询）判定。本机 `REQUIRE(client != nullptr)`
  快红不挂死。
- 编写期缺陷留证：监听器初稿在测试线程内同步 `accept`，而客户端请求只有在
  `runOnce` 返回端口后才会发出——自死锁挂死整个用例。修复为后台线程运行 +
  promise 在 listen 成立时立刻交付端口 + 全链路 15s/5s 封顶，断言前必 `join`。
- `tests/CMakeLists.txt:33` 注册新文件；七门禁全绿（format-clean 70 文件、
  架构 4、测试布局 4、文档漂移 7、迁移 6、版本同步、OpenAPI 15 paths/28 ops
  不变——本轮无新 HTTP 面）。

### 诚实边界与遗留
- 关单成功不等于本地立刻 `CLOSED`：本地落账仍由下一轮清扫读渠道答案完成
  （最多延迟一个清扫周期）。若要求"close 应答 204 即本地终态"，需要清扫内
  二次查单，本轮未做——保守且少一处状态写权。
- 429/500 之类的可重试失败不做单请求重试，依赖下一轮清扫天然重放；窗口期内
  用户若已付款，close 会被 403 拒绝，通知/查单链照常接管。
- 清扫门依赖 order 行自身 `expire_at`：第十轮已保证创建路径写入的
  `time_expire`/`expire_at` 两侧读数一致；未带超时的历史行保持"永不主动关"。
- 后轮候选（按风险排序）：`goods_detail` 透传、创建路径 owner 化统一、
  `CallbackService` 嵌套压平、`time_expire` 进 HTTP 契约、Alipay `closeTrade`
  接入（SPI 已就位）、查单门"已覆盖但行 PAID→升档"。
- 文档同步：CHANGELOG Fixed 一条（并把第十一轮条目被后续批次挪动的四处行号
  重新锚定到当前代码）；`TECH_SPECS.md` 无与本轮冲突的清扫表述，不改。

## 二十一、第十五轮：时间戳窗口候选证伪 + 金额解析溢出回绕收口

### 候选取证（通知验签缺时间戳窗口 / 重放防护）
第十一轮 §十七 曾把"回调验签无时间戳容忍窗口"列为候选。本轮对撞官方文档后
**证伪并撤销该候选**，取证五页（均为 2026-09-21 实测可达、内容自证身份）：

- APIv3 签名验证总述（`wiki/doc/apiv3/wechatpay/wechatpay4_0.shtml`）：
  对通知仅要求"商户接收到回调通知报文后，需在 **5 秒内完成对报文的验签**"
  ——这是**处理时限**（超时微信会重发），不是"时间戳与本地钟差超窗即拒"的
  时钟窗口判据。
- 支付成功回调通知（`wiki/doc/apiv3/apis/chapter3_4_5.shtml`）与商品券
  回调通知（`doc/v3/partner/4016435717`）：明确"商户系统**必须能够正确处理
  重复的通知**"——官方把重放/重复的防线交给商户状态机幂等，而非时间戳窗口。
- 微信支付公钥验签指引（`doc/brand/4015407582`）：给出应答验签的组成
  （应答时间戳\n应答随机串\n应答报文主体\n）与"验证失败应舍弃该应答"，
  同样**未定义**通知时间戳窗口。

**拒绝实现的理由（守卫方向）**：若强行加 5 分钟硬窗口，会把微信对非 2xx
应答的合法长间隔重试（可达数小时）误拒成永久丢单——这正是本审计一路反对的
"守卫方向反了"。而"正确处理重复通知"一侧，第一至十三轮的
CAS 状态机 + 流水台账 + 幂等 owner token 已构成闭环：同一通知重放最多产生
一次状态迁移。硬验签（平台证书 + AEAD 解密 + serial 绑定，第十轮后含静态
证书序列号核对）已挡住改包重放；无窗口即维持现状。`verifyCallback`
（`WechatChannel.cc:1177`，第十六轮应答验签入码后重锚）不做 delta 判定是**有意决定**，非遗漏。

### 同轮发现的真缺陷：`parseAmountToFen` 溢出静默回绕（资金语义）
- **机理**：controller 的 `validateAmount`（`PayHandlers.cc:44`）正则
  `^\d+(\.\d{1,2})?$` 对**数字位数无上限**，`pay_order.amount` 是
  VARCHAR(32)；`parseAmountToFen`（`PayUtils.cc:308`）里 `std::stoll`
  只对超出自身范围的字面量抛 `out_of_range`（被 `catch(...)` 吞掉返回
  false），但 17–19 位元值能正常解析，随后 `yuan * 100 + cents` 有符号
  溢出（UB，实测 MSVC/GCC 均为二进制补码回绕）。
- **可利用性**：下单金额 "184467440737095517.99" 回绕成 fen = 183——
  账面 1.83 元的"已支付"订单去平 1.8 亿元的订单语义；且回绕值仍为合法
  正数，下游全部金额对账守卫（第十一轮金额证明、第十二/十三轮覆盖判据）
  对回绕后的自洽数字全部放行。QR/退款两侧调用点
  （`PaymentService.cc:447`、`:1540`，`RefundService.cc:670`）同样受影响。
- **修复**（单点根因，`PayUtils.cc:370`）：在 `stoll` 成功后、乘法前加
  精确前置判据 `yuan > (INT64_MAX - 99) / 100 → return false`——上界即
  "能无回绕换算成 fen 的最大元值"（92233720368547757.99 → fen
  9223372036854775799），不是拍脑袋的位数帽。函数返回 false 后由各
  service 调用点既有的拒绝路径回 400，handler 正则刻意不加位数上限：
  溢出判据必须在**做乘法的地方**，否则只是第二顶帽子。
- **支付宝侧同步受益**：`parseAmountToFen` 是两侧共用的解析器，通道层
  `total_amount` 回读（`PaymentService.cc:2992`）同样被守卫。

### 测试
- `PayUtils_ParseAmountToFen`（`tests/unit/PayUtilsTest.cc:25`，本机绿，
  14→19 断言）：新增三个负例（回绕攻击值、17 个 9、上界加一分）与一个
  **精确上界正例**（"92233720368547757.99" 必须通过且 fen 逐位断言）——
  防"顺手写成更小的帽把合法极值也拒了"的守卫方向复核。纯函数级，无 DB。
- 本机验证：增量编译绿；七门禁全绿（format-clean 70 文件、架构 4、测试
  布局 4、文档漂移 7、迁移 6、版本同步 1.0.0×3、OpenAPI 15 paths/28 ops
  ——本轮零 HTTP 面变更）。
- **声明核对纪律另抓到 8 处历史行号漂移**（第十一~十四轮代码增删所致，
  最大 41 行）：CHANGELOG 与 §八/§十/§十一/§十二/§十三 中引用的
  `mapTradeState`/`refundsCoverOrderAmount`/`resolveRefundedOrderStatus`/
  `validateWechatOrderFields`/`parseRfc3339` 全部重新实测锚定。

### 诚实边界与遗留
- 回绕依赖 UB 的实际表现（补码回绕）；修复不依赖它——判据在溢出**之前**，
  任何标准实现下都成立。
- handler 仍无金额位数上限：超大但**不溢出**的合法字面量（如 15 位）照常
  进业务层。业务上无此等大额订单，属纵深防御候选而非缺陷，未加。
- 下一轮候选首位（风险排序）：**出站应答验签整体缺位**——官方口径
  "如果应答的签名验证失败，品牌商户系统应舍弃该应答"（§二十一取证品牌页），
  验签三元组口径已在手；当时 `Wechatpay-Signature` 头只在入站通知路径被读
  （`WechatChannel.cc:1321`，第十六轮后重锚），出站应答（下单/查单/退款）信任完全押在 TLS
  上。改造点在 `sendWechatRequest` 收口，需先厘清平台证书轮换窗口，风险
  大于本轮所有改动，单独成轮。余下：`goods_detail` 透传、创建路径 owner 化
  统一、`CallbackService` 嵌套压平、`time_expire` 进 HTTP 契约、Alipay
  `closeTrade` 接入、查单门"已覆盖但行 PAID→升档"。
- 文档同步：CHANGELOG Fixed 一条；`TECH_SPECS.md` 无涉；openapi 无变更。

## 二十二、第十六轮：出站应答验签收口（§二十一候选首位兑现）

### 官方取证（应答必须验签）
- APIv3 签名验证总述（`wiki/doc/apiv3/wechatpay/wechatpay4_0.shtml`，
  2026-09-21 实测）："微信支付应答商户的请求时，商户需要验签"——口径覆盖
  **所有**请求应答场景，不止回调。
- 微信支付公钥验签指引（`doc/brand/4015407582`，第十五轮已取得）：应答验签
  消息组成为"应答时间戳\n应答随机串\n应答报文主体\n"，并明示"如果应答的
  签名验证失败……应舍弃该应答"。
- 关闭订单（`doc/v3/merchant/4012526915`）：成功应答为 **204 且无报文体**，
  该页未记载 `Wechatpay-*` 应答头——204 没有可签名的 body，验签收口必须
  给它让路（第十五轮 204 早退路径先于验签块，`WechatChannel.cc:503` 后）。
- 页面重构留证：`wiki/doc/apiv3/wechatpay/wechatpay4_1.shtml` 现返回总述
  页内容（抓取时以"内容自证身份"核对发现），未据此页做任何断言。

### 缺陷与收口（单点：`sendWechatRequest`）
- **缺陷**：入站通知自第一/十轮起已有硬验签，但**出站应答**（下单/查单/
  关单/退款/退款查询）拿到 2xx + 可解析 JSON 就直接交给 service 层——
  `api_base` 之后任何能终结/旁观 TLS 会话的一方，都能用一个结构良好的
  `200 {"trade_state":"SUCCESS"}` 把任意状态写进订单机。信任完全押在传输
  层，渠道层的签名防线只做了一半。
- **收口**（`WechatChannel.cc:446` 新增 `verifyAnswer` 形参，`:520` 判定
  块）：每个**带 body 的 2xx** 应答先过 `verifyResponse` 再进解析；验签
  失败的应答**整体舍弃**——错误串只带原因，不转述伪造 body（`api_base`
  是配置，那边回来什么都不能经我们的错误信息反射给业务方）。五个业务入口
  （`createTransactionNative :648`、`queryTransaction :973`、
  `closeTransaction :1013`、`refund :1044`、`queryRefund :1072`）统一传入
  `answerVerifier()`。
- **非 2xx 不验**（守卫方向）：失败应答在 service 层只会驱动失败/待定路径，
  能伪造它的人本就不需要借这条通道改状态；强行收紧只会把微信合法的错误
  应答（含网络中间设备产出的 4xx/5xx）误伤成不可用。
- **`/v3/certificates` 例外**（`:720` 传空 verifier）：bootstrap 悖论——
  该应答本身运送信任锚，不可能用尚未取得的锚验自己。真实性由两道自有
  防线承担：应答 body 用 `api_v3_key` 做 AES-GCM 解密（AEAD 即自证），
  每张证书入库前过 `setPlatformCert` 的 X.509 解析 / 有效期 / serial 绑定 /
  CA 锚链核对（第十轮）。

### `verifyResponse` 与证书解析共享
- `verifyResponse`（`WechatChannel.cc:1196`）读 `Wechatpay-Timestamp/
  Nonce/Signature/Serial` 四头，拼"应答时间戳\n应答随机串\n应答报文主体\n"
  走与通知同一把 `verifyMessageWithCert`。
- **头集合缺失在证书解析之前拒绝**（:1196 内先判三件套、:1208 落错误串，再进 resolver）：
  未签名的应答不得消耗共享的证书下载窗口——否则一个伪造的"serial 未知"
  应答就能反复触发限流内的全量下载，Do 掉合法轮换的收敛路径。
- `resolveTrustedPlatformCert`（`WechatChannel.cc:1115`）本轮从
  `verifyCallback` 串重载里**逐字提取**（三条错误串
  "missing Wechatpay-Serial" / "failed to read static cert: …" /
  "no trusted platform certificate for serial: …" 原样保留，既有
  VerifyCallback 族断言零改动即证明提取无行为漂移）：缓存 → 静态证书
  （必须携带它所服务的 serial 本身）→ 限流下载后拒绝本次。出站与入站
  从此共用同一信任判定，入站能验的 serial 出站就能验，反之亦然。

### 生命周期（异步边界上的 `this`）
- `answerVerifier()`（`WechatChannel.cc:1222`）持 `weak_from_this()` pin：
  生产侧客户端全部由 `shared_ptr` 持有（registry `PayPlugin.cc:118` 与
  各 service），停机竞态下迟到的应答被丢弃（"client destroyed before the
  answer was verified"）而非解引用死对象——与证书刷新第十三轮同款收口。
- 栈上构造的单测客户端 pin 不住（`weak.expired()` 为真即未入主），此时
  保留裸 `this` 绑定：这些用例自身在析构前等待应答，这是可证的窗口；
  判据用 noexcept 的 `weak_from_this().expired()`，不捕 `bad_weak_ptr`
  （`weak_from_this` 不抛，只有 `shared_from_this` 抛——首版 try/catch
  是错误语义，已删）。

### 测试（正负对照，本机绿）
- `WechatPayClient_QueryTransaction_AcceptsSignedOkAnswer`
  （`WechatPayClientTest.cc:1084`，7 断言）：one-shot listener 给出文档
  形状的签名成功应答（200+JSON+四头，签名覆盖 ts\nnonce\nbody\n），端到端
  必须接受且 `trade_state` 透传——**守卫方向的正向对照**，防"验签把真应答
  也全拒了"的自锁。
- `WechatPayClient_QueryTransaction_DropsUnsignedOkAnswer`（`:1157`，6
  断言）：200 + 声称 SUCCESS 的合法 JSON、**无 Wechatpay-\* 头**必须整条
  舍弃；且该客户端**不配任何平台证书**——断言**完整错误串**（评审后由前缀
  改全串：resolver 自己的 "missing Wechatpay-Serial" 与前缀同头，前缀检查
  分辨不出"拒绝发生在 resolver 之前"这条顺序声明），全串钉死才能证明缺头
  拒绝发生在证书 resolver 之前（不落进下载窗口）。
- `WechatPayClient_VerifyResponse_BindsHeadersBodyAndSerial`（`:1224`，10
  断言）：免 socket 的单元级五态——全对通过 / body 被换而签名原样必须拒 /
  缺头集精确错误串 / 未知 serial 精确错误串 / **超长 serial 被帽**
  （"Wechatpay-Serial too long"，:1133——拒绝串会经 service 回显给
  我方 API 调用方，头部输入必须先限长再进串）。
- `WechatPayClient_DownloadCertificates_EmptyVerifierAnswersNormally`
  （`:1295`）：证书下载走 200+body 的**空 verifier** 通道必须正常返回
  "invalid certificate response format"（形状拒绝）——这是评审 BLOCKER
  的回归钉：缺陷形态下 IO 回调里抛 `bad_function_call`，应答丢失/进程受损。
  客户端必须 `make_shared` 构造（证书回调跨异步边界 weak-pin，栈对象没有
  可 pin 的控制块——这条同时是 pin 语义的活证据）。
- 既有应答路径零改动全绿：HTTP 404 错误映射、204 关单、guards、SPI 转发
  四例（非 2xx 与 204 的 carve-out 各自被原断言继续钉住）。

### 交付前子代理评审（两条真缺陷，均已修并钉）
按惯例推送前派两路只读评审（实现对撞 + 声明核对）。声明核对一侧确认既有
全部行号/断言/引文当时无漂移；实现对撞抓到两条**属实**的缺陷：
1. **BLOCKER：空 verifier 被堆包装后恒真**。`sendWechatRequest` 曾把
   `verifyAnswer` 包进 `make_shared<std::function<...>>` 再判 `if
   (verifier ...)`——指向**空函数**的 shared_ptr 恒为真，`/v3/certificates`
   （有意传空 verifier）一旦拿到 200 应答就在 IO 回调里调空 `std::function`
   抛 `bad_function_call`，**恰好炸掉为验签装载信任锚的那条路**。三新例
   全绿也盖不住它：证书下载旧例在签名前置检查就失败，从未触网。修复：
   lambda 以 init-capture 直接持 `std::function` 本体（`:484`），
   `if (verifyAnswer ...)`（:520）即"函数非空才调"；上条测试实例钉死。
2. **MAJOR：weak-pin 判空与使用之间仍有释放窗口**。`answerVerifier` 首版
   `if (pinned && weak.lock() == nullptr)` 里 lock 的临时 shared_ptr 在
   表达式结束即析构——最后一个外部持有者随后释放时，对象可在
   `verifyResponse` 触碰 `this` 前死亡。修复：pinned 分支把 `weak.lock()`
   结果**跨整个调用持有**并走 `self->verifyResponse`（:1240）。
评审另记两条不改代码的判断：204 carve-out 端点无关（五条路径通吃，当前
所有消费方对空对象都不记成功——脆弱不变量，注释已点明）；`pinned` 快照
本身（创建时 `!weak.expired()`）语义正确，维持。

### 验证记录与踩坑留证
- **cwd 地雷（本轮最大假红来源）**：集成套件自带的 Drogon listener 依赖
  `tests/main.cc` 从 **cwd** 找 config.json；从仓库根目录跑 exe 时找不到
  配置 → listener 从未启动 → 走 `testBaseUrl()` 的用例报 "http request
  failed" / "Bad server address" 假红，与代码无关。定罪方式：让本轮零涉
  及的 `HealthProbe_LivenessEndpoint` 同场对照（同样假红即非本轮改动），
  临时用 CHECK 展开打印真实错误串取证后**已删除**（调试代码不入库）。
  正确姿势固定为 `cd build/windows-msvc/tests/Release` 再跑。
- 七门禁全绿：format（本轮 --fix 收敛 2 文件后 70 文件净）、架构 4、测试
  布局 4、文档漂移 7、迁移 6、版本同步 1.0.0×3、OpenAPI 15 paths/28 ops
  （零 HTTP 面变更）。

### 诚实边界与遗留
- 应答验签沿用第十五轮"通知不加时间戳窗口"的同一判断：官方 5 秒条款是
  处理时限不是时钟窗；本轮未给应答加 delta 判定，签名新鲜度由请求-应答
  会话自身保证。
- 支付宝通道出站应答不在本收口内（独立签名体系，后续候选）。
- §二十一两处 `WechatChannel.cc` 行号引用（verifyCallback 串重载、
  `Wechatpay-Signature` 读取点）因本轮代码增删漂移，已重锚 :1177/:1321
  并在原文标注。
- 下一轮候选首位（风险排序）：`goods_detail` 透传（微信侧分账/对账依赖，
  当前建单丢弃该字段）。余下：创建路径 owner 化统一、`CallbackService`
  嵌套压平、`time_expire` 进 HTTP 契约、Alipay `closeTrade` 接入、查单门
  "已覆盖但行 PAID→升档"、Alipay 出站应答验签、handler 金额位数帽
  （纵深防御）。
- 文档同步：CHANGELOG Fixed 一条；`TECH_SPECS.md` 无涉；openapi 无变更。

## 二十三、第十七轮：金额位数帽候选证伪 + 收入台账双记账的 DB 层兜底

§二十二 候选清单里的"handler 金额位数帽（纵深防御）"本轮先取证、后裁决，
并顺手把落账路径的最后一道纯应用层防线降到 DB 层。

### 候选证伪：位数帽没有官方依据
- 官方取证（`wiki/doc/apiv3/apis/chapter3_4_1.shtml`，Native 下单请求参数页，
  身份自证通过）：`amount.total` 原文口径为"**必填 integer…单位为分，整型，
  必须大于0**"，**未标注任何最大值**。给 INT32（2147483647 分）设帽属于臆造
  约束，会误拒文档允许的订单——与第十五轮"时间戳硬窗口"同一类候选：官方
  口径不支持，证伪留证，不改码。
- 现有防线已覆盖真实风险：`parseAmountToFen` 的 INT64 溢出守卫
  （`PayUtils.cc:370`，第十五轮）保证转发给渠道的值不回绕；handler 正则
  （`PayHandlers.cc:44` `^\d+(\.\d{1,2})?$`）拒负数/多小数位/科学计数法。
- 顺带核实非缺口的两处经典门：回调金额对单已在
  `CallbackService.cc:935-1030`（支付通知）与 `:2820+`（退款通知）；
  查单侧对单在 `reconcileAmountProblem`（`PaymentService.cc:57`）。

### 真缺口：收入账的防重全在应用层 CAS
只读子代理对"通知→落账"三问审查（A 无单放行 / B 已关单收 SUCCESS /
C 重复投递）结论：三门皆有、无状态机缺陷——匹配集 `openAttemptsOfOrder`
（`CallbackService.cc:33`）排除 FAIL/关单行，查空即回滚 FAIL（`:860-879`）；
落账 CAS `status IN ('INIT','PROCESSING')` + `forUpdate()`（`:845`）行锁；
重复投递四层保证（幂等快照 / 终态跳过 / 行锁+CAS / 台账随事务提交）。
**但 `pay_ledger` 收入账在 DB 层没有任何防重**：台账 append-only，五处收入
落账点（`CallbackService.cc:1465`；`PaymentService.cc:2627/:2811/:3123/
:3293`）全部只在各自 CAS 命中的分支写入——若该门未来回归（重构、新落账门、
乱序回调），双记账将无声发生。这正是 003 对退款行用过的纵深防御形态。

### 修复：部分唯一索引
`sql/006_ledger_payment_income_unique.sql`：

```sql
CREATE UNIQUE INDEX IF NOT EXISTS uq_pay_ledger_payment_income
    ON pay_ledger(payment_no)
    WHERE entry_type = 'PAYMENT' AND payment_no IS NOT NULL;
```

- 不变式"一个 payment_no 至多一条收入账"由 DB 钉死：重复插入在落账事务内
  响亮失败，而不是静默记两笔钱。
- **REFUND 刻意不入索引**：部分退款对同一 payment_no 合法出现多条，且台账
  无 `refund_no` 列可键控——收窄谓词到 `entry_type='PAYMENT'` 是唯一无合法
  重复的形状（诚实边界：退款账的 DB 防重需要 schema 变更，单独候选）。
- 五处收入落点逐一实测均携带 `paymentNo` 且仅在 CAS 命中后写入（本轮 grep
  核对，防"索引正确但某落点绕过"的假安全感）。
- `payment_no IS NOT NULL` 谓词与 004 的 FK 语义对齐（部分账目无支付行）。
- 脏数据裁决：若既有库已含重复收入行，`CREATE UNIQUE INDEX` 会使迁移响亮
  失败——这是特性不是事故（本轮无任何已知双记账 bug，正常库必绿）。

### 验证记录
- `check_migrations.py` 绿：7 文件、5 baselined、006 受内容规则约束并通过。
- 本机无 Postgres（AGENTS.md 环境约束）：索引强制行为与迁移重放均为
  **CI-only 裁决**；本轮无 C++ 改动，既有全部本机用例不受影响（未重跑，
  零代码变更即零回归面）。
- 测试夹具自带 `CREATE TABLE IF NOT EXISTS pay_ledger` 最小表（QueryOrder/
  RefundQuery/Idempotency 族），不建 006 索引——夹具语义与生产 schema 的
  这处差异如实记录；强制用例（插重复收入账期望失败）待 DB 族在 CI 补钉。

### 诚实边界与遗留
- 本轮修复是**兜底加固**而非在案缺陷：没有证据表明双记账已发生过；价值在
  于把最关键的金额不变式从"依赖调用点纪律"降级为"DB 拒绝违约"。
- 子代理附带的低危观察如实留档不展开："SUCCESS∧全 attempt 已关闭"的异常
  通知目前只有 LOG_ERROR+FAIL 重试（通道语义上不可达：微信侧关单成功即
  不再受理支付），未落人工处理台账；refund 账 DB 防重需先加 `refund_no` 列。
- 后轮候选（风险排序）：`goods_detail` 透传（官方选填、审计自评"不算缺陷"，
  属功能增强）、创建路径 owner 化统一、`CallbackService` 嵌套压平、
  `time_expire` 进 HTTP 契约、Alipay `closeTrade`、查单门升档（§十四 有
  记录的刻意设计，重开需新证据）、Alipay 出站应答验签、**refund 账 DB
  防重（补 refund_no 列）**。位数帽已证伪出清。

## 二十四、第十八轮：time_expire 从未有 HTTP 入口——到期关单整链在生产不可达

§二十一 以来的候选"time_expire 进 HTTP 契约"本轮定罪为**真缺陷**并收口。

### 缺陷链（全部实测）
- `CreatePaymentRequest::timeExpire`（`PaymentService.h:47`）全仓**无任何赋值
  点**（handler、QR 路径都没读这个字段）→ 恒为空。
- 空字段使第十轮的三段上游修复全部空转：前置校验 `PaymentService.cc:497`、
  渠道透传 `:712`、以及全仓唯一 `order.setExpireAt` 写入点 `:650`。
- `pay_order.expire_at` 恒 NULL → 第十四轮主动关单的清扫门
  "NOTPAY ∧ expire_at 已过"（`ReconciliationService.cc:214` 的注释链：
  "the channel asks the question, `expire_at` answers it"）**在生产永不满足**
  ——超时未付订单只能靠微信侧自动失效，我方主动关单能力形同虚设。

### 修复（两入口同形接线）
- `/api/pay/create`（struct 路径）：shape 表加 `time_expire`
  （`PayHandlers.cc:177`）；请求装配 `request.timeExpire = json->get(...)`
  （`:210`）。服务侧校验/透传/落库为第十轮既有代码，本轮起才活。
- `/api/qrpay/create`（JSON 路径）：shape 表（`PayHandlers.cc:338`）+ 透传
  字段环（`:410`，注释从"all four"改"all five"）；服务侧读取
  `PaymentService.cc:1404`、**进重放哈希**（`:1461`——改期即冲突非重放）、
  预订前 400 校验（`:1545`，与 struct 路径同一 `validateTimeExpire` 官窗口
  径）、微信分支透传（`:1612`）、QR 落单 `setExpireAt`（`:2017`，解析失败
  沿 struct 路径纪律：照常落单不写 expire）。
- OpenAPI：两 schema 各加 `time_expire`（create 注明**两渠道一律 RFC 3339**
  ——校验器本体就是 parseRfc3339，`yyyy-MM-dd HH:mm:ss` 空格形会被拒——且仅
  微信透传到渠道、支付宝只用于本地 expire_at；QR 注明仅微信透传、哈希
  覆盖、400-先于-预订）。
- 支付宝 QR 分支不透传（渠道不认此形），与 struct 路径既有语义一致。

### 测试（本机绿）
- `PayHandlers_CreatePayment_TimeExpireNumber_Answers400InsteadOfThrowing`
  （`RequestBodyShapeTest.cc:199`，2 断言）与
  `PayHandlers_CreateQRPayment_TimeExpireNumber_...`（`:278`，2 断言）：
  数值形 `time_expire` 必须在 shape 门 400，不得裸穿到服务层。
- 邻集回归：两条既有 shape 例、`PayUtils_ValidateTimeExpire`（39 断言）、
  `CreateQRPayment_NegativeAmount_Refused` 全绿；QR 端到端预订（expire_at
  落库、改期冲突）属 DB 族——**CI-only**（T2 首跑 exit=127 瞬态伪码，cmd
  复跑 RET=0 全绿，与前轮同象留证）。

### 诚实边界与遗留
- 本轮未做子代理评审（预算轮尽），交付前对撞由锚点逐条实测替代；推送授权
  到手后可补跑评审。
- §十七/§二十二 引用的 `WechatChannel.cc` 锚点本轮实测未漂（`:648/:973` 原
  样）。docs-drift 唯一红仍是 006 未 stage（授权链副作用，第十七轮已记）。
- 后轮候选（风险排序）：`goods_detail` 透传（功能增强）、refund 账 DB 防重
  （需 `refund_no` 列）、创建路径 owner 化统一、`CallbackService` 嵌套压平、
  Alipay `closeTrade`、查单门升档（刻意设计）、Alipay 出站应答验签。

### 交付前子代理评审（第十八轮补跑，兑现 §二十四 的明示缺口）
只读评审按"clearReservation 泄漏 / 哈希自洽 / 校验-解析接受集一致 / 绕过面 /
契约对撞 / 锚点对撞"六路攻击。结论：**修后推送**，两条已当场修复——
- **MAJOR**：openapi create schema 与 §二十四 曾写"Alipay honours
  `yyyy-MM-dd HH:mm:ss`"——实测 `validateTimeExpire` 无条件走 `parseRfc3339`
  （`PayUtils.cc:754`），空格形在 `PayUtilsTest.cc:399` 就被钉为拒绝；且支付宝
  从不透传该字段（struct/QR 两分支均 wechat-only），只落本地 expire_at。文案
  已改为"两渠道一律 RFC 3339、仅微信透传"。（代码行为本身安全，纯文档误导。）
- **MINOR×2**：`PaymentService.cc` QR `setExpireAt` 无 else 的静默与 struct 路径
  不同形——补注释声明"校验器与本解析器同源、不可达、故意静默"；CHANGELOG 与
  §十六 的 `:477/:625` 历史锚点漂移——重锚 `:497/:650` 并标注。
评审同时**正向确认**：QR 拒绝分支与相邻 currency/amount 分支同形无预留泄漏；
重放哈希对缺省空串稳定；校验器与解析器接受集一致（不存在"渠道拒而本地无
expire"错位）；直调 service 的既有测试不带该键不抛。

---

## 二十五、第十九轮：出站验签门放错位置 + 退款终态判定采信了不该采信的应答

PR #15 等三平台 CI 裁决期间补跑评审。三路子代理评审（迁移 SQL、应答验签
绕过面、测试有效性）全部返回，两条前轮遗留的编译门禁修复另记（`fb3cd80`：
`-Wunused-but-set-variable` 与 MSVC `C4456` 各一处，属"本机单一编译器视角
看不到"的类）。

### 取证（官方"如何使用微信支付公钥验签"页 `doc/brand/4015407582`）
四问四答，逐字取回：
1. 验签义务覆盖**所有标准 API 应答**，不限 2xx；指南并要求"如果应答的签名
   验证失败，应舍弃该应答"。
2. 豁免只有**文件/图片下载接口**（其响应头不含签名值）；**204 不在豁免内**，
   验签串按空主体构造，即 `应答时间戳\n应答随机串\n\n`。
3. 指南**不要求**把 `Wechatpay-Timestamp` 与本地钟差做窗口判定，时间戳只作
   签名输入。
4. 签名串三行、行尾 `\n`。

### 缺陷与修复
- **MAJOR 一（`WechatChannel.cc`）**：验签门原写作 `if (verifyAnswer && status
  >= 200 && status < 300)`，且 204 分支在其之前直接返回成功——于是伪造的
  **未签名 204** 是所有伪造应答里最便宜的一种（连报文都不用编），却对一切
  端点等于"调用成功"；伪造的 **4xx 报文**同样绕过验签，其 `code`/`message`
  又成为退款终态判定的输入。取证 (1)(2) 说明这两类应答本来都要验签。修复：
  把验签前移为"凡带 verifier 先验签"（`:512`），204 分支移到其后（`:524`），
  只保留证书下载引导（`verifyAnswer` 为空，其应答由 AES-256-GCM 认证标签
  自证）作为豁免。
- **MAJOR 二（`RefundService.cc:58` `refundCertainlyDidNotHappen`）**：它按错误
  串形状反推"请求是否发出过"，黑名单只有 `HTTP `/`http request`/`invalid
  json response` 三类。第十八轮把出站验签接上后，新增的
  `response signature verification failed: …`（以及 `WechatChannel.cc:1241`
  "客户端在应答被验签前销毁"）都不以 `HTTP` 开头，于是被判成"本地故障、
  肯定没发生"→ 直接 `updateRefundWithError` 落 `REFUND_FAIL` 并向调用方回
  1502。请求其实已发出且有人答了，只是不该信这个答；调用方拿终态失败换新
  `out_refund_no` 重试，微信会把它当成第二笔退款——正是该函数头注释自己列的
  要避免的场景。修复：验签失败族并入"已过 HTTP/结果未知"，走 `REFUNDING` +
  `LOG_WARN`，交给对账裁定。

### 候选证伪 / 降级
- 评审提"出站无时间戳窗口"：按取证 (3) 非规范义务，且与第十五轮入站结论
  一致（通知类文档同样只给处理时限与重复通知义务），本轮**不加**。
- 评审提"签名串不绑定请求路径与我们自己的随机串"：成立，但窗口也治不了它。
  真判据是**应答身份字段须与所请求一致**——而三处读数点
  （`PaymentService.cc:2383`、`ReconciliationService.cc:227`、
  `RefundService.cc:1478`）只读 `trade_state`/`status`，不校
  `out_trade_no`/`out_refund_no`。列为后轮首位候选（需真端点语料确认字段
  恒在，否则失败即停摆结算）。

### 测试（本机绿）
- 通道族（无 DB）：`WechatPayClient_CloseTransaction_AcceptsSigned204AndSendsCloseShape`
  （`WechatPayClientTest.cc:1051`，原 `Accepts204AndSends...` 更名）——监听端
  现按指南对空主体签名，正向对照"签名 204 仍等于成功"；新增
  `..._DropsUnsigned204Answer`（`:1133`）钉未签名 204 → 精确验签失败串、空
  主体不外泄；新增
  `WechatPayClient_QueryTransaction_ReportsSignedHttpErrorWithEnvelope`
  （`:974`）证明**签名**的 404 仍带 `HTTP 404: ORDER_NOT_EXIST`（终态判定的
  合法来源不能被验签门一起削掉）；既有
  `..._ReportsHttpErrorAsFailure`（`:924`）期望改为验签失败——未签名 404 现在
  在读状态行之前就被拒。
- 服务族（DB）：新增 `PayPlugin_Refund_UnreadableAnswerStaysUnknownNotFail`
  （`RefundQueryTest.cc:3930`）：桩通道回验签失败 → 响应 `REFUNDING`、退款行
  仍 `REFUND_INIT`；正对照 `PayPlugin_Refund_WechatErrorPersistsPayload`
  （`:1069`）配置缺失（从未发出）→ `REFUND_FAIL`。`RefundStubChannel` 与
  `settleRefund` 各加一个默认为空的 error 形参，既有调用点行为不变。
- 测试有效性评审的两条自身问题：`PayPlugin_WechatCallback_DbClientNotReady`
  只有一句 `CHECK(error)`，且请求体未签名，实际走的是
  `CallbackService.cc:365` 的签名拒绝，而非其名字声称的 DB 未就绪分支——实测
  `handlePaymentCallback` 根本没有 null-DB 分支（`PayPlugin.cc:174` 在取不到
  DbClient 时直接早退，不装配服务），故更名为
  `PayPlugin_WechatCallback_DropsUnsignedBodyBeforeDb`
  （`WechatCallbackIntegrationTest.cc:338`）并补 `code=FAIL` +
  `message="signature verification failed"` 两条断言；owner_token 断言由
  "非空"升为"32 位十六进制"形状检查，并写明它**不能**证明跨投递归属（该行
  本就该带后到投递重预订后的 token）。
- `CHECK(a && b)` 陷阱：评审独立全量扫 `tests/`，无顶级 `&&`/`||` 残留（第十七
  轮 `bb54e59` 的清扫仍成立）。
- 本机：`/WX` 全绿编译；220 例 / 2005 断言全绿 exit=0（较上轮 +3 例）。

---

## 二十六、第二十轮：CI 裁决打到三条真问题——QR 拒绝路径"先应答后落库"

PR #15 的三平台裁决下来后，先做归属判断（用户明确要求：先分清哪些影响本任务、
哪些只是环境噪声），逐条对撞日志后结论是**三条全是本分支的真实缺陷**，没有一条
可以推给环境：

### 一、Linux 腿编译红：`unused variable 'winsock'`（GCC 独有）
`OneShotListener::runOnce` 里的 `static WinsockGuard winsock;` 在 `_WIN32`
分支是有构造副作用的（`WSAStartup`），但非 Windows 分支的类型是空 struct
（`WechatPayClientTest.cc:339`），GCC 因此按普通未使用局部变量报
`-Werror=unused-variable`。MSVC 看不见（本地 `/W4` 不报），clang 这一轮也没报，
所以只有 Linux 腿红。改法取"意图声明"而非删除：`[[maybe_unused]]`
（现 `:521`），并写明跨平台差异，防止后人当成冗余删掉副作用构造。

### 二、macOS 腿编译红：`lambda capture 'this' is not used`（clang 独有）
第十九轮把 SUM 聚合上提时，`RefundService.cc` 结算 lambda 的捕获表仍带着
`this`——lambda 体内一个成员都不碰（`reportMapperFailure` 是 28 行的文件级自由
函数），clang 的 `-Wunused-lambda-capture` 直接判死。MSVC 无此类诊断。删 `this`
后必须证明安全：`dbClient_->execSqlAsync` 属于**外层** `settleOrder`
（`:1717`，仍捕获 `this`），`Mapper<PayRefundModel> refundUpdater(dbClient_)`
在**函数体**（`:1791`），两者都在该 lambda 之外——已逐处实测。

### 三、Linux + Windows 双腿测例红：QR 拒绝路径的响应抢在自己的写前面
这条与前两条不是一个量级——它是**功能缺陷**，且是本轮唯一被 CI 打到、
本地全绿却漏掉的：

- 现象：`PayPlugin_QrBooking_ChannelRefusalClosesThePaymentAndAllowsRetry`
  （`QrPaymentBookingTest.cc:360`）在 `:382` 读到 `INIT`，期望 `FAIL`。
  Windows 腿挂 2 条断言，Linux 腿挂 4 条（同一测例的 retry 半边也挂）。
- 根因：QR 分支拒绝时把两件事**发了不等**——`markQrPaymentFailed` 只发
  `pay_payment` 的 FAIL 更新、`failQr` 只发幂等预留的删除，两个 `updateBy`/
  `deleteBy` 都没挂后续，紧接着就回调应答。于是客户端被告知"这单失败、可重试"
  时，库里那行还是 `INIT`（所有恢复过滤都读成"仍在途"），预留行也还在——
  **一次更正后的重试会因一笔渠道从未见过的支付被拒**。
- 为什么本地绿：断言本身没问题，问题在代码的时序。本地 Release 快，写赶在
  读之前落库；CI 慢（尤其 coverage 的 Debug+gcov）就露出。这是时序缺陷的典型
  形态，不能靠"本地多跑几次"当证据。
- 修复形状不是新发明：同文件主建单（jsapi/struct）路径自第三轮起就是
  `bookRefusedAttempt(db, paymentNo, orderNo, errPayload, done)`
  （`PaymentService.cc:360`）——把应答作为 `done` 续体传进去，写完才答；
  QR 的成功分支也早就 `promoteQrRows` → `respondQr`（幂等快照写完才答，
  见 `:1560` 注释）。**QR 的拒绝分支是这个文件里唯一的离群者。**
  本轮把应答接回这两处写的下游：`failQr`（`:1538`）从 `clearReservation`
  的回调里答；`markQrPaymentFailed`（`:1695`）新增 `afterClose` 续体，用
  `makeOnceCallback` 包住，经写回调或 `catch` 恰好一次触发（写失败的分支也
  必须答，否则请求永久悬置）。
- 测试零改动：钉的就是这个不变量，改代码即可，不需要动测例。

### 锚点漂移声明（本轮如实标注）
本轮 `PaymentService.cc` 在 1531 之后净增 26 行，因此 §十~§二十二 里凡是
`PaymentService.cc:1783/2403/2418/2464/2481/2542/2553/2595/2627/2875/2966/2992`
一类**旧快照锚点**都整体后移（`check_docs_drift.py` 的七条规则不校验行锚点，
故门禁不会红）。§二十五 引用的四处测例锚点与两处读数点锚点是本轮新增节的
主张，已逐条重测：`WechatPayClientTest.cc` `:922→:924`、`:972→:974`、
`:1049→:1051`、`:1131→:1133`；`PaymentService.cc:2357→:2383`、
`RefundService.cc:1471→:1478`（`ReconciliationService.cc:227` 未受影响）。
历史节锚点按"各轮当时实测"理解，需要精确位置时以内容 grep 为准。

### 验证记录
- `/WX` + `-DDROGON_PAY_WERROR=ON` 全量重建绿（含 `[[maybe_unused]]`、删 `this`、
  QR 续体三处改动）。
- 本机连跑 8 次套件：`PASS=8 FAIL=0`（220 例 / 2005 断言）。**注意**：修复前
  本机同样全绿，所以本机绿不是这条的证据；证据是顺序论证——应答改由写回调
  触发，行的可见状态在因果上先于响应，不再依赖快慢。最终裁决仍待 CI。
- 仓库 `clang_format.py --check`：本轮三个文件均 format-clean（本地唯一红是
  gitignore 掉的 `libs/drogon-pay/src/models_backup/`，CI 走 git 索引看不见）。

### 诚实边界与遗留
- 时序类缺陷的守卫仍是"测例 + CI"，没有静态化。若要更硬的证据，需要把
  `offerQrPayment` 的 DB 写注入屏障（测试内可强制延后），成本高，暂列后轮。
- 同文件其余 `clearReservation(..., [](bool){})` 的 fire-and-forget（`:1413`
  币种非法、`:1432` `time_expire` 非法、`:1518` 渠道不存在、`:1448`）**与本轮
  修的缺陷同形且同样可达**：这三处都在 `checkAndSetStatus`（`:1350`）取到预留
  **之后**应答，客户端按 400 更正参数立即重试时，删除尚未落地，重试会撞上
  "1004 idempotency request in progress"——被一笔已经失败的请求拒在门外。
  本轮**未动**它们，原因不是判断无碍，而是用户把本轮边界划在"CI 与评审意见
  修完即停"：改动范围不含新增测例。列为**下一轮首位候选**（修法与本轮同形，
  即把应答接进删除回调；三处 transport 文案与 body 文案不同形，收口时须逐处
  保留原串，不能顺手统一）。
- 第十九轮遗留的"应答身份字段须与所请求一致"仍是后轮首位候选。
