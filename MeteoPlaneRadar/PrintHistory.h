#pragma once
#include "Config.h"
#include "PrintWatch.h"

// The owning monitor holds its mutex while adding or copying samples.
class PrintHistory {
  struct Sample { uint32_t at; int16_t noz, nozT, bed, bedT; };
  Sample samples[PRINT_HIST_N]{};
  unsigned count = 0, head = 0;
  static int16_t tenth(float t) { return isfinite(t) ? (int16_t)lroundf(t * 10) : INT16_MIN; }
public:
  void clear() { count = head = 0; }
  bool add(const PrintInfo& p, uint32_t now) {
    if (!isfinite(p.tNoz) && !isfinite(p.tBed)) return false;
    if (count && now - samples[(head + PRINT_HIST_N - 1) % PRINT_HIST_N].at < PRINT_HIST_STEP_S * 1000UL) return false;
    samples[head] = {now, tenth(p.tNoz), tenth(p.tNozT), tenth(p.tBed), tenth(p.tBedT)};
    head = (head + 1) % PRINT_HIST_N;
    if (count < PRINT_HIST_N) ++count;
    return true;
  }
  int copy(int16_t* noz, int16_t* nozT, int16_t* bed, int16_t* bedT, uint16_t* age, int max, uint32_t now) const {
    const int n = max < (int)count ? max : (int)count;
    for (int i = 0; i < n; ++i) {
      const Sample& s = samples[(head + PRINT_HIST_N - n + i) % PRINT_HIST_N];
      if (noz) noz[i] = s.noz;
      if (nozT) nozT[i] = s.nozT;
      if (bed) bed[i] = s.bed;
      if (bedT) bedT[i] = s.bedT;
      const uint32_t sec = (now - s.at) / 1000;
      if (age) age[i] = sec > UINT16_MAX ? UINT16_MAX : (uint16_t)sec;
    }
    return n;
  }
};
