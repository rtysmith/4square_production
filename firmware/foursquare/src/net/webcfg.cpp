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
#include <esp_netif.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>


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

#define WEBCFG_API 17  // 17 = scan-locked AP selection + deterministic DHCP recovery

static WebServer  server(80);
static bool       started  = false;
static bool       updating = false;
static bool       update_failed = false;
static uint32_t   update_activity_ms = 0;
// Bytes actually handed to Update.write(), and whether Update.end() accepted
// the finished image. Both are reported back to the browser so "the upload
// finished" can never be mistaken for "the new firmware is installed".
static uint32_t   update_written = 0;
static bool       update_finalized = false;
static const uint32_t UPDATE_STALL_MS = 30000;
// Non-zero once a handler has promised the browser a reboot. The actual
// ESP.restart() happens in webcfg_tick() so the HTTP reply gets flushed and
// the socket closed first; restarting inside the handler made a good install
// look like a failed one.
static uint32_t   pending_restart_ms = 0;


// ---- Wi-Fi connection keeper ----------------------------------------------
// There is deliberately ONE owner of WiFi.begin()/disconnect(). The stock
// retry loop and the ESP auto-reconnector used to race one another, turning a
// one-beacon hiccup into repeated disconnects. This state machine waits out a
// transient, retries the last good AP first, uses exponential backoff, tries
// the second AP only after repeated failures, and resets the radio driver
// without rebooting the clock. A reboot would throw away live display state;
// this keeper can recover indefinitely while every panel keeps rendering.
static bool       wifi_seen_up       = false;
static bool       wifi_joining       = false;
static uint8_t    wifi_network       = 0;
static uint8_t    wifi_failures      = 0;
static uint32_t   wifi_down_since_ms = 0;
static uint32_t   wifi_join_since_ms = 0;
static uint32_t   wifi_dhcp_since_ms = 0;
static uint32_t   wifi_next_try_ms   = 0;
static uint32_t   wifi_retry_started_ms = 0;
static uint32_t   wifi_last_up_ms    = 0;
static uint32_t   wifi_stable_since_ms = 0;
static uint32_t   wifi_last_radio_ms = 0;
static uint32_t   wifi_recoveries    = 0;
static uint32_t   wifi_last_outage_s = 0;
static uint8_t    wifi_last_failure  = 0;

// A wobble is a wobble for four seconds, not twelve: the old grace period was
// long enough that a real drop cost a quarter of a minute before anything even
// started. The join and DHCP windows are what the ESP driver actually needs —
// a join that has not completed in 12s will not complete, and a lease that has
// not arrived in 8s is a router that wants to be asked again.
static const uint32_t WIFI_LOSS_GRACE_MS  = 4000u;
static const uint32_t WIFI_JOIN_LIMIT_MS  = 12000u;
// Some mesh/router combinations take more than one DHCP retransmission after
// association. Keep the good radio link for a full 30 seconds, renewing at
// 10-second intervals, before declaring the lease failed.
static const uint32_t WIFI_DHCP_LIMIT_MS  = 30000u;
static const uint32_t WIFI_RADIO_RESET_MS = 5u * 60u * 1000u;
// The keeper is a state machine, not a poll loop: four times a second is more
// than enough and leaves the main loop to the displays.
static const uint32_t WIFI_TICK_MS = 250u;

// The AP we were last actually online through. Re-joining with the known BSSID
// and channel skips the full scan, which is the single biggest cost in a
// reconnect — seconds become hundreds of milliseconds. Cleared after two
// failed attempts so a moved or rebooted AP is still found the slow way.
static uint8_t  wifi_last_bssid[6] = {0};
static int32_t  wifi_last_channel  = 0;
static bool     wifi_have_bssid    = false;
static uint8_t  wifi_last_network  = 0;
static uint8_t  wifi_dhcp_retries  = 0;
static uint32_t wifi_dhcp_kick_ms  = 0;

// A scan answers "is this network even here?" in about two seconds for BOTH
// SSIDs at once. Blind-joining a network that is not on the air costs the full
// join window per attempt, which is why being away from network 1 used to add
// a minute of dead time before network 2 was ever tried.
static const uint32_t WIFI_SCAN_FRESH_MS   = 120000u;
static const uint32_t WIFI_SCAN_LIMIT_MS   = 6000u;
static bool     wifi_scanning       = false;
static uint32_t wifi_scan_started_ms = 0;
static uint32_t wifi_scan_done_ms    = 0;
static bool     wifi_net_seen[2]     = {false, false};
static int32_t  wifi_net_rssi[2]     = {0, 0};
static int32_t  wifi_net_chan[2]     = {0, 0};
static uint8_t  wifi_net_bssid[2][6] = {{0}, {0}};

