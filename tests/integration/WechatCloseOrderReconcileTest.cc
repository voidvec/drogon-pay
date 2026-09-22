#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "models/PayOrder.h"
#include "models/PayPayment.h"
#include "drogon_pay/PaymentChannel.h"
#include "services/PaymentService.h"
#include "services/RefundService.h"
#include "services/ReconciliationService.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "TestConfigHelper.h"

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

// Answers a channel query with a chosen body and records which orders the
// reconciliation sweep asked about and which it asked to close, so the
// assertions read the sweep's gate directly: unpaid on the channel plus past
// the row's own deadline. The sweep reads every unpaid wechat order in the
// database, so both records are per order -- a scenario judges only its own
// row, never whoever else was left in the table.
class CloseRecordingChannel : public drogon_pay::PaymentChannel
{
  public:
    struct State
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::string> queried;
        std::vector<std::string> closed;
    };

    CloseRecordingChannel(Json::Value answer, std::shared_ptr<State> state)
        : answer_(std::move(answer)), state_(std::move(state))
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

    void queryPayment(const std::string &orderNo, JsonCallback &&callback) override
    {
        // Answer first, record second: the sweep decides inside this callback,
        // so once an order shows up in `queried` its close decision is final
        // and a scenario can read `closed` for that one order alone.
        callback(answer_, std::string());
        {
            const std::lock_guard<std::mutex> lock(state_->mutex);
            state_->queried.push_back(orderNo);
        }
        state_->cv.notify_all();
    }

    void closeOrder(const std::string &orderNo, JsonCallback &&callback) override
    {
        {
            const std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed.push_back(orderNo);
        }
        state_->cv.notify_all();
        callback(Json::Value(Json::objectValue), std::string());
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

    // The reconcile callback fires at dispatch time, not completion, so each
    // scenario waits on the downstream event that actually matters: either a
    // close was recorded, or the sweep looked at the order and stopped there.
    // The predicate reads the state through the reference it is handed because
    // it runs while waitFor holds the mutex; taking the same non-recursive lock
    // again to read those fields is a self-deadlock, and MSVC answers it by
    // throwing out of the wait instead of blocking.
    bool waitFor(const std::function<bool(const State &)> &predicate, int seconds)
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->cv.wait_for(lock, std::chrono::seconds(seconds), [this, &predicate] {
            return predicate(*state_);
        });
    }

    std::vector<std::string> closedOrders()
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->closed;
    }

  private:
    Json::Value answer_;
    std::shared_ptr<State> state_;
};

std::shared_ptr<drogon::orm::DbClient> makeCloseTestClient()
{
    // The sweep is background work: reconcile() reports dispatch, not
    // completion, so queries and bookings it started are still running when the
    // helper that called it returns. Drogon destroys a DbClient on whichever
    // thread drops the last reference, and that destructor joins the client's
    // own loop threads -- a release from one of those threads joins itself and
    // aborts the process. So this file holds one client for the whole run and
    // lets it die on the main thread at exit.
    static const std::shared_ptr<drogon::orm::DbClient> client = [] {
        Json::Value root;
        if (
          !loadConfig(root) || !root.isMember("db_clients") || !root["db_clients"].isArray() ||
          root["db_clients"].empty()
        )
        {
            return std::shared_ptr<drogon::orm::DbClient>{};
        }
        return drogon::orm::DbClient::newPgClient(buildPgConnInfo(root["db_clients"][0]), 1);
    }();
    return client;
}

void ensureCloseSweepTables(const std::shared_ptr<drogon::orm::DbClient> &client)
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
}

struct SweepOutcome
{
    bool timedOut{false};
    bool examinedSelf{false};
    bool closedSelf{false};
    std::string orderStatus;
    std::string paymentStatus;
};

bool namesOrder(const std::vector<std::string> &orders, const std::string &orderNo)
{
    return std::find(orders.begin(), orders.end(), orderNo) != orders.end();
}

