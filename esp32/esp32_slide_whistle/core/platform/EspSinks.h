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
// EspStepGen — hardware-timer step pulse generator (replaces the old blocking
// delayMicroseconds busy-wait). One shared ESP32 hardware timer fires a single
// ISR at a fixed rate; each registered axis emits at most one STEP edge per tick
// toward its target and maintains an AUTHORITATIVE executed-step counter
// (curSteps_). writeStepperMm() therefore only stores a target and never blocks
// the 1 kHz RT task; the pulse train is produced in the background. (The ISR is
// not yet IRAM-placed — see serviceTick() for the bench work that remains.)
//
// Two ISR ticks make one full pulse (rising then falling), so the per-axis step
// rate is timerHz/2. This is the "RMT/GPTimer" backend the reviews asked for,
// realised with the always-available hw_timer_t API.
//
// Status: IMPLEMENTED · COMPILES in CI against the real arduino-esp32 timer API
// (2.x here; 3.x branch guarded) · the pulse-EMISSION LOGIC is unit-tested via
// the native Arduino stub. NOT HARDWARE-VERIFIED: real ISR cadence, IRAM
// placement of digitalWrite, and actual step output require a bench. Treat as
// experimental until validated on hardware.
// ---------------------------------------------------------------------------
class EspStepGen {
public:
    // Register this axis and (on first use) start the shared timer. pinMode for
    // step/dir is done by the owning sink.
    void begin(int stepPin, int dirPin, bool invertDir) {
        stepPin_ = stepPin; dirPin_ = dirPin; invertDir_ = invertDir;
        curSteps_ = 0; targetSteps_ = 0; stepHigh_ = false;
        if (!registered_ && s_count < kMaxGens) { s_gens[s_count++] = this; registered_ = true; }
        startTimerOnce();
    }
    // Non-blocking: just publish the new target; the ISR walks curSteps_ to it.
    void setTargetSteps(long t) { targetSteps_ = t; }
    // Redefine the reference at a homing contact (no motion implied).
    void syncSteps(long s) { curSteps_ = s; targetSteps_ = s; stepHigh_ = false; }
    long curSteps() const { return curSteps_; }

    // One timer tick for this axis. Rising edge sets DIR + STEP high; the next
    // tick drops STEP and advances the executed counter. Called from the ISR on
    // hardware and directly from the unit test.
    //
    // NOTE (bench work): this is intentionally NOT IRAM_ATTR. Marking it (and the
    // ISR trampoline) IRAM_ATTR triggers the toolchain's "dangerous relocation:
    // l32r" link error because it calls digitalWrite() and touches static state,
    // which live in flash. A hardware-grade build must place the ISR in IRAM AND
    // replace digitalWrite with IRAM-safe GPIO register writes (GPIO.out_w1ts /
    // out_w1tc). Running the ISR from flash works while the flash cache is
    // enabled, which is the case here, but is not robust across flash writes.
    void serviceTick() {
        long tgt = targetSteps_, cur = curSteps_;
        if (cur == tgt || stepPin_ < 0) return;
        if (!stepHigh_) {
            bool dir = (tgt > cur) ^ invertDir_;
            if (dirPin_ >= 0) digitalWrite(dirPin_, dir ? HIGH : LOW);
            digitalWrite(stepPin_, HIGH);
            stepHigh_ = true;
        } else {
            digitalWrite(stepPin_, LOW);
            stepHigh_ = false;
            curSteps_ = cur + ((tgt > cur) ? 1 : -1);
        }
    }

private:
    static constexpr uint8_t  kMaxGens = MAX_INSTRUMENTS;
    static constexpr uint32_t kTickUs  = 25;   // 40 kHz ISR → 20 k steps/s per axis
    inline static EspStepGen* s_gens[kMaxGens] = {nullptr};
    inline static uint8_t     s_count = 0;
    inline static hw_timer_t* s_timer = nullptr;

    static void s_onTimer() {   // see serviceTick() note re: IRAM (bench work)
        for (uint8_t i = 0; i < s_count; ++i) if (s_gens[i]) s_gens[i]->serviceTick();
    }
    static void startTimerOnce() {
        if (s_timer) return;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
        s_timer = timerBegin(1000000);            // 1 MHz tick base (3.x API)
        if (s_timer) { timerAttachInterrupt(s_timer, &s_onTimer); timerAlarm(s_timer, kTickUs, true, 0); }
#else
        s_timer = timerBegin(0, 80, true);        // 80 MHz / 80 = 1 MHz (2.x API)
        if (s_timer) {
            timerAttachInterrupt(s_timer, &s_onTimer, true);
            timerAlarmWrite(s_timer, kTickUs, true);
            timerAlarmEnable(s_timer);
        }
#endif
    }

    int  stepPin_ = -1, dirPin_ = -1;
    bool invertDir_ = false, stepHigh_ = false, registered_ = false;
    volatile long curSteps_ = 0, targetSteps_ = 0;
};

