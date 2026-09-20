#include "PayUtils.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>

namespace
{
// ---- notify_url host extraction & private-address blocking (P1-3 SSRF) ----
//
// notify_url is forwarded verbatim to Alipay/WeChat as the URL they will HTTP
// POST their async callback to. An attacker-controlled value must not point at
// a private/loopback/link-local address (e.g. 127.0.0.1, 169.254.169.254 cloud
// metadata, RFC1918 ranges), since the provider would then probe or callback
// into internal services. This is a best-effort, literal-only check: it does
// NOT resolve DNS (the channel, not this service, performs the request), so a
// domain that resolves to a private IP is not caught here — that is an accepted
// limitation. The audit's core requirement (block private/loopback IP literals)
// is satisfied.

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
    // Strip userinfo (user:pass@host) if present.
    size_t at = url.find('@', hostStart);
    if (at != std::string::npos)
    {
        hostStart = at + 1;
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
    // Otherwise the host runs until the first '/', ':', or '?'.
    size_t end = url.find_first_of("/:?", hostStart);
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

// True if an IPv6 literal is loopback (::1), link-local (fe80::/10), or
// unique-local (fc00::/7). Only the minimal canonical forms are checked; the
// provider-facing risk is dominated by ::1 and link-local.
bool isPrivateIpv6(const std::string &host)
{
    std::string h = host;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    if (h == "::1")
        return true;  // loopback
    if (h.rfind("fc", 0) == 0)
        return true;  // fc00::/7 unique-local
    if (h.rfind("fd", 0) == 0)
        return true;
    if (
      h.rfind("fe8", 0) == 0 || h.rfind("fe9", 0) == 0 || h.rfind("fea", 0) == 0 ||
      h.rfind("feb", 0) == 0
    )
    {
        return true;  // fe80::/10 link-local
    }
    return false;
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
        return isPrivateIpv6(host);
    }
    // IPv4 literal.
    uint8_t ip[4];
    if (parseIpv4(host, ip))
    {
        return isPrivateIpv4(ip[0], ip[1], ip[2], ip[3]);
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
        // stoll accepts anything that fits int64_t, but the caller's scale is
        // fen: yuan * 100 overflows (UB) past ~9.2e16 yuan, and a wrapped value
        // could collide with a small legitimate amount.
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
    else if (tradeState == "CLOSED" || tradeState == "REVOKED" || tradeState == "REFUND")
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
}  // namespace pay::utils
