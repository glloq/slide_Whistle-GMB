/*
 * core/IAirSystem.h — air-system interface + composable block contracts.
 *
 * Air is NOT a fixed enum of whole systems. It is composed of four blocks
 * (Section 5): source → gate → flow (→ angle), plus sensors and a safety
 * controller. Each block has its own small interface so any source can pair
 * with any gate and any flow control.
 *
 * Hardware is reached only through IAirSink; the composition logic is pure and
 * unit-tested natively.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_IAIRSYSTEM_H
#define SWC_CORE_IAIRSYSTEM_H

#include "Types.h"

namespace swc {

// --- Note lifecycle payloads ------------------------------------------------
struct AirNoteRequest {
    uint8_t velocity   = 100;   // 0..127
    uint8_t airMin     = 0;     // per-note calibrated air window (0..127)
    uint8_t airNominal = 0;
    uint8_t airMax     = 127;
    bool    legatoHold = false; // sequencer decided to keep air across notes
};

struct AirExpression {
    uint8_t breath     = 0xFF;  // 0xFF = unset
    uint8_t expression = 0xFF;
    uint8_t volume     = 0xFF;
    uint8_t angleCc    = 0xFF;  // CC74
};

// --- Hardware boundary ------------------------------------------------------
// index lets a system own several actuators (e.g. multiple pumps).
class IAirSink {
public:
    virtual void setSourceLevel(uint8_t index, float norm01) = 0; // fan/pump drive
    virtual void setGateOpen(bool open) = 0;                      // on/off gate
    virtual void setGatePwm(float norm01) = 0;                    // pwm/servo gate
    virtual void setFlow(float norm01) = 0;                       // flow control
    virtual void setAngle(float norm01) = 0;                      // jet angle servo
    virtual float readSensorRaw() = 0;                            // NAN if absent/unreadable
    virtual ~IAirSink() = default;
};

// --- Composable block interfaces -------------------------------------------
struct AirConfig; // fwd

class IAirSource {
public:
    virtual void begin(const AirConfig& cfg, IAirSink* sink) = 0;
    virtual void prepare(const AirNoteRequest& r, uint32_t nowMs) = 0; // spin up / fill
    virtual void run(const AirNoteRequest& r, uint32_t nowMs) = 0;     // note playing
    virtual void idle(uint32_t nowMs) = 0;                             // between notes
    virtual void update(uint32_t nowMs) = 0;
    virtual bool ready() const = 0;          // enough air to open the gate
    virtual void safeState() = 0;
    virtual void resetFault() {}             // clear latched internal fault on rearm (#13)
    virtual FaultCode fault() const = 0;
    virtual ~IAirSource() = default;
};

class IAirGate {
public:
    virtual void begin(const AirConfig& cfg, IAirSink* sink) = 0;
    virtual void open(uint32_t nowMs) = 0;
    virtual void close(uint32_t nowMs) = 0;
    virtual void update(uint32_t nowMs) = 0;
    virtual bool isOpen() const = 0;
    virtual void safeState() = 0;            // must truly close the air
    virtual void resetFault() {}             // clear latched internal fault on rearm (#13)
    virtual FaultCode fault() const = 0;
    virtual ~IAirGate() = default;
};

class IFlowController {
public:
    virtual void begin(const AirConfig& cfg, IAirSink* sink) = 0;
    virtual void setTarget(uint8_t airValue) = 0;   // 0..127 desired flow
    virtual void rest() = 0;
    virtual void update(uint32_t nowMs) = 0;
    virtual float current01() const = 0;
    virtual void safeState() = 0;
    virtual ~IFlowController() = default;
};

class IAirSensor {
public:
    virtual void begin(const AirConfig& cfg, IAirSink* sink) = 0;
    virtual void update(uint32_t nowMs) = 0;
    virtual bool present() const = 0;
    virtual bool valid() const = 0;          // fresh + in range
    virtual float value() const = 0;         // physical units, last valid
    virtual FaultCode fault() const = 0;
    virtual ~IAirSensor() = default;
};

// --- Top-level interface (from the mission) --------------------------------
class IAirSystem {
public:
    virtual bool begin(const AirConfig& config, IAirSink* sink) = 0;
    virtual void update(uint32_t nowMs) = 0;

    virtual void prepareNote(const AirNoteRequest& request) = 0;
    virtual void startNote(const AirNoteRequest& request) = 0;
    virtual void updateExpression(const AirExpression& expression) = 0;
    virtual void stopNote() = 0;

    virtual void emergencyStop() = 0;
    virtual void rearm() = 0;              // recover from E-stop/fault, no rebuild
    virtual bool isReady() const = 0;
    virtual AirState state() const = 0;
    virtual FaultCode fault() const = 0;

    virtual ~IAirSystem() = default;
};

} // namespace swc

#endif // SWC_CORE_IAIRSYSTEM_H
