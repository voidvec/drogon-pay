#include "StartupValidator.h"
#include "ConfigLoader.h"
#include <drogon/drogon.h>
#include <cstdlib>

bool StartupValidator::isPlaceholder(const std::string &value)
{
    if (value.empty())
    {
        return true;
    }
    if (value.find("__env_var") == 0)
    {
        return true;
    }
    if (value.size() >= 3 && value.front() == '$' && value[1] == '{' && value.back() == '}')
    {
        return true;
    }
    return false;
}

ValidationResult StartupValidator::validateRequired(const std::vector<std::string> &requiredVars)
{
    ValidationResult result;
    result.ok = true;

    for (const auto &varName : requiredVars)
    {
        const char *envValue = std::getenv(varName.c_str());
        if (!envValue || isPlaceholder(std::string(envValue)))
        {
            result.ok = false;
            result.missingVars.push_back(varName);
        }
    }

    return result;
}

void StartupValidator::validate(const std::vector<std::string> &requiredVars)
{
    auto result = validateRequired(requiredVars);

    if (!result.ok)
    {
        for (const auto &varName : result.missingVars)
        {
            LOG_FATAL << "Missing or invalid required environment variable: " << varName;
        }
        LOG_FATAL << "Startup validation failed. Exiting.";
        exit(1);
    }

    LOG_INFO << "Startup validation passed. Loaded sensitive config:";
    for (const auto &varName : requiredVars)
    {
        const char *envValue = std::getenv(varName.c_str());
        if (envValue)
        {
            LOG_INFO << "  " << varName << " = "
                     << ConfigLoader::maskSensitive(std::string(envValue));
        }
    }
}

std::vector<std::string> StartupValidator::validateChannelReadiness(
  const Json::Value &processedConfig
)
{
    std::vector<std::string> warnings;

    const Json::Value &plugins = processedConfig["plugins"];
    if (!plugins.isArray())
    {
        return warnings;
    }

    for (const auto &plugin : plugins)
    {
        if (plugin.get("name", "").asString() != "PayPlugin")
        {
            continue;
        }
        const Json::Value &channels = plugin["config"]["channels"];
        if (!channels.isObject())
        {
            continue;
        }
        for (const auto &name : channels.getMemberNames())
        {
            const Json::Value &channel = channels[name];
            if (!channel.get("enabled", false).asBool())
            {
                continue;
            }
            // isPlaceholder covers both spellings of "not configured": an
            // env var that was absent (ConfigLoader leaves an empty string) and
            // one that never got resolved at all.
            if (isPlaceholder(channel.get("app_id", "").asString()))
            {
                const std::string warning = "Channel '" + name +
                                            "' is enabled but app_id is not set; "
                                            "its API calls and callbacks will be rejected";
                warnings.push_back(warning);
                LOG_WARN << warning;
            }
        }
    }

    return warnings;
}
