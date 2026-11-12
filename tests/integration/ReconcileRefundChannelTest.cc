#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include "models/PayOrder.h"
#include "models/PayRefund.h"
#include "drogon_pay/PayPlugin.h"
#include "drogon_pay/PaymentChannel.h"
#include "services/ReconciliationService.h"
#include "services/RefundService.h"
#include "TestConfigHelper.h"
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
using pay::test_util::buildPgConnInfo;
using pay::test_util::loadConfig;

// Records every refund the reconcile sweep asks WeChat about. The sweep runs
// on the DB event-loop thread while the case reads from the test thread, so
// the set is mutex-guarded. queryRefund answers PROCESSING with a matching
// echo/amount so the downstream sync is harmless; the case only cares about
// WHICH refunds were queried, not what the answer booked.
class RecordingWechatStub : public drogon_pay::PaymentChannel
{
  public:
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
        callback(Json::Value(Json::objectValue), "unused by this case");
    }

    void queryRefund(const std::string &refundNo, JsonCallback &&callback) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queried_.insert(refundNo);
        }
        Json::Value answer;
        answer["status"] = "PROCESSING";
        answer["out_refund_no"] = refundNo;
        answer["amount"]["refund"] = 1000;
        callback(answer, "");
    }

    bool verifyCallback(
      const drogon::HttpRequestPtr &,
      drogon_pay::CallbackEvent &,
      std::string &error
    ) override
    {
        error = "unused by this case";
        return false;
    }

    bool wasQueried(const std::string &refundNo)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queried_.count(refundNo) > 0;
    }

  private:
    std::mutex mutex_;
    std::set<std::string> queried_;
};

void ensureReconcileTables(const std::shared_ptr<drogon::orm::DbClient> &client)
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
      "order_no VARCHAR(64) NOT NULL,"
      "status VARCHAR(32) NOT NULL DEFAULT 'INIT',"
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
      "status VARCHAR(32) NOT NULL DEFAULT 'REFUND_INIT',"
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
}  // namespace

// F2: pay_refund has no channel column, so the refund sweep used to ask WeChat
// about every in-flight refund -- including Alipay ones, which WeChat answers
// REFUND_NOT_FOUND. The sweep must read the parent order's channel and query
// only WeChat orders. Driven through the public reconcile() entry, which calls
// the private syncPendingRefunds when WeChat is configured.
DROGON_TEST(Reconcile_RefundSweep_OnlyWechatOrders)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());
    const auto connInfo = buildPgConnInfo(root["db_clients"][0]);
    CHECK(!connInfo.empty());
    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);
    ensureReconcileTables(client);

    auto stub = std::make_shared<RecordingWechatStub>();
    const std::string wxOrder = "ord_wx_" + drogon::utils::getUuid();
    const std::string aliOrder = "ord_ali_" + drogon::utils::getUuid();
    const std::string wxRefund = "refund_wx_" + drogon::utils::getUuid();
    const std::string aliRefund = "refund_ali_" + drogon::utils::getUuid();

    const std::vector<std::pair<std::string, std::string>> orders =
      {{wxOrder, "wechat"}, {aliOrder, "alipay"}};
    for (const auto &[orderNo, channel] : orders)
    {
        client->execSqlSync(
          "INSERT INTO pay_order (order_no, user_id, amount, status, channel) "
          "VALUES ($1, 1, '10.00', 'PAID', $2)",
          orderNo,
          channel
        );
        client->execSqlSync(
          "INSERT INTO pay_payment (payment_no, order_no, status, amount) "
          "VALUES ($1, $2, 'SUCCESS', '10.00')",
          "pay_" + orderNo,
          orderNo
        );
    }
    client->execSqlSync(
      "INSERT INTO pay_refund (refund_no, order_no, payment_no, status, amount) "
      "VALUES ($1, $2, $3, 'REFUNDING', '10.00')",
      wxRefund,
      wxOrder,
      "pay_" + wxOrder
    );
    client->execSqlSync(
      "INSERT INTO pay_refund (refund_no, order_no, payment_no, status, amount) "
      "VALUES ($1, $2, $3, 'REFUNDING', '10.00')",
      aliRefund,
      aliOrder,
      "pay_" + aliOrder
    );

    PayPlugin plugin;
    plugin.setTestChannels({{"wechat", stub}}, client);
    ReconciliationService reconcile(
      plugin.paymentService(), plugin.refundService(), {{"wechat", stub}}, client
    );
    reconcile.reconcile([](int, int) {});

    // Condition-based wait: the sweep dispatches every queryRefund in one
    // findBy callback, so once the WeChat refund has been asked, any sibling
    // (buggy) Alipay query has already been dispatched too.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stub->wasQueried(wxRefund) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(stub->wasQueried(wxRefund));    // WeChat refund is asked
    CHECK(!stub->wasQueried(aliRefund));  // Alipay refund is NOT asked

    client->execSqlSync("DELETE FROM pay_refund WHERE order_no IN ($1, $2)", wxOrder, aliOrder);
    client->execSqlSync("DELETE FROM pay_payment WHERE order_no IN ($1, $2)", wxOrder, aliOrder);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no IN ($1, $2)", wxOrder, aliOrder);
}
