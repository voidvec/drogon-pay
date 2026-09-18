# 安全检查清单和加固指南

**目标环境：** Production

---

## 🔒 安全检查清单

### 1. API密钥管理

- [ ] **密钥存储**
  - [ ] 所有API密钥存储在环境变量或密钥管理服务（如AWS Secrets Manager、Azure Key Vault）
  - [ ] 不在代码中硬编码任何密钥
  - [ ] 不在配置文件中明文存储密钥（使用加密或环境变量）
  - [ ] 密钥文件权限设置为仅所有者可读（600或400）

- [ ] **密钥轮换**
  - [ ] 建立密钥定期轮换机制（建议每90天）
  - [ ] 有密钥泄露应急响应流程
  - [ ] 测试密钥与生产密钥分离

- [ ] **密钥权限**
  - [ ] 使用最小权限原则（Principle of Least Privilege）
  - [ ] 每个API密钥具有明确的scope限制
  - [ ] 定期审计API密钥的访问权限
  - [ ] 禁用未使用的API密钥

**实施建议：**

```bash
# 使用环境变量存储敏感信息
export WECHAT_PAY_MCH_ID="your_merchant_id"
export WECHAT_PAY_API_V3_KEY="your_api_v3_key"
export WECHAT_PAY_SERIAL_NO="your_serial_no"
export WECHAT_PAY_PRIVATE_KEY_PATH="/path/to/private/key"
```

config.json 中通过 `__env_var:VAR_NAME__` 占位符引用上述环境变量
（由示例宿主的 ConfigLoader 在启动时替换，敏感值不落盘）：

```json
{
  "channels": {
    "wechat": {
      "enabled": true,
      "mch_id": "__env_var:WECHAT_PAY_MCH_ID__",
      "api_v3_key": "__env_var:WECHAT_PAY_API_V3_KEY__",
      "serial_no": "__env_var:WECHAT_PAY_SERIAL_NO__",
      "private_key_path": "__env_var:WECHAT_PAY_PRIVATE_KEY_PATH__"
    }
  }
}
```

---

### 2. 数据保护

- [ ] **传输加密**
  - [ ] 所有外部API调用使用HTTPS（TLS 1.2+）
  - [ ] 禁用不安全的TLS版本和加密算法
  - [ ] 配置强密码套件（Cipher Suites）
  - [ ] 启用HSTS（HTTP Strict Transport Security）

- [ ] **敏感数据加密**
  - [ ] 数据库中敏感字段加密存储（如支付凭证）
  - [ ] 使用强加密算法（AES-256-GCM）
  - [ ] 密钥管理使用独立的密钥管理服务
  - [ ] 日志中不记录敏感信息（如完整银行卡号、CVV）

- [ ] **数据脱敏**
  - [ ] 日志中的敏感数据脱敏处理
  - [ ] 错误消息不泄露敏感信息
  - [ ] API响应中移除内部字段

**实施示例：**

```cpp
// 日志脱敏示例
std::string maskSensitiveData(const std::string& data) {
    if (data.length() <= 8) {
        return "****";
    }
    return data.substr(0, 4) + "****" + data.substr(data.length() - 4);
}

LOG_INFO << "Payment processed: order_no=" << orderNo
          << ", card=" << maskSensitiveData(cardNumber);
```

---

### 3. 认证和授权

- [ ] **API认证**
  - [ ] 所有API端点使用X-Api-Key认证（除公开端点）
  - [ ] API密钥验证在所有受保护端点上启用
  - [ ] 失败的认证尝试记录到日志
  - [ ] 实施速率限制防止暴力破解

- [ ] **权限范围（Scope）**
  - [ ] 每个API密钥具有明确的功能范围
  - [ ] Scope验证在授权检查中强制执行
  - [ ] 记录所有授权拒绝事件

- [ ] **会话管理**
  - [ ] 会话token使用安全的随机生成
  - [ ] 会话超时配置合理（如30分钟）
  - [ ] 会话token安全存储（HttpOnly, Secure标志）

**当前权限范围（Scope）配置：**

```cpp
// 实际生效的 scope（按路由路径相对 base_path 解析）：
// order_query   - 查询订单（GET {base}/query）
// refund        - 发起退款（POST {base}/refund）
// refund_query  - 查询退款（GET {base}/refund/query）
// reconcile     - 对账摘要（GET {base}/reconcile/summary）
```

配置位置：`config.json` 的 `custom_config.pay.api_key_scopes`（按密钥）与
`api_key_default_scopes`（未显式配置时的默认）。详见
[API Key 配置](../api/api_key_configuration.md)。

---

### 4. 输入验证

