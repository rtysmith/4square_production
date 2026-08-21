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
  X_LAST
};
static const uint8_t X_FIRST = (uint8_t)X_FEELS;
static const uint8_t X_COUNT = (uint8_t)(X_LAST - X_FEELS);

bool extras_is_widget(uint8_t w);
const char *extras_widget_name(uint8_t w);
void extras_face_render(GFXcanvas1 &c, uint8_t w, const FaceData &d);

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
