#include "WechatChannel.h"
#include <drogon/HttpClient.h>
#include <drogon/utils/Utilities.h>
#include <trantor/utils/Logger.h>
#include "HttpClientPool.h"
#include "../utils/PayUtils.h"
#include <cctype>
#include <fstream>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

namespace
{
/// WeChat reports certificate serials as uppercase hex without leading zeros;
/// OpenSSL's i2s_ASN1_INTEGER pads them. Compare on the normalised form.
std::string normalizeSerialHex(const std::string &raw)
{
    std::string upper;
    upper.reserve(raw.size());
    for (char c : raw)
    {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    const auto first = upper.find_first_not_of('0');
    if (first == std::string::npos)
    {
        return "0";
    }
    return upper.substr(first);
}

X509 *parseCert(const std::string &certContent)
{
    if (certContent.empty())
    {
        return nullptr;
    }
    BIO *bio = BIO_new_mem_buf(certContent.data(), static_cast<int>(certContent.size()));
    if (!bio)
    {
        return nullptr;
    }
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

/// Reject certificates that are not currently valid. Without this an expired
/// platform certificate keeps verifying notifications forever.
bool isWithinValidity(const X509 *cert, std::string &error)
{
    // X509_cmp_current_time returns <0 when `when` is in the past, >0 when the
    // future. notBefore must be past, notAfter must be ahead.
    if (X509_cmp_current_time(X509_get0_notBefore(cert)) >= 0)
    {
        error = "certificate not yet valid";
        return false;
    }
    if (X509_cmp_current_time(X509_get0_notAfter(cert)) <= 0)
    {
        error = "certificate has expired";
        return false;
    }
    return true;
}

}  // namespace

/// Hex serial number of an X.509 certificate, in the shape WeChat reports it.
std::string WechatPayClient::certificateSerialHex(const std::string &certContent)
{
    X509 *cert = parseCert(certContent);
    if (!cert)
    {
        return {};
    }
    const ASN1_INTEGER *serial = X509_get_serialNumber(cert);
    char *raw = serial ? i2s_ASN1_INTEGER(nullptr, serial) : nullptr;
    X509_free(cert);
    if (!raw)
    {
        return {};
    }
    std::string hex(raw);
    OPENSSL_free(raw);
    return normalizeSerialHex(hex);
}

namespace
{
std::string readFile(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "failed to open file: " + path;
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

bool signMessage(
  const std::string &message,
  const std::string &privateKeyPath,
  std::string &signatureB64,
  std::string &error
)
{
    std::string keyError;
    std::string keyContent = readFile(privateKeyPath, keyError);
    if (!keyError.empty())
    {
        error = keyError;
        return false;
    }

    BIO *bio = BIO_new_mem_buf(keyContent.data(), static_cast<int>(keyContent.size()));
    if (!bio)
    {
        error = "failed to create BIO for private key";
        return false;
    }

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey)
    {
        error = "failed to load private key";
        return false;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        EVP_PKEY_free(pkey);
        error = "failed to create EVP_MD_CTX";
        return false;
    }

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1)
    {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        error = "EVP_DigestSignInit failed";
        return false;
    }

    if (EVP_DigestSignUpdate(ctx, message.data(), message.size()) != 1)
    {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        error = "EVP_DigestSignUpdate failed";
        return false;
    }

    size_t sigLen = 0;
    if (EVP_DigestSignFinal(ctx, nullptr, &sigLen) != 1)
    {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        error = "EVP_DigestSignFinal size failed";
        return false;
    }

    std::string signature(sigLen, '\0');
    if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char *>(&signature[0]), &sigLen) != 1)
    {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        error = "EVP_DigestSignFinal failed";
        return false;
    }

    signature.resize(sigLen);
    signatureB64 = drogon::utils::base64Encode(signature);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return true;
}

bool verifyMessageWithCert(
  const std::string &message,
  const std::string &signatureB64,
  const std::string &certContent,
  std::string &error
)
{
    if (certContent.empty())
    {
        error = "empty certificate content";
        return false;
    }

    X509 *cert = parseCert(certContent);
    if (!cert)
    {
        error = "failed to load platform cert from content";
        return false;
    }

    std::string validityError;
    if (!isWithinValidity(cert, validityError))
    {
        X509_free(cert);
        error = "platform cert " + validityError;
        return false;
    }

    EVP_PKEY *pkey = X509_get_pubkey(cert);
    X509_free(cert);
    if (!pkey)
    {
        error = "failed to extract public key";
        return false;
    }

    auto signature = drogon::utils::base64Decode(signatureB64);
    if (signature.empty())
    {
        EVP_PKEY_free(pkey);
        error = "failed to decode signature";
        return false;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        EVP_PKEY_free(pkey);
        error = "failed to create EVP_MD_CTX";
        return false;
    }

    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1)
    {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        error = "EVP_DigestVerifyInit failed";
        return false;
    }

    if (EVP_DigestVerifyUpdate(ctx, message.data(), message.size()) != 1)
    {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        error = "EVP_DigestVerifyUpdate failed";
        return false;
    }

    int ok = EVP_DigestVerifyFinal(
      ctx, reinterpret_cast<const unsigned char *>(signature.data()), signature.size()
    );
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    if (ok != 1)
    {
        error = "signature verify failed";
        return false;
    }
    return true;
}

