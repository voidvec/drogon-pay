# 微信支付和API Key配置完整指南

**创建时间：** 2026-04-13
**目的：** 配置PayPlugin的微信支付和内部API认证

---

## 📋 配置概述

PayPlugin需要**两类**配置：

### 1️⃣ 微信支付商户配置
**用途：** 调用微信支付API
**位置：** `config.json` → `plugins` → `PayPlugin` → `config` → `channels` → `wechat`

### 2️⃣ 内部API密钥配置
**用途：** 认证内部API调用（如 /api/pay/query）
**位置：** `config.json` → `custom_config` → `pay` → `api_keys`

---

## 🔑 第一部分：微信支付商户配置

### 必需的配置项

```json
{
  "plugins": [
    {
      "name": "PayPlugin",
      "config": {
        "channels": {
          "wechat": {
            "enabled": true,
            "app_id": "wx8888888888888888",
            "mch_id": "1234567890",
            "serial_no": "ABCDEFGHIJKLMNOPQRST",
            "api_v3_key": "your32characterbase64encodedkey==",
            "private_key_path": "./certs/apiclient_key.pem",
            "platform_cert_path": "./certs/wechatpay_platform.pem",
            "api_base": "https://api.mch.weixin.qq.com",
            "timeout_ms": 5000
          }
        }
      }
    }
  ]
}
```

### 配置项说明

| 配置项 | 说明 | 获取方式 | 示例值 |
|--------|------|----------|--------|
| **app_id** | 微信公众号/小程序AppID | 微信公众平台 | `wx8888888888888888` |
| **mch_id** | 微信支付商户号 | 微信商户平台 | `1234567890` |
| **serial_no** | **商户API证书**序列号，只用于出站请求的 `Authorization` 签名；回调验签不使用它 | 证书文件或商户平台 | `ABCDEFGHIJKLMNOPQRST` |
| **api_v3_key** | APIv3密钥（必须正好32字节，否则回调解密失败） | 商户平台设置 | `your32characterbase64encodedkey==` |
| **private_key_path** | 商户私钥路径 | 下载证书 | `./certs/apiclient_key.pem` |
| **platform_cert_path** | 平台证书静态兜底路径（可选）：仅当证书缓存未命中、且该证书自身的序列号与通知头 `Wechatpay-Serial` 一致时才使用 | 自动下载或手动 | `./certs/wechatpay_platform.pem` |
| **platform_ca_cert_path** | 平台证书校验的根证书（CA）bundle（可选）：配置后每张下载到的平台证书必须先通过链路校验才会入缓存 | 微信支付根证书 | `./certs/wechatpay_root_ca.pem` |
| **cert_download_min_interval_seconds** | 触发 `/v3/certificates` 下载的最小间隔（秒），默认 300 | - | `300` |
| **api_base** | 微信支付API地址 | 固定值 | `https://api.mch.weixin.qq.com` |
| **timeout_ms** | 每一个出站请求的超时时间（毫秒，填 0 表示不限时），默认 5000 | 可选 | `5000` |

**平台证书与轮换**：回调签名用的是通知头 `Wechatpay-Serial` 指名的**平台证书**，与商户证书（`serial_no`）不是一回事。
通道在进程启动时通过 `/v3/certificates` 预热，并拒绝任何解析失败、不在有效期内、序列号与所登记的号码不符、
或未通过 `platform_ca_cert_path` 链路校验的证书。收到缓存里没有的序列号时，说明微信已完成轮换：通道会触发一次
受节流保护的下载，并把该条通知按失败返回，微信按自身策略重投时新证书已在缓存中。这里没有独立的定时刷新。

### 如何获取微信支付配置

