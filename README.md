<div align="center" markdown="1">

<img src=".github/meshtastic_logo.png" alt="Meshtastic Logo" width="80"/>
<h1>Meshtastic Firmware (Russian Fork)</h1>

</div>

</div>

<div align="center">
	<a href="https://meshtastic.org">Website</a>
	-
	<a href="https://meshtastic.org/docs/">Documentation</a>
	-
	<a href="https://github.com/meshtastic/firmware">Upstream Repo</a>
</div>

## Overview

This is a fork of the [official Meshtastic firmware](https://github.com/meshtastic/firmware) with Russian language support. The fork is regularly synced with upstream and adds the following features:

<img src=".github/screenshot_ru.jpg" alt="T-Deck with Russian keyboard" width="300"/>

### Russian Language Features

- **Russian OLED display** — Cyrillic text rendering on OLED screens (via `OLED_RU` build flag)
- **Russian keyboard for T-Deck** — Full Russian keyboard layout with EN/RU switching via the [device-ui fork](https://github.com/skrashevich/device-ui/tree/feat-russian-keyboard). Toggle layout by pressing **Left Shift + Mic button** simultaneously

### Telegram Bridge Management (T-Deck TFT)

This fork adds Telegram bridge controls directly in the T-Deck TFT UI so you can configure and apply changes at runtime without rebooting the device.

- **Menu path**: `System -> Connectivity -> Telegram`
- **Supported actions**:
  - Enable/disable Telegram bridge
  - Select traffic direction: `both`, `mesh_to_telegram`, `telegram_to_mesh`
  - Configure `Chat ID`, `Channels` (CSV), and bot token update mode
  - Tune advanced intervals: `Poll interval`, `Long poll timeout`, `Send interval`
  - Reload current runtime snapshot from firmware
- **Live status in UI**:
  - `running / wait_wifi / disabled`
  - `configured yes/no`
  - `wifi connected/disconnected`
  - direction mode (`both / mesh_to_telegram / telegram_to_mesh`)
  - queue usage (`used/capacity`)
- **Security behavior**:
  - token is never shown in full in UI
  - token/chat_id are not intended to be exposed in clear text logs

#### Telegram UI usage

1. Open `System -> Connectivity -> Telegram`.
2. Press `Reload` to load the latest firmware snapshot.
3. Change required fields (`Enabled`, `Direction`, `Chat ID`, `Channels`, intervals). Enable token change mode only when rotating token.
4. Press `Save`.
5. If validation fails, fix the highlighted error and save again.
6. Use `Back` to return to system settings.

#### Direction modes

The `Direction` selector controls which traffic path is active:

- `both` (`Both directions`) — Mesh messages are forwarded to Telegram, and Telegram messages can be injected back into Mesh.
- `mesh_to_telegram` (`Mesh -> Telegram only`) — only outbound forwarding from Mesh to Telegram is active.
- `telegram_to_mesh` (`Telegram -> Mesh only`) — only inbound forwarding from Telegram to Mesh is active.

Notes:

- Direction changes are applied at runtime (no reboot required).
- Direction is persisted in NVS together with other Telegram bridge settings.
- If Telegram bridge is enabled but missing token/chat_id, status shows `Enabled, not configured` regardless of selected direction.

Validation rules applied by UI and firmware API:

- `pollIntervalMs`: `200..60000`
- `sendIntervalMs`: `200..10000`
- `longPollTimeoutSec`: `0..60`
- `chatId`: int64-compatible integer string
- `channels`: empty (all channels) or CSV of channel indexes
- `directionMode`: `both | mesh_to_telegram | telegram_to_mesh`

#### Telegram HTTP API + Web Admin

Firmware also exposes Telegram bridge management over HTTP:

- `GET /api/v1/telegram/config` — read current runtime snapshot
- `PUT /api/v1/telegram/config` — apply partial settings update (`enabled`, `token`, `chat_id`, `channels`, intervals, `direction`)
- `PUT /api/v1/telegram/enabled` — quick enable/disable toggle
- `GET /api/v1/telegram/history/chats` — chat list with incoming/outgoing counters
- `GET /api/v1/telegram/history` — message history with filters (`chat_id`, `direction`, `limit`)
- `DELETE /api/v1/telegram/history` — clear in-memory history buffer

Built-in web interface:

- `GET /admin/telegram` — configuration form + message history viewer by chat
- `/admin` now contains a link to this Telegram page

Message history notes:

- Stored in RAM ring buffer only (not persisted to NVS)
- Shared by all control channels (Telegram commands, UI, HTTP API)
- Contains outgoing/incoming entries with per-entry status (`queued`, `sent`, `send_failed`, `received`, `injected`, etc.)

### Supported Devices

| Device | Russian OLED | Russian Keyboard | Telegram UI Mgmt |
|--------|:---:|:---:|:---:|
| T-Deck (TFT) | ✅ | ✅ | ✅ |
| Other devices with OLED | ✅ | — | — |

### Download

- 📦 **[Latest build artifacts](https://nightly.link/skrashevich/meshtastic-firmware/workflows/main_matrix/develop?preview)** — Pre-built firmware binaries from the `develop` branch

### Building

Follow the standard [Meshtastic build instructions](https://meshtastic.org/docs/development/firmware/build). To build the T-Deck firmware with Russian support:

```bash
pio run -e t-deck-tft
```

### Flashing

- ⚡ **[Flashing Instructions](https://meshtastic.org/docs/getting-started/flashing-firmware/)** – Install or update the firmware on your device.
