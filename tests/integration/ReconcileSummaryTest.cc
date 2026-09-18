#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "models/PayRefund.h"
#include "drogon_pay/PayPlugin.h"
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

DROGON_TEST(PayPlugin_ReconcileSummary)
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

    const auto baseOrderRows =
      client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_order WHERE status = $1", "PAYING");
    const int64_t basePaying =
      baseOrderRows.empty() ? 0 : baseOrderRows.front()["cnt"].as<int64_t>();

    const auto baseRefundRows = client->execSqlSync(
      "SELECT COUNT(*) AS cnt FROM pay_refund WHERE status IN ($1, $2)", "REFUND_INIT", "REFUNDING"
    );
    const int64_t baseRefunding =
      baseRefundRows.empty() ? 0 : baseRefundRows.front()["cnt"].as<int64_t>();

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    auto makeOrder = [&](const std::string &status) {
        PayOrder order;
        order.setOrderNo("ord_" + drogon::utils::getUuid());
        order.setUserId(10001);
        order.setAmount("9.99");
        order.setCurrency("CNY");
        order.setStatus(status);
        order.setChannel("wechat");
        order.setTitle("Test Order");
        order.setCreatedAt(trantor::Date::now());
        order.setUpdatedAt(trantor::Date::now());
        orderMapper.insert(order);
        return order;
    };

    auto paying1 = makeOrder("PAYING");
    auto paying2 = makeOrder("PAYING");
    auto paid = makeOrder("PAID");

    using PayRefund = drogon_model::pay_test::PayRefund;
    drogon::orm::Mapper<PayRefund> refundMapper(client);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    auto makePayment = [&](const std::string &orderNo, const std::string &status) {
        PayPayment payment;
        payment.setPaymentNo("pay_" + drogon::utils::getUuid());
        payment.setOrderNo(orderNo);
        payment.setStatus(status);
        payment.setAmount("9.99");
        payment.setCreatedAt(trantor::Date::now());
        payment.setUpdatedAt(trantor::Date::now());
        paymentMapper.insert(payment);
        return payment;
    };

    auto makeRefund =
      [&](const std::string &status, const std::string &orderNo, const std::string &paymentNo) {
          PayRefund refund;
          refund.setRefundNo("refund_" + drogon::utils::getUuid());
          refund.setOrderNo(orderNo);
          refund.setPaymentNo(paymentNo);
          refund.setStatus(status);
          refund.setAmount("9.99");
          refund.setCreatedAt(trantor::Date::now());
          refund.setUpdatedAt(trantor::Date::now());
          refundMapper.insert(refund);
          return refund;
      };

    auto payForPaying1 = makePayment(paying1.getValueOfOrderNo(), "SUCCESS");
    auto payForPaying2 = makePayment(paying2.getValueOfOrderNo(), "SUCCESS");
    auto payForPaid = makePayment(paid.getValueOfOrderNo(), "SUCCESS");

    auto refundInit =
      makeRefund("REFUND_INIT", paying1.getValueOfOrderNo(), payForPaying1.getValueOfPaymentNo());
    auto refunding =
      makeRefund("REFUNDING", paying2.getValueOfOrderNo(), payForPaying2.getValueOfPaymentNo());
    auto refundDone =
      makeRefund("REFUND_SUCCESS", paid.getValueOfOrderNo(), payForPaid.getValueOfPaymentNo());

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // Use today's date for the reconciliation summary
    auto now = trantor::Date::now();
    std::string date = now.toCustomFormattedString("%Y-%m-%d", false);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->reconcileSummary(
      date,
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
    const auto data = result["data"];
    CHECK(data["paying_orders"].asInt() == basePaying + 2);
    CHECK(data["refunding_refunds"].asInt() == baseRefunding + 2);

    client
      ->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundInit.getValueOfRefundNo());
    client
      ->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refunding.getValueOfRefundNo());
    client
      ->execSqlSync("DELETE FROM pay_refund WHERE refund_no = $1", refundDone.getValueOfRefundNo());
    client->execSqlSync(
      "DELETE FROM pay_payment WHERE payment_no = $1", payForPaying1.getValueOfPaymentNo()
    );
    client->execSqlSync(
      "DELETE FROM pay_payment WHERE payment_no = $1", payForPaying2.getValueOfPaymentNo()
    );
    client->execSqlSync(
      "DELETE FROM pay_payment WHERE payment_no = $1", payForPaid.getValueOfPaymentNo()
    );
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", paying1.getValueOfOrderNo());
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", paying2.getValueOfOrderNo());
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", paid.getValueOfOrderNo());
}
