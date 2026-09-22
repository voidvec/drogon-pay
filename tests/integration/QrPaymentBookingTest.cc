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

// WeChat's out_trade_no window is 6-32 characters of [0-9a-zA-Z_|*-]; the
// service enforces it before booking. A prefixed full uuid (43 characters)
// ran past the cap, so the cases share one compliant unique generator.
std::string qrOrderNo()
{
    std::string compact;
    for (const char c : drogon::utils::getUuid())
    {
        if (c != '-')
        {
            compact += c;
        }
    }
    return "ord_qr_" + compact.substr(0, 20);
}

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
    // One client for the whole run, released on the main thread at exit. These
    // cases answer through a promise and return as soon as the answer lands,
    // while the chain that produced it is still finishing its own database
    // work -- `failQr` clears the idempotency reservation after the caller has
    // been answered. Drogon destroys a DbClient on whichever thread drops the
    // last reference, and that destructor joins the client's own loop threads,
    // so a chain that outlives the case and ends up holding the last reference
    // joins itself and aborts the process (0xC0000409). Production cannot reach
    // this: its client comes from `app().getDbClient()`, which the framework
    // keeps until teardown.
    static const std::shared_ptr<drogon::orm::DbClient> client = [] {
        Json::Value root;
        if (
          !loadConfig(root) || !root.isMember("db_clients") || !root["db_clients"].isArray() ||
          root["db_clients"].empty()
        )
        {
            return std::shared_ptr<drogon::orm::DbClient>{};
        }
        return drogon::orm::DbClient::newPgClient(buildPgConnInfo(root["db_clients"][0]), 4);
    }();
    return client;
}

void ensureQrTables(const std::shared_ptr<drogon::orm::DbClient> &client)
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

// Drives one call to the service behind `/api/qrpay/create` and waits for its
// single answer, reporting how many times the channel was asked in the meantime.
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

// Stands a booking up directly so a case can start from a state the service
// itself has no way to reach in one call (a settled attempt, another user's
// order, an amount written with different spelling).
void seedQrOrder(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &orderNo,
  const std::string &amount,
  const std::string &currency,
  long long userId,
  const std::string &status
)
{
    client->execSqlSync(
      "INSERT INTO pay_order (order_no, user_id, amount, currency, status, channel, title) "
      "VALUES ($1, $2, $3, $4, $5, 'wechat', 'seeded')",
      orderNo,
      userId,
      amount,
      currency,
      status
    );
}

void seedQrPayment(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  const std::string &status
)
{
    client->execSqlSync(
      "INSERT INTO pay_payment (payment_no, order_no, status, amount, request_payload) "
      "VALUES ($1, $2, $3, $4, '{}')",
      paymentNo,
      orderNo,
      status,
      amount
    );
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

    const std::string orderNo = qrOrderNo();
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

// A refusal the channel actually answered for closes the payment row and leaves
// the order reusable: a retry for the same order_no has to append a second
// payment attempt instead of colliding with the unique order number.
DROGON_TEST(PayPlugin_QrBooking_ChannelRefusalClosesThePaymentAndAllowsRetry)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    // WeChat's own error envelope on a 4xx: the order was certainly not created,
    // so the attempt may be booked dead.
    auto stub = std::make_shared<QrStubChannel>(
      Json::Value(Json::objectValue), std::string("HTTP 403: RULEFLOW_ERROR merchant not approved")
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
    CHECK(afterRefusal.front().second.find("HTTP 403") != std::string::npos);

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

// The other direction of that guard: an answer that proves nothing -- here a 5xx,
// which can come from an intermediary while WeChat did create the order -- must
// leave the attempt in flight. Booking it FAIL would hide a code the user may
// still pay from the callback and from reconciliation.
DROGON_TEST(PayPlugin_QrBooking_UncertainOutcomeKeepsTheAttemptInFlight)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(
      Json::Value(Json::objectValue), std::string("HTTP 500: bad gateway from an intermediary")
    );

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto answer = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "7.77"), stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 500);

    const auto payments = paymentsOf(client, orderNo);
    REQUIRE(payments.size() == 1);
    CHECK(payments.front().first == "INIT");
    CHECK(orderStatusOf(client, orderNo) == "CREATED");
}

// The third flavour of an uncertain answer, and the one the code used to read
// backwards: a 2xx body that names no payable code. WeChat may have created the
// transaction behind a response an intermediary rewrote, so this attempt has to
// stay in flight like the 5xx above -- and it is reported as a failure either way,
// because there is no code to hand back.
DROGON_TEST(PayPlugin_QrBooking_AnswerWithoutCodeUrlKeepsTheAttemptInFlight)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    // An empty object is what a 200 with no `code_url`, no `prepay_id` and no
    // `code` looks like from the channel's side.
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto answer = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "6.66"), stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 500);
    CHECK(answer.channelCalls == 1);

    const auto payments = paymentsOf(client, orderNo);
    REQUIRE(payments.size() == 1);
    CHECK(payments.front().first == "INIT");
    CHECK(orderStatusOf(client, orderNo) == "CREATED");
}

