#include "BambuParse.h"
#include "PrintParse.h"
#include <string.h>
#include <stdio.h>

size_t BambuPayload::append(const uint8_t* bytes, size_t count) {
  if (overflow || count > capacity - size) { overflow = true; return 0; }
  memcpy(data + size, bytes, count);
  size += count;
  return count;
}

namespace {
PrintState stateOf(const char* s) {
  if (!strcmp(s, "RUNNING")) return PS_PRINTING;
  if (!strcmp(s, "PAUSE") || !strcmp(s, "PAUSED")) return PS_PAUSED;
  if (!strcmp(s, "PREPARE") || !strcmp(s, "SLICING") || !strcmp(s, "INIT")) return PS_PREPARE;
  if (!strcmp(s, "FINISH") || !strcmp(s, "FINISHED")) return PS_FINISHED;
  if (!strcmp(s, "FAILED")) return PS_FAILED;
  if (!strcmp(s, "CANCELLED") || !strcmp(s, "CANCELED") || !strcmp(s, "STOPPED")) return PS_CANCELLED;
  if (!strcmp(s, "IDLE") || !strcmp(s, "READY")) return PS_IDLE;
  return PS_UNKNOWN;
}

void clearJob(BambuTelemetry& t) {
  PrintInfo& p = t.info;
  p.pct = -1; p.elapsedS = p.remainS = -1; p.layer = p.layers = -1;
  p.file[0] = p.material[0] = p.message[0] = 0;
  p.filamentG = -1;
  t.task[0] = t.subtask[0] = 0;
  t.named = false;
  t.errorCode = 0;
}

void updateTemp(JsonVariantConst value, float& target) {
  if (value.is<float>()) {
    const float t = value.as<float>();
    target = isfinite(t) && t >= -50 && t <= 600 ? t : NAN;
  }
}

void jobId(JsonVariantConst value, char (&out)[48]) {
  out[0] = 0;
  if (value.is<const char*>()) {
    const char* s = value.as<const char*>();
    if (strcmp(s, "0")) PrintParse_CleanName(out, sizeof(out), s, false);
  } else if (value.is<uint64_t>() && value.as<uint64_t>() != 0) {
    snprintf(out, sizeof(out), "%llu", (unsigned long long)value.as<uint64_t>());
  }
}

JsonDocument makeFilter() {
  JsonDocument f;
  JsonObject p = f["print"].to<JsonObject>();
  for (const char* k : {"command", "gcode_state", "subtask_id", "task_id", "subtask_name", "gcode_file",
       "mc_percent", "mc_remaining_time", "layer_num", "total_layer_num", "nozzle_temper",
       "nozzle_target_temper", "bed_temper", "bed_target_temper", "print_error"}) p[k] = true;
  return f;
}
}

bool BambuParse_Report(const uint8_t* bytes, size_t len, BambuTelemetry& last, BambuChange& change) {
  change = {};
  if (!bytes || !len || len > BAMBU_REPORT_MAX) return false;
  static const JsonDocument filter = makeFilter();
  JsonDocument doc;
  if (deserializeJson(doc, bytes, len, DeserializationOption::Filter(filter))) return false;
  JsonObjectConst p = doc["print"];
  if (p.isNull() || (p["command"].is<const char*>() && p["command"] != "push_status")) return false;
  if (p.size() == 0 || (p.size() == 1 && !p["command"].isNull())) return false;

  const char* state = p["gcode_state"] | "";
  PrintInfo& out = last.info;
  const PrintState old = out.state;
  PrintState next = *state ? stateOf(state) : old;
  updateTemp(p["nozzle_temper"], out.tNoz);
  updateTemp(p["nozzle_target_temper"], out.tNozT);
  updateTemp(p["bed_temper"], out.tBed);
  updateTemp(p["bed_target_temper"], out.tBedT);
  // Full idle snapshots may still carry the previous file, progress and ETA.
  if (*state && next == PS_IDLE) {
    clearJob(last);
    out.state = PS_IDLE;
    last.haveState = change.reportedState = true;
    return true;
  }
  char task[48], subtask[48];
  jobId(p["task_id"], task);
  jobId(p["subtask_id"], subtask);
  const char* name = p["subtask_name"] | "";
  const char* fallback = p["gcode_file"] | "";
  if (const char* slash = strrchr(fallback, '/')) fallback = slash + 1;
  char clean[sizeof(out.file)]{};
  if (*name || *fallback)
    PrintParse_CleanName(clean, sizeof(clean), *name ? name : fallback, true);

  // A task and its subtask may have different IDs. Deltas often contain only one.
  const bool changedId = (*task && *last.task && strcmp(task, last.task)) ||
                         (*subtask && *last.subtask && strcmp(subtask, last.subtask));
  const bool restarted = *state && last.haveState && !PrintState_Active(old) && PrintState_Active(next);
  const bool announced = !*state && old == PS_IDLE && (*task || *subtask || clean[0]);
  const bool comparableName = last.named ? *name != 0 : !*name && *fallback;
  const bool changedName = !*task && !*subtask && comparableName && clean[0] && out.file[0] && strcmp(clean, out.file);
  change.newJob = changedId || restarted || changedName || announced;
  if (change.newJob) last.resetHistory = true;
  if (change.newJob) clearJob(last);
  if (*task) strcpy(last.task, task);
  if (*subtask) strcpy(last.subtask, subtask);
  if (*name || *fallback) {
    if (*name || !last.named) memcpy(out.file, clean, sizeof(clean));
    if (*name) last.named = true;
  }

  if (p["mc_percent"].is<float>()) {
    const float v = p["mc_percent"];
    out.pct = v >= 0 && v <= 100 ? v : -1;
  }
  if (p["mc_remaining_time"].is<int>()) {
    const int minutes = p["mc_remaining_time"];
    out.remainS = minutes >= 0 && minutes < INT32_MAX / 60 ? minutes * 60 : -1;
  }
  if (p["layer_num"].is<int16_t>()) out.layer = p["layer_num"];
  if (p["total_layer_num"].is<int16_t>()) out.layers = p["total_layer_num"];
  if (p["print_error"].is<uint32_t>()) {
    uint32_t code = p["print_error"];
    last.errorCode = code;
    if (code) snprintf(out.message, sizeof(out.message), "Bambu %08lX", (unsigned long)code);
    else out.message[0] = 0;
  }

  // A front-panel stop is reported as FAILED with "task was canceled".
  // The code subsequently clears while the printer continues to report FAILED.
  if (next == PS_FAILED && (last.errorCode == 0x0300400C ||
      (old == PS_CANCELLED && !last.errorCode && !change.newJob))) next = PS_CANCELLED;
  change.reportedState = *state && PrintState_Online(next);
  if (*state) {
    // A job replacement may arrive before its state. Its first terminal
    // report can't be attributed to the previous job.
    if (last.haveState && !changedId && !changedName) change.event = PrintParse_Event(old, next);
    last.haveState = PrintState_Online(next);
    out.state = next;
    if (next == PS_FINISHED) out.remainS = 0;
  } else if (change.newJob) {
    last.haveState = false;
    out.state = PS_UNKNOWN;
  }
  return true;
}
