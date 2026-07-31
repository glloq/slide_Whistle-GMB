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
    c.instruments[0].map.setPoint(60, 34.0f, 42);   // monotonic w.r.t. the provisional table
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
    float mm = 0; CHECK(d.instruments[0].map.positionForNote(60, mm)); CHECK_NEAR(mm, 34.0f, 1e-3);
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

// Review #3 §11.3: an integer that would WRAP through narrowing into a valid
// window must be rejected pre-narrow, not silently accepted. channel:256 → 0
// (=OMNI) is the canonical case; it passes the post-narrow `>16` check.
TEST(config_rejects_channel_overflow_prenarrow) {
    RuntimeConfig c = defaultConfig();
    c.instruments[0].enabled = true; c.instruments[0].midiChannel = 9;
    std::string json = configToJson(c);
    auto pos = json.find("\"channel\":9");
    CHECK(pos != std::string::npos);
    json.replace(pos, std::string("\"channel\":9").size(), "\"channel\":256");
    RuntimeConfig d;
    CHECK(!configFromJson(json, d).ok);       // must NOT wrap to OMNI and pass
}

// Review #3 §11.4: non-positive dynamics and inverted servo pulse windows are
// rejected by validateStructural.
TEST(config_rejects_nonpositive_dynamics) {
    RuntimeConfig c = defaultConfig();
    c.instruments[0].motion.type = SlideDriveType::StepDir;
    c.instruments[0].motion.maxSpeedMmS = 0.0f;     // invalid for a driven axis
    RuntimeConfig d;
    CHECK(!configFromJson(configToJson(c), d).ok);
}

TEST(config_rejects_inverted_servo_pulse) {
    RuntimeConfig c = defaultConfig();
    c.instruments[0].motion.servoA.minUs = 2000;
    c.instruments[0].motion.servoA.maxUs = 1000;     // min >= max
    RuntimeConfig d;
    CHECK(!configFromJson(configToJson(c), d).ok);
}

TEST(config_rejects_out_of_range_normalized) {
    RuntimeConfig c = defaultConfig();
    c.instruments[0].air.source.max01 = 1.5f;        // not in 0..1
    RuntimeConfig d;
    CHECK(!configFromJson(configToJson(c), d).ok);
}

// Review #5 §21: servo pulse-width fields are range-checked before narrowing.
// Review #6 §14: an enum value that would narrow (256→0) into a valid enum is
// rejected pre-narrow, not silently accepted.
TEST(config_rejects_out_of_range_enum) {
    RuntimeConfig c = defaultConfig();
    std::string j = configToJson(c);
    auto p = j.find("\"type\":0,\"travelMm\"");   // motion.type
    CHECK(p != std::string::npos);
    j.replace(p, std::string("\"type\":0").size(), "\"type\":256");
    RuntimeConfig d;
    CHECK(!configFromJson(j, d).ok);
}

// Review #6 §14: an out-of-range instrumentCount is rejected, not clamped.
TEST(config_rejects_instrument_count_overflow) {
    RuntimeConfig c = defaultConfig();
    std::string j = configToJson(c);
    auto p = j.find("\"instrumentCount\":1");
    CHECK(p != std::string::npos);
    j.replace(p, std::string("\"instrumentCount\":1").size(), "\"instrumentCount\":99");
    RuntimeConfig d;
    CHECK(!configFromJson(j, d).ok);
}

TEST(config_rejects_out_of_range_servo_pulse) {
    RuntimeConfig c = defaultConfig();
    c.instruments[0].motion.type = SlideDriveType::SingleServo;
    c.instruments[0].motion.servoA.restUs = 5000;    // out of 100..3000
    RuntimeConfig d;
    CHECK(!configFromJson(configToJson(c), d).ok);
}