#### 步骤1：注册微信支付商户号
1. 访问 [微信支付商户平台](https://pay.weixin.qq.com/)
2. 使用微信扫码登录
3. 完成商户注册和认证
4. 获取商户号（mch_id）

#### 步骤2：获取AppID
1. 登录 [微信公众平台](https://mp.weixin.qq.com/)
2. 开发 → 基本配置 → AppID
3. 复制你的AppID

#### 步骤3：申请商户API证书
1. 登录微信支付商户平台
2. 账户中心 → API安全 → API证书
3. 申请证书（需要使用证书工具生成）
4. 下载证书文件（apiclient_cert.pem, apiclient_key.pem）

#### 步骤4：查看证书序列号
**方法1：从商户平台查看**
```
商户平台 → 账户中心 → API安全 → API证书 → 查看证书
```

**方法2：从证书文件提取**
```bash
openssl x509 -in apiclient_cert.pem -noout -serial
```

#### 步骤5：设置APIv3密钥
1. 商户平台 → 账户中心 → API安全 → APIv3密钥
2. 设置密钥（32个字符，支持数字+大小写字母）
3. 保存密钥（重要：无法再次查看）

#### 步骤6：放置证书文件
```bash
examples/pay-server/
├── certs/
│   ├── apiclient_cert.pem      # 商户证书
│   ├── apiclient_key.pem       # 商户私钥（保密！）
│   └── wechatpay_platform.pem  # 平台证书（自动下载）
```

---

## 🔐 第二部分：内部API密钥配置

### 为什么需要API Keys？

PayPlugin的某些API端点（如 `/api/pay/query`, `/api/pay/refund/query`）需要API密钥认证，防止未授权访问。

### 配置方式

```json
{
  "custom_config": {
    "pay": {
      "api_keys": [
        "test-api-key",
        "prod-api-key-123",
        "admin-key-456"
      ],
      "api_key_scopes": {
        "test-api-key": ["order_query", "refund", "refund_query"],
        "prod-api-key-123": ["order_query", "refund_query"],
        "admin-key-456": ["order_query", "refund", "refund_query", "reconcile"]
      },
      "api_key_default_scopes": ["read", "order_query", "refund", "refund_query", "reconcile"]
    }
  }
}
```

### 配置项说明

| 配置项 | 说明 | 示例 |
|--------|------|------|
| **api_keys** | 允许的API密钥列表 | `["test-api-key", "prod-key"]` |
| **api_key_scopes** | 每个密钥的权限范围 | `{"key": ["order_query", "refund"]}` |
| **api_key_default_scopes** | 密钥未显式配置 scope 时的默认权限 | `["read", "order_query", "refund", "refund_query", "reconcile"]` |

### 权限范围（Scopes）

Scope 按请求路径相对配置的 `base_path`（默认 `/api/pay`）解析。实际生效的 scope：

| Scope | 说明 | 守护的端点 |
|-------|------|-----------|
| **order_query** | 查询订单 | GET {base}/query |
| **refund** | 发起退款 | POST {base}/refund |
| **refund_query** | 查询退款 | GET {base}/refund/query |
| **reconcile** | 对账摘要 | GET {base}/reconcile/summary |

> 其余路由（创建支付 / 扫码支付 / metrics）不按 scope 守护，仅需有效 API Key。

### 如何使用API Keys

**客户端请求示例：**
```bash
curl "http://localhost:5566/api/pay/query?order_no=xxx" \
  -H "X-Api-Key: test-api-key"
```

**如果没有API Key：**
```bash
curl "http://localhost:5566/api/pay/query?order_no=xxx"
# 返回：HTTP 503 Service Unavailable
# Body（纯文本）: api key not configured
```

---

## 🧪 第三部分：测试环境配置

### 选项A：使用真实微信支付环境（需要真实商户号）

**适用场景：** 集成测试、预发布验证

```json
{
  "plugins": [
    {
      "name": "PayPlugin",
      "config": {
        "channels": {
          "wechat": {
            "enabled": true,
            "app_id": "wx8888888888888888",
            "mch_id": "1234567890",
            "serial_no": "ABCDEFGHIJKLMNOPQRST",
            "api_v3_key": "your32characterbase64encodedkey==",
            "private_key_path": "./certs/apiclient_key.pem",
            "platform_cert_path": "./certs/wechatpay_platform.pem",
            "api_base": "https://api.mch.weixin.qq.com",
            "timeout_ms": 5000,
            "notify_url": "https://your-domain.com/api/pay/notify/wechat"
          }
        }
      }
    }
  ],
  "custom_config": {
    "pay": {
      "api_keys": ["test-integration-key", "test-qa-key"],
      "api_key_scopes": {
        "test-integration-key": ["order_query", "refund", "refund_query", "reconcile"],
        "test-qa-key": ["order_query", "refund_query"]
      }
    }
  }
}
```

**优点：**
- ✅ 真实的支付流程
- ✅ 可以测试完整场景
- ✅ 接近生产环境

**缺点：**
- ❌ 需要真实的微信支付商户号
- ❌ 需要真实的证书
- ❌ 可能产生真实交易

### 选项B：使用Mock服务（推荐用于开发/测试）

**适用场景：** 开发、单元测试、本地调试

**步骤1：安装Mock服务**

使用 [wechat-pay-mock-server](https://github.com/wechatpay-apiv3/wechatpay-mock-server) 或类似工具。

**步骤2：配置Mock服务**

```json
{
  "plugins": [
    {
      "name": "PayPlugin",
      "config": {
        "channels": {
          "wechat": {
            "enabled": true,
            "app_id": "wx_mock_8888888888",
            "mch_id": "1230000109",
            "serial_no": "MOCK_SERIAL_NO_123",
            "api_v3_key": "mock32characterbase64encodedkey==",
            "private_key_path": "./certs/mock_apiclient_key.pem",
            "platform_cert_path": "./certs/mock_wechatpay_platform.pem",
            "api_base": "http://localhost:8080",  // Mock服务地址
            "timeout_ms": 5000,
            "notify_url": "http://localhost:5566/api/pay/notify/wechat"
          }
        }
      }
    }
  ],
  "custom_config": {
    "pay": {
      "api_keys": ["dev-test-key", "mock-test-key"],
      "api_key_scopes": {
        "dev-test-key": ["order_query", "refund", "refund_query", "reconcile"],
        "mock-test-key": ["order_query", "refund_query"]
      }
    }
  }
}
```

**优点：**
- ✅ 不需要真实商户号
- ✅ 快速测试
- ✅ 可控的测试场景
- ✅ 无交易费用

**缺点：**
- ❌ 无法测试真实支付流程
- ❌ 需要额外搭建Mock服务

### 选项C：仅启用内部API Key认证（最小配置）

**适用场景：** 仅测试内部API性能

```json
{
  "plugins": [
    {
      "name": "PayPlugin",
      "config": {
        "channels": {
          "wechat": {
            "enabled": true,
            "app_id": "PLACEHOLDER",
            "mch_id": "PLACEHOLDER",
            "serial_no": "PLACEHOLDER",
            "api_v3_key": "PLACEHOLDER_32_CHARACTER_KEY==",
            "private_key_path": "./certs/placeholder.pem",
            "platform_cert_path": "./certs/placeholder.pem",
            "api_base": "https://api.mch.weixin.qq.com",
            "timeout_ms": 5000
          }
        }
      }
    }
  ],
  "custom_config": {
    "pay": {
      "api_keys": [
        "perf-test-key-1",
        "perf-test-key-2",
        "perf-test-key-3"
      ],
      "api_key_scopes": {
        "perf-test-key-1": ["order_query", "refund", "refund_query", "reconcile"],
        "perf-test-key-2": ["order_query", "refund_query"],
        "perf-test-key-3": ["order_query"]
      }
    }
  }
}
```

**说明：**
- 微信支付配置使用占位符（因为不会真正调用）
- 内部API Key配置真实值（用于测试认证）
- **适合：** 性能测试、功能验证

---

## 🚀 快速配置指南（5分钟）

### 场景：本地开发/性能测试

#### 步骤1：修改 config.json

```bash
cd examples/pay-server
cp config.json config.json.backup
```

#### 步骤2：添加API Keys

编辑 `config.json`，找到 `custom_config.pay` 部分：

```json
{
  "custom_config": {
    "pay": {
      "api_keys": [
        "test-dev-key",
        "performance-test-key",
        "admin-key"
      ],
      "api_key_scopes": {
        "test-dev-key": ["order_query", "refund", "refund_query", "reconcile"],
        "performance-test-key": ["order_query", "refund_query"],
        "admin-key": ["order_query", "refund", "refund_query", "reconcile"]
      },
      "api_key_default_scopes": ["read", "order_query", "refund", "refund_query", "reconcile"]
    }
  }
}
```

#### 步骤3：创建占位符证书（如果文件不存在）

```bash
mkdir -p certs
openssl genrsa -out certs/apiclient_key.pem 2048
openssl req -new -x509 -key certs/apiclient_key.pem -out certs/apiclient_cert.pem -days 365 -subj "/CN=Placeholder"
```

#### 步骤4：重启PayServer

```bash
# 停止旧进程
taskkill /F /IM PayServer.exe

# 启动新进程
cd examples/pay-server
build/windows-msvc/examples/pay-server/Release/PayServer.exe
```

#### 步骤5：验证配置

```bash
# 测试有API Key
curl -H "X-Api-Key: test-dev-key" \
  "http://localhost:5566/api/pay/query?order_no=test_123"

# 应该返回：HTTP 200 + JSON响应（即使订单不存在）

# 测试无API Key
curl "http://localhost:5566/api/pay/query?order_no=test_123"

# 应该返回：HTTP 503 + 纯文本 "api key not configured"
```

---

## 🔍 配置验证

### 验证清单

- [ ] 证书文件存在且可读
- [ ] API keys已配置在 `custom_config.pay.api_keys`
- [ ] API key scopes已配置
- [ ] PayServer启动无错误
- [ ] 查询API（`/api/pay/query`）返回预期响应（需要API key）
- [ ] Prometheus metrics端点（`/metrics`）可访问（不需要API key）

### 常见配置错误

#### 错误1：API key未配置

**现象：**
```bash
curl "http://localhost:5566/api/pay/query?order_no=test"
HTTP 503: api key not configured
```

**解决：**
```json
{
  "custom_config": {
    "pay": {
      "api_keys": ["your-api-key"]
    }
  }
}
```
或设置环境变量 `PAY_API_KEY` / `PAY_API_KEYS`。

#### 错误2：证书文件不存在

**现象：**
```
ERROR: Failed to load private key from ./certs/apiclient_key.pem
```

**解决：**
```bash
# 检查文件是否存在
ls -la certs/apiclient_key.pem

# 或创建占位符证书（用于测试）
openssl genrsa -out certs/apiclient_key.pem 2048
```

#### 错误3：APIv3密钥格式错误

**现象：**
```
ERROR: Invalid API v3 key format
```

**解决：**
- 确保是32字节的base64编码
- 示例：`your32characterbase64encodedkey==`

#### 错误4：权限不足

**现象：**
```bash
curl -H "X-Api-Key: query-only-key" \
  -X POST http://localhost:5566/api/pay/refund \
  -d '{"order_no":"xxx","amount":"9.99"}'

HTTP 403: api key scope not allowed
```

**解决：**
```json
{
  "api_key_scopes": {
    "query-only-key": ["order_query", "refund"]  // 添加 "refund" 权限
  }
}
```

---

## 📝 配置模板

### 开发环境配置

```json
{
  "custom_config": {
    "pay": {
      "api_keys": [
        "dev-key-1",
        "dev-key-2"
      ],
      "api_key_scopes": {
        "dev-key-1": ["order_query", "refund", "refund_query", "reconcile"],
        "dev-key-2": ["order_query", "refund_query"]
      }
    }
  }
}
```

### 测试环境配置

```json
{
  "custom_config": {
    "pay": {
      "api_keys": [
        "test-query-key",
        "test-refund-key",
        "test-admin-key"
      ],
      "api_key_scopes": {
        "test-query-key": ["order_query", "refund_query"],
        "test-refund-key": ["order_query", "refund"],
        "test-admin-key": ["order_query", "refund", "refund_query", "reconcile"]
      }
    }
  }
}
```

### 生产环境配置

```json
{
  "custom_config": {
    "pay": {
      "api_keys": [
        // 不要在配置文件中硬编码生产密钥！
        // 使用环境变量或密钥管理服务
      ],
      "api_key_scopes": {
        // 从环境变量加载
      }
    }
  }
}
```

**生产环境最佳实践：**
```bash
# 使用环境变量
export PAY_API_KEY_READ="prod-read-key-xxx"
export PAY_API_KEY_WRITE="prod-write-key-yyy"

# 或使用密钥管理服务（如Vault）
```

---

## 🎯 总结

### 配置优先级

1. **立即配置（P0）**：
   - ✅ `api_keys` 数组
   - ✅ `api_key_scopes` 对象
   - **用途：** 解锁被阻塞的API端点

2. **按需配置（P1）**：
   - ⏸️ 微信支付商户配置
   - ⏸️ 证书文件
   - **用途：** 真实支付场景

3. **生产配置（P2）**：
   - ⏸️ 密钥轮换机制
   - ⏸️ 环境变量集成
   - **用途：** 生产环境部署

### 快速开始（性能测试）

**最小配置（5分钟）：**

1. 编辑 `config.json`：
```json
{
  "custom_config": {
    "pay": {
      "api_keys": ["perf-test-key"],
      "api_key_scopes": {
        "perf-test-key": ["order_query", "refund", "refund_query", "reconcile"]
      }
    }
  }
}
```

2. 重启PayServer

3. 运行性能测试：
```bash
curl -H "X-Api-Key: perf-test-key" \
  "http://localhost:5566/api/pay/query?order_no=test_123"
```

---

**文档位置：** `docs/api/api_configuration_guide.md`  
**配置文件：** `examples/pay-server/config.json`  
**证书目录：** `examples/pay-server/certs/`

**下一步：** 配置完成后，重新运行性能测试脚本！
