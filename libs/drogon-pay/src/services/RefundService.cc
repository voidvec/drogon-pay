#include "RefundService.h"
#include "../models/PayRefund.h"
#include "../models/PayOrder.h"
#include "../models/PayPayment.h"
#include "../models/PayLedger.h"
#include "../utils/PayUtils.h"
#include "../utils/OnceCallback.h"
#include <drogon/drogon.h>
#include <random>
#include <sstream>
#include <iomanip>

using namespace drogon;
using namespace drogon::orm;

// Model type aliases for convenience
namespace
{
using PayRefundModel = drogon_model::pay_test::PayRefund;
using PayOrderModel = drogon_model::pay_test::PayOrder;
using PayPaymentModel = drogon_model::pay_test::PayPayment;
using PayLedgerModel = drogon_model::pay_test::PayLedger;
}  // namespace

namespace
{
// Reports a synchronous Mapper construction failure through the shared callback.
void reportMapperFailure(
  const std::shared_ptr<RefundService::RefundCallback> &sharedCb,
  const std::string &what
)
{
    LOG_ERROR << "[RefundService] Mapper construction failed: " << what;
    if (sharedCb && *sharedCb)
    {
        Json::Value error;
        error["code"] = 1500;
        error["message"] = std::string("Database error: ") + what;
        (*sharedCb)(error, std::error_code(1500, std::system_category()));
    }
}

// Only a refund that certainly did not happen may be booked terminal. A
// REFUND_FAIL on an outcome we do not know invites a retry under a fresh
// out_refund_no, which WeChat would honour as a second refund.
//   - a local channel fault (missing config, client not ready) never sent a
//     request, so nothing happened;
//   - `HTTP 4xx: <code> <message>` is WeChat refusing the refund -- safe to
//     read only because the channel verifies the answer signature over an
//     error body as well as over a success one;
//   - a 5xx, a 4xx with no error envelope (an intermediary answered for us),
//     a timeout, a transport failure, an unparseable body or an answer that
//     failed to verify says nothing either way -- reconciliation decides those
//     from the channel's answer. An unverifiable answer is NOT a local fault:
//     the request left and something answered, so booking terminal FAIL here
//     would invite a retry under a fresh out_refund_no that WeChat honours as
//     a second refund.
bool refundCertainlyDidNotHappen(const std::string &error)
{
    const bool httpRefusal =
      error.rfind("HTTP 4", 0) == 0 && error.find("no error envelope") == std::string::npos;
    if (httpRefusal)
    {
        return true;
    }
    const bool wentThroughHttp = error.rfind("HTTP ", 0) == 0 ||
                                 error.rfind("http request", 0) == 0 ||
                                 error == "invalid json response" ||
                                 error.rfind("response signature verification failed", 0) == 0;
    return !wentThroughHttp;
}

// TODO(dedup): duplicated in PaymentService.cc and CallbackService.cc.
// Extract to PayUtils.h/cc in a future refactoring iteration.
void insertLedgerEntry(
  const std::shared_ptr<DbClient> &dbClient,
  int64_t userId,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &entryType,
  const std::string &amount
)
{
    if (!dbClient)
    {
        return;
    }

    auto insertRow = [dbClient, userId, orderNo, paymentNo, entryType, amount]() {
        PayLedgerModel ledger;
        ledger.setUserId(userId);
        ledger.setOrderNo(orderNo);
        if (paymentNo.empty())
        {
            ledger.setPaymentNoToNull();
        }
        else
        {
            ledger.setPaymentNo(paymentNo);
        }
        ledger.setEntryType(entryType);
        ledger.setAmount(amount);
        ledger.setCreatedAt(trantor::Date::now());

        try
        {
            Mapper<PayLedgerModel> ledgerMapper(dbClient);
            ledgerMapper.insert(
              ledger,
              [](const PayLedgerModel &) {},
              [](const DrogonDbException &e) {
                  LOG_WARN << "Ledger insert error: " << e.base().what();
              }
            );
        }
        catch (const std::exception &e)
        {
            LOG_WARN << "[RefundService] Mapper construction failed: " << e.what();
        }
        catch (...)
        {
            LOG_WARN << "[RefundService] Mapper construction failed: unknown exception";
        }
    };

    if (orderNo.empty() || entryType.empty())
    {
        insertRow();
        return;
    }

    if (paymentNo.empty())
    {
        try
        {
            Mapper<PayLedgerModel> ledgerLookup(dbClient);
            ledgerLookup.limit(1).findBy(
              Criteria(PayLedgerModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
                Criteria(PayLedgerModel::Cols::_entry_type, CompareOperator::EQ, entryType) &&
                Criteria(PayLedgerModel::Cols::_payment_no, CompareOperator::IsNull),
              [insertRow](const std::vector<PayLedgerModel> &rows) {
                  if (rows.empty())
                  {
                      insertRow();
                  }
              },
              [](const DrogonDbException &e) {
                  LOG_WARN << "Ledger lookup error: " << e.base().what();
              }
            );
        }
        catch (const std::exception &e)
        {
            LOG_WARN << "[RefundService] Mapper construction failed: " << e.what();
        }
        catch (...)
        {
            LOG_WARN << "[RefundService] Mapper construction failed: unknown exception";
        }
        return;
    }

    try
    {
        Mapper<PayLedgerModel> ledgerLookup(dbClient);
        ledgerLookup.limit(1).findBy(
          Criteria(PayLedgerModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
            Criteria(PayLedgerModel::Cols::_entry_type, CompareOperator::EQ, entryType) &&
            Criteria(PayLedgerModel::Cols::_payment_no, CompareOperator::EQ, paymentNo),
          [insertRow](const std::vector<PayLedgerModel> &rows) {
              if (rows.empty())
              {
                  insertRow();
              }
          },
          [](const DrogonDbException &e) { LOG_WARN << "Ledger lookup error: " << e.base().what(); }
        );
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "[RefundService] Mapper construction failed: " << e.what();
    }
    catch (...)
    {
        LOG_WARN << "[RefundService] Mapper construction failed: unknown exception";
    }
}

std::string toRfc3339Utc(const trantor::Date &when)
{
    const auto seconds = static_cast<time_t>(when.microSecondsSinceEpoch() / 1000000);
    std::tm tmUtc{};
#ifdef _WIN32
    gmtime_s(&tmUtc, &seconds);
#else
    gmtime_r(&seconds, &tmUtc);
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmUtc) == 0)
    {
        return {};
    }
    return buffer;
}

std::string toJsonString(const Json::Value &json)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}
}  // namespace

RefundService::RefundService(
  std::map<std::string, drogon_pay::PaymentChannelPtr> channels,
  std::shared_ptr<drogon::orm::DbClient> dbClient,
  std::shared_ptr<IdempotencyService> idempotencyService
)
    : channels_(std::move(channels)), dbClient_(dbClient), idempotencyService_(idempotencyService)
{
}

drogon_pay::PaymentChannelPtr RefundService::findChannel(const std::string &name) const
{
    auto it = channels_.find(name);
    return (it != channels_.end()) ? it->second : nullptr;
}

