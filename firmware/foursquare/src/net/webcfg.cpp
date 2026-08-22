// webcfg.cpp — layout, buttons and firmware updates over plain HTTP.
//
// WHY THIS EXISTS AT ALL: ArduinoOTA speaks a raw UDP protocol that no browser
// can produce, and the panel layout was only reachable by rebuilding the
// image. Both are now one HTTP request, which is the only update path a person
// without a Terminal can actually use.
//
// SHAPE OF THE THING, deliberately: no JSON parser, no dependency. Every
// setter takes query arguments, which WebServer already parses for us, and the
// only JSON in the file is hand-written into a fixed buffer on the way out.
// Adding ArduinoJson to buy sixty bytes of syntax is not a trade worth making
// on a part with 400 KB of usable RAM and a build that must stay reproducible.
//
// NOTHING HERE IS AUTHENTICATED EXCEPT THE UPDATE, and that is on purpose:
// reading the temperature or moving a widget is worth exactly what physical
// access to the same room is worth, while writing to the flash is not. The
// update endpoint checks the same SHA-256 digest ArduinoOTA already carries,
// so there is one secret for both paths and no new one to lose.
#include "webcfg.h"

#ifndef DEMO_BUILD
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#include "../settings/store.h"
#include "../screens/display.h"
#include "../screens/extras.h"
#include "../app/ui.h"
#include "../app/leds.h"   // the three status LEDs, editable from the app
#include "../secrets.h"
#if __has_include("../build_info.h")
#include "../build_info.h"
#else
#define FOURSQUARE_BUILD_ID "unknown"
#endif

#define WEBCFG_API 6   // 3 = buttons; 4 = LEDs; 5 = weather + the +7d corner; 6 = ticking/bar seconds

static WebServer  server(80);
static bool       started  = false;
static bool       updating = false;
static bool       update_failed = false;
static uint32_t   update_activity_ms = 0;
static const uint32_t UPDATE_STALL_MS = 30000;

bool webcfg_updating() { return updating; }

// CORS is open because the request comes from a page on your machine talking
// to a device on your own network; there is no cookie, no session and nothing
// a hostile origin could learn that it could not learn by scanning the LAN.
static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}

static void send_json(int code, const char *body) {
  cors();
  server.send(code, "application/json", body);
}

// A query argument, clamped, with a default when it is absent or nonsense.
static uint8_t arg_u8(const char *name, uint8_t fallback, uint8_t hi) {
  if (!server.hasArg(name)) return fallback;
  long v = server.arg(name).toInt();
  if (v < 0 || v > hi) return fallback;
  return (uint8_t)v;
}

static void handle_status() {
  char body[780];
  snprintf(body, sizeof body,
    "{\"firmware\":\"4square\",\"build_id\":\"%s\",\"api\":%d,\"ip\":\"%s\",\"ssid\":\"%s\","
    "\"rssi\":%d,\"uptime_s\":%lu,\"temp_c10\":%d,\"humidity\":%u,"
    "\"extras\":1,\"wide\":%d,\"linkedin\":{\"valid\":%s,\"followers\":%ld,\"gained7d\":%ld},"
    "\"hour24\":%u,\"temp_f\":%u,\"page\":%u,"
    "\"buttons\":[%u,%u,%u,%u],"
    "\"leds\":{\"mode\":%u,\"bright\":%u},\"slots\":["
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u}]}",
    FOURSQUARE_BUILD_ID, WEBCFG_API,
    WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), (int)WiFi.RSSI(),
    (unsigned long)(millis() / 1000),
    ui_env.sht_ok ? (int)(ui_env.sht_c * 10.0f) : -9999,
    (unsigned)ui_env.rh,
    extras_wide_pinned(),
    extras_linkedin_valid() ? "true" : "false",
    (long)extras_linkedin_followers(), (long)extras_linkedin_gained(),
    (unsigned)cfg.hour24, (unsigned)cfg.temp_unit, (unsigned)ui_page(),
    (unsigned)btn_page_for(0), (unsigned)btn_page_for(1),
    (unsigned)btn_page_for(2), (unsigned)btn_page_for(3),
    (unsigned)cfg.led_mode, (unsigned)cfg.led_bright,
    (unsigned)cfg.slot_widget[0], (unsigned)cfg.slot_style[0], (unsigned)cfg.slot_overlay[0],
    (unsigned)cfg.slot_widget[1], (unsigned)cfg.slot_style[1], (unsigned)cfg.slot_overlay[1],
    (unsigned)cfg.slot_widget[2], (unsigned)cfg.slot_style[2], (unsigned)cfg.slot_overlay[2],
    (unsigned)cfg.slot_widget[3], (unsigned)cfg.slot_style[3], (unsigned)cfg.slot_overlay[3]);
  send_json(200, body);
}

