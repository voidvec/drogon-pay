#include "PayUtils.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <trantor/utils/Date.h>

namespace
{
// ---- notify_url host extraction & private-address blocking (P1-3 SSRF) ----
//
// notify_url is forwarded verbatim to Alipay/WeChat as the URL they will HTTP
// POST their async callback to. An attacker-controlled value must not point at
// a private/loopback/link-local address (e.g. 127.0.0.1, 169.254.169.254 cloud
// metadata, RFC1918 ranges), since the provider would then probe or callback
// into internal services.
//
// The check has to agree with the reader about where the host is. A host is
// therefore taken from the authority only (userinfo = text up to the LAST '@'
// before the path, and the authority stops at '/', '?', ':' or '#'), and an
// IPv4 literal is refused in every form a resolver accepts rather than only
// the dotted quad. IPv6 is refused by whitelist (only 2000::/3 global unicast
// passes) because its textual forms are too many to enumerate; IPv4 is refused
// by the private/reserved range blocklist below plus the numeric-shape rule.
//
// This is a literal-only check: it does NOT resolve DNS (the channel, not this
// service, performs the request), so a domain that resolves to a private IP is
// not caught here -- that is an accepted limitation.

// Extract the host portion of an http(s) URL. Handles bracketed IPv6 literals
// (http://[::1]/...). Returns empty if the authority cannot be parsed.
std::string extractUrlHost(const std::string &url)
{
    // url is already known to start with http:// or https:// here.
    size_t hostStart = url.find("://");
    if (hostStart == std::string::npos)
    {
        return {};
    }
    hostStart += 3;
    if (hostStart >= url.size())
    {
        return {};
    }
    // Strip userinfo (user:pass@host) if present. The authority ends at the
    // first '/', '?' or '#', so an '@' beyond that is part of the path and not
    // userinfo; and where the authority holds several '@', a spec-following
    // client reads the host after the LAST one -- taking the first would check a
    // different host than the one the provider will connect to.
    size_t authorityEnd = url.find_first_of("/?#", hostStart);
    if (authorityEnd == std::string::npos)
    {
        authorityEnd = url.size();
    }
    if (authorityEnd > hostStart)
    {
        const size_t at = url.rfind('@', authorityEnd - 1);
        if (at != std::string::npos && at >= hostStart)
        {
            hostStart = at + 1;
        }
    }
    // IPv6 literal in brackets: [::1] — take everything up to the closing ']'.
    if (hostStart < url.size() && url[hostStart] == '[')
    {
        size_t close = url.find(']', hostStart + 1);
        if (close == std::string::npos)
        {
            return {};
        }
        return url.substr(hostStart + 1, close - hostStart - 1);
    }
    // Otherwise the host runs until the first '/', ':', '?' or '#'. The fragment
    // marker has to terminate the host: without it, "http://127.0.0.1#x.com"
    // reads as the host "127.0.0.1#x.com", which is neither a parseable IPv4
    // literal nor a numeric address shape, so it passed the check while the
    // provider connected to 127.0.0.1.
    size_t end = url.find_first_of("/:?#", hostStart);
    if (end == std::string::npos)
    {
        return url.substr(hostStart);
    }
    return url.substr(hostStart, end - hostStart);
}

bool parseIpv4(const std::string &host, uint8_t out[4])
{
    size_t start = 0;
    for (int i = 0; i < 4; ++i)
    {
        size_t dot = (i < 3) ? host.find('.', start) : std::string::npos;
        std::string seg =
          (dot == std::string::npos) ? host.substr(start) : host.substr(start, dot - start);
        if (seg.empty() || seg.size() > 3)
        {
            return false;
        }
        for (char c : seg)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
        }
        int v = 0;
        for (char c : seg)
        {
            v = v * 10 + (c - '0');
            if (v > 255)
            {
                return false;
            }
        }
        // Reject leading zeros like "01" (ambiguous octal), allow "0".
        if (seg.size() > 1 && seg[0] == '0')
        {
            return false;
        }
        out[i] = static_cast<uint8_t>(v);
        if (i < 3)
        {
            if (dot == std::string::npos)
            {
                return false;  // not enough segments
            }
            start = dot + 1;
        }
        else if (dot != std::string::npos)
        {
            return false;  // too many segments
        }
    }
    return true;
}

