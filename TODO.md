# Meshtastic ↔ Telegram Bridge (firmware-level)

## Context

Двунаправленный мост между mesh-сетью Meshtastic и Telegram прямо в прошивке ESP32.
Текстовые сообщения из mesh пересылаются в Telegram-чат, а сообщения из Telegram инжектируются в mesh-сеть.
Аналог MQTT-моста, но для Telegram Bot API.

**Выбор:**
- Каналы: настраиваемый список (через конфиг)
- Конфигурация: NVS Preferences + compile-time дефолты
- Платформа: все ESP32 с WiFi (HAS_WIFI)

## Архитектурное решение

Следуем паттерну MQTT-модуля (`/src/mqtt/MQTT.h`):
- Отдельный класс `TelegramBridge`, наследующий `concurrency::OSThread` (не MeshModule)
- Реализует `Observer<const meshtastic_MeshPacket *>` для подписки на `TextMessageModule`
- Инжекция в mesh через `service->sendToMesh()`
- HTTPS через `WiFiClientSecure` + `HTTPClient`
- Условная компиляция: `MESHTASTIC_EXCLUDE_TELEGRAM`

## Новые файлы (5)

| Файл | Назначение |
|------|------------|
| `src/telegram/TelegramConfig.h` | Константы, дефолты, конфигурация |
| `src/telegram/TelegramAPI.h` | HTTP-клиент Telegram Bot API (заголовок) |
| `src/telegram/TelegramAPI.cpp` | HTTP-клиент: sendMessage, getUpdates |
| `src/telegram/TelegramBridge.h` | Основной класс моста (заголовок) |
| `src/telegram/TelegramBridge.cpp` | OSThread + Observer, вся логика моста |

## Модифицируемые файлы (3)

| Файл | Изменение |
|------|-----------|
| `src/configuration.h` | Добавить `MESHTASTIC_EXCLUDE_TELEGRAM` (default=1) |
| `src/main.cpp` | Добавить `telegramInit()` после `mqttInit()` |
| `variants/esp32s3/t-deck/platformio.ini` | Build-флаги для T-Deck |

---

## Задачи

### Задача 1: Конфигурация и система сборки
- [ ] Создать `src/telegram/TelegramConfig.h` с дефолтами:
  - `TELEGRAM_BOT_TOKEN` (пустая строка)
  - `TELEGRAM_CHAT_ID` (пустая строка)
  - `TELEGRAM_CHANNELS` (пустая строка = все каналы, или "0,1,3")
  - `TELEGRAM_POLL_INTERVAL_MS` (3000)
  - `TELEGRAM_LONG_POLL_TIMEOUT` (0 — non-blocking)
  - `TELEGRAM_MAX_QUEUE_SIZE` (16)
  - `TELEGRAM_SEND_INTERVAL_MS` (1000)
  - `TELEGRAM_API_HOST` / `TELEGRAM_API_PORT`
- [ ] В `src/configuration.h` добавить `MESHTASTIC_EXCLUDE_TELEGRAM` (default=1) рядом с другими EXCLUDE
- [ ] В T-Deck platformio.ini добавить `-DMESHTASTIC_EXCLUDE_TELEGRAM=0`

### Задача 2: HTTP-клиент Telegram Bot API
**Зависимости:** Задача 1

- [ ] Создать `src/telegram/TelegramAPI.h` и `src/telegram/TelegramAPI.cpp`

**Класс `TelegramAPI`:**
- `setToken(const char*)` — установить токен бота
- `sendMessage(chatId, text)` → bool — отправка сообщения (POST, `application/x-www-form-urlencoded`)
- `getUpdates(out[], outSize, timeout)` → int — получение обновлений (GET, long polling)
- `isConfigured()` → bool

**Технические детали:**
- TLS: `WiFiClientSecure` с `setInsecure()` (аналогично MQTT — `/src/mqtt/MQTT.cpp:535`)
- Минимальный JSON-парсер (без ArduinoJson): извлечение `update_id`, `chat_id`, `from.first_name`, `text`
- URL-кодирование текста для POST body
- HTTP timeout: 5 сек на соединение
- `getUpdates` с `timeout=0` (non-blocking)

**Структура `TelegramMessage`:**
```cpp
struct TelegramMessage {
    int64_t update_id;
    int64_t chat_id;
    std::string from_name;
    std::string text;
};
```

### Задача 3: Основной класс TelegramBridge
**Зависимости:** Задачи 1, 2

- [ ] Создать `src/telegram/TelegramBridge.h` и `src/telegram/TelegramBridge.cpp`

**Класс `TelegramBridge` : `concurrency::OSThread`, `Observer<const meshtastic_MeshPacket *>`**

**Состояния (FSM):**
```
STATE_DISABLED → (нет конфига)
STATE_WAIT_WIFI → STATE_RUNNING → STATE_WAIT_WIFI (WiFi потерян)
```

**Ключевые методы:**

