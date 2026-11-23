#pragma once
#include <Arduino.h>
#include <cstdio>
enum { IPv4, IPv6 };
struct IPAddress {
  int family = IPv4;
  int type() const { return family; }
  bool fromString(const String& text) {
    unsigned a, b, c, d;
    char extra;
    return sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 &&
           a < 256 && b < 256 && c < 256 && d < 256;
  }
};
