// Minimal Arduino.h stub for NATIVELY exercising the real EspMotionSink /
// EspAirSink backends (review #8 §1 asked for a test of the actual backend, not
// only a fake IMotionSink). It records enough to observe behaviour:
//   - g_stepPulses counts rising edges on the pin registered as g_stepPin,
//     so a test can prove writeStepperMm() emits zero steps after a homing sync.
#pragma once
#include <cstdint>
#include <cmath>

#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
#define bit(b) (1UL << (b))

// --- observation hooks used by the test ---
inline int  g_stepPin    = -1;   // the pin whose rising edges we count
inline long g_stepPulses = 0;    // number of HIGH writes to g_stepPin
inline int  g_endstopLevel = 0;  // value returned by digitalRead()

inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int val) { if (pin == g_stepPin && val == HIGH) ++g_stepPulses; }
inline int  digitalRead(int) { return g_endstopLevel; }
inline void delayMicroseconds(unsigned) {}
inline int  analogRead(int) { return 0; }
inline bool ledcAttach(uint8_t, uint32_t, uint8_t) { return true; }
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}
inline void ledcDetach(uint8_t) {}
inline void ledcDetachPin(uint8_t) {}
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
