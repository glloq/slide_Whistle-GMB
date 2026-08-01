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
inline JsonValue endstopToJson(const EndstopConfig& e) {
    JsonValue o = JsonValue::makeObj();
    o.set("present", e.present); o.set("pin", (int)e.pin);
    o.set("normallyClosed", e.normallyClosed); o.set("activeHigh", e.activeHigh);
    o.set("internalPullup", e.internalPullup);
    return o;
}
// Reads an integer field and verifies it lies in [lo,hi] BEFORE it is narrowed
// to a small integer type. Without this, e.g. channel:256 wraps to 0 (=OMNI)
// and slips past the post-narrowing range check in validateStructural (review
// #3 §11.3). Any out-of-range field clears `ok`, which aborts the decode.
inline long checkedInt(const JsonValue& v, const char* key, long def,
                       long lo, long hi, bool& ok) {
    long r = v.int_or(key, def);
    if (r < lo || r > hi) ok = false;
    return r;
}

// Valid GPIO numbers on the supported ESP32 variants top out below this; -1
// means "not wired". Pins outside this window are rejected pre-narrowing.
static constexpr long PIN_LO = -1;
static constexpr long PIN_HI = 48;

// Upper bound for any millisecond duration field (1 hour). Durations were read
// straight into uint32_t via num_or, so a NEGATIVE JSON value wrapped to a huge
// timeout (review #9 §5). checkedU32 rejects negatives and absurd values before
// the narrowing.
static constexpr long MS_MAX = 3600000;
inline uint32_t checkedU32(const JsonValue& v, const char* key, uint32_t def,
                           long hi, bool& ok) {
    long r = v.int_or(key, (long)def);
    if (r < 0 || r > hi) { ok = false; return def; }
    return (uint32_t)r;
}
// Reads a floating field and requires it be FINITE and within [lo,hi] — a NaN or
// Inf (or an out-of-range value) fails the decode. The `>=`/`<=` pair is false
// for NaN, so this also rejects NaN without a separate isnan() call (#9 §5).
inline float checkedNum(const JsonValue& v, const char* key, float def,
                        double lo, double hi, bool& ok) {
    double r = v.num_or(key, def);
    if (!(r >= lo && r <= hi)) { ok = false; return def; }
    return (float)r;
}

inline void endstopFromJson(const JsonValue& v, EndstopConfig& e, bool& ok) {
    e.present = v.bool_or("present", e.present);
    e.pin = (int8_t)checkedInt(v, "pin", e.pin, PIN_LO, PIN_HI, ok);
    e.normallyClosed = v.bool_or("normallyClosed", e.normallyClosed);
    e.activeHigh = v.bool_or("activeHigh", e.activeHigh);
    e.internalPullup = v.bool_or("internalPullup", e.internalPullup);
}

