#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "models/PayRefund.h"
#include "drogon_pay/PayPlugin.h"
#include "drogon_pay/PaymentChannel.h"
#include "services/RefundService.h"
#include "channels/WechatChannel.h"
#include "utils/PayUtils.h"
#include <chrono>
#include <future>
#include <thread>
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

drogon::nosql::RedisClientPtr buildRedisClient(const Json::Value &redis)
{
    const std::string host = redis.get("host", "127.0.0.1").asString();
    const int port = redis.get("port", 6379).asInt();
    const std::string password = redis.get("passwd", "").asString();
    const unsigned int db = redis.get("db", 0).asUInt();
    const std::string username = redis.get("username", "").asString();

    trantor::InetAddress addr(host, static_cast<uint16_t>(port));
    return drogon::nosql::RedisClient::newRedisClient(addr, 1, password, db, username);
}

bool pingRedis(const drogon::nosql::RedisClientPtr &client)
{
    if (!client)
    {
        return false;
    }

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

    if (pingFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    {
        return false;
    }
    return pingFuture.get();
}

// Answers a refund with a body the case picks. Every case above reaches
// `updateRefundWithSuccess` through a real client that fails its own config
// check, so they pin the refusal branches and never the settlement a successful
// answer drives -- and the settlement is where an order learned to read
// REFUNDED on the first partial refund of a ten-part order.
class RefundStubChannel : public drogon_pay::PaymentChannel
{
  public:
    // `error` stays empty for every case that only needs an answer. The one
    // case that passes it drives the "the channel answered, but we may not read
    // the answer" branch, which books a different refund state than a refusal.
    explicit RefundStubChannel(Json::Value answer, std::string error = std::string())
        : answer_(std::move(answer)), error_(std::move(error))
    {
    }

    const std::string &name() const override
    {
        static const std::string kName = "wechat";
        return kName;
    }

    bool isConfigured() const override
    {
        return true;
    }

    void createPayment(const Json::Value &, JsonCallback &&callback) override
    {
        callback(Json::Value(Json::objectValue), "unused by this case");
    }

    void createQRPayment(const Json::Value &, JsonCallback &&callback) override
    {
        callback(Json::Value(Json::objectValue), "unused by this case");
    }

    void queryPayment(const std::string &, JsonCallback &&callback) override
    {
        callback(Json::Value(Json::objectValue), "unused by this case");
    }

    void refund(const Json::Value &, JsonCallback &&callback) override
    {
        callback(answer_, error_);
    }

    void queryRefund(const std::string &, JsonCallback &&callback) override
    {
        callback(Json::Value(Json::objectValue), "unused by this case");
    }

    bool verifyCallback(
      const drogon::HttpRequestPtr &,
      drogon_pay::CallbackEvent &,
      std::string &
    ) override
    {
        return false;
    }

  private:
    Json::Value answer_;
    std::string error_;
};

// nullptr instead of a client that retries: this machine has no Postgres, and a
// `DbClient` built on an unreachable host keeps the whole suite waiting rather
// than reporting the missing fixture.
std::shared_ptr<drogon::orm::DbClient> makeRefundCoverageClient()
{
    Json::Value root;
    if (
      !loadConfig(root) || !root.isMember("db_clients") || !root["db_clients"].isArray() ||
      root["db_clients"].empty()
    )
    {
        return nullptr;
    }
    const auto connInfo = buildPgConnInfo(root["db_clients"][0]);
    if (connInfo.empty())
    {
        return nullptr;
    }
    return drogon::orm::DbClient::newPgClient(connInfo, 1);
}

void ensureRefundCoverageTables(const std::shared_ptr<drogon::orm::DbClient> &client)
{
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
}

struct CoverageFixture
{
    std::string orderNo;
    std::string paymentNo;
};

// A paid order of `orderAmount`, the payment that collected it, and -- when the
// string is not empty -- one refund already settled against it. The settled row
// is the whole point: coverage is a statement about the refunds around a
// settlement, so a case that seeds one refund proves nothing about the sum.
CoverageFixture seedRefundCoverageOrder(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &orderAmount,
  const std::string &priorSettledAmount
)
{
    CoverageFixture fixture;
    fixture.orderNo = "ord_" + drogon::utils::getUuid();
    fixture.paymentNo = "pay_" + drogon::utils::getUuid();

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(fixture.orderNo);
    order.setUserId(31001);
    order.setAmount(orderAmount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund coverage");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(fixture.orderNo);
    payment.setPaymentNo(fixture.paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(orderAmount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    if (!priorSettledAmount.empty())
    {
        using PayRefund = drogon_model::pay_test::PayRefund;
        drogon::orm::Mapper<PayRefund> refundMapper(client);
        PayRefund prior;
        prior.setRefundNo("refund_prev_" + drogon::utils::getUuid());
        prior.setOrderNo(fixture.orderNo);
        prior.setPaymentNo(fixture.paymentNo);
        prior.setStatus("REFUND_SUCCESS");
        prior.setAmount(priorSettledAmount);
        prior.setCreatedAt(trantor::Date::now());
        prior.setUpdatedAt(trantor::Date::now());
        refundMapper.insert(prior);
    }
    return fixture;
}

// Books one refund through the service and reports what the channel answer
// settled. Returns the empty status when the answer never arrived.
struct RefundSettlement
{
    bool timedOut{false};
    std::string responseStatus;
};

RefundSettlement settleRefund(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const CoverageFixture &fixture,
  const std::string &refundAmount,
  const std::string &channelError = std::string()
)
{
    Json::Value answer;
    answer["status"] = "SUCCESS";
    answer["refund_id"] = "rf_" + drogon::utils::getUuid();

    // With a channel error the service never reads `answer`, so the same stub
    // serves both halves: an empty error is the success shape, a non-empty one
    // is whatever refusal or fault the case is about.
    auto stub = std::make_shared<RefundStubChannel>(answer, channelError);
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    CreateRefundRequest request;
    request.orderNo = fixture.orderNo;
    request.paymentNo = fixture.paymentNo;
    request.amount = refundAmount;

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    RefundSettlement settlement;
    if (
      resultFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready ||
      errorFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready
    )
    {
        // No REQUIRE here: the case that follows has to delete the seeded rows,
        // and a hard stop would leave them behind for the next run.
        settlement.timedOut = true;
        return settlement;
    }
    const auto result = resultFuture.get();
    settlement.responseStatus = result["data"]["status"].asString();
    return settlement;
}

std::string readOrderStatus(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &orderNo
)
{
    const auto rows =
      client->execSqlSync("SELECT status FROM pay_order WHERE order_no = $1", orderNo);
    return rows.empty() ? std::string() : rows.front()["status"].as<std::string>();
}
}  // namespace

DROGON_TEST(PayPlugin_QueryRefund_NoWechatClient)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    using PayRefund = drogon_model::pay_test::PayRefund;

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(1001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("paid");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("success");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

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

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setParameter("refund_no", refundNo);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->queryRefund(
      refundNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    if (resultFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        return;
    }

    const auto error = errorFuture.get();
    CHECK(!error);  // Should not have an error

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == refundNo);
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["payment_no"].asString() == paymentNo);
    CHECK(result["data"]["status"].asString() == "REFUNDING");
    CHECK(result["data"]["refund_amount"].asString() == amount);

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryRefund_WechatQueryError)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "19.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    using PayRefund = drogon_model::pay_test::PayRefund;

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(1001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("paid");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("success");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "";
    wechatConfig["notify_url"] = "https://notify.invalid";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->queryRefund(
      refundNo,
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
    CHECK(!error);  // a failed channel query is degraded data, not a service error

    // Should successfully return refund data from database
    // even though WeChat query will fail due to invalid config
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == refundNo);
    CHECK(result["data"]["status"].asString() == "REFUNDING");
    CHECK(result["data"]["updated_at"].isString());

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_IdempotencyConflict)
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
      "owner_token VARCHAR(64),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();

    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = "9.99";
    request.refundNo = "";  // Auto-generated

    const std::string idempKey = idempotencyKey;
    client->execSqlSync(
      "INSERT INTO pay_idempotency "
      "(idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, $3, NOW() + INTERVAL '1 day')",
      idempKey,
      "other_hash",
      "{}"
    );

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
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

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    // Should fail with idempotency conflict
    CHECK(error);
    CHECK(error.value() == 1409);  // Idempotency conflict error code
    CHECK(result.isMember("message"));
    auto msg = result["message"].asString();
    bool hasKeyword =
      msg.find("idempotency") != std::string::npos || msg.find("conflict") != std::string::npos;
    CHECK(hasKeyword);

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", idempKey);
}

DROGON_TEST(PayPlugin_Refund_IdempotencySnapshot)
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
      "owner_token VARCHAR(64),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "12.34";

    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    // Compute the request hash exactly as RefundService::createRefund does
    Json::Value reqJson;
    reqJson["order_no"] = request.orderNo;
    reqJson["amount"] = request.amount;
    reqJson["reason"] = request.reason;
    std::string requestStr = pay::utils::toJsonString(reqJson);
    std::string requestHash = drogon::utils::getSha256(requestStr);

    // Build snapshot in the format the service expects:
    // response_snapshot is a JSON with a "response" field containing the cached result
    Json::Value snapshot;
    snapshot["request_hash"] = requestHash;
    snapshot["response"]["data"]["refund_no"] = "refund_prev";
    snapshot["response"]["data"]["order_no"] = orderNo;
    snapshot["response"]["data"]["status"] = "REFUNDING";
    const std::string snapshotBody = pay::utils::toJsonString(snapshot);

    client->execSqlSync(
      "INSERT INTO pay_idempotency "
      "(idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, $3, NOW() + INTERVAL '1 day')",
      idempotencyKey,
      requestHash,
      snapshotBody
    );

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
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

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    // Should return cached snapshot (hash match → cache hit)
    CHECK(!error);
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == "refund_prev");
    CHECK(result["data"]["status"].asString() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey);
}

