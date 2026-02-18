#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RadioLibInterface.h"
#include "airtime.h"
#include "main.h"
#include "mesh/http/ContentHelper.h"
#include "mesh/http/WebServer.h"
#include "telegram/TelegramBridge.h"
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "SPILock.h"
#include "power.h"
#include "serialization/JSON.h"
#include <FSCommon.h>
#include <HTTPBodyParser.hpp>
#include <HTTPMultipartBodyParser.hpp>
#include <HTTPURLEncodedBodyParser.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

/*
  Including the esp32_https_server library will trigger a compile time error. I've
  tracked it down to a reoccurrance of this bug:
    https://gcc.gnu.org/bugzilla/show_bug.cgi?id=57824
  The work around is described here:
    https://forums.xilinx.com/t5/Embedded-Development-Tools/Error-with-Standard-Libaries-in-Zynq/td-p/450032

  Long story short is we need "#undef str" before including the esp32_https_server.
    - Jm Casler (jm@casler.org) Oct 2020
*/
#undef str

// Includes for the https server
//   https://github.com/fhessel/esp32_https_server
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>
#include <HTTPServer.hpp>
#include <SSLCert.hpp>

// The HTTPS Server comes in a separate namespace. For easier use, include it here.
using namespace httpsserver;

#include "mesh/http/ContentHandler.h"

#define DEST_FS_USES_LITTLEFS

// We need to specify some content-type mapping, so the resources get delivered with the
// right content type and are displayed correctly in the browser
char const *contentTypes[][2] = {{".txt", "text/plain"},     {".html", "text/html"},
                                 {".js", "text/javascript"}, {".png", "image/png"},
                                 {".jpg", "image/jpg"},      {".gz", "application/gzip"},
                                 {".gif", "image/gif"},      {".json", "application/json"},
                                 {".css", "text/css"},       {".ico", "image/vnd.microsoft.icon"},
                                 {".svg", "image/svg+xml"},  {"", ""}};

// const char *certificate = NULL; // change this as needed, leave as is for no TLS check (yolo security)

// Our API to handle messages to and from the radio.
HttpAPI webAPI;

namespace
{
constexpr size_t HISTORY_DEFAULT_LIMIT = 50;
constexpr size_t HISTORY_MAX_LIMIT = 200;

std::string trimString(const std::string &value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        start++;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }

    return value.substr(start, end - start);
}

std::string toLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void setTelegramApiHeaders(HTTPResponse *res, const char *methods)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", methods);
    res->setHeader("Access-Control-Allow-Headers", "Content-Type");
}

bool handleApiOptions(HTTPRequest *req, HTTPResponse *res)
{
    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204);
        res->print("");
        return true;
    }

    return false;
}

void writeJson(HTTPResponse *res, const JSONObject &jsonObj)
{
    JSONValue *value = new JSONValue(jsonObj);
    const std::string jsonString = value->Stringify();
    res->print(jsonString.c_str());
    delete value;
}

void writeTelegramError(HTTPResponse *res, int statusCode, const char *code, const std::string &message)
{
    res->setStatusCode(statusCode);

    JSONObject errorObj;
    errorObj["code"] = new JSONValue(code);
    errorObj["message"] = new JSONValue(message.c_str());

    JSONObject jsonObj;
    jsonObj["status"] = new JSONValue("error");
    jsonObj["error"] = new JSONValue(errorObj);
    writeJson(res, jsonObj);
}

bool parseBoolValue(const std::string &rawValue, bool &out)
{
    const std::string normalized = toLowerCopy(trimString(rawValue));
    if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes") {
        out = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
        out = false;
        return true;
    }
    return false;
}

