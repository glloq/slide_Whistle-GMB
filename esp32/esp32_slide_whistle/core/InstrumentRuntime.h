/*
 * core/InstrumentRuntime.h — build a live Instrument from its config.
 *
 * This is the object the RT task holds per flute. It owns one concrete instance
 * of every actuator backend (no heap) and every air block, selects the active
 * ones from InstrumentConfig, and exposes a ready-to-run Instrument. Changing
 * the mechanism = changing config, no recompile (acceptance #2/#3).
 *
 * Portable and unit-tested: given fake sinks, build() wires the right actuator
 * so the whole chain runs off-device.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_INSTRUMENTRUNTIME_H
#define SWC_CORE_INSTRUMENTRUNTIME_H

#include "SlideActuators.h"
#include "AirEngine.h"
#include "Instrument.h"
#include "RuntimeConfig.h"

namespace swc {

class InstrumentRuntime {
public:
    // sinks are the platform hardware boundary (fakes in tests).
    InstrumentRuntime(IMotionSink* motionSink, IAirSink* airSink)
        : disabled_(motionSink), stepper_(motionSink),
          servo_(motionSink), dual_(motionSink),
          motionSink_(motionSink), airSink_(airSink) {}

    // Build (or rebuild) from config. Returns false on invalid config.
    bool build(uint8_t id, const InstrumentConfig& cfg) {
        cfg_ = cfg;
        map_ = cfg.map;                       // copy calibration table

        switch (cfg.motion.type) {
            case SlideDriveType::Disabled:    act_ = &disabled_; break;
            case SlideDriveType::StepDir:     act_ = &stepper_;  break;
            case SlideDriveType::SingleServo: act_ = &servo_;    break;
            case SlideDriveType::DualServo:   act_ = &dual_;     break;
        }
        if (!act_->begin(cfg.motion)) return false;
        if (!air_.begin(cfg.air, airSink_)) return false;
        inst_.begin(id, act_, &air_, &map_, cfg);
        return true;
    }

    // Hardware-safe boot: keep everything de-energised until validated/homed.
    void enterSafeState() {
        air_.emergencyStop();
        if (act_) act_->emergencyStop();
    }

    Instrument&     instrument() { return inst_; }
    ISlideActuator* actuator()  { return act_; }
    AirSystem&      air()        { return air_; }
    NoteMap&        map()        { return map_; }
    const InstrumentConfig& config() const { return cfg_; }

    // Ticked by the RT task.
    void update(uint32_t nowMs, uint32_t nowUs) {
        if (act_) act_->update(nowUs);
        air_.setNow(nowMs);
        air_.update(nowMs);
        inst_.update(nowMs, nowUs);
    }

private:
    DisabledSlideActuator    disabled_;
    StepDirSlideActuator     stepper_;
    SingleServoSlideActuator servo_;
    DualServoSlideActuator   dual_;
    AirSystem  air_;
    NoteMap    map_;
    Instrument inst_;
    InstrumentConfig cfg_;
    ISlideActuator*  act_ = nullptr;
    IMotionSink*     motionSink_ = nullptr;
    IAirSink*        airSink_ = nullptr;
};

} // namespace swc

#endif // SWC_CORE_INSTRUMENTRUNTIME_H
