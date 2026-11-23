#include "BambuWatch.h"
#include "BambuParse.h"
#include "BambuSettings.h"
#include "PrintHistory.h"
#include "PrintNotices.h"
#include "NetSink.h"
#include "WebConfig.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <freertos/semphr.h>
#include <time.h>
#include <algorithm>

extern const uint8_t caStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t caEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr size_t PROFILE_MAX = 16384;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex;
TaskHandle_t worker;
PrintInfo snapshot;
PrintHistory history;
uint32_t historyRevision;
uint32_t seenRevision, dataRevision;

bool current(uint32_t rev) {
  return BambuSettings_Revision() == rev && WiFi.status() == WL_CONNECTED && !WebConfig_UpdateBusy();
}

// Both the display loop and the worker can observe a settings change first.
// The first observer discards the old printer's samples and pending notices.
void resetSnapshot(uint32_t rev, PrintState state) {
  if (dataRevision == rev) return;
  snapshot = PrintInfo();
  snapshot.state = state;
  history.clear();
  ++historyRevision;
  PrintNotices_Clear(SCREEN_BAMBU_I);
  dataRevision = rev;
}

void publish(uint32_t rev, const PrintInfo& info, BambuChange change = {}) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (rev == BambuSettings_Revision()) {
    resetSnapshot(rev, info.state);
    snapshot = info;
    if (change.newJob) {
      history.clear();
      ++historyRevision;
    }
    if (PrintState_Online(info.state) && history.add(info, millis())) ++historyRevision;
    PrintNotices_Post(SCREEN_BAMBU_I, change.event, info);
  }
  xSemaphoreGive(mutex);
}

void status(uint32_t rev, PrintState state, uint32_t retry = 0) {
  PrintInfo info;
  info.state = state;
  info.nextPollMs = retry;
  info.updatedMs = millis();
  publish(rev, info);
}

// PubSubClient can initiate TCP on its own. Only the TLS session checked
// below may send an MQTT CONNECT (which contains the password).
class BambuSocket : public WiFiClientSecure {
public:
  uint32_t deadline = 0, revision = 0;
  void beginPacket() { reading = false; deadline = millis() + 8000; }
  int connect(const char*, uint16_t) override { return 0; }
  int connect(IPAddress, uint16_t) override { return 0; }
  int available() override {
    const uint32_t now = millis();
    if (int32_t(now - deadline) >= 0 || (now - lastCheck >= 20 && !current(revision))) {
      if (fd() >= 0) stop();
      return 0;
    }
    if (now - lastCheck >= 20) lastCheck = now;
    if (pos < count) return count - pos;
    const int n = WiFiClientSecure::available();
    // PubSubClient discards an incomplete header after its three-second
    // byte timeout. Close this stream first so it cannot resume mid-packet.
    if (!n && reading && now - lastByte >= 2500 && fd() >= 0) stop();
    return n;
  }
  // PubSubClient reads one byte at a time. Drain TLS records in blocks.
  int read() override {
    if (pos == count) {
      pos = 0;
      count = 0;
      const int n = WiFiClientSecure::read(cache, sizeof(cache));
      if (n <= 0) return -1;
      count = n;
    }
    reading = true;
    lastByte = millis();
    return cache[pos++];
  }
  bool open(const char* host) {
    setConnectionTimeout(4000);
    return WiFiClientSecure::connect(host, BAMBU_MQTT_PORT) == 1;
  }
private:
  bool reading = false;
  uint32_t lastCheck = 0, lastByte = 0;
  uint8_t cache[512];
  int pos = 0, count = 0;
};

