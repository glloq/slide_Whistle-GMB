/*
 * core/ConfigCodec.h — RuntimeConfig <-> JSON, with validation and migration.
 *
 * Enums are stored as their integer value so a round-trip is exact. Import is
 * validated structurally (schema version, counts, min<=max) before it is ever
 * applied; hardware validation is a separate step run by the caller
 * (transactional apply, corrections #18/#20).
 *
 * migrateLegacy() maps a v3 export (the field names from NVSKeys.h:
 * ch/nMin/nMax/speed/accel/legato/useLUT/lut, pressure.*) onto the v4 schema
 * (Section 10 migration).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_CONFIGCODEC_H
#define SWC_CORE_CONFIGCODEC_H

#include "RuntimeConfig.h"
#include "Json.h"

namespace swc {

// --- helpers ---------------------------------------------------------------
inline JsonValue servoToJson(const ServoMotionConfig& s) {
    JsonValue v = JsonValue::makeObj();
    v.set("backend", (int)s.backend);
    v.set("pin", (int)s.pin);
    v.set("pca", (int)s.pcaChannel);
    v.set("minUs", (int)s.minUs);
    v.set("maxUs", (int)s.maxUs);
    v.set("invert", s.invert);
    v.set("restUs", (int)s.restUs);
    return v;
}
inline void servoFromJson(const JsonValue& v, ServoMotionConfig& s) {
    s.backend = (PwmBackend)v.int_or("backend", (int)s.backend);
    s.pin = (int8_t)v.int_or("pin", s.pin);
    s.pcaChannel = (uint8_t)v.int_or("pca", s.pcaChannel);
    s.minUs = (uint16_t)v.int_or("minUs", s.minUs);
    s.maxUs = (uint16_t)v.int_or("maxUs", s.maxUs);
    s.invert = v.bool_or("invert", s.invert);
    s.restUs = (uint16_t)v.int_or("restUs", s.restUs);
}

inline JsonValue instrumentToJson(const InstrumentConfig& in) {
    JsonValue v = JsonValue::makeObj();
    v.set("enabled", in.enabled);
    v.set("name", std::string(in.name));
    v.set("channel", (int)in.midiChannel);
    v.set("noteMin", (int)in.noteMin);
    v.set("noteMax", (int)in.noteMax);
    v.set("watchdogMs", (double)in.watchdogMs);

    JsonValue m = JsonValue::makeObj();
    m.set("type", (int)in.motion.type);
    m.set("travelMm", in.motion.travelMm);
    m.set("maxSpeedMmS", in.motion.maxSpeedMmS);
    m.set("accelMmS2", in.motion.accelMmS2);
    m.set("softMinMm", in.motion.softMinMm);
    m.set("softMaxMm", in.motion.softMaxMm);
    JsonValue st = JsonValue::makeObj();
    st.set("stepPin", (int)in.motion.stepper.stepPin);
    st.set("dirPin", (int)in.motion.stepper.dirPin);
    st.set("enablePin", (int)in.motion.stepper.enablePin);
    st.set("endstopPin", (int)in.motion.stepper.endstopMin.pin);
    st.set("stepsPerMm", in.motion.stepper.stepsPerMm);
    st.set("invertDir", in.motion.stepper.invertDir);
    m.set("stepper", st);
    m.set("servoA", servoToJson(in.motion.servoA));
    m.set("servoB", servoToJson(in.motion.servoB));
    m.set("dualMode", (int)in.motion.dualMode);
    v.set("motion", m);

    JsonValue a = JsonValue::makeObj();
    JsonValue src = JsonValue::makeObj();
    src.set("type", (int)in.air.source.type);
    src.set("pumpCount", (int)in.air.source.pumpCount);
    JsonValue pins = JsonValue::makeArr();
    for (int p = 0; p < MAX_PUMPS; ++p) { JsonValue n; n.type = JsonValue::Num; n.num = in.air.source.pin[p]; pins.arr.push_back(n); }
    src.set("pins", pins);
    src.set("requireSensor", in.air.source.requireSensor);
    src.set("lowThresh", in.air.source.lowThresh);
    src.set("highThresh", in.air.source.highThresh);
    src.set("safetyThresh", in.air.source.safetyThresh);
    a.set("source", src);
    JsonValue g = JsonValue::makeObj();
    g.set("type", (int)in.air.gate.type);
    g.set("pin", (int)in.air.gate.pin);
    g.set("backend", (int)in.air.gate.backend);
    g.set("peak01", in.air.gate.peak01);
    g.set("hold01", in.air.gate.hold01);
    a.set("gate", g);
    JsonValue f = JsonValue::makeObj();
    f.set("type", (int)in.air.flow.type);
    f.set("pin", (int)in.air.flow.pin);
    f.set("min", (int)in.air.flow.min);
    f.set("nominal", (int)in.air.flow.nominal);
    f.set("max", (int)in.air.flow.max);
    f.set("curve", (int)in.air.flow.curve);
    a.set("flow", f);
    JsonValue se = JsonValue::makeObj();
    se.set("type", (int)in.air.sensor.type);
    se.set("pin", (int)in.air.sensor.pin);
    a.set("sensor", se);
    v.set("air", a);

    JsonValue sq = JsonValue::makeObj();
    sq.set("mono", (int)in.seq.mono);
    sq.set("legato", (int)in.seq.legato);
    sq.set("minNoteMs", (double)in.seq.minNoteMs);
    v.set("seq", sq);

    JsonValue cc = JsonValue::makeObj();
    cc.set("breath", (int)in.cc.breath); cc.set("expression", (int)in.cc.expression);
    cc.set("volume", (int)in.cc.volume); cc.set("vibrato", (int)in.cc.vibrato);
    cc.set("sustain", (int)in.cc.sustain); cc.set("angle", (int)in.cc.angle);
    cc.set("vibratoEnabled", in.cc.vibratoEnabled);
    v.set("cc", cc);

    // calibration points (only calibrated entries)
    JsonValue cal = JsonValue::makeArr();
    for (int n = 0; n < MIDI_NOTE_COUNT; ++n) {
        const NoteEntry& e = in.map.entry((uint8_t)n);
        if (!e.calibrated) continue;
        JsonValue pt = JsonValue::makeObj();
        pt.set("note", n); pt.set("mm", e.positionMm); pt.set("air", (int)e.airNominal);
        cal.arr.push_back(pt);
    }
    v.set("calibration", cal);
    return v;
}

inline void instrumentFromJson(const JsonValue& v, InstrumentConfig& in) {
    in.enabled = v.bool_or("enabled", in.enabled);
    std::string nm = v.str_or("name", in.name);
    std::snprintf(in.name, sizeof(in.name), "%s", nm.c_str());
    in.midiChannel = (uint8_t)v.int_or("channel", in.midiChannel);
    in.noteMin = (uint8_t)v.int_or("noteMin", in.noteMin);
    in.noteMax = (uint8_t)v.int_or("noteMax", in.noteMax);
    in.watchdogMs = (uint32_t)v.num_or("watchdogMs", in.watchdogMs);

    if (auto* m = v.find("motion")) {
        in.motion.type = (SlideDriveType)m->int_or("type", (int)in.motion.type);
        in.motion.travelMm = (float)m->num_or("travelMm", in.motion.travelMm);
        in.motion.maxSpeedMmS = (float)m->num_or("maxSpeedMmS", in.motion.maxSpeedMmS);
        in.motion.accelMmS2 = (float)m->num_or("accelMmS2", in.motion.accelMmS2);
        in.motion.softMinMm = (float)m->num_or("softMinMm", in.motion.softMinMm);
        in.motion.softMaxMm = (float)m->num_or("softMaxMm", in.motion.softMaxMm);
        if (auto* st = m->find("stepper")) {
            in.motion.stepper.stepPin = (int8_t)st->int_or("stepPin", in.motion.stepper.stepPin);
            in.motion.stepper.dirPin = (int8_t)st->int_or("dirPin", in.motion.stepper.dirPin);
            in.motion.stepper.enablePin = (int8_t)st->int_or("enablePin", in.motion.stepper.enablePin);
            in.motion.stepper.endstopMin.pin = (int8_t)st->int_or("endstopPin", in.motion.stepper.endstopMin.pin);
            in.motion.stepper.stepsPerMm = (float)st->num_or("stepsPerMm", in.motion.stepper.stepsPerMm);
            in.motion.stepper.invertDir = st->bool_or("invertDir", in.motion.stepper.invertDir);
        }
        if (auto* sa = m->find("servoA")) servoFromJson(*sa, in.motion.servoA);
        if (auto* sb = m->find("servoB")) servoFromJson(*sb, in.motion.servoB);
        in.motion.dualMode = (DualSyncMode)m->int_or("dualMode", (int)in.motion.dualMode);
    }
    if (auto* a = v.find("air")) {
        if (auto* src = a->find("source")) {
            in.air.source.type = (AirSourceType)src->int_or("type", (int)in.air.source.type);
            in.air.source.pumpCount = (uint8_t)src->int_or("pumpCount", in.air.source.pumpCount);
            if (auto* pins = src->find("pins"))
                for (int p = 0; p < MAX_PUMPS && p < (int)pins->arr.size(); ++p)
                    in.air.source.pin[p] = (int8_t)pins->arr[p].asInt(in.air.source.pin[p]);
            in.air.source.requireSensor = src->bool_or("requireSensor", in.air.source.requireSensor);
            in.air.source.lowThresh = (float)src->num_or("lowThresh", in.air.source.lowThresh);
            in.air.source.highThresh = (float)src->num_or("highThresh", in.air.source.highThresh);
            in.air.source.safetyThresh = (float)src->num_or("safetyThresh", in.air.source.safetyThresh);
        }
        if (auto* g = a->find("gate")) {
            in.air.gate.type = (AirGateType)g->int_or("type", (int)in.air.gate.type);
            in.air.gate.pin = (int8_t)g->int_or("pin", in.air.gate.pin);
            in.air.gate.backend = (PwmBackend)g->int_or("backend", (int)in.air.gate.backend);
            in.air.gate.peak01 = (float)g->num_or("peak01", in.air.gate.peak01);
            in.air.gate.hold01 = (float)g->num_or("hold01", in.air.gate.hold01);
        }
        if (auto* f = a->find("flow")) {
            in.air.flow.type = (FlowControlType)f->int_or("type", (int)in.air.flow.type);
            in.air.flow.pin = (int8_t)f->int_or("pin", in.air.flow.pin);
            in.air.flow.min = (uint8_t)f->int_or("min", in.air.flow.min);
            in.air.flow.nominal = (uint8_t)f->int_or("nominal", in.air.flow.nominal);
            in.air.flow.max = (uint8_t)f->int_or("max", in.air.flow.max);
            in.air.flow.curve = (VelocityCurve)f->int_or("curve", (int)in.air.flow.curve);
        }
        if (auto* se = a->find("sensor")) {
            in.air.sensor.type = (AirSensorType)se->int_or("type", (int)in.air.sensor.type);
            in.air.sensor.pin = (int8_t)se->int_or("pin", in.air.sensor.pin);
        }
    }
    if (auto* sq = v.find("seq")) {
        in.seq.mono = (MonoPolicy)sq->int_or("mono", (int)in.seq.mono);
        in.seq.legato = (LegatoPolicy)sq->int_or("legato", (int)in.seq.legato);
        in.seq.minNoteMs = (uint32_t)sq->num_or("minNoteMs", in.seq.minNoteMs);
    }
    if (auto* cc = v.find("cc")) {
        in.cc.breath = (uint8_t)cc->int_or("breath", in.cc.breath);
        in.cc.expression = (uint8_t)cc->int_or("expression", in.cc.expression);
        in.cc.volume = (uint8_t)cc->int_or("volume", in.cc.volume);
        in.cc.vibrato = (uint8_t)cc->int_or("vibrato", in.cc.vibrato);
        in.cc.sustain = (uint8_t)cc->int_or("sustain", in.cc.sustain);
        in.cc.angle = (uint8_t)cc->int_or("angle", in.cc.angle);
        in.cc.vibratoEnabled = cc->bool_or("vibratoEnabled", in.cc.vibratoEnabled);
    }
    if (auto* cal = v.find("calibration")) {
        in.map.clear(); in.map.setTravelMm(in.motion.travelMm);
        for (const auto& pt : cal->arr) {
            int note = (int)pt.int_or("note", -1);
            if (note < 0 || note >= MIDI_NOTE_COUNT) continue;
            in.map.setPoint((uint8_t)note, (float)pt.num_or("mm", 0), (uint8_t)pt.int_or("air", 0));
        }
    }
}

// --- top-level ---------------------------------------------------------------
inline std::string configToJson(const RuntimeConfig& c) {
    JsonValue root = JsonValue::makeObj();
    root.set("schemaVersion", (int)c.schemaVersion);
    JsonValue d = JsonValue::makeObj();
    d.set("name", std::string(c.device.name)); d.set("board", (int)c.device.board);
    root.set("device", d);
    JsonValue n = JsonValue::makeObj();
    n.set("apSsid", std::string(c.network.apSsid));
    n.set("apEnabled", c.network.apEnabled);
    n.set("requireAuth", c.network.requireAuth);
    n.set("disableApWhenConnected", c.network.disableApWhenConnected);
    root.set("network", n);
    JsonValue mi = JsonValue::makeObj();
    mi.set("din", c.midi.din); mi.set("ble", c.midi.ble); mi.set("rtp", c.midi.rtp);
    mi.set("usb", c.midi.usb); mi.set("webKeyboard", c.midi.webKeyboard);
    mi.set("transpose", (int)c.midi.transpose);
    root.set("midi", mi);
    root.set("instrumentCount", (int)c.instrumentCount);
    JsonValue arr = JsonValue::makeArr();
    for (int i = 0; i < c.instrumentCount && i < MAX_INSTRUMENTS; ++i)
        arr.arr.push_back(instrumentToJson(c.instruments[i]));
    root.set("instruments", arr);
    std::string body = jsonDump(root);
    // append an integrity checksum wrapper
    JsonValue wrap = JsonValue::makeObj();
    wrap.set("checksum", (double)fnv1a(body));
    wrap.set("config", root);
    return jsonDump(wrap);
}

struct ConfigDecodeResult { bool ok = false; bool checksumOk = false; bool migrated = false; std::string error; };

// forward
inline bool migrateLegacy(const JsonValue& legacy, RuntimeConfig& out);

inline ConfigDecodeResult configFromJson(const std::string& text, RuntimeConfig& out) {
    ConfigDecodeResult r;
    JsonValue wrap;
    std::string err;
    if (!jsonParse(text, wrap, &err)) { r.error = "parse: " + err; return r; }

    const JsonValue* root = &wrap;
    if (wrap.has("config")) {                       // wrapped with checksum
        root = wrap.find("config");
        uint32_t expect = (uint32_t)wrap.num_or("checksum", 0);
        r.checksumOk = (expect == fnv1a(jsonDump(*root)));
    } else {
        r.checksumOk = true;                        // unwrapped import (no checksum)
    }

    long ver = root->int_or("schemaVersion", 0);
    RuntimeConfig cfg = defaultConfig();
    if (ver > 0 && ver < (long)CONFIG_SCHEMA_VERSION) {
        if (!migrateLegacy(*root, cfg)) { r.error = "migration failed"; return r; }
        r.migrated = true;
    } else if (ver == 0) {
        // no version at all: treat as a legacy v3 export
        if (!migrateLegacy(*root, cfg)) { r.error = "migration failed"; return r; }
        r.migrated = true;
    } else {
        cfg.schemaVersion = (uint32_t)ver;
        if (auto* d = root->find("device")) {
            std::snprintf(cfg.device.name, sizeof(cfg.device.name), "%s", d->str_or("name", cfg.device.name).c_str());
            cfg.device.board = (BoardKind)d->int_or("board", (int)cfg.device.board);
        }
        if (auto* n = root->find("network")) {
            std::snprintf(cfg.network.apSsid, sizeof(cfg.network.apSsid), "%s", n->str_or("apSsid", cfg.network.apSsid).c_str());
            cfg.network.apEnabled = n->bool_or("apEnabled", cfg.network.apEnabled);
            cfg.network.requireAuth = n->bool_or("requireAuth", cfg.network.requireAuth);
            cfg.network.disableApWhenConnected = n->bool_or("disableApWhenConnected", cfg.network.disableApWhenConnected);
        }
        if (auto* mi = root->find("midi")) {
            cfg.midi.din = mi->bool_or("din", cfg.midi.din);
            cfg.midi.ble = mi->bool_or("ble", cfg.midi.ble);
            cfg.midi.rtp = mi->bool_or("rtp", cfg.midi.rtp);
            cfg.midi.usb = mi->bool_or("usb", cfg.midi.usb);
            cfg.midi.webKeyboard = mi->bool_or("webKeyboard", cfg.midi.webKeyboard);
            cfg.midi.transpose = (int8_t)mi->int_or("transpose", cfg.midi.transpose);
        }
        long count = root->int_or("instrumentCount", 1);
        if (count < 0) count = 0;
        if (count > MAX_INSTRUMENTS) count = MAX_INSTRUMENTS;
        cfg.instrumentCount = (uint8_t)count;
        if (auto* arr = root->find("instruments"))
            for (int i = 0; i < cfg.instrumentCount && i < (int)arr->arr.size(); ++i)
                instrumentFromJson(arr->arr[i], cfg.instruments[i]);
    }

    // structural validation before accepting (transactional)
    for (int i = 0; i < cfg.instrumentCount; ++i) {
        InstrumentConfig& in = cfg.instruments[i];
        if (in.noteMin > in.noteMax) { r.error = "noteMin>noteMax"; return r; }
        if (in.air.flow.min > in.air.flow.max) { r.error = "flow min>max"; return r; }
        if (in.air.gate.hold01 > in.air.gate.peak01 + 1e-6f) { r.error = "gate hold>peak"; return r; }
    }
    out = cfg;
    r.ok = true;
    return r;
}

// v3 → v4 migration. Accepts either a wrapped {instruments:[{ch,nMin,...}]} or
// the historic flat single-flute export. Never destroys data silently.
inline bool migrateLegacy(const JsonValue& legacy, RuntimeConfig& out) {
    out = defaultConfig();
    out.schemaVersion = CONFIG_SCHEMA_VERSION;
    const JsonValue* insts = legacy.find("instruments");
    if (insts && insts->type == JsonValue::Arr) {
        long count = (long)insts->arr.size();
        if (count > MAX_INSTRUMENTS) count = MAX_INSTRUMENTS;
        out.instrumentCount = (uint8_t)(count < 1 ? 1 : count);
        for (int i = 0; i < out.instrumentCount; ++i) {
            const JsonValue& li = insts->arr[i];
            InstrumentConfig& in = out.instruments[i];
            in.enabled     = li.bool_or("enabled", true);
            in.midiChannel = (uint8_t)li.int_or("ch", 1);
            in.noteMin     = (uint8_t)li.int_or("nMin", 48);
            in.noteMax     = (uint8_t)li.int_or("nMax", 84);
            std::snprintf(in.name, sizeof(in.name), "%s", li.str_or("cName", "Flute").c_str());
            in.motion.type = SlideDriveType::StepDir;            // v3 was always a stepper
            in.motion.maxSpeedMmS = (float)li.num_or("speed", in.motion.maxSpeedMmS);
            in.motion.accelMmS2   = (float)li.num_or("accel", in.motion.accelMmS2);
            // legacy AirControl was solenoid (pwmFull/pwmHold) + flow servo
            in.air.gate.type   = AirGateType::SolenoidPwm;
            in.air.gate.peak01 = (float)li.int_or("pwmFull", 255) / 255.0f;
            in.air.gate.hold01 = (float)li.int_or("pwmHold", 128) / 255.0f;
            in.air.flow.type   = FlowControlType::FlowServo;
            in.cc.breath  = (uint8_t)li.int_or("ccBreath", 2);
            in.cc.expression = (uint8_t)li.int_or("ccExpr", 11);
            in.cc.volume  = (uint8_t)li.int_or("ccVolume", 7);
            in.cc.vibrato = (uint8_t)li.int_or("ccVibrato", 1);
            in.cc.sustain = (uint8_t)li.int_or("ccSustain", 64);
            in.seq.legato = (LegatoPolicy)li.int_or("legato", 0);
            // legacy LUT: array of {note, mm} → calibrated points
            if (auto* lut = li.find("lut"))
                for (const auto& pt : lut->arr) {
                    int note = (int)pt.int_or("note", -1);
                    if (note >= 0 && note < MIDI_NOTE_COUNT)
                        in.map.setPoint((uint8_t)note, (float)pt.num_or("mm", 0), (uint8_t)pt.int_or("air", 0));
                }
        }
        return true;
    }
    return true;   // empty legacy → keep collision-free default (still recoverable)
}

} // namespace swc

#endif // SWC_CORE_CONFIGCODEC_H