TEST(config_rejects_transpose_overflow) {
    RuntimeConfig c = defaultConfig();
    std::string json = configToJson(c);
    auto pos = json.find("\"transpose\":");
    CHECK(pos != std::string::npos);
    auto end = json.find_first_of(",}", pos);
    json.replace(pos, end - pos, "\"transpose\":200");   // wraps int8_t otherwise
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

// Review #18: field-by-field round-trip of the FULL configuration surface.
TEST(config_roundtrip_full_fields) {
    RuntimeConfig c = defaultConfig();
    c.instrumentCount = 1;
    InstrumentConfig& i = c.instruments[0];
    i.enabled = true; i.midiChannel = 4; i.noteMin = 40; i.noteMax = 90; i.watchdogMs = 12345;
    // motion / stepper — every field
    i.motion.type = SlideDriveType::StepDir;
    i.motion.travelMm = 137.5f; i.motion.maxSpeedMmS = 95.0f; i.motion.accelMmS2 = 640.0f;
    i.motion.softMinMm = 2.0f; i.motion.softMaxMm = 135.0f;
    auto& s = i.motion.stepper;
    s.stepPin = 32; s.dirPin = 33; s.enablePin = 25; s.enableActiveHigh = true; s.invertDir = true;
    s.stepsPerRev = 400; s.microsteps = 32; s.stepsPerMm = 123.4f;
    s.homingFastMmS = 44.0f; s.homingSlowMmS = 6.5f; s.homeTowardZero = false;
    s.homeOffsetMm = 7.25f; s.homeBackoffMm = 4.5f; s.phaseTimeoutMs = 9000; s.idleDisableMs = 15000;
    s.alwaysHold = true;
    s.endstopMin = {true, true, true, false, 34};
    s.endstopMax = {true, false, false, true, 35};
    // servoA multipoint + trim/offset
    auto& sa = i.motion.servoA;
    sa.backend = PwmBackend::Pca9685; sa.pcaChannel = 5; sa.freqHz = 330;
    sa.minUs = 700; sa.maxUs = 2300; sa.invert = true; sa.restUs = 1400; sa.safeUs = 1450;
    sa.trimUs = -20; sa.offsetUs = 15; sa.detachIdleMs = 8000;
    sa.cal[0] = {0.0f, 700}; sa.cal[1] = {60.0f, 1500}; sa.cal[2] = {135.0f, 2300}; sa.calCount = 3;
    i.motion.dualMode = DualSyncMode::MasterSlave; i.motion.servoBEnabled = false;
    // air — source (tank), gate, flow, angle, sensor
    auto& so = i.air.source;
    so.type = AirSourceType::PumpsTank; so.pumpCount = 3; so.pin[0]=16; so.pin[1]=17; so.pin[2]=18;
    so.idle01=0.1f; so.min01=0.2f; so.max01=0.95f; so.startBoost01=0.3f;
    so.spinUpMs=250; so.stopDelayMs=600; so.cascadeDelayMs=90;
    so.tankMode = TankRegulationMode::Level; so.tankPwm=false; so.target=70;
    so.requireSensor=true; so.lowThresh=45; so.highThresh=85; so.safetyThresh=110;
    so.refillTimeoutMs=7000; so.minOffMs=400;
    auto& g = i.air.gate;
    g.type = AirGateType::ServoValve; g.pin=27; g.backend=PwmBackend::Gpio; g.pcaChannel=2;
    g.activeHigh=false; g.openTimeoutMs=1500; g.peak01=0.9f; g.peakMs=35; g.hold01=0.35f;
    g.closed01=0.15f; g.open01=0.85f; g.openDelayMs=25; g.closeDelayMs=18;
    auto& f = i.air.flow;
    f.type = FlowControlType::FlowServo; f.pin=26; f.backend=PwmBackend::Pca9685; f.pcaChannel=7;
    f.min=8; f.nominal=70; f.max=118; f.rest01=0.05f; f.curve=VelocityCurve::Exponential;
    f.expo=2.6f; f.maxSlewPerMs=0.03f;
    auto& an = i.air.angle;
    an.enabled=true; an.pin=13; an.backend=PwmBackend::Gpio; an.pcaChannel=1;
    an.rest01=0.4f; an.min01=0.1f; an.nominal01=0.5f; an.max01=0.9f; an.useCc74=false;
    auto& sc = i.air.sensor;
    sc.type=AirSensorType::TofVL53L0X; sc.pin=36; sc.i2cAddr=0x29;
    sc.rawMin=10; sc.rawMax=4000; sc.physMin=1; sc.physMax=99; sc.invert=true;
    sc.filterAlpha=0.22f; sc.staleTimeoutMs=750; sc.physLo=2; sc.physHi=115;
    i.air.valveOpenTimeoutMs=2500; i.air.minNoteMs=30;
    // seq — full legato/timings/vibrato
    i.seq.mono = MonoPolicy::HighestNote; i.seq.legato = LegatoPolicy::HoldWithinDistance;
    i.seq.legatoMaxDistanceMm=9.5f; i.seq.legatoMaxTimeMs=140; i.seq.legatoMaxMoveTimeMs=55.0f;
    i.seq.minNoteMs=20; i.seq.prepareTimeoutMs=3500; i.seq.vibratoUnit=VibratoUnit::Millimetre;
    i.seq.vibratoRateHz=6.2f;
    // calibration with full per-note window
    i.map.setPoint(60, 42.5f, 55); { NoteEntry& e = i.map.entry(60); e.positionToleranceMm=0.7f; e.airMin=30; e.airMax=90; }

    std::string json = configToJson(c);
    RuntimeConfig d;
    CHECK(configFromJson(json, d).ok);
    const InstrumentConfig& o = d.instruments[0];
    // spot-check across every subsystem
    CHECK_EQ(o.watchdogMs, 12345u);
    CHECK_NEAR(o.motion.travelMm, 137.5f, 1e-3);
    CHECK_EQ(o.motion.stepper.stepsPerRev, 400);
    CHECK_EQ(o.motion.stepper.microsteps, 32);
    CHECK_NEAR(o.motion.stepper.homeOffsetMm, 7.25f, 1e-3);
    CHECK_EQ(o.motion.stepper.idleDisableMs, 15000u);
    CHECK(o.motion.stepper.endstopMax.present);
    CHECK_EQ(o.motion.stepper.endstopMax.pin, 35);
    CHECK(o.motion.stepper.endstopMin.normallyClosed);
    CHECK_EQ(o.motion.servoA.pcaChannel, 5);
    CHECK_EQ(o.motion.servoA.freqHz, 330);
    CHECK_EQ(o.motion.servoA.trimUs, -20);
    CHECK_EQ(o.motion.servoA.calCount, 3);
    CHECK_NEAR(o.motion.servoA.cal[1].mm, 60.0f, 1e-3);
    CHECK(!o.motion.servoBEnabled);
    CHECK(o.air.source.type == AirSourceType::PumpsTank);
    CHECK_EQ(o.air.source.pin[2], 18);
    CHECK(o.air.source.tankMode == TankRegulationMode::Level);
    CHECK_EQ(o.air.source.refillTimeoutMs, 7000u);
    CHECK_NEAR(o.air.gate.hold01, 0.35f, 1e-3);
    CHECK_EQ(o.air.gate.peakMs, 35u);
    CHECK_NEAR(o.air.gate.open01, 0.85f, 1e-3);
    CHECK_EQ(o.air.flow.pcaChannel, 7);
    CHECK(o.air.flow.curve == VelocityCurve::Exponential);
    CHECK_NEAR(o.air.flow.expo, 2.6f, 1e-3);
    CHECK(o.air.angle.enabled); CHECK_EQ(o.air.angle.pin, 13);
    CHECK(o.air.sensor.type == AirSensorType::TofVL53L0X);
    CHECK_EQ(o.air.sensor.i2cAddr, 0x29);
    CHECK_NEAR(o.air.sensor.filterAlpha, 0.22f, 1e-3);
    CHECK_EQ(o.air.valveOpenTimeoutMs, 2500u);
    CHECK(o.seq.mono == MonoPolicy::HighestNote);
    CHECK(o.seq.legato == LegatoPolicy::HoldWithinDistance);
    CHECK_NEAR(o.seq.legatoMaxDistanceMm, 9.5f, 1e-3);
    CHECK_EQ(o.seq.prepareTimeoutMs, 3500u);
    CHECK(o.seq.vibratoUnit == VibratoUnit::Millimetre);
    const NoteEntry& e = o.map.entry(60);
    CHECK_NEAR(e.positionMm, 42.5f, 1e-3);
    CHECK_NEAR(e.positionToleranceMm, 0.7f, 1e-3);
    CHECK_EQ(e.airMin, 30); CHECK_EQ(e.airMax, 90);
}

// Review #7: a preset's provisional linear table survives export→import→save
// and stays playable.
TEST(preset_provisional_table_survives_roundtrip) {
    RuntimeConfig c = defaultConfig(); c.instrumentCount = 1;
    c.instruments[0].enabled = true;
    applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);   // fills a provisional table
    float mm0 = 0; CHECK(c.instruments[0].map.positionForNote(60, mm0));   // playable in-memory
    std::string json = configToJson(c);
    RuntimeConfig d;
    CHECK(configFromJson(json, d).ok);
    float mm1 = 0; CHECK(d.instruments[0].map.positionForNote(60, mm1));   // still playable
    CHECK_NEAR(mm1, mm0, 1e-3);
    CHECK(!d.instruments[0].map.entry(60).calibrated);   // still flagged provisional
}

