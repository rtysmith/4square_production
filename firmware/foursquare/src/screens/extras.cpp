// extras.cpp — see extras.h.
#include "extras.h"
#include "display.h"
#include "../app/ui.h"
#include "../board/sensors.h"
#include "../net/webcfg.h"
#include "../settings/store.h"
#include "faces.h"   // W_COUNT, widget_name(), widget_allows()
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <Preferences.h>

// Stamped by the app on every build. A fork that has never been built by the
// Control Center still compiles; it just reports itself as a development image.
#if __has_include("../fw_version.h")
#include "../fw_version.h"
#endif
#ifndef FOURSQUARE_FW_VERSION
#define FOURSQUARE_FW_VERSION "v0.dev"
#endif
#ifndef FOURSQUARE_BUILT_BY
#define FOURSQUARE_BUILT_BY "SWYR.com"
#endif



// ===========================================================================
// small drawing helpers, all clipped to the anti-burn-in safe area
// ===========================================================================
// Anti burn-in: when auto offset is on, everything the derived screens draw
// walks a couple of pixels around a slow four-step cycle. Defined at the end
// of the file next to the setting; the helpers below only read it.
static int8_t x_dx();
static int8_t x_dy();

static void x_text(GFXcanvas1 &c, const char *s, uint8_t size, int16_t y,
                   int16_t x) {
  c.setTextSize(size);
  c.setTextColor(1);
  c.setCursor((int16_t)(x + x_dx()), (int16_t)(y + x_dy()));
  c.print(s);
}

static void x_center(GFXcanvas1 &c, const char *s, uint8_t size, int16_t y) {
  const int16_t w = (int16_t)(strlen(s) * 6 * size - size);
  int16_t x = (int16_t)(SAFE_X0 + (SAFE_W - w) / 2);
  if (x < SAFE_X0) x = SAFE_X0;
  x_text(c, s, size, y, x);
}

static void x_bar(GFXcanvas1 &c, int16_t y, int16_t h, uint8_t pct) {
  if (pct > 100) pct = 100;
  c.drawRect(SAFE_X0, y, SAFE_W, h, 1);
  const int16_t w = (int16_t)((int32_t)(SAFE_W - 4) * pct / 100);
  if (w > 0) c.fillRect((int16_t)(SAFE_X0 + 2), (int16_t)(y + 2), w,
                        (int16_t)(h - 4), 1);
}

// ---- the boot screen -------------------------------------------------------

// It holds until the clock is actually on the network, because that is the one
// thing you cannot tell by looking at a clock face. While it waits it shows the
// version, the recovery step it is on, why the last attempt failed, and how
// long it has been trying. Once the link is up it says READY for a beat and
// gets out of the way on its own.
//
// The ten minute cap is a safety valve, not a feature: after that the saved
// layout takes over anyway (the Wi-Fi panel keeps reporting) so a clock on a
// dead network still tells the time.
static const uint32_t SPLASH_MIN_MS   = 2500u;
// Once an IP arrives, show the "we are live" boot cards for a full ten seconds
// so the address and mDNS name can actually be read off the clock.
static const uint32_t SPLASH_READY_MS = 10000u;
static uint32_t splash_ready_at = 0;
static bool     splash_done = false;
static uint32_t splash_done_at = 0;
// The handover: for a few seconds after the boot screen releases, every panel
// is repainted, otherwise only the panels that normally tick get cleared and
// the other three keep showing boot text.
static const uint32_t SPLASH_SWEEP_MS = 3000u;

const char *extras_fw_version() { return FOURSQUARE_FW_VERSION; }

bool extras_splash_sweep() {
  if (!splash_done || splash_done_at == 0) return false;
  return (uint32_t)(millis() - splash_done_at) < SPLASH_SWEEP_MS;
}

bool extras_splash_active() {
  if (splash_done) return false;
  const uint32_t now = millis();
  // NO TIME CAP: the boot screen is the only place the recovery steps are
  // visible, so it stays up until the clock really has an address.
  // ASSOCIATED IS NOT ONLINE: hold the splash until there is an actual IP

  // address, not merely a link. webcfg_wifi_online() is the only flag that
  // means "other machines can reach this clock".
  if (webcfg_wifi_online()) {
    if (splash_ready_at == 0) splash_ready_at = now;
    if (now - splash_ready_at >= SPLASH_READY_MS && now >= SPLASH_MIN_MS) {
      splash_done = true;
      splash_done_at = now ? now : 1u;
      return false;
    }
  } else {
    splash_ready_at = 0;
  }
  return true;
}

// WHICH PANEL AM I? face_render() is not told, and changing its signature
// would mean patching every call site in a fork we do not own. During the
// splash every panel is repainted in one burst (clock_sec_mask() returns 0x0F),
// so the panels arrive back-to-back: a gap longer than a repaint means a new
// sweep has started and the counter goes back to the top-left. Worst case the
// four cards are rotated, never blank and never duplicated.
static uint8_t  splash_slot = 0;
static uint32_t splash_last_draw_ms = 0;

static uint8_t splash_next_slot() {
  const uint32_t now = millis();
  if (splash_last_draw_ms == 0 || (uint32_t)(now - splash_last_draw_ms) > 150u)
    splash_slot = 0;
  else
    splash_slot = (uint8_t)((splash_slot + 1u) & 3u);
  splash_last_draw_ms = now;
  return splash_slot;
}

static const char *splash_stage_label(uint8_t stage) {
  if (stage == 0) return "ONLINE";
  if (stage == 1) return "LINK CHK";
  if (stage == 2) return "JOINING";
  if (stage == 3) return "GET IP";
  if (stage == 4) return "RETRY";
  if (stage == 6) return "SCANNING";
  if (stage == 7) return "SETUP AP";
  return "RADIO RST";
}

static const char *splash_fail_label(uint8_t failure) {
  if (failure == 1) return "NOT FOUND";
  if (failure == 2) return "BAD PASSWORD";
  if (failure == 3) return "JOIN TIMEOUT";
  if (failure == 4) return "NO IP";
  if (failure == 5) return "RADIO STUCK";
  return 0;
}


// Long SSIDs do not fit a 128px panel at size 1; clip rather than wrap so the
// name still reads as itself.
static void splash_fit(char *out, size_t n, const char *s, size_t max_chars) {
  if (!s) s = "";
  size_t len = strlen(s);
  if (max_chars > n - 1) max_chars = n - 1;
  if (len <= max_chars) { snprintf(out, n, "%s", s); return; }
  memcpy(out, s, max_chars - 1);
  out[max_chars - 1] = '.';
  out[max_chars] = 0;
}

void extras_splash_draw(GFXcanvas1 &c) {
  char b[32];
  c.fillScreen(0);
  c.setFont(nullptr);
  c.setTextWrap(false);

  const uint32_t now  = millis();
  const uint32_t secs = now / 1000u;
  const uint8_t  slot = splash_next_slot();

  // ---- SETUP MODE: no network is saved (or three joins failed) -------------
  // The four panels stop reporting a join that is not happening and become
  // the instructions for handing the clock a network from a phone.
  if (webcfg_wifi_portal()) {
    switch (slot) {
      case 0:
        x_center(c, "WIFI", 2, (int16_t)(SAFE_Y0 + 6));
        x_center(c, "SETUP", 2, (int16_t)(SAFE_Y0 + 26));
        x_center(c, FOURSQUARE_FW_VERSION, 1, (int16_t)(SAFE_Y0 + 50));
        break;
      case 1: {
        char ss[24];
        splash_fit(ss, sizeof ss, webcfg_portal_ssid(), 18);
        x_center(c, "1 JOIN WIFI", 1, (int16_t)(SAFE_Y0 + 6));
        x_center(c, ss, 1, (int16_t)(SAFE_Y0 + 24));
        x_center(c, "NO PASSWORD", 1, (int16_t)(SAFE_Y0 + 42));
        break;
      }
      case 2: {
        char ip[24];
        splash_fit(ip, sizeof ip, webcfg_portal_ip(), 18);
        x_center(c, "2 OPEN", 1, (int16_t)(SAFE_Y0 + 6));
        x_center(c, ip, 1, (int16_t)(SAFE_Y0 + 24));
        x_center(c, "IN A BROWSER", 1, (int16_t)(SAFE_Y0 + 42));
        break;
      }
      default:
        x_center(c, "3 ENTER YOUR", 1, (int16_t)(SAFE_Y0 + 4));
        x_center(c, "NETWORK", 1, (int16_t)(SAFE_Y0 + 18));
        x_center(c, "IT RESTARTS", 1, (int16_t)(SAFE_Y0 + 34));
        x_center(c, "AND JOINS", 1, (int16_t)(SAFE_Y0 + 48));
        break;
    }
    return;
  }

  const uint8_t  stage = webcfg_wifi_stage();
  const bool     online = webcfg_wifi_online();
  const char    *failed = splash_fail_label(webcfg_wifi_failure());

  switch (slot) {
    // ---- top left: who this is ---------------------------------------------
    case 0:
      x_center(c, "4SQUARE", 2, (int16_t)(SAFE_Y0 + 4));
      x_center(c, FOURSQUARE_FW_VERSION, 1, (int16_t)(SAFE_Y0 + 22));
      // WHY IT IS AT THIS SCREEN AGAIN. A clock that restarted and a clock
      // that merely lost Wi-Fi look identical here, so the reset reason and
      // how long the previous run lasted are printed straight onto the glass.
      snprintf(b, sizeof b, "WOKE %s", webcfg_reset_text());
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 34));
      if (webcfg_boot_count() > 1) {
        const uint32_t prev = webcfg_prev_uptime_s();
        if (prev >= 3600u) snprintf(b, sizeof b, "RAN %luh BEFORE", (unsigned long)(prev / 3600u));
        else if (prev >= 60u) snprintf(b, sizeof b, "RAN %lum BEFORE", (unsigned long)(prev / 60u));
        else snprintf(b, sizeof b, "RAN %lus BEFORE", (unsigned long)prev);
        x_center(c, b, 1, (int16_t)(SAFE_Y0 + 46));
      } else {
        x_center(c, FOURSQUARE_BUILT_BY, 1, (int16_t)(SAFE_Y0 + 46));
      }
      break;

    // ---- top right: the network it is trying -------------------------------
    // "NET <ssid>" on ONE line so the whole name is visible at a glance, and
    // the address underneath as soon as the router hands one out.
    case 1: {
      char ss[24];
      splash_fit(ss, sizeof ss, webcfg_wifi_target_ssid(), 16);
      snprintf(b, sizeof b, "NET %s", ss);
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 2));
      snprintf(b, sizeof b, "AP %u  TRY %u", (unsigned)webcfg_wifi_network(),
               (unsigned)webcfg_wifi_attempt());
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 16));
      snprintf(b, sizeof b, "IP %s", online && ui_env.ip[0] ? ui_env.ip : "WAITING");
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 30));
      if (online) {
        snprintf(b, sizeof b, "SIGNAL %d dBm", (int)ui_env.rssi);
        x_center(c, b, 1, (int16_t)(SAFE_Y0 + 44));
      } else {
        x_center(c, splash_stage_label(stage), 1, (int16_t)(SAFE_Y0 + 44));
      }
      break;
    }


    // ---- bottom left: what it is doing right now ---------------------------
    case 2:
      snprintf(b, sizeof b, "STEP %s", splash_stage_label(online ? 0 : stage));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 4));
      x_bar(c, (int16_t)(SAFE_Y0 + 18), 8,
            online ? 100 : webcfg_wifi_progress());
      snprintf(b, sizeof b, "%u OF 5", (unsigned)(online ? 5 : (stage > 5 ? 1 : stage)));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 32));
      snprintf(b, sizeof b, "UP %lu:%02lu",
               (unsigned long)(secs / 60u), (unsigned long)(secs % 60u));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 46));
      break;

    // ---- bottom right: the result, good or bad -----------------------------
    default:
      if (online) {
        x_center(c, "READY", 1, (int16_t)(SAFE_Y0 + 4));
        x_center(c, ui_env.ip, 1, (int16_t)(SAFE_Y0 + 18));
        x_center(c, "foursquare-", 1, (int16_t)(SAFE_Y0 + 32));
        x_center(c, "revo.local", 1, (int16_t)(SAFE_Y0 + 42));
        x_bar(c, (int16_t)(SAFE_Y0 + 54), 7, 100);
      } else {
        snprintf(b, sizeof b, "LAST %s", failed ? failed : "OK");
        x_center(c, b, 1, (int16_t)(SAFE_Y0 + 4));
        x_center(c, stage == 3 ? "WAITING ON" : "WAITING FOR", 1,
                 (int16_t)(SAFE_Y0 + 20));
        x_center(c, stage == 3 ? "ROUTER IP" : "WIFI LINK", 1,
                 (int16_t)(SAFE_Y0 + 32));
        snprintf(b, sizeof b, "TRY %u AP %u", (unsigned)webcfg_wifi_attempt(),
                 (unsigned)webcfg_wifi_network());
        x_center(c, b, 1, (int16_t)(SAFE_Y0 + 48));
      }
      break;
  }
}





// A caption over a big value: the shape every derived screen uses, so they
// all read as one family rather than nineteen one-offs.
static void x_pair(GFXcanvas1 &c, const char *cap, const char *val,
                   uint8_t val_size) {
  x_center(c, cap, 1, (int16_t)(SAFE_Y0 + 2));
  x_center(c, val, val_size, (int16_t)(SAFE_Y0 + 16));
}

// The same shape for a panel that HAS no number: the caption, a plain NO DATA,
// and underneath it the fetcher's own reason, wrapped onto two short lines so
// nothing runs off the edge. A blank screen that explains itself.
static void x_why(GFXcanvas1 &c, const char *cap, uint8_t which) {
  x_center(c, cap, 1, (int16_t)(SAFE_Y0 + 2));
  x_center(c, "NO DATA", 2, (int16_t)(SAFE_Y0 + 14));
  const char *r = extras_feed_reason(which);
  if (r == nullptr || r[0] == 0) r = "WAITING";
  const uint8_t cols = (uint8_t)(SAFE_W / 6);
  if (strlen(r) <= cols) {
    x_center(c, r, 1, (int16_t)(SAFE_Y0 + 36));
    return;
  }
  // Break at the last space that still fits; otherwise cut hard.
  char a[24], b2[24];
  size_t cut = cols;
  for (size_t i = 0; i < strlen(r) && i <= cols; i++) if (r[i] == ' ') cut = i;
  snprintf(a, sizeof a, "%.*s", (int)cut, r);
  snprintf(b2, sizeof b2, "%s", r + cut + (r[cut] == ' ' ? 1 : 0));
  x_center(c, a, 1, (int16_t)(SAFE_Y0 + 32));
  x_center(c, b2, 1, (int16_t)(SAFE_Y0 + 41));
}

