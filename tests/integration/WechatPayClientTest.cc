#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include "channels/WechatChannel.h"
#include "TestConfigHelper.h"
#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <chrono>

namespace
{
std::string encryptAesGcm(
  const std::string &plaintext,
  const std::string &nonce,
  const std::string &aad,
  const std::string &apiV3Key
)
{
    const EVP_CIPHER *cipher = EVP_aes_256_gcm();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
    {
        return {};
    }

    if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (
      EVP_EncryptInit_ex(
        ctx,
        nullptr,
        nullptr,
        reinterpret_cast<const unsigned char *>(apiV3Key.data()),
        reinterpret_cast<const unsigned char *>(nonce.data())
      ) != 1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    int outLen = 0;
    if (!aad.empty())
    {
        if (
          EVP_EncryptUpdate(
            ctx,
            nullptr,
            &outLen,
            reinterpret_cast<const unsigned char *>(aad.data()),
            static_cast<int>(aad.size())
          ) != 1
        )
        {
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }
    }

    std::string ciphertext(plaintext.size(), '\0');
    if (
      EVP_EncryptUpdate(
        ctx,
        reinterpret_cast<unsigned char *>(&ciphertext[0]),
        &outLen,
        reinterpret_cast<const unsigned char *>(plaintext.data()),
        static_cast<int>(plaintext.size())
      ) != 1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    int totalLen = outLen;

    if (
      EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(&ciphertext[totalLen]), &outLen) !=
      1
    )
    {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    totalLen += outLen;
    ciphertext.resize(totalLen);

    unsigned char tag[16] = {};
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    EVP_CIPHER_CTX_free(ctx);

    ciphertext.append(reinterpret_cast<const char *>(tag), sizeof(tag));
    return drogon::utils::base64Encode(ciphertext);
}

bool generateKeyAndCert(
  EVP_PKEY **outKey,
  std::string &certPem,
  long notBeforeOffset = 0,
  long validitySeconds = 60 * 60
)
{
    if (!outKey)
    {
        return false;
    }
    *outKey = nullptr;

    EVP_PKEY *pkey = EVP_PKEY_new();
    if (!pkey)
    {
        return false;
    }

    RSA *rsa = RSA_new();
    BIGNUM *e = BN_new();
    if (!rsa || !e)
    {
        RSA_free(rsa);
        BN_free(e);
        EVP_PKEY_free(pkey);
        return false;
    }

    if (
      BN_set_word(e, RSA_F4) != 1 || RSA_generate_key_ex(rsa, 2048, e, nullptr) != 1 ||
      EVP_PKEY_assign_RSA(pkey, rsa) != 1
    )
    {
        RSA_free(rsa);
        BN_free(e);
        EVP_PKEY_free(pkey);
        return false;
    }
    BN_free(e);

    X509 *cert = X509_new();
    if (!cert)
    {
        EVP_PKEY_free(pkey);
        return false;
    }

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), notBeforeOffset);
    X509_gmtime_adj(X509_get_notAfter(cert), validitySeconds);
    X509_set_pubkey(cert, pkey);

    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Test"), -1, -1, 0
    );
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, EVP_sha256()) == 0)
    {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }

    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
    {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }
    if (PEM_write_bio_X509(bio, cert) != 1)
    {
        BIO_free(bio);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }

    BUF_MEM *buf = nullptr;
    BIO_get_mem_ptr(bio, &buf);
    if (!buf || !buf->data || buf->length == 0)
    {
        BIO_free(bio);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }

    certPem.assign(buf->data, buf->length);
    BIO_free(bio);
    X509_free(cert);
    *outKey = pkey;
    return true;
}

