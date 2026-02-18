#pragma once

#include "configuration.h"

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

enum class TelegramControlSource : uint8_t {
    UNKNOWN = 0,
    DEVICE_UI = 1,
    TELEGRAM_CHAT = 2,
    HTTP_API = 3,
    SERIAL_API = 4,
    OTHER = 255,
};

enum class TelegramControlError : uint8_t {
    NONE = 0,
    NOT_AVAILABLE = 1,
    INVALID_ARGUMENT = 2,
    PERSISTENCE_ERROR = 3,
};

enum class TelegramDirectionMode : uint8_t {
    BOTH = 0,
    MESH_TO_TELEGRAM = 1,
    TELEGRAM_TO_MESH = 2,
};

enum class TelegramHistoryDirection : uint8_t {
    OUTGOING = 0,
    INCOMING = 1,
};

enum class TelegramHistoryStatus : uint8_t {
    QUEUED = 0,
    SENT = 1,
    SEND_FAILED = 2,
    RECEIVED = 3,
    INJECTED = 4,
    IGNORED_CHAT = 5,
    COMMAND = 6,
    INJECT_FAILED = 7,
};

enum class TelegramHistoryFilterDirection : uint8_t {
    BOTH = 0,
    OUTGOING = 1,
    INCOMING = 2,
};

struct TelegramControlPatch {
    bool hasEnabled = false;
    bool enabled = false;

    bool hasToken = false;
    std::string token;

    bool hasChatId = false;
    std::string chatId;

    bool hasChannels = false;
    std::string channels;

    bool hasPollIntervalMs = false;
    uint32_t pollIntervalMs = 0;

    bool hasLongPollTimeoutSec = false;
    uint32_t longPollTimeoutSec = 0;

    bool hasSendIntervalMs = false;
    uint32_t sendIntervalMs = 0;

    bool hasDirectionMode = false;
    TelegramDirectionMode directionMode = TelegramDirectionMode::BOTH;
};

struct TelegramControlSnapshot {
    bool featureAvailable = false;
    bool enabled = false;
    bool running = false;
    bool configured = false;
    bool wifiConnected = false;

    bool allowAllChannels = true;
    std::string channels;
    uint8_t meshChannelForInject = 0;

    uint16_t queueUsed = 0;
    uint16_t queueCapacity = 0;

    uint32_t pollIntervalMs = 0;
    uint32_t longPollTimeoutSec = 0;
    uint32_t sendIntervalMs = 0;

    TelegramDirectionMode directionMode = TelegramDirectionMode::BOTH;
    bool meshToTelegramEnabled = true;
    bool telegramToMeshEnabled = true;

    bool hasToken = false;
    bool hasChatId = false;
    std::string chatId;
};

struct TelegramControlResult {
    TelegramControlError error = TelegramControlError::NONE;
    bool changed = false;
    bool persisted = false;
    std::string message;

    bool ok() const { return error == TelegramControlError::NONE; }
};

struct TelegramHistoryEntry {
    uint32_t timestampMs = 0;
    std::string chatId;
    std::string sender;
    std::string text;
    TelegramHistoryDirection direction = TelegramHistoryDirection::OUTGOING;
    TelegramHistoryStatus status = TelegramHistoryStatus::QUEUED;
};

struct TelegramHistoryChatSummary {
    std::string chatId;
    uint16_t incomingCount = 0;
    uint16_t outgoingCount = 0;
    uint32_t lastTimestampMs = 0;
};

#if !MESHTASTIC_EXCLUDE_TELEGRAM && HAS_WIFI && defined(ARCH_ESP32)

#include "Observer.h"
#include "concurrency/Lock.h"
#include "concurrency/OSThread.h"
#include "mesh/PointerQueue.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "telegram/TelegramAPI.h"
#include "telegram/TelegramConfig.h"

#include <set>

class TelegramBridge : public concurrency::OSThread, public Observer<const meshtastic_MeshPacket *>
{
  public:
    TelegramBridge();

    TelegramControlSnapshot getControlSnapshot();
    TelegramControlResult applyControlPatch(const TelegramControlPatch &patch, TelegramControlSource source);
    TelegramControlResult setEnabled(bool enabled, TelegramControlSource source);
    std::vector<TelegramHistoryEntry> getHistory(const std::string &chatIdFilter,
                                                 TelegramHistoryFilterDirection directionFilter, size_t limit);
    std::vector<TelegramHistoryChatSummary> getHistoryChats(size_t limit);
    void clearHistory();

  protected:
    int32_t runOnce() override;
    int onNotify(const meshtastic_MeshPacket *packet) override;

  private:
    enum class State {
        STATE_DISABLED,
        STATE_WAIT_WIFI,
        STATE_RUNNING,
    };

    struct QueueEntry {
        std::string text;
    };

    struct HistoryRingEntry {
        TelegramHistoryEntry entry;
    };

    TelegramAPI api;
    PointerQueue<QueueEntry> messageQueue;
    mutable concurrency::Lock configLock;

