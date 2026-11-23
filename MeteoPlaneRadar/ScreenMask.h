#pragma once
#include <ArduinoJson.h>
#include "Config.h"

struct ScreenSettingKey { const char* key; uint8_t idx; };
static constexpr ScreenSettingKey SCREEN_DATA_KEYS[] = {
  {"clock", SCREEN_CLOCK_I}, {"planes", SCREEN_PLANES_I}, {"meteo", SCREEN_METEO_I},
  {"forecast", SCREEN_FORECAST_I}, {"price", SCREEN_PRICE_I}, {"mix", SCREEN_MIX_I},
  {"print", SCREEN_PRINT_I}, {"bambu", SCREEN_BAMBU_I}, {"fx", SCREEN_FX_I}, {"crypto", SCREEN_CRYPTO_I}
};

inline uint16_t ScreenMask_Merge(uint16_t mask, JsonObjectConst changes) {
  for (const auto& entry : SCREEN_DATA_KEYS) {
    JsonVariantConst value = changes[entry.key];
    if (value.isNull()) continue;
    const uint16_t bit = 1u << entry.idx;
    if (value.as<bool>()) mask |= bit;
    else mask &= ~bit;
  }
  return mask;
}
