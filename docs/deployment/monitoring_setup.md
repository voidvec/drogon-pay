# 监控和告警配置指南

**目标环境：** Production

---

## 📊 概述

Pay Plugin提供了完整的监控能力，支持Prometheus指标采集和Grafana可视化。

### 监控架构

```
┌─────────────┐      ┌─────────────┐      ┌─────────────┐
│  PayServer  │──────│ Prometheus  │──────│  Grafana    │
│  :5566      │      │  :9090      │      │  :3000      │
└─────────────┘      └─────────────┘      └─────────────┘
      /metrics              scrape               visualize
```

---

## 🔧 Prometheus配置

### 1. 安装Prometheus

**Linux (Ubuntu/Debian):**
```bash
# 下载Prometheus
wget https://github.com/prometheus/prometheus/releases/download/v2.45.0/prometheus-2.45.0.linux-amd64.tar.gz
tar xvfz prometheus-2.45.0.linux-amd64.tar.gz
cd prometheus-2.45.0.linux-amd64

# 启动Prometheus
./prometheus --config.file=prometheus.yml
```

**Windows:**
```powershell
# 下载Windows版本
# https://prometheus.io/download/

# 解压并编辑配置
notepad prometheus.yml

# 启动Prometheus
.\prometheus.exe
```

**Docker:**
```bash
docker run -d \
  -p 9090:9090 \
  -v $(pwd)/prometheus.yml:/etc/prometheus/prometheus.yml \
  prom/prometheus
```

### 2. 配置Prometheus

使用提供的配置文件：`deploy/prometheus_config.yml`

```yaml
global:
  scrape_interval: 15s
  evaluation_interval: 15s

alerting:
  alertmanagers:
    - static_configs:
        - targets: []

rule_files:
  - 'alerts.yml'

scrape_configs:
  - job_name: 'payserver'
    static_configs:
      - targets: ['localhost:5566']
    metrics_path: '/metrics'
    scrape_interval: 10s
```

### 3. 配置告警规则

创建 `deploy/alerts.yml`:

```yaml
groups:
  - name: payserver_alerts
    interval: 30s
    rules:
      # 高响应时间告警
      - alert: HighResponseTime
        expr: histogram_quantile(0.95, http_request_duration_seconds_bucket) > 0.2
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High response time detected"
          description: "P95 response time is above 200ms for 5 minutes"

      # 高错误率告警
      - alert: HighErrorRate
        expr: rate(http_requests_total{status=~"5.."}[5m]) > 0.05
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "High error rate detected"
          description: "Error rate is above 5% for 5 minutes"

      # 服务不可用告警
      - alert: ServiceDown
        expr: up{job="payserver"} == 0
        for: 1m
        labels:
          severity: critical
        annotations:
          summary: "PayServer is down"
          description: "PayServer has been down for 1 minute"

      # 数据库连接池耗尽
      - alert: DatabasePoolExhausted
        expr: db_connections_active / db_connections_max > 0.9
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Database connection pool nearly exhausted"
          description: "Database connection usage is above 90%"

      # Redis连接池耗尽
      - alert: RedisPoolExhausted
        expr: redis_connections_active / redis_connections_max > 0.9
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Redis connection pool nearly exhausted"
          description: "Redis connection usage is above 90%"
```

---

## 📈 Grafana仪表板

### 1. 安装Grafana

**Linux:**
```bash
# 添加Grafana仓库
sudo apt-get install -y software-properties-common
sudo add-apt-repository "deb https://packages.grafana.com/oss/deb stable main"
wget -q -O - https://packages.grafana.com/gpg.key | sudo apt-key add -
sudo apt-get update
sudo apt-get install grafana

# 启动Grafana
sudo systemctl start grafana-server
sudo systemctl enable grafana-server
```

**Windows:**
```powershell
# 下载Windows版本
# https://grafana.com/grafana/download?platform=windows

# 解压并启动
.\bin\grafana-server.exe
```

**Docker:**
```bash
docker run -d \
  -p 3000:3000 \
  --name=grafana \
  grafana/grafana
```

### 2. 配置数据源

1. 访问 `http://localhost:3000` (默认用户名/密码: admin/admin)
2. 添加Prometheus数据源：
   - URL: `http://localhost:9090`
   - Access: Server (default)

