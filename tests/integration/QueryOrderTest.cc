#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "drogon_pay/PayPlugin.h"
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