// SANITIZE, THEN SAVE, THEN REPAINT — in that order, and never skip the first
// step. slot_widget and slot_style are used as raw array indices by the
// renderer, so a value straight off the wire is a crash waiting for a bored
// person with curl. settings_sanitize() is the same clamp the EEPROM loader
// runs, which means an HTTP write cannot put the record into a state a power
// cycle would not already have to survive.
static void handle_layout() {
  char names[4][4] = {{'w','0',0,0},{'s','0',0,0},{'o','0',0,0},{0}};
  for (uint8_t i = 0; i < N_SCREENS; i++) {
    names[0][1] = names[1][1] = names[2][1] = (char)('0' + i);
    // The ceiling is the last DERIVED screen, not W_COUNT: ids 32..50 are the
    // extras in screens/extras.h. settings_sanitize() below still rejects the
    // gap between the two ranges, so a bad id cannot reach the renderer.
    cfg.slot_widget[i]  = arg_u8(names[0], cfg.slot_widget[i],
                                 (uint8_t)(X_FIRST + X_COUNT - 1));
    cfg.slot_style[i]   = arg_u8(names[1], cfg.slot_style[i],   0x7F);
    // 4 is OV_LIWEEK (the weekly LinkedIn gain, bottom-left) and 5 is the
    // seconds bar; both live past the stock overlay enum, so the ceiling is 5.
    cfg.slot_overlay[i] = arg_u8(names[2], cfg.slot_overlay[i], 5);
  }
  if (server.hasArg("hour24")) cfg.hour24    = arg_u8("hour24", cfg.hour24, 1);
  if (server.hasArg("tempf"))  cfg.temp_unit = arg_u8("tempf",  cfg.temp_unit, 1);

  settings_sanitize(cfg);

  // save=0 is the live preview the editor uses while you are still dragging:
  // the panels change immediately, the EEPROM does not. A 24LC256 is good for
  // a hundred thousand writes per page and a drag can produce dozens of
  // requests a second, so the write has to be the explicit act.
  bool save = arg_u8("save", 1, 1) == 1;
  if (save) { settings_mark_dirty(); settings_save(); }

  // SHOW IT NOW. Without this the panels keep whatever they were painting
  // until something else happens to make them dirty — for an hour digit that
  // is up to a minute away, which reads as "saving did nothing". Two things
  // are needed, not one:
  //
  //   1. the clock page has to be the page on screen. If the last button
  //      press left it on sensors, markets or animations, the layout is
  //      simply not what the glass is showing. ui_button(0) is MODE, and it
  //      only cycles the clock variant when the clock page is ALREADY up, so
  //      the guard below matters.
  //   2. a pinned wide animation covers all four panels by design, so an
  //      explicit save has to take them back.
  if (save && extras_wide_pinned() >= 0) extras_set_wide(-1);
  ui_show_layout();   // clock page, variant 0 (the saved styles), repaint now

  // Echo what the clock actually kept, AFTER sanitising. If a slot comes back
  // different from what was sent, the editor can say so instead of claiming a
  // save that the firmware quietly rejected.
  char body[192];
  snprintf(body, sizeof body,
    "{\"ok\":true,\"saved\":%s,\"slots\":["
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u}]}",
    save ? "true" : "false",
    (unsigned)cfg.slot_widget[0], (unsigned)cfg.slot_style[0], (unsigned)cfg.slot_overlay[0],
    (unsigned)cfg.slot_widget[1], (unsigned)cfg.slot_style[1], (unsigned)cfg.slot_overlay[1],
    (unsigned)cfg.slot_widget[2], (unsigned)cfg.slot_style[2], (unsigned)cfg.slot_overlay[2],
    (unsigned)cfg.slot_widget[3], (unsigned)cfg.slot_style[3], (unsigned)cfg.slot_overlay[3]);
  send_json(200, body);
}

