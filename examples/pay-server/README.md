# pay-server — drogon-pay 示例宿主

演示如何在一个真实 Drogon 应用中集成 [drogon-pay](../../libs/drogon-pay/) 插件库。
本目录**不是发布物**：它承载的全部是宿主关注点，供接入方参考照抄。

## 宿主 vs 库的职责边界

| 本示例宿主负责（不在库内） | drogon-pay 库负责 |
|---|---|
| `.env` 加载与 config.json 占位符替换（`ConfigLoader`） | PayPlugin 装配：渠道注册 → 服务构造 → 路由程序化注册 |
| 启动前置校验（`StartupValidator`） | 支付/退款/回调/对账/幂等全部业务逻辑 |
| CORS 与安全响应头（`SecurityHeaders.h`） | 微信/支付宝内置渠道（含证书刷新、HttpClient 复用） |
| `/healthz`、`/metrics` 端点 | `{base_path}/*` 支付路由 + API Key 鉴权 |
| 证书文件、部署脚本、Docker | — |

依赖方向单向：宿主 → 库公共头（`drogon_pay/*`）。库不反向依赖本目录任何代码。

## 运行

### 1. 前置条件

- PostgreSQL 13+ 与 Redis 6+（本示例启用了 Redis 幂等缓存；纯数据库幂等
  可从 config.json 删除 `redis_client` 键）
- 表结构用仓库根的唯一执行器建：`examples/pay-server/scripts/setup_database.sh`
  （Windows 用同目录 `.bat`），它调用 `scripts/migrate_db.py` 重置 schema 并按
  版本号重放 `sql/` 链，同时在 `schema_migrations` 里记账

### 2. 配置环境变量

```bash
cp .env.example .env   # 填入数据库口令、渠道密钥等
```

config.json 中所有 `__env_var:NAME__` 占位符由 `ConfigLoader` 在启动时替换，
敏感信息不落盘进配置文件。

### 3. 构建与启动（仓库根目录）

```bash
conan install . --output-folder=build/windows-msvc -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset windows-msvc
cmake --build --preset windows-msvc
build\windows-msvc\examples\pay-server\Release\PayServer.exe   # 工作目录须能找到 config.json/.env
```

（Linux/macOS 将 preset 换成对应平台即可；构建由根 CMake 的
`DROGON_PAY_BUILD_EXAMPLES` 开关控制，默认 ON。）

### 4. 验证

```bash
curl http://localhost:5566/healthz
curl "http://localhost:5566/api/pay/query?order_no=x" -H "X-Api-Key: <key>"   # 非 404 即路由已注册
```

配套管理界面见 [examples/pay-admin](../pay-admin/)（Vue 3，默认消费
`/api/pay/*` 前缀，与本宿主的默认 `base_path` 一致）。

## config.json 要点（新 channels schema）

```json
"plugins": [{
  "name": "PayPlugin",
  "config": {
    "base_path": "/api/pay",
    "db_client": "default",
    "redis_client": "default",
    "idempotency_ttl_seconds": 604800,
    "reconcile": { "enabled": true, "interval_seconds": 300, "batch_size": 50 },
    "channels": {
      "wechat": { "enabled": true, "...": "..." },
      "alipay": { "enabled": true, "...": "..." }
    }
  }
}]
```

- 旧版 `wechat_pay` / `alipay_sandbox` 顶层块已移除，插件检测到会拒绝启动；
  完整键映射与自定义渠道开发见
  [docs/development/plugin_integration.md](../../docs/development/plugin_integration.md)
- `redis_client` 为可选 opt-in：不写该键则幂等走纯数据库路径

## 目录说明

```
pay-server/
├── main.cc                  # .env 加载 → 占位符替换 → 启动校验 → CORS/安全头 → run()
├── config.json              # 新 channels schema 示例（含 PromExporter/Hodor 限流）
├── controllers/             # 宿主专属路由：/healthz、/metrics
├── utils/                   # ConfigLoader / StartupValidator / SecurityHeaders
├── scripts/                 # 构建、e2e、运维脚本
├── deploy/                  # 监控（Prometheus/Grafana）等部署资产
├── Dockerfile / docker-compose.yml
└── certs/                   # 渠道证书（git 忽略，勿提交）
```
