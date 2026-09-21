#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/Utilities.h>
#include <chrono>
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

// The service now enforces WeChat's official out_trade_no window (6-32 chars
// of [0-9a-zA-Z_|*-]) before booking, so these service-level callers supply a
// compliant unique order number instead of relying on the old empty default.
std::string shortOrderNo()
{
    std::string compact;
    for (const char c : drogon::utils::getUuid())
    {
        if (c != '-')
        {
            compact += c;
        }
    }
    return "cix" + compact.substr(0, 25);
}

void ensureCreatePaymentTables(const std::shared_ptr<drogon::orm::DbClient> &client)
{
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
    request.orderNo = shortOrderNo();
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
    request.orderNo = shortOrderNo();
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

// The payment-end-time guard, end to end: a value the channel cannot be shown
// has to be refused before anything is booked, and a value it can be shown for
// has to get through. Every timestamp here is a fixed constant, so the cases do
// not drift with the clock or the runner's timezone.
DROGON_TEST(PayPlugin_CreatePayment_TimeExpireGuard)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);
    ensureCreatePaymentTables(client);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);
    auto paymentService = plugin.paymentService();

    const std::string title = "TimeExpireGuard_" + drogon::utils::getUuid();

    // Answers one call with the body's code and message, and reports whether the
    // order row exists afterwards -- the point of a pre-booking guard is that a
    // refused request leaves nothing in the sweep set.
    auto attempt = [&](const std::string &channel, const std::string &timeExpire) {
        CreatePaymentRequest request;
        request.orderNo = shortOrderNo();
        request.userId = 10009;
        request.amount = "9.99";
        request.currency = "CNY";
        request.description = title;
        request.channel = channel;
        request.timeExpire = timeExpire;

        // Shared so a delivery that answers after this helper returns cannot
        // write into a destroyed promise.
        auto resultPromise = std::make_shared<std::promise<Json::Value>>();
        auto resultFuture = resultPromise->get_future();
        paymentService->createPayment(
          request, "", [resultPromise](const Json::Value &result, const std::error_code &) {
              resultPromise->set_value(result);
          }
        );
        const auto ready = resultFuture.wait_for(std::chrono::seconds(10));
        CHECK(ready == std::future_status::ready);
        if (ready != std::future_status::ready)
        {
            return std::make_pair(std::string("timed out"), false);
        }
        const Json::Value result = resultFuture.get();
        const std::string message = result.get("message", "").asString();

        const auto countRows =
          client->execSqlSync("SELECT COUNT(*) AS cnt FROM pay_order WHERE title = $1", title);
        const bool booked = !countRows.empty() && countRows.front()["cnt"].as<int64_t>() > 0;
        return std::make_pair(message, booked);
    };

    // The exact mismatch this round found: a space-separated timestamp is what
    // the old local parse accepted and what WeChat answers with a 400 of its
    // own, after the order row already exists.
    const auto spaced = attempt("wechat", "2099-05-20 13:29:35");
    CHECK(spaced.first.find("time_expire") != std::string::npos);
    CHECK(!spaced.second);

    // An expiry before the order is created is a QR code that can never be paid.
    const auto past = attempt("wechat", "2000-01-01T00:00:00Z");
    CHECK(past.first.find("in the past") != std::string::npos);
    CHECK(!past.second);

    // The channel's own window: past seven days WeChat moves the deadline
    // silently, so the disagreement is refused rather than accepted.
    const auto beyondWindow = attempt("wechat", "2099-05-20T13:29:35Z");
    CHECK(beyondWindow.first.find("within 7 days") != std::string::npos);
    CHECK(!beyondWindow.second);

    // Positive control: a well-formed deadline is not this guard's business for
    // a channel that never receives the field, so the call has to get past it
    // and fail (if at all) on something else. Without this case a guard that
    // refused everything would still pass the three checks above.
    const auto legalOtherChannel = attempt("alipay", "2099-05-20T13:29:35+08:00");
    CHECK(legalOtherChannel.first.find("time_expire") == std::string::npos);

    client->execSqlSync(
      "DELETE FROM pay_payment WHERE order_no IN (SELECT order_no FROM pay_order WHERE title = $1)",
      title
    );
    client->execSqlSync("DELETE FROM pay_order WHERE title = $1", title);
}
