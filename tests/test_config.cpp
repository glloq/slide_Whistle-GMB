/*
 * tests/test_config.cpp — JSON layer + ConfigCodec + ConfigStore.
 * Covers Section 18 "Validation": old JSON config, corrupted config, strings
 * with quotes/backslashes/UTF-8, transactional import, checksum integrity.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/ConfigStore.h"
#include "../esp32/esp32_slide_whistle/core/Presets.h"
#include <map>

using namespace swc;

// --- JSON layer -------------------------------------------------------------
TEST(json_roundtrip_basic) {
    JsonValue o = JsonValue::makeObj();
    o.set("n", 42); o.set("f", 1.5); o.set("b", true); o.set("s", std::string("hi"));
    JsonValue a = JsonValue::makeArr();
    for (int i = 0; i < 3; ++i) { JsonValue v; v.type = JsonValue::Num; v.num = i; a.arr.push_back(v); }
    o.set("arr", a);
    std::string text = jsonDump(o);
    JsonValue back; std::string err;
    CHECK(jsonParse(text, back, &err));
    CHECK_EQ(back.int_or("n", 0), 42);
    CHECK_NEAR(back.num_or("f", 0), 1.5, 1e-9);
    CHECK(back.bool_or("b", false));
    CHECK(back.str_or("s", "") == "hi");
    CHECK_EQ((long)back.find("arr")->arr.size(), 3);
}

TEST(json_escaping_quotes_backslash_utf8) {
    JsonValue o = JsonValue::makeObj();
    o.set("name", std::string("a\"b\\c\nd\tè→😀"));   // quote, backslash, ctrl, UTF-8
    std::string text = jsonDump(o);
    JsonValue back;
    CHECK(jsonParse(text, back, nullptr));
    CHECK(back.str_or("name", "") == "a\"b\\c\nd\tè→😀");   // exact round-trip
}

TEST(json_rejects_garbage) {
    JsonValue v; std::string err;
    CHECK(!jsonParse("{ bad json", v, &err));
    CHECK(!jsonParse("{\"a\":}", v, nullptr));
    CHECK(!jsonParse("", v, nullptr));
    CHECK(!jsonParse("{\"a\":1} trailing", v, nullptr));
}

// --- ConfigCodec ------------------------------------------------------------
TEST(config_roundtrip) {
    RuntimeConfig c = defaultConfig();
    c.instrumentCount = 2;
    c.instruments[0].enabled = true; applyPreset(c.instruments[0], PresetId::StepperSolenoidPwmFlow);
    std::snprintf(c.instruments[0].name, 24, "Soprano \"1\"");
    c.instruments[0].map.setPoint(60, 24.5f, 42);
    c.instruments[1].enabled = true; applyPreset(c.instruments[1], PresetId::SingleServoMinimalAir);
    c.instruments[1].midiChannel = 2;

    std::string json = configToJson(c);
    RuntimeConfig d;
    ConfigDecodeResult r = configFromJson(json, d);
    CHECK(r.ok);
    CHECK(r.checksumOk);
    CHECK(!r.migrated);
    CHECK_EQ(d.instrumentCount, 2);
    CHECK(std::string(d.instruments[0].name) == "Soprano \"1\"");
    CHECK(d.instruments[0].motion.type == SlideDriveType::StepDir);
    CHECK(d.instruments[0].air.gate.type == AirGateType::SolenoidPwm);
    CHECK_EQ(d.instruments[1].midiChannel, 2);
    CHECK(d.instruments[1].motion.type == SlideDriveType::SingleServo);
    float mm = 0; CHECK(d.instruments[0].map.positionForNote(60, mm)); CHECK_NEAR(mm, 24.5f, 1e-3);
}

TEST(config_checksum_detects_tamper) {
    RuntimeConfig c = defaultConfig(); c.instruments[0].enabled = true;
    std::string json = configToJson(c);
    // flip a digit inside the config body without fixing the checksum
    auto pos = json.find("\"channel\":1");
    CHECK(pos != std::string::npos);
    json[pos + 10] = '3';
    RuntimeConfig d;
    ConfigDecodeResult r = configFromJson(json, d);
    CHECK(r.ok);            // still structurally decodable
    CHECK(!r.checksumOk);   // but integrity check fails
}

TEST(config_rejects_bad_ranges) {
    RuntimeConfig c = defaultConfig(); c.instruments[0].enabled = true;
    c.instruments[0].noteMin = 80; c.instruments[0].noteMax = 40;   // invalid
    std::string json = configToJson(c);
    RuntimeConfig d;
    CHECK(!configFromJson(json, d).ok);
}

TEST(migrate_legacy_v3) {
    // Legacy v3 export using NVSKeys field names.
    const char* legacy = R"({
      "schemaVersion": 3,
      "instruments": [
        {"ch":3,"nMin":50,"nMax":80,"enabled":true,"speed":90,"accel":600,
         "pwmFull":255,"pwmHold":128,"legato":1,"cName":"Old Flute",
         "ccBreath":2,"ccVibrato":1,
         "lut":[{"note":60,"mm":30.0,"air":40},{"note":72,"mm":90.0,"air":55}]}
      ]
    })";
    RuntimeConfig d;
    ConfigDecodeResult r = configFromJson(legacy, d);
    CHECK(r.ok);
    CHECK(r.migrated);
    CHECK_EQ(d.schemaVersion, (uint32_t)CONFIG_SCHEMA_VERSION);
    CHECK_EQ(d.instruments[0].midiChannel, 3);
    CHECK_EQ(d.instruments[0].noteMin, 50);
    CHECK(d.instruments[0].motion.type == SlideDriveType::StepDir);
    CHECK(d.instruments[0].air.gate.type == AirGateType::SolenoidPwm);
    CHECK_NEAR(d.instruments[0].air.gate.peak01, 1.0f, 1e-2);
    CHECK_NEAR(d.instruments[0].air.gate.hold01, 0.5f, 2e-2);
    float mm = 0; CHECK(d.instruments[0].map.positionForNote(66, mm)); CHECK_NEAR(mm, 60.0f, 1.0f);
}

// --- ConfigStore ------------------------------------------------------------
struct FakeFs : IConfigFs {
    std::map<std::string, std::string> files;
    bool read(const char* p, std::string& out) override {
        auto it = files.find(p); if (it == files.end()) return false; out = it->second; return true;
    }
    bool write(const char* p, const std::string& d) override { files[p] = d; return true; }
    bool remove(const char* p) override { files.erase(p); return true; }
    bool exists(const char* p) override { return files.count(p) > 0; }
    bool rename(const char* a, const char* b) override {
        auto it = files.find(a); if (it == files.end()) return false;
        files[b] = it->second; files.erase(it); return true;
    }
};

TEST(store_save_and_load_primary) {
    FakeFs fs; ConfigStore st; st.begin(&fs);
    RuntimeConfig c = defaultConfig(); c.instruments[0].enabled = true; c.instruments[0].midiChannel = 5;
    CHECK(st.save(c));
    CHECK(fs.exists(ConfigStore::MAIN));
    CHECK(!fs.exists(ConfigStore::TMP));      // temp cleaned up via rename
    RuntimeConfig d;
    CHECK(st.load(d) == LoadOutcome::Primary);
    CHECK_EQ(d.instruments[0].midiChannel, 5);
}

TEST(store_recovers_from_corrupt_primary) {
    FakeFs fs; ConfigStore st; st.begin(&fs);
    RuntimeConfig c = defaultConfig(); c.instruments[0].midiChannel = 7;
    st.save(c);                               // primary + (no bak yet)
    c.instruments[0].midiChannel = 9;
    st.save(c);                               // now bak holds the ch=7 version, main ch=9
    fs.files[ConfigStore::MAIN] = "{ this is corrupt";   // trash the primary
    RuntimeConfig d;
    CHECK(st.load(d) == LoadOutcome::Backup); // falls back to backup
    CHECK_EQ(d.instruments[0].midiChannel, 7);
}

TEST(store_default_when_all_lost) {
    FakeFs fs; ConfigStore st; st.begin(&fs);
    RuntimeConfig d;
    CHECK(st.load(d) == LoadOutcome::Default);
    HardwareResourceValidator v; buildClaims(v, d);
    CHECK(!HardwareResourceValidator::hasErrors(v.validate()));   // safe default
}

TEST(store_import_transactional) {
    FakeFs fs; ConfigStore st; st.begin(&fs);
    RuntimeConfig current = defaultConfig(); current.instruments[0].midiChannel = 1;
    st.save(current);
    RuntimeConfig out = current; std::string err;
    // invalid import must NOT change stored config
    CHECK(!st.importJson("{ broken", out, err));
    CHECK(!err.empty());
    RuntimeConfig check; st.load(check);
    CHECK_EQ(check.instruments[0].midiChannel, 1);   // unchanged
    // valid import persists
    RuntimeConfig good = defaultConfig(); good.instruments[0].midiChannel = 8;
    std::string gj = st.exportJson(good);
    CHECK(st.importJson(gj, out, err));
    st.load(check);
    CHECK_EQ(check.instruments[0].midiChannel, 8);
}

TEST(store_factory_reset) {
    FakeFs fs; ConfigStore st; st.begin(&fs);
    RuntimeConfig c = defaultConfig(); c.instruments[0].midiChannel = 6; st.save(c);
    RuntimeConfig d;
    CHECK(st.factoryReset(d));
    CHECK(!fs.exists(ConfigStore::BAK));
    RuntimeConfig e; st.load(e);
    CHECK_EQ(e.schemaVersion, (uint32_t)CONFIG_SCHEMA_VERSION);
}
