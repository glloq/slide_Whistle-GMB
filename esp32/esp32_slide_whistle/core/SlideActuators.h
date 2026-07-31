/*
 * core/SlideActuators.h — concrete ISlideActuator backends.
 *
 *   StepDirSlideActuator     — A4988/DRV8825/TMC step-dir, non-blocking homing
 *   SingleServoSlideActuator — one position servo, mm↔µs calibration
 *   DualServoSlideActuator   — two synchronised servos (same cmd cycle)
 *   DisabledSlideActuator    — no movement, air-only bench testing
 *
 * All share a trapezoidal motion profile integrator so the rest of the engine
 * only ever deals in millimetres. Homing is a state machine with per-phase
 * timeouts — no blocking `while`, no infinite retry loop (corrections #4, #5).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 * NOT TESTED — REQUIRES HARDWARE: real step timing, servo linkage linearity.
 */
#ifndef SWC_CORE_SLIDEACTUATORS_H
#define SWC_CORE_SLIDEACTUATORS_H

#include "ISlideActuator.h"
#include <cmath>

namespace swc {

// ---------------------------------------------------------------------------
// Shared base: trapezoidal profile + timing bookkeeping
// ---------------------------------------------------------------------------
class SlideActuatorBase : public ISlideActuator {
public:
    explicit SlideActuatorBase(IMotionSink* sink) : sink_(sink) {}

    bool begin(const SlideMotionConfig& cfg) override {
        cfg_    = cfg;
        pos_    = 0.0f;
        vel_    = 0.0f;
        target_ = 0.0f;
        lastUs_ = 0;
        haveTime_ = false;
        fault_  = FaultCode::None;
        homed_  = false;
        state_  = MotionState::Idle;
        return validate();
    }

    void update(uint32_t nowUs) override {
        float dt = tickSeconds(nowUs);
        if (state_ == MotionState::EStopped || state_ == MotionState::Fault) return;
        if (state_ == MotionState::Homing) { homingStep(dt, nowUs); applyOutput(); return; }
        integrate(dt);
        applyOutput();
    }

    bool requestPositionMm(float positionMm) override {
        if (state_ == MotionState::EStopped || state_ == MotionState::Fault) return false;
        if (!homed_ && cfg_.type == SlideDriveType::StepDir) return false;
        if (positionMm < cfg_.softMinMm - 1e-3f || positionMm > cfg_.softMaxMm + 1e-3f) {
            // An out-of-range target is a real fault, not a soft no-op: latch
            // Fault, stop, and de-energise the driver so update() halts and
            // isReadyForAir() reports not-ready (review #5 §P0.3). The caller
            // (sequencer) also closes the air on the false return.
            fault_ = FaultCode::TargetOutOfRange;
            state_ = MotionState::Fault;
            vel_ = 0.0f; target_ = pos_;
            if (sink_) sink_->enableDriver(false);
            return false;
        }
        target_ = clampv(positionMm, cfg_.softMinMm, cfg_.softMaxMm);
        state_  = MotionState::Moving;
        return true;
    }

    void stopControlled() override {
        target_ = pos_;
        vel_    = 0.0f;
        if (state_ == MotionState::Moving) state_ = MotionState::Holding;
    }

    void emergencyStop() override {
        vel_ = 0.0f; target_ = pos_;
        state_ = MotionState::EStopped;
        // Preserve the ROOT cause: an e-stop triggered BY an existing fault
        // (Overpressure, HomingTimeout, TargetOutOfRange, …) must not overwrite
        // it — the EStopped state already records the safety action, and fault()
        // should still report why (review #6 §19).
        if (fault_ == FaultCode::None) fault_ = FaultCode::EmergencyStop;
        if (sink_) sink_->enableDriver(false);
    }

    bool isHomed() const override { return homed_; }
    bool isMoving() const override { return state_ == MotionState::Moving; }

    void clearFault() override {
        // Re-arm from E-stop / Fault. Homing state is preserved so a homed
        // servo stays ready; an unhomed stepper simply becomes homable again.
        if (state_ == MotionState::EStopped || state_ == MotionState::Fault) {
            fault_ = FaultCode::None;
            state_ = MotionState::Idle;
            vel_ = 0.0f; target_ = pos_;
        }
    }

