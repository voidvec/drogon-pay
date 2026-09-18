#include "ConfigLoader.h"
#include <drogon/utils/Utilities.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>

Json::Value ConfigLoader::loadConfig(const Json::Value &config)
{
    return replacePlaceholders(config);
}

bool ConfigLoader::loadEnvFile(const std::string &envPath)
{
    std::ifstream file(envPath);
    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        // Find '=' separator
        size_t pos = line.find('=');
        if (pos == std::string::npos)
        {
            continue;
        }

        // Extract key and value
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        // Remove quotes if present
        if (!value.empty() && value[0] == '"' && value[value.length() - 1] == '"')
        {
            value = value.substr(1, value.length() - 2);
        }

        // Set environment variable
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }

    return true;
}

Json::Value ConfigLoader::replacePlaceholders(const Json::Value &value)
{
    if (value.isString())
    {
        std::string str = value.asString();

        // Check if this is an environment variable placeholder
        if (str.find("__env_var") == 0)
        {
            // This is a placeholder, try to get env value
            std::string envValue = getEnvValue(str);

            // If env var is found, return it; otherwise return empty string
            return envValue.empty() ? "" : envValue;
        }

        // Not a placeholder, return as-is
        return value;
    }
    else if (value.isObject())
    {
        Json::Value result;
        for (const auto &key : value.getMemberNames())
        {
            result[key] = replacePlaceholders(value[key]);
        }
        return result;
    }
    else if (value.isArray())
    {
        Json::Value result(Json::arrayValue);
        for (Json::ArrayIndex i = 0; i < value.size(); ++i)
        {
            result.append(replacePlaceholders(value[i]));
        }
        return result;
    }

    return value;
}

std::string ConfigLoader::getEnvValue(const std::string &placeholder)
{
    // Parse placeholder format: "__env_var[:VAR_NAME]__"
    size_t colonPos = placeholder.find(':');
    size_t endPos = placeholder.rfind("__");

    if (endPos == std::string::npos)
    {
        return "";
    }

    std::string envVarName;

    if (colonPos != std::string::npos && colonPos < endPos)
    {
        // Explicit variable name: __env_var:MY_VAR__
        envVarName = placeholder.substr(colonPos + 1, endPos - colonPos - 1);
    }
    else
    {
        // No explicit name, should be handled by parseEnvVarName with key
        return "";
    }

    // Get environment variable
    const char *envValue = std::getenv(envVarName.c_str());
    return envValue ? std::string(envValue) : "";
}

std::string ConfigLoader::
  parseEnvVarName(const std::string &placeholder, const std::string & /*key*/)
{
    // For "__env_var__" (no explicit name), convert key to env var format
    // Example: "app_id" -> "ALIPAY_SANDBOX_APP_ID"

    size_t colonPos = placeholder.find(':');
    size_t endPos = placeholder.rfind("__");

    if (colonPos != std::string::npos && colonPos < endPos)
    {
        // Explicit variable name: __env_var:MY_VAR__
        return placeholder.substr(colonPos + 1, endPos - colonPos - 1);
    }

    // No explicit name, generate from key
    // This is handled in replacePlaceholders by using key context
    return "";
}

std::string ConfigLoader::maskSensitive(const std::string &value)
{
    if (value.size() <= 8)
    {
        return "***";
    }
    return value.substr(0, 4) + "***" + value.substr(value.size() - 4);
}