bool parseUInt32Value(const std::string &rawValue, uint32_t &out)
{
    const std::string normalized = trimString(rawValue);
    if (normalized.empty()) {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = strtoul(normalized.c_str(), &end, 10);
    if (end == normalized.c_str() || (end != nullptr && *end != '\0') || parsed > UINT32_MAX) {
        return false;
    }

    out = static_cast<uint32_t>(parsed);
    return true;
}

const char *directionModeToString(TelegramDirectionMode mode)
{
    switch (mode) {
    case TelegramDirectionMode::MESH_TO_TELEGRAM:
        return "mesh_to_telegram";
    case TelegramDirectionMode::TELEGRAM_TO_MESH:
        return "telegram_to_mesh";
    case TelegramDirectionMode::BOTH:
    default:
        return "both";
    }
}

bool parseDirectionMode(const std::string &rawValue, TelegramDirectionMode &out)
{
    const std::string normalized = toLowerCopy(trimString(rawValue));
    if (normalized == "both" || normalized == "bidir") {
        out = TelegramDirectionMode::BOTH;
        return true;
    }
    if (normalized == "mesh_to_telegram" || normalized == "m2t") {
        out = TelegramDirectionMode::MESH_TO_TELEGRAM;
        return true;
    }
    if (normalized == "telegram_to_mesh" || normalized == "t2m") {
        out = TelegramDirectionMode::TELEGRAM_TO_MESH;
        return true;
    }
    return false;
}

const char *historyDirectionToString(TelegramHistoryDirection direction)
{
    return direction == TelegramHistoryDirection::INCOMING ? "incoming" : "outgoing";
}

const char *historyStatusToString(TelegramHistoryStatus status)
{
    switch (status) {
    case TelegramHistoryStatus::SENT:
        return "sent";
    case TelegramHistoryStatus::SEND_FAILED:
        return "send_failed";
    case TelegramHistoryStatus::RECEIVED:
        return "received";
    case TelegramHistoryStatus::INJECTED:
        return "injected";
    case TelegramHistoryStatus::IGNORED_CHAT:
        return "ignored_chat";
    case TelegramHistoryStatus::COMMAND:
        return "command";
    case TelegramHistoryStatus::INJECT_FAILED:
        return "inject_failed";
    case TelegramHistoryStatus::QUEUED:
    default:
        return "queued";
    }
}

bool parseHistoryDirectionFilter(const std::string &rawValue, TelegramHistoryFilterDirection &out)
{
    const std::string normalized = toLowerCopy(trimString(rawValue));
    if (normalized.empty() || normalized == "both") {
        out = TelegramHistoryFilterDirection::BOTH;
        return true;
    }
    if (normalized == "incoming") {
        out = TelegramHistoryFilterDirection::INCOMING;
        return true;
    }
    if (normalized == "outgoing") {
        out = TelegramHistoryFilterDirection::OUTGOING;
        return true;
    }
    return false;
}

const char *controlErrorToCode(TelegramControlError error)
{
    switch (error) {
    case TelegramControlError::INVALID_ARGUMENT:
        return "invalid_argument";
    case TelegramControlError::PERSISTENCE_ERROR:
        return "persistence_error";
    case TelegramControlError::NOT_AVAILABLE:
        return "not_available";
    case TelegramControlError::NONE:
    default:
        return "none";
    }
}

int controlErrorToHttpStatus(TelegramControlError error)
{
    switch (error) {
    case TelegramControlError::INVALID_ARGUMENT:
        return 400;
    case TelegramControlError::NOT_AVAILABLE:
        return 503;
    case TelegramControlError::PERSISTENCE_ERROR:
        return 500;
    case TelegramControlError::NONE:
    default:
        return 200;
    }
}

JSONObject makeControlSnapshotJson(const TelegramControlSnapshot &snapshot)
{
    JSONObject configObj;
    configObj["feature_available"] = new JSONValue(snapshot.featureAvailable);
    configObj["enabled"] = new JSONValue(snapshot.enabled);
    configObj["running"] = new JSONValue(snapshot.running);
    configObj["configured"] = new JSONValue(snapshot.configured);
    configObj["wifi_connected"] = new JSONValue(snapshot.wifiConnected);
    configObj["allow_all_channels"] = new JSONValue(snapshot.allowAllChannels);
    configObj["channels"] = new JSONValue(snapshot.channels.c_str());
    configObj["mesh_channel_for_inject"] = new JSONValue(static_cast<unsigned int>(snapshot.meshChannelForInject));
    configObj["queue_used"] = new JSONValue(static_cast<unsigned int>(snapshot.queueUsed));
    configObj["queue_capacity"] = new JSONValue(static_cast<unsigned int>(snapshot.queueCapacity));
    configObj["poll_interval_ms"] = new JSONValue(snapshot.pollIntervalMs);
    configObj["long_poll_timeout_sec"] = new JSONValue(snapshot.longPollTimeoutSec);
    configObj["send_interval_ms"] = new JSONValue(snapshot.sendIntervalMs);
    configObj["direction"] = new JSONValue(directionModeToString(snapshot.directionMode));
    configObj["mesh_to_telegram_enabled"] = new JSONValue(snapshot.meshToTelegramEnabled);
    configObj["telegram_to_mesh_enabled"] = new JSONValue(snapshot.telegramToMeshEnabled);
    configObj["has_token"] = new JSONValue(snapshot.hasToken);
    configObj["has_chat_id"] = new JSONValue(snapshot.hasChatId);
    configObj["chat_id"] = new JSONValue(snapshot.chatId.c_str());
    return configObj;
}

JSONObject makeControlResultJson(const TelegramControlResult &result)
{
    JSONObject resultObj;
    resultObj["ok"] = new JSONValue(result.ok());
    resultObj["changed"] = new JSONValue(result.changed);
    resultObj["persisted"] = new JSONValue(result.persisted);
    resultObj["error"] = new JSONValue(controlErrorToCode(result.error));
    resultObj["message"] = new JSONValue(result.message.c_str());
    return resultObj;
}

std::string normalizeContentType(const std::string &rawContentType)
{
    std::string normalized = rawContentType;
    const size_t semicolon = normalized.find(';');
    if (semicolon != std::string::npos) {
        normalized.resize(semicolon);
    }
    return toLowerCopy(trimString(normalized));
}

std::string readParserFieldValue(HTTPBodyParser &parser)
{
    std::string value;
    while (!parser.endOfField()) {
        byte buffer[128];
        const size_t readLength = parser.read(buffer, sizeof(buffer));
        if (readLength == 0) {
            break;
        }
        value.append(reinterpret_cast<const char *>(buffer), readLength);
    }
    return value;
}

std::map<std::string, std::string> parseUrlEncodedBody(HTTPRequest *req)
{
    std::map<std::string, std::string> fields;
    if (normalizeContentType(req->getHeader("Content-Type")) != "application/x-www-form-urlencoded") {
        return fields;
    }

    HTTPURLEncodedBodyParser parser(req);
    while (parser.nextField()) {
        const std::string key = parser.getFieldName();
        if (key.empty()) {
            while (!parser.endOfField()) {
                byte discard[32];
                if (parser.read(discard, sizeof(discard)) == 0) {
                    break;
                }
            }
            continue;
        }
        fields[key] = readParserFieldValue(parser);
    }

    return fields;
}

bool getRequestValue(HTTPRequest *req, const std::map<std::string, std::string> &bodyFields, const char *name,
                     std::string &out)
{
    ResourceParameters *params = req->getParams();
    if (params != nullptr && params->getQueryParameter(name, out)) {
        return true;
    }

    const auto bodyIt = bodyFields.find(name);
    if (bodyIt != bodyFields.end()) {
        out = bodyIt->second;
        return true;
    }

    return false;
}

bool parseTelegramPatch(HTTPRequest *req, const std::map<std::string, std::string> &bodyFields, TelegramControlPatch &patch,
                        std::string &error)
{
    std::string value;

    if (getRequestValue(req, bodyFields, "enabled", value)) {
        bool enabledValue = false;
        if (!parseBoolValue(value, enabledValue)) {
            error = "enabled must be boolean";
            return false;
        }
        patch.hasEnabled = true;
        patch.enabled = enabledValue;
    }

    if (getRequestValue(req, bodyFields, "token", value)) {
        patch.hasToken = true;
        patch.token = value;
    }

    if (getRequestValue(req, bodyFields, "chat_id", value) || getRequestValue(req, bodyFields, "chatId", value)) {
        patch.hasChatId = true;
        patch.chatId = value;
    }

    if (getRequestValue(req, bodyFields, "channels", value)) {
        patch.hasChannels = true;
        patch.channels = value;
    }

    if (getRequestValue(req, bodyFields, "poll_interval_ms", value) || getRequestValue(req, bodyFields, "pollIntervalMs", value)) {
        uint32_t parsed = 0;
        if (!parseUInt32Value(value, parsed)) {
            error = "poll_interval_ms must be uint";
            return false;
        }
        patch.hasPollIntervalMs = true;
        patch.pollIntervalMs = parsed;
    }

    if (getRequestValue(req, bodyFields, "long_poll_timeout_sec", value) ||
        getRequestValue(req, bodyFields, "longPollTimeoutSec", value)) {
        uint32_t parsed = 0;
        if (!parseUInt32Value(value, parsed)) {
            error = "long_poll_timeout_sec must be uint";
            return false;
        }
        patch.hasLongPollTimeoutSec = true;
        patch.longPollTimeoutSec = parsed;
    }

    if (getRequestValue(req, bodyFields, "send_interval_ms", value) || getRequestValue(req, bodyFields, "sendIntervalMs", value)) {
        uint32_t parsed = 0;
        if (!parseUInt32Value(value, parsed)) {
            error = "send_interval_ms must be uint";
            return false;
        }
        patch.hasSendIntervalMs = true;
        patch.sendIntervalMs = parsed;
    }

    if (getRequestValue(req, bodyFields, "direction", value) || getRequestValue(req, bodyFields, "directionMode", value) ||
        getRequestValue(req, bodyFields, "direction_mode", value)) {
        TelegramDirectionMode directionMode = TelegramDirectionMode::BOTH;
        if (!parseDirectionMode(value, directionMode)) {
            error = "direction must be both|mesh_to_telegram|telegram_to_mesh";
            return false;
        }
        patch.hasDirectionMode = true;
        patch.directionMode = directionMode;
    }

    if (!patch.hasEnabled && !patch.hasToken && !patch.hasChatId && !patch.hasChannels && !patch.hasPollIntervalMs &&
        !patch.hasLongPollTimeoutSec && !patch.hasSendIntervalMs && !patch.hasDirectionMode) {
        error = "no supported fields provided";
        return false;
    }

    return true;
}

} // namespace

