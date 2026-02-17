#include "telegram/TelegramBridge.h"

#if !MESHTASTIC_EXCLUDE_TELEGRAM && HAS_WIFI && defined(ARCH_ESP32)

#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "modules/TextMessageModule.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

TelegramBridge *telegramBridge = nullptr;

namespace
{
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 1000;
constexpr uint32_t RUN_INTERVAL_MS = 200;
constexpr uint32_t HEAP_WARN_INTERVAL_MS = 10000;
constexpr uint32_t MAX_BACKOFF_MS = 60000;
constexpr size_t TELEGRAM_UPDATE_BATCH_SIZE = 8;

size_t utf8SafePrefixLength(const std::string &value, size_t maxBytes)
{
    if (value.size() <= maxBytes)
        return value.size();

    size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
        end--;
    }

    if (end == 0)
        return maxBytes;

    return end;
}

} // namespace

void telegramInit()
{
    if (!telegramBridge) {
        telegramBridge = new TelegramBridge();
    }
}

TelegramBridge::TelegramBridge() : concurrency::OSThread("telegram"), messageQueue(TELEGRAM_MAX_QUEUE_SIZE)
{
    messageQueue.setReader(this);
    loadConfig();

    if (textMessageModule != nullptr) {
        observe(textMessageModule);
    }

    if (!api.isConfigured() || !hasConfiguredChatId) {
        LOG_INFO("Telegram bridge disabled: missing bot token or chat_id");
        state = State::STATE_DISABLED;
        disable();
        return;
    }

    state = State::STATE_WAIT_WIFI;
    LOG_INFO("Telegram bridge initialized");
}

int32_t TelegramBridge::runOnce()
{
    if (state == State::STATE_DISABLED)
        return disable();

    if (!isWifiConnected()) {
        if (state != State::STATE_WAIT_WIFI) {
            LOG_INFO("Telegram bridge waiting for WiFi");
        }
        state = State::STATE_WAIT_WIFI;
        return WIFI_RETRY_INTERVAL_MS;
    }

    if (state == State::STATE_WAIT_WIFI) {
        state = State::STATE_RUNNING;
        LOG_INFO("Telegram bridge running");
    }

    sendQueuedMessages();

    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - lastPollAtMs) >= pollIntervalMs) {
        processIncomingTelegram();
        lastPollAtMs = now;
    }

    if (static_cast<uint32_t>(now - lastHeapWarnMs) >= HEAP_WARN_INTERVAL_MS) {
        lastHeapWarnMs = now;
        const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (freeHeap < MINIMUM_SAFE_FREE_HEAP) {
            LOG_WARN("Telegram bridge low heap: %u", freeHeap);
        }
    }

    return RUN_INTERVAL_MS;
}

int TelegramBridge::onNotify(const meshtastic_MeshPacket *packet)
{
    if (packet == nullptr || state == State::STATE_DISABLED)
        return 0;

    if (!isChannelAllowed(packet->channel))
        return 0;

    if (isSelfInjected(packet->id))
        return 0;

    const std::string formatted = formatMeshMessage(packet);
    if (!formatted.empty()) {
        enqueueTelegramMessage(formatted);
    }

    return 0;
}

void TelegramBridge::loadConfig()
{
    token = TELEGRAM_BOT_TOKEN;
    chatId = TELEGRAM_CHAT_ID;
    channelsConfig = TELEGRAM_CHANNELS;
    pollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
    longPollTimeoutSec = TELEGRAM_LONG_POLL_TIMEOUT;
    sendIntervalMs = TELEGRAM_SEND_INTERVAL_MS;

    Preferences prefs;
    if (prefs.begin("telegram", true)) {
        token = prefs.getString("bot_token", token.c_str()).c_str();
        chatId = prefs.getString("chat_id", chatId.c_str()).c_str();
        channelsConfig = prefs.getString("channels", channelsConfig.c_str()).c_str();
        prefs.end();
    }

    api.setToken(token.c_str());
    hasConfiguredChatId = parseChatId(chatId, configuredChatId);
    applyChannelsConfig(channelsConfig, false);
}

bool TelegramBridge::parseChatId(const std::string &rawChatId, int64_t &outChatId) const
{
    const std::string trimmed = trim(rawChatId);
    if (trimmed.empty())
        return false;

    char *end = nullptr;
    const long long parsed = strtoll(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || (end != nullptr && *end != '\0'))
        return false;

    outChatId = static_cast<int64_t>(parsed);
    return true;
}

bool TelegramBridge::saveChannelsConfig(const std::string &rawChannels)
{
    Preferences prefs;
    if (!prefs.begin("telegram", false)) {
        LOG_WARN("Telegram bridge failed to open NVS for channels");
        return false;
    }

    prefs.putString("channels", rawChannels.c_str());
    prefs.end();
    return true;
}

std::string TelegramBridge::trim(const std::string &value)
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

