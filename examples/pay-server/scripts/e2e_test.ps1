# =============================================================================
# Pay Plugin E2E Test Script (PowerShell)
# =============================================================================
# This script tests all major API endpoints using real HTTP requests
# It's much faster than updating all integration tests (3-4 hours vs 9-13 hours)
# =============================================================================

# Configuration
$BaseUrl = if ($env:BASE_URL) { $env:BASE_URL } else { "http://localhost:5566" }
# Default matches PAY_API_KEY in examples/pay-server/.env; override via $env:API_KEY
$ApiKey = if ($env:API_KEY) { $env:API_KEY } else { "test_key_123456" }
$TestResults = @()

# =============================================================================
# Helper Functions
# =============================================================================

function Log-Test {
    param([string]$Message)
    Write-Host "[TEST] $Message" -ForegroundColor Yellow
}

function Log-Pass {
    param([string]$Message)
    Write-Host "[PASS] $Message" -ForegroundColor Green
    $script:TestResults += "PASS: $Message"
}

function Log-Fail {
    param([string]$Message)
    Write-Host "[FAIL] $Message" -ForegroundColor Red
    $script:TestResults += "FAIL: $Message"
}

function Log-Info {
    param([string]$Message)
    Write-Host "[INFO] $Message"
}

function Get-RandomId {
    return "test_$(Get-Date -Format 'yyyyMMddHHmmss')_$((Get-Random -Maximum 9999))"
}

# =============================================================================
# Pre-test Checks
# =============================================================================

function Test-Server {
    Log-Test "Checking if PayServer is running at $BaseUrl"

    try {
        $response = Invoke-RestMethod -Uri "$BaseUrl/metrics" -Method Get -ErrorAction Stop
        Log-Pass "Server is running"
        return $true
    }
    catch {
        Log-Fail "Server is not running. Please start PayServer first:"
        Log-Info "  examples\pay-server\scripts\run_server.bat"
        Log-Info "  (or run build\windows-msvc\examples\pay-server\Release\PayServer.exe from that directory)"
        exit 1
    }
}

# =============================================================================
# Test Cases
# =============================================================================

$script:TestOrderNo = ""
$script:TestPaymentNo = ""
$script:TestRefundNo = ""

function Test-CreatePayment {
    Log-Test "Creating payment"

    $orderNo = Get-RandomId
    $body = @{
        order_no = $orderNo
        user_id = 10001
        amount = "9.99"
        currency = "CNY"
        title = "E2E Test Order"
    } | ConvertTo-Json

    try {
        $headers = @{
            "Idempotency-Key" = "${orderNo}_idempotency"
            "X-Api-Key" = $ApiKey
            "Content-Type" = "application/json"
        }

        $response = Invoke-RestMethod -Uri "$BaseUrl/api/pay/create" `
            -Method Post `
            -Headers $headers `
            -Body $body `
            -ErrorAction Stop

        if ($response.order_no) {
            Log-Pass "Payment created successfully"
            if ($response.status -eq "PAYING") {
                Log-Pass "Status is PAYING"
            }
            $script:TestOrderNo = $response.order_no
            $script:TestPaymentNo = $response.payment_no
            Log-Info "order_no: $TestOrderNo"
            Log-Info "payment_no: $TestPaymentNo"
        }
        else {
            Log-Fail "Response missing order_no"
            Log-Info "Response: $($response | ConvertTo-Json)"
        }
    }
    catch {
        Log-Fail "Failed to create payment: $($_.Exception.Message)"
    }
}