class ReportStream : public Stream {
public:
  explicit ReportStream(uint8_t* buf) : payload{buf, BAMBU_REPORT_MAX} {}
  BambuPayload payload;
  size_t write(uint8_t c) override { return payload.append(&c, 1); }
  size_t write(const uint8_t* p, size_t n) override { return payload.append(p, n); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

String accountName(JsonVariantConst v) {
  String id;
  if (v.is<uint64_t>()) { char b[24]; snprintf(b, sizeof(b), "%llu", v.as<unsigned long long>()); id = b; }
  else if (v.is<const char*>()) id = v.as<const char*>();
  if (id.startsWith("u_")) id.remove(0, 2);
  if (!id.length() || id.length() > 24) return {};
  for (size_t i = 0; i < id.length(); ++i) if (id[i] < '0' || id[i] > '9') return {};
  return "u_" + id;
}

String userFromToken(const String& token, uint8_t* buf) {
  const int first = token.indexOf('.');
  const int end = token.indexOf('.', first + 1);
  if (first < 0 || end <= first+1) return {};
  String part = token.substring(first+1, end);
  part.replace('-', '+'); part.replace('_', '/');
  while (part.length() % 4) part += '=';
  size_t len = 0;
  if (mbedtls_base64_decode(buf, PROFILE_MAX, &len, (const uint8_t*)part.c_str(), part.length())) return {};
  JsonDocument doc;
  if (deserializeJson(doc, buf, len)) return {};
  for (const char* key : {"username", "uid", "user_id", "sub"}) {
    String name = accountName(doc[key]);
    if (name.length()) return name;
  }
  return {};
}

PrintState fetchAccount(const BambuConnection& c, uint32_t rev, uint8_t* buf, String& user) {
  esp_http_client_config_t config{};
  config.url = c.china ? "https://api.bambulab.cn/v1/user-service/my/profile"
                       : "https://api.bambulab.com/v1/user-service/my/profile";
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 4000;
  config.disable_auto_redirect = true;
  esp_http_client_handle_t http = esp_http_client_init(&config);
  if (!http) return PS_OFFLINE;
  const String auth = "Bearer " + c.token;
  esp_http_client_set_header(http, "Authorization", auth.c_str());
  esp_http_client_set_header(http, "Accept", "application/json");
  PrintState result = PS_OFFLINE;
  if (esp_http_client_open(http, 0) == ESP_OK) {
    int64_t announced = esp_http_client_fetch_headers(http);
    const int code = esp_http_client_get_status_code(http);
    if (code == 401 || code == 403) result = PS_AUTH;
    if (code == 200 && announced <= (int64_t)PROFILE_MAX) {
      size_t total = 0;
      const uint32_t start = millis();
      while (total < PROFILE_MAX && current(rev) && millis() - start < 8000) {
        int n = esp_http_client_read(http, (char*)buf + total, PROFILE_MAX - total);
        if (n <= 0) break;
        total += n;
        if (esp_http_client_is_complete_data_received(http)) break;
      }
      JsonDocument filter, doc;
      filter["uidStr"] = true;
      filter["uid"] = true;
      if (esp_http_client_is_complete_data_received(http) &&
          !deserializeJson(doc, buf, total, DeserializationOption::Filter(filter))) {
        user = accountName(doc["uidStr"]);
        if (!user.length()) user = accountName(doc["uid"]);
      }
      result = user.length() ? PS_IDLE : PS_BADREPLY;
    }
  }
  esp_http_client_cleanup(http);
  return result;
}

PrintState session(BambuConnection& c, uint32_t rev, BambuTelemetry& last, String& user, uint8_t* buf, bool& received) {
  if (!Net_HeapOk("BAMBU")) return PS_OFFLINE;
  if (c.cloud && time(nullptr) < 1700000000) return PS_UNKNOWN;
  if (c.cloud && !user.length()) {
    user = userFromToken(c.token, buf);
    if (!user.length()) {
      PrintState result = fetchAccount(c, rev, buf, user);
      if (result != PS_IDLE || !current(rev)) return result == PS_IDLE ? PS_UNKNOWN : result;
    }
  }
  const char* host = c.cloud ? (c.china ? "cn.mqtt.bambulab.com" : "us.mqtt.bambulab.com") : c.host.c_str();
  BambuSocket tls;
  tls.revision = rev;
  tls.deadline = millis() + 16000;
  tls.setHandshakeTimeout(NET_TLS_HANDSHAKE_S);
  if (c.cloud) tls.setCACertBundle(caStart, size_t(caEnd - caStart));
  else tls.setInsecure();  // first-use pairing, then compare the leaf fingerprint before MQTT
  if (!tls.open(host)) return PS_OFFLINE;
  char pin[65]{};
  if (!c.cloud) {
    uint8_t hash[32];
    if (!tls.getFingerprintSHA256(hash)) return PS_CERT_CHANGED;
    for (size_t i = 0; i < sizeof(hash); ++i) snprintf(pin + 2*i, 3, "%02x", hash[i]);
    if (c.fingerprint.length() && c.fingerprint != pin) return PS_CERT_CHANGED;
  }
  if (!current(rev)) return PS_UNKNOWN;
  ReportStream rx(buf);
  PubSubClient mqtt(tls);
  mqtt.setServer(host, BAMBU_MQTT_PORT);
  mqtt.setStream(rx);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);
  if (!mqtt.setBufferSize(c.cloud ? c.token.length() + 160 : 384)) return PS_OFFLINE;
  char clientId[32], report[64], request[64];
  snprintf(clientId, sizeof(clientId), "mpr-%012llx", (unsigned long long)ESP.getEfuseMac());
  snprintf(report, sizeof(report), "device/%s/report", c.serial.c_str());
  snprintf(request, sizeof(request), "device/%s/request", c.serial.c_str());
  if (!mqtt.connect(clientId, c.cloud ? user.c_str() : "bblp", c.cloud ? c.token.c_str() : c.code.c_str())) {
    const int rc = mqtt.state();
    return rc == MQTT_CONNECT_BAD_CREDENTIALS || rc == MQTT_CONNECT_UNAUTHORIZED ? PS_AUTH : PS_OFFLINE;
  }
  if (!c.cloud && !c.fingerprint.length()) {
    if (!BambuSettings_RememberPin(rev, pin)) return PS_BADREPLY;
    c.fingerprint = pin;
  }
  if (!mqtt.subscribe(report)) return PS_OFFLINE;
  uint32_t fresh = millis(), requested = fresh, lowHeapSince = 0;
  uint32_t lastHeapCheck = fresh - BAMBU_HEAP_CHECK_MS;
  bool needState = true, stale = false, delivered = false;
  mqtt.setCallback([&](char* topic, uint8_t*, unsigned int) {
    delivered = true;
    if (strcmp(topic, report) || rx.payload.overflow || !current(rev)) return;
    BambuChange change;
    if (!BambuParse_Report(buf, rx.payload.size, last, change)) return;
    // A temperature delta on reconnect cannot make an old job live again.
    if (last.haveState && (!needState || change.reportedState)) {
      needState = false; stale = false; received = true; fresh = millis();
      last.info.updatedMs = fresh;
      change.newJob = last.resetHistory;
      last.resetHistory = false;
      publish(rev, last.info, change);
    }
  });
  static const char pushall[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
  if (!mqtt.publish(request, pushall)) return PS_OFFLINE;
  status(rev, PS_NO_REPORT);
  while (current(rev) && mqtt.connected()) {
    tls.beginPacket();
    rx.payload.reset();
    delivered = false;
    if (!mqtt.loop()) break;
    if (rx.payload.size && !delivered) break;  // incomplete frame, reconnect on a clean boundary
    const uint32_t now = millis();
    const uint32_t limit = PrintState_Active(last.info.state) ? BAMBU_ACTIVE_STALE_MS : BAMBU_IDLE_STALE_MS;
    if (!stale && !needState && now - fresh >= limit) {
      status(rev, PS_NO_REPORT);
      stale = needState = true;
    }
    if ((needState || !PrintState_Active(last.info.state)) &&
        now - requested >= (c.cloud ? BAMBU_CLOUD_REFRESH_MS : BAMBU_LAN_REFRESH_MS)) {
      if (!mqtt.publish(request, pushall)) break;
      requested = now;
    }
    // Allow short heap dips during a radar request. A sustained shortage
    // releases this TLS session so foreground HTTPS can make progress.
    if (now - lastHeapCheck >= BAMBU_HEAP_CHECK_MS) {
      lastHeapCheck = now;
      const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (freeHeap >= NET_MIN_HEAP) lowHeapSince = 0;
      else if (!lowHeapSince) lowHeapSince = now;
      else if (now - lowHeapSince > 5000) {
        Serial.printf("BAMBU: malo volne pameti (%u B < %u B), odpojuji MQTT\n",
                      (unsigned)freeHeap, (unsigned)NET_MIN_HEAP);
        break;
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
  }
  return PS_OFFLINE;
}

void monitor(void*) {
  uint32_t gen = 0;
  BambuConnection c;
  BambuTelemetry last;
  String user;
  unsigned failures = 0;
  for (;;) {
    if (gen != BambuSettings_Revision()) {
      gen = BambuSettings_Copy(c);
      last = BambuTelemetry();
      user.clear(); failures = 0;
    }
    if (!c.on || !c.serial.length() || (!c.cloud && !c.host.length())) {
      status(gen, PS_DISABLED);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (!(c.cloud ? c.token : c.code).length()) {
      status(gen, PS_AUTH);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (!current(gen)) {
      status(gen, PS_OFFLINE);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    auto* buf = (uint8_t*)heap_caps_malloc(BAMBU_REPORT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool received = false;
    PrintState result = buf ? session(c, gen, last, user, buf, received) : PS_OFFLINE;
    heap_caps_free(buf);
    if (BambuSettings_Revision() != gen) continue;
    if (received) failures = 0;
    if (failures < 6) ++failures;
    uint32_t delayMs = std::min<uint32_t>(300000, 5000UL << failures);
    if (result == PS_AUTH || result == PS_CERT_CHANGED) delayMs = 300000;
    if (!current(gen)) delayMs = 1000;
    status(gen, result, millis() + delayMs);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delayMs));
  }
}
}

void BambuWatch_Begin() {
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  BambuWatch_Tick();
}
void BambuWatch_Tick() {
  const uint32_t gen = BambuSettings_Revision();
  if (gen == seenRevision) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  resetSnapshot(gen, BambuSettings_On() ? PS_UNKNOWN : PS_DISABLED);
  xSemaphoreGive(mutex);
  if (!worker && BambuSettings_On()) {
    if (xTaskCreatePinnedToCore(monitor, "bambu", 10240, nullptr, 1, &worker, 0) != pdPASS) return;
  } else if (worker) xTaskNotifyGive(worker);
  seenRevision = gen;
}
bool BambuWatch_Get(PrintInfo* out, uint32_t* graphRevision) {
  if (!mutex || !out) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  *out = snapshot;
  if (graphRevision) *graphRevision = historyRevision;
  xSemaphoreGive(mutex);
  return true;
}
int BambuWatch_History(int16_t* noz, int16_t* nozT, int16_t* bed, int16_t* bedT, uint16_t* age, int max) {
  if (!mutex) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  int n = history.copy(noz, nozT, bed, bedT, age, max, millis());
  xSemaphoreGive(mutex);
  return n;
}
