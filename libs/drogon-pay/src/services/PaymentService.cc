#include "PaymentService.h"
#include "drogon_pay/PayErrorCategory.h"
#include "../models/PayOrder.h"
#include "../models/PayPayment.h"
#include "../models/PayLedger.h"
#include "../models/PayRefund.h"
#include "../utils/OnceCallback.h"
#include "../utils/PayUtils.h"
#include <drogon/drogon.h>
#include <random>
#include <sstream>
#include <iomanip>

using namespace drogon;
using namespace drogon::orm;

// Model type aliases for convenience
namespace
{
using PayOrderModel = drogon_model::pay_test::PayOrder;
using PayPaymentModel = drogon_model::pay_test::PayPayment;
using PayLedgerModel = drogon_model::pay_test::PayLedger;
using PayRefundModel = drogon_model::pay_test::PayRefund;
}  // namespace

namespace
{
std::string generatePaymentNoValue()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 99999999);

    std::ostringstream oss;
    time_t now = std::time(nullptr);
    struct tm tmInfo;
#ifdef _WIN32
    localtime_s(&tmInfo, &now);
#else
    localtime_r(&now, &tmInfo);
#endif
    oss << "PAY" << std::put_time(&tmInfo, "%Y%m%d%H%M%S");
    oss << std::setfill('0') << std::setw(8) << dis(gen);

    return oss.str();
}

// Both reconcile doors (the WeChat transaction query and the Alipay trade query)
// settle an order from a channel answer that names an amount. Compare that
// amount with the one this payment row asked for before booking: an answer with
// no amount, or with a different one, is not evidence about this trade. The
// notification path already refuses to book on such an answer; this keeps the
// query path from being the door that lets it through. Returns an empty string
// when the answer may book, and the reason it may not otherwise.
std::string reconcileAmountProblem(int64_t answerTotalFen, const std::string &paymentAmount)
{
    int64_t paymentTotalFen = 0;
    if (!pay::utils::parseAmountToFen(paymentAmount, paymentTotalFen))
    {
        return "payment amount '" + paymentAmount + "' is not a usable amount";
    }
    if (answerTotalFen <= 0)
    {
        return "channel answer carried no amount while the payment asks for " +
               std::to_string(paymentTotalFen) + " fen";
    }
    if (answerTotalFen != paymentTotalFen)
    {
        return "amount mismatch: channel answer " + std::to_string(answerTotalFen) +
               " fen, payment row " + std::to_string(paymentTotalFen) + " fen";
    }
    return {};
}

// A `trade_state=REFUND` answer says the trade entered refunding, which one
// partial refund of an order is enough to produce -- so the mapped `REFUNDED`
// reaching the WeChat query door is only a claim, and the caller's settled-
// refund sum (read in the same transaction) is the evidence that decides it.
// An unparsable order total proves nothing, which downgrades too: `PAID` is
// what a REFUND trade has nonetheless evidenced -- the money arrived, and some
// of it is on its way back.
std::string orderStatusAfterRefundCoverage(
  const std::string &mappedOrderStatus,
  const std::string &orderAmount,
  int64_t settledRefundFen
)
{
    int64_t orderTotalFen = 0;
    if (!pay::utils::parseAmountToFen(orderAmount, orderTotalFen))
    {
        orderTotalFen = 0;
    }
    return pay::utils::resolveRefundedOrderStatus(
      mappedOrderStatus, settledRefundFen, orderTotalFen
    );
}

// TODO(dedup): insertLedgerEntry is duplicated across PaymentService.cc,
// RefundService.cc, and CallbackService.cc. Extract to PayUtils.h/cc in a
// future refactoring iteration.
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
            LOG_WARN << "Ledger mapper error: " << e.what();
        }
        catch (...)
        {
            LOG_WARN << "Ledger mapper error: unknown exception";
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
            LOG_WARN << "[PaymentService] Mapper construction failed: " << e.what();
        }
        catch (...)
        {
            LOG_WARN << "[PaymentService] Mapper construction failed: unknown exception";
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
        LOG_WARN << "[PaymentService] Mapper construction failed: " << e.what();
    }
    catch (...)
    {
        LOG_WARN << "[PaymentService] Mapper construction failed: unknown exception";
    }
}

// Shared 1003 error payload for Mapper-construction catch blocks.
Json::Value dbErrorResponse(const std::string &what)
{
    Json::Value response;
    response["code"] = 1003;
    response["message"] = "Database error: " + what;
    return response;
}

// Uniform catch handler: log and report 1003 through the shared callback.
void reportMapperFailure(
  const std::shared_ptr<PaymentService::PaymentCallback> &sharedCb,
  const std::string &what
)
{
    LOG_ERROR << "[PaymentService] Mapper construction failed: " << what;
    if (*sharedCb)
    {
        (*sharedCb)(dbErrorResponse(what), std::make_error_code(std::errc::io_error));
    }
}

// What `channelResultError` answers for a 2xx body that names no payable code.
// A file-local constant because `attemptCertainlyNotCreated` has to recognise
// the same string: this is a contract violation, not a channel refusal.
constexpr const char *kNoPayableCode = "WeChat response carries neither code_url nor prepay_id";

// A 2xx transport result is not the same thing as a created transaction.
// WeChat V3 answers native orders with `code_url` (jsapi with `prepay_id`) and
// carries no business-code field; Alipay answers with business code "10000".
// Without this gate an error body under a 200 moved the order to PAYING for a
// transaction the channel never accepted.
std::string channelResultError(const std::string &channel, const Json::Value &result)
{
    if (channel == "alipay")
    {
        const std::string code = result.get("code", "").asString();
        if (code == "10000")
        {
            return {};
        }
        std::string detail = result.get("sub_msg", "").asString();
        if (detail.empty())
        {
            detail = result.get("msg", "").asString();
        }
        return detail.empty() ? "Alipay error: code " + code : "Alipay error: " + detail;
    }

    if (channel == "wechat")
    {
        // `isMember` alone would pass on `{"code_url":null}` and on an empty
        // string, which is the same phantom order under a different body.
        const Json::Value codeUrl = result.get("code_url", "");
        const Json::Value prepayId = result.get("prepay_id", "");
        if (
          (codeUrl.isString() && !codeUrl.asString().empty()) ||
          (prepayId.isString() && !prepayId.asString().empty())
        )
        {
            return {};
        }
        const std::string code = result.get("code", "").asString();
        if (code.empty())
        {
            return kNoPayableCode;
        }
        return "WeChat error: " + code + " " + result.get("message", "").asString();
    }

    // Custom channels define their own success shape; the channel SPI's `error`
    // argument is the only signal available for them.
    return {};
}

// WeChat V3 takes an ISO-4217 three-letter code, and the order row has to keep
// exactly what was offered: the callback refuses to settle when
// `pay_order.currency` differs from the currency in the notification, so a
// malformed or case-shifted code here becomes money that cannot be booked.
// Lowercase input is normalised, anything that is not three letters is refused.
std::string normalizeQrCurrency(const std::string &currency, bool &valid)
{
    valid = currency.size() == 3;
    if (!valid)
    {
        return {};
    }
    std::string normalized;
    for (const char c : currency)
    {
        if (c < 'A' || c > 'Z')
        {
            if (c < 'a' || c > 'z')
            {
                valid = false;
                return {};
            }
            normalized.push_back(static_cast<char>(c - 'a' + 'A'));
            continue;
        }
        normalized.push_back(c);
    }
    return normalized;
}

// Whether an attempt may be booked as closed. Only an answer that proves the
// channel refused may close the row: a timeout, a transport fault, a body that
// carries no payable code, or a 4xx answered by an intermediary prove nothing,
// and closing those makes a transaction the user can still pay invisible to
// every recovery filter -- the same rule `refundCertainlyDidNotHappen` applies
// to a refund row. Both create paths share this predicate because both hand the
// channel a booked row: an attempt left at INIT is still settled by the callback.
bool attemptCertainlyNotCreated(const std::string &failure)
{
    // The channel's own error envelope, from either the body or `HTTP 4xx`.
    if (failure.rfind("Alipay error: ", 0) == 0 || failure.rfind("WeChat error: ", 0) == 0)
    {
        return true;
    }
    const bool httpRefusal =
      failure.rfind("HTTP 4", 0) == 0 && failure.find("no error envelope") == std::string::npos;
    if (httpRefusal)
    {
        return true;
    }
    // A 2xx with no payable code says the answer made no sense, not that WeChat
    // refused: the transaction can exist behind a body an intermediary rewrote,
    // so this attempt has to stay in flight for the notification.
    if (failure == kNoPayableCode)
    {
        return false;
    }
    const bool wentThroughHttp = failure.rfind("HTTP ", 0) == 0 ||
                                 failure.rfind("http request", 0) == 0 ||
                                 failure == "invalid json response";
    return !wentThroughHttp;
}

// Books an attempt the channel provably refused: the payment row to FAIL with
// the channel's reason, then its order to FAILED. `done` runs at every terminal
// -- booked, refused by a database error, or never started -- because the caller
// answers the request from there. Answering earlier leaves this chain running
// after the caller has gone, and the chain is then the last owner of the DB
// client: drogon drops it on one of the client's own loop threads, whose
// destructor joins that very thread and aborts the process.
void bookRefusedAttempt(
  const std::shared_ptr<DbClient> &db,
  const std::string &paymentNo,
  const std::string &orderNo,
  const std::string &errPayload,
  const std::function<void()> &done
)
{
    auto reportFault = [paymentNo, done](const std::string &detail) {
        LOG_ERROR << "[PaymentService] Refused attempt " << paymentNo
                  << " not booked as closed: " << detail;
        done();
    };
    if (!db)
    {
        reportFault("no database client");
        return;
    }
    try
    {
        Mapper<PayPaymentModel> paymentMapper(db);
        paymentMapper.findOne(
          Criteria(PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo),
          [db, orderNo, errPayload, done, reportFault](PayPaymentModel payment) {
              payment.setStatus("FAIL");
              payment.setResponsePayload(errPayload);
              try
              {
                  Mapper<PayPaymentModel> paymentUpdater(db);
                  paymentUpdater.update(
                    payment,
                    [db, orderNo, done, reportFault](const size_t) {
                        try
                        {
                            Mapper<PayOrderModel> orderMapper(db);
                            orderMapper.findOne(
                              Criteria(
                                PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo
                              ),
                              [db, done, reportFault](PayOrderModel order) {
                                  order.setStatus("FAILED");
                                  try
                                  {
                                      Mapper<PayOrderModel> orderUpdater(db);
                                      orderUpdater.update(
                                        order,
                                        [done](const size_t) { done(); },
                                        [reportFault](const DrogonDbException &e) {
                                            reportFault(
                                              std::string("order write: ") + e.base().what()
                                            );
                                        }
                                      );
                                  }
                                  catch (const std::exception &e)
                                  {
                                      reportFault(std::string("order mapper: ") + e.what());
                                  }
                                  catch (...)
                                  {
                                      reportFault("order mapper: unknown exception");
                                  }
                              },
                              [reportFault](const DrogonDbException &e) {
                                  reportFault(std::string("order read: ") + e.base().what());
                              }
                            );
                        }
                        catch (const std::exception &e)
                        {
                            reportFault(std::string("order mapper: ") + e.what());
                        }
                        catch (...)
                        {
                            reportFault("order mapper: unknown exception");
                        }
                    },
                    [reportFault](const DrogonDbException &e) {
                        reportFault(std::string("payment write: ") + e.base().what());
                    }
                  );
              }
              catch (const std::exception &e)
              {
                  reportFault(std::string("payment mapper: ") + e.what());
              }
              catch (...)
              {
                  reportFault("payment mapper: unknown exception");
              }
          },
          [reportFault](const DrogonDbException &e) {
              reportFault(std::string("payment read: ") + e.base().what());
          }
        );
    }
    catch (const std::exception &e)
    {
        reportFault(std::string("payment mapper: ") + e.what());
    }
    catch (...)
    {
        reportFault("payment mapper: unknown exception");
    }
}
}  // namespace

PaymentService::PaymentService(
  std::map<std::string, drogon_pay::PaymentChannelPtr> channels,
  std::shared_ptr<DbClient> dbClient,
  nosql::RedisClientPtr redisClient,
  std::shared_ptr<IdempotencyService> idempotencyService
)
    : channels_(std::move(channels)),
      dbClient_(dbClient),
      redisClient_(redisClient),
      idempotencyService_(idempotencyService)
{
}

drogon_pay::PaymentChannelPtr PaymentService::findChannel(const std::string &name) const
{
    auto it = channels_.find(name);
    return (it != channels_.end()) ? it->second : nullptr;
}