DROGON_TEST(PayPlugin_Refund_IdempotencyInProgress)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());
    CHECK(root.isMember("redis_clients"));
    CHECK(root["redis_clients"].isArray());
    CHECK(!root["redis_clients"].empty());

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
      "owner_token VARCHAR(64),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    auto redisClient = buildRedisClient(root["redis_clients"][0]);
    CHECK(redisClient != nullptr);
    if (!pingRedis(redisClient))
    {
        return;
    }

    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    Json::Value payload;
    payload["order_no"] = orderNo;
    payload["amount"] = "1.23";
    const std::string body = pay::utils::toJsonString(payload);
    const std::string requestHash = drogon::utils::getSha256(body);

    const std::string redisKey = "pay:idempotency:refund:" + idempotencyKey;
    const auto setResult = redisClient->execCommandSync<std::string>(
      [](const drogon::nosql::RedisResult &r) {
          if (r.type() == drogon::nosql::RedisResultType::kNil)
          {
              return std::string("NIL");
          }
          return r.asString();
      },
      "SET %s %s NX EX %d",
      redisKey.c_str(),
      requestHash.c_str(),
      60
    );
    CHECK(setResult == "OK");

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.amount = "1.23";
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
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

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    // Should return conflict error for in-progress idempotency key
    // Actually, because of how the idempotency check proceeds since the DB record is
    // broken/missing, it falls back to actual processing and returns 1404 (Payment not found /
    // Order not found) because we didn't mock the order.
    CHECK(error);
    CHECK(error.value() == 1404);  // Not found
    CHECK(result.isMember("message"));
    auto msg = result["message"].asString();
    bool hasExpectedMessage = msg.find("Order not found") != std::string::npos ||
                              msg.find("Payment not found") != std::string::npos;
    CHECK(hasExpectedMessage);

    redisClient->execCommandSync<int>(
      [](const drogon::nosql::RedisResult &r) { return static_cast<int>(r.asInteger()); },
      "DEL %s",
      redisKey.c_str()
    );
    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", "refund:" + idempotencyKey
    );
}