// True if the IPv4 literal is private/loopback/link-local/reserved. Blocks the
// ranges cited in the audit: 10/8, 172.16/12, 192.168/16, 127/8, 169.254/16,
// plus 0.0.0.0/8 and 100.64/10 (CGNAT).
bool isPrivateIpv4(uint8_t a, uint8_t b, uint8_t /*c*/, uint8_t /*d*/)
{
    if (a == 10)
        return true;  // 10.0.0.0/8
    if (a == 172 && (b & 0xF0) == 16)
        return true;  // 172.16.0.0/12
    if (a == 192 && b == 168)
        return true;  // 192.168.0.0/16
    if (a == 127)
        return true;  // 127.0.0.0/8 (loopback)
    if (a == 169 && b == 254)
        return true;  // 169.254.0.0/16 (link-local + metadata)
    if (a == 0)
        return true;  // 0.0.0.0/8 ("this network")
    if (a == 100 && (b & 0xC0) == 64)
        return true;  // 100.64.0.0/10 (CGNAT)
    return false;
}

// True if an IPv6 literal denotes a global unicast address (2000::/3), which is
// what a provider-facing callback has to be. The previous blocklist matched the
// textual forms "::1", "fc..", "fd.." and "fe8.."/"fe9.."/"fea.."/"feb.." and
// was therefore satisfied by every other spelling of the same addresses -- the
// expanded loopback, an IPv4-mapped "::ffff:127.0.0.1", or a 6to4-style
// rendering of a link-local. Naming the allowed class instead closes all of
// those at once and subsumes the old prefix test.
bool isGlobalUnicastIpv6(const std::string &host)
{
    size_t i = 0;
    while (i < host.size() && host[i] == ':')
    {
        // A leading ':' is the compressed zero group, so the first 16 bits are
        // zero and the address is not in 2000::/3.
        return false;
    }
    int value = 0;
    int digits = 0;
    while (i < host.size() && digits < 4)
    {
        const char c = host[i];
        int nibble = -1;
        if (c >= '0' && c <= '9')
        {
            nibble = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            nibble = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            nibble = c - 'A' + 10;
        }
        else
        {
            break;
        }
        value = value * 16 + nibble;
        ++i;
        ++digits;
    }
    return (value & 0xE000) == 0x2000;
}

// True if the host is an address literal in some spelling other than the
// canonical dotted quad: "127.1", "2130706433", "0x7f.1", "010.1.1.1". Every
// resolver feeds those to `inet_aton`, which expands them to a full address, so
// they are IP literals and not domains -- reading them as the latter is what
// let the loopback forms through. A label of that shape is all decimal digits,
// or a "0x" hex group; a real hostname such as "host42.example.com" or
// "deadbeef.example" has a label that is neither.
bool isNumericAddressShape(const std::string &host)
{
    size_t i = 0;
    while (true)
    {
        const size_t dot = host.find('.', i);
        const size_t end = (dot == std::string::npos) ? host.size() : dot;
        const size_t labelEnd = end;
        if (labelEnd == i)
        {
            return false;  // an empty label is a malformed name, not an address
        }
        size_t k = i;
        bool hexPrefixed = false;
        if (labelEnd - k > 2 && host[k] == '0' && (host[k + 1] == 'x' || host[k + 1] == 'X'))
        {
            hexPrefixed = true;
            k += 2;
        }
        if (k == labelEnd)
        {
            return false;  // "0x" with no digits
        }
        for (; k < labelEnd; ++k)
        {
            const char c = host[k];
            const bool isHexDigit =
              (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (hexPrefixed ? !isHexDigit : (c < '0' || c > '9'))
            {
                return false;  // an ordinary domain label
            }
        }
        if (dot == std::string::npos)
        {
            return true;  // every label was a numeric address group
        }
        i = dot + 1;
    }
}

bool isBlockedHost(const std::string &host)
{
    if (host.empty())
    {
        return true;  // no host is invalid
    }
    // Domain literal block.
    std::string lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "localhost" || lower == "localhost.")
    {
        return true;
    }
    // IPv6 literal (contains ':').
    if (host.find(':') != std::string::npos)
    {
        return !isGlobalUnicastIpv6(host);
    }
    // IPv4 literal.
    uint8_t ip[4];
    if (parseIpv4(host, ip))
    {
        return isPrivateIpv4(ip[0], ip[1], ip[2], ip[3]);
    }
    // Not the canonical dotted quad: refuse the other address spellings rather
    // than letting them through as "some domain".
    if (isNumericAddressShape(lower))
    {
        return true;
    }
    // Plain domain (not an IP literal): not blocked here. DNS rebinding is an
    // accepted limitation (see file comment).
    return false;
}
}  // namespace