1. **Конструктор** — загрузка конфига из NVS (с fallback на compile-time), инициализация API, очереди
2. **`runOnce()`** — FSM: проверка WiFi → отправка очереди → опрос Telegram → return interval
3. **`onNotify(mp)`** — Observer callback: фильтрация по каналам, форматирование, отправка/очередь
4. **`formatMeshMessage(mp)`** — формат: `<b>NodeName</b> [SN] (ch N):\nТекст`
5. **`injectToMesh(text, senderName)`** — создание MeshPacket: `[TG|Имя] текст`, sendToMesh
6. **`processIncomingTelegram()`** — getUpdates → фильтр по chat_id → injectToMesh
7. **`sendQueuedMessages()`** — отправка из очереди с rate limiting

**Конфигурация из NVS:**
```cpp
Preferences prefs;
prefs.begin("telegram", true);
token = prefs.getString("bot_token", TELEGRAM_BOT_TOKEN);
chatId = prefs.getString("chat_id", TELEGRAM_CHAT_ID);
channels = prefs.getString("channels", TELEGRAM_CHANNELS); // "0,1,3" или "" = все
prefs.end();
```

**Фильтрация каналов:**
- Парсинг строки `channels` в `std::set<uint8_t>`
- В `onNotify()`: если set не пуст, проверить `mp->channel ∈ set`
- Telegram→Mesh: отправка в первый канал из списка (или Primary)

**Очередь сообщений:**
- `PointerQueue<QueueEntry>` размером `TELEGRAM_MAX_QUEUE_SIZE`
- При переполнении — drop oldest (паттерн MQTT)

**Защита от петель:**
- Кольцевой буфер ID пакетов, инжектированных нами (8 записей)
- В `onNotify()`: пропуск если `mp->id` в буфере

**Rate limiting:**
- Минимум `TELEGRAM_SEND_INTERVAL_MS` между отправками в Telegram
- При N последовательных ошибок — экспоненциальный backoff

### Задача 4: Интеграция в систему инициализации
**Зависимости:** Задача 3

- [ ] Добавить `telegramInit()` в `src/telegram/TelegramBridge.cpp`
- [ ] В `src/main.cpp` после `mqttInit()` добавить вызов:
```cpp
#if !MESHTASTIC_EXCLUDE_TELEGRAM
#include "telegram/TelegramBridge.h"
telegramInit();
#endif
```

**Порядок инициализации:** `setupModules()` → `mqttInit()` → `telegramInit()` — TextMessageModule уже создан.

### Задача 5: Обработка edge cases и надёжность
**Зависимости:** Задача 3

- [ ] Петли: проверка selfInjectedIds в onNotify
- [ ] WiFi disconnect/reconnect: переход STATE_RUNNING → STATE_WAIT_WIFI, сохранение очереди
- [ ] HTTP таймаут: 5 сек, graceful failure
- [ ] Telegram rate limit: 30 msg/sec — соблюдаем через SEND_INTERVAL
- [ ] Длинные сообщения: truncate до 233 байт для mesh, до 4096 для Telegram
- [ ] UTF-8: корректная обработка многобайтных символов (русский текст)
- [ ] Память: мониторинг free heap, ограничение размера очереди

### Задача 6: Runtime-конфигурация через NVS
**Зависимости:** Задача 3

- [ ] Команда `/config channels 0,1,3` в Telegram-чате
- [ ] Команда `/status` — текущее состояние моста
- [ ] Команда `/ping` — проверка связи
- [ ] Сохранение в NVS и применение без перезагрузки

### Задача 7: Тестирование
**Зависимости:** Задачи 4, 5, 6

**Тест-план:**
1. [ ] Компиляция с `MESHTASTIC_EXCLUDE_TELEGRAM=0` для T-Deck — без ошибок
2. [ ] Компиляция с `MESHTASTIC_EXCLUDE_TELEGRAM=1` — размер бинарника не изменился
3. [ ] Запуск без конфига — мост отключается, нет crash
4. [ ] Mesh→Telegram: сообщение с другого узла появляется в Telegram с именем отправителя
5. [ ] Telegram→Mesh: сообщение из чата появляется на mesh-узлах с префиксом `[TG|Имя]`
6. [ ] Потеря WiFi: сообщения буферизуются, отправляются после восстановления
7. [ ] Защита от петель: Telegram-сообщение не эхом возвращается обратно
8. [ ] Одновременная работа с MQTT: оба моста работают без конфликтов

---

## Граф зависимостей

```
Задача 1 (Конфиг) ──→ Задача 2 (API клиент) ──→ Задача 3 (TelegramBridge) ──→ Задача 4 (Интеграция)
                                                        │                            │
                                                        ├──→ Задача 5 (Edge cases)   │
                                                        ├──→ Задача 6 (NVS конфиг)   │
                                                        └──→ Задача 7 (Тестирование) ←┘
```

## Ключевые файлы-образцы

| Файл | Почему важен |
|------|-------------|
| `/src/mqtt/MQTT.h/.cpp` | Паттерн OSThread, WiFiClientSecure, очередь, reconnect |
| `/src/modules/TextMessageModule.h/.cpp` | Observable для подписки на текстовые сообщения |
| `/src/mesh/MeshService.h` | `sendToMesh()` для инжекции в mesh |
| `/src/mesh/Router.h` | `allocForSending()` для создания пакетов |
| `/src/mesh/NodeDB.h` | `getMeshNode()` для получения имени отправителя |
| `/src/configuration.h` | Паттерн MESHTASTIC_EXCLUDE_* |
