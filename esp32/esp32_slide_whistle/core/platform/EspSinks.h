/*
 * core/platform/EspSinks.h — ESP32 hardware implementations of the sink
 * boundaries (IMotionSink / IAirSink). This is the ONLY place the control core
 * reaches a GPIO / servo / PWM output.
 *
 * Compiled only under Arduino (guarded), so the native test build is
 * unaffected. Concrete but hardware-untested — do not treat as validated.
 *
 * Status: IMPLEMENTED (structure) · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE
 *
 * Integration note: this file is intentionally NOT yet included by the working
 * esp32_slide_whistle.ino. Wiring the real-time task to own an Instrument and
 * these sinks is the next integration step (see HARDWARE_MATRIX.md).
 */
#ifndef SWC_CORE_ESPSINKS_H
#define SWC_CORE_ESPSINKS_H

#if defined(ARDUINO)

#include <Arduino.h>
#include "../ISlideActuator.h"
#include "../IAirSystem.h"
#include "../PwmOutput.h"

namespace swc {

// ---------------------------------------------------------------------------
// Motion sink: open-loop step generation + servo PWM.
// The core computes the trapezoidal profile; this sink only realises the
// commanded position. Step pulses are emitted as a bounded delta per call so a
// single tick never blocks (a future RMT/timer backend can replace this).
// ---------------------------------------------------------------------------
class EspMotionSink : public IMotionSink {
public:
    void begin(const SlideMotionConfig& cfg) {
        cfg_ = cfg;
        const auto& s = cfg.stepper;
        if (cfg.type == SlideDriveType::StepDir) {
            if (s.stepPin >= 0)   pinMode(s.stepPin, OUTPUT);
            if (s.dirPin >= 0)    pinMode(s.dirPin, OUTPUT);
            if (s.enablePin >= 0) { pinMode(s.enablePin, OUTPUT); enableDriver(false); }
            if (s.endstopMin.pin >= 0)
                pinMode(s.endstopMin.pin, s.endstopMin.internalPullup ? INPUT_PULLUP : INPUT);
            curSteps_ = 0;
        }
        if (cfg.type == SlideDriveType::SingleServo || cfg.type == SlideDriveType::DualServo)
            attachServo(0, cfg.servoA);
        if (cfg.type == SlideDriveType::DualServo)
            attachServo(1, cfg.servoB);
    }

    void writeStepperMm(float mm) override {
        long target = (long)(mm * cfg_.stepper.stepsPerMm);
        long delta  = target - curSteps_;
        if (delta == 0) return;
        bool dir = (delta > 0) ^ cfg_.stepper.invertDir;
        if (cfg_.stepper.dirPin >= 0) digitalWrite(cfg_.stepper.dirPin, dir ? HIGH : LOW);
        long steps = delta > 0 ? delta : -delta;
        if (steps > kMaxStepsPerCall) steps = kMaxStepsPerCall;   // bounded, non-blocking
        for (long i = 0; i < steps; ++i) {
            digitalWrite(cfg_.stepper.stepPin, HIGH);
            delayMicroseconds(3);
            digitalWrite(cfg_.stepper.stepPin, LOW);
            delayMicroseconds(3);
        }
        curSteps_ += (delta > 0 ? steps : -steps);
    }

    void writeServoUs(uint8_t index, uint16_t us) override {
        if (index > 1) return;
        // 50 Hz, 16-bit: duty = us / 20000 * 65535
        servo_[index].writeRaw((uint32_t)((float)us / 20000.0f * servo_[index].maxDuty()));
    }

    void enableDriver(bool on) override {
        if (cfg_.stepper.enablePin < 0) return;
        bool level = cfg_.stepper.enableActiveHigh ? on : !on;
        digitalWrite(cfg_.stepper.enablePin, level ? HIGH : LOW);
    }

    bool readEndstop(bool useMax) override {
        const EndstopConfig& e = useMax ? cfg_.stepper.endstopMax : cfg_.stepper.endstopMin;
        if (e.pin < 0) return false;
        bool raw = digitalRead(e.pin) == HIGH;
        bool triggered = e.activeHigh ? raw : !raw;
        return e.normallyClosed ? !triggered : triggered;
    }

private:
    void attachServo(uint8_t idx, const ServoMotionConfig& s) {
        if (s.backend != PwmBackend::Gpio || s.pin < 0) return;   // PCA9685 backend: TODO
        PwmConfig p; p.pin = s.pin; p.freqHz = s.freqHz; p.resolution = 16;
        servo_[idx].attach(p);
    }
    static constexpr long kMaxStepsPerCall = 8;
    SlideMotionConfig cfg_;
    long      curSteps_ = 0;
    PwmOutput servo_[2];
};

// ---------------------------------------------------------------------------
// Air sink: fan/pump PWM, solenoid GPIO, servo gate/flow/angle, analog sensor.
// ---------------------------------------------------------------------------
class EspAirSink : public IAirSink {
public:
    void setSourceLevel(uint8_t index, float v) override {
        if (index < 3 && source_[index].attached()) source_[index].writeNormalized(v);
    }
    void setGateOpen(bool open) override {
        if (gatePin_ >= 0) digitalWrite(gatePin_, (open == gateActiveHigh_) ? HIGH : LOW);
    }
    void setGatePwm(float v) override { if (gate_.attached()) gate_.writeNormalized(v); }
    void setFlow(float v) override    { if (flow_.attached()) flow_.writeNormalized(v); }
    void setAngle(float v) override   { if (angle_.attached()) angle_.writeNormalized(v); }
    float readSensorRaw() override {
        if (sensorPin_ < 0) return NAN;
        return (float)analogRead(sensorPin_);
    }

    // Wiring helpers (called by the platform bring-up code)
    void configureSourcePwm(uint8_t i, int pin, uint32_t freq) {
        if (i >= 3 || pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = freq;
        source_[i].attach(p);
    }
    void configureSolenoid(int pin, bool activeHigh) {
        gatePin_ = pin; gateActiveHigh_ = activeHigh;
        if (pin >= 0) pinMode(pin, OUTPUT);
    }
    void configureGatePwm(int pin, uint32_t freq) {
        if (pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = freq;
        gate_.attach(p);
    }
    void configureFlow(int pin) {
        if (pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = 50; p.resolution = 16;
        flow_.attach(p);
    }
    void configureAngle(int pin) {
        if (pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = 50; p.resolution = 16;
        angle_.attach(p);
    }
    void configureSensor(int pin) { sensorPin_ = pin; }

private:
    PwmOutput source_[3], gate_, flow_, angle_;
    int  gatePin_ = -1, sensorPin_ = -1;
    bool gateActiveHigh_ = true;
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_ESPSINKS_H
