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
#include <limits>

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
// Stack size for the HTTP task (HTTPS + TLS needs significant stack)
constexpr uint32_t HTTP_TASK_STACK_SIZE = 12288;
// Queue depth for incoming Telegram messages
constexpr size_t INCOMING_QUEUE_DEPTH = 32;
static_assert(TELEGRAM_HISTORY_MAX_ENTRIES > 0, "TELEGRAM_HISTORY_MAX_ENTRIES must be greater than zero");

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

bool isValidDirectionMode(uint8_t rawMode)
{
    return rawMode == static_cast<uint8_t>(TelegramDirectionMode::BOTH) ||
           rawMode == static_cast<uint8_t>(TelegramDirectionMode::MESH_TO_TELEGRAM) ||
           rawMode == static_cast<uint8_t>(TelegramDirectionMode::TELEGRAM_TO_MESH);
}

bool allowsMeshToTelegram(TelegramDirectionMode mode)
{
    return mode == TelegramDirectionMode::BOTH || mode == TelegramDirectionMode::MESH_TO_TELEGRAM;
}

bool allowsTelegramToMesh(TelegramDirectionMode mode)
{
    return mode == TelegramDirectionMode::BOTH || mode == TelegramDirectionMode::TELEGRAM_TO_MESH;
}

bool historyDirectionMatches(TelegramHistoryDirection direction, TelegramHistoryFilterDirection filter)
{
    if (filter == TelegramHistoryFilterDirection::BOTH) {
        return true;
    }

    if (filter == TelegramHistoryFilterDirection::OUTGOING) {
        return direction == TelegramHistoryDirection::OUTGOING;
    }

    return direction == TelegramHistoryDirection::INCOMING;
}

const char *directionToString(TelegramDirectionMode mode)
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
        snapshot.featureAvailable = false;
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

std::vector<TelegramHistoryEntry> telegramGetHistory(const std::string &chatIdFilter,
                                                     TelegramHistoryFilterDirection directionFilter, size_t limit)
{
    if (!telegramBridge) {
        return std::vector<TelegramHistoryEntry>();
    }

    return telegramBridge->getHistory(chatIdFilter, directionFilter, limit);
}

std::vector<TelegramHistoryChatSummary> telegramGetHistoryChats(size_t limit)
{
    if (!telegramBridge) {
        return std::vector<TelegramHistoryChatSummary>();
    }

    return telegramBridge->getHistoryChats(limit);
}

bool telegramClearHistory()
{
    if (!telegramBridge) {
        return false;
    }

    telegramBridge->clearHistory();
    return true;
}

