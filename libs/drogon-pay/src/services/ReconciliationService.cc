#include "ReconciliationService.h"
#include "../models/PayOrder.h"
#include "../models/PayRefund.h"
#include <drogon/drogon.h>

using namespace drogon;

namespace
{
using PayOrderModel = drogon_model::pay_test::PayOrder;
using PayRefundModel = drogon_model::pay_test::PayRefund;
}  // namespace

ReconciliationService::ReconciliationService(
  std::shared_ptr<PaymentService> paymentService,
  std::shared_ptr<RefundService> refundService,
  std::map<std::string, drogon_pay::PaymentChannelPtr> channels,
  std::shared_ptr<drogon::orm::DbClient> dbClient
)
    : paymentService_(paymentService),
      refundService_(refundService),
      channels_(std::move(channels)),
      dbClient_(dbClient),
      reconcileTimerId_(0),
      reconcileIntervalSeconds_(300),
      reconcileBatchSize_(50)
{
}

drogon_pay::PaymentChannelPtr ReconciliationService::findChannel(const std::string &name) const
{
    auto it = channels_.find(name);
    return it != channels_.end() ? it->second : nullptr;
}

void ReconciliationService::startReconcileTimer(trantor::EventLoop *loop)
{
    timerLoop_ = loop ? loop : drogon::app().getLoop();
    reconcileTimerId_ =
      timerLoop_->runEvery(std::chrono::seconds(reconcileIntervalSeconds_), [this]() {
          this->reconcile([](int success, int failed) {
              LOG_INFO << "Reconciliation completed: success=" << success << ", failed=" << failed;
          });
      });

    LOG_INFO << "Reconciliation timer started (interval: " << reconcileIntervalSeconds_ << "s)";
}

void ReconciliationService::stopReconcileTimer()
{
    if (reconcileTimerId_ && timerLoop_)
    {
        timerLoop_->invalidateTimer(reconcileTimerId_);
        reconcileTimerId_ = 0;
        LOG_INFO << "Reconciliation timer stopped";
    }
}

void ReconciliationService::setReconcileOptions(int intervalSeconds, int batchSize)
{
    if (intervalSeconds > 0)
    {
        reconcileIntervalSeconds_ = intervalSeconds;
    }
    if (batchSize > 0)
    {
        reconcileBatchSize_ = batchSize;
    }
}

void ReconciliationService::reconcile(std::function<void(int success, int failed)> &&callback)
{
    // Track dispatched sweeps and dispatch-time failures
    auto successCount = std::make_shared<int>(0);
    auto failedCount = std::make_shared<int>(0);

    // Sync pending WeChat Pay orders (only if configured)
    if (isWeChatConfigured())
    {
        ++(*successCount);
        syncPendingWeChatOrders(failedCount);
    }
    else
    {
        LOG_DEBUG << "WeChat Pay not configured, skipping WeChat order reconciliation";
    }

    // Sync pending Alipay orders (only if configured)
    if (isAlipayConfigured())
    {
        ++(*successCount);
        syncPendingAlipayOrders(failedCount);
    }
    else
    {
        LOG_DEBUG << "Alipay not configured, skipping Alipay order reconciliation";
    }

    // Sync pending refunds (only if WeChat is configured)
    if (isWeChatConfigured())
    {
        ++(*successCount);
        syncPendingRefunds(failedCount);
    }
    else
    {
        LOG_DEBUG << "WeChat Pay not configured, skipping refund reconciliation";
    }

    // Garbage-collect expired idempotency rows so pay_idempotency does not grow
    // unbounded (P2). The read path already filters expired rows out, but they
    // still accumulate on disk; this periodic purge reclaims them. Fire-and-
    // forget: GC failures must not block reconciliation.
    // Batch GC (raw-SQL exemption #3): server-side NOW() comparison over the
    // full table; Mapper deleteBy would depend on the client-side clock.
    if (dbClient_)
    {
        dbClient_->execSqlAsync(
          "DELETE FROM pay_idempotency WHERE expire_at IS NOT NULL AND expire_at < NOW()",
          [](const drogon::orm::Result &r) {
              if (r.affectedRows() > 0)
              {
                  LOG_INFO << "[ReconciliationService] Purged " << r.affectedRows()
                           << " expired idempotency rows";
              }
          },
          [](const drogon::orm::DrogonDbException &e) {
              LOG_WARN << "[ReconciliationService] Idempotency purge error: " << e.base().what();
          }
        );
    }

    // NOTE: callback fires when reconcile sweeps are dispatched, not completed.
    // success = number of sweeps triggered; failed = dispatch-time failures
    // (Mapper construction). Per-sweep async results are logged; failure counts
    // feed monitoring via logs.
    callback(*successCount, *failedCount);
}

