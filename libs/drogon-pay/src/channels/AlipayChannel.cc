#include "AlipayChannel.h"
#include "HttpClientPool.h"
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <trantor/utils/Logger.h>
#include <ctime>

AlipaySandboxClient::AlipaySandboxClient(const Json::Value &config) : config_(config)
{
    appId_ = config.get("app_id", "").asString();
    sellerId_ = config.get("seller_id", "").asString();
    privateKeyPath_ = config.get("private_key_path", "").asString();
    alipayPublicKeyPath_ = config.get("alipay_public_key_path", "").asString();
    gatewayUrl_ = config.get("gateway_url", "https://openapi.alipaydev.com/gateway.do").asString();
    notifyUrl_ = config.get("notify_url", "").asString();

    // Get timeout from config, default to 30 seconds
    timeoutMs_ = config.get("timeout_ms", 30000).asInt();

    // Load key files once at construction (B3). Avoids per-request disk I/O and
    // the TOCTOU window of re-reading on every sign/verify call. Missing files
    // are tolerated here (isConfigured() guards usage); the PEM strings stay
    // empty and sign/verify will fail safely.
    auto loadFile = [](const std::string &path, std::string &out) {
        if (path.empty())
        {
            return;
        }
        std::ifstream f(path);
        if (!f.is_open())
        {
            LOG_WARN << "AlipaySandboxClient: could not open key file (will fail at use): " << path;
            return;
        }
        std::stringstream buf;
        buf << f.rdbuf();
        out = buf.str();
    };
    loadFile(privateKeyPath_, privateKeyPem_);
    loadFile(alipayPublicKeyPath_, alipayPublicKeyPem_);

    LOG_INFO << "AlipaySandboxClient initialized with AppID: " << appId_
             << ", notify_url: " << notifyUrl_ << ", timeout: " << timeoutMs_ << "ms";
}

