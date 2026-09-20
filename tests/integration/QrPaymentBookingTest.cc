#include <drogon/drogon_test.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/Utilities.h>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "drogon_pay/PayPlugin.h"
#include "drogon_pay/PaymentChannel.h"
#include "services/PaymentService.h"
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

// Stands in for a real WeChat client: it answers whatever the case under test
// needs and records the payload it was offered, so the booking behaviour can be
// watched without a network call.
class QrStubChannel : public drogon_pay::PaymentChannel
{
  public:
    QrStubChannel(Json::Value result, std::string error)
        : result_(std::move(result)), error_(std::move(error))
    {
    }

    void succeedWith(Json::Value result)
    {
        result_ = std::move(result);
        error_.clear();
    }

    void failWith(std::string error)
    {
        result_ = Json::Value(Json::objectValue);
        error_ = std::move(error);
    }

    int calls() const
    {
        return calls_;
    }

    Json::Value lastPayload() const
    {
        return lastPayload_;
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

    void createPayment(const Json::Value &payload, JsonCallback &&callback) override
    {
        lastPayload_ = payload;
        ++calls_;
        callback(result_, error_);
    }

    void createQRPayment(const Json::Value &payload, JsonCallback &&callback) override
    {
        lastPayload_ = payload;
        ++calls_;
        callback(result_, error_);
    }

    void queryPayment(const std::string &, JsonCallback &&callback) override
    {
        callback(result_, error_);
    }

    void refund(const Json::Value &, JsonCallback &&callback) override
    {
        callback(result_, error_);
    }

    void queryRefund(const std::string &, JsonCallback &&callback) override
    {
        callback(result_, error_);
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
    Json::Value result_;
    std::string error_;
    Json::Value lastPayload_{Json::objectValue};
    int calls_{0};
};

std::shared_ptr<drogon::orm::DbClient> makeTestClient()
{
    Json::Value root;
    if (
      !loadConfig(root) || !root.isMember("db_clients") || !root["db_clients"].isArray() ||
      root["db_clients"].empty()
    )
    {
        return nullptr;
    }
    return drogon::orm::DbClient::newPgClient(buildPgConnInfo(root["db_clients"][0]), 4);
}

void ensureQrTables(const std::shared_ptr<drogon::orm::DbClient> &client)
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

Json::Value qrRequest(const std::string &orderNo, const std::string &amount)
{
    Json::Value request;
    request["order_no"] = orderNo;
    request["amount"] = amount;
    request["channel"] = "wechat";
    request["subject"] = "QR booking test";
    request["user_id"] = 4242;
    return request;
}

struct QrAnswer
{
    Json::Value result;
    std::error_code error;
    int channelCalls{0};
};

// Drives one `/api/qrpay/create` attempt and waits for its single answer,
// reporting how many times the channel was asked in the meantime.
QrAnswer offerQrPayment(
  const std::shared_ptr<PaymentService> &service,
  const Json::Value &request,
  const std::shared_ptr<QrStubChannel> &stub
)
{
    auto answered = std::make_shared<std::promise<std::pair<Json::Value, std::error_code>>>();
    auto future = answered->get_future();
    const int callsBefore = stub->calls();
    service->createQRPayment(
      request, [answered](const Json::Value &result, const std::error_code &error) {
          answered->set_value(std::make_pair(result, error));
      }
    );
    if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
    {
        return QrAnswer{
          Json::Value(Json::objectValue), std::make_error_code(std::errc::timed_out), stub->calls()
        };
    }
    const auto answer = future.get();
    return QrAnswer{answer.first, answer.second, stub->calls() - callsBefore};
}

std::string orderStatusOf(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &orderNo
)
{
    const auto rows =
      client->execSqlSync("SELECT status FROM pay_order WHERE order_no = $1", orderNo);
    return rows.empty() ? std::string() : rows.front()["status"].as<std::string>();
}

std::vector<std::pair<std::string, std::string>> paymentsOf(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &orderNo
)
{
    std::vector<std::pair<std::string, std::string>> rows;
    const auto found = client->execSqlSync(
      "SELECT status, response_payload FROM pay_payment WHERE order_no = $1 ORDER BY id", orderNo
    );
    for (const auto &row : found)
    {
        rows
          .emplace_back(row["status"].as<std::string>(), row["response_payload"].as<std::string>());
    }
    return rows;
}

bool anyPaymentHasStatus(
  const std::vector<std::pair<std::string, std::string>> &rows,
  const std::string &status
)
{
    for (const auto &[rowStatus, payload] : rows)
    {
        if (rowStatus == status)
        {
            return true;
        }
    }
    return false;
}
}  // namespace

// Audit item C5: `/api/qrpay/create` used to ask the channel for a code and then
// write a pay_order with no pay_payment row, so the V3 notification for a paid
// WeChat QR order had nothing to settle against. The rows are now booked first,
// which is what these cases pin down.
DROGON_TEST(PayPlugin_QrBooking_WechatQrCreatesAPaymentRow)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = "ord_qr_" + drogon::utils::getUuid();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=testQr";
    stub->succeedWith(accepted);

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto answer = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "9.99"), stub);
    CHECK(!answer.error);
    CHECK(answer.channelCalls == 1);
    REQUIRE(answer.result.get("code", -1).asInt() == 0);
    CHECK(answer.result["data"]["code_url"].asString() == "weixin://wxpay/bizpayurl?pr=testQr");

    // The amount the channel was offered is in fen, which is the other half of
    // why this path used to fail: the Alipay field names went to WeChat.
    CHECK(stub->lastPayload()["amount"]["total"].asInt64() == 999);
    CHECK(stub->lastPayload()["out_trade_no"].asString() == orderNo);

    CHECK(orderStatusOf(client, orderNo) == "PAYING");
    const auto payments = paymentsOf(client, orderNo);
    REQUIRE(payments.size() == 1);
    CHECK(payments.front().first == "PROCESSING");
    CHECK(payments.front().second.find("code_url") != std::string::npos);
}

