#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include "models/PayCallback.h"
#include "models/PayIdempotency.h"
#include "models/PayLedger.h"
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "models/PayRefund.h"
#include "drogon_pay/PayPlugin.h"
#include "channels/WechatChannel.h"
#include "services/CallbackService.h"
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

std::string toJsonCompact(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

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

bool generateKeyAndCert(EVP_PKEY **outKey, std::string &certPem)
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
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60);
    X509_set_pubkey(cert, pkey);

    auto subjectName = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(
      subjectName, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Test"), -1, -1, 0
    );
    X509_set_issuer_name(cert, subjectName);

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
}  // namespace

DROGON_TEST(PayPlugin_WechatCallback_WechatClientNotReady)
{
    PayPlugin plugin;
    // Phase 2: services are only constructed via initAndStart()/setTestChannels()
    // (the old lazy-init accessor was a data race and is gone). Initialize with
    // an empty channel map so callbackService() is valid but has no wechat
    // channel, which must surface the "wechat client not ready" error.
    plugin.setTestChannels({}, nullptr);

    std::string body = "{}";
    std::string signature = "";
    std::string timestamp = "";
    std::string nonce = "";
    std::string serial = "";

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto callbackService = plugin.callbackService();
    callbackService->handlePaymentCallback(
      body,
      signature,
      timestamp,
      nonce,
      serial,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    CHECK(result["code"].asString() == "FAIL");
    CHECK(result["message"].asString() == "wechat client not ready");
}

DROGON_TEST(PayPlugin_WechatCallback_DbClientNotReady)
{
    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, nullptr);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody("{}");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_EndToEnd)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "SUCCESS");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAID");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT entry_type FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["entry_type"].as<std::string>() == "PAYMENT");

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_IdempotencyHitRecordsCallback)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    const std::string notifyId = "notify_" + drogon::utils::getUuid();
    notify["id"] = notifyId;
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    client->execSqlSync(
      "INSERT INTO pay_idempotency "
      "(idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, $3, NOW() + INTERVAL '1 day')",
      notifyId,
      "hash",
      "{}"
    );

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    int64_t callbackCount = 0;
    for (int i = 0; i < 20; ++i)
    {
        const auto callbackRows = client->execSqlSync(
          "SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo
        );
        if (!callbackRows.empty())
        {
            callbackCount = static_cast<int64_t>(callbackRows.size());
            CHECK(callbackRows.front()["processed"].as<bool>());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(callbackCount >= 1);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", notifyId);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundIdempotencyHitRecordsCallback)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();

    // Insert parent records to satisfy FK constraints
    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount("9.99");
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount("9.99");
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount("9.99");
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    const std::string refundId = "rf_" + drogon::utils::getUuid();
    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = refundId;
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    const std::string notifyId = "notify_" + drogon::utils::getUuid();
    notify["id"] = notifyId;
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    client->execSqlSync(
      "INSERT INTO pay_idempotency "
      "(idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, $3, NOW() + INTERVAL '1 day')",
      notifyId,
      "hash",
      "{}"
    );

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handleRefundCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    int64_t callbackCount = 0;
    for (int i = 0; i < 20; ++i)
    {
        const auto callbackRows = client->execSqlSync(
          "SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo
        );
        if (!callbackRows.empty())
        {
            callbackCount = static_cast<int64_t>(callbackRows.size());
            CHECK(callbackRows.front()["processed"].as<bool>());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(callbackCount >= 1);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", notifyId);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_TransactionClosed)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "CLOSED";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.CLOSED";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "FAIL");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "CLOSED");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_TransactionRevoked)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "REVOKED";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.REVOKED";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "FAIL");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "CLOSED");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_TransactionRefundState)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "REFUND";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.REFUND";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "FAIL");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "CLOSED");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_TransactionUserPaying)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "USERPAYING";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.USERPAYING";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_TransactionNotPay)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "NOTPAY";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.NOTPAY";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_DuplicatePaymentNoDoubleLedger)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto sendCallback = [&](const std::string &notifyId) {
        Json::Value notify;
        notify["id"] = notifyId;
        notify["event_type"] = "TRANSACTION.SUCCESS";
        notify["resource_type"] = "encrypt-resource";
        notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
        notify["resource"]["ciphertext"] = ciphertext;
        notify["resource"]["nonce"] = nonce;
        notify["resource"]["associated_data"] = aad;
        const std::string body = toJsonCompact(notify);

        const std::string timestamp = std::to_string(
          std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
          )
            .count()
        );
        const std::string headerNonce = "headerNonce_" + drogon::utils::getUuid();
        const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
        std::string signatureB64;
        CHECK(signMessage(message, pkey, signatureB64));

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(body);
        req->addHeader("Wechatpay-Timestamp", timestamp);
        req->addHeader("Wechatpay-Nonce", headerNonce);
        req->addHeader("Wechatpay-Signature", signatureB64);
        req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

        auto callbackService = plugin.callbackService();
        std::promise<Json::Value> resultPromise;
        std::promise<std::error_code> errorPromise;
        callbackService->handlePaymentCallback(
          std::string(req->body()),
          std::string(req->getHeader("Wechatpay-Signature")),
          std::string(req->getHeader("Wechatpay-Timestamp")),
          std::string(req->getHeader("Wechatpay-Nonce")),
          std::string(req->getHeader("Wechatpay-Serial")),
          [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
              resultPromise.set_value(result);
              errorPromise.set_value(error);
          }
        );

        auto resultFuture = resultPromise.get_future();
        auto errorFuture = errorPromise.get_future();
        REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto result = resultFuture.get();
        const auto error = errorFuture.get();
        CHECK(!error);
    };

    const std::string notifyId1 = "notify_" + drogon::utils::getUuid();
    const std::string notifyId2 = "notify_" + drogon::utils::getUuid();
    sendCallback(notifyId1);
    sendCallback(notifyId2);

    const auto ledgerRows = client->execSqlSync(
      "SELECT COUNT(*) AS cnt FROM pay_ledger "
      "WHERE order_no = $1 AND entry_type = 'PAYMENT'",
      orderNo
    );
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 1);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", notifyId1);
    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", notifyId2);
    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidSignature)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    std::string signatureB64;
    CHECK(signMessage("tampered\n", pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto callbackRows =
      client->execSqlSync("SELECT id FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.empty());

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_DecryptFailure)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string correctApiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "00000000000000000000000000000000";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, correctApiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto callbackRows =
      client->execSqlSync("SELECT id FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.empty());

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_MissingSignatureHeaders)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = "dummy";
    notify["resource"]["nonce"] = "nonce";
    notify["resource"]["associated_data"] = "transaction";
    const std::string body = toJsonCompact(notify);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto callbackRows =
      client->execSqlSync("SELECT id FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.empty());

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_WechatCallback_MissingResource)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidJson)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    const std::string body = "{";

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidResourceFields)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = "";
    notify["resource"]["nonce"] = "";
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_MissingEventType)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    const std::string plainText = "{}";
    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidRefundEventType)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = "refund_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["refund_id"] = "wx_refund_1";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 100;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidTradeState)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = "ord_" + drogon::utils::getUuid();
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "UNKNOWN";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 100;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_MissingTransactionId)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = "ord_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 100;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidRefundAssociatedData)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = "refund_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["refund_id"] = "wx_refund_1";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 100;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidTransactionAssociatedData)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = "ord_" + drogon::utils::getUuid();
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 1999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_UnsupportedResourceType)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "invalid-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = "cipher";
    notify["resource"]["nonce"] = "nonce";
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_UnsupportedAlgorithm)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "UNKNOWN";
    notify["resource"]["ciphertext"] = "cipher";
    notify["resource"]["nonce"] = "nonce";
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidResourceJson)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    const std::string plainText = "{";
    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_SerialMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = "dummy";
    notify["resource"]["nonce"] = "nonce";
    notify["resource"]["associated_data"] = "transaction";
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_OTHER");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_AppIdMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = "wx_other";
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto callbackRows =
      client->execSqlSync("SELECT id FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.empty());

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_MchIdMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_refund "
      "ADD COLUMN IF NOT EXISTS response_payload TEXT"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = "mch_other";
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_AmountMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 1999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto callbackRows =
      client->execSqlSync("SELECT id FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.empty());

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_CurrencyMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["trade_state"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "USD";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "transaction";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "TRANSACTION.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto callbackRows =
      client->execSqlSync("SELECT id FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.empty());

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundSuccess)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    const std::string refundId = "rf_" + drogon::utils::getUuid();
    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = refundId;
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handleRefundCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUND_SUCCESS");
    CHECK(updatedRefund.getValueOfChannelRefundNo() == refundId);
    int64_t payloadReady = 0;
    for (int i = 0; i < 20; ++i)
    {
        const auto payloadRows = client->execSqlSync(
          "SELECT response_payload FROM pay_refund WHERE refund_no = $1", refundNo
        );
        if (!payloadRows.empty() && !payloadRows.front()["response_payload"].isNull())
        {
            const auto payload = payloadRows.front()["response_payload"].as<std::string>();
            if (payload.find(refundId) != std::string::npos)
            {
                payloadReady = 1;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(payloadReady == 1);

    const auto ledgerRows =
      client->execSqlSync("SELECT entry_type FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["entry_type"].as<std::string>() == "REFUND");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundAmountMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 1999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundCurrencyMismatch)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "USD";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundNotFound)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handleRefundCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundMissingFields)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_MissingRefundId)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = "refund_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 100;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_RefundClosed)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_callback ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "raw_body TEXT NOT NULL,"
      "signature VARCHAR(512),"
      "serial_no VARCHAR(64),"
      "verified BOOLEAN NOT NULL DEFAULT FALSE,"
      "processed BOOLEAN NOT NULL DEFAULT FALSE,"
      "received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "ALTER TABLE pay_callback "
      "ALTER COLUMN signature TYPE VARCHAR(512)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_ledger ("
      "id BIGSERIAL PRIMARY KEY,"
      "user_id BIGINT NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64),"
      "entry_type VARCHAR(32) NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "balance VARCHAR(32),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_status"] = "CLOSED";
    plain["refund_id"] = "wx_refund_1";
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.CLOSED";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handleRefundCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(!error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUND_FAIL");

    const auto callbackRows =
      client->execSqlSync("SELECT processed FROM pay_callback WHERE payment_no = $1", paymentNo);
    CHECK(callbackRows.size() >= 1);
    CHECK(callbackRows.front()["processed"].as<bool>());

    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    CHECK(ledgerRows.size() >= 1);
    CHECK(ledgerRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_callback WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidRefundStatus)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["refund_status"] = "UNKNOWN_STATUS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 999;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}

DROGON_TEST(PayPlugin_WechatCallback_InvalidRefundAmount)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "channel VARCHAR(32) NOT NULL DEFAULT 'alipay',"
      "title VARCHAR(512),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_refund ("
      "id BIGSERIAL PRIMARY KEY,"
      "refund_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL REFERENCES pay_order(order_no),"
      "payment_no VARCHAR(64) NOT NULL REFERENCES pay_payment(payment_no),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(10001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setRequestPayload("{}");
    payment.setResponsePayload("{}");
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo(refundNo);
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    EVP_PKEY *pkey = nullptr;
    std::string certPem;
    CHECK(generateKeyAndCert(&pkey, certPem));

    const auto tempDir = std::filesystem::temp_directory_path();
    const auto certPath = tempDir / ("wechatpay_cb_" + drogon::utils::getUuid() + ".pem");
    {
        std::ofstream out(certPath.string(), std::ios::binary);
        out << certPem;
    }

    const std::string apiV3Key = "0123456789abcdef0123456789abcdef";
    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = apiV3Key;
    wechatConfig["platform_cert_path"] = certPath.string();
    wechatConfig["serial_no"] = "SERIAL_TEST";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    Json::Value plain;
    plain["out_refund_no"] = refundNo;
    plain["refund_id"] = "rf_" + drogon::utils::getUuid();
    plain["refund_status"] = "SUCCESS";
    plain["out_trade_no"] = orderNo;
    plain["transaction_id"] = "tx_" + drogon::utils::getUuid();
    plain["appid"] = wechatConfig["app_id"].asString();
    plain["mchid"] = wechatConfig["mch_id"].asString();
    plain["amount"]["refund"] = 0;
    plain["amount"]["total"] = 999;
    plain["amount"]["currency"] = "CNY";
    const std::string plainText = toJsonCompact(plain);

    const std::string nonce = "nonce123";
    const std::string aad = "refund";
    const std::string ciphertext = encryptAesGcm(plainText, nonce, aad, apiV3Key);
    CHECK(!ciphertext.empty());

    Json::Value notify;
    notify["id"] = "notify_" + drogon::utils::getUuid();
    notify["event_type"] = "REFUND.SUCCESS";
    notify["resource_type"] = "encrypt-resource";
    notify["resource"]["algorithm"] = "AEAD_AES_256_GCM";
    notify["resource"]["ciphertext"] = ciphertext;
    notify["resource"]["nonce"] = nonce;
    notify["resource"]["associated_data"] = aad;
    const std::string body = toJsonCompact(notify);

    const std::string timestamp = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      )
        .count()
    );
    const std::string headerNonce = "headerNonce";
    const std::string message = timestamp + "\n" + headerNonce + "\n" + body + "\n";
    std::string signatureB64;
    CHECK(signMessage(message, pkey, signatureB64));

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(body);
    req->addHeader("Wechatpay-Timestamp", timestamp);
    req->addHeader("Wechatpay-Nonce", headerNonce);
    req->addHeader("Wechatpay-Signature", signatureB64);
    req->addHeader("Wechatpay-Serial", "SERIAL_TEST");

    auto callbackService = plugin.callbackService();
    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    callbackService->handlePaymentCallback(
      std::string(req->body()),
      std::string(req->getHeader("Wechatpay-Signature")),
      std::string(req->getHeader("Wechatpay-Timestamp")),
      std::string(req->getHeader("Wechatpay-Nonce")),
      std::string(req->getHeader("Wechatpay-Serial")),
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();
    CHECK(error);

    const auto updatedRefund = refundMapper.findByPrimaryKey(refund.getValueOfId());
    CHECK(updatedRefund.getValueOfStatus() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", notify["id"].asString()
    );

    EVP_PKEY_free(pkey);
    std::error_code ec;
    std::filesystem::remove(certPath, ec);
}