// The derived screens and the wide animations, both writable from the editor.
//   wide=-1        stop playing an animation across all four panels
//   wide=64..71    pin one
//   followers=/gained=  push numbers in directly, instead of waiting for the
//                       clock's own fifteen-minute fetch
static void handle_extras() {
  if (server.hasArg("wide")) extras_set_wide((int)server.arg("wide").toInt());
  if (server.hasArg("followers"))
    extras_set_linkedin((int32_t)server.arg("followers").toInt(),
                        (int32_t)server.arg("gained").toInt());
  char body[128];
  snprintf(body, sizeof body,
           "{\"ok\":true,\"wide\":%d,\"followers\":%ld,\"gained7d\":%ld}",
           extras_wide_pinned(), (long)extras_linkedin_followers(),
           (long)extras_linkedin_gained());
  send_json(200, body);
}

// WHAT THE FOUR BUTTONS ON THE BACK DO.
//   b0..b3   the page each short press jumps to (0 clock, 1 sensors,
//            2 markets, 3 animations, 4 settings)
//   reset=1  back to the factory order
//   save=0   try it without spending an EEPROM write
// Holding MODE (LEDs) and holding SET (settings) are deliberately NOT
// remappable: the hold on SET is the way back into the settings page from
// anywhere, so it stays put however these four are arranged.
static void handle_buttons() {
  if (arg_u8("reset", 0, 1) == 1) btn_map_reset();
  char name[3] = { 'b', '0', 0 };
  for (uint8_t i = 0; i < 4; i++) {
    name[1] = (char)('0' + i);
    if (server.hasArg(name))
      btn_page_set(i, arg_u8(name, btn_page_for(i), PG_COUNT - 1));
  }
  bool save = arg_u8("save", 1, 1) == 1;
  if (save) { settings_mark_dirty(); settings_save(); }

  char body[128];
  snprintf(body, sizeof body,
           "{\"ok\":true,\"saved\":%s,\"buttons\":[%u,%u,%u,%u]}",
           save ? "true" : "false",
           (unsigned)btn_page_for(0), (unsigned)btn_page_for(1),
           (unsigned)btn_page_for(2), (unsigned)btn_page_for(3));
  send_json(200, body);
}

// THE THREE STATUS LEDS on the back edge, from the app.
//   mode=0|1    off, or the status light
//   bright=..   how bright, 0-255, still scaled by the room's light
//   fire=<id>   play one signal now, so you can learn what it looks like
//   test=0      stop whatever is playing
// IDLE STAYS DARK either way. mode=1 does not light the LEDs up and leave
// them lit; it lets events — joining, joined, a save, a fault — show
// themselves and then go quiet again. That is the design of the thing and
// there is no setting here that turns it into an always-on lamp.
static void handle_leds() {
  bool touched = false;
  if (server.hasArg("mode")) {
    cfg.led_mode = arg_u8("mode", cfg.led_mode, 1);
    if (!cfg.led_mode) led_all_off();
    touched = true;
  }
  if (server.hasArg("bright")) {
    long v = server.arg("bright").toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    cfg.led_bright = (uint8_t)v;
    touched = true;
  }
  if (touched) { settings_mark_dirty(); settings_save(); }

  // A test signal is NOT a setting: it is fired and forgotten, so asking for
  // one never writes the EEPROM. led_fire() clamps to the real state list.
  if (server.hasArg("fire")) {
    long s = server.arg("fire").toInt();
    if (s > 0 && s < LED_COUNT) led_fire((LedStatus)s);
  }
  if (arg_u8("stop", 0, 1) == 1) led_all_off();

  char body[96];
  snprintf(body, sizeof body, "{\"ok\":true,\"mode\":%u,\"bright\":%u}",
           (unsigned)cfg.led_mode, (unsigned)cfg.led_bright);
  send_json(200, body);
}

static void handle_button() {
  uint8_t b = arg_u8("b", 0, 3);
  bool    l = arg_u8("long", 0, 1) == 1;
  ui_button(b, l);
  send_json(200, "{\"ok\":true}");
}

// The upload is authenticated against OTA_PASS_SHA256 — the digest already
// compiled into the image for ArduinoOTA — so there is exactly one update
// password for both paths. The comparison is length-constant because a
// byte-at-a-time strcmp on a secret over a network is a timing oracle, and a
// 64-character hex string is a very patient thing to guess.
static bool update_authorized() {
  String given = server.arg("auth");
  const char *want = OTA_PASS_SHA256;
  size_t n = strlen(want);
  if (given.length() != n) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) {
    char a = given[i];
    if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');   // hex case is not a secret
    diff |= (uint8_t)(a ^ want[i]);
  }
  return diff == 0;
}

