#include <drogon/drogon_test.h>
#include <cstdio>
#include <cstdint>
#include <trantor/utils/Date.h>
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

    // Overflow: stoll accepts these digit counts, but scaling yuan to fen does
    // not fit int64. Before the guard "184467440737095517.99" wrapped to fen
    // 183 -- a huge amount that parsed as 1.83 and booked that way.
    CHECK(!pay::utils::parseAmountToFen("184467440737095517.99", fen));
    CHECK(!pay::utils::parseAmountToFen("99999999999999999.99", fen));
    // One above the largest representable yuan (one cent over INT64_MAX fen).
    CHECK(!pay::utils::parseAmountToFen("92233720368547758.00", fen));
    // Exact ceiling: the greatest amount that still converts, not shadowed by
    // an over-broad cap that would also reject it.
    fen = 0;
    CHECK(pay::utils::parseAmountToFen("92233720368547757.99", fen));
    CHECK(fen == 9223372036854775799LL);
}

DROGON_TEST(PayUtils_RefundsCoverOrderAmount)
{
    // Exactly back, and more than back (a channel that rounds a fee into the
    // refund still returned the whole order).
    CHECK(pay::utils::refundsCoverOrderAmount(1000, 1000));
    CHECK(pay::utils::refundsCoverOrderAmount(1200, 1000));

    // One settled refund of a refundable order does not cover it: WeChat honours
    // up to fifty partial refunds per order, so the first of any size leaves the
    // order in the state it was.
    CHECK(!pay::utils::refundsCoverOrderAmount(999, 1000));
    CHECK(!pay::utils::refundsCoverOrderAmount(1, 1000));
    CHECK(!pay::utils::refundsCoverOrderAmount(0, 1000));

    // A total nobody measured proves nothing, so the answer is false for every
    // sum: a zero total is not an order fully returned, it is an amount that was
    // never read (or a SUM over rows that did not parse).
    CHECK(!pay::utils::refundsCoverOrderAmount(0, 0));
    CHECK(!pay::utils::refundsCoverOrderAmount(1000, 0));
    CHECK(!pay::utils::refundsCoverOrderAmount(1000, -1));
    CHECK(!pay::utils::refundsCoverOrderAmount(-1, 1000));
}

