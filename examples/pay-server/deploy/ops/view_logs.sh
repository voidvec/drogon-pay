#!/bin/bash
# Pay Plugin Log Viewer Script
# Description: View and filter PayPlugin logs

# ============================================================================
# Configuration
# ============================================================================
SERVICE_NAME="${SERVICE_NAME:-payserver}"
LOG_LINES="${LOG_LINES:-100}"

# ============================================================================
# Functions
# ============================================================================

show_help() {
    cat << EOF
Pay Plugin Log Viewer

Usage: $0 [OPTIONS]

Options:
    -f, --follow        Follow log output (like tail -f)
    -n, --lines N       Show last N lines (default: 100)
    -e, --errors        Show only errors
    -w, --warnings      Show only warnings and errors
    -s, --since TIME    Show logs since TIME (e.g., "1 hour ago", "2026-04-13 10:00:00")
    -p, --pattern PATTERN  Filter by PATTERN (grep)
    -h, --help          Show this help

Examples:
    $0 -f                           # Follow logs in real-time
    $0 -e                           # Show only errors
    $0 -n 50                        # Show last 50 lines
    $0 -w -s "1 hour ago"           # Show warnings and errors from last hour
    $0 -p "payment_no=TXN"          # Show lines containing "payment_no=TXN"

EOF
}

# ============================================================================
# Parse Arguments
# ============================================================================

FOLLOW=false
SHOW_ERRORS=false
SHOW_WARNINGS=false
SINCE=""
PATTERN=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--follow)
            FOLLOW=true
            shift
            ;;
        -n|--lines)
            LOG_LINES="$2"
            shift 2
            ;;
        -e|--errors)
            SHOW_ERRORS=true
            shift
            ;;
        -w|--warnings)
            SHOW_WARNINGS=true
            shift
            ;;
        -s|--since)
            SINCE="$2"
            shift 2
            ;;
        -p|--pattern)
            PATTERN="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# ============================================================================
# Build Journalctl Command
# ============================================================================

CMD="sudo journalctl -u $SERVICE_NAME"

if [ "$FOLLOW" = true ]; then
    CMD="$CMD -f"
else
    CMD="$CMD -n $LOG_LINES"
fi

if [ -n "$SINCE" ]; then
    CMD="$CMD --since '$SINCE'"
fi

if [ "$SHOW_ERRORS" = true ]; then
    CMD="$CMD -p err"
elif [ "$SHOW_WARNINGS" = true ]; then
    CMD="$CMD -p err -p warn"
fi

# ============================================================================
# Execute Command
# ============================================================================

if [ -n "$PATTERN" ]; then
    eval $CMD | grep --color=auto "$PATTERN"
else
    eval $CMD
fi
