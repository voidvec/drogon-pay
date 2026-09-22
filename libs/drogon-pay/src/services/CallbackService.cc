#include "CallbackService.h"
#include "../utils/PayUtils.h"
#include "drogon_pay/PayErrorCategory.h"
#include "../models/PayOrder.h"
#include "../models/PayPayment.h"
#include "../models/PayRefund.h"
#include "../models/PayCallback.h"
#include "../models/PayIdempotency.h"
#include "../models/PayLedger.h"
#include <drogon/drogon.h>
#include <chrono>
#include <cstdlib>
#include <sstream>

using namespace drogon;
using PayOrderModel = drogon_model::pay_test::PayOrder;
using PayPaymentModel = drogon_model::pay_test::PayPayment;
using PayRefundModel = drogon_model::pay_test::PayRefund;
using PayCallbackModel = drogon_model::pay_test::PayCallback;
using PayIdempotencyModel = drogon_model::pay_test::PayIdempotency;
using PayLedgerModel = drogon_model::pay_test::PayLedger;

namespace
{

// The payment attempts of one order that a notification may be matched against.
// A QR order carries one row per precreate attempt, and an attempt a channel
// refusal closed (FAIL) can never settle, so it must not be the row a
// notification is booked on: "newest row wins" lets a refused later attempt
// shadow the payable one, the settlement CAS then matches nothing, and the
// delivery is ACKed as SUCCESS with no money booked. An order whose attempts are
// all closed finds no row here and is reported, not silently acknowledged.
drogon::orm::Criteria openAttemptsOfOrder(const std::string &orderNo)
{
    return drogon::orm::Criteria(
             PayPaymentModel::Cols::_order_no, drogon::orm::CompareOperator::EQ, orderNo
           ) &&
           drogon::orm::Criteria(
             PayPaymentModel::Cols::_status,
             drogon::orm::CompareOperator::In,
             std::vector<std::string>{"INIT", "PROCESSING", "SUCCESS", "REFUNDED"}
           );
}

// Drop an idempotency reservation whose snapshot was never finalized -- and only
// while it is still unfinalized. Between the read that found it and this delete
// the delivery that took the reservation can finish and finalize the snapshot,
// and deleting then would erase the proof a callback was handled: a later
// delivery would run the settlement from scratch. `respond` answers the current
// delivery either way, so a delete that matched nothing is a retry signal, not a
// lost one.
//
// The delete stays key-scoped on purpose: recovering a reservation whose holder
// crashed requires clearing a row this delivery does NOT own, so no owner check
// can live here. What makes stealing a still-live reservation safe is on the
// other side -- finalizeReservation only stamps the row whose owner_token this
// delivery wrote, so a holder that was dropped mid-flight loses ownership,
// detects it at finalize time, and rolls back instead of committing.
void dropUnfinalizedReservation(
  const std::shared_ptr<drogon::orm::DbClient> &dbClient,
  const std::string &idempotencyKey,
  const std::string &label,
  const std::function<void()> &respond
)
{
    try
    {
        drogon::orm::Mapper<PayIdempotencyModel> staleRemover(dbClient);
        staleRemover.deleteBy(
          drogon::orm::Criteria(
            PayIdempotencyModel::Cols::_idempotency_key,
            drogon::orm::CompareOperator::EQ,
            idempotencyKey
          ) &&
            drogon::orm::Criteria(
              PayIdempotencyModel::Cols::_response_snapshot, drogon::orm::CompareOperator::IsNull
            ),
          [respond](const size_t) { respond(); },
          [respond, label](const drogon::orm::DrogonDbException &e) {
              LOG_ERROR << "[CallbackService] Could not drop the stale idempotency reservation "
                           "for "
                        << label << ": " << e.base().what();
              respond();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[CallbackService] Could not drop the stale idempotency reservation for "
                  << label << ": " << e.what();
        respond();
    }
    catch (...)
    {
        respond();
    }
}

// The random token a callback delivery writes into the reservation it takes
// (pay_idempotency.owner_token). It names THIS delivery as the holder so the
// finalize below can tell "my reservation is still mine" from "a retry dropped
// it and a later delivery re-reserved under the same key". Dashless UUID = 32
// chars, inside the column's VARCHAR(64).
std::string newReservationToken()
{
    std::string token;
    for (const char c : drogon::utils::getUuid())
    {
        if (c != '-')
        {
            token += c;
        }
    }
    return token;
}

// Finalize the reservation THIS delivery owns: write the response snapshot and
// report back whether the ownership guard held. Raw SQL under the documented
// `UPDATE ... RETURNING` exemption (db-operations): the generated
// PayIdempotencyModel can express the owner-scoped WHERE through Criteria, but
// no Mapper form emits RETURNING, and a deterministic match count is exactly
// what this needs -- same reason the reserve INSERT gives one.
//
// Zero matched rows means the reservation was dropped (the read path of a
// concurrent retry) and re-reserved by a later delivery while this handler
// ran. Its caller MUST NOT ACK as if it had finalized proof: inside the
// business transaction the only sound answer is rollback + FAIL (the channel
// retries and the next delivery reads whoever really completed); after the
// transaction already committed, the settlement is the truth and losing the
// snapshot only costs one extra retry cycle, so the caller warns and ACKs.
void finalizeReservation(
  const std::shared_ptr<drogon::orm::DbClient> &client,
  const std::string &idempotencyKey,
  const std::string &ownerToken,
  const std::string &snapshot,
  const std::function<void()> &onFinalized,
  const std::function<void()> &onLost,
  const std::function<void(const drogon::orm::DrogonDbException &)> &onError
)
{
    client->execSqlAsync(
      "UPDATE pay_idempotency SET response_snapshot = $1 "
      "WHERE idempotency_key = $2 AND owner_token = $3 RETURNING idempotency_key",
      [onFinalized, onLost](const drogon::orm::Result &result) {
          if (result.empty())
          {
              onLost();
              return;
          }
          onFinalized();
      },
      onError,
      snapshot,
      idempotencyKey,
      ownerToken
    );
}

// TODO(dedup): duplicated in PaymentService.cc and RefundService.cc.
// Extract to PayUtils.h/cc in a future refactoring iteration.
void insertLedgerEntry(
  const std::shared_ptr<drogon::orm::DbClient> &dbClient,
  int64_t userId,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &entryType,
  const std::string &amount,
  std::function<void()> onSuccess
)
{
    if (!dbClient)
    {
        LOG_WARN << "[CallbackService] DbClient is null in insertLedgerEntry";
        if (onSuccess)
            onSuccess();
        return;
    }

    PayLedgerModel ledger;
    ledger.setUserId(userId);
    ledger.setOrderNo(orderNo);
    ledger.setPaymentNo(paymentNo);
    ledger.setEntryType(entryType);
    ledger.setAmount(amount);
    ledger.setCreatedAt(trantor::Date::now());

    try
    {
        drogon::orm::Mapper<PayLedgerModel> mapper(dbClient);
        mapper.insert(
          ledger,
          [entryType, orderNo, paymentNo, amount, onSuccess](const PayLedgerModel &) {
              LOG_DEBUG << "[CallbackService] Ledger entry inserted: entry_type=" << entryType
                        << ", order_no=" << orderNo << ", payment_no=" << paymentNo
                        << ", amount=" << amount;
              if (onSuccess)
                  onSuccess();
          },
          [entryType, orderNo, paymentNo, onSuccess](const drogon::orm::DrogonDbException &e) {
              LOG_WARN << "[CallbackService] Failed to insert ledger entry: entry_type="
                       << entryType << ", order_no=" << orderNo << ", error: " << e.base().what();
              // Continue even if ledger insert fails - don't block the callback
              if (onSuccess)
                  onSuccess();
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "[CallbackService] Ledger mapper error: " << e.what();
        // Continue even if the ledger write fails - don't block the callback
        if (onSuccess)
            onSuccess();
    }
    catch (...)
    {
        LOG_WARN << "[CallbackService] Ledger mapper error: unknown exception";
        if (onSuccess)
            onSuccess();
    }
}

// Uniform FAIL response when a Mapper cannot even be constructed (e.g. the
// DB client is unavailable): report the error so the channel retries.
void reportMapperFailure(
  const std::shared_ptr<CallbackService::CallbackResult> &cbPtr,
  const std::string &what
)
{
    LOG_ERROR << "[CallbackService] Mapper construction failed: " << what;
    Json::Value error;
    error["code"] = "FAIL";
    error["message"] = "internal error";
    (*cbPtr)(error, pay::makePayError(1400, "db mapper unavailable"));
}

}  // namespace

CallbackService::CallbackService(
  drogon_pay::PaymentChannelPtr wechatChannel,
  std::shared_ptr<drogon::orm::DbClient> dbClient,
  drogon::nosql::RedisClientPtr redisClient
)
    : wechatClient_(std::dynamic_pointer_cast<WechatPayClient>(wechatChannel)),
      dbClient_(dbClient),
      redisClient_(redisClient)
{
    // SPI whitelist: callback decrypt/verify needs the concrete WeChat client.
    // Neither dependency is asserted on purpose: both absent states are
    // handled per method (the `!wechatClient_` / `!dbClient_` branches in
    // handlePaymentCallback, handleRefundCallback and verifySignature), each
    // has a test that constructs the service with that dependency missing to
    // pin the response, and PayPlugin::setTestChannels always builds this
    // service so the accessor never returns null. redisClient_ is optional per
    // CallbackService.h.
}

bool CallbackService::isTimestampFresh(const std::string &timestamp, std::string &errorMsg)
{
    int64_t cbTime = 0;
    try
    {
        cbTime = std::stoll(timestamp);
    }
    catch (...)
    {
        errorMsg = "invalid timestamp";
        return false;
    }
    const auto nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()
    )
                          .count();
    constexpr int64_t kMaxSkewSeconds = 300;  // 5 minutes
    if (std::llabs(nowSec - cbTime) > kMaxSkewSeconds)
    {
        LOG_WARN << "[CallbackService] Callback timestamp out of window: cb=" << cbTime
                 << " now=" << nowSec;
        errorMsg = "timestamp out of acceptable window";
        return false;
    }
    return true;
}

void CallbackService::checkNonce(
  const std::string &nonce,
  std::function<void(bool firstSight)> proceed
)
{
    if (!redisClient_)
    {
        // Fail-open: no Redis configured. The DB idempotency table still dedupes
        // duplicate processing; the nonce cache is an optimization layer.
        LOG_WARN << "[CallbackService] Redis unavailable, skipping nonce replay check";
        proceed(true);
        return;
    }
    if (nonce.empty())
    {
        // No nonce to track (malformed request); let downstream validation
        // decide. Treat as first sight.
        proceed(true);
        return;
    }
    auto sharedProceed = std::make_shared<std::function<void(bool)>>(std::move(proceed));
    std::string redisKey = "cb:nonce:" + nonce;
    // SET NX EX 360: reserves the nonce for 360s (slightly > the 300s timestamp
    // window). Returns "OK" on first sight, nil if the nonce was already seen.
    redisClient_->execCommandAsync(
      [sharedProceed](const nosql::RedisResult &result) {
          if (result.isNil())
          {
              // Nonce already present within the window -> replay.
              LOG_WARN << "[CallbackService] Replay detected: nonce already seen";
              (*sharedProceed)(false);
          }
          else
          {
              (*sharedProceed)(true);
          }
      },
      [sharedProceed](const nosql::RedisException &e) {
          // Fail-open on Redis errors: do not drop a legitimate callback because
          // the cache layer is transiently unavailable. DB idempotency guards
          // duplicate processing.
          LOG_WARN << "[CallbackService] Redis nonce check error, failing open: " << e.what();
          (*sharedProceed)(true);
      },
      "SET %s 1 NX EX 360",
      redisKey.c_str()
    );
}

void CallbackService::handlePaymentCallback(
  const std::string &body,
  const std::string &signature,
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &serialNo,
  CallbackResult &&callback
)
{
    if (!wechatClient_)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "wechat client not ready";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    auto respond = [callback](const Json::Value &result, const std::string &errorMsg) {
        if (!errorMsg.empty())
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = errorMsg;
            callback(error, std::error_code(1400, std::system_category()));
            return;
        }
        callback(result, std::error_code());
    };

    // Verify signature first
    if (!verifySignature(body, signature, timestamp, nonce, serialNo))
    {
        LOG_WARN << "[CallbackService] Signature verification failed";
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "signature verification failed";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }
    LOG_DEBUG << "[CallbackService] Signature verified successfully";

    // Replay protection (P1-1): freshness window. The nonce cache check gates
    // the expensive DB transaction at the bottom of this function (see the
    // checkNonce call wrapping proceedWithDb); body parsing is cheap and runs
    // synchronously first.
    {
        std::string tsError;
        if (!isTimestampFresh(timestamp, tsError))
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = tsError;
            respond(error, tsError);
            return;
        }
    }

    // Parse callback body
    Json::CharReaderBuilder builder;
    Json::Value notifyJson;
    std::string parseErrors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &notifyJson, &parseErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid json";
        respond(error, "invalid json");
        return;
    }

    // Validate event_type
    const std::string eventType = notifyJson.get("event_type", "").asString();
    if (eventType.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing event_type";
        respond(error, "missing event_type");
        return;
    }

