#include "BambuParse.h"
#include "BambuDisplay.h"
#include "BambuSettings.h"
#include "PrintHistory.h"
#include "PrintNotices.h"
#include "ScreenMask.h"
#include <Preferences.h>
#include <cassert>
#include <iostream>
#include <vector>

static uint32_t clockMs;
uint32_t millis() { return clockMs; }

static JsonDocument json(const char* s) {
  JsonDocument doc;
  assert(!deserializeJson(doc, s));
  return doc;
}
static BambuChange report(BambuTelemetry& t, const std::string& fields) {
  auto bytes = "{\"print\":{" + fields + "}}";
  BambuChange c;
  assert(BambuParse_Report(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), t, c));
  return c;
}

static void telemetry() {
  BambuTelemetry t;
  auto c = report(t, R"("gcode_state":"FINISH")");
  assert(c.event == PE_NONE && c.reportedState && t.info.state == PS_FINISHED);
  c = report(t, R"("command":"push_status","gcode_state":"PREPARE","task_id":"180","subtask_id":"22","subtask_name":"Part.3mf","nozzle_temper":210.5,"bed_temper":55)");
  assert(c.newJob && c.event == PE_NONE && !strcmp(t.info.file, "Part"));
  report(t, R"("gcode_state":"RUNNING","mc_remaining_time":91,"mc_percent":18,"total_layer_num":100,"layer_num":2)");
  assert(!report(t, R"("task_id":180)").newJob && t.info.state == PS_PRINTING);
  assert(!report(t, R"("subtask_id":22)").newJob && t.info.remainS == 5460);
  c = report(t, R"("layer_num":3,"nozzle_target_temper":220)");
  assert(!c.reportedState && t.info.state == PS_PRINTING && t.info.layer == 3 && t.info.layers == 100);
  assert(t.info.remainS == 5460 && t.info.tNoz == 210.5 && t.info.tBed == 55 && t.info.pct == 18);
  assert(report(t, R"("gcode_state":"PAUSE")").event == PE_PAUSED);
  assert(report(t, R"("gcode_state":"PAUSE")").event == PE_NONE);
  assert(report(t, R"("gcode_state":"FINISH")").event == PE_FINISHED);
  assert(report(t, R"("gcode_state":"FINISH")").event == PE_NONE);
  assert(t.info.remainS == 0);
  c = report(t, R"("subtask_id":"23","gcode_file":"Another.gcode")");
  assert(c.newJob && t.info.state == PS_UNKNOWN && t.info.layer < 0 && t.info.pct < 0 && t.info.tBed == 55);
  assert(!strcmp(t.info.file, "Another"));
  report(t, R"("gcode_state":"RUNNING","subtask_name":"Another file")");
  report(t, R"("print_error":50348044)");
  assert(report(t, R"("gcode_state":"FAILED")").event == PE_NONE && t.info.state == PS_CANCELLED);
  assert(report(t, R"("gcode_state":"FAILED","print_error":0)").event == PE_NONE && t.info.state == PS_CANCELLED);
  report(t, R"("gcode_state":"RUNNING")");
  assert(report(t, R"("gcode_state":"FAILED","print_error":1234)").event == PE_FAILED);
  assert(report(t, R"("gcode_state":"FAILED")").event == PE_NONE);
  report(t, R"("gcode_state":"RUNNING")");
  assert(report(t, R"("gcode_state":"STOPPED")").event == PE_NONE);
  report(t, R"("gcode_state":"IDLE","subtask_name":"Previous","mc_percent":100,"bed_temper":42)");
  assert(t.info.state == PS_IDLE && !t.info.file[0] && t.info.pct < 0 && t.info.layers < 0 && t.info.tBed == 42);
  c = report(t, R"("task_id":"99","gcode_file":"/cache/New.gcode","total_layer_num":35)");
  assert(c.newJob && !c.reportedState);
  report(t, R"("gcode_state":"RUNNING")");
  assert(!strcmp(t.info.file, "New") && t.info.layers == 35);

  const std::string large = "{\"print\":{\"ams\":{\"unused\":\"" + std::string(38000, 'x') +
                            "\"},\"gcode_state\":\"RUNNING\",\"mc_percent\":42}}";
  std::vector<uint8_t> storage(BAMBU_REPORT_MAX);
  BambuPayload rx{storage.data(), storage.size()};
  for (size_t i = 0; i < large.size();) {
    const size_t n = std::min<size_t>(197, large.size() - i);
    assert(rx.append((const uint8_t*)large.data()+i, n) == n);
    i += n;
  }
  assert(BambuParse_Report(rx.data, rx.size, t, c) && t.info.pct == 42);
  for (const std::string bad : {"{\"print\":{\"mc_percent\":99", "{}", "{\"print\":{\"command\":\"ack\"}}"}) {
    assert(!BambuParse_Report((const uint8_t*)bad.data(), bad.size(), t, c) && t.info.pct == 42);
  }
  assert(!rx.append((const uint8_t*)large.data(), large.size()) && rx.overflow);
  rx.reset();
  assert(rx.size == 0 && !rx.overflow);
}

