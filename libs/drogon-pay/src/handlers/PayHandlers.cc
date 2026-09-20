#include "PayHandlers.h"
#include "drogon_pay/PayPlugin.h"
#include "../services/PaymentService.h"
#include "../services/RefundService.h"
#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <regex>
#include <stdexcept>

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

    // Get user_id from JSON body or attributes (set by auth middleware)
    if (json->isMember("user_id"))
    {
        request.userId = (*json)["user_id"].asInt64();
    }
    else
    {
        try
        {
            request.userId = req->attributes()->get<int64_t>("user_id");
        }
        catch (const std::exception &)
        {
            // For API key authentication, user_id is required
            Json::Value error;
            error["code"] = 401;
            error["message"] = "User ID required. Please provide user_id in request body.";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k401Unauthorized);
            callback(resp);
            return;
        }
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
    auto paymentService = plugin->paymentService();

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

    // Build payment request
    Json::Value request;
    request["order_no"] = (*json)["order_no"].asString();
    request["amount"] = (*json)["amount"].asString();
    request["channel"] = (*json)["channel"].asString();
    request["user_id"] = (*json)["user_id"].asInt();

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

    // Get service and call QR payment
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto paymentService = plugin->paymentService();

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
    auto paymentService = plugin->paymentService();

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
    auto refundService = plugin->refundService();

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
    auto refundService = plugin->refundService();

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
    auto paymentService = plugin->paymentService();

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
    auto paymentService = plugin->paymentService();

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