inline JsonValue servoToJson(const ServoMotionConfig& s) {
    JsonValue v = JsonValue::makeObj();
    v.set("backend", (int)s.backend);
    v.set("pin", (int)s.pin);
    v.set("pca", (int)s.pcaChannel);
    v.set("freqHz", (int)s.freqHz);
    v.set("minUs", (int)s.minUs);
    v.set("maxUs", (int)s.maxUs);
    v.set("invert", s.invert);
    v.set("restUs", (int)s.restUs);
    v.set("safeUs", (int)s.safeUs);
    v.set("trimUs", (int)s.trimUs);
    v.set("offsetUs", (int)s.offsetUs);
    v.set("detachIdleMs", (double)s.detachIdleMs);
    // multi-point calibration (non-linear linkage correction)
    JsonValue cal = JsonValue::makeArr();
    for (uint8_t i = 0; i < s.calCount && i < 8; ++i) {
        JsonValue p = JsonValue::makeObj(); p.set("mm", s.cal[i].mm); p.set("us", (int)s.cal[i].us);
        cal.arr.push_back(p);
    }
    v.set("cal", cal);
    return v;
}
inline void servoFromJson(const JsonValue& v, ServoMotionConfig& s, bool& ok) {
    s.backend = (PwmBackend)checkedInt(v, "backend", (int)s.backend, 0, 1, ok);
    s.pin = (int8_t)checkedInt(v, "pin", s.pin, PIN_LO, PIN_HI, ok);
    s.pcaChannel = (uint8_t)checkedInt(v, "pca", s.pcaChannel, 0, 15, ok);
    s.freqHz = (uint16_t)checkedInt(v, "freqHz", s.freqHz, 1, 400, ok);
    s.minUs = (uint16_t)checkedInt(v, "minUs", s.minUs, 100, 3000, ok);
    s.maxUs = (uint16_t)checkedInt(v, "maxUs", s.maxUs, 100, 3000, ok);
    s.invert = v.bool_or("invert", s.invert);
    // Pulse-width fields are range-checked BEFORE narrowing so an out-of-range
    // value can't be silently truncated into a plausible one (review #5 §21).
    s.restUs = (uint16_t)checkedInt(v, "restUs", s.restUs, 100, 3000, ok);
    s.safeUs = (uint16_t)checkedInt(v, "safeUs", s.safeUs, 100, 3000, ok);
    s.trimUs = (int16_t)checkedInt(v, "trimUs", s.trimUs, -1000, 1000, ok);
    s.offsetUs = (int16_t)checkedInt(v, "offsetUs", s.offsetUs, -1000, 1000, ok);
    s.detachIdleMs = checkedU32(v, "detachIdleMs", s.detachIdleMs, MS_MAX, ok);
    if (auto* cal = v.find("cal")) {
        uint8_t n = 0;
        for (const auto& p : cal->arr) {
            if (n >= 8) break;
            s.cal[n].mm = (float)p.num_or("mm", 0);
            s.cal[n].us = (uint16_t)checkedInt(p, "us", 1500, 100, 3000, ok);
            n++;
        }
        if (n >= 2) s.calCount = n;
    }
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
    const auto& sp = in.motion.stepper;
    JsonValue st = JsonValue::makeObj();
    st.set("stepPin", (int)sp.stepPin);
    st.set("dirPin", (int)sp.dirPin);
    st.set("enablePin", (int)sp.enablePin);
    st.set("enableActiveHigh", sp.enableActiveHigh);
    st.set("invertDir", sp.invertDir);
    st.set("stepsPerRev", (int)sp.stepsPerRev);
    st.set("microsteps", (int)sp.microsteps);
    st.set("stepsPerMm", sp.stepsPerMm);
    st.set("homingFastMmS", sp.homingFastMmS);
    st.set("homingSlowMmS", sp.homingSlowMmS);
    st.set("homeTowardZero", sp.homeTowardZero);
    st.set("homeOffsetMm", sp.homeOffsetMm);
    st.set("homeBackoffMm", sp.homeBackoffMm);
    st.set("phaseTimeoutMs", (double)sp.phaseTimeoutMs);
    st.set("idleDisableMs", (double)sp.idleDisableMs);
    st.set("alwaysHold", sp.alwaysHold);
    st.set("endstopMin", endstopToJson(sp.endstopMin));
    st.set("endstopMax", endstopToJson(sp.endstopMax));
    m.set("stepper", st);
    m.set("servoA", servoToJson(in.motion.servoA));
    m.set("servoB", servoToJson(in.motion.servoB));
    m.set("servoBEnabled", in.motion.servoBEnabled);
    m.set("dualMode", (int)in.motion.dualMode);
    v.set("motion", m);

    JsonValue a = JsonValue::makeObj();
    const auto& so = in.air.source;
    JsonValue src = JsonValue::makeObj();
    src.set("type", (int)so.type);
    src.set("pumpCount", (int)so.pumpCount);
    JsonValue pins = JsonValue::makeArr();
    for (int p = 0; p < MAX_PUMPS; ++p) { JsonValue n; n.type = JsonValue::Num; n.num = so.pin[p]; pins.arr.push_back(n); }
    src.set("pins", pins);
    src.set("idle01", so.idle01); src.set("min01", so.min01); src.set("max01", so.max01);
    src.set("startBoost01", so.startBoost01);
    src.set("spinUpMs", (double)so.spinUpMs);
    src.set("stopDelayMs", (double)so.stopDelayMs);
    src.set("cascadeDelayMs", (double)so.cascadeDelayMs);
    src.set("tankMode", (int)so.tankMode);
    src.set("tankPwm", so.tankPwm);
    src.set("target", so.target);
    src.set("pidKp", so.pidKp); src.set("pidKi", so.pidKi);
    src.set("requireSensor", so.requireSensor);
    src.set("lowThresh", so.lowThresh);
    src.set("highThresh", so.highThresh);
    src.set("safetyThresh", so.safetyThresh);
    src.set("refillTimeoutMs", (double)so.refillTimeoutMs);
    src.set("minOffMs", (double)so.minOffMs);
    a.set("source", src);
    const auto& gc = in.air.gate;
    JsonValue g = JsonValue::makeObj();
    g.set("type", (int)gc.type); g.set("pin", (int)gc.pin);
    g.set("backend", (int)gc.backend); g.set("pca", (int)gc.pcaChannel);
    g.set("activeHigh", gc.activeHigh);
    g.set("openTimeoutMs", (double)gc.openTimeoutMs);
    g.set("peak01", gc.peak01); g.set("peakMs", (double)gc.peakMs); g.set("hold01", gc.hold01);
    g.set("closed01", gc.closed01); g.set("open01", gc.open01);
    g.set("openDelayMs", (double)gc.openDelayMs); g.set("closeDelayMs", (double)gc.closeDelayMs);
    g.set("servoMinUs", (int)gc.servoMinUs); g.set("servoMaxUs", (int)gc.servoMaxUs);
    a.set("gate", g);
    const auto& fc = in.air.flow;
    JsonValue f = JsonValue::makeObj();
    f.set("type", (int)fc.type); f.set("pin", (int)fc.pin);
    f.set("backend", (int)fc.backend); f.set("pca", (int)fc.pcaChannel);
    f.set("min", (int)fc.min); f.set("nominal", (int)fc.nominal); f.set("max", (int)fc.max);
    f.set("rest01", fc.rest01); f.set("curve", (int)fc.curve); f.set("expo", fc.expo);
    f.set("maxSlewPerMs", fc.maxSlewPerMs);
    f.set("servoMinUs", (int)fc.servoMinUs); f.set("servoMaxUs", (int)fc.servoMaxUs);
    a.set("flow", f);
    const auto& ac = in.air.angle;
    JsonValue ang = JsonValue::makeObj();
    ang.set("enabled", ac.enabled); ang.set("pin", (int)ac.pin);
    ang.set("backend", (int)ac.backend); ang.set("pca", (int)ac.pcaChannel);
    ang.set("rest01", ac.rest01); ang.set("min01", ac.min01);
    ang.set("nominal01", ac.nominal01); ang.set("max01", ac.max01);
    ang.set("useCc74", ac.useCc74);
    ang.set("servoMinUs", (int)ac.servoMinUs); ang.set("servoMaxUs", (int)ac.servoMaxUs);
    a.set("angle", ang);
    const auto& sc = in.air.sensor;
    JsonValue se = JsonValue::makeObj();
    se.set("type", (int)sc.type); se.set("pin", (int)sc.pin); se.set("i2cAddr", (int)sc.i2cAddr);
    se.set("rawMin", sc.rawMin); se.set("rawMax", sc.rawMax);
    se.set("physMin", sc.physMin); se.set("physMax", sc.physMax);
    se.set("invert", sc.invert); se.set("filterAlpha", sc.filterAlpha);
    se.set("staleTimeoutMs", (double)sc.staleTimeoutMs);
    se.set("physLo", sc.physLo); se.set("physHi", sc.physHi);
    a.set("sensor", se);
    a.set("valveOpenTimeoutMs", (double)in.air.valveOpenTimeoutMs);
    a.set("minNoteMs", (double)in.air.minNoteMs);
    v.set("air", a);

    JsonValue sq = JsonValue::makeObj();
    sq.set("mono", (int)in.seq.mono);
    sq.set("legato", (int)in.seq.legato);
    sq.set("legatoMaxDistanceMm", in.seq.legatoMaxDistanceMm);
    sq.set("legatoMaxTimeMs", (double)in.seq.legatoMaxTimeMs);
    sq.set("legatoMaxMoveTimeMs", in.seq.legatoMaxMoveTimeMs);
    sq.set("minNoteMs", (double)in.seq.minNoteMs);
    sq.set("prepareTimeoutMs", (double)in.seq.prepareTimeoutMs);
    sq.set("vibratoUnit", (int)in.seq.vibratoUnit);
    sq.set("vibratoRateHz", in.seq.vibratoRateHz);
    v.set("seq", sq);

    JsonValue cc = JsonValue::makeObj();
    cc.set("breath", (int)in.cc.breath); cc.set("expression", (int)in.cc.expression);
    cc.set("volume", (int)in.cc.volume); cc.set("vibrato", (int)in.cc.vibrato);
    cc.set("sustain", (int)in.cc.sustain); cc.set("angle", (int)in.cc.angle);
    cc.set("vibratoEnabled", in.cc.vibratoEnabled);
    v.set("cc", cc);

    // calibration points: every ENABLED entry (calibrated or provisional) so a
    // preset's provisional table survives a save/reload (review item #7).
    JsonValue cal = JsonValue::makeArr();
    for (int n = 0; n < MIDI_NOTE_COUNT; ++n) {
        const NoteEntry& e = in.map.entry((uint8_t)n);
        if (!e.enabled) continue;
        JsonValue pt = JsonValue::makeObj();
        pt.set("note", n); pt.set("mm", e.positionMm);
        pt.set("tol", e.positionToleranceMm);
        pt.set("airMin", (int)e.airMin); pt.set("air", (int)e.airNominal); pt.set("airMax", (int)e.airMax);
        pt.set("calibrated", e.calibrated);   // false = provisional / not hw-validated
        pt.set("enabled", e.enabled);
        cal.arr.push_back(pt);
    }
    v.set("calibration", cal);
    return v;
}

