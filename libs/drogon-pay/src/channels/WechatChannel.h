#pragma once

#include "drogon_pay/PaymentChannel.h"

#include <json/json.h>
#include <functional>
#include <string>
#include <map>
#include <shared_mutex>
#include <mutex>
#include <chrono>

class WechatPayClient : public drogon_pay::PaymentChannel
{
  public:
    using JsonCallback = std::function<void(const Json::Value &result, const std::string &error)>;

    explicit WechatPayClient(const Json::Value &config);

    // ---- PaymentChannel SPI ----
    const std::string &name() const override;
    bool isConfigured() const override;
    // Wechat "native" payments ARE QR payments; both SPI entry points map to
    // the same v3 native-transaction API.
    void createPayment(const Json::Value &payload, JsonCallback &&callback) override;
    void createQRPayment(const Json::Value &payload, JsonCallback &&callback) override;
    void queryPayment(const std::string &orderNo, JsonCallback &&callback) override;
    void refund(const Json::Value &payload, JsonCallback &&callback) override;
    void queryRefund(const std::string &refundNo, JsonCallback &&callback) override;
    bool verifyCallback(
      const drogon::HttpRequestPtr &req,
      drogon_pay::CallbackEvent &event,
      std::string &error
    ) override;
    /// Warm up the platform certificates (asymmetric capability folded into
    /// the channel life cycle instead of a plugin-owned timer bootstrap).
    void onStart() override;

    // ---- Wechat-specific capabilities (reach via dynamic_pointer_cast) ----
    void createTransactionNative(const Json::Value &payload, JsonCallback &&callback);
    void queryTransaction(const std::string &orderNo, JsonCallback &&callback);

    void downloadCertificates(JsonCallback &&callback);
    std::string getPlatformCert(const std::string &serialNo) const;
    /// Cache a platform certificate, but only after it proves itself: the
    /// content must parse as X.509, be within its validity window, carry
    /// exactly the serial number it is filed under, and (when
    /// `platform_ca_cert_path` is configured) chain to that trust anchor.
    /// Returns false and leaves the cache untouched otherwise.
    bool setPlatformCert(const std::string &serialNo, const std::string &certContent);

    std::string buildAuthorizationHeader(
      const std::string &method,
      const std::string &url,
      const std::string &body,
      const std::string &timestamp,
      const std::string &nonce,
      std::string &error
    ) const;

    /// Verify a notification against the platform certificate named by
    /// `serialNo`. Not const: a serial that is not in the cache triggers a
    /// throttled certificate refresh so WeChat's next retry can be verified
    /// (platform certificates rotate; the cache has to converge on the new one).
    bool verifyCallback(
      const std::string &timestamp,
      const std::string &nonce,
      const std::string &body,
      const std::string &signature,
      const std::string &serialNo,
      std::string &error
    );

    bool decryptResource(
      const std::string &ciphertext,
      const std::string &nonce,
      const std::string &associatedData,
      std::string &plaintext,
      std::string &error
    ) const;

    /// Hex serial number of an X.509 certificate in the shape WeChat reports
    /// them (uppercase, no leading zeros). Empty when the content does not
    /// parse. Used to bind a cached certificate to the serial it is filed under.
    static std::string certificateSerialHex(const std::string &certContent);

    const std::string &getAppId() const
    {
        return appId_;
    }

    const std::string &getMchId() const
    {
        return mchId_;
    }

  private:
    Json::Value config_;
    std::string appId_;
    std::string mchId_;
    std::string serialNo_;
    std::string apiV3Key_;
    std::string privateKeyPath_;
    std::string platformCertPath_;
    std::string platformCaCertPath_;
    std::string apiBase_;
    std::string notifyUrl_;
    int timeoutMs_{5000};

    std::map<std::string, std::string> platformCerts_;
    mutable std::shared_mutex certsMutex_;
    int certDownloadMinIntervalSeconds_{300};
    mutable std::mutex certDownloadMutex_;
    std::chrono::steady_clock::time_point lastCertDownloadAt_{};
};
