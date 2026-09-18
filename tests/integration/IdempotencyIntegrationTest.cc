#include <drogon/drogon_test.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Mapper.h>
#include <drogon/utils/Utilities.h>
#include "models/PayCallback.h"
#include "models/PayIdempotency.h"
#include "models/PayLedger.h"
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "models/PayRefund.h"
#include "services/IdempotencyService.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>
#include <chrono>
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

}  // namespace

DROGON_TEST(PayIdempotency_DbUniqueKey)
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

    const std::string key = "test_" + drogon::utils::getUuid();

    client->execSqlSync(
      "INSERT INTO pay_idempotency (idempotency_key, request_hash, "
      "response_snapshot, expire_at) VALUES ($1, $2, $3, NOW() + "
      "INTERVAL '1 day')",
      key,
      "hash",
      "{}"
    );

    bool uniqueHit = false;
    try
    {
        client->execSqlSync(
          "INSERT INTO pay_idempotency (idempotency_key, request_hash, "
          "response_snapshot, expire_at) VALUES ($1, $2, $3, NOW() + "
          "INTERVAL '1 day')",
          key,
          "hash2",
          "{}"
        );
    }
    catch (const drogon::orm::DrogonDbException &)
    {
        uniqueHit = true;
    }

    CHECK(uniqueHit);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", key);
}

DROGON_TEST(PayIdempotency_RedisSetNx)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("redis_clients"));
    CHECK(root["redis_clients"].isArray());
    CHECK(!root["redis_clients"].empty());

    const auto &redis = root["redis_clients"][0];
    const std::string host = redis.get("host", "127.0.0.1").asString();
    const int port = redis.get("port", 6379).asInt();
    const std::string password = redis.get("passwd", "").asString();
    const unsigned int db = redis.get("db", 0).asUInt();
    const std::string username = redis.get("username", "").asString();

    trantor::InetAddress addr(host, static_cast<uint16_t>(port));
    auto client = drogon::nosql::RedisClient::newRedisClient(addr, 1, password, db, username);
    CHECK(client != nullptr);

    // Guard against hanging when redis is not reachable.
    auto pingPromise = std::make_shared<std::promise<bool>>();
    auto pingFuture = pingPromise->get_future();
    client->execCommandAsync(
      [pingPromise](const drogon::nosql::RedisResult &r) {
          try
          {
              pingPromise->set_value(r.asString() == "PONG");
          }
          catch (...)
          {
              pingPromise->set_value(false);
          }
      },
      [pingPromise](const drogon::nosql::RedisException &) { pingPromise->set_value(false); },
      "PING"
    );

    if (
      pingFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready || !pingFuture.get()
    )
    {
        return;
    }

    const std::string key = "pay:test:idemp:" + drogon::utils::getUuid();

    const auto first = client->execCommandSync<std::string>(
      [](const drogon::nosql::RedisResult &r) { return r.asString(); },
      "SET %s %s NX EX %d",
      key.c_str(),
      "1",
      60
    );
    CHECK(first == "OK");

    const auto second = client->execCommandSync<std::string>(
      [](const drogon::nosql::RedisResult &r) {
          if (r.type() == drogon::nosql::RedisResultType::kNil)
          {
              return std::string("NIL");
          }
          return r.asString();
      },
      "SET %s %s NX EX %d",
      key.c_str(),
      "1",
      60
    );
    CHECK(second == "NIL");

    client->execCommandSync<int>(
      [](const drogon::nosql::RedisResult &r) { return static_cast<int>(r.asInteger()); },
      "DEL %s",
      key.c_str()
    );
}

DROGON_TEST(PayIdempotency_OrmRoundTrip)
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

    using PayIdempotency = drogon_model::pay_test::PayIdempotency;
    drogon::orm::Mapper<PayIdempotency> mapper(client);

    const std::string key = "orm_" + drogon::utils::getUuid();
    PayIdempotency row;
    row.setIdempotencyKey(key);
    row.setRequestHash("hash");
    row.setResponseSnapshot("{\"ok\":true}");
    const auto now = trantor::Date::now();
    const auto expiresAt = trantor::Date(now.microSecondsSinceEpoch() + 3600LL * 1000000LL);
    row.setExpireAt(expiresAt);

    mapper.insert(row);

    const auto fetched = mapper.findByPrimaryKey(key);
    CHECK(fetched.getValueOfIdempotencyKey() == key);
    CHECK(fetched.getValueOfRequestHash() == "hash");
    CHECK(fetched.getValueOfResponseSnapshot() == "{\"ok\":true}");

    mapper.deleteByPrimaryKey(key);
}