inline void instrumentFromJson(const JsonValue& v, InstrumentConfig& in, bool& ok) {
    in.enabled = v.bool_or("enabled", in.enabled);
    std::string nm = v.str_or("name", in.name);
    std::snprintf(in.name, sizeof(in.name), "%s", nm.c_str());
    in.midiChannel = (uint8_t)checkedInt(v, "channel", in.midiChannel, 0, 16, ok);
    in.noteMin = (uint8_t)checkedInt(v, "noteMin", in.noteMin, 0, 127, ok);
    in.noteMax = (uint8_t)checkedInt(v, "noteMax", in.noteMax, 0, 127, ok);
    in.watchdogMs = checkedU32(v, "watchdogMs", in.watchdogMs, MS_MAX, ok);

    if (auto* m = v.find("motion")) {
        in.motion.type = (SlideDriveType)checkedInt(*m, "type", (int)in.motion.type, 0, 3, ok);
        in.motion.travelMm = checkedNum(*m, "travelMm", in.motion.travelMm, 0.1, 100000, ok);
        in.motion.maxSpeedMmS = checkedNum(*m, "maxSpeedMmS", in.motion.maxSpeedMmS, 0, 1000000, ok);
        in.motion.accelMmS2 = checkedNum(*m, "accelMmS2", in.motion.accelMmS2, 0, 100000000, ok);
        in.motion.softMinMm = checkedNum(*m, "softMinMm", in.motion.softMinMm, -100000, 100000, ok);
        in.motion.softMaxMm = checkedNum(*m, "softMaxMm", in.motion.softMaxMm, -100000, 100000, ok);
        if (auto* st = m->find("stepper")) {
            auto& sp = in.motion.stepper;
            sp.stepPin = (int8_t)checkedInt(*st, "stepPin", sp.stepPin, PIN_LO, PIN_HI, ok);
            sp.dirPin = (int8_t)checkedInt(*st, "dirPin", sp.dirPin, PIN_LO, PIN_HI, ok);
            sp.enablePin = (int8_t)checkedInt(*st, "enablePin", sp.enablePin, PIN_LO, PIN_HI, ok);
            sp.enableActiveHigh = st->bool_or("enableActiveHigh", sp.enableActiveHigh);
            sp.invertDir = st->bool_or("invertDir", sp.invertDir);
            sp.stepsPerRev = (uint16_t)checkedInt(*st, "stepsPerRev", sp.stepsPerRev, 1, 10000, ok);
            sp.microsteps = (uint16_t)checkedInt(*st, "microsteps", sp.microsteps, 1, 256, ok);
            sp.stepsPerMm = checkedNum(*st, "stepsPerMm", sp.stepsPerMm, 0.01, 100000, ok);
            sp.homingFastMmS = checkedNum(*st, "homingFastMmS", sp.homingFastMmS, 0, 1000000, ok);
            sp.homingSlowMmS = checkedNum(*st, "homingSlowMmS", sp.homingSlowMmS, 0, 1000000, ok);
            sp.homeTowardZero = st->bool_or("homeTowardZero", sp.homeTowardZero);
            sp.homeOffsetMm = checkedNum(*st, "homeOffsetMm", sp.homeOffsetMm, 0, 100000, ok);
            sp.homeBackoffMm = checkedNum(*st, "homeBackoffMm", sp.homeBackoffMm, 0, 100000, ok);
            sp.phaseTimeoutMs = checkedU32(*st, "phaseTimeoutMs", sp.phaseTimeoutMs, MS_MAX, ok);
            sp.idleDisableMs = checkedU32(*st, "idleDisableMs", sp.idleDisableMs, MS_MAX, ok);
            sp.alwaysHold = st->bool_or("alwaysHold", sp.alwaysHold);
            if (auto* e = st->find("endstopMin")) endstopFromJson(*e, sp.endstopMin, ok);
            else if (st->has("endstopPin")) sp.endstopMin.pin = (int8_t)checkedInt(*st, "endstopPin", sp.endstopMin.pin, PIN_LO, PIN_HI, ok);
            if (auto* e = st->find("endstopMax")) endstopFromJson(*e, sp.endstopMax, ok);
        }
        if (auto* sa = m->find("servoA")) servoFromJson(*sa, in.motion.servoA, ok);
        if (auto* sb = m->find("servoB")) servoFromJson(*sb, in.motion.servoB, ok);
        in.motion.servoBEnabled = m->bool_or("servoBEnabled", in.motion.servoBEnabled);
        in.motion.dualMode = (DualSyncMode)checkedInt(*m, "dualMode", (int)in.motion.dualMode, 0, 2, ok);
    }
    if (auto* a = v.find("air")) {
        if (auto* src = a->find("source")) {
            auto& so = in.air.source;
            so.type = (AirSourceType)checkedInt(*src, "type", (int)so.type, 0, 4, ok);
            so.pumpCount = (uint8_t)checkedInt(*src, "pumpCount", so.pumpCount, 0, MAX_PUMPS, ok);
            if (auto* pins = src->find("pins"))
                for (int p = 0; p < MAX_PUMPS && p < (int)pins->arr.size(); ++p) {
                    long pv = pins->arr[p].asInt(so.pin[p]);
                    if (pv < PIN_LO || pv > PIN_HI) ok = false;
                    so.pin[p] = (int8_t)pv;
                }
            so.idle01 = checkedNum(*src, "idle01", so.idle01, 0, 1, ok);
            so.min01 = checkedNum(*src, "min01", so.min01, 0, 1, ok);
            so.max01 = checkedNum(*src, "max01", so.max01, 0, 1, ok);
            so.startBoost01 = checkedNum(*src, "startBoost01", so.startBoost01, 0, 1, ok);
            so.spinUpMs = checkedU32(*src, "spinUpMs", so.spinUpMs, MS_MAX, ok);
            so.stopDelayMs = checkedU32(*src, "stopDelayMs", so.stopDelayMs, MS_MAX, ok);
            so.cascadeDelayMs = checkedU32(*src, "cascadeDelayMs", so.cascadeDelayMs, MS_MAX, ok);
            so.tankMode = (TankRegulationMode)checkedInt(*src, "tankMode", (int)so.tankMode, 0, 2, ok);
            so.tankPwm = src->bool_or("tankPwm", so.tankPwm);
            so.target = checkedNum(*src, "target", so.target, -1000000, 1000000, ok);
            so.pidKp = checkedNum(*src, "pidKp", so.pidKp, 0, 100000, ok);
            so.pidKi = checkedNum(*src, "pidKi", so.pidKi, 0, 100000, ok);
            so.requireSensor = src->bool_or("requireSensor", so.requireSensor);
            so.lowThresh = checkedNum(*src, "lowThresh", so.lowThresh, -1000000, 1000000, ok);
            so.highThresh = checkedNum(*src, "highThresh", so.highThresh, -1000000, 1000000, ok);
            so.safetyThresh = checkedNum(*src, "safetyThresh", so.safetyThresh, -1000000, 1000000, ok);
            so.refillTimeoutMs = checkedU32(*src, "refillTimeoutMs", so.refillTimeoutMs, MS_MAX, ok);
            so.minOffMs = checkedU32(*src, "minOffMs", so.minOffMs, MS_MAX, ok);
        }
        if (auto* g = a->find("gate")) {
            auto& gc = in.air.gate;
            gc.type = (AirGateType)checkedInt(*g, "type", (int)gc.type, 0, 5, ok);
            gc.pin = (int8_t)checkedInt(*g, "pin", gc.pin, PIN_LO, PIN_HI, ok);
            gc.backend = (PwmBackend)checkedInt(*g, "backend", (int)gc.backend, 0, 1, ok);
            gc.pcaChannel = (uint8_t)checkedInt(*g, "pca", gc.pcaChannel, 0, 15, ok);
            gc.activeHigh = g->bool_or("activeHigh", gc.activeHigh);
            gc.openTimeoutMs = checkedU32(*g, "openTimeoutMs", gc.openTimeoutMs, MS_MAX, ok);
            gc.peak01 = checkedNum(*g, "peak01", gc.peak01, 0, 1, ok);
            gc.peakMs = checkedU32(*g, "peakMs", gc.peakMs, MS_MAX, ok);
            gc.hold01 = checkedNum(*g, "hold01", gc.hold01, 0, 1, ok);
            gc.closed01 = checkedNum(*g, "closed01", gc.closed01, 0, 1, ok);
            gc.open01 = checkedNum(*g, "open01", gc.open01, 0, 1, ok);
            gc.openDelayMs = checkedU32(*g, "openDelayMs", gc.openDelayMs, MS_MAX, ok);
            gc.closeDelayMs = checkedU32(*g, "closeDelayMs", gc.closeDelayMs, MS_MAX, ok);
            gc.servoMinUs = (uint16_t)checkedInt(*g, "servoMinUs", gc.servoMinUs, 100, 3000, ok);
            gc.servoMaxUs = (uint16_t)checkedInt(*g, "servoMaxUs", gc.servoMaxUs, 100, 3000, ok);
        }
        if (auto* f = a->find("flow")) {
            auto& fc = in.air.flow;
            fc.type = (FlowControlType)checkedInt(*f, "type", (int)fc.type, 0, 5, ok);
            fc.pin = (int8_t)checkedInt(*f, "pin", fc.pin, PIN_LO, PIN_HI, ok);
            fc.backend = (PwmBackend)checkedInt(*f, "backend", (int)fc.backend, 0, 1, ok);
            fc.pcaChannel = (uint8_t)checkedInt(*f, "pca", fc.pcaChannel, 0, 15, ok);
            fc.min = (uint8_t)checkedInt(*f, "min", fc.min, 0, 127, ok);
            fc.nominal = (uint8_t)checkedInt(*f, "nominal", fc.nominal, 0, 127, ok);
            fc.max = (uint8_t)checkedInt(*f, "max", fc.max, 0, 127, ok);
            fc.rest01 = checkedNum(*f, "rest01", fc.rest01, 0, 1, ok);
            fc.curve = (VelocityCurve)checkedInt(*f, "curve", (int)fc.curve, 0, 4, ok);
            fc.expo = checkedNum(*f, "expo", fc.expo, 0.01, 100, ok);
            fc.maxSlewPerMs = checkedNum(*f, "maxSlewPerMs", fc.maxSlewPerMs, 0, 1000, ok);
            fc.servoMinUs = (uint16_t)checkedInt(*f, "servoMinUs", fc.servoMinUs, 100, 3000, ok);
            fc.servoMaxUs = (uint16_t)checkedInt(*f, "servoMaxUs", fc.servoMaxUs, 100, 3000, ok);
        }
        if (auto* ang = a->find("angle")) {
            auto& ac = in.air.angle;
            ac.enabled = ang->bool_or("enabled", ac.enabled);
            ac.pin = (int8_t)checkedInt(*ang, "pin", ac.pin, PIN_LO, PIN_HI, ok);
            ac.backend = (PwmBackend)checkedInt(*ang, "backend", (int)ac.backend, 0, 1, ok);
            ac.pcaChannel = (uint8_t)checkedInt(*ang, "pca", ac.pcaChannel, 0, 15, ok);
            ac.rest01 = checkedNum(*ang, "rest01", ac.rest01, 0, 1, ok);
            ac.min01 = checkedNum(*ang, "min01", ac.min01, 0, 1, ok);
            ac.nominal01 = checkedNum(*ang, "nominal01", ac.nominal01, 0, 1, ok);
            ac.max01 = checkedNum(*ang, "max01", ac.max01, 0, 1, ok);
            ac.useCc74 = ang->bool_or("useCc74", ac.useCc74);
            ac.servoMinUs = (uint16_t)checkedInt(*ang, "servoMinUs", ac.servoMinUs, 100, 3000, ok);
            ac.servoMaxUs = (uint16_t)checkedInt(*ang, "servoMaxUs", ac.servoMaxUs, 100, 3000, ok);
        }
        if (auto* se = a->find("sensor")) {
            auto& sc = in.air.sensor;
            sc.type = (AirSensorType)checkedInt(*se, "type", (int)sc.type, 0, 7, ok);
            sc.pin = (int8_t)checkedInt(*se, "pin", sc.pin, PIN_LO, PIN_HI, ok);
            sc.i2cAddr = (uint8_t)checkedInt(*se, "i2cAddr", sc.i2cAddr, 0, 127, ok);
            sc.rawMin = checkedNum(*se, "rawMin", sc.rawMin, -1000000, 1000000, ok);
            sc.rawMax = checkedNum(*se, "rawMax", sc.rawMax, -1000000, 1000000, ok);
            sc.physMin = checkedNum(*se, "physMin", sc.physMin, -1000000, 1000000, ok);
            sc.physMax = checkedNum(*se, "physMax", sc.physMax, -1000000, 1000000, ok);
            sc.invert = se->bool_or("invert", sc.invert);
            sc.filterAlpha = checkedNum(*se, "filterAlpha", sc.filterAlpha, 0, 1, ok);
            sc.staleTimeoutMs = checkedU32(*se, "staleTimeoutMs", sc.staleTimeoutMs, MS_MAX, ok);
            sc.physLo = checkedNum(*se, "physLo", sc.physLo, -1000000, 1000000, ok);
            sc.physHi = checkedNum(*se, "physHi", sc.physHi, -1000000, 1000000, ok);
        }
        in.air.valveOpenTimeoutMs = checkedU32(*a, "valveOpenTimeoutMs", in.air.valveOpenTimeoutMs, MS_MAX, ok);
        in.air.minNoteMs = checkedU32(*a, "minNoteMs", in.air.minNoteMs, MS_MAX, ok);
    }
    if (auto* sq = v.find("seq")) {
        in.seq.mono = (MonoPolicy)checkedInt(*sq, "mono", (int)in.seq.mono, 0, 2, ok);
        in.seq.legato = (LegatoPolicy)checkedInt(*sq, "legato", (int)in.seq.legato, 0, 5, ok);
        in.seq.legatoMaxDistanceMm = checkedNum(*sq, "legatoMaxDistanceMm", in.seq.legatoMaxDistanceMm, 0, 100000, ok);
        in.seq.legatoMaxTimeMs = checkedU32(*sq, "legatoMaxTimeMs", in.seq.legatoMaxTimeMs, MS_MAX, ok);
        in.seq.legatoMaxMoveTimeMs = checkedNum(*sq, "legatoMaxMoveTimeMs", in.seq.legatoMaxMoveTimeMs, 0, MS_MAX, ok);
        in.seq.minNoteMs = checkedU32(*sq, "minNoteMs", in.seq.minNoteMs, MS_MAX, ok);
        in.seq.prepareTimeoutMs = checkedU32(*sq, "prepareTimeoutMs", in.seq.prepareTimeoutMs, MS_MAX, ok);
        in.seq.vibratoUnit = (VibratoUnit)checkedInt(*sq, "vibratoUnit", (int)in.seq.vibratoUnit, 0, 2, ok);
        in.seq.vibratoRateHz = checkedNum(*sq, "vibratoRateHz", in.seq.vibratoRateHz, 0, 100, ok);
    }
    if (auto* cc = v.find("cc")) {
        in.cc.breath = (uint8_t)checkedInt(*cc, "breath", in.cc.breath, 0, 127, ok);
        in.cc.expression = (uint8_t)checkedInt(*cc, "expression", in.cc.expression, 0, 127, ok);
        in.cc.volume = (uint8_t)checkedInt(*cc, "volume", in.cc.volume, 0, 127, ok);
        in.cc.vibrato = (uint8_t)checkedInt(*cc, "vibrato", in.cc.vibrato, 0, 127, ok);
        in.cc.sustain = (uint8_t)checkedInt(*cc, "sustain", in.cc.sustain, 0, 127, ok);
        in.cc.angle = (uint8_t)checkedInt(*cc, "angle", in.cc.angle, 0, 127, ok);
        in.cc.vibratoEnabled = cc->bool_or("vibratoEnabled", in.cc.vibratoEnabled);
    }
    if (auto* cal = v.find("calibration")) {
        in.map.clear(); in.map.setTravelMm(in.motion.travelMm);
        for (const auto& pt : cal->arr) {
            int note = (int)pt.int_or("note", -1);
            if (note < 0 || note >= MIDI_NOTE_COUNT) continue;
            // Air levels are 0..127; range-check before narrowing (review #5 §21).
            uint8_t airNom = (uint8_t)checkedInt(pt, "air", 0, 0, 127, ok);
            in.map.setPoint((uint8_t)note, (float)pt.num_or("mm", 0), airNom);
            NoteEntry& e = in.map.entry((uint8_t)note);
            e.positionToleranceMm = (float)pt.num_or("tol", e.positionToleranceMm);
            e.airMin = (uint8_t)checkedInt(pt, "airMin", e.airMin, 0, 127, ok);
            e.airMax = (uint8_t)checkedInt(pt, "airMax", e.airMax, 0, 127, ok);
            e.enabled = pt.bool_or("enabled", true);
            e.calibrated = pt.bool_or("calibrated", true);  // preserve provisional status
        }
    }
}

