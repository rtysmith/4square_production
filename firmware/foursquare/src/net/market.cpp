#include "market.h"
#include "../demo.h"
#ifdef DEMO_BUILD
// ===========================================================================
// DEMO BUILD -- THE QUOTES ARE BAKED IN AND THERE IS NO NETWORK CODE HERE.
// ===========================================================================
// The whole HTTPS half of this file is #ifdef'd out below, so a demo image
// contains no WiFiClientSecure, no TLS, no fetch and no endpoint URLs. The
// markets page keeps the same four symbols and the same Quote struct, so
// pages.cpp is untouched and the layout prover proves the same screens.
//
// WHY THEY DRIFT. A frozen price reads as a mock-up the moment somebody looks
// at the page twice. These walk on a DETERMINISTIC pseudo-random sequence
// seeded per symbol, so the numbers move a little every few seconds the way a
// live tape does, and the SAME demo unit tells the same story every time --
// which matters when you are showing it to somebody and want to know what it
// is going to say. Nothing here is random at run time and nothing depends on
// the clock being set.
//
// The values are plausible-looking, not real, and are not represented as real
// anywhere on the glass.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Price in CENTS (integer throughout, so nothing can drift into 313.3300001)
// and an opening change in basis points, which is what Quote::chg_bp carries.
struct DemoQuote {
  const char *sym;
  int32_t     base_cents;
  int16_t     chg_bp;      // hundredths of a percent
  uint16_t    seed;        // per-symbol, so each walks its own path
  int32_t     step_cents;  // how far one tick can move it
};

// SPX, NVDA, AAPL, BTC in basket 0, in that order -- the four the user asked
// to keep. A mix of red and green so the page shows both directions at once
// rather than depending on which way a random walk happened to go.
static const DemoQuote DEMO_QUOTES[] = {
  {"SPX",  660412,   84, 0x2F1D,   45},   // 6604.12   +0.84%   up
  {"NVDA",  18327,  213, 0x7A03,    9},   //  183.27   +2.13%   up
  {"AAPL",  23196,  -47, 0x51C7,   11},   //  231.96   -0.47%   down
  {"BTC", 11284300,  126, 0x1E95, 3500},  // 112843    +1.26%   up
  {"TSLA",  41058, -132, 0x63B1,   21},   //  410.58   -1.32%   down
  {"MSFT",  51873,   38, 0x0D4F,   17},   //  518.73   +0.38%   up
  {"AMZN",  22941,   91, 0x39E6,   13},   //  229.41   +0.91%   up
};
static const uint8_t N_DEMO = (uint8_t)(sizeof(DEMO_QUOTES) / sizeof(DEMO_QUOTES[0]));
static const uint8_t DEMO_BASKET[2][4] = { {0, 1, 2, 3}, {4, 5, 6, 3} };

// One step every 3 s: slow enough to read, fast enough that a price has
// visibly moved by the time somebody looks back at the page.
static const uint32_t DEMO_STEP_MS = 3000;

static Quote    demo_cache[7];
static uint32_t demo_step = 0;
static uint32_t demo_next = 0;

// A 16-bit xorshift folded from the symbol's seed and the step number. PURE in
// (seed, step): the walk is reproducible and needs no stored state.
static uint16_t demo_noise(uint16_t seed, uint32_t step) {
  uint16_t x = (uint16_t)(seed ^ (step * 40503u));
  if (!x) x = 0xACE1;
  x ^= (uint16_t)(x << 7);
  x ^= (uint16_t)(x >> 9);
  x ^= (uint16_t)(x << 8);
  return x;
}

// Price to text. Same shape as the live path on purpose: an index runs to five
// figures and a share price to two decimals, and the renderer must never see a
// float.
static void demo_fmt(char *b, size_t n, int32_t cents) {
  int32_t whole = cents / 100, frac = cents % 100;
  if (frac < 0) frac = -frac;
  if (whole >= 10000)     snprintf(b, n, "%ld", (long)whole);
  else if (whole >= 1000) snprintf(b, n, "%ld.%01ld", (long)whole, (long)(frac / 10));
  else                    snprintf(b, n, "%ld.%02ld", (long)whole, (long)frac);
}

