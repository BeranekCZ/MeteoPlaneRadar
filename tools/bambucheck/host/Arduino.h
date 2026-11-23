#pragma once
// Single-threaded host stand-ins for the ESP32 clock, Arduino String and locks.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
struct String : std::string {
  using std::string::string;
  using std::string::operator=;
  bool isEmpty() const { return empty(); }
};
uint32_t millis();
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) (void)(m)
#define portEXIT_CRITICAL(m) (void)(m)
inline size_t testStrlcpy(char* dst, const char* src, size_t cap) {
  const size_t len = strlen(src);
  if (cap) { const size_t n = std::min(cap - 1, len); memcpy(dst, src, n); dst[n] = 0; }
  return len;
}
#define strlcpy testStrlcpy
