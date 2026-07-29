/*
 * core/Presets.h — the 11 mounting presets (Section 6).
 *
 * A preset only fills component types + initial parameters (and a safe,
 * collision-free ESP32-WROOM pin plan). It never locks the instrument into a
 * monolithic class: every field stays editable afterwards.
 *
 * The WROOM pin plan below uses only WiFi-safe, non-flash pins and never drives
 * an input-only pin as an output, so every preset passes the validator.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_PRESETS_H
#define SWC_CORE_PRESETS_H

#include "RuntimeConfig.h"

namespace swc {

enum class PresetId : uint8_t {
    StepperFanDiverter = 0,   // 1  historic Arduino Fan-Servo
    StepperSolenoidPwmFlow,   // 2  historic Arduino Solenoid-Servo
    StepperSolenoidOnly,      // 3  external air, fixed flow
    StepperServoValveFlow,    // 4
    StepperFlowServoAsValve,  // 5
    StepperFanPwmFlow,        // 6
    StepperPumpsValve,        // 7
    StepperPumpsTankSensor,   // 8
    SingleServoMinimalAir,    // 9
    DualServoMinimalAir,      // 10
    FullyCustom,              // 11
    COUNT
};

inline const char* presetName(PresetId p) {
    switch (p) {
        case PresetId::StepperFanDiverter:     return "Stepper + fan + diverter servo";
        case PresetId::StepperSolenoidPwmFlow:  return "Stepper + PWM solenoid + flow servo";
        case PresetId::StepperSolenoidOnly:     return "Stepper + solenoid only";
        case PresetId::StepperServoValveFlow:   return "Stepper + servo valve + flow servo";
        case PresetId::StepperFlowServoAsValve:  return "Stepper + flow servo as valve";
        case PresetId::StepperFanPwmFlow:       return "Stepper + PWM fan + flow servo";
        case PresetId::StepperPumpsValve:       return "Stepper + pumps + valve";
        case PresetId::StepperPumpsTankSensor:   return "Stepper + pumps + tank + sensor + valve";
        case PresetId::SingleServoMinimalAir:    return "Single slide servo + minimal air";
        case PresetId::DualServoMinimalAir:      return "Dual slide servos + minimal air";
        case PresetId::FullyCustom:             return "Fully custom";
        default: return "?";
    }
}

// Safe WROOM pin plan (single active instrument).
namespace pins {
    static constexpr int8_t STEP = 32, DIR = 33, EN = 25, ENDSTOP = 34;
    static constexpr int8_t FLOW = 26, GATE = 27, FAN = 14, ANGLE = 13;
    static constexpr int8_t PUMP0 = 16, PUMP1 = 17, PUMP2 = 18;
    static constexpr int8_t SENSOR = 36;   // ADC1, WiFi-safe
    static constexpr int8_t SERVO_A = 26, SERVO_B = 13;
}

inline void configureStepper(InstrumentConfig& i) {
    i.motion.type = SlideDriveType::StepDir;
    i.motion.travelMm = 100; i.motion.softMinMm = 0; i.motion.softMaxMm = 100;
    i.motion.stepper.stepPin = pins::STEP;
    i.motion.stepper.dirPin  = pins::DIR;
    i.motion.stepper.enablePin = pins::EN;
    i.motion.stepper.endstopMin.pin = pins::ENDSTOP;
    i.motion.stepper.endstopMin.present = true;
    i.motion.stepper.stepsPerMm = 80;
}

inline void applyPreset(InstrumentConfig& i, PresetId p) {
    // reset air blocks to neutral, then fill per preset
    i.air = AirConfig{};
    i.cc = CcMap{};
    switch (p) {
        case PresetId::StepperFanDiverter:
            configureStepper(i);
            i.air.source.type = AirSourceType::FanPwm; i.air.source.pin[0] = pins::FAN;
            i.air.gate.type   = AirGateType::ServoDiverter; i.air.gate.pin = pins::GATE;
            i.air.flow.type   = FlowControlType::FlowServo; i.air.flow.pin = pins::FLOW;
            break;
        case PresetId::StepperSolenoidPwmFlow:
            configureStepper(i);
            i.air.source.type = AirSourceType::ExternalPassive;
            i.air.gate.type   = AirGateType::SolenoidPwm; i.air.gate.pin = pins::GATE;
            i.air.flow.type   = FlowControlType::FlowServo; i.air.flow.pin = pins::FLOW;
            break;
        case PresetId::StepperSolenoidOnly:
            configureStepper(i);
            i.air.source.type = AirSourceType::ExternalPassive;
            i.air.gate.type   = AirGateType::SolenoidSimple; i.air.gate.pin = pins::GATE;
            i.air.flow.type   = FlowControlType::None;
            break;
        case PresetId::StepperServoValveFlow:
            configureStepper(i);
            i.air.gate.type   = AirGateType::ServoValve; i.air.gate.pin = pins::GATE;
            i.air.flow.type   = FlowControlType::FlowServo; i.air.flow.pin = pins::FLOW;
            break;
        case PresetId::StepperFlowServoAsValve:
            configureStepper(i);
            i.air.gate.type   = AirGateType::FlowServoAsValve; i.air.gate.pin = pins::FLOW;
            i.air.flow.type   = FlowControlType::FlowServo; i.air.flow.pin = pins::FLOW;
            break;
        case PresetId::StepperFanPwmFlow:
            configureStepper(i);
            i.air.source.type = AirSourceType::FanPwm; i.air.source.pin[0] = pins::FAN;
            i.air.gate.type   = AirGateType::None;
            i.air.flow.type   = FlowControlType::FlowServo; i.air.flow.pin = pins::FLOW;
            break;
        case PresetId::StepperPumpsValve:
            configureStepper(i);
            i.air.source.type = AirSourceType::PumpsDirect; i.air.source.pumpCount = 2;
            i.air.source.pin[0] = pins::PUMP0; i.air.source.pin[1] = pins::PUMP1;
            i.air.gate.type   = AirGateType::SolenoidSimple; i.air.gate.pin = pins::GATE;
            break;
        case PresetId::StepperPumpsTankSensor:
            configureStepper(i);
            i.air.source.type = AirSourceType::PumpsTank; i.air.source.pumpCount = 2;
            i.air.source.pin[0] = pins::PUMP0; i.air.source.pin[1] = pins::PUMP1;
            i.air.source.requireSensor = true;
            i.air.sensor.type = AirSensorType::PressureAnalog; i.air.sensor.pin = pins::SENSOR;
            i.air.gate.type   = AirGateType::SolenoidSimple; i.air.gate.pin = pins::GATE;
            break;
        case PresetId::SingleServoMinimalAir:
            i.motion.type = SlideDriveType::SingleServo;
            i.motion.servoA.backend = PwmBackend::Gpio; i.motion.servoA.pin = pins::SERVO_A;
            i.air.gate.type = AirGateType::SolenoidSimple; i.air.gate.pin = pins::GATE;
            break;
        case PresetId::DualServoMinimalAir:
            i.motion.type = SlideDriveType::DualServo; i.motion.dualMode = DualSyncMode::Opposite;
            i.motion.servoA.backend = PwmBackend::Gpio; i.motion.servoA.pin = pins::SERVO_A;
            i.motion.servoB.backend = PwmBackend::Gpio; i.motion.servoB.pin = pins::SERVO_B;
            i.air.gate.type = AirGateType::SolenoidSimple; i.air.gate.pin = pins::GATE;
            break;
        case PresetId::FullyCustom:
        default:
            i.motion.type = SlideDriveType::Disabled;
            i.motion.stepper.stepPin = i.motion.stepper.dirPin = -1;
            i.air.source.type = AirSourceType::ExternalPassive;
            i.air.gate.type = AirGateType::None;
            break;
    }
    // provisional linear calibration so the instrument is playable pre-tuning
    i.map.setTravelMm(i.motion.travelMm);
    i.map.generateLinear(i.noteMin, i.noteMax, 0.0f, i.motion.travelMm);
}

} // namespace swc

#endif // SWC_CORE_PRESETS_H
