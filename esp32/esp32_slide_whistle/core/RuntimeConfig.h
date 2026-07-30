/*
 * core/RuntimeConfig.h — versioned runtime configuration + whole-config
 * validation entry point.
 *
 * Section 10: a schema-versioned RuntimeConfig describing device, network,
 * MIDI and up to MAX_INSTRUMENTS instruments, each fully self-describing
 * (motion + air + sequencer + CC map + calibration + safety).
 *
 * The default configuration leaves every hardware pin UNASSIGNED (-1) so it is
 * guaranteed collision-free and forces the user to pick pins before enabling an
 * instrument (Section 11 / correction #21).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_RUNTIMECONFIG_H
#define SWC_CORE_RUNTIMECONFIG_H

#include "SlideActuators.h"
#include "AirSystem.h"
#include "NoteSequencer.h"
#include "NoteMap.h"
#include "HardwareValidator.h"

namespace swc {

static constexpr uint32_t CONFIG_SCHEMA_VERSION = 4;   // v3 (legacy NVS) → v4

struct CcMap {
    uint8_t breath = 2, expression = 11, volume = 7, vibrato = 1, sustain = 64, angle = 74;
    bool    vibratoEnabled = true;   // if false, CC1 does NOT drive vibrato (correction #9)
};

struct DeviceConfig {
    char      name[24] = "Slide Whistle";
    BoardKind board    = BoardKind::Esp32Wroom;
};

struct NetworkConfig {
    char apSsid[24]   = "SlideWhistle";
    bool apEnabled    = true;
    bool apPasswordGenerated = true;   // generated at first boot, never a fixed default
    bool requireAuth  = true;          // critical commands require the admin token
    bool disableApWhenConnected = false;
    char allowedOrigin[48] = "";       // if set, only this Origin may POST (#32)
};

struct MidiConfig {
    bool din = true, ble = true, rtp = false, usb = false;   // usb only on S2/S3
    bool webKeyboard = true;
    int8_t transpose = 0;
};

struct InstrumentConfig {
    bool     enabled = false;
    char     name[24] = "Flute";
    uint8_t  midiChannel = 1;   // 0 = omni
    uint8_t  noteMin = 48, noteMax = 84;
    SlideMotionConfig motion;
    AirConfig         air;
    SequencerConfig   seq;
    CcMap             cc;
    NoteMap           map;      // calibration table
    uint32_t watchdogMs = 30000; // long-note-safe: not driven by MIDI traffic alone
};

struct RuntimeConfig {
    uint32_t       schemaVersion = CONFIG_SCHEMA_VERSION;
    DeviceConfig   device;
    NetworkConfig  network;
    MidiConfig     midi;
    InstrumentConfig instruments[MAX_INSTRUMENTS];
    uint8_t        instrumentCount = 1;
};

// Collision-free factory default: one instrument, disabled, pins unassigned.
inline RuntimeConfig defaultConfig() {
    RuntimeConfig c;
    c.instrumentCount = 1;
    InstrumentConfig& i = c.instruments[0];
    i.enabled = false;
    i.motion.type = SlideDriveType::Disabled;   // safe until configured
    i.motion.stepper.stepPin = i.motion.stepper.dirPin = i.motion.stepper.enablePin = -1;
    i.motion.stepper.endstopMin.pin = -1;
    i.motion.servoA.pin = i.motion.servoB.pin = -1;
    i.air.source.type = AirSourceType::ExternalPassive;
    i.air.gate.type   = AirGateType::None;
    return c;
}

inline void claimServo(HardwareResourceValidator& v, const ServoMotionConfig& s,
                       const std::string& base, const std::string& owner);

// ---------------------------------------------------------------------------
// Whole-config validation: turn a RuntimeConfig into hardware claims and run
// the validator (Section 11). Only ENABLED instruments claim resources.
// ---------------------------------------------------------------------------
inline void buildClaims(HardwareResourceValidator& v, const RuntimeConfig& c) {
    v.reset();
    v.setBoard(c.device.board == BoardKind::Esp32S3 ? BoardProfile::s3() : BoardProfile::wroom());
    for (uint8_t n = 0; n < c.instrumentCount && n < MAX_INSTRUMENTS; ++n) {
        const InstrumentConfig& in = c.instruments[n];
        if (!in.enabled) continue;
        std::string base = "instruments[" + std::to_string(n) + "]";
        std::string own  = std::string("instrument ") + std::to_string(n);

        // --- motion ---
        switch (in.motion.type) {
            case SlideDriveType::StepDir: {
                const auto& s = in.motion.stepper;
                v.requirePin(s.stepPin, true, false, base + ".motion.stepper.stepPin", own + " step");  // mandatory
                v.requirePin(s.dirPin, true, false, base + ".motion.stepper.dirPin", own + " dir");     // mandatory
                v.claimPin(s.enablePin, true, false, base + ".motion.stepper.enablePin", own + " enable");  // optional
                v.claimPin(s.endstopMin.pin, false, false, base + ".motion.stepper.endstopMin.pin", own + " endstop");
                if (s.endstopMax.present)
                    v.claimPin(s.endstopMax.pin, false, false, base + ".motion.stepper.endstopMax.pin", own + " endstop2");
                break;
            }
            case SlideDriveType::SingleServo:
                claimServo(v, in.motion.servoA, base + ".motion.servoA", own + " servo");
                break;
            case SlideDriveType::DualServo:
                claimServo(v, in.motion.servoA, base + ".motion.servoA", own + " servoA");
                claimServo(v, in.motion.servoB, base + ".motion.servoB", own + " servoB");
                break;
            case SlideDriveType::Disabled: break;
        }

        // --- air source pins (fan/pumps) ---
        {
            const auto& src = in.air.source;
            if (src.type == AirSourceType::FanOnOff || src.type == AirSourceType::FanPwm) {
                v.requirePin(src.pin[0], true, false, base + ".air.source.fanPin", own + " fan");
                if (src.type == AirSourceType::FanPwm) v.claimLedc(base + ".air.source.fanLedc");
            } else if (src.type == AirSourceType::PumpsDirect || src.type == AirSourceType::PumpsTank) {
                for (uint8_t p = 0; p < src.pumpCount && p < MAX_PUMPS; ++p) {
                    v.requirePin(src.pin[p], true, false, base + ".air.source.pump[" + std::to_string(p) + "]", own + " pump");
                    v.claimLedc(base + ".air.source.pumpLedc[" + std::to_string(p) + "]");
                }
                if (src.type == AirSourceType::PumpsTank)
                    v.claimRange((long)src.lowThresh, (long)src.highThresh, base + ".air.source.thresholds");
            }
        }

        // --- gate pin ---
        // FlowServoAsValve reuses the flow servo, so the flow claim already
        // covers the pin — do not claim it twice.
        if (in.air.gate.type != AirGateType::None &&
            in.air.gate.type != AirGateType::FlowServoAsValve) {
            if (in.air.gate.backend == PwmBackend::Pca9685)
                v.claimPca(0x40, in.air.gate.pcaChannel, base + ".air.gate.pcaChannel");
            else {
                v.requirePin(in.air.gate.pin, true, false, base + ".air.gate.pin", own + " gate");
                // solenoid PWM AND every servo gate (valve/diverter) need a LEDC
                // channel; only the plain on/off solenoid does not (review #25).
                if (in.air.gate.type != AirGateType::SolenoidSimple)
                    v.claimLedc(base + ".air.gate.ledc");
            }
        }

        // --- jet-angle servo (independent output, review #25) ---
        if (in.air.angle.enabled) {
            if (in.air.angle.backend == PwmBackend::Pca9685)
                v.claimPca(0x40, in.air.angle.pcaChannel, base + ".air.angle.pcaChannel");
            else {
                v.requirePin(in.air.angle.pin, true, false, base + ".air.angle.pin", own + " angle");
                v.claimLedc(base + ".air.angle.ledc");
            }
        }

        // --- flow pin ---
        if (in.air.flow.type != FlowControlType::None) {
            if (in.air.flow.backend == PwmBackend::Pca9685)
                v.claimPca(0x40, in.air.flow.pcaChannel, base + ".air.flow.pcaChannel");
            else if (in.air.flow.pin >= 0) {
                v.claimPin(in.air.flow.pin, true, false, base + ".air.flow.pin", own + " flow");
                v.claimLedc(base + ".air.flow.ledc");
            }
            v.claimRange(in.air.flow.min, in.air.flow.max, base + ".air.flow.range");
        }

        // --- sensor pin (analog constrained to ADC1 when WiFi is on) ---
        if (in.air.sensor.type == AirSensorType::PressureAnalog ||
            in.air.sensor.type == AirSensorType::HallAnalog) {
            // A tank source that requires the sensor MUST have its pin assigned.
            if (in.air.source.type == AirSourceType::PumpsTank && in.air.source.requireSensor)
                v.requirePin(in.air.sensor.pin, false, true, base + ".air.sensor.pin", own + " sensor");
            else
                v.claimPin(in.air.sensor.pin, false, true, base + ".air.sensor.pin", own + " sensor");
            v.claimRange((long)in.air.sensor.physLo, (long)in.air.sensor.physHi, base + ".air.sensor.range");
        } else if (in.air.sensor.type == AirSensorType::EndstopMechanical ||
                   in.air.sensor.type == AirSensorType::EndstopOptical ||
                   in.air.sensor.type == AirSensorType::DigitalLevel) {
            v.claimPin(in.air.sensor.pin, false, false, base + ".air.sensor.pin", own + " sensor");
        }

        // --- servo backends via PCA9685 ---
        if (in.motion.type == SlideDriveType::SingleServo || in.motion.type == SlideDriveType::DualServo) {
            if (in.motion.servoA.backend == PwmBackend::Pca9685)
                v.claimPca(0x40, in.motion.servoA.pcaChannel, base + ".motion.servoA.pcaChannel");
            if (in.motion.type == SlideDriveType::DualServo && in.motion.servoB.backend == PwmBackend::Pca9685)
                v.claimPca(0x40, in.motion.servoB.pcaChannel, base + ".motion.servoB.pcaChannel");
        }
    }
}

inline void claimServo(HardwareResourceValidator& v, const ServoMotionConfig& s,
                       const std::string& base, const std::string& owner) {
    if (s.backend == PwmBackend::Gpio) {
        v.requirePin(s.pin, true, false, base + ".pin", owner);   // servo pin mandatory (#24)
        v.claimLedc(base + ".ledc");
    }
    v.claimRange(s.minUs, s.maxUs, base + ".pulse");
}

} // namespace swc

#endif // SWC_CORE_RUNTIMECONFIG_H