- [ ] **请求验证**
  - [ ] 所有用户输入进行验证和清理
  - [ ] 验证数据类型、长度、格式
  - [ ] 拒绝包含恶意字符的输入
  - [ ] 使用白名单而非黑名单验证

- [ ] **SQL注入防护**
  - [ ] 所有数据库查询使用参数化查询（ORM自动处理）
  - [ ] 不拼接SQL字符串
  - [ ] 使用最小权限的数据库用户

- [ ] **XSS防护**
  - [ ] 输出时进行HTML编码
  - [ ] 设置Content-Type: application/json
  - [ ] 启用CSP（Content Security Policy）

---

### 5. 安全头配置

在Drogon框架中配置安全头：

**config.json配置：**

```json
{
  "headers": {
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "X-XSS-Protection": "1; mode=block",
    "Strict-Transport-Security": "max-age=31536000; includeSubDomains",
    "Content-Security-Policy": "default-src 'none'",
    "Referrer-Policy": "no-referrer"
  }
}
```

**在代码中添加安全头：**

```cpp
// 在PayPlugin.cc或Filter中添加
void addSecurityHeaders(const drogon::HttpResponsePtr &resp) {
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "DENY");
    resp->addHeader("X-XSS-Protection", "1; mode=block");
    resp->addHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
    resp->addHeader("Referrer-Policy", "no-referrer");
}
```

---

### 6. 依赖安全

- [ ] **依赖扫描**
  - [ ] 定期扫描第三方依赖漏洞
  - [ ] 使用Snyk、OWASP Dependency-Check或类似工具
  - [ ] 及时更新有已知漏洞的依赖

- [ ] **Conan依赖管理**
  ```bash
  # 扫描Conan依赖
  conan inspect .. --format=json

  # 更新到最新安全版本
  conan create . --build=missing
  ```

- [ ] **已知漏洞检查**
  - [ ] OpenSSL版本检查（确保无已知漏洞）
  - [ ] Drogon框架版本检查
  - [ ] 其他C++库的安全版本验证

**当前依赖：**
- OpenSSL: 需验证版本（建议1.1.1+或3.0+）
- Drogon: 使用最新稳定版
- PostgreSQL客户端: 使用最新版本
- Redis客户端: 使用最新版本

---

### 7. 日志和审计

- [ ] **日志记录**
  - [ ] 所有认证失败记录
  - [ ] 所有授权拒绝记录
  - [ ] 敏感操作记录（支付创建、退款创建）
  - [ ] 系统错误和异常记录

- [ ] **审计日志**
  - [ ] 不可篡改的审计日志
  - [ ] 日志包含：时间戳、用户、操作、结果
  - [ ] 审计日志定期归档
  - [ ] 审计日志访问权限限制

- [ ] **日志保护**
  - [ ] 日志文件权限设置为仅所有者可写
  - [ ] 敏感信息不记录在日志中
  - [ ] 日志定期轮转和清理

**日志格式建议：**

```cpp
// 审计日志格式
LOG_INFO << "AUDIT: timestamp=" << timestamp
          << ", user=" << user_id
          << ", action=" << action
          << ", resource=" << resource
          << ", result=" << result
          << ", ip=" << client_ip
          << ", api_key=" << maskApiKey(api_key);
```

---

### 8. 网络安全

- [ ] **防火墙配置**
  - [ ] 仅开放必要端口（5566用于HTTP服务）
  - [ ] 数据库端口不对外暴露
  - [ ] Redis端口不对外暴露
  - [ ] 使用防火墙规则限制访问来源

- [ ] **DoS防护**
  - [ ] 实施速率限制（Rate Limiting）
  - [ ] 使用连接数限制
  - [ ] 启用Drogon的内置限流功能

- [ ] **入侵检测**
  - [ ] 监控异常请求模式
  - [ ] 检测暴力破解尝试
  - [ ] 记录可疑活动

**速率限制配置：**

示例宿主通过 `drogon::plugin::Hodor` 插件实现，在 config.json 中按 URL
正则配置（令牌桶算法），例如对 `/api/pay/create`、`/api/pay/refund` 分别
设更紧的 `ip_capacity`。详见 `examples/pay-server/config.json` 中的
`plugins[].Hodor` 配置块。

---

### 9. 配置安全

- [ ] **配置文件权限**
  - [ ] config.json权限设置为600
  - [ ] 不在版本控制中提交生产配置
  - [ ] 使用.gitignore排除敏感配置

- [ ] **环境隔离**
  - [ ] 开发、测试、生产环境分离
  - [ ] 不同环境使用不同的数据库凭据
  - [ ] 生产环境使用独立的API密钥

