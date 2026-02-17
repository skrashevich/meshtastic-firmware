---
source: Official GitHub repository files
library: TelegramESP32
package: telegramesp32
topic: architecture-dependencies-api-surface-memory-runtime-control
fetched: 2026-02-17T00:00:00Z
official_docs: https://github.com/crozone-technology/TelegramESP32
---

## Repository and version

- Repo: `crozone-technology/TelegramESP32`
- `library.properties` version: `0.1.2`

## Dependencies and platform assumptions

From `library.properties` and headers:
- Direct dependency declared: `ArduinoJson`
- Includes/uses: `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`, `ArduinoJson.h`
- Assumes Arduino-style environment on ESP32-class targets.

## Internal architecture (from `src/TelegramESP32.h/.cpp`)

- Single class `TelegramESP32` holding:
  - one `WiFiClientSecure`
  - one `HTTPClient`
  - fixed chat table (`MAX_CHATS = 5`)
  - static buffers: `msgBuffer[256]`, `urlBuffer[128]`, `payloadBuffer[256]`
- Polling model:
  - `loop()` -> `receiveMessage()` -> `getUpdates()`
  - parses only latest update from returned array (`result[result.size()-1]`)
- Message sending:
  - form payload built as `chat_id=%s&text=%s`
  - `Content-Type: application/x-www-form-urlencoded`
  - minimum interval gate via `MESSAGE_MIN_INTERVAL` (default 1000 ms)

## Public API surface

Public methods:
- `begin()`
- `sendMessage(const String&)`
- `sendMessageToChat(const String& chatName, const String& message)`
- `broadcast(const String&)`
- `receiveMessage()`
- `setMessageCallback(MessageCallback)`
- `setMessageInterval(unsigned long interval)`
- `loop()`
- `addChat(String id, ChatType type, String name = "")`

## Runtime and memory tradeoffs

Observed tradeoffs from implementation:
- TLS trust: `client.setInsecure()` (no cert validation)
- `DynamicJsonDocument(1024)` in parse path; may fail on larger `getUpdates` payloads.
- Fixed small buffers (`urlBuffer[128]`, `payloadBuffer[256]`) risk truncation with long token/text.
- Uses Arduino `String`, increasing heap churn potential over long uptime.
- `sendRequest` treats any positive HTTP code as success (`httpCode > 0`), not strict 2xx.

## Low-level control capability assessment

Requested control features vs library support:
- Custom outbound queueing: **Not provided**
- Retry/backoff strategy: **Not provided**
- Channel/chat filtering policy beyond callback logic: **Minimal**
- Full update stream processing: **Limited** (keeps only last message text)
- Offset durability/persistence hooks: **Not provided**
- Parse-mode/entity control in send API: **Not exposed**

Conclusion:
- Good for simple demo-level notify/command flows.
- Not ideal when firmware needs deterministic queueing, robust backoff, strict filtering, or durable long-poll cursor management.

## Primary links used

- Repo: https://github.com/crozone-technology/TelegramESP32
- README: https://raw.githubusercontent.com/crozone-technology/TelegramESP32/main/README.md
- Header: https://raw.githubusercontent.com/crozone-technology/TelegramESP32/main/src/TelegramESP32.h
- Source: https://raw.githubusercontent.com/crozone-technology/TelegramESP32/main/src/TelegramESP32.cpp
- Library metadata: https://raw.githubusercontent.com/crozone-technology/TelegramESP32/main/library.properties