bool TelegramBridge::parseChannelNumber(const std::string &tokenText, uint8_t &channelNumber)
{
    if (tokenText.empty())
        return false;

    char *end = nullptr;
    const unsigned long parsed = strtoul(tokenText.c_str(), &end, 10);
    if (end == tokenText.c_str() || (end != nullptr && *end != '\0') || parsed > UINT8_MAX)
        return false;

    channelNumber = static_cast<uint8_t>(parsed);
    return true;
}

bool TelegramBridge::applyChannelsConfig(const std::string &rawChannels, bool persist)
{
    const std::string normalized = trim(rawChannels);
    if (normalized.empty()) {
        allowAllChannels = true;
        allowedChannels.clear();
        telegramToMeshChannel = channels.getPrimaryIndex();
        channelsConfig.clear();

        if (persist) {
            saveChannelsConfig(channelsConfig);
        }
        return true;
    }

    std::set<uint8_t> parsedChannels;
    size_t offset = 0;
    while (offset <= normalized.size()) {
        const size_t comma = normalized.find(',', offset);
        const std::string tokenText = trim(normalized.substr(offset, comma == std::string::npos ? std::string::npos : comma - offset));

        uint8_t channel = 0;
        if (!parseChannelNumber(tokenText, channel)) {
            return false;
        }

        if (channel >= channels.getNumChannels()) {
            return false;
        }

        parsedChannels.insert(channel);
        if (comma == std::string::npos)
            break;
        offset = comma + 1;
    }

    if (parsedChannels.empty()) {
        return false;
    }

    allowAllChannels = false;
    allowedChannels = parsedChannels;
    telegramToMeshChannel = *allowedChannels.begin();
    channelsConfig = normalized;

    if (persist) {
        saveChannelsConfig(channelsConfig);
    }

    return true;
}

bool TelegramBridge::isChannelAllowed(uint8_t channel) const
{
    return allowAllChannels || allowedChannels.find(channel) != allowedChannels.end();
}

std::string TelegramBridge::truncateUtf8(const std::string &value, size_t maxBytes)
{
    return value.substr(0, utf8SafePrefixLength(value, maxBytes));
}

std::string TelegramBridge::htmlEscape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 16);

    for (char c : value) {
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }

    return escaped;
}

bool TelegramBridge::isWifiConnected() const
{
    return WiFi.isConnected();
}

bool TelegramBridge::matchesConfiguredChat(int64_t incomingChatId) const
{
    return hasConfiguredChatId && incomingChatId == configuredChatId;
}

bool TelegramBridge::isSelfInjected(uint32_t packetId) const
{
    for (size_t i = 0; i < SELF_INJECTED_ID_COUNT; ++i) {
        if (selfInjectedIds[i] == packetId && packetId != 0) {
            return true;
        }
    }
    return false;
}

void TelegramBridge::rememberSelfInjected(uint32_t packetId)
{
    selfInjectedIds[selfInjectedIndex] = packetId;
    selfInjectedIndex = (selfInjectedIndex + 1) % SELF_INJECTED_ID_COUNT;
}

void TelegramBridge::enqueueTelegramMessage(const std::string &text)
{
    std::string payload = truncateUtf8(text, TELEGRAM_MAX_TEXT_SIZE);
    if (payload.empty())
        return;

    if (messageQueue.numFree() == 0) {
        QueueEntry *dropped = messageQueue.dequeuePtr(0);
        if (dropped) {
            delete dropped;
        }
        LOG_WARN("Telegram queue full, dropped oldest message");
    }

    QueueEntry *entry = new QueueEntry();
    entry->text = std::move(payload);

    if (!messageQueue.enqueue(entry, 0)) {
        LOG_WARN("Telegram queue enqueue failed");
        delete entry;
    }
}

void TelegramBridge::sendQueuedMessages()
{
    if (!api.isConfigured() || chatId.empty())
        return;

    const uint32_t now = millis();
    if (nextRetryAtMs != 0 && static_cast<int32_t>(now - nextRetryAtMs) < 0)
        return;

    if (static_cast<uint32_t>(now - lastSendAtMs) < sendIntervalMs)
        return;

    if (!hasPendingMessage) {
        QueueEntry *entry = messageQueue.dequeuePtr(0);
        if (!entry)
            return;

        pendingMessage = std::move(entry->text);
        delete entry;
        hasPendingMessage = true;
    }

    if (api.sendMessage(chatId, pendingMessage)) {
        hasPendingMessage = false;
        pendingMessage.clear();
        consecutiveSendErrors = 0;
        nextRetryAtMs = 0;
        lastSendAtMs = now;
        return;
    }

    consecutiveSendErrors = std::min<uint8_t>(consecutiveSendErrors + 1, 10);

    uint32_t backoffMs = sendIntervalMs;
    for (uint8_t i = 0; i < consecutiveSendErrors; ++i) {
        if (backoffMs >= (MAX_BACKOFF_MS / 2)) {
            backoffMs = MAX_BACKOFF_MS;
            break;
        }
        backoffMs *= 2;
    }

    nextRetryAtMs = now + backoffMs;
    LOG_WARN("Telegram send failed, retry in %u ms", backoffMs);
}

