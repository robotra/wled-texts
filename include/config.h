#pragma once

// ── Config portal ─────────────────────────────────────────────────────────────
#define CONFIG_AP_SSID            "SMS-Matrix-Setup"  // AP name shown on devices
#define CONFIG_PORTAL_TIMEOUT_MS  300000              // portal window (ms)

// ── LED Matrix ────────────────────────────────────────────────────────────────
#define LED_PIN         16     // GPIO pin connected to matrix DIN
#define MATRIX_WIDTH    32
#define MATRIX_HEIGHT   8
#define LED_BRIGHTNESS  20    // 0-255; keep low when powering from USB

// Matrix wiring layout — adjust if text appears garbled:
//   NEO_MATRIX_ZIGZAG      = serpentine (most common for hand-wired panels)
//   NEO_MATRIX_PROGRESSIVE = all rows left-to-right (common on pre-made panels)
#define MATRIX_TYPE  (NEO_MATRIX_TOP + NEO_MATRIX_LEFT + \
                      NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG)

// ── NTP / time ────────────────────────────────────────────────────────────────
#define NTP_SERVER  "pool.ntp.org"
// POSIX timezone string — handles DST automatically.
// Common US values:
//   Eastern : "EST5EDT,M3.2.0,M11.1.0"
//   Central : "CST6CDT,M3.2.0,M11.1.0"
//   Mountain: "MST7MDT,M3.2.0,M11.1.0"
//   Pacific : "PST8PDT,M3.2.0,M11.1.0"
#define TZ_POSIX  "EST5EDT,M3.2.0,M11.1.0"

// ── Network services ──────────────────────────────────────────────────────────
// mDNS hostname — device will be reachable at http://<MDNS_HOSTNAME>.local/
#define MDNS_HOSTNAME  "sms-matrix"

// Settings web server credentials (HTTP Basic Auth)
#define WEB_USERNAME  "admin"
#define WEB_PASSWORD  "sms-matrix"

// OTA update password (use Arduino IDE / PlatformIO OTA upload)
#define OTA_PASSWORD  "sms-matrix"

// ── Text / scroll ─────────────────────────────────────────────────────────────
#define TEXT_COLOR_R    255
#define TEXT_COLOR_G    255
#define TEXT_COLOR_B    255

#define SCROLL_DELAY_MS   50    // ms between scroll steps (lower = faster)
#define POLL_INTERVAL_MS  5000  // ms between Twilio API polls
