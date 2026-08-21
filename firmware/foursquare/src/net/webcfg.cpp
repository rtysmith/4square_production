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
#include <Update.h>
#include <mbedtls/sha256.h>

#include "../settings/store.h"
#include "../screens/display.h"
#include "../app/ui.h"
#include "../secrets.h"

#define WEBCFG_API 1

static WebServer  server(80);
static bool       started  = false;
static bool       updating = false;
static bool       update_failed = false;

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
  char body[512];
  snprintf(body, sizeof body,
    "{\"firmware\":\"4square\",\"api\":%d,\"ip\":\"%s\",\"ssid\":\"%s\","
    "\"rssi\":%d,\"uptime_s\":%lu,\"temp_c10\":%d,\"humidity\":%u,"
    "\"hour24\":%u,\"temp_f\":%u,\"slots\":["
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u}]}",
    WEBCFG_API,
    WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), (int)WiFi.RSSI(),
    (unsigned long)(millis() / 1000),
    ui_env.sht_ok ? (int)(ui_env.sht_c * 10.0f) : -9999,
    (unsigned)ui_env.rh,
    (unsigned)cfg.hour24, (unsigned)cfg.temp_unit,
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
    cfg.slot_widget[i]  = arg_u8(names[0], cfg.slot_widget[i],  W_COUNT - 1);
    cfg.slot_style[i]   = arg_u8(names[1], cfg.slot_style[i],   0x7F);
    cfg.slot_overlay[i] = arg_u8(names[2], cfg.slot_overlay[i], OV_COUNT - 1);
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

  char body[64];
  snprintf(body, sizeof body, "{\"ok\":true,\"saved\":%s}", save ? "true" : "false");
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
    disp_all_off(false);
    enableLoopWDT();
    WiFi.setTxPower(WIFI_POWER_11dBm);
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
    update_failed = true;
    disp_all_off(false);
    enableLoopWDT();
    WiFi.setTxPower(WIFI_POWER_11dBm);
  }
}

static void handle_restart() {
  if (!update_authorized()) {
    send_json(401, "{"ok":false,"error":"wrong update password"}");
    return;
  }
  send_json(200, "{"ok":true,"rebooting":true}");
  delay(250);
  ESP.restart();
}

// WHY THE RADIO KEPT DISAPPEARING: the ESP32 default is modem sleep, which
// parks the receiver between DTIM beacons to save a few milliamps. On a
// mains-powered clock that saving buys nothing and costs everything — the
// board stops answering pings for seconds at a time, and a router with an
// aggressive idle timeout eventually drops the association entirely. So we
// turn sleep off, ask the stack to reassociate on its own, and keep a slow
// watchdog below in case the AP reboots underneath us.
void webcfg_begin() {
  if (started) return;
  started = true;

  WiFi.setSleep(false);          // never park the receiver
  WiFi.setAutoReconnect(true);   // the stack retries without our help
  WiFi.persistent(false);        // credentials are compiled in; avoid needless flash writes

  if (MDNS.begin("foursquare-revo")) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "device", "4square");
  }


  server.on("/api/status", HTTP_GET,  handle_status);
  server.on("/api/layout", HTTP_GET,  handle_layout);
  server.on("/api/layout", HTTP_POST, handle_layout);
  server.on("/api/button", HTTP_GET,  handle_button);
  server.on("/api/button", HTTP_POST, handle_button);
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

void webcfg_tick() {
  if (!started) return;
  server.handleClient();
}
#endif  // !DEMO_BUILD