bool decryptAesGcm(
  const std::string &ciphertextB64,
  const std::string &nonce,
  const std::string &aad,
  const std::string &apiV3Key,
  std::string &plaintext,
  std::string &error
)
{
    if (apiV3Key.size() != 32)
    {
        error = "api_v3_key must be 32 bytes";
        return false;
    }

    auto ciphertext = drogon::utils::base64Decode(ciphertextB64);
    if (ciphertext.size() < 16)
    {
        error = "ciphertext too short";
        return false;
    }

    // WeChat fixes the AEAD IV at 12 bytes for notification resources and for
    // the downloaded certificates. Accepting an arbitrary attacker-supplied
    // length would let a malformed resource steer the cipher parameters.
    if (nonce.size() != 12)
    {
        error = "nonce must be 12 bytes";
        return false;
    }

    const size_t tagLen = 16;
    const size_t textLen = ciphertext.size() - tagLen;
    const unsigned char *tag = reinterpret_cast<const unsigned char *>(ciphertext.data() + textLen);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
    {
        error = "failed to create cipher ctx";
        return false;
    }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        error = "EVP_DecryptInit_ex failed";
        return false;
    }

    if (
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        error = "set iv len failed";
        return false;
    }

    if (
      EVP_DecryptInit_ex(
        ctx,
        nullptr,
        nullptr,
        reinterpret_cast<const unsigned char *>(apiV3Key.data()),
        reinterpret_cast<const unsigned char *>(nonce.data())
      ) != 1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        error = "set key/iv failed";
        return false;
    }

    int outLen = 0;
    if (!aad.empty())
    {
        if (
          EVP_DecryptUpdate(
            ctx,
            nullptr,
            &outLen,
            reinterpret_cast<const unsigned char *>(aad.data()),
            static_cast<int>(aad.size())
          ) != 1
        )
        {
            EVP_CIPHER_CTX_free(ctx);
            error = "set aad failed";
            return false;
        }
    }

    int totalLen = 0;
    if (textLen > 0)
    {
        plaintext.resize(textLen);
        if (
          EVP_DecryptUpdate(
            ctx,
            reinterpret_cast<unsigned char *>(&plaintext[0]),
            &outLen,
            reinterpret_cast<const unsigned char *>(ciphertext.data()),
            static_cast<int>(textLen)
          ) != 1
        )
        {
            EVP_CIPHER_CTX_free(ctx);
            error = "decrypt update failed";
            return false;
        }
        totalLen = outLen;
    }
    else
    {
        plaintext.clear();
    }

    if (
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tagLen, const_cast<unsigned char *>(tag)) != 1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        error = "set tag failed";
        return false;
    }

    // GCM never emits bytes in the final call, but the tag check happens here:
    // a mismatch is what rejects a tampered or wrongly-keyed resource.
    unsigned char finalBuf[EVP_MAX_BLOCK_LENGTH] = {};
    int finalLen = 0;
    int finalOk = EVP_DecryptFinal_ex(ctx, finalBuf, &finalLen);
    EVP_CIPHER_CTX_free(ctx);
    if (finalOk != 1)
    {
        plaintext.clear();
        error = "decrypt final failed";
        return false;
    }
    if (finalLen > 0)
    {
        plaintext.append(reinterpret_cast<const char *>(finalBuf), static_cast<size_t>(finalLen));
    }
    return true;
}

