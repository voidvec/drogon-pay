# Pay Plugin Configuration Guide

## Configuration File Location

Configuration is loaded from `config.json` (the example host lives in
`examples/pay-server/config.json`). Sensitive values use `__env_var:NAME__`
placeholders that the host's `ConfigLoader` substitutes from the environment
at startup, so the file itself stays safe to commit.

## Configuration Structure

```json
{
  "listeners": [
    {
      "address": "0.0.0.0",
      "port": 5566,
      "https": false
    }
  ],
  "db_clients": [
    {
      "name": "default",
      "rdbms": "postgresql",
      "host": "127.0.0.1",
      "port": 5432,
      "dbname": "pay_test",
      "user": "test",
      "passwd": "__env_var:PAY_DB_PASSWORD__",
      "number_of_connections": 4
    }
  ],
  "redis_clients": [
    {
      "name": "default",
      "host": "127.0.0.1",
      "port": 6379,
      "passwd": "__env_var:PAY_REDIS_PASSWORD__",
      "db": 0,
      "number_of_connections": 4
    }
  ],
  "plugins": [
    {
      "name": "PayPlugin",
      "dependencies": [],
      "config": {
        "base_path": "/api/pay",
        "db_client": "default",
        "redis_client": "default",
        "idempotency_ttl_seconds": 604800,
        "reconcile": {
          "enabled": true,
          "interval_seconds": 300,
          "batch_size": 50
        },
        "channels": {
          "wechat": {
            "enabled": true,
            "app_id": "__env_var:WECHAT_PAY_APP_ID__",
            "mch_id": "__env_var:WECHAT_PAY_MCH_ID__",
            "serial_no": "__env_var:WECHAT_PAY_SERIAL_NO__",
            "api_v3_key": "__env_var:WECHAT_PAY_API_V3_KEY__",
            "private_key_path": "__env_var:WECHAT_PAY_PRIVATE_KEY_PATH__",
            "platform_cert_path": "__env_var:WECHAT_PAY_PLATFORM_CERT_PATH__",
            "platform_ca_cert_path": "__env_var:WECHAT_PAY_PLATFORM_CA_CERT_PATH__",
            "cert_download_min_interval_seconds": 300,
            "notify_url": "__env_var:WECHAT_PAY_NOTIFY_URL__",
            "api_base": "https://api.mch.weixin.qq.com",
            "timeout_ms": 5000
          },
          "alipay": {
            "enabled": true,
            "app_id": "__env_var:ALIPAY_SANDBOX_APP_ID__",
            "gateway_url": "__env_var:ALIPAY_SANDBOX_GATEWAY_URL__",
            "private_key_path": "__env_var:ALIPAY_SANDBOX_PRIVATE_KEY_PATH__",
            "alipay_public_key_path": "__env_var:ALIPAY_SANDBOX_PUBLIC_KEY_PATH__",
            "notify_url": "http://localhost:5566/api/pay/notify/alipay",
            "timeout_ms": 30000
          }
        }
      }
    }
  ],
  "custom_config": {
    "pay": {
      "api_keys": [],
      "api_key_scopes": {},
      "api_key_default_scopes": ["read", "order_query", "refund", "refund_query", "reconcile"],
      "metrics_base_url": "http://127.0.0.1:5566/metrics/base"
    }
  }
}
```

> Database / Redis / listener blocks are standard Drogon config
> (`db_clients`, `redis_clients`, `listeners`); the plugin only references them
> by name via `db_client` / `redis_client`.

## Configuration Parameters

### Plugin-level (`PayPlugin.config`)

| Parameter | Description | Required | Default |
|-----------|-------------|----------|---------|
| `base_path` | Route prefix for all pay routes | No | `/api/pay` |
| `db_client` | Name of the Drogon `db_clients` entry (PostgreSQL) | No | `default` |
| `redis_client` | Name of the Drogon `redis_clients` entry (**opt-in**) | No | (omitted → DB-only idempotency) |
| `idempotency_ttl_seconds` | Idempotency record TTL (seconds) | No | 604800 (7 days) |
| `reconcile.enabled` | Run the scheduled reconciliation timer | No | false |
| `reconcile.interval_seconds` | Reconcile interval | No | 300 |
| `reconcile.batch_size` | Reconcile batch size | No | 50 |
| `channels.<name>.enabled` | Enable a channel; unknown/disabled → `CHANNEL_NOT_AVAILABLE` | No | false |

### WeChat Pay (`channels.wechat`)

