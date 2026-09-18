#!/bin/bash
# Pay Server Health Check Script
# Usage: ./healthcheck.sh [options]
#   -v, --verbose    Enable verbose output
#   -q, --quiet      Suppress output (exit code only)
#   -h, --help       Show this help message

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG_FILE="$PROJECT_ROOT/config.json"

# Default values
VERBOSE=false
QUIET=false

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -q|--quiet)
            QUIET=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "  -v, --verbose    Enable verbose output"
            echo "  -q, --quiet      Suppress output (exit code only)"
            echo "  -h, --help       Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Logging functions
log_info() {
    [[ "$QUIET" == "false" ]] && echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    [[ "$QUIET" == "false" ]] && echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    [[ "$QUIET" == "false" ]] && echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    [[ "$VERBOSE" == "true" ]] && [[ "$QUIET" == "false" ]] && echo -e "[DEBUG] $1"
}

# Load configuration from config.json or environment
load_config() {
    if [[ -f "$CONFIG_FILE" ]]; then
        log_debug "Loading configuration from $CONFIG_FILE"

        # Extract server configuration
        SERVER_HOST=$(jq -r '.listeners[0].address // "0.0.0.0"' "$CONFIG_FILE")
        SERVER_PORT=$(jq -r '.listeners[0].port // 8080' "$CONFIG_FILE")

        # Extract database configuration
        DB_HOST=$(jq -r '.plugins[0].config.database.host // "localhost"' "$CONFIG_FILE")
        DB_PORT=$(jq -r '.plugins[0].config.database.port // 5432' "$CONFIG_FILE")
        DB_NAME=$(jq -r '.plugins[0].config.database.dbname // "pay_test"' "$CONFIG_FILE")
        DB_USER=$(jq -r '.plugins[0].config.database.user // "postgres"' "$CONFIG_FILE")

        # Extract Redis configuration
        REDIS_HOST=$(jq -r '.plugins[0].config.redis.host // "localhost"' "$CONFIG_FILE")
        REDIS_PORT=$(jq -r '.plugins[0].config.redis.port // 6379' "$CONFIG_FILE")
    else
        log_debug "Using environment variables"

        SERVER_HOST=${SERVER_HOST:-localhost}
        SERVER_PORT=${SERVER_PORT:-8080}
        DB_HOST=${DB_HOST:-localhost}
        DB_PORT=${DB_PORT:-5432}
        DB_NAME=${DB_NAME:-pay_test}
        DB_USER=${DB_USER:-postgres}
        REDIS_HOST=${REDIS_HOST:-localhost}
        REDIS_PORT=${REDIS_PORT:-6379}
    fi

    log_debug "Server: $SERVER_HOST:$SERVER_PORT"
    log_debug "Database: $DB_HOST:$DB_PORT/$DB_NAME"
    log_debug "Redis: $REDIS_HOST:$REDIS_PORT"
}

# Check HTTP endpoint
check_http() {
    local url="http://$SERVER_HOST:$SERVER_PORT/api/health"
    log_debug "Checking HTTP endpoint: $url"

    local response=$(curl -s -w "\n%{http_code}" "$url" 2>/dev/null || echo "000")
    local http_code=$(echo "$response" | tail -n1)
    local body=$(echo "$response" | head -n-1)

    if [[ "$http_code" == "200" ]]; then
        log_info "HTTP health check: OK (200)"
        [[ "$VERBOSE" == "true" ]] && log_debug "Response: $body"
        return 0
    else
        log_error "HTTP health check: FAILED ($http_code)"
        return 1
    fi
}

# Check database connection
check_database() {
    log_debug "Checking database connection: $DB_HOST:$DB_PORT"

    if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -c "SELECT 1;" &> /dev/null; then
        log_info "Database connection: OK"
        return 0
    else
        log_error "Database connection: FAILED"
        return 1
    fi
}

# Check Redis connection
check_redis() {
    log_debug "Checking Redis connection: $REDIS_HOST:$REDIS_PORT"

    if redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" ping | grep -q "PONG"; then
        log_info "Redis connection: OK"
        return 0
    else
        log_error "Redis connection: FAILED"
        return 1
    fi
}

# Check disk space
check_disk_space() {
    log_debug "Checking disk space..."

    local threshold=90
    local usage=$(df -h / | awk 'NR==2 {print $5}' | sed 's/%//')

    if [[ $usage -lt $threshold ]]; then
        log_info "Disk space: OK (${usage}% used)"
        return 0
    else
        log_warn "Disk space: LOW (${usage}% used, threshold: ${threshold}%)"
        return 1
    fi
}

# Check memory usage
check_memory() {
    log_debug "Checking memory usage..."

    local total_mem=$(free -m | awk 'NR==2 {print $2}')
    local used_mem=$(free -m | awk 'NR==2 {print $3}')
    local usage=$((used_mem * 100 / total_mem))

    if [[ $usage -lt 90 ]]; then
        log_info "Memory usage: OK (${usage}% used)"
        return 0
    else
        log_warn "Memory usage: HIGH (${usage}% used)"
        return 1
    fi
}

# Main health check
main() {
    local exit_code=0

    load_config

    log_info "Starting health check..."
    echo "----------------------------------------"

    # Run checks
    check_http || exit_code=1
    check_database || exit_code=1
    check_redis || exit_code=1
    check_disk_space || exit_code=1
    check_memory || exit_code=1

    echo "----------------------------------------"

    if [[ $exit_code -eq 0 ]]; then
        log_info "All health checks passed"
    else
        log_error "Some health checks failed"
    fi

    exit $exit_code
}

# Run main
main
