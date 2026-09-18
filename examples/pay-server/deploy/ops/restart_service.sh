#!/bin/bash
# Pay Plugin Service Restart Script
# Description: Graceful restart of PayPlugin service with health check

set -e  # Exit on error

# ============================================================================
# Configuration
# ============================================================================
SERVICE_NAME="${SERVICE_NAME:-payserver}"
# /readyz (not the deprecated /health alias, whose Sunset header already reads
# 2026-08-28): the restart is only done once DB/Redis are actually reachable.
HEALTH_CHECK_URL="${HEALTH_CHECK_URL:-http://localhost:5566/readyz}"
MAX_WAIT="${MAX_WAIT:-60}"

# ============================================================================
# Functions
# ============================================================================

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1"
}

error_exit() {
    log "ERROR: $1"
    exit 1
}

# ============================================================================
# Pre-Restart Checks
# ============================================================================

log "Starting service restart: $SERVICE_NAME"

# Check if service is running
if systemctl is-active --quiet "$SERVICE_NAME"; then
    log "Service is currently running, stopping..."
    sudo systemctl stop "$SERVICE_NAME" || error_exit "Failed to stop service"
    log "Service stopped"
else
    log "Service is not running"
fi

# ============================================================================
# Start Service
# ============================================================================

log "Starting service..."
sudo systemctl start "$SERVICE_NAME" || error_exit "Failed to start service"

log "Service start command executed"

# ============================================================================
# Health Check
# ============================================================================

log "Waiting for service to be ready (max $MAX_WAIT seconds)..."
WAIT_TIME=0

while [ $WAIT_TIME -lt $MAX_WAIT ]; do
    if curl -f -s "$HEALTH_CHECK_URL" > /dev/null 2>&1; then
        log "Service is healthy!"
        break
    fi

    sleep 2
    WAIT_TIME=$((WAIT_TIME + 2))
    echo -n "."
done

echo ""

if [ $WAIT_TIME -ge $MAX_WAIT ]; then
    log "WARNING: Service did not become healthy within $MAX_WAIT seconds"
    log "Current service status:"
    sudo systemctl status "$SERVICE_NAME" --no-pager
    exit 1
fi

# ============================================================================
# Service Status
# ============================================================================

log "Service status:"
sudo systemctl status "$SERVICE_NAME" --no-pager -l

log "Restart completed successfully!"

exit 0
