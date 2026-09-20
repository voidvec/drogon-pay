#pragma once
#include <cstdint>
#include <json/json.h>
#include <string>

namespace pay::utils
{
bool getRequiredString(const Json::Value &json, const char *key, std::string &value);

bool parseAmountToFen(const std::string &amount, int64_t &fen);

// True when `amount` parses and equals `expectedFen`. A negative expectedFen
// (the caller's "could not resolve" sentinel) never matches, so callers get a
// fail-closed comparison from one call instead of repeating the parse + compare
// that a channel-notification consistency check needs.
bool amountEqualsFen(const std::string &amount, int64_t expectedFen);

std::string toJsonString(const Json::Value &value);

void mapTradeState(
  const std::string &tradeState,
  std::string &orderStatus,
  std::string &paymentStatus
);

std::string mapRefundStatus(const std::string &wechatStatus);

// Percent-encode a single URL path segment (RFC 3986 unreserved set passes
// through). WeChat V3 addresses resources by merchant number inside the path
// and signs exactly that string, so an identifier carrying a literal `?`, `#`,
// `&` or `/` would move the request to another resource under a valid
// signature.
std::string urlEncodePathSegment(const std::string &raw);

// Validate a callback notify URL. Returns true if the URL is empty (no notify
// URL supplied is allowed) or passes scheme + length + host checks. On failure,
// sets errorMessage. The host check rejects private/loopback/link-local
// addresses (RFC1918, 127/8, 169.254/16 incl. cloud metadata, IPv6 ::1, etc.)
// and "localhost" to prevent SSRF via an attacker-controlled notify_url (P1-3).
// Used by both PaymentService (create) and RefundService.
bool validateNotifyUrl(const std::string &url, std::string &errorMessage);
}  // namespace pay::utils
