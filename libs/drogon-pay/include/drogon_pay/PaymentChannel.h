#pragma once

#include <drogon/HttpRequest.h>
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace drogon_pay
{

/**
 * @brief Normalized asynchronous notification produced by
 *        PaymentChannel::verifyCallback().
 *
 * Channel implementations verify the transport-level signature, decrypt the
 * payload when needed and map channel-specific fields onto this event so the
 * callback service can stay channel-agnostic.
 */
struct CallbackEvent
{
    std::string channel;       // channel name, e.g. "wechat" / "alipay"
    std::string orderNo;       // merchant order number (out_trade_no)
    std::string channelTxnId;  // channel-side transaction id
    std::string tradeStatus;   // raw channel trade state (e.g. SUCCESS, TRADE_SUCCESS)
    bool paid{false};          // normalized "payment succeeded" flag
    int64_t amountTotal{0};    // total amount in minor units (cents), 0 if absent
    std::string rawPayload;    // raw (decrypted) payload for auditing/ledger
    Json::Value payload;       // parsed payload (channel-specific fields)
};

/**
 * @brief Payment channel SPI.
 *
 * Contract for implementations:
 *  - All methods must be thread-safe; they are invoked concurrently from
 *    Drogon IO threads.
 *  - Outbound HTTP must reuse connections (e.g. one HttpClient per IO loop
 *    via drogon::IOThreadStorage) instead of creating a client per request.
 *  - Asymmetric, channel-only capabilities (e.g. wechat certificate refresh,
 *    alipay closeTrade) stay on the concrete class and are reached through an
 *    explicit std::dynamic_pointer_cast at the call site.
 */
class PaymentChannel
{
  public:
    using JsonCallback = std::function<void(const Json::Value &result, const std::string &error)>;

    virtual ~PaymentChannel() = default;

    /// Stable channel identifier used in requests/config ("wechat", "alipay", ...).
    virtual const std::string &name() const = 0;

    /// Whether the channel holds usable credentials.
    virtual bool isConfigured() const = 0;

    /// Create an app/wap payment (channel-specific payment parameters in result).
    virtual void createPayment(const Json::Value &payload, JsonCallback &&callback) = 0;

    /// Create a QR-code payment (result carries the code/QR content).
    virtual void createQRPayment(const Json::Value &payload, JsonCallback &&callback) = 0;

    /// Query payment state by merchant order number.
    virtual void queryPayment(const std::string &orderNo, JsonCallback &&callback) = 0;

    /**
     * @brief Close an unpaid trade on the channel side by merchant order
     *        number.
     *
     * The channel only honours a close while the trade has not been paid, so a
     * paid trade answers the refusal and stays untouched. Callers must treat
     * "closed" as the answer they proved and every refusal or transport error
     * as "the trade is still what the next query says it is".
     * Default: unsupported, so a channel without a close API keeps compiling
     * and the caller sees an explicit error instead of silence.
     */
    virtual void closeOrder(const std::string &orderNo, JsonCallback &&callback)
    {
        (void)orderNo;
        Json::Value result;
        callback(result, "channel does not support closing orders");
    }

    /// Create a refund.
    virtual void refund(const Json::Value &payload, JsonCallback &&callback) = 0;

    /// Query refund state by merchant refund number / order number.
    virtual void queryRefund(const std::string &refundNo, JsonCallback &&callback) = 0;

    /**
     * @brief Verify an incoming asynchronous notification and normalize it.
     * @return true when the signature checks out and @p event was populated;
     *         false with @p error set otherwise.
     */
    virtual bool verifyCallback(
      const drogon::HttpRequestPtr &req,
      CallbackEvent &event,
      std::string &error
    ) = 0;

    /// Called once after registration, on the plugin's worker loop (e.g. warm
    /// up certificates). Default: no-op.
    virtual void onStart()
    {
    }

    /// Called during plugin shutdown. Default: no-op.
    virtual void onStop()
    {
    }
};

using PaymentChannelPtr = std::shared_ptr<PaymentChannel>;

/// Factory signature hosts use to plug custom channels in.
using ChannelFactory = std::function<PaymentChannelPtr(const Json::Value &config)>;

}  // namespace drogon_pay
