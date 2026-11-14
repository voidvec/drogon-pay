#include "IdempotencyService.h"
#include "../models/PayIdempotency.h"
#include "../utils/OnceCallback.h"
#include "../utils/PayUtils.h"
#include <drogon/drogon.h>
#include <openssl/sha.h>
#include <sstream>

using namespace drogon;

namespace
{
using PayIdempotencyModel = drogon_model::pay_test::PayIdempotency;
}  // namespace

IdempotencyService::IdempotencyService(
  std::shared_ptr<orm::DbClient> dbClient,
  nosql::RedisClientPtr redisClient,
  int64_t ttlSeconds
)
    : dbClient_(dbClient), redisClient_(redisClient), ttlSeconds_(ttlSeconds)
{
}

void IdempotencyService::checkAndSet(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &request,
  CheckCallback &&callback
)
{
    auto legacyCb =
      pay::utils::makeOnceCallback<void(bool, const Json::Value &)>(std::move(callback));
    checkAndSetStatus(
      idempotencyKey, requestHash, request, [legacyCb](const CheckResult &result) mutable {
          switch (result.status)
          {
              case CheckStatus::Started:
                  legacyCb.call(true, Json::Value());
                  return;
              case CheckStatus::Replay:
                  legacyCb.call(true, result.cachedResult);
                  return;
              case CheckStatus::InProgress:
              case CheckStatus::Conflict:
              case CheckStatus::Error:
                  legacyCb.call(false, Json::Value());
                  return;
          }
      }
    );
}

void IdempotencyService::checkAndSetStatus(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &request,
  StatusCallback &&callback
)
{
    auto onceCb = pay::utils::makeOnceCallback<void(const CheckResult &)>(std::move(callback));
    auto sharedCb = std::make_shared<pay::utils::OnceCallback<void(const CheckResult &)>>(onceCb);
    (void)request;

    // DB is the source of truth. Redis read-through is intentionally disabled
    // until it can share the same Started/Replay/InProgress/Conflict contract.
    checkDatabase(idempotencyKey, requestHash, [sharedCb](const CheckResult &result) {
        sharedCb->call(result);
    });
}

