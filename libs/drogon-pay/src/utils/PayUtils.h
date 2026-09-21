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

// Whether the refunds that have *settled* on an order return everything the
// order collected. One settled refund is evidence about itself only: WeChat
// answers `trade_state=REFUND` for a partly refunded trade as well as for a
// fully refunded one, and accepts up to fifty partial refunds per order, so an
// order may read `REFUNDED` once, and only once, the settled refunds cover its
// total. An amount nobody measured (zero, or a sum that did not parse) proves
// nothing: the answer is false, and the order keeps the status it already has.
bool refundsCoverOrderAmount(int64_t settledRefundFen, int64_t orderTotalFen);

// Downgrade a channel-derived order status that the ledger has not earned.
// WeChat answers `trade_state=REFUND` for a trade that *entered* refunding --
// a single partial refund is enough -- so a mapped `REFUNDED` is only a claim,
// and `refundsCoverOrderAmount` is the evidence that decides it: an uncovered
// `REFUNDED` becomes `PAID`, which is what a REFUND trade has nonetheless
// proven (the money arrived, and some of it is on its way back). Every other
// status passes through untouched.
std::string resolveRefundedOrderStatus(
  const std::string &mappedOrderStatus,
  int64_t settledRefundFen,
  int64_t orderTotalFen
);

std::string toJsonString(const Json::Value &value);

// Map a WeChat V3 `trade_state` onto the local order and payment statuses. The
// two answers are independent: a trade can have collected money and no longer be
// paid for (`REFUND`), so the payment side reports whether money arrived while
// the order side reports where the trade stands now.
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

// Validate the fields a WeChat V3 order request must satisfy (official Native
// precreate parameter table): out_trade_no is 6-32 characters from
// [0-9a-zA-Z_|*-], description is non-empty and at most 127 characters, attach
// is at most 128 characters (lengths count UTF-8 code points). WeChat rejects
// every other shape with a 400 of its own, but by then the order row is
// already booked locally and would sit in the reconciliation sweep forever
// against a trade that can never exist. Returns true only when all supplied
// fields are valid; errorMessage names the first violation.
bool validateWechatOrderFields(
  const std::string &outTradeNo,
  const std::string &description,
  const std::string &attach,
  std::string &errorMessage
);

// Parse a strict RFC 3339 timestamp (`yyyy-MM-DDTHH:mm:ss` plus `Z` or
// `±HH:MM`, optional fractional seconds) into seconds since the Unix epoch, UTC.
// Trantor's own readers are not usable here: `fromDbStringLocal` splits on a
// SPACE and lets `std::stol` stop at the 'T', so a correct value reads back as
// local midnight with the whole time-of-day dropped without an error, and
// `fromISOString` applies the machine's zone on top of the string's own offset.
// Returns false and sets errorMessage when the value is not a real moment.
bool parseRfc3339(const std::string &value, int64_t &secondsSinceEpoch, std::string &errorMessage);

// Validate an order's payment end time before it is booked or sent to a channel.
// `time_expire` is RFC 3339 (`yyyy-MM-DDTHH:mm:ss` plus `Z` or `±HH:MM`, official
// Native precreate page) and the string is forwarded to the channel verbatim, so
// a space-separated or offset-less shape is refused there with a 400 of its own
// after the order row already exists locally. The instant also has to lie in the
// future -- an expiry before 下单时间 books an order that can never be paid --
// and, for WeChat, no more than 7 days ahead: past that the channel silently
// moves the deadline, leaving our `expire_at` disagreeing with the trade.
// Returns true only when the value is usable; errorMessage names the violation.
bool validateTimeExpire(
  const std::string &timeExpire,
  const std::string &channel,
  std::string &errorMessage
);
}  // namespace pay::utils
