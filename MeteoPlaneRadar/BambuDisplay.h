#pragma once
#include "PrintWatch.h"
#include <string.h>

namespace BambuDisplay {
// temps() prints whole degrees with "%.0f", including the sign of a rounded zero.
inline bool sameDegrees(float a, float b) {
  if (!isfinite(a) || !isfinite(b)) return !isfinite(a) && !isfinite(b);
  return signbit(a) == signbit(b) && (a == b || nearbyintf(a) == nearbyintf(b));
}

inline bool sameTemperature(float a, float aTarget, float b, float bTarget) {
  if (!sameDegrees(a, b)) return false;
  if (!isfinite(a)) return true;
  if ((aTarget > 0) != (bTarget > 0)) return false;
  return !(aTarget > 0) || sameDegrees(aTarget, bTarget);
}

inline bool changed(const PrintInfo& before, const PrintInfo& after, bool graphChanged, bool alert) {
  if (!alert && before.state != after.state) return true;
  if (!alert && !PrintState_Online(after.state))
    return after.state == PS_OFFLINE && before.nextPollMs != after.nextPollMs;

  if (graphChanged ||
      !sameTemperature(before.tNoz, before.tNozT, after.tNoz, after.tNozT) ||
      !sameTemperature(before.tBed, before.tBedT, after.tBed, after.tBedT)) return true;

  // Bambu supplies progress and remaining minutes, but no elapsed time.
  // Its alert ring is fixed and its banner keeps the file and message from the event.
  if (alert || after.state == PS_IDLE) return false;
  if (after.state == PS_CANCELLED) return strcmp(before.file, after.file) != 0;

  if (after.state != PS_FINISHED && before.pct != after.pct) return true;  // progress ring
  if (PrintState_Active(after.state) && before.remainS != after.remainS) return true;
  if ((before.layer >= 0 && before.layers > 0) || (after.layer >= 0 && after.layers > 0)) {
    if (before.layer != after.layer || before.layers != after.layers) return true;
  }
  if (after.state == PS_PAUSED || after.state == PS_FAILED || after.state == PS_FINISHED) {
    if (strcmp(before.message, after.message) != 0) return true;
  }
  return (after.state != PS_PAUSED || !after.message[0]) && strcmp(before.file, after.file) != 0;
}
}