namespace pay::utils
{
bool getRequiredString(const Json::Value &json, const char *key, std::string &value)
{
    if (!json.isMember(key))
    {
        return false;
    }
    if (json[key].isString())
    {
        value = json[key].asString();
        return !value.empty();
    }
    if (json[key].isNumeric())
    {
        value = json[key].asString();
        return !value.empty();
    }
    return false;
}

bool parseAmountToFen(const std::string &amount, int64_t &fen)
{
    if (amount.empty())
    {
        return false;
    }

    std::string yuanPart;
    std::string centPart;
    const auto dotPos = amount.find('.');
    if (dotPos == std::string::npos)
    {
        yuanPart = amount;
        centPart = "00";
    }
    else
    {
        yuanPart = amount.substr(0, dotPos);
        centPart = amount.substr(dotPos + 1);
    }

    if (yuanPart.empty())
    {
        yuanPart = "0";
    }

    for (char c : yuanPart)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    for (char c : centPart)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    if (centPart.size() > 2)
    {
        return false;
    }
    if (centPart.size() == 1)
    {
        centPart.push_back('0');
    }
    if (centPart.empty())
    {
        centPart = "00";
    }

    try
    {
        const int64_t yuan = std::stoll(yuanPart);
        const int64_t cents = std::stoll(centPart);
        // stoll only rejects literals above its own range; a 17-19 digit yuan
        // parses fine and yuan*100 then wraps (signed overflow is UB, in
        // practice mod 2^64), so "184467440737095517.99" could quietly become
        // fen 183. Reject anything that cannot scale to fen without overflow.
        if (yuan > (std::numeric_limits<int64_t>::max() - 99) / 100)
        {
            return false;
        }
        fen = yuan * 100 + cents;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool amountEqualsFen(const std::string &amount, int64_t expectedFen)
{
    int64_t fen = 0;
    // A negative expectation (an amount the caller could not resolve) never
    // matches: parsed fen is always >= 0, so the guard fails closed.
    return parseAmountToFen(amount, fen) && fen == expectedFen;
}

bool refundsCoverOrderAmount(int64_t settledRefundFen, int64_t orderTotalFen)
{
    return orderTotalFen > 0 && settledRefundFen >= orderTotalFen;
}

std::string resolveRefundedOrderStatus(
  const std::string &mappedOrderStatus,
  int64_t settledRefundFen,
  int64_t orderTotalFen
)
{
    if (
      mappedOrderStatus == "REFUNDED" && !refundsCoverOrderAmount(settledRefundFen, orderTotalFen)
    )
    {
        return "PAID";
    }
    return mappedOrderStatus;
}

std::string toJsonString(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

void mapTradeState(
  const std::string &tradeState,
  std::string &orderStatus,
  std::string &paymentStatus
)
{
    orderStatus = "FAILED";
    paymentStatus = "FAIL";
    if (tradeState == "SUCCESS")
    {
        orderStatus = "PAID";
        paymentStatus = "SUCCESS";
    }
    else if (tradeState == "USERPAYING" || tradeState == "NOTPAY")
    {
        orderStatus = "PAYING";
        paymentStatus = "PROCESSING";
    }
    // `REFUND` is the state a trade reaches *after* the money arrived: it says
    // the order was turned into a refund, so answering "the payment failed" here
    // recorded a collected payment as one that never happened -- the payment row
    // fell to FAIL, no PAYMENT ledger entry was written, and the order read
    // CLOSED as if it had expired unpaid. The refund itself is still evidenced
    // by `pay_refund`, which this endpoint says nothing about, so the payment
    // settles to SUCCESS and only the order carries the refunded state.
    else if (tradeState == "REFUND")
    {
        orderStatus = "REFUNDED";
        paymentStatus = "SUCCESS";
    }
    else if (tradeState == "CLOSED" || tradeState == "REVOKED")
    {
        orderStatus = "CLOSED";
        paymentStatus = "FAIL";
    }
}

std::string mapRefundStatus(const std::string &wechatStatus)
{
    if (wechatStatus == "SUCCESS")
    {
        return "REFUND_SUCCESS";
    }
    if (wechatStatus == "CLOSED")
    {
        return "REFUND_FAIL";
    }
    if (wechatStatus == "ABNORMAL")
    {
        return "REFUND_FAIL";
    }
    if (wechatStatus == "PROCESSING")
    {
        return "REFUNDING";
    }
    return "";
}

std::string urlEncodePathSegment(const std::string &raw)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved)
        {
            out.push_back(static_cast<char>(c));
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[(c >> 4) & 0xF]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

bool validateNotifyUrl(const std::string &url, std::string &errorMessage)
{
    errorMessage.clear();
    if (url.empty())
    {
        // No notify URL is allowed; the channel falls back to its configured default.
        return true;
    }
    if (url.find("http://") != 0 && url.find("https://") != 0)
    {
        errorMessage = "invalid notify_url (must start with http:// or https://)";
        return false;
    }
    if (url.length() > 512)
    {
        errorMessage = "notify_url too long (max 512 characters)";
        return false;
    }
    // SSRF defense (P1-3): reject URLs whose host is a private/loopback/
    // link-local address or "localhost". The notify_url is forwarded to the
    // payment channel as the URL it will call back, so an internal host would
    // let an attacker probe internal services via the provider. See file-level
    // comment for the DNS-rebinding limitation.
    const std::string host = extractUrlHost(url);
    if (isBlockedHost(host))
    {
        errorMessage = "notify_url host must not be a private, loopback, or link-local address";
        return false;
    }
    return true;
}

namespace
{
// UTF-8 code points = bytes that are not continuation bytes (10xxxxxx).
size_t utf8CodePointLength(const std::string &s)
{
    size_t n = 0;
    for (const unsigned char c : s)
    {
        if ((c & 0xC0) != 0x80)
        {
            ++n;
        }
    }
    return n;
}
}  // namespace

bool validateWechatOrderFields(
  const std::string &outTradeNo,
  const std::string &description,
  const std::string &attach,
  std::string &errorMessage
)
{
    // Official Native precreate: "商户系统内部订单号，要求6-32个字符内，
    // 只能是数字、大小写字母_-|* 且在同一个商户号下唯一".
    if (outTradeNo.size() < 6 || outTradeNo.size() > 32)
    {
        errorMessage = "order_no must be 6-32 characters for WeChat Pay";
        return false;
    }
    for (const char c : outTradeNo)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '|' || c == '*';
        if (!ok)
        {
            errorMessage = "order_no may only contain digits, letters, and _ - | * for WeChat Pay";
            return false;
        }
    }

    // description is required by the channel, at most 127 characters.
    if (description.empty())
    {
        errorMessage = "description is required for WeChat Pay";
        return false;
    }
    if (utf8CodePointLength(description) > 127)
    {
        errorMessage = "description exceeds 127 characters";
        return false;
    }

    // attach is optional, at most 128 characters when present.
    if (!attach.empty() && utf8CodePointLength(attach) > 128)
    {
        errorMessage = "attach exceeds 128 characters";
        return false;
    }

    return true;
}

namespace
{
bool isAsciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool readFixedDigits(const std::string &s, size_t at, size_t count, int &value)
{
    if (at + count > s.size())
    {
        return false;
    }
    int accumulated = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (!isAsciiDigit(s[at + i]))
        {
            return false;
        }
        accumulated = accumulated * 10 + (s[at + i] - '0');
    }
    value = accumulated;
    return true;
}

bool isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month)
{
    static constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && isLeapYear(year))
    {
        return 29;
    }
    return kDaysInMonth[month - 1];
}

// Days between 1970-01-01 and the given proleptic-Gregorian date (Howard
// Hinnant's `days_from_civil`), so a deadline can be compared against the clock
// without pulling a calendar library into the utility layer.
int64_t daysFromCivil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned shiftedMonth = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    const unsigned dayOfYear = (153u * shiftedMonth + 2u) / 5u + static_cast<unsigned>(day) - 1u;
    const unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}
}  // namespace