static void displayUpdates() {
  BambuTelemetry t;
  report(t, R"("gcode_state":"RUNNING","mc_percent":42,"mc_remaining_time":18,"nozzle_temper":210.1)");
  PrintInfo previous = t.info;
  PrintHistory history;
  assert(history.add(t.info, 1000));
  report(t, R"("gcode_state":"RUNNING","mc_percent":42,"nozzle_temper":210.4)");
  t.info.updatedMs = 1100;
  assert(!BambuDisplay::changed(previous, t.info, history.add(t.info, 1100), false));
  assert(BambuDisplay::changed(previous, t.info, history.add(t.info, 11000), false));

  report(t, R"("nozzle_temper":210.7)");
  assert(BambuDisplay::changed(previous, t.info, false, false));
  previous = t.info;
  report(t, R"("nozzle_target_temper":220,"bed_target_temper":60)");
  assert(BambuDisplay::changed(previous, t.info, false, false));
  previous = t.info;
  report(t, R"("bed_target_temper":50,"layer_num":4,"print_error":123)");
  assert(!BambuDisplay::changed(previous, t.info, false, false)); // bed reading and layer total still missing
  report(t, R"("total_layer_num":80)");
  assert(BambuDisplay::changed(previous, t.info, false, false));
  previous = t.info;
  report(t, R"("mc_percent":43)");
  assert(BambuDisplay::changed(previous, t.info, false, false));
  assert(!BambuDisplay::changed(previous, t.info, false, true)); // behind the banner
  assert(BambuDisplay::changed(previous, t.info, true, true));   // graph below the banner
  previous = t.info;
  report(t, R"("mc_remaining_time":17)");
  assert(BambuDisplay::changed(previous, t.info, false, false));
  previous = t.info;
  report(t, R"("gcode_state":"FINISH")");
  assert(BambuDisplay::changed(previous, t.info, false, false));
  previous = t.info;
  report(t, R"("mc_percent":100)");
  assert(!BambuDisplay::changed(previous, t.info, false, false));

  previous.state = t.info.state = PS_NO_REPORT;
  assert(!BambuDisplay::changed(previous, t.info, true, false));
  previous.state = t.info.state = PS_OFFLINE;
  t.info.nextPollMs = 30000;
  assert(BambuDisplay::changed(previous, t.info, false, false));
}