static void demo_recompute() {
  for (uint8_t i = 0; i < N_DEMO; i++) {
    const DemoQuote &d = DEMO_QUOTES[i];
    // A BOUNDED walk, not a drift: the offset is derived from the CURRENT step
    // only, so the price wanders around its base forever instead of marching
    // off it.
    uint16_t r = demo_noise(d.seed, demo_step);
    int32_t off = (int32_t)(r % (uint16_t)(2 * d.step_cents + 1)) - d.step_cents;
    int32_t px  = d.base_cents + off;

    // The change percentage moves WITH the price, by the same arithmetic a
    // real one would. A price that ticks while its percentage sits still is
    // the tell that gives a mock-up away, and it is one line to not have it.
    int32_t bp = d.chg_bp + (int32_t)((int64_t)off * 10000 / d.base_cents);
    if (bp >  32000) bp =  32000;
    if (bp < -32000) bp = -32000;

    Quote &q = demo_cache[i];
    snprintf(q.sym, sizeof q.sym, "%s", d.sym);
    demo_fmt(q.price, sizeof q.price, px);
    q.chg_bp = (int16_t)bp;
    q.valid  = true;
  }
}

void market_begin() { demo_step = 0; demo_next = 0; demo_recompute(); }

void market_tick(uint32_t now, bool wifi_up) {
  (void)wifi_up;                       // there is no radio in this build
  if ((int32_t)(now - demo_next) < 0) return;
  demo_next = now + DEMO_STEP_MS;
  demo_step++;
  demo_recompute();
}

void market_basket(uint8_t basket, Quote out[4]) {
  if (basket > 1) basket = 0;
  for (uint8_t k = 0; k < 4; k++) out[k] = demo_cache[DEMO_BASKET[basket][k]];
}

// A demo unit is always fresh; a staleness marker on a page that cannot go
// stale would be saying something untrue.
uint32_t market_last_ok() { return millis() ? millis() : 1; }
uint8_t  market_ok_count() { return N_DEMO; }
void     market_probe() { Serial.println("# DEMO BUILD: quotes are baked in, nothing is fetched"); }

#else   // ---- not DEMO_BUILD: the real thing, over the radio ----------------

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------- sources --
// Both endpoints were verified live before being written in, which is the only
// reason they are here: every "free no-key finance API" list on the web is
// mostly dead links, and Stooq — the classic answer — now answers with a
// JavaScript proof-of-work challenge no MCU can solve.
//
// CBOE's delayed-quote CDN: no key, no User-Agent, no cookie, ~520 bytes, one
// level of nesting. Coinbase and Bitstamp likewise.
//
// THE S&P INDEX SYMBOL IS `_SPX`, with an underscore. `SPX` and `^SPX` both
// return a 403 with an XML AccessDenied body — which is why parse_cboe()
// rejects anything that starts with '<' rather than hunting for a number in
// an error document.
//
// Everything is HTTPS; plain http 301s on every one of them. Hence
// WiFiClientSecure + setInsecure(): there is no certificate to pin that would
// not eventually expire and silently kill the page.

struct Src {
  const char *sym;      // what the panel shows
  const char *url;
  uint8_t     kind;     // 0 = CBOE json, 1 = Bitstamp ticker
};

static const Src SRC[] = {
  {"SPX",  "https://cdn.cboe.com/api/global/delayed_quotes/quotes/_SPX.json", 0},
  {"NVDA", "https://cdn.cboe.com/api/global/delayed_quotes/quotes/NVDA.json", 0},
  {"AAPL", "https://cdn.cboe.com/api/global/delayed_quotes/quotes/AAPL.json", 0},
  {"BTC",  "https://www.bitstamp.net/api/v2/ticker/btcusd/",                  1},
  {"TSLA", "https://cdn.cboe.com/api/global/delayed_quotes/quotes/TSLA.json", 0},
  {"MSFT", "https://cdn.cboe.com/api/global/delayed_quotes/quotes/MSFT.json", 0},
  {"AMZN", "https://cdn.cboe.com/api/global/delayed_quotes/quotes/AMZN.json", 0},
};
static const uint8_t N_SRC = (uint8_t)(sizeof(SRC) / sizeof(SRC[0]));

// Which four each basket shows. BTC appears in both and is fetched once —
// the basket is a view onto the cache, not a fetch list.
static const uint8_t BASKET[2][4] = { {0, 1, 2, 3}, {4, 5, 6, 3} };

static Quote              cache[N_SRC];
static uint8_t            next_i = 0;
static uint32_t           last_ok = 0;
static SemaphoreHandle_t  cache_mtx = nullptr;
static TaskHandle_t       task_h = nullptr;

// One symbol every 4 s, so a full sweep of seven takes ~28 s and the set is
// never more than a minute stale. Fetching all seven back to back would hold
// the loop for several seconds and stall the clock repaint.
static const uint32_t STEP_MS  = 4000;
static const uint32_t SWEEP_MS = 60000;
// Nothing is fetched for the first 15 s. The watchdog panic that found this
// happened at 13.4 s, with the first TLS handshake racing WiFi association and
// the OTA server coming up. There is no reason for a quote to compete with
// those — nobody is looking at the market page one second after power-on.
static const uint32_t FIRST_MS = 15000;

