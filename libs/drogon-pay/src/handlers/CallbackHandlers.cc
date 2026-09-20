#include "CallbackHandlers.h"
#include "drogon_pay/PayPlugin.h"
#include "../channels/AlipayChannel.h"
#include "../services/CallbackService.h"
#include "../services/PaymentService.h"
#include <algorithm>
#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <sstream>
#include <unordered_map>

void WechatCallbackController::notify(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    // Extract callback parameters from request
    std::string body = std::string(req->body());
    std::string signature = std::string(req->getHeader("Wechatpay-Signature"));
    std::string timestamp = std::string(req->getHeader("Wechatpay-Timestamp"));
    std::string nonce = std::string(req->getHeader("Wechatpay-Nonce"));
    std::string serialNo = std::string(req->getHeader("Wechatpay-Serial"));

    // Get CallbackService from Plugin
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto callbackService = plugin->callbackService();

    // Route to appropriate callback handler based on event_type
    // Parse body to determine callback type
    Json::Value bodyJson;
    std::string eventType;

    // Use CharReaderBuilder instead of deprecated Reader
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    const char *str = body.c_str();
    bool parseOk = reader->parse(str, str + body.length(), &bodyJson, &errors);

    // Validate JSON and event_type before routing (C2-1 fix).
    if (!parseOk)
    {
        LOG_WARN << "[WECHAT_CALLBACK] Invalid JSON body: " << errors;
        Json::Value response;
        response["code"] = 40002;
        response["message"] = "Invalid JSON body";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    if (!bodyJson.isMember("event_type"))
    {
        LOG_WARN << "[WECHAT_CALLBACK] Missing event_type in callback body";
        Json::Value response;
        response["code"] = 40003;
        response["message"] = "Missing event_type";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    eventType = bodyJson["event_type"].asString();

    // Reject unknown event types that are neither TRANSACTION nor REFUND.
    if (eventType != "TRANSACTION.SUCCESS" && eventType.find("REFUND") == std::string::npos)
    {
        LOG_WARN << "[WECHAT_CALLBACK] Unknown event_type: " << eventType;
        Json::Value response;
        response["code"] = 40004;
        response["message"] = "Unknown event_type: " + eventType;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    // Route to payment or refund callback handler
    if (eventType.find("REFUND") != std::string::npos)
    {
        // Handle refund callback
        callbackService->handleRefundCallback(
          body,
          signature,
          timestamp,
          nonce,
          serialNo,
          [callback](const Json::Value &result, const std::error_code &error) {
              auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
              if (error)
              {
                  resp->setStatusCode(drogon::k500InternalServerError);
              }
              callback(resp);
          }
        );
    }
    else
    {
        // Handle payment callback (default)
        callbackService->handlePaymentCallback(
          body,
          signature,
          timestamp,
          nonce,
          serialNo,
          [callback](const Json::Value &result, const std::error_code &error) {
              auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
              if (error)
              {
                  resp->setStatusCode(drogon::k500InternalServerError);
              }
              callback(resp);
          }
        );
    }
}

void AlipayCallbackController::notify(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    LOG_DEBUG << "[ALIPAY_CALLBACK] Received notification";

    // Alipay callbacks arrive as x-www-form-urlencoded, not JSON.
    std::string body = std::string(req->body());

    // Parse the form data.
    std::unordered_map<std::string, std::string> params;
    std::stringstream ss(body);
    std::string pair;

    while (std::getline(ss, pair, '&'))
    {
        size_t pos = pair.find('=');
        if (pos != std::string::npos)
        {
            std::string key = pair.substr(0, pos);
            std::string value = pair.substr(pos + 1);

            // URL decode: first replace '+' with ' ' (x-www-form-urlencoded
            // convention), then decode percent-encoded sequences.
            // (C1-2 fix: '+' was previously not converted to space).
            std::replace(value.begin(), value.end(), '+', ' ');
            std::string decoded = drogon::utils::urlDecode(value);
            params[key] = decoded;
        }
    }

    // Extract signature BEFORE removing it from params (verifyCallback also
    // excludes 'sign', but we need the value here).
    const std::string sign = params.count("sign") ? params["sign"] : std::string{};

    // Get the Alipay client. If it is not configured we MUST reject the callback
    // rather than processing it unverified - accepting an unverified callback
    // would let any party forge a payment-success notification (P0-1).
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto alipayClient = plugin->alipayClient();
    if (!alipayClient)
    {
        LOG_ERROR << "[ALIPAY_CALLBACK] Alipay client not configured, rejecting callback";
        Json::Value response;
        response["code"] = "FAIL";
        response["message"] = "Alipay client not configured";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setContentTypeString("application/json");
        resp->addHeader("Content-Type", "application/json; charset=utf-8");
        callback(resp);
        return;
    }

    // Build the parameter set for signature verification. verifyCallback
    // expects a JSON object whose members (excluding 'sign'/'sign_type') are
    // sorted and joined as k=v to form the signed payload.
    Json::Value verifyParams;
    for (const auto &kv : params)
    {
        verifyParams[kv.first] = kv.second;
    }

    // Verify the callback signature BEFORE any database mutation. A failed
    // signature means the notification is not authentic and must not advance
    // any order state (P0-1: previously signature verification was never
    // invoked, allowing forged payment-success notifications).
    if (!alipayClient->verifyCallback(verifyParams, sign))
    {
        LOG_WARN << "[ALIPAY_CALLBACK] Signature verification failed, rejecting callback";
        Json::Value response;
        response["code"] = "FAIL";
        response["message"] = "signature verification failed";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setContentTypeString("application/json");
        resp->addHeader("Content-Type", "application/json; charset=utf-8");
        callback(resp);
        return;
    }
    LOG_DEBUG << "[ALIPAY_CALLBACK] Signature verified successfully";

    // Extract the key callback parameters.
    std::string outTradeNo = params["out_trade_no"];
    std::string tradeNo = params["trade_no"];
    std::string tradeStatus = params["trade_status"];
    std::string totalAmount = params["total_amount"];
    std::string appId = params["app_id"];
    std::string sellerId = params["seller_id"];
    std::string notifyTime = params["notify_time"];
    std::string notifyType = params["notify_type"];
    std::string notifyId = params["notify_id"];

    LOG_DEBUG << "[ALIPAY_CALLBACK] out_trade_no=" << outTradeNo << " trade_no=" << tradeNo
              << " trade_status=" << tradeStatus << " total_amount=" << totalAmount;

    // Merchant-identity re-check (Alipay notification mandate). The signature
    // already binds app_id, but a notification carrying a foreign app_id means
    // a mis-configured gateway or a cross-app replay and must not advance our
    // orders. Only enforced when we know our own app_id.
    const std::string expectedAppId = alipayClient->getAppId();
    if (!expectedAppId.empty() && appId != expectedAppId)
    {
        LOG_WARN << "[ALIPAY_CALLBACK] app_id mismatch, rejecting callback. expected="
                 << expectedAppId << " got=" << appId;
        Json::Value response;
        response["code"] = "FAIL";
        response["message"] = "app_id mismatch";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setContentTypeString("application/json");
        resp->addHeader("Content-Type", "application/json; charset=utf-8");
        callback(resp);
        return;
    }

    // Build a JSON result object in the shape syncOrderStatusFromAlipay expects.
    Json::Value alipayResult;
    alipayResult["code"] = "10000";  // Alipay success response code
    alipayResult["msg"] = "Success";
    alipayResult["trade_no"] = tradeNo;
    alipayResult["out_trade_no"] = outTradeNo;
    alipayResult["trade_status"] = tradeStatus;
    alipayResult["total_amount"] = totalAmount;
    alipayResult["app_id"] = appId;
    alipayResult["seller_id"] = sellerId;
    alipayResult["notify_time"] = notifyTime;
    alipayResult["notify_type"] = notifyType;
    alipayResult["notify_id"] = notifyId;

    auto paymentService = plugin->paymentService();

    // Call syncOrderStatusFromAlipay to update the database.
    paymentService->syncOrderStatusFromAlipay(
      outTradeNo, alipayResult, [callback, outTradeNo, tradeStatus](const std::string &status) {
          LOG_DEBUG << "[ALIPAY_CALLBACK] Sync completed for order " << outTradeNo
                    << " status=" << status;

          // Acknowledge the notification back to Alipay.
          Json::Value response;
          response["code"] = "SUCCESS";
          response["message"] = "OK";

          LOG_INFO << "[AlipayCallback] Callback processed successfully: order_no=" << outTradeNo
                   << ", trade_status=" << tradeStatus << ", synced_status=" << status;

          auto resp = HttpResponse::newHttpJsonResponse(response);
          resp->setContentTypeString("application/json");
          resp->addHeader("Content-Type", "application/json; charset=utf-8");

          callback(resp);
      }
    );
}