    void applyDynamic(const SlideMotionConfig& c) override {
        // Only the safe, dynamic fields — never pins / type / backend (#6).
        cfg_.maxSpeedMmS = c.maxSpeedMmS;
        cfg_.accelMmS2   = c.accelMmS2;
        cfg_.softMinMm   = c.softMinMm;
        cfg_.softMaxMm   = c.softMaxMm;
    }

    bool isReadyForAir() const override {
        if (state_ == MotionState::Fault || state_ == MotionState::EStopped) return false;
        if (!homed_) return false;
        return !isMoving() && std::fabs(pos_ - target_) <= toleranceMm();
    }

    float currentPositionMm() const override { return pos_; }
    float targetPositionMm()  const override { return target_; }
    MotionState state() const override { return state_; }
    FaultCode   fault() const override { return fault_; }

protected:
    virtual void applyOutput() = 0;
    virtual bool validate() { return true; }
    virtual float toleranceMm() const { return 0.2f; }

    float tickSeconds(uint32_t nowUs) {
        if (!haveTime_) { lastUs_ = nowUs; haveTime_ = true; return 0.0f; }
        uint32_t d = elapsed_u32(nowUs, lastUs_);   // rollover-safe
        lastUs_ = nowUs;
        return float(d) * 1e-6f;
    }

    // Trapezoidal 1-DOF profile toward target_.
    void integrate(float dt) {
        if (dt <= 0.0f) return;
        float d = target_ - pos_;
        float a = cfg_.accelMmS2;
        float vmax = cfg_.maxSpeedMmS;
        float dir = d > 0 ? 1.0f : -1.0f;
        float stopDist = (vel_ * vel_) / (2.0f * (a > 1e-6f ? a : 1e-6f));

        if (std::fabs(d) <= toleranceMm() && std::fabs(vel_) < 1e-3f) {
            vel_ = 0.0f;
            if (state_ == MotionState::Moving) state_ = MotionState::Holding;
            return;
        }
        // decelerate if we would overshoot, else accelerate toward target
        if (std::fabs(d) <= stopDist) vel_ -= dir * a * dt;   // brake
        else                          vel_ += dir * a * dt;   // push
        vel_ = clampv(vel_, -vmax, vmax);
        pos_ += vel_ * dt;

        // snap when close and slow
        if (std::fabs(target_ - pos_) <= toleranceMm() && std::fabs(vel_) <= a * dt) {
            pos_ = target_; vel_ = 0.0f;
            if (state_ == MotionState::Moving) state_ = MotionState::Holding;
        }
        pos_ = clampv(pos_, cfg_.softMinMm, cfg_.softMaxMm);
    }

    // Default homing: absolute backends (servo) are homed instantly at 0 mm.
    virtual void homingStep(float /*dt*/, uint32_t /*nowUs*/) {
        pos_ = 0.0f; target_ = 0.0f; vel_ = 0.0f;
        homed_ = true;
        state_ = MotionState::Idle;
    }

    IMotionSink*      sink_ = nullptr;
    SlideMotionConfig cfg_;
    float pos_ = 0, vel_ = 0, target_ = 0;
    uint32_t lastUs_ = 0;
    bool     haveTime_ = false;
    bool     homed_ = false;
    MotionState state_ = MotionState::Uninitialised;
    FaultCode   fault_ = FaultCode::None;
};

// ---------------------------------------------------------------------------
// Disabled — no movement; always ready (air-only testing)
// ---------------------------------------------------------------------------
class DisabledSlideActuator : public SlideActuatorBase {
public:
    explicit DisabledSlideActuator(IMotionSink* sink = nullptr) : SlideActuatorBase(sink) {}
    bool begin(const SlideMotionConfig& cfg) override {
        SlideActuatorBase::begin(cfg);
        homed_ = true; state_ = MotionState::Idle; return true;
    }
    void update(uint32_t) override {}
    // No physical motion, but record the target so telemetry/tests can see the
    // musically-computed position that *would* be commanded.
    bool requestPositionMm(float mm) override { target_ = mm; pos_ = mm; return true; }
    bool requestHoming() override { homed_ = true; state_ = MotionState::Idle; return true; }
    bool isReadyForAir() const override { return state_ != MotionState::EStopped; }
protected:
    void applyOutput() override {}
};

// ---------------------------------------------------------------------------
// Step/Dir stepper with non-blocking homing FSM
// ---------------------------------------------------------------------------
class StepDirSlideActuator : public SlideActuatorBase {
public:
    explicit StepDirSlideActuator(IMotionSink* sink) : SlideActuatorBase(sink) {}

