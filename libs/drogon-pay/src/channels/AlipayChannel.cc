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

        // notify_url is a top-level public parameter, added by
        // buildSignedRequestParams() from notifyUrl_. It must NOT live inside
        // biz_content: Alipay ignores it there and sends no async notification.

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
        // Notification verification excludes sign AND sign_type (Alipay's
        // rsaCheckV1 rule); empty-valued parameters never participate.
        const std::string data = joinSortedParams(params, /*excludeSignType=*/true);
        return verify(data, signature);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "verifyCallback error: " << e.what();
        return false;
    }
}

std::string AlipaySandboxClient::joinSortedParams(const Json::Value &params, bool excludeSignType)
{
    std::vector<std::string> keys;
    for (const auto &key : params.getMemberNames())
    {
        if (key == "sign" || (excludeSignType && key == "sign_type"))
        {
            continue;
        }
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    std::string data;
    for (const auto &key : keys)
    {
        const Json::Value &value = params[key];
        if (value.isNull())
        {
            continue;
        }
        const std::string sval = value.asString();
        // Alipay's signing rule excludes parameters whose value is empty.
        if (sval.empty())
        {
            continue;
        }
        if (!data.empty())
        {
            data += "&";
        }
        data += key + "=" + sval;
    }
    return data;
}

std::string AlipaySandboxClient::encodeFormParams(const Json::Value &params)
{
    std::vector<std::string> keys = params.getMemberNames();
    std::sort(keys.begin(), keys.end());
    std::string body;
    for (const auto &key : keys)
    {
        if (!body.empty())
        {
            body += "&";
        }
        body +=
          drogon::utils::urlEncode(key) + "=" + drogon::utils::urlEncode(params[key].asString());
    }
    return body;
}

std::string AlipaySandboxClient::responseKeyForMethod(const std::string &method)
{
    std::string key = method;
    std::replace(key.begin(), key.end(), '.', '_');
    return key + "_response";
}

std::string AlipaySandboxClient::extractResponseNode(
  const std::string &rawBody,
  const std::string &responseKey
)
{
    // Anchor the key search to the ROOT object. A naive find() matches the first
    // raw occurrence of "<key>" anywhere -- including inside a sibling object --
    // so a nested-decoy splice could verify a genuine nested node while jsoncpp
    // delivers a forged top-level one (rejectDupKeys only rejects SAME-level
    // duplicates). Track brace depth and string state, and accept the token only
    // when it is a direct root member (depth == 1) used as a key (followed by
    // ':'), never a nested key or a string value. Verified bytes then equal the
    // bytes of the member jsoncpp delivers.
    const size_t n = rawBody.size();
    auto skipSpace = [&rawBody, n](size_t p) {
        while (p < n && (rawBody[p] == ' ' || rawBody[p] == '\t' || rawBody[p] == '\n' ||
                         rawBody[p] == '\r'))
        {
            ++p;
        }
        return p;
    };

    int depth = 0;
    size_t i = 0;
    while (i < n)
    {
        const char c = rawBody[i];
        if (c == '"')
        {
            // Read the string token (escape-aware) to its closing quote. Only
            // depth-1 strings can be root keys, so only they accumulate content.
            const bool maybeRootKey = (depth == 1);
            size_t s = i + 1;
            std::string content;
            bool closed = false;
            while (s < n)
            {
                const char d = rawBody[s];
                if (d == '\\')
                {
                    if (s + 1 >= n)
                    {
                        break;
                    }
                    if (maybeRootKey)
                    {
                        content += rawBody[s + 1];
                    }
                    s += 2;
                    continue;
                }
                if (d == '"')
                {
                    closed = true;
                    break;
                }
                if (maybeRootKey)
                {
                    content += d;
                }
                ++s;
            }
            if (!closed)
            {
                return "";
            }
            if (maybeRootKey)
            {
                const size_t after = skipSpace(s + 1);
                if (after < n && rawBody[after] == ':' && content == responseKey)
                {
                    const size_t start = skipSpace(after + 1);
                    if (start >= n || rawBody[start] != '{')
                    {
                        return "";
                    }
                    // Extract the balanced object value (string/escape aware);
                    // array brackets inside it do not affect brace depth.
                    int nodeDepth = 0;
                    bool inString = false;
                    bool escaped = false;
                    for (size_t p = start; p < n; ++p)
                    {
                        const char ch = rawBody[p];
                        if (inString)
                        {
                            if (escaped)
                            {
                                escaped = false;
                            }
                            else if (ch == '\\')
                            {
                                escaped = true;
                            }
                            else if (ch == '"')
                            {
                                inString = false;
                            }
                        }
                        else if (ch == '"')
                        {
                            inString = true;
                        }
                        else if (ch == '{')
                        {
                            ++nodeDepth;
                        }
                        else if (ch == '}')
                        {
                            --nodeDepth;
                            if (nodeDepth == 0)
                            {
                                return rawBody.substr(start, p - start + 1);
                            }
                        }
                    }
                    return "";
                }
            }
            i = s + 1;
            continue;
        }
        if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            --depth;
        }
        ++i;
    }
    return "";
}

