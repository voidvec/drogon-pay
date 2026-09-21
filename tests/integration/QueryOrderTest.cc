#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "models/PayRefund.h"
#include "drogon_pay/PayPlugin.h"
#include "drogon_pay/PaymentChannel.h"
#include "channels/WechatChannel.h"
#include "services/PaymentService.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

// Answers a channel query with a chosen body. The cases above build a real
// `WechatPayClient` that fails its own config check before it can reach the
// network, so they pin the degraded branch of `queryOrder` and never the state
// machine the answer drives -- which is how a wrong `trade_state` mapping stayed
// invisible in a file named after those very states.
class QueryStubChannel : public drogon_pay::PaymentChannel
{
  public:
    explicit QueryStubChannel(Json::Value answer) : answer_(std::move(answer))
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
        callback(answer_, std::string());
    }

    void refund(const Json::Value &, JsonCallback &&callback) override
    {
        callback(Json::Value(Json::objectValue), "unused by this case");
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
};

std::shared_ptr<drogon::orm::DbClient> makeQueryTestClient()
{
    Json::Value root;
    if (
      !loadConfig(root) || !root.isMember("db_clients") || !root["db_clients"].isArray() ||
      root["db_clients"].empty()
    )
    {
        return nullptr;
    }
    return drogon::orm::DbClient::newPgClient(buildPgConnInfo(root["db_clients"][0]), 1);
}

// `pay_ledger` is part of the assertion: a trade that collected money has to
// leave a PAYMENT entry behind wherever it is settled, and the two status
// columns alone cannot show that.
void ensureQuerySyncTables(const std::shared_ptr<drogon::orm::DbClient> &client)
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
    // The REFUND answer is now judged against the refund ledger, so the ledger
    // has to exist for the sync to read it -- with no table the SUM aborts the
    // transaction and the case proves nothing about the coverage rule.
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
}

}  // namespace

DROGON_TEST(PayPlugin_QueryOrder_NoWechatClient)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string amount = "19.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["amount"].asString() == amount);
    CHECK(result["data"]["currency"].asString() == "CNY");
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["channel"].asString() == "wechat");
    CHECK(result["data"]["title"].asString() == "Query Order");

    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatQueryError)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string amount = "29.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20002);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Error");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

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

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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

    // Should successfully return order data from database
    // even though WeChat query will fail due to invalid config
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");

    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatSuccess)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "49.90";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20003);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Success");
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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatSuccess_PaymentAlreadySuccess)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "59.90";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20005);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Paid");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setChannelTradeNo("wx_txn_prev");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "SUCCESS");
    CHECK(updatedPayment.getValueOfChannelTradeNo() == "wx_txn_prev");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatUserPaying)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "19.00";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20004);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Paying");
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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatNotPay)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.50";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20005);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Notpay");
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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatClosed)
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

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "23.00";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20006);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Closed");
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

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
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
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

namespace
{
// Books an order with one open payment attempt, lets the channel answer a query
// with `tradeState` reporting `answerTotalFen`, and reports what the rows and the
// ledger look like afterwards. When `priorSettledRefundAmount` is not empty, one
// REFUND_SUCCESS refund of that amount is seeded first: the REFUNDED a `REFUND`
// answer maps to is now judged against the refunds settled on the order, so a
// case without the ledger behind it proves the downgrade, one with it the
// upgrade.
struct SyncOutcome
{
    bool timedOut{false};
    bool transportError{false};
    std::string answerStatus;
    std::string orderStatus;
    std::string paymentStatus;
    int64_t paymentLedgerRows{0};
};

SyncOutcome settleFromChannelAnswer(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &tradeState,
  const int64_t answerTotalFen,
  const std::string &priorSettledRefundAmount = ""
)
{
    using PayOrder = drogon_model::pay_test::PayOrder;
    using PayPayment = drogon_model::pay_test::PayPayment;

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "49.90";

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20011);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Sync");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    if (!priorSettledRefundAmount.empty())
    {
        using PayRefund = drogon_model::pay_test::PayRefund;
        drogon::orm::Mapper<PayRefund> refundMapper(client);
        PayRefund prior;
        prior.setRefundNo("refund_prev_" + drogon::utils::getUuid());
        prior.setOrderNo(orderNo);
        prior.setPaymentNo(paymentNo);
        prior.setStatus("REFUND_SUCCESS");
        prior.setAmount(priorSettledRefundAmount);
        prior.setCreatedAt(trantor::Date::now());
        prior.setUpdatedAt(trantor::Date::now());
        refundMapper.insert(prior);
    }

    Json::Value answer;
    answer["trade_state"] = tradeState;
    answer["transaction_id"] = "wx_sync_test_transaction";
    answer["amount"]["total"] = static_cast<Json::Int64>(answerTotalFen);
    answer["amount"]["currency"] = "CNY";
    answer["amount"]["payer_total"] = static_cast<Json::Int64>(answerTotalFen);