    bool requestHoming() override {
        if (state_ == MotionState::EStopped) return false;
        homed_   = false;
        fault_   = FaultCode::None;
        hphase_  = HPhase::FastSeek;
        phaseStartUs_ = 0; havePhaseTime_ = false;
        state_   = MotionState::Homing;
        if (sink_) sink_->enableDriver(true);
        return true;
    }

protected:
    void applyOutput() override { if (sink_) sink_->writeStepperMm(pos_); }
    float toleranceMm() const override { return 0.5f / cfg_.stepper.stepsPerMm * 4.0f + 0.05f; }

    bool validate() override {
        if (cfg_.stepper.stepsPerMm <= 0.0f) { fault_ = FaultCode::ConfigInvalid; return false; }
        return true;
    }

    enum class HPhase : uint8_t { Idle, FastSeek, BackoffUntilReleased, SlowSeek, MoveToOffset, Done };

    void homingStep(float dt, uint32_t nowUs) override {
        if (!havePhaseTime_) { phaseStartUs_ = nowUs; havePhaseTime_ = true; }
        // per-phase timeout — never spins forever (correction #4/#5)
        if (elapsed_u32(nowUs, phaseStartUs_) > cfg_.stepper.phaseTimeoutMs * 1000u) {
            fault_ = FaultCode::HomingTimeout;
            state_ = MotionState::Fault;
            if (sink_) sink_->enableDriver(false);
            return;
        }
        const float dir = cfg_.stepper.homeTowardZero ? -1.0f : 1.0f;
        const bool  hit = sink_ && sink_->readEndstop(!cfg_.stepper.homeTowardZero);

        switch (hphase_) {
            case HPhase::FastSeek:                        // drive toward the switch
                if (hit) { enterPhase(HPhase::BackoffUntilReleased, nowUs); backoffFrom_ = pos_; }
                else     pos_ += dir * cfg_.stepper.homingFastMmS * dt;
                break;
            case HPhase::BackoffUntilReleased:            // retreat until released + min distance
                pos_ -= dir * cfg_.stepper.homingFastMmS * dt;
                if (!hit && std::fabs(pos_ - backoffFrom_) >= cfg_.stepper.homeBackoffMm)
                    enterPhase(HPhase::SlowSeek, nowUs);
                break;
            case HPhase::SlowSeek:                         // creep back to precise contact
                if (hit) {
                    // Define the mm reference AT the switch: 0 mm when homing to
                    // the min end, travelMm when homing to the max end. Homing to
                    // max previously also set pos_=0, so every later positive
                    // target drove toward the max butée instead of inward (#7 §1).
                    pos_ = cfg_.stepper.homeTowardZero ? 0.0f : cfg_.travelMm;
                    // Realign the executed-step counter to this reference so the
                    // next commanded move starts from here, not from the raw seek
                    // count (#7 §1). No-op on absolute backends.
                    if (sink_) sink_->syncPositionMm(pos_);
                    // Move inward off the switch by the offset (toward the middle
                    // of the travel, i.e. away from whichever butée we hit).
                    const float inward = cfg_.stepper.homeTowardZero ? 1.0f : -1.0f;
                    target_ = pos_ + inward * cfg_.stepper.homeOffsetMm;
                    enterPhase(HPhase::MoveToOffset, nowUs);
                } else {
                    pos_ += dir * cfg_.stepper.homingSlowMmS * dt;
                }
                break;
            case HPhase::MoveToOffset: {                   // real move, not a logical snap
                float d = target_ - pos_;
                float step = cfg_.stepper.homingSlowMmS * dt;
                if (std::fabs(d) <= step) {
                    pos_ = target_; homed_ = true;
                    hphase_ = HPhase::Done; state_ = MotionState::Idle;
                } else {
                    pos_ += (d > 0 ? 1.0f : -1.0f) * step;
                }
                break;
            }
            default: break;
        }
        // Only a GENEROUS seek bound during homing — never the musical soft
        // limits, which would stall the seek before reaching a distant switch.
        const float seekBound = cfg_.travelMm * 2.0f + 20.0f;
        pos_ = clampv(pos_, -seekBound, seekBound);
    }

