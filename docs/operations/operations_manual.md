# 运维手册

**目标环境：** Production

---

## 📋 目录

1. [日常操作](#日常操作)
2. [故障处理](#故障处理)
3. [备份和恢复](#备份和恢复)
4. [扩容和缩容](#扩容和缩容)
5. [监控和告警](#监控和告警)
6. [性能优化](#性能优化)

---

## 🔄 日常操作

### 1. 启动和停止服务

#### Linux (systemd)

**启动服务：**
```bash
sudo systemctl start payplugin
```

**停止服务：**
```bash
sudo systemctl stop payplugin
```

**重启服务：**
```bash
sudo systemctl restart payplugin
```

**查看服务状态：**
```bash
sudo systemctl status payplugin
```

**启用开机自启：**
```bash
sudo systemctl enable payplugin
```

#### Windows

**启动服务（手动）：**
```powershell
cd D:\pay-server
.\build\Release\PayServer.exe
```

**停止服务：**
按 `Ctrl+C` 或关闭窗口

**作为Windows服务运行：**
使用NSSM (Non-Sucking Service Manager):
```powershell
# 安装NSSM
choco install nssm

# 安装服务
nssm install PayServer "D:\build\windows-msvc\Release\PayServer.exe"
nssm set PayServer AppDirectory "D:\pay-server"
nssm set PayServer DisplayName "Pay Plugin Server"
nssm set PayServer Description "Payment processing service"
nssm start PayServer
```

#### Docker

**启动容器：**
```bash
docker run -d \
  --name payserver \
  -p 5566:5566 \
  -v $(pwd)/config.json:/app/config.json \
  payserver:latest
```

**停止容器：**
```bash
docker stop payserver
```

**重启容器：**
```bash
docker restart payserver
```

**查看日志：**
```bash
docker logs -f payserver
```

---

### 2. 配置更新

#### 更新config.json

**步骤：**

1. **备份当前配置**
```bash
cp config.json config.json.backup.$(date +%Y%m%d_%H%M%S)
```

2. **编辑配置文件**
```bash
vi config.json
# 或使用其他编辑器
```

3. **验证JSON格式**
```bash
# 使用Python验证
python3 -m json.tool config.json

# 或使用jq
jq empty config.json
```

4. **重启服务**
```bash
sudo systemctl restart payplugin  # Linux
```

#### 配置变更验证

**健康检查：**
```bash
curl http://localhost:5566/healthz   # 存活探针
curl http://localhost:5566/readyz    # 就绪探针（探测 DB/Redis 连通性）
```

**预期响应（就绪）：**
```json
{
  "status": "ready"
}
```

> 说明：`/health` 曾是 `/readyz` 的废弃别名，其 `Sunset` 头写的日期已过，别名已退役，
> 现在返回 404；就绪失败时 `/readyz` 返回 `{"status":"not_ready","failed":["db"]}`。

**API测试：**
```bash
curl -H "X-Api-Key: test-key" \
  http://localhost:5566/api/pay/query?order_no=test_1
```

---

### 3. 日志查看

#### Linux systemd日志

**实时查看日志：**
```bash
sudo journalctl -u payplugin -f
```

**查看最近100行日志：**
```bash
sudo journalctl -u payplugin -n 100
```

**查看指定时间范围日志：**
```bash
sudo journalctl -u payplugin --since "2026-04-13 10:00:00" --until "2026-04-13 11:00:00"
```

**按优先级过滤：**
```bash
# 只看错误和警告
sudo journalctl -u payplugin -p err -p warn
```

**导出日志：**
```bash
sudo journalctl -u payplugin --since "24 hours ago" > payplugin_$(date +%Y%m%d).log
```

#### 应用日志文件

如果配置了日志文件输出：

**查看日志：**
```bash
tail -f /var/log/payserver/payserver.log
```

**按关键词搜索：**
```bash
grep "ERROR" /var/log/payserver/payserver.log
grep "payment_no=TXN" /var/log/payserver/payserver.log
```

**统计错误数量：**
```bash
grep -c "ERROR" /var/log/payserver/payserver.log
```

#### 日志级别调整

**临时调整（修改config.json）：**
```json
{
  "log": {
    "log_level": "DEBUG"  // TRACE, DEBUG, INFO, WARN, ERROR
  }
}
```

**重启服务使配置生效：**
```bash
sudo systemctl restart payplugin
```

---

### 4. 数据库维护

#### 连接数据库

```bash
psql -U pay_user -d pay_production
```

#### 常用查询

**检查连接数：**
```sql
SELECT count(*) FROM pg_stat_activity WHERE datname = 'pay_production';
```

**检查慢查询：**
```sql
SELECT query, calls, total_time, mean_time
FROM pg_stat_statements
ORDER BY mean_time DESC
LIMIT 10;
```

**检查表大小：**
```sql
SELECT
  schemaname,
  tablename,
  pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) AS size
FROM pg_tables
WHERE schemaname = 'public'
ORDER BY pg_total_relation_size(schemaname||'.'||tablename) DESC;
```

**检查索引使用情况：**
```sql
SELECT
  schemaname,
  tablename,
  indexname,
  idx_scan,
  idx_tup_read,
  idx_tup_fetch
FROM pg_stat_user_indexes
ORDER BY idx_scan DESC;
```

#### 数据库VACUUM

**手动VACUUM：**
```sql
VACUUM ANALYZE;
```

**自动VACUUM配置：**
```sql
-- 修改postgresql.conf
autovacuum = on
autovacuum_vacuum_scale_factor = 0.1
autovacuum_analyze_scale_factor = 0.05
```

---

### 5. Redis维护

#### 连接Redis

```bash
redis-cli
```

#### 常用命令

**查看信息：**
```bash
redis-cli INFO
```

**检查内存使用：**
```bash
redis-cli INFO memory
```

**检查键数量：**
```bash
redis-cli DBSIZE
```

**查看慢查询：**
```bash
redis-cli SLOWLOG GET 10
```

**清空过期键：**
```bash
redis-cli --scan --pattern "idempotency:*" | xargs redis-cli DEL
```

---

## 🚨 故障处理

### 1. 常见错误诊断

#### 服务无法启动

**症状：**
- systemctl status显示 "failed"
- 应用立即退出

**诊断步骤：**

1. **检查配置文件**
```bash
# 验证JSON格式
python3 -m json.tool /opt/payplugin/config.json
```

2. **检查端口占用**
```bash
sudo netstat -tulpn | grep 5566
# 或
sudo lsof -i :5566
```

3. **查看详细错误日志**
```bash
sudo journalctl -u payplugin -n 50 --no-pager
```

4. **检查数据库连接**
```bash
psql -U pay_user -d pay_production -c "SELECT 1;"
```

5. **检查Redis连接**
```bash
redis-cli PING
```

**常见原因：**
- config.json格式错误
- 数据库连接失败
- Redis连接失败
- 端口被占用
- 权限不足

#### 数据库连接失败

**症状：**
- 日志显示 "Database connection failed"
- /readyz返回 `{"status":"not_ready","failed":["db"]}`

**诊断：**

1. **检查PostgreSQL服务**
```bash
sudo systemctl status postgresql
```

2. **检查数据库可访问性**
```bash
psql -U pay_user -d pay_production -h localhost
```

3. **检查连接数**
```sql
SELECT count(*) FROM pg_stat_activity WHERE datname = 'pay_production';
```

4. **检查连接池配置**
```bash
# 查看config.json中的number_of_connections
grep -A 5 "db_clients" config.json
```

**解决方案：**
- 启动PostgreSQL服务
- 增加max_connections配置
- 检查数据库凭据
- 增加连接池大小

#### Redis连接失败

**症状：**
- 日志显示 "Redis connection failed"
- 幂等性功能不工作

**诊断：**

1. **检查Redis服务**
```bash
sudo systemctl status redis-server
```

2. **测试连接**
```bash
redis-cli PING
```

3. **检查内存**
```bash
redis-cli INFO memory | grep used_memory_human
```

**解决方案：**
- 启动Redis服务
- 清理Redis内存
- 增加maxmemory配置
- 检查连接池配置

#### 内存泄漏

**症状：**
- 服务运行一段时间后内存持续增长
- 系统出现OOM（Out of Memory）

**诊断：**

1. **监控进程内存**
```bash
# 实时监控
watch -n 1 'ps aux | grep PayServer'

# 详细内存映射
pmap -x $(pidof PayServer)
```

2. **检查连接泄漏**
```bash
# 数据库连接
psql -U pay_user -d pay_production -c "SELECT count(*) FROM pg_stat_activity WHERE datname = 'pay_production';"

# Redis连接
redis-cli CLIENT LIST | wc -l
```

**解决方案：**
- 重启服务
- 修复连接泄漏代码
- 增加内存限制
- 优化缓存策略

#### 性能突然下降

**症状：**
- 响应时间突然增加
- CPU使用率异常高
- 数据库慢查询增加

**诊断：**

1. **检查系统负载**
```bash
top
htop
```

2. **检查磁盘I/O**
```bash
iostat -x 1
```

3. **检查数据库锁**
```sql
SELECT * FROM pg_stat_activity WHERE state != 'idle';
```

4. **查看慢查询日志**
```bash
tail -f /var/log/postgresql/postgresql-*.log | grep "duration:"
```

**解决方案：**
- 杀死长时间运行的查询
- 重启数据库
- 优化慢查询
- 增加资源

---

### 2. 数据恢复

#### 数据库恢复

**从备份恢复：**

1. **停止应用**
```bash
sudo systemctl stop payplugin
```

2. **删除当前数据库**
```bash
psql -U postgres -c "DROP DATABASE pay_production;"
```

3. **创建新数据库**
```bash
psql -U postgres -c "CREATE DATABASE pay_production OWNER pay_user;"
```

4. **恢复备份**
```bash
psql -U pay_user -d pay_production < backup_20260413.sql
```

5. **启动应用**
```bash
sudo systemctl start payplugin
```

**时间点恢复（PITR）：**
```bash
# PostgreSQL WAL归档恢复
# 需要提前配置archive_mode和archive_command
```

#### Redis恢复

**从RDB文件恢复：**

1. **停止Redis**
```bash
sudo systemctl stop redis-server
```

2. **备份当前dump.rdb**
```bash
cp /var/lib/redis/dump.rdb /var/lib/redis/dump.rdb.backup
```

3. **替换为备份文件**
```bash
cp /path/to/backup/dump.rdb /var/lib/redis/dump.rdb
```

4. **启动Redis**
```bash
sudo systemctl start redis-server
```

---

### 3. 紧急回滚

#### 应用回滚

**步骤：**

1. **停止当前版本**
```bash
sudo systemctl stop payplugin
```

2. **切换到备份版本**
```bash
cd /opt/payplugin
mv current current_failed
mv backup current
```

3. **启动备份版本**
```bash
sudo systemctl start payplugin
```

4. **验证服务**
```bash
# /readyz 会真的打一次 DB/Redis；/healthz 只证明进程活着。
# 不要用 /health：那是 /readyz 的旧别名，已退役并返回 404。
curl -f http://localhost:5566/readyz
```

#### 数据库迁移回滚

**步骤：**

1. **停止应用**
```bash
sudo systemctl stop payplugin
```

2. **执行回滚SQL**
```bash
psql -U pay_user -d pay_production < rollback_migration.sql
```

3. **启动应用**
```bash
sudo systemctl start payplugin
```

---

## 💾 备份和恢复

### 1. 数据库备份

#### 自动备份脚本

**创建backup_db.sh:**
```bash
#!/bin/bash
# Pay Plugin Database Backup Script

# Configuration
DB_NAME="pay_production"
DB_USER="pay_user"
BACKUP_DIR="/var/backups/payserver"
RETENTION_DAYS=30

# Create backup directory
mkdir -p $BACKUP_DIR

# Backup filename with timestamp
BACKUP_FILE="$BACKUP_DIR/payserver_$(date +%Y%m%d_%H%M%S).sql"

# Perform backup
pg_dump -U $DB_USER -d $DB_NAME -F p -f $BACKUP_FILE

# Compress backup
gzip $BACKUP_FILE

# Remove old backups (older than RETENTION_DAYS)
find $BACKUP_DIR -name "payserver_*.sql.gz" -mtime +$RETENTION_DAYS -delete

# Log
echo "$(date): Backup completed: $BACKUP_FILE.gz" >> /var/log/payserver/backup.log
```

**设置定时任务（cron）：**
```bash
# 编辑crontab
crontab -e

# 每天凌晨2点备份
0 2 * * * /opt/payserver/scripts/backup_db.sh
```

#### 手动备份

```bash
# 备份到SQL文件
pg_dump -U pay_user -d pay_production -F p -f backup.sql

# 备份并压缩
pg_dump -U pay_user -d pay_production -F p | gzip > backup.sql.gz

# 仅备份schema（不含数据）
pg_dump -U pay_user -d pay_production --schema-only -f schema.sql

# 仅备份数据（不含schema）
pg_dump -U pay_user -d pay_production --data-only -f data.sql
```

### 2. 配置备份

**备份配置文件：**
```bash
# 备份config.json
cp config.json config.json.backup.$(date +%Y%m%d_%H%M%S)

# 备份到远程
rsync -avz config.json user@backup-server:/backups/payserver/
```

**自动备份脚本：**
```bash
#!/bin/bash
# Configuration Backup Script

CONFIG_FILE="/opt/payplugin/config.json"
BACKUP_DIR="/var/backups/payserver/config"

mkdir -p $BACKUP_DIR

# Backup with timestamp
cp $CONFIG_FILE "$BACKUP_DIR/config.json.$(date +%Y%m%d_%H%M%S)"

# Keep only last 30 backups
ls -t $BACKUP_DIR/config.json.* | tail -n +31 | xargs rm --
```

### 3. Redis备份

**手动触发RDB快照：**
```bash
redis-cli BGSAVE
```

**检查RDB文件：**
```bash
ls -lh /var/lib/redis/dump.rdb
```

**备份RDB文件：**
```bash
cp /var/lib/redis/dump.rdb /var/backups/payserver/redis_$(date +%Y%m%d_%H%M%S).rdb
```

---

## 📈 扩容和缩容

### 1. 水平扩展

#### 负载均衡配置

**使用Nginx作为负载均衡器：**

```nginx
# /etc/nginx/nginx.conf

upstream payserver_backend {
    least_conn;
    server payserver1:5566 weight=1;
    server payserver2:5566 weight=1;
    server payserver3:5566 weight=1;
}

server {
    listen 80;
    server_name pay.example.com;

    location / {
        proxy_pass http://payserver_backend;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

**部署多个实例：**

`PayServer` **不解析命令行参数**（`examples/pay-server/main.cc` 里是 `int main()`，
没有 `argc/argv`）：它固定读**当前工作目录**下的 `./config.json` 和 `./.env`。
所以端口、监听地址、凭据的差异全部来自 cwd，实例之间的隔离靠目录，不靠 flag——
写 `./PayServer --port 5567` 不会报错，但那个参数会被静默丢掉，三个进程会去抢
同一个 5566。

1. **每个实例一个目录**，各放一份自己的 `config.json`（端口不同）与 `.env`
```bash
for p in 5566 5567 5568; do
  mkdir -p "/srv/pay-$p"
  cp build/linux-release/examples/pay-server/.env "/srv/pay-$p/"
  sed "s/5566/$p/" build/linux-release/examples/pay-server/config.json \
      > "/srv/pay-$p/config.json"
  cp build/linux-release/examples/pay-server/PayServer "/srv/pay-$p/"
done
```

2. **从各自目录启动**（cwd 决定读哪份 config）
```bash
(cd /srv/pay-5566 && ./PayServer &) ; (cd /srv/pay-5567 && ./PayServer &)
```

3. **配置负载均衡器**（Nginx、HAProxy等）

### 2. 垂直扩展

#### 增加服务器资源

**CPU扩展：**
```bash
# 检查当前CPU
nproc
lscpu

# 增加线程数（config.json）
{
  "app": {
    "number_of_threads": 8  // 从4增加到8
  }
}
```

**内存扩展：**
```bash
# 检查当前内存
free -h

# 增加数据库连接池
{
  "db_clients": [
    {
      "number_of_connections": 64  // 从32增加到64
    }
  ]
}
```

**磁盘扩展：**
```bash
# 检查磁盘使用
df -h

# 扩展PostgreSQL存储
# 修改postgresql.conf
# 增加shared_buffers, work_mem等
```

### 3. 数据库扩展

#### 读写分离

**配置主从复制：**

1. **主库配置（postgresql.conf）：**
```ini
wal_level = replica
max_wal_senders = 3
wal_keep_size = 100MB
```

2. **从库配置：**
```ini
hot_standby = on
standby_mode = on
primary_conninfo = 'host=primary_ip port=5432 user=replicator'
```

3. **应用配置**（读写分离）
- 写操作 → 主库
- 读操作 → 从库

#### 分库分表

**按用户ID分片：**
```cpp
// 确定数据库
int shard_id = user_id % SHARD_COUNT;
std::string db_name = "pay_production_shard_" + std::to_string(shard_id);
```

---

## 📊 监控和告警

### 关键指标监控

- **服务可用性**: /readyz端点检查
- **响应时间**: P50/P95/P99延迟
- **错误率**: 5xx错误率
- **吞吐量**: QPS
- **资源使用**: CPU、内存、磁盘、网络
- **数据库**: 连接数、慢查询、锁等待
- **Redis**: 内存使用、连接数、慢查询

详见 [monitoring_setup.md](../deployment/monitoring_setup.md)

---

## ⚡ 性能优化

### 1. 应用层优化

- 增加工作线程数
- 优化数据库连接池
- 启用缓存
- 减少日志输出

### 2. 数据库优化

- 添加索引
- 优化查询
- 调整PostgreSQL参数
- 定期VACUUM

### 3. 系统层优化

- 调整文件描述符限制
- 优化TCP参数
- 使用SSD存储
- 增加内存