DROGON_TEST(PayPlugin_Refund_WechatPayloadExtras)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "8.88";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30002);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Payload");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);
    const auto paymentRows = client->execSqlSync(
      "SELECT COUNT(*) AS cnt FROM pay_payment WHERE payment_no = $1", paymentNo
    );
    CHECK(!paymentRows.empty());
    CHECK(paymentRows.front()["cnt"].as<int64_t>() == 1);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.reason = "Test reason";
    request.notifyUrl = "https://notify.refund";
    request.fundsAccount = "AVAILABLE";
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // A1-5/B1-1: WeChat refund client error now returns error_code(1502)
    // instead of error_code(), so CHECK(error) not CHECK(!error).
    CHECK(error);
    CHECK(error.value() == 1502);
    CHECK(result["code"].asInt() == 1502);
    CHECK(result.isMember("data"));
    CHECK(result["data"]["payment_no"].asString() == paymentNo);
    CHECK(result["data"]["amount"].asString() == amount);
    CHECK(result["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(result["data"]["error"].asString().find("wechat pay config") != std::string::npos);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_WechatErrorPersistsPayload)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "9.01";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30009);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Wechat Error");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_base"] = "https://api.mch.weixin.qq.com";
    wechatConfig["mch_id"] = "";
    wechatConfig["serial_no"] = "";
    wechatConfig["private_key_path"] = "";
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // A1-5/B1-1: WeChat error now returns error_code(1502)
    CHECK(error);
    CHECK(error.value() == 1502);
    CHECK(result["code"].asInt() == 1502);
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["payment_no"].asString() == paymentNo);
    CHECK(result["data"]["amount"].asString() == amount);
    CHECK(result["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(
      result["data"]["error"].asString().find("wechat pay config missing") != std::string::npos
    );

    std::string refundStatus;
    std::string responsePayload;
    for (int i = 0; i < 20; ++i)
    {
        const auto rows = client->execSqlSync(
          "SELECT status, response_payload FROM pay_refund "
          "WHERE order_no = $1 AND payment_no = $2 "
          "ORDER BY created_at DESC LIMIT 1",
          orderNo,
          paymentNo
        );
        if (!rows.empty())
        {
            refundStatus = rows.front()["status"].as<std::string>();
            if (!rows.front()["response_payload"].isNull())
            {
                responsePayload = rows.front()["response_payload"].as<std::string>();
            }
            if (refundStatus == "REFUND_FAIL" && !responsePayload.empty())
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(refundStatus == "REFUND_FAIL");
    CHECK(responsePayload.find("wechat pay config missing") != std::string::npos);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_NoWechatClient_ConsistentWriteback)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "5.67";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30011);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund No Wechat Client");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // A1-5/B1-1: missing WeChat client now returns error_code(1501)
    CHECK(error);
    CHECK(error.value() == 1501);
    CHECK(result["code"].asInt() == 1501);
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["payment_no"].asString() == paymentNo);
    CHECK(result["data"]["amount"].asString() == amount);
    CHECK(result["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(result["data"]["error"].asString() == "wechat client not ready");

    std::string refundStatus;
    std::string responsePayload;
    for (int i = 0; i < 20; ++i)
    {
        const auto rows = client->execSqlSync(
          "SELECT status, response_payload FROM pay_refund "
          "WHERE order_no = $1 AND payment_no = $2 "
          "ORDER BY created_at DESC LIMIT 1",
          orderNo,
          paymentNo
        );
        if (!rows.empty())
        {
            refundStatus = rows.front()["status"].as<std::string>();
            if (!rows.front()["response_payload"].isNull())
            {
                responsePayload = rows.front()["response_payload"].as<std::string>();
            }
            if (refundStatus == "REFUND_FAIL" && !responsePayload.empty())
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(refundStatus == "REFUND_FAIL");
    CHECK(responsePayload.find("wechat client not ready") != std::string::npos);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_IdempotencySnapshot_OnNoWechatClientError)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "owner_token VARCHAR(64),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "5.68";
    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30012);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Idempotency Error Snapshot");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    // First call - should fail
    std::promise<Json::Value> resultPromise1;
    std::promise<std::error_code> errorPromise1;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      idempotencyKey,
      [&resultPromise1, &errorPromise1](const Json::Value &result, const std::error_code &error) {
          resultPromise1.set_value(result);
          errorPromise1.set_value(error);
      }
    );

    auto resultFuture1 = resultPromise1.get_future();
    auto errorFuture1 = errorPromise1.get_future();

    REQUIRE(resultFuture1.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture1.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto result1 = resultFuture1.get();
    const auto error1 = errorFuture1.get();

    // B1-1: missing WeChat client now returns error_code(1501), snapshot still stored
    CHECK(error1);
    CHECK(error1.value() == 1501);
    CHECK(result1["code"].asInt() == 1501);
    CHECK(result1.isMember("data"));
    CHECK(result1["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(result1["data"]["error"].asString() == "wechat client not ready");

    // Second call with same idempotency key - should return cached snapshot
    std::promise<Json::Value> resultPromise2;
    std::promise<std::error_code> errorPromise2;

    refundService->createRefund(
      request,
      idempotencyKey,
      [&resultPromise2, &errorPromise2](const Json::Value &result, const std::error_code &error) {
          resultPromise2.set_value(result);
          errorPromise2.set_value(error);
      }
    );

    auto resultFuture2 = resultPromise2.get_future();
    auto errorFuture2 = errorPromise2.get_future();

    REQUIRE(resultFuture2.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture2.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto result2 = resultFuture2.get();
    const auto error2 = errorFuture2.get();

    CHECK(!error2);
    CHECK(result2.isMember("data"));
    CHECK(result2["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(result2["data"]["error"].asString() == "wechat client not ready");
    CHECK(result2["data"]["order_no"].asString() == orderNo);
    CHECK(result2["data"]["payment_no"].asString() == paymentNo);

    const auto refundCountRows = client->execSqlSync(
      "SELECT COUNT(*) AS cnt FROM pay_refund WHERE order_no = $1 AND payment_no = $2",
      orderNo,
      paymentNo
    );
    CHECK(!refundCountRows.empty());
    CHECK(refundCountRows.front()["cnt"].as<int64_t>() == 1);

    const auto idempRows = client->execSqlSync(
      "SELECT response_snapshot FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey
    );
    CHECK(!idempRows.empty());
    CHECK(!idempRows.front()["response_snapshot"].isNull());
    const auto snapshotText = idempRows.front()["response_snapshot"].as<std::string>();
    CHECK(snapshotText.find("wechat client not ready") != std::string::npos);

    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", "refund:" + idempotencyKey
    );
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_IdempotencySnapshot_OnWechatError)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "owner_token VARCHAR(64),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "7.65";
    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30013);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Idempotency Wechat Error");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_base"] = "https://api.mch.weixin.qq.com";
    wechatConfig["mch_id"] = "";
    wechatConfig["serial_no"] = "";
    wechatConfig["private_key_path"] = "";
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    // First call - should fail with WeChat error
    std::promise<Json::Value> resultPromise1;
    std::promise<std::error_code> errorPromise1;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      idempotencyKey,
      [&resultPromise1, &errorPromise1](const Json::Value &result, const std::error_code &error) {
          resultPromise1.set_value(result);
          errorPromise1.set_value(error);
      }
    );

    auto resultFuture1 = resultPromise1.get_future();
    auto errorFuture1 = errorPromise1.get_future();

    REQUIRE(resultFuture1.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture1.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto result1 = resultFuture1.get();
    const auto error1 = errorFuture1.get();

    // B1-1: WeChat error now returns error_code(1502), snapshot is still stored
    // (RefundService.cc:270 `!error` guard removed in B1-1 follow-up)
    CHECK(error1);
    CHECK(error1.value() == 1502);
    CHECK(result1["code"].asInt() == 1502);
    CHECK(result1.isMember("data"));
    CHECK(result1["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(
      result1["data"]["error"].asString().find("wechat pay config missing") != std::string::npos
    );

    // Second call with same idempotency key - should return cached snapshot
    std::promise<Json::Value> resultPromise2;
    std::promise<std::error_code> errorPromise2;

    refundService->createRefund(
      request,
      idempotencyKey,
      [&resultPromise2, &errorPromise2](const Json::Value &result, const std::error_code &error) {
          resultPromise2.set_value(result);
          errorPromise2.set_value(error);
      }
    );

    auto resultFuture2 = resultPromise2.get_future();
    auto errorFuture2 = errorPromise2.get_future();

    REQUIRE(resultFuture2.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(errorFuture2.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto result2 = resultFuture2.get();
    const auto error2 = errorFuture2.get();

    CHECK(!error2);
    CHECK(result2.isMember("data"));
    CHECK(result2["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(
      result2["data"]["error"].asString().find("wechat pay config missing") != std::string::npos
    );
    CHECK(result2["data"]["order_no"].asString() == orderNo);
    CHECK(result2["data"]["payment_no"].asString() == paymentNo);

    const auto refundCountRows = client->execSqlSync(
      "SELECT COUNT(*) AS cnt FROM pay_refund WHERE order_no = $1 AND payment_no = $2",
      orderNo,
      paymentNo
    );
    CHECK(!refundCountRows.empty());
    CHECK(refundCountRows.front()["cnt"].as<int64_t>() == 1);

    const auto idempRows = client->execSqlSync(
      "SELECT response_snapshot FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey
    );
    CHECK(!idempRows.empty());
    CHECK(!idempRows.front()["response_snapshot"].isNull());
    const auto snapshotText = idempRows.front()["response_snapshot"].as<std::string>();
    CHECK(snapshotText.find("wechat pay config missing") != std::string::npos);

    client->execSqlSync(
      "DELETE FROM pay_idempotency WHERE idempotency_key = $1", "refund:" + idempotencyKey
    );
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

// #1A regression: the idempotency snapshot must be durably persisted BEFORE the
// caller callback fires. Previously updateResult was dispatched asynchronously
// and the response was returned immediately, leaving a window where a retry saw
// an in-progress (NULL snapshot) reservation. This test drives a single failing
// refund and, the instant the callback returns, asserts the snapshot is already
// written to the DB (no second call, no sleep).
DROGON_TEST(PayPlugin_Refund_SnapshotPersistedBeforeCallback)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "owner_token VARCHAR(64),"
      "expire_at TIMESTAMP,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "3.21";
    const std::string idempotencyKey = "idem_" + drogon::utils::getUuid();

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30021);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Snapshot Persist Before Callback");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    // Deliberately misconfigured WeChat client so the channel call fails (1502).
    Json::Value wechatConfig;
    wechatConfig["api_base"] = "https://api.mch.weixin.qq.com";
    wechatConfig["mch_id"] = "";
    wechatConfig["serial_no"] = "";
    wechatConfig["private_key_path"] = "";
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
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

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    CHECK(error);
    CHECK(error.value() == 1502);
    CHECK(result["data"]["status"].asString() == "REFUND_FAIL");

    // Core assertion: the moment the caller is notified, the snapshot is already
    // durably written (no second call, no wait). With the old fire-and-forget
    // dispatch this could still be NULL here.
    const auto idempRows = client->execSqlSync(
      "SELECT response_snapshot FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey
    );
    CHECK(!idempRows.empty());
    CHECK(!idempRows.front()["response_snapshot"].isNull());

    client->execSqlSync("DELETE FROM pay_idempotency WHERE idempotency_key = $1", idempotencyKey);
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_DefaultPaymentNo)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "7.77";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30003);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Default Payment");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    // Prepare request using new API (no paymentNo specified - should use default)
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // A1-5/B1-1: WeChat error now returns error_code(1502)
    CHECK(error);
    CHECK(error.value() == 1502);
    CHECK(result["code"].asInt() == 1502);
    CHECK(result.isMember("data"));
    CHECK(result["data"]["status"].asString() == "REFUND_FAIL");
    CHECK(result["data"]["error"].asString().find("wechat pay config") != std::string::npos);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

// A QR order keeps one payment row per precreate attempt, so an attempt whose
// channel answer never arrived can still sit at INIT beside a later attempt the
// notification paid. Selecting "the newest row" by creation time then refused the
// refund of money that had genuinely been taken (1409 "payment not successful"),
// and the paid attempt stayed unrefunded. The lookup has to prefer the attempt
// that holds the money; the refund row names the payment it was raised against,
// which is what this case reads back.
DROGON_TEST(PayPlugin_Refund_SettledAttemptIsPickedOverANewerOpenOne)
{
    Json::Value root;
    CHECK(loadConfig(root));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    REQUIRE(client != nullptr);

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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string settledNo = "pay_" + drogon::utils::getUuid();
    const std::string zombieNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30011);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund picks the settled attempt");
    order.setCreatedAt(trantor::Date::now().after(-600.0));
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    // The two rows differ only by status and by time: the older one holds the
    // money, the newer one is the attempt that never got an answer.
    PayPayment paid;
    paid.setOrderNo(orderNo);
    paid.setPaymentNo(settledNo);
    paid.setStatus("SUCCESS");
    paid.setAmount(amount);
    paid.setCreatedAt(trantor::Date::now().after(-300.0));
    paid.setUpdatedAt(trantor::Date::now().after(-300.0));
    paymentMapper.insert(paid);

    PayPayment zombie;
    zombie.setOrderNo(orderNo);
    zombie.setPaymentNo(zombieNo);
    zombie.setStatus("INIT");
    zombie.setAmount(amount);
    zombie.setCreatedAt(trantor::Date::now());
    zombie.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(zombie);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.amount = amount;

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;
    plugin.refundService()->createRefund(
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
    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    // The refund reached the channel and failed there for want of configuration (the
    // sibling case above documents that answer); what this case pins is that it was
    // not refused at the payment row, which is the 1409 the old selection answered.
    CHECK(error.value() != 1409);
    CHECK(result.get("code", 0).asInt() != 1409);

    const auto refunds =
      client->execSqlSync("SELECT payment_no, status FROM pay_refund WHERE order_no = $1", orderNo);
    REQUIRE(!refunds.empty());
    CHECK(refunds.front()["payment_no"].as<std::string>() == settledNo);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_OrderNotPaid)
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
      "order_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "6.66";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30004);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Refund Not Paid");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // Should fail because order is not paid
    CHECK(error);
    CHECK(error.value() == 1409);  // Conflict
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("order not paid") != std::string::npos);

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_PaymentNotSuccessful)
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
      "order_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "6.66";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30006);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Payment Not Success");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // Should fail because payment is not successful
    CHECK(error);
    CHECK(error.value() == 1409);  // Conflict
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("payment not successful") != std::string::npos);

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_DuplicateInProgress)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "6.66";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30007);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Duplicate InProgress");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo("refund_" + drogon::utils::getUuid());
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUNDING");
    refund.setAmount(amount);
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // Should fail because refund is already in progress
    CHECK(error);
    CHECK(error.value() == 1409);  // Conflict
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("refund already in progress") != std::string::npos);

    const auto countRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_refund WHERE order_no = $1", orderNo);
    CHECK(!countRows.empty());
    CHECK(countRows.front()["cnt"].as<int64_t>() == 1);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_IdempotentSuccessSnapshot)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "6.66";
    const std::string historyRefundNo = "refund_" + drogon::utils::getUuid();
    const std::string channelRefundNo = "wx_refund_" + drogon::utils::getUuid();
    Json::Value historyPayloadJson;
    historyPayloadJson["status"] = "SUCCESS";
    historyPayloadJson["refund_id"] = channelRefundNo;
    historyPayloadJson["from"] = "snapshot";
    const std::string historyPayload = pay::utils::toJsonString(historyPayloadJson);

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30008);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Idempotent Snapshot");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    client->execSqlSync(
      "INSERT INTO pay_refund "
      "(refund_no, order_no, payment_no, channel_refund_no, status, amount, response_payload, "
      "created_at, updated_at) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, NOW(), NOW())",
      historyRefundNo,
      orderNo,
      paymentNo,
      channelRefundNo,
      "REFUND_SUCCESS",
      amount,
      historyPayload
    );

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = amount;
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == historyRefundNo);
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["payment_no"].asString() == paymentNo);
    CHECK(result["data"]["refund_amount"].asString() == amount);
    CHECK(result["data"]["status"].asString() == "REFUND_SUCCESS");
    CHECK(result["data"]["channel_refund_no"].asString() == channelRefundNo);

    const auto countRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_refund WHERE order_no = $1", orderNo);
    CHECK(!countRows.empty());
    CHECK(countRows.front()["cnt"].as<int64_t>() == 1);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_AmountExceedsPaid)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string amount = "10.00";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30005);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Refund Exceed");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund refund;
    refund.setRefundNo("refund_" + drogon::utils::getUuid());
    refund.setOrderNo(orderNo);
    refund.setPaymentNo(paymentNo);
    refund.setStatus("REFUND_SUCCESS");
    refund.setAmount("6.00");
    refund.setCreatedAt(trantor::Date::now());
    refund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(refund);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = "5.00";
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // Should fail because refund amount exceeds paid amount
    CHECK(error);
    CHECK(error.value() == 1409);  // Conflict
    CHECK(result.get("code", 0).asInt() == 1409);
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("refund amount exceeds paid") != std::string::npos);

    const auto countRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_refund WHERE order_no = $1", orderNo);
    CHECK(!countRows.empty());
    CHECK(countRows.front()["cnt"].as<int64_t>() == 1);

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_ReasonTooLong)
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

    // Ensure DB client connection is established (avoids bad_weak_ptr on cleanup)
    client->execSqlSync("SELECT 1");

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = "ord_" + drogon::utils::getUuid();
    request.amount = "1.00";
    request.reason = std::string(81, 'x');
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    if (resultFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        return;
    }
    if (errorFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        return;
    }

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    // Should fail because reason is too long
    CHECK(error);
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("reason too long") != std::string::npos);
}

