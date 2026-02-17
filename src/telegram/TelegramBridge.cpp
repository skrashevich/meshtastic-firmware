#include "telegram/TelegramBridge.h"

#if !MESHTASTIC_EXCLUDE_TELEGRAM && HAS_WIFI && defined(ARCH_ESP32)

#include "concurrency/LockGuard.h"
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
constexpr uint32_t MIN_POLL_INTERVAL_MS = 200;
constexpr uint32_t MAX_POLL_INTERVAL_MS = 60000;
constexpr uint32_t MIN_SEND_INTERVAL_MS = 200;
constexpr uint32_t MAX_SEND_INTERVAL_MS = 10000;
constexpr uint32_t MAX_LONG_POLL_TIMEOUT_SEC = 60;
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

uint32_t normalizePollInterval(uint32_t value)
{
    if (value < MIN_POLL_INTERVAL_MS || value > MAX_POLL_INTERVAL_MS) {
        return TELEGRAM_POLL_INTERVAL_MS;
    }
    return value;
}

uint32_t normalizeSendInterval(uint32_t value)
{
    if (value < MIN_SEND_INTERVAL_MS || value > MAX_SEND_INTERVAL_MS) {
        return TELEGRAM_SEND_INTERVAL_MS;
    }
    return value;
}

uint32_t normalizeLongPollTimeout(uint32_t value)
{
    if (value > MAX_LONG_POLL_TIMEOUT_SEC) {
        return TELEGRAM_LONG_POLL_TIMEOUT;
    }
    return value;
}

const char *sourceToString(TelegramControlSource source)
{
    switch (source) {
    case TelegramControlSource::DEVICE_UI:
        return "device-ui";
    case TelegramControlSource::TELEGRAM_CHAT:
        return "telegram-chat";
    case TelegramControlSource::HTTP_API:
        return "http-api";
    case TelegramControlSource::SERIAL_API:
        return "serial-api";
    case TelegramControlSource::OTHER:
        return "other";
    case TelegramControlSource::UNKNOWN:
    default:
        return "unknown";
    }
}

} // namespace

void telegramInit()
{
    if (!telegramBridge) {
        telegramBridge = new TelegramBridge();
    }
}

TelegramControlSnapshot telegramGetControlSnapshot()
{
    if (!telegramBridge) {
        TelegramControlSnapshot snapshot;
        snapshot.featureAvailable = true;
        snapshot.queueCapacity = TELEGRAM_MAX_QUEUE_SIZE;
        return snapshot;
    }

    return telegramBridge->getControlSnapshot();
}

TelegramControlResult telegramApplyControlPatch(const TelegramControlPatch &patch, TelegramControlSource source)
{
    if (!telegramBridge) {
        TelegramControlResult result;
        result.error = TelegramControlError::NOT_AVAILABLE;
        result.message = "Telegram bridge is not initialized";
        return result;
    }

    return telegramBridge->applyControlPatch(patch, source);
}

TelegramControlResult telegramSetEnabled(bool enabled, TelegramControlSource source)
{
    if (!telegramBridge) {
        TelegramControlResult result;
        result.error = TelegramControlError::NOT_AVAILABLE;
        result.message = "Telegram bridge is not initialized";
        return result;
    }

    return telegramBridge->setEnabled(enabled, source);
}

TelegramBridge::TelegramBridge() : concurrency::OSThread("telegram"), messageQueue(TELEGRAM_MAX_QUEUE_SIZE)
{
    messageQueue.setReader(this);
    loadConfig();

    if (textMessageModule != nullptr) {
        observe(textMessageModule);
    }

    {
        concurrency::LockGuard guard(&configLock);
        refreshOperationalStateLocked();
    }

    LOG_INFO("Telegram bridge initialized");
}

