#include "telegram/TelegramAPI.h"

#if !MESHTASTIC_EXCLUDE_TELEGRAM && HAS_WIFI && defined(ARCH_ESP32)

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{
int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string &output, uint32_t codepoint)
{
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string toStringInt64(int64_t value)
{
    char buffer[32] = {0};
    snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return std::string(buffer);
}

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

bool responseIsOk(const std::string &response)
{
    return response.find("\"ok\":true") != std::string::npos || response.find("\"ok\": true") != std::string::npos;
}
} // namespace

void TelegramAPI::setToken(const char *newToken)
{
    token = newToken ? newToken : "";
}

bool TelegramAPI::isConfigured() const
{
    return !token.empty();
}

void TelegramAPI::setUpdateOffset(int64_t offset)
{
    nextUpdateOffset = std::max<int64_t>(offset, 0);
}

std::string TelegramAPI::truncateUtf8(const std::string &value, size_t maxBytes)
{
    return value.substr(0, utf8SafePrefixLength(value, maxBytes));
}

std::string TelegramAPI::buildApiUrl(const char *method) const
{
    if (!isConfigured() || method == nullptr)
        return std::string();

    std::string url = "https://";
    url += TELEGRAM_API_HOST;
    if (TELEGRAM_API_PORT != 443) {
        url += ":";
        url += std::to_string(TELEGRAM_API_PORT);
    }
    url += "/bot";
    url += token;
    url += "/";
    url += method;
    return url;
}

std::string TelegramAPI::urlEncode(const std::string &value)
{
    static const char *hex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return encoded;
}

std::string TelegramAPI::jsonUnescape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }

        if (++i >= value.size())
            break;

        switch (value[i]) {
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            if (i + 4 >= value.size())
                break;

            const int h1 = hexValue(value[i + 1]);
            const int h2 = hexValue(value[i + 2]);
            const int h3 = hexValue(value[i + 3]);
            const int h4 = hexValue(value[i + 4]);
            if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0)
                break;

            const uint32_t codepoint = static_cast<uint32_t>((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
            appendUtf8(out, codepoint);
            i += 4;
            break;
        }
        default:
            out.push_back(value[i]);
            break;
        }
    }

    return out;
}

bool TelegramAPI::parseInt64AfterKey(const std::string &text, const char *key, size_t start, int64_t &out)
{
    if (key == nullptr)
        return false;

    const size_t keyPos = text.find(key, start);
    if (keyPos == std::string::npos)
        return false;

    size_t pos = keyPos + std::strlen(key);
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        pos++;
    }

    bool isNegative = false;
    if (pos < text.size() && text[pos] == '-') {
        isNegative = true;
        pos++;
    }

    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos])))
        return false;

    int64_t value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        value = (value * 10) + static_cast<int64_t>(text[pos] - '0');
        pos++;
    }

    out = isNegative ? -value : value;
    return true;
}

bool TelegramAPI::parseStringAfterKey(const std::string &text, const char *key, size_t start, std::string &out)
{
    if (key == nullptr)
        return false;

    const size_t keyPos = text.find(key, start);
    if (keyPos == std::string::npos)
        return false;

    size_t pos = keyPos + std::strlen(key);
    std::string raw;
    raw.reserve(64);

    bool escaped = false;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (escaped) {
            raw.push_back(ch);
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            raw.push_back(ch);
            escaped = true;
            continue;
        }

        if (ch == '"') {
            out = jsonUnescape(raw);
            return true;
        }

        raw.push_back(ch);
    }

    return false;
}

