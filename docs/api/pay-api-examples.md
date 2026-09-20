# Pay API Examples

> **Machine-checked contract:** the authoritative definition of every route
> below is [`examples/pay-server/openapi.yaml`](../../examples/pay-server/openapi.yaml).
> `scripts/check_openapi_routes.py` (run by the `static-analysis` gate in
> `.github/workflows/ci.yml`) diffs that file against the routes the code
> registers, so a doc example that disagrees with the spec is caught in CI.
> Update the spec first, then this page.

> **Route prefix is configurable.** All pay routes are registered under the
> `base_path` key of the PayPlugin config block (default `/api/pay`); the
> examples below use the default. If your host sets a different `base_path`,
> substitute it accordingly. Exception: the QR create endpoint stays at the
> historical `/api/qrpay/create` path when `base_path` is the default (it
> becomes `{base_path}/qrpay/create` otherwise). See
> [plugin_integration.md](../development/plugin_integration.md) for the full
> route table.

> **Money is a decimal string in yuan units**, e.g. `"9.99"` or `"100"`. It is
> never a cent integer and never a JSON number; the format is
> `^\d+(\.\d{1,2})?$` and anything else is rejected with business code `40001`.

## Authentication

Protected routes accept `X-Api-Key: YOUR_API_KEY` or
`Authorization: Bearer YOUR_API_KEY`. When the server has no keys configured at
all it answers `503 api key not configured` rather than opening up. Scope
enforcement covers `order_query`, `refund`, `refund_query` and `reconcile`;
`/api/pay/orders` and the metrics routes are reachable with any valid key.

## Create Payment

```bash
curl -X POST http://localhost:5566/api/pay/create \
  -H "X-Api-Key: YOUR_API_KEY" \
  -H "Idempotency-Key: ORDER_ID-1" \
  -H "Content-Type: application/json" \
  -d '{
    "order_no": "c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82",
    "amount": "9.99",
    "currency": "CNY",
    "description": "Demo Order",
    "channel": "wechat",
    "user_id": 10001,
    "notify_url": "https://example.com/api/pay/notify/wechat"
  }'
```

`order_no` and `amount` are required; a body missing either one, or carrying any
field below it as the wrong JSON type (an object where a string is read), is
answered `400` with `code: 400`. `user_id` is required too, but answered
differently: it may come from the body or from a `user_id` request attribute —
which only exists if a middleware in front of this plugin sets it, and nothing in
this repository does — and it has to be a positive integer, so a call that names
no usable owner gets `401` with `code: 401`. It has to be positive because
`/api/pay/orders` reads `0` as "no owner filter": an order booked under `0` has no
owner, appears only in that unfiltered listing, and no owner-scoped query can
ever name it.
Response:

```json
{
  "code": 0,
  "message": "Payment created successfully",
  "data": {
    "order_no": "c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82",
    "payment_no": "PAY2026091810345612345678",
    "status": "PAYING",
    "code_url": "weixin://wxpay/bizpayurl?pr=xxxx",
    "prepay_id": "wx181034567890abcdef"
  }
}
```

The Alipay channel returns `qr_code` / `out_trade_no` / `alipay_response` in
`data` instead of `code_url` / `prepay_id` / `wechat_response`.

Note: `Idempotency-Key` (or `X-Idempotency-Key`) is optional but recommended for
retry safety. Omitting it makes the server derive
`payment:<order_no>:<user_id>:<sha256(amount + currency)>`, which still dedupes
but is invisible to the caller. A replay with a *changed* body is refused:
`/api/pay/refund` answers `409`, while the two create endpoints signal the same
collision with code `1004` over HTTP `404`. On `/api/qrpay/create` the derived
key is `QR_<order_no>_<channel>` unless the body or header supplies one.

## Create QR Payment

```bash
curl -X POST http://localhost:5566/api/qrpay/create \
  -H "X-Api-Key: YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "order_no": "ord-2026-0001",
    "amount": "1.00",
    "channel": "alipay",
    "user_id": 10001,
    "product_name": "Membership"
  }'
```

