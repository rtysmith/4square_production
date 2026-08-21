// The subset of Arduino's Print that Adafruit_GFX actually inherits and uses.
// GFX overrides write(uint8_t) to render a glyph; everything below funnels
// into that, which is precisely how text reaches the canvas on the device too.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *buf, size_t n) {
    size_t k = 0;
    while (n--) k += write(*buf++);
    return k;
  }
  size_t print(const char *s) {
    return s ? write((const uint8_t *)s, strlen(s)) : 0;
  }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v)      { char b[16]; snprintf(b, sizeof b, "%d", v);  return print(b); }
  size_t print(unsigned v) { char b[16]; snprintf(b, sizeof b, "%u", v);  return print(b); }
  size_t print(long v)     { char b[24]; snprintf(b, sizeof b, "%ld", v); return print(b); }
  size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof b, "%lu", v); return print(b); }
  size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); return print(b); }
  size_t println() { return print("\r\n"); }
  template <typename T> size_t println(T v) { return print(v) + println(); }
  template <typename T> size_t println(T v, int d) { return print(v, d) + println(); }
};