void registerHandlers(HTTPServer *insecureServer, HTTPSServer *secureServer)
{

    // For every resource available on the server, we need to create a ResourceNode
    // The ResourceNode links URL and HTTP method to a handler function

    ResourceNode *nodeAPIv1ToRadioOptions = new ResourceNode("/api/v1/toradio", "OPTIONS", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1ToRadio = new ResourceNode("/api/v1/toradio", "PUT", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1FromRadioOptions = new ResourceNode("/api/v1/fromradio", "OPTIONS", &handleAPIv1FromRadio);
    ResourceNode *nodeAPIv1FromRadio = new ResourceNode("/api/v1/fromradio", "GET", &handleAPIv1FromRadio);

    //    ResourceNode *nodeHotspotApple = new ResourceNode("/hotspot-detect.html", "GET", &handleHotspot);
    //    ResourceNode *nodeHotspotAndroid = new ResourceNode("/generate_204", "GET", &handleHotspot);

    ResourceNode *nodeAdmin = new ResourceNode("/admin", "GET", &handleAdmin);
    //    ResourceNode *nodeAdminSettings = new ResourceNode("/admin/settings", "GET", &handleAdminSettings);
    //    ResourceNode *nodeAdminSettingsApply = new ResourceNode("/admin/settings/apply", "POST", &handleAdminSettingsApply);
    //    ResourceNode *nodeAdminFs = new ResourceNode("/admin/fs", "GET", &handleFs);
    //    ResourceNode *nodeUpdateFs = new ResourceNode("/admin/fs/update", "POST", &handleUpdateFs);
    //    ResourceNode *nodeDeleteFs = new ResourceNode("/admin/fs/delete", "GET", &handleDeleteFsContent);

    ResourceNode *nodeRestart = new ResourceNode("/restart", "POST", &handleRestart);
    ResourceNode *nodeFormUpload = new ResourceNode("/upload", "POST", &handleFormUpload);

    ResourceNode *nodeJsonScanNetworks = new ResourceNode("/json/scanNetworks", "GET", &handleScanNetworks);
    ResourceNode *nodeJsonReport = new ResourceNode("/json/report", "GET", &handleReport);
    ResourceNode *nodeJsonNodes = new ResourceNode("/json/nodes", "GET", &handleNodes);
    ResourceNode *nodeJsonFsBrowseStatic = new ResourceNode("/json/fs/browse/static", "GET", &handleFsBrowseStatic);
    ResourceNode *nodeJsonDelete = new ResourceNode("/json/fs/delete/static", "DELETE", &handleFsDeleteStatic);

    ResourceNode *nodeAPIv1TelegramConfigOptions = new ResourceNode("/api/v1/telegram/config", "OPTIONS", &handleTelegramConfig);
    ResourceNode *nodeAPIv1TelegramConfigGet = new ResourceNode("/api/v1/telegram/config", "GET", &handleTelegramConfig);
    ResourceNode *nodeAPIv1TelegramConfigPut = new ResourceNode("/api/v1/telegram/config", "PUT", &handleTelegramConfig);

    ResourceNode *nodeAPIv1TelegramEnabledOptions =
        new ResourceNode("/api/v1/telegram/enabled", "OPTIONS", &handleTelegramEnabled);
    ResourceNode *nodeAPIv1TelegramEnabledPut = new ResourceNode("/api/v1/telegram/enabled", "PUT", &handleTelegramEnabled);

    ResourceNode *nodeAPIv1TelegramHistoryOptions =
        new ResourceNode("/api/v1/telegram/history", "OPTIONS", &handleTelegramHistory);
    ResourceNode *nodeAPIv1TelegramHistoryGet = new ResourceNode("/api/v1/telegram/history", "GET", &handleTelegramHistory);
    ResourceNode *nodeAPIv1TelegramHistoryDelete =
        new ResourceNode("/api/v1/telegram/history", "DELETE", &handleTelegramHistory);

    ResourceNode *nodeAPIv1TelegramHistoryChatsOptions =
        new ResourceNode("/api/v1/telegram/history/chats", "OPTIONS", &handleTelegramHistoryChats);
    ResourceNode *nodeAPIv1TelegramHistoryChatsGet =
        new ResourceNode("/api/v1/telegram/history/chats", "GET", &handleTelegramHistoryChats);

    ResourceNode *nodeAdminTelegram = new ResourceNode("/admin/telegram", "GET", &handleAdminTelegram);

    ResourceNode *nodeRoot = new ResourceNode("/*", "GET", &handleStatic);

    // Secure nodes
    secureServer->registerNode(nodeAPIv1ToRadioOptions);
    secureServer->registerNode(nodeAPIv1ToRadio);
    secureServer->registerNode(nodeAPIv1FromRadioOptions);
    secureServer->registerNode(nodeAPIv1FromRadio);
    //    secureServer->registerNode(nodeHotspotApple);
    //    secureServer->registerNode(nodeHotspotAndroid);
    secureServer->registerNode(nodeRestart);
    secureServer->registerNode(nodeFormUpload);
    secureServer->registerNode(nodeJsonScanNetworks);
    secureServer->registerNode(nodeJsonFsBrowseStatic);
    secureServer->registerNode(nodeJsonDelete);
    secureServer->registerNode(nodeJsonReport);
    secureServer->registerNode(nodeJsonNodes);
    secureServer->registerNode(nodeAPIv1TelegramConfigOptions);
    secureServer->registerNode(nodeAPIv1TelegramConfigGet);
    secureServer->registerNode(nodeAPIv1TelegramConfigPut);
    secureServer->registerNode(nodeAPIv1TelegramEnabledOptions);
    secureServer->registerNode(nodeAPIv1TelegramEnabledPut);
    secureServer->registerNode(nodeAPIv1TelegramHistoryOptions);
    secureServer->registerNode(nodeAPIv1TelegramHistoryGet);
    secureServer->registerNode(nodeAPIv1TelegramHistoryDelete);
    secureServer->registerNode(nodeAPIv1TelegramHistoryChatsOptions);
    secureServer->registerNode(nodeAPIv1TelegramHistoryChatsGet);
    //    secureServer->registerNode(nodeUpdateFs);
    //    secureServer->registerNode(nodeDeleteFs);
    secureServer->registerNode(nodeAdmin);
    secureServer->registerNode(nodeAdminTelegram);
    //    secureServer->registerNode(nodeAdminFs);
    //    secureServer->registerNode(nodeAdminSettings);
    //    secureServer->registerNode(nodeAdminSettingsApply);
    secureServer->registerNode(nodeRoot); // This has to be last

    // Insecure nodes
    insecureServer->registerNode(nodeAPIv1ToRadioOptions);
    insecureServer->registerNode(nodeAPIv1ToRadio);
    insecureServer->registerNode(nodeAPIv1FromRadioOptions);
    insecureServer->registerNode(nodeAPIv1FromRadio);
    //    insecureServer->registerNode(nodeHotspotApple);
    //    insecureServer->registerNode(nodeHotspotAndroid);
    insecureServer->registerNode(nodeRestart);
    insecureServer->registerNode(nodeFormUpload);
    insecureServer->registerNode(nodeJsonScanNetworks);
    insecureServer->registerNode(nodeJsonFsBrowseStatic);
    insecureServer->registerNode(nodeJsonDelete);
    insecureServer->registerNode(nodeJsonReport);
    insecureServer->registerNode(nodeJsonNodes);
    insecureServer->registerNode(nodeAPIv1TelegramConfigOptions);
    insecureServer->registerNode(nodeAPIv1TelegramConfigGet);
    insecureServer->registerNode(nodeAPIv1TelegramConfigPut);
    insecureServer->registerNode(nodeAPIv1TelegramEnabledOptions);
    insecureServer->registerNode(nodeAPIv1TelegramEnabledPut);
    insecureServer->registerNode(nodeAPIv1TelegramHistoryOptions);
    insecureServer->registerNode(nodeAPIv1TelegramHistoryGet);
    insecureServer->registerNode(nodeAPIv1TelegramHistoryDelete);
    insecureServer->registerNode(nodeAPIv1TelegramHistoryChatsOptions);
    insecureServer->registerNode(nodeAPIv1TelegramHistoryChatsGet);
    //    insecureServer->registerNode(nodeUpdateFs);
    //    insecureServer->registerNode(nodeDeleteFs);
    insecureServer->registerNode(nodeAdmin);
    insecureServer->registerNode(nodeAdminTelegram);
    //    insecureServer->registerNode(nodeAdminFs);
    //    insecureServer->registerNode(nodeAdminSettings);
    //    insecureServer->registerNode(nodeAdminSettingsApply);
    insecureServer->registerNode(nodeRoot); // This has to be last
}

void handleAPIv1FromRadio(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    LOG_DEBUG("webAPI handleAPIv1FromRadio");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    // std::string paramAll = "all";
    std::string valueAll;

    // Status code is 200 OK by default.
    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        res->print("");
        return;
    }

    uint8_t txBuf[MAX_STREAM_BUF_SIZE];
    uint32_t len = 1;

    if (params->getQueryParameter("all", valueAll)) {

        // If all is true, return all the buffers we have available
        //   to us at this point in time.
        if (valueAll == "true") {
            while (len) {
                len = webAPI.getFromRadio(txBuf);
                res->write(txBuf, len);
            }

            // Otherwise, just return one protobuf
        } else {
            len = webAPI.getFromRadio(txBuf);
            res->write(txBuf, len);
        }

        // the param "all" was not specified. Return just one protobuf
    } else {
        len = webAPI.getFromRadio(txBuf);
        res->write(txBuf, len);
    }

    LOG_DEBUG("webAPI handleAPIv1FromRadio, len %d", len);
}

void handleAPIv1ToRadio(HTTPRequest *req, HTTPResponse *res)
{
    LOG_DEBUG("webAPI handleAPIv1ToRadio");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Headers", "Content-Type");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "PUT, OPTIONS");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        res->print("");
        return;
    }

    byte buffer[MAX_TO_FROM_RADIO_SIZE];
    size_t s = req->readBytes(buffer, MAX_TO_FROM_RADIO_SIZE);

    LOG_DEBUG("Received %d bytes from PUT request", s);
    webAPI.handleToRadio(buffer, s);

    res->write(buffer, s);
    LOG_DEBUG("webAPI handleAPIv1ToRadio");
}