void IdempotencyService::checkDatabase(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  StatusCallback &&callback
)
{
    if (idempotencyKey.empty())
    {
        CheckResult result;
        result.status = CheckStatus::Started;
        callback(result);
        return;
    }

    auto onceCb = pay::utils::makeOnceCallback<void(const CheckResult &)>(std::move(callback));
    auto sharedCb = std::make_shared<pay::utils::OnceCallback<void(const CheckResult &)>>(onceCb);
    auto dbClient = dbClient_;
    const auto ttlSeconds = ttlSeconds_;
    const auto now = trantor::Date::now();
    const auto expiresAt =
      trantor::Date(now.microSecondsSinceEpoch() + ttlSeconds * static_cast<int64_t>(1000000));

    // Atomic reserve-or-reclaim (raw-SQL exemption: INSERT ... ON CONFLICT, and
    // server-side NOW() shared with purgeExpired). A fresh key inserts; a key
    // whose reservation has expired is reclaimed in the same statement (reset
    // hash, NULL snapshot, new TTL) so a retry gets Started instead of falling
    // through to the TTL-filtered findBy, finding nothing, and reporting a
    // permanent "key disappeared" Error until the purge sweep runs. A conflict
    // on a still-live row (expire_at NULL = never expires, or >= NOW()) updates
    // zero rows and falls through to findBy for InProgress/Replay/Conflict. The
    // Mapper cannot express ON CONFLICT ... DO UPDATE ... WHERE.
    dbClient->execSqlAsync(
      "INSERT INTO pay_idempotency (idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, NULL, $3) "
      "ON CONFLICT (idempotency_key) DO UPDATE "
      "SET request_hash = EXCLUDED.request_hash, response_snapshot = NULL, "
      "expire_at = EXCLUDED.expire_at "
      "WHERE pay_idempotency.expire_at IS NOT NULL AND pay_idempotency.expire_at < NOW()",
      [dbClient, idempotencyKey, requestHash, sharedCb](const orm::Result &insertResult) {
          if (insertResult.affectedRows() > 0)
          {
              LOG_DEBUG << "[IdempotencyService] Reserved idempotency key=" << idempotencyKey;
              CheckResult checkResult;
              checkResult.status = CheckStatus::Started;
              sharedCb->call(checkResult);
              return;
          }

          try
          {
              // TTL filter uses client-side now() instead of DB-side NOW();
              // second-level clock skew between app and DB is acceptable for
              // the idempotency TTL (second-granularity expiry).
              orm::Mapper<PayIdempotencyModel> idempMapper(dbClient);
              idempMapper.findBy(
                orm::Criteria(
                  PayIdempotencyModel::Cols::_idempotency_key,
                  orm::CompareOperator::EQ,
                  idempotencyKey
                ) &&
                  (orm::Criteria(
                     PayIdempotencyModel::Cols::_expire_at, orm::CompareOperator::IsNull
                   ) ||
                   orm::Criteria(
                     PayIdempotencyModel::Cols::_expire_at,
                     orm::CompareOperator::GT,
                     trantor::Date::now()
                   )),
                [idempotencyKey,
                 requestHash,
                 sharedCb](const std::vector<PayIdempotencyModel> &rows) {
                    if (!rows.empty())
                    {
                        const std::string cachedHash = rows.front().getValueOfRequestHash();

                        if (cachedHash == requestHash)
                        {
                            if (!rows.front().getResponseSnapshot())
                            {
                                LOG_DEBUG
                                  << "[IdempotencyService] Idempotency key in progress: key="
                                  << idempotencyKey;
                                CheckResult checkResult;
                                checkResult.status = CheckStatus::InProgress;
                                checkResult.message = "idempotency request is already in progress";
                                sharedCb->call(checkResult);
                                return;
                            }

                            // Same request - backfill Redis and return
                            Json::Value snapshot;
                            Json::CharReaderBuilder builder;
                            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                            std::string errors;
                            const std::string &snapshotStr =
                              rows.front().getValueOfResponseSnapshot();
                            LOG_DEBUG
                              << "[IdempotencyService] Loading from DB: key=" << idempotencyKey
                              << ", snapshot length=" << snapshotStr.size();
                            bool parseSuccess = reader->parse(
                              snapshotStr.data(),
                              snapshotStr.data() + snapshotStr.size(),
                              &snapshot,
                              &errors
                            );
                            Json::Value response = snapshot["response"];
                            LOG_DEBUG
                              << "[IdempotencyService] Parsed from DB: success=" << parseSuccess
                              << ", has_response=" << snapshot.isMember("response")
                              << ", has_data=" << response.isMember("data")
                              << ", isNull=" << response.isNull()
                              << ", members=" << response.getMemberNames().size()
                              << ", errors=" << errors;

                            if (!parseSuccess || response.isNull())
                            {
                                CheckResult checkResult;
                                checkResult.status = CheckStatus::InProgress;
                                checkResult.message = "idempotency response is not ready";
                                sharedCb->call(checkResult);
                                return;
                            }

                            LOG_DEBUG
                              << "[IdempotencyService] Idempotency hit: key=" << idempotencyKey
                              << ", returning cached response, has_data="
                              << response.isMember("data");
                            CheckResult checkResult;
                            checkResult.status = CheckStatus::Replay;
                            checkResult.cachedResult = response;
                            sharedCb->call(checkResult);
                        }
                        else
                        {
                            // Different request - conflict
                            LOG_DEBUG
                              << "[IdempotencyService] Idempotency conflict: key=" << idempotencyKey
                              << ", cached_hash=" << cachedHash.substr(0, 8) << "..."
                              << ", request_hash=" << requestHash.substr(0, 8) << "...";
                            CheckResult checkResult;
                            checkResult.status = CheckStatus::Conflict;
                            checkResult.message = "idempotency key conflict";
                            sharedCb->call(checkResult);
                        }
                        return;
                    }

                    CheckResult checkResult;
                    checkResult.status = CheckStatus::Error;
                    checkResult.message = "idempotency key disappeared after conflict";
                    sharedCb->call(checkResult);
                },
                [sharedCb](const orm::DrogonDbException &e) {
                    LOG_ERROR << "Idempotency DB select error: " << e.base().what();
                    CheckResult checkResult;
                    checkResult.status = CheckStatus::Error;
                    checkResult.message = e.base().what();
                    sharedCb->call(checkResult);
                }
              );
          }
          catch (const std::exception &e)
          {
              LOG_ERROR << "[IdempotencyService] Mapper construction failed: " << e.what();
              CheckResult checkResult;
              checkResult.status = CheckStatus::Error;
              checkResult.message = e.what();
              sharedCb->call(checkResult);
          }
          catch (...)
          {
              LOG_ERROR << "[IdempotencyService] Mapper construction failed: unknown exception";
              CheckResult checkResult;
              checkResult.status = CheckStatus::Error;
              checkResult.message = "mapper construction failed";
              sharedCb->call(checkResult);
          }
      },
      [sharedCb](const orm::DrogonDbException &e) {
          // On database error, fail the idempotency check
          LOG_ERROR << "Idempotency DB reserve error: " << e.base().what();
          CheckResult checkResult;
          checkResult.status = CheckStatus::Error;
          checkResult.message = e.base().what();
          sharedCb->call(checkResult);
      },
      idempotencyKey,
      requestHash,
      expiresAt
    );
}