// "2H OLD" style footnote for a panel showing cached numbers that have stopped
// refreshing. Silent while the feed is healthy.
static void x_stale(GFXcanvas1 &c, uint8_t which, int32_t after_min) {
  const int32_t age = extras_feed_age_min(which);
  if (age < after_min) return;
  char t[16];
  if (age < 90) snprintf(t, sizeof t, "%ldM OLD", (long)age);
  else          snprintf(t, sizeof t, "%ldH OLD", (long)(age / 60));
  x_center(c, t, 1, (int16_t)(SAFE_Y0 + SAFE_H - 8));
}




// ===========================================================================
// the rolling history — one sample a minute, an hour deep, plus today's range
// ===========================================================================
static const uint8_t HIST_N = 60;
struct History {
  int16_t  temp[HIST_N];
  uint8_t  rh[HIST_N];
  uint8_t  filled;
  uint8_t  head;
  uint32_t last_ms;
  int16_t  hi_c10, lo_c10;
  uint8_t  hi_lo_hour;   // the hour the range was last reset on
  bool     any;
};
static History hist;

static int32_t li_followers = 0;
static int32_t li_gained    = 0;
static bool    li_valid     = false;
static bool    li_week_ok   = false;

static uint8_t wx_icon    = 0;
static int16_t wx_cur_c10 = 0;
static int16_t wx_max_c10 = 0;
static int16_t wx_min_c10 = 0;
static uint8_t wx_pop     = 0;
static bool    wx_valid   = false;
// Minutes past local midnight; -1 until the app has told us.
static int16_t sun_rise   = -1;
static int16_t sun_set    = -1;

// How the seconds bar looks. Kept for the whole clock rather than per panel:
// it is one visual decision, and a panel-sized setting would need a byte the
// EEPROM record does not have spare.
static uint8_t secbar_thick = 2;
static uint8_t secbar_ticks = 0;

static Preferences remote_cache;
static bool cache_open = false;

static void cache_begin() {
  if (!cache_open) cache_open = remote_cache.begin("remote-data", false);
}

void extras_set_secbar(uint8_t thick, uint8_t ticks) {
  secbar_thick = thick < 1 ? 1 : (thick > 4 ? 4 : thick);
  secbar_ticks = ticks > 2 ? 0 : ticks;
  cache_begin();
  if (cache_open) {
    remote_cache.putUChar("sb_thick", secbar_thick);
    remote_cache.putUChar("sb_ticks", secbar_ticks);
  }
}
uint8_t extras_secbar_thick() { return secbar_thick; }
uint8_t extras_secbar_ticks() { return secbar_ticks; }

void extras_set_sun(int16_t sunrise_min, int16_t sunset_min) {
  sun_rise = sunrise_min;
  sun_set  = sunset_min;
  cache_begin();
  if (cache_open) {
    remote_cache.putShort("sun_r", sun_rise);
    remote_cache.putShort("sun_s", sun_set);
  }
}

void extras_set_linkedin(int32_t followers, int32_t gained7d, bool week_known) {
  const bool changed = !li_valid || li_followers != followers ||
                       (week_known && (!li_week_ok || li_gained != gained7d));
  li_followers = followers;
  li_valid     = true;
  if (week_known) {
    li_gained  = gained7d;
    li_week_ok = true;
  }
  if (changed) {
    cache_begin();
    if (cache_open) {
      remote_cache.putLong("li_total", followers);
      remote_cache.putBool("li_ok", true);
      if (week_known) {
        remote_cache.putLong("li_week", gained7d);
        remote_cache.putBool("li_wk_ok", true);
      }
    }
  }
}
bool    extras_linkedin_valid()      { return li_valid; }
bool    extras_linkedin_week_valid() { return li_week_ok; }
int32_t extras_linkedin_followers()  { return li_followers; }
int32_t extras_linkedin_gained()     { return li_gained; }

void extras_cache_restore() {
  cache_begin();
  if (!cache_open) return;
  if (remote_cache.getBool("li_ok", false)) {
    li_followers = remote_cache.getLong("li_total", 0);
    li_gained = remote_cache.getLong("li_week", 0);
    li_valid = true;
    li_week_ok = remote_cache.getBool("li_wk_ok", false);
  }
  secbar_thick = remote_cache.getUChar("sb_thick", 2);
  if (secbar_thick < 1 || secbar_thick > 4) secbar_thick = 2;
  secbar_ticks = remote_cache.getUChar("sb_ticks", 0);
  if (secbar_ticks > 2) secbar_ticks = 0;
  sun_rise = remote_cache.getShort("sun_r", -1);
  sun_set  = remote_cache.getShort("sun_s", -1);
  if (remote_cache.getBool("wx_ok", false)) {
    wx_icon = remote_cache.getUChar("wx_icon", 0);
    wx_cur_c10 = remote_cache.getShort("wx_cur", 0);
    wx_max_c10 = remote_cache.getShort("wx_hi", 0);
    wx_min_c10 = remote_cache.getShort("wx_lo", 0);
    wx_pop = remote_cache.getUChar("wx_pop", 0);
    wx_valid = true;
  }
}

void extras_set_weather(uint8_t icon, int16_t cur_c10, int16_t max_c10,
                        int16_t min_c10, uint8_t pop) {
  const uint8_t safe_icon = icon > 7 ? 7 : icon;
  const uint8_t safe_pop = pop > 100 ? 100 : pop;
  const bool changed = !wx_valid || wx_icon != safe_icon || wx_cur_c10 != cur_c10 ||
    wx_max_c10 != max_c10 || wx_min_c10 != min_c10 || wx_pop != safe_pop;
  wx_icon    = safe_icon;
  wx_cur_c10 = cur_c10;
  wx_max_c10 = max_c10;
  wx_min_c10 = min_c10;
  wx_pop     = safe_pop;
  wx_valid   = true;
  if (changed) {
    cache_begin();
    if (cache_open) {
      remote_cache.putUChar("wx_icon", wx_icon);
      remote_cache.putShort("wx_cur", wx_cur_c10);
      remote_cache.putShort("wx_hi", wx_max_c10);
      remote_cache.putShort("wx_lo", wx_min_c10);
      remote_cache.putUChar("wx_pop", wx_pop);
      remote_cache.putBool("wx_ok", true);
    }
  }
}
bool extras_weather_valid() { return wx_valid; }

// ---- why a remote panel is empty -------------------------------------------
// Four tiny slots (LinkedIn, weather, markets, sports), written by the fetcher
// and read by the renderers. Kept in RAM only: a reason is about right now,
// and after a reboot the first read is seconds away anyway.
#define FEED_N 4
static char     feed_reason[FEED_N][18] = { "STARTING", "STARTING", "STARTING", "STARTING" };
static uint32_t feed_ok_ms[FEED_N]      = { 0, 0, 0, 0 };
static bool     feed_ever_ok[FEED_N]    = { false, false, false, false };

void extras_feed_note(uint8_t which, const char *reason) {
  if (which >= FEED_N || reason == nullptr) return;
  snprintf(feed_reason[which], sizeof feed_reason[which], "%s", reason);
}
void extras_feed_ok(uint8_t which) {
  if (which >= FEED_N) return;
  feed_reason[which][0] = 0;
  feed_ok_ms[which] = millis();
  feed_ever_ok[which] = true;
}
const char *extras_feed_reason(uint8_t which) {
  return which >= FEED_N ? "" : feed_reason[which];
}
int32_t extras_feed_age_min(uint8_t which) {
  if (which >= FEED_N || !feed_ever_ok[which]) return -1;
  return (int32_t)((millis() - feed_ok_ms[which]) / 60000u);
}

void extras_tick(uint32_t now_ms, int16_t temp_c10, uint8_t rh, bool sht_ok,
                 uint8_t hour) {
  if (!sht_ok) return;
  // Midnight resets the daily range. Comparing the hour rather than a date
  // keeps this free of any calendar arithmetic that could be wrong.
  if (!hist.any || (hour == 0 && hist.hi_lo_hour != 0)) {
    hist.hi_c10 = temp_c10;
    hist.lo_c10 = temp_c10;
    hist.any    = true;
  }
  hist.hi_lo_hour = hour;
  if (temp_c10 > hist.hi_c10) hist.hi_c10 = temp_c10;
  if (temp_c10 < hist.lo_c10) hist.lo_c10 = temp_c10;

  if (hist.last_ms != 0 && (uint32_t)(now_ms - hist.last_ms) < 60000u) return;
  hist.last_ms = now_ms;
  hist.temp[hist.head] = temp_c10;
  hist.rh[hist.head]   = rh;
  hist.head = (uint8_t)((hist.head + 1) % HIST_N);
  if (hist.filled < HIST_N) hist.filled++;
}

// The oldest sample we hold, which is an hour ago once the buffer has filled.
static bool hist_oldest(int16_t *temp, uint8_t *rh) {
  if (hist.filled < 2) return false;
  const uint8_t idx = (uint8_t)((hist.head + HIST_N - hist.filled) % HIST_N);
  *temp = hist.temp[idx];
  *rh   = hist.rh[idx];
  return true;
}

// ===========================================================================
// the maths behind the derived screens
// ===========================================================================
static float c_of(int16_t c10) { return (float)c10 / 10.0f; }

static float dew_point_c(float tc, float rh) {
  if (rh < 1.0f) rh = 1.0f;
  const float a = 17.62f, b = 243.12f;
  const float g = (a * tc) / (b + tc) + logf(rh / 100.0f);
  return (b * g) / (a - g);
}

// Rothfusz heat index, in Celsius in and out. Below 27 C it is not defined, so
// we hand back the dry-bulb temperature rather than a made-up number.
static float heat_index_c(float tc, float rh) {
  const float tf = tc * 9.0f / 5.0f + 32.0f;
  if (tf < 80.0f) return tc;
  float hi = -42.379f + 2.04901523f * tf + 10.14333127f * rh
           - 0.22475541f * tf * rh - 0.00683783f * tf * tf
           - 0.05481717f * rh * rh + 0.00122874f * tf * tf * rh
           + 0.00085282f * tf * rh * rh - 0.00000199f * tf * tf * rh * rh;
  return (hi - 32.0f) * 5.0f / 9.0f;
}

static float abs_humidity(float tc, float rh) {
  const float sat = 6.112f * expf((17.67f * tc) / (tc + 243.5f));
  return (sat * rh * 2.1674f) / (273.15f + tc);
}

static void fmt_temp(char *b, size_t n, float tc, bool as_f) {
  const float v = as_f ? (tc * 9.0f / 5.0f + 32.0f) : tc;
  snprintf(b, n, "%d\xF7", (int)lroundf(v));
}