| Parameter | Description | Required | Default |
|-----------|-------------|----------|---------|
| `app_id` | WeChat AppID | Yes | - |
| `mch_id` | Merchant ID | Yes | - |
| `api_v3_key` | API v3 Key; must be exactly 32 bytes, callbacks cannot be decrypted without it | Yes | - |
| `serial_no` | **Merchant** API certificate serial number. Sent in the `Authorization` header of outbound calls; it has no part in callback verification | Yes | - |
| `private_key_path` | Merchant private key path | Yes | - |
| `notify_url` | Payment callback URL | Yes | - |
| `platform_cert_path` | Static platform certificate, used as a fallback only when the cache is cold *and* the certificate's own serial matches the notification's `Wechatpay-Serial` | No | - |
| `platform_ca_cert_path` | Trust anchor bundle; when set, a downloaded platform certificate must chain to it before it is cached | No | - (chain check skipped) |
| `cert_download_min_interval_seconds` | Minimum gap between `/v3/certificates` downloads (floor: 1s) | No | 300 |
| `cert_refresh_interval_seconds` | Periodic certificate refresh, read by `PayPlugin`'s timer (values below 300 are refused with a warning) | No | 43200 |
| `api_base` | WeChat API base URL | No | https://api.mch.weixin.qq.com |
| `timeout_ms` | Per-request timeout applied to every outbound WeChat call (`0` disables it) | No | 5000 |

#### Platform certificates and rotation

Callback signatures are verified with the platform certificate named by the
notification's `Wechatpay-Serial` header, which is a different certificate (and
a different numbering space) from the merchant certificate named by
`serial_no`. The channel warms the certificate set at start-up through
`/v3/certificates`, refuses to cache a certificate that does not parse as
X.509, is outside its validity window, carries a serial other than the one it
is filed under, or fails the optional `platform_ca_cert_path` chain check.

Two mechanisms keep the cache current. When a notification names a serial that
is not cached, WeChat has rotated: the channel triggers a throttled download
and answers that notification as a failure, so WeChat retries it and finds the
new certificate already cached. Independently of that, `PayPlugin` re-downloads
the set on a timer every `cert_refresh_interval_seconds` (default 43200, below
300 refused with a warning), so an idle process does not sit on a stale cache;
`onStart()` covers the restart case with a warm-up.

### Alipay (`channels.alipay`)

| Parameter | Description | Required | Default |
|-----------|-------------|----------|---------|
| `app_id` | Alipay AppID | Yes | - |
| `gateway_url` | Gateway URL | Yes | - |
| `private_key_path` | Application private key path | Yes | - |
| `alipay_public_key_path` | Alipay public key path | Yes | - |
| `notify_url` | Payment callback URL | Yes | - |
| `timeout_ms` | API timeout | No | 30000 |

### Database / Redis (Drogon `db_clients` / `redis_clients`)

These are standard Drogon connection-pool blocks, not plugin-specific keys. The
plugin picks the entry whose `name` matches `db_client` / `redis_client`.

### API Key / auth (`custom_config.pay`)

| Parameter | Description | Required | Default |
|-----------|-------------|----------|---------|
| `api_keys` | Allowed API keys | No (can also come from env) | `[]` |
| `api_key_scopes` | Per-key scopes | No | `{}` |
| `api_key_default_scopes` | Scopes used when a key has no explicit entry | No | (host-defined) |

Enforced scopes (resolved from the path under `base_path`): `order_query`,
`refund`, `refund_query`, `reconcile`. See
[API Key Configuration](../api/api_key_configuration.md).

## Environment Variables

Sensitive values are referenced with the `__env_var:NAME__` placeholder and
loaded from the environment (the example host reads a `.env` file via
`ConfigLoader`):

```json
{
  "channels": {
    "wechat": {
      "api_v3_key": "__env_var:WECHAT_PAY_API_V3_KEY__",
      "private_key_path": "__env_var:WECHAT_PAY_PRIVATE_KEY_PATH__"
    }
  }
}
```

Required by the example host (`StartupValidator`): `PAY_DB_PASSWORD`,
`PAY_API_KEY`. `PAY_REDIS_PASSWORD` is optional. See
[environment_setup.md](environment_setup.md) for the full list.

A missing channel variable is not fatal, because a partial rollout (one channel
configured, another not) has to keep booting — but `app_id` for an `enabled`
channel is reported as a `LOG_WARN` at startup (`validateChannelReadiness`).
Without it the merchant-identity re-check on incoming Alipay notifications
enforces nothing, since it only compares against an id it knows.

## Validation

Configuration is validated on startup:
- Required parameters must be present
- An enabled channel whose `app_id` never resolved is warned about (not fatal)
- Certificate files must exist
- Database connection must succeed
- Redis connection must succeed (when `redis_client` is configured)

If validation fails:
1. Check log output for specific error
2. Verify file paths are correct
3. Ensure database is running
4. Confirm Redis is running (or omit `redis_client` to run DB-only)
