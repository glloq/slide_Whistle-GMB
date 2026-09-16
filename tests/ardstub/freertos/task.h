#pragma once
#include <freertos/FreeRTOS.h>
typedef void* TaskHandle_t;
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t,
                                          void*, unsigned, TaskHandle_t*, int) { return pdPASS; }
inline TickType_t xTaskGetTickCount() { return 0; }
inline void vTaskDelayUntil(TickType_t*, TickType_t) {}
