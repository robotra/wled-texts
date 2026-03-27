# SMS Matrix Display

An ESP32-based scrolling LED matrix display that shows incoming SMS messages fetched from the Twilio API. Configured entirely through a captive-portal web interface — no code changes needed for setup.

---

## Features

- Scrolls the most recent incoming SMS on a 32×8 NeoPixel matrix
- Remembers and restores the last message and text colour across reboots
- Runtime control via SMS commands (text `!color red`, `!speed 200`, `!help`, etc.)
- Settings page accessible at `http://sms-matrix.local/` (mDNS) — no IP address needed
- Settings page protected by HTTP Basic Auth
- Over-the-air firmware updates (no USB cable needed after first flash)
- DST-aware clock via NTP (`!time` is correct year-round)
- Wi-Fi setup via a captive portal (no hardcoded credentials)
- Displays the device IP address on the matrix after connecting
- Twilio SMS polling every 5 seconds on a dedicated core (smooth scrolling, no hitching)
- Optional sender whitelist (allow only specific phone numbers)
- Credential reset via 3 rapid power cycles

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32 (e.g. ESP32 DevKit V1) |
| LED Matrix | 32×8 NeoPixel / WS2812B panel |
| Data pin | GPIO 16 → matrix DIN |
| Power | 5V — use a dedicated supply for the matrix at higher brightness |

### Wiring

```
ESP32 GPIO 16  ──►  Matrix DIN
ESP32 GND      ──►  Matrix GND
5V supply      ──►  Matrix VCC + ESP32 VIN
```

> **Note:** The first LED is at the bottom-right corner; rows snake in a zigzag (serpentine) pattern upward. If text appears garbled or mirrored, adjust `MATRIX_TYPE` in `include/config.h`.

---

## Software Dependencies