bool signMessage(const std::string &message, EVP_PKEY *pkey, std::string &signatureB64)
{
    if (!pkey)
    {
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        return false;
    }
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1)
    {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    if (EVP_DigestSignUpdate(ctx, message.data(), message.size()) != 1)
    {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    size_t sigLen = 0;
    if (EVP_DigestSignFinal(ctx, nullptr, &sigLen) != 1)
    {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    std::string signature(sigLen, '\0');
    if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char *>(&signature[0]), &sigLen) != 1)
    {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    signature.resize(sigLen);
    signatureB64 = drogon::utils::base64Encode(signature);
    return true;
}

std::string privateKeyPem(EVP_PKEY *pkey)
{
    if (!pkey)
    {
        return {};
    }
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
    {
        return {};
    }
    if (PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
    {
        BIO_free(bio);
        return {};
    }
    char *data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string out;
    if (data && length > 0)
    {
        out.assign(data, static_cast<size_t>(length));
    }
    BIO_free(bio);
    return out;
}

std::filesystem::path writeTempPem(const std::string &content)
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("wechatpay_test_" + drogon::utils::getUuid() + ".pem");
    std::ofstream out(path.string(), std::ios::binary);
    out << content;
    return path;
}

// generateKeyAndCert issues a certificate with serial 1, and the client caches
// platform certificates under the serial the certificate itself carries, so a
// notification header must name exactly this value.
const char *kTestSerial = "1";

// WeChat fixes the AEAD IV at 12 bytes, so every nonce here is 12 bytes.
const char *kTestNonce = "nonce0000001";

void removeTempPem(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
}  // namespace

DROGON_TEST(WechatPayClient_DecryptResource)
{
    Json::Value config;
    config["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    const std::string plaintext = R"({"foo":"bar","amount":100})";
    const std::string nonce = kTestNonce;
    const std::string aad = "transaction";

    const std::string ciphertextB64 =
      encryptAesGcm(plaintext, nonce, aad, config["api_v3_key"].asString());
    CHECK(!ciphertextB64.empty());

    std::string decrypted;
    std::string error;
    CHECK(client.decryptResource(ciphertextB64, nonce, aad, decrypted, error));
    CHECK(error.empty());
    CHECK(decrypted == plaintext);
}

DROGON_TEST(WechatPayClient_CertificateSerialHex)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));
    CHECK(WechatPayClient::certificateSerialHex(certPem) == kTestSerial);
    // Garbage in, empty out: callers treat "" as "this content is not a
    // certificate" rather than as a serial that can match anything.
    CHECK(WechatPayClient::certificateSerialHex("not a certificate").empty());
    CHECK(WechatPayClient::certificateSerialHex("").empty());
    EVP_PKEY_free(pkey);
}

DROGON_TEST(WechatPayClient_VerifyCallback)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));
    const auto certPath = writeTempPem(certPem);

    Json::Value config;
    config["platform_cert_path"] = certPath.string();
    config["serial_no"] = "MERCHANT_CERT_SERIAL";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    const std::string timestamp = "1700000000";
    const std::string nonce = kTestNonce;
    const std::string body = R"({"id":"test"})";
    const std::string message = timestamp + "\n" + nonce + "\n" + body + "\n";

    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    std::string error;
    CHECK(client.verifyCallback(timestamp, nonce, body, signatureB64, kTestSerial, error));
    CHECK(error.empty());

    EVP_PKEY_free(pkey);
    removeTempPem(certPath);
}

DROGON_TEST(WechatPayClient_VerifyCallback_SerialMismatch)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));
    const auto certPath = writeTempPem(certPem);

    Json::Value config;
    config["platform_cert_path"] = certPath.string();
    config["serial_no"] = "MERCHANT_CERT_SERIAL";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    const std::string timestamp = "1700000000";
    const std::string nonce = kTestNonce;
    const std::string body = R"({"id":"test"})";
    const std::string message = timestamp + "\n" + nonce + "\n" + body + "\n";

    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    // A header naming a serial the deployed certificate does not carry must not
    // be served by that certificate, even though the signature itself verifies
    // against its public key.
    std::string error;
    CHECK(!client.verifyCallback(timestamp, nonce, body, signatureB64, "99", error));
    CHECK(error == std::string("no trusted platform certificate for serial: ") + "99");
    // The merchant's own certificate serial is a different numbering space and
    // no longer acts as a match criterion.
    CHECK(
      !client.verifyCallback(timestamp, nonce, body, signatureB64, "MERCHANT_CERT_SERIAL", error)
    );
    CHECK(error == "no trusted platform certificate for serial: MERCHANT_CERT_SERIAL");

    EVP_PKEY_free(pkey);
    removeTempPem(certPath);
}

