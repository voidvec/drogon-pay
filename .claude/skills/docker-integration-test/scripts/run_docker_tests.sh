#!/bin/bash
# Docker 集成测试执行脚本
# 执行完整的 Docker 环境集成测试并生成报告

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 配置
PROJECT_DIR="$(pwd)"
RESULTS_DIR="${PROJECT_DIR}/test-results"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
REPORT_FILE="${PROJECT_DIR}/docker-test-report-${TIMESTAMP}.html"

echo -e "${BLUE}🐳 Docker 集成测试开始...${NC}"
echo "================================================"

# 创建结果目录
mkdir -p "${RESULTS_DIR}"

# 步骤1: 环境准备
echo -e "\n${BLUE}📋 步骤1: 环境准备${NC}"
echo "-------------------------------------------"

echo "停止现有容器..."
docker-compose down -v 2>/dev/null || true

echo "清理旧数据..."
docker system prune -f

echo "启动所有服务..."
docker-compose up -d

echo "等待服务就绪..."
sleep 30

# 步骤2: 健康检查
echo -e "\n${BLUE}🏥 步骤2: 健康检查${NC}"
echo "-------------------------------------------"

echo "检查容器状态..."
docker-compose ps > "${RESULTS_DIR}/container_status.txt"

echo "检查服务日志..."
docker-compose logs payserver > "${RESULTS_DIR}/server_logs.txt" 2>&1

# 健康检查 JSON 生成
python3 <<'EOF'
import json
import subprocess

services = {
    "payserver": "http://localhost:5566/healthz",
    "postgres": "localhost:5432",
    "redis": "localhost:6379",
}

health_status = {}

for service, endpoint in services.items():
    try:
        if service == "postgres":
            result = subprocess.run(
                ["docker", "exec", "postgres", "pg_isready", "-U", "test"],
                capture_output=True, text=True, timeout=10
            )
            health_status[service] = {
                "status": "healthy" if result.returncode == 0 else "unhealthy",
                "uptime": "unknown"
            }
        elif service == "redis":
            result = subprocess.run(
                ["docker", "exec", "redis", "redis-cli", "ping"],
                capture_output=True, text=True, timeout=10
            )
            health_status[service] = {
                "status": "healthy" if "PONG" in result.stdout else "unhealthy",
                "uptime": "unknown"
            }
        else:
            result = subprocess.run(
                ["curl", "-f", "-s", "-o", "/dev/null", "-w", "%{http_code}", endpoint],
                capture_output=True, text=True, timeout=10
            )
            health_status[service] = {
                "status": "healthy" if result.stdout == "200" else "unhealthy",
                "uptime": "unknown"
            }
    except Exception as e:
        health_status[service] = {
            "status": "unhealthy",
            "uptime": "error"
        }

with open("test-results/health_status.json", "w") as f:
    json.dump(health_status, f, indent=2)

print(json.dumps(health_status, indent=2))
EOF

# 步骤3: 数据库初始化验证
echo -e "\n${BLUE}💾 步骤3: 数据库初始化验证${NC}"
echo "-------------------------------------------"

echo "验证 PostgreSQL schema..."
docker exec postgres psql -U postgres -d pay_test -c "\dt" > "${RESULTS_DIR}/db_schema.txt" 2>&1

echo "验证核心数据表..."
docker exec postgres psql -U postgres -d pay_test -c "SELECT count(*) FROM pay_payment;" > "${RESULTS_DIR}/db_data.txt" 2>&1

# 步骤4: 后端单元测试
echo -e "\n${BLUE}🔧 步骤4: 后端单元测试${NC}"
echo "-------------------------------------------"

echo "在 Docker 容器中运行 C++ 测试..."
docker exec payserver /bin/bash -c "cd build && PayBackendTests.exe --output-on-failure -V" > "${RESULTS_DIR}/backend_tests.txt" 2>&1 || true

# 步骤5: 支付 API E2E 测试
echo -e "\n${BLUE}💳 步骤5: 支付 API E2E 测试${NC}"
echo "-------------------------------------------"

echo "运行支付 E2E 测试脚本..."
python3 .claude/skills/docker-integration-test/scripts/pay_e2e_test.py || true

# 步骤6: 性能基准测试
echo -e "\n${BLUE}📊 步骤6: 性能基准测试${NC}"
echo "-------------------------------------------"

echo "Redis 性能测试..."
docker exec redis redis-cli ping > "${RESULTS_DIR}/redis_ping.txt" 2>&1

echo "数据库连接测试..."
docker exec postgres psql -U postgres -d pay_test -c \
  "SELECT count(*) FROM pg_stat_activity WHERE datname='pay_test';" > "${RESULTS_DIR}/db_connections.txt" 2>&1

# 步骤7: 生成 HTML 报告
echo -e "\n${BLUE}📄 步骤7: 生成 HTML 报告${NC}"
echo "-------------------------------------------"

python3 .claude/skills/docker-integration-test/scripts/generate_report.py \
    --test-results "${RESULTS_DIR}" \
    --output "${REPORT_FILE}"

# 最终总结
echo -e "\n${BLUE}🎯 测试总结${NC}"
echo "================================================"

# 读取健康状态
if [ -f "${RESULTS_DIR}/health_status.json" ]; then
    HEALTHY_COUNT=$(jq '[.[] | select(.status == "healthy")] | length' "${RESULTS_DIR}/health_status.json")
    TOTAL_COUNT=$(jq 'length' "${RESULTS_DIR}/health_status.json")
    echo -e "健康检查: ${GREEN}${HEALTHY_COUNT}/${TOTAL_COUNT}${NC} 服务正常"
fi

echo -e "\n${GREEN}✅ 测试完成！${NC}"
echo -e "📊 详细报告: ${BLUE}${REPORT_FILE}${NC}"
echo -e "📁 结果目录: ${BLUE}${RESULTS_DIR}${NC}"

echo -e "\n${YELLOW}💡 提示: 使用 Ctrl+C 停止 Docker 服务${NC}"
echo -e "如需停止所有服务，请运行: ${BLUE}docker-compose down${NC}"
