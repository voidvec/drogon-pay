# drogon-pay 宿主集成指南（Plugin Integration Guide）

`drogon-pay` 是一个可复用的 Drogon 支付插件库（STATIC 库）。任意 Drogon 应用
通过 **Conan 依赖 + `find_package(DrogonPay)` + config.json 插件块** 即可获得完整
的支付能力（创建/查询/退款/回调/对账/幂等），无需拷贝任何业务代码。

- 库目标：`DrogonPay::DrogonPay`（CMake 包名 `DrogonPay`，Conan 包 `drogon-pay/1.0.0`）
- 公共 API 仅 4 个头文件：`drogon_pay/PayPlugin.h`、`drogon_pay/PaymentChannel.h`、
  `drogon_pay/ChannelRegistry.h`、`drogon_pay/PayErrorCategory.h`
- 参考宿主：[examples/pay-server](../../examples/pay-server/)（完整宿主）与
  [test_package/main.cc](../../test_package/main.cc)（最小可运行宿主）

## Drogon 版本兼容矩阵

| drogon-pay | Drogon | 说明 |
|---|---|---|
| 1.0.x | **1.9.13（钉死）** | 静态库 + C++ ABI 决定库与宿主**必须使用同一 Drogon 版本**；Drogon 升级将随本库 minor 版本发布 |

## 一、宿主接入五步

### 步骤 1：声明 Conan 依赖

宿主 `conanfile.py`（或 conanfile.txt）中：

```python
def requirements(self):
    self.requires("drogon-pay/1.0.0")
    # drogon/1.9.13、openssl、jsoncpp 会作为传递依赖自动带入
```

```bash
conan install . --output-folder=build -s build_type=Release -s compiler.cppstd=17 --build=missing
```

### 步骤 2：CMake 链接

```cmake
find_package(DrogonPay CONFIG REQUIRED)

add_executable(my_server main.cc)
target_link_libraries(my_server PRIVATE DrogonPay::DrogonPay)
```

`DrogonPay::DrogonPay` 会传递 Drogon/OpenSSL/JsonCpp 的头与库，宿主无需重复声明。

**静态链接安全网**：`PayPlugin` 通过 Drogon 的 DrObject 机制自注册。若宿主链接器
裁剪了该符号（表现为 `drogon::app().getPlugin<PayPlugin>()` 返回空），在 `main()`
中调用一次 `drogon_pay::ensureLinked();` 即可强制保留目标文件。

### 步骤 3：config.json 增加插件块

```json
{
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
            "app_id": "...",
            "mch_id": "...",
            "serial_no": "...",
            "api_v3_key": "...",
            "private_key_path": "certs/apiclient_key.pem",
            "platform_cert_path": "certs/platform_cert.pem",
            "notify_url": "https://your.host/api/pay/notify/wechat",
            "api_base": "https://api.mch.weixin.qq.com",
            "timeout_ms": 5000
          },
          "alipay": {
            "enabled": true,
            "app_id": "...",
            "seller_id": "...",
            "private_key_path": "certs/alipay_private_key.pem",
            "alipay_public_key_path": "certs/alipay_public_key.pem",
            "gateway_url": "https://openapi-sandbox.dl.alipaydev.com/gateway.do",
            "notify_url": "https://your.host/api/pay/notify/alipay",
            "timeout_ms": 30000
          }
        }
      }
    }
  ]
}
```

配置键说明：

| 键 | 默认值 | 说明 |
|---|---|---|
| `base_path` | `"/api/pay"` | 所有支付路由的前缀，可配置（见下方路由表） |
| `db_client` | `"default"` | Drogon `db_clients` 中的名字（PostgreSQL） |
| `redis_client` | **无（可选）** | **opt-in**：仅当显式写出该键才启用 Redis 幂等缓存；不写则幂等退化为纯数据库路径。切勿写出一个未在 `redis_clients` 中配置的名字 |
| `idempotency_ttl_seconds` | `604800` | 幂等键保留时长 |
| `reconcile.enabled` | `false` | 对账定时器（跑在插件独立 worker 线程，不占 IO loop） |
| `channels.<name>.enabled` | `false` | 渠道开关；未启用/未注册渠道的请求返回 `CHANNEL_NOT_AVAILABLE`（**没有兜底渠道**） |