DROGON_TEST(PayCallback_OrmRoundTrip)
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

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    const std::string orderNo = "order_" + drogon::utils::getUuid();
    PayOrder orderRow;
    orderRow.setOrderNo(orderNo);
    orderRow.setUserId(12345);
    orderRow.setAmount("10.00");
    orderRow.setCurrency("CNY");
    orderRow.setStatus("SUCCESS");
    orderRow.setChannel("WECHAT");
    orderRow.setTitle("Test order");
    orderMapper.insert(orderRow);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);

    PayPayment paymentRow;
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    paymentRow.setPaymentNo(paymentNo);
    paymentRow.setOrderNo(orderNo);
    paymentRow.setStatus("SUCCESS");
    paymentRow.setAmount("10.00");
    paymentRow.setRequestPayload("{}");
    paymentRow.setResponsePayload("{}");

    paymentMapper.insert(paymentRow);
    CHECK(paymentRow.getValueOfId() > 0);

    using PayCallback = drogon_model::pay_test::PayCallback;
    drogon::orm::Mapper<PayCallback> mapper(client);

    PayCallback row;
    row.setPaymentNo(paymentNo);  //  使用已存在的 payment_no
    row.setRawBody("{\"resource\":{}}");
    row.setSignature("sig");
    row.setSerialNo("serial");
    row.setVerified(true);
    row.setProcessed(false);
    row.setReceivedAt(trantor::Date::now());

    mapper.insert(row);
    const auto id = row.getValueOfId();
    CHECK(id > 0);

    const auto fetched = mapper.findByPrimaryKey(id);
    CHECK(fetched.getValueOfPaymentNo() == row.getValueOfPaymentNo());
    CHECK(fetched.getValueOfRawBody() == row.getValueOfRawBody());
    CHECK(fetched.getValueOfSignature() == row.getValueOfSignature());
    CHECK(fetched.getValueOfSerialNo() == row.getValueOfSerialNo());
    CHECK(fetched.getValueOfVerified() == row.getValueOfVerified());
    CHECK(fetched.getValueOfProcessed() == row.getValueOfProcessed());

    mapper.deleteByPrimaryKey(id);
    paymentMapper.deleteByPrimaryKey(paymentRow.getValueOfId());
    orderMapper.deleteByPrimaryKey(orderRow.getPrimaryKey());
}

DROGON_TEST(PayPayment_OrmRoundTrip)
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
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "payment_no VARCHAR(64) UNIQUE NOT NULL,"
      "order_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> mapper(client);

    const auto orderNo = "order_" + drogon::utils::getUuid();
    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);

    PayOrder orderRow;
    orderRow.setOrderNo(orderNo);
    orderRow.setUserId(12345);
    orderRow.setAmount("12.34");
    orderRow.setCurrency("CNY");
    orderRow.setStatus("SUCCESS");
    orderRow.setChannel("WECHAT");
    orderRow.setTitle("Test order");

    orderMapper.insert(orderRow);
    const auto paymentNo = "pay_" + drogon::utils::getUuid();

    PayPayment row;
    row.setOrderNo(orderNo);
    row.setPaymentNo(paymentNo);
    row.setChannelTradeNo("wx_" + drogon::utils::getUuid());
    row.setStatus("SUCCESS");
    row.setAmount("12.34");
    row.setRequestPayload("{\"request\":true}");
    row.setResponsePayload("{\"response\":true}");

    mapper.insert(row);
    const auto id = row.getValueOfId();
    CHECK(id > 0);

    const auto fetched = mapper.findByPrimaryKey(id);
    CHECK(fetched.getValueOfOrderNo() == orderNo);
    CHECK(fetched.getValueOfPaymentNo() == paymentNo);
    CHECK(fetched.getValueOfChannelTradeNo() == row.getValueOfChannelTradeNo());
    CHECK(fetched.getValueOfStatus() == "SUCCESS");
    CHECK(fetched.getValueOfAmount() == "12.34");
    CHECK(fetched.getValueOfRequestPayload() == row.getValueOfRequestPayload());
    CHECK(fetched.getValueOfResponsePayload() == row.getValueOfResponsePayload());

    mapper.deleteByPrimaryKey(id);
}