static void settings() {
  BambuSettings_Begin();
  assert(!BambuSettings_On());
  auto apply = [](const char* s, bool web = true) { return BambuSettings_Apply(json(s).as<JsonVariantConst>(), web); };
  assert(apply(R"({"on":true,"host":"192.168.1.42","serial":"SN42","code":"ABCD1234","token":"test-token"})") == 200);
  BambuConnection c;
  const uint32_t first = BambuSettings_Copy(c);
  assert(BambuSettings_RememberPin(first, std::string(64, 'a').c_str()));
  assert(!BambuSettings_RememberPin(first, std::string(64, 'b').c_str()));
  assert(apply(R"({"code":"","token":"","pause":false,"night":2})") == 200);
  assert(first == BambuSettings_Copy(c) && !BambuSettings_Alerts().pause && c.code == "ABCD1234");
  for (bool indicators : {true, false}) {
    JsonDocument out;
    BambuSettings_ToJson(out.to<JsonObject>(), indicators);
    std::string s; serializeJson(out, s);
    assert(s.find("ABCD1234") == s.npos && s.find("test-token") == s.npos && s.find("aaaa") == s.npos);
    assert(out["hasCode"].is<bool>() == indicators && out["hasToken"].is<bool>() == indicators);
  }
  assert(apply(R"({"code":"XYZW5678","token":"different","forget":true,"error":false})", false) == 200);
  assert(BambuSettings_Copy(c) == first && c.code == "ABCD1234" && c.token == "test-token");
  assert(apply(R"({"host":"999.1.2.3","on":false})") == 400 && BambuSettings_On());
  assert(apply(R"({"serial":"illegal/#"})") == 400 && apply(R"({"code":"short"})") == 400);
  Preferences::failWrite = true;
  assert(apply(R"({"on":false})") == 507 && BambuSettings_On());
  Preferences::failWrite = false;
  BambuSettings_Reset();
  BambuSettings_Begin();  // simulate a boot with the same NVS blob
  BambuSettings_Copy(c);
  assert(c.on && c.fingerprint.size() == 64 && c.code == "ABCD1234" && c.token == "test-token");
  assert(apply(R"({"serial":"NEW","mode":"cloud","region":"cn"})") == 200);
  BambuSettings_Copy(c);
  assert(c.code.empty() && c.token.empty() && c.fingerprint.empty() && c.cloud && c.china);
  assert(!BambuSettings_RememberPin(first, "stale"));
  assert(apply(R"({"code":"12345678","token":"next"})") == 200 && apply(R"({"forget":true})") == 200);
  BambuSettings_Copy(c);
  assert(c.token.empty() && c.code.empty() && c.serial == "NEW");
}

static void screensAndNotices() {
  assert(SCREEN_BAMBU_I == 10 && SCREEN_N == 11 && SCREEN_SETTINGS_I == 6);
  uint16_t mask = 1u << SCREEN_CLOCK_I;
  assert(ScreenMask_Merge(mask, json(R"({"clock":false,"bambu":true})").as<JsonObjectConst>()) == 1u << 10);
  mask |= (1u<<10) | (1u<<SCREEN_CRYPTO_I);
  assert(ScreenMask_Merge(mask, json(R"({"clock":false})").as<JsonObjectConst>()) == (mask & ~1u));
  assert(ScreenMask_Merge(1u << 10, json(R"({"bambu":false})").as<JsonObjectConst>()) == 0);
  PrintHistory prusa, bambu;
  PrintInfo p; p.tNoz = 217.3f;
  bambu.add(p, 1000); bambu.add(p, 2000);
  int16_t nozzle[2]; uint16_t age[2];
  assert(bambu.copy(nozzle, nullptr, nullptr, nullptr, age, 2, 5000) == 1 && nozzle[0] == 2173 && age[0] == 4);
  assert(prusa.copy(nullptr,nullptr,nullptr,nullptr,nullptr,2,5000) == 0);
  PrintNotice n;
  clockMs = UINT32_MAX - 4;
  strcpy(p.file, "B");
  PrintNotices_Post(SCREEN_BAMBU_I, PE_FINISHED, p);
  PrintNotices_Post(SCREEN_PRINT_I, PE_PAUSED, p);
  clockMs = 2;
  PrintNotices_Post(SCREEN_PRINT_I, PE_FAILED, p);
  assert(PrintNotices_Take(n) && n.screen == SCREEN_PRINT_I && n.event == PE_PAUSED);
  assert(PrintNotices_Take(n) && n.screen == SCREEN_BAMBU_I && !strcmp(n.file,"B"));
  assert(PrintNotices_Take(n) && n.event == PE_FAILED && !PrintNotices_Take(n));
  PrintNotices_Post(SCREEN_BAMBU_I, PE_PAUSED, p);
  PrintNotices_Post(SCREEN_PRINT_I, PE_FINISHED, p);
  PrintNotices_Clear(SCREEN_BAMBU_I);
  assert(PrintNotices_Take(n) && n.screen == SCREEN_PRINT_I && !PrintNotices_Take(n));
  PrintNotices_Post(SCREEN_PRINT_I, PE_FINISHED, p);
  PrintNotices_Expire(clockMs + PRINT_ALERT_MAX_MS);
  assert(!PrintNotices_Take(n));
}

int main() {
  telemetry();
  displayUpdates();
  settings();
  screensAndNotices();
  std::cout << "Telemetry, display updates, settings, screens, history and notices passed\n";
}
