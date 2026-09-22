/// =============================================================================
/// @file   AlipayAmountGateTest.cc
/// @brief  Amount consistency gate in PaymentService::syncOrderStatusFromAlipay.
///
/// Alipay's notification rules make the merchant confirm that total_amount
/// equals the order amount before treating a trade as paid, so a 0.01 payment
/// must never settle an 88.88 order. The gate is pure database logic: these
/// cases hand the sync entry point a notification-shaped JSON and a real
/// Postgres transaction through setTestClients, with no channel and no network.
///
/// The mismatch case proves the guard rejects; the match case is the positive
/// control that proves it does not reject legitimate amounts (a guard that
/// always refuses would otherwise pass the suite).
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "drogon_pay/PayPlugin.h"
#include "services/PaymentService.h"
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include "TestConfigHelper.h"

using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

namespace
{
using PayOrder = drogon_model::pay_test::PayOrder;
using PayPayment = drogon_model::pay_test::PayPayment;
using DbClientPtr = std::shared_ptr<drogon::orm::DbClient>;

using pay::test_util::waitForFutureReady;

// The pay_ledger insert runs inside the same transaction as the order update,
// so the positive case needs the table present: a lookup that errors out on a
// missing table aborts the transaction and the COMMIT silently turns into a
// rollback, which would fail the assertion for the wrong reason.
void ensureSchema(const DbClientPtr &client)
{
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) UNIQUE NOT NULL,"
      "user_id BIGINT NOT NULL,"
      "amount VARCHAR(32) NOT NULL,"
      "currency VARCHAR(8) NOT NULL DEFAULT 'CNY',"
      "status VARCHAR(32) NOT NULL DEFAULT 'CREATED',"
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
      "status VARCHAR(32) NOT NULL DEFAULT 'INIT',"
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
}

// An order mid-flight: 88.88 requested, payment still PROCESSING, so the sync
// is what decides whether it becomes PAID.
struct Fixture
{
    std::string orderNo;
    std::string paymentNo;
};

Fixture insertPendingAlipayOrder(
  const DbClientPtr &client,
  int64_t userId,
  const std::string &amount
)
{
    Fixture fx;
    fx.orderNo = "ord_" + drogon::utils::getUuid();
    fx.paymentNo = "pay_" + drogon::utils::getUuid();

    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(fx.orderNo);
    order.setUserId(userId);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("alipay");
    order.setTitle("Amount Gate");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(fx.orderNo);
    payment.setPaymentNo(fx.paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    return fx;
}

// The shape alipay.trade.query returns and the callback handler forwards to the
// sync entry point: a success code plus the trade fields the gate reads.
Json::Value alipayTradeResult(const std::string &totalAmount)
{
    Json::Value result;
    result["code"] = "10000";
    result["trade_status"] = "TRADE_SUCCESS";
    result["trade_no"] = "20260922220010000007";
    result["total_amount"] = totalAmount;
    return result;
}

struct SyncOutcome
{
    bool ready{false};
    std::string status;
};

SyncOutcome runSync(PayPlugin &plugin, const std::string &orderNo, const Json::Value &result)
{
    auto shared = std::make_shared<std::promise<std::string>>();
    plugin.paymentService()->syncOrderStatusFromAlipay(
      orderNo, result, [shared](const std::string &status) { shared->set_value(status); }
    );

    auto future = shared->get_future();
    SyncOutcome outcome;
    outcome.ready = waitForFutureReady(future, std::chrono::seconds(10));
    if (outcome.ready)
    {
        outcome.status = future.get();
    }
    return outcome;
}

// Single-shot committed reads. The sync callback fires from the transaction's
// update callback, i.e. before the COMMIT lands, so a caller that needs the
// settled state polls rather than reading once.
std::string readOrderStatus(const DbClientPtr &client, const std::string &orderNo)
{
    drogon::orm::Mapper<PayOrder> mapper(client);
    auto shared = std::make_shared<std::promise<std::string>>();
    mapper.findBy(
      drogon::orm::Criteria(PayOrder::Cols::_order_no, drogon::orm::CompareOperator::EQ, orderNo),
      [shared](const std::vector<PayOrder> &rows) {
          shared->set_value(
            rows.empty() ? std::string("<missing>") : rows.front().getValueOfStatus()
          );
      },
      [shared](const drogon::orm::DrogonDbException &e) {
          shared->set_value(std::string("<error>") + e.base().what());
      }
    );

    auto future = shared->get_future();
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        return "<timeout>";
    }
    return future.get();
}