DROGON_TEST(PayUtils_ResolveRefundedOrderStatus)
{
    // The claim the ledger supports stands: refunds covering the total let the
    // mapped REFUNDED through untouched.
    CHECK(pay::utils::resolveRefundedOrderStatus("REFUNDED", 1000, 1000) == "REFUNDED");
    CHECK(pay::utils::resolveRefundedOrderStatus("REFUNDED", 1200, 1000) == "REFUNDED");

    // The claim the ledger does not support is downgraded to what a
    // `trade_state=REFUND` trade has nonetheless proven -- the money arrived --
    // which is PAID. Both the missing refund and the unreadable total land here.
    CHECK(pay::utils::resolveRefundedOrderStatus("REFUNDED", 999, 1000) == "PAID");
    CHECK(pay::utils::resolveRefundedOrderStatus("REFUNDED", 0, 1000) == "PAID");
    CHECK(pay::utils::resolveRefundedOrderStatus("REFUNDED", 0, 0) == "PAID");
    CHECK(pay::utils::resolveRefundedOrderStatus("REFUNDED", 1000, 0) == "PAID");

    // Every other mapped status is nobody's claim to check.
    CHECK(pay::utils::resolveRefundedOrderStatus("PAID", 0, 0) == "PAID");
    CHECK(pay::utils::resolveRefundedOrderStatus("PAYING", 5000, 1000) == "PAYING");
    CHECK(pay::utils::resolveRefundedOrderStatus("CLOSED", 0, 1000) == "CLOSED");
    CHECK(pay::utils::resolveRefundedOrderStatus("FAILED", 9999, 1000) == "FAILED");
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

    pay::utils::mapTradeState("REVOKED", orderStatus, paymentStatus);
    CHECK(orderStatus == "CLOSED");
    CHECK(paymentStatus == "FAIL");

    // The one that decides whether the money was ever collected. `REFUND` is a
    // trade that *did* settle and was then turned into a refund, while `CLOSED`
    // and `REVOKED` name trades that never took money at all -- so the payment
    // side must keep reporting the collection even as the order reports refund.
    pay::utils::mapTradeState("REFUND", orderStatus, paymentStatus);
    CHECK(orderStatus == "REFUNDED");
    CHECK(paymentStatus == "SUCCESS");

    pay::utils::mapTradeState("PAYERROR", orderStatus, paymentStatus);
    CHECK(orderStatus == "FAILED");
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

    // A host that is not the canonical four-dotted-octet form is still an IP
    // literal to every resolver (`inet_aton`), so it must not be classified as a
    // domain and waved through. Each of these denotes loopback or 10/8.
    CHECK(!pay::utils::validateNotifyUrl("http://127.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://2130706433/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://0x7f.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://017700000001/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://10.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://0/cb", err));
    // Leading zeros are already refused as ambiguous inside a dotted quad; the
    // whole host must not then fall back to "looks like a domain".
    CHECK(!pay::utils::validateNotifyUrl("http://010.1.1.1/cb", err));
    // A host of that shape that is a real domain (letters outside the hex set) is
    // still allowed -- the refusal is about numeric literals, not short hosts.
    CHECK(pay::utils::validateNotifyUrl("http://host42.example.com/cb", err));

    // The authority's userinfo ends at the LAST '@' before the path, which is
    // where a spec-following client reads the host from.
    CHECK(!pay::utils::validateNotifyUrl("http://user:pw@127.0.0.1/cb", err));
    CHECK(!pay::utils::validateNotifyUrl("http://a.example@b.example@127.0.0.1/cb", err));
    // A public host after the userinfo stays public.
    CHECK(pay::utils::validateNotifyUrl("http://127.0.0.1@pub.example/cb", err));
    // ... and an '@' that sits in the path is not userinfo at all: the host is
    // still the one that was named, not whatever follows the '@'.
    CHECK(pay::utils::validateNotifyUrl("http://pub.example/p@127.0.0.1", err));
    CHECK(!pay::utils::validateNotifyUrl("http://pub.example@169.254.169.254#f", err));
    // A fragment is not part of the host: the client connects to the literal in
    // front of the '#', so the check has to stop there too.
    CHECK(!pay::utils::validateNotifyUrl("http://127.0.0.1#x.example", err));
    CHECK(!pay::utils::validateNotifyUrl("http://169.254.169.254:80#meta", err));
    CHECK(pay::utils::validateNotifyUrl("https://pub.example.com/#cb", err));
}

DROGON_TEST(PayUtils_UrlEncodePathSegment)
{
    // The unreserved set passes through untouched: real order numbers are
    // alphanumeric, and re-encoding them would change the string WeChat signed.
    CHECK(pay::utils::urlEncodePathSegment("ORDER-2026_09.20~1") == "ORDER-2026_09.20~1");
    CHECK(
      pay::utils::urlEncodePathSegment("wx20260920abcdef0123456789") == "wx20260920abcdef0123456789"
    );
    CHECK(!pay::utils::urlEncodePathSegment("aZ09-_.").empty());

    // The characters that would re-shape the request target.
    CHECK(pay::utils::urlEncodePathSegment("o?1") == "o%3F1");
    CHECK(pay::utils::urlEncodePathSegment("o/../../admin") == "o%2F..%2F..%2Fadmin");
    CHECK(pay::utils::urlEncodePathSegment("o#frag") == "o%23frag");
    CHECK(pay::utils::urlEncodePathSegment("o&a=1") == "o%26a%3D1");
    CHECK(pay::utils::urlEncodePathSegment("o 1") == "o%201");
    CHECK(pay::utils::urlEncodePathSegment("o\n1") == "o%0A1");

    CHECK(pay::utils::urlEncodePathSegment("").empty());

    // Non-ASCII bytes are encoded per byte, uppercase hex (U+516D in UTF-8).
    CHECK(pay::utils::urlEncodePathSegment("\xE5\x85\xAD") == "%E5%85%AD");
}

