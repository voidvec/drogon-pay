#pragma once

#include <json/json.h>
#include <string>
#include <vector>

struct ValidationResult
{
    bool ok = false;
    std::vector<std::string> missingVars;
};

class StartupValidator
{
  public:
    static bool isPlaceholder(const std::string &value);
    static ValidationResult validateRequired(const std::vector<std::string> &requiredVars);
    static void validate(const std::vector<std::string> &requiredVars);

    // Non-fatal channel readiness check against the resolved config. An enabled
    // channel whose app_id never arrived stays silent at startup and then
    // rejects every request at runtime, so the gap has to be reported before
    // the first callback does it by accident. Returns one warning string per
    // problem (empty when the config is coherent) and logs each as LOG_WARN.
    static std::vector<std::string> validateChannelReadiness(const Json::Value &processedConfig);
};