bool AlipaySandboxClient::verifyResponseSignature(
  const std::string &rawBody,
  const std::string &responseKey,
  const std::string &alipayPublicKeyPem
)
{
    if (alipayPublicKeyPem.empty())
    {
        LOG_ERROR << "Alipay response verify: public key not loaded";
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    // Reject duplicate keys: extractResponseNode signs the FIRST raw occurrence
    // of the node, but jsoncpp's operator[] keeps the LAST duplicate. Accepting
    // duplicates would let a splice verify a genuine node yet deliver a forged
    // one. Alipay never emits duplicate keys, so this rejects only attacks.
    builder["rejectDupKeys"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    if (!reader->parse(rawBody.data(), rawBody.data() + rawBody.size(), &root, &errors))
    {
        LOG_ERROR << "Alipay response verify: invalid JSON";
        return false;
    }
    // sign must be a string. An object/array/number here is malformed and must
    // fail closed (false), not throw Json::LogicError out of asString() -- this
    // is a public static API whose contract is "false when invalid".
    const Json::Value signVal = root.get("sign", Json::Value());
    if (!signVal.isString())
    {
        LOG_ERROR << "Alipay response verify: missing or non-string sign (fail-closed)";
        return false;
    }
    const std::string signature = signVal.asString();
    if (signature.empty())
    {
        LOG_ERROR << "Alipay response verify: empty sign (fail-closed)";
        return false;
    }
    const std::string node = extractResponseNode(rawBody, responseKey);
    if (node.empty())
    {
        LOG_ERROR << "Alipay response verify: response node not found: " << responseKey;
        return false;
    }
    return verifyWithPem(node, signature, alipayPublicKeyPem);
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
        // Common params + biz_content + top-level notify_url + RSA2 signature,
        // all assembled in one place so create/precreate/query/refund share the
        // exact same signing contract.
        const Json::Value params = buildSignedRequestParams(method, bizContent);
        const std::string requestBody = encodeFormParams(params);

        LOG_DEBUG << "[AlipayClient] Request: method=" << method << ", app_id=" << appId_
                  << ", timestamp=" << params["timestamp"].asString();

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

        // Copy the public key into the callback so verification does not depend
        // on `this` outliving the async round-trip.
        const std::string pubPem = alipayPublicKeyPem_;
        const std::string responseKey = responseKeyForMethod(method);

        client->sendRequest(
          req,
          [callback,
           responseKey,
           pubPem](drogon::ReqResult /*result*/, const drogon::HttpResponsePtr &response) {
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
                  // Must match verifyResponseSignature: a duplicate response key
                  // would make the parsed node differ from the verified raw node.
                  builder["rejectDupKeys"] = true;
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

                  // Synchronous-response signature check (fail-closed): Alipay
                  // signs the exact raw JSON of the response node. A missing or
                  // invalid sign means the body cannot be trusted (MITM /
                  // spoofed gateway), so the response is rejected outright.
                  if (!verifyResponseSignature(bodyStr, responseKey, pubPem))
                  {
                      LOG_ERROR << "Alipay response signature verification failed: " << responseKey;
                      Json::Value error;
                      error["error"] = "response signature verification failed";
                      callback(Json::Value(), error.toStyledString());
                      return;
                  }

                  // The verified node must exist under its exact method key.
                  // No "any key containing response" fallback: that could hand
                  // back an attacker-chosen sibling node.
                  if (!root.isMember(responseKey))
                  {
                      LOG_ERROR << "Alipay response node missing: " << responseKey;
                      Json::Value error;
                      error["error"] = "response node missing: " + responseKey;
                      callback(Json::Value(), error.toStyledString());
                      return;
                  }
                  callback(root[responseKey], "");
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

    // RSA sign using SHA256. Each EVP step is checked: an unchecked failure
    // would otherwise emit a zero-length/garbage signature that the gateway
    // rejects opaquely.
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx)
    {
        EVP_PKEY_free(pkey);
        LOG_ERROR << "Failed to allocate EVP_MD_CTX for signing";
        return "";
    }

    size_t signatureLen = 0;
    const bool signOk = EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
                        EVP_DigestSignUpdate(mdctx, data.c_str(), data.length()) == 1 &&
                        EVP_DigestSignFinal(mdctx, nullptr, &signatureLen) == 1 && signatureLen > 0;

    std::vector<unsigned char> signature(signOk ? signatureLen : 0);
    const bool finalOk = signOk && EVP_DigestSignFinal(mdctx, signature.data(), &signatureLen) == 1;

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    if (!finalOk)
    {
        LOG_ERROR << "Alipay signing failed (EVP)";
        return "";
    }

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
    if (alipayPublicKeyPem_.empty())
    {
        LOG_ERROR << "Alipay public key not loaded (path: " << alipayPublicKeyPath_ << ")";
        return false;
    }
    return verifyWithPem(data, signature, alipayPublicKeyPem_);
}

bool AlipaySandboxClient::verifyWithPem(
  const std::string &data,
  const std::string &signature,
  const std::string &publicKeyPem
)
{
    if (publicKeyPem.empty() || signature.empty())
    {
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

    // RSA verify. Every EVP step is checked so a truncated/garbage signature or
    // an unusable key can never fall through to a "verify passed" verdict.
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx)
    {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool ok = false;
    if (
      EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
      EVP_DigestVerifyUpdate(mdctx, data.c_str(), data.length()) == 1
    )
    {
        ok = (EVP_DigestVerifyFinal(mdctx, binarySig.data(), binarySig.size()) == 1);
    }

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    return ok;
}

Json::Value AlipaySandboxClient::buildCommonParams() const
{
    Json::Value params;
    params["app_id"] = appId_;

    // Alipay requires the timestamp in GMT+8 ("yyyy-MM-dd HH:mm:ss"), regardless
    // of the host's local timezone. Shift the epoch by +8h and render it as UTC
    // so a server in any zone produces the same wall-clock the gateway expects.
    // gmtime_s/gmtime_r are the reentrant variants (Drogon calls this from
    // multiple IO-loop threads; the shared static tm of gmtime is unsafe).
    const std::time_t nowGmt8 = std::time(nullptr) + 8 * 3600;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &nowGmt8);
#else
    gmtime_r(&nowGmt8, &tm);
#endif
    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
    params["timestamp"] = std::string(timestamp);

    params["version"] = "1.0";
    params["charset"] = "utf-8";
    params["sign_type"] = "RSA2";
    return params;
}

Json::Value AlipaySandboxClient::buildSignedRequestParams(
  const std::string &method,
  const Json::Value &bizContent
) const
{
    Json::Value params = buildCommonParams();
    params["method"] = method;

    // biz_content is a compact JSON string (no spaces/newlines); Alipay signs
    // and receives it verbatim.
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    params["biz_content"] = Json::writeString(builder, bizContent);

    // notify_url is a TOP-LEVEL public parameter. When it lives inside
    // biz_content Alipay ignores it and never sends the async notification.
    if (!notifyUrl_.empty())
    {
        params["notify_url"] = notifyUrl_;
    }

    // Request signing excludes only `sign` (sign_type DOES participate here,
    // unlike notification verification). Empty/null values are dropped by
    // joinSortedParams per Alipay's rule.
    const std::string signData = joinSortedParams(params, /*excludeSignType=*/false);
    params["sign"] = sign(signData);
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
