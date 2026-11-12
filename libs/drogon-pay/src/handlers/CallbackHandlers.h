#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

using namespace drogon;

class AlipaySandboxClient;
class PaymentService;

/// Plain handler classes held by PayPlugin; routes {base_path}/notify/wechat
/// and {base_path}/notify/alipay are registered programmatically in
/// PayPlugin::initAndStart (see PayHandlers.h for the rationale).
class WechatCallbackController
{
  public:
    void notify(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
};

class AlipayCallbackController
{
  public:
    void notify(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
};

/// Verify + book an already-parsed Alipay notification and acknowledge it.
/// Split out of AlipayCallbackController::notify so the protocol contract
/// (signature, app_id/seller_id re-check, plain-text "success" ack) can be
/// exercised with a locally-built client/service, without the frozen global
/// ChannelRegistry. Both pointers must be non-null (the caller guards them).
void processAlipayNotify(
  const std::unordered_map<std::string, std::string> &params,
  const std::shared_ptr<AlipaySandboxClient> &alipayClient,
  const std::shared_ptr<PaymentService> &paymentService,
  const std::function<void(const HttpResponsePtr &)> &callback
);