static bool is_leap(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static const uint8_t MDAYS[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

static uint16_t day_of_year(const FaceData &d) {
  uint16_t n = d.day;
  for (uint8_t m = 1; m < d.month && m <= 12; m++) {
    n = (uint16_t)(n + MDAYS[m - 1]);
    if (m == 2 && is_leap(d.year)) n++;
  }
  return n;
}
static uint16_t year_days(const FaceData &d) {
  return (uint16_t)(is_leap(d.year) ? 366 : 365);
}

// ISO 8601 week. weekday is 0=Sunday in FaceData; ISO counts Monday as 1.
static uint8_t iso_week(const FaceData &d) {
  const int16_t doy = (int16_t)day_of_year(d);
  const int16_t dow = (int16_t)((d.weekday + 6) % 7) + 1;   // 1..7, Mon..Sun
  int16_t week = (int16_t)((doy - dow + 10) / 7);
  if (week < 1) week = 52;
  if (week > 52) {
    // Week 53 exists only when the year starts or ends on the right weekday.
    const int16_t last = (int16_t)(is_leap(d.year) ? 366 : 365);
    if (last - doy >= 4 - dow) week = 1;
  }
  return (uint8_t)week;
}

// Conway-style phase, 0..7. Good to a day, which is all a 48 px disc can show.
static uint8_t moon_phase(const FaceData &d) {
  int y = d.year, m = d.month;
  const int day = d.day;
  if (m < 3) { y -= 1; m += 12; }
  const double jd = 365.25 * (y + 4716) + 30.6001 * (m + 1) + day
                  - 1524.5 + (2 - (y / 100) + ((y / 100) / 4));
  double age = fmod(jd - 2451550.1, 29.530588853);
  if (age < 0) age += 29.530588853;
  const int idx = (int)((age / 29.530588853) * 8.0 + 0.5) % 8;
  return (uint8_t)idx;
}

static const char *season_of(const FaceData &d) {
  switch (d.month) {
    case 12: case 1:  case 2:  return "WINTER";
    case 3:  case 4:  case 5:  return "SPRING";
    case 6:  case 7:  case 8:  return "SUMMER";
    default:                   return "AUTUMN";
  }
}

// The market and sport readings live at the bottom of this file, next to the
// lists they belong to. The panel renderers only read them.
static bool        q_have(uint8_t i);
static int32_t     q_price_at(uint8_t i);
static int32_t     q_chg_at(uint8_t i);
static bool        s_have(uint8_t i);
static const char *s_home(uint8_t i);
static const char *s_away(uint8_t i);
static const char *s_state(uint8_t i);


// ===========================================================================
// the derived screens
// ===========================================================================
bool extras_is_widget(uint8_t w) { return w >= X_FIRST && w < (uint8_t)X_LAST; }

static const char *const X_NAMES[] = {
  "FEELS", "DEW PT", "ABS RH", "COMFORT", "T TREND", "HI/LO", "RH TREND",
  "DAY NO", "WEEK", "DAYS LEFT", "QUARTER", "MOON", "SEASON", "WIFI",
  "UPTIME", "LIGHT", "IP", "FOLLOWERS", "7 DAYS", "CLOCK", "DATE", "WEATHER",
  "ABOUT", "STOCKS", "SPORTS"

};


const char *extras_widget_name(uint8_t w) {
  return extras_is_widget(w) ? X_NAMES[w - X_FIRST] : "?";
}

static void draw_moon(GFXcanvas1 &c, uint8_t phase) {
  const int16_t cx = (int16_t)(SAFE_X0 + SAFE_W / 2);
  const int16_t cy = (int16_t)(SAFE_Y0 + SAFE_H / 2 + 3);
  const int16_t r  = 18;
  c.drawCircle(cx, cy, r, 1);
  if (phase == 0) return;                       // new moon: outline only
  if (phase == 4) { c.fillCircle(cx, cy, r, 1); return; }
  // Lit fraction as a terminator ellipse, row by row, which is cheap and
  // reads correctly at this size.
  const float f = (float)phase / 8.0f;
  const float k = cosf(2.0f * 3.14159265f * f);  // -1..1 terminator offset
  for (int16_t dy = -r; dy <= r; dy++) {
    const float hw = sqrtf((float)(r * r - dy * dy));
    const int16_t x0 = (int16_t)(cx - hw);
    const int16_t x1 = (int16_t)(cx + hw);
    const int16_t xt = (int16_t)(cx - hw * k);
    if (phase < 4) c.drawLine(xt, (int16_t)(cy + dy), x1, (int16_t)(cy + dy), 1);
    else           c.drawLine(x0, (int16_t)(cy + dy), xt, (int16_t)(cy + dy), 1);
  }
}

static void draw_wifi(GFXcanvas1 &c) {
  char b[20];
  if (!ui_env.wifi_up) {
    const uint8_t stage = webcfg_wifi_stage();
    const uint8_t pct = webcfg_wifi_progress();
    const uint8_t failure = webcfg_wifi_failure();
    const char *label = "RESETTING RADIO";
    if (stage == 1) label = "CHECKING SIGNAL";
    else if (stage == 2) label = "JOINING NETWORK";
    else if (stage == 3) label = "REQUESTING IP";
    else if (stage == 4) label = "WAITING TO RETRY";
    else if (stage == 6) label = "SCANNING NETWORKS";
    else if (stage == 7) label = "SETUP MODE";
    if (stage == 7) {
      char j[24];
      x_center(c, "WIFI SETUP", 1, (int16_t)(SAFE_Y0 + 1));
      snprintf(j, sizeof j, "JOIN %s", webcfg_portal_ssid());
      x_center(c, j, 1, (int16_t)(SAFE_Y0 + 15));
      x_center(c, "THEN OPEN", 1, (int16_t)(SAFE_Y0 + 29));
      x_center(c, webcfg_portal_ip(), 1, (int16_t)(SAFE_Y0 + 43));
      return;
    }
    const char *failed = "NO FAILURE YET";
    if (failure == 1) failed = "FAILED: NOT FOUND";
    else if (failure == 2) failed = "FAILED: PASSWORD";
    else if (failure == 3) failed = "FAILED: JOIN TIMEOUT";
    else if (failure == 4) failed = "FAILED: NO IP";
    else if (failure == 5) failed = "FAILED: RADIO STUCK";
    x_center(c, "WIFI DOWN", 1, (int16_t)(SAFE_Y0 + 1));
    x_center(c, label, 1, (int16_t)(SAFE_Y0 + 13));
    x_center(c, failed, 1, (int16_t)(SAFE_Y0 + 25));
    x_bar(c, (int16_t)(SAFE_Y0 + 37), 8, pct);
    snprintf(b, sizeof b, "NET %u TRY %u %u%%",
             (unsigned)webcfg_wifi_network(),
             (unsigned)webcfg_wifi_attempt(), (unsigned)pct);
    x_center(c, b, 1, (int16_t)(SAFE_Y0 + 49));
    return;
  }
  const int rssi = ui_env.rssi;
  int bars = 0;
  if (rssi > -55)      bars = 4;
  else if (rssi > -66) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  x_center(c, "WIFI", 1, (int16_t)(SAFE_Y0 + 2));
  const int16_t base = (int16_t)(SAFE_Y0 + 34);
  for (int i = 0; i < 4; i++) {
    const int16_t h = (int16_t)(6 + i * 6);
    const int16_t x = (int16_t)(SAFE_X0 + 26 + i * 16);
    if (i < bars) c.fillRect(x, (int16_t)(base - h), 11, h, 1);
    else          c.drawRect(x, (int16_t)(base - h), 11, h, 1);
  }
  snprintf(b, sizeof b, "%d dBm", rssi);
  x_center(c, b, 1, (int16_t)(base + 5));
}

// ---- the corner overlay, drawn by the derived screens themselves -----------
// face_render() short-circuits for widget ids 32+, so the firmware's own
// overlay pass never runs on them; that is why setting a corner on a derived
// panel used to do nothing at all. Seconds are read through a detector because
// not every FaceData in the wild carries them: if the field is absent the
// seconds corner simply stays blank instead of failing the build.
template <class T> static auto x_secs(const T &d, int) -> decltype((int)d.second) {
  return (int)d.second;
}
template <class T> static int x_secs(const T &, long) { return -1; }

// The weekly LinkedIn gain, small, along the RIGHT edge so it reads as a
// footnote to the follower count rather than competing with the sunrise time
// that lives on the left.
void extras_overlay_week_at(GFXcanvas1 &c, bool top) {
  char t[12];
  if (!li_valid || !li_week_ok) snprintf(t, sizeof t, "+--");
  else                         snprintf(t, sizeof t, "%+ld", (long)li_gained);
  const int16_t w = (int16_t)(strlen(t) * 6 - 1);
  x_text(c, t, 1, top ? SAFE_Y0 : (int16_t)(SAFE_Y0 + SAFE_H - 8),
         (int16_t)(SAFE_X0 + SAFE_W - w));
}
void extras_overlay_week(GFXcanvas1 &c) { extras_overlay_week_at(c, false); }

// Seconds without digits: a straight filled bar across the panel that grows
// left to right once a minute. No outline. The thickness and the hash marks
// are the clock-wide settings above, and it can ride the top edge instead of
// the bottom when the panel's overlay byte says so.
void extras_overlay_secbar_at(GFXcanvas1 &c, int seconds, bool top) {
  if (seconds < 0) return;
  const int16_t th = (int16_t)secbar_thick;
  const int16_t y = top ? (int16_t)(SAFE_Y0 + 1)
                        : (int16_t)(SAFE_Y0 + SAFE_H - th);
  const int16_t x = (int16_t)(SAFE_X0 + 2);
  const int16_t w = (int16_t)(SAFE_W - 4);
  if (w < 8) return;
  const int16_t fill = (int16_t)((long)w * (seconds % 60) / 59L);
  if (fill > 0) c.fillRect(x, y, fill, th, 1);

  // Hash marks sit just clear of the bar, on the side facing the panel, so
  // they read as a scale rather than as part of the fill.
  if (secbar_ticks) {
    const int step = secbar_ticks == 1 ? 15 : 10;
    const int16_t mark_y = top ? (int16_t)(y + th + 1) : (int16_t)(y - 3);
    for (int sec = step; sec < 60; sec += step) {
      const int16_t mx = (int16_t)(x + (int16_t)((long)w * sec / 59L));
      c.drawFastVLine(mx, mark_y, 2, 1);
    }
  }
}
void extras_overlay_secbar(GFXcanvas1 &c, int seconds) {
  extras_overlay_secbar_at(c, seconds, false);
}

// Signal strength as three bars. Empty outlines for the bars the signal does
// not reach; a small x when the radio is down altogether.
void extras_overlay_wifi_at(GFXcanvas1 &c, bool top) {
  const int16_t base = top ? (int16_t)(SAFE_Y0 + 9) : (int16_t)(SAFE_Y0 + SAFE_H - 2);
  const int16_t x0 = (int16_t)(SAFE_X0 + SAFE_W - 14);
  if (!ui_env.wifi_up) {
    c.drawLine(x0, (int16_t)(base - 8), (int16_t)(x0 + 8), base, 1);
    c.drawLine(x0, base, (int16_t)(x0 + 8), (int16_t)(base - 8), 1);
    return;
  }
  const int rssi = ui_env.rssi;
  int bars = 1;
  if (rssi > -60)      bars = 3;
  else if (rssi > -72) bars = 2;
  for (int i = 0; i < 3; i++) {
    const int16_t h = (int16_t)(3 + i * 3);
    const int16_t x = (int16_t)(x0 + i * 5);
    if (i < bars) c.fillRect(x, (int16_t)(base - h), 3, h, 1);
    else          c.drawRect(x, (int16_t)(base - h), 3, h, 1);
  }
}
void extras_overlay_wifi(GFXcanvas1 &c) { extras_overlay_wifi_at(c, false); }

// Sunrise on the left, sunset on the right, both as a small arrow and a
// wall-clock time. The numbers come from the app with the forecast, so they
// follow the same ZIP as the weather panel and survive a reboot in flash.
// A little sun sitting on the horizon with an arrow through it: disc, ground
// line, and three rays pointing the way it is going. At this size a bare
// triangle read as a play button, which is why the disc is here.
static void sun_glyph(GFXcanvas1 &c, int16_t x, int16_t y, bool up) {
  const int16_t cx = (int16_t)(x + 3);
  const int16_t cy = (int16_t)(y + 3);
  c.fillCircle(cx, cy, 2, 1);
  // The horizon under (rising) or over (setting) the disc.
  c.drawFastHLine(x, up ? (int16_t)(y + 6) : (int16_t)(y - 1), 7, 1);
  // Arrow: a stalk away from the horizon with a head on the end.
  const int16_t tip = up ? (int16_t)(y - 1) : (int16_t)(y + 6);
  const int16_t step = up ? 1 : -1;
  for (int16_t i = 0; i < 2; i++) {
    c.drawFastHLine((int16_t)(cx - i), (int16_t)(tip + i * step),
                    (int16_t)(1 + i * 2), 1);
  }
}

void extras_overlay_sun(GFXcanvas1 &c, uint8_t which, bool top) {
  const int16_t y = top ? SAFE_Y0 : (int16_t)(SAFE_Y0 + SAFE_H - 7);
  char t[10];
  // Sunrise is always the earlier of the two and always sits on the left. If a
  // cached pair ever arrives the wrong way round, sort it here rather than
  // trusting whatever wrote the flash.
  int16_t rise = sun_rise, set = sun_set;
  if (rise >= 0 && set >= 0 && rise > set) { const int16_t s = rise; rise = set; set = s; }
  if (which == 7 || which == 9) {
    if (rise >= 0) {
      // 24-hour on both ends so 6:47 and 20:23 can never be read as the
      // same time of day.
      snprintf(t, sizeof t, "%d:%02d", (int)(rise / 60), (int)(rise % 60));
      sun_glyph(c, SAFE_X0, (int16_t)(y + 1), true);
      x_text(c, t, 1, y, (int16_t)(SAFE_X0 + 9));
    }
  }
  if (which == 8 || which == 9) {
    if (set >= 0) {
      snprintf(t, sizeof t, "%d:%02d", (int)(set / 60), (int)(set % 60));
      const int16_t w = (int16_t)(strlen(t) * 6 - 1);
      const int16_t x = (int16_t)(SAFE_X0 + SAFE_W - w);
      if (which == 9 && x < SAFE_X0 + 60) return;  // no room for both, keep sunrise
      sun_glyph(c, (int16_t)(x - 9), (int16_t)(y + 1), false);
      x_text(c, t, 1, y, x);
    }
  }
}


static void x_overlay_one(GFXcanvas1 &c, uint8_t ov, bool top, const FaceData &d) {
  if (ov == 0) return;                      // OV_NONE
  if (ov == 4) { extras_overlay_week_at(c, top); return; }        // LinkedIn week
  if (ov == 5) { extras_overlay_secbar_at(c, x_secs(d, 0), top); return; }
  if (ov == 6) { extras_overlay_wifi_at(c, top); return; }        // Wi-Fi bars
  if (ov >= 7 && ov <= 9) { extras_overlay_sun(c, ov, top); return; }
  char t[12];
  t[0] = 0;
  if (ov == 1) {                            // OV_SECONDS
    const int s = x_secs(d, 0);
    if (s < 0) return;
    snprintf(t, sizeof t, ":%02d", s % 60);
  } else if (ov == 2) {                     // OV_AMPM
    snprintf(t, sizeof t, "%s", d.hour < 12 ? "AM" : "PM");
  } else if (ov == 3) {                     // OV_TEMP
    if (d.temp_c10 <= -600) return;
    const float tc = c_of(d.temp_c10);
    snprintf(t, sizeof t, "%d%c", (int)lroundf(d.temp_f ? tc * 9.0f / 5.0f + 32.0f : tc),
             d.temp_f ? 'F' : 'C');
  } else {
    return;
  }
  const int16_t w = (int16_t)(strlen(t) * 6 - 1);
  // Lifted a few pixels so AM/PM and the seconds bar don't feel glued together.
  x_text(c, t, 1, top ? SAFE_Y0 : (int16_t)(SAFE_Y0 + SAFE_H - 12),
         (int16_t)(SAFE_X0 + SAFE_W - w));
}

// The saved byte holds two items: the low nibble rides the bottom edge, the
// high nibble the top. Either may be zero, so one byte covers "nothing", "just
// a bottom item", "just a top item" and "one of each".
static void x_overlay(GFXcanvas1 &c, uint8_t ovraw, const FaceData &d) {
  x_overlay_one(c, (uint8_t)(ovraw & 0x0F), false, d);
  x_overlay_one(c, (uint8_t)((ovraw >> 4) & 0x0F), true, d);
}

// The whole overlay switch, callable from faces.cpp for an ordinary widget.
void extras_overlay_full(GFXcanvas1 &c, uint8_t ov, const FaceData &d) {
  x_overlay(c, ov, d);
}

// ---- the weather icon ------------------------------------------------------
// Eight little scenes drawn inside a 32x30 box.
//
// WHY THE CLOUDS ARE SOLID: an outline cloud on a 128x64 mono OLED is three
// overlapping circle arcs, and at this size the leftover internal arcs read as
// noise. A filled silhouette is unmistakable at a glance from across a desk,
// which is the whole job. Where something sits BEHIND the cloud (the sun in
// "partly"), the cloud is punched out of it first with a one-pixel gap, so the
// two shapes stay separate instead of merging into a blob.
static void wx_cloud_shape(GFXcanvas1 &c, int16_t x, int16_t y, int16_t grow, uint16_t col) {
  c.fillCircle((int16_t)(x + 8), (int16_t)(y + 8), (int16_t)(6 + grow), col);
  c.fillCircle((int16_t)(x + 17), (int16_t)(y + 6), (int16_t)(8 + grow), col);
  c.fillCircle((int16_t)(x + 24), (int16_t)(y + 9), (int16_t)(5 + grow), col);
  c.fillRect((int16_t)(x - grow), (int16_t)(y + 8 - grow),
             (int16_t)(26 + 2 * grow), (int16_t)(7 + 2 * grow), col);
}

static void wx_cloud(GFXcanvas1 &c, int16_t x, int16_t y) {
  wx_cloud_shape(c, x, y, 0, 1);
}

static void wx_sun(GFXcanvas1 &c, int16_t cx, int16_t cy, int16_t r, bool rays) {
  c.fillCircle(cx, cy, r, 1);
  if (!rays) return;
  for (uint8_t i = 0; i < 8; i++) {
    const float a = (float)i * 3.14159265f / 4.0f;
    const int16_t x0 = (int16_t)(cx + cosf(a) * (r + 2));
    const int16_t y0 = (int16_t)(cy + sinf(a) * (r + 2));
    const int16_t x1 = (int16_t)(cx + cosf(a) * (r + 5));
    const int16_t y1 = (int16_t)(cy + sinf(a) * (r + 5));
    c.drawLine(x0, y0, x1, y1, 1);
  }
}

/** A slanted raindrop streak. */
static void wx_drop(GFXcanvas1 &c, int16_t x, int16_t y, int16_t len) {
  c.drawLine(x, y, (int16_t)(x - 2), (int16_t)(y + len), 1);
}

/** A six-point flake: three crossing strokes. */
static void wx_flake(GFXcanvas1 &c, int16_t cx, int16_t cy, int16_t r) {
  c.drawFastHLine((int16_t)(cx - r), cy, (int16_t)(2 * r + 1), 1);
  c.drawLine((int16_t)(cx - r), (int16_t)(cy - r), (int16_t)(cx + r), (int16_t)(cy + r), 1);
  c.drawLine((int16_t)(cx - r), (int16_t)(cy + r), (int16_t)(cx + r), (int16_t)(cy - r), 1);
}

static void wx_icon_draw(GFXcanvas1 &c, uint8_t icon, int16_t x, int16_t y) {
  switch (icon) {
    case 0:                                    // clear
      wx_sun(c, (int16_t)(x + 15), (int16_t)(y + 15), 8, true);
      break;
    case 1:                                    // partly cloudy
      wx_sun(c, (int16_t)(x + 21), (int16_t)(y + 6), 6, true);
      wx_cloud_shape(c, (int16_t)(x + 1), (int16_t)(y + 11), 2, 0);   // gap
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 11));
      break;
    case 2:                                    // cloudy
      wx_sun(c, (int16_t)(x + 22), (int16_t)(y + 5), 4, false);
      wx_cloud_shape(c, (int16_t)(x + 1), (int16_t)(y + 8), 2, 0);
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 8));
      break;
    case 3:                                    // fog
      wx_cloud(c, (int16_t)(x + 2), (int16_t)(y + 1));
      for (uint8_t i = 0; i < 3; i++)
        c.drawFastHLine((int16_t)(x + 1 + (i & 1) * 5), (int16_t)(y + 19 + i * 4), 22, 1);
      break;
    case 4:                                    // drizzle
      wx_cloud(c, (int16_t)(x + 2), (int16_t)(y + 1));
      for (uint8_t i = 0; i < 3; i++)
        wx_drop(c, (int16_t)(x + 8 + i * 7), (int16_t)(y + 19), 4);
      break;
    case 5:                                    // rain
      wx_cloud(c, (int16_t)(x + 2), (int16_t)(y + 1));
      for (uint8_t i = 0; i < 4; i++)
        wx_drop(c, (int16_t)(x + 5 + i * 6), (int16_t)(y + 19), 8);
      break;
    case 6:                                    // snow
      wx_cloud(c, (int16_t)(x + 2), (int16_t)(y + 1));
      for (uint8_t i = 0; i < 3; i++)
        wx_flake(c, (int16_t)(x + 7 + i * 8), (int16_t)(y + 24), 3);
      break;
    default: {                                 // storm
      wx_cloud(c, (int16_t)(x + 2), (int16_t)(y + 1));
      // A solid bolt: two triangles, so it survives the pixel shifter.
      c.fillTriangle((int16_t)(x + 17), (int16_t)(y + 18), (int16_t)(x + 9), (int16_t)(y + 26),
                     (int16_t)(x + 16), (int16_t)(y + 26), 1);
      c.fillTriangle((int16_t)(x + 18), (int16_t)(y + 24), (int16_t)(x + 11), (int16_t)(y + 31),
                     (int16_t)(x + 15), (int16_t)(y + 24), 1);
      break;
    }
  }
}


