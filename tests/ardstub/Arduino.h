/*
 * tests/ardstub/Arduino.h — Arduino-ESP32 API surface stub.
 *
 * Just enough of arduino-esp32 to SYNTAX-CHECK the guarded platform layer
 * (core/platform headers) off-device: the real toolchain is only available in the
 * PlatformIO CI jobs, and waiting for those to catch a typo in MainApp.h is a
 * slow feedback loop. Nothing here executes — the checker runs -fsyntax-only.
 */
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

// arduino-esp32's Arduino.h pulls the FreeRTOS headers in; mirror that so the
// platform layer sees the same symbols it does on the real toolchain.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
#define bit(b) (1UL << (b))
#define IRAM_ATTR
#define F(s) (s)
#define PROGMEM

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int  digitalRead(int) { return 0; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned) {}
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }
inline int  analogRead(int) { return 0; }
inline uint32_t esp_random() { return 0; }

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
inline void portENTER_CRITICAL_ISR(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL_ISR(portMUX_TYPE*) {}

struct hw_timer_t;
inline hw_timer_t* timerBegin(unsigned char, unsigned short, bool) { return nullptr; }
inline void timerAttachInterrupt(hw_timer_t*, void (*)(), bool) {}
inline void timerAlarmWrite(hw_timer_t*, unsigned long long, bool) {}
inline void timerAlarmEnable(hw_timer_t*) {}
inline hw_timer_t* timerBegin(unsigned int) { return nullptr; }
inline void timerAttachInterrupt(hw_timer_t*, void (*)()) {}
inline void timerAlarm(hw_timer_t*, unsigned long long, bool, unsigned long long) {}

// --- String ----------------------------------------------------------------
class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}
    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool startsWith(const String& p) const { return s_.rfind(p.s_, 0) == 0; }
    String& operator=(const char* s) { s_ = s ? s : ""; return *this; }
    bool operator==(const String& o) const { return s_ == o.s_; }
private:
    std::string s_;
};

// --- Print / Serial ---------------------------------------------------------
class Print {
public:
    void print(const char*) {}
    void println(const char*) {}
    void println() {}
    void printf(const char*, ...) {}
    size_t write(const uint8_t*, size_t) { return 0; }
    size_t write(uint8_t) { return 0; }
};

#define SERIAL_8N1 0x800001c

class HardwareSerial : public Print {
public:
    explicit HardwareSerial(int = 0) {}
    void begin(unsigned long) {}
    void begin(unsigned long, uint32_t, int8_t = -1, int8_t = -1) {}
    void end() {}
    int  available() { return 0; }
    int  read() { return -1; }
    void flush() {}
};

extern HardwareSerial Serial;

// --- ESP --------------------------------------------------------------------
struct EspClass {
    void restart() {}
    uint64_t getEfuseMac() { return 0; }
    uint32_t getFreeHeap() { return 0; }
};
extern EspClass ESP;
