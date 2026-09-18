#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
#include <future>
#include "models/PayIdempotency.h"
#include "drogon_pay/PayPlugin.h"
#include "channels/WechatChannel.h"
#include "services/PaymentService.h"
#include "utils/PayUtils.h"
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

bool writeTempPrivateKey(const std::filesystem::path &path)
{
    EVP_PKEY *pkey = EVP_PKEY_new();
    if (!pkey)
    {
        return false;
    }

    RSA *rsa = RSA_new();
    BIGNUM *bn = BN_new();
    if (!rsa || !bn)
    {
        if (bn)
        {
            BN_free(bn);
        }
        if (rsa)
        {
            RSA_free(rsa);
        }
        EVP_PKEY_free(pkey);
        return false;
    }

    if (BN_set_word(bn, RSA_F4) != 1 || RSA_generate_key_ex(rsa, 2048, bn, nullptr) != 1)
    {
        BN_free(bn);
        RSA_free(rsa);
        EVP_PKEY_free(pkey);
        return false;
    }

    if (EVP_PKEY_assign_RSA(pkey, rsa) != 1)
    {
        BN_free(bn);
        RSA_free(rsa);
        EVP_PKEY_free(pkey);
        return false;
    }
    BN_free(bn);

    std::ofstream out(path.string(), std::ios::binary);
    if (!out)
    {
        EVP_PKEY_free(pkey);
        return false;
    }

    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
    {
        EVP_PKEY_free(pkey);
        return false;
    }
    if (PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
    {
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return false;
    }

    BUF_MEM *buf = nullptr;
    BIO_get_mem_ptr(bio, &buf);
    if (!buf || !buf->data || buf->length == 0)
    {
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return false;
    }

    out.write(buf->data, static_cast<std::streamsize>(buf->length));
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    return static_cast<bool>(out);
}

void ensureCreatePaymentTables(const std::shared_ptr<drogon::orm::DbClient> &client)
{
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
}

bool waitForOrderStatus(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &title,
  std::string &orderNo,
  const std::string &orderStatus,
  const std::string &paymentStatus
)
{
    for (int i = 0; i < 40; ++i)
    {
        const auto orderRows =
          client->execSqlSync("SELECT order_no, status FROM pay_order WHERE title = $1", title);
        if (!orderRows.empty())
        {
            orderNo = orderRows.front()["order_no"].as<std::string>();
            const auto currentOrderStatus = orderRows.front()["status"].as<std::string>();
            const auto paymentRows =
              client->execSqlSync("SELECT status FROM pay_payment WHERE order_no = $1", orderNo);
            if (!paymentRows.empty())
            {
                const auto currentPaymentStatus = paymentRows.front()["status"].as<std::string>();
                if (currentOrderStatus == orderStatus && currentPaymentStatus == paymentStatus)
                {
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
}  // namespace

DROGON_TEST(PayPlugin_CreatePayment_WechatError)
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
    ensureCreatePaymentTables(client);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["app_id"] = "";
    wechatConfig["mch_id"] = "";
    wechatConfig["notify_url"] = "";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    const std::string title = "CreatePayError_" + drogon::utils::getUuid();

    CreatePaymentRequest request;
    request.userId = 10001;
    request.amount = "9.99";
    request.currency = "CNY";
    request.description = title;
    // Phase 2: unknown/empty channel no longer falls back to wechat, so the
    // channel must be explicit for the wechat misconfiguration error to fire.
    request.channel = "wechat";

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->createPayment(
      request,
      "",
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(error);  // Should have an error
    CHECK(error.message().find("missing appid/mchid/notify_url") != std::string::npos);

    std::string orderNo;
    CHECK(waitForOrderStatus(client, title, orderNo, "FAILED", "FAIL"));

    client->execSqlSync("DELETE FROM pay_payment WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_CreatePayment_WechatSuccess)
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
    ensureCreatePaymentTables(client);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    const std::string title = "CreatePayOK_" + drogon::utils::getUuid();

    CreatePaymentRequest request;
    request.userId = 10003;
    request.amount = "9.99";
    request.currency = "CNY";
    request.description = title;
    request.channel = "wechat";

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->createPayment(
      request,
      "",
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(error);  // Should have an error (missing appid/mchid/notify_url)
    CHECK(error.message().find("missing appid/mchid/notify_url") != std::string::npos);

    std::string orderNo;
    CHECK(waitForOrderStatus(client, title, orderNo, "FAILED", "FAIL"));

    client->execSqlSync("DELETE FROM pay_payment WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_CreatePayment_IdempotencySnapshot)
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
    ensureCreatePaymentTables(client);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["notify_url"] = "https://notify.invalid";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();
    const std::string title = "CreatePayIdem_" + drogon::utils::getUuid();

    CreatePaymentRequest request;
    request.userId = 10002;
    request.amount = "19.99";
    request.currency = "CNY";
    request.description = title;

    // Calculate request hash using the same method as PaymentService::createPayment
    Json::Value requestJson;
    requestJson["order_no"] = request.orderNo;
    requestJson["amount"] = request.amount;
    requestJson["currency"] = request.currency;
    requestJson["description"] = request.description;
    const std::string requestStr = pay::utils::toJsonString(requestJson);
    const std::string requestHash = drogon::utils::getSha256(requestStr);

    // Build snapshot in the format expected by IdempotencyService::checkDatabase
    // {"request_hash": "...", "response": {...}}
    Json::Value cachedResponse;
    cachedResponse["order_no"] = "prev_order";
    cachedResponse["payment_no"] = "prev_payment";
    cachedResponse["status"] = "PAYING";
    Json::Value snapshot;
    snapshot["request_hash"] = requestHash;
    snapshot["response"] = cachedResponse;
    const std::string snapshotBody = pay::utils::toJsonString(snapshot);

    client->execSqlSync(
      "INSERT INTO pay_idempotency "
      "(idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, $3, NOW() + INTERVAL '1 day')",
      idempotencyKey,
      requestHash,
      snapshotBody
    );

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->createPayment(
      request,
      idempotencyKey,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);  // Should not have an error

    const auto result = resultFuture.get();
    CHECK(result["order_no"].asString() == "prev_order");
    CHECK(result["payment_no"].asString() == "prev_payment");
    CHECK(result["status"].asString() == "PAYING");

    const auto countRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_order WHERE title = $1", title);
    CHECK(!countRows.empty());
    CHECK(countRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey);
}

DROGON_TEST(PayPlugin_CreatePayment_IdempotencyConflict)
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
    ensureCreatePaymentTables(client);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "mch_123";
    wechatConfig["notify_url"] = "https://notify.invalid";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();
    const std::string title = "CreatePayConflict_" + drogon::utils::getUuid();

    CreatePaymentRequest request;
    request.userId = 10003;
    request.amount = "29.99";
    request.currency = "CNY";
    request.description = title;

    client->execSqlSync(
      "INSERT INTO pay_idempotency "
      "(idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, $3, NOW() + INTERVAL '1 day')",
      idempotencyKey,
      "other_hash",
      "{}"
    );

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->createPayment(
      request,
      idempotencyKey,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    REQUIRE(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(error);  // Should have an error
    CHECK(error.message().find("idempotency key conflict") != std::string::npos);

    const auto countRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_order WHERE title = $1", title);
    CHECK(!countRows.empty());
    CHECK(countRows.front()["cnt"].as<int64_t>() == 0);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey);
}