Unlike `/api/pay/create`, `channel` and `user_id` are mandatory here (all four
required members answer `400` when missing, mistyped, or — for `user_id` — not a
positive int64), and `product_name` becomes the channel `subject`; `description`
is accepted but unused on this route, since WeChat's `description` is filled from
that subject. `currency` (three letters, upper-cased for you, `CNY` by default
and forced to `CNY` on Alipay), `notify_url`, `buyer_id` and `idempotency_key` all
reach the booking: `notify_url` is checked against the same SSRF gate
`/api/pay/create` applies and then sent to WeChat — Alipay takes its callback
address from server configuration, so the member is inert there — and `buyer_id`
is an Alipay-only field.

What each refusal means — read `code`, the HTTP status is coarser:

| `code` | HTTP | Reason |
|--------|------|--------|
| `400` | 400 | Missing/mistyped member, a non-positive `user_id`, a currency that is not three letters, a private `notify_url`, or an existing order that is already settled or describes another amount, currency, channel or owner |
| `40001` | 400 | Amount shape the channels cannot represent (the same `^\d+(\.\d{1,2})?$` check the pay route runs) |
| `1004` | 404 | The idempotency key was taken by a *different* body, or is still in flight |
| `1005` | 500 | Unknown or unconfigured channel |
| `500` | 500 | The channel refused the request |
| `1003` | 500 | Database fault (including the idempotency check itself) |

A channel refusal closes that attempt's `pay_payment` row (`FAIL`) and leaves the
order alone, so the next call with the same `order_no` appends a new attempt and
gets its own code; an outcome that cannot be proved refused — a timeout, a
malformed answer — keeps the attempt in flight instead, because a code may well be
live on WeChat's side.

## Query Order

```bash
curl "http://localhost:5566/api/pay/query?order_no=c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82" \
  -H "X-Api-Key: YOUR_API_KEY"
```

```json
{
  "code": 0,
  "message": "Order found",
  "data": {
    "order_no": "c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82",
    "amount": "9.99",
    "currency": "CNY",
    "status": "PAID",
    "channel": "wechat",
    "title": "Demo Order",
    "user_id": 10001,
    "trade_no": "420000202609181234567890"
  }
}
```

This endpoint queries the channel live before answering. If that query fails
you still get the stored snapshot, but with `code: 1` (degraded) and a
`data.wechat_query_error` / `data.alipay_query_error` field — branch on those,
not on the HTTP status.

## List Orders

```bash
curl "http://localhost:5566/api/pay/orders?status=PAID&user_id=10001&limit=20&offset=0" \
  -H "X-Api-Key: YOUR_API_KEY"
```

```json
{
  "code": 200,
  "message": "Success",
  "data": [
    {
      "order_no": "c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82",
      "user_id": 10001,
      "amount": "9.99",
      "currency": "CNY",
      "status": "PAID",
      "channel": "wechat",
      "title": "Demo Order",
      "created_at": "2026-09-18 10:34:56",
      "updated_at": "2026-09-18 10:36:02",
      "payment_no": "PAY2026091810345612345678",
      "trade_no": "420000202609181234567890",
      "paid_at": "2026-09-18 10:36:02"
    }
  ]
}
```

Differences worth knowing: success is `code: 200` rather than `0`; `data` is
the row array itself with no total count; `limit` above 100 is clamped instead
of rejected; `paid_at` mirrors `updated_at` because the schema has no paid
timestamp column.

## Refund

```bash
curl -X POST http://localhost:5566/api/pay/refund \
  -H "X-Api-Key: YOUR_API_KEY" \
  -H "Idempotency-Key: REFUND-ORDER_ID-1" \
  -H "Content-Type: application/json" \
  -d '{
    "order_no": "c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82",
    "payment_no": "PAY2026091810345612345678",
    "amount": "9.99",
    "reason": "Customer request"
  }'
```

```json
{
  "code": 0,
  "message": "Refund created successfully",
  "data": {
    "refund_no": "RF2026091810370012345678",
    "order_no": "c2d7c8ad-6a4b-4f3f-9a31-32db0a3fcd82",
    "payment_no": "PAY2026091810345612345678",
    "refund_amount": "9.99",
    "status": "REFUND_SUCCESS",
    "channel_refund_no": "503000000000000000000000001"
  }
}
```