void handleTelegramConfig(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread) {
        webServerThread->markActivity();
    }

    setTelegramApiHeaders(res, "GET, PUT, OPTIONS");
    if (handleApiOptions(req, res)) {
        return;
    }

    if (req->getMethod() == "GET") {
        const TelegramControlSnapshot snapshot = telegramGetControlSnapshot();

        JSONObject dataObj;
        dataObj["config"] = new JSONValue(makeControlSnapshotJson(snapshot));

        JSONObject jsonObj;
        jsonObj["status"] = new JSONValue("ok");
        jsonObj["data"] = new JSONValue(dataObj);
        writeJson(res, jsonObj);
        return;
    }

    std::map<std::string, std::string> bodyFields;
    if (req->getMethod() == "PUT") {
        bodyFields = parseUrlEncodedBody(req);
    }

    TelegramControlPatch patch;
    std::string parseError;
    if (!parseTelegramPatch(req, bodyFields, patch, parseError)) {
        writeTelegramError(res, 400, "invalid_argument", parseError);
        return;
    }

    const TelegramControlResult result = telegramApplyControlPatch(patch, TelegramControlSource::HTTP_API);
    if (!result.ok()) {
        writeTelegramError(res, controlErrorToHttpStatus(result.error), controlErrorToCode(result.error), result.message);
        return;
    }

    const TelegramControlSnapshot snapshot = telegramGetControlSnapshot();

    JSONObject dataObj;
    dataObj["result"] = new JSONValue(makeControlResultJson(result));
    dataObj["config"] = new JSONValue(makeControlSnapshotJson(snapshot));

    JSONObject jsonObj;
    jsonObj["status"] = new JSONValue("ok");
    jsonObj["data"] = new JSONValue(dataObj);
    writeJson(res, jsonObj);
}

void handleTelegramEnabled(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread) {
        webServerThread->markActivity();
    }

    setTelegramApiHeaders(res, "PUT, OPTIONS");
    if (handleApiOptions(req, res)) {
        return;
    }

    std::map<std::string, std::string> bodyFields;
    if (req->getMethod() == "PUT") {
        bodyFields = parseUrlEncodedBody(req);
    }

    std::string enabledRaw;
    if (!getRequestValue(req, bodyFields, "enabled", enabledRaw)) {
        writeTelegramError(res, 400, "invalid_argument", "enabled is required");
        return;
    }

    bool enabledValue = false;
    if (!parseBoolValue(enabledRaw, enabledValue)) {
        writeTelegramError(res, 400, "invalid_argument", "enabled must be boolean");
        return;
    }

    const TelegramControlResult result = telegramSetEnabled(enabledValue, TelegramControlSource::HTTP_API);
    if (!result.ok()) {
        writeTelegramError(res, controlErrorToHttpStatus(result.error), controlErrorToCode(result.error), result.message);
        return;
    }

    const TelegramControlSnapshot snapshot = telegramGetControlSnapshot();

    JSONObject dataObj;
    dataObj["result"] = new JSONValue(makeControlResultJson(result));
    dataObj["config"] = new JSONValue(makeControlSnapshotJson(snapshot));

    JSONObject jsonObj;
    jsonObj["status"] = new JSONValue("ok");
    jsonObj["data"] = new JSONValue(dataObj);
    writeJson(res, jsonObj);
}

void handleTelegramHistory(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread) {
        webServerThread->markActivity();
    }

    setTelegramApiHeaders(res, "GET, DELETE, OPTIONS");
    if (handleApiOptions(req, res)) {
        return;
    }

    if (req->getMethod() == "DELETE") {
        if (!telegramClearHistory()) {
            writeTelegramError(res, 503, "not_available", "Telegram bridge is not available");
            return;
        }

        JSONObject dataObj;
        dataObj["cleared"] = new JSONValue(true);

        JSONObject jsonObj;
        jsonObj["status"] = new JSONValue("ok");
        jsonObj["data"] = new JSONValue(dataObj);
        writeJson(res, jsonObj);
        return;
    }

    ResourceParameters *params = req->getParams();
    std::string chatId;
    std::string directionRaw;
    std::string limitRaw;

    if (params != nullptr) {
        params->getQueryParameter("chat_id", chatId);
        params->getQueryParameter("direction", directionRaw);
        params->getQueryParameter("limit", limitRaw);
    }

    TelegramHistoryFilterDirection directionFilter = TelegramHistoryFilterDirection::BOTH;
    if (!directionRaw.empty() && !parseHistoryDirectionFilter(directionRaw, directionFilter)) {
        writeTelegramError(res, 400, "invalid_argument", "direction must be incoming|outgoing|both");
        return;
    }

    size_t limit = HISTORY_DEFAULT_LIMIT;
    if (!limitRaw.empty()) {
        uint32_t parsedLimit = 0;
        if (!parseUInt32Value(limitRaw, parsedLimit)) {
            writeTelegramError(res, 400, "invalid_argument", "limit must be uint");
            return;
        }
        limit = parsedLimit;
    }

    if (limit > HISTORY_MAX_LIMIT) {
        limit = HISTORY_MAX_LIMIT;
    }

    const std::vector<TelegramHistoryEntry> history = telegramGetHistory(chatId, directionFilter, limit);

    JSONArray messagesArray;
    for (const TelegramHistoryEntry &entry : history) {
        JSONObject messageObj;
        messageObj["timestamp_ms"] = new JSONValue(static_cast<unsigned int>(entry.timestampMs));
        messageObj["chat_id"] = new JSONValue(entry.chatId.c_str());
        messageObj["direction"] = new JSONValue(historyDirectionToString(entry.direction));
        messageObj["status"] = new JSONValue(historyStatusToString(entry.status));
        messageObj["sender"] = new JSONValue(entry.sender.c_str());
        messageObj["text"] = new JSONValue(entry.text.c_str());
        messagesArray.push_back(new JSONValue(messageObj));
    }

    JSONObject dataObj;
    dataObj["count"] = new JSONValue(static_cast<unsigned int>(history.size()));
    dataObj["messages"] = new JSONValue(messagesArray);

    JSONObject jsonObj;
    jsonObj["status"] = new JSONValue("ok");
    jsonObj["data"] = new JSONValue(dataObj);
    writeJson(res, jsonObj);
}

void handleTelegramHistoryChats(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread) {
        webServerThread->markActivity();
    }

    setTelegramApiHeaders(res, "GET, OPTIONS");
    if (handleApiOptions(req, res)) {
        return;
    }

    size_t limit = HISTORY_MAX_LIMIT;
    ResourceParameters *params = req->getParams();
    std::string limitRaw;
    if (params != nullptr && params->getQueryParameter("limit", limitRaw) && !limitRaw.empty()) {
        uint32_t parsedLimit = 0;
        if (!parseUInt32Value(limitRaw, parsedLimit)) {
            writeTelegramError(res, 400, "invalid_argument", "limit must be uint");
            return;
        }
        limit = parsedLimit;
    }

    if (limit > HISTORY_MAX_LIMIT) {
        limit = HISTORY_MAX_LIMIT;
    }

    const std::vector<TelegramHistoryChatSummary> chats = telegramGetHistoryChats(limit);

    JSONArray chatsArray;
    for (const TelegramHistoryChatSummary &chat : chats) {
        JSONObject chatObj;
        chatObj["chat_id"] = new JSONValue(chat.chatId.c_str());
        chatObj["incoming_count"] = new JSONValue(static_cast<unsigned int>(chat.incomingCount));
        chatObj["outgoing_count"] = new JSONValue(static_cast<unsigned int>(chat.outgoingCount));
        chatObj["total_count"] = new JSONValue(static_cast<unsigned int>(chat.incomingCount + chat.outgoingCount));
        chatObj["last_timestamp_ms"] = new JSONValue(static_cast<unsigned int>(chat.lastTimestampMs));
        chatsArray.push_back(new JSONValue(chatObj));
    }

    JSONObject dataObj;
    dataObj["count"] = new JSONValue(static_cast<unsigned int>(chats.size()));
    dataObj["chats"] = new JSONValue(chatsArray);

    JSONObject jsonObj;
    jsonObj["status"] = new JSONValue("ok");
    jsonObj["data"] = new JSONValue(dataObj);
    writeJson(res, jsonObj);
}

void htmlDeleteDir(const char *dirname)
{

    File root = FSCom.open(dirname);
    if (!root) {
        return;
    }
    if (!root.isDirectory()) {
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            htmlDeleteDir(file.name());
            file.flush();
            file.close();
        } else {
            String fileName = String(file.name());
            file.flush();
            file.close();
            LOG_DEBUG("    %s", fileName.c_str());
            FSCom.remove(fileName);
        }
        file = root.openNextFile();
    }
    root.flush();
    root.close();
}