function Test-QueryOrder {
    Log-Test "Querying order"

    if (-not $script:TestOrderNo) {
        Log-Fail "No order_no available. Create payment first."
        return
    }

    try {
        $headers = @{
            "X-Api-Key" = $ApiKey
        }

        $response = Invoke-RestMethod -Uri "$BaseUrl/api/pay/query?order_no=$TestOrderNo" `
            -Method Get `
            -Headers $headers `
            -ErrorAction Stop

        if ($response.data.order_no -eq $TestOrderNo) {
            Log-Pass "Order queried successfully"
            $validStatuses = @("PAYING", "PAID", "REFUNDING", "REFUND_SUCCESS", "REFUND_FAIL", "CLOSED")
            if ($response.data.status -in $validStatuses) {
                Log-Pass "Order has valid status: $($response.data.status)"
            }
        }
        else {
            Log-Fail "Order_no mismatch"
            Log-Info "Response: $($response | ConvertTo-Json)"
        }
    }
    catch {
        Log-Fail "Failed to query order: $($_.Exception.Message)"
    }
}

function Test-CreateRefund {
    Log-Test "Creating refund"

    if (-not $script:TestOrderNo -or -not $script:TestPaymentNo) {
        Log-Fail "No order/payment available. Create payment first."
        return
    }

    $refundNo = Get-RandomId
    $body = @{
        order_no = $TestOrderNo
        payment_no = $TestPaymentNo
        amount = "9.99"
    } | ConvertTo-Json

    try {
        $headers = @{
            "Idempotency-Key" = "${refundNo}_idempotency"
            "X-Api-Key" = $ApiKey
            "Content-Type" = "application/json"
        }

        $response = Invoke-RestMethod -Uri "$BaseUrl/api/pay/refund" `
            -Method Post `
            -Headers $headers `
            -Body $body `
            -ErrorAction Stop

        if ($response.refund_no) {
            Log-Pass "Refund created successfully"
            $script:TestRefundNo = $response.refund_no
            Log-Info "refund_no: $TestRefundNo"
        }
        else {
            Log-Fail "Response missing refund_no"
            Log-Info "Response: $($response | ConvertTo-Json)"
        }
    }
    catch {
        Log-Fail "Failed to create refund: $($_.Exception.Message)"
    }
}

function Test-QueryRefund {
    Log-Test "Querying refund"

    if (-not $script:TestRefundNo) {
        Log-Fail "No refund_no available. Create refund first."
        return
    }

    try {
        $headers = @{
            "X-Api-Key" = $ApiKey
        }

        $response = Invoke-RestMethod -Uri "$BaseUrl/api/pay/refund/query?refund_no=$TestRefundNo" `
            -Method Get `
            -Headers $headers `
            -ErrorAction Stop

        if ($response.data.refund_no -eq $TestRefundNo) {
            Log-Pass "Refund queried successfully"
            $validStatuses = @("REFUNDING", "REFUND_SUCCESS", "REFUND_FAIL")
            if ($response.data.status -in $validStatuses) {
                Log-Pass "Refund has valid status: $($response.data.status)"
            }
        }
        else {
            Log-Fail "Refund_no mismatch"
            Log-Info "Response: $($response | ConvertTo-Json)"
        }
    }
    catch {
        Log-Fail "Failed to query refund: $($_.Exception.Message)"
    }
}

function Test-AuthMetrics {
    Log-Test "Testing auth metrics endpoint"

    try {
        $headers = @{
            "X-Api-Key" = $ApiKey
        }

        $response = Invoke-RestMethod -Uri "$BaseUrl/api/pay/metrics/auth" `
            -Method Get `
            -Headers $headers `
            -ErrorAction Stop

        if ($response.metrics -or $response.PSObject.Properties.Count -gt 0) {
            Log-Pass "Auth metrics endpoint working"
        }
        else {
            Log-Fail "Auth metrics endpoint returned empty response"
        }
    }
    catch {
        Log-Fail "Failed to get auth metrics: $($_.Exception.Message)"
    }
}

function Test-PrometheusMetrics {
    Log-Test "Testing Prometheus metrics endpoint"

    try {
        $response = Invoke-RestMethod -Uri "$BaseUrl/metrics" `
            -Method Get `
            -ErrorAction Stop

        $responseString = $response | Out-String
        if ($responseString -match "HELP") {
            Log-Pass "Prometheus metrics endpoint working"
        }
        else {
            Log-Fail "Prometheus metrics endpoint response unexpected"
        }
    }
    catch {
        Log-Fail "Failed to get Prometheus metrics: $($_.Exception.Message)"
    }
}

# =============================================================================
# Main Test Runner
# =============================================================================

function Main {
    Write-Host "==============================================" -ForegroundColor Cyan
    Write-Host "Pay Plugin E2E Test Suite" -ForegroundColor Cyan
    Write-Host "==============================================" -ForegroundColor Cyan
    Write-Host "Base URL: $BaseUrl"
    Write-Host "API Key: $ApiKey"
    Write-Host "==============================================" -ForegroundColor Cyan
    Write-Host ""

    # Pre-test checks
    Test-Server
    Write-Host ""

    # Run tests
    Test-CreatePayment
    Write-Host ""

    Test-QueryOrder
    Write-Host ""

    Test-CreateRefund
    Write-Host ""

    Test-QueryRefund
    Write-Host ""

    Test-AuthMetrics
    Write-Host ""

    Test-PrometheusMetrics
    Write-Host ""

    # Print summary
    Write-Host "==============================================" -ForegroundColor Cyan
    Write-Host "Test Summary" -ForegroundColor Cyan
    Write-Host "==============================================" -ForegroundColor Cyan

    $passed = 0
    $failed = 0

    foreach ($result in $script:TestResults) {
        if ($result -like "PASS:*") {
            $passed++
            Write-Host "  $result" -ForegroundColor Green
        }
        else {
            $failed++
            Write-Host "  $result" -ForegroundColor Red
        }
    }

    Write-Host "==============================================" -ForegroundColor Cyan
    Write-Host "Total: $($passed + $failed) tests"
    Write-Host "Passed: $passed"
    Write-Host "Failed: $failed"
    Write-Host "==============================================" -ForegroundColor Cyan

    if ($failed -eq 0) {
        Write-Host "All tests passed!" -ForegroundColor Green
        exit 0
    }
    else {
        Write-Host "Some tests failed!" -ForegroundColor Red
        exit 1
    }
}

# Run main function
Main