TelegramControlSnapshot TelegramBridge::getControlSnapshot()
{
    TelegramControlSnapshot snapshot;
    snapshot.featureAvailable = true;

    concurrency::LockGuard guard(&configLock);

    snapshot.enabled = bridgeEnabled;
    snapshot.running = (state == State::STATE_RUNNING);
    snapshot.configured = isConfiguredLocked();
    snapshot.wifiConnected = isWifiConnected();

    snapshot.allowAllChannels = allowAllChannels;
    snapshot.channels = channelsConfig;
    snapshot.meshChannelForInject = telegramToMeshChannel;

    snapshot.queueUsed = static_cast<uint16_t>(messageQueue.numUsed());
    snapshot.queueCapacity = TELEGRAM_MAX_QUEUE_SIZE;

    snapshot.pollIntervalMs = pollIntervalMs;
    snapshot.longPollTimeoutSec = longPollTimeoutSec;
    snapshot.sendIntervalMs = sendIntervalMs;

    snapshot.hasToken = !token.empty();
    snapshot.hasChatId = hasConfiguredChatId;
    snapshot.chatId = chatId;

    return snapshot;
}

TelegramControlResult TelegramBridge::setEnabled(bool enabledSetting, TelegramControlSource source)
{
    TelegramControlPatch patch;
    patch.hasEnabled = true;
    patch.enabled = enabledSetting;
    return applyControlPatch(patch, source);
}

TelegramControlResult TelegramBridge::applyControlPatch(const TelegramControlPatch &patch, TelegramControlSource source)
{
    TelegramControlResult result;
    bool changed = false;

    concurrency::LockGuard guard(&configLock);

    if (patch.hasPollIntervalMs) {
        if (patch.pollIntervalMs < MIN_POLL_INTERVAL_MS || patch.pollIntervalMs > MAX_POLL_INTERVAL_MS) {
            result.error = TelegramControlError::INVALID_ARGUMENT;
            result.message = "pollIntervalMs out of range";
            return result;
        }
    }

    if (patch.hasSendIntervalMs) {
        if (patch.sendIntervalMs < MIN_SEND_INTERVAL_MS || patch.sendIntervalMs > MAX_SEND_INTERVAL_MS) {
            result.error = TelegramControlError::INVALID_ARGUMENT;
            result.message = "sendIntervalMs out of range";
            return result;
        }
    }

    if (patch.hasLongPollTimeoutSec) {
        if (patch.longPollTimeoutSec > MAX_LONG_POLL_TIMEOUT_SEC) {
            result.error = TelegramControlError::INVALID_ARGUMENT;
            result.message = "longPollTimeoutSec out of range";
            return result;
        }
    }

    if (patch.hasEnabled && patch.enabled != bridgeEnabled) {
        bridgeEnabled = patch.enabled;
        changed = true;
    }

    if (patch.hasToken) {
        const std::string newToken = trim(patch.token);
        if (newToken != token) {
            token = newToken;
            api.setToken(token.c_str());
            changed = true;
        }
    }

    if (patch.hasChatId) {
        const std::string newChatId = trim(patch.chatId);
        int64_t parsedChatId = 0;
        const bool hasParsed = parseChatId(newChatId, parsedChatId);

        if (!newChatId.empty() && !hasParsed) {
            result.error = TelegramControlError::INVALID_ARGUMENT;
            result.message = "chatId must be integer";
            return result;
        }

        if (newChatId != chatId || hasConfiguredChatId != hasParsed || (hasParsed && configuredChatId != parsedChatId)) {
            chatId = newChatId;
            hasConfiguredChatId = hasParsed;
            if (hasParsed) {
                configuredChatId = parsedChatId;
            } else {
                configuredChatId = 0;
            }
            changed = true;
        }
    }

    if (patch.hasChannels) {
        const std::string previousChannels = channelsConfig;
        const bool previousAllowAll = allowAllChannels;
        const std::set<uint8_t> previousAllowed = allowedChannels;
        const uint8_t previousTxChannel = telegramToMeshChannel;

        if (!applyChannelsConfig(patch.channels)) {
            result.error = TelegramControlError::INVALID_ARGUMENT;
            result.message = "channels format invalid";
            return result;
        }

        if (previousChannels != channelsConfig || previousAllowAll != allowAllChannels || previousAllowed != allowedChannels ||
            previousTxChannel != telegramToMeshChannel) {
            changed = true;
        }
    }

    if (patch.hasPollIntervalMs && pollIntervalMs != patch.pollIntervalMs) {
        pollIntervalMs = patch.pollIntervalMs;
        changed = true;
    }

    if (patch.hasLongPollTimeoutSec && longPollTimeoutSec != patch.longPollTimeoutSec) {
        longPollTimeoutSec = patch.longPollTimeoutSec;
        changed = true;
    }

    if (patch.hasSendIntervalMs && sendIntervalMs != patch.sendIntervalMs) {
        sendIntervalMs = patch.sendIntervalMs;
        changed = true;
    }

    result.changed = changed;
    if (!changed) {
        result.persisted = true;
        result.message = "No changes";
        return result;
    }

    if (!saveSettingsToNvsLocked()) {
        result.error = TelegramControlError::PERSISTENCE_ERROR;
        result.persisted = false;
        result.message = "Failed to save Telegram settings";
        refreshOperationalStateLocked();
        return result;
    }

    refreshOperationalStateLocked();

    result.persisted = true;
    if (bridgeEnabled && !isConfiguredLocked()) {
        result.message = "Settings saved, bridge needs bot token and chat_id";
    } else {
        result.message = "Settings saved";
    }

    LOG_INFO("Telegram control patch applied from %s", sourceToString(source));
    return result;
}

