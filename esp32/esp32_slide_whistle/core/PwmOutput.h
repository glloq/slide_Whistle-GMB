/*
 * core/PwmOutput.h — version-independent PWM/LEDC wrapper (Section 16).
 *
 * Arduino-ESP32 2.x and 3.x expose incompatible LEDC APIs
 * (ledcSetup/ledcAttachPin/ledcWrite by channel  vs  ledcAttach/ledcWrite by
 * pin). This wrapper isolates that difference behind one small class so the
 * rest of the firmware never mixes the two APIs.
 *
 * On a native (non-Arduino) build it compiles to a recording stub, which keeps
 * the header syntactically checked by the native test build.
 *
 * Status: IMPLEMENTED (wrapper) · NOT TESTED — REQUIRES HARDWARE (real LEDC)
 */
#ifndef SWC_CORE_PWMOUTPUT_H
#define SWC_CORE_PWMOUTPUT_H

#include <cstdint>

namespace swc {

struct PwmConfig {
    int      pin        = -1;
    uint32_t freqHz     = 5000;
    uint8_t  resolution = 12;    // bits
    uint8_t  channel    = 0;     // used by the 2.x API only
    bool     activeHigh = true;
};

class PwmOutput {
public:
    bool attach(const PwmConfig& c) {
        cfg_ = c;
        if (c.pin < 0) return false;
        maxDuty_ = (1u << c.resolution) - 1u;
#if defined(ARDUINO)
    #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
        attached_ = ledcAttach((uint8_t)c.pin, c.freqHz, c.resolution);
    #else
        ledcSetup(c.channel, c.freqHz, c.resolution);
        ledcAttachPin((uint8_t)c.pin, c.channel);
        attached_ = true;
    #endif
#else
        attached_ = true;   // native stub
#endif
        writeRaw(0);
        return attached_;
    }

    // value in [0,1]; respects activeHigh polarity
    void writeNormalized(float value) {
        if (value < 0.f) value = 0.f; else if (value > 1.f) value = 1.f;
        writeRaw((uint32_t)(value * maxDuty_ + 0.5f));
    }

    void writeRaw(uint32_t duty) {
        if (!attached_) return;
        if (duty > maxDuty_) duty = maxDuty_;
        uint32_t out = cfg_.activeHigh ? duty : (maxDuty_ - duty);
        lastDuty_ = out;
#if defined(ARDUINO)
    #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite((uint8_t)cfg_.pin, out);
    #else
        ledcWrite(cfg_.channel, out);
    #endif
#endif
    }

    void detach() {
#if defined(ARDUINO)
    #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
        if (attached_) ledcDetach((uint8_t)cfg_.pin);
    #else
        if (attached_) ledcDetachPin((uint8_t)cfg_.pin);
    #endif
#endif
        attached_ = false;
    }

    uint32_t maxDuty() const { return maxDuty_; }
    uint32_t lastDuty() const { return lastDuty_; }   // for tests/telemetry
    bool attached() const { return attached_; }

private:
    PwmConfig cfg_;
    uint32_t  maxDuty_ = 0, lastDuty_ = 0;
    bool      attached_ = false;
};

// Servo-specific output: a 50 Hz PWM whose duty encodes a 1–2 ms pulse. A
// servo valve / flow / angle must NOT be driven with a 0–100 % duty (that is a
// 0–20 ms pulse) — writeNormalized() here interpolates within [minUs, maxUs]
// so 0..1 maps to the calibrated pulse window (fixes review item #13).
class ServoOutput {
public:
    bool attach(int pin, uint16_t minUs = 1000, uint16_t maxUs = 2000,
                uint16_t freqHz = 50, uint8_t resolution = 16) {
        minUs_ = minUs; maxUs_ = maxUs; periodUs_ = 1000000u / (freqHz ? freqHz : 50);
        PwmConfig c; c.pin = pin; c.freqHz = freqHz; c.resolution = resolution; c.activeHigh = true;
        return pwm_.attach(c);
    }
    void writeMicroseconds(uint16_t us) {
        if (us < minUs_) us = minUs_; else if (us > maxUs_) us = maxUs_;
        uint32_t duty = (uint32_t)((float)us / (float)periodUs_ * pwm_.maxDuty() + 0.5f);
        pwm_.writeRaw(duty);
        lastUs_ = us;
    }
    void writeNormalized(float v) {
        if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
        writeMicroseconds((uint16_t)(minUs_ + v * (maxUs_ - minUs_) + 0.5f));
    }
    void detach() { pwm_.detach(); }
    bool attached() const { return pwm_.attached(); }
    uint16_t lastUs() const { return lastUs_; }
private:
    PwmOutput pwm_;
    uint16_t minUs_ = 1000, maxUs_ = 2000, lastUs_ = 1500;
    uint32_t periodUs_ = 20000;
};

} // namespace swc

#endif // SWC_CORE_PWMOUTPUT_H
