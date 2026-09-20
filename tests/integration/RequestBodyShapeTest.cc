/// =============================================================================
/// @file   RequestBodyShapeTest.cc
/// @brief  Handler-level guards for request bodies whose members carry the
///         wrong JSON type (the remote-liveness cases).
///
///         jsoncpp converts a member of an unexpected type by throwing, and an
///         exception that escapes a handler is caught by trantor's event loop
///         only to be rethrown once the loop unwinds -- which stops the loop and
///         unwinds `app().run()`, taking the process with it. Before the shape
///         checks every POST route below answered a body such as
///         `{"order_no":"a","amount":{}}` by dying instead of by answering. The
///         callback route needs no credential at all, so that body came from the
///         internet; the write routes sit behind an API key, so there the same
///         fault was one leaked key away from taking the gateway down.
///
///         These cases call the controllers directly, like
///         CallbackControllerTest.cc does: every body here but one is refused
///         during validation, so no plugin, database or channel is reached. The
///         exception is the int64-owner case, which asserts a legitimate tenant
///         id gets *past* the guard -- it then goes wherever the service makes it
///         go, and the assertion only cares that the refusal is not the one the
///         32-bit gate used to answer.
/// =============================================================================

#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <json/json.h>
#include <chrono>
#include <future>
#include <string>
#include <utility>

#include "handlers/CallbackHandlers.h"
#include "handlers/PayHandlers.h"

namespace
{
struct Answer
{
    drogon::HttpStatusCode status{drogon::k500InternalServerError};
    Json::Value body;
    bool answered{false};
    std::string fault;
};

std::string toJsonText(const Json::Value &json)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

using Handler = std::function<
  void(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&)>;

// Runs one handler against a crafted body and reports how it got home. `fault`
// names the escape that would otherwise have killed the process: an exception
// leaving the handler, or no answer at all.
Answer offer(const Handler &handler, const Json::Value &body)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(toJsonText(body));

    auto answered = std::make_shared<std::promise<Answer>>();
    auto future = answered->get_future();
    try
    {
        handler(req, [answered](const drogon::HttpResponsePtr &resp) {
            Answer answer;
            answer.answered = true;
            answer.status = resp->getStatusCode();
            if (auto json = resp->getJsonObject())
            {
                answer.body = *json;
            }
            answered->set_value(answer);
        });
    }
    catch (const std::exception &e)
    {
        Answer answer;
        answer.fault = std::string("handler threw: ") + e.what();
        return answer;
    }
    catch (...)
    {
        Answer answer;
        answer.fault = "handler threw an unknown exception";
        return answer;
    }
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        Answer answer;
        answer.fault = "handler never answered";
        return answer;
    }
    return future.get();
}

// Whether the handler refused the body the way a shape guard should: with an
// answer (never an escaping exception) carrying HTTP 400 and that body code.
bool refusedWith(const Answer &answer, int code)
{
    return answer.answered && answer.fault.empty() && answer.status == drogon::k400BadRequest &&
           answer.body.get("code", -1).asInt() == code;
}

Json::Value qrBody(const Json::Value &amount, const Json::Value &userId)
{
    Json::Value body;
    body["order_no"] = "ord_shape_" + drogon::utils::getUuid();
    body["amount"] = amount;
    body["channel"] = "wechat";
    body["user_id"] = userId;
    return body;
}

Json::Value payBody(const Json::Value &amount, const Json::Value &userId)
{
    Json::Value body;
    body["order_no"] = "ord_shape_" + drogon::utils::getUuid();
    body["amount"] = amount;
    if (!userId.isNull())
    {
        body["user_id"] = userId;
    }
    return body;
}
}  // namespace

// =============================================================================
// `/api/pay/create`
// =============================================================================

DROGON_TEST(PayHandlers_CreatePayment_AmountObject_Answers400InsteadOfThrowing)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createPayment(req, std::move(cb)); };
    const auto answer = offer(handler, payBody(Json::Value(Json::objectValue), Json::Value(7)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

DROGON_TEST(PayHandlers_CreatePayment_AmountArray_Answers400InsteadOfThrowing)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createPayment(req, std::move(cb)); };
    const auto answer = offer(handler, payBody(Json::Value(Json::arrayValue), Json::Value(7)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

// `user_id` used to be read with `asInt64()` as soon as it was present, so a
// quoted number threw on the way to the database.
DROGON_TEST(PayHandlers_CreatePayment_UserIdString_Answers400InsteadOfThrowing)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createPayment(req, std::move(cb)); };
    const auto answer = offer(handler, payBody(Json::Value("1.00"), Json::Value("7")));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

// A body with no `user_id` at all is the documented 401, and it doubles as the
// positive control for the guards above: the answer comes from after the shape
// validation, so a well-shaped body is shown to pass it. The branch used to be
// unreachable -- `Attributes::get` returns 0 for a key nobody stored rather than
// throwing, so the order was booked under `user_id = 0`, the very value
// `queryOrderList` reads as "no owner filter".
DROGON_TEST(PayHandlers_CreatePayment_MissingUserId_StillAnswers401)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createPayment(req, std::move(cb)); };
    const auto answer = offer(handler, payBody(Json::Value("1.00"), Json::Value()));
    CHECK(answer.answered);
    CHECK(answer.status == drogon::k401Unauthorized);
    CHECK(answer.body.get("code", -1).asInt() == 401);
}

