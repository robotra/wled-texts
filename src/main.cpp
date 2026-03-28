// =============================================================================
//  SMS Matrix — main.cpp
//
//  Scrolls the most-recent inbound Twilio SMS across an 8×32 NeoPixel matrix.
//
//  Boot flow
//  ---------
//  1. Power on → matrix initialised, NVS opened.
//  2. If the device has been power-cycled 3 times in quick succession the NVS
//     is wiped and the config portal opens so the user can re-enter credentials.
//  3. If no WiFi SSID is stored the config portal opens automatically.
//  4. Config is loaded from NVS, the device connects to WiFi in station mode.
//  5. NTP sync begins; timezone (POSIX string) is loaded from NVS so DST is
//     handled automatically for !time.
//  6. Settings web server starts on port 80 (HTTP Basic Auth protected).
//     mDNS advertises the device as http://sms-matrix.local/.
//  7. ArduinoOTA is started so firmware can be updated over WiFi.
//  8. A background FreeRTOS task (core 0) polls the Twilio REST API every
//     POLL_INTERVAL_MS milliseconds.
//  9. loop() runs on core 1, servicing OTA, the web server, pending display
//     changes, the !help expiry timer, and the scroll animation.
//
//  Inter-task synchronisation
//  --------------------------
//  The poll task and loop() share `pendingMessage` / `hasPendingMessage`.
//  `msgMutex` ensures the loop() never reads a partially-written string.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>              // WiFi station + soft-AP
#include <WebServer.h>         // Tiny HTTP server used by the config portal
#include <HTTPClient.h>        // Outbound HTTP(S) requests to Twilio
#include <WiFiClientSecure.h>  // TLS-capable TCP socket (used with HTTPClient)
#include <Preferences.h>       // Arduino wrapper around ESP32 NVS (flash KV store)
#include <ArduinoJson.h>       // Deserialises the Twilio JSON response
#include <Adafruit_NeoMatrix.h>// High-level text/graphics on NeoPixel matrices
#include <Adafruit_GFX.h>      // Base graphics library required by NeoMatrix
#include <ESPmDNS.h>           // mDNS — advertises the device as sms-matrix.local
#include <ArduinoOTA.h>        // Over-the-air firmware updates over WiFi
#include "config.h"            // Hardware pin / timing constants (no secrets)

// =============================================================================
//  Matrix object
//  The constructor arguments describe the physical layout; change MATRIX_TYPE
//  in config.h if the text appears mirrored, upside-down, or in the wrong order.
// =============================================================================
Adafruit_NeoMatrix matrix(
    MATRIX_WIDTH,   // number of pixels across
    MATRIX_HEIGHT,  // number of pixels tall
    LED_PIN,        // GPIO connected to the data-in pin of the first pixel
    MATRIX_TYPE,    // wiring topology (progressive / zigzag, origin corner, etc.)
    NEO_GRB + NEO_KHZ800  // colour order and signal frequency — standard WS2812B
);

// =============================================================================
//  Runtime config — loaded from NVS at boot via loadConfig().
//  Never hard-code credentials here; they are entered through the web portal.
// =============================================================================
struct Config {
    String ssid;          // WiFi network name
    String pass;          // WiFi password (may be empty for open networks)
    String twilioSid;     // Twilio Account SID  (starts with "AC…")
    String twilioToken;   // Twilio Auth Token
    String twilioNumber;  // E.164 number that receives SMS (e.g. "+15550001234")
    String whitelist;     // Newline-separated E.164 senders; empty = allow all
};

Config      cfg;          // Global config populated by loadConfig()
Preferences prefs;        // NVS handle — kept open for the lifetime of the sketch
WebServer   server(80);   // HTTP server instance used only during the config portal

// =============================================================================
//  Scrolling state
//  All access to these from loop() only — no locking needed.
// =============================================================================

// The text currently being scrolled across the matrix.
String currentMessage = "Waiting for SMS...";

// Twilio message SID of the last message we displayed.
// Used to detect when a genuinely new message has arrived.
String lastSeenSid = "";

// Horizontal cursor position for the scroll animation.
// Starts at MATRIX_WIDTH (just off the right edge) and decrements each tick.
int scrollX = MATRIX_WIDTH;

// Timestamp (millis()) of the last scroll step — used for non-blocking timing.
unsigned long lastScrollTime = 0;

// When non-zero, millis() timestamp at which to revert from the !help text back
// to the previous message.  Set by !help, checked in loop().
unsigned long helpExpiresAt  = 0;
String        preHelpMessage = "";  // Message to restore when help expires

// =============================================================================
//  Inter-task message handoff (poll task → loop)
//
//  The poll task writes pendingMessage + sets hasPendingMessage = true while
//  holding msgMutex.  loop() picks it up with a non-blocking tryTake so it
//  never stalls the animation.
// =============================================================================
SemaphoreHandle_t msgMutex;                    // Binary mutex protecting the fields below
String            pendingMessage    = "";       // Latest SMS body from the poll task
volatile bool     hasPendingMessage = false;   // Written core 0, read core 1 — must be volatile