void extras_face_render(GFXcanvas1 &c, uint8_t w, uint8_t ov, const FaceData &d) {
  c.fillScreen(0);
  c.setFont(nullptr);
  c.setTextWrap(false);
  char b[24];

  const bool have_env = d.humidity <= 100 && d.temp_c10 > -600;
  const float tc = c_of(d.temp_c10);
  const float rh = (float)d.humidity;

  switch (w) {
    case X_FEELS: {
      if (!have_env) { x_pair(c, "FEELS", "--", 4); break; }
      fmt_temp(b, sizeof b, heat_index_c(tc, rh), d.temp_f);
      x_pair(c, "FEELS LIKE", b, 4);
      break;
    }
    case X_DEW: {
      if (!have_env) { x_pair(c, "DEW POINT", "--", 4); break; }
      fmt_temp(b, sizeof b, dew_point_c(tc, rh), d.temp_f);
      x_pair(c, "DEW POINT", b, 4);
      break;
    }
    case X_ABSHUM: {
      if (!have_env) { x_pair(c, "ABS HUMIDITY", "--", 3); break; }
      snprintf(b, sizeof b, "%.1f", (double)abs_humidity(tc, rh));
      x_pair(c, "ABS HUMIDITY", b, 4);
      x_center(c, "g/m3", 1, (int16_t)(SAFE_Y0 + 44));
      break;
    }
    case X_COMFORT: {
      if (!have_env) { x_pair(c, "COMFORT", "--", 3); break; }
      const float dp = dew_point_c(tc, rh);
      const char *verdict = "OK";
      if (d.humidity < 30)      verdict = "DRY";
      else if (dp >= 18.0f)     verdict = "MUGGY";
      else if (d.humidity > 65) verdict = "DAMP";
      x_pair(c, "COMFORT", verdict, 3);
      snprintf(b, sizeof b, "%u%%RH  DP %d", (unsigned)d.humidity,
               (int)lroundf(d.temp_f ? dp * 9.0f / 5.0f + 32.0f : dp));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 44));
      break;
    }
    case X_TTREND: {
      int16_t t0; uint8_t r0;
      if (!hist_oldest(&t0, &r0)) { x_pair(c, "1 HR TREND", "WAIT", 2); break; }
      float delta = c_of((int16_t)(d.temp_c10 - t0));
      if (d.temp_f) delta = delta * 9.0f / 5.0f;
      snprintf(b, sizeof b, "%c%.1f", delta >= 0 ? '+' : '-',
               (double)fabsf(delta));
      x_pair(c, "LAST HOUR", b, 4);
      // The arrow is the reading; the number is the detail.
      const int16_t ax = (int16_t)(SAFE_X0 + SAFE_W / 2);
      const int16_t ay = (int16_t)(SAFE_Y0 + 46);
      if (delta > 0.05f)       c.fillTriangle((int16_t)(ax - 6), (int16_t)(ay + 5), (int16_t)(ax + 6), (int16_t)(ay + 5), ax, (int16_t)(ay - 4), 1);
      else if (delta < -0.05f) c.fillTriangle((int16_t)(ax - 6), (int16_t)(ay - 4), (int16_t)(ax + 6), (int16_t)(ay - 4), ax, (int16_t)(ay + 5), 1);
      else                     c.fillRect((int16_t)(ax - 7), (int16_t)(ay), 14, 2, 1);
      break;
    }
    case X_RHTREND: {
      int16_t t0; uint8_t r0;
      if (!hist_oldest(&t0, &r0)) { x_pair(c, "RH TREND", "WAIT", 2); break; }
      const int delta = (int)d.humidity - (int)r0;
      snprintf(b, sizeof b, "%+d%%", delta);
      x_pair(c, "RH, LAST HOUR", b, 4);
      break;
    }
    case X_HILO: {
      if (!hist.any) { x_pair(c, "TODAY", "--", 3); break; }
      x_center(c, "TODAY", 1, (int16_t)(SAFE_Y0 + 2));
      float hi = c_of(hist.hi_c10), lo = c_of(hist.lo_c10);
      if (d.temp_f) { hi = hi * 9.0f / 5.0f + 32.0f; lo = lo * 9.0f / 5.0f + 32.0f; }
      snprintf(b, sizeof b, "%d\xF7", (int)lroundf(hi));
      x_center(c, b, 3, (int16_t)(SAFE_Y0 + 14));
      snprintf(b, sizeof b, "LOW %d\xF7", (int)lroundf(lo));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 42));
      break;
    }
    case X_DOY: {
      snprintf(b, sizeof b, "%u", (unsigned)day_of_year(d));
      x_pair(c, "DAY OF YEAR", b, 5);
      snprintf(b, sizeof b, "of %u", (unsigned)year_days(d));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 44));
      break;
    }
    case X_WEEKNO: {
      snprintf(b, sizeof b, "W%02u", (unsigned)iso_week(d));
      x_pair(c, "WEEK", b, 5);
      break;
    }
    case X_DAYSLEFT: {
      const int left = (int)year_days(d) - (int)day_of_year(d);
      snprintf(b, sizeof b, "%d", left);
      x_pair(c, "DAYS LEFT", b, 5);
      break;
    }
    case X_QUARTER: {
      const uint8_t q = (uint8_t)((d.month - 1) / 3 + 1);
      const uint16_t doy = day_of_year(d);
      const uint16_t start = (uint16_t)((q - 1) * 91);
      uint8_t pct = (uint8_t)(((int32_t)doy - start) * 100 / 91);
      if (pct > 100) pct = 100;
      snprintf(b, sizeof b, "Q%u", (unsigned)q);
      x_pair(c, "QUARTER", b, 5);
      x_bar(c, (int16_t)(SAFE_Y0 + 44), 8, pct);
      break;
    }
    case X_MOON: {
      static const char *const PH[8] = { "NEW", "WAX CRES", "1ST QTR",
        "WAX GIB", "FULL", "WAN GIB", "3RD QTR", "WAN CRES" };
      const uint8_t p = moon_phase(d);
      x_center(c, PH[p], 1, (int16_t)(SAFE_Y0));
      draw_moon(c, p);
      break;
    }
    case X_SEASON:
      x_pair(c, "SEASON", season_of(d), 2);
      break;
    case X_WIFI:
      draw_wifi(c);
      break;
    case X_UPTIME: {
      const uint32_t s = millis() / 1000u;
      if (s >= 86400u) snprintf(b, sizeof b, "%ud %uh", (unsigned)(s / 86400u), (unsigned)((s % 86400u) / 3600u));
      else if (s >= 3600u) snprintf(b, sizeof b, "%uh %um", (unsigned)(s / 3600u), (unsigned)((s % 3600u) / 60u));
      else snprintf(b, sizeof b, "%um", (unsigned)(s / 60u));
      x_pair(c, "UP", b, 3);
      break;
    }
    case X_LIGHT: {
      uint32_t pct = (uint32_t)light_raw() * 100u / 1023u;
      if (pct > 100) pct = 100;
      snprintf(b, sizeof b, "%u%%", (unsigned)pct);
      x_pair(c, "ROOM LIGHT", b, 4);
      x_bar(c, (int16_t)(SAFE_Y0 + 44), 8, (uint8_t)pct);
      break;
    }
    case X_IPADDR: {
      x_center(c, "ADDRESS", 1, (int16_t)(SAFE_Y0 + 4));
      x_center(c, ui_env.wifi_up ? ui_env.ip : "offline", 1,
               (int16_t)(SAFE_Y0 + 22));
      x_center(c, ui_env.wifi_up ? ui_env.ssid : "", 1, (int16_t)(SAFE_Y0 + 36));
      break;
    }
    case X_LIFOLLOWERS: {
      // The panel is called LINKEDIN, whatever it is showing.
      if (!li_valid) { x_why(c, "LINKEDIN", 0); break; }
      if (li_followers >= 10000) snprintf(b, sizeof b, "%ld.%ldk", (long)(li_followers / 1000), (long)((li_followers % 1000) / 100));
      else snprintf(b, sizeof b, "%ld", (long)li_followers);
      // With a corner in play the pair is centred in the space ABOVE the
      // overlay row rather than jammed against the top edge: pinning it high
      // left the number floating with a hole under it, which is the thing that
      // looked broken.
      if ((ov & 0x0F) != 0) {
        x_center(c, "LINKEDIN", 1, (int16_t)(SAFE_Y0 + 3));
        x_center(c, b, 3, (int16_t)(SAFE_Y0 + 17));
      } else {
        x_pair(c, "LINKEDIN", b, 4);
        // Numbers that have stopped refreshing say so rather than quietly
        // pretending to be current.
        x_stale(c, 0, 45);
      }
      break;
    }
    case X_LIWEEK: {
      if (!li_valid) { x_why(c, "7 DAYS", 0); break; }
      if (!li_week_ok) {
        // A follower total with no weekly figure behind it. Say so, rather
        // than printing a zero that looks like a week of no growth.
        x_center(c, "NEW, 7 DAYS", 1, (int16_t)(SAFE_Y0 + 2));
        x_center(c, "--", 3, (int16_t)(SAFE_Y0 + 16));
        x_center(c, "NO WEEK YET", 1, (int16_t)(SAFE_Y0 + 42));
        break;
      }
      snprintf(b, sizeof b, "%+ld", (long)li_gained);
      x_pair(c, "NEW, 7 DAYS", b, 4);
      x_stale(c, 0, 45);
      break;
    }
    // ---- the two "whole thing in one panel" screens ------------------------
    // The firmware's own HOUR and MINUTE each own a panel. These put the
    // reading a person actually asks for into ONE panel, so a single clock
    // panel and a single date panel can sit next to two other things.
    case X_CLOCKHM: {
      uint8_t hh = d.hour;
      if (!d.hour24) { hh = (uint8_t)(d.hour % 12); if (hh == 0) hh = 12; }
      snprintf(b, sizeof b, "%u:%02u", (unsigned)hh, (unsigned)d.minute);
      x_center(c, b, 3, (int16_t)(SAFE_Y0 + 12));
      // The AM/PM corner already says it: don't print a second one right
      // beside it. Otherwise it sits under the time. Keep the full-panel
      // label on the same baseline even when the seconds bar is selected —
      // lifting it made AM/PM crowd the clock digits instead of reading as the
      // lower line of the clock.
      if (!d.hour24 && (ov & 0x0F) != 2 && ((ov >> 4) & 0x0F) != 2) {
        int16_t y = (int16_t)(SAFE_Y0 + 38);
        x_center(c, d.hour < 12 ? "AM" : "PM", 1, y);
      }
      break;
    }

    case X_DATELINE: {
      static const char *const WD[7] =
        { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY",
          "FRIDAY", "SATURDAY" };
      static const char *const MO[12] =
        { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
      const uint8_t wd = (uint8_t)(d.weekday % 7);
      const uint8_t mo = (uint8_t)((d.month >= 1 && d.month <= 12) ? d.month - 1 : 0);
      x_center(c, WD[wd], 1, (int16_t)(SAFE_Y0 + 2));
      snprintf(b, sizeof b, "%u %s", (unsigned)d.day, MO[mo]);
      x_center(c, b, 3, (int16_t)(SAFE_Y0 + 14));
      snprintf(b, sizeof b, "%u", (unsigned)d.year);
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 42));
      break;
    }

    // ---- outside ------------------------------------------------------------
    // The whole forecast in one panel: the condition in words across the top,
    // the sky drawn on the left, the temperature now filling the middle, the
    // day's high over low on the right, and the chance of rain as one plain
    // rule along the bottom. No degree marks — on a panel this size the little
    // circle costs a whole character of width and tells you nothing you did
    // not already know from a screen labelled WEATHER.
    case X_WEATHER: {
      if (!wx_valid) { x_why(c, "WEATHER", 1); break; }
      static const char *const WXN[8] = { "CLEAR", "PARTLY", "CLOUDY", "FOG",
                                          "DRIZZLE", "RAIN", "SNOW", "STORMS" };

      const float now_c = (float)wx_cur_c10 / 10.0f;
      const float max_c = (float)wx_max_c10 / 10.0f;
      const float min_c = (float)wx_min_c10 / 10.0f;
      const int now_t = (int)lroundf(d.temp_f ? now_c * 9.0f / 5.0f + 32.0f : now_c);
      const int max_t = (int)lroundf(d.temp_f ? max_c * 9.0f / 5.0f + 32.0f : max_c);
      const int min_t = (int)lroundf(d.temp_f ? min_c * 9.0f / 5.0f + 32.0f : min_c);

      // Condition across the top, centred, with a rule under it.
      x_center(c, WXN[wx_icon & 7], 1, SAFE_Y0);
      c.drawFastHLine(SAFE_X0, (int16_t)(SAFE_Y0 + 9), SAFE_W, 1);

      // The three columns of the middle band: sky, now, high/low. The right
      // column is measured from the widest of H/L rather than guessed, so a
      // three-digit reading cannot push the big number off its own panel.
      const int16_t band = (int16_t)(SAFE_Y0 + 11);
      char hi[8], lo[8];
      snprintf(hi, sizeof hi, "H%d", max_t);
      snprintf(lo, sizeof lo, "L%d", min_t);
      const size_t rchars = strlen(hi) > strlen(lo) ? strlen(hi) : strlen(lo);
      const int16_t rw = (int16_t)(rchars * 6 - 1);
      const int16_t rx = (int16_t)(SAFE_X0 + SAFE_W - rw);

      wx_icon_draw(c, wx_icon, SAFE_X0, band);

      snprintf(b, sizeof b, "%d", now_t);
      {
        // Size 3 if it fits between the icon and the high/low column, size 2
        // if it does not. Nothing gets clipped and nothing is needlessly small.
        const int16_t left = (int16_t)(SAFE_X0 + 33);
        const int16_t room = (int16_t)(rx - 3 - left);
        const int16_t w3 = (int16_t)(strlen(b) * 18 - 3);
        const uint8_t size = w3 <= room ? 3 : 2;
        const int16_t w = (int16_t)(strlen(b) * 6 * size - (size - 1));
        const int16_t x = (int16_t)(left + (room - w) / 2);
        x_text(c, b, size, (int16_t)(band + (size == 3 ? 5 : 8)), x < left ? left : x);
      }

      x_text(c, hi, 1, (int16_t)(band + 4), rx);
      x_text(c, lo, 1, (int16_t)(band + 16), rx);

      // Chance of rain: one line, not a box. The label sits on the left of the
      // bottom row and the rule to its right fills left-to-right with the
      // percentage — a single 2px stroke, so it reads as an underline rather
      // than another container competing with the panel border.
      {
        const int16_t ly = (int16_t)(SAFE_Y0 + SAFE_H - 7);
        snprintf(b, sizeof b, "RAIN %u%%", (unsigned)wx_pop);
        x_text(c, b, 1, (int16_t)(ly - 1), SAFE_X0);
        const int16_t lx = (int16_t)(SAFE_X0 + (int16_t)(strlen(b) * 6) + 4);
        const int16_t lw = (int16_t)(SAFE_X0 + SAFE_W - lx);
        if (lw > 6) {
          c.drawFastHLine(lx, (int16_t)(ly + 5), lw, 1);           // the track
          const int16_t fw = (int16_t)((int32_t)lw * wx_pop / 100);
          if (fw > 0) c.fillRect(lx, (int16_t)(ly + 2), fw, 3, 1); // how much
        }
      }
      break;
    }

    // Who made it, and which image is running. The same card the clock shows
    // for the first four seconds after power-up, available permanently.
    case X_CREDITS: {
      x_center(c, "4SQUARE", 2, (int16_t)(SAFE_Y0 + 4));
      x_center(c, extras_fw_version(), 2, (int16_t)(SAFE_Y0 + 24));
      x_center(c, "BUILT BY", 1, (int16_t)(SAFE_Y0 + 44));
      x_center(c, FOURSQUARE_BUILT_BY, 1, (int16_t)(SAFE_Y0 + 54));
      break;
    }

    // One saved symbol at a time, swapped every six seconds so a single panel
    // can carry a short watchlist without becoming unreadable.
    case X_STOCK: {
      const uint8_t n = extras_ticker_count();
      if (n == 0) {
        x_center(c, "STOCKS", 1, (int16_t)(SAFE_Y0 + 6));
        x_center(c, "NONE SET", 2, (int16_t)(SAFE_Y0 + 24));
        x_center(c, "HOLD A BUTTON", 1, (int16_t)(SAFE_Y0 + 52));
        break;
      }
      const uint8_t i = (uint8_t)((millis() / 6000u) % n);
      if (!q_have(i)) { x_why(c, extras_ticker(i), 2); break; }
      x_center(c, extras_ticker(i), 2, (int16_t)(SAFE_Y0 + 2));
      char b[24];
      const int32_t p = q_price_at(i);
      snprintf(b, sizeof b, "%ld.%02u", (long)(p / 100), (unsigned)(labs(p) % 100));
      x_center(c, b, 2, (int16_t)(SAFE_Y0 + 26));
      const int32_t ch = q_chg_at(i);
      snprintf(b, sizeof b, "%s%ld.%02u%%", ch < 0 ? "-" : "+",
               (long)(labs(ch) / 100), (unsigned)(labs(ch) % 100));
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 52));
      break;
    }

    // A followed team: the two sides of the game and where it is.
    case X_SPORTS: {
      const uint8_t n = extras_team_count();
      if (n == 0) {
        x_center(c, "SPORTS", 1, (int16_t)(SAFE_Y0 + 6));
        x_center(c, "NO TEAMS", 2, (int16_t)(SAFE_Y0 + 24));
        x_center(c, "HOLD A BUTTON", 1, (int16_t)(SAFE_Y0 + 52));
        break;
      }
      const uint8_t i = (uint8_t)((millis() / 8000u) % n);
      if (!s_have(i)) { x_why(c, extras_team(i), 3); break; }
      x_center(c, s_home(i), 2, (int16_t)(SAFE_Y0 + 6));
      x_center(c, s_away(i), 2, (int16_t)(SAFE_Y0 + 28));
      x_center(c, s_state(i), 1, (int16_t)(SAFE_Y0 + 52));
      break;
    }

    default:


      x_pair(c, "EXTRA", "?", 3);
      break;
  }


  x_overlay(c, ov, d);
}

