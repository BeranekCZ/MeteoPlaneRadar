#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

struct BambuConnection {
  bool on = false, cloud = false, china = false;
  String host, serial, code, token, fingerprint;
};

struct BambuAlerts {
  bool finish = true, error = true, pause = true;
  uint8_t night = 0;
};

void BambuSettings_Begin();
void BambuSettings_Reset();
bool BambuSettings_On();                         // loop task
BambuAlerts BambuSettings_Alerts();               // loop task
uint32_t BambuSettings_Revision();
uint32_t BambuSettings_Copy(BambuConnection& c);  // network task, consistent snapshot
bool BambuSettings_RememberPin(uint32_t revision, const char* fingerprint);
void BambuSettings_ToJson(JsonObject out, bool indicators);
// Returns an HTTP status. Imports accept only public settings.
int BambuSettings_Apply(JsonVariantConst patch, bool fromWeb);
