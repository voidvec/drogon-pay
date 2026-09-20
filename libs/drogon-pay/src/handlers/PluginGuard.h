#pragma once

#include <functional>
#include <string>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include "drogon_pay/PayPlugin.h"

// `drogon::app().getPlugin<PayPlugin>()` is documented to return null: a host
// whose linker drops the DrObject self-registration symbol sees exactly that,
// which is why `drogon_pay::ensureLinked()` exists (see `PayPlugin.h`). A config
// that registers these routes without a PayPlugin entry reaches the same state.
// Calling a member on the null pointer faults inside the handler, and an
// exception -- let alone a segfault -- is not a lost request but a stopped event
// loop and an unwound `app().run()`: the whole gateway goes down. The two
// callback routes are anonymous, so that needs no credential at all, and on the
// WeChat notify route the dereference also sits *before* body validation, which
// would make every shape guard downstream unreachable in exactly this state.
//
// Every service lookup therefore goes through a presence test and answers this
// fault, which uses the code the contract already reserves for "one of our own
// dependencies is missing" (1501 -> HTTP 503). A channel retrying is the safe
// outcome for a callback: the answer is never a success acknowledgement, so no
// notification is consumed on a process that cannot book it.
inline void respondPluginUnavailable(
  const std::function<void(const drogon::HttpResponsePtr &)> &callback,
  const std::string &what
)
{
    LOG_ERROR << "[PayHandlers] " << what
              << " unavailable: no PayPlugin registered in this process";
    Json::Value body;
    body["code"] = 1501;
    body["message"] = what + " is not available in this process";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}