// Stop and restart the station's DHCP client so the next association starts a
// clean DISCOVER/OFFER exchange. This is the reliable way to clear a stale or
// half-finished lease; WiFi.config() sentinels behave differently per core.
static void wifi_force_dhcp() {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) {
    Serial.println("# wifi keeper: station interface unavailable for DHCP");
    return;
  }
  const esp_err_t stopped = esp_netif_dhcpc_stop(sta);
  esp_netif_ip_info_t empty = {};
  const esp_err_t cleared = esp_netif_set_ip_info(sta, &empty);
  const esp_err_t started_dhcp = esp_netif_dhcpc_start(sta);
  Serial.printf("# wifi keeper: DHCP reset stop=%d clear=%d start=%d\n",
                (int)stopped, (int)cleared, (int)started_dhcp);
}

static uint32_t wifi_retry_delay(uint8_t failures) {

  if (failures < 2) return 2000u;
  if (failures < 4) return 5000u;
  if (failures < 8) return 15000u;
  return 30000u;
}

static bool wifi_scan_fresh(uint32_t now) {
  return wifi_scan_done_ms && (uint32_t)(now - wifi_scan_done_ms) < WIFI_SCAN_FRESH_MS;
}

static void wifi_scan_begin(uint32_t now) {
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
  wifi_scanning = true;
  wifi_scan_started_ms = now == 0 ? 1 : now;
  Serial.println("# wifi keeper: scanning for saved networks");
}

// Returns true once the scan is finished (or gave up) and results are usable.
static bool wifi_scan_poll(uint32_t now) {
  const int n = WiFi.scanComplete();
  if (n >= 0) {
    const bool have_second = WIFI_SSID2[0] && strcmp(WIFI_SSID, WIFI_SSID2) != 0;
    wifi_net_seen[0] = wifi_net_seen[1] = false;
    for (int i = 0; i < n; i++) {
      const String s = WiFi.SSID(i);
      int slot = -1;
      if (s == WIFI_SSID) slot = 0;
      else if (have_second && s == WIFI_SSID2) slot = 1;
      if (slot < 0) continue;
      const int32_t rssi = WiFi.RSSI(i);
      if (wifi_net_seen[slot] && rssi <= wifi_net_rssi[slot]) continue;
      wifi_net_seen[slot] = true;
      wifi_net_rssi[slot] = rssi;
      wifi_net_chan[slot] = WiFi.channel(i);
      if (const uint8_t *b = WiFi.BSSID(i)) memcpy(wifi_net_bssid[slot], b, 6);
    }
    Serial.printf("# wifi keeper: scan done - net1 %s, net2 %s\n",
                  wifi_net_seen[0] ? "present" : "absent",
                  wifi_net_seen[1] ? "present" : "absent");
    WiFi.scanDelete();
    wifi_scanning = false;
    wifi_scan_done_ms = now == 0 ? 1 : now;
    return true;
  }
  if (n == WIFI_SCAN_FAILED || (uint32_t)(now - wifi_scan_started_ms) >= WIFI_SCAN_LIMIT_MS) {
    WiFi.scanDelete();
    wifi_scanning = false;
    wifi_scan_done_ms = 0;  // unknown: fall back to blind joining
    return true;
  }
  return false;
}

// Point wifi_network at whichever saved SSID the last scan actually saw; when
// both are on the air the stronger one wins.
static void wifi_pick_from_scan(uint32_t now) {
  if (!wifi_scan_fresh(now)) return;
  const bool have_second = WIFI_SSID2[0] && strcmp(WIFI_SSID, WIFI_SSID2) != 0;
  if (!have_second) { wifi_network = 0; return; }
  if (wifi_net_seen[0] && wifi_net_seen[1]) {
    wifi_network = wifi_net_rssi[1] > wifi_net_rssi[0] ? 1 : 0;
  } else if (wifi_net_seen[0]) {
    wifi_network = 0;
  } else if (wifi_net_seen[1]) {
    wifi_network = 1;
  }
}

bool webcfg_wifi_online() {
  return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0u;
}

uint8_t webcfg_wifi_stage() {
  const uint32_t now = millis();
  if (webcfg_wifi_online()) return 0;
  if (WiFi.status() == WL_CONNECTED) return 3;
  if (wifi_joining) return 2;
  if (wifi_scanning) return 6;
  if (wifi_seen_up && (uint32_t)(now - wifi_last_up_ms) < WIFI_LOSS_GRACE_MS) return 1;
  if ((int32_t)(wifi_next_try_ms - now) > 0) return 4;
  return 5;
}

