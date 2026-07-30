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
inline void endstopFromJson(const JsonValue& v, EndstopConfig& e) {
    e.present = v.bool_or("present", e.present);
    e.pin = (int8_t)v.int_or("pin", e.pin);
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
inline void servoFromJson(const JsonValue& v, ServoMotionConfig& s) {
    s.backend = (PwmBackend)v.int_or("backend", (int)s.backend);
    s.pin = (int8_t)v.int_or("pin", s.pin);
    s.pcaChannel = (uint8_t)v.int_or("pca", s.pcaChannel);
    s.freqHz = (uint16_t)v.int_or("freqHz", s.freqHz);
    s.minUs = (uint16_t)v.int_or("minUs", s.minUs);
    s.maxUs = (uint16_t)v.int_or("maxUs", s.maxUs);
    s.invert = v.bool_or("invert", s.invert);
    s.restUs = (uint16_t)v.int_or("restUs", s.restUs);
    s.safeUs = (uint16_t)v.int_or("safeUs", s.safeUs);
    s.trimUs = (int16_t)v.int_or("trimUs", s.trimUs);
    s.offsetUs = (int16_t)v.int_or("offsetUs", s.offsetUs);
    s.detachIdleMs = (uint32_t)v.num_or("detachIdleMs", s.detachIdleMs);
    if (auto* cal = v.find("cal")) {
        uint8_t n = 0;
        for (const auto& p : cal->arr) {
            if (n >= 8) break;
            s.cal[n].mm = (float)p.num_or("mm", 0);
            s.cal[n].us = (uint16_t)p.int_or("us", 1500);
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
            auto& sp = in.motion.stepper;
            sp.stepPin = (int8_t)st->int_or("stepPin", sp.stepPin);
            sp.dirPin = (int8_t)st->int_or("dirPin", sp.dirPin);
            sp.enablePin = (int8_t)st->int_or("enablePin", sp.enablePin);
            sp.enableActiveHigh = st->bool_or("enableActiveHigh", sp.enableActiveHigh);
            sp.invertDir = st->bool_or("invertDir", sp.invertDir);
            sp.stepsPerRev = (uint16_t)st->int_or("stepsPerRev", sp.stepsPerRev);
            sp.microsteps = (uint16_t)st->int_or("microsteps", sp.microsteps);
            sp.stepsPerMm = (float)st->num_or("stepsPerMm", sp.stepsPerMm);
            sp.homingFastMmS = (float)st->num_or("homingFastMmS", sp.homingFastMmS);
            sp.homingSlowMmS = (float)st->num_or("homingSlowMmS", sp.homingSlowMmS);
            sp.homeTowardZero = st->bool_or("homeTowardZero", sp.homeTowardZero);
            sp.homeOffsetMm = (float)st->num_or("homeOffsetMm", sp.homeOffsetMm);
            sp.homeBackoffMm = (float)st->num_or("homeBackoffMm", sp.homeBackoffMm);
            sp.phaseTimeoutMs = (uint32_t)st->num_or("phaseTimeoutMs", sp.phaseTimeoutMs);
            sp.idleDisableMs = (uint32_t)st->num_or("idleDisableMs", sp.idleDisableMs);
            sp.alwaysHold = st->bool_or("alwaysHold", sp.alwaysHold);
            if (auto* e = st->find("endstopMin")) endstopFromJson(*e, sp.endstopMin);
            else if (st->has("endstopPin")) sp.endstopMin.pin = (int8_t)st->int_or("endstopPin", sp.endstopMin.pin);
            if (auto* e = st->find("endstopMax")) endstopFromJson(*e, sp.endstopMax);
        }
        if (auto* sa = m->find("servoA")) servoFromJson(*sa, in.motion.servoA);
        if (auto* sb = m->find("servoB")) servoFromJson(*sb, in.motion.servoB);
        in.motion.servoBEnabled = m->bool_or("servoBEnabled", in.motion.servoBEnabled);
        in.motion.dualMode = (DualSyncMode)m->int_or("dualMode", (int)in.motion.dualMode);
    }
    if (auto* a = v.find("air")) {
        if (auto* src = a->find("source")) {
            auto& so = in.air.source;
            so.type = (AirSourceType)src->int_or("type", (int)so.type);
            so.pumpCount = (uint8_t)src->int_or("pumpCount", so.pumpCount);
            if (auto* pins = src->find("pins"))
                for (int p = 0; p < MAX_PUMPS && p < (int)pins->arr.size(); ++p)
                    so.pin[p] = (int8_t)pins->arr[p].asInt(so.pin[p]);
            so.idle01 = (float)src->num_or("idle01", so.idle01);
            so.min01 = (float)src->num_or("min01", so.min01);
            so.max01 = (float)src->num_or("max01", so.max01);
            so.startBoost01 = (float)src->num_or("startBoost01", so.startBoost01);
            so.spinUpMs = (uint32_t)src->num_or("spinUpMs", so.spinUpMs);
            so.stopDelayMs = (uint32_t)src->num_or("stopDelayMs", so.stopDelayMs);
            so.cascadeDelayMs = (uint32_t)src->num_or("cascadeDelayMs", so.cascadeDelayMs);
            so.tankMode = (TankRegulationMode)src->int_or("tankMode", (int)so.tankMode);
            so.tankPwm = src->bool_or("tankPwm", so.tankPwm);
            so.target = (float)src->num_or("target", so.target);
            so.requireSensor = src->bool_or("requireSensor", so.requireSensor);
            so.lowThresh = (float)src->num_or("lowThresh", so.lowThresh);
            so.highThresh = (float)src->num_or("highThresh", so.highThresh);
            so.safetyThresh = (float)src->num_or("safetyThresh", so.safetyThresh);
            so.refillTimeoutMs = (uint32_t)src->num_or("refillTimeoutMs", so.refillTimeoutMs);
            so.minOffMs = (uint32_t)src->num_or("minOffMs", so.minOffMs);
        }
        if (auto* g = a->find("gate")) {
            auto& gc = in.air.gate;
            gc.type = (AirGateType)g->int_or("type", (int)gc.type);
            gc.pin = (int8_t)g->int_or("pin", gc.pin);
            gc.backend = (PwmBackend)g->int_or("backend", (int)gc.backend);
            gc.pcaChannel = (uint8_t)g->int_or("pca", gc.pcaChannel);
            gc.activeHigh = g->bool_or("activeHigh", gc.activeHigh);
            gc.openTimeoutMs = (uint32_t)g->num_or("openTimeoutMs", gc.openTimeoutMs);
            gc.peak01 = (float)g->num_or("peak01", gc.peak01);
            gc.peakMs = (uint32_t)g->num_or("peakMs", gc.peakMs);
            gc.hold01 = (float)g->num_or("hold01", gc.hold01);
            gc.closed01 = (float)g->num_or("closed01", gc.closed01);
            gc.open01 = (float)g->num_or("open01", gc.open01);
            gc.openDelayMs = (uint32_t)g->num_or("openDelayMs", gc.openDelayMs);
            gc.closeDelayMs = (uint32_t)g->num_or("closeDelayMs", gc.closeDelayMs);
            gc.servoMinUs = (uint16_t)g->int_or("servoMinUs", gc.servoMinUs);
            gc.servoMaxUs = (uint16_t)g->int_or("servoMaxUs", gc.servoMaxUs);
        }
        if (auto* f = a->find("flow")) {
            auto& fc = in.air.flow;
            fc.type = (FlowControlType)f->int_or("type", (int)fc.type);
            fc.pin = (int8_t)f->int_or("pin", fc.pin);
            fc.backend = (PwmBackend)f->int_or("backend", (int)fc.backend);
            fc.pcaChannel = (uint8_t)f->int_or("pca", fc.pcaChannel);
            fc.min = (uint8_t)f->int_or("min", fc.min);
            fc.nominal = (uint8_t)f->int_or("nominal", fc.nominal);
            fc.max = (uint8_t)f->int_or("max", fc.max);
            fc.rest01 = (float)f->num_or("rest01", fc.rest01);
            fc.curve = (VelocityCurve)f->int_or("curve", (int)fc.curve);
            fc.expo = (float)f->num_or("expo", fc.expo);
            fc.maxSlewPerMs = (float)f->num_or("maxSlewPerMs", fc.maxSlewPerMs);
            fc.servoMinUs = (uint16_t)f->int_or("servoMinUs", fc.servoMinUs);
            fc.servoMaxUs = (uint16_t)f->int_or("servoMaxUs", fc.servoMaxUs);
        }
        if (auto* ang = a->find("angle")) {
            auto& ac = in.air.angle;
            ac.enabled = ang->bool_or("enabled", ac.enabled);
            ac.pin = (int8_t)ang->int_or("pin", ac.pin);
            ac.backend = (PwmBackend)ang->int_or("backend", (int)ac.backend);
            ac.pcaChannel = (uint8_t)ang->int_or("pca", ac.pcaChannel);
            ac.rest01 = (float)ang->num_or("rest01", ac.rest01);
            ac.min01 = (float)ang->num_or("min01", ac.min01);
            ac.nominal01 = (float)ang->num_or("nominal01", ac.nominal01);
            ac.max01 = (float)ang->num_or("max01", ac.max01);
            ac.useCc74 = ang->bool_or("useCc74", ac.useCc74);
            ac.servoMinUs = (uint16_t)ang->int_or("servoMinUs", ac.servoMinUs);
            ac.servoMaxUs = (uint16_t)ang->int_or("servoMaxUs", ac.servoMaxUs);
        }
        if (auto* se = a->find("sensor")) {
            auto& sc = in.air.sensor;
            sc.type = (AirSensorType)se->int_or("type", (int)sc.type);
            sc.pin = (int8_t)se->int_or("pin", sc.pin);
            sc.i2cAddr = (uint8_t)se->int_or("i2cAddr", sc.i2cAddr);
            sc.rawMin = (float)se->num_or("rawMin", sc.rawMin);
            sc.rawMax = (float)se->num_or("rawMax", sc.rawMax);
            sc.physMin = (float)se->num_or("physMin", sc.physMin);
            sc.physMax = (float)se->num_or("physMax", sc.physMax);
            sc.invert = se->bool_or("invert", sc.invert);
            sc.filterAlpha = (float)se->num_or("filterAlpha", sc.filterAlpha);
            sc.staleTimeoutMs = (uint32_t)se->num_or("staleTimeoutMs", sc.staleTimeoutMs);
            sc.physLo = (float)se->num_or("physLo", sc.physLo);
            sc.physHi = (float)se->num_or("physHi", sc.physHi);
        }
        in.air.valveOpenTimeoutMs = (uint32_t)a->num_or("valveOpenTimeoutMs", in.air.valveOpenTimeoutMs);
        in.air.minNoteMs = (uint32_t)a->num_or("minNoteMs", in.air.minNoteMs);
    }
    if (auto* sq = v.find("seq")) {
        in.seq.mono = (MonoPolicy)sq->int_or("mono", (int)in.seq.mono);
        in.seq.legato = (LegatoPolicy)sq->int_or("legato", (int)in.seq.legato);
        in.seq.legatoMaxDistanceMm = (float)sq->num_or("legatoMaxDistanceMm", in.seq.legatoMaxDistanceMm);
        in.seq.legatoMaxTimeMs = (uint32_t)sq->num_or("legatoMaxTimeMs", in.seq.legatoMaxTimeMs);
        in.seq.legatoMaxMoveTimeMs = (float)sq->num_or("legatoMaxMoveTimeMs", in.seq.legatoMaxMoveTimeMs);
        in.seq.minNoteMs = (uint32_t)sq->num_or("minNoteMs", in.seq.minNoteMs);
        in.seq.prepareTimeoutMs = (uint32_t)sq->num_or("prepareTimeoutMs", in.seq.prepareTimeoutMs);
        in.seq.vibratoUnit = (VibratoUnit)sq->int_or("vibratoUnit", (int)in.seq.vibratoUnit);
        in.seq.vibratoRateHz = (float)sq->num_or("vibratoRateHz", in.seq.vibratoRateHz);
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
            NoteEntry& e = in.map.entry((uint8_t)note);
            e.positionToleranceMm = (float)pt.num_or("tol", e.positionToleranceMm);
            e.airMin = (uint8_t)pt.int_or("airMin", e.airMin);
            e.airMax = (uint8_t)pt.int_or("airMax", e.airMax);
            e.enabled = pt.bool_or("enabled", true);
            e.calibrated = pt.bool_or("calibrated", true);  // preserve provisional status
        }
    }
}

// --- validation --------------------------------------------------------------
inline bool enumOk(int v, int hi) { return v >= 0 && v <= hi; }

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
