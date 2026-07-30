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
