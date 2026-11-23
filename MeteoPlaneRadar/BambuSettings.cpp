#include "BambuSettings.h"
#include "Config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <memory>
#include <new>
#include <freertos/semphr.h>

namespace {
constexpr size_t TOKEN_MAX = 2048;
constexpr const char* KEY = "bambu";
BambuConnection connection;
BambuAlerts alerts;
std::atomic<uint32_t> revision{1};
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex;

struct Guard {
  Guard() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mutex); }
};

void publicJson(JsonObject out, const BambuConnection& c, const BambuAlerts& a) {
  out["on"] = c.on;
  out["mode"] = c.cloud ? "cloud" : "lan";
  out["region"] = c.china ? "cn" : "global";
  out["host"] = c.host;
  out["serial"] = c.serial;
  out["finish"] = a.finish;
  out["error"] = a.error;
  out["pause"] = a.pause;
  out["night"] = a.night;
}

bool store(const BambuConnection& c, const BambuAlerts& a) {
  JsonDocument doc;
  publicJson(doc.to<JsonObject>(), c, a);
  doc["code"] = c.code;
  doc["token"] = c.token;
  doc["pin"] = c.fingerprint;
  const size_t size = measureJson(doc);
  std::unique_ptr<char[]> bytes(new (std::nothrow) char[size + 1]);
  if (doc.overflowed() || !bytes || serializeJson(doc, bytes.get(), size + 1) != size) return false;
  Preferences p;
  return p.begin("planeradar", false) && p.putBytes(KEY, bytes.get(), size) == size;
}

bool readText(JsonVariantConst v, String& dest, size_t limit, bool secret = false) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return false;
  JsonString s = v.as<JsonString>();
  if (s.size() > limit || strlen(s.c_str()) != s.size()) return false;
  for (size_t i = 0; i < s.size(); ++i)
    if (uint8_t(s.c_str()[i]) < 33 || uint8_t(s.c_str()[i]) > 126) return false;
  if (s.size() || !secret) dest = s.c_str();
  return true;
}

bool choice(JsonVariantConst v, const char* a, const char* b, bool& second) {
  if (v.isNull()) return true;
  if (v == a) second = false;
  else if (v == b) second = true;
  else return false;
  return true;
}

bool applyPublic(JsonObjectConst o, BambuConnection& c, BambuAlerts& a) {
  if (!choice(o["mode"], "lan", "cloud", c.cloud) ||
      !choice(o["region"], "global", "cn", c.china) ||
      !readText(o["host"], c.host, 15) || !readText(o["serial"], c.serial, 32)) return false;
  IPAddress ip;
  if (c.host.length() && (!ip.fromString(c.host) || ip.type() != IPv4)) return false;
  for (size_t i = 0; i < c.serial.length(); ++i)
    if (!isalnum(static_cast<unsigned char>(c.serial[i]))) return false;
  for (const char* k : {"on", "finish", "error", "pause"})
    if (!o[k].isNull() && !o[k].is<bool>()) return false;
  if (!o["night"].isNull() && (!o["night"].is<uint8_t>() || o["night"].as<int>() > PRINT_NIGHT_CALM)) return false;
  c.on = o["on"] | c.on;
  a.finish = o["finish"] | a.finish;
  a.error = o["error"] | a.error;
  a.pause = o["pause"] | a.pause;
  a.night = o["night"] | a.night;
  return true;
}
}

void BambuSettings_Begin() {
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  Preferences p;
  if (!p.begin("planeradar", true) || !p.isKey(KEY)) return;
  size_t size = p.getBytesLength(KEY);
  if (!size || size > 8192) return;
  std::unique_ptr<char[]> buf(new (std::nothrow) char[size]);
  JsonDocument doc;
  if (!buf || p.getBytes(KEY, buf.get(), size) != size || deserializeJson(doc, buf.get(), size)) return;
  BambuConnection c;
  BambuAlerts a;
  if (!applyPublic(doc.as<JsonObjectConst>(), c, a) ||
      !readText(doc["code"], c.code, 8) || !readText(doc["token"], c.token, TOKEN_MAX) ||
      !readText(doc["pin"], c.fingerprint, 64)) return;
  connection = c;
  alerts = a;
}

void BambuSettings_Reset() {
  Guard lock;
  connection = BambuConnection();
  alerts = BambuAlerts();
  ++revision;  // Settings_ClearAll clears our key in the same namespace.
}

bool BambuSettings_On() { return connection.on; }
BambuAlerts BambuSettings_Alerts() { return alerts; }
uint32_t BambuSettings_Revision() { return revision.load(); }

uint32_t BambuSettings_Copy(BambuConnection& dest) {
  Guard lock;
  dest = connection;
  return revision.load();
}

bool BambuSettings_RememberPin(uint32_t gen, const char* pin) {
  Guard lock;
  if (gen != revision.load() || connection.cloud) return false;
  if (connection.fingerprint.length()) return connection.fingerprint == pin;
  BambuConnection next = connection;
  next.fingerprint = pin;
  if (!store(next, alerts)) return false;
  connection.fingerprint = next.fingerprint;
  return true;
}

void BambuSettings_ToJson(JsonObject out, bool indicators) {
  Guard lock;
  publicJson(out, connection, alerts);
  if (indicators) {
    out["hasCode"] = !connection.code.isEmpty();
    out["hasToken"] = !connection.token.isEmpty();
  }
}

int BambuSettings_Apply(JsonVariantConst patch, bool fromWeb) {
  if (patch.isNull()) return 200;
  if (!patch.is<JsonObjectConst>()) return 400;
  JsonObjectConst o = patch.as<JsonObjectConst>();
  Guard lock;
  BambuConnection c = connection;
  BambuAlerts a = alerts;
  if (!applyPublic(o, c, a)) return 400;
  if (c.serial != connection.serial) { c.code.clear(); c.fingerprint.clear(); }
  if (c.china != connection.china) c.token.clear();
  if (fromWeb) {
    if (o["forget"] == true) { c.code.clear(); c.token.clear(); c.fingerprint.clear(); }
    else if (!readText(o["code"], c.code, 8, true) || !readText(o["token"], c.token, TOKEN_MAX, true))
      return 400;
  }
  if (c.code.length() && c.code.length() != 8) return 400;
  bool reconnect = c.on != connection.on || c.cloud != connection.cloud || c.china != connection.china ||
      c.host != connection.host || c.serial != connection.serial || c.code != connection.code ||
      c.token != connection.token || c.fingerprint != connection.fingerprint;
  if (!reconnect && a.finish == alerts.finish && a.error == alerts.error &&
      a.pause == alerts.pause && a.night == alerts.night) return 200;
  if (!store(c, a)) return 507;
  connection = c;
  alerts = a;
  if (reconnect) ++revision;
  return 200;
}