Built with [PlatformIO](https://platformio.org/) targeting the `esp32dev` board.

| Library | Version |
|---|---|
| Adafruit NeoMatrix | ^1.3.0 |
| Adafruit GFX Library | ^1.11.9 |
| ArduinoJson | ^7.3.0 |

Dependencies are declared in `platformio.ini` and installed automatically by PlatformIO. `ESPmDNS` and `ArduinoOTA` are part of the ESP32 Arduino core — no extra installation needed.

---

## Building & Flashing

1. Install [VS Code](https://code.visualstudio.com/) and the [PlatformIO extension](https://platformio.org/install/ide?install=vscode).
2. Open this folder in VS Code.
3. Click **Build** (✓) to compile, then **Upload** (→) to flash.
4. Open the Serial Monitor at **115200 baud** to see debug output.

After the first USB flash, subsequent firmware updates can be done [over the air](#ota-updates).

---

## First-Time Setup

1. Power on the device for the first time (no credentials stored). The matrix scrolls `SETUP: SMS-Matrix-Setup`.
2. On your phone or laptop, connect to the Wi-Fi network **`SMS-Matrix-Setup`**.
3. A captive portal should open automatically. If not, browse to `http://192.168.4.1`.
4. Fill in the form:
   - **WiFi SSID / Password** — your home/office network
   - **Twilio Account SID** — found in the [Twilio Console](https://console.twilio.com/)
   - **Twilio Auth Token** — found in the Twilio Console
   - **Twilio Phone Number** — the number that receives SMS, in E.164 format (e.g. `+12025551234`)
   - **Allowed Senders** *(optional)* — one E.164 number per line; leave blank to accept messages from anyone
5. Click **Save & Reboot**. The device reboots, connects to Wi-Fi, and scrolls its IP address.

The portal stays open for **5 minutes**. If it closes before you save, power-cycle the device and try again.

> On subsequent boots the portal is skipped and the device connects directly to Wi-Fi. To re-open the portal, perform a [credential reset](#resetting-credentials).

---

## Settings Page

After the initial setup, credentials can be updated at any time without a factory reset by visiting the settings page in a browser:

```
http://sms-matrix.local/
```

If mDNS doesn't work on your network, use the IP address scrolled on the matrix at boot (also printed to Serial Monitor).

The settings page includes a **Timezone** dropdown that auto-detects your location from the browser. Saving a new timezone takes effect immediately for `!time` — no reboot required.

The page is protected by **HTTP Basic Auth**. Default credentials:

| Field | Default |
|---|---|
| Username | `admin` |
| Password | `sms-matrix` |

Change these by editing `WEB_USERNAME` and `WEB_PASSWORD` in `include/config.h` before flashing.

---

## OTA Updates

After the first USB flash, firmware can be updated over WiFi:

**PlatformIO** — add to `platformio.ini`:
```ini
upload_protocol = espota
upload_port = sms-matrix.local
upload_flags = --auth=sms-matrix
```
Then use the normal **Upload** button.

**Arduino IDE** — the device appears under **Tools → Port** as a network port once it's on the same WiFi network. Select it and upload normally; enter `sms-matrix` as the password when prompted.

Change the OTA password by editing `OTA_PASSWORD` in `include/config.h`.

---

## Resetting Credentials

To clear stored Wi-Fi and Twilio credentials and re-open the config portal:

1. Power the device **off**.
2. Power it **on** — wait for the matrix to light up, then immediately power it **off** again (within 3 seconds).
3. Repeat once more (3 power cycles total).

The matrix will scroll **RESET** and the config portal will open.

---

## Twilio Setup

1. Create a free account at [twilio.com](https://www.twilio.com/).
2. Buy or verify a phone number that can receive SMS.
3. Note your **Account SID** and **Auth Token** from the Twilio Console dashboard.
4. Enter these in the config portal as described above.

The device polls the Twilio REST API every 5 seconds and displays the most recent inbound message. It tracks the message SID so the same message is not displayed twice, and persists the SID to flash so a reboot never replays an already-seen message.

Polling runs on **core 0** via a FreeRTOS task, so HTTP latency never interrupts the scroll animation on **core 1**.

---

## SMS Commands

Any message that begins with `!` is treated as a command and is not displayed on the matrix. Commands are subject to the sender whitelist — only permitted numbers can change display settings.

**Persistence:** `!color` is saved to flash and survives a reboot. `!speed` and `!brightness` apply for the current session only — the display always starts at speed 128 and brightness 40 on power-up.

Send `!help` to scroll a summary of all commands on the display.

### `!color <name>`

Changes the text colour. Supported names:

`red` · `green` · `blue` · `white` · `yellow` · `cyan` · `magenta` · `orange` · `purple` · `pink`

```
!color red
!color cyan
```

### `!color <R> <G> <B>`

Sets a custom colour using three space-separated integers (0–255 each).

```
!color 255 128 0
```

### `!speed <1-255>`

Sets the scroll speed for the current session. Higher values are faster. The value maps to a frame delay of `256 − speed` milliseconds. Resets to speed 128 (128 ms/step) on next power-up.

```
!speed 200     ← fast (56 ms/step)
!speed 50      ← slow (206 ms/step)
```

### `!brightness <0-255>`

Sets the LED brightness for the current session. Resets to 40 on next power-up. Keep low (≤80) when powered from USB.

```
!brightness 50
!brightness 200
```

### `!time`

Displays the current date and time, e.g. `Fri Mar 27 09:41 AM`. Requires a WiFi connection for NTP sync. DST is handled automatically via the POSIX timezone rule in `config.h`. The result is not saved — it won't reappear after a reboot.

```
!time
```

> Set your timezone by editing `TZ_POSIX` in `include/config.h`. Common values:
> - Eastern: `"EST5EDT,M3.2.0,M11.1.0"`
> - Central: `"CST6CDT,M3.2.0,M11.1.0"`
> - Mountain: `"MST7MDT,M3.2.0,M11.1.0"`
> - Pacific: `"PST8PDT,M3.2.0,M11.1.0"`

### `!help`

Scrolls a summary of all available commands for 60 seconds, then automatically reverts to the previous message.

```
!help
```

---

## Configuration Reference

All compile-time settings are in [include/config.h](include/config.h).

| Define | Default | Description |
|---|---|---|
| `CONFIG_AP_SSID` | `SMS-Matrix-Setup` | Name of the setup Wi-Fi network |
| `CONFIG_PORTAL_TIMEOUT_MS` | `300000` (5 min) | How long the portal stays open |
| `LED_PIN` | `16` | GPIO pin connected to matrix DIN |
| `MATRIX_WIDTH` | `32` | Number of LED columns |
| `MATRIX_HEIGHT` | `8` | Number of LED rows |
| `LED_BRIGHTNESS` | `20` | Compile-time brightness default (runtime boot value is fixed at 40) |
| `MATRIX_TYPE` | `BOTTOM + RIGHT + COLUMNS + ZIGZAG` | Matrix wiring orientation |
| `TEXT_COLOR_R/G/B` | `255, 255, 255` | Default scroll text colour (white) |
| `SCROLL_DELAY_MS` | `50` | Compile-time scroll delay (runtime boot value is fixed at 128 ms) |
| `POLL_INTERVAL_MS` | `5000` | Milliseconds between Twilio API polls |
| `NTP_SERVER` | `pool.ntp.org` | NTP server for `!time` |
| `TZ_POSIX` | `EST5EDT,...` | POSIX timezone rule (handles DST automatically) |
| `MDNS_HOSTNAME` | `sms-matrix` | mDNS hostname (`sms-matrix.local`) |
| `WEB_USERNAME` | `admin` | Settings page username |
| `WEB_PASSWORD` | `sms-matrix` | Settings page password |
| `OTA_PASSWORD` | `sms-matrix` | OTA firmware update password |

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Text is upside-down or mirrored | Adjust `MATRIX_TYPE` in `config.h` (try swapping `TOP`/`BOTTOM`, `LEFT`/`RIGHT`, or `ZIGZAG`/`PROGRESSIVE`) |
| Portal doesn't open automatically | Browse manually to `http://192.168.4.1` while connected to `SMS-Matrix-Setup` |
| Device won't connect to Wi-Fi | Reset credentials (3 power cycles) and re-enter them |
| No messages displayed | Check Twilio credentials and ensure messages are sent **to** the configured number |
| Matrix is too dim / too bright | Send `!brightness <value>` or adjust `LED_BRIGHTNESS` in `config.h` (values above ~80 require a dedicated 5V supply) |
| `!color` / `!speed` command ignored | The sending number must be on the whitelist (or the whitelist must be empty) |
| Display shows old message after reboot | Expected — the last non-command message is restored from flash on every boot |
| `sms-matrix.local` not resolving | Use the IP address shown on the matrix at boot; mDNS can be unreliable on some routers |
| OTA upload fails | Ensure the device is on the same WiFi network and the password in `platformio.ini` matches `OTA_PASSWORD` in `config.h` |
| `!time` shows wrong time | Check `TZ_POSIX` in `config.h` matches your timezone |
