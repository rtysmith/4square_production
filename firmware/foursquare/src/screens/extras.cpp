// extras.cpp — see extras.h.
#include "extras.h"
#include "display.h"
#include "../app/ui.h"
#include "../board/sensors.h"
#include "../net/webcfg.h"
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
static void x_text(GFXcanvas1 &c, const char *s, uint8_t size, int16_t y,
                   int16_t x) {
  c.setTextSize(size);
  c.setTextColor(1);
  c.setCursor(x, y);
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
  const uint8_t  stage = webcfg_wifi_stage();
  const bool     online = webcfg_wifi_online();
  const char    *failed = splash_fail_label(webcfg_wifi_failure());

  switch (slot) {
    // ---- top left: who this is ---------------------------------------------
    case 0:
      x_center(c, "4SQUARE", 2, (int16_t)(SAFE_Y0 + 4));
      x_center(c, FOURSQUARE_FW_VERSION, 1, (int16_t)(SAFE_Y0 + 24));
      x_center(c, "BUILT BY", 1, (int16_t)(SAFE_Y0 + 38));
      x_center(c, FOURSQUARE_BUILT_BY, 1, (int16_t)(SAFE_Y0 + 48));
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

void extras_set_linkedin(int32_t followers, int32_t gained7d) {
  const bool changed = !li_valid || li_followers != followers || li_gained != gained7d;
  li_followers = followers;
  li_gained    = gained7d;
  li_valid     = true;
  if (changed) {
    cache_begin();
    if (cache_open) {
      remote_cache.putLong("li_total", followers);
      remote_cache.putLong("li_week", gained7d);
      remote_cache.putBool("li_ok", true);
    }
  }
}
bool    extras_linkedin_valid()     { return li_valid; }
int32_t extras_linkedin_followers() { return li_followers; }
int32_t extras_linkedin_gained()    { return li_gained; }

void extras_cache_restore() {
  cache_begin();
  if (!cache_open) return;
  if (remote_cache.getBool("li_ok", false)) {
    li_followers = remote_cache.getLong("li_total", 0);
    li_gained = remote_cache.getLong("li_week", 0);
    li_valid = true;
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
// Two tiny slots, written by the fetcher and read by the renderers. Kept in
// RAM only: a reason is about right now, and after a reboot the first read is
// seconds away anyway.
static char     feed_reason[2][18] = { "STARTING", "STARTING" };
static uint32_t feed_ok_ms[2]      = { 0, 0 };
static bool     feed_ever_ok[2]    = { false, false };

void extras_feed_note(uint8_t which, const char *reason) {
  if (which > 1 || reason == nullptr) return;
  snprintf(feed_reason[which], sizeof feed_reason[which], "%s", reason);
}
void extras_feed_ok(uint8_t which) {
  if (which > 1) return;
  feed_reason[which][0] = 0;
  feed_ok_ms[which] = millis();
  feed_ever_ok[which] = true;
}
const char *extras_feed_reason(uint8_t which) {
  return which > 1 ? "" : feed_reason[which];
}
int32_t extras_feed_age_min(uint8_t which) {
  if (which > 1 || !feed_ever_ok[which]) return -1;
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

// ===========================================================================
// the derived screens
// ===========================================================================
bool extras_is_widget(uint8_t w) { return w >= X_FIRST && w < (uint8_t)X_LAST; }

static const char *const X_NAMES[] = {
  "FEELS", "DEW PT", "ABS RH", "COMFORT", "T TREND", "HI/LO", "RH TREND",
  "DAY NO", "WEEK", "DAYS LEFT", "QUARTER", "MOON", "SEASON", "WIFI",
  "UPTIME", "LIGHT", "IP", "FOLLOWERS", "7 DAYS", "CLOCK", "DATE", "WEATHER",
  "ABOUT"

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
  if (!li_valid) snprintf(t, sizeof t, "+--");
  else           snprintf(t, sizeof t, "%+ld", (long)li_gained);
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
  if (which == 7 || which == 9) {
    if (sun_rise >= 0) {
      snprintf(t, sizeof t, "%d:%02d", (int)(sun_rise / 60), (int)(sun_rise % 60));
      sun_glyph(c, SAFE_X0, (int16_t)(y + 1), true);
      x_text(c, t, 1, y, (int16_t)(SAFE_X0 + 9));
    }
  }
  if (which == 8 || which == 9) {
    if (sun_set >= 0) {
      int hh = (int)(sun_set / 60);
      if (hh > 12) hh -= 12;
      snprintf(t, sizeof t, "%d:%02d", hh, (int)(sun_set % 60));
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
