#pragma once

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_TELEGRAM && HAS_WIFI && defined(ARCH_ESP32)

#include "telegram/TelegramConfig.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

struct TelegramMessage {
    int64_t update_id = 0;
    int64_t chat_id = 0;
    std::string from_name;
    std::string text;
};

class TelegramAPI
{
  public:
    void setToken(const char *token);
    bool sendMessage(const std::string &chatId, const std::string &text) const;
    int getUpdates(TelegramMessage out[], size_t outSize, uint32_t timeoutSeconds);
    bool isConfigured() const;
    void setUpdateOffset(int64_t offset);

  private:
    std::string token;
    int64_t nextUpdateOffset = 0;

    std::string buildApiUrl(const char *method) const;

    static std::string truncateUtf8(const std::string &value, size_t maxBytes);
    static std::string urlEncode(const std::string &value);
    static std::string jsonUnescape(const std::string &value);

    static bool parseInt64AfterKey(const std::string &text, const char *key, size_t start, int64_t &out);
    static bool parseStringAfterKey(const std::string &text, const char *key, size_t start, std::string &out);

    bool parseUpdate(const std::string &rawUpdate, TelegramMessage &message) const;
};

#endif
