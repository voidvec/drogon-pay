#pragma once

/// Shared CORS + security header setup, used by both main.cc and tests/main.cc.
/// Extracting this ensures tests run against the same header configuration
/// as production (P2-6.9 fix: security headers were not testable because
/// setupCors() was only called in main()).

#include <drogon/drogon.h>
#include <json/json.h>

namespace pay::security
{

inline void setupCorsAndSecurityHeaders()
{
    auto isAllowed = [](const std::string &origin) -> bool {
        if (origin.empty())
            return false;

        const auto &customConfig = drogon::app().getCustomConfig();
        const auto &allowOrigins = customConfig["cors"]["allow_origins"];

        if (allowOrigins.isArray())
        {
            for (const auto &allowed : allowOrigins)
            {
                if (allowed.asString() == origin)
                    return true;
            }
        }
        return false;
    };

    // CORS preflight (OPTIONS) handler
    drogon::app().registerSyncAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req) -> drogon::HttpResponsePtr {
          if (req->method() == drogon::HttpMethod::Options)
          {
              const auto &origin = req->getHeader("Origin");
              if (isAllowed(origin))
              {
                  auto resp = drogon::HttpResponse::newHttpResponse();
                  resp->addHeader("Access-Control-Allow-Origin", origin);

                  const auto &requestMethod = req->getHeader("Access-Control-Request-Method");
                  if (!requestMethod.empty())
                  {
                      resp->addHeader("Access-Control-Allow-Methods", requestMethod);
                  }

                  resp->addHeader("Access-Control-Allow-Credentials", "true");

                  const auto &requestHeaders = req->getHeader("Access-Control-Request-Headers");
                  if (!requestHeaders.empty())
                  {
                      resp->addHeader("Access-Control-Allow-Headers", requestHeaders);
                  }
                  return resp;
              }
          }
          return {};
      }
    );

    // Post-handling: CORS headers for allowed origins + security headers
    drogon::app().registerPostHandlingAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
          const auto &origin = req->getHeader("Origin");
          if (isAllowed(origin))
          {
              resp->addHeader("Access-Control-Allow-Origin", origin);
              resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
              resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
              resp->addHeader("Access-Control-Allow-Credentials", "true");
          }
          // Security headers (P1-5)
          resp->addHeader("X-Content-Type-Options", "nosniff");
          resp->addHeader("X-Frame-Options", "DENY");
          resp->addHeader("X-XSS-Protection", "1; mode=block");
          resp->addHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
          resp->addHeader("Content-Security-Policy", "default-src 'none'; script-src 'self'");
          resp->addHeader("Referrer-Policy", "no-referrer");
          resp->addHeader("Permissions-Policy", "geolocation=(), microphone=(), camera=()");
      }
    );

    // Drogon's router auto-answers OPTIONS for registered paths and echoes the
    // Origin header into Access-Control-Allow-Origin unconditionally. Strip
    // CORS allow headers from every outgoing response whose Origin is not in
    // the whitelist (pre-sending advice runs for framework-generated
    // responses too, unlike post-handling advice).
    drogon::app().registerPreSendingAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
          const auto &origin = req->getHeader("Origin");
          if (!origin.empty() && !isAllowed(origin))
          {
              resp->removeHeader("Access-Control-Allow-Origin");
              resp->removeHeader("Access-Control-Allow-Methods");
              resp->removeHeader("Access-Control-Allow-Headers");
              resp->removeHeader("Access-Control-Allow-Credentials");
          }
      }
    );
}

}  // namespace pay::security