// ---------------------------------------------------------------------------
// Motion sink: hardware-timer step generation + servo PWM.
// The core computes the trapezoidal profile; this sink realises the commanded
// position through EspStepGen (non-blocking) and reports the executed position
// back from the authoritative step counter.
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
            // Configure the max endstop too — homing toward the max end reads it,
            // and continuous supervision needs both inputs driven, not floating
            // (review #7 §3).
            if (s.endstopMax.pin >= 0)
                pinMode(s.endstopMax.pin, s.endstopMax.internalPullup ? INPUT_PULLUP : INPUT);
            stepGen_.begin(s.stepPin, s.dirPin, s.invertDir);   // hardware-timer stepping
        }
        if (cfg.type == SlideDriveType::SingleServo || cfg.type == SlideDriveType::DualServo)
            attachServo(0, cfg.servoA);
        if (cfg.type == SlideDriveType::DualServo)
            attachServo(1, cfg.servoB);
    }

    // Non-blocking: publish the target step count; the hardware-timer ISR walks
    // the executed counter to it. No delayMicroseconds busy-wait in the RT task.
    void writeStepperMm(float mm) override {
        stepGen_.setTargetSteps(lroundf(mm * cfg_.stepper.stepsPerMm));
    }

    // Test hook: on hardware the shared timer ISR services every axis; off-device
    // (no timer) the native backend test drives the pulse logic through here.
    void serviceStepGenTick() { stepGen_.serviceTick(); }

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

    // Re-define the executed-step counter to the mm reference the actuator just
    // established at a homing contact, so the next writeStepperMm() commands zero
    // motion instead of a phantom correction back to the pre-home count
    // (review #8 §1 — the base-class default no-op left curSteps_ untouched).
    void syncPositionMm(float mm) override {
        stepGen_.syncSteps(lroundf(mm * cfg_.stepper.stepsPerMm));
    }

    // Authoritative executed position from the pulses the timer ISR has actually
    // emitted so far. During a fast move the executed counter trails the
    // commanded mm (the ISR is still walking toward the target), so the actuator
    // gates air on this (review #7/#8 §6).
    bool executedPositionMm(float& mm) const override {
        if (cfg_.stepper.stepsPerMm <= 0.0f) return false;
        mm = float(stepGen_.curSteps()) / cfg_.stepper.stepsPerMm;
        return true;
    }

private:
    void attachServo(uint8_t idx, const ServoMotionConfig& s) {
        if (s.backend != PwmBackend::Gpio || s.pin < 0) return;   // PCA9685 backend: TODO
        PwmConfig p; p.pin = s.pin; p.freqHz = s.freqHz; p.resolution = 16;
        servo_[idx].attach(p);
    }
    SlideMotionConfig cfg_;
    EspStepGen stepGen_;
    PwmOutput  servo_[2];
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
        if (solenoidPin_ >= 0) digitalWrite(solenoidPin_, (open == solenoidActiveHigh_) ? HIGH : LOW);
    }
    // Gate PWM: solenoid economiser uses a raw duty; a servo gate uses a µs pulse.
    void setGatePwm(float v) override {
        if (gateServo_.attached())    gateServo_.writeNormalized(v);
        else if (gatePwm_.attached()) gatePwm_.writeNormalized(v);
    }
    void setFlow(float v) override {
        if (flowServo_.attached())    flowServo_.writeNormalized(v);
        else if (flowPwm_.attached()) flowPwm_.writeNormalized(v);
    }
    void setAngle(float v) override { if (angleServo_.attached()) angleServo_.writeNormalized(v); }
    float readSensorRaw() override { return sensorPin_ < 0 ? NAN : (float)analogRead(sensorPin_); }

    // ---- wiring helpers -----------------------------------------------------
    void configureSourcePwm(uint8_t i, int pin, uint32_t freq) {
        if (i >= 3 || pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = freq;
        source_[i].attach(p);
    }
    void configureSolenoid(int pin, bool activeHigh) {
        solenoidPin_ = pin; solenoidActiveHigh_ = activeHigh;
        if (pin < 0) return;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, activeHigh ? LOW : HIGH);   // force CLOSED immediately (#8)
    }
    void configureSolenoidPwm(int pin, uint32_t freq) {
        if (pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = freq;
        gatePwm_.attach(p); gatePwm_.writeRaw(0);     // closed
    }
    void configureGateServo(int pin, uint16_t minUs, uint16_t maxUs) {
        if (pin >= 0) gateServo_.attach(pin, minUs, maxUs);
    }
    void configureFlowServo(int pin, uint16_t minUs, uint16_t maxUs) {
        if (pin >= 0) flowServo_.attach(pin, minUs, maxUs);
    }
    void configureFlowPwm(int pin, uint32_t freq) {
        if (pin < 0) return;
        PwmConfig p; p.pin = pin; p.freqHz = freq;
        flowPwm_.attach(p);
    }
    void configureAngleServo(int pin, uint16_t minUs, uint16_t maxUs) {
        if (pin >= 0) angleServo_.attach(pin, minUs, maxUs);
    }
    void configureSensor(int pin) { sensorPin_ = pin; if (pin >= 0) pinMode(pin, INPUT); }

private:
    PwmOutput   source_[3];
    PwmOutput   gatePwm_, flowPwm_;                  // solenoid PWM / proportional PWM
    ServoOutput gateServo_, flowServo_, angleServo_; // µs-calibrated servos (#13)
    int  solenoidPin_ = -1, sensorPin_ = -1;
    bool solenoidActiveHigh_ = true;
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_ESPSINKS_H
