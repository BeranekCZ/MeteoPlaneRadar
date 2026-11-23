#pragma once
using StaticSemaphore_t = int;
using SemaphoreHandle_t = int*;
constexpr int portMAX_DELAY = -1;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p) { return p; }
inline void xSemaphoreTake(SemaphoreHandle_t, int) {}
inline void xSemaphoreGive(SemaphoreHandle_t) {}
