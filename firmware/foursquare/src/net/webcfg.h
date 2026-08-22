// webcfg.h — the small HTTP face of the clock: layout, buttons, updates.
//
// ABSENT FROM A DEMO IMAGE. A demo build has no radio at all (src/demo.h), so
// every symbol here lives behind the same guard the OTA code uses.
#pragma once
#include <Arduino.h>

#ifndef DEMO_BUILD
// Start the HTTP server. Safe to call repeatedly; only the first call binds.
void webcfg_begin();

// Pump the HTTP server while Wi-Fi is connected.
void webcfg_tick();

// The clock's single Wi-Fi owner. It keeps the current association through
// transient drops, retries both configured networks with bounded backoff, and
// power-cycles only the radio when the driver itself stops responding.
void webcfg_wifi_keeper_tick();

// Live recovery state for the physical Wi-Fi panel. Stage values are:
// 0 live, 1 checking a brief loss, 2 joining, 3 requesting an IP address,
// 4 waiting to retry, 5 resetting the radio.
uint8_t webcfg_wifi_stage();
uint8_t webcfg_wifi_progress();
uint8_t webcfg_wifi_attempt();
uint8_t webcfg_wifi_network();
// Last concrete failure: 0 none, 1 network missing, 2 authentication rejected,
// 3 join timeout, 4 DHCP supplied no address, 5 radio unresponsive.
uint8_t webcfg_wifi_failure();

// True while an HTTP firmware upload is in flight, so the main loop can leave
// the screens and the I2C bus alone.
bool webcfg_updating();
#endif
