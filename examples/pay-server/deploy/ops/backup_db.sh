#!/bin/bash
# Pay Plugin Database Backup Script
# Description: Automated PostgreSQL database backup with compression and retention

set -e  # Exit on error

# ============================================================================
# Configuration
# ============================================================================
DB_NAME="${DB_NAME:-pay_production}"
DB_USER="${DB_USER:-pay_user}"
BACKUP_DIR="${BACKUP_DIR:-/var/backups/payserver}"
RETENTION_DAYS="${RETENTION_DAYS:-30}"
LOG_FILE="${LOG_FILE:-/var/log/payserver/backup.log}"

# Create backup directory
mkdir -p "$BACKUP_DIR"
mkdir -p "$(dirname "$LOG_FILE")"

# ============================================================================
# Functions
# ============================================================================

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE"
}

error_exit() {
    log "ERROR: $1"
    exit 1
}

# ============================================================================
# Backup Process
# ============================================================================

log "Starting database backup: $DB_NAME"

# Backup filename with timestamp
BACKUP_FILE="$BACKUP_DIR/payserver_$(date +%Y%m%d_%H%M%S).sql"

# Perform backup
log "Running pg_dump..."
pg_dump -U "$DB_USER" -d "$DB_NAME" -F p -f "$BACKUP_FILE" || error_exit "pg_dump failed"

# Compress backup
log "Compressing backup..."
gzip "$BACKUP_FILE" || error_exit "Compression failed"

BACKUP_FILE_GZ="$BACKUP_FILE.gz"

# Get file size
FILE_SIZE=$(du -h "$BACKUP_FILE_GZ" | cut -f1)

log "Backup completed successfully: $BACKUP_FILE_GZ ($FILE_SIZE)"

# Remove old backups (older than RETENTION_DAYS)
log "Cleaning up old backups (older than $RETENTION_DAYS days)..."
find "$BACKUP_DIR" -name "payserver_*.sql.gz" -mtime +"$RETENTION_DAYS" -delete -print | while read -r file; do
    log "Deleted old backup: $file"
done

# List current backups
BACKUP_COUNT=$(ls -1 "$BACKUP_DIR"/payserver_*.sql.gz 2>/dev/null | wc -l)
log "Total backups: $BACKUP_COUNT"

log "Backup process completed successfully"

exit 0