static void handle_update_result() {
  if (Update.hasError() || update_failed) {
    Update.abort();
    updating = false;
    update_activity_ms = 0;
    disp_all_off(false);
    enableLoopWDT();
    WiFi.setTxPower(WIFI_POWER_17dBm);
    send_json(500, "{\"ok\":false,\"error\":\"the image was rejected\"}");
    return;
  }
  send_json(200, "{\"ok\":true,\"rebooting\":true}");
  delay(250);
  ESP.restart();
}

static void handle_update_data() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!update_authorized()) return;              // the POST handler answers 401
    updating = true;
    update_failed = false;
    update_activity_ms = millis();
    Serial.printf("# http update start: %s\n", up.filename.c_str());
    // Same three precautions the ArduinoOTA path takes, for the same measured
    // reasons: the loop watchdog cannot see a blocking transfer, 4 KB of I2C
    // during peak radio current is what used to kill these, and the TX cap
    // exists to protect the LDO with the screens lit — which they are not.
    disableLoopWDT();
    disp_all_off(true);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      update_failed = true;
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!updating || update_failed) return;
    update_activity_ms = millis();
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      update_failed = true;
      Update.abort();
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!updating) return;
    if (Update.end(true)) Serial.printf("# http update ok: %u bytes\n", up.totalSize);
    else                  Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    updating = false;
    update_activity_ms = 0;
    update_failed = true;
    disp_all_off(false);
    enableLoopWDT();
    WiFi.setTxPower(WIFI_POWER_17dBm);
  }
}

static void handle_restart() {
  if (!update_authorized()) {
    send_json(401, "{\"ok\":false,\"error\":\"wrong update password\"}");
    return;
  }
  send_json(200, "{\"ok\":true,\"rebooting\":true}");
  delay(250);
  ESP.restart();
}

// WHY THE RADIO KEPT DISAPPEARING: the ESP32 default is modem sleep, which
// parks the receiver between DTIM beacons to save a few milliamps. On a
// mains-powered clock that saving buys nothing and costs everything — the
// board stops answering pings for seconds at a time, and a router with an
// aggressive idle timeout eventually drops the association entirely. So we
// turn sleep off. Reconnection deliberately remains owned by foursquare.ino's
// single 20-second retry state machine. Enabling the ESP-IDF auto-reconnector
// here as well creates two competing owners: one tries to rejoin the previous
// AP while the other disconnects and tries the fallback AP. That race is what
// made a brief signal dip turn into a long outage.
void webcfg_begin() {
  if (started) return;
  started = true;

  WiFi.setSleep(false);          // never park the receiver
  WiFi.setAutoReconnect(false);  // one reconnect owner: wifi_tick() only
  WiFi.persistent(false);        // credentials are compiled in; avoid needless flash writes

  if (MDNS.begin("foursquare-revo")) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "device", "4square");
  }


  server.on("/api/status", HTTP_GET,  handle_status);
  server.on("/api/layout", HTTP_GET,  handle_layout);
  server.on("/api/layout", HTTP_POST, handle_layout);
  server.on("/api/leds", HTTP_GET,  handle_leds);
  server.on("/api/leds", HTTP_POST, handle_leds);
  server.on("/api/buttons", HTTP_GET,  handle_buttons);
  server.on("/api/buttons", HTTP_POST, handle_buttons);
  server.on("/api/button", HTTP_GET,  handle_button);
  server.on("/api/button", HTTP_POST, handle_button);
  server.on("/api/extras", HTTP_GET,  handle_extras);
  server.on("/api/extras", HTTP_POST, handle_extras);
  server.on("/api/restart", HTTP_POST, handle_restart);
  server.on("/api/update", HTTP_POST,
            []() { if (!update_authorized()) { send_json(401, "{\"ok\":false,\"error\":\"wrong update password\"}"); return; }
                   handle_update_result(); },
            handle_update_data);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
    send_json(404, "{\"ok\":false,\"error\":\"no such endpoint\"}");
  });

  server.begin();
  Serial.print("# webcfg on http://");
  Serial.println(WiFi.localIP());
}