    State state = State::STATE_DISABLED;
    bool bridgeEnabled = TELEGRAM_ENABLED_DEFAULT;

    std::string token;
    std::string chatId;
    int64_t configuredChatId = 0;
    bool hasConfiguredChatId = false;

    std::string channelsConfig;
    std::set<uint8_t> allowedChannels;
    bool allowAllChannels = true;
    uint8_t telegramToMeshChannel = 0;

    uint32_t pollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
    uint32_t longPollTimeoutSec = TELEGRAM_LONG_POLL_TIMEOUT;
    uint32_t sendIntervalMs = TELEGRAM_SEND_INTERVAL_MS;
    TelegramDirectionMode directionMode = TelegramDirectionMode::BOTH;

    uint32_t lastHeapWarnMs = 0;

    HistoryRingEntry historyEntries[TELEGRAM_HISTORY_MAX_ENTRIES];
    size_t historyHead = 0;
    size_t historyCount = 0;

    static constexpr size_t SELF_INJECTED_ID_COUNT = 8;
    uint32_t selfInjectedIds[SELF_INJECTED_ID_COUNT] = {0};
    size_t selfInjectedIndex = 0;

    // HTTP task (runs all blocking HTTPS calls in a separate FreeRTOS task)
    TaskHandle_t _httpTaskHandle = nullptr;
    QueueHandle_t _incomingQueue = nullptr; // TelegramMessage* pointers from HTTP task
    volatile bool _httpTaskShouldStop = false;

    void loadConfig();
    bool saveSettingsToNvsLocked();
    void refreshOperationalStateLocked();
    bool isConfiguredLocked() const;

    bool parseChatId(const std::string &rawChatId, int64_t &outChatId) const;
    bool applyChannelsConfig(const std::string &rawChannels);
    static std::string trim(const std::string &value);
    static bool parseChannelNumber(const std::string &token, uint8_t &channelNumber);
    bool isChannelAllowed(uint8_t channel) const;

    static std::string truncateUtf8(const std::string &value, size_t maxBytes);
    static std::string htmlEscape(const std::string &value);

    bool isWifiConnected() const;
    bool matchesConfiguredChat(int64_t incomingChatId) const;

    bool isSelfInjected(uint32_t packetId) const;
    void rememberSelfInjected(uint32_t packetId);

    void enqueueTelegramMessage(const std::string &text);
    void appendHistory(TelegramHistoryDirection direction, TelegramHistoryStatus status, const std::string &chat,
                       const std::string &sender, const std::string &text);
    void appendHistoryLocked(TelegramHistoryDirection direction, TelegramHistoryStatus status, const std::string &chat,
                             const std::string &sender, const std::string &text);

    bool handleTelegramCommand(const TelegramMessage &message);
    std::string buildStatusMessage();

    std::string formatMeshMessage(const meshtastic_MeshPacket *packet) const;
    bool injectToMesh(const std::string &text, const std::string &senderName);

    // HTTP task management
    void startHttpTask();
    void stopHttpTask();
    static void httpTaskEntryPoint(void *param);
    void httpTaskLoop();
    void processIncomingMessage(const TelegramMessage *msg);
};

void telegramInit();
TelegramControlSnapshot telegramGetControlSnapshot();
TelegramControlResult telegramApplyControlPatch(const TelegramControlPatch &patch, TelegramControlSource source);
TelegramControlResult telegramSetEnabled(bool enabled, TelegramControlSource source);
std::vector<TelegramHistoryEntry> telegramGetHistory(const std::string &chatIdFilter,
                                                     TelegramHistoryFilterDirection directionFilter, size_t limit);
std::vector<TelegramHistoryChatSummary> telegramGetHistoryChats(size_t limit);
bool telegramClearHistory();
extern TelegramBridge *telegramBridge;

#else

inline void telegramInit() {}

inline TelegramControlSnapshot telegramGetControlSnapshot()
{
    TelegramControlSnapshot snapshot;
    snapshot.featureAvailable = false;
    snapshot.queueCapacity = 0;
    return snapshot;
}

inline TelegramControlResult telegramApplyControlPatch(const TelegramControlPatch &, TelegramControlSource)
{
    TelegramControlResult result;
    result.error = TelegramControlError::NOT_AVAILABLE;
    result.message = "Telegram bridge is not available in this build";
    return result;
}

inline TelegramControlResult telegramSetEnabled(bool, TelegramControlSource)
{
    TelegramControlResult result;
    result.error = TelegramControlError::NOT_AVAILABLE;
    result.message = "Telegram bridge is not available in this build";
    return result;
}

inline std::vector<TelegramHistoryEntry> telegramGetHistory(const std::string &, TelegramHistoryFilterDirection, size_t)
{
    return std::vector<TelegramHistoryEntry>();
}

inline std::vector<TelegramHistoryChatSummary> telegramGetHistoryChats(size_t)
{
    return std::vector<TelegramHistoryChatSummary>();
}

inline bool telegramClearHistory()
{
    return false;
}

#endif