// ===========================================================================
// the wide canvas — one 256x128 scene, four windows onto it
// ===========================================================================
static int wide_pinned = -1;
int  extras_wide_pinned() { return wide_pinned; }
void extras_set_wide(int id) {
  if (id < 0) { wide_pinned = -1; return; }
  if (id < XA_FIRST || id >= (int)XA_LAST) { wide_pinned = -1; return; }
  wide_pinned = id;
}
int extras_wide_next() {
  if (wide_pinned < 0)                    wide_pinned = (int)XA_FIRST;
  else if (wide_pinned + 1 >= (int)XA_LAST) wide_pinned = -1;
  else                                    wide_pinned = wide_pinned + 1;
  return wide_pinned;
}

static const int16_t WIDE_W = SCR_W * 2;
static const int16_t WIDE_H = SCR_H * 2;

// One panel's window into the wide canvas, and the safe-area clip. Every wide
// primitive goes through here, so nothing can land under the shift clip.
struct Win { int16_t ox, oy; };
static Win win_of(uint8_t slot) {
  Win w;
  w.ox = (int16_t)((slot & 1) ? SCR_W : 0);
  w.oy = (int16_t)((slot & 2) ? SCR_H : 0);
  return w;
}

static void w_px(GFXcanvas1 &c, const Win &w, int16_t gx, int16_t gy) {
  const int16_t x = (int16_t)(gx - w.ox), y = (int16_t)(gy - w.oy);
  if (x < SAFE_X0 || x > SAFE_X1 || y < SAFE_Y0 || y > SAFE_Y1) return;
  c.drawPixel(x, y, 1);
}

static void w_rect(GFXcanvas1 &c, const Win &w, int16_t gx, int16_t gy,
                   int16_t rw, int16_t rh, bool fill) {
  for (int16_t yy = 0; yy < rh; yy++)
    for (int16_t xx = 0; xx < rw; xx++) {
      const bool edge = (xx == 0 || yy == 0 || xx == rw - 1 || yy == rh - 1);
      if (fill || edge) w_px(c, w, (int16_t)(gx + xx), (int16_t)(gy + yy));
    }
}

static void w_disc(GFXcanvas1 &c, const Win &w, int16_t gx, int16_t gy,
                   int16_t r) {
  for (int16_t dy = -r; dy <= r; dy++)
    for (int16_t dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r)
        w_px(c, w, (int16_t)(gx + dx), (int16_t)(gy + dy));
}