// Books one wechat order (with one open payment attempt) whose expire_at is
// `expireOffsetSeconds` from now -- nullopt leaves the column NULL -- lets the
// reconcile sweep look at it through a stub whose query answers `tradeState`,
// and reports what the sweep decided about that one order plus where its rows
// landed. The sweep reads every unpaid wechat order in the database up to its
// batch limit, newest first, so this row is booked as the newest one and the
// wait is for the sweep's answer about it -- not for a count of closes that
// whatever else is in the table would also feed.
SweepOutcome runCloseSweep(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &tradeState,
  const std::optional<int64_t> &expireOffsetSeconds,
  const std::string &wantedOrderStatus
)
{
    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "49.90";

    // The only interpolated value is an int64 seconds offset, so no string
    // from outside this helper can shape the statement.
    std::string expireExpr = "NULL";
    if (expireOffsetSeconds)
    {
        expireExpr = "NOW() + make_interval(secs => " + std::to_string(*expireOffsetSeconds) + ")";
    }
    client->execSqlSync(
      "INSERT INTO pay_order (order_no, user_id, amount, currency, status, channel, title, "
      "expire_at, created_at, updated_at) VALUES ($1, $2, $3, 'CNY', 'PAYING', 'wechat', "
      "'Close Sweep', " +
        expireExpr + ", NOW() - INTERVAL '600 seconds', NOW())",
      orderNo,
      int64_t{31001},
      amount
    );
    client->execSqlSync(
      "INSERT INTO pay_payment (payment_no, order_no, status, amount, created_at, updated_at) "
      "VALUES ($1, $2, 'PROCESSING', $3, NOW() - INTERVAL '600 seconds', NOW() - INTERVAL "
      "'600 seconds')",
      paymentNo,
      orderNo,
      amount
    );

    Json::Value answer;
    answer["trade_state"] = tradeState;
    answer["transaction_id"] = "wx_close_test_transaction";
    answer["amount"]["total"] = static_cast<Json::Int64>(4990);
    answer["amount"]["currency"] = "CNY";
    answer["amount"]["payer_total"] = static_cast<Json::Int64>(4990);

    auto state = std::make_shared<CloseRecordingChannel::State>();
    auto stub = std::make_shared<CloseRecordingChannel>(answer, state);
    std::map<std::string, drogon_pay::PaymentChannelPtr> channels;
    channels["wechat"] = stub;

    auto paymentService = std::make_shared<PaymentService>(channels, client, nullptr, nullptr);
    auto refundService = std::make_shared<RefundService>(channels, client, nullptr);
    ReconciliationService reconciliation(paymentService, refundService, channels, client);

    std::promise<void> dispatched;
    reconciliation.reconcile([&dispatched](int, int) { dispatched.set_value(); });

    SweepOutcome outcome;
    outcome.examinedSelf = stub->waitFor(
      [&orderNo](const CloseRecordingChannel::State &state) {
          return namesOrder(state.queried, orderNo);
      },
      10
    );
    if (
      !outcome.examinedSelf ||
      dispatched.get_future().wait_for(std::chrono::seconds(5)) != std::future_status::ready
    )
    {
        outcome.timedOut = true;
    }
    // Read once the sweep has answered about this order: it decides inside that
    // answer, so the close it provoked is already on the list.
    outcome.closedSelf = namesOrder(stub->closedOrders(), orderNo);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto orderRows =
          client->execSqlSync("SELECT status FROM pay_order WHERE order_no = $1", orderNo);
        outcome.orderStatus =
          orderRows.empty() ? std::string{} : orderRows.front()["status"].as<std::string>();
        const auto paymentRows =
          client->execSqlSync("SELECT status FROM pay_payment WHERE payment_no = $1", paymentNo);
        outcome.paymentStatus =
          paymentRows.empty() ? std::string{} : paymentRows.front()["status"].as<std::string>();
        if (outcome.orderStatus == wantedOrderStatus)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client->execSqlSync("DELETE FROM pay_ledger WHERE order_no = $1", orderNo);
    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
    return outcome;
}
}  // namespace

// Audit round 14: nothing in the codebase ever asked WeChat to close a trade,
// so an expired-but-unpaid order stayed pay-able on the channel until WeChat's
// own lazy expiry, and the reconcile sweep -- the one component that looks at
// exactly these orders -- reported them as still unpaid each pass without ever
// ending them. The sweep now closes a trade the channel itself still calls
// NOTPAY once the order's own deadline has passed.
DROGON_TEST(PayPlugin_Reconcile_ExpiredUnpaidWechatOrderIsClosedOnChannel)
{
    auto client = makeCloseTestClient();
    REQUIRE(client != nullptr);
    ensureCloseSweepTables(client);

    const auto closed = runCloseSweep(client, "NOTPAY", std::optional<int64_t>(-3600), "PAYING");
    CHECK(!closed.timedOut);
    CHECK(closed.examinedSelf);
    CHECK(closed.closedSelf);
    // The close ends the trade on the channel; the local rows converge to
    // CLOSED on the next sweep, so this one must leave them untouched.
    CHECK(closed.orderStatus == "PAYING");

    // The guard has to stay a guard: the same unpaid answer on an order whose
    // deadline has not passed, or on an order nobody measured a deadline for,
    // must never reach the close API -- closing early would cut off a payment
    // the merchant is still waiting for.
    const auto alive = runCloseSweep(client, "NOTPAY", std::optional<int64_t>(3600), "PAYING");
    CHECK(!alive.timedOut);
    CHECK(alive.examinedSelf);
    CHECK(!alive.closedSelf);

    const auto unmeasured = runCloseSweep(client, "NOTPAY", std::optional<int64_t>(), "PAYING");
    CHECK(!unmeasured.timedOut);
    CHECK(unmeasured.examinedSelf);
    CHECK(!unmeasured.closedSelf);

    // And a trade the channel reports as paid is settled, not closed, even
    // after its deadline: the money arrived, so the close door stays shut.
    const auto paid = runCloseSweep(client, "SUCCESS", std::optional<int64_t>(-3600), "PAID");
    CHECK(!paid.timedOut);
    CHECK(paid.examinedSelf);
    CHECK(!paid.closedSelf);
    CHECK(paid.paymentStatus == "SUCCESS");
}