bool TelegramAPI::parseUpdate(const std::string &rawUpdate, TelegramMessage &message) const
{
    if (!parseInt64AfterKey(rawUpdate, "\"update_id\":", 0, message.update_id))
        return false;

    size_t messagePos = rawUpdate.find("\"message\"");
    if (messagePos == std::string::npos)
        messagePos = rawUpdate.find("\"channel_post\"");
    if (messagePos == std::string::npos)
        return false;

    const size_t chatPos = rawUpdate.find("\"chat\"", messagePos);
    if (chatPos == std::string::npos)
        return false;

    if (!parseInt64AfterKey(rawUpdate, "\"id\":", chatPos, message.chat_id))
        return false;

    const size_t fromPos = rawUpdate.find("\"from\"", messagePos);
    if (fromPos != std::string::npos) {
        parseStringAfterKey(rawUpdate, "\"first_name\":\"", fromPos, message.from_name);
    }

    if (message.from_name.empty()) {
        message.from_name = "Telegram";
    }

    if (!parseStringAfterKey(rawUpdate, "\"text\":\"", messagePos, message.text))
        return false;

    return !message.text.empty();
}

bool TelegramAPI::sendMessage(const std::string &chatId, const std::string &text) const
{
    if (!isConfigured() || chatId.empty())
        return false;

    const std::string url = buildApiUrl("sendMessage");
    if (url.empty())
        return false;

    const std::string safeText = truncateUtf8(text, TELEGRAM_MAX_TEXT_SIZE);
    std::string body = "chat_id=" + urlEncode(chatId);
    body += "&parse_mode=HTML";
    body += "&text=" + urlEncode(safeText);

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);

    if (!http.begin(secureClient, String(url.c_str()))) {
        LOG_WARN("Telegram API begin() failed for sendMessage");
        return false;
    }

    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    const int httpCode = http.POST(String(body.c_str()));
    if (httpCode <= 0) {
        LOG_WARN("Telegram sendMessage POST failed: %d", httpCode);
        http.end();
        return false;
    }

    const String response = http.getString();
    http.end();
    return (httpCode >= 200 && httpCode < 300) && responseIsOk(std::string(response.c_str()));
}

int TelegramAPI::getUpdates(TelegramMessage out[], size_t outSize, uint32_t timeoutSeconds)
{
    if (!isConfigured() || out == nullptr || outSize == 0)
        return 0;

    std::string url = buildApiUrl("getUpdates");
    if (url.empty())
        return 0;

    url += "?timeout=" + std::to_string(timeoutSeconds);
    url += "&limit=" + std::to_string(outSize);
    if (nextUpdateOffset > 0) {
        url += "&offset=" + toStringInt64(nextUpdateOffset);
    }

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    const uint32_t readTimeoutMs = TELEGRAM_HTTP_TIMEOUT_MS + (timeoutSeconds * 1000);
    http.setTimeout(readTimeoutMs);

    if (!http.begin(secureClient, String(url.c_str()))) {
        LOG_WARN("Telegram API begin() failed for getUpdates");
        return 0;
    }

    const int httpCode = http.GET();
    if (httpCode <= 0) {
        LOG_WARN("Telegram getUpdates GET failed: %d", httpCode);
        http.end();
        return 0;
    }

    if (httpCode < 200 || httpCode >= 300) {
        LOG_WARN("Telegram getUpdates HTTP error: %d", httpCode);
        http.end();
        return 0;
    }

    const String responseString = http.getString();
    http.end();

    const std::string response(responseString.c_str());
    if (!responseIsOk(response))
        return 0;

    int parsedCount = 0;
    int64_t maxUpdateId = nextUpdateOffset > 0 ? (nextUpdateOffset - 1) : -1;

    size_t cursor = 0;
    while (parsedCount < static_cast<int>(outSize)) {
        const size_t updateStart = response.find("\"update_id\":", cursor);
        if (updateStart == std::string::npos)
            break;

        const size_t updateEnd = response.find("\"update_id\":", updateStart + 1);
        const size_t length = (updateEnd == std::string::npos) ? std::string::npos : (updateEnd - updateStart);
        const std::string rawUpdate = response.substr(updateStart, length);

        TelegramMessage message;
        if (parseUpdate(rawUpdate, message)) {
            out[parsedCount++] = std::move(message);
            maxUpdateId = std::max(maxUpdateId, out[parsedCount - 1].update_id);
        }

        cursor = (updateEnd == std::string::npos) ? response.size() : updateEnd;
    }

    if (maxUpdateId >= 0)
        nextUpdateOffset = maxUpdateId + 1;

    return parsedCount;
}

#endif
