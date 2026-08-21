// market.h — the UP page's data. Quotes over WiFi.
//
// NOT PURE, and deliberately kept out of pages.cpp for that reason: this talks
// to the network, so nothing here can be linked into the host prover. What
// crosses the boundary is a Quote with a PREFORMATTED price string, which is
// what lets the renderer stay provable.
#pragma once
#include <Arduino.h>
#include "../screens/pages.h"

void market_begin();

// Advances one symbol at a time. Never blocks for longer than a single HTTPS
// GET, and only runs when WiFi is actually up.
void market_tick(uint32_t now, bool wifi_up);

// Copy the four quotes of `basket` (0 or 1) out for rendering.
void market_basket(uint8_t basket, Quote out[4]);

// For the sensor page and the serial report.
uint32_t market_last_ok(); // millis of the last successful fetch, 0 if never
uint8_t  market_ok_count();

// Serial `M`: one fetch with every stage reported separately.
void     market_probe();