uint8_t webcfg_wifi_progress() {
  const uint32_t now = millis();
  uint32_t elapsed = 0, span = 1;
  switch (webcfg_wifi_stage()) {
    case 0: return 100;
    case 1: elapsed = now - wifi_last_up_ms; span = WIFI_LOSS_GRACE_MS; break;
    case 2: elapsed = now - wifi_join_since_ms; span = WIFI_JOIN_LIMIT_MS; break;
    case 3: elapsed = now - wifi_dhcp_since_ms; span = WIFI_DHCP_LIMIT_MS; break;
    case 4:
      elapsed = now - wifi_retry_started_ms;
      span = wifi_next_try_ms - wifi_retry_started_ms;
      break;
    case 6: elapsed = now - wifi_scan_started_ms; span = WIFI_SCAN_LIMIT_MS; break;
    default: return 5;
  }
  if (elapsed >= span) return 100;
  return (uint8_t)((elapsed * 100u) / span);
}

uint8_t webcfg_wifi_attempt() { return (uint8_t)(wifi_failures + 1u); }
uint8_t webcfg_wifi_network() { return (uint8_t)(wifi_network + 1u); }
const char *webcfg_wifi_target_ssid() {
  const bool have_second = WIFI_SSID2[0] && strcmp(WIFI_SSID, WIFI_SSID2) != 0;
  return (wifi_network == 1 && have_second) ? WIFI_SSID2 : WIFI_SSID;
}
uint8_t webcfg_wifi_failure() { return wifi_last_failure; }

