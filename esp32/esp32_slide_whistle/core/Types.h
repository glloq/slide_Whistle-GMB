/*
 * core/Types.h — Portable domain types for the universal slide-whistle engine.
 *
 * This header is deliberately Arduino-free (pure C++17) so the whole control
 * core compiles and is unit-tested natively (`pio test -e native`, g++) as well
 * as on the ESP32. No hardware access, no dynamic allocation on the hot path.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_TYPES_H
#define SWC_CORE_TYPES_H

#include <cstdint>
#include <cstddef>

namespace swc {

// ---------------------------------------------------------------------------
// Global limits (kept small enough for static allocation on ESP32)
// ---------------------------------------------------------------------------
static constexpr uint8_t  MAX_INSTRUMENTS   = 4;
static constexpr uint16_t MIDI_NOTE_COUNT    = 128;   // full MIDI range
static constexpr uint8_t  MAX_NOTE_STACK     = 16;    // held notes per instrument
static constexpr uint8_t  MAX_PUMPS          = 3;
// Max STEP pulse rate the EspStepGen backend can produce per axis: the shared
// timer runs at 40 kHz and two ticks make one pulse, so 20 000 steps/s. The
// validator rejects a config whose maxSpeedMmS × stepsPerMm exceeds this — above
// it the generator cannot keep up and the slide silently lags its profile
// (review #9 §3.4). Keep in sync with EspStepGen::kTickUs (1e6 / (2·kTickUs)).
static constexpr uint32_t STEP_GEN_MAX_HZ    = 20000;

// ---------------------------------------------------------------------------
// Fault codes — shared by actuators and air systems
// ---------------------------------------------------------------------------
enum class FaultCode : uint8_t {
    None = 0,
    NotHomed,
    HomingTimeout,
    TargetOutOfRange,
    EndstopInconsistent,
    ConfigInvalid,
    SensorMissing,
    SensorStale,
    SensorOutOfRange,
    Overpressure,
    ValveTimeout,
    PumpTimeout,
    DriverFault,
    EmergencyStop,
};

// ---------------------------------------------------------------------------
// Slide motion state machine (ISlideActuator)
// ---------------------------------------------------------------------------
enum class MotionState : uint8_t {
    Uninitialised = 0,
    Idle,             // homed, at rest, ready to accept targets
    Homing,           // non-blocking homing FSM in progress
    Moving,           // travelling toward target
    Holding,          // at target within tolerance
    Fault,
    EStopped,
};

// ---------------------------------------------------------------------------
// Air system state machine (IAirSystem)
// ---------------------------------------------------------------------------
enum class AirState : uint8_t {
    Uninitialised = 0,
    Idle,             // gate closed, source at rest
    Preparing,        // source spinning up / tank filling for a note
    Playing,          // gate open, flow applied
    Releasing,        // ramping down after note off
    Fault,
    EStopped,
};

// ---------------------------------------------------------------------------
// Slide drive backends
// ---------------------------------------------------------------------------
enum class SlideDriveType : uint8_t {
    Disabled = 0,     // no movement — air-only bench testing
    StepDir,          // A4988 / DRV8825 / TMC2208-2209 (step/dir)
    SingleServo,      // one position servo (GPIO or PCA9685)
    DualServo,        // two synchronised servos
};

// PWM/servo output backend
enum class PwmBackend : uint8_t {
    Gpio = 0,         // ESP32 LEDC on a GPIO
    Pca9685,          // external PCA9685 channel over I2C
};

// Dual-servo synchronisation mode
enum class DualSyncMode : uint8_t {
    SameDirection = 0,
    Opposite,
    MasterSlave,
};

// ---------------------------------------------------------------------------
// Air building blocks (composable, NOT a fixed enum of whole systems)
// ---------------------------------------------------------------------------
enum class AirSourceType : uint8_t {
    ExternalPassive = 0,   // compressor / regulated tank — no electrical output
    FanOnOff,
    FanPwm,
    PumpsDirect,           // 1..3 pumps feeding the whistle directly
    PumpsTank,             // 1..3 pumps + regulated tank/bellows
};

enum class AirGateType : uint8_t {
    None = 0,              // something else cuts the air
    SolenoidSimple,
    SolenoidPwm,           // peak/hold PWM economiser
    ServoValve,
    ServoDiverter,         // permanent fan + servo directing the flow
    FlowServoAsValve,      // flow servo returns to a truly-closed position
};

enum class FlowControlType : uint8_t {
    None = 0,
    FlowServo,
    ProportionalPwm,
    FanPwm,
    PumpPwm,
    SourcePlusFlowServo,
};

enum class VelocityCurve : uint8_t {
    Linear = 0,
    Quadratic,
    Cubic,
    Exponential,
    Custom,
};

enum class AirSensorType : uint8_t {
    None = 0,
    PressureAnalog,
    TofVL53L0X,
    TofVL6180X,
    HallAnalog,
    EndstopMechanical,
    EndstopOptical,
    DigitalLevel,
};

enum class TankRegulationMode : uint8_t {
    Pressure = 0,
    Level,
    Position,
};

// ---------------------------------------------------------------------------
// Monophonic note-priority policy
// ---------------------------------------------------------------------------
enum class MonoPolicy : uint8_t {
    LastNote = 0,     // last pressed wins, fall back to previous on release
    HighestNote,
    LowestNote,
};

// ---------------------------------------------------------------------------
// Legato policy — governs whether air is cut between notes
// ---------------------------------------------------------------------------
enum class LegatoPolicy : uint8_t {
    AlwaysClose = 0,      // cut air between every note
    HoldWithinTime,       // hold if the previous note-on was recent
    HoldWithinDistance,   // hold if slide travel is small
    HoldWithinMoveTime,   // hold if estimated move time is short
    Glissando,            // keep air open during the move
    SafetyLargeMove,      // close whenever a large move is needed
};

// Note-mapping strategy
enum class MappingMode : uint8_t {
    CalibratedTable = 0,  // per-note calibrated points
    Interpolated,         // interpolate between calibrated points
    LinearProvisional,    // provisional linear mapping
    PhysicalModel,        // frequency/length physical model (optional)
};

// ---------------------------------------------------------------------------
// Small helpers (no <algorithm> dependency to stay light on MCU)
// ---------------------------------------------------------------------------
template <typename T>
inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Unsigned time subtraction that survives millis()/micros() rollover.
inline uint32_t elapsed_u32(uint32_t now, uint32_t since) {
    return static_cast<uint32_t>(now - since);
}

} // namespace swc

#endif // SWC_CORE_TYPES_H
