/*
 * tests/test_validation.cpp — HardwareResourceValidator + RuntimeConfig +
 * presets. Covers Section 18 "Validation": GPIO duplicates, input-only,
 * reserved pins, ADC/WiFi, LEDC, PCA, min/max ranges, default config validity,
 * and that every preset validates clean.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/RuntimeConfig.h"
#include "../esp32/esp32_slide_whistle/core/Presets.h"

using namespace swc;

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