// The request hash covers the currency the booking actually uses, not the string
// the caller typed: WeChat takes an upper-case ISO code and the service
// normalises what it offers to the channel, so `cny` and `CNY` are one and the
// same order. Hashing the raw field answered the second caller with 1004.
DROGON_TEST(PayPlugin_QrBooking_CurrencySpellingIsNotAnIdempotencyConflict)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=case";
    stub->succeedWith(accepted);

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto lowerCase = qrRequest(orderNo, "3.21");
    lowerCase["currency"] = "cny";
    const auto first = offerQrPayment(plugin.paymentService(), lowerCase, stub);
    CHECK(!first.error);
    REQUIRE(first.result.get("code", -1).asInt() == 0);
    // What the channel was offered and what the order records are the normalised
    // code, which is the reason the hash may use it too.
    CHECK(stub->lastPayload()["amount"]["currency"].asString() == "CNY");
    const auto booked =
      client->execSqlSync("SELECT currency FROM pay_order WHERE order_no = $1", orderNo);
    REQUIRE(!booked.empty());
    CHECK(booked.front()["currency"].as<std::string>() == "CNY");

    auto upperCase = qrRequest(orderNo, "3.21");
    upperCase["currency"] = "CNY";
    const auto second = offerQrPayment(plugin.paymentService(), upperCase, stub);
    CHECK(!second.error);
    CHECK(second.result.get("code", -1).asInt() == 0);
    // A replay of the first answer, not a second transaction: the channel was
    // asked exactly once across both calls.
    CHECK(second.channelCalls == 0);
    CHECK(second.result["data"]["code_url"].asString() == "weixin://wxpay/bizpayurl?pr=case");
}

// An attempt that carries money rules out a second code even when the order row
// never moved: the status writes for order and payment are separate steps on this
// path, so an order can still read CREATED under a payment that succeeded.
DROGON_TEST(PayPlugin_QrBooking_SettledAttemptRefusesAnotherCode)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    seedQrOrder(client, orderNo, "9.99", "CNY", 4242, "CREATED");
    seedQrPayment(client, orderNo, "pay_" + drogon::utils::getUuid(), "9.99", "SUCCESS");

    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto answer = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "9.99"), stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 400);
    // Nothing may be offered, so the channel must not even be asked.
    CHECK(answer.channelCalls == 0);
    CHECK(paymentsOf(client, orderNo).size() == 1);
}

// `order_no` comes from the caller: without the owner check, an entry that guesses
// another user's order number would be handed a payable code for that charge.
DROGON_TEST(PayPlugin_QrBooking_AnotherUsersOrderIsNotReused)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    seedQrOrder(client, orderNo, "9.99", "CNY", 999999, "CREATED");

    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto answer = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "9.99"), stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 400);
    CHECK(answer.channelCalls == 0);
}

// The positive control for the owner check: the same order with the same owner is
// reusable, and a yuan string spelled differently is the same charge.
DROGON_TEST(PayPlugin_QrBooking_SameOwnerAndAmountSpellingIsReusable)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    seedQrOrder(client, orderNo, "1.50", "CNY", 4242, "CREATED");

    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=spelling";
    stub->succeedWith(accepted);

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    const auto answer = offerQrPayment(plugin.paymentService(), qrRequest(orderNo, "1.5"), stub);
    CHECK(!answer.error);
    REQUIRE(answer.result.get("code", -1).asInt() == 0);
    CHECK(answer.channelCalls == 1);

    const auto payments = paymentsOf(client, orderNo);
    REQUIRE(payments.size() == 1);
    CHECK(payments.front().first == "PROCESSING");
    CHECK(stub->lastPayload()["amount"]["total"].asInt64() == 150);
}