DROGON_TEST(PayUtils_ValidateWechatOrderFields)
{
    std::string err;

    // Positive controls: the shapes real order numbers take must pass, or the
    // guard would reject payable orders.
    CHECK(pay::utils::validateWechatOrderFields("ORDER20260920A1", "商品", "", err));
    CHECK(pay::utils::validateWechatOrderFields("123456", "Goods|A*B-C_D", "attach", err));
    CHECK(
      pay::utils::validateWechatOrderFields(
        "abcdefghijklmnopqrstuvwxyz012345", "32-char id", "", err
      )
    );  // exactly 32

    // out_trade_no length: the official window is 6-32 characters.
    CHECK(!pay::utils::validateWechatOrderFields("", "d", "", err));
    CHECK(!pay::utils::validateWechatOrderFields("12345", "d", "", err));  // 5 is too short
    CHECK(
      !pay::utils::validateWechatOrderFields("abcdefghijklmnopqrstuvwxyz0123456", "d", "", err)
    );  // 33 is too long

    // out_trade_no character set: digits, letters and _ - | * only. The
    // look-alikes that previously reached the channel and faulted the booking.
    CHECK(!pay::utils::validateWechatOrderFields("order@1", "d", "", err));
    CHECK(!pay::utils::validateWechatOrderFields("order.name", "d", "", err));
    CHECK(!pay::utils::validateWechatOrderFields("order/name", "d", "", err));
    CHECK(!pay::utils::validateWechatOrderFields("order name", "d", "", err));
    CHECK(!pay::utils::validateWechatOrderFields("订单123456", "d", "", err));

    // description is required and capped at 127 code points (not bytes).
    CHECK(!pay::utils::validateWechatOrderFields("order123", "", "", err));
    const std::string desc128(128, 'x');
    CHECK(!pay::utils::validateWechatOrderFields("order123", desc128, "", err));
    // 127 CJK characters are 381 bytes but only 127 code points: legal.
    std::string cjk127;
    for (int i = 0; i < 127; ++i)
    {
        cjk127 += "六";
    }
    CHECK(pay::utils::validateWechatOrderFields("order123", cjk127, "", err));
    CHECK(!pay::utils::validateWechatOrderFields("order123", cjk127 + "六", "", err));

    // attach is optional but capped at 128 code points.
    CHECK(pay::utils::validateWechatOrderFields("order123", "d", std::string(128, 'a'), err));
    CHECK(!pay::utils::validateWechatOrderFields("order123", "d", std::string(129, 'a'), err));
}

namespace
{
// RFC 3339 in UTC for an instant relative to now, rendered by this test rather
// than by the library under test: a helper that shared trantor's date handling
// could not tell an error in the guard apart from an error in the helper.
std::string rfc3339UtcFromEpoch(int64_t epochSeconds)
{
    int64_t days = epochSeconds / 86400;
    int64_t secondOfDay = epochSeconds - days * 86400;
    if (secondOfDay < 0)
    {
        secondOfDay += 86400;
        --days;
    }
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const int64_t shiftedYear = static_cast<int64_t>(yearOfEra) + era * 400;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned movedMonth = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * movedMonth + 2) / 5 + 1;
    const unsigned month = movedMonth + (movedMonth < 10 ? 3 : -9);
    const unsigned year = static_cast<unsigned>(shiftedYear + (month <= 2));

    char buf[32];
    std::snprintf(
      buf,
      sizeof(buf),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      year,
      month,
      day,
      static_cast<unsigned>(secondOfDay / 3600),
      static_cast<unsigned>((secondOfDay % 3600) / 60),
      static_cast<unsigned>(secondOfDay % 60)
    );
    return buf;
}

std::string rfc3339UtcAfter(long long seconds)
{
    return rfc3339UtcFromEpoch(trantor::Date::now().secondsSinceEpoch() + seconds);
}
}  // namespace

