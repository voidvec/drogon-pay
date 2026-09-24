---
name: docker-integration-test
description: 在 Docker Compose 环境中执行支付系统的完整集成测试和健康检查。验证支付创建、退款、回调处理、订单查询、对账以及前后端通信的端到端功能。
---

# 支付系统集成测试

在 Docker Compose 环境中执行支付系统的完整集成测试和健康检查，生成详细的 HTML 测试报告。

## 使用时机

- 代码变更后验证支付功能正常性
- 提交前运行完整测试套件
- 验证 Docker 环境部署状态
- 检查支付系统集成问题
- 生成测试报告用于代码审查

## 测试范围

### 1. 环境健康检查
- Docker 服务状态验证（PostgreSQL、Redis、PayServer）
- 网络连接检查
- 端口可用性确认（5566、5432、6379）
- 依赖服务就绪状态

### 2. 支付核心流程测试
- 支付创建 → 订单查询完整流程
- 退款创建 → 退款查询流程
- 支付回调签名验证
- 幂等性验证
- API Key 认证测试

### 3. API 端点测试
- `POST /api/pay/create` - 创建支付
- `GET /api/pay/query?order_no=...` - 查询支付
- `POST /api/pay/refund` - 创建退款
- `GET /api/pay/refund/query?refund_no=...` - 查询退款
- `POST /api/pay/notify/{wechat|alipay}` - 支付回调（渠道签名，非 API Key）
- `GET /healthz` / `GET /readyz` - 存活/就绪
- `GET /metrics` - Prometheus 指标（仅回环地址）

路径以 `examples/pay-server/openapi.yaml` 为准，`base_path` 可配置（默认
`/api/pay`）；金额一律是**元为单位的十进制字符串**，如 `"9.99"`。

### 4. 数据库集成测试
- PostgreSQL 连接验证
- 数据模型完整性（pay_payment、pay_refund、pay_callback、pay_idempotency、pay_ledger）
- 事务处理正确性
- 查询性能检查
- 数据一致性验证

### 5. Redis 集成测试
- 连接和认证测试
- 缓存读写功能
- 过期机制验证
- 幂等性缓存验证

### 6. 前后端集成测试
- Vue 前端与后端 API 通信
- 支付流程完整性
- 用户体验验证
- 错误处理正确性

## 测试流程

### 步骤 1: 环境准备

```powershell
# 停止现有容器
docker-compose down -v

# 启动 Docker 服务
docker-compose up -d

# 等待服务就绪
timeout /t 10 /nobreak
```

### 步骤 2: 健康检查

```bash
# 检查容器状态
docker-compose ps

# 检查服务日志
docker-compose logs payserver

# 验证端口可用性
curl -f http://localhost:5566/healthz || exit 1
```

### 步骤 3: 数据库初始化验证

```bash
# 连接 PostgreSQL 验证 schema
docker exec postgres psql -U postgres -d pay_test -c "\dt"

# 验证表结构
docker exec postgres psql -U postgres -d pay_test -c "\d pay_payment"
docker exec postgres psql -U postgres -d pay_test -c "\d pay_refund"
```

### 步骤 4: 后端单元测试

**不在容器里跑**：runtime 镜像里只有 `/app/PayServer` 和 `/app/config.json`
（`examples/pay-server/Dockerfile` 的 runtime 阶段明确不装测试二进制），也没有
`build/` 目录，所以 `docker exec payserver ... PayBackendTests` 跑不起来。C++ 套件
在宿主上跑，与 compose 环境无关：

```bash
ctest --test-dir build/linux-release --output-on-failure   # 或 scripts/test.sh
```

本 skill 覆盖的是**HTTP 层**：下面的步骤 5 起直接打 compose 起来的 5566。

### 步骤 5: 支付 API 集成测试

```bash
# 1. 创建支付
curl -s -X POST "http://localhost:5566/api/pay/create" \
  -H "Content-Type: application/json" \
  -H "X-Api-Key: test-dev-key" \
  -d '{"channel":"alipay","order_no":"E2E-TEST-001","amount":"9.99","user_id":10001,"description":"Integration test"}'

# 2. 查询支付
curl -s -X GET "http://localhost:5566/api/pay/query?order_no=E2E-TEST-001" \
  -H "X-Api-Key: test-dev-key"

# 3. 创建退款
curl -s -X POST "http://localhost:5566/api/pay/refund" \
  -H "Content-Type: application/json" \
  -H "X-Api-Key: test-dev-key" \
  -d '{"order_no":"E2E-TEST-001","amount":"9.99","reason":"Test refund"}'

# 4. 查询退款
curl -s -X GET "http://localhost:5566/api/pay/refund/query?refund_no=E2E-TEST-001" \
  -H "X-Api-Key: test-dev-key"
```

### 步骤 6: 幂等性测试