// `/api/qrpay/create` used to rebuild the request from the four required fields
// only, so a caller's `currency` never reached either the channel or the order
// row. The currency matters for WeChat: the notification's currency is compared
// against `pay_order.currency`, so an order booked as CNY and offered as USD can
// never settle. Lowercase input is normalised to the ISO-4217 uppercase form.
DROGON_TEST(PayPlugin_QrBooking_CallerCurrencyIsNormalisedIntoTheOfferAndTheOrder)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=currency";
    stub->succeedWith(accepted);

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto request = qrRequest(orderNo, "9.99");
    request["currency"] = "usd";
    const auto answer = offerQrPayment(plugin.paymentService(), request, stub);
    CHECK(!answer.error);
    REQUIRE(answer.result.get("code", -1).asInt() == 0);
    CHECK(stub->lastPayload()["amount"]["currency"].asString() == "USD");

    const auto rows =
      client->execSqlSync("SELECT currency, amount FROM pay_order WHERE order_no = $1", orderNo);
    REQUIRE(!rows.empty());
    CHECK(rows.front()["currency"].as<std::string>() == "USD");
    CHECK(rows.front()["amount"].as<std::string>() == "9.99");
}

// A malformed currency is refused before anything is booked: the channel would
// only answer PARAM_ERROR, and an order row whose currency no notification can
// match is a charge that can never settle.
DROGON_TEST(PayPlugin_QrBooking_MalformedCurrencyIsRefusedBeforeBooking)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto request = qrRequest(orderNo, "9.99");
    request["currency"] = "DOLLARS";
    const auto answer = offerQrPayment(plugin.paymentService(), request, stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 400);
    CHECK(answer.channelCalls == 0);
    const auto rows = client->execSqlSync("SELECT id FROM pay_order WHERE order_no = $1", orderNo);
    CHECK(rows.empty());
}

// `notify_url` decides where WeChat posts the settlement. Passing it through
// means the QR route now applies the same SSRF gate `/api/pay/create` has, so an
// internal address is refused and a public one reaches the channel payload.
DROGON_TEST(PayPlugin_QrBooking_PrivateNotifyUrlIsRefusedWithoutAskingTheChannel)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto request = qrRequest(orderNo, "9.99");
    request["notify_url"] = "http://169.254.169.254/latest/meta-data/";
    const auto answer = offerQrPayment(plugin.paymentService(), request, stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 400);
    CHECK(answer.channelCalls == 0);
}

DROGON_TEST(PayPlugin_QrBooking_PublicNotifyUrlReachesTheChannelPayload)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=notify";
    stub->succeedWith(accepted);

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto request = qrRequest(orderNo, "9.99");
    request["notify_url"] = "https://merchant.example.com/pay/notify";
    const auto answer = offerQrPayment(plugin.paymentService(), request, stub);
    CHECK(!answer.error);
    REQUIRE(answer.result.get("code", -1).asInt() == 0);
    CHECK(
      stub->lastPayload()["notify_url"].asString() == "https://merchant.example.com/pay/notify"
    );
}

// The owner written on the order row is the caller's, not a fallback. The booking
// used to default a missing `user_id` to buyer 1 -- and read it through a JSON
// default that was a *string*, which `asInt64()` throws on -- so money was either
// attributed to the wrong tenant or the booking faulted. An id that only fits 64
// bits is booked verbatim, matching the BIGINT column.
DROGON_TEST(PayPlugin_QrBooking_CallerOwnerIsBookedOnTheOrder)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    Json::Value accepted;
    accepted["code_url"] = "weixin://wxpay/bizpayurl?pr=owner";
    stub->succeedWith(accepted);

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto request = qrRequest(orderNo, "9.99");
    request["user_id"] = Json::Value(static_cast<Json::Int64>(3000000000LL));
    const auto answer = offerQrPayment(plugin.paymentService(), request, stub);
    CHECK(!answer.error);
    REQUIRE(answer.result.get("code", -1).asInt() == 0);

    const auto rows =
      client->execSqlSync("SELECT user_id FROM pay_order WHERE order_no = $1", orderNo);
    REQUIRE(!rows.empty());
    CHECK(rows.front()["user_id"].as<int64_t>() == 3000000000LL);
}

// A booking with no owner is refused before the order row exists and before the
// channel is asked, so the rejected attempt cannot leave a charge nobody owns.
DROGON_TEST(PayPlugin_QrBooking_MissingOwnerIsRefusedBeforeBooking)
{
    auto client = makeTestClient();
    REQUIRE(client != nullptr);
    ensureQrTables(client);

    const std::string orderNo = qrOrderNo();
    auto stub = std::make_shared<QrStubChannel>(Json::Value(Json::objectValue), std::string());
    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);

    auto request = qrRequest(orderNo, "9.99");
    request.removeMember("user_id");
    const auto answer = offerQrPayment(plugin.paymentService(), request, stub);
    CHECK(answer.error);
    CHECK(answer.result.get("code", 0).asInt() == 400);
    CHECK(answer.channelCalls == 0);
    const auto rows = client->execSqlSync("SELECT id FROM pay_order WHERE order_no = $1", orderNo);
    CHECK(rows.empty());
}
