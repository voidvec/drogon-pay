/// =============================================================================
/// @file   QueryOrderListAndReconcileTest.cc
/// @brief  P1 tests for queryOrderList and reconcileSummary (paths 6, 7).
///
/// Covers parameter combinations, empty results, status filtering, pagination,
/// and multi-state reconciliation (5.3, 5.4, 5.5).
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "drogon_pay/PayPlugin.h"
#include "channels/WechatChannel.h"
#include "services/PaymentService.h"
#include <future>
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

using PayOrder = drogon_model::pay_test::PayOrder;
using PayPayment = drogon_model::pay_test::PayPayment;

void ensureOrderTables(const std::shared_ptr<drogon::orm::DbClient> &client)
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
      "channel_trade_no VARCHAR(128),"
      "status VARCHAR(32) NOT NULL DEFAULT 'INIT',"
      "amount VARCHAR(32) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_idempotency ("
      "idempotency_key VARCHAR(128) PRIMARY KEY,"
      "request_hash VARCHAR(64) NOT NULL,"
      "response_snapshot TEXT,"
      "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    );
}

/// Insert a test order+payment synchronously and return the pair.
/// Each field uses a single $1 parameter split into separate execSqlSync calls
/// to avoid multi-$N binding ambiguities.
std::pair<std::string, std::string> insertTestOrder(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &status,
  const std::string &channel,
  const std::string &amount,
  const std::string &prefix
)
{
    const auto orderNo = prefix + drogon::utils::getUuid();
    const auto paymentNo = "pay_" + orderNo;

    client->execSqlSync(
      "INSERT INTO pay_order (order_no,user_id,amount,currency,status,channel) "
      "VALUES ($1, 30001, '" +
        amount + "', 'CNY', '" + status + "', '" + channel + "')",
      orderNo
    );
    client->execSqlSync(
      "INSERT INTO pay_payment (payment_no,order_no,amount) VALUES ($1,$2,$3)",
      paymentNo,
      orderNo,
      amount
    );

    return {orderNo, paymentNo};
}

/// Clean up test data by prefix pattern.
/// Uses inline SQL (not parameter binding) because LIKE $1 with Drogon's
/// execSqlSync variadic binding may not correctly form the LIKE pattern.
void cleanupByPrefix(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &prefix
)
{
    client->execSqlSync("DELETE FROM pay_payment WHERE order_no LIKE '" + prefix + "%'");
    client->execSqlSync("DELETE FROM pay_order WHERE order_no LIKE '" + prefix + "%'");
}

}  // namespace

// =============================================================================
// P1-5.3 / 5.4: queryOrderList parameter combinations + empty results
// =============================================================================

DROGON_TEST(QueryOrderList_MultiStatus_Filtering)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    CHECK(client != nullptr);
    ensureOrderTables(client);
    // Clean up leftovers from previous runs that share these prefixes
    cleanupByPrefix(client, "ord_s1_");
    cleanupByPrefix(client, "ord_f1_");
    cleanupByPrefix(client, "ord_p1_");
    cleanupByPrefix(client, "ord_s2_");

    // Insert orders with mixed statuses
    const auto [o1, p1] = insertTestOrder(client, "SUCCESS", "alipay", "1.00", "ord_s1_");
    const auto [o2, p2] = insertTestOrder(client, "FAILED", "wechat", "2.00", "ord_f1_");
    const auto [o3, p3] = insertTestOrder(client, "PAYING", "alipay", "3.00", "ord_p1_");
    const auto [o4, p4] = insertTestOrder(client, "SUCCESS", "wechat", "4.00", "ord_s2_");

    // Build PayPlugin for queryOrderList
    Json::Value wcConfig;
    wcConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wcConfig["platform_cert_path"] = "nonexistent.pem";
    wcConfig["serial_no"] = "SERIAL";
    wcConfig["app_id"] = "wx_app";
    wcConfig["mch_id"] = "mch_123";
    wcConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wcConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    // --- Test 1: Filter by status=SUCCESS ---
    {
        std::promise<std::pair<Json::Value, std::error_code>> promise;
        plugin.paymentService()->queryOrderList(
          "SUCCESS", 0, 10, 0, [&promise](const Json::Value &r, const std::error_code &e) {
              promise.set_value({r, e});
          }
        );
        auto pairFuture = promise.get_future();
        REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
        auto [res, err] = pairFuture.get();
        CHECK(!err);
        CHECK(res["code"].asInt() == 200);
        CHECK(res["data"].size() >= 2);
    }

    // --- Test 2: Filter by status=FAILED ---
    // --- Test 2: Filter by status=FAILED ---
    {
        std::promise<std::pair<Json::Value, std::error_code>> promise;
        plugin.paymentService()->queryOrderList(
          "FAILED", 0, 10, 0, [&promise](const Json::Value &r, const std::error_code &e) {
              promise.set_value({r, e});
          }
        );
        auto pairFuture = promise.get_future();
        REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
        auto [res, err] = pairFuture.get();
        CHECK(!err);
        CHECK(res["code"].asInt() == 200);
        CHECK(res["data"].size() >= 1);
    }

    // --- Test 3: All statuses ---
    {
        std::promise<std::pair<Json::Value, std::error_code>> promise;
        plugin.paymentService()->queryOrderList(
          "all", 0, 10, 0, [&promise](const Json::Value &r, const std::error_code &e) {
              promise.set_value({r, e});
          }
        );
        auto pairFuture = promise.get_future();
        REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
        auto [res, err] = pairFuture.get();
        CHECK(!err);
        CHECK(res["code"].asInt() == 200);
        CHECK(res["data"].size() >= 4);
    }

    // Cleanup
    client->execSqlSync("DELETE FROM pay_payment WHERE order_no IN ($1,$2,$3,$4)", o1, o2, o3, o4);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no IN ($1,$2,$3,$4)", o1, o2, o3, o4);
}