TelegramBridge::TelegramBridge() : concurrency::OSThread("telegram"), messageQueue(TELEGRAM_MAX_QUEUE_SIZE)
{
    _incomingQueue = xQueueCreate(INCOMING_QUEUE_DEPTH, sizeof(TelegramMessage *));
    if (!_incomingQueue) {
        LOG_ERROR("Telegram bridge failed to create incoming queue");
    }

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
    snapshot.directionMode = directionMode;
    snapshot.meshToTelegramEnabled = allowsMeshToTelegram(directionMode);
    snapshot.telegramToMeshEnabled = allowsTelegramToMesh(directionMode);

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

std::vector<TelegramHistoryEntry> TelegramBridge::getHistory(const std::string &chatIdFilter,
                                                             TelegramHistoryFilterDirection directionFilter, size_t limit)
{
    std::vector<TelegramHistoryEntry> result;
    if (limit == 0) {
        return result;
    }

    const std::string normalizedChatId = trim(chatIdFilter);
    const size_t boundedLimit = std::min(limit, static_cast<size_t>(TELEGRAM_HISTORY_MAX_ENTRIES));
    result.reserve(boundedLimit);

    concurrency::LockGuard guard(&configLock);
    for (size_t i = 0; i < historyCount && result.size() < boundedLimit; ++i) {
        const size_t reverseIndex = historyCount - 1 - i;
        const size_t index = (historyHead + reverseIndex) % TELEGRAM_HISTORY_MAX_ENTRIES;
        const TelegramHistoryEntry &entry = historyEntries[index].entry;

        if (!normalizedChatId.empty() && entry.chatId != normalizedChatId) {
            continue;
        }

        if (!historyDirectionMatches(entry.direction, directionFilter)) {
            continue;
        }

        result.push_back(entry);
    }

    return result;
}

std::vector<TelegramHistoryChatSummary> TelegramBridge::getHistoryChats(size_t limit)
{
    std::vector<TelegramHistoryChatSummary> summaries;

    concurrency::LockGuard guard(&configLock);
    summaries.reserve(std::min(historyCount, static_cast<size_t>(TELEGRAM_HISTORY_MAX_ENTRIES)));

    for (size_t i = 0; i < historyCount; ++i) {
        const size_t index = (historyHead + i) % TELEGRAM_HISTORY_MAX_ENTRIES;
        const TelegramHistoryEntry &entry = historyEntries[index].entry;
        if (entry.chatId.empty()) {
            continue;
        }

        auto summary = std::find_if(summaries.begin(), summaries.end(), [&entry](const TelegramHistoryChatSummary &candidate) {
            return candidate.chatId == entry.chatId;
        });

        if (summary == summaries.end()) {
            TelegramHistoryChatSummary created;
            created.chatId = entry.chatId;
            summaries.push_back(created);
            summary = summaries.end() - 1;
        }

        if (entry.direction == TelegramHistoryDirection::INCOMING) {
            if (summary->incomingCount < std::numeric_limits<uint16_t>::max()) {
                summary->incomingCount++;
            }
        } else {
            if (summary->outgoingCount < std::numeric_limits<uint16_t>::max()) {
                summary->outgoingCount++;
            }
        }

        if (entry.timestampMs > summary->lastTimestampMs) {
            summary->lastTimestampMs = entry.timestampMs;
        }
    }

    std::sort(summaries.begin(), summaries.end(),
              [](const TelegramHistoryChatSummary &left, const TelegramHistoryChatSummary &right) {
                  return left.lastTimestampMs > right.lastTimestampMs;
              });

    if (limit > 0 && summaries.size() > limit) {
        summaries.resize(limit);
    }

    return summaries;
}

void TelegramBridge::clearHistory()
{
    concurrency::LockGuard guard(&configLock);
    for (size_t i = 0; i < TELEGRAM_HISTORY_MAX_ENTRIES; ++i) {
        historyEntries[i].entry.chatId.clear();
        historyEntries[i].entry.sender.clear();
        historyEntries[i].entry.text.clear();
        historyEntries[i].entry.timestampMs = 0;
        historyEntries[i].entry.direction = TelegramHistoryDirection::OUTGOING;
        historyEntries[i].entry.status = TelegramHistoryStatus::QUEUED;
    }

    historyHead = 0;
    historyCount = 0;
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

    if (patch.hasDirectionMode) {
        if (!isValidDirectionMode(static_cast<uint8_t>(patch.directionMode))) {
            result.error = TelegramControlError::INVALID_ARGUMENT;
            result.message = "directionMode is invalid";
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
            // Note: api.setToken() is applied by the HTTP task at the start of each iteration
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

    if (patch.hasDirectionMode && directionMode != patch.directionMode) {
        directionMode = patch.directionMode;
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

// ─── HTTP task management ────────────────────────────────────────────────────

void TelegramBridge::httpTaskEntryPoint(void *param)
{
    static_cast<TelegramBridge *>(param)->httpTaskLoop();
    vTaskDelete(nullptr);
}

void TelegramBridge::startHttpTask()
{
    if (_httpTaskHandle != nullptr) {
        return; // already running or previous stop not yet complete
    }

    _httpTaskShouldStop = false;

    TaskHandle_t handle = nullptr;
    const BaseType_t result =
        xTaskCreate(httpTaskEntryPoint, "tg_http", HTTP_TASK_STACK_SIZE, this, 1, &handle);

    if (result == pdPASS) {
        _httpTaskHandle = handle;
        LOG_INFO("Telegram HTTP task started");
    } else {
        LOG_ERROR("Telegram HTTP task creation failed");
    }
}

void TelegramBridge::stopHttpTask()
{
    if (_httpTaskHandle == nullptr) {
        return;
    }
    // Signal task to exit. The task will set _httpTaskHandle = nullptr on exit.
    _httpTaskShouldStop = true;
    LOG_INFO("Telegram HTTP task stop requested");
}

/**
 * Runs in a dedicated FreeRTOS task. All blocking HTTPS calls happen here so
 * the main OSThread scheduler is never blocked.
 *
 * Thread-safety contract:
 *   - Reads config fields under configLock (short critical sections only)
 *   - All HTTPS I/O happens WITHOUT the lock held
 *   - Writes history via appendHistory() which acquires configLock internally
 *   - Puts received TelegramMessage* into _incomingQueue for runOnce() to process
 */
void TelegramBridge::httpTaskLoop()
{
    bool hasPending = false;
    std::string pending;
    std::string pendingChatId;
    uint32_t nextRetryMs = 0;
    uint8_t sendErrors = 0;
    uint32_t lastPollMs = 0;

    while (!_httpTaskShouldStop) {
        // Short delay so we can check the stop flag frequently
        vTaskDelay(pdMS_TO_TICKS(200));

        if (_httpTaskShouldStop) {
            break;
        }

        // Snapshot config under lock (avoid holding lock during HTTPS calls)
        std::string localToken, localChatId;
        uint32_t localSendInterval, localPollInterval, localLongPollTimeout;
        TelegramDirectionMode localDirection;
        {
            concurrency::LockGuard guard(&configLock);
            if (!isConfiguredLocked()) {
                continue;
            }
            localToken = token;
            localChatId = chatId;
            localSendInterval = sendIntervalMs;
            localPollInterval = pollIntervalMs;
            localLongPollTimeout = longPollTimeoutSec;
            localDirection = directionMode;
        }

        // Keep API token in sync (only the HTTP task calls api methods)
        api.setToken(localToken.c_str());

        const uint32_t now = millis();

        // ── Send outgoing messages (mesh → Telegram) ────────────────────────
        if (allowsMeshToTelegram(localDirection)) {
            const bool retryReady = (nextRetryMs == 0 || static_cast<int32_t>(now - nextRetryMs) >= 0);

            if (retryReady) {
                if (!hasPending) {
                    QueueEntry *entry = messageQueue.dequeuePtr(0);
                    if (entry) {
                        pending = std::move(entry->text);
                        pendingChatId = localChatId;
                        delete entry;
                        hasPending = true;
                        appendHistory(TelegramHistoryDirection::OUTGOING, TelegramHistoryStatus::QUEUED,
                                      pendingChatId, "", pending);
                    }
                }

                if (hasPending && !_httpTaskShouldStop) {
                    if (api.sendMessage(pendingChatId, pending)) {
                        appendHistory(TelegramHistoryDirection::OUTGOING, TelegramHistoryStatus::SENT,
                                      pendingChatId, "", pending);
                        hasPending = false;
                        pending.clear();
                        sendErrors = 0;
                        nextRetryMs = 0;
                    } else {
                        appendHistory(TelegramHistoryDirection::OUTGOING, TelegramHistoryStatus::SEND_FAILED,
                                      pendingChatId, "", pending);
                        sendErrors = std::min<uint8_t>(sendErrors + 1, 10);
                        uint32_t backoff = localSendInterval;
                        for (uint8_t i = 0; i < sendErrors; ++i) {
                            if (backoff >= MAX_BACKOFF_MS / 2) {
                                backoff = MAX_BACKOFF_MS;
                                break;
                            }
                            backoff *= 2;
                        }
                        nextRetryMs = now + backoff;
                        LOG_WARN("Telegram send failed, retry in %u ms", backoff);
                    }
                }
            }
        }

        if (_httpTaskShouldStop) {
            break;
        }

        // ── Poll for incoming messages (Telegram → mesh) ────────────────────
        if (allowsTelegramToMesh(localDirection) &&
            static_cast<uint32_t>(now - lastPollMs) >= localPollInterval) {
            lastPollMs = now;

            TelegramMessage updates[TELEGRAM_UPDATE_BATCH_SIZE];
            const int count = api.getUpdates(updates, TELEGRAM_UPDATE_BATCH_SIZE, localLongPollTimeout);

            for (int i = 0; i < count && !_httpTaskShouldStop; ++i) {
                TelegramMessage *msg = new TelegramMessage(std::move(updates[i]));
                if (xQueueSendToBack(_incomingQueue, &msg, 0) != pdTRUE) {
                    LOG_WARN("Telegram incoming queue full, dropping message");
                    delete msg;
                }
            }
        }
    }

    // Signal to startHttpTask() that it is safe to create a new task
    _httpTaskHandle = nullptr;
    LOG_INFO("Telegram HTTP task stopped");
}

// ─── Main OSThread runOnce ───────────────────────────────────────────────────

int32_t TelegramBridge::runOnce()
{
    State currentState = State::STATE_DISABLED;
    {
        concurrency::LockGuard guard(&configLock);
        currentState = state;
    }

    if (currentState == State::STATE_DISABLED) {
        stopHttpTask();
        return disable();
    }

    if (!isWifiConnected()) {
        stopHttpTask();
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

    // Ensure the HTTP task is running (idempotent)
    startHttpTask();

    // Process incoming messages from HTTP task (non-blocking)
    if (_incomingQueue != nullptr) {
        TelegramMessage *msg = nullptr;
        while (xQueueReceive(_incomingQueue, &msg, 0) == pdTRUE && msg != nullptr) {
            processIncomingMessage(msg);
            delete msg;
            msg = nullptr;
        }
    }

    // Periodic heap check
    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - lastHeapWarnMs) >= HEAP_WARN_INTERVAL_MS) {
        lastHeapWarnMs = now;
        const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (freeHeap < MINIMUM_SAFE_FREE_HEAP) {
            LOG_WARN("Telegram bridge low heap: %u", freeHeap);
        }
    }

    return RUN_INTERVAL_MS;
}

// ─── Incoming message processing (called from main OSThread) ─────────────────

void TelegramBridge::processIncomingMessage(const TelegramMessage *msg)
{
    if (msg == nullptr) {
        return;
    }

    const std::string incomingChatId = std::to_string(msg->chat_id);

    if (!matchesConfiguredChat(msg->chat_id)) {
        appendHistory(TelegramHistoryDirection::INCOMING, TelegramHistoryStatus::IGNORED_CHAT,
                      incomingChatId, msg->from_name, msg->text);
        return;
    }

    appendHistory(TelegramHistoryDirection::INCOMING, TelegramHistoryStatus::RECEIVED,
                  incomingChatId, msg->from_name, msg->text);

    if (handleTelegramCommand(*msg)) {
        appendHistory(TelegramHistoryDirection::INCOMING, TelegramHistoryStatus::COMMAND,
                      incomingChatId, msg->from_name, msg->text);
        return;
    }

    {
        concurrency::LockGuard guard(&configLock);
        if (!allowsTelegramToMesh(directionMode)) {
            return;
        }
    }

    const bool injected = injectToMesh(msg->text, msg->from_name);
    appendHistory(TelegramHistoryDirection::INCOMING,
                  injected ? TelegramHistoryStatus::INJECTED : TelegramHistoryStatus::INJECT_FAILED,
                  incomingChatId, msg->from_name, msg->text);
}

// ─── Mesh packet observer ────────────────────────────────────────────────────

int TelegramBridge::onNotify(const meshtastic_MeshPacket *packet)
{
    if (packet == nullptr)
        return 0;

    {
        concurrency::LockGuard guard(&configLock);
        if (state == State::STATE_DISABLED || !allowsMeshToTelegram(directionMode) || !isChannelAllowed(packet->channel)) {
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

// ─── Configuration ────────────────────────────────────────────────────────────

void TelegramBridge::loadConfig()
{
    token = TELEGRAM_BOT_TOKEN;
    chatId = TELEGRAM_CHAT_ID;
    channelsConfig = TELEGRAM_CHANNELS;
    bridgeEnabled = TELEGRAM_ENABLED_DEFAULT;
    pollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
    longPollTimeoutSec = TELEGRAM_LONG_POLL_TIMEOUT;
    sendIntervalMs = TELEGRAM_SEND_INTERVAL_MS;
    directionMode = TelegramDirectionMode::BOTH;

    Preferences prefs;
    if (prefs.begin("telegram", true)) {
        token = prefs.getString("bot_token", token.c_str()).c_str();
        chatId = prefs.getString("chat_id", chatId.c_str()).c_str();
        channelsConfig = prefs.getString("channels", channelsConfig.c_str()).c_str();
        bridgeEnabled = prefs.getBool("enabled", bridgeEnabled);
        pollIntervalMs = prefs.getUInt("poll_ms", pollIntervalMs);
        longPollTimeoutSec = prefs.getUInt("long_poll", longPollTimeoutSec);
        sendIntervalMs = prefs.getUInt("send_ms", sendIntervalMs);

        const uint8_t rawDirection = prefs.getUChar("direction", static_cast<uint8_t>(directionMode));
        if (isValidDirectionMode(rawDirection)) {
            directionMode = static_cast<TelegramDirectionMode>(rawDirection);
        }

        prefs.end();
    }

    pollIntervalMs = normalizePollInterval(pollIntervalMs);
    longPollTimeoutSec = normalizeLongPollTimeout(longPollTimeoutSec);
    sendIntervalMs = normalizeSendInterval(sendIntervalMs);

    // Note: api.setToken() is NOT called here — the HTTP task applies the token
    // at the start of each iteration to avoid data races.
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
    prefs.putUChar("direction", static_cast<uint8_t>(directionMode));
    prefs.end();
    return true;
}

bool TelegramBridge::isConfiguredLocked() const
{
    // Use TelegramBridge::token directly to avoid data races with the HTTP task
    // (api.isConfigured() reads api::token which is only written by the HTTP task)
    return !token.empty() && hasConfiguredChatId;
}

void TelegramBridge::refreshOperationalStateLocked()
{
    const bool configured = isConfiguredLocked();
    if (!bridgeEnabled || !configured) {
        state = State::STATE_DISABLED;
        // Request the HTTP task to stop (non-blocking)
        _httpTaskShouldStop = true;
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

// ─── Utility helpers ─────────────────────────────────────────────────────────

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

void TelegramBridge::appendHistory(TelegramHistoryDirection direction, TelegramHistoryStatus status,
                                   const std::string &chat, const std::string &sender, const std::string &text)
{
    concurrency::LockGuard guard(&configLock);
    appendHistoryLocked(direction, status, chat, sender, text);
}

void TelegramBridge::appendHistoryLocked(TelegramHistoryDirection direction, TelegramHistoryStatus status,
                                         const std::string &chat, const std::string &sender, const std::string &text)
{
    TelegramHistoryEntry entry;
    entry.timestampMs = millis();
    entry.chatId = trim(chat);
    if (entry.chatId.empty()) {
        entry.chatId = "unknown";
    }

    entry.sender = truncateUtf8(trim(sender), 64);
    entry.text = truncateUtf8(text, TELEGRAM_HISTORY_TEXT_MAX_SIZE);
    entry.direction = direction;
    entry.status = status;

    size_t writeIndex = (historyHead + historyCount) % TELEGRAM_HISTORY_MAX_ENTRIES;
    if (historyCount == TELEGRAM_HISTORY_MAX_ENTRIES) {
        writeIndex = historyHead;
        historyHead = (historyHead + 1) % TELEGRAM_HISTORY_MAX_ENTRIES;
    } else {
        historyCount++;
    }

    historyEntries[writeIndex].entry = std::move(entry);
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

// ─── Telegram command handling ────────────────────────────────────────────────

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

    static const std::string directionCommand = "/config direction";
    if (command.rfind(directionCommand, 0) == 0) {
        const std::string value = trim(command.substr(directionCommand.size()));

        TelegramDirectionMode newMode = TelegramDirectionMode::BOTH;
        if (value == "both" || value == "bidir") {
            newMode = TelegramDirectionMode::BOTH;
        } else if (value == "mesh_to_telegram" || value == "m2t") {
            newMode = TelegramDirectionMode::MESH_TO_TELEGRAM;
        } else if (value == "telegram_to_mesh" || value == "t2m") {
            newMode = TelegramDirectionMode::TELEGRAM_TO_MESH;
        } else {
            enqueueTelegramMessage("Invalid direction. Use: both|mesh_to_telegram|telegram_to_mesh");
            return true;
        }

        TelegramControlPatch patch;
        patch.hasDirectionMode = true;
        patch.directionMode = newMode;

        const TelegramControlResult updateResult = applyControlPatch(patch, TelegramControlSource::TELEGRAM_CHAT);
        if (!updateResult.ok()) {
            enqueueTelegramMessage("Failed to update direction mode");
            return true;
        }

        enqueueTelegramMessage(std::string("Direction updated: ") + directionToString(newMode));
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

    status += "\ndirection: ";
    status += directionToString(snapshot.directionMode);

    return truncateUtf8(status, TELEGRAM_MAX_TEXT_SIZE);
}

// ─── Mesh integration ─────────────────────────────────────────────────────────

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
        if (!allowsTelegramToMesh(directionMode)) {
            return false;
        }
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