### 步骤 4：执行迁移建表

用仓库根的唯一执行器 [scripts/migrate_db.py](../../scripts/migrate_db.py) 应用
[sql/](../../sql/) 下的迁移（PostgreSQL）：

```bash
python3 scripts/migrate_db.py --host 127.0.0.1 --user <user> --db <dbname>
python3 scripts/migrate_db.py --host 127.0.0.1 --user <user> --db <dbname> --status   # 看已应用版本
python3 scripts/migrate_db.py --host 127.0.0.1 --user <user> --db <dbname> --dry-run  # 看下次会跑什么
```

凭据从环境变量取（`PGPASSWORD`，或应用配置同名的 `PAY_DB_PASSWORD`），命令行不
要写口令。执行器会自行发现 `sql/NNN_*.sql`、按版本号补齐未应用的部分，每条迁移
与其 `schema_migrations` 记录在同一事务内提交；若某个已应用的版本被改过字节，它会
直接拒绝执行。

`sql/000_*.sql` 是**测试环境重置助手**，不在版本链里，执行器会跳过它——这也正是
部署脚本曾经把它当迁移跑、每次部署先删一遍表的原因。

### 步骤 5：启动并验证

```cpp
#include <drogon/drogon.h>
#include <drogon_pay/PayPlugin.h>

int main()
{
    drogon_pay::ensureLinked();  // 静态链接安全网（可选但推荐）
    drogon::app().loadConfigFile("config.json");
    drogon::app().run();
}
```

启动日志会打印已注册渠道清单与路由前缀：

```
PayPlugin routes registered under base path '/api/pay' (QR create: '/api/qrpay/create')
```

验证路由可达：`curl http://localhost:5566/api/pay/query?order_no=x -H "X-Api-Key: ..."`
（任何非 404 响应即证明插件路由已注册）。

## 二、注册的路由（全部程序化注册）

`{base}` 为 `base_path` 配置值，默认 `/api/pay`。除回调路由外全部经
`checkAuth`（X-Api-Key + scope）前置校验：

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `{base}/create` | 创建支付 |
| POST | `/api/qrpay/create`* | 创建扫码支付（*历史路径：仅当 `base_path` 为默认值时保持原样；否则为 `{base}/qrpay/create`） |
| GET | `{base}/query` | 查询订单 |
| POST | `{base}/refund` | 发起退款 |
| GET | `{base}/refund/query` | 查询退款 |
| GET | `{base}/orders` | 订单列表 |
| GET | `{base}/reconcile/summary` | 对账摘要 |
| GET | `{base}/metrics/auth` | 鉴权指标（JSON） |
| GET | `{base}/metrics/auth.prom` | 鉴权指标（Prometheus 文本） |
| POST | `{base}/notify/wechat` | 微信回调（签名验证，无 API Key） |
| POST | `{base}/notify/alipay` | 支付宝回调（签名验证，无 API Key） |

宿主专属端点（`/healthz`、`/metrics`、CORS/安全头）**不属于库**，参考
`examples/pay-server` 自行实现。

## 三、旧 → 新配置键映射表（v1.0 破坏性变更）

检测到以下旧键时插件会**拒绝启动**并打印指向本文档的迁移错误：

| 旧键（已移除） | 新键 | 备注 |
|---|---|---|
| `wechat_pay: {...}` | `channels.wechat: {...}` | 块内字段名不变，另加 `enabled: true` |
| `alipay_sandbox: {...}` | `channels.alipay: {...}` | 块内字段名不变，另加 `enabled: true` |
| `redis_client`（曾默认 `"default"`） | `redis_client`（**可选 opt-in**） | 无 Redis 的宿主直接省略该键即可；旧行为会对未配置的名字触发 Drogon RedisClientManager 的退出期崩溃 |
| —（路由硬编码 `/api/pay/*`） | `base_path` | 新增，可配置路由前缀 |
| —（`ADD_METHOD_TO` 静态注册 + `PayAuthFilter`） | 程序化注册 + `checkAuth()` | 宿主无需 WHOLE_ARCHIVE，无 filter 名依赖 |