DROGON_TEST(QueryOrderList_UserIdFilter)
{
    Json::Value root;
    CHECK(loadConfig(root));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    CHECK(client != nullptr);
    ensureOrderTables(client);
    cleanupByPrefix(client, "ord_uid");

    // Insert two orders with userId=90001 plus one with different userId
    // (single $1 param only — multi-$N binding unreliable with this driver)
    {
        auto orderNo = "ord_uid1_" + drogon::utils::getUuid();
        client->execSqlSync(
          "INSERT INTO pay_order (order_no,user_id,amount,currency,status,channel) "
          "VALUES ($1, 90001, '1.00', 'CNY', 'SUCCESS', 'alipay')",
          orderNo
        );
        client->execSqlSync(
          "INSERT INTO pay_payment (payment_no,order_no,amount) VALUES ($1,$2,$3)",
          "pay_" + orderNo,
          orderNo,
          "1.00"
        );
    }
    {
        auto orderNo = "ord_uid2_" + drogon::utils::getUuid();
        client->execSqlSync(
          "INSERT INTO pay_order (order_no,user_id,amount,currency,status,channel) "
          "VALUES ($1, 90001, '2.00', 'CNY', 'FAILED', 'wechat')",
          orderNo
        );
        client->execSqlSync(
          "INSERT INTO pay_payment (payment_no,order_no,amount) VALUES ($1,$2,$3)",
          "pay_" + orderNo,
          orderNo,
          "2.00"
        );
    }
    {
        auto orderNo = "ord_uid3_" + drogon::utils::getUuid();
        client->execSqlSync(
          "INSERT INTO pay_order (order_no,user_id,amount,currency,status,channel) "
          "VALUES ($1, 70002, '3.00', 'CNY', 'SUCCESS', 'alipay')",
          orderNo
        );
        client->execSqlSync(
          "INSERT INTO pay_payment (payment_no,order_no,amount) VALUES ($1,$2,$3)",
          "pay_" + orderNo,
          orderNo,
          "3.00"
        );
    }

    PayPlugin plugin;
    Json::Value wcConfig;
    wcConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wcConfig["platform_cert_path"] = "nonexistent.pem";
    wcConfig["serial_no"] = "SERIAL";
    wcConfig["app_id"] = "wx_app";
    wcConfig["mch_id"] = "mch_123";
    wcConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wcConfig);
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<std::pair<Json::Value, std::error_code>> promise;
    plugin.paymentService()->queryOrderList(
      "all", 90001, 10, 0, [&promise](const Json::Value &r, const std::error_code &e) {
          promise.set_value({r, e});
      }
    );
    auto pairFuture = promise.get_future();
    REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
    auto [res, err] = pairFuture.get();
    CHECK(!err);
    CHECK(res["code"].asInt() == 200);
    CHECK(res["data"].size() >= 2);

    // Cleanup
    client->execSqlSync("DELETE FROM pay_payment WHERE order_no LIKE 'ord_uid%'");
    client->execSqlSync("DELETE FROM pay_order WHERE order_no LIKE 'ord_uid%'");
}

