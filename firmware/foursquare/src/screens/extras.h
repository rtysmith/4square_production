// extras.h — derived screens and animations that span all four panels.
//
// PURE, in the same sense faces.cpp is: extras_face_render() and
// extras_wide_draw() are handed a canvas and a snapshot. The two things that
// cannot be pure — the rolling temperature history and the LinkedIn numbers —
// live behind extras_tick()/extras_set_linkedin() and are read through a
// snapshot struct, so the renderers still take all of their input as arguments.
#pragma once
#include <Adafruit_GFX.h>
#include <stdint.h>
#include "faces.h"

// ---- derived widgets -------------------------------------------------------
// 32 leaves a deliberate gap above W_COUNT (14) so the firmware can grow its
// own widget list without ever colliding with this one.
enum XWidget : uint8_t {
  X_FEELS = 32,   // heat index from temperature + humidity
  X_DEW,          // dew point
  X_ABSHUM,       // absolute humidity, g/m3
  X_COMFORT,      // DRY / OK / MUGGY
  X_TTREND,       // temperature change over the last hour
  X_HILO,         // today's high and low
  X_RHTREND,      // humidity change over the last hour
  X_DOY,          // day of year
  X_WEEKNO,       // ISO week number
  X_DAYSLEFT,     // days left in the year
  X_QUARTER,      // quarter, with a progress bar
  X_MOON,         // moon phase, drawn
  X_SEASON,       // meteorological season
  X_WIFI,         // signal bars + RSSI
  X_UPTIME,       // how long since the last boot
  X_LIGHT,        // ambient light, %
  X_IPADDR,       // the address the app talks to
  X_LIFOLLOWERS,  // LinkedIn followers
  X_LIWEEK,       // LinkedIn, gained in the last 7 days
  X_CLOCKHM,      // hour AND minute together, in one panel
  X_DATELINE,     // weekday, day and month together, in one panel
  X_WEATHER,      // outside: icon, now, today's high, chance of rain
  X_LAST
};

static const uint8_t X_FIRST = (uint8_t)X_FEELS;
static const uint8_t X_COUNT = (uint8_t)(X_LAST - X_FEELS);

bool extras_is_widget(uint8_t w);
const char *extras_widget_name(uint8_t w);
// ov is the corner overlay the editor saved for this panel (OV_NONE/SECONDS/
// AMPM/TEMP/LINKEDIN WEEK). The derived screens draw it themselves —
// face_render returns early for them, so without this a corner setting would
// silently do nothing.
void extras_face_render(GFXcanvas1 &c, uint8_t w, uint8_t ov, const FaceData &d);
// The weekly LinkedIn gain, drawn small in the bottom-left corner. Shared with
// faces.cpp so a normal (non-derived) panel can carry the same corner.
void extras_overlay_week(GFXcanvas1 &c);
// The seconds as a filling bar along the bottom edge instead of a ":07" count.
// Shared with faces.cpp so an ordinary panel can carry it too.
void extras_overlay_secbar(GFXcanvas1 &c, int seconds);
// Three little signal bars in the bottom-right corner; a crossed-out stub when
// the radio is down. Shared with faces.cpp like the two above.
void extras_overlay_wifi(GFXcanvas1 &c);


// ---- animations that cross the bezel ---------------------------------------
// The four panels are treated as one 256x128 canvas. Each panel renders the
// same scene at its own origin, so a sprite walks off one panel and onto the
// next instead of teleporting.
enum XAnim : uint8_t {
  XA_PACMAN = 64,
  XA_DK,
  XA_INVADERS,
  XA_SNAKE,
  XA_BALL,
  XA_ROCKET,
  XA_TRAIN,
  XA_RAIN,
  XA_LAST
};
static const uint8_t XA_FIRST = (uint8_t)XA_PACMAN;
static const uint8_t XA_COUNT = (uint8_t)(XA_LAST - XA_PACMAN);