void PaymentService::createPayment(
  const CreatePaymentRequest &request,
  const std::string &idempotencyKey,
  PaymentCallback &&callback
)
{
    // Calculate request hash for idempotency
    Json::Value reqJson;
    reqJson["order_no"] = request.orderNo;
    reqJson["amount"] = request.amount;
    reqJson["currency"] = request.currency;
    reqJson["description"] = request.description;
    const std::string requestStr = pay::utils::toJsonString(reqJson);

    // Use SHA-256 for cryptographic hashing (more secure than std::hash)
    std::string requestHash = drogon::utils::getSha256(requestStr);

    auto finalCb = pay::utils::makeOnceCallback<void(const Json::Value &, const std::error_code &)>(
      std::move(callback)
    );
    auto sharedCb = std::make_shared<decltype(finalCb)>(finalCb);
    auto idempotencyService = idempotencyService_;

    // Check idempotency
    idempotencyService_->checkAndSetStatus(
      idempotencyKey,
      requestHash,
      [&request]() {
          Json::Value req;
          req["order_no"] = request.orderNo;
          req["amount"] = request.amount;
          return req;
      }(),
      [this, request, idempotencyKey, requestHash, sharedCb, idempotencyService](
        const IdempotencyService::CheckResult &checkResult
      ) mutable {
          if (checkResult.status == IdempotencyService::CheckStatus::Conflict)
          {
              // Idempotency conflict
              Json::Value error;
              error["code"] = 1004;
              error["message"] = "Idempotency conflict: different parameters for same key";
              sharedCb->call(error, pay::makePayError(1004, "idempotency key conflict"));
              return;
          }

          if (checkResult.status == IdempotencyService::CheckStatus::InProgress)
          {
              Json::Value error;
              error["code"] = 1004;
              error["message"] = "Idempotency request is already in progress";
              sharedCb->call(error, pay::makePayError(1004, "idempotency request in progress"));
              return;
          }

          if (checkResult.status == IdempotencyService::CheckStatus::Error)
          {
              Json::Value error;
              error["code"] = 1003;
              error["message"] = "Idempotency check failed";
              sharedCb->call(error, pay::makePayError(1003, "idempotency check failed"));
              return;
          }

          if (checkResult.status == IdempotencyService::CheckStatus::Replay)
          {
              // Return cached result
              sharedCb->call(checkResult.cachedResult, std::error_code());
              return;
          }

          // Proceed with payment creation
          std::string paymentNo = generatePaymentNoValue();
          int64_t totalFen = 0;
          if (!pay::utils::parseAmountToFen(request.amount, totalFen))
          {
              Json::Value error;
              error["code"] = 1001;
              error["message"] = "Invalid amount format";
              auto ec = pay::makePayError(1001, "Invalid amount format");
              if (!idempotencyKey.empty())
              {
                  // Validation failed after the key was reserved: release it so
                  // the client can retry with a corrected amount.
                  idempotencyService
                    ->clearReservation(idempotencyKey, requestHash, [sharedCb, error, ec](bool) {
                        sharedCb->call(error, ec);
                    });
                  return;
              }
              sharedCb->call(error, ec);
              return;
          }
          // Validate notify_url before it reaches the channel request. Without
          // this check an attacker-controlled notify_url is forwarded verbatim
          // to the provider, enabling SSRF (P1-3). RefundService already does
          // this; both now share pay::utils::validateNotifyUrl.
          {
              std::string urlError;
              if (!pay::utils::validateNotifyUrl(request.notifyUrl, urlError))
              {
                  Json::Value error;
                  error["code"] = 1001;
                  error["message"] = urlError;
                  auto ec = pay::makePayError(1001, urlError);
                  if (!idempotencyKey.empty())
                  {
                      idempotencyService->clearReservation(
                        idempotencyKey, requestHash, [sharedCb, error, ec](bool) {
                            sharedCb->call(error, ec);
                        }
                      );
                      return;
                  }
                  sharedCb->call(error, ec);
                  return;
              }
          }

          // `time_expire` is forwarded to the channel verbatim and also decides
          // the local `expire_at`, so a shape the two sides read differently has
          // to be refused before the order row exists: the channel answers a 400
          // of its own to a non-RFC-3339 value, and a deadline beyond its window
          // is silently moved by the channel, leaving the two expiries disagreeing.
          if (!request.timeExpire.empty())
          {
              std::string timeExpireError;
              if (!pay::utils::validateTimeExpire(
                    request.timeExpire, request.channel, timeExpireError
                  ))
              {
                  Json::Value error;
                  error["code"] = 1001;
                  error["message"] = timeExpireError;
                  auto ec = pay::makePayError(1001, timeExpireError);
                  if (!idempotencyKey.empty())
                  {
                      idempotencyService->clearReservation(
                        idempotencyKey, requestHash, [sharedCb, error, ec](bool) {
                            sharedCb->call(error, ec);
                        }
                      );
                      return;
                  }
                  sharedCb->call(error, ec);
                  return;
              }
          }

          // WeChat names its resources by the merchant order number itself, and
          // the official parameter table caps out_trade_no at 6-32 characters
          // from [0-9a-zA-Z_|*-]. Booking an order the channel can never name
          // left a CREATED/PAYING row in the reconciliation sweep forever.
          if (request.channel == "wechat")
          {
              std::string fieldError;
              if (!pay::utils::validateWechatOrderFields(
                    request.orderNo, request.description, request.attach, fieldError
                  ))
              {
                  Json::Value error;
                  error["code"] = 1001;
                  error["message"] = fieldError;
                  auto ec = pay::makePayError(1001, fieldError);
                  if (!idempotencyKey.empty())
                  {
                      idempotencyService->clearReservation(
                        idempotencyKey, requestHash, [sharedCb, error, ec](bool) {
                            sharedCb->call(error, ec);
                        }
                      );
                      return;
                  }
                  sharedCb->call(error, ec);
                  return;
              }
          }

          auto wrappedCb = [idempotencyService,
                            idempotencyKey,
                            requestHash,
                            sharedCb](const Json::Value &result, const std::error_code &error) {
              if (!idempotencyKey.empty() && !error && result.isMember("data"))
              {
                  idempotencyService->updateResult(
                    idempotencyKey,
                    requestHash,
                    result,
                    [idempotencyService, idempotencyKey, requestHash, sharedCb, result, error](
                      bool success
                    ) {
                        if (success)
                        {
                            sharedCb->call(result, error);
                            return;
                        }
                        // The snapshot write failed: release the reservation so a
                        // retry is not poisoned with InProgress (NULL snapshot)
                        // until the TTL expires. (B1-1 follow-up)
                        LOG_ERROR << "[PaymentService] Failed to save idempotency snapshot; "
                                     "clearing reservation for key="
                                  << idempotencyKey;
                        idempotencyService->clearReservation(
                          idempotencyKey, requestHash, [sharedCb, result, error](bool) {
                              sharedCb->call(result, error);
                          }
                        );
                    }
                  );
                  return;
              }
              if (!idempotencyKey.empty() && error)
              {
                  // Operation failed after the key was reserved: release the
                  // in-flight reservation so the next retry is not reported as
                  // InProgress (key poisoning).
                  idempotencyService->clearReservation(
                    idempotencyKey, requestHash, [sharedCb, result, error](bool) {
                        sharedCb->call(result, error);
                    }
                  );
                  return;
              }
              sharedCb->call(result, error);
          };
          proceedCreatePayment(request, paymentNo, totalFen, std::move(wrappedCb));
      }
    );
}