// A channel refusal closes the payment row and leaves the order reusable: a
// retry for the same order_no has to append a second payment attempt instead of
// colliding with the unique order number.
DROGON_TEST(PayPlugin_QrBooking_ChannelRefusalClosesThePaymentAndAllowsRetry)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = "ord_qr_" + drogon::utils::getUuid();
    auto stub = std::make_shared<QrStubChannel>(
      Json::Value(Json::objectValue), std::string("HTTP 500 Internal Server Error")
    );

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto refused = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "12.34"), stub);
    CHECK(refused.error);
    CHECK(refused.result.get("code", 0).asInt() == 500);
    CHECK(orderStatusOf(client, orderNo) == "CREATED");
    const auto afterRefusal = paymentsOf(client, orderNo);
    REQUIRE(afterRefusal.size() == 1);
    CHECK(afterRefusal.front().first == "FAIL");
    CHECK(afterRefusal.front().second.find("HTTP 500") != std::string::npos);

    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=retry";
    stub->succeedWith(accepted);

    const auto retried = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "12.34"), stub);
    CHECK(!retried.error);
    REQUIRE(retried.result.get("code", -1).asInt() == 0);
    CHECK(retried.result["data"]["code_url"].asString() == "weixin://wxpay/bizpayurl?pr=retry");

    // The order row is reused (one row) while each attempt keeps its own payment
    // row, so the failed attempt stays auditable.
    CHECK(orderStatusOf(client, orderNo) == "PAYING");
    const auto afterRetry = paymentsOf(client, orderNo);
    REQUIRE(afterRetry.size() == 2);
    CHECK(anyPaymentHasStatus(afterRetry, "FAIL"));
    CHECK(anyPaymentHasStatus(afterRetry, "PROCESSING"));
}