    void enterPhase(HPhase p, uint32_t nowUs) { hphase_ = p; phaseStartUs_ = nowUs; havePhaseTime_ = true; }

    HPhase   hphase_ = HPhase::Idle;
    uint32_t phaseStartUs_ = 0;
    bool     havePhaseTime_ = false;
    float    backoffFrom_ = 0.0f;
};

// ---------------------------------------------------------------------------
// mm ↔ microseconds servo calibration (piecewise-linear, multi-point)
// ---------------------------------------------------------------------------
inline uint16_t servoMmToUs(const ServoMotionConfig& s, float mm) {
    const ServoCalPoint* c = s.cal;
    uint8_t n = s.calCount < 2 ? 2 : s.calCount;
    float us;
    if (mm <= c[0].mm)        us = c[0].us;
    else if (mm >= c[n-1].mm) us = c[n-1].us;
    else {
        us = c[n-1].us;
        for (uint8_t i = 1; i < n; ++i) {
            if (mm <= c[i].mm) {
                float t = (mm - c[i-1].mm) / (c[i].mm - c[i-1].mm);
                us = lerp(float(c[i-1].us), float(c[i].us), t);
                break;
            }
        }
    }
    float out = us + s.trimUs + s.offsetUs;
    if (s.invert) out = float(s.minUs) + float(s.maxUs) - out;
    return (uint16_t)clampv(out, float(s.minUs), float(s.maxUs));
}

// ---------------------------------------------------------------------------
// Single position servo
// ---------------------------------------------------------------------------
class SingleServoSlideActuator : public SlideActuatorBase {
public:
    explicit SingleServoSlideActuator(IMotionSink* sink) : SlideActuatorBase(sink) {}
    bool requestHoming() override { hphase_ = 0; state_ = MotionState::Homing; return true; }
protected:
    void applyOutput() override {
        if (sink_) sink_->writeServoUs(0, servoMmToUs(cfg_.servoA, pos_));
    }
    void homingStep(float, uint32_t) override {
        pos_ = 0.0f; target_ = 0.0f; vel_ = 0.0f; homed_ = true; state_ = MotionState::Idle;
    }
    int hphase_ = 0;
};

// ---------------------------------------------------------------------------
// Two synchronised servos — both commanded in the same update cycle
// ---------------------------------------------------------------------------
class DualServoSlideActuator : public SlideActuatorBase {
public:
    explicit DualServoSlideActuator(IMotionSink* sink) : SlideActuatorBase(sink) {}
    bool requestHoming() override { state_ = MotionState::Homing; return true; }
    // Open-loop by design when no position feedback is installed.
    bool openLoop() const { return true; }
protected:
    void applyOutput() override {
        if (!sink_) return;
        uint16_t a = servoMmToUs(cfg_.servoA, pos_);
        sink_->writeServoUs(0, a);                       // same cycle as B
        if (cfg_.servoBEnabled) {
            float mmB = pos_;
            if (cfg_.dualMode == DualSyncMode::Opposite) mmB = cfg_.travelMm - pos_;
            uint16_t b = servoMmToUs(cfg_.servoB, mmB);
            sink_->writeServoUs(1, b);
        }
    }
    void homingStep(float, uint32_t) override {
        pos_ = 0.0f; target_ = 0.0f; vel_ = 0.0f; homed_ = true; state_ = MotionState::Idle;
    }
};

} // namespace swc

#endif // SWC_CORE_SLIDEACTUATORS_H