void PaymentService::proceedCreatePayment(
  const CreatePaymentRequest &request,
  const std::string &paymentNo,
  int64_t totalFen,
  PaymentCallback &&callback
)
{
    // Wrap callback in a shared once-only wrapper: concurrent DB-error and
    // channel-error branches may both try to respond; only the first wins.
    auto onceCb = pay::utils::makeOnceCallback<void(const Json::Value &, const std::error_code &)>(
      std::move(callback)
    );
    auto sharedCb = std::make_shared<PaymentCallback>(
      [onceCb](const Json::Value &response, const std::error_code &ec) {
          onceCb.call(response, ec);
      }
    );

    // Guard the whole synchronous setup: the Mapper constructor may throw
    // before any async error branch is reachable.
    try
    {
        // Create order record in database
        Mapper<PayOrderModel> orderMapper(dbClient_);
        PayOrderModel order;
        order.setOrderNo(request.orderNo);
        order.setUserId(request.userId);
        order.setAmount(request.amount);
        order.setCurrency(request.currency);
        order.setStatus("CREATED");
        order.setChannel(request.channel);
        order.setTitle(request.description);
        order.setCreatedAt(trantor::Date::now());
        // Parse and set expire_at if timeExpire is provided
        if (!request.timeExpire.empty())
        {
            // The same reading the guard before the booking applies, so the row
            // records the exact instant the channel is offered. Trantor's own
            // readers cannot be used for this: `fromDbStringLocal` splits on a
            // SPACE and lets `std::stol` stop at the 'T' without throwing, so a
            // correct RFC 3339 value came back as local midnight with the whole
            // time-of-day silently dropped; `fromISOString` adds the machine's
            // zone on top of the string's own offset.
            int64_t expireSeconds = 0;
            std::string expireError;
            if (pay::utils::parseRfc3339(request.timeExpire, expireSeconds, expireError))
            {
                order.setExpireAt(trantor::Date(expireSeconds * 1000000));
            }
            else
            {
                LOG_WARN << "Failed to parse timeExpire '" << request.timeExpire
                         << "': " << expireError;
                // Continue without setting expire_at
            }
        }

        // Build payment request payload based on channel
        Json::Value payload;

        if (request.channel == "alipay")
        {
            // Alipay API format
            // Convert fen to yuan for Alipay (string format)
            // Use integer arithmetic to avoid floating point precision issues
            const int64_t yuan = totalFen / 100;
            const int64_t cents = totalFen % 100;
            std::ostringstream yuanStream;
            yuanStream << yuan << "." << (cents < 10 ? "0" : "") << cents;
            const std::string totalAmountYuan = yuanStream.str();

            payload["total_amount"] = totalAmountYuan;
            payload["subject"] =
              request.description;  // Alipay uses 'subject' instead of 'description'
            payload["out_trade_no"] = request.orderNo;

            // Add buyer_id for sandbox testing
            const char *buyerIdEnv = std::getenv("ALIPAY_SANDBOX_BUYER_ID");
            if (buyerIdEnv && strlen(buyerIdEnv) > 0)
            {
                payload["buyer_id"] = std::string(buyerIdEnv);
            }

            if (!request.notifyUrl.empty())
            {
                payload["notify_url"] = request.notifyUrl;
            }
        }
        else
        {
            // WeChat Pay API format (original format)
            payload["description"] = request.description;
            payload["out_trade_no"] = request.orderNo;
            payload["amount"]["total"] = static_cast<Json::Int64>(totalFen);
            payload["amount"]["currency"] = request.currency;

            if (!request.notifyUrl.empty())
            {
                payload["notify_url"] = request.notifyUrl;
            }

            if (!request.sceneInfo.isNull())
            {
                payload["scene_info"] = request.sceneInfo;
            }

            // Add time_expire if provided
            if (!request.timeExpire.empty())
            {
                payload["time_expire"] = request.timeExpire;
            }

            // Add attach if provided
            if (!request.attach.empty())
            {
                payload["attach"] = request.attach;
            }
        }

        const std::string requestPayload = pay::utils::toJsonString(payload);

        // Wrap PayOrder INSERT + PayPayment INSERT in a single transaction.
        // Channel API call happens AFTER COMMIT (outside the transaction),
        // matching the RefundService pattern. (A1-1 fix)
        dbClient_->newTransactionAsync(
          [this, request, paymentNo, payload, requestPayload, sharedCb, order](
            const std::shared_ptr<Transaction> &transPtr
          ) mutable {
              if (!transPtr)
              {
                  if (*sharedCb)
                  {
                      Json::Value err;
                      err["code"] = 1003;
                      err["message"] = "Transaction unavailable";
                      (*sharedCb)(err, pay::makePayError(1003, "Transaction unavailable"));
                  }
                  return;
              }

              auto failDb = [sharedCb, transPtr](const DrogonDbException &e) {
                  transPtr->rollback();
                  if (*sharedCb)
                  {
                      Json::Value err;
                      err["code"] = 1003;
                      err["message"] = "Database error: " + std::string(e.base().what());
                      (*sharedCb)(err, pay::makePayError(1003, "Database error"));
                  }
              };

              // 1. INSERT PayOrder inside the transaction.
              try
              {
                  Mapper<PayOrderModel> txnOrderMapper(transPtr);
                  txnOrderMapper.insert(
                    order,
                    [this, request, paymentNo, payload, requestPayload, sharedCb, transPtr, failDb](
                      const PayOrderModel &
                    ) {
                        LOG_DEBUG << "[PaymentService] Order created (in txn): order_no="
                                  << request.orderNo << ", payment_no=" << paymentNo;

                        // 2. INSERT PayPayment inside the same transaction.
                        try
                        {
                            Mapper<PayPaymentModel> txnPaymentMapper(transPtr);
                            PayPaymentModel payment;
                            payment.setOrderNo(request.orderNo);
                            payment.setPaymentNo(paymentNo);
                            payment.setStatus("INIT");
                            payment.setAmount(request.amount);
                            payment.setRequestPayload(requestPayload);
                            payment.setCreatedAt(trantor::Date::now());
                            txnPaymentMapper.insert(
                              payment,
                              [this, request, paymentNo, payload, sharedCb, transPtr](
                                const PayPaymentModel &
                              ) {
                                  LOG_DEBUG << "[PaymentService] Payment record created (in txn): "
                                               "payment_no="
                                            << paymentNo << ", order_no=" << request.orderNo
                                            << ", channel=" << request.channel;

                                  // 3. COMMIT before any channel API call.
                                  transPtr->execSqlAsync(
                                    "COMMIT",
                                    [this, request, paymentNo, payload, sharedCb](const Result &) {
                                        LOG_DEBUG
                                          << "[PaymentService] Transaction committed: payment_no="
                                          << paymentNo;

                                        // 4. Channel API call (OUTSIDE transaction).
                                        auto paymentCallback = [this, request, paymentNo, sharedCb](
                                                                 const Json::Value &result,
                                                                 const std::string &transportError
                                                               ) {
                                            // `transportError` is what the channel
                                            // reported; channelResultError adds the
                                            // case where the call came back clean but
                                            // the body does not describe an accepted
                                            // transaction.
                                            const std::string error =
                                              transportError.empty()
                                                ? channelResultError(request.channel, result)
                                                : transportError;
                                            if (!error.empty())
                                            {
                                                // Handle payment error
                                                Json::Value errJson;
                                                errJson["error"] = error;
                                                const std::string errPayload =
                                                  pay::utils::toJsonString(errJson);

                                                // Only a provable refusal may close the booked row.
                                                // A timeout or an unreadable answer leaves a
                                                // prepay_id possibly live on WeChat's side, and
                                                // `FAIL` is exactly the status
                                                // `openAttemptsOfOrder` hides -- so closing an
                                                // uncertain attempt strands the money the callback
                                                // later reports, which is the QR failure this rule
                                                // was written for.
                                                //
                                                // The answer goes out where the booking ends, not
                                                // beside it. A booking still running after the
                                                // answer outlives the caller that provoked it while
                                                // holding the DB client that caller's world owns,
                                                // and it made the caller's error code a race
                                                // between this 1002 and the booking's own failure
                                                // branches.
                                                auto answerChannelError =
                                                  [request, error, sharedCb]() {
                                                      if (*sharedCb)
                                                      {
                                                          Json::Value response;
                                                          response["code"] = 1002;
                                                          const std::string channelName =
                                                            request.channel == "alipay"
                                                              ? "Alipay"
                                                              : "WeChat Pay";
                                                          response["message"] =
                                                            channelName + " error: " + error;
                                                          (*sharedCb)(
                                                            response, pay::makePayError(1002, error)
                                                          );
                                                      }
                                                  };
                                                if (attemptCertainlyNotCreated(error))
                                                {
                                                    bookRefusedAttempt(
                                                      dbClient_,
                                                      paymentNo,
                                                      request.orderNo,
                                                      errPayload,
                                                      answerChannelError
                                                    );
                                                }
                                                else
                                                {
                                                    LOG_WARN << "[PaymentService] Attempt "
                                                             << paymentNo
                                                             << " outcome unknown; leaving it in "
                                                                "flight: "
                                                             << error;
                                                    answerChannelError();
                                                }
                                                return;
                                            }

                                            // Success - promote the rows this attempt booked:
                                            // payment INIT -> PROCESSING with the channel payload,
                                            // then order CREATED -> PAYING.
                                            const std::string responsePayload =
                                              pay::utils::toJsonString(result);

                                            // Both writes name the columns they change and require
                                            // the row to still be where this attempt left it. A
                                            // full-row write from the copy taken before the channel
                                            // answered rolls back whatever a fast notification
                                            // already settled -- the payment back from SUCCESS to
                                            // PROCESSING, the order back from PAID to PAYING -- and
                                            // the money stays captured with nothing to show for it.
                                            // This is the guarded shape the QR path adopted; the
                                            // jsapi path had been left behind.
                                            auto answer = [request, paymentNo, result, sharedCb](
                                                            const std::string &failureDetail
                                                          ) {
                                                if (!failureDetail.empty())
                                                {
                                                    LOG_WARN
                                                      << "[PaymentService] Status row for "
                                                      << paymentNo
                                                      << " not updated: " << failureDetail
                                                      << "; answering with the channel result "
                                                         "anyway";
                                                }
                                                if (!*sharedCb)
                                                {
                                                    return;
                                                }

                                                Json::Value response;
                                                response["code"] = 0;
                                                response["message"] =
                                                  "Payment created successfully";
                                                Json::Value data;
                                                data["order_no"] = request.orderNo;
                                                data["payment_no"] = paymentNo;
                                                data["status"] = "PAYING";

                                                // Add payment channel response details
                                                if (request.channel == "alipay")
                                                {
                                                    // Alipay response
                                                    data["alipay_response"] = result;
                                                    const auto qrCode =
                                                      result.get("qr_code", "").asString();
                                                    if (!qrCode.empty())
                                                    {
                                                        data["qr_code"] = qrCode;
                                                    }
                                                }
                                                else
                                                {
                                                    // WeChat Pay response
                                                    data["wechat_response"] = result;
                                                    const auto codeUrl =
                                                      result.get("code_url", "").asString();
                                                    if (!codeUrl.empty())
                                                    {
                                                        data["code_url"] = codeUrl;
                                                    }
                                                    const auto prepayId =
                                                      result.get("prepay_id", "").asString();
                                                    if (!prepayId.empty())
                                                    {
                                                        data["prepay_id"] = prepayId;
                                                    }
                                                }

                                                response["data"] = data;
                                                (*sharedCb)(response, std::error_code());
                                            };

                                            auto failDb = [sharedCb](const DrogonDbException &e) {
                                                if (*sharedCb)
                                                {
                                                    Json::Value response;
                                                    response["code"] = 1003;
                                                    response["message"] =
                                                      "Database error: " +
                                                      std::string(e.base().what());
                                                    (*sharedCb)(
                                                      response,
                                                      pay::makePayError(
                                                        1003,
                                                        "Database error: " +
                                                          std::string(e.base().what())
                                                      )
                                                    );
                                                }
                                            };

                                            try
                                            {
                                                Mapper<PayPaymentModel> paymentUpdater(dbClient_);
                                                paymentUpdater.updateBy(
                                                  {PayPaymentModel::Cols::_status,
                                                   PayPaymentModel::Cols::_response_payload},
                                                  [this, request, answer, paymentNo, failDb](
                                                    const size_t updated
                                                  ) {
                                                      if (updated == 0)
                                                      {
                                                          answer(
                                                            "payment " + paymentNo +
                                                            " had already moved out of "
                                                            "INIT/PROCESSING"
                                                          );
                                                          return;
                                                      }
                                                      try
                                                      {
                                                          Mapper<PayOrderModel> orderUpdater(
                                                            dbClient_
                                                          );
                                                          orderUpdater.updateBy(
                                                            {PayOrderModel::Cols::_status},
                                                            [answer](const size_t) { answer(""); },
                                                            [answer](const DrogonDbException &e) {
                                                                answer(e.base().what());
                                                            },
                                                            Criteria(
                                                              PayOrderModel::Cols::_order_no,
                                                              CompareOperator::EQ,
                                                              request.orderNo
                                                            ) &&
                                                              Criteria(
                                                                PayOrderModel::Cols::_status,
                                                                CompareOperator::In,
                                                                std::vector<std::string>{
                                                                  "CREATED", "PAYING"
                                                                }
                                                              ),
                                                            "PAYING"
                                                          );
                                                      }
                                                      catch (const std::exception &e)
                                                      {
                                                          answer(e.what());
                                                      }
                                                      catch (...)
                                                      {
                                                          answer("unknown exception");
                                                      }
                                                  },
                                                  failDb,
                                                  Criteria(
                                                    PayPaymentModel::Cols::_payment_no,
                                                    CompareOperator::EQ,
                                                    paymentNo
                                                  ) &&
                                                    Criteria(
                                                      PayPaymentModel::Cols::_status,
                                                      CompareOperator::In,
                                                      std::vector<std::string>{"INIT", "PROCESSING"}
                                                    ),
                                                  "PROCESSING",
                                                  responsePayload
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

                                        // Route through the channel registry. Unknown or
                                        // unconfigured channels are rejected explicitly —
                                        // never fall back to another channel.
                                        LOG_DEBUG
                                          << "[PaymentService] Calling payment channel: channel="
                                          << request.channel << ", order_no=" << request.orderNo
                                          << ", payment_no=" << paymentNo;
                                        auto channelImpl = findChannel(request.channel);
                                        if (!channelImpl)
                                        {
                                            LOG_ERROR << "[PaymentService] Channel not available: "
                                                      << request.channel;
                                            // Reuse the channel-error path so the idempotency
                                            // reservation is cleaned up like any channel failure.
                                            Json::Value empty;
                                            paymentCallback(
                                              empty, "CHANNEL_NOT_AVAILABLE: " + request.channel
                                            );
                                        }
                                        else
                                        {
                                            // Both wechat (native transaction) and alipay
                                            // (precreate) surface as QR payments here.
                                            channelImpl->createQRPayment(
                                              payload, std::move(paymentCallback)
                                            );
                                        }
                                    },
                                    [sharedCb](const DrogonDbException &e) {
                                        LOG_ERROR << "Failed to commit transaction: "
                                                  << e.base().what();
                                        if (*sharedCb)
                                        {
                                            Json::Value err;
                                            err["code"] = 1003;
                                            err["message"] = "Failed to commit transaction: " +
                                                             std::string(e.base().what());
                                            (*sharedCb)(
                                              err,
                                              pay::makePayError(
                                                1003,
                                                "Failed to commit transaction: " +
                                                  std::string(e.base().what())
                                              )
                                            );
                                        }
                                    }
                                  );
                              },
                              failDb
                            );
                        }
                        catch (const std::exception &e)
                        {
                            transPtr->rollback();
                            LOG_ERROR << "[PaymentService] Payment Mapper construction failed: "
                                      << e.what();
                            reportMapperFailure(sharedCb, e.what());
                        }
                        catch (...)
                        {
                            transPtr->rollback();
                            LOG_ERROR << "[PaymentService] Payment Mapper construction failed: "
                                         "unknown exception";
                            reportMapperFailure(sharedCb, "unknown exception");
                        }
                    },
                    failDb
                  );
              }
              catch (const std::exception &e)
              {
                  transPtr->rollback();
                  LOG_ERROR << "[PaymentService] Order Mapper construction failed: " << e.what();
                  reportMapperFailure(sharedCb, e.what());
              }
              catch (...)
              {
                  transPtr->rollback();
                  LOG_ERROR
                    << "[PaymentService] Order Mapper construction failed: unknown exception";
                  reportMapperFailure(sharedCb, "unknown exception");
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        if (*sharedCb)
        {
            Json::Value response;
            response["code"] = 1003;
            response["message"] = "Exception during payment creation: " + std::string(e.what());
            (*sharedCb)(response, std::make_error_code(std::errc::io_error));
        }
    }
    catch (...)
    {
        if (*sharedCb)
        {
            Json::Value response;
            response["code"] = 1003;
            response["message"] = "Exception during payment creation: unknown exception";
            (*sharedCb)(response, std::make_error_code(std::errc::io_error));
        }
    }
}

void PaymentService::createQRPayment(const Json::Value &request, PaymentCallback &&callback)
{
    auto finalCb = pay::utils::makeOnceCallback<void(const Json::Value &, const std::error_code &)>(
      std::move(callback)
    );
    auto sharedCb = std::make_shared<decltype(finalCb)>(finalCb);

    // The rows are now written before the channel is called, so a missing client
    // has to be reported rather than faulting inside the Mapper.
    if (!dbClient_)
    {
        Json::Value response;
        response["code"] = 1003;
        response["message"] = "Database client not available";
        sharedCb->call(response, pay::makePayError(1003, "database client not available"));
        return;
    }

    // Extract parameters
    std::string orderNo = request.get("order_no", "").asString();
    std::string amount = request.get("amount", "").asString();
    std::string channel = request.get("channel", "alipay").asString();
    std::string subject = request.get("subject", "Payment").asString();
    std::string timeExpire = request.get("time_expire", "").asString();

    if (orderNo.empty() || amount.empty())
    {
        Json::Value response;
        response["code"] = 400;
        response["message"] = "Missing required parameters: order_no, amount";
        sharedCb->call(response, pay::makePayError(400, "missing required parameters"));
        return;
    }

    // The owner is resolved once, here. Both reads this replaces defaulted to
    // buyer "1" when the field was absent -- and that default was a *string*,
    // which jsoncpp's `asInt64()` throws on, so an absent field faulted the
    // booking while a present-but-zero one booked the order under the id that
    // `queryOrderList` reads as "no owner filter".
    const Json::Value userIdValue = request.get("user_id", Json::Value());
    const int64_t userId = userIdValue.isInt64() ? userIdValue.asInt64() : 0;
    if (userId <= 0)
    {
        Json::Value response;
        response["code"] = 400;
        response["message"] = "Missing or invalid user_id";
        sharedCb->call(response, pay::makePayError(400, "missing user_id"));
        return;
    }

    // The currency is derived once, here, and both the request hash below and the
    // booking further down use the same value. The order row has to record the
    // currency the channel was actually offered: Alipay precreate only ever
    // prices in CNY, while WeChat V3 takes a currency field and the callback
    // compares it against the order. Hashing the raw field instead made the same
    // booking conflict with itself -- `"cny"` against `"CNY"`, or an absent field
    // against an explicit `"CNY"` -- and answered the second caller with 1004.
    const std::string requestedCurrency =
      channel == "wechat" ? request.get("currency", "CNY").asString() : std::string("CNY");
    bool currencyValid = false;
    const std::string currency = normalizeQrCurrency(requestedCurrency, currencyValid);

    // Idempotency: derive key from order_no + channel (same order can be re-requested).
    // (A1-4 fix: add idempotency protection to QR payment)
    std::string idempotencyKey =
      request.get("idempotency_key", "QR_" + orderNo + "_" + channel).asString();

    Json::Value reqHashObj;
    reqHashObj["order_no"] = orderNo;
    reqHashObj["amount"] = amount;
    reqHashObj["channel"] = channel;
    reqHashObj["subject"] = subject;
    // The hash has to cover every field that changes the booking. It used to stop
    // at `subject`, so a second caller naming another `user_id` -- or another
    // currency, buyer or callback URL -- for the same order number hashed to the
    // same request and was answered as a replay of the first caller's QR code.
    reqHashObj["user_id"] = static_cast<Json::Int64>(userId);
    reqHashObj["currency"] = currencyValid ? currency : requestedCurrency;
    reqHashObj["notify_url"] = request.get("notify_url", "").asString();
    reqHashObj["buyer_id"] = request.get("buyer_id", "").asString();
    reqHashObj["time_expire"] = timeExpire;
    std::string requestHash = drogon::utils::getSha256(pay::utils::toJsonString(reqHashObj));

    auto idempotencyService = idempotencyService_;

    idempotencyService->checkAndSetStatus(
      idempotencyKey,
      requestHash,
      [&request]() {
          Json::Value r;
          r["order_no"] = request.get("order_no", "").asString();
          r["amount"] = request.get("amount", "").asString();
          r["channel"] = request.get("channel", "alipay").asString();
          return r;
      }(),
      [this,
       orderNo,
       amount,
       channel,
       subject,
       userId,
       currency,
       currencyValid,
       timeExpire,
       request,
       sharedCb,
       idempotencyService,
       idempotencyKey,
       requestHash](const IdempotencyService::CheckResult &checkResult) mutable {
          if (checkResult.status == IdempotencyService::CheckStatus::Conflict)
          {
              Json::Value error;
              error["code"] = 1004;
              error["message"] = "Idempotency conflict: different parameters for same key";
              sharedCb->call(error, pay::makePayError(1004, "idempotency key conflict"));
              return;
          }

          if (checkResult.status == IdempotencyService::CheckStatus::InProgress)
          {
              Json::Value error;
              error["code"] = 1004;
              error["message"] = "Idempotency request is already in progress";
              sharedCb->call(error, pay::makePayError(1004, "idempotency request in progress"));
              return;
          }

          if (checkResult.status == IdempotencyService::CheckStatus::Error)
          {
              Json::Value error;
              error["code"] = 1003;
              error["message"] = "Idempotency check failed";
              sharedCb->call(error, pay::makePayError(1003, "idempotency check failed"));
              return;
          }

          if (checkResult.status == IdempotencyService::CheckStatus::Replay)
          {
              sharedCb->call(checkResult.cachedResult, std::error_code());
              return;
          }

          // Proceed with QR payment creation. The payload shape is per channel:
          // Alipay precreate takes `total_amount` in yuan, WeChat V3 native takes
          // `amount.total` in fen plus a `description`. Sending the Alipay field
          // names to WeChat made every WeChat QR order a 400 PARAM_ERROR.
          if (!currencyValid)
          {
              idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {});
              Json::Value response;
              response["code"] = 400;
              response["message"] = "Invalid currency: expected a three-letter ISO-4217 code";
              sharedCb->call(response, pay::makePayError(400, "invalid currency"));
              return;
          }

          // `time_expire` follows the create-route discipline: a shape the
          // channel can never honour, or a deadline outside its window, is
          // refused before the order exists -- the same asymmetry the struct
          // path documents (the channel moves a too-far deadline silently and
          // the two expiries then disagree).
          std::string timeExpireError;
          if (
            !timeExpire.empty() &&
            !pay::utils::validateTimeExpire(timeExpire, channel, timeExpireError)
          )
          {
              idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {});
              Json::Value response;
              response["code"] = 400;
              response["message"] = timeExpireError;
              sharedCb->call(response, pay::makePayError(400, timeExpireError));
              return;
          }

          Json::Value payload;
          payload["out_trade_no"] = orderNo;

          if (channel == "wechat")
          {
              int64_t totalFen = 0;
              if (!pay::utils::parseAmountToFen(amount, totalFen) || totalFen <= 0)
              {
                  idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {});
                  Json::Value response;
                  response["code"] = 400;
                  response["message"] = "Invalid amount for WeChat native payment";
                  sharedCb
                    ->call(response, pay::makePayError(400, "invalid amount for native payment"));
                  return;
              }
              // Same official window as /api/pay/create: an order number the
              // channel can never name must not be booked into the sweep set.
              {
                  std::string fieldError;
                  if (!pay::utils::validateWechatOrderFields(orderNo, subject, "", fieldError))
                  {
                      idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {
                      });
                      Json::Value response;
                      response["code"] = 400;
                      response["message"] = fieldError;
                      sharedCb->call(response, pay::makePayError(400, fieldError));
                      return;
                  }
              }
              payload["description"] = subject;
              payload["amount"]["total"] = static_cast<Json::Int64>(totalFen);
              payload["amount"]["currency"] = currency;
              const std::string notifyUrl = request.get("notify_url", "").asString();
              if (!notifyUrl.empty())
              {
                  // A caller-supplied callback address is forwarded to WeChat, so it
                  // goes through the same SSRF gate `/api/pay/create` applies (P1-3):
                  // without it this endpoint became a way to make WeChat POST payment
                  // notifications to an address inside the deployment network.
                  std::string urlError;
                  if (!pay::utils::validateNotifyUrl(notifyUrl, urlError))
                  {
                      idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {
                      });
                      Json::Value response;
                      response["code"] = 400;
                      response["message"] = urlError;
                      sharedCb->call(response, pay::makePayError(400, urlError));
                      return;
                  }
                  payload["notify_url"] = notifyUrl;
              }
              if (!timeExpire.empty())
              {
                  payload["time_expire"] = timeExpire;
              }
          }
          else
          {
              payload["total_amount"] = amount;
              payload["subject"] = subject;

              if (request.isMember("buyer_id"))
              {
                  payload["buyer_id"] = request["buyer_id"].asString();
              }
          }

          LOG_DEBUG << "[PaymentService] Creating QR payment: channel=" << channel
                    << ", order_no=" << orderNo << ", amount=" << amount;

          // Route through the channel registry; unknown channels are rejected
          // explicitly instead of assuming alipay.
          auto channelImpl = findChannel(channel);
          if (!channelImpl)
          {
              idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {});
              Json::Value response;
              response["code"] = 1005;
              response["message"] = "CHANNEL_NOT_AVAILABLE: " + channel;
              sharedCb
                ->call(response, pay::makePayError(1005, "channel not available: " + channel));
              return;
          }

          const std::string paymentNo = generatePaymentNoValue();
          const std::string requestPayload = pay::utils::toJsonString(payload);

          // Everything that can still be refused after the reservation is taken
          // answers through here: the reservation is released so a corrected retry
          // is not poisoned, and the transport error carries the same status the
          // body names. `extra` holds the channel's own code fields when it has
          // them.
          auto failQr = [sharedCb, idempotencyService, idempotencyKey, requestHash](
                          int code, const std::string &message, const Json::Value &extra
                        ) {
              idempotencyService->clearReservation(idempotencyKey, requestHash, [](bool) {});
              Json::Value response = extra;
              response["code"] = code;
              response["message"] = message;
              if (code >= 400 && code < 500)
              {
                  sharedCb->call(response, pay::makePayError(code, message));
                  return;
              }
              sharedCb->call(response, std::make_error_code(std::errc::io_error));
          };

          auto respondQr =
            [sharedCb, idempotencyService, idempotencyKey, requestHash](const Json::Value &data) {
                Json::Value response;
                response["code"] = 0;
                response["message"] = "QR code created successfully";
                response["data"] = data;

                // Persist the idempotency snapshot BEFORE responding so a retry
                // cannot observe an in-progress (NULL snapshot) reservation, and
                // release the reservation if the write fails so retries are not
                // poisoned. (B1-1 follow-up)
                idempotencyService->updateResult(
                  idempotencyKey,
                  requestHash,
                  response,
                  [sharedCb, idempotencyService, idempotencyKey, requestHash, response](
                    bool success
                  ) {
                      if (success)
                      {
                          sharedCb->call(response, std::error_code());
                          return;
                      }
                      LOG_ERROR << "[PaymentService] Failed to save QR idempotency "
                                   "snapshot; clearing reservation for key="
                                << idempotencyKey;
                      idempotencyService
                        ->clearReservation(idempotencyKey, requestHash, [sharedCb, response](bool) {
                            sharedCb->call(response, std::error_code());
                        });
                  }
                );
            };

          // The channel has accepted the order, so the rows have to say so before
          // the client is handed the code: payment INIT -> PROCESSING with the
          // channel payload, then order CREATED -> PAYING.
          auto promoteQrRows = [this, respondQr](
                                 const PayPaymentModel &payment,
                                 const PayOrderModel &order,
                                 const Json::Value &result,
                                 const Json::Value &data
                               ) {
              Json::Value channelResponse;
              channelResponse["channel_response"] = result;
              const std::string responseText = pay::utils::toJsonString(channelResponse);

              // A row update that faults still answers with the code. Withholding a
              // payable QR would only lose money that is already offered, and
              // CallbackService settles a non-final payment row when the
              // notification arrives, so the rows heal on their own.
              auto answer = [respondQr, data](const std::string &failureDetail) {
                  if (!failureDetail.empty())
                  {
                      LOG_WARN << "[PaymentService] QR status row not updated: " << failureDetail
                               << "; answering with the channel code anyway";
                  }
                  respondQr(data);
              };

              // Both writes name the columns they change and require the row to still
              // be in the state this attempt left it. A full-row write from the copy
              // taken before the channel answered would roll back whatever a fast
              // notification or reconcile already booked: the payment back from
              // SUCCESS to PROCESSING, the order back from PAID to PAYING.
              try
              {
                  Mapper<PayPaymentModel> paymentUpdater(dbClient_);
                  paymentUpdater.updateBy(
                    {PayPaymentModel::Cols::_status, PayPaymentModel::Cols::_response_payload},
                    [this, order, answer, payment](const size_t updated) {
                        if (updated == 0)
                        {
                            answer(
                              "payment " + payment.getValueOfPaymentNo() +
                              " had already moved out of INIT/PROCESSING"
                            );
                            return;
                        }
                        try
                        {
                            Mapper<PayOrderModel> orderUpdater(dbClient_);
                            orderUpdater.updateBy(
                              {PayOrderModel::Cols::_status},
                              [answer](const size_t) { answer(""); },
                              [answer](const DrogonDbException &e) { answer(e.base().what()); },
                              Criteria(
                                PayOrderModel::Cols::_order_no,
                                CompareOperator::EQ,
                                order.getValueOfOrderNo()
                              ) &&
                                Criteria(
                                  PayOrderModel::Cols::_status,
                                  CompareOperator::In,
                                  std::vector<std::string>{"CREATED", "PAYING"}
                                ),
                              "PAYING"
                            );
                        }
                        catch (const std::exception &e)
                        {
                            answer(e.what());
                        }
                        catch (...)
                        {
                            answer("unknown error");
                        }
                    },
                    [answer](const DrogonDbException &e) { answer(e.base().what()); },
                    Criteria(
                      PayPaymentModel::Cols::_payment_no,
                      CompareOperator::EQ,
                      payment.getValueOfPaymentNo()
                    ) &&
                      Criteria(
                        PayPaymentModel::Cols::_status,
                        CompareOperator::In,
                        std::vector<std::string>{"INIT", "PROCESSING"}
                      ),
                    "PROCESSING",
                    responseText
                  );
              }
              catch (const std::exception &e)
              {
                  answer(e.what());
              }
              catch (...)
              {
                  answer("unknown error");
              }
          };

          // A refusal closes the payment row only, and only while the row is still
          // the attempt this request booked. The order keeps its status because,
          // with `pay_payment` allowing several rows per order, it may still carry
          // an earlier attempt whose code is live; overwriting it with FAILED here
          // would hide money that is genuinely payable.
          // Runs from the channel callback, which a real (HTTP) channel fires
          // after the service may already be gone: hold the DB client, not `this`.
          auto markQrPaymentFailed = [db = dbClient_](
                                       const PayPaymentModel &payment, const std::string &message
                                     ) {
              Json::Value errJson;
              errJson["error"] = message;
              const std::string responseText = pay::utils::toJsonString(errJson);
              const std::string paymentNo = payment.getValueOfPaymentNo();
              auto reportFault = [paymentNo](const std::string &detail) {
                  LOG_WARN << "[PaymentService] Failed to record the QR payment failure for "
                           << paymentNo << ": " << detail;
              };
              try
              {
                  Mapper<PayPaymentModel> paymentUpdater(db);
                  paymentUpdater.updateBy(
                    {PayPaymentModel::Cols::_status, PayPaymentModel::Cols::_response_payload},
                    [reportFault, paymentNo](const size_t updated) {
                        if (updated == 0)
                        {
                            reportFault("the row had already moved out of INIT/PROCESSING");
                        }
                    },
                    [reportFault](const DrogonDbException &e) { reportFault(e.base().what()); },
                    Criteria(PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo) &&
                      Criteria(
                        PayPaymentModel::Cols::_status,
                        CompareOperator::In,
                        std::vector<std::string>{"INIT", "PROCESSING"}
                      ),
                    "FAIL",
                    responseText
                  );
              }
              catch (const std::exception &e)
              {
                  reportFault(e.what());
              }
              catch (...)
              {
                  reportFault("unknown error");
              }
          };

          // The order and its payment row are written before the channel is asked
          // for anything. This endpoint used to call the channel first and then
          // persist only a pay_order, so the notification for a paid QR order found
          // no payment row, answered FAIL, and the money sat on an order that could
          // never settle (audit item C5). Booking first also turns a database fault
          // into "no charge was offered" instead of an orphaned transaction.
          auto offerQrChannel =
            [channelImpl, payload, orderNo, channel, failQr, markQrPaymentFailed, promoteQrRows](
              const PayOrderModel &order, const PayPaymentModel &payment
            ) {
                channelImpl->createQRPayment(
                  payload,
                  [orderNo, channel, failQr, markQrPaymentFailed, promoteQrRows, order, payment](
                    const Json::Value &result, const std::string &error
                  ) {
                      if (!error.empty())
                      {
                          const std::string message = "QR payment creation failed: " + error;
                          if (attemptCertainlyNotCreated(error))
                          {
                              markQrPaymentFailed(payment, message);
                          }
                          else
                          {
                              // The channel may still have created the transaction.
                              // Closing the row would hide a code the user can pay
                              // from every recovery filter, so an attempt with an
                              // unknown outcome stays in flight for the notification
                              // or reconciliation to settle.
                              LOG_WARN << "[PaymentService] QR attempt "
                                       << payment.getValueOfPaymentNo()
                                       << " outcome unknown; leaving it in flight: " << error;
                          }
                          failQr(500, message, Json::Value());
                          return;
                      }

                      // Success is per channel: V3 has no business-code field, so a
                      // WeChat order is only accepted once it carries code_url.
                      const std::string resultError = channelResultError(channel, result);
                      if (!resultError.empty())
                      {
                          Json::Value extra;
                          if (channel == "alipay")
                          {
                              extra["alipay_code"] = result.get("code", "").asString();
                              extra["alipay_sub_code"] = result.get("sub_code", "").asString();
                          }
                          else
                          {
                              extra["wechat_code"] = result.get("code", "").asString();
                          }
                          if (attemptCertainlyNotCreated(resultError))
                          {
                              markQrPaymentFailed(payment, resultError);
                          }
                          else
                          {
                              LOG_WARN << "[PaymentService] QR attempt "
                                       << payment.getValueOfPaymentNo()
                                       << " answered without a payable code; leaving it in "
                                          "flight: "
                                       << resultError;
                          }
                          failQr(500, resultError, extra);
                          return;
                      }

                      Json::Value data;
                      data["order_no"] = orderNo;
                      if (channel == "wechat")
                      {
                          // Native transactions hand back a `weixin://` URL to render.
                          data["code_url"] = result.get("code_url", "").asString();
                      }
                      else
                      {
                          // Alipay precreate hands back the QR content plus the
                          // merchant order number echoed as `out_trade_no`. It is
                          // not Alipay's own number -- that is `trade_no`, which
                          // only exists once the buyer has paid.
                          if (result.isMember("qr_code"))
                          {
                              data["qr_code"] = result["qr_code"].asString();
                          }
                          if (result.isMember("out_trade_no"))
                          {
                              data["out_trade_no"] = result["out_trade_no"].asString();
                          }
                      }
                      promoteQrRows(payment, order, result, data);
                  }
                );
            };

          // One payment row per attempt: `pay_payment.order_no` is indexed, not
          // unique, so a retry appends rather than collides.
          auto bookQrPayment =
            [this, orderNo, amount, paymentNo, requestPayload, failQr, offerQrChannel](
              const PayOrderModel &order
            ) {
                try
                {
                    Mapper<PayPaymentModel> paymentMapper(dbClient_);
                    PayPaymentModel payment;
                    payment.setOrderNo(orderNo);
                    payment.setPaymentNo(paymentNo);
                    payment.setStatus("INIT");
                    payment.setAmount(amount);
                    payment.setRequestPayload(requestPayload);
                    payment.setCreatedAt(trantor::Date::now());

                    paymentMapper.insert(
                      payment,
                      [offerQrChannel, order](const PayPaymentModel &insertedPayment) {
                          offerQrChannel(order, insertedPayment);
                      },
                      [failQr](const DrogonDbException &e) {
                          failQr(
                            500,
                            "Failed to book the QR payment: " + std::string(e.base().what()),
                            Json::Value()
                          );
                      }
                    );
                }
                catch (const std::exception &e)
                {
                    failQr(
                      500, std::string("Failed to book the QR payment: ") + e.what(), Json::Value()
                    );
                }
                catch (...)
                {
                    failQr(500, "Failed to book the QR payment: unknown error", Json::Value());
                }
            };

          auto insertQrOrder = [this,
                                orderNo,
                                amount,
                                channel,
                                currency,
                                subject,
                                userId,
                                timeExpire,
                                request,
                                failQr,
                                bookQrPayment]() {
              try
              {
                  Mapper<PayOrderModel> orderMapper(dbClient_);
                  PayOrderModel newOrder;
                  newOrder.setOrderNo(orderNo);
                  newOrder.setAmount(amount);
                  newOrder.setCurrency(currency);
                  newOrder.setStatus("CREATED");
                  newOrder.setChannel(channel);
                  newOrder.setTitle(subject);
                  newOrder.setUserId(userId);
                  if (!timeExpire.empty())
                  {
                      // The shape was refused upstream if it could not parse;
                      // a late failure books the order without an expire_at and
                      // the close sweep simply never sees it -- same discipline
                      // as the create route.
                      int64_t expireSeconds = 0;
                      std::string expireError;
                      if (pay::utils::parseRfc3339(timeExpire, expireSeconds, expireError))
                      {
                          newOrder.setExpireAt(trantor::Date(expireSeconds * 1000000));
                      }
                      // No else: the validator upstream parses through this
                      // very function, so reaching here unparsable is not a
                      // state this route can produce -- deliberately silent.
                  }

                  orderMapper.insert(
                    newOrder,
                    [bookQrPayment](const PayOrderModel &insertedOrder) {
                        bookQrPayment(insertedOrder);
                    },
                    [failQr](const DrogonDbException &e) {
                        failQr(
                          500,
                          "Failed to book the QR order: " + std::string(e.base().what()),
                          Json::Value()
                        );
                    }
                  );
              }
              catch (const std::exception &e)
              {
                  failQr(
                    500, std::string("Failed to book the QR order: ") + e.what(), Json::Value()
                  );
              }
              catch (...)
              {
                  failQr(500, "Failed to book the QR order: unknown error", Json::Value());
              }
          };

          // A payment row that already carries money rules out a second code, no
          // matter what the order row says: the order and payment are written by
          // different code paths here, and an order left at CREATED by a faulted
          // status write can sit under a settled attempt.
          auto refuseIfAlreadySettled =
            [this, orderNo, failQr, bookQrPayment](const PayOrderModel &existingOrder) {
                try
                {
                    Mapper<PayPaymentModel> settledProbe(dbClient_);
                    settledProbe.findBy(
                      Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
                        Criteria(
                          PayPaymentModel::Cols::_status,
                          CompareOperator::In,
                          std::vector<std::string>{"SUCCESS", "REFUNDED"}
                        ),
                      [orderNo, existingOrder, failQr, bookQrPayment](
                        const std::vector<PayPaymentModel> &settledRows
                      ) {
                          if (!settledRows.empty())
                          {
                              failQr(
                                400,
                                "Order " + orderNo + " already has a settled payment (" +
                                  settledRows.front().getValueOfStatus() + ")",
                                Json::Value()
                              );
                              return;
                          }
                          bookQrPayment(existingOrder);
                      },
                      [orderNo, failQr](const DrogonDbException &e) {
                          failQr(
                            500,
                            "Failed to read the QR order's payment attempts: " +
                              std::string(e.base().what()),
                            Json::Value()
                          );
                      }
                    );
                }
                catch (const std::exception &e)
                {
                    failQr(
                      500,
                      std::string("Failed to read the QR order's payment attempts: ") + e.what(),
                      Json::Value()
                    );
                }
                catch (...)
                {
                    failQr(
                      500,
                      "Failed to read the QR order's payment attempts: unknown error",
                      Json::Value()
                    );
                }
            };

          // `pay_order.order_no` is UNIQUE, so a retry after a failure has to reuse
          // the row the first attempt left behind. Reuse is only safe while the
          // order still describes the same charge: money already taken must not be
          // offered a second code, and an order whose amount, currency, channel or
          // owner differs from this request would settle -- or hand a code for --
          // the wrong charge.
          try
          {
              Mapper<PayOrderModel> orderProbe(dbClient_);
              orderProbe.findBy(
                Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo),
                [orderNo,
                 amount,
                 channel,
                 currency,
                 userId,
                 request,
                 failQr,
                 refuseIfAlreadySettled,
                 insertQrOrder](const std::vector<PayOrderModel> &rows) {
                    if (rows.empty())
                    {
                        insertQrOrder();
                        return;
                    }

                    const PayOrderModel existing = rows.front();
                    const std::string existingStatus = existing.getValueOfStatus();
                    if (
                      existingStatus == "PAID" || existingStatus == "SUCCESS" ||
                      existingStatus == "REFUNDED"
                    )
                    {
                        failQr(
                          400,
                          "Order " + orderNo + " is already " + existingStatus +
                            " and cannot be offered again",
                          Json::Value()
                        );
                        return;
                    }
                    // Yuan strings are compared as money, not as text: "1.5" and
                    // "1.50" are one charge, and refusing that reuse would strand a
                    // paid order behind a formatting difference.
                    int64_t bookedFen = 0;
                    int64_t requestedFen = 0;
                    const bool bookedAsFen =
                      pay::utils::parseAmountToFen(existing.getValueOfAmount(), bookedFen);
                    const bool requestedAsFen = pay::utils::parseAmountToFen(amount, requestedFen);
                    const bool sameAmount = (bookedAsFen && requestedAsFen)
                                              ? bookedFen == requestedFen
                                              : existing.getValueOfAmount() == amount;
                    if (!sameAmount)
                    {
                        failQr(
                          400,
                          "Order " + orderNo + " exists with amount " + existing.getValueOfAmount(),
                          Json::Value()
                        );
                        return;
                    }
                    if (existing.getValueOfCurrency() != currency)
                    {
                        failQr(
                          400,
                          "Order " + orderNo + " exists with currency " +
                            existing.getValueOfCurrency(),
                          Json::Value()
                        );
                        return;
                    }
                    if (existing.getValueOfChannel() != channel)
                    {
                        failQr(
                          400,
                          "Order " + orderNo + " belongs to channel " +
                            existing.getValueOfChannel(),
                          Json::Value()
                        );
                        return;
                    }
                    // The order number is caller-supplied, so without this an entry
                    // that guesses another user's `order_no` would be offered a code
                    // for that user's charge -- and its notification would then be
                    // booked against the wrong owner.
                    if (existing.getValueOfUserId() != userId)
                    {
                        failQr(400, "Order " + orderNo + " belongs to another user", Json::Value());
                        return;
                    }
                    refuseIfAlreadySettled(existing);
                },
                [failQr](const DrogonDbException &e) {
                    failQr(
                      500,
                      "Failed to read the QR order: " + std::string(e.base().what()),
                      Json::Value()
                    );
                }
              );
          }
          catch (const std::exception &e)
          {
              failQr(500, std::string("Failed to read the QR order: ") + e.what(), Json::Value());
          }
          catch (...)
          {
              failQr(500, "Failed to read the QR order: unknown error", Json::Value());
          }
      }
    );
}