DROGON_TEST(QueryOrderList_Pagination)
{
    Json::Value root;
    CHECK(loadConfig(root));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    CHECK(client != nullptr);
    ensureOrderTables(client);
    cleanupByPrefix(client, "ord_pg_");

    // Insert 8 orders and clean up pre-existing test data first

    std::vector<std::string> orderNos;
    for (int i = 0; i < 8; ++i)
    {
        auto orderNo = "ord_pg_" + std::to_string(i) + "_" + drogon::utils::getUuid();
        orderNos.push_back(orderNo);
        client->execSqlSync(
          "INSERT INTO pay_order (order_no,user_id,amount,currency,status,channel) VALUES "
          "($1, 40000, '" +
            std::to_string(1 + i) + ".00', 'CNY', 'PAYING', 'alipay')",
          orderNo
        );
        client->execSqlSync(
          "INSERT INTO pay_payment (payment_no,order_no,amount) VALUES ($1,$2,$3)",
          "pay_" + orderNo,
          orderNo,
          std::to_string(1 + i) + ".00"
        );
    }

    PayPlugin plugin;
    Json::Value wcConfig;
    wcConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wcConfig["platform_cert_path"] = "nonexistent.pem";
    wcConfig["serial_no"] = "SERIAL";
    wcConfig["app_id"] = "wx_app";
    wcConfig["mch_id"] = "mch_123";
    wcConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wcConfig);
    plugin.setTestClients(wechatClient, nullptr, client);

    // --- Page 1: limit=3, offset=0 ---
    {
        std::promise<std::pair<Json::Value, std::error_code>> promise;
        plugin.paymentService()->queryOrderList(
          "all", 0, 3, 0, [&promise](const Json::Value &r, const std::error_code &e) {
              promise.set_value({r, e});
          }
        );
        auto pairFuture = promise.get_future();
        REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
        auto [res, err] = pairFuture.get();
        CHECK(!err);
        CHECK(res["code"].asInt() == 200);
        // With limit=3, offset=0: should return at least 3 rows
        CHECK(res["data"].size() >= 3);
    }

    // --- Page 2: limit=3, offset=3 ---
    {
        std::promise<std::pair<Json::Value, std::error_code>> promise;
        plugin.paymentService()->queryOrderList(
          "all", 0, 3, 3, [&promise](const Json::Value &r, const std::error_code &e) {
              promise.set_value({r, e});
          }
        );
        auto pairFuture = promise.get_future();
        REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
        auto [res, err] = pairFuture.get();
        CHECK(!err);
        CHECK(res["code"].asInt() == 200);
    }

    // --- Last page: offset way beyond total ---
    {
        std::promise<std::pair<Json::Value, std::error_code>> promise;
        plugin.paymentService()->queryOrderList(
          "all", 0, 10, 99999, [&promise](const Json::Value &r, const std::error_code &e) {
              promise.set_value({r, e});
          }
        );
        auto pairFuture = promise.get_future();
        REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
        auto [res, err] = pairFuture.get();
        CHECK(!err);
        CHECK(res["code"].asInt() == 200);
    }

    // Cleanup
    for (const auto &on : orderNos)
    {
        client->execSqlSync("DELETE FROM pay_payment WHERE order_no = $1", on);
        client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", on);
    }
}

DROGON_TEST(QueryOrderList_EmptyDatabase)
{
    Json::Value root;
    CHECK(loadConfig(root));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    CHECK(client != nullptr);
    ensureOrderTables(client);
    cleanupByPrefix(client, "ord_cr_");
    cleanupByPrefix(client, "ord_py_");

    // Insert one CREATED and one PAYING order
    const auto [co, cp] = insertTestOrder(client, "CREATED", "alipay", "0.50", "ord_cr_");
    Json::Value wcConfig;
    wcConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wcConfig["platform_cert_path"] = "nonexistent.pem";
    wcConfig["serial_no"] = "SERIAL";
    wcConfig["app_id"] = "wx_app";
    wcConfig["mch_id"] = "mch_123";
    wcConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wcConfig);
    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    // Query with a filter that matches nothing
    std::promise<std::pair<Json::Value, std::error_code>> promise;
    plugin.paymentService()->queryOrderList(
      "NONEXISTENT", 0, 10, 0, [&promise](const Json::Value &r, const std::error_code &e) {
          promise.set_value({r, e});
      }
    );
    auto pairFuture = promise.get_future();
    REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
    auto [res, err] = pairFuture.get();
    CHECK(!err);
    CHECK(res["code"].asInt() == 200);
    CHECK(res["data"].size() == 0);
}