std::string readPaymentStatus(const DbClientPtr &client, const std::string &paymentNo)
{
    drogon::orm::Mapper<PayPayment> mapper(client);
    auto shared = std::make_shared<std::promise<std::string>>();
    mapper.findBy(
      drogon::orm::Criteria(
        PayPayment::Cols::_payment_no, drogon::orm::CompareOperator::EQ, paymentNo
      ),
      [shared](const std::vector<PayPayment> &rows) {
          shared->set_value(
            rows.empty() ? std::string("<missing>") : rows.front().getValueOfStatus()
          );
      },
      [shared](const drogon::orm::DrogonDbException &e) {
          shared->set_value(std::string("<error>") + e.base().what());
      }
    );

    auto future = shared->get_future();
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        return "<timeout>";
    }
    return future.get();
}

std::string waitForStatus(const std::function<std::string()> &read, const std::string &expected)
{
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        const std::string seen = read();
        if (seen == expected)
        {
            return seen;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return read();
}

DbClientPtr openTestDb()
{
    Json::Value root;
    if (
      !loadConfig(root) || !root.isMember("db_clients") || !root["db_clients"].isArray() ||
      root["db_clients"].empty()
    )
    {
        return nullptr;
    }
    const std::string connInfo = buildPgConnInfo(root["db_clients"][0]);
    if (connInfo.empty())
    {
        return nullptr;
    }
    return drogon::orm::DbClient::newPgClient(connInfo, 1);
}

}  // namespace

DROGON_TEST(PayPlugin_SyncOrderStatusFromAlipay_AmountMismatch_RefusesCredit)
{
    auto client = openTestDb();
    REQUIRE(client != nullptr);
    ensureSchema(client);

    const Fixture fx = insertPendingAlipayOrder(client, 30001, "88.88");

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    // A notification that claims 0.01 was paid against an 88.88 order.
    const auto outcome = runSync(plugin, fx.orderNo, alipayTradeResult("0.01"));
    REQUIRE(outcome.ready);

    // Empty status is the service's "refused" signal, which the callback
    // handler turns into a FAIL response instead of a processed ack.
    CHECK(outcome.status == "");

    // Nothing advanced: the transaction rolled back, so neither the order nor
    // the payment moved, and no ledger row was written for it.
    CHECK(readOrderStatus(client, fx.orderNo) == "PAYING");
    CHECK(readPaymentStatus(client, fx.paymentNo) == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", fx.orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", fx.paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", fx.orderNo);
}

DROGON_TEST(PayPlugin_SyncOrderStatusFromAlipay_AmountMatches_CreditsOrder)
{
    auto client = openTestDb();
    REQUIRE(client != nullptr);
    ensureSchema(client);

    const Fixture fx = insertPendingAlipayOrder(client, 30002, "88.88");

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    const auto outcome = runSync(plugin, fx.orderNo, alipayTradeResult("88.88"));
    REQUIRE(outcome.ready);
    CHECK(outcome.status == "PAID");

    CHECK(waitForStatus([&] { return readOrderStatus(client, fx.orderNo); }, "PAID") == "PAID");
    CHECK(
      waitForStatus([&] { return readPaymentStatus(client, fx.paymentNo); }, "SUCCESS") == "SUCCESS"
    );

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", fx.orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", fx.paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", fx.orderNo);
}