bool parseRfc3339(const std::string &value, int64_t &secondsSinceEpoch, std::string &errorMessage)
{
    static const char *kFormatError =
      "expected RFC 3339 (yyyy-MM-DDTHH:mm:ss with a Z or ±HH:MM timezone)";

    const size_t n = value.size();
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (
      n < 20 || !readFixedDigits(value, 0, 4, year) || value[4] != '-' ||
      !readFixedDigits(value, 5, 2, month) || value[7] != '-' ||
      !readFixedDigits(value, 8, 2, day) || value[10] != 'T' ||
      !readFixedDigits(value, 11, 2, hour) || value[13] != ':' ||
      !readFixedDigits(value, 14, 2, minute) || value[16] != ':' ||
      !readFixedDigits(value, 17, 2, second)
    )
    {
        errorMessage = kFormatError;
        return false;
    }
    if (
      month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month) || hour > 23 ||
      minute > 59 || second > 59
    )
    {
        errorMessage = "the timestamp names a moment that does not exist";
        return false;
    }

    size_t at = 19;
    if (at < n && value[at] == '.')
    {
        // Fractional seconds are legal RFC 3339 and carry nothing for a deadline;
        // the digits are consumed and dropped.
        const size_t firstFractionDigit = at + 1;
        at = firstFractionDigit;
        while (at < n && isAsciiDigit(value[at]))
        {
            ++at;
        }
        if (at == firstFractionDigit)
        {
            errorMessage = kFormatError;
            return false;
        }
    }

    int offsetSeconds = 0;
    if (at == n)
    {
        errorMessage = "the timestamp must carry a timezone (Z or ±HH:MM)";
        return false;
    }
    if (value[at] == 'Z')
    {
        ++at;
    }
    else if (value[at] == '+' || value[at] == '-')
    {
        const int sign = value[at] == '+' ? 1 : -1;
        ++at;
        int offsetHour = 0;
        int offsetMinute = 0;
        if (
          !readFixedDigits(value, at, 2, offsetHour) || at + 2 >= n || value[at + 2] != ':' ||
          !readFixedDigits(value, at + 3, 2, offsetMinute)
        )
        {
            errorMessage = kFormatError;
            return false;
        }
        at += 5;
        if (offsetHour > 23 || offsetMinute > 59)
        {
            errorMessage = "the timezone offset is out of range";
            return false;
        }
        offsetSeconds = sign * (offsetHour * 3600 + offsetMinute * 60);
    }
    else
    {
        errorMessage = kFormatError;
        return false;
    }
    if (at != n)
    {
        // Trailing text means the caller would be shown a different instant than
        // the one the channel is given.
        errorMessage = kFormatError;
        return false;
    }

    secondsSinceEpoch =
      daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offsetSeconds;
    return true;
}

bool validateTimeExpire(
  const std::string &timeExpire,
  const std::string &channel,
  std::string &errorMessage
)
{
    // The value is forwarded to the channel unchanged, so only this strict
    // reading is safe: trantor's own readers accept a space separator, and
    // `fromISOString` adds the machine's zone on top of the string's offset.
    int64_t expirySeconds = 0;
    std::string parseError;
    if (!parseRfc3339(timeExpire, expirySeconds, parseError))
    {
        errorMessage = "time_expire " + parseError;
        return false;
    }

    const int64_t nowSeconds = trantor::Date::now().secondsSinceEpoch();
    if (expirySeconds <= nowSeconds)
    {
        errorMessage = "time_expire is already in the past";
        return false;
    }
    // Official Native precreate: 支付结束时间需在下单时间的 7 天以内，超过则由系统
    // 自动调整 -- an adjustment the caller cannot observe, so it is refused here
    // instead of accepted and quietly moved.
    if (channel == "wechat" && expirySeconds > nowSeconds + 7 * 86400)
    {
        errorMessage = "time_expire must be within 7 days for WeChat Pay";
        return false;
    }
    return true;
}
}  // namespace pay::utils