int32_t TelegramBridge::runOnce()
{
    State currentState = State::STATE_DISABLED;
    uint32_t currentPollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
    {
        concurrency::LockGuard guard(&configLock);
        currentState = state;
        currentPollIntervalMs = pollIntervalMs;
    }

    if (currentState == State::STATE_DISABLED)
        return disable();

    if (!isWifiConnected()) {
        bool needsLog = false;
        {
            concurrency::LockGuard guard(&configLock);
            needsLog = state != State::STATE_WAIT_WIFI;
            state = State::STATE_WAIT_WIFI;
        }
        if (needsLog) {
            LOG_INFO("Telegram bridge waiting for WiFi");
        }
        return WIFI_RETRY_INTERVAL_MS;
    }

    {
        concurrency::LockGuard guard(&configLock);
        if (state == State::STATE_WAIT_WIFI) {
            state = State::STATE_RUNNING;
            LOG_INFO("Telegram bridge running");
        }
    }

    sendQueuedMessages();

    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - lastPollAtMs) >= currentPollIntervalMs) {
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
    if (packet == nullptr)
        return 0;

    {
        concurrency::LockGuard guard(&configLock);
        if (state == State::STATE_DISABLED || !isChannelAllowed(packet->channel)) {
            return 0;
        }
    }

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
    bridgeEnabled = TELEGRAM_ENABLED_DEFAULT;
    pollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
    longPollTimeoutSec = TELEGRAM_LONG_POLL_TIMEOUT;
    sendIntervalMs = TELEGRAM_SEND_INTERVAL_MS;

    Preferences prefs;
    if (prefs.begin("telegram", true)) {
        token = prefs.getString("bot_token", token.c_str()).c_str();
        chatId = prefs.getString("chat_id", chatId.c_str()).c_str();
        channelsConfig = prefs.getString("channels", channelsConfig.c_str()).c_str();
        bridgeEnabled = prefs.getBool("enabled", bridgeEnabled);
        pollIntervalMs = prefs.getUInt("poll_ms", pollIntervalMs);
        longPollTimeoutSec = prefs.getUInt("long_poll", longPollTimeoutSec);
        sendIntervalMs = prefs.getUInt("send_ms", sendIntervalMs);
        prefs.end();
    }

    pollIntervalMs = normalizePollInterval(pollIntervalMs);
    longPollTimeoutSec = normalizeLongPollTimeout(longPollTimeoutSec);
    sendIntervalMs = normalizeSendInterval(sendIntervalMs);

    api.setToken(token.c_str());
    hasConfiguredChatId = parseChatId(chatId, configuredChatId);
    if (!applyChannelsConfig(channelsConfig)) {
        LOG_WARN("Invalid Telegram channels config in NVS, fallback to all channels");
        channelsConfig.clear();
        applyChannelsConfig(channelsConfig);
    }
}