// The same guard has to hold for an owner that is present but meaningless: a zero
// or negative id attributes the money to no one.
DROGON_TEST(PayHandlers_CreatePayment_ZeroUserId_Answers401)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createPayment(req, std::move(cb)); };
    const auto answer = offer(handler, payBody(Json::Value("1.00"), Json::Value(0)));
    CHECK(answer.answered);
    CHECK(answer.status == drogon::k401Unauthorized);
    CHECK(answer.body.get("code", -1).asInt() == 401);
}

// =============================================================================
// `/api/qrpay/create`
// =============================================================================

DROGON_TEST(PayHandlers_CreateQRPayment_AmountObject_Answers400InsteadOfThrowing)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createQRPayment(req, std::move(cb)); };
    const auto answer = offer(handler, qrBody(Json::Value(Json::objectValue), Json::Value(7)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

DROGON_TEST(PayHandlers_CreateQRPayment_UserIdObject_Answers400InsteadOfThrowing)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createQRPayment(req, std::move(cb)); };
    const auto answer = offer(handler, qrBody(Json::Value("9.99"), Json::Value(Json::objectValue)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

// The QR route used to skip the amount-format check `/api/pay/create` runs, so
// an unrepresentable amount reached Alipay verbatim and was booked on the order.
DROGON_TEST(PayHandlers_CreateQRPayment_OverPreciseAmount_Refused)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createQRPayment(req, std::move(cb)); };
    const auto answer = offer(handler, qrBody(Json::Value("12.345"), Json::Value(7)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 40001));
}

DROGON_TEST(PayHandlers_CreateQRPayment_NegativeAmount_Refused)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createQRPayment(req, std::move(cb)); };
    const auto answer = offer(handler, qrBody(Json::Value("-100"), Json::Value(7)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 40001));
}

// An owner id that only fits 64 bits is a legitimate tenant, and the column is
// BIGINT: the QR route read it through the 32-bit gate, so it refused such an
// id as "must be an integer" (and would have truncated it had the gate been
// looser than the read).
DROGON_TEST(PayHandlers_CreateQRPayment_OwnerAboveInt32Range_NotRefusedAsMistyped)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createQRPayment(req, std::move(cb)); };
    const auto answer = offer(
      handler, qrBody(Json::Value("9.99"), Json::Value(static_cast<Json::Int64>(3000000000LL)))
    );
    CHECK(!refusedWith(answer, 400));
    CHECK(answer.body.get("message", "").asString() != "Field user_id must be an integer");
}

// A QR order also needs a positive owner, for the reason `/api/pay/create` has
// the same guard.
DROGON_TEST(PayHandlers_CreateQRPayment_ZeroUserId_Refused)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.createQRPayment(req, std::move(cb)); };
    const auto answer = offer(handler, qrBody(Json::Value("9.99"), Json::Value(0)));
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

// =============================================================================
// `/api/pay/refund`
// =============================================================================

DROGON_TEST(PayHandlers_Refund_AmountObject_Answers400InsteadOfThrowing)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.refund(req, std::move(cb)); };
    Json::Value body;
    body["order_no"] = "ord_shape_" + drogon::utils::getUuid();
    body["amount"] = Json::Value(Json::objectValue);
    const auto answer = offer(handler, body);
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 400));
}

DROGON_TEST(PayHandlers_Refund_OverPreciseAmount_Refused)
{
    PayController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.refund(req, std::move(cb)); };
    Json::Value body;
    body["order_no"] = "ord_shape_" + drogon::utils::getUuid();
    body["amount"] = "0.001";
    const auto answer = offer(handler, body);
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 40001));
}

// =============================================================================
// `/api/pay/notify/wechat`
// =============================================================================

// The callback route is unsigned until the service has verified the headers, so
// this body is whatever an anonymous caller sends. `event_type` used to be the
// first conversion in the file.
DROGON_TEST(CallbackHandlers_WechatNotify_EventTypeObject_Answers400InsteadOfThrowing)
{
    WechatCallbackController controller;
    auto handler = [&controller](
                     const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb
                   ) { controller.notify(req, std::move(cb)); };
    Json::Value body;
    body["id"] = "notify_shape_" + drogon::utils::getUuid();
    body["event_type"] = Json::Value(Json::objectValue);
    const auto answer = offer(handler, body);
    CHECK(answer.fault.empty());
    CHECK(refusedWith(answer, 40003));
}
