# ТЗ: Управление Telegram bridge из device-ui (T-Deck TFT)

## 1. Цель

Реализовать UI в `device-ui` для включения/отключения и настройки Telegram bridge на T-Deck TFT через единый программный интерфейс прошивки, который не привязан к конкретному каналу вызова и может повторно использоваться из других каналов (например, HTTP API).

## 2. Область работ

- В рамках этого ТЗ: взаимодействие UI с уже добавленным firmware API.
- Вне рамок: реализация полноценного HTTP endpoint (описано как следующий шаг).

## 3. Базовый архитектурный принцип

Все каналы управления (device-ui, в будущем HTTP API, serial API и т.д.) должны вызывать один и тот же слой управления:

- `telegramGetControlSnapshot()`
- `telegramApplyControlPatch(...)`
- `telegramSetEnabled(...)`

Источник изменения передается через `TelegramControlSource`.

Файлы API:

- `src/telegram/TelegramBridge.h`
- `src/telegram/TelegramBridge.cpp`

## 4. Контракт программного интерфейса

### 4.1 Снимок текущего состояния

`TelegramControlSnapshot telegramGetControlSnapshot()`

Ключевые поля:

- `featureAvailable` - фича собрана в текущий build
- `enabled` - логический флаг включения bridge
- `running` - bridge в рабочем состоянии (не только включен, но и выполняется)
- `configured` - есть валидные `token + chat_id`
- `wifiConnected` - текущее состояние WiFi
- `allowAllChannels`, `channels`, `meshChannelForInject`
- `queueUsed`, `queueCapacity`
- `pollIntervalMs`, `longPollTimeoutSec`, `sendIntervalMs`
- `hasToken`, `hasChatId`, `chatId`

### 4.2 Частичное обновление

`TelegramControlResult telegramApplyControlPatch(const TelegramControlPatch&, TelegramControlSource)`

Поддерживаемые поля патча:

- `enabled`
- `token`
- `chatId`
- `channels`
- `pollIntervalMs`
- `longPollTimeoutSec`
- `sendIntervalMs`

### 4.3 Быстрое переключение

`TelegramControlResult telegramSetEnabled(bool, TelegramControlSource)`

### 4.4 Результат операции

`TelegramControlResult`:

- `error`: `NONE | NOT_AVAILABLE | INVALID_ARGUMENT | PERSISTENCE_ERROR`
- `changed`: были ли изменения
- `persisted`: удалось ли сохранить в NVS
- `message`: человекочитаемый итог

## 5. Правила валидации

Должны соблюдаться на UI до отправки и повторно проверяться в firmware API:

- `pollIntervalMs`: `200..60000`
- `sendIntervalMs`: `200..10000`
- `longPollTimeoutSec`: `0..60`
- `chatId`: целое число (строка, приводимая к int64)
- `channels`: пусто (все каналы) или CSV целых индексов, каждый `< channels.getNumChannels()`

## 6. Требования к UI (T-Deck TFT)

## 6.1 Точка входа

Добавить пункт в системное меню:

- `System -> Connectivity -> Telegram`

## 6.2 Экран "Telegram"

Элементы:

- `Enabled` (toggle)
- `Status` (readonly):
  - `running/wait_wifi/disabled`
  - `configured yes/no`
  - `wifi connected/disconnected`
  - `queue used/capacity`
- `Chat ID` (editable text)
- `Channels` (editable CSV)
- `Token`:
  - режим ввода "изменить токен"
  - в отображении не показывать полный токен
  - показывать только факт наличия (`configured/not configured`)
- `Advanced`:
  - `Poll interval (ms)`
  - `Long poll timeout (sec)`
  - `Send interval (ms)`

Кнопки:

- `Save`
- `Reload`
- `Back`

## 6.3 Сценарии взаимодействия

### Сценарий A: открыть экран

1. UI вызывает `telegramGetControlSnapshot()`
2. Заполняет форму значениями
3. Если `featureAvailable=false`, блокирует элементы редактирования и показывает `Telegram not available in this build`

### Сценарий B: переключить enabled

1. UI вызывает `telegramSetEnabled(value, TelegramControlSource::DEVICE_UI)`
2. По успеху показывает `Saved`
3. Обновляет экран новым snapshot

### Сценарий C: сохранить форму

1. UI формирует `TelegramControlPatch` только из реально измененных полей
2. Вызывает `telegramApplyControlPatch(patch, TelegramControlSource::DEVICE_UI)`
3. Обрабатывает `TelegramControlResult`:
   - `NONE`: success
   - `INVALID_ARGUMENT`: показать inline ошибку поля
   - `PERSISTENCE_ERROR`: показать error banner
   - `NOT_AVAILABLE`: показать недоступность функции
4. Делает `Reload` через `telegramGetControlSnapshot()`

### Сценарий D: enabled=true, но не настроено

Если после save получено сообщение вида `bridge needs bot token and chat_id`, UI показывает статус `Enabled, not configured`.

## 7. UX-ограничения

- Не перезагружать устройство после save.
- Изменения применяются сразу (runtime apply).
- В случае ошибки сохранять введенные пользователем значения в форме до явного `Reload`.

## 8. Безопасность

- Не логировать и не отображать токен в открытом виде.
- В debug-выводе UI не печатать token/chat_id целиком.
- Для telemetry/UI состояния использовать только `hasToken/hasChatId` и статусы.

## 9. Расширение на другие каналы (HTTP API и др.)

Любой новый канал управления обязан:

1. Читать через `telegramGetControlSnapshot()`
2. Писать через `telegramApplyControlPatch()`/`telegramSetEnabled()`
3. Передавать корректный `TelegramControlSource`

### Рекомендуемый HTTP mapping (следующий этап)

- `GET /api/v1/telegram/config` -> snapshot
- `PATCH /api/v1/telegram/config` -> patch
- `POST /api/v1/telegram/enabled` -> `{ "enabled": true|false }`

HTTP слой не должен менять настройки напрямую, только через этот API.

## 10. Критерии приемки

1. UI позволяет включать/выключать bridge без reboot.
2. UI позволяет менять `chatId/channels/token` и интервалы.
3. Ошибки валидации корректно отображаются пользователю.
4. Состояние после `Save` совпадает с `telegramGetControlSnapshot()`.
5. Токен не выводится в открытом виде.
6. Конфигурация переживает перезагрузку (NVS persistence).
7. API одинаково пригоден для device-ui и будущего HTTP API.
