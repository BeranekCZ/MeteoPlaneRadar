#pragma once
#include <Arduino.h>
#include <map>
struct Preferences {
  static inline std::map<std::string, std::string> data;
  static inline bool failWrite = false;
  bool begin(const char* name, bool) { return !strcmp(name, "planeradar"); }
  bool isKey(const char* key) const { return data.count(key) != 0; }
  size_t putBytes(const char* key, const void* buf, size_t n) {
    if (failWrite) return 0;
    data[key].assign(static_cast<const char*>(buf), n);
    return n;
  }
  size_t getBytesLength(const char* key) const { return data.at(key).size(); }
  size_t getBytes(const char* key, void* buf, size_t n) const {
    if (getBytesLength(key) != n) return 0;
    memcpy(buf, data.at(key).data(), n);
    return n;
  }
};