DROGON_TEST(WechatPayClient_VerifyCallback_MissingSerial)
{
    Json::Value config;
    config["serial_no"] = "MERCHANT_CERT_SERIAL";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    std::string error;
    CHECK(!client.verifyCallback("1700000000", kTestNonce, "{}", "sig", "", error));
    CHECK(error == "missing Wechatpay-Serial");
}

DROGON_TEST(WechatPayClient_VerifyCallback_MissingCert)
{
    Json::Value config;
    config["serial_no"] = "MERCHANT_CERT_SERIAL";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    std::string error;
    CHECK(!client.verifyCallback("1700000000", kTestNonce, "{}", "sig", "1", error));
    CHECK(error == "no trusted platform certificate for serial: 1");
}

DROGON_TEST(WechatPayClient_DecryptResource_InvalidKey)
{
    Json::Value config;
    config["api_v3_key"] = "short_key";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    std::string plaintext;
    std::string error;
    CHECK(!client.decryptResource("ciphertext", "nonce", "aad", plaintext, error));
    CHECK(error == "api_v3_key must be 32 bytes");
}

DROGON_TEST(WechatPayClient_DecryptResource_ShortCiphertext)
{
    Json::Value config;
    config["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    std::string plaintext;
    std::string error;
    CHECK(!client.decryptResource("c2hvcnQ=", "nonce", "aad", plaintext, error));
    CHECK(error == "ciphertext too short");
}

DROGON_TEST(WechatPayClient_DecryptResource_InvalidTag)
{
    Json::Value config;
    config["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    std::string plaintext;
    std::string error;
    const std::string fakeCiphertext = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    CHECK(!client.decryptResource(fakeCiphertext, kTestNonce, "aad", plaintext, error));
    CHECK(error == "decrypt final failed");
    CHECK(plaintext.empty());
}

DROGON_TEST(WechatPayClient_DecryptResource_RejectsShortNonce)
{
    Json::Value config;
    config["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    // A resource cannot choose its own IV length: AEAD-256-GCM with a
    // non-canonical IV is a cipher-parameter downgrade, not a variant WeChat
    // ever sends.
    const std::string ciphertextB64 =
      encryptAesGcm(R"({"x":1})", "short", "aad", config["api_v3_key"].asString());
    CHECK(!ciphertextB64.empty());

    std::string plaintext;
    std::string error;
    CHECK(!client.decryptResource(ciphertextB64, "short", "aad", plaintext, error));
    CHECK(error == "nonce must be 12 bytes");
}

DROGON_TEST(WechatPayClient_VerifyCallback_InvalidSignature)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));
    const auto certPath = writeTempPem(certPem);

    Json::Value config;
    config["platform_cert_path"] = certPath.string();
    config["serial_no"] = "MERCHANT_CERT_SERIAL";
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    const std::string timestamp = "1700000000";
    const std::string nonce = kTestNonce;
    const std::string body = R"({"id":"test"})";

    std::string signatureB64;
    CHECK(signMessage("tampered\n", pkey, signatureB64));

    std::string error;
    CHECK(!client.verifyCallback(timestamp, nonce, body, signatureB64, kTestSerial, error));
    CHECK(error == "signature verify failed");

    EVP_PKEY_free(pkey);
    removeTempPem(certPath);
}

DROGON_TEST(WechatPayClient_DecryptResource_MissingKey)
{
    Json::Value config;
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    std::string plaintext;
    std::string error;
    CHECK(!client.decryptResource("c2hvcnQ=", "nonce", "aad", plaintext, error));
    CHECK(error == "api_v3_key is not configured");
}