// Pending colour change (written by poll task, applied by loop() via matrix.setTextColor)
volatile bool hasPendingColor = false;         // Written core 0, read core 1 — must be volatile
uint8_t pendingColorR   = TEXT_COLOR_R;
uint8_t pendingColorG   = TEXT_COLOR_G;
uint8_t pendingColorB   = TEXT_COLOR_B;

// Pending scroll-speed change.  Speed 1–255: higher = faster.
// Stored internally as a delay (ms): delay = 256 - speed.
// Reset to 128 ms on every boot regardless of last !speed command.
volatile bool hasPendingSpeed    = false;      // Written core 0, read core 1 — must be volatile
unsigned long pendingScrollDelay = 128;
unsigned long currentScrollDelay = 128;        // Active delay used by loop(); fixed at boot

// Pending brightness change.
// Reset to 40 on every boot regardless of last !brightness command.
volatile bool hasPendingBrightness = false;    // Written core 0, read core 1 — must be volatile
uint8_t       pendingBrightness    = 40;

// =============================================================================
//  Config portal HTML
//
//  Stored in flash (PROGMEM) rather than SRAM to save precious heap space.
//  Placeholder tokens ({{SSID}}, etc.) are replaced at request time with
//  the current NVS values so the form is pre-filled on revisits.
//
//  The countdown script in the page mirrors CONFIG_PORTAL_TIMEOUT_MS (300 s).
// =============================================================================
static const char CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
  <title>SMS Matrix Setup</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    *{box-sizing:border-box}
    body{font-family:sans-serif;max-width:420px;margin:30px auto;padding:0 16px;background:#111;color:#eee}
    h1{color:#4fc3f7;margin-bottom:4px}
    p{color:#888;font-size:13px;margin-top:0}
    label{display:block;font-size:13px;margin:12px 0 4px;color:#aaa}
    input,textarea,select{width:100%;padding:9px 10px;border:1px solid #333;background:#222;color:#eee;border-radius:4px;font-size:14px}
    input:focus,textarea:focus,select:focus{border-color:#4fc3f7;outline:none}
    textarea{height:90px;resize:vertical;font-family:monospace}
    button{margin-top:20px;width:100%;padding:12px;background:#4fc3f7;color:#111;border:none;border-radius:4px;font-size:15px;font-weight:bold;cursor:pointer}
    .note{color:#f57c00;font-size:13px;margin-top:14px;text-align:center}
  </style>
</head>
<body>
  <h1>SMS Matrix Setup</h1>
  <p>Connect to <b>SMS-Matrix-Setup</b>, fill in the form, and save before the 5-minute window closes.</p>
  <form method="POST" action="/save">
    <label>WiFi SSID</label>
    <input name="ssid" type="text" value="{{SSID}}" required autocomplete="off">
    <label>WiFi Password</label>
    <input name="pass" type="password" value="{{PASS}}" autocomplete="off">
    <label>Twilio Account SID</label>
    <input name="sid" type="text" value="{{SID}}" required autocomplete="off" placeholder="ACxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx">
    <label>Twilio Auth Token</label>
    <input name="token" type="password" value="{{TOKEN}}" required autocomplete="off">
    <label>Twilio Phone Number</label>
    <input name="number" type="tel" value="{{NUMBER}}" required placeholder="+1xxxxxxxxxx">
    <label>Allowed Senders (one number per line, E.164 format)</label>
    <textarea name="whitelist" placeholder="+12025551234&#10;+14155559876&#10;&#10;Leave empty to allow everyone.">{{WHITELIST}}</textarea>
    <label>Timezone</label>
    <select name="tz" id="tzsel">
      <option value="EST5EDT,M3.2.0,M11.1.0">Eastern (EST/EDT)</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">Central (CST/CDT)</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">Mountain (MST/MDT)</option>
      <option value="MST7">Mountain – Arizona (no DST)</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">Pacific (PST/PDT)</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">Alaska (AKST/AKDT)</option>
      <option value="HST10">Hawaii (HST)</option>
      <option value="UTC0">UTC</option>
    </select>
    <button type="submit">Save &amp; Reboot</button>
  </form>
  <div class="note">Portal closes in <span id="t">300</span>s</div>
  <script>
    var s=300,iv=setInterval(function(){document.getElementById('t').textContent=--s;if(s<=0)clearInterval(iv)},1000);
    // Pre-select stored timezone, or auto-detect from browser if none stored
    (function(){
      var stored="{{TZ}}";
      var ianaMap={
        'America/New_York':'EST5EDT,M3.2.0,M11.1.0',
        'America/Detroit':'EST5EDT,M3.2.0,M11.1.0',
        'America/Indiana/Indianapolis':'EST5EDT,M3.2.0,M11.1.0',
        'America/Chicago':'CST6CDT,M3.2.0,M11.1.0',
        'America/Denver':'MST7MDT,M3.2.0,M11.1.0',
        'America/Phoenix':'MST7',
        'America/Los_Angeles':'PST8PDT,M3.2.0,M11.1.0',
        'America/Anchorage':'AKST9AKDT,M3.2.0,M11.1.0',
        'Pacific/Honolulu':'HST10'
      };
      var val=stored||ianaMap[Intl.DateTimeFormat().resolvedOptions().timeZone]||'';
      var sel=document.getElementById('tzsel');
      for(var i=0;i<sel.options.length;i++){
        if(sel.options[i].value===val){sel.selectedIndex=i;break;}
      }
    })();
  </script>
</body></html>
)rawliteral";

// =============================================================================
//  requireAuth()
//
//  Checks HTTP Basic Auth credentials against WEB_USERNAME / WEB_PASSWORD from
//  config.h.  Call at the top of any handler that should be protected.
//  Returns true if authenticated; false if a 401 challenge was sent (the caller
//  must return immediately without sending another response).
// =============================================================================
bool requireAuth() {
    if (!server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {
        server.requestAuthentication(BASIC_AUTH, "SMS Matrix");
        return false;
    }
    return true;
}

// =============================================================================
//  handleRoot()
//
//  Serves the setup form at GET /.
//  Reads the current NVS values so the form is pre-filled — except passwords,
//  which are intentionally left blank for security.
// =============================================================================
void handleRoot() {
    if (!requireAuth()) return;

    // Copy HTML from flash to a RAM String so we can do replacements on it
    String html = FPSTR(CONFIG_HTML);

    // Substitute each placeholder with the matching stored value.
    // Passwords are shown as empty strings — the user must re-enter them.
    html.replace("{{SSID}}",      prefs.getString("ssid",      ""));
    html.replace("{{PASS}}",      prefs.getString("pass",      ""));  // intentionally blank
    html.replace("{{SID}}",       prefs.getString("tw_sid",    ""));
    html.replace("{{TOKEN}}",     prefs.getString("tw_token",  ""));  // intentionally blank
    html.replace("{{NUMBER}}",    prefs.getString("tw_num",    ""));
    html.replace("{{WHITELIST}}", prefs.getString("whitelist", ""));
    html.replace("{{TZ}}",        prefs.getString("tz_posix",  ""));

    server.send(200, "text/html", html);
}

// =============================================================================
//  handleSave()
//
//  Receives the POST from the setup form, persists each field to NVS, sends an
//  acknowledgment page, then restarts the ESP32 so it boots into normal mode.
//
//  The whitelist is normalised: Windows-style CRLF line endings are converted
//  to LF, and leading/trailing whitespace is stripped before storing.
// =============================================================================
void handleSave() {
    if (!requireAuth()) return;

    // Persist each submitted field; hasArg guards against missing form fields.
    if (server.hasArg("ssid"))   prefs.putString("ssid",     server.arg("ssid"));
    if (server.hasArg("pass"))   prefs.putString("pass",     server.arg("pass"));
    if (server.hasArg("sid"))    prefs.putString("tw_sid",   server.arg("sid"));
    if (server.hasArg("token"))  prefs.putString("tw_token", server.arg("token"));
    if (server.hasArg("number")) prefs.putString("tw_num",   server.arg("number"));
    if (server.hasArg("tz")) {
        String tz = server.arg("tz");
        prefs.putString("tz_posix", tz);
        // Apply immediately so !time reflects the new zone without waiting for reboot
        setenv("TZ", tz.c_str(), 1);
        tzset();
    }

    if (server.hasArg("whitelist")) {
        String wl = server.arg("whitelist");
        // Normalise line endings so the whitelist looks the same regardless of
        // which OS the user's browser is running on.
        wl.replace("\r\n", "\n");  // Windows CRLF → LF
        wl.replace("\r", "\n");    // Old Mac CR  → LF
        wl.trim();                  // Remove surrounding blank lines / spaces
        prefs.putString("whitelist", wl);
    }

    // Acknowledge the save before restarting so the browser receives the reply.
    server.send(200, "text/html",
        "<html><body style='background:#111;color:#4fc3f7;font-family:sans-serif;"
        "padding:40px;text-align:center'><h2>Saved! Rebooting...</h2></body></html>");

    delay(1500);      // Give the browser time to render the response
    ESP.restart();    // Full reset; device will boot into normal (non-portal) mode
}

// =============================================================================
//  handleRedirect()
//
//  Catches the automatic "captive portal detection" probes that Android and iOS
//  send when joining an unknown WiFi network and redirects them to the setup
//  page.  Without this, phones may show "No internet — tap to sign in" but then
//  fail to open the portal automatically.
//
//  Known probe URLs:
//    Android : GET /generate_204
//    iOS/macOS: GET /hotspot-detect.html
// =============================================================================
void handleRedirect() {
    server.sendHeader("Location", "http://192.168.4.1/", true);  // absolute URL required
    server.send(302, "text/plain", "");
}

// =============================================================================
//  runConfigPortal()
//
//  Brings up a WiFi soft-AP and serves the setup form until the user submits
//  valid credentials or until CONFIG_PORTAL_TIMEOUT_MS elapses.
//
//  The matrix keeps scrolling while the portal runs so the user can see the
//  AP name and knows the device is waiting.  handleSave() calls ESP.restart()
//  on success, so this function only returns on timeout.
// =============================================================================
void runConfigPortal() {
    // Show the AP name on the matrix so the user knows what to connect to
    currentMessage = String("SETUP: ") + CONFIG_AP_SSID;
    scrollX = MATRIX_WIDTH;  // Reset scroll so the message starts from the right

    // Bring up the soft-AP.  No password = open network, easier for first setup.
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_AP_SSID);
    Serial.println("Config AP started. Connect to: " + String(CONFIG_AP_SSID));
    Serial.println("Portal URL: http://" + WiFi.softAPIP().toString() + "/");

    // Register URL routes
    server.on("/",                    HTTP_GET,  handleRoot);
    server.on("/save",                HTTP_POST, handleSave);
    server.on("/generate_204",        HTTP_GET,  handleRedirect);  // Android captive probe
    server.on("/hotspot-detect.html", HTTP_GET,  handleRedirect);  // iOS captive probe
    server.onNotFound(handleRedirect);  // Catch all other URLs → redirect to portal
    server.begin();

    unsigned long portalStart = millis();

    // Service loop — runs until the portal times out (handleSave restarts early)
    while (millis() - portalStart < CONFIG_PORTAL_TIMEOUT_MS) {
        server.handleClient();  // Process any pending HTTP request

        // Non-blocking scroll animation — identical logic to the normal loop()
        unsigned long now = millis();
        if (now - lastScrollTime >= SCROLL_DELAY_MS) {
            lastScrollTime = now;

            matrix.fillScreen(0);            // Clear all pixels
            matrix.setCursor(scrollX, 1);    // Y=1 centres 7px-tall text in 8 rows
            matrix.print(currentMessage);
            matrix.show();                   // Push pixel data to the strip

            scrollX--;
            // Each character is 6 px wide (5 px glyph + 1 px gap)
            int textPixelWidth = (int)currentMessage.length() * 6;
            // Once the text has fully scrolled off the left edge, reset to the right
            if (scrollX < -textPixelWidth) scrollX = MATRIX_WIDTH;
        }
    }

    // Timeout reached — clean up and return so the caller can decide what to do
    server.stop();
    WiFi.softAPdisconnect(true);  // true = also disable the radio
    Serial.println("Config portal closed, resuming normal boot.");
}

// =============================================================================
//  loadConfig()
//
//  Reads all credential fields from NVS into the global `cfg` struct.
//  Called once after the config portal check in setup().
//  Empty strings are the default so missing keys fail gracefully.
// =============================================================================
void loadConfig() {
    cfg.ssid         = prefs.getString("ssid",      "");
    cfg.pass         = prefs.getString("pass",      "");
    cfg.twilioSid    = prefs.getString("tw_sid",    "");
    cfg.twilioToken  = prefs.getString("tw_token",  "");
    cfg.twilioNumber = prefs.getString("tw_num",    "");
    cfg.whitelist    = prefs.getString("whitelist", "");
}

// =============================================================================
//  isWhitelisted()
//
//  Returns true if `number` is allowed to post messages to the display.
//  If the whitelist is empty every sender is allowed (opt-in filtering).
//
//  The whitelist is stored as a newline-separated list of E.164 numbers.
//  A sentinel "\n" is appended before searching so that a prefix like "+1555"
//  cannot accidentally match "+15550001234".
// =============================================================================
bool isWhitelisted(const String& number) {
    // Empty whitelist → accept messages from anyone
    if (cfg.whitelist.isEmpty()) return true;

    // Append sentinel so every entry — including the last — ends with \n,
    // allowing a simple indexOf(number + "\n") exact-match search.
    String list = cfg.whitelist + "\n";
    return list.indexOf(number + "\n") >= 0;
}

// =============================================================================
//  connectWiFi()
//
//  Connects the ESP32 to the stored WiFi network in station mode.
//  Times out after 15 seconds; if still disconnected it prints a warning
//  and returns — pollTwilio() will retry on the next poll cycle.
//
//  On successful connection the IP address is scrolled across the matrix so
//  the user can confirm connectivity without needing Serial Monitor.
// =============================================================================
void connectWiFi() {
    // Guard: nothing to do if credentials were never saved
    if (cfg.ssid.isEmpty()) {
        Serial.println("No WiFi credentials stored.");
        return;
    }

    Serial.print("Connecting to WiFi: " + cfg.ssid);
    WiFi.mode(WIFI_STA);                            // Station (client) mode
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str()); // Start the association process

    unsigned long start = millis();
    // Poll every 500 ms for up to 15 seconds
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(".");  // Progress dots on Serial Monitor
    }

    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        Serial.println(" connected. IP: " + ip);
        // Scroll the IP address so the user can see it on the matrix
        currentMessage = ip;
        scrollX = MATRIX_WIDTH;
    } else {
        Serial.println(" failed — check credentials.");
        // Caller (pollTwilio) will try again on the next cycle
    }
}

// =============================================================================
//  parseCommand()
//
//  Handles messages that begin with '!'.  Recognised commands:
//
//    !color <name>        — named colour (red, green, blue, white, yellow, cyan,
//                           magenta, orange, purple, pink)
//    !color <R> <G> <B>  — custom colour as three 0-255 integers (persisted)
//    !speed <1-255>       — scroll speed; higher = faster; delay = 256-speed ms
//                           (session only — resets to 128 on next boot)
//    !brightness <0-255>  — LED brightness (session only — resets to 40 on boot)
//    !time                — display current date and time (not persisted)
//    !help                — scroll a summary of available commands
//
//  Color is the only setting persisted across reboots; speed and brightness
//  always start at their fixed boot values (128 and 40 respectively).
//
//  Returns true if the text was a command (even if unrecognised), so the caller
//  knows not to display it as a regular message.
//
//  Must be called from the poll task only; uses msgMutex for all writes.
// =============================================================================
bool parseCommand(const String& text) {
    if (!text.startsWith("!")) return false;

    // Strip the leading '!' and split into verb + argument
    String cmd = text.substring(1);
    cmd.trim();

    int    spaceIdx = cmd.indexOf(' ');
    String verb     = (spaceIdx >= 0) ? cmd.substring(0, spaceIdx) : cmd;
    String arg      = (spaceIdx >= 0) ? cmd.substring(spaceIdx + 1) : "";
    verb.toLowerCase();
    arg.trim();

    // ------------------------------------------------------------------
    //  !color
    // ------------------------------------------------------------------
    if (verb == "color" || verb == "colour") {
        uint8_t r = 0, g = 0, b = 0;
        bool    matched = true;

        String argLower = arg;
        argLower.toLowerCase();

        if      (argLower == "red")     { r=255; g=0;   b=0;   }
        else if (argLower == "green")   { r=0;   g=255; b=0;   }
        else if (argLower == "blue")    { r=0;   g=0;   b=255; }
        else if (argLower == "white")   { r=255; g=255; b=255; }
        else if (argLower == "yellow")  { r=255; g=200; b=0;   }
        else if (argLower == "cyan")    { r=0;   g=255; b=255; }
        else if (argLower == "magenta") { r=255; g=0;   b=255; }
        else if (argLower == "orange")  { r=255; g=100; b=0;   }
        else if (argLower == "purple")  { r=128; g=0;   b=128; }
        else if (argLower == "pink")    { r=255; g=105; b=180; }
        else {
            // Try "R G B" format — three space-separated integers
            int s1 = arg.indexOf(' ');
            int s2 = (s1 >= 0) ? arg.indexOf(' ', s1 + 1) : -1;
            if (s1 > 0 && s2 > 0) {
                r = (uint8_t)constrain(arg.substring(0, s1).toInt(),          0, 255);
                g = (uint8_t)constrain(arg.substring(s1 + 1, s2).toInt(),     0, 255);
                b = (uint8_t)constrain(arg.substring(s2 + 1).toInt(),         0, 255);
            } else {
                Serial.println("!color: unknown colour '" + arg + "'");
                matched = false;
            }
        }

        if (matched) {
            if (xSemaphoreTake(msgMutex, portMAX_DELAY)) {
                pendingColorR   = r;
                pendingColorG   = g;
                pendingColorB   = b;
                hasPendingColor = true;
                xSemaphoreGive(msgMutex);
            }
            // Persist so the colour survives a reboot
            prefs.putUChar("color_r", r);
            prefs.putUChar("color_g", g);
            prefs.putUChar("color_b", b);
            Serial.printf("!color → R=%d G=%d B=%d\n", r, g, b);
        }
        return true;
    }

    // ------------------------------------------------------------------
    //  !speed
    // ------------------------------------------------------------------
    if (verb == "speed") {
        int raw = arg.toInt();
        if (raw < 1 || raw > 255) {
            Serial.println("!speed: argument must be 1-255, got '" + arg + "'");
            return true;
        }
        int speed = raw;
        // Map speed 1–255 to delay 255–1 ms (higher speed = shorter delay)
        unsigned long delayMs = (unsigned long)(256 - speed);
        if (delayMs < 1) delayMs = 1;

        if (xSemaphoreTake(msgMutex, portMAX_DELAY)) {
            pendingScrollDelay = delayMs;
            hasPendingSpeed    = true;
            xSemaphoreGive(msgMutex);
        }
        // Persist so the speed survives a reboot
        prefs.putUChar("scroll_spd", (uint8_t)speed);
        Serial.printf("!speed %d → delay %lums\n", speed, delayMs);
        return true;
    }

    // ------------------------------------------------------------------
    //  !brightness  — set LED brightness 0-255
    // ------------------------------------------------------------------
    if (verb == "brightness") {
        int raw = arg.toInt();
        if (raw < 0 || raw > 255) {
            Serial.println("!brightness: argument must be 0-255, got '" + arg + "'");
            return true;
        }
        uint8_t bri = (uint8_t)raw;
        if (xSemaphoreTake(msgMutex, portMAX_DELAY)) {
            pendingBrightness    = bri;
            hasPendingBrightness = true;
            xSemaphoreGive(msgMutex);
        }
        prefs.putUChar("brightness", bri);
        Serial.printf("!brightness → %d\n", bri);
        return true;
    }

    // ------------------------------------------------------------------
    //  !time  — display the current date and time
    // ------------------------------------------------------------------
    if (verb == "time") {
        struct tm timeinfo;
        String display;
        if (getLocalTime(&timeinfo, 5000)) {
            char buf[32];
            strftime(buf, sizeof(buf), "%a %b %d %I:%M %p", &timeinfo);
            display = String(buf);
        } else {
            display = "Time not synced";
        }
        if (xSemaphoreTake(msgMutex, portMAX_DELAY)) {
            pendingMessage    = display;
            hasPendingMessage = true;
            xSemaphoreGive(msgMutex);
        }
        // Not persisted — a stored timestamp would be stale after reboot
        Serial.println("!time → " + display);
        return true;
    }

    // ------------------------------------------------------------------
    //  !help  — scroll a summary of available commands for 60 seconds,
    //           then restore the previous message automatically
    // ------------------------------------------------------------------
    if (verb == "help") {
        String help = "Commands: !color <name/R G B>  !speed 1-255  !brightness 0-255  !time  !help";
        if (xSemaphoreTake(msgMutex, portMAX_DELAY)) {
            pendingMessage    = help;
            hasPendingMessage = true;
            xSemaphoreGive(msgMutex);
        }
        // helpExpiresAt and preHelpMessage are written here (poll task / core 0)
        // and read in loop() (core 1).  They're only ever set together as a pair
        // so the worst-case race — loop() reads a stale preHelpMessage for one
        // iteration — is benign; it will revert to the right message next tick.
        preHelpMessage = currentMessage;
        helpExpiresAt  = millis() + 60000UL;  // 60 seconds
        Serial.println("!help: will revert in 60 s");
        return true;
    }

    Serial.println("Unknown command: !" + verb);
    return true;  // Was a command attempt (leading '!'), even if unrecognised
}

// =============================================================================
//  pollTwilio()
//
//  Fetches the single most-recent inbound message from the Twilio REST API.
//  If the message SID is new (not already displayed) and the sender is on the
//  whitelist, the message body is handed off to loop() via the mutex-protected
//  pendingMessage / hasPendingMessage pair.
//
//  API endpoint used:
//    GET https://api.twilio.com/2010-04-01/Accounts/{SID}/Messages.json
//        ?To={number}&PageSize=1
//  Authentication: HTTP Basic with Account SID as username, Auth Token as password.
//
//  TLS certificate verification is disabled (setInsecure) — sufficient for a
//  hobby project but should be replaced with pinned certs in production.
// =============================================================================
void pollTwilio() {
    // Skip if Twilio credentials haven't been configured yet
    if (cfg.twilioSid.isEmpty() || cfg.twilioToken.isEmpty()) return;

    // Reconnect if we've lost the WiFi association since the last poll
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        return;  // Don't attempt the HTTP request this cycle; retry next tick
    }

    // URL-encode the '+' in the E.164 number so it survives as a query parameter
    String toNumber = cfg.twilioNumber;
    toNumber.replace("+", "%2B");

    // Build the full API URL requesting only the 1 most-recent message
    String url = "https://api.twilio.com/2010-04-01/Accounts/";
    url += cfg.twilioSid;
    url += "/Messages.json?To=";
    url += toNumber;
    url += "&PageSize=1";

    // WiFiClientSecure handles the TLS handshake; setInsecure() skips cert
    // validation — the connection is still encrypted, just not authenticated.
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    // Twilio uses HTTP Basic Auth: Account SID as username, Auth Token as password
    http.setAuthorization(cfg.twilioSid.c_str(), cfg.twilioToken.c_str());

    int code = http.GET();

    if (code == HTTP_CODE_OK) {
        String body = http.getString();

        // Parse the JSON response.  JsonDocument uses the stack for small docs.
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err) {
            Serial.println("JSON parse error: " + String(err.c_str()));
        } else {
            // Twilio returns {"messages": [...], "end": ..., ...}
            JsonArray messages = doc["messages"];

            if (messages.size() > 0) {
                // We requested PageSize=1 so index 0 is always the newest message
                String sid  = messages[0]["sid"].as<String>();   // Unique message ID
                String text = messages[0]["body"].as<String>();  // SMS body text
                String from = messages[0]["from"].as<String>();  // Sender E.164 number

                if (!isWhitelisted(from)) {
                    // Sender is not on the whitelist — log and ignore
                    Serial.println("Blocked message from: " + from);
                } else if (sid != lastSeenSid) {
                    // This is a message we haven't displayed yet
                    lastSeenSid = sid;
                    // Persist the SID so a reboot won't re-process this message
                    prefs.putString("last_sid", sid);
                    Serial.println("New message [" + sid + "] from " + from + ": " + text);

                    if (text.startsWith("!")) {
                        // Command — parse and apply; do NOT display the raw text
                        parseCommand(text);
                    } else {
                        // Regular message — hand off to loop() for display
                        if (xSemaphoreTake(msgMutex, portMAX_DELAY)) {
                            pendingMessage    = text;
                            hasPendingMessage = true;  // Signal loop() to pick this up
                            xSemaphoreGive(msgMutex);
                        }
                        // Persist so the message is shown again after a reboot
                        prefs.putString("last_msg", text);
                    }
                }
                // else: same SID as last time — nothing new, do nothing
            }
        }
    } else {
        Serial.println("Twilio poll failed, HTTP " + String(code));
    }

    http.end();  // Release the TCP connection and free HTTPClient resources
}

// =============================================================================
//  pollTask()
//
//  FreeRTOS task entry point.  Runs on core 0 so the heavy-ish HTTPS request
//  does not cause frame drops in the scroll animation on core 1.
//
//  vTaskDelay yields the core between polls so the WiFi stack gets CPU time.
// =============================================================================
void pollTask(void* /* unused param */) {
    for (;;) {
        pollTwilio();
        // Block this task for POLL_INTERVAL_MS without burning CPU
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

// =============================================================================
//  scrollTick()
//
//  Advances the marquee by one pixel to the left and redraws the matrix.
//  Called by loop() every SCROLL_DELAY_MS milliseconds.
//
//  Each character in the built-in GFX font is 5 pixels wide with a 1-pixel
//  gap, so total text width = length * 6 pixels.
// =============================================================================
void scrollTick() {
    matrix.fillScreen(0);           // Clear the display (all pixels off)
    matrix.setCursor(scrollX, 1);   // Y=1: top of the 7-tall glyph sits at row 1
    matrix.print(currentMessage);
    matrix.show();                  // Latch the new pixel data out to the LEDs

    scrollX--;  // Move one pixel left each tick

    // Total width of the text in pixels (6 px per character: 5 glyph + 1 gap)
    int textPixelWidth = (int)currentMessage.length() * 6;

    // Once the last pixel of the text has scrolled off the left edge of the
    // matrix, reset the cursor to just beyond the right edge to begin again.
    if (scrollX < -textPixelWidth) scrollX = MATRIX_WIDTH;
}

// =============================================================================
//  setup()
//
//  Runs once at power-on or after a reset.  Initialises hardware, checks for
//  the factory-reset gesture, runs the config portal if needed, loads
//  credentials, connects to WiFi, and spawns the Twilio poll task.
// =============================================================================
void setup() {
    Serial.begin(115200);

    // ---- Matrix init --------------------------------------------------------
    matrix.begin();
    matrix.setTextWrap(false);   // Long text scrolls off the edge instead of wrapping
    matrix.setBrightness(LED_BRIGHTNESS);
    // Pack R, G, B components into the 16-bit colour format NeoMatrix uses
    matrix.setTextColor(matrix.Color(TEXT_COLOR_R, TEXT_COLOR_G, TEXT_COLOR_B));

    // ---- NVS init -----------------------------------------------------------
    // "sms-matrix" is the namespace; false = read-write mode.
    // All credential keys live under this namespace.
    prefs.begin("sms-matrix", false);

    // ---- Factory-reset gesture: 3 rapid power cycles ------------------------
    // Each boot increments a counter.  If three boots occur within 3 seconds of
    // each other (i.e. the counter reaches 3 before the 3-second delay below
    // clears it), the NVS is wiped and the config portal opens.
    // Useful for recovering from wrong credentials without needing Serial access.
    int bootCount = prefs.getInt("boot_cnt", 0) + 1;
    prefs.putInt("boot_cnt", bootCount);

    if (bootCount >= 3) {
        // Three quick resets detected — erase everything and open the portal
        prefs.clear();                    // Wipe the entire "sms-matrix" namespace
        prefs.begin("sms-matrix", false); // Re-open the now-empty namespace
        currentMessage = "RESET";
        scrollX = MATRIX_WIDTH;
        Serial.println("3 rapid power cycles detected — credentials cleared.");
        runConfigPortal();  // Returns only on timeout; handleSave() restarts first
    } else {
        // Normal boot: wait 3 seconds for a potential next rapid cycle, then
        // clear the counter so it does not accumulate across normal reboots.
        delay(3000);
        prefs.putInt("boot_cnt", 0);
    }

    // ---- First-run portal ---------------------------------------------------
    // If no WiFi SSID has been stored yet the user must configure the device.
    // This also fires after a factory reset (NVS was just cleared above).
    if (prefs.getString("ssid", "").isEmpty()) {
        // "Connect to SMS-Matrix-Setup on your phone/laptop to configure."
        runConfigPortal();
    }

    // ---- Normal operation ---------------------------------------------------
    // Load whatever credentials are now stored (guaranteed non-empty here)
    loadConfig();

    // ---- Display settings ---------------------------------------------------
    // Colour is the only setting persisted across reboots.
    // Speed and brightness always start at fixed values so the display is
    // predictable after a power cycle regardless of prior !speed / !brightness
    // commands.  Use !speed or !brightness to adjust for the current session.
    uint8_t savedR = prefs.getUChar("color_r", TEXT_COLOR_R);
    uint8_t savedG = prefs.getUChar("color_g", TEXT_COLOR_G);
    uint8_t savedB = prefs.getUChar("color_b", TEXT_COLOR_B);
    matrix.setTextColor(matrix.Color(savedR, savedG, savedB));
    matrix.setBrightness(40);   // Fixed boot value; !brightness changes for session only
    currentScrollDelay = 128;   // Fixed boot value (≈speed 128); !speed changes for session only
    pendingScrollDelay = 128;

    // Last message SID — prevents the poll task from re-displaying it on first run
    lastSeenSid = prefs.getString("last_sid", "");

    // Last message text — shown immediately so the display isn't blank after reboot
    String savedMsg = prefs.getString("last_msg", "");

    connectWiFi();  // Sets currentMessage = IP on success (useful debug info)

    // ---- NTP — sync time with DST awareness ---------------------------------
    // Load the timezone saved via the settings page; fall back to the compile-
    // time TZ_POSIX from config.h if none has been saved yet.
    // configTime with offset=0 + setenv/tzset lets the C library handle DST
    // automatically, so !time is correct year-round.
    configTime(0, 0, NTP_SERVER);
    String savedTz = prefs.getString("tz_posix", TZ_POSIX);
    setenv("TZ", savedTz.c_str(), 1);
    tzset();

    // ---- Settings web server (HTTP Basic Auth) -------------------------------
    // Reachable at http://<LAN-IP>/ or http://sms-matrix.local/ after mDNS starts.
    // Protected by WEB_USERNAME / WEB_PASSWORD from config.h.
    server.on("/",     HTTP_GET,  handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    Serial.println("Settings server: http://" + WiFi.localIP().toString() + "/");

    // ---- mDNS ---------------------------------------------------------------
    // Advertises the device as http://sms-matrix.local/ so the settings page
    // has a stable hostname even if the DHCP-assigned IP changes.
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: http://" + String(MDNS_HOSTNAME) + ".local/");
    }

    // ---- OTA ----------------------------------------------------------------
    // Allows firmware updates over WiFi from PlatformIO or the Arduino IDE.
    // Password is OTA_PASSWORD from config.h.
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        // Pause the display during the update
        matrix.fillScreen(0);
        matrix.setCursor(0, 1);
        matrix.print("OTA...");
        matrix.show();
    });
    ArduinoOTA.begin();
    Serial.println("OTA ready");

    // After WiFi connect, restore the last message so it scrolls from the start.
    // If no message has ever been received, keep the IP (or "Waiting for SMS…").
    if (!savedMsg.isEmpty()) {
        currentMessage = savedMsg;
        scrollX        = MATRIX_WIDTH;
    }

    // Mutex must be created before the poll task can take it
    msgMutex = xSemaphoreCreateMutex();

    // Pin the poll task to core 0 with 8 KB stack and priority 1.
    // Core 1 is left exclusively for the Arduino loop() / scroll animation.
    xTaskCreatePinnedToCore(
        pollTask,       // Task function
        "twilio_poll",  // Name for debugging
        8192,           // Stack size in bytes (HTTPS + JSON needs headroom)
        NULL,           // No parameter passed to the task
        1,              // Priority (1 = just above idle)
        NULL,           // We don't need a task handle
        0               // Core 0
    );
}

// =============================================================================
//  loop()
//
//  Runs repeatedly on core 1.  Responsibilities:
//
//  1. OTA + web server — ArduinoOTA.handle() and server.handleClient() are
//     called every iteration so firmware updates and settings changes are
//     serviced promptly.
//
//  2. !help expiry — if !help is active and its 60-second window has elapsed,
//     reverts currentMessage to the message that was showing before !help.
//
//  3. Message / settings handoff — picks up pending changes deposited by the
//     poll task under msgMutex (non-blocking tryTake so animation never stalls).
//
//  4. Scroll tick — advances the marquee every currentScrollDelay milliseconds
//     using a millis() timer instead of delay() to avoid stutter.
// =============================================================================
void loop() {
    // ---- Service OTA and the settings web server ----------------------------
    ArduinoOTA.handle();
    server.handleClient();

    // ---- !help expiry — revert to previous message after 60 seconds ---------
    if (helpExpiresAt != 0 && millis() >= helpExpiresAt) {
        helpExpiresAt  = 0;
        currentMessage = preHelpMessage;
        scrollX        = MATRIX_WIDTH;
    }

    // ---- Check for a new message from the poll task -------------------------
    if (hasPendingMessage) {
        // Non-blocking: if the mutex isn't available right now, skip and try
        // next iteration rather than stalling the animation.
        if (xSemaphoreTake(msgMutex, 0)) {
            currentMessage    = pendingMessage;  // Replace the displayed text
            scrollX           = MATRIX_WIDTH;    // Restart scroll from the right edge
            hasPendingMessage = false;           // Acknowledge the handoff
            helpExpiresAt     = 0;              // Cancel any active !help timer
            xSemaphoreGive(msgMutex);
        }
    }

    // ---- Apply pending colour change ----------------------------------------
    if (hasPendingColor) {
        if (xSemaphoreTake(msgMutex, 0)) {
            matrix.setTextColor(matrix.Color(pendingColorR, pendingColorG, pendingColorB));
            hasPendingColor = false;
            xSemaphoreGive(msgMutex);
        }
    }

    // ---- Apply pending speed change -----------------------------------------
    if (hasPendingSpeed) {
        if (xSemaphoreTake(msgMutex, 0)) {
            currentScrollDelay = pendingScrollDelay;
            hasPendingSpeed    = false;
            xSemaphoreGive(msgMutex);
        }
    }

    // ---- Apply pending brightness change ------------------------------------
    if (hasPendingBrightness) {
        if (xSemaphoreTake(msgMutex, 0)) {
            matrix.setBrightness(pendingBrightness);
            hasPendingBrightness = false;
            xSemaphoreGive(msgMutex);
        }
    }

    // ---- Non-blocking scroll timer ------------------------------------------
    unsigned long now = millis();
    if (now - lastScrollTime >= currentScrollDelay) {
        lastScrollTime = now;
        scrollTick();  // Advance one pixel and redraw
    }
}
