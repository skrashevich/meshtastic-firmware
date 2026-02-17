#pragma once

#include "configuration.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

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

    uint32_t lastPollAtMs = 0;
    uint32_t lastSendAtMs = 0;
    uint32_t nextRetryAtMs = 0;
    uint8_t consecutiveSendErrors = 0;
    uint32_t lastHeapWarnMs = 0;

    bool hasPendingMessage = false;
    std::string pendingMessage;

    static constexpr size_t SELF_INJECTED_ID_COUNT = 8;
    uint32_t selfInjectedIds[SELF_INJECTED_ID_COUNT] = {0};
    size_t selfInjectedIndex = 0;

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
    void sendQueuedMessages();
    void processIncomingTelegram();

    bool handleTelegramCommand(const TelegramMessage &message);
    std::string buildStatusMessage();

    std::string formatMeshMessage(const meshtastic_MeshPacket *packet) const;
    bool injectToMesh(const std::string &text, const std::string &senderName);
};

void telegramInit();
TelegramControlSnapshot telegramGetControlSnapshot();
TelegramControlResult telegramApplyControlPatch(const TelegramControlPatch &patch, TelegramControlSource source);
TelegramControlResult telegramSetEnabled(bool enabled, TelegramControlSource source);
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

#endif