JSONArray htmlListDir(const char *dirname, uint8_t levels)
{
    File root = FSCom.open(dirname, FILE_O_READ);
    JSONArray fileList;
    if (!root) {
        return fileList;
    }
    if (!root.isDirectory()) {
        return fileList;
    }

    // iterate over the file list
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                fileList.push_back(new JSONValue(htmlListDir(file.path(), levels - 1)));
#else
                fileList.push_back(new JSONValue(htmlListDir(file.name(), levels - 1)));
#endif
                file.close();
            }
        } else {
            JSONObject thisFileMap;
            thisFileMap["size"] = new JSONValue((int)file.size());
#ifdef ARCH_ESP32
            String fileName = String(file.path()).substring(1);
            thisFileMap["name"] = new JSONValue(fileName.c_str());
#else
            String fileName = String(file.name()).substring(1);
            thisFileMap["name"] = new JSONValue(fileName.c_str());
#endif
            String tempName = String(file.name()).substring(1);
            if (tempName.endsWith(".gz")) {
#ifdef ARCH_ESP32
                String modifiedFile = String(file.path()).substring(1);
#else
                String modifiedFile = String(file.name()).substring(1);
#endif
                modifiedFile.remove((modifiedFile.length() - 3), 3);
                thisFileMap["nameModified"] = new JSONValue(modifiedFile.c_str());
            }
            fileList.push_back(new JSONValue(thisFileMap));
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return fileList;
}

void handleFsBrowseStatic(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    concurrency::LockGuard g(spiLock);
    auto fileList = htmlListDir("/static", 10);

    // create json output structure
    JSONObject filesystemObj;
    filesystemObj["total"] = new JSONValue((int)FSCom.totalBytes());
    filesystemObj["used"] = new JSONValue((int)FSCom.usedBytes());
    filesystemObj["free"] = new JSONValue(int(FSCom.totalBytes() - FSCom.usedBytes()));

    JSONObject jsonObjInner;
    jsonObjInner["files"] = new JSONValue(fileList);
    jsonObjInner["filesystem"] = new JSONValue(filesystemObj);

    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(jsonObjInner);
    jsonObjOuter["status"] = new JSONValue("ok");

    JSONValue *value = new JSONValue(jsonObjOuter);

    std::string jsonString = value->Stringify();
    res->print(jsonString.c_str());

    delete value;

    // Clean up the fileList to prevent memory leak
    for (auto *val : fileList) {
        delete val;
    }
}

void handleFsDeleteStatic(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string paramValDelete;

    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "DELETE");

    if (params->getQueryParameter("delete", paramValDelete)) {
        std::string pathDelete = "/" + paramValDelete;
        concurrency::LockGuard g(spiLock);
        if (FSCom.remove(pathDelete.c_str())) {

            LOG_INFO("%s", pathDelete.c_str());
            JSONObject jsonObjOuter;
            jsonObjOuter["status"] = new JSONValue("ok");
            JSONValue *value = new JSONValue(jsonObjOuter);
            std::string jsonString = value->Stringify();
            res->print(jsonString.c_str());
            delete value;
            return;
        } else {

            LOG_INFO("%s", pathDelete.c_str());
            JSONObject jsonObjOuter;
            jsonObjOuter["status"] = new JSONValue("Error");
            JSONValue *value = new JSONValue(jsonObjOuter);
            std::string jsonString = value->Stringify();
            res->print(jsonString.c_str());
            delete value;
            return;
        }
    }
}

void handleStatic(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    std::string parameter1;
    // Print the first parameter value
    if (params->getPathParameter(0, parameter1)) {

        std::string filename = "/static/" + parameter1;
        std::string filenameGzip = "/static/" + parameter1 + ".gz";

        // Try to open the file
        File file;

        bool has_set_content_type = false;

        if (filename == "/static/") {
            filename = "/static/index.html";
            filenameGzip = "/static/index.html.gz";
        }

        concurrency::LockGuard g(spiLock);

        if (FSCom.exists(filename.c_str())) {
            file = FSCom.open(filename.c_str());
            if (!file.available()) {
                LOG_WARN("File not available - %s", filename.c_str());
            }
        } else if (FSCom.exists(filenameGzip.c_str())) {
            file = FSCom.open(filenameGzip.c_str());
            res->setHeader("Content-Encoding", "gzip");
            if (!file.available()) {
                LOG_WARN("File not available - %s", filenameGzip.c_str());
            }
        } else {
            has_set_content_type = true;
            filenameGzip = "/static/index.html.gz";
            file = FSCom.open(filenameGzip.c_str());
            res->setHeader("Content-Type", "text/html");
            if (!file.available()) {

                LOG_WARN("File not available - %s", filenameGzip.c_str());
                res->println("Web server is running.<br><br>The content you are looking for can't be found. Please see: <a "
                             "href=https://meshtastic.org/docs/software/web-client/>FAQ</a>.<br><br><a "
                             "href=/admin>admin</a>");

                return;
            } else {
                res->setHeader("Content-Encoding", "gzip");
            }
        }

        res->setHeader("Content-Length", httpsserver::intToString(file.size()));

        // Content-Type is guessed using the definition of the contentTypes-table defined above
        int cTypeIdx = 0;
        do {
            if (filename.rfind(contentTypes[cTypeIdx][0]) != std::string::npos) {
                res->setHeader("Content-Type", contentTypes[cTypeIdx][1]);
                has_set_content_type = true;
                break;
            }
            cTypeIdx += 1;
        } while (strlen(contentTypes[cTypeIdx][0]) > 0);

        if (!has_set_content_type) {
            // Set a default content type
            res->setHeader("Content-Type", "application/octet-stream");
        }

        // Read the file and write it to the HTTP response body
        size_t length = 0;
        do {
            char buffer[256];
            length = file.read((uint8_t *)buffer, 256);
            std::string bufferString(buffer, length);
            res->write((uint8_t *)bufferString.c_str(), bufferString.size());
        } while (length > 0);

        file.close();

        return;
    } else {
        LOG_ERROR("This should not have happened");
        res->println("ERROR: This should not have happened");
    }
}

void handleFormUpload(HTTPRequest *req, HTTPResponse *res)
{

    LOG_DEBUG("Form Upload - Disable keep-alive");
    res->setHeader("Connection", "close");

    // First, we need to check the encoding of the form that we have received.
    // The browser will set the Content-Type request header, so we can use it for that purpose.
    // Then we select the body parser based on the encoding.
    // Actually we do this only for documentary purposes, we know the form is going
    // to be multipart/form-data.
    LOG_DEBUG("Form Upload - Creating body parser reference");
    HTTPBodyParser *parser;
    std::string contentType = req->getHeader("Content-Type");

    // The content type may have additional properties after a semicolon, for example:
    // Content-Type: text/html;charset=utf-8
    // Content-Type: multipart/form-data;boundary=------s0m3w31rdch4r4c73rs
    // As we're interested only in the actual mime _type_, we strip everything after the
    // first semicolon, if one exists:
    size_t semicolonPos = contentType.find(";");
    if (semicolonPos != std::string::npos) {
        contentType.resize(semicolonPos);
    }

    // Now, we can decide based on the content type:
    if (contentType == "multipart/form-data") {
        LOG_DEBUG("Form Upload - multipart/form-data");
        parser = new HTTPMultipartBodyParser(req);
    } else {
        LOG_DEBUG("Unknown POST Content-Type: %s", contentType.c_str());
        return;
    }

    res->println("<html><head><meta http-equiv=\"refresh\" content=\"1;url=/static\" /><title>File "
                 "Upload</title></head><body><h1>File Upload</h1>");

    // We iterate over the fields. Any field with a filename is uploaded.
    // Note that the BodyParser consumes the request body, meaning that you can iterate over the request's
    // fields only a single time. The reason for this is that it allows you to handle large requests
    // which would not fit into memory.
    bool didwrite = false;

    // parser->nextField() will move the parser to the next field in the request body (field meaning a
    // form field, if you take the HTML perspective). After the last field has been processed, nextField()
    // returns false and the while loop ends.
    while (parser->nextField()) {
        // For Multipart data, each field has three properties:
        // The name ("name" value of the <input> tag)
        // The filename (If it was a <input type="file">, this is the filename on the machine of the
        //   user uploading it)
        // The mime type (It is determined by the client. So do not trust this value and blindly start
        //   parsing files only if the type matches)
        std::string name = parser->getFieldName();
        std::string filename = parser->getFieldFilename();
        std::string mimeType = parser->getFieldMimeType();
        // We log all three values, so that you can observe the upload on the serial monitor:
        LOG_DEBUG("handleFormUpload: field name='%s', filename='%s', mimetype='%s'", name.c_str(), filename.c_str(),
                  mimeType.c_str());

        // Double check that it is what we expect
        if (name != "file") {
            LOG_DEBUG("Skip unexpected field");
            res->println("<p>No file found.</p>");
            return;
        }

        // Double check that it is what we expect
        if (filename == "") {
            LOG_DEBUG("Skip unexpected field");
            res->println("<p>No file found.</p>");
            return;
        }

        // You should check file name validity and all that, but we skip that to make the core
        // concepts of the body parser functionality easier to understand.
        std::string pathname = "/static/" + filename;

        concurrency::LockGuard g(spiLock);
        // Create a new file to stream the data into
        File file = FSCom.open(pathname.c_str(), FILE_O_WRITE);
        size_t fileLength = 0;
        didwrite = true;

        // With endOfField you can check whether the end of field has been reached or if there's
        // still data pending. With multipart bodies, you cannot know the field size in advance.
        while (!parser->endOfField()) {
            esp_task_wdt_reset();

            byte buf[512];
            size_t readLength = parser->read(buf, 512);
            // LOG_DEBUG("readLength - %i", readLength);

            // Abort the transfer if there is less than 50k space left on the filesystem.
            if (FSCom.totalBytes() - FSCom.usedBytes() < 51200) {
                file.flush();
                file.close();
                res->println("<p>Write aborted! Reserving 50k on filesystem.</p>");

                // enableLoopWDT();

                delete parser;
                return;
            }

            // if (readLength) {
            file.write(buf, readLength);
            fileLength += readLength;
            LOG_DEBUG("File Length %i", fileLength);
            //}
        }
        // enableLoopWDT();

        file.flush();
        file.close();

        res->printf("<p>Saved %d bytes to %s</p>", (int)fileLength, pathname.c_str());
    }
    if (!didwrite) {
        res->println("<p>Did not write any file</p>");
    }
    res->println("</body></html>");
    delete parser;
}

