# Pay Plugin 部署指南

**目标环境：** Production

---

## 📋 系统要求

### 硬件要求

**最低配置：**
- CPU: 4核心
- 内存: 2GB
- 磁盘: 10GB 可用空间

**推荐配置：**
- CPU: 8核心+
- 内存: 4GB+
- 磁盘: 20GB+ SSD

### 软件要求

**操作系统：**
- Windows 10/Server 2016+
- Linux (Ubuntu 20.04+, CentOS 7+)

**依赖软件：**
- PostgreSQL 13.0+
- Redis 6.0+
- Drogon Framework 1.9.13（钉死版本；库与宿主必须一致）
- Conan 2 (依赖管理)
- Visual Studio 2022 (Windows) / GCC / Clang (Linux)
- CMake 3.21+

---

## 🔧 依赖安装

### Windows

#### 1. 安装PostgreSQL

```powershell
# 使用Chocolatey
choco install postgresql

# 或从官网下载安装包
# https://www.postgresql.org/download/windows/
```

#### 2. 安装Redis

```powershell
# 使用Chocolatey
choco install redis-64

# 或使用WSL
wsl redis-server
```

#### 3. 安装Visual Studio 2022

确保安装以下组件：
- Desktop development with C++
- CMake tools
- Windows 10 SDK

#### 4. 安装Conan 2

```powershell
pip install conan
```

### Linux (Ubuntu 20.04)

```bash
# 更新包管理器
sudo apt update

# 安装PostgreSQL
sudo apt install postgresql postgresql-contrib

# 安装Redis
sudo apt install redis-server

# 安装构建工具
sudo apt install build-essential cmake git

# 安装Conan
pip3 install conan

# 安装Drogon依赖
sudo apt install libcurl4-openssl-dev libssl-dev zlib1g-dev
```

---

## 📦 编译和构建

### 1. 克隆项目

```bash
git clone https://github.com/lucaswang420/drogon-pay.git
cd drogon-pay
```

### 2. 安装依赖并编译（一条命令，脚本内部跑 Conan + preset）

**Linux / macOS:**
```bash
examples/pay-server/scripts/build.sh          # Release；-debug 走 linux-debug / macos-debug
```

**Windows:**
```powershell
examples\pay-server\scripts\build.bat         # Release；-debug 走 windows-msvc-debug
```

两个脚本逐参数对齐，内部依次执行 `conan install` → `cmake --preset` →
`cmake --build --preset`，最后把 `config.json` / `.env` / `certs/` 复制到二进制旁边：

```bash
# 需要自定义配置时再展开成手工形式
conan install . --output-folder=build/linux-release -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset linux-release
cmake --build --preset linux-release -j$(nproc)
```

**重要：** 
- ⚠️ 生产构建必须使用 **Release** 模式！
- ⚠️ Debug 是开发/覆盖率用的：脚本的 `-debug` 走 `linux-debug` / `macos-debug` /
  `windows-msvc-debug`，每个预设目录有**自己**的 Conan 依赖树（`conan install
  --output-folder=build/<preset> -s build_type=<cfg>`），所以 Debug 目标链接的是
  Debug 依赖，不会和 Release 的混链。`.github/workflows/coverage.yml` 跑的就是
  Debug + gcov。
- ⚠️ 优先使用平台脚本（`build.sh` / `build.bat`），其次才是 CMake preset，不要手写裸 CMake 命令

### 3. 验证编译

```bash
# 检查可执行文件
ls build/windows-msvc/examples/pay-server/Release/PayServer.exe  # Windows
ls build/linux-release/examples/pay-server/PayServer             # Linux

# 运行测试（可选）
ctest --test-dir build/linux-release                  # Linux
build/windows-msvc/tests/Release/PayBackendTests.exe  # Windows
```

---

## ⚙️ 配置

### 1. 数据库配置

#### 创建数据库

```sql
-- 连接到PostgreSQL
psql -U postgres

-- 创建数据库和用户
CREATE DATABASE pay_production;
CREATE USER pay_user WITH PASSWORD 'your_secure_password';
GRANT ALL PRIVILEGES ON DATABASE pay_production TO pay_user;

-- 退出
\q
```

### 2. Redis配置

#### 编辑redis.conf

```conf
# /etc/redis/redis.conf

bind 127.0.0.1
port 6379
requirepass your_redis_password
```

#### 启动Redis

**Windows:**
```powershell
redis-server
```

**Linux:**
```bash
sudo systemctl start redis-server
sudo systemctl enable redis-server
```

### 3. 应用配置

#### 编辑config.json

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
      "dbname": "pay_production",
      "user": "pay_user",
      "passwd": "your_secure_password",
      "number_of_connections": 32
    }
  ],
  "redis_clients": [
    {
      "name": "default",
      "host": "127.0.0.1",
      "port": 6379,
      "passwd": "your_redis_password",
      "number_of_connections": 32
    }
  ],
  "app": {
    "number_of_threads": 4,
    "max_connections": 100000
  }
}
```

---

## 🚀 部署

### 1. 准备部署目录

```bash
# 创建部署目录
sudo mkdir -p /opt/payplugin
sudo chown $USER:$USER /opt/payplugin

# 复制文件
cp -r examples/pay-server/* /opt/payplugin/
cd /opt/payplugin
```

### 2. 创建systemd服务（Linux）

```bash
sudo nano /etc/systemd/system/payplugin.service
```

```ini
[Unit]
Description=Pay Plugin Payment Service
After=network.target postgresql.service redis.service

[Service]
Type=simple
User=payuser
WorkingDirectory=/opt/payplugin
ExecStart=/opt/payplugin/build/linux-release/examples/pay-server/PayServer
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable payplugin
sudo systemctl start payplugin
```

### 3. 启动服务

**手动启动（测试）：**
```bash
cd /opt/payplugin
build\windows-msvc\examples\pay-server\Release\PayServer.exe
```

**服务启动：**
```bash
sudo systemctl start payplugin
```

### 4. 验证部署

#### 健康检查

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

---

## 📊 监控

### Prometheus指标

访问：`http://localhost:5566/metrics`

### 日志

**应用日志：** 控制台输出或日志文件

**查看日志（Linux systemd）：**
```bash
sudo journalctl -u payplugin -f
```

---

## 🐛 故障排查

### 常见问题

#### 1. 服务无法启动

**检查：**
- 配置文件语法
- 数据库连接
- Redis连接
- 端口占用

#### 2. API返回500错误

**检查：**
- 数据库连接状态
- config.json配置
- 应用日志

#### 3. 性能问题

**优化：**
- 增加线程数
- 增加数据库连接数
- 启用缓存