DROGON_TEST(PayUtils_ValidateTimeExpire)
{
    std::string err;
    constexpr long long kDay = 86400;

    // Positive controls: the shapes a real caller sends have to pass, or the
    // guard would refuse payable orders.
    CHECK(pay::utils::validateTimeExpire(rfc3339UtcAfter(2 * kDay), "wechat", err));
    CHECK(pay::utils::validateTimeExpire(rfc3339UtcAfter(6 * kDay), "wechat", err));
    CHECK(pay::utils::validateTimeExpire(rfc3339UtcAfter(2 * kDay), "alipay", err));
    // An explicit numeric offset and a fractional part are both legal RFC 3339.
    CHECK(pay::utils::validateTimeExpire("2099-05-20T13:29:35+08:00", "alipay", err));
    CHECK(
      pay::utils::validateTimeExpire(
        rfc3339UtcAfter(2 * kDay).substr(0, 19) + ".123Z", "wechat", err
      )
    );

    // The mismatch this guard exists for: the space-separated shape the local
    // database parser used to accept is the one WeChat answers with a 400.
    CHECK(!pay::utils::validateTimeExpire("2099-05-20 13:29:35", "alipay", err));
    // An RFC 3339 instant without a zone has no single reading; the official
    // format always names one.
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29:35", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-5-20T13:29:35Z", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29:35+0800", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29:35ZZ", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29:35+25:00", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29:35.", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("tomorrow", "alipay", err));

    // Calendar validity, not just digit shape: the channel reads the date, so a
    // day that does not exist cannot be a deadline.
    CHECK(!pay::utils::validateTimeExpire("2099-13-20T13:29:35Z", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-02-30T13:29:35Z", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2100-02-29T13:29:35Z", "alipay", err));  // not leap
    CHECK(pay::utils::validateTimeExpire("2096-02-29T13:29:35Z", "alipay", err));   // leap
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T24:29:35Z", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:60:35Z", "alipay", err));
    CHECK(!pay::utils::validateTimeExpire("2099-05-20T13:29:60Z", "alipay", err));

    // An expiry before the order is created books a QR code that can never be
    // paid; the window is the instant's own, so both channels refuse it.
    CHECK(!pay::utils::validateTimeExpire(rfc3339UtcAfter(-3600), "alipay", err));
    CHECK(!pay::utils::validateTimeExpire(rfc3339UtcAfter(-3600), "wechat", err));

    // The 7-day window is WeChat's own (official Native precreate: past it the
    // channel moves the deadline silently), and it is not applied to a channel
    // that never sees the field.
    CHECK(!pay::utils::validateTimeExpire(rfc3339UtcAfter(8 * kDay), "wechat", err));
    CHECK(pay::utils::validateTimeExpire(rfc3339UtcAfter(8 * kDay), "alipay", err));

    // The instant a stored `expire_at` is built from, so the row and the value the
    // channel is shown cannot drift apart. Offsets go the way RFC 3339 means them
    // (`13:29:35+08:00` is `05:29:35Z`), and the leap day is a real moment.
    int64_t parsed = -1;
    std::string parseErr;
    CHECK(pay::utils::parseRfc3339("1970-01-01T00:00:00Z", parsed, parseErr));
    CHECK(parsed == 0);
    CHECK(pay::utils::parseRfc3339("2000-01-01T00:00:00Z", parsed, parseErr));
    CHECK(parsed == 946684800);
    CHECK(pay::utils::parseRfc3339("2000-01-01T00:00:00+08:00", parsed, parseErr));
    CHECK(parsed == 946684800 - 28800);
    CHECK(pay::utils::parseRfc3339("2000-01-01T00:00:00-08:00", parsed, parseErr));
    CHECK(parsed == 946684800 + 28800);
    CHECK(pay::utils::parseRfc3339("2024-02-29T23:59:59Z", parsed, parseErr));
    CHECK(parsed == 1709251199);
    CHECK(pay::utils::parseRfc3339("2024-02-29T23:59:59.999999Z", parsed, parseErr));
    CHECK(parsed == 1709251199);

    // What the old local parse did with a valid value: `fromDbStringLocal` splits
    // on a SPACE, so the day field became "20T13:29:35+08:00" and `std::stol`
    // stopped at the 'T' without throwing -- the whole time-of-day disappeared
    // and nothing reported it. Both sides share the runner's zone, so the equality
    // below is about that truncation rather than about any timezone.
    CHECK(
      trantor::Date::fromDbStringLocal("2099-05-20T13:29:35+08:00") ==
      trantor::Date::fromDbStringLocal("2099-05-20")
    );
}
