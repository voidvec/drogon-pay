#!/bin/bash
# =============================================================================
# Pay Plugin E2E Test Script
# =============================================================================
# This script tests all major API endpoints using real HTTP requests
# It's much faster than updating all integration tests (3-4 hours vs 9-13 hours)
# =============================================================================

set -e  # Exit on error

# Configuration
BASE_URL="${BASE_URL:-http://localhost:5566}"
API_KEY="${API_KEY:-test-api-key}"
TEST_RESULTS=()

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# =============================================================================
# Helper Functions
# =============================================================================

log_test() {
    echo -e "${YELLOW}[TEST]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    TEST_RESULTS+=("PASS: $1")
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    TEST_RESULTS+=("FAIL: $1")
}

log_info() {
    echo -e "[INFO] $1"
}

# Generate random ID for testing
generate_id() {
    echo "test_$(date +%s)_$RANDOM"
}

# =============================================================================
# Pre-test Checks
# =============================================================================

check_server() {
    log_test "Checking if PayServer is running at $BASE_URL"

    if curl -s -f "$BASE_URL/metrics" > /dev/null 2>&1; then
        log_pass "Server is running"
        return 0
    else
        log_fail "Server is not running. Please start PayServer first:"
        log_info "  cd build/linux-release/examples/pay-server"
        log_info "  ./PayServer"
        exit 1
    fi
}

# =============================================================================
# Test Cases
# =============================================================================

test_create_payment() {
    log_test "Creating payment"

    local order_no=$(generate_id)
    local response=$(curl -s -X POST "$BASE_URL/pay/create" \
        -H "Idempotency-Key: ${order_no}_idempotency" \
        -H "Content-Type: application/json" \
        -d "{
            \"user_id\": \"10001\",
            \"amount\": \"9.99\",
            \"currency\": \"CNY\",
            \"title\": \"E2E Test Order\"
        }")

    # Check if response contains expected fields
    if echo "$response" | grep -q "order_no"; then
        log_pass "Payment created successfully"
        echo "$response" | grep -q "PAYING" && log_pass "Status is PAYING"
        # Save order_no for subsequent tests
        TEST_ORDER_NO=$(echo "$response" | grep -o '"order_no":"[^"]*"' | cut -d'"' -f4)
        TEST_PAYMENT_NO=$(echo "$response" | grep -o '"payment_no":"[^"]*"' | cut -d'"' -f4)
        log_info "order_no: $TEST_ORDER_NO"
        log_info "payment_no: $TEST_PAYMENT_NO"
    else
        log_fail "Failed to create payment"
        echo "Response: $response"
        return 1
    fi
}

test_query_order() {
    log_test "Querying order"

    if [ -z "$TEST_ORDER_NO" ]; then
        log_fail "No order_no available. Create payment first."
        return 1
    fi

    local response=$(curl -s "$BASE_URL/pay/query?order_no=$TEST_ORDER_NO" \
        -H "X-Api-Key: $API_KEY")

    if echo "$response" | grep -q "$TEST_ORDER_NO"; then
        log_pass "Order queried successfully"
        echo "$response" | grep -q "\"status\":\"[PAYINGPAIDREFUNDINGREFUND_SUCCESSREFUND_FAILCLOSED]\"" && \
            log_pass "Order has valid status"
    else
        log_fail "Failed to query order"
        echo "Response: $response"
        return 1
    fi
}

test_create_refund() {
    log_test "Creating refund"

    if [ -z "$TEST_ORDER_NO" ] || [ -z "$TEST_PAYMENT_NO" ]; then
        log_fail "No order/payment available. Create payment first."
        return 1
    fi

    local refund_no=$(generate_id)
    local response=$(curl -s -X POST "$BASE_URL/pay/refund" \
        -H "Idempotency-Key: ${refund_no}_idempotency" \
        -H "X-Api-Key: $API_KEY" \
        -H "Content-Type: application/json" \
        -d "{
            \"order_no\": \"$TEST_ORDER_NO\",
            \"payment_no\": \"$TEST_PAYMENT_NO\",
            \"amount\": \"9.99\"
        }")

    if echo "$response" | grep -q "refund_no"; then
        log_pass "Refund created successfully"
        TEST_REFUND_NO=$(echo "$response" | grep -o '"refund_no":"[^"]*"' | cut -d'"' -f4)
        log_info "refund_no: $TEST_REFUND_NO"
    else
        log_fail "Failed to create refund"
        echo "Response: $response"
        return 1
    fi
}

test_query_refund() {
    log_test "Querying refund"

    if [ -z "$TEST_REFUND_NO" ]; then
        log_fail "No refund_no available. Create refund first."
        return 1
    fi

    local response=$(curl -s "$BASE_URL/pay/refund/query?refund_no=$TEST_REFUND_NO" \
        -H "X-Api-Key: $API_KEY")

    if echo "$response" | grep -q "$TEST_REFUND_NO"; then
        log_pass "Refund queried successfully"
        echo "$response" | grep -q "\"status\":\"[REFUNDINGREFUND_SUCCESSREFUND_FAIL]\"" && \
            log_pass "Refund has valid status"
    else
        log_fail "Failed to query refund"
        echo "Response: $response"
        return 1
    fi
}

test_auth_metrics() {
    log_test "Testing auth metrics endpoint"

    local response=$(curl -s "$BASE_URL/pay/metrics/auth" \
        -H "X-Api-Key: $API_KEY")

    if echo "$response" | grep -q "metrics"; then
        log_pass "Auth metrics endpoint working"
    else
        log_fail "Auth metrics endpoint failed"
        echo "Response: $response"
    fi
}

test_prometheus_metrics() {
    log_test "Testing Prometheus metrics endpoint"

    local response=$(curl -s "$BASE_URL/metrics")

    if echo "$response" | grep -q "HELP"; then
        log_pass "Prometheus metrics endpoint working"
    else
        log_fail "Prometheus metrics endpoint failed"
        echo "Response: $response"
    fi
}

# =============================================================================
# Main Test Runner
# =============================================================================

main() {
    echo "=============================================="
    echo "Pay Plugin E2E Test Suite"
    echo "=============================================="
    echo "Base URL: $BASE_URL"
    echo "API Key: $API_KEY"
    echo "=============================================="
    echo ""

    # Pre-test checks
    check_server
    echo ""

    # Run tests
    test_create_payment
    echo ""

    test_query_order
    echo ""

    test_create_refund
    echo ""

    test_query_refund
    echo ""

    test_auth_metrics
    echo ""

    test_prometheus_metrics
    echo ""

    # Print summary
    echo "=============================================="
    echo "Test Summary"
    echo "=============================================="

    local passed=0
    local failed=0

    for result in "${TEST_RESULTS[@]}"; do
        if [[ $result == PASS* ]]; then
            ((passed++))
        else
            ((failed++))
        fi
        echo "  $result"
    done

    echo "=============================================="
    echo "Total: $((passed + failed)) tests"
    echo "Passed: $passed"
    echo "Failed: $failed"
    echo "=============================================="

    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    else
        echo -e "${RED}Some tests failed!${NC}"
        exit 1
    fi
}

# Run main function
main "$@"