void RefundService::createRefund(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  RefundCallback &&callback
)
{
    // Calculate request hash for idempotency
    Json::Value reqJson;
    reqJson["order_no"] = request.orderNo;
    reqJson["amount"] = request.amount;
    reqJson["reason"] = request.reason;
    const std::string requestStr = pay::utils::toJsonString(reqJson);

    // Use SHA-256 for cryptographic hashing (more secure than std::hash)
    std::string requestHash = drogon::utils::getSha256(requestStr);

    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Wrap callback to save response to idempotency cache before calling user callback
    auto wrappedCallback = [this,
                            idempotencyKey,
                            requestHash,
                            sharedCb](const Json::Value &response, const std::error_code &error) {
        LOG_DEBUG << "[RefundService] wrappedCallback: key=" << idempotencyKey
                  << ", error=" << error.value() << ", has_data=" << response.isMember("data");
        // Persist the idempotency snapshot BEFORE responding to the caller.
        // Previously the snapshot write was dispatched asynchronously and the
        // HTTP response was sent immediately, so a retry arriving before the
        // write landed observed an in-progress (NULL snapshot) reservation and
        // was rejected as InProgress. Awaiting the write collapses that window.
        //
        // Store the snapshot whenever the response carries data, regardless of
        // error_code — soft errors (e.g. 1502) still produce a replay-safe
        // response. Hard errors (no data) already had their reservation
        // released by proceedCb, so they fall through to a direct response.
        // (B1-1 follow-up)
        if (!idempotencyKey.empty() && response.isMember("data"))
        {
            LOG_DEBUG << "[RefundService] Saving idempotency snapshot for key=" << idempotencyKey;
            idempotencyService_->updateResult(
              idempotencyKey,
              requestHash,
              response,
              [this, idempotencyKey, requestHash, sharedCb, response, error](bool success) mutable {
                  if (success)
                  {
                      LOG_DEBUG << "[RefundService] Idempotency snapshot saved successfully";
                      if (*sharedCb)
                      {
                          (*sharedCb)(response, error);
                      }
                      return;
                  }
                  // The snapshot write failed: the reservation would otherwise
                  // stay stuck with a NULL snapshot and poison every retry with
                  // InProgress until the TTL expires. Release it so the next
                  // retry can re-attempt the refund instead. (B1-1 follow-up)
                  LOG_ERROR << "[RefundService] Failed to save idempotency snapshot; clearing "
                               "reservation for key="
                            << idempotencyKey;
                  idempotencyService_->clearReservation(
                    idempotencyKey, requestHash, [sharedCb, response, error](bool) {
                        if (*sharedCb)
                        {
                            (*sharedCb)(response, error);
                        }
                    }
                  );
              }
            );
            return;
        }
        // No snapshot to store (empty key or no data). Respond directly.
        if (*sharedCb)
        {
            (*sharedCb)(response, error);
        }
    };
    auto wrappedSharedCb = std::make_shared<RefundCallback>(std::move(wrappedCallback));

    // Skip idempotency check for empty key (used in tests)
    if (idempotencyKey.empty())
    {
        LOG_DEBUG << "[RefundService] Empty idempotency key, skipping check";
        proceedRefund(request, idempotencyKey, requestHash, std::move(*wrappedSharedCb));
        return;
    }

    // Check idempotency
    idempotencyService_->checkAndSet(
      idempotencyKey,
      requestHash,
      [&request]() {
          Json::Value req;
          req["order_no"] = request.orderNo;
          req["amount"] = request.amount;
          req["reason"] = request.reason;
          return req;
      }(),
      [this, request, idempotencyKey, requestHash, wrappedSharedCb](
        bool canProceed, const Json::Value &cachedResult
      ) mutable {
          if (!canProceed)
          {
              // Idempotency conflict
              if (*wrappedSharedCb)
              {
                  Json::Value error;
                  error["code"] = 1409;
                  error["message"] = "Idempotency conflict: different parameters for same key";
                  (*wrappedSharedCb)(error, std::error_code(1409, std::system_category()));
              }
              return;
          }

          if (!cachedResult.isNull())
          {
              // Return cached result
              if (*wrappedSharedCb)
              {
                  (*wrappedSharedCb)(cachedResult, std::error_code());
              }
              return;
          }

          // Proceed with refund creation. Wrap the callback so that if the
          // operation fails after the key was reserved, the in-flight
          // reservation is released (preventing key poisoning on retry).
          // NOTE: Only clear reservation for "hard" errors (no data).
          // "Soft" errors (e.g., WeChat code=1501/1502) still produce a
          // usable response that should be cached as an idempotency snapshot.
          // (B1-1 follow-up: error.code() now properly reflects the real
          // error, so the old `error` check would incorrectly clear the
          // reservation even for cacheable error responses.)
          auto proceedCb = [this, idempotencyKey, requestHash, wrappedSharedCb](
                             const Json::Value &response, const std::error_code &error
                           ) {
              if (!idempotencyKey.empty() && error && !response.isMember("data"))
              {
                  idempotencyService_->clearReservation(
                    idempotencyKey, requestHash, [wrappedSharedCb, response, error](bool) {
                        if (*wrappedSharedCb)
                        {
                            (*wrappedSharedCb)(response, error);
                        }
                    }
                  );
                  return;
              }
              if (*wrappedSharedCb)
              {
                  (*wrappedSharedCb)(response, error);
              }
          };
          proceedRefund(request, idempotencyKey, requestHash, std::move(proceedCb));
      }
    );
}