Refunding more than the unpaid remainder is rejected before the channel is
called; an upstream channel failure surfaces as HTTP `502` with code `1502`.

## Refund Query

```bash
curl "http://localhost:5566/api/pay/refund/query?refund_no=RF2026091810370012345678" \
  -H "X-Api-Key: YOUR_API_KEY"
```

`data` carries `refund_no`, `order_no`, `payment_no`, `status`, `refund_amount`,
`channel_refund_no` and an RFC 3339 UTC `updated_at`; for the wechat channel a
successful channel query also refreshes `data.status` and appends
`data.wechat_response`.

## Reconciliation Summary

```bash
curl "http://localhost:5566/api/pay/reconcile/summary" \
  -H "X-Api-Key: YOUR_API_KEY"
```

```json
{
  "code": 0,
  "message": "Reconciliation summary",
  "data": {
    "paying_orders": 2,
    "refunding_refunds": 0,
    "oldest_paying_updated": "2026-09-18 10:34:56",
    "oldest_refund_updated": ""
  }
}
```

The optional `date` parameter is accepted by the controller but **discarded by
the service**, which always counts the whole table. Treat this as an
"anything stuck right now" gauge, not a daily report, until that is fixed.

## Auth Metrics

```bash
curl "http://localhost:5566/api/pay/metrics/auth" -H "X-Api-Key: YOUR_API_KEY"
```

`{"missing_key":0,"invalid_key":3,"scope_denied":0,"not_configured":0}` —
process-lifetime counters, not persisted.

## Auth Metrics (Prometheus)

```bash
curl "http://localhost:5566/api/pay/metrics/auth.prom" -H "X-Api-Key: YOUR_API_KEY"
```

## Combined Metrics (Prometheus)

```bash
curl "http://localhost:5566/metrics"
```

Loopback only (`drogon::LocalHostFilter`); no API key. It appends the pay auth
counters to the Drogon `PromExporter` page configured as `/metrics/base`.

## WeChat Pay Callback (Sample Body)

```json
{
  "id": "4200000000000000000000000000",
  "create_time": "2026-09-18T10:34:56+08:00",
  "resource_type": "encrypt-resource",
  "event_type": "TRANSACTION.SUCCESS",
  "summary": "Payment successful",
  "resource": {
    "algorithm": "AEAD_AES_256_GCM",
    "ciphertext": "BASE64_CIPHERTEXT_WITH_TAG",
    "nonce": "RANDOM_NONCE",
    "associated_data": "transaction"
  }
}
```

Required headers for signature verification: `Wechatpay-Timestamp`,
`Wechatpay-Nonce`, `Wechatpay-Signature`, `Wechatpay-Serial`.

This body is **WeChat's agreed format, not this project's contract** — it is
passed through after verification and may change without a change here. Only
`event_type` is interpreted for routing (`TRANSACTION.SUCCESS`, or anything
containing `REFUND`); unknown types get `400` with code `40004`. Acknowledgement
is `{"code":"SUCCESS","message":"OK"}`; verification or persistence failures
answer `{"code":"FAIL",...}` with HTTP `500` so WeChat retries.

## Alipay Callback

Alipay posts `application/x-www-form-urlencoded`, not JSON:

```
out_trade_no=ord-2026-0001&trade_no=2026091822001412345678901234
&trade_status=TRADE_SUCCESS&total_amount=9.99&app_id=2021000000000000
&seller_id=2088000000000000&notify_time=2026-09-18 10:34:56
&notify_type=trade_status_sync&notify_id=2026091800012345678901234567890123
&sign_type=RSA2&sign=BASE64_SIGNATURE
```

`sign` is verified over the remaining sorted parameters before any state change,
and a failed verification is rejected. As with WeChat, the body is a
channel-defined format rather than this project's contract.

**Known deviation:** Alipay expects the literal string `success` as the
acknowledgement. This server answers a JSON body instead, and answers HTTP `200`
even when it rejects the notification — which Alipay reads as accepted. Do not
build a retry-dependent integration on this response until that is fixed.
