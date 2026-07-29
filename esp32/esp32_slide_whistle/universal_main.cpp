/*
 * universal_main.cpp — entry point for the universal slide-whistle firmware.
 *
 * Built only by the `esp32-universal` PlatformIO env (see platformio.ini),
 * which excludes the legacy esp32_slide_whistle.ino so the two never collide.
 * All the behaviour lives in the tested core + the guarded platform layer.
 *
 * Status: EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE
 */
#include "core/platform/MainApp.h"

#if defined(ARDUINO)
static swc::MainApp g_app;
void setup() { g_app.setup(); }
void loop()  { g_app.loop(); }
#endif