std::string toJsonString(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

/// Turn a non-2xx WeChat response into the channel error string. V3 reports
/// failures as `{"code":"PARAM_ERROR","message":"..."}` under a 4xx/5xx status,
/// so the status alone is enough to fail but the body says why. Bounded because
/// this text ends up in response payloads and client-facing messages.
std::string httpFailureText(int status, const std::string &detail)
{
    constexpr size_t kMaxDetail = 200;
    std::string trimmed =
      detail.size() > kMaxDetail ? detail.substr(0, kMaxDetail) + "..." : detail;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
    {
        trimmed.pop_back();
    }
    return "HTTP " + std::to_string(status) + ": " + trimmed;
}

void sendWechatRequest(
  const std::string &apiBase,
  const std::string &method,
  const std::string &path,
  const std::string &body,
  const std::string &authHeader,
  int timeoutMs,
  std::function<bool(const drogon::HttpResponsePtr &, std::string &)> verifyAnswer,
  WechatPayClient::JsonCallback &&callback
)
{
    // Reuse one HttpClient per IO loop instead of building (and TLS
    // handshaking) a fresh client for every request.
    auto client = drogon_pay::cachedHttpClient(apiBase);
    auto req = drogon::HttpRequest::newHttpRequest();
    if (method == "GET")
    {
        req->setMethod(drogon::Get);
    }
    else if (method == "POST")
    {
        req->setMethod(drogon::Post);
        req->setBody(body);
    }
    else
    {
        Json::Value result;
        callback(result, "unsupported method");
        return;
    }

    req->setPath(path);
    req->addHeader("Accept", "application/json");
    req->addHeader("Content-Type", "application/json");
    req->addHeader("User-Agent", "PayPlugin/1.0");
    req->addHeader("Authorization", authHeader);

    auto cb = std::make_shared<WechatPayClient::JsonCallback>(std::move(callback));

    // Drogon takes seconds and disables the timer at zero, which is what an
    // unset/zero `timeout_ms` means here.
    const double timeoutSeconds = timeoutMs > 0 ? timeoutMs / 1000.0 : 0;
    client->sendRequest(
      req,
      [cb,
       verifyAnswer = std::move(verifyAnswer),
       timeoutMs](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
          Json::Value bodyJson;
          if (result != drogon::ReqResult::Ok || !resp)
          {
              (*cb)(
                bodyJson,
                result == drogon::ReqResult::Timeout
                  ? "http request timed out after " + std::to_string(timeoutMs) + "ms"
                  : "http request failed"
              );
              return;
          }

          const int status = static_cast<int>(resp->statusCode());

          // `204 No Content` is a documented success answer (the close-order
          // API answers with it and no body at all), so an empty 2xx body is
          // success with an empty object rather than "invalid json response".
          if (status == 204)
          {
              Json::Value bodyJson(Json::objectValue);
              (*cb)(bodyJson, "");
              return;
          }

          // The documented answer signature covers the response body, so every
          // 2xx answer that carries one must verify before the service layer is
          // allowed to read state out of it. A 204 has no body to sign and is
          // handled above; non-2xx answers only ever drive the failure path,
          // which an attacker who can forge them does not need this channel
          // for. An answer that fails to verify is dropped without a body --
          // handing the forged JSON to the caller would re-open the hole even
          // through the error text. `verifyAnswer` is empty (boolean-false) for
          // the certificate-download bootstrap, which must NOT dereference an
          // empty std::function.
          if (verifyAnswer && status >= 200 && status < 300)
          {
              std::string verifyError;
              if (!verifyAnswer(resp, verifyError))
              {
                  (*cb)(bodyJson, "response signature verification failed: " + verifyError);
                  return;
              }
          }

          bool parsed = false;
          auto json = resp->getJsonObject();
          if (json)
          {
              bodyJson = *json;
              parsed = true;
          }
          else
          {
              Json::CharReaderBuilder builder;
              std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
              std::string errors;
              const auto &raw = resp->body();
              parsed = reader->parse(raw.data(), raw.data() + raw.size(), &bodyJson, &errors);
          }

          // A non-2xx status is a failure even when the body parses cleanly:
          // V3 answers `{"code":"ORDER_NOT_EXIST"}` under a 404, and treating
          // that as success leaves orders and refunds in a phantom state.
          if (status < 200 || status >= 300)
          {
              std::string detail;
              if (parsed)
              {
                  detail = bodyJson.get("code", "").asString() + " " +
                           bodyJson.get("message", "").asString();
              }
              if (detail.empty() || detail == " ")
              {
                  // The body stays out of the error string on purpose: this text
                  // is reflected into responses to our own API callers, and
                  // `api_base` is configuration -- whatever answers there must
                  // not be echoed through us.
                  LOG_TRACE << "[WechatChannel] non-2xx body: " << resp->body();
                  detail = "no error envelope";
              }
              (*cb)(bodyJson, httpFailureText(status, detail));
              return;
          }

          if (!parsed)
          {
              (*cb)(bodyJson, "invalid json response");
              return;
          }
          (*cb)(bodyJson, "");
      },
      timeoutSeconds
    );
}
}  // namespace

WechatPayClient::WechatPayClient(const Json::Value &config) : config_(config)
{
    appId_ = config.get("app_id", "").asString();
    mchId_ = config.get("mch_id", "").asString();
    serialNo_ = config.get("serial_no", "").asString();
    apiV3Key_ = config.get("api_v3_key", "").asString();
    privateKeyPath_ = config.get("private_key_path", "").asString();
    platformCertPath_ = config.get("platform_cert_path", "").asString();
    platformCaCertPath_ = config.get("platform_ca_cert_path", "").asString();
    apiBase_ = config.get("api_base", "https://api.mch.weixin.qq.com").asString();
    notifyUrl_ = config.get("notify_url", "").asString();
    certDownloadMinIntervalSeconds_ = config.get("cert_download_min_interval_seconds", 300).asInt();
    // One second floor: the throttle is what keeps an unauthenticated inbound
    // header from turning into an outbound signed request per notification.
    if (certDownloadMinIntervalSeconds_ < 1)
    {
        certDownloadMinIntervalSeconds_ = 1;
    }
    timeoutMs_ = config.get("timeout_ms", 5000).asInt();
    if (timeoutMs_ < 0)
    {
        timeoutMs_ = 0;
    }
}