其他行为变更：

- **未知/未启用渠道显式返回 `CHANNEL_NOT_AVAILABLE`**，不再兜底到微信
- `setTestClients(...)` 保留为兼容适配器，新测试代码请用
  `setTestChannels(std::map<std::string, PaymentChannelPtr>, dbClient)`

## 四、自定义渠道开发指南

渠道扩展 = **实现 SPI + 注册工厂 + config 启用**，无需修改库内任何代码。

### 1. 实现 `drogon_pay::PaymentChannel`

```cpp
#include <drogon_pay/PaymentChannel.h>

class MyChannel : public drogon_pay::PaymentChannel
{
  public:
    explicit MyChannel(const Json::Value &config);

    const std::string &name() const override;      // "mychannel"
    bool isConfigured() const override;

    void createPayment(const Json::Value &payload, JsonCallback &&cb) override;
    void createQRPayment(const Json::Value &payload, JsonCallback &&cb) override;
    void queryPayment(const std::string &orderNo, JsonCallback &&cb) override;
    void refund(const Json::Value &payload, JsonCallback &&cb) override;
    void queryRefund(const std::string &refundNo, JsonCallback &&cb) override;

    bool verifyCallback(const drogon::HttpRequestPtr &req,
                        drogon_pay::CallbackEvent &event,
                        std::string &error) override;

    void onStart() override;  // 可选：预热证书/密钥
    void onStop() override;   // 可选：排空在途任务
};
```

**接口契约（必须遵守）**：

- **线程安全**：所有方法会被多个 Drogon IO 线程并发调用；可变状态用
  `shared_ptr` 快照 + 原子替换（参考内置微信渠道的证书刷新实现）
- **复用 HttpClient**：出站 HTTP 必须每 IO loop 一个客户端
  （`drogon::IOThreadStorage<HttpClientPtr>` + keep-alive），禁止逐请求新建
- `verifyCallback` 负责验签 + 解密 + 把渠道字段归一化进 `CallbackEvent`
  （`orderNo`/`paid`/`amountTotal` 等），使回调服务保持渠道无关
- 非对称能力（如微信证书刷新）留在具体类上，调用方经
  `std::dynamic_pointer_cast` 显式获取——不要塞进 SPI

### 2. 在 `app().run()` 之前注册工厂

```cpp
#include <drogon_pay/ChannelRegistry.h>

int main()
{
    drogon_pay::ChannelRegistry::registerFactory(
        "mychannel",
        [](const Json::Value &config) -> drogon_pay::PaymentChannelPtr {
            return std::make_shared<MyChannel>(config);
        });

    drogon::app().loadConfigFile("config.json");
    drogon::app().run();
}
```

### 3. config.json 启用

```json
"channels": {
  "mychannel": { "enabled": true, "app_id": "...", "timeout_ms": 5000 }
}
```

`PayPlugin::initAndStart` 会以该 JSON 块调用工厂，注册完成后**冻结注册表**
（运行期无锁只读查找）。此后 `POST {base}/create` 携带
`"channel": "mychannel"` 即路由到你的实现。

### 注册表生命周期

1. **注册阶段**（单线程）：`initAndStart` 内先注册内置渠道
   （显式 `registerBuiltinChannels()`，不用自注册宏——静态库会丢符号），
   再消费宿主注册的工厂
2. **冻结**：`freeze()` 后注册表不可变
3. **运行期**：`find()` 无锁；未知渠道返回 `nullptr` → 服务层映射为
   `CHANNEL_NOT_AVAILABLE`

## 五、常见问题

- **`getPlugin<PayPlugin>()` 返回空** → 见步骤 2 的 `ensureLinked()` 安全网
- **启动即抛 `legacy config key ... detected`** → 按第三节映射表迁移配置
- **退出时 0xC0000005（仅无 Redis 宿主）** → 升级到含 redis opt-in 修复的
  版本，并确认 config 中没有指向不存在客户端的 `redis_client` 键
- **想改 API 前缀但前端已固定 `/api/pay`** → 保持 `base_path` 默认值即可，
  `examples/pay-admin` 前端零改动