static void wifi_keeper_start_join(uint32_t now, bool reset_radio) {
  if (reset_radio) {
    Serial.println("# wifi keeper: recycling radio only; displays stay live");
    wifi_last_failure = 5;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    delay(250);
    wifi_last_radio_ms = now;
  } else {
    WiFi.disconnect(false, false);
    delay(40);
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.setTxPower(WIFI_POWER_17dBm);
  // Some routers refuse a DHCP request with an empty client hostname.
  WiFi.setHostname("foursquare-revo");
  // Clear a stale station address and restart the DHCP client for real. Calling
  // WiFi.config() with INADDR_NONE is core-version dependent and on some builds
  // leaves the station with a bogus static address, which is exactly the
  // "associated, 0.0.0.0 forever" symptom - so drive esp_netif directly.
  wifi_force_dhcp();


  const bool have_second = WIFI_SSID2[0] && strcmp(WIFI_SSID, WIFI_SSID2) != 0;
  const char *ssid = wifi_network == 1 && have_second ? WIFI_SSID2 : WIFI_SSID;
  const char *pass = wifi_network == 1 && have_second ? WIFI_PASS2 : WIFI_PASS;
  wifi_dhcp_retries = 0;
  if (reset_radio) wifi_have_bssid = false;
  const uint8_t slot = (wifi_network == 1 && have_second) ? 1 : 0;
  if (wifi_have_bssid && wifi_last_network == wifi_network && wifi_failures < 2) {
    Serial.printf("# wifi keeper: fast rejoin to network %u on channel %d\n",
                  (unsigned)(wifi_network + 1), (int)wifi_last_channel);
    WiFi.begin(ssid, pass, wifi_last_channel, wifi_last_bssid, true);
  } else if (wifi_scan_fresh(now) && wifi_net_seen[slot]) {
    // The scan just told us the exact radio and channel: join it directly and
    // skip the driver's own full-band scan.
    Serial.printf("# wifi keeper: joining network %u from scan (ch %d, %d dBm)\n",
                  (unsigned)(wifi_network + 1), (int)wifi_net_chan[slot],
                  (int)wifi_net_rssi[slot]);
    WiFi.begin(ssid, pass, wifi_net_chan[slot], wifi_net_bssid[slot], true);
  } else {
    wifi_have_bssid = false;
    Serial.printf("# wifi keeper: joining configured network %u\n", (unsigned)(wifi_network + 1));
    WiFi.begin(ssid, pass);
  }
  wifi_joining = true;
  wifi_join_since_ms = now;
  wifi_dhcp_since_ms = 0;
  wifi_dhcp_kick_ms = 0;
}

void webcfg_wifi_keeper_tick() {
  // This runs from the main loop even before a network exists, unlike
  // webcfg_begin(). Restore the last successful remote data at the earliest
  // possible moment so a cold boot during an outage never produces blanks.
  static bool restored_remote_data = false;
  if (!restored_remote_data) {
    restored_remote_data = true;
    extras_cache_restore();
  }
  const uint32_t now = millis();
  // Rate limit: the driver's state does not change faster than this and the
  // panels want the cycles more than the radio does.
  static uint32_t last_tick_ms = 0;
  if (last_tick_ms && (uint32_t)(now - last_tick_ms) < WIFI_TICK_MS) return;
  last_tick_ms = now == 0 ? 1 : now;
  const wl_status_t status = WiFi.status();


  // ASSOCIATED IS NOT ONLINE. The radio can sit at WL_CONNECTED with an
  // address of 0.0.0.0 when the router's DHCP handshake is lost - the clock
  // says "Wi-Fi connected" while nothing on the network can reach it. Treat a
  // missing lease as a failed join: wait a short while for DHCP, then rejoin
  // (which restarts the lease request) rather than sitting there forever.
  const bool have_lease = (uint32_t)WiFi.localIP() != 0u;

  if (status == WL_CONNECTED && have_lease) {
    const bool newly_up = !wifi_seen_up;
    const uint32_t outage_ms = wifi_down_since_ms ? (uint32_t)(now - wifi_down_since_ms) : 0;
    wifi_seen_up = true;
    wifi_joining = false;
    wifi_failures = 0;
    wifi_last_failure = 0;
    wifi_down_since_ms = 0;
    wifi_dhcp_since_ms = 0;
    wifi_last_up_ms = now;
    ui_env.wifi_up = true;
    ui_env.rssi = WiFi.RSSI();
    // The address is what every screen actually shows. The keeper owns the
    // join now, so the keeper owns this field too: refresh it on every online
    // tick (a renewed lease can change it) rather than trusting whatever the
    // original sketch's one-shot connect left behind.
    snprintf(ui_env.ip, sizeof ui_env.ip, "%s",
             WiFi.localIP().toString().c_str());

    // A lease that arrives without a name server (or one wiped by our own
    // DHCP restarts) leaves the clock online but unable to resolve anything,
    // which is exactly what an empty LinkedIn/weather panel looks like. Fill
    // in a public resolver whenever the router did not give us one.
    if ((uint32_t)WiFi.dnsIP() == 0u) {
      esp_netif_t *sta_dns = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if (sta_dns) {
        esp_netif_dns_info_t d1 = {};
        d1.ip.type = ESP_IPADDR_TYPE_V4;
        d1.ip.u_addr.ip4.addr = ipaddr_addr("1.1.1.1");
        esp_netif_set_dns_info(sta_dns, ESP_NETIF_DNS_MAIN, &d1);
        esp_netif_dns_info_t d2 = {};
        d2.ip.type = ESP_IPADDR_TYPE_V4;
        d2.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
        esp_netif_set_dns_info(sta_dns, ESP_NETIF_DNS_BACKUP, &d2);
        Serial.println("# wifi keeper: lease had no DNS; using 1.1.1.1");
      }
    }


    if (newly_up) {
      wifi_stable_since_ms = now == 0 ? 1 : now;
      if (outage_ms) {
        wifi_last_outage_s = outage_ms / 1000u;
        wifi_recoveries++;
      }
      Serial.print("# wifi keeper: online at ");
      Serial.println(WiFi.localIP());
      // Remember exactly which radio we are talking to so the next reconnect
      // can skip the scan entirely.
      if (const uint8_t *bssid = WiFi.BSSID()) {
        memcpy(wifi_last_bssid, bssid, 6);
        wifi_last_channel = WiFi.channel();
        wifi_last_network = wifi_network;
        wifi_have_bssid = true;
      }
      led_hold(LED_WIFI_JOINING, false);
      // mDNS can lose its netif when the radio is recycled. Rebind it on every
      // recovered association so foursquare-revo.local remains dependable.
      MDNS.end();
      if (MDNS.begin("foursquare-revo")) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "device", "4square");
      }
    }
    return;
  }

  if (status == WL_CONNECTED && !have_lease) {
    // Associated, waiting on the address. Give DHCP a fair window, then drop
    // the association so the next join asks for a lease from scratch.
    if (wifi_dhcp_since_ms == 0) {
      wifi_dhcp_since_ms = now == 0 ? 1 : now;
      wifi_dhcp_kick_ms = wifi_dhcp_since_ms;
      wifi_joining = false;
      // Association is complete. Restart DHCP now, against the live station
      // interface, rather than hoping a pre-association request survived.
      wifi_force_dhcp();
    }
    ui_env.wifi_up = false;
    ui_env.ota_ready = false;
    wifi_stable_since_ms = 0;
    if (wifi_down_since_ms == 0) wifi_down_since_ms = now == 0 ? 1 : now;
    const uint32_t dhcp_elapsed = (uint32_t)(now - wifi_dhcp_since_ms);
    // Keep the association intact while renewing at 10s and 20s. Calling
    // WiFi.reconnect() here used to tear down a valid Garland association and
    // reset the timer, so the DHCP timeout never truly elapsed.
    if (wifi_dhcp_retries < 2 &&
        (uint32_t)(now - wifi_dhcp_kick_ms) >= 10000u) {
      wifi_dhcp_retries++;
      wifi_dhcp_kick_ms = now;
      Serial.printf("# wifi keeper: still associated; renewing DHCP (%u/2)\n",
                    (unsigned)wifi_dhcp_retries);
      wifi_force_dhcp();
      return;
    }
    if (dhcp_elapsed < WIFI_DHCP_LIMIT_MS) return;
    wifi_last_failure = 4;
    Serial.println("# wifi keeper: associated but no DHCP lease; rejoining");
    wifi_dhcp_since_ms = 0;
    wifi_dhcp_kick_ms = 0;
    wifi_seen_up = false;
    wifi_joining = false;
    wifi_have_bssid = false;  // this AP hands out associations but no leases
    if (wifi_failures < 250) wifi_failures++;
    // A missing lease is not evidence that the other SSID is in range. Keep
    // retrying the AP the scan selected; only a new scan may switch networks.
    wifi_pick_from_scan(now);
    wifi_keeper_start_join(now, wifi_failures >= 4);
    return;
  }
  wifi_dhcp_since_ms = 0;
  wifi_dhcp_kick_ms = 0;
  wifi_dhcp_retries = 0;


  ui_env.wifi_up = false;
  ui_env.ota_ready = false;
  wifi_stable_since_ms = 0;
  if (wifi_down_since_ms == 0) wifi_down_since_ms = now == 0 ? 1 : now;

  // Do not tear down a healthy association for a momentary status wobble.
  if (wifi_seen_up && (uint32_t)(now - wifi_last_up_ms) < WIFI_LOSS_GRACE_MS) return;
  wifi_seen_up = false;

  if (wifi_joining) {
    // Do not sit out the whole join window when the driver has already given a
    // verdict: "no such network" and "rejected" are final answers.
    const bool dead = (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED);
    if (!dead && (uint32_t)(now - wifi_join_since_ms) < WIFI_JOIN_LIMIT_MS) return;
    wifi_joining = false;
    if (status == WL_NO_SSID_AVAIL) wifi_last_failure = 1;
    else if (status == WL_CONNECT_FAILED) wifi_last_failure = 2;
    else wifi_last_failure = 3;
    if (wifi_failures < 250) wifi_failures++;
    // A cached BSSID that did not answer is worse than no cache: forget it and
    // let the next attempt scan for the AP wherever it now is.
    if (wifi_failures >= 2) wifi_have_bssid = false;
    const bool have_second = WIFI_SSID2[0] && strcmp(WIFI_SSID, WIFI_SSID2) != 0;
    // If the SSID was not even on the air, switch immediately and re-scan -
    // there is nothing to wait for. Otherwise give each network two attempts.
    const bool absent = (wifi_last_failure == 1);
    if (absent) wifi_scan_done_ms = 0;  // force a fresh look at what is around
    if (have_second) {
      // Never bounce back onto a network the last scan proved is not on the
      // air: that is what turns "one join" into five wasted attempts.
      if (wifi_scan_fresh(now) && (wifi_net_seen[0] != wifi_net_seen[1])) {
        wifi_network = wifi_net_seen[1] ? 1 : 0;
      } else if (!absent && (wifi_failures % 2) == 0) {
        wifi_network ^= 1;
      }
    }

    wifi_retry_started_ms = now;
    wifi_next_try_ms = now + (absent ? 250u : wifi_retry_delay(wifi_failures));
    Serial.printf("# wifi keeper: join failed (%u); retry %u queued\n",
                  (unsigned)wifi_last_failure, (unsigned)wifi_failures);
  }

  if ((int32_t)(now - wifi_next_try_ms) < 0) return;

  // Before blind-joining, find out which of the saved networks is actually in
  // range. Two seconds of scanning replaces up to a minute of hopeful joins.
  if (wifi_scanning) {
    if (!wifi_scan_poll(now)) return;
    wifi_pick_from_scan(now);
  } else if (!wifi_have_bssid && !wifi_scan_fresh(now)) {
    wifi_scan_begin(now);
    return;
  } else {
    // Re-apply what the last scan saw so a stale target cannot be re-tried.
    wifi_pick_from_scan(now);
  }

  // A completed scan is authoritative. Never hand an SSID that the scan just
  // proved absent to WiFi.begin(), because the driver then performs another
  // full-band scan and burns the entire join timeout. If neither saved network
  // is visible, wait briefly and scan again; if Garland alone is visible,
  // wifi_pick_from_scan() above has already selected it.
  if (wifi_scan_fresh(now) &&
      !wifi_net_seen[wifi_network == 1 ? 1 : 0]) {
    wifi_last_failure = 1;
    wifi_retry_started_ms = now;
    wifi_next_try_ms = now + 2000u;
    wifi_scan_done_ms = 0;
    Serial.println("# wifi keeper: selected network absent; waiting to rescan");
    return;
  }
  led_hold(LED_WIFI_JOINING, true);
  const bool reset_radio = wifi_failures >= 4 &&
    (wifi_last_radio_ms == 0 || (uint32_t)(now - wifi_last_radio_ms) >= WIFI_RADIO_RESET_MS);
  wifi_keeper_start_join(now, reset_radio);
}

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
  char body[1216];
  snprintf(body, sizeof body,
    "{\"firmware\":\"4square\",\"build_id\":\"%s\",\"version\":\"%s\",\"api\":%d,\"ip\":\"%s\",\"ssid\":\"%s\","
    "\"rssi\":%d,\"wifi_recoveries\":%lu,\"last_outage_s\":%lu,"
    "\"wifi_stage\":%u,\"wifi_progress\":%u,\"wifi_attempt\":%u,\"wifi_network\":%u,\"wifi_failure\":%u,"
    "\"uptime_s\":%lu,\"temp_c10\":%d,\"humidity\":%u,"
    "\"extras\":1,\"wide\":%d,\"linkedin\":{\"valid\":%s,\"followers\":%ld,\"gained7d\":%ld},"
    "\"hour24\":%u,\"temp_f\":%u,\"page\":%u,"
    "\"buttons\":[%u,%u,%u,%u],"
    "\"leds\":{\"mode\":%u,\"bright\":%u},"
    "\"secbar\":{\"thick\":%u,\"ticks\":%u},\"slots\":["
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u},"
    "{\"widget\":%u,\"style\":%u,\"overlay\":%u}]}",
    FOURSQUARE_BUILD_ID, extras_fw_version(), WEBCFG_API,

    WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), (int)WiFi.RSSI(),
    (unsigned long)wifi_recoveries, (unsigned long)wifi_last_outage_s,
    (unsigned)webcfg_wifi_stage(), (unsigned)webcfg_wifi_progress(),
    (unsigned)webcfg_wifi_attempt(), (unsigned)webcfg_wifi_network(),
    (unsigned)webcfg_wifi_failure(),
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
    (unsigned)extras_secbar_thick(), (unsigned)extras_secbar_ticks(),
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
    // 4 is OV_LIWEEK (the weekly LinkedIn gain), 5 the seconds bar, 6 the
    // Wi-Fi bars and 7..9 sunrise/sunset; all live past the stock overlay
    // enum. The byte holds two of them: low nibble along the bottom edge of
    // the panel, high nibble along the top, so 0x51 is a seconds bar on top
    // with digital seconds underneath.
    cfg.slot_overlay[i] = arg_u8(names[2], cfg.slot_overlay[i], 0x99);
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
// How the seconds bar is drawn, for every panel that carries one:
//   thick=1..4   the height of the bar in pixels
//   ticks=0|1|2  no hash marks, marks at 15/30/45, or marks every ten seconds
static void handle_secbar() {
  const uint8_t thick = arg_u8("thick", extras_secbar_thick(), 4);
  const uint8_t ticks = arg_u8("ticks", extras_secbar_ticks(), 2);
  extras_set_secbar(thick == 0 ? 1 : thick, ticks);
  ui_show_layout();
  char body[96];
  snprintf(body, sizeof body, "{\"ok\":true,\"thick\":%u,\"ticks\":%u}",
           (unsigned)extras_secbar_thick(), (unsigned)extras_secbar_ticks());
  send_json(200, body);
}

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