void WechatPayClient::createTransactionNative(const Json::Value &payload, JsonCallback &&callback)
{
    Json::Value request = payload;
    if (request.get("appid", "").asString().empty())
    {
        request["appid"] = appId_;
    }
    if (request.get("mchid", "").asString().empty())
    {
        request["mchid"] = mchId_;
    }
    if (request.get("notify_url", "").asString().empty())
    {
        request["notify_url"] = notifyUrl_;
    }

    if (
      request.get("appid", "").asString().empty() || request.get("mchid", "").asString().empty() ||
      request.get("notify_url", "").asString().empty()
    )
    {
        Json::Value result;
        callback(result, "missing appid/mchid/notify_url");
        return;
    }

    const std::string path = "/v3/pay/transactions/native";
    const std::string body = toJsonString(request);

    const std::string timestamp = std::to_string(std::time(nullptr));
    const std::string nonce = drogon::utils::getUuid();
    std::string error;
    std::string auth = buildAuthorizationHeader("POST", path, body, timestamp, nonce, error);
    if (!error.empty())
    {
        Json::Value result;
        callback(result, error);
        return;
    }

    sendWechatRequest(
      apiBase_, "POST", path, body, auth, timeoutMs_, answerVerifier(), std::move(callback)
    );
}

void WechatPayClient::downloadCertificates(JsonCallback &&callback)
{
    // Build the signed request first: the auth header depends only on local
    // key material, so a signing/config failure never reaches the network and
    // must not spend the shared download window. Stamping before this point let
    // a misconfigured key consume the interval on a request that never existed,
    // starving the next genuine rotation refresh.
    const std::string path = "/v3/certificates";
    const std::string body;
    const std::string timestamp = std::to_string(std::time(nullptr));
    const std::string nonce = drogon::utils::getUuid();
    std::string error;
    std::string auth = buildAuthorizationHeader("GET", path, body, timestamp, nonce, error);
    if (!error.empty())
    {
        Json::Value result;
        if (callback)
            callback(result, error);
        return;
    }

    if (certDownloadMinIntervalSeconds_ > 0)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(certDownloadMutex_);
        if (lastCertDownloadAt_.time_since_epoch().count() != 0)
        {
            const auto elapsed =
              std::chrono::duration_cast<std::chrono::seconds>(now - lastCertDownloadAt_).count();
            if (elapsed < certDownloadMinIntervalSeconds_)
            {
                Json::Value result;
                if (callback)
                {
                    callback(result, "certificate download throttled");
                }
                return;
            }
        }
        // The window is spent only now that the request is genuinely about to be
        // issued, so the check-and-stamp stays atomic against concurrent
        // notifications while a no-op never consumes it.
        lastCertDownloadAt_ = now;
    }

    auto cb = std::make_shared<JsonCallback>(std::move(callback));
    // Hold the client weakly across the async boundary. Every production entry
    // point owns this object through a shared_ptr (the registry and the services
    // both), so a client torn down during shutdown while a refresh is in flight
    // would otherwise leave the response handler dereferencing a dangling `this`
    // to reach `decryptResource`/`setPlatformCert`. Locking here turns that race
    // into a dropped answer.
    const std::weak_ptr<WechatPayClient> selfWeak = weak_from_this();
    sendWechatRequest(
      apiBase_,
      "GET",
      path,
      body,
      auth,
      timeoutMs_,
      // No answer verifier here on purpose: this is the bootstrap that *arms*
      // answer verification. Verifying it would need a platform certificate,
      // which is exactly what this response delivers -- the check would be
      // circular. The answer authenticates itself instead: the certificate
      // bodies are AES-GCM encrypted under the merchant's own `api_v3_key`,
      // so only a holder of that key could have produced them, and
      // `setPlatformCert` additionally binds serial, validity window and the
      // optional CA anchor before anything is cached.
      AnswerVerifier(),
      [selfWeak, cb](const Json::Value &result, const std::string &err) {
          auto self = selfWeak.lock();
          if (!self)
          {
              if (*cb)
                  (*cb)(Json::Value{}, "wechat client destroyed before certificate response");
              return;
          }
          if (!err.empty())
          {
              if (*cb)
                  (*cb)(result, err);
              return;
          }
          if (!result.isMember("data") || !result["data"].isArray())
          {
              if (*cb)
                  (*cb)(result, "invalid certificate response format");
              return;
          }
          size_t installed = 0;
          for (const auto &certNode : result["data"])
          {
              std::string serialNo = certNode.get("serial_no", "").asString();
              auto encNode = certNode["encrypt_certificate"];
              if (serialNo.empty() || encNode.isNull())
                  continue;
              std::string ciphertext = encNode.get("ciphertext", "").asString();
              std::string nonceStr = encNode.get("nonce", "").asString();
              std::string associatedData = encNode.get("associated_data", "").asString();
              std::string plaintext;
              std::string decryptErr;
              if (!self
                     ->decryptResource(ciphertext, nonceStr, associatedData, plaintext, decryptErr))
              {
                  LOG_WARN << "[WechatChannel] certificate decrypt failed for serial " << serialNo
                           << ": " << decryptErr;
                  continue;
              }
              // setPlatformCert validates parse, validity window, serial
              // self-consistency and (optionally) the CA chain before caching.
              if (self->setPlatformCert(serialNo, plaintext))
              {
                  ++installed;
              }
          }
          if (installed == 0)
          {
              if (*cb)
                  (*cb)(result, "no platform certificate passed validation");
              return;
          }
          if (*cb)
              (*cb)(result, "");
      }
    );
}