void PaymentService::queryOrder(const std::string &orderNo, PaymentCallback &&callback)
{
    if (!dbClient_)
    {
        Json::Value response;
        response["code"] = 1003;
        response["message"] = "Database client not available";
        callback(response, std::make_error_code(std::errc::io_error));
        return;
    }

    if (orderNo.empty())
    {
        Json::Value response;
        response["code"] = 1001;
        response["message"] = "Missing order_no parameter";
        callback(response, std::make_error_code(std::errc::invalid_argument));
        return;
    }

    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<PaymentCallback>(std::move(callback));

    // Query order from database
    try
    {
        Mapper<PayOrderModel> orderMapper(dbClient_);
        auto criteria = Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);

        orderMapper.findOne(
          criteria,
          [this, orderNo, sharedCb](const PayOrderModel &order) {
              Json::Value response;
              response["code"] = 0;
              response["message"] = "Order found";
              Json::Value data;
              data["order_no"] = order.getValueOfOrderNo();
              data["amount"] = order.getValueOfAmount();
              data["currency"] = order.getValueOfCurrency();
              data["status"] = order.getValueOfStatus();
              data["channel"] = order.getValueOfChannel();
              data["title"] = order.getValueOfTitle();
              data["user_id"] = static_cast<Json::Int64>(order.getValueOfUserId());

              const std::string channel = order.getValueOfChannel();
              LOG_DEBUG << "[PAYMENT_SERVICE] queryOrder: order_no=" << orderNo
                        << " channel=" << channel
                        << " current_status=" << data["status"].asString();

              // Query real-time status from the payment channel (via SPI).
              // The response-parsing lambdas stay channel-specific because the
              // raw JSON schemas differ.
              auto channelImpl = findChannel(channel);
              if (channel == "wechat" && channelImpl)
              {
                  // Query transaction from WeChat Pay
                  channelImpl->queryPayment(
                    orderNo,
                    [this,
                     orderNo,
                     data,
                     sharedCb](const Json::Value &result, const std::string &error) {
                        if (!error.empty())
                        {
                            // Return database data with error header.
                            // code=1 signals "degraded data" — client should check
                            // wechat_query_error. (A1-3 fix)
                            Json::Value innerResponse;
                            innerResponse["code"] = 1;
                            innerResponse["message"] = "Order found (with query error)";
                            innerResponse["data"] = data;
                            innerResponse["data"]["wechat_query_error"] = error;
                            if (*sharedCb)
                            {
                                (*sharedCb)(innerResponse, std::error_code());
                            }
                            return;
                        }

                        // Sync order status from WeChat response
                        syncOrderStatusFromWechat(
                          orderNo, result, [data, result, sharedCb](const std::string &status) {
                              Json::Value innerResponse;
                              innerResponse["code"] = 0;
                              innerResponse["message"] = "Order found";
                              innerResponse["data"] = data;

                              if (!status.empty())
                              {
                                  innerResponse["data"]["status"] = status;
                              }
                              const auto channelRefundNo = result.get("refund_id", "").asString();
                              if (!channelRefundNo.empty())
                              {
                                  innerResponse["data"]["channel_refund_no"] = channelRefundNo;
                              }
                              innerResponse["data"]["wechat_response"] = result;
                              if (*sharedCb)
                              {
                                  (*sharedCb)(innerResponse, std::error_code());
                              }
                          }
                        );
                    }
                  );
              }
              else if (channel == "alipay" && channelImpl)
              {
                  // Query trade from Alipay
                  LOG_DEBUG << "[PAYMENT_SERVICE] Querying Alipay API for order " << orderNo;
                  channelImpl->queryPayment(
                    orderNo,
                    [this,
                     orderNo,
                     data,
                     sharedCb](const Json::Value &result, const std::string &error) {
                        if (!error.empty())
                        {
                            LOG_ERROR << "[PAYMENT_SERVICE] Alipay query error for " << orderNo
                                      << ": " << error;
                            // Return database data with error header.
                            // code=1 signals "degraded data" — client should check
                            // alipay_query_error. (A1-3 fix)
                            Json::Value innerResponse;
                            innerResponse["code"] = 1;
                            innerResponse["message"] = "Order found (with query error)";
                            innerResponse["data"] = data;
                            innerResponse["data"]["alipay_query_error"] = error;
                            if (*sharedCb)
                            {
                                (*sharedCb)(innerResponse, std::error_code());
                            }
                            return;
                        }

                        LOG_DEBUG << "[PAYMENT_SERVICE] Alipay response for " << orderNo
                                  << " code=" << result.get("code", "?").asString()
                                  << " trade_status=" << result.get("trade_status", "?").asString();

                        // Sync order status from Alipay response
                        syncOrderStatusFromAlipay(
                          orderNo,
                          result,
                          [data, result, sharedCb, orderNo](const std::string &status) {
                              LOG_DEBUG
                                << "[PAYMENT_SERVICE] syncOrderStatusFromAlipay returned status="
                                << status << " for order " << orderNo;

                              Json::Value innerResponse;
                              innerResponse["code"] = 0;
                              innerResponse["message"] = "Order found";
                              innerResponse["data"] = data;

                              // Always update status if Alipay returns valid status
                              if (!status.empty())
                              {
                                  innerResponse["data"]["status"] = status;
                                  LOG_DEBUG << "[PAYMENT_SERVICE] Updated order status to: "
                                            << status;
                              }
                              else
                              {
                                  // If Alipay query failed or returned unknown status,
                                  // keep the database status
                                  LOG_DEBUG << "[PAYMENT_SERVICE] Alipay query failed, keeping "
                                               "database status: "
                                            << data["status"].asString();
                              }

                              const auto tradeNo = result.get("trade_no", "").asString();
                              if (!tradeNo.empty())
                              {
                                  innerResponse["data"]["trade_no"] = tradeNo;
                              }
                              innerResponse["data"]["alipay_response"] = result;

                              // Safely access status field for logging
                              const auto &finalStatus = innerResponse["data"]["status"];
                              if (finalStatus.isString())
                              {
                                  LOG_DEBUG << "[PAYMENT_SERVICE] Final response status="
                                            << finalStatus.asString() << " for order " << orderNo;
                              }
                              else
                              {
                                  LOG_DEBUG
                                    << "[PAYMENT_SERVICE] Final response status=<non-string type>"
                                    << " for order " << orderNo;
                              }

                              if (*sharedCb)
                              {
                                  (*sharedCb)(innerResponse, std::error_code());
                              }
                          }
                        );
                    }
                  );
              }
              else
              {
                  // Channel not registered, return database data
                  LOG_DEBUG << "[PAYMENT_SERVICE] Using database data for order " << orderNo
                            << " (channel=" << channel
                            << " has_channel=" << (channelImpl != nullptr) << ")";
                  response["data"] = data;
                  if (*sharedCb)
                  {
                      (*sharedCb)(response, std::error_code());
                  }
              }
          },
          [sharedCb](const DrogonDbException &e) {
              if (*sharedCb)
              {
                  Json::Value response;
                  response["code"] = 1004;
                  response["message"] = "Order not found: " + std::string(e.base().what());
                  (*sharedCb)(
                    response,
                    pay::makePayError(1004, "Order not found: " + std::string(e.base().what()))
                  );
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

void PaymentService::syncOrderStatusFromWechat(
  const std::string &orderNo,
  const Json::Value &result,
  std::function<void(const std::string &status)> &&rawCallback
)
{
    // Once-only wrapper (P0): the SUCCESS-branch fire-and-forget update and
    // its error branch could otherwise both invoke the callback.
    auto onceCb = pay::utils::makeOnceCallback<void(const std::string &)>(std::move(rawCallback));
    std::function<void(const std::string &)> callback = [onceCb](const std::string &status) {
        onceCb.call(status);
    };

    const std::string tradeState = result.get("trade_state", "").asString();
    if (tradeState.empty())
    {
        if (callback)
        {
            callback("");
        }
        return;
    }

    // Map trade state to order and payment status
    std::string orderStatus;
    std::string paymentStatus;
    pay::utils::mapTradeState(tradeState, orderStatus, paymentStatus);

    const std::string transactionId = result.get("transaction_id", "").asString();
    const std::string responsePayload = pay::utils::toJsonString(result);
    // Read the answer's amount here rather than from a database callback: `result`
    // is a reference the caller owns and only lives for this synchronous frame.
    const int64_t answerTotalFen = result["amount"].get("total", 0).asInt64();

    if (!dbClient_)
    {
        if (callback)
        {
            callback(orderStatus);
        }
        return;
    }

    LOG_DEBUG << "Sync order status from WeChat: order_no=" << orderNo
              << " trade_state=" << tradeState << " order_status=" << orderStatus
              << " payment_status=" << paymentStatus;

    // Find the latest payment record for this order
    try
    {
        Mapper<PayPaymentModel> paymentMapper(dbClient_);
        // "Latest payment" means the latest attempt that can still carry money: a
        // QR order keeps one row per precreate attempt and the newest may be one a
        // refusal closed, which the status CAS below could never have moved anyway.
        auto paymentCriteria =
          Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
          Criteria(
            PayPaymentModel::Cols::_status,
            CompareOperator::In,
            std::vector<std::string>{"INIT", "PROCESSING", "SUCCESS", "REFUNDED"}
          );

        paymentMapper.orderBy(PayPaymentModel::Cols::_created_at, SortOrder::DESC)
          .limit(1)
          .findBy(
            paymentCriteria,
            [this,
             orderNo,
             orderStatus,
             paymentStatus,
             transactionId,
             responsePayload,
             answerTotalFen,
             callback](const std::vector<PayPaymentModel> &rows) {
                if (rows.empty())
                {
                    if (callback)
                    {
                        callback(orderStatus);
                    }
                    return;
                }

                auto payment = rows.front();
                const auto paymentNo = payment.getValueOfPaymentNo();
                // An answer is only evidence about the trade it names, so the total
                // it reports has to be the total this payment row asked for before it
                // can settle anything. `REFUNDED` settles too: that state is reached
                // from a trade whose money did arrive.
                if (orderStatus == "PAID" || orderStatus == "REFUNDED")
                {
                    const std::string amountProblem =
                      reconcileAmountProblem(answerTotalFen, payment.getValueOfAmount());
                    if (!amountProblem.empty())
                    {
                        LOG_ERROR << "[PaymentService] Not settling " << orderNo
                                  << " from the channel answer: " << amountProblem;
                        if (callback)
                        {
                            callback("");
                        }
                        return;
                    }
                }

                // Use transaction for atomic updates
                dbClient_->newTransactionAsync([orderNo,
                                                orderStatus,
                                                paymentStatus,
                                                transactionId,
                                                responsePayload,
                                                payment,
                                                paymentNo,
                                                callback](
                                                 const std::shared_ptr<Transaction> &transPtr
                                               ) mutable {
                    auto rollbackDone = [callback, transPtr](const DrogonDbException &e) {
                        LOG_ERROR << "Reconcile transaction error: " << e.base().what();
                        transPtr->rollback();
                        if (callback)
                        {
                            callback("");
                        }
                    };

                    auto transDb = std::static_pointer_cast<DbClient>(transPtr);

                    // A `trade_state=REFUND` answer is a claim that the whole order
                    // came back, but one settled partial refund of an order is
                    // enough to produce it, so the ledger gets the vote. Statements
                    // issued on one transaction run in submission order: both
                    // order-write sites below read this sum already resolved. A
                    // failed read leaves the transaction aborted, and the
                    // statements queued after it fall into the existing error
                    // paths, which roll back and report.
                    // Aggregate SUM (raw-SQL exemption #3): the Mapper cannot express SUM.
                    auto settledRefundFen = std::make_shared<int64_t>(0);
                    if (orderStatus == "REFUNDED")
                    {
                        transPtr->execSqlAsync(
                          "SELECT COALESCE(SUM(CAST(amount AS NUMERIC)), 0) AS sum_amount "
                          "FROM pay_refund WHERE order_no = $1 AND status = $2",
                          [settledRefundFen, orderNo](const Result &r) {
                              if (!r.empty())
                              {
                                  const auto sumText = r.front()["sum_amount"].as<std::string>();
                                  if (!pay::utils::parseAmountToFen(sumText, *settledRefundFen))
                                  {
                                      LOG_ERROR << "[PaymentService] Settled-refund sum for "
                                                << orderNo
                                                << " is not a usable amount: " << sumText;
                                      *settledRefundFen = 0;
                                  }
                              }
                          },
                          [orderNo](const DrogonDbException &e) {
                              LOG_ERROR << "[PaymentService] Settled-refund sum for " << orderNo
                                        << " failed: " << e.base().what();
                          },
                          orderNo,
                          std::string("REFUND_SUCCESS")
                        );
                    }

                    // If payment is already SUCCESS, only update order
                    if (payment.getValueOfStatus() == "SUCCESS")
                    {
                        try
                        {
                            Mapper<PayOrderModel> orderMapper(transPtr);
                            auto orderCriteria = Criteria(
                              PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo
                            );
                            orderMapper.findOne(
                              orderCriteria,
                              [orderStatus,
                               paymentNo,
                               callback,
                               transPtr,
                               transDb,
                               settledRefundFen](PayOrderModel order) {
                                  // The sum queued at the top of this transaction has
                                  // landed by now, so the answer's REFUNDED claim meets
                                  // the ledger before it reaches the row.
                                  const std::string finalOrderStatus =
                                    orderStatusAfterRefundCoverage(
                                      orderStatus, order.getValueOfAmount(), *settledRefundFen
                                    );
                                  if (order.getValueOfStatus() != "PAID")
                                  {
                                      const auto userId = order.getValueOfUserId();
                                      const auto orderAmount = order.getValueOfAmount();
                                      const auto orderNo = order.getValueOfOrderNo();
                                      order.setStatus(finalOrderStatus);
                                      try
                                      {
                                          Mapper<PayOrderModel> orderUpdater(transPtr);
                                          orderUpdater.update(
                                            order,
                                            [callback,
                                             userId,
                                             orderNo,
                                             paymentNo,
                                             orderAmount,
                                             finalOrderStatus,
                                             transPtr,
                                             transDb](const size_t) {
                                                if (
                                                  finalOrderStatus == "PAID" ||
                                                  finalOrderStatus == "REFUNDED"
                                                )
                                                {
                                                    insertLedgerEntry(
                                                      transDb,
                                                      userId,
                                                      orderNo,
                                                      paymentNo,
                                                      "PAYMENT",
                                                      orderAmount
                                                    );
                                                }
                                                if (callback)
                                                {
                                                    callback(finalOrderStatus);
                                                }
                                            },
                                            [callback, transPtr](const DrogonDbException &e) {
                                                LOG_ERROR << "Reconcile order update error: "
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
                                          LOG_ERROR << "Reconcile order update error: " << e.what();
                                          transPtr->rollback();
                                          if (callback)
                                          {
                                              callback("");
                                          }
                                      }
                                      catch (...)
                                      {
                                          LOG_ERROR
                                            << "Reconcile order update error: unknown exception";
                                          transPtr->rollback();
                                          if (callback)
                                          {
                                              callback("");
                                          }
                                      }
                                      // The callback fires from the update lambdas above;
                                      // reporting success here would race the async update
                                      // and mask its failure.
                                      return;
                                  }
                                  // Order already PAID: report what this round actually
                                  // established, not the channel's raw claim.
                                  if (callback)
                                  {
                                      callback(finalOrderStatus);
                                  }
                              },
                              [callback, transPtr](const DrogonDbException &e) {
                                  LOG_ERROR << "Reconcile order select error: " << e.base().what();
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
                            LOG_ERROR << "Reconcile order select error: " << e.what();
                            transPtr->rollback();
                            if (callback)
                            {
                                callback("");
                            }
                        }
                        catch (...)
                        {
                            LOG_ERROR << "Reconcile order select error: unknown exception";
                            transPtr->rollback();
                            if (callback)
                            {
                                callback("");
                            }
                        }
                        return;
                    }

                    // Update payment status with concurrency control
                    // Check if payment is already in a final state to prevent concurrent updates
                    const std::string currentStatus = payment.getValueOfStatus();
                    if (currentStatus == "SUCCESS" || currentStatus == "REFUNDED")
                    {
                        // Payment already in final state, no need to update
                        LOG_DEBUG << "[PaymentService] Payment " << paymentNo
                                  << " already in final state: " << currentStatus;
                        transPtr->rollback();
                        if (callback)
                        {
                            callback(currentStatus == "SUCCESS" ? "PAID" : "REFUNDED");
                        }
                        return;
                    }

                    payment.setStatus(paymentStatus);
                    payment.setChannelTradeNo(transactionId);
                    payment.setResponsePayload(responsePayload);
                    // CAS-style status transition: only update if still non-final, so a
                    // concurrent callback/reconcile that already advanced this payment
                    // is not overwritten (lost-update prevention). Uses UPDATE...RETURNING
                    // (raw-SQL exemption #2) so an empty result set reveals the lost race.
                    transPtr->execSqlAsync(
                      "UPDATE pay_payment "
                      "SET status = $1, channel_trade_no = $2, response_payload = $3 "
                      "WHERE payment_no = $4 "
                      "AND status IN ('INIT', 'PROCESSING') RETURNING 1",
                      [orderNo,
                       orderStatus,
                       paymentNo,
                       callback,
                       transPtr,
                       transDb,
                       settledRefundFen](const Result &casResult) {
                          if (casResult.size() == 0)
                          {
                              LOG_DEBUG
                                << "[PaymentService] Reconcile: payment already advanced by "
                                   "concurrent txn: "
                                << paymentNo << ", skipping";
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback(orderStatus);
                              }
                              return;
                          }
                          // Update order status
                          try
                          {
                              Mapper<PayOrderModel> orderMapper(transPtr);
                              auto orderCriteria = Criteria(
                                PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo
                              );
                              orderMapper.findOne(
                                orderCriteria,
                                [orderStatus,
                                 paymentNo,
                                 callback,
                                 transPtr,
                                 transDb,
                                 settledRefundFen](PayOrderModel order) {
                                    const std::string finalOrderStatus =
                                      orderStatusAfterRefundCoverage(
                                        orderStatus, order.getValueOfAmount(), *settledRefundFen
                                      );
                                    if (order.getValueOfStatus() == "PAID")
                                    {
                                        if (callback)
                                        {
                                            callback(finalOrderStatus);
                                        }
                                        return;
                                    }
                                    const auto userId = order.getValueOfUserId();
                                    const auto orderAmount = order.getValueOfAmount();
                                    const auto orderNo = order.getValueOfOrderNo();
                                    order.setStatus(finalOrderStatus);
                                    try
                                    {
                                        Mapper<PayOrderModel> orderUpdater(transPtr);
                                        orderUpdater.update(
                                          order,
                                          [callback,
                                           finalOrderStatus,
                                           userId,
                                           orderNo,
                                           paymentNo,
                                           orderAmount,
                                           transPtr,
                                           transDb](const size_t) {
                                              if (
                                                finalOrderStatus == "PAID" ||
                                                finalOrderStatus == "REFUNDED"
                                              )
                                              {
                                                  insertLedgerEntry(
                                                    transDb,
                                                    userId,
                                                    orderNo,
                                                    paymentNo,
                                                    "PAYMENT",
                                                    orderAmount
                                                  );
                                              }
                                              if (callback)
                                              {
                                                  callback(finalOrderStatus);
                                              }
                                          },
                                          [callback, transPtr](const DrogonDbException &e) {
                                              LOG_ERROR << "Reconcile order update error: "
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
                                        LOG_ERROR << "Reconcile order update error: " << e.what();
                                        transPtr->rollback();
                                        if (callback)
                                        {
                                            callback("");
                                        }
                                    }
                                    catch (...)
                                    {
                                        LOG_ERROR
                                          << "Reconcile order update error: unknown exception";
                                        transPtr->rollback();
                                        if (callback)
                                        {
                                            callback("");
                                        }
                                    }
                                },
                                [callback, transPtr](const DrogonDbException &e) {
                                    LOG_ERROR << "Reconcile order select error: "
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
                              LOG_ERROR << "Reconcile order select error: " << e.what();
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback("");
                              }
                          }
                          catch (...)
                          {
                              LOG_ERROR << "Reconcile order select error: unknown exception";
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback("");
                              }
                          }
                      },
                      rollbackDone,
                      paymentStatus,
                      transactionId,
                      responsePayload,
                      paymentNo
                    );
                });
            },
            [callback](const DrogonDbException &e) {
                LOG_ERROR << "Reconcile payment select error: " << e.base().what();
                if (callback)
                {
                    callback("");
                }
            }
          );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Reconcile payment select error: " << e.what();
        if (callback)
        {
            callback("");
        }
    }
    catch (...)
    {
        LOG_ERROR << "Reconcile payment select error: unknown exception";
        if (callback)
        {
            callback("");
        }
    }
}

void PaymentService::syncOrderStatusFromAlipay(
  const std::string &orderNo,
  const Json::Value &result,
  std::function<void(const std::string &status)> &&rawCallback
)
{
    // Once-only wrapper (P0): mirrors the WeChat reconcile path.
    auto onceCb = pay::utils::makeOnceCallback<void(const std::string &)>(std::move(rawCallback));
    std::function<void(const std::string &)> callback = [onceCb](const std::string &status) {
        onceCb.call(status);
    };

    const std::string responseCode = result.get("code", "").asString();
    if (responseCode != "10000")
    {
        // Alipay API call failed or trade not found
        if (callback)
        {
            callback("");
        }
        return;
    }

    const std::string tradeStatus = result.get("trade_status", "").asString();
    if (tradeStatus.empty())
    {
        if (callback)
        {
            callback("");
        }
        return;
    }

    // Map Alipay trade_status to order and payment status
    std::string orderStatus;
    std::string paymentStatus;

    if (tradeStatus == "TRADE_SUCCESS" || tradeStatus == "TRADE_FINISHED")
    {
        orderStatus = "PAID";
        paymentStatus = "SUCCESS";
    }
    else if (tradeStatus == "WAIT_BUYER_PAY")
    {
        orderStatus = "PAYING";
        paymentStatus = "PROCESSING";
    }
    else if (tradeStatus == "TRADE_CLOSED")
    {
        orderStatus = "FAILED";
        // The payment column's vocabulary is FAIL (what `mapTradeState` writes for
        // the same outcome on the WeChat side); FAILED is an order status. Spelling
        // it FAILED here left a row that no FAIL-keyed query could find.
        paymentStatus = "FAIL";
    }
    else
    {
        // Unknown status
        LOG_WARN << "Unknown Alipay trade_status: " << tradeStatus << " for order " << orderNo;
        if (callback)
        {
            callback("");
        }
        return;
    }

    const std::string transactionId = result.get("trade_no", "").asString();
    const std::string responsePayload = pay::utils::toJsonString(result);

    // Alipay mandates that the notification's total_amount equals the merchant
    // order amount before the order is treated as paid. Resolve the notified
    // amount to fen once, in this synchronous frame -- `result` is a reference
    // the caller owns, so a database callback must not read it; a PAID
    // transition with an unverifiable amount is rejected below (defends against
    // a low-value payment confirming a high-value order).
    int64_t notifiedFen = -1;
    if (orderStatus == "PAID")
    {
        const std::string notifiedAmount = result.get("total_amount", "").asString();
        if (!pay::utils::parseAmountToFen(notifiedAmount, notifiedFen))
        {
            notifiedFen = -1;
        }
    }

    if (!dbClient_)
    {
        if (callback)
        {
            callback(orderStatus);
        }
        return;
    }

    LOG_DEBUG << "Sync order status from Alipay: order_no=" << orderNo
              << " trade_status=" << tradeStatus << " order_status=" << orderStatus
              << " payment_status=" << paymentStatus;

    // Find the latest payment record for this order
    try
    {
        Mapper<PayPaymentModel> paymentMapper(dbClient_);
        // "Latest payment" means the latest attempt that can still carry money: a
        // QR order keeps one row per precreate attempt and the newest may be one a
        // refusal closed, which the status CAS below could never have moved anyway.
        auto paymentCriteria =
          Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo) &&
          Criteria(
            PayPaymentModel::Cols::_status,
            CompareOperator::In,
            std::vector<std::string>{"INIT", "PROCESSING", "SUCCESS", "REFUNDED"}
          );

        paymentMapper.orderBy(PayPaymentModel::Cols::_created_at, SortOrder::DESC)
          .limit(1)
          .findBy(
            paymentCriteria,
            [this,
             orderNo,
             orderStatus,
             paymentStatus,
             transactionId,
             responsePayload,
             notifiedFen,
             callback](const std::vector<PayPaymentModel> &rows) {
                if (rows.empty())
                {
                    if (callback)
                    {
                        callback(orderStatus);
                    }
                    return;
                }

                auto payment = rows.front();
                const auto paymentNo = payment.getValueOfPaymentNo();

                // A notification is only evidence about the trade it names, so the
                // total it reports has to be the total this payment row asked for
                // before anything settles. This is the earlier of the two amount
                // gates: this one reads the payment row, the ones inside the
                // transaction read the order row, and a divergence between the two
                // rows is exactly what the pair is there to catch.
                if (orderStatus == "PAID")
                {
                    const std::string amountProblem =
                      reconcileAmountProblem(notifiedFen, payment.getValueOfAmount());
                    if (!amountProblem.empty())
                    {
                        LOG_ERROR << "[PaymentService] Not settling " << orderNo
                                  << " from the channel answer: " << amountProblem;
                        if (callback)
                        {
                            callback("");
                        }
                        return;
                    }
                }

                // Use transaction for atomic updates
                dbClient_->newTransactionAsync([orderNo,
                                                orderStatus,
                                                paymentStatus,
                                                transactionId,
                                                responsePayload,
                                                notifiedFen,
                                                payment,
                                                paymentNo,
                                                callback](
                                                 const std::shared_ptr<Transaction> &transPtr
                                               ) mutable {
                    auto rollbackDone = [callback, transPtr](const DrogonDbException &e) {
                        LOG_ERROR << "Alipay reconcile transaction error: " << e.base().what();
                        transPtr->rollback();
                        if (callback)
                        {
                            callback("");
                        }
                    };

                    auto transDb = std::static_pointer_cast<DbClient>(transPtr);

                    // If payment is already SUCCESS, only update order
                    if (payment.getValueOfStatus() == "SUCCESS")
                    {
                        try
                        {
                            Mapper<PayOrderModel> orderMapper(transPtr);
                            auto orderCriteria = Criteria(
                              PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo
                            );
                            orderMapper.findOne(
                              orderCriteria,
                              [orderStatus, paymentNo, callback, transPtr, transDb, notifiedFen](
                                PayOrderModel order
                              ) {
                                  if (order.getValueOfStatus() != "PAID")
                                  {
                                      const auto userId = order.getValueOfUserId();
                                      const auto orderAmount = order.getValueOfAmount();
                                      const auto orderNo = order.getValueOfOrderNo();
                                      // Amount consistency gate: never credit a PAID order
                                      // whose stored amount differs from what the callback
                                      // says was actually paid (Alipay notification rule).
                                      if (
                                        orderStatus == "PAID" &&
                                        !pay::utils::amountEqualsFen(orderAmount, notifiedFen)
                                      )
                                      {
                                          int64_t orderFen = -1;
                                          pay::utils::parseAmountToFen(orderAmount, orderFen);
                                          LOG_ERROR << "Alipay reconcile REJECTED: amount mismatch"
                                                    << " for order " << orderNo
                                                    << " (notified fen=" << notifiedFen
                                                    << ", order fen=" << orderFen << ")";
                                          transPtr->rollback();
                                          if (callback)
                                          {
                                              callback("");
                                          }
                                          return;
                                      }
                                      order.setStatus(orderStatus);
                                      try
                                      {
                                          Mapper<PayOrderModel> orderUpdater(transPtr);
                                          orderUpdater.update(
                                            order,
                                            [callback,
                                             userId,
                                             orderNo,
                                             paymentNo,
                                             orderAmount,
                                             orderStatus,
                                             transPtr,
                                             transDb](const size_t) {
                                                if (orderStatus == "PAID")
                                                {
                                                    insertLedgerEntry(
                                                      transDb,
                                                      userId,
                                                      orderNo,
                                                      paymentNo,
                                                      "PAYMENT",
                                                      orderAmount
                                                    );
                                                }
                                                if (callback)
                                                {
                                                    callback(orderStatus);
                                                }
                                            },
                                            [callback, transPtr](const DrogonDbException &e) {
                                                LOG_ERROR << "Alipay reconcile order update error: "
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
                                          LOG_ERROR << "Alipay reconcile order update error: "
                                                    << e.what();
                                          transPtr->rollback();
                                          if (callback)
                                          {
                                              callback("");
                                          }
                                      }
                                      catch (...)
                                      {
                                          LOG_ERROR << "Alipay reconcile order update error: "
                                                       "unknown exception";
                                          transPtr->rollback();
                                          if (callback)
                                          {
                                              callback("");
                                          }
                                      }
                                      // The callback fires from the update lambdas above;
                                      // reporting success here would race the async update
                                      // and mask its failure.
                                      return;
                                  }
                                  // Order already PAID, no update needed
                                  if (callback)
                                  {
                                      callback(orderStatus);
                                  }
                              },
                              [callback, transPtr](const DrogonDbException &e) {
                                  LOG_ERROR << "Alipay reconcile order select error: "
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
                            LOG_ERROR << "Alipay reconcile order select error: " << e.what();
                            transPtr->rollback();
                            if (callback)
                            {
                                callback("");
                            }
                        }
                        catch (...)
                        {
                            LOG_ERROR << "Alipay reconcile order select error: unknown exception";
                            transPtr->rollback();
                            if (callback)
                            {
                                callback("");
                            }
                        }
                        return;
                    }

                    // Update payment status with concurrency control
                    // Check if payment is already in a final state to prevent concurrent updates
                    const std::string currentStatus = payment.getValueOfStatus();
                    if (currentStatus == "SUCCESS" || currentStatus == "REFUNDED")
                    {
                        // Payment already in final state, no need to update
                        LOG_DEBUG << "[PaymentService] Payment " << paymentNo
                                  << " already in final state: " << currentStatus;
                        transPtr->rollback();
                        if (callback)
                        {
                            callback(currentStatus == "SUCCESS" ? "PAID" : "REFUNDED");
                        }
                        return;
                    }

                    payment.setStatus(paymentStatus);
                    payment.setChannelTradeNo(transactionId);
                    payment.setResponsePayload(responsePayload);
                    // CAS-style status transition (mirrors WeChat reconcile path). Uses
                    // UPDATE...RETURNING (raw-SQL exemption #2) so an empty result set
                    // reveals the lost race.
                    transPtr->execSqlAsync(
                      "UPDATE pay_payment "
                      "SET status = $1, channel_trade_no = $2, response_payload = $3 "
                      "WHERE payment_no = $4 "
                      "AND status IN ('INIT', 'PROCESSING') RETURNING 1",
                      [orderNo, orderStatus, paymentNo, callback, transPtr, transDb, notifiedFen](
                        const Result &casResult
                      ) {
                          if (casResult.size() == 0)
                          {
                              LOG_DEBUG
                                << "[PaymentService] Alipay reconcile: payment already advanced "
                                   "by concurrent txn: "
                                << paymentNo << ", skipping";
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback(orderStatus);
                              }
                              return;
                          }
                          // Update order status
                          try
                          {
                              Mapper<PayOrderModel> orderMapper(transPtr);
                              auto orderCriteria = Criteria(
                                PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo
                              );
                              orderMapper.findOne(
                                orderCriteria,
                                [orderStatus, paymentNo, callback, transPtr, transDb, notifiedFen](
                                  PayOrderModel order
                                ) {
                                    if (order.getValueOfStatus() == "PAID")
                                    {
                                        if (callback)
                                        {
                                            callback(orderStatus);
                                        }
                                        return;
                                    }
                                    const auto userId = order.getValueOfUserId();
                                    const auto orderAmount = order.getValueOfAmount();
                                    const auto orderNo = order.getValueOfOrderNo();
                                    // Amount consistency gate: never credit a PAID order
                                    // whose stored amount differs from what the callback
                                    // says was actually paid (Alipay notification rule).
                                    if (
                                      orderStatus == "PAID" &&
                                      !pay::utils::amountEqualsFen(orderAmount, notifiedFen)
                                    )
                                    {
                                        int64_t orderFen = -1;
                                        pay::utils::parseAmountToFen(orderAmount, orderFen);
                                        LOG_ERROR << "Alipay reconcile REJECTED: amount mismatch"
                                                  << " for order " << orderNo
                                                  << " (notified fen=" << notifiedFen
                                                  << ", order fen=" << orderFen << ")";
                                        transPtr->rollback();
                                        if (callback)
                                        {
                                            callback("");
                                        }
                                        return;
                                    }
                                    order.setStatus(orderStatus);
                                    try
                                    {
                                        Mapper<PayOrderModel> orderUpdater(transPtr);
                                        orderUpdater.update(
                                          order,
                                          [callback,
                                           orderStatus,
                                           userId,
                                           orderNo,
                                           paymentNo,
                                           orderAmount,
                                           transPtr,
                                           transDb](const size_t) {
                                              if (orderStatus == "PAID")
                                              {
                                                  insertLedgerEntry(
                                                    transDb,
                                                    userId,
                                                    orderNo,
                                                    paymentNo,
                                                    "PAYMENT",
                                                    orderAmount
                                                  );
                                              }
                                              if (callback)
                                              {
                                                  callback(orderStatus);
                                              }
                                          },
                                          [callback, transPtr](const DrogonDbException &e) {
                                              LOG_ERROR << "Alipay reconcile order update error: "
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
                                        LOG_ERROR << "Alipay reconcile order update error: "
                                                  << e.what();
                                        transPtr->rollback();
                                        if (callback)
                                        {
                                            callback("");
                                        }
                                    }
                                    catch (...)
                                    {
                                        LOG_ERROR << "Alipay reconcile order update error: unknown "
                                                     "exception";
                                        transPtr->rollback();
                                        if (callback)
                                        {
                                            callback("");
                                        }
                                    }
                                },
                                [callback, transPtr](const DrogonDbException &e) {
                                    LOG_ERROR << "Alipay reconcile order select error: "
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
                              LOG_ERROR << "Alipay reconcile order select error: " << e.what();
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback("");
                              }
                          }
                          catch (...)
                          {
                              LOG_ERROR << "Alipay reconcile order select error: unknown exception";
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback("");
                              }
                          }
                      },
                      rollbackDone,
                      paymentStatus,
                      transactionId,
                      responsePayload,
                      paymentNo
                    );
                });
            },
            [callback](const DrogonDbException &e) {
                LOG_ERROR << "Alipay reconcile payment select error: " << e.base().what();
                if (callback)
                {
                    callback("");
                }
            }
          );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Alipay reconcile payment select error: " << e.what();
        if (callback)
        {
            callback("");
        }
    }
    catch (...)
    {
        LOG_ERROR << "Alipay reconcile payment select error: unknown exception";
        if (callback)
        {
            callback("");
        }
    }
}

void PaymentService::reconcileSummary(const std::string & /*date*/, PaymentCallback &&callback)
{
    if (!dbClient_)
    {
        Json::Value response;
        response["code"] = 1003;
        response["message"] = "Database client not available";
        callback(response, std::make_error_code(std::errc::io_error));
        return;
    }

    auto responded = std::make_shared<std::atomic<bool>>(false);
    auto pending = std::make_shared<std::atomic<int>>(2);
    auto summary = std::make_shared<Json::Value>();
    (*summary)["paying_orders"] = 0;
    (*summary)["refunding_refunds"] = 0;
    (*summary)["oldest_paying_updated"] = "";
    (*summary)["oldest_refund_updated"] = "";

    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<PaymentCallback>(std::move(callback));

    auto finishIfReady = [sharedCb, responded, pending, summary]() {
        if (pending->fetch_sub(1) != 1)
        {
            return;
        }
        if (responded->exchange(true))
        {
            return;
        }
        if (*sharedCb)
        {
            Json::Value response;
            response["code"] = 0;
            response["message"] = "Reconciliation summary";
            response["data"] = *summary;
            (*sharedCb)(response, std::error_code());
        }
    };

    // Query paying orders (COUNT + oldest updated_at). The aggregate pair is
    // split into Mapper::count() and an ORDER BY ... LIMIT 1 probe; atomic
    // consistency between the two values is not required for this monitoring
    // summary.
    try
    {
        Mapper<PayOrderModel> orderCounter(dbClient_);
        orderCounter.count(
          Criteria(PayOrderModel::Cols::_status, CompareOperator::EQ, "PAYING"),
          [this, summary, finishIfReady, sharedCb, responded](const size_t cnt) {
              (*summary)["paying_orders"] = static_cast<Json::Int64>(cnt);
              try
              {
                  Mapper<PayOrderModel> oldestProbe(dbClient_);
                  oldestProbe.orderBy(PayOrderModel::Cols::_updated_at, SortOrder::ASC)
                    .limit(1)
                    .findBy(
                      Criteria(PayOrderModel::Cols::_status, CompareOperator::EQ, "PAYING"),
                      [summary, finishIfReady](const std::vector<PayOrderModel> &rows) {
                          if (!rows.empty() && rows.front().getUpdatedAt())
                          {
                              (*summary)["oldest_paying_updated"] =
                                rows.front().getValueOfUpdatedAt().toDbStringLocal();
                          }
                          finishIfReady();
                      },
                      [sharedCb, responded](const DrogonDbException &e) {
                          if (responded->exchange(true))
                          {
                              return;
                          }
                          if (*sharedCb)
                          {
                              Json::Value response;
                              response["code"] = 1003;
                              response["message"] =
                                "Database error: " + std::string(e.base().what());
                              (*sharedCb)(response, std::make_error_code(std::errc::io_error));
                          }
                      }
                    );
              }
              catch (const std::exception &e)
              {
                  if (!responded->exchange(true))
                  {
                      reportMapperFailure(sharedCb, e.what());
                  }
              }
              catch (...)
              {
                  if (!responded->exchange(true))
                  {
                      reportMapperFailure(sharedCb, "unknown exception");
                  }
              }
          },
          [sharedCb, responded](const DrogonDbException &e) {
              if (responded->exchange(true))
              {
                  return;
              }
              if (*sharedCb)
              {
                  Json::Value response;
                  response["code"] = 1003;
                  response["message"] = "Database error: " + std::string(e.base().what());
                  (*sharedCb)(response, std::make_error_code(std::errc::io_error));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        if (!responded->exchange(true))
        {
            reportMapperFailure(sharedCb, e.what());
        }
    }
    catch (...)
    {
        if (!responded->exchange(true))
        {
            reportMapperFailure(sharedCb, "unknown exception");
        }
    }

    // Query refunding refunds (same count + oldest-probe split as above).
    try
    {
        Mapper<PayRefundModel> refundCounter(dbClient_);
        refundCounter.count(
          Criteria(
            PayRefundModel::Cols::_status,
            CompareOperator::In,
            std::vector<std::string>{"REFUND_INIT", "REFUNDING"}
          ),
          [this, summary, finishIfReady, sharedCb, responded](const size_t cnt) {
              (*summary)["refunding_refunds"] = static_cast<Json::Int64>(cnt);
              try
              {
                  Mapper<PayRefundModel> oldestProbe(dbClient_);
                  oldestProbe.orderBy(PayRefundModel::Cols::_updated_at, SortOrder::ASC)
                    .limit(1)
                    .findBy(
                      Criteria(
                        PayRefundModel::Cols::_status,
                        CompareOperator::In,
                        std::vector<std::string>{"REFUND_INIT", "REFUNDING"}
                      ),
                      [summary, finishIfReady](const std::vector<PayRefundModel> &rows) {
                          if (!rows.empty() && rows.front().getUpdatedAt())
                          {
                              (*summary)["oldest_refund_updated"] =
                                rows.front().getValueOfUpdatedAt().toDbStringLocal();
                          }
                          finishIfReady();
                      },
                      [sharedCb, responded](const DrogonDbException &e) {
                          if (responded->exchange(true))
                          {
                              return;
                          }
                          if (*sharedCb)
                          {
                              Json::Value response;
                              response["code"] = 1003;
                              response["message"] =
                                "Database error: " + std::string(e.base().what());
                              (*sharedCb)(response, std::make_error_code(std::errc::io_error));
                          }
                      }
                    );
              }
              catch (const std::exception &e)
              {
                  if (!responded->exchange(true))
                  {
                      reportMapperFailure(sharedCb, e.what());
                  }
              }
              catch (...)
              {
                  if (!responded->exchange(true))
                  {
                      reportMapperFailure(sharedCb, "unknown exception");
                  }
              }
          },
          [sharedCb, responded](const DrogonDbException &e) {
              if (responded->exchange(true))
              {
                  return;
              }
              if (*sharedCb)
              {
                  Json::Value response;
                  response["code"] = 1003;
                  response["message"] = "Database error: " + std::string(e.base().what());
                  (*sharedCb)(response, std::make_error_code(std::errc::io_error));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        if (!responded->exchange(true))
        {
            reportMapperFailure(sharedCb, e.what());
        }
    }
    catch (...)
    {
        if (!responded->exchange(true))
        {
            reportMapperFailure(sharedCb, "unknown exception");
        }
    }
}

std::string PaymentService::generatePaymentNo()
{
    // Generate unique payment number
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 99999999);

    std::ostringstream oss;
    time_t now = std::time(nullptr);
    struct tm tmInfo;
#ifdef _WIN32
    localtime_s(&tmInfo, &now);
#else
    localtime_r(&now, &tmInfo);
#endif
    oss << "PAY" << std::put_time(&tmInfo, "%Y%m%d%H%M%S");
    oss << std::setfill('0') << std::setw(8) << dis(gen);

    return oss.str();
}

void PaymentService::queryOrderList(
  const std::string &status,
  const int64_t userId,
  const size_t limit,
  const size_t offset,
  PaymentCallback &&callback
)
{
    LOG_DEBUG << "[PAYMENT_SERVICE] queryOrderList called with status=" << status
              << ", userId=" << userId << ", limit=" << limit << ", offset=" << offset;

    // Build base SQL query with parameter placeholders to prevent SQL injection.
    // Raw-SQL exemption #4: LEFT JOIN is not expressible via Drogon Mapper.
    // All filter values are bound via SqlBinder ($1-$4), NOT string concatenation.
    //
    // Bound parameters (variable count):
    //   $1: status filter     (only when !status.empty() && status != "all")
    //   $2: user_id filter    (only when userId > 0)
    //   $N: LIMIT value       (always present, capped at [1, 100])
    //   $N: OFFSET value      (always present)
    static const std::string kOrderListBaseSQL =
      "SELECT po.order_no, po.user_id, po.amount, po.currency, "
      "po.status, po.channel, po.title, po.created_at, po.updated_at, "
      "pp.payment_no, pp.channel_trade_no, pp.response_payload "
      "FROM pay_order po "
      "LEFT JOIN pay_payment pp ON po.order_no = pp.order_no "
      "WHERE 1=1";
    std::string sql = kOrderListBaseSQL;

    // Build parameter list and count
    std::vector<std::string> params;
    size_t paramIndex = 1;

    // Add status filter if provided (use parameterized query)
    if (!status.empty() && status != "all")
    {
        sql += " AND po.status = $" + std::to_string(paramIndex++);
        params.push_back(status);
    }

    // Add user_id filter if provided (0 means no filter, use parameterized query)
    if (userId > 0)
    {
        sql += " AND po.user_id = $" + std::to_string(paramIndex++);
        params.push_back(std::to_string(userId));
    }

    // Add ordering and pagination
    sql += " ORDER BY po.created_at DESC";

    // Add limit (use parameterized query)
    size_t actualLimit = (limit > 0 && limit <= 100) ? limit : 50;
    sql += " LIMIT $" + std::to_string(paramIndex++);
    params.push_back(std::to_string(actualLimit));

    // Add offset (use parameterized query)
    sql += " OFFSET $" + std::to_string(paramIndex++);
    params.push_back(std::to_string(offset));

    LOG_DEBUG << "[PAYMENT_SERVICE] Executing parameterized SQL with " << params.size()
              << " parameters";

    // Parameter count varies at run time (2-4), which the variadic
    // execSqlAsync() cannot express; bind via the SqlBinder streaming
    // interface (still fully parameterized -- prevents SQL injection).
    // NOTE: raw SQL retained pending JOIN-compliance analysis (LEFT JOIN is
    // not expressible via Mapper); tracked as a db-operations follow-up.
    auto binder = *dbClient_ << sql;
    for (const auto &p : params)
    {
        binder << p;
    }
    binder >>
      [callback](const Result &result) {
          try
          {
              Json::Value response;
              response["code"] = 200;
              response["message"] = "Success";
              response["data"] = Json::Value(Json::arrayValue);

              for (size_t i = 0; i < result.size(); ++i)
              {
                  const auto &row = result[i];

                  Json::Value order;
                  order["order_no"] = row["order_no"].as<std::string>();
                  order["user_id"] = row["user_id"].as<int64_t>();
                  order["amount"] = row["amount"].as<std::string>();
                  order["currency"] = row["currency"].as<std::string>();
                  order["status"] = row["status"].as<std::string>();
                  order["channel"] = row["channel"].as<std::string>();
                  order["title"] = row["title"].as<std::string>();
                  order["created_at"] = row["created_at"].as<std::string>();
                  order["updated_at"] = row["updated_at"].as<std::string>();

                  // Add payment info if exists
                  if (!row["payment_no"].isNull())
                  {
                      order["payment_no"] = row["payment_no"].as<std::string>();
                  }
                  if (!row["channel_trade_no"].isNull())
                  {
                      order["trade_no"] = row["channel_trade_no"].as<std::string>();
                  }
                  if (!row["updated_at"].isNull())
                  {
                      order["paid_at"] = row["updated_at"].as<std::string>();
                  }
                  if (!row["response_payload"].isNull())
                  {
                      // Parse JSON from response_payload
                      try
                      {
                          Json::Value channelResponse;
                          Json::Reader reader;
                          reader.parse(row["response_payload"].as<std::string>(), channelResponse);
                          order["channel_response"] = channelResponse;
                      }
                      catch (...)
                      {
                          // If parsing fails, skip channel_response
                      }
                  }

                  response["data"].append(order);
              }

              LOG_DEBUG << "[PAYMENT_SERVICE] queryOrderList found " << response["data"].size()
                        << " orders";
              callback(response, std::error_code());
          }
          catch (const std::exception &e)
          {
              LOG_ERROR << "[PAYMENT_SERVICE] Exception in queryOrderList: " << e.what();
              Json::Value error;
              error["code"] = 1500;
              error["message"] = "Internal server error";
              callback(error, std::make_error_code(std::errc::io_error));
          }
      } >>
      [callback](const DrogonDbException &e) {
          LOG_ERROR << "[PAYMENT_SERVICE] Database error in queryOrderList: " << e.base().what();
          Json::Value error;
          error["code"] = 1500;
          error["message"] = "Database error: " + std::string(e.base().what());
          callback(error, std::make_error_code(std::errc::io_error));
      };
}
