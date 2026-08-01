/*
 * tests/test_validation.cpp — HardwareResourceValidator + RuntimeConfig +
 * presets. Covers Section 18 "Validation": GPIO duplicates, input-only,
 * reserved pins, ADC/WiFi, LEDC, PCA, min/max ranges, default config validity,
 * and that every preset validates clean.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/RuntimeConfig.h"
#include "../esp32/esp32_slide_whistle/core/Presets.h"
#include "../esp32/esp32_slide_whistle/core/PwmOutput.h"

using namespace swc;

TEST(pwm_output_normalized_and_polarity) {
    PwmOutput p; PwmConfig c; c.pin = 26; c.resolution = 12; c.activeHigh = true;
    CHECK(p.attach(c));
    CHECK_EQ(p.maxDuty(), 4095u);
    p.writeNormalized(0.5f);   CHECK_NEAR(p.lastDuty(), 2048, 2);
    p.writeNormalized(2.0f);   CHECK_EQ(p.lastDuty(), 4095u);   // clamped
    // active-low inverts the duty
    PwmOutput q; PwmConfig d = c; d.activeHigh = false; q.attach(d);
    q.writeNormalized(0.0f);   CHECK_EQ(q.lastDuty(), 4095u);   // 0% → full raw when active-low
}

TEST(servo_output_maps_to_microseconds) {
    // Review #13: a servo must get a 1–2 ms pulse, NOT a 0–100% duty at 50 Hz.
    ServoOutput s;
    CHECK(s.attach(26, 1000, 2000, 50, 16));
    s.writeNormalized(0.0f); CHECK_EQ(s.lastUs(), 1000);
    s.writeNormalized(0.5f); CHECK_EQ(s.lastUs(), 1500);
    s.writeNormalized(1.0f); CHECK_EQ(s.lastUs(), 2000);
    s.writeMicroseconds(3000); CHECK_EQ(s.lastUs(), 2000);   // clamped to maxUs
    s.writeMicroseconds(500);  CHECK_EQ(s.lastUs(), 1000);   // clamped to minUs
}

static bool hasCode(const std::vector<ValidationIssue>& v, const char* code) {
    for (const auto& i : v) if (i.code == code) return true;
    return false;
}

TEST(validate_gpio_conflict) {
    HardwareResourceValidator v;
    v.reset();
    v.claimPin(22, true, false, "a"); v.claimPin(22, true, false, "b");
    auto r = v.validate();
    CHECK(hasCode(r, "GPIO_CONFLICT"));
}

TEST(validate_input_only_as_output) {
    HardwareResourceValidator v; v.reset();
    v.claimPin(34, true, false, "out");    // 34 is input-only on WROOM
    CHECK(hasCode(v.validate(), "GPIO_INPUT_ONLY"));
    // but input-only as an INPUT is fine
    HardwareResourceValidator v2; v2.reset();
    v2.claimPin(34, false, false, "in");
    CHECK(!hasCode(v2.validate(), "GPIO_INPUT_ONLY"));
}

TEST(validate_flash_reserved) {
    HardwareResourceValidator v; v.reset();
    v.claimPin(7, true, false, "x");
    CHECK(hasCode(v.validate(), "GPIO_RESERVED_FLASH"));
}

TEST(validate_strapping_is_warning) {
    HardwareResourceValidator v; v.reset();
    v.claimPin(0, true, false, "x");
    auto r = v.validate();
    CHECK(hasCode(r, "GPIO_STRAPPING"));
    CHECK(!HardwareResourceValidator::hasErrors(r));   // warning only, no hard error
}

TEST(validate_adc2_wifi) {
    HardwareResourceValidator v; v.reset(); v.setWifi(true);
    v.claimPin(4, false, true, "sensor");   // GPIO4 is ADC2
    CHECK(hasCode(v.validate(), "ADC2_WIFI"));
    // ADC1 pin is fine
    HardwareResourceValidator v2; v2.reset(); v2.setWifi(true);
    v2.claimPin(36, false, true, "sensor"); // GPIO36 is ADC1
    CHECK(!hasCode(v2.validate(), "ADC2_WIFI"));
}

TEST(validate_ledc_exhaustion) {
    HardwareResourceValidator v; v.reset();
    for (int i = 0; i < 17; ++i) v.claimLedc("ch");   // WROOM has 16
    CHECK(hasCode(v.validate(), "LEDC_EXHAUSTED"));
}

TEST(validate_pca_conflict) {
    HardwareResourceValidator v; v.reset();
    v.claimPca(0x40, 3, "a"); v.claimPca(0x40, 3, "b");
    CHECK(hasCode(v.validate(), "PCA_CONFLICT"));
}

TEST(validate_range) {
    HardwareResourceValidator v; v.reset();
    v.claimRange(100, 10, "flow");
    CHECK(hasCode(v.validate(), "RANGE_INVALID"));
}

TEST(default_config_is_collision_free) {
    RuntimeConfig c = defaultConfig();
    HardwareResourceValidator v; buildClaims(v, c);
    auto r = v.validate();
    CHECK(!HardwareResourceValidator::hasErrors(r));
    CHECK_EQ(c.schemaVersion, (uint32_t)CONFIG_SCHEMA_VERSION);
}

TEST(every_preset_validates_clean) {
    for (uint8_t p = 0; p < (uint8_t)PresetId::COUNT; ++p) {
        RuntimeConfig c = defaultConfig();
        c.instrumentCount = 1;
        c.instruments[0].enabled = true;
        applyPreset(c.instruments[0], (PresetId)p);
        HardwareResourceValidator v; buildClaims(v, c);
        auto r = v.validate();
        if (HardwareResourceValidator::hasErrors(r)) {
            for (const auto& is : r)
                if (is.severity == Severity::Error)
                    std::printf("    preset %u error: %s (%s) %s\n", p,
                                is.code.c_str(), is.field.c_str(), is.message.c_str());
        }
        CHECK(!HardwareResourceValidator::hasErrors(r));
    }
}

TEST(two_instruments_conflict_detected) {
    RuntimeConfig c = defaultConfig();
    c.instrumentCount = 2;
    c.instruments[0].enabled = true; applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
    c.instruments[1].enabled = true; applyPreset(c.instruments[1], PresetId::StepperSolenoidOnly);
    // same preset → same pins on both instruments → conflict
    HardwareResourceValidator v; buildClaims(v, c);
    CHECK(hasCode(v.validate(), "GPIO_CONFLICT"));
}

TEST(ledc_allocator_unique_channels) {
    // Review #3: outputs must get distinct LEDC channels, not all channel 0.
    LedcAllocator a(4);
    CHECK_EQ(a.allocate(), 0);
    CHECK_EQ(a.allocate(), 1);
    CHECK_EQ(a.allocate(), 2);
    CHECK_EQ(a.allocate(), 3);
    CHECK_EQ(a.allocate(), -1);    // exhausted
    a.reset();
    CHECK_EQ(a.allocate(), 0);
}

// Review #3 §9.2: a released channel returns to the pool and is reused (lowest
// free), so a reconfigure doesn't leak channels; used() tracks live count.
TEST(ledc_allocator_release_and_reuse) {
    LedcAllocator a(4);
    CHECK_EQ(a.allocate(), 0);
    CHECK_EQ(a.allocate(), 1);
    CHECK_EQ(a.allocate(), 2);
    CHECK_EQ(a.used(), 3);
    a.release(1);                  // free the middle channel
    CHECK_EQ(a.used(), 2);
    CHECK(!a.isAllocated(1));
    CHECK_EQ(a.allocate(), 1);     // reused, not leaked
    CHECK_EQ(a.allocate(), 3);     // then the last free one
    CHECK_EQ(a.allocate(), -1);    // now full
    a.release(9);                  // out-of-range release is a no-op
    CHECK_EQ(a.used(), 4);
}

// Review #3 §9.3: the S3 has fewer channels — capacity caps allocation.
TEST(ledc_allocator_capacity_cap) {
    LedcAllocator a(16);
    a.setCapacity(8);
    for (int i = 0; i < 8; ++i) CHECK(a.allocate() >= 0);
    CHECK_EQ(a.allocate(), -1);    // 8-channel board is full
}

TEST(validate_required_pins_missing) {
    // Review #24: an enabled stepper with unassigned STEP/DIR is an error.
    RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
    c.instruments[0].enabled = true;
    c.instruments[0].motion.type = SlideDriveType::StepDir;
    c.instruments[0].motion.stepper.stepPin = -1;   // unassigned mandatory pin
    c.instruments[0].motion.stepper.dirPin = -1;
    HardwareResourceValidator v; buildClaims(v, c);
    CHECK(hasCode(v.validate(), "PIN_REQUIRED"));
    // once assigned (a preset), no PIN_REQUIRED
    applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
    HardwareResourceValidator v2; buildClaims(v2, c);
    CHECK(!hasCode(v2.validate(), "PIN_REQUIRED"));
}

TEST(validate_angle_servo_claimed) {
    // Review #25: an enabled angle servo consumes a pin + a LEDC channel.
    RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
    c.instruments[0].enabled = true;
    applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
    c.instruments[0].air.angle.enabled = true;
    c.instruments[0].air.angle.pin = c.instruments[0].motion.stepper.stepPin;  // clash on purpose
    HardwareResourceValidator v; buildClaims(v, c);
    CHECK(hasCode(v.validate(), "GPIO_CONFLICT"));   // angle pin now participates
}

// Review #7 §7: backends the firmware cannot drive must be rejected, not
// silently accepted. A PCA9685 servo/gate/flow output and a ToF or digital
// level sensor all produce UNSUPPORTED_BACKEND.
TEST(validate_unsupported_pca_and_sensor_backends) {
    {   RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
        c.instruments[0].enabled = true;
        applyPreset(c.instruments[0], PresetId::SingleServoMinimalAir);
        c.instruments[0].motion.servoA.backend = PwmBackend::Pca9685;
        HardwareResourceValidator v; buildClaims(v, c);
        CHECK(hasCode(v.validate(), "UNSUPPORTED_BACKEND")); }
    {   RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
        c.instruments[0].enabled = true;
        applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
        c.instruments[0].air.gate.backend = PwmBackend::Pca9685;
        HardwareResourceValidator v; buildClaims(v, c);
        CHECK(hasCode(v.validate(), "UNSUPPORTED_BACKEND")); }
    {   RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
        c.instruments[0].enabled = true;
        applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
        c.instruments[0].air.sensor.type = AirSensorType::TofVL53L0X;
        c.instruments[0].air.sensor.pin = 39;
        HardwareResourceValidator v; buildClaims(v, c);
        CHECK(hasCode(v.validate(), "UNSUPPORTED_BACKEND")); }
    {   RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
        c.instruments[0].enabled = true;
        applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
        c.instruments[0].air.sensor.type = AirSensorType::DigitalLevel;
        c.instruments[0].air.sensor.pin = 39;
        HardwareResourceValidator v; buildClaims(v, c);
        CHECK(hasCode(v.validate(), "UNSUPPORTED_BACKEND")); }
}

// Review #7 §3/§14: the endstop on the homing side and an active GPIO flow
// output are mandatory — an unassigned pin is a hard error.
TEST(validate_homing_endstop_and_flow_pin_required) {
    // Homing toward zero with no min endstop → PIN_REQUIRED.
    RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
    c.instruments[0].enabled = true;
    applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
    c.instruments[0].motion.stepper.homeTowardZero = true;
    c.instruments[0].motion.stepper.endstopMin.pin = -1;   // homing side unassigned
    HardwareResourceValidator v; buildClaims(v, c);
    CHECK(hasCode(v.validate(), "PIN_REQUIRED"));
    // A FlowServo output with no pin is also required.
    RuntimeConfig c2 = defaultConfig(); c2.instrumentCount = 1;
    c2.instruments[0].enabled = true;
    applyPreset(c2.instruments[0], PresetId::StepperSolenoidOnly);
    c2.instruments[0].air.flow.type = FlowControlType::FlowServo;
    c2.instruments[0].air.flow.backend = PwmBackend::Gpio;
    c2.instruments[0].air.flow.pin = -1;                    // no flow pin
    HardwareResourceValidator v2; buildClaims(v2, c2);
    CHECK(hasCode(v2.validate(), "PIN_REQUIRED"));
}

// Review #8 §13: FlowServoAsValve is not actually wired (the gate servo is left
// unattached), so selecting it is UNSUPPORTED_BACKEND rather than a silent
// no-close valve.
TEST(validate_flow_servo_as_valve_unsupported) {
    RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
    c.instruments[0].enabled = true;
    applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
    c.instruments[0].air.gate.type = AirGateType::FlowServoAsValve;
    c.instruments[0].air.gate.pin  = c.instruments[0].air.flow.pin;
    HardwareResourceValidator v; buildClaims(v, c);
    CHECK(hasCode(v.validate(), "UNSUPPORTED_BACKEND"));
}

// Review #8 §19: soft limits must sit inside travel and be ordered; source
// drive levels must be ordered.
TEST(validate_structural_soft_limits_and_levels) {
    // soft max beyond travel
    { RuntimeConfig c = defaultConfig(); c.instrumentCount = 1; c.instruments[0].enabled = true;
      applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
      c.instruments[0].motion.travelMm = 100; c.instruments[0].motion.softMaxMm = 150;
      HardwareResourceValidator v; buildClaims(v, c);
      CHECK(hasCode(v.validate(), "BOUND_INVALID")); }
    // inverted soft window
    { RuntimeConfig c = defaultConfig(); c.instrumentCount = 1; c.instruments[0].enabled = true;
      applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
      c.instruments[0].motion.softMinMm = 60; c.instruments[0].motion.softMaxMm = 40;
      HardwareResourceValidator v; buildClaims(v, c);
      CHECK(hasCode(v.validate(), "RANGE_INVALID")); }
    // source min01 > max01
    { RuntimeConfig c = defaultConfig(); c.instrumentCount = 1; c.instruments[0].enabled = true;
      applyPreset(c.instruments[0], PresetId::StepperFanPwmFlow);
      c.instruments[0].air.source.min01 = 0.9f; c.instruments[0].air.source.max01 = 0.3f;
      HardwareResourceValidator v; buildClaims(v, c);
      CHECK(hasCode(v.validate(), "RANGE_INVALID")); }
}

// Review #7 §14: a FanOnOff source consumes a LEDC channel (it is driven via
// PWM in the air sink), not only FanPwm.
TEST(validate_fan_onoff_claims_ledc) {
    // Four S3 instruments (8 LEDC channels) each with fan + angle + flow servo
    // = 3 LEDC → 12 > 8 with the fan; 2 → 8 (not over) once the fan is removed.
    auto build = [](AirSourceType src) {
        RuntimeConfig c = defaultConfig();
        c.device.board = BoardKind::Esp32S3; c.instrumentCount = 4;
        for (uint8_t n = 0; n < 4; ++n) {
            InstrumentConfig& in = c.instruments[n];
            in = InstrumentConfig{};
            in.enabled = true; in.midiChannel = n + 1;
            in.motion.type = SlideDriveType::Disabled;
            in.air.source.type = src; in.air.source.pin[0] = 10 + n;
            in.air.gate.type = AirGateType::None;
            in.air.flow.type = FlowControlType::FlowServo;
            in.air.flow.backend = PwmBackend::Gpio; in.air.flow.pin = 14 + n;
            in.air.angle.enabled = true; in.air.angle.backend = PwmBackend::Gpio; in.air.angle.pin = 18 + n;
            in.air.sensor.type = AirSensorType::None;
        }
        HardwareResourceValidator v; buildClaims(v, c);
        return v.validate();
    };
    CHECK(hasCode(build(AirSourceType::FanOnOff), "LEDC_EXHAUSTED"));       // 12 > 8
    CHECK(!hasCode(build(AirSourceType::ExternalPassive), "LEDC_EXHAUSTED")); // 8, fan removed
}