bool ReconciliationService::isWeChatConfigured() const
{
    // Check if WeChat channel has valid configuration (not placeholders)
    auto channel = findChannel("wechat");
    return channel && channel->isConfigured();
}

bool ReconciliationService::isAlipayConfigured() const
{
    // Check if Alipay channel has valid configuration (not placeholders)
    auto channel = findChannel("alipay");
    return channel && channel->isConfigured();
}

void ReconciliationService::syncPendingWeChatOrders(const std::shared_ptr<int> &failedCount)
{
    if (!dbClient_)
    {
        return;
    }

    auto wechatChannel = findChannel("wechat");
    if (!wechatChannel)
    {
        return;
    }

    try
    {
        // Sweep both PAYING and CREATED orders. CREATED is included so that an
        // order whose third-party trade was created but whose local status update
        // failed (partial failure between channel call and DB write) is recovered;
        // the created_at filter avoids racing with an in-flight create request.
        // The cutoff is computed client-side (was DB-side NOW() - INTERVAL
        // '5 minutes'); second-level clock skew between app and DB is acceptable.
        const auto cutoff = trantor::Date::now().after(-300.0);
        orm::Mapper<PayOrderModel> orderMapper(dbClient_);
        orderMapper.orderBy(PayOrderModel::Cols::_updated_at, orm::SortOrder::DESC)
          .limit(reconcileBatchSize_)
          .findBy(
            (orm::Criteria(
               PayOrderModel::Cols::_status,
               orm::CompareOperator::In,
               std::vector<std::string>{"PAYING", "CREATED"}
             ) &&
             orm::Criteria(PayOrderModel::Cols::_channel, orm::CompareOperator::EQ, "wechat")) &&
              (orm::Criteria(PayOrderModel::Cols::_status, orm::CompareOperator::EQ, "PAYING") ||
               orm::Criteria(PayOrderModel::Cols::_created_at, orm::CompareOperator::LT, cutoff)),
            [this, wechatChannel](const std::vector<PayOrderModel> &rows) {
                for (const auto &row : rows)
                {
                    const std::string orderNo = row.getValueOfOrderNo();
                    // The row's own deadline, read as a nullable pointer: an
                    // order created without `time_expire` carries no expiry the
                    // sweep may judge against.
                    const auto expireAt = row.getExpireAt();
                    wechatChannel->queryPayment(
                      orderNo,
                      [this,
                       wechatChannel,
                       orderNo,
                       expireAt](const Json::Value &result, const std::string &error) {
                          if (!error.empty())
                          {
                              LOG_WARN << "WeChat query failed for order " << orderNo << ": "
                                       << error;
                              return;
                          }
                          paymentService_->syncOrderStatusFromWechat(
                            orderNo, result, [](const std::string &) {}
                          );
                          // An unpaid trade whose deadline has passed is still
                          // pay-able on the channel until WeChat's own lazy
                          // expiry runs, so the sweep closes it: `trade_state`
                          // asks the question, `expire_at` answers it, and only
                          // both together start the close. `NOTPAY` is the one
                          // state that is unpaid on the channel and unsettled
                          // locally -- `CLOSED`/`REVOKED` answers are already
                          // booked terminal by the sync above, and a paid trade
                          // is settled there too, so a close that follows is
                          // answered by the refusal and changes nothing.
                          const std::string tradeState = result.get("trade_state", "").asString();
                          const bool expired = expireAt && (*expireAt < trantor::Date::now());
                          if (tradeState == "NOTPAY" && expired)
                          {
                              wechatChannel->closeOrder(
                                orderNo,
                                [orderNo](const Json::Value &, const std::string &closeError) {
                                    if (!closeError.empty())
                                    {
                                        LOG_INFO << "WeChat close for expired unpaid order "
                                                 << orderNo << " was refused: " << closeError;
                                    }
                                    else
                                    {
                                        LOG_DEBUG << "Closed expired unpaid order " << orderNo
                                                  << " on the channel; the next sweep books it "
                                                     "CLOSED";
                                    }
                                }
                              );
                          }
                      }
                    );
                }
            },
            [](const orm::DrogonDbException &e) {
                LOG_ERROR << "WeChat reconcile query error: " << e.base().what();
            }
          );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[ReconciliationService] Mapper construction failed: " << e.what();
        ++(*failedCount);
    }
    catch (...)
    {
        LOG_ERROR << "[ReconciliationService] Mapper construction failed: unknown exception";
        ++(*failedCount);
    }
}

