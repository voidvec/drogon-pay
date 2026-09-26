#include <drogon/drogon.h>
#include <drogon_pay/PayPlugin.h>
#include "utils/ConfigLoader.h"
#include "utils/StartupValidator.h"
#include "utils/SecurityHeaders.h"
#include <fstream>
#include <json/json.h>
#include <string>

using namespace drogon;

int main()
{
    // Static-library linking safety net: keeps the PayPlugin DrObject
    // auto-registration object file (see docs/development/plugin_integration.md)
    drogon_pay::ensureLinked();

    // 1. Load .env file into process environment
    ConfigLoader::loadEnvFile(".env");

    // 2. Validate required environment variables
    // PAY_REDIS_PASSWORD is intentionally optional: the bundled docker-compose
    // runs Redis without auth, and many deployments use a no-auth Redis. Only
    // the DB password and API key are mandatory.
    StartupValidator::validate({"PAY_DB_PASSWORD", "PAY_API_KEY"});

    // 3. Read config.json and replace __env_var:XXX__ placeholders
    std::ifstream configFile("./config.json");
    if (!configFile.is_open())
    {
        LOG_FATAL << "Failed to open config.json";
        return 1;
    }
    Json::Value config;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, configFile, &config, &errors))
    {
        LOG_FATAL << "Failed to parse config.json: " << errors;
        return 1;
    }
    Json::Value processedConfig = ConfigLoader::loadConfig(config);

    // 3b. Warn about enabled channels that cannot work (non-fatal, so a
    // partial rollout still boots) instead of leaving the gap to surface as
    // rejected callbacks.
    StartupValidator::validateChannelReadiness(processedConfig);

    // 4. Load processed config into Drogon
    drogon::app().loadConfigJson(std::move(processedConfig));
    pay::security::setupCorsAndSecurityHeaders();
    drogon::app().run();
    return 0;
}