// THE BUG THIS EXISTS TO KILL: a partial upload used to answer 200 and
// reboot. Update.end() failing was only printed to the serial port, and a
// transfer that never delivered a single byte left every flag untouched — so
// the browser was told "installed" while the clock came back on the OLD image.
// Now the only path to a reboot is: authorised, begun, bytes written, and
// Update.end() accepted the image. Everything else is a 500 with the reason.
static void update_stand_down() {
  Update.abort();
  updating = false;
  update_activity_ms = 0;
  disp_all_off(false);
  enableLoopWDT();
  WiFi.setTxPower(WIFI_POWER_17dBm);
}

static void handle_update_result() {
  const char *why = 0;
  if (update_failed || Update.hasError())      why = "the board rejected the image";
  else if (!updating)                          why = "no firmware data arrived";
  else if (update_written == 0)                why = "the upload was empty";
  else if (!update_finalized)                  why = "the image was cut short before the end";

  if (why) {
    update_stand_down();
    char body[192];
    snprintf(body, sizeof body,
             "{\"ok\":false,\"written\":%lu,\"error\":\"%s\"}",
             (unsigned long)update_written, why);
    send_json(500, body);
    return;
  }
  char body[128];
  snprintf(body, sizeof body,
           "{\"ok\":true,\"written\":%lu,\"rebooting\":true}",
           (unsigned long)update_written);
  send_json(200, body);
  // Do NOT restart inside the handler. ESP.restart() here kills the socket
  // before WebServer has flushed and closed it, so the browser sees a network
  // error on a successful install and the clock looks like it never rebooted.
  // Hand the reboot to webcfg_tick(), one full loop later.
  pending_restart_ms = millis() + 400;
}