bool TelegramBridge::saveSettingsToNvsLocked()
{
    Preferences prefs;
    if (!prefs.begin("telegram", false)) {
        LOG_WARN("Telegram bridge failed to open NVS for settings");
        return false;
    }

    prefs.putString("bot_token", token.c_str());
    prefs.putString("chat_id", chatId.c_str());
    prefs.putString("channels", channelsConfig.c_str());
    prefs.putBool("enabled", bridgeEnabled);
    prefs.putUInt("poll_ms", pollIntervalMs);
    prefs.putUInt("long_poll", longPollTimeoutSec);
    prefs.putUInt("send_ms", sendIntervalMs);
    prefs.end();
    return true;
}

bool TelegramBridge::isConfiguredLocked() const
{
    return api.isConfigured() && hasConfiguredChatId;
}

void TelegramBridge::refreshOperationalStateLocked()
{
    const bool configured = isConfiguredLocked();
    if (!bridgeEnabled || !configured) {
        state = State::STATE_DISABLED;
        hasPendingMessage = false;
        pendingMessage.clear();
        consecutiveSendErrors = 0;
        nextRetryAtMs = 0;
        disable();

        if (bridgeEnabled && !configured) {
            LOG_INFO("Telegram bridge disabled: missing bot token or chat_id");
        }
        return;
    }

    state = isWifiConnected() ? State::STATE_RUNNING : State::STATE_WAIT_WIFI;
    enabled = true;
    setIntervalFromNow(0);
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

bool TelegramBridge::applyChannelsConfig(const std::string &rawChannels)
{
    const std::string normalized = trim(rawChannels);
    if (normalized.empty()) {
        allowAllChannels = true;
        allowedChannels.clear();
        telegramToMeshChannel = channels.getPrimaryIndex();
        channelsConfig.clear();
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
    concurrency::LockGuard guard(&configLock);
    return hasConfiguredChatId && incomingChatId == configuredChatId;
}

bool TelegramBridge::isSelfInjected(uint32_t packetId) const
{
    concurrency::LockGuard guard(&configLock);
    for (size_t i = 0; i < SELF_INJECTED_ID_COUNT; ++i) {
        if (selfInjectedIds[i] == packetId && packetId != 0) {
            return true;
        }
    }
    return false;
}

void TelegramBridge::rememberSelfInjected(uint32_t packetId)
{
    concurrency::LockGuard guard(&configLock);
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
    std::string targetChatId;
    uint32_t currentSendIntervalMs = TELEGRAM_SEND_INTERVAL_MS;
    {
        concurrency::LockGuard guard(&configLock);
        if (state == State::STATE_DISABLED || !isConfiguredLocked() || chatId.empty()) {
            return;
        }
        targetChatId = chatId;
        currentSendIntervalMs = sendIntervalMs;
    }

    if (targetChatId.empty()) {
        return;
    }

    const uint32_t now = millis();
    if (nextRetryAtMs != 0 && static_cast<int32_t>(now - nextRetryAtMs) < 0)
        return;

    if (static_cast<uint32_t>(now - lastSendAtMs) < currentSendIntervalMs)
        return;

    if (!hasPendingMessage) {
        QueueEntry *entry = messageQueue.dequeuePtr(0);
        if (!entry)
            return;

        pendingMessage = std::move(entry->text);
        delete entry;
        hasPendingMessage = true;
    }

    if (api.sendMessage(targetChatId, pendingMessage)) {
        hasPendingMessage = false;
        pendingMessage.clear();
        consecutiveSendErrors = 0;
        nextRetryAtMs = 0;
        lastSendAtMs = now;
        return;
    }

    consecutiveSendErrors = std::min<uint8_t>(consecutiveSendErrors + 1, 10);

    uint32_t backoffMs = currentSendIntervalMs;
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
    uint32_t timeoutSec = TELEGRAM_LONG_POLL_TIMEOUT;
    {
        concurrency::LockGuard guard(&configLock);
        if (state == State::STATE_DISABLED || !isConfiguredLocked()) {
            return;
        }
        timeoutSec = longPollTimeoutSec;
    }

    TelegramMessage updates[TELEGRAM_UPDATE_BATCH_SIZE];
    const int count = api.getUpdates(updates, TELEGRAM_UPDATE_BATCH_SIZE, timeoutSec);
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
        TelegramControlPatch patch;
        patch.hasChannels = true;
        patch.channels = trim(command.substr(channelsCommand.size()));

        const TelegramControlResult updateResult = applyControlPatch(patch, TelegramControlSource::TELEGRAM_CHAT);
        if (!updateResult.ok()) {
            enqueueTelegramMessage("Invalid channels. Use: /config channels 0,1,3");
            return true;
        }

        TelegramControlSnapshot snapshot = getControlSnapshot();
        if (snapshot.allowAllChannels) {
            enqueueTelegramMessage("Channels updated: all");
        } else {
            enqueueTelegramMessage("Channels updated: " + snapshot.channels);
        }
        return true;
    }

    static const std::string enabledCommand = "/config enabled";
    if (command.rfind(enabledCommand, 0) == 0) {
        const std::string value = trim(command.substr(enabledCommand.size()));

        bool newEnabled = false;
        if (value == "1" || value == "on" || value == "true") {
            newEnabled = true;
        } else if (value == "0" || value == "off" || value == "false") {
            newEnabled = false;
        } else {
            enqueueTelegramMessage("Invalid value. Use: /config enabled on|off");
            return true;
        }

        const TelegramControlResult updateResult = setEnabled(newEnabled, TelegramControlSource::TELEGRAM_CHAT);
        if (!updateResult.ok()) {
            enqueueTelegramMessage("Failed to update Telegram enabled state");
            return true;
        }

        enqueueTelegramMessage(newEnabled ? "Telegram bridge enabled" : "Telegram bridge disabled");
        return true;
    }

    return false;
}

std::string TelegramBridge::buildStatusMessage()
{
    TelegramControlSnapshot snapshot = getControlSnapshot();

    std::string status = "<b>Telegram bridge</b>\n";
    status += "state: ";
    if (!snapshot.enabled) {
        status += "disabled";
    } else if (snapshot.running) {
        status += "running";
    } else {
        status += "wait_wifi";
    }

    status += "\nconfigured: ";
    status += snapshot.configured ? "yes" : "no";

    status += "\nwifi: ";
    status += snapshot.wifiConnected ? "connected" : "disconnected";

    status += "\nqueue: ";
    status += std::to_string(snapshot.queueUsed);
    status += "/";
    status += std::to_string(snapshot.queueCapacity);

    status += "\nchannels: ";
    status += snapshot.allowAllChannels ? "all" : snapshot.channels;

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

    uint8_t channel = 0;
    {
        concurrency::LockGuard guard(&configLock);
        channel = telegramToMeshChannel;
    }

    meshtastic_MeshPacket *packet = router->allocForSending();
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet->channel = channel;
    packet->decoded.payload.size = payload.size();
    memcpy(packet->decoded.payload.bytes, payload.data(), payload.size());

    rememberSelfInjected(packet->id);
    service->sendToMesh(packet, RX_SRC_LOCAL);
    return true;
}

#endif