// Review #26: an out-of-range enum is rejected, not silently cast.
TEST(config_rejects_bad_enum) {
    const char* bad = R"({"schemaVersion":4,"instrumentCount":1,"instruments":[
        {"enabled":true,"channel":1,"noteMin":48,"noteMax":84,
         "motion":{"type":99,"travelMm":100}}]})";
    RuntimeConfig d;
    CHECK(!configFromJson(bad, d).ok);
}

// Review #28: a future schema version is refused.
TEST(config_rejects_future_schema) {
    const char* fut = R"({"schemaVersion":9999,"instrumentCount":1,"instruments":[
        {"enabled":false,"channel":1,"noteMin":48,"noteMax":84}]})";
    RuntimeConfig d;
    ConfigDecodeResult r = configFromJson(fut, d);
    CHECK(!r.ok);
}

// Review #27: min<=nominal<=max and tank thresholds enforced.
TEST(config_rejects_flow_and_tank_ranges) {
    RuntimeConfig c = defaultConfig(); c.instruments[0].enabled = true;
    c.instruments[0].air.flow.min = 100; c.instruments[0].air.flow.nominal = 10; c.instruments[0].air.flow.max = 120;
    RuntimeConfig d;
    CHECK(!configFromJson(configToJson(c), d).ok);        // nominal < min
}

