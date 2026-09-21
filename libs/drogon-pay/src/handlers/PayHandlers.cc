#include "PayHandlers.h"
#include "drogon_pay/PayPlugin.h"
#include "../services/PaymentService.h"
#include "../services/RefundService.h"
#include "PluginGuard.h"
#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
// Helper function to map error codes to HTTP status codes
drogon::HttpStatusCode mapErrorToHttpStatus(int errorCode)
{
    switch (errorCode)
    {
        case 1404:  // Not found
        case 1004:  // Order/refund not found
            return drogon::k404NotFound;
        case 1001:  // Invalid input
        case 1400:  // Bad request
        case 400:   // Bad request reported with its transport-status number
            return drogon::k400BadRequest;
        case 1409:  // Idempotency conflict
            return drogon::k409Conflict;
        case 1501:  // Channel client not ready (our dependency/config fault)
            return drogon::k503ServiceUnavailable;
        case 1502:  // Upstream channel returned a business failure (not our fault)
            return drogon::k502BadGateway;
        case 1002:  // Payment gateway error
        case 1003:  // Database error
        default:
            return drogon::k500InternalServerError;
    }
}

// Validate amount string format: positive decimal with up to 2 fractional digits.
// Accepts: "100", "0.5", "99.99". Rejects: "-100", "12.345", "abc", "1e5".
// (A1-6/B1-2 fix: controller-level input validation)
static bool validateAmount(const std::string &amount)
{
    static const std::regex pattern(R"(^\d+(\.\d{1,2})?$)");
    return !amount.empty() && std::regex_match(amount, pattern);
}

// The JSON type a request-body field has to carry before it can be read.
enum class FieldType
{
    String,
    Int64
};

struct BodyField
{
    const char *key;
    FieldType type;
    bool optional = false;
};

// Check that every field the handler is about to read exists in the shape it
// expects. jsoncpp throws when a member of the wrong type is converted (an
// object where a string was read), and an exception escaping a handler is
// rethrown out of the event loop by trantor, which stops the loop and unwinds
// `app().run()` -- a single anonymous `{"order_no":"a","amount":{}}` body would
// have taken the whole gateway down. Validating the shape first turns that
// request into a 400 answer instead.
static bool validateBodyTypes(
  const Json::Value &json,
  const std::vector<BodyField> &fields,
  std::string &error
)
{
    for (const auto &field : fields)
    {
        if (!json.isMember(field.key))
        {
            if (field.optional)
            {
                continue;
            }
            error = std::string("Field ") + field.key + " is required";
            return false;
        }
        const Json::Value &value = json[field.key];
        bool matches = false;
        const char *expected = "a string";
        switch (field.type)
        {
            case FieldType::String:
                matches = value.isString();
                break;
            case FieldType::Int64:
                matches = value.isInt64();
                expected = "an integer";
                break;
        }
        if (!matches)
        {
            error = std::string("Field ") + field.key + " must be " + expected;
            return false;
        }
    }
    return true;
}

// Answer the standard 400 envelope for a body that failed shape validation.
static void respondBadRequest(
  const std::function<void(const drogon::HttpResponsePtr &)> &callback,
  const std::string &message
)
{
    Json::Value error;
    error["code"] = 400;
    error["message"] = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
    resp->setStatusCode(drogon::k400BadRequest);
    callback(resp);
}

}  // namespace