std::string WechatPayClient::getPlatformCert(const std::string &serialNo) const
{
    // Both sides of the cache use the canonical spelling, so a certificate
    // stored for one notification header is also found by another header that
    // merely writes the same serial differently.
    std::shared_lock<std::shared_mutex> lock(certsMutex_);
    auto it = platformCerts_.find(normalizeSerialHex(serialNo));
    if (it != platformCerts_.end())
    {
        return it->second;
    }
    return "";
}

namespace
{
/// Optional trust anchor: when `platform_ca_cert_path` is configured, a
/// downloaded platform certificate must chain to a CA in that file. Without
/// it, whoever answers for `api_base` decides which certificate later verifies
/// payment notifications.
bool chainsToConfiguredAnchors(
  const std::string &certPem,
  const std::string &caPath,
  std::string &error
)
{
    if (caPath.empty())
    {
        return true;
    }

    std::string readErr;
    const std::string caPem = readFile(caPath, readErr);
    if (!readErr.empty())
    {
        error = "cannot read platform_ca_cert_path: " + readErr;
        return false;
    }

    X509_STORE *store = X509_STORE_new();
    if (!store)
    {
        error = "failed to create X509_STORE";
        return false;
    }

    size_t anchors = 0;
    BIO *bio = BIO_new_mem_buf(caPem.data(), static_cast<int>(caPem.size()));
    if (bio)
    {
        // The file may hold a bundle: keep reading until the PEMs run out.
        while (true)
        {
            X509 *ca = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            if (!ca)
            {
                break;
            }
            if (X509_STORE_add_cert(store, ca) == 1)
            {
                ++anchors;
            }
            X509_free(ca);
        }
        BIO_free(bio);
    }
    if (anchors == 0)
    {
        X509_STORE_free(store);
        error = "platform_ca_cert_path holds no readable certificate";
        return false;
    }

    X509 *leaf = parseCert(certPem);
    if (!leaf)
    {
        X509_STORE_free(store);
        error = "platform cert does not parse";
        return false;
    }

    X509_STORE_CTX *vfy = X509_STORE_CTX_new();
    bool ok = false;
    if (vfy && X509_STORE_CTX_init(vfy, store, leaf, nullptr) == 1)
    {
        ok = X509_verify_cert(vfy) == 1;
        if (!ok)
        {
            const int code = X509_STORE_CTX_get_error(vfy);
            error =
              std::string("chain verification failed: ") + X509_verify_cert_error_string(code);
        }
        X509_STORE_CTX_cleanup(vfy);
    }
    else
    {
        error = "failed to init X509_STORE_CTX";
    }
    if (vfy)
    {
        X509_STORE_CTX_free(vfy);
    }
    X509_free(leaf);
    X509_STORE_free(store);
    return ok;
}
}  // namespace