// Review #29: an import with a wrong checksum is rejected.
TEST(store_import_rejects_bad_checksum) {
    FakeFs fs; ConfigStore st; st.begin(&fs);
    RuntimeConfig c = defaultConfig(); c.instruments[0].enabled = true;
    std::string json = st.exportJson(c);
    auto pos = json.find("\"channel\":1");
    CHECK(pos != std::string::npos);
    json[pos + 10] = '5';                                 // tamper the body, checksum now stale
    RuntimeConfig out; std::string err;
    CHECK(!st.importJson(json, out, err));
    CHECK(err == "checksum mismatch");
}

// Review #22: air-servo pulse windows (gate/flow/angle) round-trip.
TEST(config_air_servo_us_roundtrip) {
    RuntimeConfig c = defaultConfig(); c.instruments[0].enabled = true;
    auto& a = c.instruments[0].air;
    a.gate.type = AirGateType::ServoValve; a.gate.pin = 27;
    a.gate.servoMinUs = 800; a.gate.servoMaxUs = 2200;
    a.flow.type = FlowControlType::FlowServo; a.flow.pin = 26;
    a.flow.servoMinUs = 900; a.flow.servoMaxUs = 2100;
    a.angle.enabled = true; a.angle.pin = 13; a.angle.servoMinUs = 1100; a.angle.servoMaxUs = 1900;
    RuntimeConfig d;
    CHECK(configFromJson(configToJson(c), d).ok);
    CHECK_EQ(d.instruments[0].air.gate.servoMinUs, 800);
    CHECK_EQ(d.instruments[0].air.gate.servoMaxUs, 2200);
    CHECK_EQ(d.instruments[0].air.flow.servoMinUs, 900);
    CHECK_EQ(d.instruments[0].air.angle.servoMaxUs, 1900);
}
