#pragma once

#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>

// Each PNGdec instance needs about 45 kB. Keep its workspace in PSRAM so
// cloud MQTT and the radar's HTTPS downloads can use internal RAM together.
inline bool RadarPng_Ensure(PNG*& decoder) {
  if (!decoder) {
    void* mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    decoder = new (mem) PNG();
  }
  return true;
}
