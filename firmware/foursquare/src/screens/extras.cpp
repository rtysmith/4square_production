// extras.cpp — see extras.h.
#include "extras.h"
#include "display.h"
#include "../app/ui.h"
#include "../board/sensors.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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

// A caption over a big value: the shape every derived screen uses, so they
// all read as one family rather than nineteen one-offs.
static void x_pair(GFXcanvas1 &c, const char *cap, const char *val,
                   uint8_t val_size) {
  x_center(c, cap, 1, (int16_t)(SAFE_Y0 + 2));
  x_center(c, val, val_size, (int16_t)(SAFE_Y0 + 16));
}

static void x_bar(GFXcanvas1 &c, int16_t y, int16_t h, uint8_t pct) {
  if (pct > 100) pct = 100;
  c.drawRect(SAFE_X0, y, SAFE_W, h, 1);
  const int16_t w = (int16_t)((int32_t)(SAFE_W - 4) * pct / 100);
  if (w > 0) c.fillRect((int16_t)(SAFE_X0 + 2), (int16_t)(y + 2), w,
                        (int16_t)(h - 4), 1);
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

void extras_set_linkedin(int32_t followers, int32_t gained7d) {
  li_followers = followers;
  li_gained    = gained7d;
  li_valid     = true;
}
bool    extras_linkedin_valid()     { return li_valid; }
int32_t extras_linkedin_followers() { return li_followers; }
int32_t extras_linkedin_gained()    { return li_gained; }

static uint8_t wx_icon    = 0;
static int16_t wx_cur_c10 = 0;
static int16_t wx_max_c10 = 0;
static int16_t wx_min_c10 = 0;
static uint8_t wx_pop     = 0;
static bool    wx_valid   = false;

void extras_set_weather(uint8_t icon, int16_t cur_c10, int16_t max_c10,
                        int16_t min_c10, uint8_t pop) {
  wx_icon    = icon > 7 ? 7 : icon;
  wx_cur_c10 = cur_c10;
  wx_max_c10 = max_c10;
  wx_min_c10 = min_c10;
  wx_pop     = pop > 100 ? 100 : pop;
  wx_valid   = true;
}
bool extras_weather_valid() { return wx_valid; }

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
  "UPTIME", "LIGHT", "IP", "FOLLOWERS", "7 DAYS", "CLOCK", "DATE", "WEATHER"
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
  if (!ui_env.wifi_up) { x_pair(c, "WIFI", "DOWN", 3); return; }
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

// The weekly LinkedIn gain, bottom LEFT, small. Left rather than right so it
// can sit alongside seconds or a temperature on the same panel without the two
// colliding.
void extras_overlay_week(GFXcanvas1 &c) {
  char t[12];
  if (!li_valid) snprintf(t, sizeof t, "+--");
  else           snprintf(t, sizeof t, "%+ld", (long)li_gained);
  x_text(c, t, 1, (int16_t)(SAFE_Y0 + SAFE_H - 8), SAFE_X0);
}

// Seconds without digits: a straight filled bar across the bottom of the safe
// area that grows left to right once a minute. No outline — just the bar.
void extras_overlay_secbar(GFXcanvas1 &c, int seconds) {
  if (seconds < 0) return;
  const int16_t y = (int16_t)(SAFE_Y0 + SAFE_H - 4);
  const int16_t x = (int16_t)(SAFE_X0 + 2);
  const int16_t w = (int16_t)(SAFE_W - 4);
  if (w < 8) return;
  const int16_t fill = (int16_t)((long)w * (seconds % 60) / 59L);
  if (fill > 0) c.fillRect(x, y, fill, 4, 1);
}

static void x_overlay(GFXcanvas1 &c, uint8_t ov, const FaceData &d) {
  if (ov == 0) return;                      // OV_NONE
  if (ov == 4) { extras_overlay_week(c); return; }   // OV_LIWEEK, bottom left
  if (ov == 5) { extras_overlay_secbar(c, x_secs(d, 0)); return; }  // OV_SECBAR
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
  x_text(c, t, 1, (int16_t)(SAFE_Y0 + SAFE_H - 12), (int16_t)(SAFE_X0 + SAFE_W - w));
}

// ---- the weather icon ------------------------------------------------------
// Drawn, not written: eight little scenes in a 34x30 box, each built from
// circles, lines and a cloud made of three overlapping discs.
static void wx_cloud(GFXcanvas1 &c, int16_t x, int16_t y, bool filled) {
  if (filled) {
    c.fillCircle((int16_t)(x + 8), (int16_t)(y + 8), 7, 1);
    c.fillCircle((int16_t)(x + 18), (int16_t)(y + 6), 9, 1);
    c.fillCircle((int16_t)(x + 26), (int16_t)(y + 9), 6, 1);
    c.fillRect(x, (int16_t)(y + 8), 28, 8, 1);
  } else {
    c.drawCircle((int16_t)(x + 8), (int16_t)(y + 8), 7, 1);
    c.drawCircle((int16_t)(x + 18), (int16_t)(y + 6), 9, 1);
    c.drawCircle((int16_t)(x + 26), (int16_t)(y + 9), 6, 1);
    c.drawFastHLine(x, (int16_t)(y + 16), 28, 1);
  }
}

static void wx_sun(GFXcanvas1 &c, int16_t cx, int16_t cy, int16_t r, bool rays) {
  c.fillCircle(cx, cy, r, 1);
  if (!rays) return;
  for (uint8_t i = 0; i < 8; i++) {
    const float a = (float)i * 3.14159265f / 4.0f;
    const int16_t x0 = (int16_t)(cx + cosf(a) * (r + 3));
    const int16_t y0 = (int16_t)(cy + sinf(a) * (r + 3));
    const int16_t x1 = (int16_t)(cx + cosf(a) * (r + 6));
    const int16_t y1 = (int16_t)(cy + sinf(a) * (r + 6));
    c.drawLine(x0, y0, x1, y1, 1);
  }
}

static void wx_icon_draw(GFXcanvas1 &c, uint8_t icon, int16_t x, int16_t y) {
  switch (icon) {
    case 0:                                    // clear
      wx_sun(c, (int16_t)(x + 15), (int16_t)(y + 14), 7, true);
      break;
    case 1:                                    // partly cloudy
      wx_sun(c, (int16_t)(x + 8), (int16_t)(y + 6), 5, true);
      wx_cloud(c, (int16_t)(x + 2), (int16_t)(y + 8), false);
      break;
    case 2:                                    // cloudy
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 5), false);
      break;
    case 3:                                    // fog
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 1), false);
      for (uint8_t i = 0; i < 3; i++)
        c.drawFastHLine((int16_t)(x + 2 + (i & 1) * 4), (int16_t)(y + 20 + i * 4), 24, 1);
      break;
    case 4:                                    // drizzle
    case 5: {                                  // rain
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 1), icon == 5);
      const uint8_t drops = icon == 5 ? 4 : 3;
      for (uint8_t i = 0; i < drops; i++) {
        const int16_t dx = (int16_t)(x + 5 + i * 7);
        c.drawLine(dx, (int16_t)(y + 20), (int16_t)(dx - 2), (int16_t)(y + 27), 1);
      }
      break;
    }
    case 6:                                    // snow
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 1), false);
      for (uint8_t i = 0; i < 3; i++) {
        const int16_t sx = (int16_t)(x + 7 + i * 9);
        const int16_t sy = (int16_t)(y + 24);
        c.drawFastHLine((int16_t)(sx - 3), sy, 7, 1);
        c.drawFastVLine(sx, (int16_t)(sy - 3), 7, 1);
      }
      break;
    default:                                   // storm
      wx_cloud(c, (int16_t)(x + 1), (int16_t)(y + 1), true);
      c.drawLine((int16_t)(x + 16), (int16_t)(y + 19), (int16_t)(x + 11), (int16_t)(y + 25), 1);
      c.drawLine((int16_t)(x + 11), (int16_t)(y + 25), (int16_t)(x + 17), (int16_t)(y + 25), 1);
      c.drawLine((int16_t)(x + 17), (int16_t)(y + 25), (int16_t)(x + 11), (int16_t)(y + 31), 1);
      break;
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
      if (li_followers >= 10000) snprintf(b, sizeof b, "%ld.%ldk", (long)(li_followers / 1000), (long)((li_followers % 1000) / 100));
      else snprintf(b, sizeof b, "%ld", (long)li_followers);
      if (!li_valid) snprintf(b, sizeof b, "--");
      // Leave a clear bottom row when a corner is selected: a size-3 value
      // pinned high cannot reach the overlay baseline.
      if (ov != 0) {
        x_center(c, "LINKEDIN", 1, (int16_t)(SAFE_Y0));
        x_center(c, b, 3, (int16_t)(SAFE_Y0 + 10));
      } else {
        x_pair(c, "LINKEDIN", b, 4);
      }
      break;
    }
    case X_LIWEEK: {
      if (!li_valid) { x_pair(c, "7 DAYS", "--", 4); break; }
      snprintf(b, sizeof b, "%+ld", (long)li_gained);
      x_pair(c, "NEW, 7 DAYS", b, 4);
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
      // beside it. Otherwise it sits under the time, clear of the corners.
      // When the seconds bar runs along the bottom, lift AM/PM so the two
      // don't touch.
      if (!d.hour24 && ov != 2) {
        int16_t y = (int16_t)(SAFE_Y0 + 38);
        if (ov == 5) y = (int16_t)(SAFE_Y0 + 28);
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
    // The whole forecast, using the whole panel: the condition in words across
    // the top, the sky drawn on the left, the temperature now in the middle,
    // today's high and low stacked on the right, and the chance of rain as a
    // labelled bar along the bottom edge.
    case X_WEATHER: {
      if (!wx_valid) { x_pair(c, "WEATHER", "--", 4); break; }
      static const char *const WXN[8] = { "CLEAR", "PARTLY", "CLOUDY", "FOG",
                                          "DRIZZLE", "RAIN", "SNOW", "STORMS" };

      const float now_c = (float)wx_cur_c10 / 10.0f;
      const float max_c = (float)wx_max_c10 / 10.0f;
      const float min_c = (float)wx_min_c10 / 10.0f;
      const int now_t = (int)lroundf(d.temp_f ? now_c * 9.0f / 5.0f + 32.0f : now_c);
      const int max_t = (int)lroundf(d.temp_f ? max_c * 9.0f / 5.0f + 32.0f : max_c);
      const int min_t = (int)lroundf(d.temp_f ? min_c * 9.0f / 5.0f + 32.0f : min_c);

      // Condition across the top, centred, with a rule under it.
      x_center(c, WXN[wx_icon & 7], 1, (int16_t)(SAFE_Y0));
      c.drawFastHLine(SAFE_X0, (int16_t)(SAFE_Y0 + 9), SAFE_W, 1);

      // The sky, drawn, hard left.
      wx_icon_draw(c, wx_icon, SAFE_X0, (int16_t)(SAFE_Y0 + 12));

      // Now, as big as the panel allows, in the middle band.
      snprintf(b, sizeof b, "%d%c", now_t, (char)0xF8);
      {
        const int16_t w = (int16_t)(strlen(b) * 18 - 3);
        int16_t x = (int16_t)(SAFE_X0 + 36);
        if (x + w > SAFE_X0 + SAFE_W - 30) x = (int16_t)(SAFE_X0 + SAFE_W - 30 - w);
        if (x < SAFE_X0 + 34) x = (int16_t)(SAFE_X0 + 34);
        x_text(c, b, 3, (int16_t)(SAFE_Y0 + 16), x);
      }

      // Today's high over today's low, right-hand column.
      {
        const int16_t rx = (int16_t)(SAFE_X0 + SAFE_W - 30);
        snprintf(b, sizeof b, "H%d%c", max_t, (char)0xF8);
        x_text(c, b, 1, (int16_t)(SAFE_Y0 + 16), rx);
        snprintf(b, sizeof b, "L%d%c", min_t, (char)0xF8);
        x_text(c, b, 1, (int16_t)(SAFE_Y0 + 28), rx);
      }

      // Chance of rain: the words on the left of the bottom row, the bar
      // filling whatever is left of it.
      {
        const int16_t by = (int16_t)(SAFE_Y0 + SAFE_H - 9);
        snprintf(b, sizeof b, "RAIN %u%%", (unsigned)wx_pop);
        x_text(c, b, 1, (int16_t)(by + 1), SAFE_X0);
        const int16_t bx = (int16_t)(SAFE_X0 + 50);
        const int16_t bw = (int16_t)(SAFE_X0 + SAFE_W - bx);
        if (bw > 8) {
          c.drawRect(bx, by, bw, 8, 1);
          const int16_t fw = (int16_t)((int32_t)(bw - 4) * wx_pop / 100);
          if (fw > 0) c.fillRect((int16_t)(bx + 2), (int16_t)(by + 2), fw, 4, 1);
        }
      }
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
