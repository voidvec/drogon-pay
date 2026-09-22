#include <drogon/drogon_test.h>
#include "utils/PayUtils.h"

DROGON_TEST(PayUtils_GetRequiredString)
{
    Json::Value json;
    json["user_id"] = "123";
    json["amount"] = 456;

    std::string value;
    CHECK(pay::utils::getRequiredString(json, "user_id", value));
    CHECK(value == "123");

    value.clear();
    CHECK(pay::utils::getRequiredString(json, "amount", value));
    CHECK(value == "456");

    value.clear();
    CHECK(!pay::utils::getRequiredString(json, "missing", value));
}

DROGON_TEST(PayUtils_ParseAmountToFen)
{
    int64_t fen = 0;
    CHECK(pay::utils::parseAmountToFen("12.34", fen));
    CHECK(fen == 1234);

    CHECK(pay::utils::parseAmountToFen("12", fen));
    CHECK(fen == 1200);

    CHECK(pay::utils::parseAmountToFen("0.1", fen));
    CHECK(fen == 10);

    CHECK(pay::utils::parseAmountToFen("0.01", fen));
    CHECK(fen == 1);

    CHECK(pay::utils::parseAmountToFen(".5", fen));
    CHECK(fen == 50);

    CHECK(!pay::utils::parseAmountToFen("", fen));
    CHECK(!pay::utils::parseAmountToFen("12.345", fen));
    CHECK(!pay::utils::parseAmountToFen("12.a", fen));
    CHECK(!pay::utils::parseAmountToFen("-1.00", fen));
}

// The amount consistency gate compares fen with `!=`, so a value that wraps on
// yuan * 100 could collide with a small legitimate amount and be accepted.
// parseAmountToFen must refuse to produce a fen value it cannot represent.
DROGON_TEST(PayUtils_ParseAmountToFen_RejectsOverflow)
{
    int64_t fen = 0;

    // Largest amount that still fits: fen max is int64 max, so 92233720368547757.99.
    CHECK(pay::utils::parseAmountToFen("92233720368547757.99", fen));
    CHECK(fen == 9223372036854775799LL);

    // One above the representable scale: rejected rather than wrapping.
    CHECK(!pay::utils::parseAmountToFen("92233720368547758", fen));
    CHECK(!pay::utils::parseAmountToFen("92233720368547758.07", fen));

    // Past int64 entirely: stoll throws, the parse reports failure.
    CHECK(!pay::utils::parseAmountToFen("9223372036854775808", fen));
    CHECK(!pay::utils::parseAmountToFen("999999999999999999999999.99", fen));
}

DROGON_TEST(PayUtils_AmountEqualsFen)
{
    CHECK(pay::utils::amountEqualsFen("88.88", 8888));
    // Equivalent spellings of the same amount must compare equal.
    CHECK(pay::utils::amountEqualsFen("0.5", 50));
    CHECK(pay::utils::amountEqualsFen("0.50", 50));
    CHECK(pay::utils::amountEqualsFen("12", 1200));

    CHECK(!pay::utils::amountEqualsFen("88.88", 8889));
    CHECK(!pay::utils::amountEqualsFen("88.8", 8888));

    // Unparsable amounts never match, whatever was expected.
    CHECK(!pay::utils::amountEqualsFen("", 0));
    CHECK(!pay::utils::amountEqualsFen("12.345", 12345));

    // The caller's "could not resolve" sentinel is negative: fail closed.
    CHECK(!pay::utils::amountEqualsFen("88.88", -1));
    CHECK(!pay::utils::amountEqualsFen("0.00", -1));
}

DROGON_TEST(PayUtils_MapTradeState)
{
    std::string orderStatus;
    std::string paymentStatus;

    pay::utils::mapTradeState("SUCCESS", orderStatus, paymentStatus);
    CHECK(orderStatus == "PAID");
    CHECK(paymentStatus == "SUCCESS");

    pay::utils::mapTradeState("USERPAYING", orderStatus, paymentStatus);
    CHECK(orderStatus == "PAYING");
    CHECK(paymentStatus == "PROCESSING");

    pay::utils::mapTradeState("NOTPAY", orderStatus, paymentStatus);
    CHECK(orderStatus == "PAYING");
    CHECK(paymentStatus == "PROCESSING");

    pay::utils::mapTradeState("CLOSED", orderStatus, paymentStatus);
    CHECK(orderStatus == "CLOSED");
    CHECK(paymentStatus == "FAIL");

    pay::utils::mapTradeState("UNKNOWN", orderStatus, paymentStatus);
    CHECK(orderStatus == "FAILED");
    CHECK(paymentStatus == "FAIL");
}