### 3. 导入仪表板

导入提供的仪表板配置：`deploy/grafana_dashboard.json`

或者手动创建仪表板，包含以下面板：

#### 关键指标面板

**1. 请求吞吐量 (QPS)**
```promql
sum(rate(http_requests_total[1m]))
```

**2. P50/P95/P99延迟**
```promql
# P50
histogram_quantile(0.50, http_request_duration_seconds_bucket)

# P95
histogram_quantile(0.95, http_request_duration_seconds_bucket)

# P99
histogram_quantile(0.99, http_request_duration_seconds_bucket)
```

**3. 错误率**
```promql
sum(rate(http_requests_total{status=~"5.."}[5m])) / sum(rate(http_requests_total[5m]))
```

**4. 数据库连接数**
```promql
db_connections_active
db_connections_idle
```

**5. Redis连接数**
```promql
redis_connections_active
redis_connections_idle
```

**6. 支付成功率**
```promql
sum(rate(payment_success_total[5m])) / sum(rate(payment_attempts_total[5m]))
```

> 注意：`payment_success_total` / `payment_attempts_total` /
> `refund_success_total` / `refund_attempts_total` 并非插件默认导出的指标。
> 当前插件仅导出鉴权计数（见上方"业务指标（鉴权）"）。如需支付/退款
> 成功率面板，需先在业务层实现这些计数器。

### 4. 仪表板布局

推荐的仪表板布局：

```
┌─────────────────────────────────────────────────┐
│           PayServer 监控仪表板                    │
├─────────────┬─────────────┬─────────────────────┤
│  QPS        │  错误率      │  可用性              │
│  (实时)     │  (5分钟)    │  (uptime)           │
├─────────────┴─────────────┴─────────────────────┤
│           P50/P95/P99 延迟                       │
│           (时间序列图表)                          │
├─────────────────────────────────────────────────┤
│  请求量分布 (按endpoint)                         │
│  (饼图)                                          │
├─────────────────────┬───────────────────────────┤
│  数据库连接池        │  Redis连接池              │
│  (仪表盘)            │  (仪表盘)                 │
├─────────────────────┴───────────────────────────┤
│  支付/退款成功率                                 │
│  (时间序列图表)                                   │
└─────────────────────────────────────────────────┘
```

---

## 📊 可用指标

### HTTP指标

| 指标名称 | 类型 | 描述 |
|---------|------|------|
| `http_requests_total` | Counter | HTTP请求总数 |
| `http_request_duration_seconds` | Histogram | HTTP请求延迟 |
| `http_requests_in_flight` | Gauge | 当前正在处理的请求数 |

### 业务指标（鉴权）

插件暴露的鉴权指标（见 `PayAuthMetrics`），在 `/api/pay/metrics/auth.prom`
以及聚合的 `/metrics` 端点中以 Prometheus 文本格式输出：

| 指标名称 | 类型 | 描述 |
|---------|------|------|
| `pay_auth_missing_key_total` | Counter | 请求缺少 API Key 的次数 |
| `pay_auth_invalid_key_total` | Counter | API Key 无效的次数 |
| `pay_auth_scope_denied_total` | Counter | 因 scope 不足被拒绝的次数 |
| `pay_auth_not_configured_total` | Counter | 未配置任何 API Key 的命中次数 |

> 说明：`/metrics` 端点聚合了 Drogon PromExporter 的基础指标（路径
> `/metrics/base`）与上述鉴权指标。HTTP 吞吐 / 延迟等通用指标来自 Drogon
> 自身的导出器。

### 数据库指标

| 指标名称 | 类型 | 描述 |
|---------|------|------|
| `db_connections_active` | Gauge | 活跃数据库连接数 |
| `db_connections_idle` | Gauge | 空闲数据库连接数 |
| `db_connections_max` | Gauge | 最大数据库连接数 |
| `db_query_duration_seconds` | Histogram | 数据库查询延迟 |

### Redis指标

| 指标名称 | 类型 | 描述 |
|---------|------|------|
| `redis_connections_active` | Gauge | 活跃Redis连接数 |
| `redis_connections_idle` | Gauge | 空闲Redis连接数 |
| `redis_connections_max` | Gauge | 最大Redis连接数 |
| `redis_command_duration_seconds` | Histogram | Redis命令延迟 |

---