DROGON_TEST(QueryOrderList_StatusCodeConsistency_CREATED_and_PAYING)
{
    // Verify A1-2 fix: both CREATED and PAYING orders appear in queryOrderList
    Json::Value root;
    CHECK(loadConfig(root));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    CHECK(client != nullptr);
    ensureOrderTables(client);
    cleanupByPrefix(client, "ord_cr_");
    cleanupByPrefix(client, "ord_py_");

    // Insert one CREATED and one PAYING order
    const auto [co, cp] = insertTestOrder(client, "CREATED", "alipay", "0.50", "ord_cr_");
    const auto [po, pp] = insertTestOrder(client, "PAYING", "wechat", "1.50", "ord_py_");

    PayPlugin plugin;
    Json::Value wcConfig;
    wcConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wcConfig["platform_cert_path"] = "nonexistent.pem";
    wcConfig["serial_no"] = "SERIAL";
    wcConfig["app_id"] = "wx_app";
    wcConfig["mch_id"] = "mch_123";
    wcConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wcConfig);
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<std::pair<Json::Value, std::error_code>> promise;
    plugin.paymentService()
      ->queryOrderList("all", 0, 10, 0, [&promise](const Json::Value &r, const std::error_code &e) {
          promise.set_value({r, e});
      });
    auto pairFuture = promise.get_future();
    REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
    auto [res, err] = pairFuture.get();
    CHECK(!err);
    CHECK(res["code"].asInt() == 200);
    // Both CREATED and PAYING should appear
    bool foundCreated = false, foundPaying = false;
    for (const auto &o : res["data"])
    {
        auto s = o["status"].asString();
        if (s == "CREATED")
            foundCreated = true;
        if (s == "PAYING")
            foundPaying = true;
    }
    CHECK(foundCreated);
    CHECK(foundPaying);

    client->execSqlSync("DELETE FROM pay_payment WHERE order_no IN ($1,$2)", co, po);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no IN ($1,$2)", co, po);
}

// =============================================================================
// P1-5.5: reconcileSummary multi-state combinations
// =============================================================================

DROGON_TEST(ReconcileSummary_MultiState)
{
    Json::Value root;
    CHECK(loadConfig(root));
    const auto &db = root["db_clients"][0];
    auto client = drogon::orm::DbClient::newPgClient(buildPgConnInfo(db), 1);
    CHECK(client != nullptr);
    ensureOrderTables(client);

    // Insert PAYING orders (will be counted in paying_orders)
    insertTestOrder(client, "PAYING", "alipay", "10.00", "ord_rec_p1_");
    insertTestOrder(client, "PAYING", "wechat", "15.00", "ord_rec_p2_");

    PayPlugin plugin;
    Json::Value wcConfig;
    wcConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wcConfig["platform_cert_path"] = "nonexistent.pem";
    wcConfig["serial_no"] = "SERIAL";
    wcConfig["app_id"] = "wx_app";
    wcConfig["mch_id"] = "mch_123";
    wcConfig["api_base"] = "http://127.0.0.1:9";
    auto wechatClient = std::make_shared<WechatPayClient>(wcConfig);
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<std::pair<Json::Value, std::error_code>> promise;
    plugin.paymentService()->reconcileSummary(
      "2026-07-29",
      [&promise](const Json::Value &r, const std::error_code &e) { promise.set_value({r, e}); }
    );
    auto pairFuture = promise.get_future();
    REQUIRE(pay::test_util::waitForFutureReady(pairFuture, std::chrono::seconds(5)));
    auto [res, err] = pairFuture.get();
    CHECK(!err);
    CHECK(res["code"].asInt() == 0);
    CHECK(res.isMember("data"));

    auto &d = res["data"];
    CHECK(d["paying_orders"].asInt() >= 2);
    CHECK(d["refunding_refunds"].asInt() >= 0);

    client->execSqlSync("DELETE FROM pay_payment WHERE order_no LIKE 'ord_rec_%'");
    client->execSqlSync("DELETE FROM pay_order WHERE order_no LIKE 'ord_rec_%'");
}
