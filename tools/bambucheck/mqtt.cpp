// Exercise PubSubClient 2.8 framing with its actual Stream interface and a
// deterministic, in-memory broker. No printer or network credentials are used.
#include <PubSubClient.h>
#include "BambuParse.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

extern "C" uint32_t millis() { static uint32_t tick = 0; return ++tick; }

class Broker : public Client {
public:
  std::vector<uint8_t> in, out;
  size_t pos = 0;
  bool open = true;
  int connect(IPAddress, uint16_t) override { return 0; }
  int connect(const char*, uint16_t) override { return 0; }
  size_t write(uint8_t b) override { out.push_back(b); return 1; }
  size_t write(const uint8_t* ptr, size_t n) override { out.insert(out.end(),ptr,ptr+n);return n; }
  int available() override { return static_cast<int>(in.size()-pos); }
  int read() override { return available() ? in[pos++] : -1; }
  int read(uint8_t* p, size_t n) override {
    n=std::min(n, in.size()-pos);
    std::copy_n(in.data()+pos,n,p);pos+=n;return int(n);
  }
  int peek() override { return available()?in[pos]:-1; }
  void flush() override {}
  void stop() override { open=false; }
  uint8_t connected() override { return open; }
  operator bool() override { return open; }
  void report(const std::string& topic, const std::string& payload) {
    size_t len=2+topic.size()+payload.size();
    in.push_back(0x30);
    do {
      const uint8_t digit = len%128; len/=128;
      in.push_back(digit | (len ? 0x80 : 0));
    } while(len);
    in.push_back(uint8_t(topic.size() >> 8));in.push_back(uint8_t(topic.size()));
    in.insert(in.end(),topic.begin(),topic.end());
    in.insert(in.end(),payload.begin(),payload.end());
  }
};

static uint8_t storage[BAMBU_REPORT_MAX];
class ReportSink : public Stream {
public:
  BambuPayload payload{storage,sizeof(storage)};
  size_t write(uint8_t b) override { return payload.append(&b,1); }
};
static ReportSink sink;
static BambuTelemetry telemetry;
static unsigned reports;
static const char* topic = "device/TEST42/report";
static void accept(char* name, uint8_t*, unsigned int) {
  if (strcmp(name,topic) != 0 || sink.payload.overflow) return;
  BambuChange change;
  if (BambuParse_Report(storage,sink.payload.size,telemetry,change)) ++reports;
}

int main() {
  Broker broker;
  PubSubClient mqtt(broker);
  mqtt.setServer("test",8883);
  assert(mqtt.setBufferSize(384));
  mqtt.setStream(sink);mqtt.setCallback(accept);mqtt.setKeepAlive(600);mqtt.setSocketTimeout(1);
  broker.in={0x20,2,0,0};  // successful CONNACK
  assert(mqtt.connect("monitor","bblp","testcode") && mqtt.subscribe(topic));
  const std::string prefix=R"({"print":{"gcode_state":"RUNNING","mc_percent":42,"unused":")";
  const std::string large=prefix+std::string(38500,'x')+"\"}}";
  broker.report(topic,large);
  assert(mqtt.loop() && reports==1 && telemetry.info.pct==42 && sink.payload.size==large.size());
  sink.payload.reset();
  broker.report("device/OTHER/report", R"({"print":{"mc_percent":11}})");
  assert(mqtt.loop() && reports==1 && telemetry.info.pct==42);
  sink.payload.reset();
  broker.report(topic, prefix+std::string(BAMBU_REPORT_MAX,'x')+"\"}}");
  assert(mqtt.loop() && reports==1 && sink.payload.overflow);
  sink.payload.reset();
  broker.report(topic,R"({"print":{"mc_percent":43}})");
  assert(mqtt.loop() && reports==2 && telemetry.info.pct==43);
  sink.payload.reset();
  broker.report(topic,large);
  broker.in.resize(broker.in.size()-5);  // wire truncation before the frame ends
  mqtt.loop();
  assert(reports==2 && sink.payload.size>0 && telemetry.info.pct==43);
  std::cout<<"MQTT framing, large, oversized, wrong-topic and truncated reports passed\n";
}