void AlipaySandboxClient::createTrade(const Json::Value &payload, JsonCallback &&callback)
{
    try
    {
        Json::Value bizContent = payload;
        if (!bizContent.isMember("out_trade_no"))
            bizContent["out_trade_no"] = generateUUID();
        if (!bizContent.isMember("total_amount"))
            bizContent["total_amount"] = "0.01";
        if (!bizContent.isMember("subject"))
            bizContent["subject"] = "测试订单";
        // buyer_id is optional - if not provided, Alipay will use the sandbox default
        // For sandbox testing, you can obtain test buyer accounts from Alipay sandbox console

        sendRequest("alipay.trade.create", bizContent, std::move(callback));
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "createTrade error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

void AlipaySandboxClient::precreateTrade(const Json::Value &payload, JsonCallback &&callback)
{
    try
    {
        Json::Value bizContent = payload;

        // Required parameters for alipay.trade.precreate:
        // out_trade_no: Merchant order number
        // total_amount: Payment amount
        // subject: Order title/product name

        if (!bizContent.isMember("out_trade_no"))
        {
            Json::Value error;
            error["error"] = "out_trade_no is required";
            callback(Json::Value(), error.asString());
            return;
        }
        if (!bizContent.isMember("total_amount"))
        {
            Json::Value error;
            error["error"] = "total_amount is required";
            callback(Json::Value(), error.asString());
            return;
        }
        if (!bizContent.isMember("subject"))
        {
            bizContent["subject"] = "Payment Order";
        }

        // Add notify_url if configured and not provided in payload
        if (!notifyUrl_.empty() && !bizContent.isMember("notify_url"))
        {
            bizContent["notify_url"] = notifyUrl_;
            LOG_DEBUG << "Added notify_url to precreate request: " << notifyUrl_;
        }

        // Optional: QR code expiration time (default 2 hours, max 24 hours)
        // bizContent["timeout_express"] = "90m";  // 90 minutes

        sendRequest("alipay.trade.precreate", bizContent, std::move(callback));
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "precreateTrade error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

void AlipaySandboxClient::queryTrade(const std::string &outTradeNo, JsonCallback &&callback)
{
    try
    {
        Json::Value bizContent;
        bizContent["out_trade_no"] = outTradeNo;
        sendRequest("alipay.trade.query", bizContent, std::move(callback));
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "queryTrade error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

void AlipaySandboxClient::refund(const Json::Value &payload, JsonCallback &&callback)
{
    try
    {
        Json::Value bizContent = payload;
        if (!bizContent.isMember("out_trade_no"))
        {
            Json::Value error;
            error["error"] = "out_trade_no is required";
            callback(Json::Value(), error.asString());
            return;
        }
        if (!bizContent.isMember("refund_amount"))
            bizContent["refund_amount"] = "0.01";
        sendRequest("alipay.trade.refund", bizContent, std::move(callback));
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "refund error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

void AlipaySandboxClient::queryRefund(const std::string &outTradeNo, JsonCallback &&callback)
{
    try
    {
        Json::Value bizContent;
        bizContent["out_trade_no"] = outTradeNo;
        bizContent["query_type"] = "refund";
        sendRequest("alipay.trade.fastpay.refund.query", bizContent, std::move(callback));
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "queryRefund error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

void AlipaySandboxClient::closeTrade(const std::string &outTradeNo, JsonCallback &&callback)
{
    try
    {
        Json::Value bizContent;
        bizContent["out_trade_no"] = outTradeNo;
        sendRequest("alipay.trade.close", bizContent, std::move(callback));
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "closeTrade error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

bool AlipaySandboxClient::verifyCallback(const Json::Value &params, const std::string &signature)
{
    try
    {
        std::string data;
        std::vector<std::string> keys;
        for (const auto &key : params.getMemberNames())
        {
            if (key != "sign" && key != "sign_type")
                keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto &key : keys)
        {
            if (!params[key].isNull())
            {
                if (!data.empty())
                    data += "&";
                data += key + "=" + params[key].asString();
            }
        }
        return verify(data, signature);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "verifyCallback error: " << e.what();
        return false;
    }
}

std::string AlipaySandboxClient::generateUUID() const
{
    return drogon::utils::getUuid();
}

void AlipaySandboxClient::sendRequest(
  const std::string &method,
  const Json::Value &bizContent,
  JsonCallback &&callback
)
{
    LOG_DEBUG << "sendRequest called for method: " << method;
    try
    {
        // Build common parameters
        Json::Value commonParams = buildCommonParams();
        commonParams["method"] = method;

        // Convert bizContent to compact JSON string (no spaces/newlines)
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string bizContentStr = Json::writeString(builder, bizContent);
        commonParams["biz_content"] = bizContentStr;

        commonParams["charset"] = "utf-8";
        commonParams["version"] = "1.0";
        commonParams["sign_type"] = "RSA2";

        // Build string to sign - only include required parameters per Alipay spec
        std::string signData;
        std::vector<std::string> keys;

        // Only include these parameters in signature (alphabetical order):
        // app_id, biz_content, charset, method, sign_type, timestamp
        // NOTE: charset is signed and transmitted via the URL query string
        // (official SDK behavior); without it the gateway falls back to GBK
        // responses and Chinese sub_msg text gets mangled when stored as UTF-8.
        keys.push_back("app_id");
        keys.push_back("biz_content");
        keys.push_back("charset");
        keys.push_back("method");
        keys.push_back("sign_type");
        keys.push_back("timestamp");

        for (const auto &key : keys)
        {
            if (!commonParams[key].isNull())
            {
                if (!signData.empty())
                {
                    signData += "&";
                }
                // Use raw values for signature (no URL encoding)
                signData += key + "=" + commonParams[key].asString();
            }
        }

        // Sign the data
        std::string signature = sign(signData);
        commonParams["sign"] = signature;

        LOG_DEBUG << "[AlipayClient] Request: method=" << method << ", app_id=" << appId_
                  << ", timestamp=" << commonParams["timestamp"].asString();
        LOG_TRACE << "[AlipayClient] Signing data length: " << signData.length() << " bytes";

        // Build request body (form format) - only include parameters that are in signature
        std::string requestBody;
        requestBody += "app_id=" + drogon::utils::urlEncode(commonParams["app_id"].asString());
        requestBody +=
          "&biz_content=" + drogon::utils::urlEncode(commonParams["biz_content"].asString());
        requestBody += "&method=" + drogon::utils::urlEncode(commonParams["method"].asString());
        requestBody +=
          "&sign_type=" + drogon::utils::urlEncode(commonParams["sign_type"].asString());
        requestBody +=
          "&timestamp=" + drogon::utils::urlEncode(commonParams["timestamp"].asString());
        requestBody += "&sign=" + drogon::utils::urlEncode(signature);

        LOG_DEBUG << "[AlipayClient] API request: method=" << method << ", url=" << gatewayUrl_;

        // Parse gateway URL to get base URL (without path)
        std::string baseUrl = gatewayUrl_;
        size_t pathPos = baseUrl.find("/gateway.do");
        if (pathPos != std::string::npos)
        {
            baseUrl = baseUrl.substr(0, pathPos);
        }

        // Base URL removed from log for security

        // Reuse one HttpClient per IO loop instead of a fresh client (and TLS
        // handshake) per request.
        auto client = drogon_pay::cachedHttpClient(baseUrl);

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        // charset must also appear in the URL query: the gateway decides the
        // response encoding from the query string (official SDK behavior), the
        // body param alone still yields GBK responses.
        req->setPath("/gateway.do?charset=utf-8");
        req->setContentTypeCode(drogon::CT_APPLICATION_X_FORM);
        req->setBody(requestBody);
        // Add charset to Content-Type header
        req->addHeader("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");

        client->sendRequest(
          req,
          [this,
           callback,
           method](drogon::ReqResult /*result*/, const drogon::HttpResponsePtr &response) {
              try
              {
                  if (!response)
                  {
                      LOG_ERROR << "Alipay HTTP error: No response";
                      Json::Value errorJson;
                      errorJson["error"] = "No response from Alipay server";
                      callback(Json::Value(), errorJson.toStyledString());
                      return;
                  }

                  // Check HTTP status code
                  auto statusCode = response->getStatusCode();
                  if (statusCode != drogon::k200OK)
                  {
                      LOG_ERROR << "Alipay HTTP error: status " << statusCode;
                      Json::Value errorJson;
                      errorJson["error"] = "HTTP status code: " + std::to_string(statusCode);
                      callback(Json::Value(), errorJson.toStyledString());
                      return;
                  }

                  auto body = response->body();
                  LOG_TRACE << "Alipay response: " << body;

                  // Parse JSON response
                  Json::Value root;

                  // Use CharReaderBuilder instead of deprecated Reader
                  Json::CharReaderBuilder builder;
                  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                  std::string errors;
                  std::string bodyStr = std::string(body);
                  const char *str = bodyStr.c_str();

                  if (!reader->parse(str, str + bodyStr.length(), &root, &errors))
                  {
                      LOG_ERROR << "Failed to parse Alipay response: " << body;
                      Json::Value error;
                      error["error"] = "Invalid response format";
                      error["raw_response"] = std::string(body);
                      callback(Json::Value(), error.toStyledString());
                      return;
                  }

                  // Check for error response
                  if (root.isMember("error_response"))
                  {
                      callback(Json::Value(), root["error_response"].toStyledString());
                      return;
                  }

                  // Extract the response
                  std::string responseKey = "alipay_" + method + "_response";
                  if (root.isMember(responseKey))
                  {
                      callback(root[responseKey], "");
                  }
                  else
                  {
                      // Fallback: try to find any key with "response"
                      for (const auto &key : root.getMemberNames())
                      {
                          if (key.find("response") != std::string::npos)
                          {
                              callback(root[key], "");
                              return;
                          }
                      }
                      callback(root, "");
                  }
              }
              catch (const std::exception &e)
              {
                  LOG_ERROR << "Alipay callback error: " << e.what();
                  Json::Value error;
                  error["error"] = e.what();
                  callback(Json::Value(), error.asString());
              }
          },
          timeoutMs_
        );  // Set timeout in milliseconds
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "AlipaySandboxClient::sendRequest error: " << e.what();
        Json::Value error;
        error["error"] = e.what();
        callback(Json::Value(), error.asString());
    }
}

std::string AlipaySandboxClient::sign(const std::string &data) const
{
    LOG_TRACE << "[AlipayClient] Generating signature for " << data.length() << " bytes of data";

    const std::string &privateKeyPem = privateKeyPem_;
    if (privateKeyPem.empty())
    {
        LOG_ERROR << "Private key not loaded (path: " << privateKeyPath_ << ")";
        return "";
    }

    if (privateKeyPem.empty())
    {
        LOG_ERROR << "Empty private key file";
        return "";
    }

    // Read private key using OpenSSL
    BIO *bio = BIO_new_mem_buf(privateKeyPem.c_str(), static_cast<int>(privateKeyPem.length()));
    if (!bio)
    {
        LOG_ERROR << "Failed to create BIO for private key";
        return "";
    }

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);

    if (!pkey)
    {
        LOG_ERROR << "Failed to read private key";
        return "";
    }

    // RSA sign using SHA256
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);

    EVP_DigestSignUpdate(mdctx, data.c_str(), data.length());

    size_t signatureLen = 0;
    EVP_DigestSignFinal(mdctx, nullptr, &signatureLen);

    std::vector<unsigned char> signature(signatureLen);
    EVP_DigestSignFinal(mdctx, signature.data(), &signatureLen);

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    // Convert to Base64 string (Alipay requires Base64, not hex)
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    BIO_write(b64, signature.data(), static_cast<int>(signatureLen));
    BIO_flush(b64);

    BUF_MEM *bufferPtr;
    BIO_get_mem_ptr(b64, &bufferPtr);

    std::string signatureStr(bufferPtr->data, bufferPtr->length);
    BIO_free_all(b64);

    return signatureStr;
}

bool AlipaySandboxClient::verify(const std::string &data, const std::string &signature) const
{
    const std::string &publicKeyPem = alipayPublicKeyPem_;
    if (publicKeyPem.empty())
    {
        LOG_ERROR << "Alipay public key not loaded (path: " << alipayPublicKeyPath_ << ")";
        return false;
    }

    // Read public key
    BIO *bio = BIO_new_mem_buf(publicKeyPem.c_str(), static_cast<int>(publicKeyPem.length()));
    if (!bio)
    {
        LOG_ERROR << "Failed to create BIO for public key";
        return false;
    }

    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);

    if (!pkey)
    {
        LOG_ERROR << "Failed to read public key";
        return false;
    }

    // Convert Base64 signature to binary
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *mem = BIO_new_mem_buf(signature.c_str(), static_cast<int>(signature.length()));
    b64 = BIO_push(b64, mem);

    std::vector<unsigned char> binarySig;
    unsigned char sigBuf[256];
    int bytesRead;
    while ((bytesRead = BIO_read(b64, sigBuf, sizeof(sigBuf))) > 0)
    {
        binarySig.insert(binarySig.end(), sigBuf, sigBuf + bytesRead);
    }
    BIO_free_all(b64);

    // RSA verify
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestVerifyUpdate(mdctx, data.c_str(), data.length());

    int result = EVP_DigestVerifyFinal(mdctx, binarySig.data(), binarySig.size());

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    return result == 1;
}

Json::Value AlipaySandboxClient::buildCommonParams() const
{
    Json::Value params;
    params["app_id"] = appId_;

    // Format timestamp as "yyyy-MM-dd HH:mm:ss" for Alipay API
    auto now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
    params["timestamp"] = std::string(timestamp);

    params["nonce"] = generateUUID();
    return params;
}

bool AlipaySandboxClient::isConfigured() const
{
    // Helper function to check if a value is a placeholder
    auto isPlaceholder = [](const std::string &value) -> bool {
        return value.empty() || value.find("__env_var:") == 0;
    };

    // Check all required configuration fields
    if (
      isPlaceholder(appId_) || isPlaceholder(sellerId_) || isPlaceholder(privateKeyPath_) ||
      isPlaceholder(alipayPublicKeyPath_) || isPlaceholder(gatewayUrl_)
    )
    {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// PaymentChannel SPI
// ---------------------------------------------------------------------------

const std::string &AlipaySandboxClient::name() const
{
    static const std::string kName = "alipay";
    return kName;
}

void AlipaySandboxClient::createPayment(const Json::Value &payload, JsonCallback &&callback)
{
    createTrade(payload, std::move(callback));
}

void AlipaySandboxClient::createQRPayment(const Json::Value &payload, JsonCallback &&callback)
{
    precreateTrade(payload, std::move(callback));
}

void AlipaySandboxClient::queryPayment(const std::string &orderNo, JsonCallback &&callback)
{
    queryTrade(orderNo, std::move(callback));
}

bool AlipaySandboxClient::verifyCallback(
  const drogon::HttpRequestPtr &req,
  drogon_pay::CallbackEvent &event,
  std::string &error
)
{
    // Alipay notifications arrive as x-www-form-urlencoded, not JSON.
    const std::string body = std::string(req->body());

    std::unordered_map<std::string, std::string> params;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&'))
    {
        size_t pos = pair.find('=');
        if (pos == std::string::npos)
        {
            continue;
        }
        std::string key = pair.substr(0, pos);
        std::string value = pair.substr(pos + 1);
        // URL decode: '+' means space in x-www-form-urlencoded, then decode
        // percent-encoded sequences.
        std::replace(value.begin(), value.end(), '+', ' ');
        params[key] = drogon::utils::urlDecode(value);
    }

    const auto signIt = params.find("sign");
    const std::string signature = (signIt != params.end()) ? signIt->second : std::string{};
    if (signature.empty())
    {
        error = "missing sign parameter";
        return false;
    }

    // Build the parameter set for signature verification; verifyCallback()
    // excludes 'sign'/'sign_type' itself.
    Json::Value verifyParams;
    for (const auto &kv : params)
    {
        verifyParams[kv.first] = kv.second;
    }
    if (!verifyCallback(verifyParams, signature))
    {
        error = "signature verification failed";
        return false;
    }

    const std::string tradeStatus = params["trade_status"];

    event.channel = name();
    event.orderNo = params["out_trade_no"];
    event.channelTxnId = params["trade_no"];
    event.tradeStatus = tradeStatus;
    event.paid = (tradeStatus == "TRADE_SUCCESS" || tradeStatus == "TRADE_FINISHED");
    // total_amount is in yuan with up to 2 decimals; normalize to cents.
    if (!params["total_amount"].empty())
    {
        try
        {
            event.amountTotal =
              static_cast<int64_t>(std::llround(std::stod(params["total_amount"]) * 100.0));
        }
        catch (const std::exception &)
        {
            event.amountTotal = 0;
        }
    }
    event.rawPayload = body;
    event.payload = verifyParams;
    return true;
}