    if (eventType.rfind("TRANSACTION.", 0) != 0)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid transaction event_type";
        respond(error, "invalid transaction event_type");
        return;
    }

    // Validate resource
    if (!notifyJson.isMember("resource"))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing resource";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    const auto &resource = notifyJson["resource"];
    const std::string resourceType = notifyJson.get("resource_type", "").asString();
    if (resourceType.empty() || resourceType != "encrypt-resource")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource_type";
        respond(error, "unsupported resource_type");
        return;
    }

    const std::string algorithm = resource.get("algorithm", "").asString();
    if (algorithm.empty() || algorithm != "AEAD_AES_256_GCM")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource algorithm";
        respond(error, "unsupported resource algorithm");
        return;
    }

    const std::string ciphertext = resource.get("ciphertext", "").asString();
    const std::string nonceStr = resource.get("nonce", "").asString();
    const std::string associatedData = resource.get("associated_data", "").asString();
    if (ciphertext.empty() || nonceStr.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource";
        respond(error, "invalid resource");
        return;
    }

    if (associatedData != "transaction")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid transaction associated_data";
        respond(error, "invalid transaction associated_data");
        return;
    }

    // Decrypt resource
    LOG_DEBUG << "[CallbackService] Decrypting resource...";
    std::string plaintext;
    std::string decryptError;
    if (!wechatClient_
           ->decryptResource(ciphertext, nonceStr, associatedData, plaintext, decryptError))
    {
        LOG_WARN << "[CallbackService] Decryption failed: " << decryptError;
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = decryptError;
        respond(error, decryptError);
        return;
    }
    LOG_DEBUG << "[CallbackService] Decryption successful";

    // Parse decrypted JSON
    Json::Value plainJson;
    Json::CharReaderBuilder plainBuilder;
    std::string plainErrors;
    std::unique_ptr<Json::CharReader> plainReader(plainBuilder.newCharReader());
    if (!plainReader
           ->parse(plaintext.data(), plaintext.data() + plaintext.size(), &plainJson, &plainErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource json";
        respond(error, "invalid resource json");
        return;
    }

    // Validate appid and mchid
    const std::string appId = plainJson.get("appid", "").asString();
    const std::string mchId = plainJson.get("mchid", "").asString();
    if (!appId.empty() && wechatClient_ && appId != wechatClient_->getAppId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "appid mismatch";
        respond(error, "appid mismatch");
        return;
    }
    if (!mchId.empty() && wechatClient_ && mchId != wechatClient_->getMchId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "mchid mismatch";
        respond(error, "mchid mismatch");
        return;
    }

    // Extract payment details
    const std::string orderNo = plainJson.get("out_trade_no", "").asString();
    const std::string transactionId = plainJson.get("transaction_id", "").asString();
    const std::string tradeState = plainJson.get("trade_state", "").asString();

    if (orderNo.empty() || tradeState.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing out_trade_no/trade_state";
        respond(error, "missing out_trade_no/trade_state");
        return;
    }

    const bool tradeStateValid = tradeState == "SUCCESS" || tradeState == "USERPAYING" ||
                                 tradeState == "NOTPAY" || tradeState == "CLOSED" ||
                                 tradeState == "REVOKED" || tradeState == "REFUND";
    if (!tradeStateValid)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid trade_state";
        respond(error, "invalid trade_state");
        return;
    }

    if (tradeState == "SUCCESS" && transactionId.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing transaction_id";
        respond(error, "missing transaction_id");
        return;
    }

    // Idempotency check
    std::string idempotencyKey = notifyJson.get("id", "").asString();
    if (idempotencyKey.empty())
    {
        idempotencyKey = orderNo + ":" + tradeState;
    }

    LOG_DEBUG << "[CallbackService] Preparing to process callback for order: " << orderNo;

    auto cbPtr = std::make_shared<CallbackResult>(std::move(callback));

    auto proceedWithDb = [this,
                          cbPtr,
                          idempotencyKey,
                          orderNo,
                          transactionId,
                          tradeState,
                          plaintext,
                          body,
                          signature,
                          serialNo,
                          plainJson]() {
        LOG_DEBUG << "[CallbackService] proceedWithDb lambda called for order: " << orderNo;
        try
        {
            drogon::orm::Mapper<PayIdempotencyModel> idempMapper(dbClient_);
            auto idempCriteria = drogon::orm::Criteria(
              PayIdempotencyModel::Cols::_idempotency_key,
              drogon::orm::CompareOperator::EQ,
              idempotencyKey
            );
            idempMapper.findOne(
              idempCriteria,
              [this, cbPtr, orderNo, idempotencyKey, body, signature, serialNo](
                const PayIdempotencyModel &existing
              ) {
                  // A reservation whose snapshot was never finalized is not proof the
                  // callback was handled: the delivery that took it is either still
                  // running or died before its transaction committed. Acknowledging
                  // one as a duplicate stops the channel's retries and leaves the
                  // money booked nowhere, so drop the stale reservation and answer
                  // FAIL. The next delivery then takes the full path, where the
                  // settlement CAS leaves a concurrent winner's work intact.
                  if (!existing.getResponseSnapshot())
                  {
                      LOG_WARN << "[CallbackService] Idempotency record for order " << orderNo
                               << " has no finalized snapshot; dropping the stale reservation";
                      auto respondRetryLater = [cbPtr]() {
                          Json::Value error;
                          error["code"] = "FAIL";
                          error["message"] = "callback still in progress";
                          (*cbPtr)(error, pay::makePayError(1400, "callback still in progress"));
                      };
                      dropUnfinalizedReservation(
                        dbClient_, idempotencyKey, "order " + orderNo, respondRetryLater
                      );
                      return;
                  }

                  // Already processed - record callback and return success
                  LOG_DEBUG << "[CallbackService] Idempotency key found for order: " << orderNo
                            << ", recording callback";

                  auto respondSuccess = [cbPtr]() {
                      Json::Value ok;
                      ok["code"] = "SUCCESS";
                      ok["message"] = "OK";
                      (*cbPtr)(ok, std::error_code());
                  };

                  auto respondDbError = [cbPtr](const drogon::orm::DrogonDbException &e) {
                      LOG_ERROR << "[CallbackService] DB error recording idempotent callback: "
                                << e.base().what();
                      Json::Value error;
                      error["code"] = "FAIL";
                      error["message"] = "internal error";
                      (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
                  };

                  try
                  {
                      drogon::orm::Mapper<PayPaymentModel> paymentLookup(dbClient_);
                      // An order can carry several payment attempts (a refused QR
                      // attempt keeps its row and the retry appends a new one), and
                      // findOne() answers that with "Found more than one row", which
                      // made this path reject the duplicate notification instead of
                      // recording it. Attach the audit row to the same attempt the
                      // settlement path settles, so the trail names the row the money
                      // is on rather than whichever insert happened to be last.
                      paymentLookup
                        .orderBy(PayPaymentModel::Cols::_created_at, drogon::orm::SortOrder::DESC)
                        .limit(1)
                        .findBy(
                          openAttemptsOfOrder(orderNo),
                          [this, cbPtr, body, signature, serialNo, respondSuccess, respondDbError](
                            const std::vector<PayPaymentModel> &rows
                          ) {
                              if (rows.empty())
                              {
                                  respondDbError(drogon::orm::UnexpectedRows("0 rows found"));
                                  return;
                              }
                              const std::string paymentNo = rows.front().getValueOfPaymentNo();

                              try
                              {
                                  drogon::orm::Mapper<PayCallbackModel> callbackMapper(dbClient_);
                                  PayCallbackModel callbackRow;
                                  callbackRow.setPaymentNo(paymentNo);
                                  callbackRow.setRawBody(body);
                                  callbackRow.setSignature(signature);
                                  callbackRow.setSerialNo(serialNo);
                                  callbackRow.setVerified(true);
                                  callbackRow.setProcessed(true);
                                  callbackRow.setReceivedAt(trantor::Date::now());

                                  callbackMapper.insert(
                                    callbackRow,
                                    [respondSuccess](const PayCallbackModel &) {
                                        respondSuccess();
                                    },
                                    respondDbError
                                  );
                              }
                              catch (const std::exception &e)
                              {
                                  reportMapperFailure(cbPtr, e.what());
                              }
                              catch (...)
                              {
                                  reportMapperFailure(cbPtr, "unknown exception");
                              }
                          },
                          [respondDbError](const drogon::orm::DrogonDbException &e) {
                              LOG_ERROR << "[CallbackService] Payment not found during "
                                           "idempotent callback: "
                                        << e.base().what();
                              respondDbError(e);
                          }
                        );
                  }
                  catch (const std::exception &e)
                  {
                      reportMapperFailure(cbPtr, e.what());
                  }
                  catch (...)
                  {
                      reportMapperFailure(cbPtr, "unknown exception");
                  }
              },
              [this,
               cbPtr,
               idempotencyKey,
               orderNo,
               transactionId,
               tradeState,
               plaintext,
               body,
               signature,
               serialNo,
               plainJson](const drogon::orm::DrogonDbException &e) {
                  // Only UnexpectedRows means "key not found"; any other DB failure
                  // must NOT be treated as a new callback (risk of double handling).
                  if (dynamic_cast<const drogon::orm::UnexpectedRows *>(&e) == nullptr)
                  {
                      LOG_ERROR << "[CallbackService] Idempotency lookup DB error: "
                                << e.base().what();
                      Json::Value error;
                      error["code"] = "FAIL";
                      error["message"] = "internal error";
                      (*cbPtr)(error, pay::makePayError(1400, "idempotency lookup failed"));
                      return;
                  }
                  LOG_DEBUG
                    << "[CallbackService] Idempotency key not found, processing new callback";
                  const std::string requestHash = drogon::utils::getMd5(body);
                  // Reserve with response_snapshot = NULL (P2-4.2). The snapshot is
                  // finalized (set to the response) only after the business
                  // transaction commits below, so a crash between the reservation
                  // and the commit leaves an in-flight (NULL-snapshot) row rather
                  // than one that falsely reads as "completed".
                  const auto now = trantor::Date::now();
                  const auto expiresAt = trantor::Date(
                    now.microSecondsSinceEpoch() + static_cast<int64_t>(7) * 24 * 60 * 60 * 1000000
                  );
                  // This delivery's ownership token for the reservation below.
                  const std::string ownerToken = newReservationToken();

                  // Insert idempotency record on main client (outside transaction)
                  // so it's committed and visible to subsequent calls immediately.
                  // Raw SQL exemption (db-operations): Mapper::insert cannot express
                  // ON CONFLICT DO NOTHING, which is required here for deterministic
                  // duplicate detection -- the PG backend only throws a generic
                  // Failure whose message text depends on the server locale, so
                  // matching "duplicate key"/"23505" in the error string is
                  // unreliable. RETURNING gives a deterministic row count:
                  // 1 = first insert, 0 = concurrent duplicate.
                  try
                  {
                      dbClient_->execSqlAsync(
                        "INSERT INTO pay_idempotency (idempotency_key, request_hash, "
                        "response_snapshot, owner_token, expire_at) "
                        "VALUES ($1, $2, NULL, $3, $4) "
                        "ON CONFLICT (idempotency_key) DO NOTHING "
                        "RETURNING idempotency_key",
                        [this,
                         cbPtr,
                         idempotencyKey,
                         ownerToken,
                         orderNo,
                         transactionId,
                         tradeState,
                         plaintext,
                         body,
                         signature,
                         serialNo,
                         plainJson](const drogon::orm::Result &insertResult) {
                            if (insertResult.empty())
                            {
                                // 0 rows: a concurrent delivery reserved this key
                                // first, but an unfinalized reservation is not proof
                                // it completed -- the winner may still be running or
                                // may have died before setting the snapshot. ACKing
                                // SUCCESS here would stop the channel's retries and
                                // strand the settlement, which is exactly the case the
                                // read path refuses at findOne (NULL snapshot -> drop +
                                // retry). Answer FAIL so the notification is retried;
                                // the next delivery then reads the row and a finalized
                                // snapshot is acknowledged idempotently.
                                LOG_DEBUG << "[CallbackService] Concurrent callback "
                                             "reservation for key: "
                                          << idempotencyKey << ", requesting retry";
                                Json::Value error;
                                error["code"] = "FAIL";
                                error["message"] = "callback still in progress";
                                (*cbPtr)(
                                  error, pay::makePayError(1400, "callback still in progress")
                                );
                                return;
                            }
                            LOG_DEBUG
                              << "[CallbackService] Creating database transaction for order: "
                              << orderNo;
                            dbClient_->newTransactionAsync([this,
                                                            cbPtr,
                                                            idempotencyKey,
                                                            ownerToken,
                                                            orderNo,
                                                            transactionId,
                                                            tradeState,
                                                            plaintext,
                                                            body,
                                                            signature,
                                                            serialNo,
                                                            plainJson](
                                                             const std::shared_ptr<
                                                               drogon::orm::Transaction> &transPtr
                                                           ) mutable {
                                auto respondDbError =
                                  [cbPtr](const drogon::orm::DrogonDbException &e) {
                                      LOG_ERROR << "[CallbackService] DB error in callback "
                                                   "transaction: "
                                                << e.base().what();
                                      Json::Value error;
                                      error["code"] = "FAIL";
                                      error["message"] = "internal error";
                                      (*cbPtr)(
                                        error, pay::makePayError(1400, "db transaction unavailable")
                                      );
                                  };

                                try
                                {
                                    drogon::orm::Mapper<PayPaymentModel> paymentMapper(transPtr);
                                    auto paymentCriteria = openAttemptsOfOrder(orderNo);
                                    paymentMapper
                                      .orderBy(
                                        PayPaymentModel::Cols::_created_at,
                                        drogon::orm::SortOrder::DESC
                                      )
                                      .limit(1)
                                      .forUpdate()
                                      .findBy(
                                        paymentCriteria,
                                        [this,
                                         cbPtr,
                                         orderNo,
                                         transactionId,
                                         tradeState,
                                         plaintext,
                                         body,
                                         signature,
                                         serialNo,
                                         plainJson,
                                         transPtr,
                                         respondDbError,
                                         idempotencyKey,
                                         ownerToken](const std::vector<PayPaymentModel> &rows) {
                                            LOG_DEBUG << "[CallbackService] Payment query returned "
                                                      << rows.size()
                                                      << " rows for order: " << orderNo;
                                            if (rows.empty())
                                            {
                                                LOG_ERROR << "[CallbackService] Payment not "
                                                             "found for order: "
                                                          << orderNo;
                                                transPtr->rollback();
                                                Json::Value error;
                                                error["code"] = "FAIL";
                                                error["message"] = "payment not found";
                                                (*cbPtr)(
                                                  error,
                                                  std::error_code(1404, std::system_category())
                                                );
                                                return;
                                            }

                                            auto payment = rows.front();
                                            const std::string paymentNo =
                                              payment.getValueOfPaymentNo();
                                            LOG_DEBUG
                                              << "[CallbackService] Found payment: " << paymentNo
                                              << " for order: " << orderNo;

                                            // Skip if payment already in final state
                                            const std::string currentStatus =
                                              payment.getValueOfStatus();
                                            if (
                                              currentStatus == "SUCCESS" ||
                                              currentStatus == "REFUNDED"
                                            )
                                            {
                                                LOG_DEBUG
                                                  << "[CallbackService] Payment " << paymentNo
                                                  << " already in final state: " << currentStatus
                                                  << ", skipping duplicate callback";
                                                transPtr->rollback();
                                                Json::Value ok;
                                                ok["code"] = "SUCCESS";
                                                ok["message"] = "OK";
                                                (*cbPtr)(ok, std::error_code());
                                                return;
                                            }

                                            const std::string orderAmount =
                                              payment.getValueOfAmount();

                                            payment.getPayOrder(
                                              transPtr,
                                              [this,
                                               cbPtr,
                                               orderNo,
                                               paymentNo,
                                               orderAmount,
                                               transactionId,
                                               tradeState,
                                               plaintext,
                                               body,
                                               signature,
                                               serialNo,
                                               plainJson,
                                               transPtr,
                                               respondDbError,
                                               payment,
                                               idempotencyKey,
                                               ownerToken](PayOrderModel order) mutable {
                                                  LOG_DEBUG << "[CallbackService] Order found "
                                                               "for order: "
                                                            << orderNo;
                                                  const std::string orderCurrency =
                                                    order.getValueOfCurrency();
                                                  const auto &amountJson = plainJson["amount"];
                                                  const std::string notifyCurrency =
                                                    amountJson.get("currency", "").asString();
                                                  const int64_t notifyTotalFen =
                                                    amountJson.get("total", 0).asInt64();
                                                  int64_t orderTotalFen = 0;
                                                  if (
                                                    !pay::utils::parseAmountToFen(
                                                      orderAmount, orderTotalFen
                                                    ) ||
                                                    notifyTotalFen <= 0
                                                  )
                                                  {
                                                      transPtr->rollback();
                                                      Json::Value error;
                                                      error["code"] = "FAIL";
                                                      error["message"] =
                                                        "invalid amount in callback";
                                                      (*cbPtr)(
                                                        error,
                                                        pay::makePayError(
                                                          400, "invalid amount in callback"
                                                        )
                                                      );
                                                      return;
                                                  }
                                                  if (
                                                    !notifyCurrency.empty() &&
                                                    notifyCurrency != orderCurrency
                                                  )
                                                  {
                                                      transPtr->rollback();
                                                      Json::Value error;
                                                      error["code"] = "FAIL";
                                                      error["message"] = "currency mismatch";
                                                      (*cbPtr)(
                                                        error,
                                                        pay::makePayError(
                                                          400, "invalid amount in callback"
                                                        )
                                                      );
                                                      return;
                                                  }
                                                  if (notifyTotalFen != orderTotalFen)
                                                  {
                                                      transPtr->rollback();
                                                      Json::Value error;
                                                      error["code"] = "FAIL";
                                                      error["message"] = "amount mismatch";
                                                      (*cbPtr)(
                                                        error,
                                                        pay::makePayError(
                                                          400, "invalid amount in callback"
                                                        )
                                                      );
                                                      return;
                                                  }

                                                  // `payer_total` is what the user handed
                                                  // over. It sits below `total` whenever a
                                                  // coupon covers the difference, and a
                                                  // full coupon makes it exactly 0, which
                                                  // is legitimate. Only the impossible
                                                  // direction -- paid more than the order
                                                  // -- is refused; the gap itself is logged
                                                  // because the ledger books `total`.
                                                  const int64_t payerTotalFen =
                                                    amountJson.get("payer_total", 0).asInt64();
                                                  if (
                                                    amountJson.isMember("payer_total") &&
                                                    payerTotalFen > notifyTotalFen
                                                  )
                                                  {
                                                      transPtr->rollback();
                                                      Json::Value error;
                                                      error["code"] = "FAIL";
                                                      error["message"] =
                                                        "invalid payer_total in callback";
                                                      (*cbPtr)(
                                                        error,
                                                        pay::makePayError(
                                                          400, "invalid amount in callback"
                                                        )
                                                      );
                                                      return;
                                                  }
                                                  if (
                                                    payerTotalFen > 0 &&
                                                    payerTotalFen != notifyTotalFen
                                                  )
                                                  {
                                                      LOG_WARN << "[CallbackService] Paid "
                                                                  "amount differs from the "
                                                                  "order amount for order: "
                                                               << orderNo
                                                               << " payer_total=" << payerTotalFen
                                                               << " total=" << notifyTotalFen;
                                                  }

                                                  // Two notifications for one order number
                                                  // must name one channel transaction; a
                                                  // different id means another transaction
                                                  // is being booked under this order.
                                                  const std::string bookedTxn =
                                                    payment.getValueOfChannelTradeNo();
                                                  if (
                                                    !bookedTxn.empty() && !transactionId.empty() &&
                                                    bookedTxn != transactionId
                                                  )
                                                  {
                                                      LOG_ERROR << "[CallbackService] "
                                                                   "transaction_id differs "
                                                                   "from the booked one for "
                                                                   "order: "
                                                                << orderNo;
                                                      transPtr->rollback();
                                                      Json::Value error;
                                                      error["code"] = "FAIL";
                                                      error["message"] = "transaction_id mismatch";
                                                      (*cbPtr)(
                                                        error,
                                                        pay::makePayError(
                                                          400, "transaction_id mismatch"
                                                        )
                                                      );
                                                      return;
                                                  }

                                                  std::string orderStatus;
                                                  std::string paymentStatus;
                                                  pay::utils::mapTradeState(
                                                    tradeState, orderStatus, paymentStatus
                                                  );
                                                  LOG_DEBUG
                                                    << "[CallbackService] Mapped trade state "
                                                       "'"
                                                    << tradeState
                                                    << "' to order status: " << orderStatus
                                                    << ", payment status: " << paymentStatus
                                                    << " for order: " << orderNo;

                                                  // A `trade_state=REFUND` answer says the
                                                  // trade entered refunding, which one
                                                  // settled partial refund is enough to
                                                  // produce, so the mapped `REFUNDED` is
                                                  // only a claim and the settled-refund
                                                  // ledger decides it. Queued ahead of the
                                                  // callback-record insert: statements on
                                                  // one transaction run in submission
                                                  // order, so by the time the record is in
                                                  // the sum has landed and the order write
                                                  // below sees the final status.
                                                  // Aggregate SUM (raw-SQL exemption #3):
                                                  // the Mapper cannot express SUM.
                                                  auto resolvedOrderStatus =
                                                    std::make_shared<std::string>(orderStatus);
                                                  if (orderStatus == "REFUNDED")
                                                  {
                                                      int64_t notifyOrderTotalFen = 0;
                                                      if (!pay::utils::parseAmountToFen(
                                                            order.getValueOfAmount(),
                                                            notifyOrderTotalFen
                                                          ))
                                                      {
                                                          // An unparsable total proves
                                                          // nothing, which keeps the claim
                                                          // downgraded.
                                                          notifyOrderTotalFen = 0;
                                                      }
                                                      transPtr->execSqlAsync(
                                                        "SELECT COALESCE(SUM(CAST(amount AS "
                                                        "NUMERIC)), 0) AS sum_amount "
                                                        "FROM pay_refund WHERE order_no = $1 "
                                                        "AND status = $2",
                                                        [resolvedOrderStatus,
                                                         orderStatus,
                                                         orderNo,
                                                         notifyOrderTotalFen](
                                                          const drogon::orm::Result &sumResult
                                                        ) {
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
                                                                    settledRefundFen = 0;
                                                                }
                                                            }
                                                            *resolvedOrderStatus = pay::utils::
                                                              resolveRefundedOrderStatus(
                                                                orderStatus,
                                                                settledRefundFen,
                                                                notifyOrderTotalFen
                                                              );
                                                            LOG_DEBUG
                                                              << "[CallbackService] Order "
                                                              << orderNo << " answered REFUND with "
                                                              << settledRefundFen
                                                              << " fen settled against "
                                                              << notifyOrderTotalFen
                                                              << " fen total: booking it as "
                                                              << *resolvedOrderStatus;
                                                        },
                                                        [orderNo](
                                                          const drogon::orm::DrogonDbException &e
                                                        ) {
                                                            // The failed read aborts this
                                                            // transaction on Postgres, so
                                                            // the statements queued after
                                                            // it fall into the existing
                                                            // error paths and the whole
                                                            // notification is retried.
                                                            LOG_ERROR
                                                              << "[CallbackService] Settled-refund "
                                                                 "sum for "
                                                              << orderNo
                                                              << " failed: " << e.base().what();
                                                        },
                                                        orderNo,
                                                        std::string("REFUND_SUCCESS")
                                                      );
                                                  }

                                                  PayCallbackModel callbackRow;
                                                  callbackRow.setPaymentNo(paymentNo);
                                                  callbackRow.setRawBody(body);
                                                  callbackRow.setSignature(signature);
                                                  callbackRow.setSerialNo(serialNo);
                                                  callbackRow.setVerified(true);
                                                  callbackRow.setProcessed(true);
                                                  callbackRow.setReceivedAt(trantor::Date::now());

                                                  try
                                                  {
                                                      drogon::orm::Mapper<PayCallbackModel>
                                                        callbackMapper(transPtr);
                                                      LOG_DEBUG << "[CallbackService] About "
                                                                   "to insert callback "
                                                                   "record for order: "
                                                                << orderNo;
                                                      callbackMapper.insert(
                                                        callbackRow,
                                                        [this,
                                                         cbPtr,
                                                         orderNo,
                                                         paymentNo,
                                                         resolvedOrderStatus,
                                                         paymentStatus,
                                                         transactionId,
                                                         plaintext,
                                                         transPtr,
                                                         respondDbError,
                                                         payment,
                                                         order,
                                                         idempotencyKey,
                                                         ownerToken,
                                                         body,
                                                         signature,
                                                         serialNo](
                                                          const PayCallbackModel &
                                                        ) mutable {
                                                            // The refund-coverage sum
                                                            // queued above has landed by
                                                            // now: from here on,
                                                            // `orderStatus` is what the
                                                            // ledger proved, not the raw
                                                            // claim.
                                                            const std::string orderStatus =
                                                              *resolvedOrderStatus;
                                                            LOG_DEBUG << "[CallbackService] "
                                                                         "Callback record "
                                                                         "inserted for order: "
                                                                      << orderNo;
                                                            auto transDb = std::static_pointer_cast<
                                                              drogon::orm::DbClient>(transPtr);

                                                            // CAS-style status transition:
                                                            // only update if the payment is
                                                            // still in a non-final state.
                                                            // This closes the TOCTOU window
                                                            // between the in-application
                                                            // status check above and the
                                                            // write, so a concurrent
                                                            // callback (or reconcile) that
                                                            // already advanced this payment
                                                            // to SUCCESS/REFUNDED causes
                                                            // an empty RETURNING set and
                                                            // we skip
                                                            // the downstream order/ledger
                                                            // writes instead of overwriting
                                                            // and double-ledgering. Uses
                                                            // UPDATE...RETURNING (raw-SQL
                                                            // exemption #2).
                                                            transPtr->execSqlAsync(
                                                              "UPDATE pay_payment "
                                                              "SET status = $1, "
                                                              "channel_trade_no = $2, "
                                                              "response_payload = $3 "
                                                              "WHERE payment_no = $4 "
                                                              "AND status IN ('INIT', "
                                                              "'PROCESSING') RETURNING "
                                                              "1",
                                                              [this,
                                                               cbPtr,
                                                               orderStatus,
                                                               paymentNo,
                                                               transDb,
                                                               orderNo,
                                                               transactionId,
                                                               plaintext,
                                                               payment,
                                                               order,
                                                               transPtr,
                                                               respondDbError,
                                                               idempotencyKey,
                                                               ownerToken,
                                                               body,
                                                               signature,
                                                               serialNo](
                                                                const drogon::orm::Result &r
                                                              ) mutable {
                                                                  if (r.size() == 0)
                                                                  {
                                                                      LOG_DEBUG << "[CallbackServ"
                                                                                   "ice] Payment "
                                                                                   "already "
                                                                                   "advanced by "
                                                                                   "a concurrent "
                                                                                   "transaction "
                                                                                   "for order: "
                                                                                << orderNo
                                                                                << ", skipping";
                                                                      transPtr->rollback();
                                                                      // Record this
                                                                      // verified delivery
                                                                      // in the audit
                                                                      // trail (P4): the
                                                                      // business
                                                                      // transaction was
                                                                      // just rolled back,
                                                                      // so insert the
                                                                      // pay_callback row
                                                                      // on dbClient_
                                                                      // (independent of
                                                                      // transPtr) to keep
                                                                      // the audit trail
                                                                      // complete even
                                                                      // when the state
                                                                      // transition is
                                                                      // skipped.
                                                                      PayCallbackModel dupRow;
                                                                      dupRow.setPaymentNo(
                                                                        paymentNo
                                                                      );
                                                                      dupRow.setRawBody(body);
                                                                      dupRow.setSignature(
                                                                        signature
                                                                      );
                                                                      dupRow.setSerialNo(serialNo);
                                                                      dupRow.setVerified(true);
                                                                      dupRow.setProcessed(true);
                                                                      dupRow.setReceivedAt(
                                                                        trantor::Date::now()
                                                                      );
                                                                      try
                                                                      {
                                                                          drogon::orm::Mapper<
                                                                            PayCallbackModel>
                                                                            dupMapper(dbClient_);
                                                                          dupMapper.insert(
                                                                            dupRow,
                                                                            [cbPtr](
                                                                              const PayCallbackModel
                                                                                &
                                                                            ) {
                                                                                Json::Value ok;
                                                                                ok
                                                                                  ["cod"
                                                                                   "e"] =
                                                                                    "SUCC"
                                                                                    "ESS";
                                                                                ok
                                                                                  ["messa"
                                                                                   "ge"] = "OK";
                                                                                (*cbPtr)(
                                                                                  ok,
                                                                                  std::error_code()
                                                                                );
                                                                            },
                                                                            [cbPtr](
                                                                              const drogon::orm::
                                                                                DrogonDbException &
                                                                            ) {
                                                                                // Audit
                                                                                // insert
                                                                                // failed;
                                                                                // still
                                                                                // tell
                                                                                // the
                                                                                // channel
                                                                                // to stop
                                                                                // retrying
                                                                                // (state
                                                                                // already
                                                                                // advanced).
                                                                                Json::Value ok;
                                                                                ok
                                                                                  ["cod"
                                                                                   "e"] =
                                                                                    "SUCC"
                                                                                    "ESS";
                                                                                ok
                                                                                  ["messa"
                                                                                   "ge"] = "OK";
                                                                                (*cbPtr)(
                                                                                  ok,
                                                                                  std::error_code()
                                                                                );
                                                                            }
                                                                          );
                                                                      }
                                                                      catch (
                                                                        const std::exception &e
                                                                      )
                                                                      {
                                                                          // Audit mapper
                                                                          // failed; state
                                                                          // already
                                                                          // advanced, so
                                                                          // still ACK the
                                                                          // channel.
                                                                          LOG_ERROR << "[Callback"
                                                                                       "Service] "
                                                                                       "Audit "
                                                                                       "mapper "
                                                                                       "error: "
                                                                                    << e.what();
                                                                          Json::Value ok;
                                                                          ok["code"] = "SUCCESS";
                                                                          ok["message"] = "OK";
                                                                          (*cbPtr)(
                                                                            ok, std::error_code()
                                                                          );
                                                                      }
                                                                      catch (...)
                                                                      {
                                                                          LOG_ERROR << "[Callback"
                                                                                       "Service] "
                                                                                       "Audit "
                                                                                       "mapper "
                                                                                       "error: "
                                                                                       "unknown "
                                                                                       "exceptio"
                                                                                       "n";
                                                                          Json::Value ok;
                                                                          ok["code"] = "SUCCESS";
                                                                          ok["message"] = "OK";
                                                                          (*cbPtr)(
                                                                            ok, std::error_code()
                                                                          );
                                                                      }
                                                                      return;
                                                                  }
                                                                  LOG_DEBUG << "[CallbackService]"
                                                                               " Payment updated "
                                                                               "via CAS for "
                                                                               "order: "
                                                                            << orderNo;
                                                                  try
                                                                  {
                                                                      drogon::orm::Mapper<
                                                                        PayOrderModel>
                                                                        orderUpdater(transPtr);
                                                                      // Update order
                                                                      // fields
                                                                      order.setStatus(orderStatus);
                                                                      LOG_DEBUG << "[CallbackServ"
                                                                                   "ice] About "
                                                                                   "to update "
                                                                                   "order record "
                                                                                   "for order: "
                                                                                << orderNo
                                                                                << ", status: "
                                                                                << orderStatus;
                                                                      orderUpdater
                                                                        .update(
                                                                          order,
                                                                          [cbPtr,
                                                                           orderStatus,
                                                                           paymentNo,
                                                                           transDb,
                                                                           orderNo,
                                                                           order,
                                                                           transPtr,
                                                                           idempotencyKey,
                                                                           ownerToken,
                                                                           plaintext,
                                                                           this](const size_t) {
                                                                              LOG_DEBUG << "[Call"
                                                                                           "backS"
                                                                                           "ervic"
                                                                                           "e] "
                                                                                           "Order"
                                                                                           " upda"
                                                                                           "ted "
                                                                                           "succe"
                                                                                           "ssful"
                                                                                           "ly "
                                                                                           "for "
                                                                                           "order"
                                                                                           ": "
                                                                                        << orderNo
                                                                                        << ", "
                                                                                           "prepa"
                                                                                           "ring "
                                                                                           "final"
                                                                                           " resp"
                                                                                           "onse";
                                                                              if (
                                                                                orderStatus ==
                                                                                  "PAID" ||
                                                                                orderStatus ==
                                                                                  "REFUNDED"
                                                                              )
                                                                              {
                                                                                  insertLedgerEntry(
                                                                                    transDb,
                                                                                    order
                                                                                      .getValueOfUserId(),
                                                                                    orderNo,
                                                                                    paymentNo,
                                                                                    "PAYM"
                                                                                    "ENT",
                                                                                    order
                                                                                      .getValueOfAmount(),
                                                                                    [cbPtr,
                                                                                     orderNo,
                                                                                     transPtr,
                                                                                     idempotencyKey,
                                                                                     ownerToken,
                                                                                     plaintext]() {
                                                                                        LOG_DEBUG
                                                                                          << "[Call"
                                                                                             "backS"
                                                                                             "ervic"
                                                                                             "e] "
                                                                                             "Manua"
                                                                                             "lly "
                                                                                             "commi"
                                                                                             "tting"
                                                                                             " tran"
                                                                                             "sacti"
                                                                                             "on "
                                                                                             "for "
                                                                                             "order"
                                                                                             ": "
                                                                                          << orderNo;
                                                                                        try
                                                                                        {
                                                                                            finalizeReservation(
                                                                                              transPtr,
                                                                                              idempotencyKey,
                                                                                              ownerToken,
                                                                                              plaintext,
                                                                                              [cbPtr,
                                                                                               orderNo,
                                                                                               transPtr]() {
                                                                                                  // Explicit COMMIT (raw-SQL exemption candidate):
                                                                                                  // the channel is ACKed only after COMMIT succeeds;
                                                                                                  // an implicit destructor-time commit would ACK
                                                                                                  // before durability and lose the channel retry.
                                                                                                  transPtr
                                                                                                    ->execSqlAsync(
                                                                                                      "COMMIT",
                                                                                                      [cbPtr,
                                                                                                       orderNo](
                                                                                                        const drogon::
                                                                                                          orm::
                                                                                                            Result
                                                                                                              &
                                                                                                      ) {
                                                                                                          LOG_DEBUG
                                                                                                            << "[CallbackServic"
                                                                                                               "e] Transaction "
                                                                                                               "committed, "
                                                                                                               "calling final "
                                                                                                               "success "
                                                                                                               "callback for "
                                                                                                               "order: "
                                                                                                            << orderNo;
                                                                                                          Json::Value
                                                                                                            ok;
                                                                                                          ok
                                                                                                            ["code"] =
                                                                                                              "SUCCESS";
                                                                                                          ok
                                                                                                            ["message"] =
                                                                                                              "OK";
                                                                                                          (*cbPtr)(
                                                                                                            ok,
                                                                                                            std::
                                                                                                              error_code()
                                                                                                          );
                                                                                                      },
                                                                                                      [cbPtr,
                                                                                                       orderNo,
                                                                                                       transPtr](
                                                                                                        const drogon::
                                                                                                          orm::DrogonDbException
                                                                                                            &e
                                                                                                      ) {
                                                                                                          LOG_ERROR
                                                                                                            << "[CallbackServic"
                                                                                                               "e] Failed to "
                                                                                                               "commit: "
                                                                                                            << e.base()
                                                                                                                 .what();
                                                                                                          transPtr
                                                                                                            ->rollback();
                                                                                                          Json::Value
                                                                                                            err;
                                                                                                          err
                                                                                                            ["code"] =
                                                                                                              "FAIL";
                                                                                                          err
                                                                                                            ["message"] =
                                                                                                              "internal error";
                                                                                                          (*cbPtr)(
                                                                                                            err,
                                                                                                            pay::makePayError(
                                                                                                              1400,
                                                                                                              "db commit error"
                                                                                                            )
                                                                                                          );
                                                                                                      }
                                                                                                    );
                                                                                              },
                                                                                              [cbPtr,
                                                                                               orderNo,
                                                                                               transPtr,
                                                                                               idempotencyKey]() {
                                                                                                  // The reservation no longer belongs to
                                                                                                  // this delivery: a concurrent retry's
                                                                                                  // read path dropped it and a later
                                                                                                  // delivery re-reserved under the
                                                                                                  // same key. Committing would ACK
                                                                                                  // proof this transaction does not
                                                                                                  // hold, so roll the settlement
                                                                                                  // back and let the channel retry;
                                                                                                  // the next delivery runs under
                                                                                                  // whoever holds the key now.
                                                                                                  LOG_WARN
                                                                                                    << "[CallbackService] Idempotency reservation for order "
                                                                                                    << orderNo
                                                                                                    << " was taken over; rolling back and requesting retry";
                                                                                                  transPtr
                                                                                                    ->rollback();
                                                                                                  Json::Value
                                                                                                    err;
                                                                                                  err
                                                                                                    ["code"] =
                                                                                                      "FAIL";
                                                                                                  err
                                                                                                    ["message"] =
                                                                                                      "internal error";
                                                                                                  (*cbPtr)(
                                                                                                    err,
                                                                                                    pay::makePayError(
                                                                                                      1400,
                                                                                                      "idempotency reservation lost"
                                                                                                    )
                                                                                                  );
                                                                                              },
                                                                                              [cbPtr,
                                                                                               orderNo,
                                                                                               transPtr](
                                                                                                const drogon::
                                                                                                  orm::DrogonDbException
                                                                                                    &e
                                                                                              ) {
                                                                                                  LOG_ERROR
                                                                                                    << "[CallbackService] "
                                                                                                       "Failed to update "
                                                                                                       "idempotency: "
                                                                                                    << e.base()
                                                                                                         .what();
                                                                                                  transPtr
                                                                                                    ->rollback();
                                                                                                  Json::Value
                                                                                                    err;
                                                                                                  err
                                                                                                    ["code"] =
                                                                                                      "FAIL";
                                                                                                  err
                                                                                                    ["message"] =
                                                                                                      "internal error";
                                                                                                  (*cbPtr)(
                                                                                                    err,
                                                                                                    pay::makePayError(
                                                                                                      1400,
                                                                                                      "db idempotency error"
                                                                                                    )
                                                                                                  );
                                                                                              }
                                                                                            );
                                                                                        }
                                                                                        catch (
                                                                                          const std::
                                                                                            exception
                                                                                              &e
                                                                                        )
                                                                                        {
                                                                                            transPtr
                                                                                              ->rollback();
                                                                                            reportMapperFailure(
                                                                                              cbPtr,
                                                                                              e.what()
                                                                                            );
                                                                                        }
                                                                                        catch (...)
                                                                                        {
                                                                                            transPtr
                                                                                              ->rollback();
                                                                                            reportMapperFailure(
                                                                                              cbPtr,
                                                                                              "unkn"
                                                                                              "own "
                                                                                              "exce"
                                                                                              "ptio"
                                                                                              "n"
                                                                                            );
                                                                                        }
                                                                                    }
                                                                                  );
                                                                              }
                                                                              else
                                                                              {
                                                                                  LOG_DEBUG
                                                                                    << "["
                                                                                       "C"
                                                                                       "a"
                                                                                       "l"
                                                                                       "l"
                                                                                       "b"
                                                                                       "a"
                                                                                       "c"
                                                                                       "k"
                                                                                       "S"
                                                                                       "e"
                                                                                       "r"
                                                                                       "v"
                                                                                       "i"
                                                                                       "c"
                                                                                       "e"
                                                                                       "]"
                                                                                       " "
                                                                                       "M"
                                                                                       "a"
                                                                                       "n"
                                                                                       "u"
                                                                                       "a"
                                                                                       "l"
                                                                                       "l"
                                                                                       "y"
                                                                                       " "
                                                                                       "c"
                                                                                       "o"
                                                                                       "m"
                                                                                       "m"
                                                                                       "i"
                                                                                       "t"
                                                                                       "t"
                                                                                       "i"
                                                                                       "n"
                                                                                       "g"
                                                                                       " "
                                                                                       "t"
                                                                                       "r"
                                                                                       "a"
                                                                                       "n"
                                                                                       "s"
                                                                                       "a"
                                                                                       "c"
                                                                                       "t"
                                                                                       "i"
                                                                                       "o"
                                                                                       "n"
                                                                                       " "
                                                                                       "f"
                                                                                       "o"
                                                                                       "r"
                                                                                       " "
                                                                                       "o"
                                                                                       "r"
                                                                                       "d"
                                                                                       "e"
                                                                                       "r"
                                                                                       ":"
                                                                                       " "
                                                                                    << orderNo;
                                                                                  // Explicit
                                                                                  // COMMIT
                                                                                  // (raw-SQL
                                                                                  // exemption
                                                                                  // candidate):
                                                                                  // the
                                                                                  // channel
                                                                                  // is
                                                                                  // ACKed
                                                                                  // only
                                                                                  // after
                                                                                  // COMMIT
                                                                                  // succeeds;
                                                                                  // an
                                                                                  // implicit
                                                                                  // destructor-time
                                                                                  // commit
                                                                                  // would
                                                                                  // ACK
                                                                                  // before
                                                                                  // durability
                                                                                  // and
                                                                                  // lose
                                                                                  // the
                                                                                  // channel
                                                                                  // retry.
                                                                                  transPtr->execSqlAsync(
                                                                                    "CO"
                                                                                    "MM"
                                                                                    "I"
                                                                                    "T",
                                                                                    [cbPtr,
                                                                                     orderNo,
                                                                                     idempotencyKey,
                                                                                     ownerToken,
                                                                                     plaintext,
                                                                                     this](
                                                                                      const drogon::
                                                                                        orm::Result
                                                                                          &
                                                                                    ) {
                                                                                        LOG_DEBUG
                                                                                          << "[Call"
                                                                                             "backS"
                                                                                             "ervic"
                                                                                             "e] "
                                                                                             "Trans"
                                                                                             "actio"
                                                                                             "n "
                                                                                             "commi"
                                                                                             "tted,"
                                                                                             " "
                                                                                             "calli"
                                                                                             "ng "
                                                                                             "final"
                                                                                             " succ"
                                                                                             "ess "
                                                                                             "callb"
                                                                                             "ack "
                                                                                             "for "
                                                                                             "order"
                                                                                             ": "
                                                                                          << orderNo;
                                                                                        // Finalize
                                                                                        // the
                                                                                        // idempotency
                                                                                        // reservation
                                                                                        // (P2-4.2):
                                                                                        // mark
                                                                                        // the
                                                                                        // row
                                                                                        // complete
                                                                                        // now
                                                                                        // that
                                                                                        // the
                                                                                        // business
                                                                                        // tx
                                                                                        // committed.
                                                                                        try
                                                                                        {
                                                                                            finalizeReservation(
                                                                                              dbClient_,
                                                                                              idempotencyKey,
                                                                                              ownerToken,
                                                                                              plaintext,
                                                                                              [cbPtr]() {
                                                                                                  Json::Value
                                                                                                    ok;
                                                                                                  ok
                                                                                                    ["code"] =
                                                                                                      "SUCCESS";
                                                                                                  ok
                                                                                                    ["message"] =
                                                                                                      "OK";
                                                                                                  (*cbPtr)(
                                                                                                    ok,
                                                                                                    std::
                                                                                                      error_code()
                                                                                                  );
                                                                                              },
                                                                                              [cbPtr,
                                                                                               orderNo,
                                                                                               idempotencyKey]() {
                                                                                                  // The business transaction has
                                                                                                  // already COMMITted: the
                                                                                                  // settlement is durable and
                                                                                                  // this delivery really
                                                                                                  // completed, so the ACK is
                                                                                                  // truthful even though the
                                                                                                  // snapshot could not be
                                                                                                  // stamped onto the (now
                                                                                                  // replaced) reservation.
                                                                                                  // A duplicate delivery then
                                                                                                  // re-runs the read path and
                                                                                                  // the CAS branch reports it.
                                                                                                  LOG_WARN
                                                                                                    << "[CallbackService] Idempotency reservation for order "
                                                                                                    << orderNo
                                                                                                    << " was taken over after commit; the settlement is durable, the snapshot is not";
                                                                                                  Json::Value
                                                                                                    ok;
                                                                                                  ok
                                                                                                    ["code"] =
                                                                                                      "SUCCESS";
                                                                                                  ok
                                                                                                    ["message"] =
                                                                                                      "OK";
                                                                                                  (*cbPtr)(
                                                                                                    ok,
                                                                                                    std::
                                                                                                      error_code()
                                                                                                  );
                                                                                              },
                                                                                              [cbPtr](
                                                                                                const drogon::
                                                                                                  orm::DrogonDbException
                                                                                                    &e
                                                                                              ) {
                                                                                                  LOG_ERROR
                                                                                                    << "[CallbackService] "
                                                                                                       "Failed to finalize "
                                                                                                       "idempotency row: "
                                                                                                    << e.base()
                                                                                                         .what();
                                                                                                  Json::Value
                                                                                                    ok;
                                                                                                  ok
                                                                                                    ["code"] =
                                                                                                      "SUCCESS";
                                                                                                  ok
                                                                                                    ["message"] =
                                                                                                      "OK";
                                                                                                  (*cbPtr)(
                                                                                                    ok,
                                                                                                    std::
                                                                                                      error_code()
                                                                                                  );
                                                                                              }
                                                                                            );
                                                                                        }
                                                                                        catch (
                                                                                          const std::
                                                                                            exception
                                                                                              &e
                                                                                        )
                                                                                        {
                                                                                            LOG_ERROR
                                                                                              << "["
                                                                                                 "C"
                                                                                                 "a"
                                                                                                 "l"
                                                                                                 "l"
                                                                                                 "b"
                                                                                                 "a"
                                                                                                 "c"
                                                                                                 "k"
                                                                                                 "S"
                                                                                                 "e"
                                                                                                 "r"
                                                                                                 "v"
                                                                                                 "i"
                                                                                                 "c"
                                                                                                 "e"
                                                                                                 "]"
                                                                                                 " "
                                                                                                 "F"
                                                                                                 "a"
                                                                                                 "i"
                                                                                                 "l"
                                                                                                 "e"
                                                                                                 "d"
                                                                                                 " "
                                                                                                 "t"
                                                                                                 "o"
                                                                                                 " "
                                                                                                 "f"
                                                                                                 "i"
                                                                                                 "n"
                                                                                                 "a"
                                                                                                 "l"
                                                                                                 "i"
                                                                                                 "z"
                                                                                                 "e"
                                                                                                 " "
                                                                                                 "i"
                                                                                                 "d"
                                                                                                 "e"
                                                                                                 "m"
                                                                                                 "p"
                                                                                                 "o"
                                                                                                 "t"
                                                                                                 "e"
                                                                                                 "n"
                                                                                                 "c"
                                                                                                 "y"
                                                                                                 " "
                                                                                                 "r"
                                                                                                 "o"
                                                                                                 "w"
                                                                                                 ":"
                                                                                                 " "
                                                                                              << e.what();
                                                                                            Json::
                                                                                              Value
                                                                                                ok;
                                                                                            ok
                                                                                              ["cod"
                                                                                               "e"] =
                                                                                                "SU"
                                                                                                "CC"
                                                                                                "ES"
                                                                                                "S";
                                                                                            ok
                                                                                              ["mes"
                                                                                               "sag"
                                                                                               "e"] =
                                                                                                "O"
                                                                                                "K";
                                                                                            (*cbPtr)(
                                                                                              ok,
                                                                                              std::
                                                                                                error_code()
                                                                                            );
                                                                                        }
                                                                                        catch (...)
                                                                                        {
                                                                                            LOG_ERROR
                                                                                              << "["
                                                                                                 "C"
                                                                                                 "a"
                                                                                                 "l"
                                                                                                 "l"
                                                                                                 "b"
                                                                                                 "a"
                                                                                                 "c"
                                                                                                 "k"
                                                                                                 "S"
                                                                                                 "e"
                                                                                                 "r"
                                                                                                 "v"
                                                                                                 "i"
                                                                                                 "c"
                                                                                                 "e"
                                                                                                 "]"
                                                                                                 " "
                                                                                                 "F"
                                                                                                 "a"
                                                                                                 "i"
                                                                                                 "l"
                                                                                                 "e"
                                                                                                 "d"
                                                                                                 " "
                                                                                                 "t"
                                                                                                 "o"
                                                                                                 " "
                                                                                                 "f"
                                                                                                 "i"
                                                                                                 "n"
                                                                                                 "a"
                                                                                                 "l"
                                                                                                 "i"
                                                                                                 "z"
                                                                                                 "e"
                                                                                                 " "
                                                                                                 "i"
                                                                                                 "d"
                                                                                                 "e"
                                                                                                 "m"
                                                                                                 "p"
                                                                                                 "o"
                                                                                                 "t"
                                                                                                 "e"
                                                                                                 "n"
                                                                                                 "c"
                                                                                                 "y"
                                                                                                 " "
                                                                                                 "r"
                                                                                                 "o"
                                                                                                 "w"
                                                                                                 ":"
                                                                                                 " "
                                                                                                 "u"
                                                                                                 "n"
                                                                                                 "k"
                                                                                                 "n"
                                                                                                 "o"
                                                                                                 "w"
                                                                                                 "n"
                                                                                                 " "
                                                                                                 "e"
                                                                                                 "x"
                                                                                                 "c"
                                                                                                 "e"
                                                                                                 "p"
                                                                                                 "t"
                                                                                                 "i"
                                                                                                 "o"
                                                                                                 "n";
                                                                                            Json::
                                                                                              Value
                                                                                                ok;
                                                                                            ok
                                                                                              ["cod"
                                                                                               "e"] =
                                                                                                "SU"
                                                                                                "CC"
                                                                                                "ES"
                                                                                                "S";
                                                                                            ok
                                                                                              ["mes"
                                                                                               "sag"
                                                                                               "e"] =
                                                                                                "O"
                                                                                                "K";
                                                                                            (*cbPtr)(
                                                                                              ok,
                                                                                              std::
                                                                                                error_code()
                                                                                            );
                                                                                        }
                                                                                    },
                                                                                    [cbPtr,
                                                                                     orderNo](
                                                                                      const drogon::
                                                                                        orm::
                                                                                          DrogonDbException
                                                                                            &e
                                                                                    ) {
                                                                                        LOG_ERROR
                                                                                          << "[Call"
                                                                                             "backS"
                                                                                             "ervic"
                                                                                             "e] "
                                                                                             "Faile"
                                                                                             "d "
                                                                                             "to "
                                                                                             "commi"
                                                                                             "t "
                                                                                             "trans"
                                                                                             "actio"
                                                                                             "n "
                                                                                             "for "
                                                                                             "order"
                                                                                             ": "
                                                                                          << orderNo
                                                                                          << ", "
                                                                                             "error"
                                                                                             ": "
                                                                                          << e.base()
                                                                                               .what();
                                                                                        Json::Value
                                                                                          error;
                                                                                        error
                                                                                          ["c"
                                                                                           "o"
                                                                                           "d"
                                                                                           "e"] =
                                                                                            "FAIL";
                                                                                        error
                                                                                          ["m"
                                                                                           "e"
                                                                                           "s"
                                                                                           "s"
                                                                                           "a"
                                                                                           "g"
                                                                                           "e"] =
                                                                                            "Failed"
                                                                                            " to "
                                                                                            "commit"
                                                                                            " trans"
                                                                                            "actio"
                                                                                            "n";
                                                                                        (*cbPtr)(
                                                                                          error,
                                                                                          pay::
                                                                                            makePayError(
                                                                                              1400,
                                                                                              "db "
                                                                                              "tran"
                                                                                              "sact"
                                                                                              "ion "
                                                                                              "unav"
                                                                                              "aila"
                                                                                              "ble"
                                                                                            )
                                                                                        );
                                                                                    }
                                                                                  );
                                                                              }
                                                                          },
                                                                          [cbPtr, orderNo](
                                                                            const drogon::orm::
                                                                              DrogonDbException &e
                                                                          ) {
                                                                              LOG_ERROR
                                                                                << "[Call"
                                                                                   "backS"
                                                                                   "ervic"
                                                                                   "e] "
                                                                                   "Order"
                                                                                   " "
                                                                                   "updat"
                                                                                   "e "
                                                                                   "faile"
                                                                                   "d "
                                                                                   "for "
                                                                                   "order"
                                                                                   ": "
                                                                                << orderNo
                                                                                << ", "
                                                                                   "error"
                                                                                   ": "
                                                                                << e.base().what();
                                                                              Json::Value error;
                                                                              error["code"] =
                                                                                "FAIL";
                                                                              error
                                                                                ["messag"
                                                                                 "e"] =
                                                                                  std::string(
                                                                                    "db"
                                                                                    " e"
                                                                                    "rr"
                                                                                    "or"
                                                                                    ": "
                                                                                  ) +
                                                                                  e.base().what();
                                                                              (*cbPtr)(
                                                                                error,
                                                                                pay::makePayError(
                                                                                  1400,
                                                                                  "db "
                                                                                  "tran"
                                                                                  "sact"
                                                                                  "ion "
                                                                                  "unav"
                                                                                  "aila"
                                                                                  "ble"
                                                                                )
                                                                              );
                                                                          }
                                                                        );
                                                                  }
                                                                  catch (const std::exception &e)
                                                                  {
                                                                      transPtr->rollback();
                                                                      reportMapperFailure(
                                                                        cbPtr, e.what()
                                                                      );
                                                                  }
                                                                  catch (...)
                                                                  {
                                                                      transPtr->rollback();
                                                                      reportMapperFailure(
                                                                        cbPtr,
                                                                        "unknown "
                                                                        "exception"
                                                                      );
                                                                  }
                                                              },
                                                              respondDbError,
                                                              paymentStatus,
                                                              transactionId,
                                                              plaintext,
                                                              paymentNo
                                                            );
                                                        },
                                                        respondDbError
                                                      );
                                                  }
                                                  catch (const std::exception &e)
                                                  {
                                                      transPtr->rollback();
                                                      reportMapperFailure(cbPtr, e.what());
                                                  }
                                                  catch (...)
                                                  {
                                                      transPtr->rollback();
                                                      reportMapperFailure(
                                                        cbPtr, "unknown exception"
                                                      );
                                                  }
                                              },
                                              respondDbError
                                            );
                                        },
                                        respondDbError
                                      );
                                }
                                catch (const std::exception &e)
                                {
                                    transPtr->rollback();
                                    reportMapperFailure(cbPtr, e.what());
                                }
                                catch (...)
                                {
                                    transPtr->rollback();
                                    reportMapperFailure(cbPtr, "unknown exception");
                                }
                            });
                        },
                        [cbPtr, idempotencyKey](const drogon::orm::DrogonDbException &e) {
                            // Real DB failure: report FAIL so the channel retries instead
                            // of acknowledging an unprocessed callback. Duplicates no
                            // longer surface here (ON CONFLICT DO NOTHING resolves them
                            // on the success path with an empty result).
                            LOG_ERROR << "[CallbackService] Idempotency insert failed for key: "
                                      << idempotencyKey << ", error: " << e.base().what();
                            Json::Value error;
                            error["code"] = "FAIL";
                            error["message"] = "internal error";
                            (*cbPtr)(error, pay::makePayError(1400, "idempotency insert failed"));
                        },
                        idempotencyKey,
                        requestHash,
                        ownerToken,
                        expiresAt
                      );
                  }
                  catch (const std::exception &e)
                  {
                      reportMapperFailure(cbPtr, e.what());
                  }
                  catch (...)
                  {
                      reportMapperFailure(cbPtr, "unknown exception");
                  }
              }
            );
        }
        catch (const std::exception &e)
        {
            reportMapperFailure(cbPtr, e.what());
        }
        catch (...)
        {
            reportMapperFailure(cbPtr, "unknown exception");
        }
    };

    // Replay protection (P1-1): gate the DB transaction behind the nonce cache.
    // checkNonce is async (Redis); proceedWithDb (which holds all captured state
    // via shared pointers) is invoked only on first sight of this nonce. On a
    // replay it returns FAIL without touching the DB. Fail-open if Redis is
    // unavailable (see checkNonce).
    LOG_DEBUG << "[CallbackService] About to call proceedWithDb() for order: " << orderNo;
    checkNonce(nonce, [proceedWithDb, respond](bool firstSight) mutable {
        if (!firstSight)
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = "replay detected";
            respond(error, "replay detected");
            return;
        }
        proceedWithDb();
    });
}

