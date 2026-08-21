// webcfg.h — the small HTTP face of the clock: layout, buttons, updates.
//
// ABSENT FROM A DEMO IMAGE. A demo build has no radio at all (src/demo.h), so
// every symbol here lives behind the same guard the OTA code uses.
#pragma once
#include <Arduino.h>

#ifndef DEMO_BUILD
// Start the HTTP server. Safe to call repeatedly; only the first call binds.
void webcfg_begin();

// Pump the server. Call once per loop, next to ArduinoOTA.handle().
void webcfg_tick();

// True while an HTTP firmware upload is in flight, so the main loop can leave
// the screens and the I2C bus alone.
bool webcfg_updating();
#endif