// --- validation --------------------------------------------------------------
inline bool enumOk(int v, int hi) { return v >= 0 && v <= hi; }
inline bool is01(float x) { return std::isfinite(x) && x >= 0.f && x <= 1.f; }

// Full structural validation (review items #26/#27). Returns "" if valid, else
// a short reason. Runs on the decoded struct before it is ever accepted.
inline std::string validateStructural(const RuntimeConfig& c) {
    if (!enumOk((int)c.device.board, 2)) return "bad device.board";
    if (c.instrumentCount > MAX_INSTRUMENTS) return "instrumentCount out of range";
    for (uint8_t idx = 0; idx < c.instrumentCount; ++idx) {
        const InstrumentConfig& in = c.instruments[idx];
        // enums
        if (!enumOk((int)in.motion.type, 3)) return "bad motion.type";
        if (!enumOk((int)in.motion.dualMode, 2)) return "bad motion.dualMode";
        if (!enumOk((int)in.air.source.type, 4)) return "bad air.source.type";
        if (!enumOk((int)in.air.gate.type, 5)) return "bad air.gate.type";
        if (!enumOk((int)in.air.flow.type, 5)) return "bad air.flow.type";
        if (!enumOk((int)in.air.sensor.type, 7)) return "bad air.sensor.type";
        if (!enumOk((int)in.air.source.tankMode, 2)) return "bad tankMode";
        if (!enumOk((int)in.air.flow.curve, 4)) return "bad flow.curve";
        if (!enumOk((int)in.seq.mono, 2)) return "bad seq.mono";
        if (!enumOk((int)in.seq.legato, 5)) return "bad seq.legato";
        if (!enumOk((int)in.seq.vibratoUnit, 2)) return "bad vibratoUnit";
        // ranges
        if (in.midiChannel > 16) return "midiChannel out of 0..16";
        if (in.noteMin > 127 || in.noteMax > 127) return "note out of 0..127";
        if (in.noteMin > in.noteMax) return "noteMin>noteMax";
        const auto& f = in.air.flow;
        if (!(f.min <= f.nominal && f.nominal <= f.max)) return "flow min<=nominal<=max violated";
        const auto& g = in.air.gate;
        if (g.hold01 > g.peak01 + 1e-6f) return "gate hold>peak";
        const auto& s = in.air.sensor;
        if (s.type != AirSensorType::None) {
            if (!(s.rawMin < s.rawMax)) return "sensor rawMin<rawMax violated";
            if (!(s.physLo < s.physHi)) return "sensor physLo<physHi violated";
            if (!(s.filterAlpha >= 0.f && s.filterAlpha <= 1.f)) return "filterAlpha out of 0..1";
        }
        const auto& so = in.air.source;
        if (so.type == AirSourceType::PumpsDirect || so.type == AirSourceType::PumpsTank)
            if (so.pumpCount < 1 || so.pumpCount > MAX_PUMPS) return "pumpCount out of 1..3";
        if (so.type == AirSourceType::PumpsTank) {
            if (!(so.lowThresh < so.highThresh)) return "tank low<high violated";
            if (!(so.highThresh < so.safetyThresh)) return "tank high<safety violated";
            if (!(so.target >= so.lowThresh && so.target <= so.highThresh)) return "tank target outside low..high";
        }
        // servo calibration must be monotonic in mm
        for (auto* sv : { &in.motion.servoA, &in.motion.servoB }) {
            if (sv->calCount < 2 || sv->calCount > 8) return "servo calCount invalid";
            for (uint8_t i = 1; i < sv->calCount; ++i)
                if (!(sv->cal[i].mm > sv->cal[i-1].mm)) return "servo calibration not monotonic";
        }
        // finite geometry
        if (!(in.motion.travelMm > 0.f) || !std::isfinite(in.motion.travelMm)) return "travelMm invalid";
        if (in.motion.softMinMm > in.motion.softMaxMm) return "softMin>softMax";
        // motion dynamics must be positive & finite for a driven axis (#3 §11.4)
        if (in.motion.type != SlideDriveType::Disabled) {
            if (!(in.motion.maxSpeedMmS > 0.f) || !std::isfinite(in.motion.maxSpeedMmS)) return "maxSpeedMmS must be > 0";
            if (!(in.motion.accelMmS2 > 0.f) || !std::isfinite(in.motion.accelMmS2)) return "accelMmS2 must be > 0";
        }
        if (in.motion.type == SlideDriveType::StepDir)
            if (!(in.motion.stepper.stepsPerMm > 0.f) || !std::isfinite(in.motion.stepper.stepsPerMm)) return "stepsPerMm must be > 0";
        // servo pulse windows must be ordered (min < max) so mm→µs never inverts
        for (auto* sv : { &in.motion.servoA, &in.motion.servoB })
            if (!(sv->minUs < sv->maxUs)) return "servo minUs<maxUs violated";
        // normalized drive levels must lie in 0..1 (#3 §11.4)
        if (!is01(so.idle01) || !is01(so.min01) || !is01(so.max01) || !is01(so.startBoost01)) return "source level not in 0..1";
        if (!is01(g.peak01) || !is01(g.hold01) || !is01(g.closed01) || !is01(g.open01)) return "gate level not in 0..1";
        if (!is01(in.air.flow.rest01)) return "flow rest01 not in 0..1";
        if (in.air.angle.enabled) {
            const auto& ac = in.air.angle;
            if (!is01(ac.rest01) || !is01(ac.min01) || !is01(ac.nominal01) || !is01(ac.max01)) return "angle level not in 0..1";
        }
        // note table monotonic (non-decreasing positions)
        if (in.map.firstNonMonotonic() >= 0) return "note table not monotonic";
    }
    // USB MIDI is impossible on a classic WROOM
    if (c.midi.usb && c.device.board == BoardKind::Esp32Wroom) return "USB MIDI unavailable on WROOM";
    return "";
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
    n.set("allowedOrigin", std::string(c.network.allowedOrigin));
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
    // Refuse configs from a FUTURE schema rather than partially decoding an
    // unknown structure (review item #28).
    if (ver > (long)CONFIG_SCHEMA_VERSION) { r.error = "unsupported (future) schemaVersion"; return r; }
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
            long bd = d->int_or("board", (int)cfg.device.board);
            if (bd < 0 || bd > 2) { r.error = "device.board out of range"; return r; }
            cfg.device.board = (BoardKind)bd;
        }
        if (auto* n = root->find("network")) {
            std::snprintf(cfg.network.apSsid, sizeof(cfg.network.apSsid), "%s", n->str_or("apSsid", cfg.network.apSsid).c_str());
            cfg.network.apEnabled = n->bool_or("apEnabled", cfg.network.apEnabled);
            cfg.network.requireAuth = n->bool_or("requireAuth", cfg.network.requireAuth);
            cfg.network.disableApWhenConnected = n->bool_or("disableApWhenConnected", cfg.network.disableApWhenConnected);
            std::snprintf(cfg.network.allowedOrigin, sizeof(cfg.network.allowedOrigin), "%s",
                          n->str_or("allowedOrigin", cfg.network.allowedOrigin).c_str());
        }
        if (auto* mi = root->find("midi")) {
            cfg.midi.din = mi->bool_or("din", cfg.midi.din);
            cfg.midi.ble = mi->bool_or("ble", cfg.midi.ble);
            cfg.midi.rtp = mi->bool_or("rtp", cfg.midi.rtp);
            cfg.midi.usb = mi->bool_or("usb", cfg.midi.usb);
            cfg.midi.webKeyboard = mi->bool_or("webKeyboard", cfg.midi.webKeyboard);
            long tr = mi->int_or("transpose", cfg.midi.transpose);
            if (tr < -64 || tr > 63) { r.error = "midi.transpose out of -64..63"; return r; }
            cfg.midi.transpose = (int8_t)tr;
        }
        // Reject an out-of-range instrumentCount rather than silently clamping
        // it (review #6 §14) — a clamp would hide a malformed config.
        long count = root->int_or("instrumentCount", 1);
        if (count < 0 || count > MAX_INSTRUMENTS) { r.error = "instrumentCount out of range"; return r; }
        cfg.instrumentCount = (uint8_t)count;
        bool ok = true;
        if (auto* arr = root->find("instruments"))
            for (int i = 0; i < cfg.instrumentCount && i < (int)arr->arr.size(); ++i)
                instrumentFromJson(arr->arr[i], cfg.instruments[i], ok);
        // Reject out-of-range integers rather than accepting a wrapped value
        // that happens to land in the valid post-narrowing window (#3 §11.3).
        if (!ok) { r.error = "integer field out of range"; return r; }
    }

    // full structural + enum validation before accepting (transactional)
    std::string verr = validateStructural(cfg);
    if (!verr.empty()) { r.error = verr; return r; }
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