DROGON_TEST(PayRefund_OrmRoundTrip)
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

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> mapper(client);

    const auto refundNo = "refund_" + drogon::utils::getUuid();
    const auto orderNo = "order_" + drogon::utils::getUuid();
    const auto paymentNo = "pay_" + drogon::utils::getUuid();

    PayRefund row;

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    drogon::orm::Mapper<PayPayment> paymentMapper(client);

    PayOrder orderRow;
    orderRow.setOrderNo(orderNo);
    orderRow.setUserId(67890);
    orderRow.setAmount("5.67");
    orderRow.setCurrency("CNY");
    orderRow.setStatus("SUCCESS");
    orderRow.setChannel("WECHAT");
    orderRow.setTitle("Refund test order");

    orderMapper.insert(orderRow);

    PayPayment paymentRow;
    paymentRow.setOrderNo(orderNo);
    paymentRow.setPaymentNo(paymentNo);
    paymentRow.setStatus("SUCCESS");
    paymentRow.setAmount("5.67");
    paymentRow.setRequestPayload("{\"request\":true}");
    paymentRow.setResponsePayload("{\"response\":true}");

    paymentMapper.insert(paymentRow);
    row.setRefundNo(refundNo);
    row.setOrderNo(orderNo);
    row.setPaymentNo(paymentNo);
    row.setChannelRefundNo("wxr_" + drogon::utils::getUuid());
    row.setStatus("SUCCESS");
    row.setAmount("5.67");

    mapper.insert(row);
    const auto id = row.getValueOfId();
    CHECK(id > 0);

    const auto fetched = mapper.findByPrimaryKey(id);
    CHECK(fetched.getValueOfRefundNo() == refundNo);
    CHECK(fetched.getValueOfOrderNo() == orderNo);
    CHECK(fetched.getValueOfPaymentNo() == paymentNo);
    CHECK(fetched.getValueOfChannelRefundNo() == row.getValueOfChannelRefundNo());
    CHECK(fetched.getValueOfStatus() == "SUCCESS");
    CHECK(fetched.getValueOfAmount() == "5.67");

    mapper.deleteByPrimaryKey(id);
}

DROGON_TEST(PayOrder_OrmRoundTrip)
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

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> mapper(client);

    const auto orderNo = "order_" + drogon::utils::getUuid();
    const int64_t userId = 12345;
    const auto expireAt = trantor::Date::now().after(3600.0);

    PayOrder row;
    row.setOrderNo(orderNo);
    row.setUserId(userId);
    row.setAmount("9.99");
    row.setCurrency("CNY");
    row.setStatus("CREATED");
    row.setChannel("WECHAT");
    row.setTitle("Test Order");
    row.setExpireAt(expireAt);

    mapper.insert(row);
    const auto id = row.getValueOfId();
    CHECK(id > 0);

    const auto fetched = mapper.findByPrimaryKey(id);
    CHECK(fetched.getValueOfOrderNo() == orderNo);
    CHECK(fetched.getValueOfUserId() == userId);
    CHECK(fetched.getValueOfAmount() == "9.99");
    CHECK(fetched.getValueOfCurrency() == "CNY");
    CHECK(fetched.getValueOfStatus() == "CREATED");
    CHECK(fetched.getValueOfChannel() == "WECHAT");
    CHECK(fetched.getValueOfTitle() == "Test Order");

    mapper.deleteByPrimaryKey(id);
}

