/*
 * core/AirEngine.h — AirSystem orchestrator + safety + presets.
 *
 * Owns one concrete instance of every block (no heap) and selects the active
 * source / gate / flow / sensor from AirConfig. Drives the note timeline of
 * Section 7 and refuses to re-open the air after emergencyStop() until begin()
 * runs again (a deferred command can never reopen the air, correction #15).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_AIRENGINE_H
#define SWC_CORE_AIRENGINE_H

#include "AirSystem.h"

namespace swc {

// ---------------------------------------------------------------------------
// Global air-safety supervisor: watches valve/pump timeouts, overpressure and
// stale sensors and forces the whole air system into a safe state.
// ---------------------------------------------------------------------------
class AirSafetyController {
public:
    void begin(const AirConfig& cfg) { c_ = cfg; tripped_ = false; fault_ = FaultCode::None; gateOpenMs_ = 0; gateOpen_ = false; }
    void noteGate(bool open, uint32_t nowMs) { if (open && !gateOpen_) gateOpenMs_ = nowMs; gateOpen_ = open; }
    // returns true if a trip condition is active this tick
    bool check(uint32_t nowMs, FaultCode sourceFault, FaultCode gateFault, const AirSensor& sensor) {
        FaultCode f = FaultCode::None;
        if (gateOpen_ && c_.valveOpenTimeoutMs && elapsed_u32(nowMs, gateOpenMs_) > c_.valveOpenTimeoutMs)
            f = FaultCode::ValveTimeout;
        if (sourceFault != FaultCode::None) f = sourceFault;
        if (gateFault   != FaultCode::None) f = gateFault;
        if (sensor.present() && sensor.fault() == FaultCode::SensorStale) f = FaultCode::SensorStale;
        if (f != FaultCode::None) { tripped_ = true; fault_ = f; }
        return tripped_;
    }
    void reset() { tripped_ = false; fault_ = FaultCode::None; gateOpen_ = false; }
    bool tripped() const { return tripped_; }
    FaultCode fault() const { return fault_; }
private:
    AirConfig c_; bool tripped_ = false, gateOpen_ = false; uint32_t gateOpenMs_ = 0;
    FaultCode fault_ = FaultCode::None;
};

// ---------------------------------------------------------------------------
// AirSystem — composes the blocks and drives the note timeline.
// ---------------------------------------------------------------------------
class AirSystem : public IAirSystem {
public:
    bool begin(const AirConfig& config, IAirSink* sink) override {
        cfg_ = config; sink_ = sink; estopped_ = false; state_ = AirState::Idle; fault_ = FaultCode::None;

        // select source
        switch (cfg_.source.type) {
            case AirSourceType::ExternalPassive: src_ = &srcExt_; break;
            case AirSourceType::FanOnOff:
            case AirSourceType::FanPwm:          src_ = &srcFan_; break;
            case AirSourceType::PumpsDirect:     src_ = &srcPumpDirect_; break;
            case AirSourceType::PumpsTank:       src_ = &srcPumpTank_; srcPumpTank_.setSensor(&sensor_); break;
        }
        // select gate
        switch (cfg_.gate.type) {
            case AirGateType::None:            gate_ = &gNone_; break;
            case AirGateType::SolenoidSimple:  gate_ = &gSolSimple_; break;
            case AirGateType::SolenoidPwm:     gate_ = &gSolPwm_; break;
            case AirGateType::ServoValve:
            case AirGateType::ServoDiverter:
            case AirGateType::FlowServoAsValve: gate_ = &gServo_; break;
        }
        flow_ = &flow_impl_;

        sensor_.begin(cfg_, sink_);
        src_->begin(cfg_, sink_);
        gate_->begin(cfg_, sink_);
        flow_->begin(cfg_, sink_);
        safety_.begin(cfg_);
        return true;
    }

    void update(uint32_t nowMs) override {
        now_ = nowMs;
        if (estopped_) return;
        sensor_.update(nowMs);
        src_->update(nowMs);
        gate_->update(nowMs);
        flow_->update(nowMs);
        safety_.noteGate(gate_->isOpen(), nowMs);
        if (safety_.check(nowMs, src_->fault(), gate_->fault(), sensor_)) {
            fault_ = safety_.fault();
            forceSafe();
            state_ = AirState::Fault;
            return;
        }
        if (state_ == AirState::Releasing && !gate_->isOpen()) state_ = AirState::Idle;
    }

    void prepareNote(const AirNoteRequest& r) override {
        if (estopped_ || state_ == AirState::Fault) return;
        req_ = r;
        src_->prepare(r, now_);
        state_ = AirState::Preparing;
    }

    void startNote(const AirNoteRequest& r) override {
        if (estopped_ || state_ == AirState::Fault) return;
        req_ = r;
        src_->run(r, now_);
        flow_->setTarget(r.airNominal ? r.airNominal : r.velocity);
        gate_->open(now_);
        state_ = AirState::Playing;
    }

    void updateExpression(const AirExpression& e) override {
        if (state_ != AirState::Playing) return;
        uint8_t v = 0xFF;
        if (e.breath     != 0xFF) v = e.breath;
        else if (e.expression != 0xFF) v = e.expression;
        else if (e.volume != 0xFF) v = e.volume;
        if (v != 0xFF) flow_->setTarget(v);
        if (cfg_.angle.enabled && e.angleCc != 0xFF && sink_)
            sink_->setAngle(lerp(cfg_.angle.min01, cfg_.angle.max01, e.angleCc / 127.0f));
    }

    void stopNote() override {
        if (estopped_) return;
        gate_->close(now_);
        flow_->rest();
        src_->idle(now_);
        if (state_ == AirState::Playing || state_ == AirState::Preparing)
            state_ = AirState::Releasing;
    }

    void emergencyStop() override {
        estopped_ = true;
        forceSafe();
        state_ = AirState::EStopped;
        fault_ = FaultCode::EmergencyStop;
    }

    bool isReady() const override {
        return !estopped_ && state_ != AirState::Fault && src_ && src_->ready();
    }
    AirState  state() const override { return state_; }
    FaultCode fault() const override { return fault_; }

    // Provide `now` to the update-driven blocks (mono clock from the RT task).
    void setNow(uint32_t nowMs) { now_ = nowMs; }
    const AirSensor& sensor() const { return sensor_; }

private:
    void forceSafe() {
        if (gate_) gate_->safeState();
        if (flow_) flow_->safeState();
        if (src_)  src_->safeState();
        safety_.reset();
    }

    AirConfig cfg_; IAirSink* sink_ = nullptr;
    AirNoteRequest req_;
    uint32_t now_ = 0;
    bool estopped_ = false;
    AirState  state_ = AirState::Uninitialised;
    FaultCode fault_ = FaultCode::None;

    // concrete blocks (one of each; active selected by pointer)
    ExternalPassiveSource srcExt_;
    FanSource             srcFan_;
    PumpDirectSource      srcPumpDirect_;
    PumpTankSource        srcPumpTank_;
    NoGate                gNone_;
    SolenoidSimpleGate    gSolSimple_;
    SolenoidPwmGate       gSolPwm_;
    ServoValveGate        gServo_;
    FlowController        flow_impl_;
    AirSensor             sensor_;
    AirSafetyController    safety_;

    IAirSource*      src_  = nullptr;
    IAirGate*        gate_ = nullptr;
    IFlowController* flow_ = nullptr;
};

} // namespace swc

#endif // SWC_CORE_AIRENGINE_H