void PayController::createPayment(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Extract and validate JSON
    auto json = req->getJsonObject();
    if (!json)
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Invalid JSON";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Extract required fields
    if (!json->isMember("order_no") || !json->isMember("amount"))
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Missing required fields: order_no and amount";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Check the shape of every field read below before reading it; absence of a
    // required field was answered above, so these are all about the type.
    {
        std::string shapeError;
        if (!validateBodyTypes(
              *json,
              {
                {"order_no", FieldType::String, true},
                {"amount", FieldType::String, true},
                {"currency", FieldType::String, true},
                {"description", FieldType::String, true},
                {"notify_url", FieldType::String, true},
                {"channel", FieldType::String, true},
                {"user_id", FieldType::Int64, true},
                {"time_expire", FieldType::String, true},
              },
              shapeError
            ))
        {
            respondBadRequest(callback, shapeError);
            return;
        }
    }

    // Validate amount format (A1-6/B1-2 fix)
    const std::string amountStr = (*json)["amount"].asString();
    if (!validateAmount(amountStr))
    {
        Json::Value error;
        error["code"] = 40001;
        error["message"] =
          "Invalid amount format. Expected positive number with up to 2 decimal places (e.g. "
          "100.00)";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Build request
    CreatePaymentRequest request;
    request.orderNo = (*json)["order_no"].asString();
    request.amount = amountStr;
    request.currency = json->get("currency", "CNY").asString();
    request.description = json->get("description", "").asString();
    request.notifyUrl = json->get("notify_url", "").asString();
    request.channel = json->get("channel", "alipay").asString();  // Default to alipay
    request.timeExpire = json->get("time_expire", "").asString();

    // Get user_id from JSON body or attributes (set by auth middleware).
    // `Attributes::get` never throws: a missing key and a key stored under
    // another type both read back as 0, so the presence test is `find()` and the
    // owner has to be a positive id -- `queryOrderList` reads 0 as "no owner
    // filter", so an order booked under it has no owner: only the unfiltered
    // listing ever shows it, and no owner-scoped query can name it.
    if (json->isMember("user_id"))
    {
        request.userId = (*json)["user_id"].asInt64();
    }
    else if (req->attributes()->find("user_id"))
    {
        request.userId = req->attributes()->get<int64_t>("user_id");
    }

    if (request.userId <= 0)
    {
        Json::Value error;
        error["code"] = 401;
        error["message"] = "User ID required. Please provide user_id in request body.";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    // Extract scene info if present
    if (json->isMember("scene_info"))
    {
        request.sceneInfo = (*json)["scene_info"];
    }

    // Get or generate idempotency key
    std::string idempotencyKey = req->getHeader("X-Idempotency-Key");
    if (idempotencyKey.empty())
    {
        idempotencyKey = req->getHeader("Idempotency-Key");
    }
    if (idempotencyKey.empty())
    {
        // Generate standardized idempotency key
        // Use order_no as the base to ensure same order always gets same key
        // Include user_id and amount for additional uniqueness
        idempotencyKey = "payment:" + request.orderNo + ":" + std::to_string(request.userId) + ":" +
                         drogon::utils::getSha256(request.amount + request.currency);
    }

    // Get service and call
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto paymentService = plugin ? plugin->paymentService() : nullptr;
    if (!paymentService)
    {
        respondPluginUnavailable(callback, "Payment service");
        return;
    }

    paymentService->createPayment(
      request, idempotencyKey, [callback](const Json::Value &result, const std::error_code &error) {
          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      }
    );
}

void PayController::createQRPayment(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Parse request body
    auto json = req->getJsonObject();
    if (!json)
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Invalid JSON";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Validate required fields
    if (
      !json->isMember("order_no") || !json->isMember("amount") || !json->isMember("channel") ||
      !json->isMember("user_id")
    )
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Missing required fields: order_no, amount, channel, user_id";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Check the shape of every field read below. The required-field answer above
    // already covers absence; this covers a field of the wrong JSON type, which
    // jsoncpp reports by throwing.
    {
        std::string shapeError;
        if (!validateBodyTypes(
              *json,
              {
                {"order_no", FieldType::String, true},
                {"amount", FieldType::String, true},
                {"channel", FieldType::String, true},
                {"user_id", FieldType::Int64, true},
                {"description", FieldType::String, true},
                {"product_name", FieldType::String, true},
                {"currency", FieldType::String, true},
                {"notify_url", FieldType::String, true},
                {"buyer_id", FieldType::String, true},
                {"idempotency_key", FieldType::String, true},
                {"time_expire", FieldType::String, true},
              },
              shapeError
            ))
        {
            respondBadRequest(callback, shapeError);
            return;
        }
    }

    // Validate amount format (A1-6/B1-2 fix). Without this the QR route accepted
    // anything the caller typed: WeChat's fen conversion rejects the bad shapes on
    // its own, but the Alipay branch forwards `total_amount` to the channel
    // verbatim and books the same string on the order row, so an unrepresentable
    // amount became a permanently un-reusable order.
    const std::string amountStr = (*json)["amount"].asString();
    if (!validateAmount(amountStr))
    {
        Json::Value error;
        error["code"] = 40001;
        error["message"] =
          "Invalid amount format. Expected positive number with up to 2 decimal places (e.g. "
          "100.00)";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // The order needs a positive owner: 0 is exactly what `queryOrderList` reads
    // as "no owner filter", so a non-positive id books money against an owner no
    // owner-scoped query can ever name.
    const int64_t qrUserId = (*json)["user_id"].asInt64();
    if (qrUserId <= 0)
    {
        respondBadRequest(callback, "Field user_id must be a positive integer");
        return;
    }

    // Build payment request
    Json::Value request;
    request["order_no"] = (*json)["order_no"].asString();
    request["amount"] = amountStr;
    request["channel"] = (*json)["channel"].asString();
    request["user_id"] = static_cast<Json::Int64>(qrUserId);

    if (json->isMember("description"))
    {
        request["description"] = (*json)["description"].asString();
    }
    else
    {
        request["description"] = "";
    }

    if (json->isMember("product_name"))
    {
        request["subject"] = (*json)["product_name"].asString();
    }
    else
    {
        request["subject"] = "Payment";
    }

    // Hand over the fields the QR service reads. Rebuilding the request from
    // scratch used to drop all five of them, so every QR order was priced in CNY
    // (`currency`), bound to the globally configured callback URL (`notify_url`),
    // never scoped to a `buyer_id`, replay-guarded only by the derived
    // "QR_<order_no>_<channel>" key -- the documented `X-Idempotency-Key` header
    // had no effect on this route -- and never carried the caller's
    // `time_expire`, which left the order without an `expire_at` the close
    // sweep could ever honour.
    for (const char *field :
         {"currency", "notify_url", "buyer_id", "idempotency_key", "time_expire"})
    {
        if (json->isMember(field))
        {
            request[field] = (*json)[field];
        }
    }
    if (request.get("idempotency_key", "").asString().empty())
    {
        std::string headerKey(req->getHeader("X-Idempotency-Key"));
        if (headerKey.empty())
        {
            headerKey = std::string(req->getHeader("Idempotency-Key"));
        }
        if (!headerKey.empty())
        {
            request["idempotency_key"] = headerKey;
        }
    }

    // Get service and call QR payment
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto paymentService = plugin ? plugin->paymentService() : nullptr;
    if (!paymentService)
    {
        respondPluginUnavailable(callback, "Payment service");
        return;
    }

    paymentService->createQRPayment(
      request, [callback](const Json::Value &result, const std::error_code &error) {
          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      }
    );
}

void PayController::queryOrder(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Get order_no from query parameter
    std::string orderNo = req->getParameter("order_no");
    LOG_DEBUG << "[PAY_CONTROLLER] queryOrder called with order_no=" << orderNo;

    if (orderNo.empty())
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Missing required parameter: order_no";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Get service and call
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto paymentService = plugin ? plugin->paymentService() : nullptr;
    if (!paymentService)
    {
        respondPluginUnavailable(callback, "Payment service");
        return;
    }

    paymentService->queryOrder(
      orderNo, [callback, orderNo](const Json::Value &result, const std::error_code &error) {
          LOG_DEBUG << "[PAY_CONTROLLER] queryOrder response for " << orderNo
                    << " - code=" << result.get("code", "?").asString();

          // Safely access status field
          if (result.isMember("data") && result["data"].isMember("status"))
          {
              const auto &status = result["data"]["status"];
              if (status.isString())
              {
                  LOG_DEBUG << " status=" << status.asString();
              }
              else
              {
                  LOG_DEBUG << " status=<non-string type>";
              }
          }
          else
          {
              LOG_DEBUG << " status=<not found>";
          }

          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      }
    );
}

void PayController::refund(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Extract and validate JSON
    auto json = req->getJsonObject();
    if (!json)
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Invalid JSON";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Extract required fields
    if (!json->isMember("order_no") || !json->isMember("amount"))
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Missing required fields: order_no and amount";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    {
        std::string shapeError;
        if (!validateBodyTypes(
              *json,
              {
                {"order_no", FieldType::String, true},
                {"amount", FieldType::String, true},
                {"reason", FieldType::String, true},
                {"notify_url", FieldType::String, true},
                {"funds_account", FieldType::String, true},
                {"payment_no", FieldType::String, true},
                {"refund_no", FieldType::String, true},
              },
              shapeError
            ))
        {
            respondBadRequest(callback, shapeError);
            return;
        }
    }

    // Validate amount format (A1-6/B1-2 fix)
    const std::string refundAmountStr = (*json)["amount"].asString();
    if (!validateAmount(refundAmountStr))
    {
        Json::Value error;
        error["code"] = 40001;
        error["message"] =
          "Invalid amount format. Expected positive number with up to 2 decimal places (e.g. "
          "100.00)";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Build request
    CreateRefundRequest request;
    request.orderNo = (*json)["order_no"].asString();
    request.amount = refundAmountStr;
    request.reason = json->get("reason", "").asString();
    request.notifyUrl = json->get("notify_url", "").asString();
    request.fundsAccount = json->get("funds_account", "").asString();

    // Optional fields
    if (json->isMember("payment_no"))
    {
        request.paymentNo = (*json)["payment_no"].asString();
    }
    if (json->isMember("refund_no"))
    {
        request.refundNo = (*json)["refund_no"].asString();
    }

    // Get or generate idempotency key
    std::string idempotencyKey = req->getHeader("X-Idempotency-Key");
    if (idempotencyKey.empty())
    {
        idempotencyKey = req->getHeader("Idempotency-Key");
    }
    if (idempotencyKey.empty())
    {
        // Generate standardized idempotency key
        // Use order_no and amount as the base to ensure same refund request gets same key
        idempotencyKey = "refund:" + request.orderNo + ":" +
                         drogon::utils::getSha256(request.amount + request.reason);
    }

    // Get service and call
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto refundService = plugin ? plugin->refundService() : nullptr;
    if (!refundService)
    {
        respondPluginUnavailable(callback, "Refund service");
        return;
    }

    refundService->createRefund(
      request, idempotencyKey, [callback](const Json::Value &result, const std::error_code &error) {
          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      }
    );
}

void PayController::queryRefund(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Get refund_no from query parameter
    std::string refundNo = req->getParameter("refund_no");
    if (refundNo.empty())
    {
        Json::Value error;
        error["code"] = 400;
        error["message"] = "Missing required parameter: refund_no";
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Get service and call
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto refundService = plugin ? plugin->refundService() : nullptr;
    if (!refundService)
    {
        respondPluginUnavailable(callback, "Refund service");
        return;
    }

    refundService
      ->queryRefund(refundNo, [callback](const Json::Value &result, const std::error_code &error) {
          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              // Use comprehensive error code mapping
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      });
}

void PayController::queryOrderList(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Get query parameters
    std::string status = req->getParameter("status");
    std::string userIdStr = req->getParameter("user_id");
    std::string limitStr = req->getParameter("limit");
    std::string offsetStr = req->getParameter("offset");

    // Parse parameters with defaults
    int64_t userId = 0;  // 0 means no filter
    if (!userIdStr.empty())
    {
        try
        {
            userId = std::stoll(userIdStr);
        }
        catch (const std::exception &)
        {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid user_id parameter";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }
    }

    size_t limit = 50;  // Default limit
    if (!limitStr.empty())
    {
        try
        {
            limit = std::stoul(limitStr);
            if (limit > 100)
                limit = 100;  // Max limit
        }
        catch (const std::exception &)
        {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid limit parameter";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }
    }

    size_t offset = 0;  // Default offset
    if (!offsetStr.empty())
    {
        try
        {
            offset = std::stoul(offsetStr);
        }
        catch (const std::exception &)
        {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid offset parameter";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }
    }

    LOG_DEBUG << "[PAY_CONTROLLER] queryOrderList called with status=" << status
              << ", userId=" << userId << ", limit=" << limit << ", offset=" << offset;

    // Get service and call
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto paymentService = plugin ? plugin->paymentService() : nullptr;
    if (!paymentService)
    {
        respondPluginUnavailable(callback, "Payment service");
        return;
    }

    paymentService->queryOrderList(
      status,
      userId,
      limit,
      offset,
      [callback](const Json::Value &result, const std::error_code &error) {
          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      }
    );
}

void PayController::reconcileSummary(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Get date from query parameter (default to today)
    std::string date = req->getParameter("date");
    if (date.empty())
    {
        // Use today's date in YYYY-MM-DD format
        auto now = trantor::Date::now();
        date = now.toCustomFormattedString("%Y-%m-%d", false);
    }

    // Get service and call
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto paymentService = plugin ? plugin->paymentService() : nullptr;
    if (!paymentService)
    {
        respondPluginUnavailable(callback, "Payment service");
        return;
    }

    paymentService
      ->reconcileSummary(date, [callback](const Json::Value &result, const std::error_code &error) {
          auto resp = HttpResponse::newHttpJsonResponse(result);
          if (error)
          {
              resp->setStatusCode(mapErrorToHttpStatus(error.value()));
          }
          callback(resp);
      });
}