static void w_line(GFXcanvas1 &c, const Win &w, int16_t x0, int16_t y0,
                   int16_t x1, int16_t y1) {
  const int16_t dx = (int16_t)abs(x1 - x0), dy = (int16_t)-abs(y1 - y0);
  int16_t sx = (int16_t)(x0 < x1 ? 1 : -1), sy = (int16_t)(y0 < y1 ? 1 : -1);
  int16_t err = (int16_t)(dx + dy);
  for (int guard = 0; guard < 1024; guard++) {
    w_px(c, w, x0, y0);
    if (x0 == x1 && y0 == y1) break;
    const int16_t e2 = (int16_t)(2 * err);
    if (e2 >= dy) { err = (int16_t)(err + dy); x0 = (int16_t)(x0 + sx); }
    if (e2 <= dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
  }
}

// ---- Pac-Man ---------------------------------------------------------------
// He eats along the top row, wraps at the right edge, and the ghost trails him.
static void wide_pacman(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const int16_t lane = (int16_t)(SCR_H / 2);
  const int16_t x = (int16_t)((f * 2) % (WIDE_W + 40) - 20);
  // the dot trail, eaten behind him
  for (int16_t dx = 8; dx < WIDE_W; dx += 14)
    if (dx > x + 10) w_disc(c, w, dx, lane, 1);
  // Pac-Man: a disc with a mouth cut by two lines of background
  w_disc(c, w, x, lane, 9);
  const int open = (f / 4) % 2;
  if (open) {
    for (int16_t dx = 0; dx <= 9; dx++) {
      for (int16_t dy = 0; dy <= dx / 2 + 1; dy++) {
        const int16_t px = (int16_t)(x + dx);
        // punch the wedge back out by redrawing black
        const int16_t sx = (int16_t)(px - w.ox);
        const int16_t syu = (int16_t)(lane - dy - w.oy);
        const int16_t syd = (int16_t)(lane + dy - w.oy);
        if (sx >= 0 && sx < SCR_W) {
          if (syu >= 0 && syu < SCR_H) c.drawPixel(sx, syu, 0);
          if (syd >= 0 && syd < SCR_H) c.drawPixel(sx, syd, 0);
        }
      }
    }
  }
  // the ghost, 30 px behind
  const int16_t gx = (int16_t)(x - 34);
  w_disc(c, w, gx, (int16_t)(lane - 2), 8);
  w_rect(c, w, (int16_t)(gx - 8), (int16_t)(lane - 2), 17, 10, true);
  for (int16_t i = 0; i < 4; i++)
    w_rect(c, w, (int16_t)(gx - 8 + i * 5), (int16_t)(lane + 8), 3, 3, true);
}

// ---- Donkey Kong -----------------------------------------------------------
// Four girders, one per panel row, and barrels rolling down across the bezel.
static void wide_dk(GFXcanvas1 &c, const Win &w, uint16_t f) {
  for (int16_t g = 0; g < 4; g++) {
    const int16_t y = (int16_t)(24 + g * 30);
    w_rect(c, w, 4, y, (int16_t)(WIDE_W - 8), 3, true);
    for (int16_t x = 8; x < WIDE_W - 8; x += 24)
      w_rect(c, w, x, (int16_t)(y - 6), 2, 6, true);
  }
  for (int b = 0; b < 3; b++) {
    const uint16_t t = (uint16_t)(f + b * 60);
    const int16_t g = (int16_t)((t / 90) % 4);
    const int16_t y = (int16_t)(24 + g * 30 - 6);
    const int16_t span = (int16_t)(WIDE_W - 24);
    const int16_t p = (int16_t)((t % 90) * span / 90);
    const int16_t x = (int16_t)((g % 2) ? WIDE_W - 12 - p : 12 + p);
    w_disc(c, w, x, y, 5);
    w_line(c, w, (int16_t)(x - 3), y, (int16_t)(x + 3), y);
  }
  // the ape, top left
  w_rect(c, w, 10, 6, 16, 14, true);
  w_rect(c, w, 6, 10, 4, 8, true);
  w_rect(c, w, 26, 10, 4, 8, true);
}

// ---- Space Invaders --------------------------------------------------------
static void wide_invaders(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const int16_t step = (int16_t)((f / 20) % 2);
  const int16_t drop = (int16_t)((f / 40) % 12);
  const int16_t sway = (int16_t)(((f / 20) % 8) * 4 - 16);
  for (int16_t row = 0; row < 3; row++)
    for (int16_t col = 0; col < 8; col++) {
      const int16_t x = (int16_t)(16 + col * 28 + sway);
      const int16_t y = (int16_t)(10 + row * 22 + drop * 3);
      w_rect(c, w, x, y, 12, 8, true);
      w_rect(c, w, (int16_t)(x - 3), (int16_t)(y + 2), 3, 4, true);
      w_rect(c, w, (int16_t)(x + 12), (int16_t)(y + 2), 3, 4, true);
      w_rect(c, w, (int16_t)(x + (step ? 0 : 2)), (int16_t)(y + 8), 3, 3, true);
      w_rect(c, w, (int16_t)(x + (step ? 9 : 7)), (int16_t)(y + 8), 3, 3, true);
    }
  // the gun, sliding along the bottom row
  const int16_t gx = (int16_t)(WIDE_W / 2 + (int16_t)(sinf(f / 30.0f) * 70.0f));
  w_rect(c, w, (int16_t)(gx - 10), (int16_t)(WIDE_H - 14), 20, 6, true);
  w_rect(c, w, (int16_t)(gx - 2), (int16_t)(WIDE_H - 20), 4, 6, true);
  const int16_t by = (int16_t)(WIDE_H - 24 - (f % 60) * 2);
  if (by > 6) w_rect(c, w, gx, by, 2, 6, true);
}

// ---- the snake, crawling the full 2x2 circuit ------------------------------
static void wide_snake(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const int16_t m = 12;
  const int16_t pw = (int16_t)(WIDE_W - 2 * m), ph = (int16_t)(WIDE_H - 2 * m);
  const int32_t per = 2 * (pw + ph);
  for (int seg = 0; seg < 26; seg++) {
    int32_t t = ((int32_t)f * 3 - seg * 5) % per;
    if (t < 0) t += per;
    int16_t x, y;
    if (t < pw)                 { x = (int16_t)(m + t);           y = m; }
    else if (t < pw + ph)       { x = (int16_t)(m + pw);          y = (int16_t)(m + t - pw); }
    else if (t < 2 * pw + ph)   { x = (int16_t)(m + pw - (t - pw - ph)); y = (int16_t)(m + ph); }
    else                        { x = m;                           y = (int16_t)(m + ph - (t - 2 * pw - ph)); }
    w_disc(c, w, x, y, (int16_t)(seg == 0 ? 4 : 3));
  }
}

// ---- one ball, four panels -------------------------------------------------
static void wide_ball(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const int16_t r = 7;
  const int16_t spanx = (int16_t)(WIDE_W - 2 * r - 8);
  const int16_t spany = (int16_t)(WIDE_H - 2 * r - 8);
  const int32_t px = ((int32_t)f * 3) % (2 * spanx);
  const int32_t py = ((int32_t)f * 2) % (2 * spany);
  const int16_t x = (int16_t)(4 + r + (px < spanx ? px : 2 * spanx - px));
  const int16_t y = (int16_t)(4 + r + (py < spany ? py : 2 * spany - py));
  w_disc(c, w, x, y, r);
  // the box it lives in, drawn once across all four panels
  w_rect(c, w, 2, 2, (int16_t)(WIDE_W - 4), (int16_t)(WIDE_H - 4), false);
}

// ---- a rocket that climbs from the bottom row to the top -------------------
static void wide_rocket(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const uint16_t t = (uint16_t)(f % 200);
  const int16_t y = (int16_t)(WIDE_H - 10 - t * (WIDE_H + 20) / 200);
  const int16_t x = (int16_t)(WIDE_W / 2 + (int16_t)(sinf(t / 18.0f) * 10.0f));
  w_rect(c, w, (int16_t)(x - 4), y, 8, 20, true);
  w_line(c, w, (int16_t)(x - 4), y, x, (int16_t)(y - 10));
  w_line(c, w, (int16_t)(x + 4), y, x, (int16_t)(y - 10));
  w_rect(c, w, (int16_t)(x - 9), (int16_t)(y + 12), 5, 8, true);
  w_rect(c, w, (int16_t)(x + 4), (int16_t)(y + 12), 5, 8, true);
  const int16_t flame = (int16_t)(6 + (f % 4) * 3);
  for (int16_t i = 0; i < flame; i++)
    w_line(c, w, (int16_t)(x - 3 + (i % 3)), (int16_t)(y + 20 + i),
                 (int16_t)(x + 3 - (i % 3)), (int16_t)(y + 20 + i));
  // a few stars, so the climb reads as movement
  for (int16_t s = 0; s < 18; s++) {
    const int16_t sx = (int16_t)((s * 37) % WIDE_W);
    const int16_t sy = (int16_t)(((s * 53) + f) % WIDE_H);
    w_px(c, w, sx, sy);
  }
}

// ---- a train, carriages spanning the bezel ---------------------------------
static void wide_train(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const int16_t base = (int16_t)(WIDE_H / 2 + 18);
  w_rect(c, w, 0, base, WIDE_W, 2, true);
  const int16_t head = (int16_t)((f * 2) % (WIDE_W + 180) - 180);
  // engine
  w_rect(c, w, head, (int16_t)(base - 22), 34, 22, false);
  w_rect(c, w, (int16_t)(head + 4), (int16_t)(base - 32), 8, 10, true);
  const int16_t puff = (int16_t)((f / 3) % 5);
  for (int16_t p = 0; p < 4; p++)
    w_disc(c, w, (int16_t)(head + 8 - p * 9 - puff), (int16_t)(base - 38 - p * 4),
           (int16_t)(3 + p));
  w_disc(c, w, (int16_t)(head + 8), base, 4);
  w_disc(c, w, (int16_t)(head + 26), base, 4);
  // carriages
  for (int16_t k = 1; k <= 3; k++) {
    const int16_t cx = (int16_t)(head - k * 46);
    w_rect(c, w, cx, (int16_t)(base - 18), 40, 18, false);
    for (int16_t win = 0; win < 3; win++)
      w_rect(c, w, (int16_t)(cx + 5 + win * 11), (int16_t)(base - 14), 7, 7, true);
    w_disc(c, w, (int16_t)(cx + 8), base, 4);
    w_disc(c, w, (int16_t)(cx + 32), base, 4);
  }
}

// ---- rain falling from the top row into a puddle on the bottom -------------
static void wide_rain(GFXcanvas1 &c, const Win &w, uint16_t f) {
  const int16_t floor_y = (int16_t)(WIDE_H - 12);
  for (int16_t d = 0; d < 26; d++) {
    const int16_t x = (int16_t)((d * 41 + (d % 3) * 7) % WIDE_W);
    const int16_t speed = (int16_t)(3 + (d % 3));
    const int16_t y = (int16_t)(((int32_t)f * speed + d * 29) % (floor_y + 20));
    if (y < floor_y) w_line(c, w, x, y, x, (int16_t)(y + 5));
    else {
      const int16_t age = (int16_t)(y - floor_y);
      w_line(c, w, (int16_t)(x - age), floor_y, (int16_t)(x - age + 3), floor_y);
      w_line(c, w, (int16_t)(x + age - 3), floor_y, (int16_t)(x + age), floor_y);
    }
  }
  w_rect(c, w, 0, (int16_t)(floor_y + 4), WIDE_W, 2, true);
}

void extras_wide_draw(GFXcanvas1 &c, uint8_t id, uint8_t slot, uint16_t frame) {
  c.fillScreen(0);
  c.setFont(nullptr);
  c.setTextWrap(false);
  const Win w = win_of((uint8_t)(slot & 3));
  switch (id) {
    case XA_PACMAN:   wide_pacman(c, w, frame);   break;
    case XA_DK:       wide_dk(c, w, frame);       break;
    case XA_INVADERS: wide_invaders(c, w, frame); break;
    case XA_SNAKE:    wide_snake(c, w, frame);    break;
    case XA_BALL:     wide_ball(c, w, frame);     break;
    case XA_ROCKET:   wide_rocket(c, w, frame);   break;
    case XA_TRAIN:    wide_train(c, w, frame);    break;
    default:          wide_rain(c, w, frame);     break;
  }
}

// ===========================================================================
// the four home screens, the watch lists, and the menu on the glass
// ===========================================================================
// Everything below is reachable two ways: by holding a button on the back, or
// over HTTP from the app and the phone page. Both call the same functions, so
// the glass and the phone can never disagree about what is stored.

// ---- anti burn-in ----------------------------------------------------------
static bool auto_off = false;
bool extras_autooffset() { return auto_off; }
void extras_set_autooffset(bool on) {
  auto_off = on;
  cache_begin();
  if (cache_open) remote_cache.putBool("x_off", on);
}
static int8_t x_dx() {
  if (!auto_off) return 0;
  const uint8_t step = (uint8_t)((millis() / 600000u) & 3u);   // every 10 min
  return (int8_t)((step == 1 || step == 2) ? 2 : 0);
}
static int8_t x_dy() {
  if (!auto_off) return 0;
  const uint8_t step = (uint8_t)((millis() / 600000u) & 3u);
  return (int8_t)((step >= 2) ? 2 : 0);
}

// ---- the watch lists -------------------------------------------------------
#define TICK_MAX 6
#define TEAM_MAX 4
static char    tick_sym[TICK_MAX][10];
static uint8_t tick_n = 0;
static int32_t q_price_c[TICK_MAX];
static int32_t q_chg_p100[TICK_MAX];
static bool    q_ok[TICK_MAX];

static char    team_id[TEAM_MAX][14];
static uint8_t team_n = 0;
static char    sc_home[TEAM_MAX][16];
static char    sc_away[TEAM_MAX][16];
static char    sc_st[TEAM_MAX][16];
static bool    sc_ok[TEAM_MAX];

static bool        q_have(uint8_t i)     { return i < tick_n && q_ok[i]; }
static int32_t     q_price_at(uint8_t i) { return i < TICK_MAX ? q_price_c[i] : 0; }
static int32_t     q_chg_at(uint8_t i)   { return i < TICK_MAX ? q_chg_p100[i] : 0; }
static bool        s_have(uint8_t i)     { return i < team_n && sc_ok[i]; }
static const char *s_home(uint8_t i)     { return i < TEAM_MAX ? sc_home[i] : ""; }
static const char *s_away(uint8_t i)     { return i < TEAM_MAX ? sc_away[i] : ""; }
static const char *s_state(uint8_t i)    { return i < TEAM_MAX ? sc_st[i] : ""; }

static char list_csv[96];

// Case-insensitive compare, kept local so the file needs no extra header.
static bool ci_eq(const char *a, const char *b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return false;
    if (!x) return true;
  }
}

static void list_save() {
  cache_begin();
  if (!cache_open) return;
  char buf[96];
  buf[0] = 0;
  for (uint8_t i = 0; i < tick_n; i++) {
    if (i) strncat(buf, ",", sizeof buf - strlen(buf) - 1);
    strncat(buf, tick_sym[i], sizeof buf - strlen(buf) - 1);
  }
  remote_cache.putString("tick", buf);
  buf[0] = 0;
  for (uint8_t i = 0; i < team_n; i++) {
    if (i) strncat(buf, ",", sizeof buf - strlen(buf) - 1);
    strncat(buf, team_id[i], sizeof buf - strlen(buf) - 1);
  }
  remote_cache.putString("team", buf);
}

// Split a comma list into one of the two arrays. Entries are upper-cased and
// trimmed to the slot width, which is also what the app's endpoint expects.
static void list_parse(const char *csv, bool teams) {
  uint8_t n = 0;
  char cur[16];
  uint8_t len = 0;
  for (const char *p = csv; ; p++) {
    if (*p && *p != ',') {
      char ch = *p;
      if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
      if (len < (uint8_t)(sizeof cur - 1)) cur[len++] = ch;
      continue;
    }
    cur[len] = 0;
    if (len) {
      if (teams) {
        if (n < TEAM_MAX) { snprintf(team_id[n], sizeof team_id[n], "%s", cur); n++; }
      } else {
        if (n < TICK_MAX) { snprintf(tick_sym[n], sizeof tick_sym[n], "%s", cur); n++; }
      }
    }
    len = 0;
    if (!*p) break;
  }
  if (teams) team_n = n; else tick_n = n;
}

static bool lists_loaded = false;
static void lists_load() {
  if (lists_loaded) return;
  lists_loaded = true;
  cache_begin();
  if (!cache_open) return;
  String t = remote_cache.getString("tick", "");
  String s = remote_cache.getString("team", "");
  list_parse(t.c_str(), false);
  list_parse(s.c_str(), true);
}

uint8_t     extras_ticker_count() { lists_load(); return tick_n; }
const char *extras_ticker(uint8_t i) { lists_load(); return i < tick_n ? tick_sym[i] : ""; }
uint8_t     extras_team_count() { lists_load(); return team_n; }
const char *extras_team(uint8_t i) { lists_load(); return i < team_n ? team_id[i] : ""; }

