#!/bin/bash
# Configuration Check Script
# Validates examples/pay-server/config.json for missing or placeholder values

echo "========================================"
echo "Pay Server Configuration Check"
echo "========================================"
echo ""

CONFIG_FILE="examples/pay-server/config.json"
ISSUES_FOUND=0

# Check if config file exists
if [ ! -f "$CONFIG_FILE" ]; then
    echo "[ERROR] ERROR: config.json not found at $CONFIG_FILE"
    exit 1
fi

echo "[PASS] config.json found"
echo ""

# Function to check for placeholder values
check_placeholder() {
    local key=$1
    local value=$2
    local field=$3
    
    if echo "$value" | grep -qiE "YOUR_|FIXME|TODO|example\.com|localhost"; then
        echo "[ERROR] $key: Placeholder value detected"
        echo "   Field: $field"
        echo "   Current value: \"$value\""
        echo ""
        ((ISSUES_FOUND++))
        return 1
    else
        echo "[PASS] $key: OK"
        return 0
    fi
}

# Check WeChat Pay configuration
echo "Checking WeChat Pay configuration..."
echo ""

check_placeholder "AppID" \
    "$(grep -oP '"app_id":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)" \
    "channels.wechat.app_id"

check_placeholder "Merchant ID" \
    "$(grep -oP '"mch_id":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)" \
    "channels.wechat.mch_id"

check_placeholder "Serial No" \
    "$(grep -oP '"serial_no":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)" \
    "channels.wechat.serial_no"

check_placeholder "API v3 Key" \
    "$(grep -oP '"api_v3_key":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)" \
    "channels.wechat.api_v3_key"

check_placeholder "Notify URL" \
    "$(grep -oP '"notify_url":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)" \
    "channels.wechat.notify_url"

# Check certificate files
echo ""
echo "Checking certificate files..."
echo ""

PRIVATE_KEY_PATH="$(grep -oP '"private_key_path":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)"
PLATFORM_CERT_PATH="$(grep -oP '"platform_cert_path":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)"

if [ ! -f "examples/pay-server/$PRIVATE_KEY_PATH" ]; then
    echo "[ERROR] Private key certificate not found"
    echo "   Expected path: examples/pay-server/$PRIVATE_KEY_PATH"
    echo ""
    ((ISSUES_FOUND++))
else
    echo "[PASS] Private key certificate found"
fi

if [ ! -f "examples/pay-server/$PLATFORM_CERT_PATH" ]; then
    echo "[ERROR] Platform certificate not found"
    echo "   Expected path: examples/pay-server/$PLATFORM_CERT_PATH"
    echo ""
    ((ISSUES_FOUND++))
else
    echo "[PASS] Platform certificate found"
fi

# Check HTTPS
echo ""
echo "Checking HTTPS configuration..."
echo ""

HTTPS_ENABLED=$(grep -oP '"https":\s*\K[true|false]' "$CONFIG_FILE" | head -1)
if [ "$HTTPS_ENABLED" == "false" ]; then
    echo "[WARNING]ï¸? HTTPS is disabled (HTTP only)"
    echo "   Impact: All traffic is unencrypted"
    echo "   Recommendation: Enable for production"
    echo ""
else
    echo "[PASS] HTTPS is enabled"
fi

# Check database password
echo ""
echo "Checking database security..."
echo ""

DB_PASSWD=$(grep -oP '"passwd":\s*"\K[^"]*' "$CONFIG_FILE" | head -1)
if [ "$DB_PASSWD" == "123456" ]; then
    echo "[WARNING]ï¸? Weak database password detected"
    echo "   Current value: \"$DB_PASSWD\""
    echo "   Recommendation: Use strong password or environment variable"
    echo ""
else
    echo "[PASS] Database password configured"
fi

REDIS_PASSWD=$(grep -A 10 '"redis_clients"' "$CONFIG_FILE" | grep -oP '"passwd":\s*"\K[^"]*' | head -1)
if [ "$REDIS_PASSWD" == "123456" ]; then
    echo "[WARNING]ï¸? Weak Redis password detected"
    echo "   Current value: \"$REDIS_PASSWD\""
    echo "   Recommendation: Use strong password or environment variable"
    echo ""
else
    echo "[PASS] Redis password configured"
fi

# Check for environment variables
echo ""
echo "Checking environment variables..."
echo ""

if [ -z "$PAY_API_KEY" ] && [ -z "$PAY_API_KEYS" ]; then
    echo "[WARNING]ï¸? No API keys configured in environment"
    echo "   Set PAY_API_KEY or PAY_API_KEYS environment variable"
    echo ""
    ((ISSUES_FOUND++))
else
    echo "[PASS] API keys configured in environment"
fi

# Summary
echo ""
echo "========================================"
echo "Check Summary"
echo "========================================"

if [ $ISSUES_FOUND -eq 0 ]; then
    echo "[PASS] All critical configurations are valid!"
    echo ""
    echo "[WARNING]ï¸? Warnings may still exist. Review above for recommendations."
    exit 0
else
    echo "[ERROR] Found $ISSUES_FOUND configuration issue(s) that need attention"
    echo ""
    echo "Next steps:"
    echo "1. Review configuration documentation: docs/configuration_status.md"
    echo "2. Update config.json with actual values"
    echo "3. Obtain WeChat Pay credentials and certificates"
    echo "4. Run this script again to verify"
    exit 1
fi