// --------------------------------------------------------------- parsing ---
// strstr + atof, no JSON library. Both bodies are flat and fixed-shape, so
// the first hit on a key is always the right one and there is no nesting to
// track.
static bool find_num(const char *body, const char *key, double *out) {
  const char *p = strstr(body, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ' || *p == '"' || *p == ':') p++;
  char *end = nullptr;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}

// Price to text, so the renderer never sees a float. Four significant-ish
// figures is what fits: an index runs to 7757, a share price to 313.33.
static void fmt_price(char *b, size_t n, double v) {
  if (v >= 10000.0)     snprintf(b, n, "%ld", (long)(v + 0.5));
  else if (v >= 1000.0) snprintf(b, n, "%.1f", v);
  else                  snprintf(b, n, "%.2f", v);
}

static bool parse_cboe(const char *body, Quote &q) {
  if (body[0] == '<') return false;          // the 403 AccessDenied XML
  double price = 0, pct = 0;
  if (!find_num(body, "\"current_price\"", &price)) return false;
  find_num(body, "\"price_change_percent\"", &pct); // absent = leave at zero
  fmt_price(q.price, sizeof q.price, price);
  double bp = pct * 100.0;
  if (bp >  32000) bp =  32000;
  if (bp < -32000) bp = -32000;
  q.chg_bp = (int16_t)(bp < 0 ? bp - 0.5 : bp + 0.5);
  return true;
}

static bool parse_bitstamp(const char *body, Quote &q) {
  if (body[0] == '<') return false;
  double last = 0, open = 0;
  if (!find_num(body, "\"last\"", &last)) return false;
  fmt_price(q.price, sizeof q.price, last);
  q.chg_bp = 0;
  // Bitstamp gives open and last but no percentage, so it is computed here.
  // Guarding on open > 0 is not paranoia: a zero would be a divide by zero on
  // a device with no FPU trap, and the result would render as a plausible
  // number rather than as an error.
  if (find_num(body, "\"open\"", &open) && open > 0.0) {
    double bp = (last - open) / open * 10000.0;
    if (bp >  32000) bp =  32000;
    if (bp < -32000) bp = -32000;
    q.chg_bp = (int16_t)(bp < 0 ? bp - 0.5 : bp + 0.5);
  }
  return true;
}

// ----------------------------------------------------------------- fetch ---
// THIS RUNS ON ITS OWN TASK, NEVER ON loop(). Three attempts got here:
//
//   1. feedLoopWDT() either side of the call. Panicked the board at 13.4 s —
//      feeding is irrelevant when the single call in between exceeds the 5 s
//      timeout on its own.
//   2. disableLoopWDT() around the call. Stopped the reboots and turned them
//      into something worse: the firmware's own stall detector reported
//      `# STALL 11095ms`, because a TLS connect to a route that does not exist
//      runs to full timeout while loop() — and therefore every repaint — waits.
//      The clock froze for 11 s out of every 4 s fetch cycle.
//   3. A separate FreeRTOS task, below. The C3 is single-core, but preemption
//      means loop() keeps painting while this one blocks on a socket.
//
// The lesson generalises: on a device with one loop and a display, NO network
// call belongs on that loop, however carefully its timeouts are chosen.
static bool fetch_one(uint8_t i) {
  WiFiClientSecure client;
  client.setInsecure();               // no cert to expire and kill the page
  // Short, because a dead route should cost seconds and not tens of them even
  // though nothing is waiting on this task any more.
  client.setTimeout(4);               // seconds
  client.setHandshakeTimeout(6);

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.setReuse(false);
  // A redirect chain is how a dead endpoint turns into a 200 full of HTML.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, SRC[i].url)) return false;

  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    // Bounded. getString() on an unexpected multi-megabyte body would be an
    // out-of-memory reboot, and "the clock reboots when the network is odd"
    // is a much worse bug than a missing quote.
    int len = http.getSize();
    if (len < 0 || len <= 2048) {
      String body = http.getString();
      Quote q;
      memset(&q, 0, sizeof q);
      snprintf(q.sym, sizeof q.sym, "%s", SRC[i].sym);
      ok = SRC[i].kind == 0 ? parse_cboe(body.c_str(), q)
                            : parse_bitstamp(body.c_str(), q);
      if (ok) {
        q.valid = true;
        // Guarded: a Quote is 24 bytes and the render task can read it between
        // any two of them. A torn copy would put one symbol's price under
        // another's name, which looks like live data and is not.
        xSemaphoreTake(cache_mtx, portMAX_DELAY);
        cache[i] = q;
        xSemaphoreGive(cache_mtx);
      }
    }
  }
  http.end();
  if (!ok) {
    Serial.print("# market "); Serial.print(SRC[i].sym);
    Serial.print(" failed, http "); Serial.println(code);
  }
  return ok;
}