const char *extras_ticker_csv() {
  lists_load();
  list_csv[0] = 0;
  for (uint8_t i = 0; i < tick_n; i++) {
    if (i) strncat(list_csv, ",", sizeof list_csv - strlen(list_csv) - 1);
    strncat(list_csv, tick_sym[i], sizeof list_csv - strlen(list_csv) - 1);
  }
  return list_csv;
}
const char *extras_team_csv() {
  lists_load();
  list_csv[0] = 0;
  for (uint8_t i = 0; i < team_n; i++) {
    if (i) strncat(list_csv, ",", sizeof list_csv - strlen(list_csv) - 1);
    strncat(list_csv, team_id[i], sizeof list_csv - strlen(list_csv) - 1);
  }
  return list_csv;
}

bool extras_ticker_add(const char *sym) {
  lists_load();
  if (sym == nullptr || !sym[0] || tick_n >= TICK_MAX) return false;
  snprintf(tick_sym[tick_n], sizeof tick_sym[tick_n], "%s", sym);
  for (char *p = tick_sym[tick_n]; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
  q_ok[tick_n] = false;
  tick_n++;
  list_save();
  return true;
}
void extras_ticker_remove(uint8_t i) {
  lists_load();
  if (i >= tick_n) return;
  for (uint8_t j = i; j + 1 < tick_n; j++) {
    memcpy(tick_sym[j], tick_sym[j + 1], sizeof tick_sym[0]);
    q_price_c[j] = q_price_c[j + 1];
    q_chg_p100[j] = q_chg_p100[j + 1];
    q_ok[j] = q_ok[j + 1];
  }
  tick_n--;
  list_save();
}
bool extras_team_add(const char *id) {
  lists_load();
  if (id == nullptr || !id[0] || team_n >= TEAM_MAX) return false;
  snprintf(team_id[team_n], sizeof team_id[team_n], "%s", id);
  for (char *p = team_id[team_n]; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
  sc_ok[team_n] = false;
  team_n++;
  list_save();
  return true;
}
void extras_team_remove(uint8_t i) {
  lists_load();
  if (i >= team_n) return;
  for (uint8_t j = i; j + 1 < team_n; j++) {
    memcpy(team_id[j], team_id[j + 1], sizeof team_id[0]);
    memcpy(sc_home[j], sc_home[j + 1], sizeof sc_home[0]);
    memcpy(sc_away[j], sc_away[j + 1], sizeof sc_away[0]);
    memcpy(sc_st[j], sc_st[j + 1], sizeof sc_st[0]);
    sc_ok[j] = sc_ok[j + 1];
  }
  team_n--;
  list_save();
}

void extras_set_quote(const char *sym, int32_t price_c, int32_t chg_pct100) {
  lists_load();
  if (sym == nullptr) return;
  for (uint8_t i = 0; i < tick_n; i++) {
    if (!ci_eq(tick_sym[i], sym)) continue;
    q_price_c[i] = price_c;
    q_chg_p100[i] = chg_pct100;
    q_ok[i] = true;
    return;
  }
}
void extras_set_score(const char *id, const char *home, const char *away,
                      const char *state) {
  lists_load();
  if (id == nullptr) return;
  for (uint8_t i = 0; i < team_n; i++) {
    if (!ci_eq(team_id[i], id)) continue;
    snprintf(sc_home[i], sizeof sc_home[i], "%s", home ? home : "");
    snprintf(sc_away[i], sizeof sc_away[i], "%s", away ? away : "");
    snprintf(sc_st[i], sizeof sc_st[i], "%s", state ? state : "");
    sc_ok[i] = true;
    return;
  }
}

// ---- the four home screens -------------------------------------------------
static uint8_t home_w[4][4], home_s[4][4], home_o[4][4], home_md[4];
static bool    home_loaded = false;
static uint8_t home_active = 0;

static void home_default(uint8_t i) {
  for (uint8_t s = 0; s < 4; s++) {
    home_w[i][s] = cfg.slot_widget[s];
    home_s[i][s] = cfg.slot_style[s];
    home_o[i][s] = cfg.slot_overlay[s];
  }
}

static void home_save(uint8_t i) {
  cache_begin();
  if (!cache_open) return;
  uint8_t blob[13];
  for (uint8_t s = 0; s < 4; s++) {
    blob[s] = home_w[i][s];
    blob[4 + s] = home_s[i][s];
    blob[8 + s] = home_o[i][s];
  }
  blob[12] = home_md[i];
  char k[6];
  snprintf(k, sizeof k, "hs%u", (unsigned)i);
  remote_cache.putBytes(k, blob, sizeof blob);
}

void extras_home_load() {
  if (home_loaded) return;
  home_loaded = true;
  cache_begin();
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t blob[13];
    char k[6];
    snprintf(k, sizeof k, "hs%u", (unsigned)i);
    if (cache_open && remote_cache.getBytes(k, blob, sizeof blob) == sizeof blob) {
      for (uint8_t s = 0; s < 4; s++) {
        home_w[i][s] = blob[s];
        home_s[i][s] = blob[4 + s];
        home_o[i][s] = blob[8 + s];
      }
      home_md[i] = blob[12] ? 1 : 0;
    } else {
      home_default(i);
      // Out of the box the four buttons behave exactly as they always have:
      // the first is the saved layout, the rest are the firmware's own pages.
      home_md[i] = (uint8_t)(i == 0 ? 0 : 1);
    }
  }
  home_active = cache_open ? remote_cache.getUChar("h_act", 0) : 0;
  if (home_active > 3) home_active = 0;
  auto_off = cache_open ? remote_cache.getBool("x_off", false) : false;
}

uint8_t extras_home_active() { extras_home_load(); return home_active; }
uint8_t extras_home_mode(uint8_t i) { extras_home_load(); return home_md[i & 3]; }
void extras_home_set_mode(uint8_t i, uint8_t mode) {
  extras_home_load();
  i &= 3;
  home_md[i] = mode ? 1 : 0;
  home_save(i);
}
void extras_home_get(uint8_t i, uint8_t s, uint8_t *w, uint8_t *st, uint8_t *ov) {
  extras_home_load();
  i &= 3; s &= 3;
  if (w)  *w  = home_w[i][s];
  if (st) *st = home_s[i][s];
  if (ov) *ov = home_o[i][s];
}
void extras_home_store(uint8_t i) {
  extras_home_load();
  i &= 3;
  home_default(i);
  home_md[i] = 0;
  home_save(i);
}
void extras_home_apply(uint8_t i) {
  extras_home_load();
  i &= 3;
  home_active = i;
  for (uint8_t s = 0; s < 4; s++) {
    cfg.slot_widget[s]  = home_w[i][s];
    cfg.slot_style[s]   = home_s[i][s];
    cfg.slot_overlay[s] = home_o[i][s];
  }
  settings_sanitize(cfg);
  settings_mark_dirty();
  settings_save();
  cache_begin();
  if (cache_open) remote_cache.putUChar("h_act", home_active);
  ui_show_layout();
}
void extras_home_set(uint8_t i, uint8_t s, uint8_t w, uint8_t st, uint8_t ov) {
  extras_home_load();
  i &= 3; s &= 3;
  home_w[i][s] = w;
  home_s[i][s] = st;
  home_o[i][s] = ov;
  home_save(i);
  if (i == home_active) extras_home_apply(i);
}

// ---- the menu --------------------------------------------------------------
enum {
  MS_ROOT = 0, MS_HOME, MS_SCREEN, MS_PANEL, MS_EDIT,
  MS_TICK, MS_TICK_ADD, MS_SPORT, MS_SPORT_ADD, MS_DISP, MS_DISP_EDIT,
  MS_NET, MS_ABOUT
};

// Which value the editor is on: 0 widget, 1 style, 2 top corner, 3 bottom.
static bool     menu_on = false;
static uint8_t  menu_screen = MS_ROOT;
static uint8_t  menu_sel = 0;
static uint8_t  menu_scr = 0, menu_panel = 0, menu_field = 0, menu_disp = 0;
static uint32_t menu_touch_ms = 0;
static uint32_t menu_open_ms = 0;
static uint8_t  menu_hist[6], menu_hist_sel[6], menu_hist_n = 0;
static char     menu_buf[24];
static char     menu_val[26];
static char     menu_entry[10];
static uint8_t  menu_entry_len = 0;
static uint8_t  menu_wheel = 1;
static const char *WHEEL = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.";
static const uint32_t MENU_IDLE_MS = 90000;

static const char *const OV_NAME[10] = {
  "NONE", "SECONDS", "AM/PM", "TEMP", "LI +7D", "SEC BAR",
  "WIFI", "SUNRISE", "SUNSET", "SUN BOTH"
};
static const char *const DISP_NAME[7] = {
  "24 HOUR", "TEMP UNIT", "LED MODE", "LED LEVEL",
  "SEC BAR", "BAR TICKS", "AUTO OFFSET"
};

bool extras_menu_active() { return menu_on; }

void extras_menu_open() {
  if (menu_on) return;
  extras_home_load();
  lists_load();
  menu_on = true;
  menu_screen = MS_ROOT;
  menu_sel = 0;
  menu_hist_n = 0;
  menu_touch_ms = millis();
  menu_open_ms = menu_touch_ms;
  ui_show_layout();
}

void extras_menu_close() {
  if (!menu_on) return;
  menu_on = false;
  ui_show_layout();
}

static void menu_go(uint8_t screen) {
  if (menu_hist_n < 6) {
    menu_hist[menu_hist_n] = menu_screen;
    menu_hist_sel[menu_hist_n] = menu_sel;
    menu_hist_n++;
  }
  menu_screen = screen;
  menu_sel = 0;
}

static void menu_back() {
  if (menu_hist_n == 0) { extras_menu_close(); return; }
  menu_hist_n--;
  menu_screen = menu_hist[menu_hist_n];
  menu_sel = menu_hist_sel[menu_hist_n];
}

// How many widget ids the editor can walk: the stock faces first, then the
// derived screens, as one continuous list.
static uint16_t widget_span() { return (uint16_t)(W_COUNT + X_COUNT); }
static uint8_t widget_at(uint16_t idx) {
  return (uint8_t)(idx < W_COUNT ? idx : (X_FIRST + (idx - W_COUNT)));
}
static uint16_t widget_index(uint8_t w) {
  if (w >= X_FIRST && w < (uint8_t)(X_FIRST + X_COUNT))
    return (uint16_t)(W_COUNT + (w - X_FIRST));
  return (uint16_t)(w < W_COUNT ? w : 0);
}

uint8_t extras_menu_count() {
  switch (menu_screen) {
    case MS_ROOT:      return 6;
    case MS_HOME:      return 4;
    case MS_SCREEN:    return 7;   // 4 panels, use, save current, mode
    case MS_PANEL:     return 4;
    case MS_EDIT:      return 1;
    case MS_TICK:      return (uint8_t)(extras_ticker_count() + 1);
    case MS_SPORT:     return (uint8_t)(extras_team_count() + 1);
    case MS_TICK_ADD:  case MS_SPORT_ADD: return 1;
    case MS_DISP:      return 7;
    case MS_DISP_EDIT: return 1;
    case MS_NET:       return 3;
    default:           return 1;
  }
}

const char *extras_menu_title() {
  switch (menu_screen) {
    case MS_HOME:      return "HOME SCREENS";
    case MS_SCREEN:    snprintf(menu_buf, sizeof menu_buf, "SCREEN %u",
                                (unsigned)(menu_scr + 1)); return menu_buf;
    case MS_PANEL:     snprintf(menu_buf, sizeof menu_buf, "S%u PANEL %c",
                                (unsigned)(menu_scr + 1),
                                (char)('A' + menu_panel)); return menu_buf;
    case MS_EDIT:      return "TURN UP/DOWN";
    case MS_TICK:      return "TICKERS";
    case MS_TICK_ADD:  return "NEW SYMBOL";
    case MS_SPORT:     return "TEAMS";
    case MS_SPORT_ADD: return "NEW TEAM";
    case MS_DISP:      return "DISPLAY";
    case MS_DISP_EDIT: return DISP_NAME[menu_disp % 7];
    case MS_NET:       return "NETWORK";
    case MS_ABOUT:     return "ABOUT";
    default:           return "MENU";
  }
}

// The current display setting as a number, so one editor can serve all of them.
static int32_t disp_get(uint8_t which) {
  switch (which) {
    case 0:  return cfg.hour24;
    case 1:  return cfg.temp_unit;
    case 2:  return cfg.led_mode;
    case 3:  return cfg.led_bright;
    case 4:  return extras_secbar_thick();
    case 5:  return extras_secbar_ticks();
    default: return auto_off ? 1 : 0;
  }
}
static void disp_step(uint8_t which, int8_t dir) {
  switch (which) {
    case 0: cfg.hour24 = (uint8_t)(cfg.hour24 ? 0 : 1); break;
    case 1: cfg.temp_unit = (uint8_t)(cfg.temp_unit ? 0 : 1); break;
    case 2: cfg.led_mode = (uint8_t)(cfg.led_mode ? 0 : 1); break;
    case 3: {
      int32_t v = (int32_t)cfg.led_bright + dir * 16;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      cfg.led_bright = (uint8_t)v;
      break;
    }
    case 4: {
      int32_t v = (int32_t)extras_secbar_thick() + dir;
      if (v < 1) v = 4;
      if (v > 4) v = 1;
      extras_set_secbar((uint8_t)v, extras_secbar_ticks());
      return;
    }
    case 5: {
      int32_t v = (int32_t)extras_secbar_ticks() + dir;
      if (v < 0) v = 2;
      if (v > 2) v = 0;
      extras_set_secbar(extras_secbar_thick(), (uint8_t)v);
      return;
    }
    default: extras_set_autooffset(!auto_off); return;
  }
  settings_mark_dirty();
  settings_save();
}
static const char *disp_text(uint8_t which) {
  const int32_t v = disp_get(which);
  switch (which) {
    case 0: return v ? "24 HOUR" : "12 HOUR";
    case 1: return v ? "FAHRENHEIT" : "CELSIUS";
    case 2: return v ? "LEDS ON" : "LEDS OFF";
    case 3: snprintf(menu_val, sizeof menu_val, "LEVEL %ld", (long)v); return menu_val;
    case 4: snprintf(menu_val, sizeof menu_val, "%ld PX TALL", (long)v); return menu_val;
    case 5: snprintf(menu_val, sizeof menu_val, "%ld TICKS", (long)v); return menu_val;
    default: return v ? "ON" : "OFF";
  }
}

const char *extras_menu_label(uint8_t row) {
  switch (menu_screen) {
    case MS_ROOT:
      switch (row) {
        case 0: return "HOME SCREENS";
        case 1: return "TICKERS";
        case 2: return "TEAMS";
        case 3: return "DISPLAY";
        case 4: return "NETWORK";
        default: return "ABOUT";
      }
    case MS_HOME:
      snprintf(menu_buf, sizeof menu_buf, "%sSCREEN %u %s",
               row == extras_home_active() ? "*" : " ", (unsigned)(row + 1),
               extras_home_mode(row) ? "PAGE" : "SAVED");
      return menu_buf;
    case MS_SCREEN:
      if (row < 4) {
        uint8_t w = 0;
        extras_home_get(menu_scr, row, &w, nullptr, nullptr);
        snprintf(menu_buf, sizeof menu_buf, "%c %s", (char)('A' + row),
                 widget_name(w));
        return menu_buf;
      }
      if (row == 4) return "SHOW IT NOW";
      if (row == 5) return "SAVE WHAT IS UP";
      snprintf(menu_buf, sizeof menu_buf, "MODE %s",
               extras_home_mode(menu_scr) ? "PAGE" : "SAVED");
      return menu_buf;
    case MS_PANEL: {
      uint8_t w = 0, s = 0, ov = 0;
      extras_home_get(menu_scr, menu_panel, &w, &s, &ov);
      switch (row) {
        case 0: snprintf(menu_buf, sizeof menu_buf, "SCREEN %s", widget_name(w)); break;
        case 1: snprintf(menu_buf, sizeof menu_buf, "STYLE %u", (unsigned)s); break;
        case 2: snprintf(menu_buf, sizeof menu_buf, "TOP %s",
                         OV_NAME[(ov >> 4) % 10]); break;
        default: snprintf(menu_buf, sizeof menu_buf, "BOTTOM %s",
                          OV_NAME[(ov & 0x0F) % 10]); break;
      }
      return menu_buf;
    }
    case MS_EDIT:      return extras_menu_value();
    case MS_TICK:
      if (row < extras_ticker_count()) {
        snprintf(menu_buf, sizeof menu_buf, "%s (DROP)", extras_ticker(row));
        return menu_buf;
      }
      return "ADD A SYMBOL";
    case MS_SPORT:
      if (row < extras_team_count()) {
        snprintf(menu_buf, sizeof menu_buf, "%s (DROP)", extras_team(row));
        return menu_buf;
      }
      return "ADD A TEAM";
    case MS_TICK_ADD:
    case MS_SPORT_ADD:
      snprintf(menu_buf, sizeof menu_buf, "%s%c", menu_entry,
               WHEEL[menu_wheel % strlen(WHEEL)]);
      return menu_buf;
    case MS_DISP:      return DISP_NAME[row % 7];
    case MS_DISP_EDIT: return disp_text(menu_disp);
    case MS_NET:
      if (row == 0) return "OPEN SETUP AP";
      if (row == 1) return "REJOIN WIFI";
      return "RESTART CLOCK";
    default:           return "BACK";
  }
}

const char *extras_menu_value() {
  switch (menu_screen) {
    case MS_EDIT: {
      uint8_t w = 0, s = 0, ov = 0;
      extras_home_get(menu_scr, menu_panel, &w, &s, &ov);
      switch (menu_field) {
        case 0: snprintf(menu_val, sizeof menu_val, "%s", widget_name(w)); break;
        case 1: snprintf(menu_val, sizeof menu_val, "STYLE %u", (unsigned)s); break;
        case 2: snprintf(menu_val, sizeof menu_val, "%s", OV_NAME[(ov >> 4) % 10]); break;
        default: snprintf(menu_val, sizeof menu_val, "%s", OV_NAME[(ov & 0x0F) % 10]); break;
      }
      return menu_val;
    }
    case MS_DISP_EDIT: return disp_text(menu_disp);
    case MS_TICK_ADD:
    case MS_SPORT_ADD: return menu_entry;
    case MS_ABOUT:     return extras_fw_version();
    default:           return "";
  }
}

uint8_t extras_menu_row() { return menu_sel; }

// One field of one panel, changed live so the value on the glass is the value
// being edited.
static void edit_step(int8_t dir) {
  uint8_t w = 0, s = 0, ov = 0;
  extras_home_get(menu_scr, menu_panel, &w, &s, &ov);
  if (menu_field == 0) {
    const uint16_t span = widget_span();
    uint16_t idx = (uint16_t)((widget_index(w) + span + dir) % span);
    w = widget_at(idx);
    if (extras_is_widget(w)) s = 0;
  } else if (menu_field == 1) {
    for (uint8_t tries = 0; tries < 8; tries++) {
      s = (uint8_t)((s + 8 + dir) % 8);
      if (widget_allows(w, s)) break;
    }
  } else if (menu_field == 2) {
    uint8_t top = (uint8_t)(((ov >> 4) + 10 + dir) % 10);
    ov = (uint8_t)((top << 4) | (ov & 0x0F));
  } else {
    uint8_t bot = (uint8_t)(((ov & 0x0F) + 10 + dir) % 10);
    ov = (uint8_t)((ov & 0xF0) | bot);
  }
  extras_home_set(menu_scr, menu_panel, w, s, ov);
}

static void menu_select() {
  switch (menu_screen) {
    case MS_ROOT:
      if (menu_sel == 0) menu_go(MS_HOME);
      else if (menu_sel == 1) menu_go(MS_TICK);
      else if (menu_sel == 2) menu_go(MS_SPORT);
      else if (menu_sel == 3) menu_go(MS_DISP);
      else if (menu_sel == 4) menu_go(MS_NET);
      else menu_go(MS_ABOUT);
      return;
    case MS_HOME:
      menu_scr = menu_sel;
      menu_go(MS_SCREEN);
      return;
    case MS_SCREEN:
      if (menu_sel < 4) { menu_panel = menu_sel; menu_go(MS_PANEL); return; }
      if (menu_sel == 4) { extras_home_set_mode(menu_scr, 0); extras_home_apply(menu_scr); return; }
      if (menu_sel == 5) { extras_home_store(menu_scr); return; }
      extras_home_set_mode(menu_scr, (uint8_t)(extras_home_mode(menu_scr) ? 0 : 1));
      return;
    case MS_PANEL:
      menu_field = menu_sel;
      menu_go(MS_EDIT);
      return;
    case MS_EDIT:
      menu_back();
      return;
    case MS_TICK:
      if (menu_sel < extras_ticker_count()) { extras_ticker_remove(menu_sel); if (menu_sel) menu_sel--; return; }
      menu_entry[0] = 0; menu_entry_len = 0; menu_wheel = 1;
      menu_go(MS_TICK_ADD);
      return;
    case MS_SPORT:
      if (menu_sel < extras_team_count()) { extras_team_remove(menu_sel); if (menu_sel) menu_sel--; return; }
      menu_entry[0] = 0; menu_entry_len = 0; menu_wheel = 1;
      menu_go(MS_SPORT_ADD);
      return;
    case MS_TICK_ADD:
    case MS_SPORT_ADD: {
      const char ch = WHEEL[menu_wheel % strlen(WHEEL)];
      if (ch == ' ') {           // the blank on the wheel is DONE
        if (menu_entry_len) {
          if (menu_screen == MS_TICK_ADD) extras_ticker_add(menu_entry);
          else                            extras_team_add(menu_entry);
        }
        menu_back();
        return;
      }
      if (menu_entry_len < (uint8_t)(sizeof menu_entry - 1)) {
        menu_entry[menu_entry_len++] = ch;
        menu_entry[menu_entry_len] = 0;
      }
      return;
    }
    case MS_DISP:
      menu_disp = menu_sel;
      menu_go(MS_DISP_EDIT);
      return;
    case MS_DISP_EDIT:
      menu_back();
      return;
    case MS_NET:
      if (menu_sel == 0) { extras_menu_close(); webcfg_portal_open(); return; }
      if (menu_sel == 1) { extras_menu_close(); webcfg_wifi_rejoin(); return; }
      extras_menu_close();
      ESP.restart();
      return;
    default:
      menu_back();
      return;
  }
}

void extras_menu_key(uint8_t button) {
  if (!menu_on) return;
  menu_touch_ms = millis();
  const uint8_t n = extras_menu_count();
  switch (button) {
    case 2:   // UP
      if (menu_screen == MS_EDIT) { edit_step(+1); return; }
      if (menu_screen == MS_DISP_EDIT) { disp_step(menu_disp, +1); return; }
      if (menu_screen == MS_TICK_ADD || menu_screen == MS_SPORT_ADD) {
        menu_wheel = (uint8_t)((menu_wheel + 1) % strlen(WHEEL));
        return;
      }
      menu_sel = (uint8_t)((menu_sel + n - 1) % n);
      return;
    case 3:   // DOWN
      if (menu_screen == MS_EDIT) { edit_step(-1); return; }
      if (menu_screen == MS_DISP_EDIT) { disp_step(menu_disp, -1); return; }
      if (menu_screen == MS_TICK_ADD || menu_screen == MS_SPORT_ADD) {
        menu_wheel = (uint8_t)((menu_wheel + strlen(WHEEL) - 1) % strlen(WHEEL));
        return;
      }
      menu_sel = (uint8_t)((menu_sel + 1) % n);
      return;
    case 1:   // SET
      menu_select();
      return;
    default:  // MODE: back, and out of the menu at the top
      if ((menu_screen == MS_TICK_ADD || menu_screen == MS_SPORT_ADD) && menu_entry_len) {
        menu_entry[--menu_entry_len] = 0;   // backspace before leaving
        return;
      }
      menu_back();
      return;
  }
}

// The panels are painted one after another in a single burst, exactly like the
// boot screen, so the same gap trick tells us which of the four this call is.
static uint8_t  menu_slot = 0;
static uint32_t menu_last_draw_ms = 0;
static uint8_t menu_next_slot() {
  const uint32_t now = millis();
  if (menu_last_draw_ms == 0 || (uint32_t)(now - menu_last_draw_ms) > 150u)
    menu_slot = 0;
  else
    menu_slot = (uint8_t)((menu_slot + 1u) & 3u);
  menu_last_draw_ms = now;
  return menu_slot;
}

void extras_menu_draw(GFXcanvas1 &c) {
  if (menu_on && millis() - menu_touch_ms > MENU_IDLE_MS) { extras_menu_close(); return; }
  c.fillScreen(0);
  c.setFont(nullptr);
  c.setTextWrap(false);
  const uint8_t slot = menu_next_slot();
  const uint8_t n = extras_menu_count();
  char b[26];
  switch (slot) {
    // ---- top left: where you are -------------------------------------------
    case 0:
      x_center(c, "MENU", 1, (int16_t)(SAFE_Y0 + 6));
      x_center(c, extras_menu_title(), 1, (int16_t)(SAFE_Y0 + 22));
      snprintf(b, sizeof b, "%u OF %u", (unsigned)(menu_sel + 1), (unsigned)n);
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 40));
      x_bar(c, (int16_t)(SAFE_Y0 + 52), 7,
            (uint8_t)(n <= 1 ? 100 : (menu_sel * 100u) / (n - 1)));
      break;

    // ---- top right: the list, three rows at a time -------------------------
    case 1: {
      for (uint8_t r = 0; r < 3; r++) {
        const int8_t off = (int8_t)(r - 1);
        const int16_t idx = (int16_t)menu_sel + off;
        if (idx < 0 || idx >= (int16_t)n) continue;
        snprintf(b, sizeof b, "%s%s", off == 0 ? ">" : " ",
                 extras_menu_label((uint8_t)idx));
        x_text(c, b, 1, (int16_t)(SAFE_Y0 + 10 + r * 18), SAFE_X0);
      }
      break;
    }

    // ---- bottom left: the value under the cursor ---------------------------
    case 2: {
      const char *v = extras_menu_value();
      if (v[0]) {
        x_center(c, "VALUE", 1, (int16_t)(SAFE_Y0 + 6));
        x_center(c, v, 1, (int16_t)(SAFE_Y0 + 24));
      } else {
        x_center(c, extras_menu_label(menu_sel), 1, (int16_t)(SAFE_Y0 + 6));
        snprintf(b, sizeof b, "%s", extras_fw_version());
        x_center(c, b, 1, (int16_t)(SAFE_Y0 + 24));
      }
      snprintf(b, sizeof b, "IP %s", ui_env.ip[0] ? ui_env.ip : "OFFLINE");
      x_center(c, b, 1, (int16_t)(SAFE_Y0 + 46));
      break;
    }

    // ---- bottom right: what the buttons do right now -----------------------
    default:
      if (menu_screen == MS_EDIT || menu_screen == MS_DISP_EDIT) {
        x_center(c, "UP/DN CHANGE", 1, (int16_t)(SAFE_Y0 + 8));
        x_center(c, "SET KEEP IT", 1, (int16_t)(SAFE_Y0 + 24));
      } else if (menu_screen == MS_TICK_ADD || menu_screen == MS_SPORT_ADD) {
        x_center(c, "UP/DN LETTER", 1, (int16_t)(SAFE_Y0 + 8));
        x_center(c, "SET ADD CHAR", 1, (int16_t)(SAFE_Y0 + 24));
        x_center(c, "BLANK = DONE", 1, (int16_t)(SAFE_Y0 + 38));
        break;
      } else {
        x_center(c, "UP/DN MOVE", 1, (int16_t)(SAFE_Y0 + 8));
        x_center(c, "SET CHOOSE", 1, (int16_t)(SAFE_Y0 + 24));
      }
      x_center(c, "MODE BACK", 1, (int16_t)(SAFE_Y0 + 40));
      x_center(c, "HOLD TO EXIT", 1, (int16_t)(SAFE_Y0 + 54));
      break;
  }
}

// ---- what a short press does ----------------------------------------------
bool extras_button_press(uint8_t i) {
  i &= 3;
  if (menu_on) {
    // The press that let go of the opening hold is not a menu keystroke.
    if (millis() - menu_open_ms < 700u) return true;
    extras_menu_key(i);
    return true;
  }
  extras_home_load();
  if (home_md[i] == 0) { extras_home_apply(i); return true; }
  return false;   // this button still selects a firmware page
}