void handleReport(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    // Helper lambda to create JSON array and clean up memory properly
    auto createJSONArrayFromLog = [](const uint32_t *logArray, int count) -> JSONValue * {
        JSONArray tempArray;
        for (int i = 0; i < count; i++) {
            tempArray.push_back(new JSONValue((int)logArray[i]));
        }
        JSONValue *result = new JSONValue(tempArray);
        // Note: Don't delete tempArray elements here - JSONValue now owns them
        return result;
    };

    // data->airtime->tx_log
    uint32_t *logArray;
    logArray = airTime->airtimeReport(TX_LOG);
    JSONValue *txLogJsonValue = createJSONArrayFromLog(logArray, airTime->getPeriodsToLog());

    // data->airtime->rx_log
    logArray = airTime->airtimeReport(RX_LOG);
    JSONValue *rxLogJsonValue = createJSONArrayFromLog(logArray, airTime->getPeriodsToLog());

    // data->airtime->rx_all_log
    logArray = airTime->airtimeReport(RX_ALL_LOG);
    JSONValue *rxAllLogJsonValue = createJSONArrayFromLog(logArray, airTime->getPeriodsToLog());

    // data->airtime
    JSONObject jsonObjAirtime;
    jsonObjAirtime["tx_log"] = txLogJsonValue;
    jsonObjAirtime["rx_log"] = rxLogJsonValue;
    jsonObjAirtime["rx_all_log"] = rxAllLogJsonValue;
    jsonObjAirtime["channel_utilization"] = new JSONValue(airTime->channelUtilizationPercent());
    jsonObjAirtime["utilization_tx"] = new JSONValue(airTime->utilizationTXPercent());
    jsonObjAirtime["seconds_since_boot"] = new JSONValue(int(airTime->getSecondsSinceBoot()));
    jsonObjAirtime["seconds_per_period"] = new JSONValue(int(airTime->getSecondsPerPeriod()));
    jsonObjAirtime["periods_to_log"] = new JSONValue(airTime->getPeriodsToLog());

    // data->wifi
    JSONObject jsonObjWifi;
    jsonObjWifi["rssi"] = new JSONValue(WiFi.RSSI());
    String wifiIPString = WiFi.localIP().toString();
    std::string wifiIP = wifiIPString.c_str();
    jsonObjWifi["ip"] = new JSONValue(wifiIP.c_str());

    // data->memory
    JSONObject jsonObjMemory;
    jsonObjMemory["heap_total"] = new JSONValue((int)memGet.getHeapSize());
    jsonObjMemory["heap_free"] = new JSONValue((int)memGet.getFreeHeap());
    jsonObjMemory["psram_total"] = new JSONValue((int)memGet.getPsramSize());
    jsonObjMemory["psram_free"] = new JSONValue((int)memGet.getFreePsram());
    spiLock->lock();
    jsonObjMemory["fs_total"] = new JSONValue((int)FSCom.totalBytes());
    jsonObjMemory["fs_used"] = new JSONValue((int)FSCom.usedBytes());
    jsonObjMemory["fs_free"] = new JSONValue(int(FSCom.totalBytes() - FSCom.usedBytes()));
    spiLock->unlock();

    // data->power
    JSONObject jsonObjPower;
    jsonObjPower["battery_percent"] = new JSONValue(powerStatus->getBatteryChargePercent());
    jsonObjPower["battery_voltage_mv"] = new JSONValue(powerStatus->getBatteryVoltageMv());
    jsonObjPower["has_battery"] = new JSONValue(BoolToString(powerStatus->getHasBattery()));
    jsonObjPower["has_usb"] = new JSONValue(BoolToString(powerStatus->getHasUSB()));
    jsonObjPower["is_charging"] = new JSONValue(BoolToString(powerStatus->getIsCharging()));

    // data->device
    JSONObject jsonObjDevice;
    jsonObjDevice["reboot_counter"] = new JSONValue((int)myNodeInfo.reboot_count);

    // data->radio
    JSONObject jsonObjRadio;
    jsonObjRadio["frequency"] = new JSONValue(RadioLibInterface::instance->getFreq());
    jsonObjRadio["lora_channel"] = new JSONValue((int)RadioLibInterface::instance->getChannelNum() + 1);

    // collect data to inner data object
    JSONObject jsonObjInner;
    jsonObjInner["airtime"] = new JSONValue(jsonObjAirtime);
    jsonObjInner["wifi"] = new JSONValue(jsonObjWifi);
    jsonObjInner["memory"] = new JSONValue(jsonObjMemory);
    jsonObjInner["power"] = new JSONValue(jsonObjPower);
    jsonObjInner["device"] = new JSONValue(jsonObjDevice);
    jsonObjInner["radio"] = new JSONValue(jsonObjRadio);

    // create json output structure
    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(jsonObjInner);
    jsonObjOuter["status"] = new JSONValue("ok");
    // serialize and write it to the stream
    JSONValue *value = new JSONValue(jsonObjOuter);
    std::string jsonString = value->Stringify();
    res->print(jsonString.c_str());
    delete value;
}

void handleNodes(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    JSONArray nodesArray;

    uint32_t readIndex = 0;
    const meshtastic_NodeInfoLite *tempNodeInfo = nodeDB->readNextMeshNode(readIndex);
    while (tempNodeInfo != NULL) {
        if (tempNodeInfo->has_user) {
            JSONObject node;

            char id[16];
            snprintf(id, sizeof(id), "!%08x", tempNodeInfo->num);

            node["id"] = new JSONValue(id);
            node["snr"] = new JSONValue(tempNodeInfo->snr);
            node["via_mqtt"] = new JSONValue(BoolToString(tempNodeInfo->via_mqtt));
            node["last_heard"] = new JSONValue((int)tempNodeInfo->last_heard);
            node["position"] = new JSONValue();

            if (nodeDB->hasValidPosition(tempNodeInfo)) {
                JSONObject position;
                position["latitude"] = new JSONValue((float)tempNodeInfo->position.latitude_i * 1e-7);
                position["longitude"] = new JSONValue((float)tempNodeInfo->position.longitude_i * 1e-7);
                position["altitude"] = new JSONValue((int)tempNodeInfo->position.altitude);
                node["position"] = new JSONValue(position);
            }

            node["long_name"] = new JSONValue(tempNodeInfo->user.long_name);
            node["short_name"] = new JSONValue(tempNodeInfo->user.short_name);
            char macStr[18];
            snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", tempNodeInfo->user.macaddr[0],
                     tempNodeInfo->user.macaddr[1], tempNodeInfo->user.macaddr[2], tempNodeInfo->user.macaddr[3],
                     tempNodeInfo->user.macaddr[4], tempNodeInfo->user.macaddr[5]);
            node["mac_address"] = new JSONValue(macStr);
            node["hw_model"] = new JSONValue(tempNodeInfo->user.hw_model);

            nodesArray.push_back(new JSONValue(node));
        }
        tempNodeInfo = nodeDB->readNextMeshNode(readIndex);
    }

    // collect data to inner data object
    JSONObject jsonObjInner;
    jsonObjInner["nodes"] = new JSONValue(nodesArray);

    // create json output structure
    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(jsonObjInner);
    jsonObjOuter["status"] = new JSONValue("ok");
    // serialize and write it to the stream
    JSONValue *value = new JSONValue(jsonObjOuter);
    std::string jsonString = value->Stringify();
    res->print(jsonString.c_str());
    delete value;

    // Clean up the nodesArray to prevent memory leak
    for (auto *val : nodesArray) {
        delete val;
    }
}

