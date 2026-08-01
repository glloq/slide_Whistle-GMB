/*
 * core/ApiRouter.h — portable /api/v1 request dispatcher.
 *
 * The whole request-handling policy lives here so it is unit-tested without a
 * web server: auth + Origin + rate-limit gate, request size / content-type
 * checks, transactional config apply, restart_required detection, and the
 * consistent response envelope. A thin ESPAsyncWebServer adapter just forwards
 * (method, path, body, headers) here and ships back {status, body}.
 *
 * Crucially, control routes only ENQUEUE structured commands — the router never
 * touches a GPIO, a servo or the sequencer directly (correction #6).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_APIROUTER_H
#define SWC_CORE_APIROUTER_H

#include <cstring>

#include "ApiResponse.h"
#include "AuthManager.h"
#include "ConfigStore.h"
#include "CommandQueue.h"
#include "Presets.h"

namespace swc {

struct ApiRequest {
    std::string method;       // "GET" / "POST"
    std::string path;         // "/api/v1/config"
    std::string body;
    std::string origin;
    std::string token;        // from Authorization / X-Auth-Token header
    std::string contentType;  // for POST
};
struct ApiReply { int status = 200; std::string body; };

struct ICommandSink { virtual bool push(const Command&) = 0; virtual ~ICommandSink() = default; };
struct IEntropy     { virtual uint32_t next() = 0;           virtual ~IEntropy() = default; };
struct IStatusSource{ virtual JsonValue statusJson() = 0;    virtual ~IStatusSource() = default; };

// Adapter so a CommandQueue<N> can be used as an ICommandSink.
template <uint16_t N>
struct QueueSink : ICommandSink {
    explicit QueueSink(CommandQueue<N>& q) : q_(q) {}
    bool push(const Command& c) override { return q_.push(c); }
    CommandQueue<N>& q_;
};

// True if any parameter that applyDynamic() does NOT push to the live objects
// differs. Instrument::applyDynamic only propagates note range/channel/CC, the
// NoteMap, the sequencer config, motion speed/accel/soft-limits, and the flow
// SHAPING (+ valve timeout + minNoteMs). Everything else below (calibration,
// homing, source/PID/tank, servo windows, sensor calibration, watchdog, …)
// takes effect only at construction, so a change to it needs a reboot — leaving
// it "dynamic" would apply it to hardware built from the previous values
// (review #7 §12). Kept in sync with Instrument::applyDynamic / *::applyDynamic.
inline bool unappliedParamsDiffer(const InstrumentConfig& a, const InstrumentConfig& b) {
    if (a.watchdogMs != b.watchdogMs) return true;
    // --- motion top-level (speed/accel/softlimits ARE dynamic → excluded) ---
    const auto& am = a.motion; const auto& bm = b.motion;
    if (am.travelMm != bm.travelMm || am.dualMode != bm.dualMode ||
        am.servoBEnabled != bm.servoBEnabled) return true;
    // --- stepper ---
    const auto& as = am.stepper; const auto& bs = bm.stepper;
    if (as.stepsPerRev != bs.stepsPerRev || as.microsteps != bs.microsteps ||
        as.stepsPerMm != bs.stepsPerMm || as.enableActiveHigh != bs.enableActiveHigh ||
        as.invertDir != bs.invertDir || as.homingFastMmS != bs.homingFastMmS ||
        as.homingSlowMmS != bs.homingSlowMmS || as.homeTowardZero != bs.homeTowardZero ||
        as.homeOffsetMm != bs.homeOffsetMm || as.homeBackoffMm != bs.homeBackoffMm ||
        as.phaseTimeoutMs != bs.phaseTimeoutMs || as.idleDisableMs != bs.idleDisableMs ||
        as.alwaysHold != bs.alwaysHold) return true;
    auto endstopDiff = [](const EndstopConfig& x, const EndstopConfig& y) {
        return x.present != y.present || x.normallyClosed != y.normallyClosed ||
               x.activeHigh != y.activeHigh || x.internalPullup != y.internalPullup;
    };
    if (endstopDiff(as.endstopMin, bs.endstopMin) || endstopDiff(as.endstopMax, bs.endstopMax))
        return true;
    // --- servo calibration (pin/backend/pca compared separately) ---
    auto servoDiff = [](const ServoMotionConfig& x, const ServoMotionConfig& y) {
        if (x.freqHz != y.freqHz || x.minUs != y.minUs || x.maxUs != y.maxUs ||
            x.invert != y.invert || x.restUs != y.restUs || x.safeUs != y.safeUs ||
            x.trimUs != y.trimUs || x.offsetUs != y.offsetUs ||
            x.detachIdleMs != y.detachIdleMs || x.calCount != y.calCount) return true;
        for (uint8_t i = 0; i < x.calCount && i < 8; ++i)
            if (x.cal[i].mm != y.cal[i].mm || x.cal[i].us != y.cal[i].us) return true;
        return false;
    };
    if (servoDiff(am.servoA, bm.servoA) || servoDiff(am.servoB, bm.servoB)) return true;
    // --- air source (type/pumpCount/pins compared separately) ---
    const auto& ax = a.air.source; const auto& bx = b.air.source;
    if (ax.idle01 != bx.idle01 || ax.min01 != bx.min01 || ax.max01 != bx.max01 ||
        ax.startBoost01 != bx.startBoost01 || ax.spinUpMs != bx.spinUpMs ||
        ax.stopDelayMs != bx.stopDelayMs || ax.cascadeDelayMs != bx.cascadeDelayMs ||
        ax.tankMode != bx.tankMode || ax.tankPwm != bx.tankPwm || ax.target != bx.target ||
        ax.pidKp != bx.pidKp || ax.pidKi != bx.pidKi || ax.lowThresh != bx.lowThresh ||
        ax.highThresh != bx.highThresh || ax.safetyThresh != bx.safetyThresh ||
        ax.refillTimeoutMs != bx.refillTimeoutMs || ax.minOffMs != bx.minOffMs ||
        ax.requireSensor != bx.requireSensor || ax.activeHigh != bx.activeHigh) return true;
    // --- gate (type/pin/backend/pca compared separately) ---
    const auto& ag = a.air.gate; const auto& bg = b.air.gate;
    if (ag.servoMinUs != bg.servoMinUs || ag.servoMaxUs != bg.servoMaxUs ||
        ag.activeHigh != bg.activeHigh || ag.openTimeoutMs != bg.openTimeoutMs ||
        ag.peak01 != bg.peak01 || ag.peakMs != bg.peakMs || ag.hold01 != bg.hold01 ||
        ag.closed01 != bg.closed01 || ag.open01 != bg.open01 ||
        ag.openDelayMs != bg.openDelayMs || ag.closeDelayMs != bg.closeDelayMs) return true;
    // --- flow servo window (shaping min/nominal/max/curve/expo/slew/rest ARE dynamic) ---
    const auto& af = a.air.flow; const auto& bf = b.air.flow;
    if (af.servoMinUs != bf.servoMinUs || af.servoMaxUs != bf.servoMaxUs ||
        af.activeHigh != bf.activeHigh) return true;
    // --- angle (enabled/pin/backend/pca compared separately) ---
    const auto& aa = a.air.angle; const auto& ba = b.air.angle;
    if (aa.servoMinUs != ba.servoMinUs || aa.servoMaxUs != ba.servoMaxUs ||
        aa.rest01 != ba.rest01 || aa.min01 != ba.min01 || aa.nominal01 != ba.nominal01 ||
        aa.max01 != ba.max01 || aa.useCc74 != ba.useCc74) return true;
    // --- sensor calibration (type/pin compared separately) ---
    const auto& an = a.air.sensor; const auto& bn = b.air.sensor;
    if (an.i2cAddr != bn.i2cAddr || an.rawMin != bn.rawMin || an.rawMax != bn.rawMax ||
        an.physMin != bn.physMin || an.physMax != bn.physMax || an.invert != bn.invert ||
        an.filterAlpha != bn.filterAlpha || an.staleTimeoutMs != bn.staleTimeoutMs ||
        an.physLo != bn.physLo || an.physHi != bn.physHi) return true;
    return false;
}

// True if applying `nn` over `oo` changes a *hardware* resource (needs reboot,
// Section 10) rather than a purely dynamic parameter.
inline bool configNeedsRestart(const RuntimeConfig& oo, const RuntimeConfig& nn) {
    if (oo.device.board != nn.device.board) return true;
    if (oo.instrumentCount != nn.instrumentCount) return true;
    // Network + auth changes re-init the WiFi/AP/session stack, which
    // applyDynamic() does NOT do — so they need a reboot (review #4 §P1).
    const auto& on = oo.network; const auto& nnw = nn.network;
    if (on.apEnabled != nnw.apEnabled || on.requireAuth != nnw.requireAuth ||
        on.disableApWhenConnected != nnw.disableApWhenConnected ||
        std::strcmp(on.apSsid, nnw.apSsid) != 0 ||
        std::strcmp(on.allowedOrigin, nnw.allowedOrigin) != 0) return true;
    // MIDI TRANSPORT enable flags need bring-up at boot (transpose stays dynamic).
    const auto& om = oo.midi; const auto& nm = nn.midi;
    if (om.din != nm.din || om.ble != nm.ble || om.rtp != nm.rtp ||
        om.usb != nm.usb || om.webKeyboard != nm.webKeyboard) return true;
    for (uint8_t i = 0; i < nn.instrumentCount && i < MAX_INSTRUMENTS; ++i) {
        const InstrumentConfig& a = oo.instruments[i];
        const InstrumentConfig& b = nn.instruments[i];
        if (a.enabled != b.enabled) return true;
        if (a.motion.type != b.motion.type) return true;
        const auto& as = a.motion.stepper; const auto& bs = b.motion.stepper;
        if (as.stepPin != bs.stepPin || as.dirPin != bs.dirPin || as.enablePin != bs.enablePin ||
            as.endstopMin.pin != bs.endstopMin.pin || as.endstopMax.pin != bs.endstopMax.pin) return true;
        if (a.motion.servoA.pin != b.motion.servoA.pin || a.motion.servoA.backend != b.motion.servoA.backend ||
            a.motion.servoA.pcaChannel != b.motion.servoA.pcaChannel) return true;
        if (a.motion.servoB.pin != b.motion.servoB.pin || a.motion.servoB.backend != b.motion.servoB.backend ||
            a.motion.servoB.pcaChannel != b.motion.servoB.pcaChannel) return true;
        if (a.air.source.type != b.air.source.type || a.air.source.pumpCount != b.air.source.pumpCount) return true;
        for (int p = 0; p < MAX_PUMPS; ++p) if (a.air.source.pin[p] != b.air.source.pin[p]) return true;
        if (a.air.gate.type != b.air.gate.type || a.air.gate.pin != b.air.gate.pin ||
            a.air.gate.backend != b.air.gate.backend || a.air.gate.pcaChannel != b.air.gate.pcaChannel) return true;
        // Flow control TYPE (and its backend/pca) changes the driver, not just a
        // tuning parameter → restart.
        if (a.air.flow.type != b.air.flow.type || a.air.flow.pin != b.air.flow.pin ||
            a.air.flow.backend != b.air.flow.backend || a.air.flow.pcaChannel != b.air.flow.pcaChannel) return true;
        // Jet-angle servo attach/pin/backend/channel.
        if (a.air.angle.enabled != b.air.angle.enabled || a.air.angle.pin != b.air.angle.pin ||
            a.air.angle.backend != b.air.angle.backend || a.air.angle.pcaChannel != b.air.angle.pcaChannel) return true;
        if (a.air.sensor.type != b.air.sensor.type || a.air.sensor.pin != b.air.sensor.pin) return true;
        // Fields that applyDynamic() cannot push to the already-built objects
        // must also force a restart (review #7 §12).
        if (unappliedParamsDiffer(a, b)) return true;
    }
    return false;
}

class ApiRouter {
public:
    void begin(AuthManager* auth, ConfigStore* store, RuntimeConfig* live,
               ICommandSink* sink, IEntropy* entropy, IStatusSource* status) {
        auth_ = auth; store_ = store; live_ = live; sink_ = sink; entropy_ = entropy; status_ = status;
    }
    void setMaxBodyBytes(size_t n) { maxBody_ = n; }
    bool restartRequired() const { return restartRequired_; }
    bool restartRequested() const { return restartRequested_; }
    void clearRestartRequested() { restartRequested_ = false; }

    // Retry a dynamic-config apply that couldn't be queued earlier (queue full).
    // Called from the network loop AND at the start of every request so the
    // "apply_pending" state is real, not cosmetic (review #4 §P1).
    void servicePending() {
        if (dynApplyPending_ && sink_) {
            Command c{}; c.type = CommandType::ApplyDynamicConfig;
            if (sink_->push(c)) dynApplyPending_ = false;
        }
    }
    bool applyPending() const { return dynApplyPending_; }

    ApiReply handle(const ApiRequest& r, uint32_t nowMs) {
        servicePending();
        // Safety commands (panic, Note Off, All Notes/Sound Off) must ALWAYS get
        // through: they are exempt from Origin and rate-limit checks so a stop
        // can never be starved by a burst of notes/CC (correction #7).
        const bool safety = isSafetyRequest(r);

        // 1. request-size guard (Section 14)
        if (r.body.size() > maxBody_) return reply(413, apiErr("BODY_TOO_LARGE", "request body exceeds limit"));
        // 2. Origin check for state-changing methods (safety exempt)
        if (!safety && r.method != "GET" && auth_ && !auth_->originAllowed(r.origin))
            return reply(403, apiErr("BAD_ORIGIN", "Origin not allowed", "Origin"));
        // 3. rate limit (safety exempt), per client so one caller can't starve
        //    the others (review #31). Key by token, else Origin, else anon.
        if (!safety && auth_ && !auth_->allowRequestFor(clientKey(r, nowMs), nowMs))
            return reply(429, apiErr("RATE_LIMITED", "too many requests"));

        // 4. routing
        if (r.path == "/api/v1/session" && r.method == "POST") return routeSession(r, nowMs);
        if (r.path == "/api/v1/status"  && r.method == "GET")  return routeStatus();
        if (r.path == "/api/v1/config"  && r.method == "GET")  return guard(Criticality::Protected, r, nowMs, [&]{ return routeGetConfig(); });
        if (r.path == "/api/v1/config"  && r.method == "POST") return guard(Criticality::Protected, r, nowMs, [&]{ return routePostConfig(r); });
        if (r.path == "/api/v1/preset"  && r.method == "POST") return guard(Criticality::Protected, r, nowMs, [&]{ return routePreset(r); });
        if (r.path == "/api/v1/factory-reset" && r.method == "POST") return guard(Criticality::Protected, r, nowMs, [&]{ return routeFactory(); });
        if (r.path == "/api/v1/restart" && r.method == "POST") return guard(Criticality::Protected, r, nowMs, [&]{ restartRequested_ = true; return reply(200, apiOk()); });
        if (r.path == "/api/v1/command" && r.method == "POST") return routeCommand(r, nowMs);
        return reply(404, apiErr("NOT_FOUND", "unknown route", r.path));
    }

private:
    template <typename Fn>
    ApiReply guard(Criticality c, const ApiRequest& r, uint32_t nowMs, Fn fn) {
        if (auth_ && !auth_->authorize(c, r.token, nowMs))
            return reply(401, apiErr("UNAUTHORIZED", "valid token required", "Authorization"));
        return fn();
    }

    ApiReply routeSession(const ApiRequest& r, uint32_t nowMs) {
        if (!ctJson(r)) return reply(415, apiErr("BAD_CONTENT_TYPE", "expected application/json"));
        JsonValue body; if (!parseJson(r, body)) return reply(400, apiErr("BAD_JSON", "malformed JSON body"));
        std::string admin = body.str_or("token", "");
        uint32_t e[2] = { entropy_ ? entropy_->next() : 1u, entropy_ ? entropy_->next() : 2u };
        std::string s = auth_ ? auth_->openSession(admin, e, nowMs) : "";
        if (s.empty()) return reply(401, apiErr("LOGIN_FAILED", "bad admin token or connection cap reached"));
        JsonValue d = JsonValue::makeObj(); d.set("session", s);
        return reply(200, apiOk(d));
    }

    ApiReply routeStatus() {
        JsonValue s = status_ ? status_->statusJson() : JsonValue::makeObj();
        s.set("restart_required", restartRequired_);   // real vs requested state, shown separately
        return reply(200, apiOk(s));
    }

    ApiReply routeGetConfig() {
        JsonValue wrap; jsonParse(store_->exportJson(*live_), wrap, nullptr);
        return reply(200, apiOk(wrap));
    }

    ApiReply routePostConfig(const ApiRequest& r) {
        if (!ctJson(r)) return reply(415, apiErr("BAD_CONTENT_TYPE", "expected application/json"));
        return applyCandidate(r.body);   // applyCandidate reports 400 on malformed JSON
    }

    ApiReply routePreset(const ApiRequest& r) {
        if (!ctJson(r)) return reply(415, apiErr("BAD_CONTENT_TYPE", "expected application/json"));
        JsonValue body; if (!parseJson(r, body)) return reply(400, apiErr("BAD_JSON", "malformed JSON body"));
        long idx = body.int_or("index", -1);
        long inst = body.int_or("instrument", 0);
        if (idx < 0 || idx >= (long)PresetId::COUNT) return reply(400, apiErr("BAD_PRESET", "unknown preset index", "index"));
        if (inst < 0 || inst >= MAX_INSTRUMENTS) return reply(400, apiErr("BAD_INSTRUMENT", "instrument out of range", "instrument"));
        RuntimeConfig cand = *live_;
        if (inst >= cand.instrumentCount) cand.instrumentCount = (uint8_t)(inst + 1);
        cand.instruments[inst].enabled = true;
        applyPreset(cand.instruments[inst], (PresetId)idx);
        return applyCandidate(store_->exportJson(cand));
    }

    ApiReply routeFactory() {
        RuntimeConfig d;
        if (!store_->factoryReset(d)) return reply(500, apiErr("PERSIST_FAILED", "could not write default config"));
        bool rr = configNeedsRestart(*live_, d);
        *live_ = d; restartRequired_ = restartRequired_ || rr;
        JsonValue data = JsonValue::makeObj(); data.set("restart_required", rr);
        return reply(200, apiOk(data));
    }

    // Validate (structural + hardware) then persist, transactionally. Never
    // applies a partial config; never reports success on failure.
    ApiReply applyCandidate(const std::string& json) {
        RuntimeConfig cand;
        ConfigDecodeResult dec = configFromJson(json, cand);
        if (!dec.ok) return reply(400, apiErr("CONFIG_INVALID", dec.error.empty() ? "config rejected" : dec.error));
        // If the payload carried an integrity checksum, it MUST match — the
        // normal POST path enforced only dec.ok, unlike ConfigStore::importJson,
        // so a tampered wrapped config could slip through (review #5 §22). An
        // unwrapped config has checksumOk=true, so this only rejects real tamper.
        if (!dec.checksumOk) return reply(400, apiErr("BAD_CHECKSUM", "config checksum mismatch"));
        HardwareResourceValidator v; buildClaims(v, cand);
        auto issues = v.validate();
        if (HardwareResourceValidator::hasErrors(issues)) {
            for (const auto& is : issues)
                if (is.severity == Severity::Error) return reply(400, apiErr(is.code, is.message, is.field));
        }
        if (!store_->save(cand)) return reply(500, apiErr("PERSIST_FAILED", "could not persist config"));
        bool rr = configNeedsRestart(*live_, cand);
        *live_ = cand;
        bool dynQueued = false;
        if (rr) {
            restartRequired_ = true;       // hardware change: needs reboot
        } else if (sink_) {
            // Dynamic-only change: tell the RT task to apply it. Only claim it is
            // applied if it actually made it onto the queue (review #5).
            Command c{}; c.type = CommandType::ApplyDynamicConfig;
            dynQueued = sink_->push(c);
            // Queue full → record a REAL pending apply that servicePending()
            // retries, instead of a fake apply_pending that never applies
            // (review #4 §P1). The saved config is already the live struct, so a
            // later ApplyDynamicConfig will push it into the objects.
            if (!dynQueued) dynApplyPending_ = true;
        }
        JsonValue data = JsonValue::makeObj();
        data.set("restart_required", rr);
        data.set("saved", true);
        data.set("applied", rr ? false : dynQueued);   // truthful
        if (!rr && !dynQueued) data.set("apply_pending", true);   // saved, retried by servicePending()
        return reply(200, apiFromValidationOk(issues, data));
    }

    ApiReply routeCommand(const ApiRequest& r, uint32_t nowMs) {
        if (!ctJson(r)) return reply(415, apiErr("BAD_CONTENT_TYPE", "expected application/json"));
        JsonValue body; if (!parseJson(r, body)) return reply(400, apiErr("BAD_JSON", "malformed JSON body"));
        Command c;
        std::string type = body.str_or("type", "");
        if      (type == "noteOn")  c.type = CommandType::NoteOn;
        else if (type == "noteOff") c.type = CommandType::NoteOff;
        else if (type == "cc")      c.type = CommandType::ControlChange;
        else if (type == "pitch")   c.type = CommandType::PitchBend;
        else if (type == "panic")   c.type = CommandType::Panic;
        else if (type == "home")    c.type = CommandType::Home;
        else if (type == "jog")     c.type = CommandType::Jog;
        else if (type == "testActuator") c.type = CommandType::TestActuator;
        else if (type == "testAir")      c.type = CommandType::TestAir;
        else if (type == "rearm")        c.type = CommandType::Rearm;
        else return reply(400, apiErr("BAD_COMMAND", "unknown command type", "type"));

        // Validate raw integer ranges BEFORE narrowing to the small Command
        // fields — otherwise channel 256→0, note 300→44, instrument 256→0 and a
        // >32767 ms duration→negative all slip through as plausible values
        // (review #4 §P1 command validation).
        long channel = body.int_or("channel", 1);
        long a       = body.int_or("a", body.int_or("note", body.int_or("cc", 0)));
        long b       = body.int_or("b", body.int_or("velocity", body.int_or("value", 0)));
        long inst    = body.int_or("instrument", 0);
        if (channel < 0 || channel > 16)          return reply(400, apiErr("BAD_RANGE", "channel out of 0..16", "channel"));
        if (a < 0 || a > 127)                     return reply(400, apiErr("BAD_RANGE", "note/cc out of 0..127", "a"));
        if (b < 0 || b > 127)                     return reply(400, apiErr("BAD_RANGE", "value out of 0..127", "b"));
        if (inst < 0 || inst >= MAX_INSTRUMENTS)  return reply(400, apiErr("BAD_INSTRUMENT", "instrument out of range", "instrument"));
        c.channel    = (uint8_t)channel;
        c.a          = (uint8_t)a;
        c.b          = (uint8_t)b;
        c.instrument = (uint8_t)inst;
        // i16 is a typed payload whose meaning depends on the command: signed
        // pitch-bend for pitch, signed jog delta (mm) for jog, and the auto-stop
        // duration (ms) for testAir — each read from its own key (#3 §7.4).
        long i16;
        if      (c.type == CommandType::Jog)     i16 = body.int_or("delta", body.int_or("deltaMm", 0));
        else if (c.type == CommandType::TestAir) i16 = body.int_or("ms", body.int_or("durationMs", 3000));
        else                                     i16 = body.int_or("bend", 0);
        if (i16 < -32768 || i16 > 32767)          return reply(400, apiErr("BAD_RANGE", "payload out of int16 range", "value"));
        c.i16 = (int16_t)i16;

        // Safety commands (panic, Note Off, All Notes/Sound Off) always pass —
        // they need no token so a release/stop can never be blocked (#7).
        bool safe = (c.type == CommandType::Panic || c.type == CommandType::NoteOff ||
                     (c.type == CommandType::ControlChange && (c.a == 120 || c.a == 123)));
        // Stamp a monotonic command id so a client can match the RT task's
        // execution ack (returned in /status) to this request (#3 §7).
        c.seq = ++cmdSeq_;
        Criticality crit = safe ? Criticality::Public : criticalityFor(c.type);
        if (!auth_ || auth_->authorize(crit, r.token, nowMs)) {
            if (!sink_ || !sink_->push(c)) return reply(503, apiErr("QUEUE_FULL", "command queue full"));
            JsonValue d = JsonValue::makeObj(); d.set("queued", true); d.set("seq", (double)c.seq);
            return reply(202, apiOk(d));   // accepted — executed by the RT task
        }
        return reply(401, apiErr("UNAUTHORIZED", "valid token required", "Authorization"));
    }

    // ok envelope that still surfaces any warnings alongside the data
    std::string apiFromValidationOk(const std::vector<ValidationIssue>& issues, const JsonValue& data) {
        JsonValue merged = data;
        JsonValue warns = JsonValue::makeArr();
        for (const auto& i : issues) {
            if (i.severity != Severity::Warning) continue;
            JsonValue w = JsonValue::makeObj(); w.set("code", i.code); w.set("message", i.message); w.set("field", i.field);
            warns.arr.push_back(w);
        }
        merged.set("warnings", warns);
        return apiOk(merged);
    }

    // A request is "safety" if it is a command that stops/releases the machine.
    bool isSafetyRequest(const ApiRequest& r) const {
        if (r.method != "POST" || r.path != "/api/v1/command") return false;
        if (r.contentType.find("application/json") == std::string::npos) return false;
        JsonValue b; if (!jsonParse(r.body, b, nullptr)) return false;
        std::string t = b.str_or("type", "");
        if (t == "panic" || t == "noteOff") return true;
        if (t == "cc") { long a = b.int_or("a", b.int_or("cc", -1)); return a == 120 || a == 123; }
        return false;
    }

    // Only a VERIFIED token is a trustworthy per-client rate-limit key —
    // otherwise an attacker rotates fake tokens for unlimited buckets, both
    // bypassing the limit and growing the map without bound. Everything
    // unverified shares one bucket per origin (review #4 network security).
    std::string clientKey(const ApiRequest& r, uint32_t nowMs) const {
        if (!r.token.empty() && auth_ && auth_->isKnownToken(r.token, nowMs)) return "t:" + r.token;
        if (!r.origin.empty()) return "o:" + r.origin;
        return "unauth";
    }

    bool ctJson(const ApiRequest& r) const { return r.contentType.find("application/json") != std::string::npos; }
    // returns true if body parsed; out.type stays Null on failure
    bool parseJson(const ApiRequest& r, JsonValue& out) { return jsonParse(r.body, out, nullptr); }
    static ApiReply reply(int status, const std::string& body) { return ApiReply{status, body}; }

    AuthManager*   auth_ = nullptr;
    ConfigStore*   store_ = nullptr;
    RuntimeConfig* live_ = nullptr;
    ICommandSink*  sink_ = nullptr;
    IEntropy*      entropy_ = nullptr;
    IStatusSource* status_ = nullptr;
    size_t         maxBody_ = 16384;
    bool           restartRequired_ = false;
    bool           restartRequested_ = false;
    bool           dynApplyPending_ = false;   // a dynamic apply awaiting queue space
    uint32_t       cmdSeq_ = 0;     // monotonic id stamped on each enqueued command
};

} // namespace swc

#endif // SWC_CORE_APIROUTER_H