void RefundService::proceedRefund(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Enhanced input validation
    // Validate reason field (only check max length, allow empty)
    if (request.reason.size() > 80)
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "reason too long (max 80 characters)";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate order_no format (basic check)
    if (request.orderNo.empty() || request.orderNo.length() > 64)
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "invalid order_no format";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate amount format
    if (request.amount.empty())
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "amount cannot be empty";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate funds_account
    if (
      !request.fundsAccount.empty() && request.fundsAccount != "AVAILABLE" &&
      request.fundsAccount != "UNSETTLED"
    )
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "invalid funds_account (must be AVAILABLE or UNSETTLED)";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate notify_url (shared helper, also used by PaymentService).
    if (!request.notifyUrl.empty())
    {
        std::string urlError;
        if (!pay::utils::validateNotifyUrl(request.notifyUrl, urlError))
        {
            if (*sharedCb)
            {
                Json::Value error;
                error["code"] = 1400;
                error["message"] = urlError;
                (*sharedCb)(error, std::error_code(1400, std::system_category()));
            }
            return;
        }
    }

    const std::string refundNo =
      request.refundNo.empty() ? drogon::utils::getUuid() : request.refundNo;
    const std::string &orderNo = request.orderNo;
    const std::string &amount = request.amount;
    const std::string &reason = request.reason;
    const std::string &paymentNo = request.paymentNo;

    // If paymentNo not provided, find the payment of this order that carries the
    // money. A QR order keeps one row per precreate attempt, which gives two
    // rules. Closed attempts (FAIL) are dropped, so a row the channel refused is
    // never the one WeChat is asked to refund. And a settled attempt beats a newer
    // open one: an attempt whose channel answer never arrived can sit at INIT
    // beside a later attempt the callback paid, so ordering by creation time alone
    // picks the zombie and answers "payment not successful" for an order whose
    // money really landed. The open statuses are kept as the fallback so an order
    // that is genuinely unpaid still gets the 1409 it got before, rather than a
    // 1404 that would say the order has no payment at all.
    if (paymentNo.empty())
    {
        auto answerWith =
          [this, request, idempotencyKey, requestHash, refundNo, orderNo, amount, reason, sharedCb](
            const std::string &chosenPaymentNo
          ) mutable {
              CreateRefundRequest newRequest = request;
              newRequest.paymentNo = chosenPaymentNo;
              proceedRefund(newRequest, idempotencyKey, requestHash, std::move(*sharedCb));
          };
        // Behind a shared_ptr so each attempt of the lookup holds the same object
        // and exactly one of them can consume the callback.
        auto proceedWith = std::make_shared<decltype(answerWith)>(std::move(answerWith));

        auto attemptsFinder = [this, orderNo, sharedCb, proceedWith](
                                std::vector<std::string> statuses, std::function<void()> thenEmpty
                              ) -> std::function<void()> {
            return [this, orderNo, sharedCb, proceedWith, statuses, thenEmpty]() {
                try
                {
                    Mapper<PayPaymentModel> paymentMapper(dbClient_);
                    auto payCriteria =
                      Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
                      Criteria(PayPaymentModel::Cols::_status, CompareOperator::In, statuses);
                    paymentMapper.orderBy(PayPaymentModel::Cols::_created_at, SortOrder::DESC)
                      .limit(1)
                      .findBy(
                        payCriteria,
                        [proceedWith, thenEmpty](const std::vector<PayPaymentModel> &rows) {
                            if (rows.empty())
                            {
                                thenEmpty();
                                return;
                            }
                            (*proceedWith)(rows.front().getValueOfPaymentNo());
                        },
                        [sharedCb](const DrogonDbException &e) mutable {
                            if (*sharedCb)
                            {
                                Json::Value error;
                                error["code"] = 1500;
                                error["message"] =
                                  std::string("Database error: ") + e.base().what();
                                (*sharedCb)(error, std::error_code(1500, std::system_category()));
                            }
                        }
                      );
                }
                catch (const std::exception &e)
                {
                    reportMapperFailure(sharedCb, e.what());
                }
                catch (...)
                {
                    reportMapperFailure(sharedCb, "unknown exception");
                }
            };
        };

        // Neither an attempt that holds money nor one still in flight: the order
        // has no refundable payment row at all (or only closed attempts).
        auto reportNoAttempt = [sharedCb]() {
            if (*sharedCb)
            {
                Json::Value error;
                error["code"] = 1404;
                error["message"] = "Payment not found";
                (*sharedCb)(error, std::error_code(1404, std::system_category()));
            }
        };
        auto askOpenAttempts = attemptsFinder({"INIT", "PROCESSING"}, reportNoAttempt);
        attemptsFinder({"SUCCESS", "REFUNDED"}, [askOpenAttempts]() { askOpenAttempts(); })();
        return;
    }

    // Validate payment exists and is successful
    try
    {
        Mapper<PayPaymentModel> paymentValidateMapper(dbClient_);
        auto paymentCriteria =
          Criteria(PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo) &&
          Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo);
        paymentValidateMapper.findOne(
          paymentCriteria,
          [this,
           request,
           idempotencyKey,
           requestHash,
           refundNo,
           orderNo,
           paymentNo,
           amount,
           reason,
           sharedCb](const PayPaymentModel &payment) mutable {
              if (payment.getValueOfStatus() != "SUCCESS")
              {
                  if (*sharedCb)
                  {
                      Json::Value error;
                      error["code"] = 1409;
                      error["message"] = "payment not successful";
                      (*sharedCb)(error, std::error_code(1409, std::system_category()));
                  }
                  return;
              }
              proceedOrderFlow(
                request,
                idempotencyKey,
                requestHash,
                refundNo,
                orderNo,
                paymentNo,
                amount,
                reason,
                std::move(*sharedCb)
              );
          },
          [sharedCb](const DrogonDbException &e) mutable {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1404;
                  error["message"] = std::string("Payment not found: ") + e.base().what();
                  (*sharedCb)(error, std::error_code(1404, std::system_category()));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        reportMapperFailure(sharedCb, e.what());
    }
    catch (...)
    {
        reportMapperFailure(sharedCb, "unknown exception");
    }
}

void RefundService::proceedOrderFlow(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    try
    {
        Mapper<PayOrderModel> orderMapper(dbClient_);
        auto criteria = Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
        orderMapper.findOne(
          criteria,
          [this,
           request,
           idempotencyKey,
           requestHash,
           refundNo,
           orderNo,
           paymentNo,
           amount,
           reason,
           sharedCb](const PayOrderModel &order) mutable {
              const std::string orderAmount = order.getValueOfAmount();
              const std::string currency = order.getValueOfCurrency();
              const std::string orderStatus = order.getValueOfStatus();

              if (orderStatus != "PAID")
              {
                  if (*sharedCb)
                  {
                      Json::Value error;
                      error["code"] = 1409;
                      error["message"] = "order not paid";
                      (*sharedCb)(error, std::error_code(1409, std::system_category()));
                  }
                  return;
              }

              int64_t refundFen = 0;
              int64_t totalFen = 0;
              if (
                !pay::utils::parseAmountToFen(amount, refundFen) ||
                !pay::utils::parseAmountToFen(orderAmount, totalFen)
              )
              {
                  if (*sharedCb)
                  {
                      Json::Value error;
                      error["code"] = 1400;
                      error["message"] = "Invalid amount format";
                      (*sharedCb)(error, std::error_code(1400, std::system_category()));
                  }
                  return;
              }
              if (refundFen <= 0 || refundFen > totalFen)
              {
                  if (*sharedCb)
                  {
                      Json::Value error;
                      error["code"] = 1400;
                      error["message"] = "Invalid refund amount";
                      (*sharedCb)(error, std::error_code(1400, std::system_category()));
                  }
                  return;
              }

              proceedWithAmountCheck(
                request,
                idempotencyKey,
                requestHash,
                refundNo,
                orderNo,
                paymentNo,
                amount,
                refundFen,
                totalFen,
                currency,
                reason,
                std::move(*sharedCb)
              );
          },
          [sharedCb](const DrogonDbException &e) mutable {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1404;
                  error["message"] = std::string("Order not found: ") + e.base().what();
                  (*sharedCb)(error, std::error_code(1404, std::system_category()));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        reportMapperFailure(sharedCb, e.what());
    }
    catch (...)
    {
        reportMapperFailure(sharedCb, "unknown exception");
    }
}

void RefundService::proceedWithAmountCheck(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Check for already successful refund with same details.
    // NOTE: This check runs OUTSIDE the eventual transaction. There is a
    // theoretical window between this read and the FOR-UPDATE transaction
    // in proceedWithInsert where another concurrent request could insert
    // a duplicate refund. The SUM check inside the transaction provides
    // atomic safety — if a duplicate slips past this pre-check, the
    // transaction will detect it via (refundedFen + refundFen > totalFen)
    // and rollback. (Tracked as a future optimization: moving this check
    // inside the transaction would save one network round-trip.)
    try
    {
        Mapper<PayRefundModel> refundMapper(dbClient_);
        refundMapper.orderBy(PayRefundModel::Cols::_updated_at, SortOrder::DESC)
          .limit(1)
          .findBy(
            Criteria(PayRefundModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
              Criteria(PayRefundModel::Cols::_payment_no, CompareOperator::EQ, paymentNo) &&
              Criteria(PayRefundModel::Cols::_amount, CompareOperator::EQ, amount) &&
              Criteria(PayRefundModel::Cols::_status, CompareOperator::EQ, "REFUND_SUCCESS"),
            [this,
             request,
             idempotencyKey,
             requestHash,
             refundNo,
             orderNo,
             paymentNo,
             amount,
             refundFen,
             totalFen,
             currency,
             reason,
             sharedCb](const std::vector<PayRefundModel> &rows) mutable {
                if (!rows.empty())
                {
                    if (*sharedCb)
                    {
                        const auto &row = rows.front();
                        Json::Value response;
                        response["code"] = 0;
                        response["message"] = "Refund already successful";
                        Json::Value data;
                        data["refund_no"] = row.getValueOfRefundNo();
                        data["order_no"] = row.getValueOfOrderNo();
                        data["payment_no"] = paymentNo;
                        data["refund_amount"] = amount;
                        data["status"] = row.getValueOfStatus();
                        if (row.getChannelRefundNo())
                        {
                            data["channel_refund_no"] = row.getValueOfChannelRefundNo();
                        }
                        response["data"] = data;
                        (*sharedCb)(response, std::error_code());
                    }
                    return;
                }
                proceedWithInProgressCheck(
                  request,
                  idempotencyKey,
                  requestHash,
                  refundNo,
                  orderNo,
                  paymentNo,
                  amount,
                  refundFen,
                  totalFen,
                  currency,
                  reason,
                  std::move(*sharedCb)
                );
            },
            [sharedCb](const DrogonDbException &e) {
                if (*sharedCb)
                {
                    Json::Value error;
                    error["code"] = 1500;
                    error["message"] = std::string("Database error: ") + e.base().what();
                    (*sharedCb)(error, std::error_code(1500, std::system_category()));
                }
            }
          );
    }
    catch (const std::exception &e)
    {
        reportMapperFailure(sharedCb, e.what());
    }
    catch (...)
    {
        reportMapperFailure(sharedCb, "unknown exception");
    }
}

void RefundService::proceedWithInProgressCheck(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Check for refund already in progress
    try
    {
        Mapper<PayRefundModel> refundCounter(dbClient_);
        refundCounter.count(
          Criteria(PayRefundModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
            Criteria(PayRefundModel::Cols::_payment_no, CompareOperator::EQ, paymentNo) &&
            Criteria(PayRefundModel::Cols::_amount, CompareOperator::EQ, amount) &&
            Criteria(
              PayRefundModel::Cols::_status,
              CompareOperator::In,
              std::vector<std::string>{"REFUND_INIT", "REFUNDING"}
            ),
          [this,
           request,
           idempotencyKey,
           requestHash,
           refundNo,
           orderNo,
           paymentNo,
           amount,
           refundFen,
           totalFen,
           currency,
           reason,
           sharedCb](const size_t count) mutable {
              if (count > 0)
              {
                  if (*sharedCb)
                  {
                      Json::Value error;
                      error["code"] = 1409;
                      error["message"] = "refund already in progress";
                      (*sharedCb)(error, std::error_code(1409, std::system_category()));
                  }
                  return;
              }
              proceedWithInsert(
                request,
                idempotencyKey,
                requestHash,
                refundNo,
                orderNo,
                paymentNo,
                amount,
                refundFen,
                totalFen,
                currency,
                reason,
                std::move(*sharedCb)
              );
          },
          [sharedCb](const DrogonDbException &e) {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1500;
                  error["message"] = std::string("Database error: ") + e.base().what();
                  (*sharedCb)(error, std::error_code(1500, std::system_category()));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        reportMapperFailure(sharedCb, e.what());
    }
    catch (...)
    {
        reportMapperFailure(sharedCb, "unknown exception");
    }
}

void RefundService::proceedWithInsert(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Atomically lock the payment row, check the cumulative refunded amount, and
    // insert the refund row (REFUND_INIT) inside one transaction. The row-level
    // lock (SELECT ... FOR UPDATE) serializes concurrent refund attempts on the
    // same payment so the SUM check + insert pair is race-free (P0-2 fix). The
    // third-party channel call happens AFTER commit (outside the transaction) so
    // the DB transaction stays short and does not block on network I/O.
    dbClient_->newTransactionAsync(
      [this,
       request,
       idempotencyKey,
       requestHash,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       refundFen,
       totalFen,
       currency,
       reason,
       sharedCb](const std::shared_ptr<Transaction> &transPtr) mutable {
          if (!transPtr)
          {
              // Transaction could not be created.
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1500;
                  error["message"] = "Transaction unavailable";
                  (*sharedCb)(error, std::error_code(1500, std::system_category()));
              }
              return;
          }
          auto failDb = [sharedCb, transPtr](const DrogonDbException &e) {
              transPtr->rollback();
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1500;
                  error["message"] = std::string("Database error: ") + e.base().what();
                  (*sharedCb)(error, std::error_code(1500, std::system_category()));
              }
          };

          // 1. Lock the payment row for this order to serialize concurrent refunds.
          try
          {
              Mapper<PayPaymentModel> paymentLock(transPtr);
              paymentLock.forUpdate().findBy(
                Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
                  Criteria(PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo),
                [this,
                 request,
                 idempotencyKey,
                 requestHash,
                 refundNo,
                 orderNo,
                 paymentNo,
                 amount,
                 refundFen,
                 totalFen,
                 currency,
                 reason,
                 sharedCb,
                 transPtr,
                 failDb](const std::vector<PayPaymentModel> &lockResult) mutable {
                    if (lockResult.empty())
                    {
                        transPtr->rollback();
                        if (*sharedCb)
                        {
                            Json::Value error;
                            error["code"] = 1404;
                            error["message"] = "payment not found for order";
                            (*sharedCb)(error, std::error_code(1404, std::system_category()));
                        }
                        return;
                    }

                    // 2. Sum already-refunded amounts under the lock.
                    // Aggregate SUM (raw-SQL exemption #3): the Mapper cannot
                    // express SUM; the query must run inside the row-lock
                    // transaction so the cumulative check stays race-free.
                    transPtr->execSqlAsync(
                      "SELECT COALESCE(SUM(CAST(amount AS NUMERIC)), 0) AS sum_amount "
                      "FROM pay_refund WHERE order_no = $1 "
                      "AND status IN ($2, $3, $4)",
                      [this,
                       request,
                       idempotencyKey,
                       requestHash,
                       refundNo,
                       orderNo,
                       paymentNo,
                       amount,
                       refundFen,
                       totalFen,
                       currency,
                       reason,
                       sharedCb,
                       transPtr,
                       failDb](const Result &r) mutable {
                          int64_t refundedFen = 0;
                          if (!r.empty())
                          {
                              const auto sumText = r.front()["sum_amount"].as<std::string>();
                              if (!pay::utils::parseAmountToFen(sumText, refundedFen))
                              {
                                  transPtr->rollback();
                                  if (*sharedCb)
                                  {
                                      Json::Value error;
                                      error["code"] = 1500;
                                      error["message"] = "Invalid refund sum";
                                      (*sharedCb)(
                                        error, std::error_code(1500, std::system_category())
                                      );
                                  }
                                  return;
                              }
                          }
                          if (refundedFen + refundFen > totalFen)
                          {
                              transPtr->rollback();
                              if (*sharedCb)
                              {
                                  Json::Value error;
                                  error["code"] = 1409;
                                  error["message"] = "refund amount exceeds paid";
                                  (*sharedCb)(error, std::error_code(1409, std::system_category()));
                              }
                              return;
                          }

                          // 3. Insert the refund row (REFUND_INIT) inside the same
                          // transaction so the cumulative check and the insert commit
                          // atomically.
                          try
                          {
                              Mapper<PayRefundModel> refundMapper(transPtr);
                              PayRefundModel refund;
                              refund.setRefundNo(refundNo);
                              refund.setOrderNo(orderNo);
                              refund.setPaymentNo(paymentNo);
                              refund.setStatus("REFUND_INIT");
                              refund.setAmount(amount);
                              refund.setCreatedAt(trantor::Date::now());
                              refundMapper.insert(
                                refund,
                                [this,
                                 request,
                                 idempotencyKey,
                                 requestHash,
                                 refundNo,
                                 orderNo,
                                 paymentNo,
                                 amount,
                                 refundFen,
                                 totalFen,
                                 currency,
                                 reason,
                                 sharedCb,
                                 transPtr](const PayRefundModel &) mutable {
                                    // 4. Look up the channel inside the transaction, then
                                    // commit before any network call.
                                    try
                                    {
                                        Mapper<PayOrderModel> orderMapper(transPtr);
                                        orderMapper.findOne(
                                          Criteria(
                                            PayOrderModel::Cols::_order_no,
                                            CompareOperator::EQ,
                                            orderNo
                                          ),
                                          [this,
                                           request,
                                           idempotencyKey,
                                           requestHash,
                                           refundNo,
                                           orderNo,
                                           paymentNo,
                                           amount,
                                           refundFen,
                                           totalFen,
                                           currency,
                                           reason,
                                           sharedCb,
                                           transPtr](const PayOrderModel &order) mutable {
                                              std::string channel = order.getValueOfChannel();
                                              LOG_DEBUG
                                                << "[RefundService] Processing refund: order_no="
                                                << orderNo << ", payment_no=" << paymentNo
                                                << ", refund_no=" << refundNo
                                                << ", channel=" << channel << ", amount=" << amount;

                                              // Explicit COMMIT (raw-SQL exemption candidate): the
                                              // refund row must be durably committed BEFORE the
                                              // external channel call, and the commit error path
                                              // must run before any side effect. An implicit
                                              // destructor-time commit would race with the network
                                              // call, so the manual COMMIT stays.
                                              transPtr->execSqlAsync(
                                                "COMMIT",
                                                [this,
                                                 request,
                                                 idempotencyKey,
                                                 requestHash,
                                                 refundNo,
                                                 orderNo,
                                                 paymentNo,
                                                 amount,
                                                 refundFen,
                                                 totalFen,
                                                 currency,
                                                 reason,
                                                 channel,
                                                 sharedCb](const Result &) mutable {
                                                    invokeRefundChannel(
                                                      request,
                                                      idempotencyKey,
                                                      requestHash,
                                                      refundNo,
                                                      orderNo,
                                                      paymentNo,
                                                      amount,
                                                      refundFen,
                                                      totalFen,
                                                      currency,
                                                      reason,
                                                      channel,
                                                      std::move(*sharedCb)
                                                    );
                                                },
                                                [sharedCb](const DrogonDbException &e) {
                                                    if (*sharedCb)
                                                    {
                                                        Json::Value error;
                                                        error["code"] = 1500;
                                                        error["message"] =
                                                          std::string("Failed to commit refund: ") +
                                                          e.base().what();
                                                        (*sharedCb)(
                                                          error,
                                                          std::error_code(
                                                            1500, std::system_category()
                                                          )
                                                        );
                                                    }
                                                }
                                              );
                                          },
                                          [sharedCb, transPtr](const DrogonDbException &e) {
                                              transPtr->rollback();
                                              if (*sharedCb)
                                              {
                                                  Json::Value error;
                                                  error["code"] = 1404;
                                                  error["message"] =
                                                    std::string("Order not found: ") +
                                                    e.base().what();
                                                  (*sharedCb)(
                                                    error,
                                                    std::error_code(1404, std::system_category())
                                                  );
                                              }
                                          }
                                        );
                                    }
                                    catch (const std::exception &e)
                                    {
                                        transPtr->rollback();
                                        reportMapperFailure(sharedCb, e.what());
                                    }
                                    catch (...)
                                    {
                                        transPtr->rollback();
                                        reportMapperFailure(sharedCb, "unknown exception");
                                    }
                                },
                                failDb
                              );
                          }
                          catch (const std::exception &e)
                          {
                              transPtr->rollback();
                              reportMapperFailure(sharedCb, e.what());
                          }
                          catch (...)
                          {
                              transPtr->rollback();
                              reportMapperFailure(sharedCb, "unknown exception");
                          }
                      },
                      failDb,
                      orderNo,
                      "REFUND_INIT",
                      "REFUNDING",
                      "REFUND_SUCCESS"
                    );
                },
                failDb
              );
          }
          catch (const std::exception &e)
          {
              transPtr->rollback();
              reportMapperFailure(sharedCb, e.what());
          }
          catch (...)
          {
              transPtr->rollback();
              reportMapperFailure(sharedCb, "unknown exception");
          }
      }
    );
}

void RefundService::invokeRefundChannel(
  const CreateRefundRequest &request,
  const std::string & /*idempotencyKey*/,
  const std::string & /*requestHash*/,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  const std::string &channel,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // The refund row (REFUND_INIT) and channel were already persisted inside the
    // caller's transaction. This method performs only the third-party channel
    // call, which must stay outside the DB transaction to avoid blocking on
    // network I/O. Route to the appropriate payment client based on channel.
    if (channel == "alipay")
    {
        // Alipay refund (payload/response formats stay channel-specific)
        auto alipayChannel = findChannel(channel);
        if (!alipayChannel)
        {
            if (*sharedCb)
            {
                Json::Value error;
                error["code"] = 1501;
                error["message"] = "Alipay client not ready";
                error["data"]["refund_no"] = refundNo;
                error["data"]["order_no"] = orderNo;
                error["data"]["payment_no"] = paymentNo;
                error["data"]["refund_amount"] = amount;
                error["data"]["status"] = "REFUND_FAIL";
                (*sharedCb)(error, std::error_code(1501, std::system_category()));
            }
            return;
        }

        Json::Value payload;
        payload["out_trade_no"] = orderNo;
        payload["refund_amount"] = amount;
        if (!reason.empty())
        {
            payload["refund_reason"] = reason;
        }

        alipayChannel->refund(
          payload,
          [this, refundNo, orderNo, paymentNo, amount, totalFen, sharedCb](
            const Json::Value &result, const std::string &error
          ) mutable {
              if (!error.empty())
              {
                  const std::string errorMessage = "Alipay error: " + error;
                  Json::Value errJson;
                  errJson["error"] = errorMessage;
                  updateRefundWithError(refundNo, errorMessage, errJson);
                  if (*sharedCb)
                  {
                      Json::Value response;
                      response["code"] = 1502;
                      response["message"] = errorMessage;
                      response["data"]["refund_no"] = refundNo;
                      response["data"]["order_no"] = orderNo;
                      response["data"]["payment_no"] = paymentNo;
                      response["data"]["refund_amount"] = amount;
                      response["data"]["status"] = "REFUND_FAIL";
                      response["data"]["error"] = errorMessage;
                      response["data"]["alipay_response"] = errJson;
                      (*sharedCb)(response, std::error_code(1502, std::system_category()));
                  }
                  return;
              }

              // Check Alipay response
              std::string alipayCode = result.get("code", "").asString();
              if (alipayCode != "10000")
              {
                  const std::string errorMessage =
                    "Alipay refund failed: " + result.get("msg", "").asString();
                  updateRefundWithError(refundNo, errorMessage, result);
                  if (*sharedCb)
                  {
                      Json::Value response;
                      response["code"] = 1502;
                      response["message"] = errorMessage;
                      response["data"]["refund_no"] = refundNo;
                      response["data"]["order_no"] = orderNo;
                      response["data"]["payment_no"] = paymentNo;
                      response["data"]["refund_amount"] = amount;
                      response["data"]["status"] = "REFUND_FAIL";
                      (*sharedCb)(response, std::error_code(1502, std::system_category()));
                  }
                  return;
              }

              std::string refundStatus = "REFUND_SUCCESS";
              const std::string refundId = result.get("refund_id", "").asString();
              updateRefundWithSuccess(
                refundNo,
                refundStatus,
                refundId,
                result,
                orderNo,
                paymentNo,
                amount,
                totalFen,
                std::move(*sharedCb)
              );
          }
        );
    }
    else if (channel == "wechat")
    {
        // WeChat refund (payload/response formats stay channel-specific)
        auto wechatChannel = findChannel(channel);
        if (!wechatChannel)
        {
            const std::string errMsg = "wechat client not ready";
            Json::Value errJson;
            errJson["error"] = errMsg;
            updateRefundWithError(refundNo, errMsg, errJson);
            if (*sharedCb)
            {
                Json::Value response;
                response["code"] = 1501;
                response["message"] = "WeChat refund client not configured";
                response["data"]["refund_no"] = refundNo;
                response["data"]["order_no"] = orderNo;
                response["data"]["payment_no"] = paymentNo;
                response["data"]["amount"] = amount;
                response["data"]["status"] = "REFUND_FAIL";
                response["data"]["error"] = errMsg;
                (*sharedCb)(response, std::error_code(1501, std::system_category()));
            }
            return;
        }

        Json::Value payload;
        payload["out_trade_no"] = orderNo;
        payload["out_refund_no"] = refundNo;
        if (!reason.empty())
        {
            payload["reason"] = reason;
        }
        if (!request.notifyUrl.empty())
        {
            payload["notify_url"] = request.notifyUrl;
        }
        if (!request.fundsAccount.empty())
        {
            payload["funds_account"] = request.fundsAccount;
        }
        payload["amount"]["refund"] = static_cast<Json::Int64>(refundFen);
        payload["amount"]["total"] = static_cast<Json::Int64>(totalFen);
        payload["amount"]["currency"] = currency;

        wechatChannel->refund(
          payload,
          [this, refundNo, orderNo, paymentNo, amount, totalFen, sharedCb](
            const Json::Value &result, const std::string &error
          ) mutable {
              if (!error.empty())
              {
                  const std::string errorMessage = "WeChat error: " + error;
                  const bool certainlyNotRefunded = refundCertainlyDidNotHappen(error);
                  Json::Value errJson;
                  errJson["error"] = errorMessage;
                  if (certainlyNotRefunded)
                  {
                      updateRefundWithError(refundNo, errorMessage, errJson);
                  }
                  else
                  {
                      LOG_WARN << "[RefundService] Refund outcome unknown for " << refundNo
                               << " (channel call did not complete): " << errorMessage;
                  }
                  if (*sharedCb)
                  {
                      Json::Value response;
                      response["code"] = 1502;
                      response["message"] = errorMessage;
                      response["data"]["refund_no"] = refundNo;
                      response["data"]["order_no"] = orderNo;
                      response["data"]["payment_no"] = paymentNo;
                      response["data"]["amount"] = amount;
                      response["data"]["status"] =
                        certainlyNotRefunded ? "REFUND_FAIL" : "REFUNDING";
                      response["data"]["error"] = errorMessage;
                      response["data"]["wechat_response"] = errJson;
                      (*sharedCb)(response, std::error_code(1502, std::system_category()));
                  }
                  return;
              }

              // V3 files an accepted refund under SUCCESS or PROCESSING and
              // answers CLOSED / ABNORMAL to say the money did not move.
              // Defaulting an unrecognised body to REFUNDING recorded a refund
              // WeChat never accepted, and routing CLOSED through the success
              // path booked a failed refund as one that succeeded.
              const std::string wechatStatus = result.get("status", "").asString();
              const std::string refundId = result.get("refund_id", "").asString();
              const std::string mappedStatus = pay::utils::mapRefundStatus(wechatStatus);
              if (mappedStatus.empty() || mappedStatus == "REFUND_FAIL")
              {
                  const std::string code = result.get("code", "").asString();
                  const bool definitive = !mappedStatus.empty() || !code.empty();
                  std::string errorMessage = mappedStatus.empty()
                                               ? "WeChat refund response has no usable status"
                                               : "WeChat reported refund status " + wechatStatus;
                  if (!code.empty())
                  {
                      errorMessage += ": " + code + " " + result.get("message", "").asString();
                  }

                  Json::Value errJson;
                  errJson["error"] = errorMessage;
                  if (definitive)
                  {
                      updateRefundWithError(refundNo, errorMessage, errJson);
                  }
                  else
                  {
                      LOG_WARN << "[RefundService] Refund outcome unknown for " << refundNo
                               << " (2xx answer carried no status): " << errorMessage;
                  }
                  if (*sharedCb)
                  {
                      Json::Value response;
                      response["code"] = 1502;
                      response["message"] = errorMessage;
                      response["data"]["refund_no"] = refundNo;
                      response["data"]["order_no"] = orderNo;
                      response["data"]["payment_no"] = paymentNo;
                      response["data"]["amount"] = amount;
                      response["data"]["status"] = definitive ? "REFUND_FAIL" : "REFUNDING";
                      response["data"]["error"] = errorMessage;
                      response["data"]["wechat_response"] = errJson;
                      (*sharedCb)(response, std::error_code(1502, std::system_category()));
                  }
                  return;
              }

              updateRefundWithSuccess(
                refundNo,
                mappedStatus,
                refundId,
                result,
                orderNo,
                paymentNo,
                amount,
                totalFen,
                std::move(*sharedCb)
              );
          }
        );
    }
    else
    {
        // Unknown channel
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1500;
            error["message"] = "Unknown payment channel: " + channel;
            error["data"]["refund_no"] = refundNo;
            error["data"]["order_no"] = orderNo;
            error["data"]["payment_no"] = paymentNo;
            error["data"]["refund_amount"] = amount;
            error["data"]["status"] = "REFUND_FAIL";
            (*sharedCb)(error, std::error_code(1500, std::system_category()));
        }
    }
}

void RefundService::updateRefundWithError(
  const std::string &refundNo,
  const std::string &errorMessage,
  const Json::Value &errJson
)
{
    // Guarded transition: this row can have reached a final status while the
    // channel call was in flight -- a REFUND_SUCCESS booked by the refund
    // notification, for instance. Writing FAIL over that would un-refund money
    // the customer actually received, so the update only fires while the row is
    // still unsettled and zero rows means another writer settled it first.
    try
    {
        Mapper<PayRefundModel> refundUpdater(dbClient_);
        refundUpdater.updateBy(
          {PayRefundModel::Cols::_status, PayRefundModel::Cols::_response_payload},
          [refundNo, errorMessage](const size_t updated) {
              if (updated == 0)
              {
                  LOG_WARN << "[RefundService] Refund " << refundNo
                           << " had already reached a final status, so REFUND_FAIL ("
                           << errorMessage << ") was not written";
              }
          },
          [refundNo](const DrogonDbException &e) {
              LOG_ERROR << "[RefundService] updateRefundWithError DB write failed for refund_no="
                        << refundNo << ": " << e.base().what();
          },
          Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo) &&
            Criteria(
              PayRefundModel::Cols::_status,
              CompareOperator::In,
              std::vector<std::string>{"REFUND_INIT", "REFUNDING"}
            ),
          std::string("REFUND_FAIL"),
          toJsonString(errJson)
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[RefundService] Mapper construction failed: " << e.what();
    }
    catch (...)
    {
        LOG_ERROR << "[RefundService] Mapper construction failed: unknown exception";
    }
}

void RefundService::updateRefundWithSuccess(
  const std::string &refundNo,
  const std::string &refundStatus,
  const std::string &refundId,
  const Json::Value &result,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t orderTotalFen,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    auto respond = [sharedCb, refundNo, orderNo, paymentNo, amount, refundId, result](
                     const std::string &status
                   ) {
        if (!*sharedCb)
        {
            return;
        }
        Json::Value response;
        response["code"] = 0;
        response["message"] = "Refund created successfully";
        Json::Value data;
        data["refund_no"] = refundNo;
        data["order_no"] = orderNo;
        data["payment_no"] = paymentNo;
        data["refund_amount"] = amount;
        data["status"] = status;
        data["channel_refund_no"] = refundId;
        data["wechat_response"] = result;
        response["data"] = data;
        (*sharedCb)(response, std::error_code());
    };

    auto failDb = [sharedCb](const DrogonDbException &e) {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1500;
            error["message"] = std::string("Database error: ") + e.base().what();
            (*sharedCb)(error, std::error_code(1500, std::system_category()));
        }
    };

    // The parent order only moves to REFUNDED once the refund reaches
    // REFUND_SUCCESS (openapi.yaml OrderStatus, TECH_SPECS.md "订单状态机"). WeChat
    // accepts an asynchronous refund with status=PROCESSING, which maps to
    // REFUNDING: the unconditional write this replaces marked the order REFUNDED
    // the moment WeChat acknowledged the request, for money that had not moved.
    // The order write is itself guarded on PAID so it cannot roll back a state
    // another writer (close, refund notification) reached in the meantime.
    auto writeRefundedOrder = [this, sharedCb, respond, failDb, refundNo, refundStatus, orderNo]() {
        try
        {
            Mapper<PayOrderModel> orderUpdater(dbClient_);
            orderUpdater.updateBy(
              {PayOrderModel::Cols::_status},
              [respond, refundStatus, refundNo, orderNo](const size_t updated) {
                  if (updated == 0)
                  {
                      LOG_WARN << "[RefundService] Order " << orderNo
                               << " was no longer PAID when refund " << refundNo
                               << " succeeded; its status was left as it is";
                  }
                  else
                  {
                      LOG_INFO << "[RefundService] Refund completed: refund_no=" << refundNo
                               << ", order_no=" << orderNo << ", status=" << refundStatus;
                  }
                  respond(refundStatus);
              },
              failDb,
              Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
                Criteria(PayOrderModel::Cols::_status, CompareOperator::EQ, "PAID"),
              std::string("REFUNDED")
            );
        }
        catch (const std::exception &e)
        {
            reportMapperFailure(sharedCb, e.what());
        }
        catch (...)
        {
            reportMapperFailure(sharedCb, "unknown exception");
        }
    };

    auto settleOrder = [this,
                        respond,
                        failDb,
                        sharedCb,
                        writeRefundedOrder,
                        refundNo,
                        refundStatus,
                        orderNo,
                        orderTotalFen]() {
        if (refundStatus != "REFUND_SUCCESS")
        {
            LOG_DEBUG << "[RefundService] Refund " << refundNo << " recorded as " << refundStatus
                      << ", leaving order " << orderNo << " untouched";
            respond(refundStatus);
            return;
        }
        // A settled refund says nothing about the refunds around it, and WeChat
        // honours up to fifty of them per order, so REFUND_SUCCESS on one row is
        // not yet evidence that the order came back. The row above already
        // committed this refund's REFUND_SUCCESS, so the sum of the settled rows
        // is everything returned to date. Overstating it would report money back
        // the customer never received; understating leaves an order PAID that a
        // later refund notification or query settles.
        // Aggregate SUM (raw-SQL exemption #3): the Mapper cannot express SUM.
        try
        {
            dbClient_->execSqlAsync(
              "SELECT COALESCE(SUM(CAST(amount AS NUMERIC)), 0) AS sum_amount "
              "FROM pay_refund WHERE order_no = $1 AND status = $2",
              [respond,
               sharedCb,
               writeRefundedOrder,
               refundNo,
               refundStatus,
               orderNo,
               orderTotalFen](const Result &r) {
                  int64_t settledRefundFen = 0;
                  if (!r.empty())
                  {
                      const auto sumText = r.front()["sum_amount"].as<std::string>();
                      if (!pay::utils::parseAmountToFen(sumText, settledRefundFen))
                      {
                          reportMapperFailure(sharedCb, "Invalid settled refund sum");
                          return;
                      }
                  }
                  if (!pay::utils::refundsCoverOrderAmount(settledRefundFen, orderTotalFen))
                  {
                      LOG_DEBUG << "[RefundService] Refund " << refundNo
                                << " settled, but the refunds on order " << orderNo << " ("
                                << settledRefundFen << " fen) do not yet cover its total ("
                                << orderTotalFen << " fen); the order stays as it is";
                      respond(refundStatus);
                      return;
                  }
                  writeRefundedOrder();
              },
              failDb,
              orderNo,
              std::string("REFUND_SUCCESS")
            );
        }
        catch (const std::exception &e)
        {
            reportMapperFailure(sharedCb, e.what());
        }
        catch (...)
        {
            reportMapperFailure(sharedCb, "unknown exception");
        }
    };

    try
    {
        Mapper<PayRefundModel> refundUpdater(dbClient_);
        refundUpdater.updateBy(
          {PayRefundModel::Cols::_status,
           PayRefundModel::Cols::_channel_refund_no,
           PayRefundModel::Cols::_response_payload},
          [settleOrder, refundNo, refundStatus, amount](const size_t updated) {
              if (updated == 0)
              {
                  // The notification (or reconciliation) advanced this row while the
                  // channel call was in flight: that status stands. The order step
                  // below is guarded too, so running it is idempotent.
                  LOG_WARN << "[RefundService] Refund " << refundNo
                           << " had already moved out of REFUND_INIT/REFUNDING, so " << refundStatus
                           << " was not written (amount=" << amount << ")";
              }
              settleOrder();
          },
          failDb,
          Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo) &&
            Criteria(
              PayRefundModel::Cols::_status,
              CompareOperator::In,
              std::vector<std::string>{"REFUND_INIT", "REFUNDING"}
            ),
          refundStatus,
          refundId,
          toJsonString(result)
        );
    }
    catch (const std::exception &e)
    {
        reportMapperFailure(sharedCb, e.what());
    }
    catch (...)
    {
        reportMapperFailure(sharedCb, "unknown exception");
    }
}

void RefundService::queryRefund(const std::string &refundNo, RefundCallback &&callback)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    try
    {
        Mapper<PayRefundModel> refundMapper(dbClient_);
        auto criteria = Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo);
        refundMapper.findOne(
          criteria,
          [this, refundNo, sharedCb](const PayRefundModel &refund) {
              Json::Value response;
              response["code"] = 0;
              response["message"] = "Query refund successful";
              Json::Value data;
              data["refund_no"] = refund.getValueOfRefundNo();
              data["order_no"] = refund.getValueOfOrderNo();
              data["payment_no"] = refund.getValueOfPaymentNo();
              data["status"] = refund.getValueOfStatus();
              data["refund_amount"] = refund.getValueOfAmount();
              data["channel_refund_no"] = refund.getValueOfChannelRefundNo();
              data["updated_at"] = toRfc3339Utc(refund.getValueOfUpdatedAt());
              response["data"] = data;

              // Refund status refresh currently exists for the wechat channel
              // only; without it we simply return the database snapshot.
              auto wechatChannel = findChannel("wechat");
              if (!wechatChannel)
              {
                  if (*sharedCb)
                  {
                      (*sharedCb)(response, std::error_code());
                  }
                  return;
              }

              wechatChannel->queryRefund(
                refundNo,
                [this,
                 refundNo,
                 response,
                 sharedCb](const Json::Value &result, const std::string &error) mutable {
                    if (!error.empty())
                    {
                        if (*sharedCb)
                        {
                            (*sharedCb)(response, std::error_code());
                        }
                        return;
                    }

                    syncRefundStatusFromWechat(
                      refundNo,
                      result,
                      [response, result, sharedCb](const std::string &status) mutable {
                          if (!status.empty())
                          {
                              response["data"]["status"] = status;
                          }
                          response["data"]["wechat_response"] = result;
                          if (*sharedCb)
                          {
                              (*sharedCb)(response, std::error_code());
                          }
                      }
                    );
                }
              );
          },
          [sharedCb](const DrogonDbException &e) {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1404;
                  error["message"] = std::string("Refund not found: ") + e.base().what();
                  (*sharedCb)(error, std::error_code(1404, std::system_category()));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        reportMapperFailure(sharedCb, e.what());
    }
    catch (...)
    {
        reportMapperFailure(sharedCb, "unknown exception");
    }
}

void RefundService::syncRefundStatusFromWechat(
  const std::string &refundNo,
  const Json::Value &result,
  std::function<void(const std::string &status)> &&rawCallback
)
{
    // Once-only wrapper: the payload update, ledger lookup and their error
    // branches run on the same transaction and could otherwise each invoke
    // the callback.
    auto onceCb = pay::utils::makeOnceCallback<void(const std::string &)>(std::move(rawCallback));
    std::function<void(const std::string &)> callback = [onceCb](const std::string &status) {
        onceCb.call(status);
    };

    const std::string wechatStatus = result.get("status", "").asString();
    if (wechatStatus.empty())
    {
        if (callback)
        {
            callback("");
        }
        return;
    }

    const std::string refundStatus = pay::utils::mapRefundStatus(wechatStatus);
    const std::string refundId = result.get("refund_id", "").asString();

    LOG_DEBUG << "Sync refund status from WeChat: refund_no=" << refundNo
              << " wechat_status=" << wechatStatus << " refund_status=" << refundStatus;

    if (refundStatus.empty())
    {
        LOG_WARN << "Unknown refund status from WeChat: " << wechatStatus;
        if (callback)
        {
            callback("");
        }
        return;
    }

    if (!dbClient_)
    {
        if (callback)
        {
            callback(refundStatus);
        }
        return;
    }

    try
    {
        Mapper<PayRefundModel> refundMapper(dbClient_);
        auto criteria = Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo);
        refundMapper.findOne(
          criteria,
          [this, refundStatus, refundId, refundNo, result, callback](PayRefundModel refund) {
              if (refund.getValueOfStatus() == "REFUND_SUCCESS")
              {
                  if (callback)
                  {
                      callback(refundStatus);
                  }
                  return;
              }
              const auto orderNo = refund.getValueOfOrderNo();
              const auto paymentNo = refund.getValueOfPaymentNo();
              const auto refundAmount = refund.getValueOfAmount();

              dbClient_->newTransactionAsync([refundStatus,
                                              refundId,
                                              refundNo,
                                              result,
                                              callback,
                                              refund,
                                              orderNo,
                                              paymentNo,
                                              refundAmount](
                                               const std::shared_ptr<Transaction> &transPtr
                                             ) mutable {
                  auto rollbackDone = [callback, transPtr](const DrogonDbException &e) {
                      LOG_ERROR << "Reconcile refund update error: " << e.base().what();
                      transPtr->rollback();
                      if (callback)
                      {
                          callback("");
                      }
                  };

                  auto transDb = std::static_pointer_cast<DbClient>(transPtr);

                  refund.setStatus(refundStatus);
                  refund.setChannelRefundNo(refundId);
                  try
                  {
                      Mapper<PayRefundModel> refundUpdater(transPtr);
                      refundUpdater.update(
                        refund,
                        [refundStatus,
                         orderNo,
                         paymentNo,
                         refundAmount,
                         refundNo,
                         result,
                         transPtr,
                         transDb,
                         callback](const size_t) {
                            const std::string responsePayload = toJsonString(result);
                            try
                            {
                                Mapper<PayRefundModel> payloadUpdater(transPtr);
                                payloadUpdater.updateBy(
                                  {PayRefundModel::Cols::_response_payload},
                                  [callback, refundStatus, transPtr](const size_t) {
                                      // Last write on the non-ledger path: report
                                      // the synced status. The REFUND_SUCCESS path
                                      // reports from the ledger lookup below.
                                      if (refundStatus != "REFUND_SUCCESS" && callback)
                                      {
                                          callback(refundStatus);
                                      }
                                  },
                                  [callback, transPtr](const DrogonDbException &e) {
                                      LOG_ERROR << "Reconcile refund payload update error: "
                                                << e.base().what();
                                      transPtr->rollback();
                                      if (callback)
                                      {
                                          callback("");
                                      }
                                  },
                                  Criteria(
                                    PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo
                                  ),
                                  responsePayload
                                );
                            }
                            catch (const std::exception &e)
                            {
                                LOG_ERROR << "[RefundService] Mapper construction failed: "
                                          << e.what();
                                transPtr->rollback();
                                if (callback)
                                {
                                    callback("");
                                }
                                return;
                            }
                            catch (...)
                            {
                                LOG_ERROR << "[RefundService] Mapper construction failed: "
                                             "unknown exception";
                                transPtr->rollback();
                                if (callback)
                                {
                                    callback("");
                                }
                                return;
                            }
                            if (refundStatus == "REFUND_SUCCESS")
                            {
                                try
                                {
                                    Mapper<PayOrderModel> orderMapper(transPtr);
                                    auto orderCriteria = Criteria(
                                      PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo
                                    );
                                    orderMapper.findOne(
                                      orderCriteria,
                                      [callback,
                                       refundStatus,
                                       orderNo,
                                       paymentNo,
                                       refundAmount,
                                       transPtr,
                                       transDb](const PayOrderModel &order) {
                                          insertLedgerEntry(
                                            transDb,
                                            order.getValueOfUserId(),
                                            orderNo,
                                            paymentNo,
                                            "REFUND",
                                            refundAmount
                                          );
                                          // A refund that settled here is evidence the
                                          // order may be closed out, but only once the
                                          // settled refunds cover its total: the refund
                                          // row was moved to REFUND_SUCCESS earlier in
                                          // this transaction, so the sum below sees it,
                                          // and covering the total is what recovers the
                                          // concurrent case where two settlements each
                                          // counted without the other and left the order
                                          // PAID.
                                          // Aggregate SUM (raw-SQL exemption #3): the
                                          // Mapper cannot express SUM.
                                          const auto orderRowStatus = order.getValueOfStatus();
                                          const auto orderAmount = order.getValueOfAmount();
                                          if (orderRowStatus != "PAID")
                                          {
                                              // Nothing to settle: the order was already
                                              // moved on (or never reached PAID), and a
                                              // closed order must not reopen.
                                              if (callback)
                                              {
                                                  callback(refundStatus);
                                              }
                                              return;
                                          }
                                          try
                                          {
                                              transPtr->execSqlAsync(
                                                "SELECT COALESCE(SUM(CAST(amount AS NUMERIC)), 0) "
                                                "AS sum_amount FROM pay_refund WHERE order_no = $1 "
                                                "AND status = $2",
                                                [callback,
                                                 refundStatus,
                                                 orderNo,
                                                 orderAmount,
                                                 transPtr](const Result &sumResult) {
                                                    int64_t settledRefundFen = 0;
                                                    if (!sumResult.empty())
                                                    {
                                                        const auto sumText =
                                                          sumResult.front()["sum_amount"]
                                                            .as<std::string>();
                                                        if (!pay::utils::parseAmountToFen(
                                                              sumText, settledRefundFen
                                                            ))
                                                        {
                                                            // An unreadable sum proves
                                                            // nothing: report the refund as
                                                            // synced and leave the order as
                                                            // it is -- a later notification
                                                            // or query re-runs the check.
                                                            settledRefundFen = 0;
                                                        }
                                                    }
                                                    int64_t orderTotalFen = 0;
                                                    if (!pay::utils::parseAmountToFen(
                                                          orderAmount, orderTotalFen
                                                        ))
                                                    {
                                                        orderTotalFen = 0;
                                                    }
                                                    if (!pay::utils::refundsCoverOrderAmount(
                                                          settledRefundFen, orderTotalFen
                                                        ))
                                                    {
                                                        LOG_DEBUG
                                                          << "[RefundService] A synced refund "
                                                             "settled, but the refunds on order "
                                                          << orderNo << " (" << settledRefundFen
                                                          << " fen) do not yet cover its total ("
                                                          << orderTotalFen
                                                          << " fen); the order stays as it is";
                                                        if (callback)
                                                        {
                                                            callback(refundStatus);
                                                        }
                                                        return;
                                                    }
                                                    Mapper<PayOrderModel> orderUpdater(transPtr);
                                                    orderUpdater.updateBy(
                                                      {PayOrderModel::Cols::_status},
                                                      [callback,
                                                       refundStatus,
                                                       orderNo](const size_t updated) {
                                                          if (updated == 0)
                                                          {
                                                              LOG_DEBUG
                                                                << "[RefundService] Order "
                                                                << orderNo
                                                                << " was no longer PAID when "
                                                                   "the synced refund covered it";
                                                          }
                                                          else
                                                          {
                                                              LOG_INFO << "[RefundService] Order "
                                                                       << orderNo
                                                                       << " is now REFUNDED on the "
                                                                          "settled-refund sum";
                                                          }
                                                          if (callback)
                                                          {
                                                              callback(refundStatus);
                                                          }
                                                      },
                                                      [callback,
                                                       transPtr](const DrogonDbException &e) {
                                                          LOG_ERROR << "Refund settle order update "
                                                                       "error: "
                                                                    << e.base().what();
                                                          transPtr->rollback();
                                                          if (callback)
                                                          {
                                                              callback("");
                                                          }
                                                      },
                                                      Criteria(
                                                        PayOrderModel::Cols::_order_no,
                                                        CompareOperator::EQ,
                                                        orderNo
                                                      ) &&
                                                        Criteria(
                                                          PayOrderModel::Cols::_status,
                                                          CompareOperator::EQ,
                                                          std::string("PAID")
                                                        ),
                                                      std::string("REFUNDED")
                                                    );
                                                },
                                                [callback,
                                                 transPtr,
                                                 orderNo](const DrogonDbException &e) {
                                                    LOG_ERROR << "Refund settled-sum lookup for "
                                                              << orderNo
                                                              << " failed: " << e.base().what();
                                                    transPtr->rollback();
                                                    if (callback)
                                                    {
                                                        callback("");
                                                    }
                                                },
                                                orderNo,
                                                std::string("REFUND_SUCCESS")
                                              );
                                          }
                                          catch (const std::exception &e)
                                          {
                                              LOG_ERROR << "[RefundService] Refund settle step "
                                                           "failed: "
                                                        << e.what();
                                              transPtr->rollback();
                                              if (callback)
                                              {
                                                  callback("");
                                              }
                                          }
                                          catch (...)
                                          {
                                              LOG_ERROR << "[RefundService] Refund settle step "
                                                           "failed: unknown exception";
                                              transPtr->rollback();
                                              if (callback)
                                              {
                                                  callback("");
                                              }
                                          }
                                      },
                                      [callback, transPtr](const DrogonDbException &e) {
                                          LOG_ERROR << "Refund ledger order lookup error: "
                                                    << e.base().what();
                                          transPtr->rollback();
                                          if (callback)
                                          {
                                              callback("");
                                          }
                                      }
                                    );
                                }
                                catch (const std::exception &e)
                                {
                                    LOG_ERROR << "[RefundService] Mapper construction failed: "
                                              << e.what();
                                    transPtr->rollback();
                                    if (callback)
                                    {
                                        callback("");
                                    }
                                }
                                catch (...)
                                {
                                    LOG_ERROR << "[RefundService] Mapper construction failed: "
                                                 "unknown exception";
                                    transPtr->rollback();
                                    if (callback)
                                    {
                                        callback("");
                                    }
                                }
                            }
                        },
                        rollbackDone
                      );
                  }
                  catch (const std::exception &e)
                  {
                      LOG_ERROR << "[RefundService] Mapper construction failed: " << e.what();
                      transPtr->rollback();
                      if (callback)
                      {
                          callback("");
                      }
                  }
                  catch (...)
                  {
                      LOG_ERROR << "[RefundService] Mapper construction failed: unknown exception";
                      transPtr->rollback();
                      if (callback)
                      {
                          callback("");
                      }
                  }
              });
          },
          [callback](const DrogonDbException &e) {
              LOG_ERROR << "Refund lookup error during sync: " << e.base().what();
              if (callback)
              {
                  callback("");
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[RefundService] Mapper construction failed: " << e.what();
        if (callback)
        {
            callback("");
        }
    }
    catch (...)
    {
        LOG_ERROR << "[RefundService] Mapper construction failed: unknown exception";
        if (callback)
        {
            callback("");
        }
    }
}