static void handle_update_data() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!update_authorized()) return;              // the POST handler answers 401
    updating = true;
    update_failed = false;
    update_written = 0;
    update_finalized = false;
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
      update_stand_down();
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!updating || update_failed) return;
    update_activity_ms = millis();
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      update_failed = true;
      Update.abort();
      Update.printError(Serial);
    } else {
      update_written += up.currentSize;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!updating || update_failed) return;
    if (Update.end(true)) {
      update_finalized = true;
      Serial.printf("# http update ok: %u bytes\n", up.totalSize);
    } else {
      update_failed = true;
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    update_failed = true;
    update_stand_down();
  }
}

static void handle_restart() {
  if (!update_authorized()) {
    send_json(401, "{\"ok\":false,\"error\":\"wrong update password\"}");
    return;
  }
  send_json(200, "{\"ok\":true,\"rebooting\":true}");
  pending_restart_ms = millis() + 400;

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
  WiFi.setAutoReconnect(false);  // one reconnect owner: connection keeper only
  WiFi.persistent(false);        // credentials are compiled in; avoid needless flash writes

  server.on("/api/status", HTTP_GET,  handle_status);
  server.on("/api/layout", HTTP_GET,  handle_layout);
  server.on("/api/layout", HTTP_POST, handle_layout);
  server.on("/api/leds", HTTP_GET,  handle_leds);
  server.on("/api/leds", HTTP_POST, handle_leds);
  server.on("/api/buttons", HTTP_GET,  handle_buttons);
  server.on("/api/buttons", HTTP_POST, handle_buttons);
  server.on("/api/button", HTTP_GET,  handle_button);
  server.on("/api/button", HTTP_POST, handle_button);
  server.on("/api/secbar", HTTP_GET,  handle_secbar);
  server.on("/api/secbar", HTTP_POST, handle_secbar);
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

static bool wifi_ready_for_remote_read(uint32_t now) {
  // Association alone is not enough: during a marginal join the status can
  // briefly say CONNECTED before DHCP/DNS and the route are usable. Starting a
  // synchronous DNS+TLS transaction in that window was the remaining route to
  // a watchdog reset shortly after boot. Keep rendering cached data and wait
  // for one uninterrupted minute of connectivity first.
  return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0u &&
         wifi_stable_since_ms != 0 &&
         (uint32_t)(now - wifi_stable_since_ms) >= 10000u;
}

// The same test, but it also tells the panels why they are still empty.
static bool remote_read_ready(uint32_t now, uint8_t which) {
  if (WiFi.status() != WL_CONNECTED) { extras_feed_note(which, "NO WIFI"); return false; }
  if ((uint32_t)WiFi.localIP() == 0u) { extras_feed_note(which, "NO IP YET"); return false; }
  if (!wifi_ready_for_remote_read(now)) { extras_feed_note(which, "WIFI SETTLING"); return false; }
  return true;
}

/**
 * Name lookup for the app's host, done up front so a resolver problem reads as
 * "NO DNS" on the panel instead of a bare TLS failure sixty seconds later.
 * The result is also what tells us whether the failure is DNS or the network.
 */
static bool host_resolves(uint8_t which) {
  IPAddress addr;
  if (WiFi.hostByName("project--93f6b6d0-48fb-4dbe-87b6-455b65129623.lovable.app", addr) == 1 &&
      (uint32_t)addr != 0u) {
    return true;
  }
  extras_feed_note(which, "NO DNS");
  Serial.println("# feed: DNS lookup failed");
  return false;
}

/** A TLS handshake needs tens of kilobytes; say so rather than failing blind. */
static bool enough_heap(uint8_t which) {
  if (ESP.getFreeHeap() >= 45000u) return true;
  extras_feed_note(which, "LOW MEMORY");
  Serial.printf("# feed: only %u bytes free, skipping fetch\n", (unsigned)ESP.getFreeHeap());
  return false;
}


// HTTPClient's negative codes as something a person can read off a panel.
static void note_http_error(uint8_t which, int code) {
  char t[18];
  switch (code) {
    case -1:  snprintf(t, sizeof t, "CANT CONNECT"); break;
    case -2:  snprintf(t, sizeof t, "SEND FAILED"); break;
    case -3:  snprintf(t, sizeof t, "SEND FAILED"); break;
    case -4:  snprintf(t, sizeof t, "NO REPLY"); break;
    case -5:  snprintf(t, sizeof t, "LINK LOST"); break;
    case -11: snprintf(t, sizeof t, "TIMED OUT"); break;
    default:
      if (code > 0) snprintf(t, sizeof t, "HTTP %d", code);
      else          snprintf(t, sizeof t, "NET ERR %d", code);
  }
  extras_feed_note(which, t);
}

static long json_long(const String &body, const char *key, bool *found) {
  const int at = body.indexOf(key);
  *found = at >= 0;
  if (at < 0) return 0;
  return body.substring(at + (int)strlen(key)).toInt();
}

static void linkedin_tick() {
  if (updating) return;
  const uint32_t now = millis();
  if (!remote_read_ready(now, 0)) return;
  if ((int32_t)(now - li_next_ms) < 0) return;
  li_next_ms = now + 15u * 60u * 1000u;
  if (!host_resolves(0) || !enough_heap(0)) { li_next_ms = now + 30000u; return; }

  WiFiClientSecure tls;
  tls.setInsecure();
  // Keep the main loop available to the web server and Wi-Fi keeper. The app's
  // endpoint has its own four-second fallback to cached data, so six seconds is
  // enough without making a healthy clock appear offline for fifteen seconds.
  tls.setTimeout(12);
  tls.setHandshakeTimeout(12);
  HTTPClient http;
  if (!http.begin(tls, LINKEDIN_URL)) {
    extras_feed_note(0, "BAD URL");
    li_next_ms = now + 30000u;
    return;
  }
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  // DNS, TLS, headers, and body reads are all synchronous in HTTPClient. Keep
  // the loop watchdog out of this finite background transaction; otherwise a
  // slow DNS/TLS exchange can reset the whole clock seconds after startup.
  disableLoopWDT();
  const int code = http.GET();
  if (code == 200) {
    const String body = http.getString();
    bool a = false, b = false;
    const long followers = json_long(body, "\"followers\":", &a);
    const long gained    = json_long(body, "\"gained7d\":", &b);
    if (a) {
      extras_set_linkedin((int32_t)followers, (int32_t)(b ? gained : 0));
      extras_feed_ok(0);
    } else {
      // A 200 with no number in it: the app answered, LinkedIn had nothing.
      extras_feed_note(0, "NO COUNT YET");
      li_next_ms = now + 60u * 1000u;
    }
    Serial.printf("# linkedin: %ld followers (+%ld)\n", followers, gained);
  } else {
    Serial.printf("# linkedin fetch failed: %d\n", code);
    note_http_error(0, code);
    // Try again sooner than the full period, but not in a tight loop.
    li_next_ms = now + 60u * 1000u;
  }
  http.end();
  enableLoopWDT();
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
  const uint32_t now = millis();
  if (!remote_read_ready(now, 1)) return;
  if ((int32_t)(now - wx_next_ms) < 0) return;
  wx_next_ms = now + 20u * 60u * 1000u;
  if (!host_resolves(1) || !enough_heap(1)) { wx_next_ms = now + 30000u; return; }

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(12);
  tls.setHandshakeTimeout(12);
  HTTPClient http;
  if (!http.begin(tls, WEATHER_URL)) {
    extras_feed_note(1, "BAD URL");
    wx_next_ms = now + 30000u;
    return;
  }
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  disableLoopWDT();
  const int code = http.GET();
  if (code == 200) {
    const String body = http.getString();
    bool a = false, b = false, cc = false, dd = false;
    const long icon = json_long(body, "\"icon\":", &a);
    const long cur  = json_long(body, "\"cur_c10\":", &b);
    const long mx   = json_long(body, "\"max_c10\":", &cc);
    const long pop  = json_long(body, "\"pop\":", &dd);
    bool ee = false, ff = false, gg = false;
    const long mn   = json_long(body, "\"min_c10\":", &ee);
    // Minutes past local midnight, so the sunrise/sunset corners need no
    // date arithmetic on the clock at all.
    const long sr   = json_long(body, "\"sunrise_min\":", &ff);
    const long ss   = json_long(body, "\"sunset_min\":", &gg);
    if (a && b) {
      extras_set_weather((uint8_t)icon, (int16_t)cur,
                         (int16_t)(cc ? mx : cur), (int16_t)(ee ? mn : cur),
                         (uint8_t)(dd ? pop : 0));
      extras_feed_ok(1);
    } else {
      // The forecast service answered the app, but with nothing usable.
      extras_feed_note(1, "NO FORECAST");
      wx_next_ms = now + 60u * 1000u;
    }
    if (ff || gg) extras_set_sun((int16_t)(ff ? sr : -1), (int16_t)(gg ? ss : -1));
    Serial.printf("# weather: icon %ld cur %ld pop %ld\n", icon, cur, pop);
  } else {
    Serial.printf("# weather fetch failed: %d\n", code);
    note_http_error(1, code);
    wx_next_ms = now + 60u * 1000u;
  }
  http.end();
  enableLoopWDT();
}

void webcfg_tick() {
  if (!started) return;
  server.handleClient();

  // The deferred reboot promised by /api/update and /api/restart. One extra
  // handleClient() above has already flushed and closed the reply socket.
  if (pending_restart_ms != 0 && (int32_t)(millis() - pending_restart_ms) >= 0) {
    Serial.println("# rebooting into the freshly installed image");
    Serial.flush();
    pending_restart_ms = 0;
    ESP.restart();
  }


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