- [ ] **配置验证**
  - [ ] 启动时验证所有必需配置
  - [ ] 缺少关键配置时拒绝启动
  - [ ] 配置错误记录详细错误信息

**.gitignore配置：**

```gitignore
# 生产配置
config.production.json
.env.production

# 密钥文件
*.key
*.pem
secrets/

# 日志
*.log
logs/
```

---

### 10. 运维安全

- [ ] **访问控制**
  - [ ] 生产服务器使用SSH密钥认证
  - [ ] 禁用root直接登录
  - [ ] 使用堡垒机（Bastion Host）访问

- [ ] **更新和补丁**
  - [ ] 定期更新操作系统补丁
  - [ ] 定期更新应用依赖
  - [ ] 及时修复已知漏洞

- [ ] **备份安全**
  - [ ] 数据库备份加密
  - [ ] 备份存储在安全位置
  - [ ] 定期测试备份恢复

- [ ] **应急响应**
  - [ ] 建立安全事件响应流程
  - [ ] 安全事件联系方式
  - [ ] 定期进行安全演练

---

## 🔍 安全审计工具

### 1. 依赖漏洞扫描

**使用Trivy扫描容器镜像：**
```bash
# 安装Trivy
brew install trivy  # macOS
# 或下载二进制文件

# 扫描Docker镜像
trivy image your-image:tag

# 扫描文件系统
trivy fs /path/to/project
```

**使用Snyk扫描依赖：**
```bash
# 安装Snyk
npm install -g snyk

# 扫描项目
snyk test
```

### 2. 代码静态分析

**使用Cppcheck进行C++代码检查：**
```bash
# 安装Cppcheck
sudo apt-get install cppcheck  # Linux
brew install cppcheck  # macOS

# 运行静态分析
cppcheck --enable=all --inconclusive --std=c++17 examples/pay-server/
```

### 3. 配置安全检查

**使用Git-secrets防止敏感信息泄露：**
```bash
# 安装git-secrets
git clone https://github.com/awslabs/git-secrets.git
cd git-secrets
make install

# 配置规则
git secrets --install
git secrets --register-aws
git secrets --add 'password.*=.*'
git secrets --add 'api_key.*=.*'
```

---

## 📋 安全加固实施优先级

### P0 - 必须立即实施（高风险）

1. ✅ API密钥不在代码中硬编码
2. ✅ 所有生产通信使用HTTPS
3. ✅ 敏感数据不记录在日志中
4. ✅ 所有受保护端点启用认证
5. ⚠️ 配置安全头（X-Frame-Options, CSP等）

### P1 - 尽快实施（中风险）

6. ⚠️ 实施速率限制
7. ⚠️ 日志脱敏处理
8. ⚠️ 定期依赖漏洞扫描
9. ⚠️ 数据库敏感字段加密
10. ⚠️ 审计日志实施

### P2 - 计划实施（低风险）

11. ⏳ HSTS配置
12. ⏳ CSP策略实施
13. ⏳ 入侵检测系统
14. ⏳ 安全信息事件管理（SIEM）
15. ⏳ 定期安全审计

---

## 🛡️ 当前项目安全状态

### 已实施的安全措施

- ✅ API 密钥经环境变量 / config.json 的 `custom_config.pay` 管理，支持 scope 权限
- ✅ X-Api-Key（或 `Authorization: Bearer`）认证在所有受保护端点
- ✅ `checkAuth()` 函数在处理器内做认证和授权（取代旧的 drogon filter）
- ✅ 密钥校验使用常量时间比较（`OPENSSL_memcmp`），避免时序侧信道
- ✅ 幂等性键防止重复提交（Redis 可选，数据库兜底）
- ✅ 错误码统一管理，响应 body 不回显密钥
- ✅ `/healthz` + `/readyz` 健康检查端点
- ✅ 示例宿主通过 `drogon::plugin::Hodor` 对 `/api/pay/*` 等路由做速率限制
- ✅ 示例宿主 `SecurityHeaders.h` 设置安全响应头

### 待实施的安全措施

- ⚠️ 敏感字段加密存储
- ⚠️ 审计日志实施
- ⚠️ 依赖漏洞扫描

---

## 📚 参考资源

- [OWASP Top 10](https://owasp.org/www-project-top-ten/)
- [OWASP Cheat Sheet Series](https://cheatsheetseries.owasp.org/)
- [CWE/SANS Top 25 Most Dangerous Software Errors](https://cwe.mitre.org/top25/)
- [Drogon Framework Security Best Practices](https://drogon.docs.cybernostics.co.uk/)
- [OpenSSL Security Best Practices](https://www.openssl.org/docs/manmaster/man7/openssl-security.html)