void TelegramBridge::processIncomingTelegram()
{
    TelegramMessage updates[TELEGRAM_UPDATE_BATCH_SIZE];
    const int count = api.getUpdates(updates, TELEGRAM_UPDATE_BATCH_SIZE, longPollTimeoutSec);
    if (count <= 0)
        return;

    for (int i = 0; i < count; ++i) {
        const TelegramMessage &message = updates[i];
        if (!matchesConfiguredChat(message.chat_id)) {
            continue;
        }

        if (handleTelegramCommand(message)) {
            continue;
        }

        injectToMesh(message.text, message.from_name);
    }
}

bool TelegramBridge::handleTelegramCommand(const TelegramMessage &message)
{
    const std::string command = trim(message.text);
    if (command.empty())
        return false;

    if (command == "/ping") {
        enqueueTelegramMessage("pong");
        return true;
    }

    if (command == "/status") {
        enqueueTelegramMessage(buildStatusMessage());
        return true;
    }

    static const std::string channelsCommand = "/config channels";
    if (command.rfind(channelsCommand, 0) == 0) {
        const std::string value = trim(command.substr(channelsCommand.size()));
        if (!applyChannelsConfig(value, true)) {
            enqueueTelegramMessage("Invalid channels. Use: /config channels 0,1,3");
            return true;
        }

        if (allowAllChannels) {
            enqueueTelegramMessage("Channels updated: all");
        } else {
            enqueueTelegramMessage("Channels updated: " + channelsConfig);
        }
        return true;
    }

    return false;
}

std::string TelegramBridge::buildStatusMessage()
{
    std::string status = "<b>Telegram bridge</b>\n";
    status += "state: ";
    switch (state) {
    case State::STATE_RUNNING:
        status += "running";
        break;
    case State::STATE_WAIT_WIFI:
        status += "wait_wifi";
        break;
    case State::STATE_DISABLED:
    default:
        status += "disabled";
        break;
    }
    status += "\nwifi: ";
    status += isWifiConnected() ? "connected" : "disconnected";
    status += "\nqueue: ";
    status += std::to_string(messageQueue.numUsed());
    status += "/";
    status += std::to_string(TELEGRAM_MAX_QUEUE_SIZE);
    status += "\nchannels: ";
    status += allowAllChannels ? "all" : channelsConfig;

    return truncateUtf8(status, TELEGRAM_MAX_TEXT_SIZE);
}

std::string TelegramBridge::formatMeshMessage(const meshtastic_MeshPacket *packet) const
{
    if (packet == nullptr || packet->decoded.payload.size == 0)
        return std::string();

    std::string nodeName;
    std::string shortName;

    const uint32_t from = getFrom(packet);
    const meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(from);
    if (sender && sender->has_user) {
        if (std::strlen(sender->user.long_name) > 0)
            nodeName = sender->user.long_name;
        if (std::strlen(sender->user.short_name) > 0)
            shortName = sender->user.short_name;
    }

    if (nodeName.empty()) {
        char fallback[16] = {0};
        snprintf(fallback, sizeof(fallback), "0x%08x", from);
        nodeName = fallback;
    }

    if (shortName.empty())
        shortName = nodeName;

    std::string payload(reinterpret_cast<const char *>(packet->decoded.payload.bytes), packet->decoded.payload.size);
    payload = truncateUtf8(payload, TELEGRAM_MAX_TEXT_SIZE);

    std::string formatted = "<b>";
    formatted += htmlEscape(nodeName);
    formatted += "</b> [";
    formatted += htmlEscape(shortName);
    formatted += "] (ch ";
    formatted += std::to_string(packet->channel);
    formatted += "):\n";
    formatted += htmlEscape(payload);

    return truncateUtf8(formatted, TELEGRAM_MAX_TEXT_SIZE);
}

bool TelegramBridge::injectToMesh(const std::string &text, const std::string &senderName)
{
    if (service == nullptr || router == nullptr)
        return false;

    if (text.empty())
        return false;

    std::string sender = trim(senderName);
    if (sender.empty())
        sender = "Telegram";

    std::string prefix = "[TG|" + sender + "] ";
    prefix = truncateUtf8(prefix, meshtastic_Constants_DATA_PAYLOAD_LEN);

    const size_t available = (meshtastic_Constants_DATA_PAYLOAD_LEN > prefix.size())
                                 ? (meshtastic_Constants_DATA_PAYLOAD_LEN - prefix.size())
                                 : 0;
    std::string payload = prefix + truncateUtf8(text, available);
    if (payload.empty())
        return false;

    meshtastic_MeshPacket *packet = router->allocForSending();
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet->channel = telegramToMeshChannel;
    packet->decoded.payload.size = payload.size();
    memcpy(packet->decoded.payload.bytes, payload.data(), payload.size());

    rememberSelfInjected(packet->id);
    service->sendToMesh(packet, RX_SRC_LOCAL);
    return true;
}

#endif