## 🔔 告警通知

### 配置Alertmanager

创建 `deploy/alertmanager.yml`:

```yaml
global:
  resolve_timeout: 5m

route:
  group_by: ['alertname', 'cluster', 'service']
  group_wait: 10s
  group_interval: 10s
  repeat_interval: 12h
  receiver: 'default'

  routes:
    - match:
        severity: critical
      receiver: 'critical'
      continue: true

    - match:
        severity: warning
      receiver: 'warning'

receivers:
  - name: 'default'
    email_configs:
      - to: 'team@example.com'
        from: 'alertmanager@example.com'
        smarthost: 'smtp.example.com:587'

  - name: 'critical'
    webhook_configs:
      - url: 'http://your-webhook-url/critical'
    email_configs:
      - to: 'oncall@example.com'
        from: 'alertmanager@example.com'
        smarthost: 'smtp.example.com:587'

  - name: 'warning'
    email_configs:
      - to: 'devops@example.com'
        from: 'alertmanager@example.com'
        smarthost: 'smtp.example.com:587'
```

启动Alertmanager:
```bash
./alertmanager --config.file=alertmanager.yml
```

在Prometheus配置中添加Alertmanager:
```yaml
alerting:
  alertmanagers:
    - static_configs:
        - targets: ['localhost:9093']
```

---

## 🔍 日志聚合

### 配置日志输出

Pay Server支持将日志输出到标准输出（stdout），便于容器化环境。

**config.json配置:**
```json
{
  "log": {
    "log_path": "",
    "logfile_base_name": "",
    "log_size_limit": 104857600,
    "log_level": "DEBUG"
  }
}
```

### 使用ELK Stack

**1. 安装Filebeat:**
```bash
sudo apt-get install filebeat
```

**2. 配置Filebeat (`filebeat.yml`):**
```yaml
filebeat.inputs:
  - type: container
    paths:
      - /var/lib/docker/containers/*/*.log

processors:
  - add_docker_metadata:

output.elasticsearch:
  hosts: ["localhost:9200"]

setup.kibana:
  host: "localhost:5601"
```

**3. 启动Filebeat:**
```bash
sudo systemctl start filebeat
sudo systemctl enable filebeat
```

---

## 📱 监控最佳实践

### 1. 告警级别

- **P0 (Critical):** 服务完全不可用，数据丢失风险
- **P1 (Warning):** 性能下降，部分功能受影响
- **P2 (Info):** 需要关注但不影响服务

### 2. 告警阈值建议

| 指标 | Warning | Critical |
|-----|---------|----------|
| P95延迟 | > 200ms | > 500ms |
| P99延迟 | > 500ms | > 1000ms |
| 错误率 | > 1% | > 5% |
| 可用性 | < 99.9% | < 99% |
| DB连接池使用率 | > 80% | > 95% |

### 3. 监控检查清单

- [ ] Prometheus正常采集指标
- [ ] Grafana仪表板配置完成
- [ ] 告警规则配置完成
- [ ] Alertmanager配置完成
- [ ] 日志聚合配置完成
- [ ] 告警通知测试通过
- [ ] 监控数据备份配置
- [ ] 监控系统高可用配置

---

## 🛠️ 故障排查

### Prometheus无法采集指标

**检查:**
```bash
# 验证/metrics端点可访问
curl http://localhost:5566/metrics

# 检查Prometheus配置
./promtool check config prometheus.yml

# 查看Prometheus日志
tail -f /var/log/prometheus/prometheus.log
```

### 告警未触发

**检查:**
```bash
# 查看当前告警状态
curl http://localhost:9090/api/v1/alerts

# 验证告警规则
./promtool check rules alerts.yml

# 检查Alertmanager连接
curl http://localhost:9093/api/v1/status
```

### Grafana无法显示数据

**检查:**
1. 数据源连接是否正常
2. 时间范围是否正确
3. 查询语法是否正确
4. Prometheus是否正常采集数据

---

## 📚 参考资源

- [Prometheus官方文档](https://prometheus.io/docs/)
- [Grafana官方文档](https://grafana.com/docs/)
- [Alertmanager配置指南](https://prometheus.io/docs/alerting/latest/alertmanager/)
- [PromQL查询语言](https://prometheus.io/docs/prometheus/latest/querying/basics/)