void IdempotencyService::updateResult(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &response,
  UpdateCallback &&callback,
  const std::shared_ptr<drogon::orm::DbClient> &transPtr
)
{
    auto onceCb = pay::utils::makeOnceCallback<void(bool)>(std::move(callback));
    auto sharedCb = std::make_shared<pay::utils::OnceCallback<void(bool)>>(onceCb);
    auto dbClient = transPtr ? transPtr : dbClient_;
    auto redisClient = redisClient_;
    const auto ttlSeconds = ttlSeconds_;

    // Build cache structure for Redis
    Json::Value cached;
    cached["request_hash"] = requestHash;
    cached["response"] = response;
    std::string cacheStr = pay::utils::toJsonString(cached);

    LOG_DEBUG << "[IdempotencyService] Saving to DB: key=" << idempotencyKey
              << ", hash=" << requestHash.substr(0, 8)
              << "..., has_response_data=" << response.isMember("data");

    // Conditional UPDATE: only fills snapshot when key+hash match
    try
    {
        orm::Mapper<PayIdempotencyModel> idempMapper(dbClient);
        idempMapper.updateBy(
          {PayIdempotencyModel::Cols::_response_snapshot},
          [redisClient, ttlSeconds, idempotencyKey, cacheStr, sharedCb](const size_t rows) {
              if (rows == 0)
              {
                  LOG_ERROR << "Idempotency update skipped: key/hash mismatch for key="
                            << idempotencyKey;
                  sharedCb->call(false);
                  return;
              }

              // Update Redis cache if available (skipped inside a transaction)
              if (redisClient)
              {
                  std::string redisKey = "idempotency:" + idempotencyKey;
                  redisClient->execCommandAsync(
                    [sharedCb](const nosql::RedisResult &) { sharedCb->call(true); },
                    [sharedCb](const nosql::RedisException &) {
                        // Ignore Redis errors - DB is source of truth
                        sharedCb->call(true);
                    },
                    "SETEX %s %d %s",
                    redisKey.c_str(),
                    ttlSeconds,
                    cacheStr.c_str()
                  );
              }
              else
              {
                  sharedCb->call(true);
              }
          },
          [sharedCb](const orm::DrogonDbException &e) {
              LOG_ERROR << "Idempotency DB upsert error: " << e.base().what();
              sharedCb->call(false);
          },
          orm::Criteria(
            PayIdempotencyModel::Cols::_idempotency_key, orm::CompareOperator::EQ, idempotencyKey
          ) &&
            orm::Criteria(
              PayIdempotencyModel::Cols::_request_hash, orm::CompareOperator::EQ, requestHash
            ),
          cacheStr
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[IdempotencyService] Mapper construction failed: " << e.what();
        sharedCb->call(false);
    }
    catch (...)
    {
        LOG_ERROR << "[IdempotencyService] Mapper construction failed: unknown exception";
        sharedCb->call(false);
    }
}

void IdempotencyService::clearReservation(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  UpdateCallback &&callback
)
{
    auto onceCb = pay::utils::makeOnceCallback<void(bool)>(std::move(callback));
    auto sharedCb = std::make_shared<pay::utils::OnceCallback<void(bool)>>(onceCb);

    if (idempotencyKey.empty())
    {
        sharedCb->call(false);
        return;
    }

    auto dbClient = dbClient_;

    try
    {
        orm::Mapper<PayIdempotencyModel> idempMapper(dbClient);
        idempMapper.deleteBy(
          orm::Criteria(
            PayIdempotencyModel::Cols::_idempotency_key, orm::CompareOperator::EQ, idempotencyKey
          ) &&
            orm::Criteria(
              PayIdempotencyModel::Cols::_request_hash, orm::CompareOperator::EQ, requestHash
            ) &&
            orm::Criteria(
              PayIdempotencyModel::Cols::_response_snapshot, orm::CompareOperator::IsNull
            ),
          [idempotencyKey, sharedCb](const size_t rows) {
              LOG_DEBUG << "[IdempotencyService] Cleared in-flight reservation key="
                        << idempotencyKey << " rows=" << rows;
              sharedCb->call(true);
          },
          [sharedCb](const orm::DrogonDbException &e) {
              LOG_ERROR << "Idempotency clearReservation error: " << e.base().what();
              sharedCb->call(false);
          }
        );
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "[IdempotencyService] Mapper construction failed: " << e.what();
        sharedCb->call(false);
    }
    catch (...)
    {
        LOG_ERROR << "[IdempotencyService] Mapper construction failed: unknown exception";
        sharedCb->call(false);
    }
}

void IdempotencyService::purgeExpired(UpdateCallback &&callback)
{
    auto onceCb = pay::utils::makeOnceCallback<void(bool)>(std::move(callback));
    auto sharedCb = std::make_shared<pay::utils::OnceCallback<void(bool)>>(onceCb);
    auto dbClient = dbClient_;

    // Batch GC (raw-SQL exemption #3): server-side NOW() comparison over the
    // full table; Mapper deleteBy would depend on the client-side clock.
    dbClient->execSqlAsync(
      "DELETE FROM pay_idempotency WHERE expire_at IS NOT NULL AND expire_at < NOW()",
      [sharedCb](const orm::Result &result) {
          LOG_INFO << "[IdempotencyService] Purged expired rows: " << result.affectedRows();
          sharedCb->call(true);
      },
      [sharedCb](const orm::DrogonDbException &e) {
          LOG_ERROR << "[IdempotencyService] purgeExpired error: " << e.base().what();
          sharedCb->call(false);
      }
    );
}
