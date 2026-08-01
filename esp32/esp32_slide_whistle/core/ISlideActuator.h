/*
 * core/ISlideActuator.h — common interface for every slide-drive backend.
 *
 * The whole engine speaks a single canonical unit: millimetres of slide travel.
 * Nothing above this interface knows whether a stepper or a servo moves the
 * slide (Section 3.4).
 *
 * Hardware is reached only through IMotionSink, so the actuator logic is pure
 * and unit-testable; the network/REST/JS layers never see a GPIO (Section 9).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_ISLIDEACTUATOR_H
#define SWC_CORE_ISLIDEACTUATOR_H

#include "Types.h"

namespace swc {

// --- Configuration ---------------------------------------------------------

struct EndstopConfig {
    bool    present        = true;
    bool    normallyClosed = false;   // NO vs NC
    bool    activeHigh      = false;
    bool    internalPullup  = true;
    int8_t  pin             = -1;
};

struct StepperMotionConfig {
    int8_t  stepPin    = -1;
    int8_t  dirPin     = -1;
    int8_t  enablePin  = -1;
    bool    enableActiveHigh = false;
    bool    invertDir  = false;
    uint16_t stepsPerRev = 200;
    uint16_t microsteps  = 16;
    float    stepsPerMm  = 80.0f;     // derived or entered directly
    float    homingFastMmS = 20.0f;
    float    homingSlowMmS = 3.0f;
    bool     homeTowardZero = true;   // homing direction
    float    homeOffsetMm   = 0.0f;   // applied after contact
    float    homeBackoffMm  = 3.0f;   // retract after contact
    uint32_t phaseTimeoutMs = 8000;   // per-phase homing timeout
    uint32_t idleDisableMs  = 0;      // 0 = never auto-disable
    bool     alwaysHold     = false;
    EndstopConfig endstopMin;
    EndstopConfig endstopMax;         // optional second endstop
};

struct ServoCalPoint { float mm; uint16_t us; };

struct ServoMotionConfig {
    PwmBackend backend   = PwmBackend::Gpio;
    int8_t     pin       = -1;        // GPIO, or PCA9685 channel
    uint8_t    pcaChannel = 0;
    uint16_t   freqHz    = 50;
    uint16_t   minUs     = 1000;
    uint16_t   maxUs     = 2000;
    bool       invert    = false;
    uint16_t   restUs    = 1500;
    uint16_t   safeUs    = 1500;
    int16_t    trimUs    = 0;         // per-servo trim
    int16_t    offsetUs  = 0;         // per-servo offset (dual)
    uint32_t   detachIdleMs = 0;      // 0 = never detach
    // Two-point calibration (0 mm ↔ A, travel ↔ B); optional extra points
    // provide a multi-point correction for non-linear linkages.
    ServoCalPoint cal[8] = {{0.0f, 1000}, {100.0f, 2000}};
    uint8_t       calCount = 2;
};

struct SlideMotionConfig {
    SlideDriveType type = SlideDriveType::Disabled;
    float travelMm      = 100.0f;
    float maxSpeedMmS   = 120.0f;
    float accelMmS2     = 800.0f;
    float softMinMm     = 0.0f;
    float softMaxMm     = 100.0f;
    StepperMotionConfig stepper;
    ServoMotionConfig   servoA;       // primary / master
    ServoMotionConfig   servoB;       // second servo (dual)
    DualSyncMode        dualMode = DualSyncMode::Opposite;
    bool                servoBEnabled = true;  // may be disabled for diagnostics
};

// --- Hardware boundary ------------------------------------------------------
// Implemented by a thin platform layer on the ESP32; a recording fake in tests.
class IMotionSink {
public:
    virtual void writeStepperMm(float mm) = 0;          // commanded slide position
    virtual void writeServoUs(uint8_t index, uint16_t us) = 0; // index 0=A,1=B
    virtual void enableDriver(bool on) = 0;
    virtual bool readEndstop(bool useMax) = 0;          // logical: true = triggered
    // Re-define the executed-step counter so that the NEXT writeStepperMm(mm)
    // commands zero motion. Called at the precise homing contact when the
    // actuator redefines its mm reference (0 at min, travelMm at max): without
    // it the hardware counter keeps the pre-home value and the first post-home
    // move drives a large phantom correction (review #7 §1). Default no-op:
    // absolute backends (servo) and test fakes need nothing.
    virtual void syncPositionMm(float mm) { (void)mm; }

    // Authoritative EXECUTED position, derived from the real step counter the
    // backend has physically emitted (curSteps_ today; an RMT/GPTimer step
    // generator's counter tomorrow). Step backends emit a BOUNDED number of
    // pulses per call, so this trails the commanded position during a fast move;
    // the actuator gates air on THIS value so it never opens before the motor
    // has actually arrived. Returns false on backends with no executed feedback
    // (servos, disabled, plain fakes) — the actuator then falls back to its
    // commanded position (review #7/#8 §6). Kept backend-agnostic so a future RMT
    // backend is a drop-in.
    virtual bool executedPositionMm(float& mm) const { (void)mm; return false; }
    virtual ~IMotionSink() = default;
};

// --- Interface --------------------------------------------------------------
class ISlideActuator {
public:
    virtual bool begin(const SlideMotionConfig& config) = 0;
    virtual void update(uint32_t nowUs) = 0;

    virtual bool requestPositionMm(float positionMm) = 0;
    virtual void stopControlled() = 0;
    virtual void emergencyStop() = 0;

    virtual bool requestHoming() = 0;
    virtual bool isHomed() const = 0;
    virtual bool isReadyForAir() const = 0;   // homed, at target, not moving
    virtual bool isMoving() const = 0;

    // Re-arm after an emergency stop / fault WITHOUT reconstructing the object.
    // A homed actuator returns to Idle; an unhomed one is left needing homing.
    virtual void clearFault() = 0;

    // Apply the DYNAMIC (no-restart) motion parameters — speed, accel, soft
    // limits — live. Pins / driver type are hardware and stay restart-only.
    virtual void applyDynamic(const SlideMotionConfig& cfg) = 0;

    virtual float currentPositionMm() const = 0;
    virtual float targetPositionMm() const = 0;
    virtual MotionState state() const = 0;
    virtual FaultCode fault() const = 0;

    virtual ~ISlideActuator() = default;
};

} // namespace swc

#endif // SWC_CORE_ISLIDEACTUATOR_H