// ------------------------------------------------------------------- task --
static void market_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(FIRST_MS));
  for (;;) {
    // WiFi.status() is checked HERE rather than trusting a flag from the main
    // loop: associated-but-no-route is exactly the state this board sits in,
    // and it is the state that makes every fetch run to full timeout.
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress((uint32_t)0)) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    if (fetch_one(next_i)) last_ok = millis() ? millis() : 1;
    next_i = (uint8_t)(next_i + 1);
    if (next_i >= N_SRC) {
      next_i = 0;
      vTaskDelay(pdMS_TO_TICKS(SWEEP_MS));
    } else {
      vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
  }
}

// ------------------------------------------------------------------ api ----
void market_begin() {
  memset(cache, 0, sizeof cache);
  for (uint8_t i = 0; i < N_SRC; i++)
    snprintf(cache[i].sym, sizeof cache[i].sym, "%s", SRC[i].sym);
  cache_mtx = xSemaphoreCreateMutex();
  // 10 KB: a TLS handshake is the deepest thing that runs on this stack.
  xTaskCreate(market_task, "market", 10240, nullptr, 1, &task_h);
}

// Deliberately empty. Fetching moved to market_task() so that loop() — and
// therefore every repaint — can never wait on the network. Kept so the sketch
// reads as "the market has a tick" rather than hiding a task behind begin().
void market_tick(uint32_t now, bool wifi_up) { (void)now; (void)wifi_up; }

void market_basket(uint8_t basket, Quote out[4]) {
  const uint8_t *b = BASKET[basket & 1];
  if (!cache_mtx) { memset(out, 0, sizeof(Quote) * 4); return; }
  xSemaphoreTake(cache_mtx, portMAX_DELAY);
  for (uint8_t s = 0; s < 4; s++) out[s] = cache[b[s]];
  xSemaphoreGive(cache_mtx);
}

uint32_t market_last_ok() { return last_ok; }

uint8_t market_ok_count() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < N_SRC; i++) if (cache[i].valid) n++;
  return n;
}

// ------------------------------------------------------------------ probe --
// Serial `M`. Every stage reported separately, because "http -1" collapses
// DNS failure, TCP refusal, a TLS handshake abort and a read timeout into one
// number, and they have four different fixes.
void market_probe() {
  Serial.print("# probe: wifi status "); Serial.print((int)WiFi.status());
  Serial.print(" rssi "); Serial.print(WiFi.RSSI());
  Serial.print(" heap "); Serial.print((unsigned long)ESP.getFreeHeap());
  Serial.print(" maxalloc "); Serial.println((unsigned long)ESP.getMaxAllocHeap());

  IPAddress ip;
  uint32_t t0 = millis();
  bool dns = WiFi.hostByName("cdn.cboe.com", ip);
  Serial.print("# probe: dns cdn.cboe.com -> ");
  Serial.print(dns ? ip.toString() : String("FAILED"));
  Serial.print(" in "); Serial.print(millis() - t0); Serial.println(" ms");
  if (!dns) return;

  // The probe DOES block loop(), and that is fine: it only runs when a human
  // types `M`, and its whole purpose is to report timings honestly.
  disableLoopWDT();
  {
    WiFiClientSecure c;
    c.setInsecure();
    c.setHandshakeTimeout(15);
    t0 = millis();
    int ok = c.connect(ip, 443);
    Serial.print("# probe: tls connect "); Serial.print(ok ? "OK" : "FAILED");
    Serial.print(" in "); Serial.print(millis() - t0);
    Serial.print(" ms lastError "); Serial.println(c.lastError(nullptr, 0));
    if (ok) {
      HTTPClient http;
      http.setTimeout(10000);
      http.begin(c, "https://cdn.cboe.com/api/global/delayed_quotes/quotes/_SPX.json");
      int code = http.GET();
      Serial.print("# probe: GET -> "); Serial.print(code);
      Serial.print(" ");  Serial.println(HTTPClient::errorToString(code));
      if (code == 200) {
        String b = http.getString();
        Serial.print("# probe: "); Serial.print(b.length());
        Serial.print(" bytes: "); Serial.println(b.substring(0, 120));
      }
      http.end();
    }
    c.stop();
  }
  enableLoopWDT();
  feedLoopWDT();
  Serial.print("# probe: heap after "); Serial.println((unsigned long)ESP.getFreeHeap());
}

#endif  // DEMO_BUILD