```bash
# 使用相同 Idempotency-Key 发送两次请求
curl -s -X POST "http://localhost:5566/api/pay/create" \
  -H "Content-Type: application/json" \
  -H "X-Api-Key: test-dev-key" \
  -H "Idempotency-Key: idem-test-001" \
  -d '{"channel":"alipay","order_no":"IDEM-TEST","amount":"9.99","user_id":10001,"description":"Idempotency test"}'

# 第二次相同请求应返回相同结果
curl -s -X POST "http://localhost:5566/api/pay/create" \
  -H "Content-Type: application/json" \
  -H "X-Api-Key: test-dev-key" \
  -H "Idempotency-Key: idem-test-001" \
  -d '{"channel":"alipay","order_no":"IDEM-TEST","amount":"9.99","user_id":10001,"description":"Idempotency test"}'
```

### 步骤 7: 性能基准测试

```bash
# Redis 性能测试
docker exec redis redis-cli ping

# 数据库连接池验证
docker exec postgres psql -U postgres -d pay_test -c \
  "SELECT count(*) as active_connections FROM pg_stat_activity WHERE datname='pay_test';"
```

## 测试报告生成

报告器只**读** `test-results/` 里的 JSON，不自己发请求，所以先跑生产者再跑报告器：

```bash
# 1. 打全链路 HTTP 用例，写 test-results/pay_e2e_results.json
python .claude/skills/docker-integration-test/scripts/pay_e2e_test.py
# 2. 渲染 HTML
python .claude/skills/docker-integration-test/scripts/generate_report.py \
  --test-results ./test-results \
  --output ./test-results/docker-integration-test-report.html
```

`pay_e2e_test.py` 的连接参数走环境变量：`PAY_BASE_URL`（默认
`http://localhost:5566`）与 `PAY_API_KEY`（**必须**与宿主进程里的值一致，
否则整批 401；`docker-compose.yml` 在该变量缺失时直接拒绝启动）。

### 本 skill 目录下的脚本

| 脚本 | 状态 | 说明 |
|------|------|------|
| `scripts/pay_e2e_test.py` | 入口 | 全链路 HTTP 用例 + 写 JSON 结果 |
| `scripts/generate_report.py` | 入口 | 把 `test-results/*.json` 渲染成 HTML |
| `scripts/run_docker_tests.sh` | **遗留、无引用** | 只做一次健康探测（`/healthz`）；没有任何文档或脚本调用它，也没有可执行位。要么改造成 `pay_e2e_test.py` 的前置门并补上引用，要么删除——别把它当入口跑 |

报告包含：
- 总体测试概览（通过率、总耗时）
- 各服务健康状态图表
- 详细测试结果（按类别分组）
- 性能指标趋势
- 失败测试的详细错误日志
- 故障排除建议
- 系统配置快照

## 故障处理

### 服务启动失败
**诊断**:
```bash
docker-compose logs payserver
docker inspect payserver
```
**解决方案**:
1. 检查环境变量配置（`PAY_DB_PASSWORD`、`PAY_REDIS_PASSWORD`、`PAY_API_KEY`）
2. 验证依赖服务状态
3. 检查端口冲突（5566、5432、6379）

### 数据库连接失败
**诊断**:
```bash
docker exec postgres psql -U postgres -d pay_test -c "SELECT 1;"
docker network inspect pay-net
```
**解决方案**:
1. 验证数据库容器状态
2. 检查网络连接
3. 确认数据库初始化完成
4. 验证凭证配置

### Redis 连接问题
**诊断**:
```bash
docker exec redis redis-cli ping
```
**解决方案**:
1. 检查 Redis 密码配置
2. 验证网络可达性
3. 确认 Redis 服务状态

### 支付创建失败
**诊断**:
```bash
curl -v -X POST "http://localhost:5566/api/pay/create" \
  -H "Content-Type: application/json" \
  -H "X-Api-Key: test-dev-key" \
  -d '{"channel":"alipay","order_no":"DIAG-TEST","amount":"1.00","user_id":10001,"description":"Diagnostic"}'
docker-compose logs payserver | grep -i error
```
**解决方案**:
1. 验证 API Key 配置
2. 检查第三方支付客户端配置
3. 确认订单号唯一性
4. 查看详细错误日志

## 性能基准

### 预期性能指标
- **健康检查**: < 2 秒
- **支付创建**: < 500ms
- **订单查询**: < 100ms
- **退款处理**: < 500ms
- **数据库查询**: < 100ms
- **Redis 操作**: < 10ms

### 负载测试
- **并发请求**: 100 个并发
- **响应时间**: P95 < 1 秒
- **成功率**: > 99%

## 最佳实践

1. **定期测试**: 每次代码变更后运行
2. **环境隔离**: 使用专用的测试数据库 `pay_test`
3. **数据清理**: 每次测试前清理旧数据
4. **日志收集**: 保存完整的测试日志
5. **性能监控**: 跟踪性能指标变化
6. **自动化集成**: 集成到 CI/CD 流程

## 注意事项

- 确保 Docker 服务正在运行
- 测试会修改数据库内容，使用专用测试环境
- 某些测试可能需要较长时间（5-10 分钟）
- 确保端口 5566、5432、6379 未被占用
- 测试报告文件较大，确保有足够磁盘空间