bool WechatPayClient::setPlatformCert(const std::string &serialNo, const std::string &certContent)
{
    if (serialNo.empty())
    {
        LOG_WARN << "[WechatChannel] refusing certificate with empty serial number";
        return false;
    }

    X509 *cert = parseCert(certContent);
    if (!cert)
    {
        LOG_WARN << "[WechatChannel] refusing non-X509 platform cert for serial " << serialNo;
        return false;
    }

    std::string validityError;
    const bool valid = isWithinValidity(cert, validityError);
    const ASN1_INTEGER *serial = X509_get_serialNumber(cert);
    char *raw = serial ? i2s_ASN1_INTEGER(nullptr, serial) : nullptr;
    const std::string actualSerial = raw ? normalizeSerialHex(raw) : std::string();
    if (raw)
    {
        OPENSSL_free(raw);
    }
    X509_free(cert);

    if (!valid)
    {
        LOG_WARN << "[WechatChannel] refusing platform cert " << serialNo << ": " << validityError;
        return false;
    }
    // The cache is keyed by the serial taken from the response body; binding it
    // to the serial inside the certificate means a response cannot file one
    // certificate under another certificate's name.
    if (actualSerial.empty() || actualSerial != normalizeSerialHex(serialNo))
    {
        LOG_WARN << "[WechatChannel] refusing platform cert: claimed serial " << serialNo
                 << " does not match certificate serial " << actualSerial;
        return false;
    }

    std::string chainError;
    if (!chainsToConfiguredAnchors(certContent, platformCaCertPath_, chainError))
    {
        LOG_WARN << "[WechatChannel] refusing platform cert " << serialNo << ": " << chainError;
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(certsMutex_);
    platformCerts_[actualSerial] = certContent;
    return true;
}

void WechatPayClient::queryTransaction(const std::string &orderNo, JsonCallback &&callback)
{
    if (orderNo.empty())
    {
        Json::Value result;
        callback(result, "missing orderNo");
        return;
    }
    if (mchId_.empty())
    {
        Json::Value result;
        callback(result, "missing mch_id");
        return;
    }

    // Both identifiers go into the path we sign, so neither may carry a
    // literal `?`, `#`, `&` or `/` that could re-shape the request target.
    const std::string path = "/v3/pay/transactions/out-trade-no/" +
                             pay::utils::urlEncodePathSegment(orderNo) +
                             "?mchid=" + pay::utils::urlEncodePathSegment(mchId_);
    const std::string body;
    const std::string timestamp = std::to_string(std::time(nullptr));
    const std::string nonce = drogon::utils::getUuid();
    std::string error;
    std::string auth = buildAuthorizationHeader("GET", path, body, timestamp, nonce, error);
    if (!error.empty())
    {
        Json::Value result;
        callback(result, error);
        return;
    }

    sendWechatRequest(
      apiBase_, "GET", path, body, auth, timeoutMs_, answerVerifier(), std::move(callback)
    );
}

void WechatPayClient::closeTransaction(const std::string &orderNo, JsonCallback &&callback)
{
    if (orderNo.empty())
    {
        Json::Value result;
        callback(result, "missing orderNo");
        return;
    }
    if (mchId_.empty())
    {
        Json::Value result;
        callback(result, "missing mch_id");
        return;
    }

    // The close is named by the same encoded path segment the query already
    // builds; `mchid` goes in the body, where the official parameter table puts
    // it. A paid trade answers the refusal (TRADE_ERROR under 403), so an
    // accepted 204 is the only answer that means "closed now".
    const std::string path =
      "/v3/pay/transactions/out-trade-no/" + pay::utils::urlEncodePathSegment(orderNo) + "/close";
    Json::Value request;
    request["mchid"] = mchId_;
    const std::string body = toJsonString(request);
    const std::string timestamp = std::to_string(std::time(nullptr));
    const std::string nonce = drogon::utils::getUuid();
    std::string error;
    std::string auth = buildAuthorizationHeader("POST", path, body, timestamp, nonce, error);
    if (!error.empty())
    {
        Json::Value result;
        callback(result, error);
        return;
    }

    sendWechatRequest(
      apiBase_, "POST", path, body, auth, timeoutMs_, answerVerifier(), std::move(callback)
    );
}

void WechatPayClient::closeOrder(const std::string &orderNo, JsonCallback &&callback)
{
    closeTransaction(orderNo, std::move(callback));
}

void WechatPayClient::refund(const Json::Value &payload, JsonCallback &&callback)
{
    Json::Value request = payload;
    if (request.get("notify_url", "").asString().empty())
    {
        request["notify_url"] = notifyUrl_;
    }

    const std::string path = "/v3/refund/domestic/refunds";
    const std::string body = toJsonString(request);
    const std::string timestamp = std::to_string(std::time(nullptr));
    const std::string nonce = drogon::utils::getUuid();
    std::string error;
    std::string auth = buildAuthorizationHeader("POST", path, body, timestamp, nonce, error);
    if (!error.empty())
    {
        Json::Value result;
        callback(result, error);
        return;
    }

    sendWechatRequest(
      apiBase_, "POST", path, body, auth, timeoutMs_, answerVerifier(), std::move(callback)
    );
}

void WechatPayClient::queryRefund(const std::string &refundNo, JsonCallback &&callback)
{
    if (refundNo.empty())
    {
        Json::Value result;
        callback(result, "missing refundNo");
        return;
    }

    const std::string path =
      "/v3/refund/domestic/refunds/" + pay::utils::urlEncodePathSegment(refundNo);
    const std::string body;
    const std::string timestamp = std::to_string(std::time(nullptr));
    const std::string nonce = drogon::utils::getUuid();
    std::string error;
    std::string auth = buildAuthorizationHeader("GET", path, body, timestamp, nonce, error);
    if (!error.empty())
    {
        Json::Value result;
        callback(result, error);
        return;
    }

    sendWechatRequest(
      apiBase_, "GET", path, body, auth, timeoutMs_, answerVerifier(), std::move(callback)
    );
}

std::string WechatPayClient::buildAuthorizationHeader(
  const std::string &method,
  const std::string &url,
  const std::string &body,
  const std::string &timestamp,
  const std::string &nonce,
  std::string &error
) const
{
    if (mchId_.empty() || serialNo_.empty() || privateKeyPath_.empty())
    {
        error = "wechat pay config missing mch_id/serial_no/private_key_path";
        return {};
    }

    std::string message =
      method + "\n" + url + "\n" + timestamp + "\n" + nonce + "\n" + body + "\n";
    std::string signatureB64;
    if (!signMessage(message, privateKeyPath_, signatureB64, error))
    {
        return {};
    }

    std::string auth = "WECHATPAY2-SHA256-RSA2048 mchid=\"" + mchId_ +
                       "\","
                       "nonce_str=\"" +
                       nonce +
                       "\","
                       "timestamp=\"" +
                       timestamp +
                       "\","
                       "serial_no=\"" +
                       serialNo_ +
                       "\","
                       "signature=\"" +
                       signatureB64 + "\"";
    return auth;
}

std::string WechatPayClient::resolveTrustedPlatformCert(
  const std::string &serialNo,
  std::string &error
)
{
    if (serialNo.empty())
    {
        error = "missing Wechatpay-Serial";
        return "";
    }

    // The serial arrives in a header the far end controls and the rejection
    // text carries it onward to our own API callers, so bound it before it can
    // turn into a log/response injection. A WeChat certificate serial is a
    // 32-hex-digit string; 64 leaves generous room for the public-key id
    // spelling without any realistic echo budget.
    if (serialNo.size() > 64)
    {
        error = "Wechatpay-Serial too long";
        return "";
    }

    std::string certContent = getPlatformCert(serialNo);

    // The header names the certificate that produced the signature, so a
    // statically deployed platform certificate may only serve the serial it
    // actually carries. Comparing against the merchant's own `serial_no`
    // compares two unrelated numbering spaces and rejects or mis-binds
    // depending on configuration.
    if (certContent.empty() && !platformCertPath_.empty())
    {
        std::string readErr;
        const std::string staticCert = readFile(platformCertPath_, readErr);
        if (!readErr.empty())
        {
            error = "failed to read static cert: " + readErr;
            return "";
        }
        if (certificateSerialHex(staticCert) == normalizeSerialHex(serialNo))
        {
            certContent = staticCert;
        }
    }

    if (certContent.empty())
    {
        // Unseen serial: WeChat rotated to a new platform certificate. Fetch
        // the current set (self-throttled) and reject this one -- WeChat
        // retries, and by then the certificate is cached.
        downloadCertificates([](const Json::Value &, const std::string &err) {
            if (!err.empty() && err != "certificate download throttled")
            {
                LOG_WARN << "[WechatChannel] refresh after unknown serial failed: " << err;
            }
        });
        error = "no trusted platform certificate for serial: " + serialNo;
        return "";
    }

    return certContent;
}

bool WechatPayClient::verifyCallback(
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &body,
  const std::string &signature,
  const std::string &serialNo,
  std::string &error
)
{
    const std::string certContent = resolveTrustedPlatformCert(serialNo, error);
    if (certContent.empty())
    {
        return false;
    }

    std::string message = timestamp + "\n" + nonce + "\n" + body + "\n";
    return verifyMessageWithCert(message, signature, certContent, error);
}

bool WechatPayClient::verifyResponse(const drogon::HttpResponsePtr &resp, std::string &error)
{
    const std::string timestamp = std::string(resp->getHeader("Wechatpay-Timestamp"));
    const std::string nonce = std::string(resp->getHeader("Wechatpay-Nonce"));
    const std::string signature = std::string(resp->getHeader("Wechatpay-Signature"));
    const std::string serialNo = std::string(resp->getHeader("Wechatpay-Serial"));

    // Refuse a missing header set before the certificate resolver ever sees an
    // empty serial: an unsigned answer must not be able to spend the shared
    // download window on a refresh that cannot make it signed.
    if (timestamp.empty() || nonce.empty() || signature.empty())
    {
        error = "missing Wechatpay-Timestamp/Nonce/Signature answer headers";
        return false;
    }

    const std::string certContent = resolveTrustedPlatformCert(serialNo, error);
    if (certContent.empty())
    {
        return false;
    }

    const std::string message = timestamp + "\n" + nonce + "\n" + std::string(resp->body()) + "\n";
    return verifyMessageWithCert(message, signature, certContent, error);
}

WechatPayClient::AnswerVerifier WechatPayClient::answerVerifier()
{
    // Response handlers run on the IO loop after the caller may have released
    // the client. Producers own it through shared_ptr (the registry and every
    // service), so pinning weakly turns the shutdown race into a dropped
    // answer -- the same call the certificate refresh made for itself. A
    // non-shared instance cannot be pinned, and its own tests wait for the
    // answer before destruction, so the raw binding stands for those only.
    std::weak_ptr<WechatPayClient> weak = weak_from_this();
    const bool pinned = !weak.expired();
    return [weak, pinned, this](const drogon::HttpResponsePtr &resp, std::string &error) {
        if (pinned)
        {
            // Hold the pin across the whole call. A bare null-check on a
            // temporary `weak.lock()` would drop that last reference the moment
            // it returned, so the owner could destroy the client in the gap
            // before `verifyResponse` touched `this` -- exactly the shutdown
            // race the pin exists to close.
            const auto self = weak.lock();
            if (!self)
            {
                error = "wechat client destroyed before the answer was verified";
                return false;
            }
            return self->verifyResponse(resp, error);
        }
        return verifyResponse(resp, error);
    };
}

bool WechatPayClient::decryptResource(
  const std::string &ciphertext,
  const std::string &nonce,
  const std::string &associatedData,
  std::string &plaintext,
  std::string &error
) const
{
    if (apiV3Key_.empty())
    {
        error = "api_v3_key is not configured";
        return false;
    }

    return decryptAesGcm(ciphertext, nonce, associatedData, apiV3Key_, plaintext, error);
}

bool WechatPayClient::isConfigured() const
{
    // Helper function to check if a value is a placeholder
    auto isPlaceholder = [](const std::string &value) -> bool {
        return value.empty() || value.find("__env_var:") == 0;
    };

    // Check all required configuration fields
    if (
      isPlaceholder(appId_) || isPlaceholder(mchId_) || isPlaceholder(serialNo_) ||
      isPlaceholder(apiV3Key_) || isPlaceholder(privateKeyPath_)
    )
    {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// PaymentChannel SPI
// ---------------------------------------------------------------------------

const std::string &WechatPayClient::name() const
{
    static const std::string kName = "wechat";
    return kName;
}

void WechatPayClient::createPayment(const Json::Value &payload, JsonCallback &&callback)
{
    // Wechat v3 "native" transactions serve both entry points.
    createTransactionNative(payload, std::move(callback));
}

void WechatPayClient::createQRPayment(const Json::Value &payload, JsonCallback &&callback)
{
    createTransactionNative(payload, std::move(callback));
}

void WechatPayClient::queryPayment(const std::string &orderNo, JsonCallback &&callback)
{
    queryTransaction(orderNo, std::move(callback));
}

bool WechatPayClient::verifyCallback(
  const drogon::HttpRequestPtr &req,
  drogon_pay::CallbackEvent &event,
  std::string &error
)
{
    const std::string body = std::string(req->body());
    const std::string signature = std::string(req->getHeader("Wechatpay-Signature"));
    const std::string timestamp = std::string(req->getHeader("Wechatpay-Timestamp"));
    const std::string nonce = std::string(req->getHeader("Wechatpay-Nonce"));
    const std::string serialNo = std::string(req->getHeader("Wechatpay-Serial"));

    if (signature.empty() || timestamp.empty() || nonce.empty())
    {
        error = "missing Wechatpay-Signature/Timestamp/Nonce headers";
        return false;
    }

    if (!verifyCallback(timestamp, nonce, body, signature, serialNo, error))
    {
        return false;
    }

    Json::Value bodyJson;
    {
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errors;
        if (!reader->parse(body.data(), body.data() + body.size(), &bodyJson, &errors))
        {
            error = "invalid JSON body: " + errors;
            return false;
        }
    }

    const std::string eventType = bodyJson.get("event_type", "").asString();

    // Decrypt the enclosed resource so callers get the actual transaction.
    // Every WeChat V3 notification carries an AES-GCM `resource` object; the
    // transaction fields the caller normalizes live inside it. A signed body
    // without one is malformed, and accepting it would hand the caller an event
    // with an empty order_no and a null payload, so it is refused here.
    Json::Value resourceJson;
    std::string plaintext;
    if (bodyJson.isMember("resource") && bodyJson["resource"].isObject())
    {
        const auto &res = bodyJson["resource"];
        std::string decryptErr;
        if (!decryptResource(
              res.get("ciphertext", "").asString(),
              res.get("nonce", "").asString(),
              res.get("associated_data", "").asString(),
              plaintext,
              decryptErr
            ))
        {
            error = "resource decrypt failed: " + decryptErr;
            return false;
        }
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errors;
        if (!reader->parse(
              plaintext.data(), plaintext.data() + plaintext.size(), &resourceJson, &errors
            ))
        {
            error = "decrypted resource is not valid JSON: " + errors;
            return false;
        }
    }
    else
    {
        error = "notification body has no resource object";
        return false;
    }

    event.channel = name();
    event.orderNo = resourceJson.get("out_trade_no", "").asString();
    event.channelTxnId = resourceJson.get("transaction_id", "").asString();
    event.tradeStatus = resourceJson.get("trade_state", eventType).asString();
    event.paid = (eventType == "TRANSACTION.SUCCESS");
    if (resourceJson.isMember("amount") && resourceJson["amount"].isObject())
    {
        event.amountTotal = resourceJson["amount"].get("total", 0).asInt64();
    }
    event.rawPayload = plaintext.empty() ? body : plaintext;
    event.payload = resourceJson.isNull() ? bodyJson : resourceJson;
    event.payload["event_type"] = eventType;
    return true;
}

void WechatPayClient::onStart()
{
    // Warm up the platform certificates; a failure here is non-fatal because
    // verifyCallback() can still fall back to the statically configured cert.
    downloadCertificates([](const Json::Value &, const std::string &err) {
        if (!err.empty())
        {
            LOG_WARN << "[WechatChannel] certificate warm-up failed: " << err;
        }
        else
        {
            LOG_INFO << "[WechatChannel] platform certificates refreshed";
        }
    });
}