void CallbackService::handleRefundCallback(
  const std::string &body,
  const std::string &signature,
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &serialNo,
  CallbackResult &&callback
)
{
    if (!wechatClient_)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "wechat client not ready";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    auto respond = [callback](const Json::Value &result, const std::string &errorMsg) {
        if (!errorMsg.empty())
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = errorMsg;
            callback(error, std::error_code(1400, std::system_category()));
            return;
        }
        callback(result, std::error_code());
    };

    // Verify signature first
    if (!verifySignature(body, signature, timestamp, nonce, serialNo))
    {
        LOG_WARN << "[CallbackService] Signature verification failed";
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "signature verification failed";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }
    LOG_DEBUG << "[CallbackService] Signature verified successfully";

    // Replay protection (P1-1): freshness window. The nonce cache check gates
    // the expensive DB transaction at the bottom of this function (see the
    // checkNonce call wrapping proceedWithDb); body parsing is cheap and runs
    // synchronously first.
    {
        std::string tsError;
        if (!isTimestampFresh(timestamp, tsError))
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = tsError;
            respond(error, tsError);
            return;
        }
    }

    // Parse callback body
    Json::CharReaderBuilder builder;
    Json::Value notifyJson;
    std::string parseErrors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &notifyJson, &parseErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid json";
        respond(error, "invalid json");
        return;
    }

    // Validate event_type
    const std::string eventType = notifyJson.get("event_type", "").asString();
    if (eventType.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing event_type";
        respond(error, "missing event_type");
        return;
    }

    if (eventType.rfind("REFUND.", 0) != 0)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid refund event_type";
        respond(error, "invalid refund event_type");
        return;
    }

    // Validate resource
    if (!notifyJson.isMember("resource"))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing resource";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    const auto &resource = notifyJson["resource"];
    const std::string resourceType = notifyJson.get("resource_type", "").asString();
    if (resourceType.empty() || resourceType != "encrypt-resource")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource_type";
        respond(error, "unsupported resource_type");
        return;
    }

    const std::string algorithm = resource.get("algorithm", "").asString();
    if (algorithm.empty() || algorithm != "AEAD_AES_256_GCM")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource algorithm";
        respond(error, "unsupported resource algorithm");
        return;
    }

    const std::string ciphertext = resource.get("ciphertext", "").asString();
    const std::string nonceStr = resource.get("nonce", "").asString();
    const std::string associatedData = resource.get("associated_data", "").asString();
    if (ciphertext.empty() || nonceStr.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource";
        respond(error, "invalid resource");
        return;
    }

    if (associatedData != "refund")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid refund associated_data";
        respond(error, "invalid refund associated_data");
        return;
    }

    // Decrypt resource
    LOG_DEBUG << "[CallbackService] Decrypting resource...";
    std::string plaintext;
    std::string decryptError;
    if (!wechatClient_
           ->decryptResource(ciphertext, nonceStr, associatedData, plaintext, decryptError))
    {
        LOG_WARN << "[CallbackService] Decryption failed: " << decryptError;
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = decryptError;
        respond(error, decryptError);
        return;
    }
    LOG_DEBUG << "[CallbackService] Decryption successful";

    // Parse decrypted JSON
    Json::Value plainJson;
    Json::CharReaderBuilder plainBuilder;
    std::string plainErrors;
    std::unique_ptr<Json::CharReader> plainReader(plainBuilder.newCharReader());
    if (!plainReader
           ->parse(plaintext.data(), plaintext.data() + plaintext.size(), &plainJson, &plainErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource json";
        respond(error, "invalid resource json");
        return;
    }

    // Validate appid and mchid
    const std::string appId = plainJson.get("appid", "").asString();
    const std::string mchId = plainJson.get("mchid", "").asString();
    if (!appId.empty() && wechatClient_ && appId != wechatClient_->getAppId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "appid mismatch";
        respond(error, "appid mismatch");
        return;
    }
    if (!mchId.empty() && wechatClient_ && mchId != wechatClient_->getMchId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "mchid mismatch";
        respond(error, "mchid mismatch");
        return;
    }

    // Extract refund details
    const std::string refundNo = plainJson.get("out_refund_no", "").asString();
    const std::string refundStatusRaw = plainJson.get("refund_status", "").asString();
    const std::string refundId = plainJson.get("refund_id", "").asString();

    if (refundNo.empty() || refundStatusRaw.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing refund_no/refund_status";
        respond(error, "missing refund_no/refund_status");
        return;
    }

    if (refundStatusRaw == "SUCCESS" && refundId.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing refund_id";
        respond(error, "missing refund_id");
        return;
    }

    // Idempotency check
    std::string idempotencyKey = notifyJson.get("id", "").asString();
    if (idempotencyKey.empty())
    {
        idempotencyKey = refundNo + ":" + refundStatusRaw;
    }

    auto cbPtr = std::make_shared<CallbackResult>(std::move(callback));

    auto proceedRefundDb = [this,
                            cbPtr,
                            idempotencyKey,
                            refundNo,
                            refundStatusRaw,
                            refundId,
                            signature,
                            serialNo,
                            plaintext,
                            body,
                            plainJson]() {
        try
        {
            drogon::orm::Mapper<PayIdempotencyModel> idempMapper(dbClient_);
            auto idempCriteria = drogon::orm::Criteria(
              PayIdempotencyModel::Cols::_idempotency_key,
              drogon::orm::CompareOperator::EQ,
              idempotencyKey
            );
            idempMapper.findOne(
              idempCriteria,
              [this, cbPtr, refundNo, idempotencyKey, body, signature, serialNo, plainJson](
                const PayIdempotencyModel &existing
              ) {
                  // Same rule as the transaction branch: an unfinalized snapshot is
                  // not evidence this refund notification was handled, so acking it
                  // would stop the channel's retries on a refund that was never
                  // booked. Drop the reservation and let the next delivery run.
                  if (!existing.getResponseSnapshot())
                  {
                      LOG_WARN << "[CallbackService] Idempotency record for refund " << refundNo
                               << " has no finalized snapshot; dropping the stale reservation";
                      auto respondRetryLater = [cbPtr]() {
                          Json::Value error;
                          error["code"] = "FAIL";
                          error["message"] = "callback still in progress";
                          (*cbPtr)(error, pay::makePayError(1400, "callback still in progress"));
                      };
                      dropUnfinalizedReservation(
                        dbClient_, idempotencyKey, "refund " + refundNo, respondRetryLater
                      );
                      return;
                  }

                  // Already processed - record callback and return success
                  LOG_DEBUG << "[CallbackService] Refund idempotency key found for refund: "
                            << refundNo << ", recording callback";

                  auto respondSuccess = [cbPtr]() {
                      Json::Value ok;
                      ok["code"] = "SUCCESS";
                      ok["message"] = "OK";
                      (*cbPtr)(ok, std::error_code());
                  };

                  auto respondDbError = [cbPtr](const drogon::orm::DrogonDbException &e) {
                      LOG_ERROR
                        << "[CallbackService] DB error recording idempotent refund callback: "
                        << e.base().what();
                      Json::Value error;
                      error["code"] = "FAIL";
                      error["message"] = "internal error";
                      (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
                  };

                  // Look up payment via out_trade_no from the decrypted plaintext
                  const std::string tradeOrderNo = plainJson.get("out_trade_no", "").asString();
                  if (tradeOrderNo.empty())
                  {
                      LOG_ERROR
                        << "[CallbackService] Missing out_trade_no in idempotent refund callback";
                      Json::Value error;
                      error["code"] = "FAIL";
                      error["message"] = "missing out_trade_no";
                      (*cbPtr)(error, pay::makePayError(1400, "missing out_trade_no"));
                      return;
                  }

                  try
                  {
                      drogon::orm::Mapper<PayPaymentModel> paymentLookup(dbClient_);
                      // Same reason as the transaction branch above: an order can
                      // carry several payment attempts, and findOne() treats that
                      // as an error rather than handing back one row. The closed
                      // ones are excluded for the same reason the settle path
                      // excludes them -- the audit row belongs to the attempt that
                      // carried the money.
                      paymentLookup
                        .orderBy(PayPaymentModel::Cols::_created_at, drogon::orm::SortOrder::DESC)
                        .limit(1)
                        .findBy(
                          openAttemptsOfOrder(tradeOrderNo),
                          [this, cbPtr, body, signature, serialNo, respondSuccess, respondDbError](
                            const std::vector<PayPaymentModel> &rows
                          ) {
                              if (rows.empty())
                              {
                                  respondDbError(drogon::orm::UnexpectedRows("0 rows found"));
                                  return;
                              }
                              const std::string paymentNo = rows.front().getValueOfPaymentNo();

                              try
                              {
                                  drogon::orm::Mapper<PayCallbackModel> callbackMapper(dbClient_);
                                  PayCallbackModel callbackRow;
                                  callbackRow.setPaymentNo(paymentNo);
                                  callbackRow.setRawBody(body);
                                  callbackRow.setSignature(signature);
                                  callbackRow.setSerialNo(serialNo);
                                  callbackRow.setVerified(true);
                                  callbackRow.setProcessed(true);
                                  callbackRow.setReceivedAt(trantor::Date::now());

                                  callbackMapper.insert(
                                    callbackRow,
                                    [respondSuccess](const PayCallbackModel &) {
                                        respondSuccess();
                                    },
                                    respondDbError
                                  );
                              }
                              catch (const std::exception &e)
                              {
                                  reportMapperFailure(cbPtr, e.what());
                              }
                              catch (...)
                              {
                                  reportMapperFailure(cbPtr, "unknown exception");
                              }
                          },
                          [respondDbError](const drogon::orm::DrogonDbException &e) {
                              LOG_ERROR << "[CallbackService] Payment not found during idempotent "
                                           "refund callback: "
                                        << e.base().what();
                              respondDbError(e);
                          }
                        );
                  }
                  catch (const std::exception &e)
                  {
                      reportMapperFailure(cbPtr, e.what());
                  }
                  catch (...)
                  {
                      reportMapperFailure(cbPtr, "unknown exception");
                  }
              },
              [this,
               cbPtr,
               idempotencyKey,
               refundNo,
               refundStatusRaw,
               refundId,
               signature,
               serialNo,
               plaintext,
               body,
               plainJson](const drogon::orm::DrogonDbException &e) {
                  // Only UnexpectedRows means "key not found"; any other DB failure
                  // must NOT be treated as a new callback (risk of double handling).
                  if (dynamic_cast<const drogon::orm::UnexpectedRows *>(&e) == nullptr)
                  {
                      LOG_ERROR << "[CallbackService] Refund idempotency lookup DB error: "
                                << e.base().what();
                      Json::Value error;
                      error["code"] = "FAIL";
                      error["message"] = "internal error";
                      (*cbPtr)(error, pay::makePayError(1400, "idempotency lookup failed"));
                      return;
                  }
                  const std::string requestHash = drogon::utils::getMd5(body);
                  // Reserve with response_snapshot = NULL (P2-4.2). Finalized after
                  // the business transaction commits below.
                  const auto now = trantor::Date::now();
                  const auto expiresAt = trantor::Date(
                    now.microSecondsSinceEpoch() + static_cast<int64_t>(7) * 24 * 60 * 60 * 1000000
                  );
                  // This delivery's ownership token for the reservation below.
                  const std::string ownerToken = newReservationToken();

                  // Raw SQL exemption (db-operations): see the payment-callback path
                  // above -- ON CONFLICT DO NOTHING RETURNING is the only
                  // deterministic duplicate-key check (the PG backend throws a
                  // generic, locale-dependent Failure), and Mapper cannot express it.
                  try
                  {
                      dbClient_->execSqlAsync(
                        "INSERT INTO pay_idempotency (idempotency_key, request_hash, "
                        "response_snapshot, owner_token, expire_at) "
                        "VALUES ($1, $2, NULL, $3, $4) "
                        "ON CONFLICT (idempotency_key) DO NOTHING "
                        "RETURNING idempotency_key",
                        [this,
                         cbPtr,
                         refundNo,
                         refundStatusRaw,
                         refundId,
                         signature,
                         serialNo,
                         plaintext,
                         body,
                         plainJson,
                         idempotencyKey,
                         ownerToken](const drogon::orm::Result &insertResult) {
                            if (insertResult.empty())
                            {
                                // 0 rows: a concurrent refund delivery reserved this key
                                // first, but an unfinalized reservation is not proof it
                                // completed -- the winner may still be running or may have
                                // died before setting the snapshot. ACKing SUCCESS here
                                // would stop the channel's retries on a refund that was
                                // never booked (the read path refuses this at findOne).
                                // Answer FAIL so it is retried; the next delivery reads the
                                // row and a finalized snapshot is acknowledged idempotently.
                                LOG_DEBUG << "[CallbackService] Concurrent refund reservation "
                                             "for key: "
                                          << idempotencyKey << ", requesting retry";
                                Json::Value error;
                                error["code"] = "FAIL";
                                error["message"] = "callback still in progress";
                                (*cbPtr)(
                                  error, pay::makePayError(1400, "callback still in progress")
                                );
                                return;
                            }
                            const std::string refundStatus =
                              pay::utils::mapRefundStatus(refundStatusRaw);
                            if (refundStatus.empty())
                            {
                                Json::Value error;
                                error["code"] = "FAIL";
                                error["message"] = "invalid refund status";
                                (*cbPtr)(
                                  error, pay::makePayError(1400, "db transaction unavailable")
                                );
                                return;
                            }

                            try
                            {
                                drogon::orm::Mapper<PayRefundModel> refundMapper(dbClient_);
                                auto refundCriteria = drogon::orm::Criteria(
                                  PayRefundModel::Cols::_refund_no,
                                  drogon::orm::CompareOperator::EQ,
                                  refundNo
                                );
                                refundMapper.findOne(
                                  refundCriteria,
                                  [this,
                                   cbPtr,
                                   refundStatus,
                                   refundId,
                                   signature,
                                   serialNo,
                                   refundNo,
                                   body,
                                   plaintext,
                                   plainJson,
                                   idempotencyKey,
                                   ownerToken](PayRefundModel refund) {
                                      // Already successful - return success
                                      if (refund.getValueOfStatus() == "REFUND_SUCCESS")
                                      {
                                          Json::Value ok;
                                          ok["code"] = "SUCCESS";
                                          ok["message"] = "OK";
                                          (*cbPtr)(ok, std::error_code());
                                          return;
                                      }

                                      const auto orderNo = refund.getValueOfOrderNo();
                                      const auto paymentNo = refund.getValueOfPaymentNo();
                                      const auto refundAmount = refund.getValueOfAmount();
                                      const auto &amountJson = plainJson["amount"];
                                      const std::string notifyCurrency =
                                        amountJson.get("currency", "").asString();
                                      const int64_t notifyRefundFen =
                                        amountJson.get("refund", 0).asInt64();
                                      int64_t refundTotalFen = 0;
                                      if (
                                        !pay::utils::parseAmountToFen(
                                          refundAmount, refundTotalFen
                                        ) ||
                                        notifyRefundFen <= 0
                                      )
                                      {
                                          Json::Value error;
                                          error["code"] = "FAIL";
                                          error["message"] = "invalid refund amount in callback";
                                          (*cbPtr)(
                                            error,
                                            pay::makePayError(1400, "db transaction unavailable")
                                          );
                                          return;
                                      }
                                      if (notifyRefundFen != refundTotalFen)
                                      {
                                          Json::Value error;
                                          error["code"] = "FAIL";
                                          error["message"] = "refund amount mismatch";
                                          (*cbPtr)(
                                            error,
                                            pay::makePayError(1400, "db transaction unavailable")
                                          );
                                          return;
                                      }

                                      try
                                      {
                                          drogon::orm::Mapper<PayOrderModel> orderMapper(dbClient_);
                                          auto orderCriteria = drogon::orm::Criteria(
                                            PayOrderModel::Cols::_order_no,
                                            drogon::orm::CompareOperator::EQ,
                                            orderNo
                                          );
                                          orderMapper.findOne(
                                            orderCriteria,
                                            [this,
                                             cbPtr,
                                             refundStatus,
                                             refundId,
                                             refundAmount,
                                             orderNo,
                                             paymentNo,
                                             notifyCurrency,
                                             signature,
                                             serialNo,
                                             refundNo,
                                             body,
                                             plaintext,
                                             refund,
                                             idempotencyKey,
                                             ownerToken](const PayOrderModel &order) mutable {
                                                const std::string orderCurrency =
                                                  order.getValueOfCurrency();
                                                if (
                                                  !notifyCurrency.empty() &&
                                                  notifyCurrency != orderCurrency
                                                )
                                                {
                                                    Json::Value error;
                                                    error["code"] = "FAIL";
                                                    error["message"] = "refund currency mismatch";
                                                    (*cbPtr)(
                                                      error,
                                                      pay::makePayError(
                                                        1400, "db transaction unavailable"
                                                      )
                                                    );
                                                    return;
                                                }

                                                refund.setStatus(refundStatus);
                                                refund.setChannelRefundNo(refundId);
                                                dbClient_->newTransactionAsync([this,
                                                                                cbPtr,
                                                                                refundStatus,
                                                                                refundAmount,
                                                                                refundId,
                                                                                orderNo,
                                                                                paymentNo,
                                                                                order,
                                                                                signature,
                                                                                serialNo,
                                                                                body,
                                                                                refundNo,
                                                                                plaintext,
                                                                                refund,
                                                                                idempotencyKey,
                                                                                ownerToken](
                                                                                 const std::
                                                                                   shared_ptr<
                                                                                     drogon::orm::
                                                                                       Transaction>
                                                                                     &transPtr
                                                                               ) mutable {
                                                    auto respondDbError =
                                                      [cbPtr, transPtr](
                                                        const drogon::orm::DrogonDbException &e
                                                      ) {
                                                          LOG_ERROR
                                                            << "[CallbackService] DB error in "
                                                               "refund callback transaction: "
                                                            << e.base().what();
                                                          transPtr->rollback();
                                                          Json::Value error;
                                                          error["code"] = "FAIL";
                                                          error["message"] = "internal error";
                                                          (*cbPtr)(
                                                            error,
                                                            pay::makePayError(
                                                              1400, "db transaction unavailable"
                                                            )
                                                          );
                                                      };

                                                    // CAS-style refund status transition: only
                                                    // update if the refund is still in a non-final
                                                    // state. The refund row was read outside this
                                                    // transaction, so without a CAS guard a
                                                    // concurrent callback could overwrite a
                                                    // REFUND_SUCCESS that another transaction just
                                                    // wrote. An empty RETURNING set means a
                                                    // concurrent transaction already advanced this
                                                    // refund; we treat it as already-processed and
                                                    // return success. Uses UPDATE...RETURNING
                                                    // (raw-SQL exemption #2).
                                                    transPtr->execSqlAsync(
                                                      "UPDATE pay_refund "
                                                      "SET status = $1, channel_refund_no = $2 "
                                                      "WHERE refund_no = $3 "
                                                      "AND status IN ('REFUND_INIT', 'REFUNDING') "
                                                      "RETURNING 1",
                                                      [this,
                                                       cbPtr,
                                                       refundStatus,
                                                       refundAmount,
                                                       refundId,
                                                       orderNo,
                                                       paymentNo,
                                                       order,
                                                       signature,
                                                       serialNo,
                                                       body,
                                                       refundNo,
                                                       plaintext,
                                                       transPtr,
                                                       idempotencyKey,
                                                       ownerToken](
                                                        const drogon::orm::Result &casResult
                                                      ) {
                                                          if (casResult.size() == 0)
                                                          {
                                                              LOG_DEBUG
                                                                << "[CallbackService] "
                                                                   "Refund already "
                                                                   "advanced by a "
                                                                   "concurrent transaction: "
                                                                << refundNo << ", skipping";
                                                              transPtr->rollback();
                                                              // Record this verified delivery in
                                                              // the audit trail (P4): insert on
                                                              // dbClient_ so it persists despite
                                                              // the rollback above, then return
                                                              // success.
                                                              PayCallbackModel dupRow;
                                                              dupRow.setPaymentNo(paymentNo);
                                                              dupRow.setRawBody(body);
                                                              dupRow.setSignature(signature);
                                                              dupRow.setSerialNo(serialNo);
                                                              dupRow.setVerified(true);
                                                              dupRow.setProcessed(true);
                                                              dupRow.setReceivedAt(
                                                                trantor::Date::now()
                                                              );
                                                              try
                                                              {
                                                                  drogon::orm::Mapper<
                                                                    PayCallbackModel>
                                                                    dupMapper(dbClient_);
                                                                  dupMapper.insert(
                                                                    dupRow,
                                                                    [cbPtr](
                                                                      const PayCallbackModel &
                                                                    ) {
                                                                        Json::Value ok;
                                                                        ok["code"] = "SUCCESS";
                                                                        ok["message"] = "OK";
                                                                        (*cbPtr)(
                                                                          ok, std::error_code()
                                                                        );
                                                                    },
                                                                    [cbPtr](
                                                                      const drogon::orm::
                                                                        DrogonDbException &
                                                                    ) {
                                                                        // Audit insert failed;
                                                                        // still tell the channel to
                                                                        // stop retrying (state is
                                                                        // already advanced).
                                                                        Json::Value ok;
                                                                        ok["code"] = "SUCCESS";
                                                                        ok["message"] = "OK";
                                                                        (*cbPtr)(
                                                                          ok, std::error_code()
                                                                        );
                                                                    }
                                                                  );
                                                              }
                                                              catch (const std::exception &e)
                                                              {
                                                                  // Audit mapper failed; state
                                                                  // already advanced, so still ACK
                                                                  // the channel.
                                                                  LOG_ERROR << "[CallbackService] "
                                                                               "Audit mapper "
                                                                               "error: "
                                                                            << e.what();
                                                                  Json::Value ok;
                                                                  ok["code"] = "SUCCESS";
                                                                  ok["message"] = "OK";
                                                                  (*cbPtr)(ok, std::error_code());
                                                              }
                                                              catch (...)
                                                              {
                                                                  LOG_ERROR
                                                                    << "[CallbackService] Audit "
                                                                       "mapper "
                                                                       "error: unknown exception";
                                                                  Json::Value ok;
                                                                  ok["code"] = "SUCCESS";
                                                                  ok["message"] = "OK";
                                                                  (*cbPtr)(ok, std::error_code());
                                                              }
                                                              return;
                                                          }
                                                          try
                                                          {
                                                              drogon::orm::Mapper<PayRefundModel>
                                                                refundPayloadUpdater(transPtr);
                                                              refundPayloadUpdater.updateBy(
                                                                {PayRefundModel::Cols::
                                                                   _response_payload},
                                                                [transPtr,
                                                                 cbPtr,
                                                                 refundStatus,
                                                                 refundNo,
                                                                 orderNo,
                                                                 order](const size_t) {
                                                                    // A settled refund is what
                                                                    // moves the parent order;
                                                                    // REFUNDING means WeChat only
                                                                    // accepted the request, so no
                                                                    // money has come back yet. Same
                                                                    // rule as
                                                                    // RefundService::updateRefundWithSuccess.
                                                                    if (
                                                                      refundStatus !=
                                                                      "REFUND_SUCCESS"
                                                                    )
                                                                    {
                                                                        return;
                                                                    }
                                                                    auto failWrite =
                                                                      [cbPtr, transPtr](
                                                                        const std::string &what
                                                                      ) {
                                                                          LOG_ERROR
                                                                            << "[CallbackService] "
                                                                               "Refunded-order "
                                                                               "update failed: "
                                                                            << what;
                                                                          transPtr->rollback();
                                                                          Json::Value error;
                                                                          error["code"] = "FAIL";
                                                                          error["message"] =
                                                                            "internal error";
                                                                          (*cbPtr)(
                                                                            error,
                                                                            pay::makePayError(
                                                                              1400,
                                                                              "db transaction "
                                                                              "unavailable"
                                                                            )
                                                                          );
                                                                      };
                                                                    // std::function so the coverage
                                                                    // read below can copy the
                                                                    // handler: a plain lambda here
                                                                    // would dangle once this
                                                                    // callback returns and its
                                                                    // frame dies.
                                                                    const std::function<
                                                                      void(const drogon::orm::
                                                                             DrogonDbException &)>
                                                                      failOrderWrite =
                                                                        [failWrite](
                                                                          const drogon::orm::
                                                                            DrogonDbException &e
                                                                        ) {
                                                                            failWrite(
                                                                              e.base().what()
                                                                            );
                                                                        };

                                                                    // One settled refund is not the
                                                                    // whole refund: WeChat honours
                                                                    // up to fifty partial refunds
                                                                    // per order, so writing
                                                                    // REFUNDED here on every one of
                                                                    // them reported an order as
                                                                    // fully returned while most of
                                                                    // the money was still with the
                                                                    // merchant. The sum has to say
                                                                    // so. It runs inside this
                                                                    // transaction, where the CAS
                                                                    // above already flipped this
                                                                    // row, so the settled rows are
                                                                    // the whole amount returned to
                                                                    // date. Aggregate SUM (raw-SQL
                                                                    // exemption #3): the Mapper
                                                                    // cannot express SUM.
                                                                    int64_t orderTotalFen = 0;
                                                                    // An amount that does not parse
                                                                    // leaves the total at zero,
                                                                    // which the coverage rule below
                                                                    // reads as "nothing measured":
                                                                    // the order keeps the status it
                                                                    // has rather than being guessed
                                                                    // at.
                                                                    pay::utils::parseAmountToFen(
                                                                      order.getValueOfAmount(),
                                                                      orderTotalFen
                                                                    );
                                                                    transPtr->execSqlAsync(
                                                                      "SELECT "
                                                                      "COALESCE(SUM(CAST(amount AS "
                                                                      "NUMERIC)), 0) AS sum_amount "
                                                                      "FROM pay_refund WHERE "
                                                                      "order_no = $1 AND status = "
                                                                      "$2",
                                                                      [transPtr,
                                                                       cbPtr,
                                                                       failWrite,
                                                                       failOrderWrite,
                                                                       refundNo,
                                                                       orderNo,
                                                                       orderTotalFen](
                                                                        const drogon::orm::Result
                                                                          &sumResult
                                                                      ) {
                                                                          int64_t settledRefundFen =
                                                                            0;
                                                                          if (!sumResult.empty())
                                                                          {
                                                                              const auto sumText =
                                                                                sumResult
                                                                                  .front()
                                                                                    ["sum_amount"]
                                                                                  .as<
                                                                                    std::string>();
                                                                              if (
                                                                                !pay::utils::
                                                                                  parseAmountToFen(
                                                                                    sumText,
                                                                                    settledRefundFen
                                                                                  )
                                                                              )
                                                                              {
                                                                                  // The sum is
                                                                                  // unreadable, so
                                                                                  // nothing is
                                                                                  // known about
                                                                                  // coverage. Roll
                                                                                  // the transaction
                                                                                  // back rather
                                                                                  // than commit a
                                                                                  // half-decision.
                                                                                  failWrite(
                                                                                    "Invalid "
                                                                                    "settled "
                                                                                    "refund sum"
                                                                                  );
                                                                                  return;
                                                                              }
                                                                          }
                                                                          if (
                                                                            !pay::utils::
                                                                              refundsCoverOrderAmount(
                                                                                settledRefundFen,
                                                                                orderTotalFen
                                                                              )
                                                                          )
                                                                          {
                                                                              LOG_DEBUG
                                                                                << "[CallbackServic"
                                                                                   "e] Refund "
                                                                                << refundNo
                                                                                << " settled, but "
                                                                                   "the refunds on "
                                                                                   "order "
                                                                                << orderNo << " ("
                                                                                << settledRefundFen
                                                                                << " fen) do not "
                                                                                   "yet cover its "
                                                                                   "total ("
                                                                                << orderTotalFen
                                                                                << " fen); the "
                                                                                   "order stays as "
                                                                                   "it is";
                                                                              return;
                                                                          }
                                                                          // Guarded on PAID and run
                                                                          // inside this
                                                                          // transaction: the CAS
                                                                          // above proved this
                                                                          // callback owns the
                                                                          // refund transition, and
                                                                          // the guard keeps a
                                                                          // concurrently closed
                                                                          // order from reopening.
                                                                          try
                                                                          {
                                                                              drogon::orm::Mapper<
                                                                                PayOrderModel>
                                                                                orderUpdater(
                                                                                  transPtr
                                                                                );
                                                                              orderUpdater.updateBy(
                                                                                {PayOrderModel::
                                                                                   Cols::_status},
                                                                                [refundNo, orderNo](
                                                                                  const size_t
                                                                                    updated
                                                                                ) {
                                                                                    if (
                                                                                      updated == 0
                                                                                    )
                                                                                    {
                                                                                        LOG_DEBUG
                                                                                          << "[Call"
                                                                                             "backS"
                                                                                             "ervic"
                                                                                             "e] "
                                                                                             "Order"
                                                                                             " "
                                                                                          << orderNo
                                                                                          << " was "
                                                                                             "no "
                                                                                             "longe"
                                                                                             "r "
                                                                                             "PAID "
                                                                                             "when "
                                                                                          << refundNo
                                                                                          << " sett"
                                                                                             "led";
                                                                                    }
                                                                                    else
                                                                                    {
                                                                                        LOG_INFO
                                                                                          << "[Call"
                                                                                             "backS"
                                                                                             "ervic"
                                                                                             "e] "
                                                                                             "Order"
                                                                                             " "
                                                                                          << orderNo
                                                                                          << " is "
                                                                                             "now "
                                                                                             "REFUN"
                                                                                             "DED ("
                                                                                          << refundNo
                                                                                          << ")";
                                                                                    }
                                                                                },
                                                                                failOrderWrite,
                                                                                drogon::orm::Criteria(
                                                                                  PayOrderModel::
                                                                                    Cols::_order_no,
                                                                                  drogon::orm::
                                                                                    CompareOperator::
                                                                                      EQ,
                                                                                  orderNo
                                                                                ) &&
                                                                                  drogon::orm::
                                                                                    Criteria(
                                                                                      PayOrderModel::
                                                                                        Cols::
                                                                                          _status,
                                                                                      drogon::orm::
                                                                                        CompareOperator::
                                                                                          EQ,
                                                                                      "PAID"
                                                                                    ),
                                                                                std::string(
                                                                                  "REFUNDED"
                                                                                )
                                                                              );
                                                                          }
                                                                          catch (
                                                                            const std::exception &e
                                                                          )
                                                                          {
                                                                              transPtr->rollback();
                                                                              reportMapperFailure(
                                                                                cbPtr, e.what()
                                                                              );
                                                                          }
                                                                          catch (...)
                                                                          {
                                                                              transPtr->rollback();
                                                                              reportMapperFailure(
                                                                                cbPtr,
                                                                                "unknown exception"
                                                                              );
                                                                          }
                                                                      },
                                                                      failOrderWrite,
                                                                      orderNo,
                                                                      std::string("REFUND_SUCCESS")
                                                                    );
                                                                },
                                                                [cbPtr, transPtr](
                                                                  const drogon::orm::
                                                                    DrogonDbException &e
                                                                ) {
                                                                    LOG_ERROR
                                                                      << "[CallbackService] "
                                                                         "Failed to update "
                                                                         "refund payload: "
                                                                      << e.base().what();
                                                                    transPtr->rollback();
                                                                    Json::Value error;
                                                                    error["code"] = "FAIL";
                                                                    error["message"] =
                                                                      "internal error";
                                                                    (*cbPtr)(
                                                                      error,
                                                                      pay::makePayError(
                                                                        1400,
                                                                        "db transaction unavailable"
                                                                      )
                                                                    );
                                                                },
                                                                drogon::orm::Criteria(
                                                                  PayRefundModel::Cols::_refund_no,
                                                                  drogon::orm::CompareOperator::EQ,
                                                                  refundNo
                                                                ),
                                                                plaintext
                                                              );
                                                          }
                                                          catch (const std::exception &e)
                                                          {
                                                              transPtr->rollback();
                                                              reportMapperFailure(cbPtr, e.what());
                                                              return;
                                                          }
                                                          catch (...)
                                                          {
                                                              transPtr->rollback();
                                                              reportMapperFailure(
                                                                cbPtr, "unknown exception"
                                                              );
                                                              return;
                                                          }

                                                          // Lambda to insert callback record and
                                                          // call final callback
                                                          auto insertCallbackAndFinish = [cbPtr,
                                                                                          transPtr,
                                                                                          paymentNo,
                                                                                          body,
                                                                                          signature,
                                                                                          serialNo,
                                                                                          idempotencyKey,
                                                                                          ownerToken,
                                                                                          plaintext]() {
                                                              PayCallbackModel callbackRow;
                                                              callbackRow.setPaymentNo(paymentNo);
                                                              callbackRow.setRawBody(body);
                                                              callbackRow.setSignature(signature);
                                                              callbackRow.setSerialNo(serialNo);
                                                              callbackRow.setVerified(true);
                                                              callbackRow.setProcessed(true);
                                                              callbackRow.setReceivedAt(
                                                                trantor::Date::now()
                                                              );

                                                              try
                                                              {
                                                                  drogon::orm::Mapper<
                                                                    PayCallbackModel>
                                                                    callbackMapper(transPtr);
                                                                  callbackMapper.insert(
                                                                    callbackRow,
                                                                    [cbPtr,
                                                                     transPtr,
                                                                     idempotencyKey,
                                                                     ownerToken,
                                                                     plaintext](
                                                                      const PayCallbackModel &
                                                                    ) {
                                                                        LOG_DEBUG
                                                                          << "[CallbackService] "
                                                                             "Manually committing "
                                                                             "transaction for "
                                                                             "refund callback";
                                                                        try
                                                                        {
                                                                            finalizeReservation(
                                                                              transPtr,
                                                                              idempotencyKey,
                                                                              ownerToken,
                                                                              plaintext,
                                                                              [cbPtr, transPtr]() {
                                                                                  // Explicit COMMIT
                                                                                  // (raw-SQL
                                                                                  // exemption
                                                                                  // candidate): the
                                                                                  // channel is
                                                                                  // ACKed only
                                                                                  // after COMMIT
                                                                                  // succeeds; an
                                                                                  // implicit
                                                                                  // destructor-time
                                                                                  // commit would
                                                                                  // ACK before
                                                                                  // durability and
                                                                                  // lose the
                                                                                  // channel retry.
                                                                                  transPtr->execSqlAsync(
                                                                                    "COMMIT",
                                                                                    [cbPtr](
                                                                                      const drogon::
                                                                                        orm::Result
                                                                                          &
                                                                                    ) {
                                                                                        LOG_DEBUG
                                                                                          << "[Call"
                                                                                             "back"
                                                                                             "Servi"
                                                                                             "ce] "
                                                                                             "Trans"
                                                                                             "acti"
                                                                                             "on "
                                                                                             "commi"
                                                                                             "tted"
                                                                                             ", "
                                                                                             "calli"
                                                                                             "ng "
                                                                                             "final"
                                                                                             " "
                                                                                             "succe"
                                                                                             "ss "
                                                                                             "callb"
                                                                                             "ack "
                                                                                             "for "
                                                                                             "refun"
                                                                                             "d";
                                                                                        Json::Value
                                                                                          ok;
                                                                                        ok["code"] =
                                                                                          "SUCCESS";
                                                                                        ok
                                                                                          ["messag"
                                                                                           "e"] =
                                                                                            "OK";
                                                                                        (*cbPtr)(
                                                                                          ok,
                                                                                          std::
                                                                                            error_code()
                                                                                        );
                                                                                    },
                                                                                    [cbPtr,
                                                                                     transPtr](
                                                                                      const drogon::
                                                                                        orm::
                                                                                          DrogonDbException
                                                                                            &e
                                                                                    ) {
                                                                                        LOG_ERROR
                                                                                          << "[Call"
                                                                                             "back"
                                                                                             "Servi"
                                                                                             "ce] "
                                                                                             "Faile"
                                                                                             "d "
                                                                                             "to "
                                                                                             "commi"
                                                                                             "t "
                                                                                             "refun"
                                                                                             "d: "
                                                                                          << e.base()
                                                                                               .what();
                                                                                        transPtr
                                                                                          ->rollback();
                                                                                        Json::Value
                                                                                          err;
                                                                                        err
                                                                                          ["code"] =
                                                                                            "FAIL";
                                                                                        err
                                                                                          ["messag"
                                                                                           "e"] =
                                                                                            std::
                                                                                              string(
                                                                                                "db"
                                                                                                " e"
                                                                                                "rr"
                                                                                                "or"
                                                                                                ": "
                                                                                              ) +
                                                                                            e.base()
                                                                                              .what();
                                                                                        (*cbPtr)(
                                                                                          err,
                                                                                          pay::
                                                                                            makePayError(
                                                                                              1400,
                                                                                              "db "
                                                                                              "comm"
                                                                                              "it "
                                                                                              "erro"
                                                                                              "r"
                                                                                            )
                                                                                        );
                                                                                    }
                                                                                  );
                                                                              },
                                                                              [cbPtr,
                                                                               transPtr,
                                                                               idempotencyKey]() {
                                                                                  // The reservation
                                                                                  // no longer
                                                                                  // belongs to this
                                                                                  // delivery: a
                                                                                  // concurrent
                                                                                  // retry's read
                                                                                  // path dropped it
                                                                                  // and a later
                                                                                  // delivery
                                                                                  // re-reserved. Do
                                                                                  // not commit
                                                                                  // proof we do not
                                                                                  // hold; roll back
                                                                                  // and let the
                                                                                  // channel retry.
                                                                                  LOG_WARN
                                                                                    << "[CallbackSe"
                                                                                       "rvice] "
                                                                                       "Refund "
                                                                                       "idempotency"
                                                                                       " reservatio"
                                                                                       "n was "
                                                                                       "taken "
                                                                                       "over; "
                                                                                       "rolling "
                                                                                       "back and "
                                                                                       "requesting "
                                                                                       "retry";
                                                                                  transPtr
                                                                                    ->rollback();
                                                                                  Json::Value err;
                                                                                  err["code"] =
                                                                                    "FAIL";
                                                                                  err["message"] =
                                                                                    "internal "
                                                                                    "error";
                                                                                  (*cbPtr)(
                                                                                    err,
                                                                                    pay::
                                                                                      makePayError(
                                                                                        1400,
                                                                                        "idempotenc"
                                                                                        "y "
                                                                                        "reservatio"
                                                                                        "n "
                                                                                        "lost"
                                                                                      )
                                                                                  );
                                                                              },
                                                                              [cbPtr, transPtr](
                                                                                const drogon::orm::
                                                                                  DrogonDbException
                                                                                    &e
                                                                              ) {
                                                                                  LOG_ERROR
                                                                                    << "[CallbackSe"
                                                                                       "rvic"
                                                                                       "e] Failed "
                                                                                       "to update "
                                                                                       "idempotency"
                                                                                       ": "
                                                                                    << e.base()
                                                                                         .what();
                                                                                  transPtr
                                                                                    ->rollback();
                                                                                  Json::Value err;
                                                                                  err["code"] =
                                                                                    "FAIL";
                                                                                  err["message"] =
                                                                                    "internal "
                                                                                    "error";
                                                                                  (*cbPtr)(
                                                                                    err,
                                                                                    pay::
                                                                                      makePayError(
                                                                                        1400,
                                                                                        "db "
                                                                                        "idempotenc"
                                                                                        "y "
                                                                                        "error"
                                                                                      )
                                                                                  );
                                                                              }
                                                                            );
                                                                        }
                                                                        catch (
                                                                          const std::exception &e
                                                                        )
                                                                        {
                                                                            transPtr->rollback();
                                                                            reportMapperFailure(
                                                                              cbPtr, e.what()
                                                                            );
                                                                        }
                                                                        catch (...)
                                                                        {
                                                                            transPtr->rollback();
                                                                            reportMapperFailure(
                                                                              cbPtr,
                                                                              "unknown exception"
                                                                            );
                                                                        }
                                                                    },
                                                                    [cbPtr, transPtr](
                                                                      const drogon::orm::
                                                                        DrogonDbException &e
                                                                    ) {
                                                                        LOG_ERROR
                                                                          << "[CallbackService] "
                                                                             "Failed to insert "
                                                                             "refund callback "
                                                                             "record: "
                                                                          << e.base().what();
                                                                        transPtr->rollback();
                                                                        Json::Value error;
                                                                        error["code"] = "FAIL";
                                                                        error["message"] =
                                                                          "internal error";
                                                                        (*cbPtr)(
                                                                          error,
                                                                          pay::makePayError(
                                                                            1400,
                                                                            "db transaction "
                                                                            "unavailable"
                                                                          )
                                                                        );
                                                                    }
                                                                  );
                                                              }
                                                              catch (const std::exception &e)
                                                              {
                                                                  transPtr->rollback();
                                                                  reportMapperFailure(
                                                                    cbPtr, e.what()
                                                                  );
                                                              }
                                                              catch (...)
                                                              {
                                                                  transPtr->rollback();
                                                                  reportMapperFailure(
                                                                    cbPtr, "unknown exception"
                                                                  );
                                                              }
                                                          };

                                                          if (refundStatus == "REFUND_SUCCESS")
                                                          {
                                                              auto transDb =
                                                                std::static_pointer_cast<
                                                                  drogon::orm::DbClient>(transPtr);
                                                              insertLedgerEntry(
                                                                transDb,
                                                                order.getValueOfUserId(),
                                                                orderNo,
                                                                paymentNo,
                                                                "REFUND",
                                                                refundAmount,
                                                                insertCallbackAndFinish
                                                              );
                                                          }
                                                          else
                                                          {
                                                              insertCallbackAndFinish();
                                                          }
                                                      },
                                                      respondDbError,
                                                      refundStatus,
                                                      refundId,
                                                      refundNo
                                                    );
                                                });
                                            },
                                            [cbPtr](const drogon::orm::DrogonDbException &e) {
                                                LOG_ERROR << "[CallbackService] Order lookup "
                                                             "failed during refund callback: "
                                                          << e.base().what();
                                                Json::Value error;
                                                error["code"] = "FAIL";
                                                error["message"] = "order not found";
                                                (*cbPtr)(
                                                  error,
                                                  std::error_code(1404, std::system_category())
                                                );
                                            }
                                          );
                                      }
                                      catch (const std::exception &e)
                                      {
                                          reportMapperFailure(cbPtr, e.what());
                                      }
                                      catch (...)
                                      {
                                          reportMapperFailure(cbPtr, "unknown exception");
                                      }
                                  },
                                  [cbPtr](const drogon::orm::DrogonDbException &e) {
                                      LOG_ERROR << "[CallbackService] Refund lookup failed "
                                                   "during refund callback: "
                                                << e.base().what();
                                      Json::Value error;
                                      error["code"] = "FAIL";
                                      error["message"] = "refund not found";
                                      (*cbPtr)(
                                        error, std::error_code(1404, std::system_category())
                                      );
                                  }
                                );
                            }
                            catch (const std::exception &e)
                            {
                                reportMapperFailure(cbPtr, e.what());
                            }
                            catch (...)
                            {
                                reportMapperFailure(cbPtr, "unknown exception");
                            }
                        },
                        [cbPtr, idempotencyKey](const drogon::orm::DrogonDbException &e) {
                            // Real DB failure: report FAIL so the channel retries instead
                            // of acknowledging an unprocessed callback. Duplicates no
                            // longer surface here (ON CONFLICT DO NOTHING resolves them
                            // on the success path with an empty result).
                            LOG_ERROR
                              << "[CallbackService] Refund idempotency insert failed for key: "
                              << idempotencyKey << ", error: " << e.base().what();
                            Json::Value error;
                            error["code"] = "FAIL";
                            error["message"] = "internal error";
                            (*cbPtr)(error, pay::makePayError(1400, "idempotency insert failed"));
                        },
                        idempotencyKey,
                        requestHash,
                        ownerToken,
                        expiresAt
                      );
                  }
                  catch (const std::exception &e)
                  {
                      reportMapperFailure(cbPtr, e.what());
                  }
                  catch (...)
                  {
                      reportMapperFailure(cbPtr, "unknown exception");
                  }
              }
            );
        }
        catch (const std::exception &e)
        {
            reportMapperFailure(cbPtr, e.what());
        }
        catch (...)
        {
            reportMapperFailure(cbPtr, "unknown exception");
        }
    };

    // Replay protection (P1-1): gate the DB transaction behind the nonce cache.
    // Mirrors the payment-callback path. Fail-open if Redis is unavailable.
    checkNonce(nonce, [proceedRefundDb, respond](bool firstSight) mutable {
        if (!firstSight)
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = "replay detected";
            respond(error, "replay detected");
            return;
        }
        proceedRefundDb();
    });
}

bool CallbackService::verifySignature(
  const std::string &body,
  const std::string &signature,
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &serialNo
)
{
    if (!wechatClient_)
    {
        LOG_ERROR << "WechatPayClient is null";
        return false;
    }

    std::string verifyError;
    if (!wechatClient_->verifyCallback(timestamp, nonce, body, signature, serialNo, verifyError))
    {
        LOG_WARN << "Signature verification failed: " << verifyError;
        return false;
    }

    return true;
}