/*
    This supports the Apple Captive Network Assistant (CNA) Portal
*/
void handleHotspot(HTTPRequest *req, HTTPResponse *res)
{
    LOG_INFO("Hotspot Request");

    /*
        If we don't do a redirect, be sure to return a "Success" message
        otherwise iOS will have trouble detecting that the connection to the SoftAP worked.
    */

    // Status code is 200 OK by default.
    // We want to deliver a simple HTML page, so we send a corresponding content type:
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    // res->println("<!DOCTYPE html>");
    res->println("<meta http-equiv=\"refresh\" content=\"0;url=/\" />");
}

void handleDeleteFsContent(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("Delete Content in /static/*");

    LOG_INFO("Delete files from /static/* : ");

    concurrency::LockGuard g(spiLock);
    htmlDeleteDir("/static");

    res->println("<p><hr><p><a href=/admin>Back to admin</a>");
}

void handleAdmin(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("<a href=/admin/telegram>Telegram Bridge</a><br>");
    //    res->println("<a href=/admin/settings>Settings</a><br>");
    //    res->println("<a href=/admin/fs>Manage Web Content</a><br>");
    res->println("<a href=/json/report>Device Report</a><br>");
}

void handleAdminTelegram(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread) {
        webServerThread->markActivity();
    }

    res->setHeader("Content-Type", "text/html; charset=utf-8");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->print(R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Meshtastic Telegram Bridge</title>
  <style>
    :root {
      --bg: #f3f5f9;
      --panel: #ffffff;
      --text: #1c2431;
      --muted: #536173;
      --line: #d9e0ec;
      --primary: #0f766e;
      --primary-soft: #d3f3ee;
      --danger: #b42318;
      --radius: 12px;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      padding: 24px;
      font-family: "IBM Plex Sans", "Segoe UI", sans-serif;
      color: var(--text);
      background: radial-gradient(circle at top right, #d8edf1, var(--bg) 45%);
    }
    main { max-width: 1100px; margin: 0 auto; }
    h1 { margin: 0 0 8px; font-size: 1.8rem; }
    p.hint { margin: 0 0 18px; color: var(--muted); }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
      gap: 16px;
    }
    .card {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: var(--radius);
      padding: 16px;
      box-shadow: 0 8px 24px rgba(8, 21, 45, 0.06);
    }
    .full { grid-column: 1 / -1; }
    .row { display: grid; gap: 10px; margin-bottom: 10px; }
    .row.two { grid-template-columns: 1fr 1fr; }
    label { font-size: 0.85rem; color: var(--muted); display: block; margin-bottom: 4px; }
    input, select, button {
      width: 100%;
      border: 1px solid var(--line);
      border-radius: 10px;
      padding: 10px 12px;
      font-size: 0.95rem;
      font-family: inherit;
      background: #fff;
      color: var(--text);
    }
    input:focus, select:focus {
      outline: 2px solid rgba(15, 118, 110, 0.25);
      border-color: var(--primary);
    }
    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      margin-top: 10px;
    }
    button {
      width: auto;
      min-width: 120px;
      border: none;
      background: var(--primary);
      color: #fff;
      cursor: pointer;
      transition: transform 120ms ease, opacity 120ms ease;
    }
    button.secondary { background: #445267; }
    button.danger { background: var(--danger); }
    button:hover { transform: translateY(-1px); opacity: 0.95; }
    .status {
      margin: 10px 0 0;
      padding: 10px 12px;
      border-radius: 10px;
      background: var(--primary-soft);
      color: #104540;
      font-size: 0.9rem;
    }
    .status.error {
      background: #fee4e2;
      color: #7a271a;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.86rem;
    }
    th, td {
      border-bottom: 1px solid var(--line);
      text-align: left;
      padding: 8px 6px;
      vertical-align: top;
    }
    th { color: var(--muted); font-weight: 600; }
    td.msg { word-break: break-word; white-space: pre-wrap; max-width: 360px; }
    @media (max-width: 720px) {
      body { padding: 14px; }
      .row.two { grid-template-columns: 1fr; }
      td.msg { max-width: 220px; }
    }
  </style>
</head>
<body>
<main>
  <h1>Telegram Bridge</h1>
  <p class="hint">HTTP management + chat message history.</p>

  <div class="grid">
    <section class="card">
      <h2>Configuration</h2>
      <form id="configForm">
        <div class="row two">
          <div>
            <label for="enabled">Enabled</label>
            <select id="enabled" name="enabled">
              <option value="true">On</option>
              <option value="false">Off</option>
            </select>
          </div>
          <div>
            <label for="direction">Direction</label>
            <select id="direction" name="direction">
              <option value="both">both</option>
              <option value="mesh_to_telegram">mesh_to_telegram</option>
              <option value="telegram_to_mesh">telegram_to_mesh</option>
            </select>
          </div>
        </div>
        <div class="row">
          <div>
            <label for="token">Bot Token (leave empty to keep current)</label>
            <input id="token" name="token" type="password" autocomplete="off" />
          </div>
        </div>
        <div class="row two">
          <div>
            <label for="chat_id">Chat ID</label>
            <input id="chat_id" name="chat_id" type="text" />
          </div>
          <div>
            <label for="channels">Channels (empty = all)</label>
            <input id="channels" name="channels" type="text" placeholder="0,1,2" />
          </div>
        </div>
        <div class="row two">
          <div>
            <label for="poll_interval_ms">Poll interval (ms)</label>
            <input id="poll_interval_ms" name="poll_interval_ms" type="number" min="200" step="100" />
          </div>
          <div>
            <label for="send_interval_ms">Send interval (ms)</label>
            <input id="send_interval_ms" name="send_interval_ms" type="number" min="200" step="100" />
          </div>
        </div>
        <div class="row">
          <div>
            <label for="long_poll_timeout_sec">Long poll timeout (sec)</label>
            <input id="long_poll_timeout_sec" name="long_poll_timeout_sec" type="number" min="0" step="1" />
          </div>
        </div>
        <div class="actions">
          <button type="submit">Save</button>
          <button type="button" class="secondary" id="reloadConfig">Reload</button>
        </div>
      </form>
      <div id="configStatus" class="status">Ready.</div>
    </section>

    <section class="card full">
      <h2>Message History</h2>
      <div class="row two">
        <div>
          <label for="historyChat">Chat</label>
          <select id="historyChat">
            <option value="">All chats</option>
          </select>
        </div>
        <div>
          <label for="historyDirection">Direction</label>
          <select id="historyDirection">
            <option value="both">both</option>
            <option value="incoming">incoming</option>
            <option value="outgoing">outgoing</option>
          </select>
        </div>
      </div>
      <div class="row two">
        <div>
          <label for="historyLimit">Limit</label>
          <input id="historyLimit" type="number" min="1" max="200" value="50" />
        </div>
      </div>
      <div class="actions">
        <button type="button" id="refreshHistory">Refresh</button>
        <button type="button" class="danger" id="clearHistory">Clear history</button>
      </div>
      <div id="historyStatus" class="status">No data loaded.</div>
      <div style="overflow:auto; margin-top: 10px;">
        <table>
          <thead>
            <tr>
              <th>Timestamp ms</th>
              <th>Chat</th>
              <th>Direction</th>
              <th>Status</th>
              <th>Sender</th>
              <th>Message</th>
            </tr>
          </thead>
          <tbody id="historyBody">
            <tr><td colspan="6">No entries</td></tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>

  <p style="margin-top:16px;"><a href="/admin">Back to admin</a></p>
</main>

<script>
  async function fetchJson(url, options) {
    const response = await fetch(url, options || {});
    let body = null;
    try { body = await response.json(); } catch (_) { body = null; }
    if (!response.ok) {
      const msg = body && body.error && body.error.message ? body.error.message : ("HTTP " + response.status);
      throw new Error(msg);
    }
    return body;
  }

  function setStatus(id, message, isError) {
    const node = document.getElementById(id);
    node.textContent = message;
    node.classList.toggle("error", Boolean(isError));
  }

  function escapeHtml(text) {
    return text
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;");
  }

  async function loadConfig() {
    try {
      const payload = await fetchJson("/api/v1/telegram/config");
      const cfg = payload.data.config;
      document.getElementById("enabled").value = cfg.enabled ? "true" : "false";
      document.getElementById("direction").value = cfg.direction || "both";
      document.getElementById("chat_id").value = cfg.chat_id || "";
      document.getElementById("channels").value = cfg.channels || "";
      document.getElementById("poll_interval_ms").value = cfg.poll_interval_ms || 0;
      document.getElementById("send_interval_ms").value = cfg.send_interval_ms || 0;
      document.getElementById("long_poll_timeout_sec").value = cfg.long_poll_timeout_sec || 0;
      document.getElementById("token").value = "";
      setStatus("configStatus", cfg.feature_available ? "Configuration loaded." : "Telegram feature unavailable in this build.", !cfg.feature_available);
    } catch (err) {
      setStatus("configStatus", err.message, true);
    }
  }

  async function saveConfig(event) {
    event.preventDefault();
    const body = new URLSearchParams();
    body.set("enabled", document.getElementById("enabled").value);
    body.set("direction", document.getElementById("direction").value);
    body.set("chat_id", document.getElementById("chat_id").value.trim());
    body.set("channels", document.getElementById("channels").value.trim());
    body.set("poll_interval_ms", document.getElementById("poll_interval_ms").value);
    body.set("send_interval_ms", document.getElementById("send_interval_ms").value);
    body.set("long_poll_timeout_sec", document.getElementById("long_poll_timeout_sec").value);
    const token = document.getElementById("token").value.trim();
    if (token.length > 0) {
      body.set("token", token);
    }

    try {
      const payload = await fetchJson("/api/v1/telegram/config", {
        method: "PUT",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body
      });
      setStatus("configStatus", payload.data.result.message || "Saved", false);
      await loadConfig();
      await loadChats();
      await loadHistory();
    } catch (err) {
      setStatus("configStatus", err.message, true);
    }
  }

  async function loadChats() {
    try {
      const payload = await fetchJson("/api/v1/telegram/history/chats?limit=100");
      const chats = payload.data.chats || [];
      const select = document.getElementById("historyChat");
      const previous = select.value;
      select.innerHTML = "<option value=''>All chats</option>";
      chats.forEach((chat) => {
        const option = document.createElement("option");
        option.value = chat.chat_id;
        option.textContent = `${chat.chat_id} (in:${chat.incoming_count}, out:${chat.outgoing_count})`;
        select.appendChild(option);
      });
      select.value = previous;
    } catch (err) {
      setStatus("historyStatus", err.message, true);
    }
  }

  async function loadHistory() {
    const chat = document.getElementById("historyChat").value;
    const direction = document.getElementById("historyDirection").value;
    const limit = document.getElementById("historyLimit").value || "50";
    const query = new URLSearchParams();
    if (chat) query.set("chat_id", chat);
    query.set("direction", direction);
    query.set("limit", limit);

    try {
      const payload = await fetchJson(`/api/v1/telegram/history?${query.toString()}`);
      const messages = payload.data.messages || [];
      const body = document.getElementById("historyBody");
      if (messages.length === 0) {
        body.innerHTML = "<tr><td colspan='6'>No entries</td></tr>";
      } else {
        body.innerHTML = messages.map((entry) => {
          const sender = entry.sender ? escapeHtml(entry.sender) : "-";
          return `<tr>
            <td>${entry.timestamp_ms}</td>
            <td>${escapeHtml(entry.chat_id)}</td>
            <td>${escapeHtml(entry.direction)}</td>
            <td>${escapeHtml(entry.status)}</td>
            <td>${sender}</td>
            <td class='msg'>${escapeHtml(entry.text || "")}</td>
          </tr>`;
        }).join("");
      }
      setStatus("historyStatus", `Loaded ${messages.length} entr${messages.length === 1 ? "y" : "ies"}.`, false);
    } catch (err) {
      setStatus("historyStatus", err.message, true);
    }
  }

  async function clearHistory() {
    if (!confirm("Clear all Telegram history entries?")) {
      return;
    }

    try {
      await fetchJson("/api/v1/telegram/history", { method: "DELETE" });
      await loadChats();
      await loadHistory();
      setStatus("historyStatus", "History cleared.", false);
    } catch (err) {
      setStatus("historyStatus", err.message, true);
    }
  }

  document.getElementById("configForm").addEventListener("submit", saveConfig);
  document.getElementById("reloadConfig").addEventListener("click", loadConfig);
  document.getElementById("refreshHistory").addEventListener("click", loadHistory);
  document.getElementById("clearHistory").addEventListener("click", clearHistory);
  document.getElementById("historyChat").addEventListener("change", loadHistory);
  document.getElementById("historyDirection").addEventListener("change", loadHistory);

  Promise.resolve()
    .then(loadConfig)
    .then(loadChats)
    .then(loadHistory);
</script>
</body>
</html>)HTML");
}