void ReconciliationService::syncPendingAlipayOrders(const std::shared_ptr<int> &failedCount)
{
    if (!dbClient_)
    {
        return;
    }

    auto alipayChannel = findChannel("alipay");
    if (!alipayChannel)
    {
        return;
    }

    try
    {
        // Sweep both PAYING and CREATED orders (see WeChat path comment); the
        // created_at filter avoids racing with an in-flight create request.
        // Client-side cutoff replaces DB-side NOW(); second-level skew is fine.
        const auto cutoff = trantor::Date::now().after(-300.0);
        orm::Mapper<PayOrderModel> orderMapper(dbClient_);
        orderMapper.orderBy(PayOrderModel::Cols::_updated_at, orm::SortOrder::DESC)
          .limit(reconcileBatchSize_)
          .findBy(
            (orm::Criteria(
               PayOrderModel::Cols::_status,
               orm::CompareOperator::In,
               std::vector<std::string>{"PAYING", "CREATED"}
             ) &&
             orm::Criteria(PayOrderModel::Cols::_channel, orm::CompareOperator::EQ, "alipay")) &&
              (orm::Criteria(PayOrderModel::Cols::_status, orm::CompareOperator::EQ, "PAYING") ||
               orm::Criteria(PayOrderModel::Cols::_created_at, orm::CompareOperator::LT, cutoff)),
            [this, alipayChannel](const std::vector<PayOrderModel> &rows) {
                for (const auto &row : rows)
                {
                    const std::string orderNo = row.getValueOfOrderNo();
                    alipayChannel->queryPayment(
                      orderNo,
                      [this, orderNo](const Json::Value &result, const std::string &error) {
                          if (!error.empty())
                          {
                              LOG_WARN << "Alipay query failed for order " << orderNo << ": "
                                       << error;
                              return;
                          }
                          // Sync order status from Alipay response
                          paymentService_->syncOrderStatusFromAlipay(
                            orderNo, result, [orderNo](const std::string &status) {
                                if (!status.empty())
                                {
                                    LOG_DEBUG
                                      << "[ReconciliationService] Order status synced: order_no="
                                      << orderNo << ", status=" << status << ", source=alipay";
                                }
                                else
                                {
                                    LOG_DEBUG << "[ReconciliationService] Order " << orderNo
                                              << " status unchanged (no sync needed)";
                                }
                            }
                          );
                      }
                    );
                }
            },
            [](const orm::DrogonDbException &e) {
                LOG_ERROR << "Alipay reconcile query error: " << e.base().what();
            }
          );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[ReconciliationService] Mapper construction failed: " << e.what();
        ++(*failedCount);
    }
    catch (...)
    {
        LOG_ERROR << "[ReconciliationService] Mapper construction failed: unknown exception";
        ++(*failedCount);
    }
}

void ReconciliationService::syncPendingRefunds(const std::shared_ptr<int> &failedCount)
{
    // Refund reconciliation currently sweeps WeChat refunds only.
    auto wechatChannel = findChannel("wechat");
    if (!dbClient_ || !wechatChannel)
    {
        return;
    }

    try
    {
        orm::Mapper<PayRefundModel> refundMapper(dbClient_);
        refundMapper.orderBy(PayRefundModel::Cols::_updated_at, orm::SortOrder::DESC)
          .limit(reconcileBatchSize_)
          .findBy(
            orm::Criteria(
              PayRefundModel::Cols::_status,
              orm::CompareOperator::In,
              std::vector<std::string>{"REFUND_INIT", "REFUNDING"}
            ),
            [this, wechatChannel](const std::vector<PayRefundModel> &rows) {
                for (const auto &row : rows)
                {
                    const std::string refundNo = row.getValueOfRefundNo();
                    wechatChannel->queryRefund(
                      refundNo,
                      [this, refundNo](const Json::Value &result, const std::string &error) {
                          if (!error.empty())
                          {
                              LOG_WARN << "Wechat refund query failed for " << refundNo << ": "
                                       << error;
                              return;
                          }
                          refundService_->syncRefundStatusFromWechat(
                            refundNo, result, [](const std::string &) {}
                          );
                      }
                    );
                }
            },
            [](const orm::DrogonDbException &e) {
                LOG_ERROR << "WeChat refund reconcile query error: " << e.base().what();
            }
          );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[ReconciliationService] Mapper construction failed: " << e.what();
        ++(*failedCount);
    }
    catch (...)
    {
        LOG_ERROR << "[ReconciliationService] Mapper construction failed: unknown exception";
        ++(*failedCount);
    }
}
