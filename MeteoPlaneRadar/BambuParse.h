#pragma once
#include "PrintWatch.h"
#include "Config.h"

// Each MQTT PUBLISH is assembled before parsing. A truncated report must never
// partially update a running job.
struct BambuPayload {
  uint8_t* data;
  size_t capacity, size = 0;
  bool overflow = false;
  void reset() { size = 0; overflow = false; }
  size_t append(const uint8_t* bytes, size_t count);
};

struct BambuTelemetry {
  PrintInfo info;
  char task[48] = "", subtask[48] = "";
  bool haveState = false;
  uint32_t errorCode = 0;
  bool named = false;
  bool resetHistory = false;
};

struct BambuChange {
  PrintEvent event = PE_NONE;
  bool newJob = false;
  bool reportedState = false;
};

bool BambuParse_Report(const uint8_t* bytes, size_t len, BambuTelemetry& last, BambuChange& change);