DROGON_TEST(WechatPayClient_DownloadCertificates)
{
    Json::Value config;
    config["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    WechatPayClient client(config);

    std::promise<std::string> promise;
    client.downloadCertificates([&promise](const Json::Value &, const std::string &err) {
        promise.set_value(err);
    });

    auto future = promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const std::string err = future.get();
    CHECK(!err.empty());
    CHECK(err.find("missing mch_id/serial_no/private_key_path") != std::string::npos);
}

DROGON_TEST(WechatPayClient_SetPlatformCert_BindsCacheKeyToCertificateSerial)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    Json::Value config;
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    // A download response cannot file one certificate under another serial:
    // that would let a spoofed body poison the cache for a future notification.
    CHECK(!client.setPlatformCert("99", certPem));
    CHECK(client.getPlatformCert("99").empty());
    CHECK(client.setPlatformCert(kTestSerial, certPem));
    CHECK(client.getPlatformCert(kTestSerial) == certPem);
    // A zero-padded spelling of the same serial lands on the same cache entry,
    // on the read side as well as the write side.
    CHECK(client.setPlatformCert("0001", certPem));
    CHECK(client.getPlatformCert("0001") == certPem);
    CHECK(client.getPlatformCert(kTestSerial) == certPem);

    EVP_PKEY_free(pkey);
}

DROGON_TEST(WechatPayClient_SetPlatformCert_RejectsUnusableContent)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    Json::Value config;
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    CHECK(!client.setPlatformCert("", certPem));
    CHECK(!client.setPlatformCert(kTestSerial, ""));
    CHECK(!client.setPlatformCert(kTestSerial, "not a certificate"));
    CHECK(!client.setPlatformCert(kTestSerial, std::string("-----BEGIN CERTIFICATE-----\n")));
    CHECK(client.getPlatformCert(kTestSerial).empty());

    EVP_PKEY_free(pkey);
}

DROGON_TEST(WechatPayClient_SetPlatformCert_RejectsOutOfValidity)
{
    EVP_PKEY *expiredKey = nullptr;
    std::string expiredPem;
    CHECK(generateKeyAndCert(&expiredKey, expiredPem, -7200, -3600));

    EVP_PKEY *futureKey = nullptr;
    std::string futurePem;
    CHECK(generateKeyAndCert(&futureKey, futurePem, 3600, 7200));

    Json::Value config;
    config["api_base"] = "http://127.0.0.1:9";
    WechatPayClient client(config);

    CHECK(!client.setPlatformCert(kTestSerial, expiredPem));
    CHECK(!client.setPlatformCert(kTestSerial, futurePem));
    CHECK(client.getPlatformCert(kTestSerial).empty());

    EVP_PKEY_free(expiredKey);
    EVP_PKEY_free(futureKey);
}

DROGON_TEST(WechatPayClient_SetPlatformCert_RejectsUnreadableAnchorBundle)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    Json::Value config;
    config["api_base"] = "http://127.0.0.1:9";
    config["platform_ca_cert_path"] = (std::filesystem::temp_directory_path() /
                                       ("missing_anchor_" + drogon::utils::getUuid() + ".pem"))
                                        .string();
    WechatPayClient client(config);

    // Failing closed on a misconfigured anchor bundle is deliberate: silently
    // skipping chain validation would make the setting look enabled while doing
    // nothing.
    CHECK(!client.setPlatformCert(kTestSerial, certPem));

    EVP_PKEY_free(pkey);
}

DROGON_TEST(WechatPayClient_QueryTransaction_ReportsHttpErrorAsFailure)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));
    const std::string keyPem = privateKeyPem(pkey);
    CHECK(!keyPem.empty());
    const auto keyPath = writeTempPem(keyPem);

    // Point the channel at the suite's own listener: the V3 path does not exist
    // there, so the call gets a real non-2xx answer instead of a connection
    // error. Before the fix the channel reported success for any parseable
    // body, which let the service layer mark a failed query as a live payment.
    Json::Value config;
    config["mch_id"] = "1900000000";
    config["serial_no"] = kTestSerial;
    config["private_key_path"] = keyPath.string();
    config["api_base"] = pay::test_util::testBaseUrl();
    WechatPayClient client(config);

    std::promise<std::string> errorPromise;
    client.queryTransaction(
      "TEST-ORDER-1",
      [&errorPromise](const Json::Value &, const std::string &err) { errorPromise.set_value(err); }
    );

    auto future = errorPromise.get_future();
    REQUIRE(pay::test_util::waitForFutureReady(future, std::chrono::seconds(10)));
    const std::string err = future.get();
    CHECK(err.rfind("HTTP 404", 0) == 0);

    EVP_PKEY_free(pkey);
    removeTempPem(keyPath);
}