// ---- the LinkedIn numbers --------------------------------------------------
// One small GET every fifteen minutes to the app's public endpoint, which is
// the thing that keeps a day-by-day history and works out the seven-day gain.
// The clock caches whatever it last read, so a failed fetch shows the previous
// number rather than blanking the panel. TLS is unauthenticated on purpose:
// this is a public counter, there is nothing here worth a certificate store,
// and the root bundle would cost more flash than the whole extras module.
static const char *const LINKEDIN_URL =
  "https://project--93f6b6d0-48fb-4dbe-87b6-455b65129623.lovable.app/api/public/linkedin";
static uint32_t li_next_ms = 8000;   // first read shortly after the radio is up

static long json_long(const String &body, const char *key, bool *found) {
  const int at = body.indexOf(key);
  *found = at >= 0;
  if (at < 0) return 0;
  return body.substring(at + (int)strlen(key)).toInt();
}

static void linkedin_tick() {
  if (updating) return;
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if ((int32_t)(now - li_next_ms) < 0) return;
  li_next_ms = now + 15u * 60u * 1000u;

  WiFiClientSecure tls;
  tls.setInsecure();
  // The app has to ask a third party for the weekly figure, so this read is
  // slower than the forecast. Six seconds was cutting it off every time.
  tls.setTimeout(15);
  HTTPClient http;
  if (!http.begin(tls, LINKEDIN_URL)) return;
  http.setTimeout(15000);
  const int code = http.GET();
  if (code == 200) {
    const String body = http.getString();
    bool a = false, b = false;
    const long followers = json_long(body, "\"followers\":", &a);
    const long gained    = json_long(body, "\"gained7d\":", &b);
    if (a) extras_set_linkedin((int32_t)followers, (int32_t)(b ? gained : 0));
  } else {
    // Try again sooner than the full period, but not in a tight loop.
    li_next_ms = now + 60u * 1000u;
  }
  http.end();
}

// ---- the forecast ----------------------------------------------------------
// Same shape as the LinkedIn read: the app talks to the weather service and
// hands the clock four small whole numbers, so there is no parser, no API key
// and no clock in the world that has to understand WMO codes. Every twenty
// minutes is plenty for a high and a chance of rain.
static const char *const WEATHER_URL =
  "https://project--93f6b6d0-48fb-4dbe-87b6-455b65129623.lovable.app/api/public/weather";
static uint32_t wx_next_ms = 12000;

static void weather_tick() {
  if (updating) return;
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if ((int32_t)(now - wx_next_ms) < 0) return;
  wx_next_ms = now + 20u * 60u * 1000u;

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(6);
  HTTPClient http;
  if (!http.begin(tls, WEATHER_URL)) return;
  http.setTimeout(6000);
  const int code = http.GET();
  if (code == 200) {
    const String body = http.getString();
    bool a = false, b = false, cc = false, dd = false;
    const long icon = json_long(body, "\"icon\":", &a);
    const long cur  = json_long(body, "\"cur_c10\":", &b);
    const long mx   = json_long(body, "\"max_c10\":", &cc);
    const long pop  = json_long(body, "\"pop\":", &dd);
    bool ee = false;
    const long mn   = json_long(body, "\"min_c10\":", &ee);
    if (a && b) {
      extras_set_weather((uint8_t)icon, (int16_t)cur,
                         (int16_t)(cc ? mx : cur), (int16_t)(ee ? mn : cur),
                         (uint8_t)(dd ? pop : 0));
    }
  } else {
    wx_next_ms = now + 60u * 1000u;
  }
  http.end();
}

void webcfg_tick() {
  if (!started) return;
  server.handleClient();

  // A browser can close or lose Wi-Fi midway through a multipart upload
  // without WebServer delivering UPLOAD_FILE_ABORTED. Never leave the display
  // dark, TX power raised, and the loop watchdog disabled indefinitely.
  if (updating && update_activity_ms != 0 &&
      (uint32_t)(millis() - update_activity_ms) > UPDATE_STALL_MS) {
    Serial.println("# http update stalled; restoring normal operation");
    Update.abort();
    updating = false;
    update_failed = true;
    update_activity_ms = 0;
    disp_all_off(false);
    enableLoopWDT();
    WiFi.setTxPower(WIFI_POWER_17dBm);
  }

  // The rolling history behind the trend and high/low screens. Cheap, and it
  // has to run even while the panels are showing something else.
  extras_tick(millis(), ui_env.sht_ok ? (int16_t)(ui_env.sht_c * 10.0f) : 0,
              ui_env.rh, ui_env.sht_ok, rtc_now(millis()).hour);

  linkedin_tick();
  weather_tick();
}
#endif  // !DEMO_BUILD
