#pragma once

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_TELEGRAM && HAS_WIFI && defined(ARCH_ESP32)

#include "Observer.h"
#include "concurrency/OSThread.h"
#include "mesh/PointerQueue.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "telegram/TelegramAPI.h"
#include "telegram/TelegramConfig.h"

#include <stddef.h>
#include <set>
#include <string>

class TelegramBridge : public concurrency::OSThread, public Observer<const meshtastic_MeshPacket *>
{
  public:
    TelegramBridge();

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

    State state = State::STATE_DISABLED;

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
    bool parseChatId(const std::string &rawChatId, int64_t &outChatId) const;
    bool applyChannelsConfig(const std::string &rawChannels, bool persist);
    bool saveChannelsConfig(const std::string &rawChannels);
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
extern TelegramBridge *telegramBridge;

#else

inline void telegramInit() {}

#endif
