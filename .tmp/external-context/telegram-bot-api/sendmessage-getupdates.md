---
source: Context7 API + official Telegram Bot API docs
library: Telegram Bot API
package: telegram-bot-api
topic: sendmessage-getupdates-long-polling-encoding-parse-mode
fetched: 2026-02-17T00:00:00Z
official_docs: https://core.telegram.org/bots/api
---

## Scope

Focused extraction for firmware integration around:
- `sendMessage`
- `getUpdates`
- long polling (`timeout`, `offset`, `limit`)
- request encoding (`URL query`, `application/x-www-form-urlencoded`)
- message formatting (`parse_mode` and HTML style support)
- practical message size constraints

## Request model and encoding

From official Bot API docs (`Making requests`):
- Endpoint shape: `https://api.telegram.org/bot<token>/METHOD_NAME`
- Supports `GET` and `POST`
- Parameter transport supported:
  - URL query string
  - `application/x-www-form-urlencoded`
  - `application/json` (except file upload)
  - `multipart/form-data`
- Requests must use UTF-8.

Firmware implication:
- For ESP32, `POST` + `application/x-www-form-urlencoded` is a lightweight default.
- URL/form escaping is required for reserved characters in `text` (`&`, `=`, `+`, `%`, `#`, etc.), otherwise message body gets corrupted.

## getUpdates long polling behavior

From official `getUpdates` section:
- `offset`:
  - should be `highest_seen_update_id + 1`
  - update is considered confirmed once `getUpdates` is called with an `offset` higher than that `update_id`
  - negative offset is supported to pull from end of queue and drop earlier backlog
- `limit`: `1-100`, default `100`
- `timeout`: seconds for long polling; default `0` (short polling)
- Not usable while webhook is configured.
- Telegram stores updates for up to 24h.

Firmware implication:
- Keep `last_update_id` in RAM and (optionally) NVS if reboot continuity matters.
- Use long polling (`timeout > 0`) to reduce wakeups and avoid aggressive request loops.

## sendMessage essentials

From Bot API docs and related message object constraints:
- Method: `sendMessage`
- Core parameters: `chat_id`, `text`
- Text length for messages is documented as up to `4096` chars.
- `parse_mode` can be used to request formatted text parsing.
- As alternative to `parse_mode`, explicit `entities` can be supplied.

Formatting references:
- Bot API docs reference HTML formatting mode and formatting options.
- Context7 extraction for Telegram entities confirms common HTML entity aliases:
  - `<b>` / `<strong>`
  - `<i>` / `<em>`
  - `<u>`
  - `<s>` / `<strike>` / `<del>`
  - `<code>`
  - `<pre>`

Firmware implication:
- Prefer plain text by default.
- Enable `parse_mode=HTML` only when needed.
- Escape user/device-sourced text before embedding into HTML mode.

## Minimal robust polling/send loop guidance

1. Build `getUpdates?offset=<last+1>&timeout=<T>&limit=<N>&allowed_updates=["message","channel_post"]`
2. Parse all returned updates in-order.
3. For each processed update, advance `last_update_id`.
4. Only after processing response, issue next poll with new offset.
5. For outbound text, percent-encode form payload values and cap/segment around 4096 chars.

## Primary links used

- Official Bot API: https://core.telegram.org/bots/api
- Official `sendMessage`: https://core.telegram.org/bots/api#sendmessage
- Official `getUpdates`: https://core.telegram.org/bots/api#getupdates
- Context7 library id used: `/websites/core_telegram_api`