// Which wide animation the DOWN page is pinned to, or -1 for "play the normal
// per-panel reel". Set over HTTP by the designer.
int  extras_wide_pinned();
void extras_set_wide(int id);
// One more press of the animations button. Walks -1 (the per-panel reel) ->
// each across-all-four animation in turn -> back to -1, so the wide scenes sit
// in the SAME cycle as everything else that button shows. Returns the new
// value; -1 means "the caller should restart the normal reel".
int  extras_wide_next();
void extras_wide_draw(GFXcanvas1 &c, uint8_t id, uint8_t slot, uint16_t frame);

// ---- the impure edge -------------------------------------------------------
// Called once a second from the net tick. Feeds the rolling history the trend
// and high/low widgets read.
void extras_tick(uint32_t now_ms, int16_t temp_c10, uint8_t rh, bool sht_ok,
                 uint8_t hour);
void extras_set_linkedin(int32_t followers, int32_t gained7d);
bool extras_linkedin_valid();
int32_t extras_linkedin_followers();
int32_t extras_linkedin_gained();

// The forecast, fed the same way: the app fetches it, the clock is handed the
// finished numbers. Temperatures are tenths of a degree Celsius, pop is a
// whole percent, icon is 0..7 (clear, partly, cloud, fog, drizzle, rain, snow,
// storm).
void extras_set_weather(uint8_t icon, int16_t cur_c10, int16_t max_c10,
                        int16_t min_c10, uint8_t pop);
bool extras_weather_valid();

// ---- what the four buttons on the back do ----------------------------------
// The mapping used to be a hardcoded switch in ui.cpp:
//
//   MODE (top left) clock   SET (bottom left) sensors
//   UP (top right) markets  DOWN (bottom right) animations
//
// It now lives in the settings record so the editor can change it. It is
// packed three bits per button into the two spare bytes that were already
// reserved, which keeps sizeof(Settings) — and therefore the EEPROM record
// length — exactly as it was. Bit 15 is the "somebody set this" marker, so a
// record written by older firmware (all zeros there) still reads back as the
// factory order below rather than as "every button means clock".
#include "../settings/store.h"
#include "pages.h"

static const uint8_t BTN_FACTORY[4] = { PG_CLOCK, PG_SENSOR, PG_MARKET, PG_ANIM };
static const uint16_t BTN_MAP_SET = 0x8000;

static inline uint16_t btnmap_raw() {
  return (uint16_t)cfg.btn_map_lo | ((uint16_t)cfg.btn_map_hi << 8);
}

/** Which page button i jumps to. Falls back to the factory order. */
static inline uint8_t btn_page_for(uint8_t i) {
  i &= 3;
  const uint16_t raw = btnmap_raw();
  if (!(raw & BTN_MAP_SET)) return BTN_FACTORY[i];
  const uint8_t p = (uint8_t)((raw >> (i * 3)) & 7);
  return (p < PG_COUNT) ? p : BTN_FACTORY[i];
}

/** Point button i at a page. Ignores a page id that does not exist. */
static inline void btn_page_set(uint8_t i, uint8_t page) {
  i &= 3;
  if (page >= PG_COUNT) return;
  uint16_t raw = btnmap_raw();
  if (!(raw & BTN_MAP_SET)) {
    raw = BTN_MAP_SET;
    for (uint8_t k = 0; k < 4; k++) raw |= (uint16_t)BTN_FACTORY[k] << (k * 3);
  }
  raw &= (uint16_t)~(7u << (i * 3));
  raw |= (uint16_t)(page & 7) << (i * 3);
  cfg.btn_map_lo = (uint8_t)(raw & 0xFF);
  cfg.btn_map_hi = (uint8_t)(raw >> 8);
}

/** Back to MODE/SET/UP/DOWN meaning clock/sensors/markets/animations. */
static inline void btn_map_reset() { cfg.btn_map_lo = 0; cfg.btn_map_hi = 0; }