void handleAdminSettings(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("This isn't done.");
    res->println("<form action=/admin/settings/apply method=post>");
    res->println("<table border=1>");
    res->println("<tr><td>Set?</td><td>Setting</td><td>current value</td><td>new value</td></tr>");
    res->println("<tr><td><input type=checkbox></td><td>WiFi SSID</td><td>false</td><td><input type=radio></td></tr>");
    res->println("<tr><td><input type=checkbox></td><td>WiFi Password</td><td>false</td><td><input type=radio></td></tr>");
    res->println(
        "<tr><td><input type=checkbox></td><td>Smart Position Update</td><td>false</td><td><input type=radio></td></tr>");
    res->println("</table>");
    res->println("<table>");
    res->println("<input type=submit value=Apply New Settings>");
    res->println("<form>");
    res->println("<p><hr><p><a href=/admin>Back to admin</a>");
}

void handleAdminSettingsApply(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "POST");
    res->println("<h1>Meshtastic</h1>");
    res->println(
        "<html><head><meta http-equiv=\"refresh\" content=\"1;url=/admin/settings\" /><title>Settings Applied. </title>");

    res->println("Settings Applied. Please wait.");
}

void handleFs(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("<a href=/admin/fs/delete>Delete Web Content</a><p><form action=/admin/fs/update "
                 "method=post><input type=submit value=UPDATE_WEB_CONTENT></form>Be patient!");
    res->println("<p><hr><p><a href=/admin>Back to admin</a>");
}

void handleRestart(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("Restarting");

    LOG_DEBUG("Restarted on HTTP(s) Request");
    webServerThread->requestRestart = (millis() / 1000) + 5;
}

void handleScanNetworks(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    // res->setHeader("Content-Type", "text/html");

    int n = WiFi.scanNetworks();

    // build list of network objects
    JSONArray networkObjs;
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            char ssidArray[50];
            String ssidString = String(WiFi.SSID(i));
            ssidString.replace("\"", "\\\"");
            ssidString.toCharArray(ssidArray, 50);

            if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                JSONObject thisNetwork;
                thisNetwork["ssid"] = new JSONValue(ssidArray);
                thisNetwork["rssi"] = new JSONValue(int(WiFi.RSSI(i)));
                networkObjs.push_back(new JSONValue(thisNetwork));
            }
            // Yield some cpu cycles to IP stack.
            //   This is important in case the list is large and it takes us time to return
            //   to the main loop.
            yield();
        }
    }

    // build output structure
    JSONObject jsonObjOuter;
    jsonObjOuter["data"] = new JSONValue(networkObjs);
    jsonObjOuter["status"] = new JSONValue("ok");

    // serialize and write it to the stream
    JSONValue *value = new JSONValue(jsonObjOuter);
    std::string jsonString = value->Stringify();
    res->print(jsonString.c_str());
    delete value;

    // Clean up the networkObjs to prevent memory leak
    for (auto *val : networkObjs) {
        delete val;
    }
}
#endif