    auto stub = std::make_shared<QueryStubChannel>(answer);
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    // Shared so a delivery that answers after this helper returns cannot write
    // into a destroyed promise.
    auto resultPromise = std::make_shared<std::promise<Json::Value>>();
    auto errorPromise = std::make_shared<std::promise<std::error_code>>();
    auto resultFuture = resultPromise->get_future();
    plugin.paymentService()->queryOrder(
      orderNo,
      [resultPromise, errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise->set_value(result);
          errorPromise->set_value(error);
      }
    );
    // Neither `REQUIRE` nor `CHECK` works here: both expand to the test-case
    // context, which a helper does not have. A timeout is reported through the
    // outcome so the case that called this fails with a visible reason.
    SyncOutcome outcome;
    if (resultFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        outcome.timedOut = true;
        client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
        client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
        client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
        client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
        return outcome;
    }
    const Json::Value result = resultFuture.get();
    outcome.transportError = static_cast<bool>(errorPromise->get_future().get());

    outcome.answerStatus = result["data"].get("status", "").asString();
    outcome.orderStatus = orderMapper.findByPrimaryKey(order.getValueOfId()).getValueOfStatus();
    outcome.paymentStatus =
      paymentMapper.findByPrimaryKey(payment.getValueOfId()).getValueOfStatus();
    const auto ledgerRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_ledger WHERE order_no = $1", orderNo);
    if (!ledgerRows.empty() && !ledgerRows.front()["cnt"].isNull())
    {
        outcome.paymentLedgerRows = ledgerRows.front()["cnt"].as<int64_t>();
    }

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    return outcome;
}
}  // namespace

// Audit round 11: `REFUND` is the state a trade reaches after the money arrived,
// and the mapping answered it as a failure. A dropped callback plus this query
// then recorded a collected payment as one that never happened -- payment FAIL,
// order CLOSED, no PAYMENT ledger entry -- while the channel bill shows the
// payment and its refund. The collection has to survive the sync.
// Audit round 13 narrows what survives: the query sees only the trade, never
// `pay_refund`, so a `REFUND` answer cannot prove the order came back in full.
// With no settled refund behind the claim, the order lands at `PAID` -- money
// collected, the refund unproven -- and the payment keeps its SUCCESS.
DROGON_TEST(PayPlugin_QueryOrder_WechatRefundKeepsTheCollectedPayment)
{
    auto client = makeQueryTestClient();
    REQUIRE(client != nullptr);
    ensureQuerySyncTables(client);

    const auto settled = settleFromChannelAnswer(client, "REFUND", 4990);
    CHECK(!settled.timedOut);
    CHECK(!settled.transportError);
    CHECK(settled.orderStatus == "PAID");
    CHECK(settled.paymentStatus == "SUCCESS");
    CHECK(settled.answerStatus == "PAID");
    CHECK(settled.paymentLedgerRows == 1);

    // The direction that must not move: a trade that never collected money stays
    // a failure, so the case above cannot read as "everything succeeded".
    const auto closed = settleFromChannelAnswer(client, "CLOSED", 4990);
    CHECK(!closed.timedOut);
    CHECK(!closed.transportError);
    CHECK(closed.orderStatus == "CLOSED");
    CHECK(closed.paymentStatus == "FAIL");
    CHECK(closed.paymentLedgerRows == 0);
}

// The downgrade must not be a ban: the same answer with the ledger behind it
// still books `REFUNDED`, and a claim covered only in part still lands at
// `PAID`. Without the covered leg this pair could pass by never writing
// `REFUNDED` at all.
DROGON_TEST(PayPlugin_QueryOrder_WechatRefundSettlesOrderOnlyWhenCovered)
{
    auto client = makeQueryTestClient();
    REQUIRE(client != nullptr);
    ensureQuerySyncTables(client);

    // 30.00 of a 49.90 order returned: the order was paid, not refunded.
    const auto partial = settleFromChannelAnswer(client, "REFUND", 4990, "30.00");
    CHECK(!partial.timedOut);
    CHECK(!partial.transportError);
    CHECK(partial.orderStatus == "PAID");
    CHECK(partial.paymentStatus == "SUCCESS");
    CHECK(partial.paymentLedgerRows == 1);

    // The whole 49.90 returned: the claim the ledger supports is booked as-is.
    const auto covered = settleFromChannelAnswer(client, "REFUND", 4990, "49.90");
    CHECK(!covered.timedOut);
    CHECK(!covered.transportError);
    CHECK(covered.orderStatus == "REFUNDED");
    CHECK(covered.paymentStatus == "SUCCESS");
    CHECK(covered.answerStatus == "REFUNDED");
    CHECK(covered.paymentLedgerRows == 1);
}

// The widened settle condition has to stay narrow where it matters: `REFUND`
// proves money arrived, but it still has to be the money this payment asked for,
// or nothing may be settled from it.
DROGON_TEST(PayPlugin_QueryOrder_WechatRefundWithForeignAmountSettlesNothing)
{
    auto client = makeQueryTestClient();
    REQUIRE(client != nullptr);
    ensureQuerySyncTables(client);

    const auto mismatched = settleFromChannelAnswer(client, "REFUND", 5000);
    CHECK(!mismatched.timedOut);
    CHECK(!mismatched.transportError);
    CHECK(mismatched.orderStatus == "PAYING");
    CHECK(mismatched.paymentStatus == "PROCESSING");
    // The answer carries no settled status, so the endpoint reports the database
    // one rather than a channel state nothing proved.
    CHECK(mismatched.answerStatus == "PAYING");
    CHECK(mismatched.paymentLedgerRows == 0);
}
