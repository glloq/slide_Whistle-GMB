/*
 * core/platform/EspEntropy.h — hardware RNG for AuthManager / ApiRouter.
 * Guarded; native builds use the deterministic test entropy instead.
 * Status: IMPLEMENTED · NOT TESTED — REQUIRES HARDWARE
 */
#ifndef SWC_CORE_ESPENTROPY_H
#define SWC_CORE_ESPENTROPY_H
#if defined(ARDUINO)
#include <esp_system.h>
#include "../ApiRouter.h"
namespace swc {
struct EspEntropy : IEntropy { uint32_t next() override { return esp_random(); } };
} // namespace swc
#endif // ARDUINO
#endif // SWC_CORE_ESPENTROPY_H
