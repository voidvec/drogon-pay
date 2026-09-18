#!/bin/bash
# Pay Server Deployment Script for Linux/macOS
# Usage: ./deploy.sh [environment]
#   environment: development | staging | production (default: development)

set -e  # Exit on error
set -o pipefail  # Exit on pipe failure

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(dirname "$(dirname "$PROJECT_ROOT")")"
BUILD_DIR="$PROJECT_ROOT/build"
# migrate_db.py is the only migration executor; bash has no `python` alias
# guarantee the way Windows batch does.
PYTHON="$(command -v python3 || command -v python || true)"
DEPLOY_ENV="${1:-development}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
check_root() {
    if [[ $EUID -eq 0 ]]; then
        log_error "This script should not be run as root"
        exit 1
    fi
}

# Load environment variables
load_env() {
    local env_file="$PROJECT_ROOT/.env.$DEPLOY_ENV"

    if [[ ! -f "$env_file" ]]; then
        log_warn "Environment file not found: $env_file"
        log_info "Using default .env.example"
        env_file="$PROJECT_ROOT/.env.example"
    fi

    if [[ -f "$env_file" ]]; then
        log_info "Loading environment from: $env_file"
        export $(cat "$env_file" | grep -v '^#' | grep -v '^[[:space:]]*$' | xargs)
    fi
}

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."

    local missing_deps=()

    # Check required commands ("pgredis" here used to abort every run: the
    # client shipped with Redis is redis-cli, which check_redis calls below)
    for cmd in cmake curl redis-cli psql; do
        if ! command -v $cmd &> /dev/null; then
            missing_deps+=($cmd)
        fi
    done

    if [[ -z "$PYTHON" ]]; then
        log_error "Neither python3 nor python is on PATH (scripts/migrate_db.py needs one)"
        exit 1
    fi

    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        log_error "Missing dependencies: ${missing_deps[*]}"
        log_info "Install missing dependencies and try again"
        exit 1
    fi

    log_info "All prerequisites met"
}

# Build application
build_app() {
    log_info "Building application for $DEPLOY_ENV environment..."

    cd "$PROJECT_ROOT"

    # Create build directory
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Configure CMake
    log_info "Configuring CMake..."
    cmake .. -DCMAKE_BUILD_TYPE=Release

    # Build
    log_info "Compiling..."
    make -j$(nproc)

    log_info "Build complete"
}

# Setup database
setup_database() {
    log_info "Setting up database..."

    # Provisioning: the app role has no CREATEDB, so creating the database is a
    # superuser step and stays here rather than moving into the executor.
    if psql -h "$DB_HOST" -U "$DB_USER" -p "$DB_PORT" -lqt | cut -d \| -f 1 | grep -qw "$DB_NAME"; then
        log_warn "Database $DB_NAME already exists"
    else
        log_info "Creating database: $DB_NAME"
        psql -h "$DB_HOST" -U "$DB_USER" -p "$DB_PORT" -c "CREATE DATABASE $DB_NAME;"
    fi

    # Apply migrations with the one executor. This used to be a
    # "for migration in $PROJECT_ROOT/sql/*.sql" loop, which was dead twice
    # over: sql/ moved to the repository root after the plugin refactor, so the
    # glob matched nothing and the step quietly succeeded without applying
    # anything. It also ran 000_drop_pay_tables.sql, which on a redeploy to
    # staging would have dropped every table along with its data.
    log_info "Running database migrations..."
    "$PYTHON" "$REPO_ROOT/scripts/migrate_db.py" \
        --host "$DB_HOST" --port "$DB_PORT" --user "$DB_USER" --db "$DB_NAME" \
        || { log_error "migrate_db.py failed - see its output above"; exit 1; }

    log_info "Database setup complete"
}

# Check Redis connection
check_redis() {
    log_info "Checking Redis connection..."

    if redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" ping | grep -q "PONG"; then
        log_info "Redis connection successful"
    else
        log_error "Cannot connect to Redis at $REDIS_HOST:$REDIS_PORT"
        exit 1
    fi
}

# Setup certificates
setup_certificates() {
    log_info "Setting up certificates..."

    local cert_dir="$PROJECT_ROOT/certs"

    if [[ ! -d "$cert_dir" ]]; then
        log_info "Creating certificates directory: $cert_dir"
        mkdir -p "$cert_dir"
    fi

    # Check if WeChat certificates exist
    if [[ ! -f "$cert_dir/apiclient_cert.pem" ]] || [[ ! -f "$cert_dir/apiclient_key.pem" ]]; then
        log_warn "WeChat Pay certificates not found in $cert_dir"
        log_info "Please place your certificates in the certs directory"
    fi
}

# Create systemd service (Linux only)
create_systemd_service() {
    if [[ "$OSTYPE" != "linux-gnu"* ]]; then
        return
    fi

    log_info "Creating systemd service..."

    local service_file="/etc/systemd/system/payserver.service"

    sudo tee "$service_file" > /dev/null <<EOF
[Unit]
Description=Pay Server
After=network.target postgresql.service redis.service

[Service]
Type=simple
User=$(whoami)
WorkingDirectory=$BUILD_DIR/Release
ExecStart=$BUILD_DIR/Release/PayServer
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

    sudo systemctl daemon-reload
    sudo systemctl enable payserver

    log_info "Systemd service created: $service_file"
}

# Health check
health_check() {
    log_info "Performing health check..."

    local max_attempts=30
    local attempt=1

    while [[ $attempt -le $max_attempts ]]; do
        if curl -f -s "http://$SERVER_HOST:$SERVER_PORT/api/health" > /dev/null 2>&1; then
            log_info "Health check passed"
            return 0
        fi

        log_info "Waiting for server to start... ($attempt/$max_attempts)"
        sleep 2
        ((attempt++))
    done

    log_error "Health check failed after $max_attempts attempts"
    return 1
}

# Deploy
deploy() {
    log_info "Starting deployment for $DEPLOY_ENV environment..."

    check_root
    load_env
    check_prerequisites
    build_app

    case "$DEPLOY_ENV" in
        development)
            # Development: local setup
            setup_database
            check_redis
            setup_certificates
            ;;
        staging|production)
            # Production: systemd service
            setup_database
            check_redis
            setup_certificates
            create_systemd_service
            ;;
        *)
            log_error "Unknown environment: $DEPLOY_ENV"
            exit 1
            ;;
    esac

    log_info "Deployment complete!"

    # Start service if production
    if [[ "$DEPLOY_ENV" == "staging" ]] || [[ "$DEPLOY_ENV" == "production" ]]; then
        log_info "Starting payserver service..."
        sudo systemctl start payserver
        health_check
        log_info "Service started successfully"
        log_info "Check status with: sudo systemctl status payserver"
    else
        log_info "To start the server manually:"
        log_info "  cd $BUILD_DIR/Release"
        log_info "  ./PayServer"
    fi
}

# Rollback
rollback() {
    log_warn "Rolling back deployment..."

    if [[ "$DEPLOY_ENV" == "staging" ]] || [[ "$DEPLOY_ENV" == "production" ]]; then
        sudo systemctl stop payserver
        sudo systemctl start payserver
        log_info "Service restarted"
    fi
}

# Main
case "${2:-deploy}" in
    deploy)
        deploy
        ;;
    rollback)
        rollback
        ;;
    *)
        echo "Usage: $0 [environment] [command]"
        echo "  environment: development | staging | production"
        echo "  command: deploy | rollback"
        exit 1
        ;;
esac