DROGON_TEST(PayUtils_MapRefundStatus)
{
    CHECK(pay::utils::mapRefundStatus("SUCCESS") == "REFUND_SUCCESS");
    CHECK(pay::utils::mapRefundStatus("CLOSED") == "REFUND_FAIL");
    CHECK(pay::utils::mapRefundStatus("ABNORMAL") == "REFUND_FAIL");
    CHECK(pay::utils::mapRefundStatus("PROCESSING") == "REFUNDING");
    CHECK(pay::utils::mapRefundStatus("UNKNOWN") == "");
}

DROGON_TEST(PayUtils_ToJsonString)
{
    Json::Value root;
    root["order_id"] = "order_1";
    root["amount"] = 1200;

    const auto json = pay::utils::toJsonString(root);

    CHECK(json.find('\n') == std::string::npos);
    CHECK(json.find("\"order_id\"") != std::string::npos);
    CHECK(json.find("\"amount\"") != std::string::npos);
}

// P1-3 SSRF: validateNotifyUrl must reject private/loopback/link-local hosts
// that an attacker could set as the channel callback target.
DROGON_TEST(PayUtils_ValidateNotifyUrl)
{
    std::string err;

    // Empty URL is allowed (channel falls back to its configured default).
    CHECK(pay::utils::validateNotifyUrl("", err));
    CHECK(err.empty());

    // Public https URL is allowed.
    CHECK(pay::utils::validateNotifyUrl("https://example.com/callback", err));
    CHECK(pay::utils::validateNotifyUrl("http://203.0.113.10/cb", err));

    // Scheme + length checks.
    CHECK(!pay::utils::validateNotifyUrl("ftp://example.com/cb", err));
    CHECK(pay::utils::validateNotifyUrl("http://example.com/cb", err));
    // Oversize.
    std::string longUrl = "https://example.com/" + std::string(600, 'x');
    CHECK(!pay::utils::validateNotifyUrl(longUrl, err));

    // Loopback IPv4 literals (127/8).
    CHECK(!pay::utils::validateNotifyUrl("http://127.0.0.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://127.1.2.3:8080/cb", err));

    // Link-local / cloud metadata (169.254/16).
    CHECK(!pay::utils::validateNotifyUrl("http://169.254.169.254/latest/meta-data/", err));
    CHECK(!pay::utils::validateNotifyUrl("http://169.254.0.1/cb", err));

    // RFC1918 private ranges.
    CHECK(!pay::utils::validateNotifyUrl("http://10.0.0.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://172.16.0.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://172.31.255.255/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://192.168.1.1/cb", err));

    // 0.0.0.0/8 and CGNAT 100.64/10.
    CHECK(!pay::utils::validateNotifyUrl("http://0.0.0.0/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://100.64.0.1/cb", err));

    // A public IPv4 just outside private ranges must pass.
    CHECK(pay::utils::validateNotifyUrl("http://11.0.0.1/cb", err));
    CHECK(pay::utils::validateNotifyUrl("http://172.32.0.1/cb", err));

    // "localhost" domain.
    CHECK(!pay::utils::validateNotifyUrl("http://localhost/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://LOCALHOST:9000/cb", err));

    // IPv6 loopback and link-local / unique-local (bracketed literals).
    CHECK(!pay::utils::validateNotifyUrl("http://[::1]/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://[fe80::1]/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://[fc00::1]/cb", err));
    // A public IPv6 literal must pass.
    CHECK(pay::utils::validateNotifyUrl("http://[2606:4700::1]/cb", err));

    // Plain public domain is allowed (DNS rebinding is an accepted limitation).
    CHECK(pay::utils::validateNotifyUrl("https://merchant.example.com/pay/notify", err));
}
