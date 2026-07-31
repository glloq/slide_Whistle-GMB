/*
 * core/AirSystem.h — composable air blocks + the AirSystem that wires them.
 *
 * Concrete blocks (each an interface implementation, freely combinable):
 *   Sources : ExternalPassive, Fan, PumpDirect, PumpTank
 *   Gates   : NoGate, SolenoidSimple, SolenoidPwm, ServoValve (+diverter/flow-as-valve)
 *   Flow    : FlowController (servo/pwm, velocity curve, slew, min<=nominal<=max)
 *   Angle   : AngleController (optional jet-angle servo)
 *   Sensor  : AirSensor (range map, filter, hysteresis, stale/out-of-range)
 *   Safety  : AirSafetyController (valve/pump timeouts, overpressure, stale sensor)
 *
 * AirSystem owns every concrete block as a member (no heap on the hot path) and
 * selects the active ones from config, then drives the Note On/Off timeline
 * (Section 7). A deferred command can never re-open the air after panic.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 * NOT TESTED — REQUIRES HARDWARE: real pump/tank dynamics, sensor calibration.
 */
#ifndef SWC_CORE_AIRSYSTEM_H
#define SWC_CORE_AIRSYSTEM_H

#include "IAirSystem.h"
#include <cmath>

namespace swc {

// ---------------------------------------------------------------------------
// Configuration for every block (only the relevant parts are consulted)
// ---------------------------------------------------------------------------
struct SourceConfig {
    AirSourceType type = AirSourceType::ExternalPassive;
    int8_t   pin[MAX_PUMPS] = {-1, -1, -1};  // fan pin uses pin[0]; pumps use pin[0..n]
    uint8_t  pumpCount = 1;              // 1..3
    float    idle01    = 0.0f;           // waiting speed between notes
    float    min01     = 0.15f;          // minimum drive that produces air
    float    max01     = 1.0f;
    float    startBoost01 = 0.0f;        // kick to overcome stiction
    uint32_t spinUpMs  = 150;            // fan/pump startup to "ready"
    uint32_t stopDelayMs = 400;          // keep running briefly after a note
    uint32_t cascadeDelayMs = 120;       // stagger multi-pump start (inrush)
    // tank regulation
    TankRegulationMode tankMode = TankRegulationMode::Pressure;
    bool     tankPwm    = true;          // PID (pwm) vs hysteresis (on/off)
    float    target     = 60.0f;         // physical regulation target
    float    pidKp      = 0.02f;         // PWM drive per unit of (target - value)
    float    pidKi      = 0.0005f;       // integral gain
    float    lowThresh  = 40.0f;
    float    highThresh = 80.0f;
    float    safetyThresh = 100.0f;      // overpressure cutoff
    uint32_t refillTimeoutMs = 5000;     // per fill cycle
    uint32_t minOffMs   = 300;
    bool     requireSensor = true;       // no auto-start if sensor missing
};

struct GateConfig {
    AirGateType type = AirGateType::SolenoidSimple;
    int8_t     pin = -1;                  // solenoid GPIO, or servo GPIO
    PwmBackend backend = PwmBackend::Gpio;
    uint8_t    pcaChannel = 0;
    uint16_t   servoMinUs = 1000;         // servo-gate pulse window (calibratable, #22)
    uint16_t   servoMaxUs = 2000;
    bool     activeHigh = true;
    uint32_t openTimeoutMs = 0;          // 0 = disabled (safety)
    // solenoid pwm economiser
    float    peak01   = 1.0f;
    uint32_t peakMs   = 40;
    float    hold01   = 0.4f;            // must be <= peak01
    // servo valve / diverter
    float    closed01 = 0.0f;
    float    open01   = 1.0f;
    uint32_t openDelayMs  = 40;
    uint32_t closeDelayMs = 30;
};

struct FlowConfig {
    FlowControlType type = FlowControlType::FlowServo;
    int8_t   pin = -1;                    // flow servo / proportional valve pin
    PwmBackend backend = PwmBackend::Gpio;
    uint8_t  pcaChannel = 0;
    uint16_t servoMinUs = 1000, servoMaxUs = 2000;   // flow-servo pulse window (#22)
    uint8_t  min = 0, nominal = 64, max = 127;
    float    rest01 = 0.0f;
    VelocityCurve curve = VelocityCurve::Linear;
    float    expo = 2.0f;                // for Exponential
    float    maxSlewPerMs = 0.02f;       // rate limit
};

struct AngleConfig {
    bool     enabled = false;
    int8_t   pin = -1;                    // jet-angle servo pin (independent of flow)
    PwmBackend backend = PwmBackend::Gpio;
    uint8_t  pcaChannel = 0;
    uint16_t servoMinUs = 1000, servoMaxUs = 2000;   // angle-servo pulse window (#22)
    float    rest01 = 0.5f, min01 = 0.0f, nominal01 = 0.5f, max01 = 1.0f;
    bool     useCc74 = true;
};

struct SensorConfig {
    AirSensorType type = AirSensorType::None;
    int8_t   pin = -1;                    // analog/digital pin (I2C sensors use the bus)
    uint8_t  i2cAddr = 0;
    float    rawMin = 0, rawMax = 4095;
    float    physMin = 0, physMax = 100;
    bool     invert = false;
    float    filterAlpha = 0.3f;         // EMA
    uint32_t staleTimeoutMs = 500;       // no change ⇒ stale
    float    physLo = 0, physHi = 120;   // valid physical range
};

struct AirConfig {
    SourceConfig source;
    GateConfig   gate;
    FlowConfig   flow;
    AngleConfig  angle;
    SensorConfig sensor;
    uint32_t     valveOpenTimeoutMs = 0; // global safety valve timeout
    uint32_t     minNoteMs = 0;          // minimum note duration
};

// ===========================================================================
// SENSOR
// ===========================================================================
class AirSensor : public IAirSensor {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override {
        c_ = cfg.sensor; sink_ = sink; filt_ = NAN; lastRaw_ = NAN;
        configured_ = (c_.type != AirSensorType::None);
        // A configured sensor starts NOT present and NOT valid until the FIRST
        // valid measurement arrives — otherwise its initial value (0) reads as
        // "pressure too low" and the tank regulator starts the pumps before any
        // real reading (review #4 §P0). fault() reports SensorMissing meanwhile,
        // which keeps the pumps off until update() gets a good sample.
        present_ = false; lastGoodMs_ = 0; lastChangeMs_ = 0; haveTime_ = false;
        stale_ = false; inRange_ = true; frozen_ = false;
    }
    void update(uint32_t nowMs) override {
        if (!configured_) return;
        if (!haveFirst_) { firstMs_ = nowMs; haveFirst_ = true; }
        float raw = sink_ ? sink_->readSensorRaw() : NAN;
        if (std::isnan(raw)) {
            // Transient bad read: keep trying. Declare the sensor ABSENT only
            // after readings have been missing longer than the timeout (measured
            // from the last good sample, or from boot if none ever arrived), and
            // recover automatically once a real value returns (review #15).
            uint32_t ref = haveTime_ ? lastGoodMs_ : firstMs_;
            if (elapsed_u32(nowMs, ref) > c_.staleTimeoutMs) present_ = false;
            return;   // no fresh sample this tick — do not fabricate one
        }
        present_ = true;                          // recovered / present
        lastGoodMs_ = nowMs; haveTime_ = true;    // a fresh sample arrived → not stale
        stale_ = false;
        if (std::isnan(filt_)) filt_ = raw;
        else filt_ = filt_ + c_.filterAlpha * (raw - filt_);
        // "frozen" is a DISTINCT diagnostic (long window) — a perfectly stable
        // reading is NOT stale (review #14).
        if (std::isnan(lastRaw_) || std::fabs(raw - lastRaw_) > 0.5f) { lastRaw_ = raw; lastChangeMs_ = nowMs; }
        frozen_ = elapsed_u32(nowMs, lastChangeMs_) > (c_.staleTimeoutMs * 20u);
        float phys = c_.physMin + (filt_ - c_.rawMin) / (c_.rawMax - c_.rawMin) *
                     (c_.physMax - c_.physMin);
        if (c_.invert) phys = c_.physMax - (phys - c_.physMin);
        inRange_ = (phys >= c_.physLo && phys <= c_.physHi);
        if (inRange_) lastValid_ = phys;
    }
    bool present() const override { return present_; }
    bool valid()   const override { return present_ && !stale_ && inRange_; }
    float value()  const override { return lastValid_; }
    bool  frozen()  const { return frozen_; }
    FaultCode fault() const override {
        if (!present_) return FaultCode::SensorMissing;
        if (stale_)    return FaultCode::SensorStale;
        if (!inRange_) return FaultCode::SensorOutOfRange;
        return FaultCode::None;
    }
private:
    SensorConfig c_; IAirSink* sink_ = nullptr;
    float filt_ = NAN, lastRaw_ = NAN, lastValid_ = 0;
    uint32_t lastGoodMs_ = 0, lastChangeMs_ = 0, firstMs_ = 0;
    bool haveTime_ = false, haveFirst_ = false;
    bool configured_ = false, present_ = false, stale_ = false, inRange_ = true, frozen_ = false;
};

// ===========================================================================
// SOURCES
// ===========================================================================
class BaseSource : public IAirSource {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override { c_ = cfg.source; sink_ = sink; fault_ = FaultCode::None; }
    void update(uint32_t) override {}
    bool ready() const override { return true; }
    void safeState() override { if (sink_) for (uint8_t i=0;i<MAX_PUMPS;++i) sink_->setSourceLevel(i, 0.0f); }
    void resetFault() override { fault_ = FaultCode::None; }
    FaultCode fault() const override { return fault_; }
protected:
    SourceConfig c_; IAirSink* sink_ = nullptr; FaultCode fault_ = FaultCode::None;
};

class ExternalPassiveSource : public BaseSource {
public:
    void prepare(const AirNoteRequest&, uint32_t) override {}
    void run(const AirNoteRequest&, uint32_t) override {}
    void idle(uint32_t) override {}
    bool ready() const override { return true; }   // air always available
};

class FanSource : public BaseSource {
public:
    void prepare(const AirNoteRequest&, uint32_t nowMs) override {
        preparedMs_ = nowMs; running_ = true; idling_ = false; ready_ = false;
        float lvl = c_.min01 + c_.startBoost01;
        if (sink_) sink_->setSourceLevel(0, clampv(lvl, 0.0f, 1.0f));
    }
    void run(const AirNoteRequest& r, uint32_t) override {
        running_ = true; idling_ = false;
        float lvl = lerp(c_.min01, c_.max01, r.velocity / 127.0f);
        if (sink_) sink_->setSourceLevel(0, lvl);
    }
    void idle(uint32_t nowMs) override { stopAtMs_ = nowMs + c_.stopDelayMs; idling_ = true; }
    void update(uint32_t nowMs) override {
        if (running_ && !ready_ && elapsed_u32(nowMs, preparedMs_) >= c_.spinUpMs) ready_ = true;
        if (idling_ && nowMs >= stopAtMs_) {
            if (sink_) sink_->setSourceLevel(0, c_.idle01);
            idling_ = false; running_ = c_.idle01 > 0.0f; ready_ = running_;
        }
    }
    bool ready() const override { return ready_; }
private:
    uint32_t preparedMs_ = 0, stopAtMs_ = 0;
    bool running_ = false, idling_ = false, ready_ = false;
};

class PumpDirectSource : public BaseSource {
public:
    void prepare(const AirNoteRequest& r, uint32_t nowMs) override {
        startMs_ = nowMs; started_ = 0; req_ = r; running_ = true; run(r, nowMs);
    }
    void run(const AirNoteRequest& r, uint32_t nowMs) override {
        req_ = r; running_ = true;
        float lvl = lerp(c_.min01, c_.max01, r.velocity / 127.0f);
        uint8_t n = clampv<uint8_t>(c_.pumpCount, 1, MAX_PUMPS);
        // cascade: enable pump i once its stagger delay elapsed (limit inrush)
        for (uint8_t i = 0; i < n; ++i) {
            bool on = elapsed_u32(nowMs, startMs_) >= uint32_t(i) * c_.cascadeDelayMs;
            if (sink_) sink_->setSourceLevel(i, on ? lvl : 0.0f);
            if (on && i + 1 > started_) started_ = i + 1;
        }
    }
    // Drive the cascade forward so pumps 2/3 actually start after their delay
    // even without a new note event (review #16).
    void update(uint32_t nowMs) override { if (running_) run(req_, nowMs); }
    void idle(uint32_t) override {
        running_ = false; started_ = 0;
        if (sink_) for (uint8_t i=0;i<MAX_PUMPS;++i) sink_->setSourceLevel(i, 0.0f);
    }
    bool ready() const override { return started_ >= clampv<uint8_t>(c_.pumpCount,1,MAX_PUMPS); }
private:
    uint32_t startMs_ = 0; uint8_t started_ = 0; bool running_ = false;
    AirNoteRequest req_;
};

class PumpTankSource : public BaseSource {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override { BaseSource::begin(cfg, sink); sensor_.begin(cfg, sink); }
    void setSensor(AirSensor* s) { extSensor_ = s; }
    void prepare(const AirNoteRequest&, uint32_t nowMs) override { regulate(nowMs); }
    void run(const AirNoteRequest&, uint32_t nowMs) override { regulate(nowMs); }
    void idle(uint32_t nowMs) override { regulate(nowMs); }
    void update(uint32_t nowMs) override { if (!extSensor_) sensor_.update(nowMs); regulate(nowMs); }
    bool ready() const override { return regulatedReady_ && fault_ == FaultCode::None; }
    void resetFault() override { fault_ = FaultCode::None; filling_ = false; regulatedReady_ = false; pidI_ = 0.0f; havePidTime_ = false; }
private:
    AirSensor* activeSensor() { return extSensor_ ? extSensor_ : &sensor_; }
    void regulate(uint32_t nowMs) {
        // Time-step (seconds) since the previous regulate() so the PI integral
        // accumulates per unit TIME, not per call — the tick rate no longer
        // changes the effective integral gain (#3 §10.1). Clamped so a long gap
        // (first call, or a stall) cannot wind the integral up in one step.
        if (havePidTime_) {
            dtS_ = float(elapsed_u32(nowMs, lastPidMs_)) / 1000.0f;
            if (dtS_ > 0.5f) dtS_ = 0.5f;
        } else { dtS_ = 0.0f; }
        lastPidMs_ = nowMs; havePidTime_ = true;
        AirSensor* s = activeSensor();
        // never auto-start pumps without a working configured sensor
        if (c_.requireSensor && (!s->present())) {
            fault_ = FaultCode::SensorMissing; regulatedReady_ = false; safeState(); return;
        }
        if (c_.requireSensor && s->fault() == FaultCode::SensorStale) {
            fault_ = FaultCode::SensorStale; regulatedReady_ = false; safeState(); return;
        }
        // An out-of-range reading means we can't trust lastValid_ — stop the
        // pumps rather than regulate on a stale/implausible value (review #18).
        if (c_.requireSensor && s->fault() == FaultCode::SensorOutOfRange) {
            fault_ = FaultCode::SensorOutOfRange; regulatedReady_ = false; safeState(); return;
        }
        float p = s->value();
        if (p >= c_.safetyThresh) { fault_ = FaultCode::Overpressure; regulatedReady_ = false; safeState(); return; }
        regulatedReady_ = (p >= c_.lowThresh);
        uint8_t n = clampv<uint8_t>(c_.pumpCount, 1, MAX_PUMPS);
        if (!filling_ && p < c_.lowThresh && elapsed_u32(nowMs, lastOffMs_) >= c_.minOffMs) {
            filling_ = true; fillStartMs_ = nowMs;                 // start a fill cycle
        }
        if (filling_) {
            if (elapsed_u32(nowMs, fillStartMs_) > c_.refillTimeoutMs) {   // rearmed EVERY cycle
                fault_ = FaultCode::PumpTimeout; filling_ = false; safeState(); return;
            }
            if (p >= c_.highThresh) { filling_ = false; lastOffMs_ = nowMs; pidI_ = 0.0f; setPumps(n, 0.0f, nowMs, false); return; }
            // PWM: regulate toward `target` with a PI law (uses target, review
            // #17). On/off: run at max within the low/high hysteresis band.
            float drive;
            if (c_.tankPwm) {
                float err = c_.target - p;
                pidI_ = clampv(pidI_ + err * c_.pidKi * dtS_, -c_.max01, c_.max01);
                drive = clampv(c_.pidKp * err + pidI_, c_.min01, c_.max01);
            } else {
                drive = c_.max01;
            }
            // Stagger the pumps in over cascadeDelayMs from the fill start so a
            // multi-pump tank doesn't slam all motors on at once (#3 §10.2).
            setPumps(n, drive, nowMs, true);
        } else if (sink_) {
            setPumps(n, 0.0f, nowMs, false);
        }
    }
    // Drive n pumps to `lvl`. When cascade, pump i only turns on once i cascade
    // delays have elapsed since the fill started; each pump advances on its own
    // as regulate() is re-entered from update() every tick.
    void setPumps(uint8_t n, float lvl, uint32_t nowMs, bool cascade) {
        if (!sink_) return;
        for (uint8_t i = 0; i < n; ++i) {
            bool on = !cascade || lvl <= 0.0f ||
                      elapsed_u32(nowMs, fillStartMs_) >= uint32_t(i) * c_.cascadeDelayMs;
            sink_->setSourceLevel(i, on ? lvl : 0.0f);
        }
    }
    AirSensor sensor_; AirSensor* extSensor_ = nullptr;
    bool regulatedReady_ = false, filling_ = false;
    uint32_t fillStartMs_ = 0, lastOffMs_ = 0;
    float pidI_ = 0.0f;
    uint32_t lastPidMs_ = 0; bool havePidTime_ = false; float dtS_ = 0.0f;
};

// ===========================================================================
// GATES
// ===========================================================================
class NoGate : public IAirGate {
public:
    void begin(const AirConfig&, IAirSink*) override {}
    void open(uint32_t) override { open_ = true; }
    void close(uint32_t) override { open_ = false; }
    void update(uint32_t) override {}
    bool isOpen() const override { return open_; }
    void safeState() override { open_ = false; }
    FaultCode fault() const override { return FaultCode::None; }
private: bool open_ = false;
};

class SolenoidSimpleGate : public IAirGate {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override { c_ = cfg.gate; sink_ = sink; }
    void open(uint32_t nowMs) override { open_ = true; openedMs_ = nowMs; if (sink_) sink_->setGateOpen(true); }
    void close(uint32_t) override { open_ = false; if (sink_) sink_->setGateOpen(false); }
    void update(uint32_t nowMs) override {
        if (open_ && c_.openTimeoutMs && elapsed_u32(nowMs, openedMs_) > c_.openTimeoutMs) {
            fault_ = FaultCode::ValveTimeout; close(nowMs);
        }
    }
    bool isOpen() const override { return open_; }
    void safeState() override { open_ = false; if (sink_) sink_->setGateOpen(false); }
    void resetFault() override { fault_ = FaultCode::None; }
    FaultCode fault() const override { return fault_; }
private:
    GateConfig c_; IAirSink* sink_ = nullptr; bool open_ = false;
    uint32_t openedMs_ = 0; FaultCode fault_ = FaultCode::None;
};

class SolenoidPwmGate : public IAirGate {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override {
        c_ = cfg.gate; sink_ = sink;
        if (c_.hold01 > c_.peak01) c_.hold01 = c_.peak01;   // consistency: hold <= peak
    }
    void open(uint32_t nowMs) override { open_ = true; openedMs_ = nowMs; phasePeak_ = true; if (sink_) sink_->setGatePwm(c_.peak01); }
    void close(uint32_t) override { open_ = false; phasePeak_ = false; if (sink_) sink_->setGatePwm(0.0f); }
    void update(uint32_t nowMs) override {
        if (open_ && phasePeak_ && elapsed_u32(nowMs, openedMs_) >= c_.peakMs) {
            phasePeak_ = false; if (sink_) sink_->setGatePwm(c_.hold01);       // drop to hold
        }
        if (open_ && c_.openTimeoutMs && elapsed_u32(nowMs, openedMs_) > c_.openTimeoutMs) {
            fault_ = FaultCode::ValveTimeout; close(nowMs);
        }
    }
    bool isOpen() const override { return open_; }
    void safeState() override { open_ = false; phasePeak_ = false; if (sink_) sink_->setGatePwm(0.0f); } // immediate close on panic
    void resetFault() override { fault_ = FaultCode::None; }
    FaultCode fault() const override { return fault_; }
    float holdLevel() const { return c_.hold01; }
private:
    GateConfig c_; IAirSink* sink_ = nullptr; bool open_ = false, phasePeak_ = false;
    uint32_t openedMs_ = 0; FaultCode fault_ = FaultCode::None;
};

// Servo valve, diverter and flow-servo-as-valve all reduce to moving a servo
// between a closed and an open position with open/close delays and an
// automatic return to the closed position.
class ServoValveGate : public IAirGate {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override { c_ = cfg.gate; sink_ = sink; if (sink_) sink_->setGatePwm(c_.closed01); }
    void open(uint32_t nowMs) override { want_ = true; changeMs_ = nowMs; }
    void close(uint32_t nowMs) override { want_ = false; changeMs_ = nowMs; }
    void update(uint32_t nowMs) override {
        if (want_ && !open_ && elapsed_u32(nowMs, changeMs_) >= c_.openDelayMs) { open_ = true; if (sink_) sink_->setGatePwm(c_.open01); }
        if (!want_ && open_ && elapsed_u32(nowMs, changeMs_) >= c_.closeDelayMs) { open_ = false; if (sink_) sink_->setGatePwm(c_.closed01); }
        if (open_ && c_.openTimeoutMs && elapsed_u32(nowMs, changeMs_) > c_.openTimeoutMs) { fault_ = FaultCode::ValveTimeout; close(nowMs); }
    }
    bool isOpen() const override { return open_; }
    void safeState() override { want_ = false; open_ = false; if (sink_) sink_->setGatePwm(c_.closed01); } // truly closed
    void resetFault() override { fault_ = FaultCode::None; }
    FaultCode fault() const override { return fault_; }
private:
    GateConfig c_; IAirSink* sink_ = nullptr; bool want_ = false, open_ = false;
    uint32_t changeMs_ = 0; FaultCode fault_ = FaultCode::None;
};

// ===========================================================================
// FLOW
// ===========================================================================
class FlowController : public IFlowController {
public:
    void begin(const AirConfig& cfg, IAirSink* sink) override {
        c_ = cfg.flow; sink_ = sink;
        // enforce min <= nominal <= max
        if (c_.nominal < c_.min) c_.nominal = c_.min;
        if (c_.nominal > c_.max) c_.nominal = c_.max;
        cur_ = c_.rest01; tgt_ = c_.rest01; haveTime_ = false;
    }
    void setTarget(uint8_t airValue) override {
        float v = clampv<float>(airValue, c_.min, c_.max);
        float span = (c_.max > c_.min) ? (v - c_.min) / float(c_.max - c_.min) : 0.0f;
        tgt_ = shape(span);
    }
    void rest() override { tgt_ = c_.rest01; }
    // Update the dynamic flow shaping params live, keeping the current output.
    void applyDynamic(const FlowConfig& f) {
        uint8_t mn = f.min, nm = f.nominal, mx = f.max;
        if (nm < mn) nm = mn;
        if (nm > mx) nm = mx;
        c_.min = mn; c_.nominal = nm; c_.max = mx;
        c_.curve = f.curve; c_.expo = f.expo; c_.maxSlewPerMs = f.maxSlewPerMs; c_.rest01 = f.rest01;
    }
    void update(uint32_t nowMs) override {
        if (c_.type == FlowControlType::None) { cur_ = 0.0f; return; }
        float dt = 1.0f;
        if (haveTime_) dt = float(elapsed_u32(nowMs, lastMs_)); else haveTime_ = true;
        lastMs_ = nowMs;
        float maxStep = c_.maxSlewPerMs * (dt > 0 ? dt : 1.0f);
        float d = tgt_ - cur_;
        if (d >  maxStep) d =  maxStep;
        if (d < -maxStep) d = -maxStep;
        cur_ += d;
        if (sink_) sink_->setFlow(cur_);
    }
    float current01() const override { return cur_; }
    void safeState() override { cur_ = c_.rest01; if (sink_) sink_->setFlow(c_.rest01); }
private:
    float shape(float x) const {
        switch (c_.curve) {
            case VelocityCurve::Linear:    return x;
            case VelocityCurve::Quadratic: return x * x;
            case VelocityCurve::Cubic:     return x * x * x;
            case VelocityCurve::Exponential: return std::pow(x, c_.expo);
            case VelocityCurve::Custom:    return x;  // hook for a user LUT
        }
        return x;
    }
    FlowConfig c_; IAirSink* sink_ = nullptr;
    float cur_ = 0, tgt_ = 0; uint32_t lastMs_ = 0; bool haveTime_ = false;
};

} // namespace swc

#endif // SWC_CORE_AIRSYSTEM_H