DROGON_TEST(PayLedger_OrmRoundTrip)
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

    using PayLedger = drogon_model::pay_test::PayLedger;
    drogon::orm::Mapper<PayLedger> mapper(client);

    const int64_t userId = 67890;
    const auto orderNo = "order_" + drogon::utils::getUuid();
    const auto paymentNo = "pay_" + drogon::utils::getUuid();

    // Insert referencing order and payment to satisfy DB foreign keys
    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder orderRow;
    orderRow.setOrderNo(orderNo);
    orderRow.setUserId(userId);
    orderRow.setAmount("3.21");
    orderRow.setCurrency("CNY");
    orderRow.setStatus("CREATED");
    orderRow.setChannel("WECHAT");
    orderRow.setTitle("Test order for ledger");
    orderMapper.insert(orderRow);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment paymentRow;
    paymentRow.setOrderNo(orderNo);
    paymentRow.setPaymentNo(paymentNo);
    paymentRow.setStatus("SUCCESS");
    paymentRow.setAmount("3.21");
    paymentMapper.insert(paymentRow);

    PayLedger row;
    row.setUserId(userId);
    row.setOrderNo(orderNo);
    row.setPaymentNo(paymentNo);
    row.setEntryType("DEBIT");
    row.setAmount("3.21");
    row.setBalance("100.00");

    mapper.insert(row);
    const auto id = row.getValueOfId();
    CHECK(id > 0);

    const auto fetched = mapper.findByPrimaryKey(id);
    CHECK(fetched.getValueOfUserId() == userId);
    CHECK(fetched.getValueOfOrderNo() == orderNo);
    CHECK(fetched.getValueOfPaymentNo() == paymentNo);
    CHECK(fetched.getValueOfEntryType() == "DEBIT");
    CHECK(fetched.getValueOfAmount() == "3.21");
    CHECK(fetched.getValueOfBalance() == "100.00");

    mapper.deleteByPrimaryKey(id);
    paymentMapper.deleteByPrimaryKey(paymentRow.getValueOfId());
    orderMapper.deleteByPrimaryKey(orderRow.getValueOfId());
}

DROGON_TEST(PayIdempotency_ClearReservationReleasesKey)
{
    // Proves the P2 fix: when an operation fails after reserving an idempotency
    // key, clearReservation() releases the in-flight row so the next retry with
    // the same key+hash re-reserves (Started) instead of being reported as
    // InProgress (key poisoning for up to the TTL).
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

    IdempotencyService svc(client, nullptr, 60);

    const std::string key = "clr_" + drogon::utils::getUuid();
    const std::string hash = "h_" + drogon::utils::getUuid();
    Json::Value req;
    req["amount"] = "1.00";

    auto check = [&](const std::string &k, const std::string &h) {
        auto p = std::make_shared<std::promise<IdempotencyService::CheckResult>>();
        auto f = p->get_future();
        svc.checkAndSetStatus(k, h, req, [p](const IdempotencyService::CheckResult &r) {
            p->set_value(r);
        });
        // tryFutureGet() returns false (and leaves `r` default-constructed) on
        // timeout instead of blocking forever; the caller's CHECK on `.status`
        // then fails loudly. REQUIRE/RETURN_ON_FAILURE cannot be used inside a
        // value-returning lambda because its `return;` would clash with
        // `return r;` (C3487).
        IdempotencyService::CheckResult r;
        if (!pay::test_util::tryFutureGet(f, r, std::chrono::seconds(5)))
        {
            FAIL("check() timed out waiting for checkAndSetStatus callback");
        }
        return r;
    };
    auto clear = [&](const std::string &k, const std::string &h) {
        auto p = std::make_shared<std::promise<bool>>();
        auto f = p->get_future();
        svc.clearReservation(k, h, [p](bool ok) { p->set_value(ok); });
        bool ok = false;
        if (!pay::test_util::tryFutureGet(f, ok, std::chrono::seconds(5)))
        {
            FAIL("clear() timed out waiting for clearReservation callback");
        }
        return ok;
    };

    // 1. First request reserves the key.
    CHECK(check(key, hash).status == IdempotencyService::CheckStatus::Started);

    // 2. Without cleanup, a retry with the same key+hash is InProgress. This is
    //    the poisoning condition the fix targets; it must hold before clearing
    //    so step 3 is a meaningful check.
    CHECK(check(key, hash).status == IdempotencyService::CheckStatus::InProgress);

    // 3. After clearReservation (the failure-path cleanup), the retry
    //    re-reserves instead of staying poisoned.
    CHECK(clear(key, hash));
    CHECK(check(key, hash).status == IdempotencyService::CheckStatus::Started);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", key);
}