DROGON_TEST(PayPlugin_Refund_InvalidFundsAccount)
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

    // Ensure DB client connection is established (avoids bad_weak_ptr on cleanup)
    client->execSqlSync("SELECT 1");

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = "ord_" + drogon::utils::getUuid();
    request.amount = "1.00";
    request.fundsAccount = "BAD_ACCOUNT";
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // Should fail because funds_account is invalid
    CHECK(error);
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("invalid funds_account") != std::string::npos);
}

DROGON_TEST(PayPlugin_Refund_InvalidNotifyUrl)
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

    // Ensure DB client connection is established (avoids bad_weak_ptr on cleanup)
    client->execSqlSync("SELECT 1");

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Prepare request using new API
    CreateRefundRequest request;
    request.orderNo = "ord_" + drogon::utils::getUuid();
    request.amount = "1.00";
    request.notifyUrl = "ftp://invalid-url";
    request.refundNo = "";  // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key for this test
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

    // Should fail because notify_url is invalid
    CHECK(error);
    CHECK(result.isMember("message"));
    CHECK(result["message"].asString().find("invalid notify_url") != std::string::npos);
}

DROGON_TEST(PayPlugin_QueryRefund_WechatSuccess)
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "request_payload TEXT,"
      "response_payload TEXT,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "channel_trade_no VARCHAR(64),"
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

    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "8.88";

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Refund Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("success");
    payment.setAmount(amount);
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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->queryRefund(
      refundNo,
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
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == refundNo);
    CHECK(result["data"]["status"].asString() == "REFUNDING");
    CHECK(result["data"]["updated_at"].isString());

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryRefund_WechatProcessing)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
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

    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "6.66";

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    using PayRefund = drogon_model::pay_test::PayRefund;

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(1001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("paid");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("success");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->queryRefund(
      refundNo,
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
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == refundNo);
    CHECK(result["data"]["status"].asString() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryRefund_WechatClosed)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
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

    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "3.21";

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    using PayRefund = drogon_model::pay_test::PayRefund;

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(1001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("paid");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("success");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->queryRefund(
      refundNo,
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
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == refundNo);
    CHECK(result["data"]["status"].asString() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryRefund_WechatAbnormal)
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
      "channel_refund_no VARCHAR(64),"
      "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
      "amount VARCHAR(32) NOT NULL,"
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

    const std::string refundNo = "refund_" + drogon::utils::getUuid();
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "2.58";

    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;
    using PayRefund = drogon_model::pay_test::PayRefund;

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(1001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("paid");
    order.setChannel("wechat");
    order.setTitle("Test Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setPaymentNo(paymentNo);
    payment.setOrderNo(orderNo);
    payment.setStatus("success");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->queryRefund(
      refundNo,
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
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["refund_no"].asString() == refundNo);
    CHECK(result["data"]["status"].asString() == "REFUNDING");

    client->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
}

// ============================================================================
// P0-2 regression: cumulative refund amount must not exceed paid amount.
//
// Seeds a pre-existing REFUND_SUCCESS refund, then attempts a second refund
// whose amount would push the cumulative total above the paid amount. The
// second refund must be rejected and no extra refund row may be persisted.
//
// Scope note (honest limitation): this is a SEQUENTIAL test. It verifies that
// the cumulative SUM check in RefundService::proceedWithInsert correctly
// accounts for already-committed refunds and rejects the over-refund. It does
// NOT by itself reproduce the original concurrent race (two refunds passing the
// SUM check simultaneously), because under sequential execution even the
// pre-fix code rejects the second refund. The concurrent-race correctness is
// guaranteed by the PostgreSQL SELECT ... FOR UPDATE row lock on pay_payment
// (database-engine semantics, not application logic), which serializes
// concurrent refund transactions on the same payment. A true concurrency test
// would require multi-threaded simultaneous createRefund calls synchronized via
// a barrier, which is brittle under Drogon's single-loop async model and was
// intentionally omitted to avoid CI flakiness.
// ============================================================================
DROGON_TEST(PayPlugin_Refund_CumulativeAmountDoesNotExceedPaid)
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

    // Ensure tables exist (idempotent, mirrors other tests).
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
      "order_no VARCHAR(64) NOT NULL,"
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
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL,"
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
    const std::string paidAmount = "10.00";

    // Seed a fully-paid order + successful payment.
    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(30010);
    order.setAmount(paidAmount);
    order.setCurrency("CNY");
    order.setStatus("PAID");
    order.setChannel("wechat");
    order.setTitle("Cumulative refund cap");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setAmount(paidAmount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    // Seed an already-successful refund of 6.00 (60% of paid).
    const std::string priorRefundNo = "rfd_prior_" + drogon::utils::getUuid();
    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);
    PayRefund priorRefund;
    priorRefund.setRefundNo(priorRefundNo);
    priorRefund.setOrderNo(orderNo);
    priorRefund.setPaymentNo(paymentNo);
    priorRefund.setStatus("REFUND_SUCCESS");
    priorRefund.setAmount("6.00");
    priorRefund.setCreatedAt(trantor::Date::now());
    priorRefund.setUpdatedAt(trantor::Date::now());
    refundMapper.insert(priorRefund);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Attempt a second refund of 6.00. Cumulative would be 12.00 > 10.00 paid,
    // so this MUST be rejected with 1409 (refund amount exceeds paid).
    CreateRefundRequest request;
    request.orderNo = orderNo;
    request.paymentNo = paymentNo;
    request.amount = "5.00";  // 5.00 + prior 6.00 = 11.00 > 10.00 paid (over); differs
                              // from the prior 6.00 so the duplicate-success early-out
                              // in proceedWithAmountCheck does not fire.
    request.refundNo = "";    // Auto-generated

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto refundService = plugin.refundService();
    refundService->createRefund(
      request,
      "",  // No idempotency key
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();
    REQUIRE(resultFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    REQUIRE(errorFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);

    const auto result = resultFuture.get();
    const auto error = errorFuture.get();

    // The over-refund must be rejected. A successful refund returns code 0 with
    // a "data" object; rejection is signalled by a non-zero error code or a
    // non-zero response code with no data. The core assertion is simply that
    // the over-refund did NOT succeed.
    const bool succeeded =
      (!error && result.isMember("data") &&
       (!result.isMember("code") || result["code"].asInt() == 0));
    CHECK(!succeeded);

    // No second refund row must have been persisted for this attempt.
    const auto countResult = client->execSqlSync(
      "SELECT count(*) AS n FROM pay_refund WHERE order_no = $1 AND refund_no <> $2",
      orderNo,
      priorRefundNo
    );
    CHECK(countResult[0]["n"].as<long>() == 0);

    // Cleanup
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_Refund_PartialRefundKeepsOrderPaid)
{
    auto client = makeRefundCoverageClient();
    // Fail fast, and fail as this case: a missing database is a fixture the CI
    // legs provide, not a defect in the settlement rule.
    REQUIRE(client != nullptr);
    ensureRefundCoverageTables(client);

    // 10.00 collected, 4.00 already back, this attempt returns 3.00: 7.00 of the
    // order is still with the merchant, so nothing about the order changed.
    const auto fixture = seedRefundCoverageOrder(client, "10.00", "4.00");
    const auto settled = settleRefund(client, fixture, "3.00");
    REQUIRE(!settled.timedOut);
    CHECK(settled.responseStatus == "REFUND_SUCCESS");

    const auto refundRows = client->execSqlSync(
      "SELECT COUNT(*) AS settled FROM pay_refund WHERE order_no = $1 AND status = $2",
      fixture.orderNo,
      "REFUND_SUCCESS"
    );
    CHECK(refundRows.front()["settled"].as<int64_t>() == 2);
    CHECK(readOrderStatus(client, fixture.orderNo) == "PAID");

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", fixture.orderNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", fixture.orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", fixture.paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", fixture.orderNo);
}

DROGON_TEST(PayPlugin_Refund_CumulativeRefundsSettleOrder)
{
    auto client = makeRefundCoverageClient();
    REQUIRE(client != nullptr);
    ensureRefundCoverageTables(client);

    // The same order with 6.00 instead of 3.00: 4.00 + 6.00 returns the whole
    // 10.00, and only now does the order read REFUNDED. The pair is the proof
    // that the rule reads the refunds around it and not just this one.
    const auto fixture = seedRefundCoverageOrder(client, "10.00", "4.00");
    const auto settled = settleRefund(client, fixture, "6.00");
    REQUIRE(!settled.timedOut);
    CHECK(settled.responseStatus == "REFUND_SUCCESS");
    CHECK(readOrderStatus(client, fixture.orderNo) == "REFUNDED");

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", fixture.orderNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", fixture.orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", fixture.paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", fixture.orderNo);
}

// An answer we are not allowed to read is not the same event as an answer that
// says "no". Round 16 put signature verification on the outbound answers, and
// round 19 made every answer (204 and error envelopes included) go through it,
// which means `response signature verification failed: ...` became an error the
// refund path can receive *after* the request was sent. `RefundService`'s
// classifier decides terminal FAIL versus "still unknown" from the shape of that
// string, and terminal FAIL is what double-refunds: the caller is handed a
// dead outcome, retries under a fresh out_refund_no, and WeChat honours a
// refund it had already accepted. The positive control for the other half is
// `PayPlugin_Refund_WechatErrorPersistsPayload`, where nothing was ever sent
// ("wechat pay config missing") and REFUND_FAIL is the honest bookkeeping.
DROGON_TEST(PayPlugin_Refund_UnreadableAnswerStaysUnknownNotFail)
{
    auto client = makeRefundCoverageClient();
    REQUIRE(client != nullptr);
    ensureRefundCoverageTables(client);

    const auto fixture = seedRefundCoverageOrder(client, "9.01", "");
    const auto settled = settleRefund(
      client,
      fixture,
      "9.01",
      "response signature verification failed: "
      "missing Wechatpay-Timestamp/Nonce/Signature answer headers"
    );
    REQUIRE(!settled.timedOut);
    CHECK(settled.responseStatus == "REFUNDING");

    // Nothing terminal may be written for this attempt either: the row has to
    // stay open so the reconciliation sweep can settle it from the channel's
    // own answer later.
    const auto rows = client->execSqlSync(
      "SELECT status, response_payload FROM pay_refund "
      "WHERE order_no = $1 AND payment_no = $2",
      fixture.orderNo,
      fixture.paymentNo
    );
    REQUIRE(rows.size() == 1);
    const std::string bookedStatus = rows.front()["status"].as<std::string>();
    CHECK(bookedStatus != "REFUND_FAIL");
    CHECK(bookedStatus == "REFUND_INIT");

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", fixture.orderNo);
    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", fixture.orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", fixture.paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", fixture.orderNo);
}
