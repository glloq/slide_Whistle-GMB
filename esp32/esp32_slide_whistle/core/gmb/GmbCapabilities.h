/*
 * core/gmb/GmbCapabilities.h — the GMB capability snapshot, derived from the
 * ACTIVE RuntimeConfig / NoteMap and from nothing else.
 *
 * There is deliberately no second, hand-maintained "GMB profile": every field
 * announced to General-Midi-Boop is computed here from the validated
 * configuration the firmware is actually running, so the two can never drift.
 *
 * The three rules this file exists to enforce:
 *
 *  1. A note is advertised only when it is genuinely resolvable. NoteMap's
 *     positionForNote() CLAMPS beyond the outermost mapped point, so it returns
 *     a position for notes that have no mapping at all; resolveNoteStrict()
 *     below is the side-effect-free capability helper that refuses to do that.
 *
 *  2. ABSENT = UNKNOWN. A timing field is emitted only when it can be defended
 *     from the actual actuator/air configuration; otherwise it is left out. It
 *     is never emitted as 0 to mean "don't know". In particular
 *     SequencerConfig::prepareTimeoutMs is a FAILURE timeout and is never
 *     exposed as prepare.max_ms.
 *
 *  3. Descriptor channels are unique. Several physical flutes that resolve to
 *     the same MIDI channel merge into ONE logical instrument with several
 *     `voices`, never into duplicate channel entries (which the GMB validator
 *     rejects outright).
 *
 * Pure, allocation-tolerant control-plane code: it runs on configuration
 * activation, never in the real-time actuator task, and it touches no hardware.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBCAPABILITIES_H
#define SWC_CORE_GMB_GMBCAPABILITIES_H

#include <cmath>
#include <string>
#include <vector>

#include "GmbIdentity.h"
#include "GmbProtocol.h"
#include "../RuntimeConfig.h"

namespace swc {
namespace gmb {

// The slide whistle's musical identity in General-Midi-Boop's own vocabulary
// (its src/midi/adaptation/InstrumentTypeConfig.js: pipe → whistle → GM 78).
static constexpr uint8_t  GM_PROGRAM_WHISTLE = 78;
static constexpr const char* INSTRUMENT_TYPE    = "pipe";
static constexpr const char* INSTRUMENT_SUBTYPE = "whistle";

// ---------------------------------------------------------------------------
// A set of MIDI notes, as a 128-bit map. Small, copyable, order-free.
// ---------------------------------------------------------------------------
class GmbNoteSet {
public:
    void clear() { for (int i = 0; i < 4; ++i) w_[i] = 0; }
    void add(uint8_t n) { if (n < 128) w_[n >> 5] |= (1u << (n & 31)); }
    bool has(uint8_t n) const { return n < 128 && (w_[n >> 5] & (1u << (n & 31))) != 0; }
    bool empty() const { return !(w_[0] | w_[1] | w_[2] | w_[3]); }

    uint8_t count() const {
        uint8_t c = 0;
        for (int n = 0; n < 128; ++n) if (has(uint8_t(n))) ++c;
        return c;
    }
    int min() const { for (int n = 0; n < 128; ++n) if (has(uint8_t(n))) return n; return -1; }
    int max() const { for (int n = 127; n >= 0; --n) if (has(uint8_t(n))) return n; return -1; }

    // True when every semitone between min() and max() is present, i.e. the set
    // can honestly be announced as {"mode":"range"}.
    bool contiguous() const {
        int lo = min(), hi = max();
        if (lo < 0) return false;
        for (int n = lo; n <= hi; ++n) if (!has(uint8_t(n))) return false;
        return true;
    }

    std::vector<uint8_t> list() const {
        std::vector<uint8_t> v;
        for (int n = 0; n < 128; ++n) if (has(uint8_t(n))) v.push_back(uint8_t(n));
        return v;
    }

    void unite(const GmbNoteSet& o) { for (int i = 0; i < 4; ++i) w_[i] |= o.w_[i]; }
    bool intersects(const GmbNoteSet& o) const {
        for (int i = 0; i < 4; ++i) if (w_[i] & o.w_[i]) return true;
        return false;
    }
    bool operator==(const GmbNoteSet& o) const {
        for (int i = 0; i < 4; ++i) if (w_[i] != o.w_[i]) return false;
        return true;
    }

private:
    uint32_t w_[4] = {0, 0, 0, 0};
};

// ---------------------------------------------------------------------------
// Strict (non-clamping) note resolution — the capability helper.
//
// NoteMap::positionForNote() answers "where do I send the slide", and for that
// job clamping to the nearest mapped endpoint is the right behaviour. It is the
// WRONG basis for a capability claim: it returns true (and the endpoint
// position) for a note that is nowhere near the mapped span, which would make
// the descriptor promise notes the instrument cannot play in tune.
//
// A note is resolvable here only when it sits INSIDE the enabled span: there is
// an enabled map point at or below it AND one at or above it. Provisional
// points (enabled but calibrated == false) count — "not hardware calibrated" is
// not the same as "unplayable" — exactly as NoteMap::usable() decides.
// ---------------------------------------------------------------------------
struct NoteResolution {
    bool  resolvable = false;
    float positionMm = 0.0f;
};

inline NoteResolution resolveNoteStrict(const NoteMap& map, uint8_t note) {
    NoteResolution r;
    if (note >= MIDI_NOTE_COUNT) return r;
    int lo = -1, hi = -1;
    for (int n = int(note); n >= 0; --n)
        if (map.entry(uint8_t(n)).enabled) { lo = n; break; }
    for (int n = int(note); n < int(MIDI_NOTE_COUNT); ++n)
        if (map.entry(uint8_t(n)).enabled) { hi = n; break; }
    if (lo < 0 || hi < 0) return r;            // outside the mapped span: NOT playable
    if (lo == hi) { r.resolvable = true; r.positionMm = map.entry(uint8_t(lo)).positionMm; return r; }
    float t = float(int(note) - lo) / float(hi - lo);
    r.resolvable = true;
    r.positionMm = lerp(map.entry(uint8_t(lo)).positionMm, map.entry(uint8_t(hi)).positionMm, t);
    return r;
}

// ---------------------------------------------------------------------------
// Snapshot structures
// ---------------------------------------------------------------------------
struct GmbVoice {
    std::string id;        // stable: "flute<physical index>"
    GmbNoteSet  notes;
};

// The two-phase timing model of §5.6, with an explicit "known" flag per field.
// Nothing is defaulted to 0: a field that is not known is simply not emitted.
struct GmbTiming {
    bool     hasPrepare    = false;
    uint16_t prepareBaseMs = 0;    // distance-independent component (air spin-up)
    uint16_t prepareMaxMs  = 0;    // worst case over the advertised span
    bool     prepareSilent = true; // false when the air can stay open during the move
    bool     hasMinNote    = false;
    uint16_t minNoteMs     = 0;
    // excite.latency_ms, release_ms and rearticulation_ms are deliberately
    // ABSENT: this firmware has no acoustic onset measurement, and a servo
    // valve's openDelayMs is a mechanism delay, not a measured acoustic
    // latency. Emitting 0 would read as "instantaneous", which is a lie.
};

struct GmbExpression {
    std::vector<uint8_t> cc;                     // sorted, deduplicated
    bool    pitchBend = false;
    uint8_t pitchBendRangeSemitones = 0;
    bool    channelAftertouch = false;
    bool    polyAftertouch = false;              // not implemented
    bool    velocity = false;
};

// The free `physical` namespace of §5.9. Strings that could not be agreed on
// across merged voices are left empty and then omitted from the JSON.
struct GmbPhysical {
    std::string family          = "winds";
    std::string mechanism       = "variable_length_slide";
    bool        continuousPitch = true;
    std::string slideDrive;      // step_dir | single_servo | dual_servo
    bool        hasTravelMm = false;
    float       travelMm    = 0.0f;
    bool        requiresHoming = false;
    std::string airSource;       // external_passive | fan_onoff | fan_pwm | pumps_direct | pumps_tank
    std::string airGate;         // none | solenoid | solenoid_pwm | servo_valve | servo_diverter | flow_servo_valve
    std::string flowControl;     // none | flow_servo | proportional_pwm | fan_pwm | pump_pwm
    std::string legato;          // always_close | glissando | hold_within_time | ...
    int         transposeSemitones = 0;
    bool        omni = false;    // a contributing flute listens on every channel
};

struct GmbInstrumentCaps {
    uint8_t     channel = 0;         // descriptor channel 0..15 (unique)
    bool        configured = false;
    std::string name;
    uint8_t     gmProgram = GM_PROGRAM_WHISTLE;
    std::string type    = INSTRUMENT_TYPE;
    std::string subtype = INSTRUMENT_SUBTYPE;
    GmbNoteSet  notes;               // union of the voices
    std::vector<GmbVoice> voices;
    uint8_t     polyphonyMax = 1;
    bool        oneNotePerVoice = false;
    GmbTiming   timing;
    GmbExpression expression;
    GmbPhysical physical;
};

struct GmbSnapshot {
    GmbIdentity identity;
    uint32_t    revision = 0;
    std::vector<GmbInstrumentCaps> instruments;   // ALWAYS at least one entry
};

// ---------------------------------------------------------------------------
// Derivation helpers (each defensible from a concrete piece of the firmware)
// ---------------------------------------------------------------------------

// Descriptor channel for a configured MIDI channel.
//   config 1..16 → descriptor 0..15
//   config 0     → OMNI. GMB has no omni representation, so the instrument is
//                  advertised on channel 0 — a channel it really does answer on
//                  — and `physical.omni` records the truth. Merging then keeps
//                  channels unique if another flute also lands on 0.
inline uint8_t descriptorChannel(uint8_t configChannel) {
    if (configChannel == 0) return 0;
    if (configChannel > 16) return 0;
    return uint8_t(configChannel - 1);
}

// The set of PHYSICAL pitches this flute can actually sound.
//
// `transpose` is the firmware's global MIDI transpose: the engine plays
// (incoming note + transpose), so a physical pitch p is only reachable when the
// host can send p - transpose as a legal MIDI note. Pitches that no MIDI note
// could reach are not advertised.
inline GmbNoteSet playableNotes(const InstrumentConfig& ic, int transpose) {
    GmbNoteSet s;
    if (!ic.enabled) return s;
    if (ic.motion.type == SlideDriveType::Disabled) return s;   // slide cannot move: no pitch control
    const float softLo = ic.motion.softMinMm;
    const float softHi = ic.motion.softMaxMm;
    const float travel = ic.motion.travelMm;
    if (!(travel > 0.0f) || !(softLo <= softHi)) return s;
    for (int n = 0; n < 128; ++n) {
        if (n < int(ic.noteMin) || n > int(ic.noteMax)) continue;
        const int hostNote = n - transpose;
        if (hostNote < 0 || hostNote > 127) continue;            // unreachable over MIDI
        NoteResolution r = resolveNoteStrict(ic.map, uint8_t(n));
        if (!r.resolvable) continue;
        if (r.positionMm < softLo - 1e-3f || r.positionMm > softHi + 1e-3f) continue;
        if (r.positionMm < -1e-3f || r.positionMm > travel + 1e-3f) continue;
        s.add(uint8_t(n));
    }
    return s;
}

// A flute is MUSICALLY configured when the persisted configuration can actually
// produce notes. This is a property of the stored configuration only — never of
// a transient runtime state (unhomed, moving, faulted, muted), which §9 of the
// protocol explicitly keeps out of `configured`.
inline bool instrumentConfigured(const InstrumentConfig& ic, bool configValid, int transpose) {
    if (!configValid || !ic.enabled) return false;
    return !playableNotes(ic, transpose).empty();
}

// Worst-case slide travel time (ms) over `spanMm`, from the symmetric
// trapezoidal profile the actuator integrator actually runs (accel == decel ==
// accelMmS2, capped at maxSpeedMmS). This is a real mechanical upper bound, not
// a timeout.
inline uint32_t slideTravelMs(float spanMm, float maxSpeedMmS, float accelMmS2) {
    if (!(spanMm > 0.0f)) return 0;
    if (!(maxSpeedMmS > 0.0f) || !(accelMmS2 > 0.0f)) return 0;
    const float triangularSpan = (maxSpeedMmS * maxSpeedMmS) / accelMmS2;  // accel + decel, no cruise
    float seconds;
    if (spanMm <= triangularSpan) seconds = 2.0f * std::sqrt(spanMm / accelMmS2);
    else                          seconds = spanMm / maxSpeedMmS + maxSpeedMmS / accelMmS2;
    float ms = std::ceil(seconds * 1000.0f);
    if (!(ms > 0.0f)) return 0;
    if (ms > 60000.0f) ms = 60000.0f;
    return uint32_t(ms);
}

// Deterministic air-preparation time (ms). `known` is false when the source's
// readiness is state-dependent and no honest constant exists.
struct AirPrepare { bool known = false; uint32_t ms = 0; };

inline AirPrepare airPrepareMs(const AirConfig& air) {
    AirPrepare p;
    switch (air.source.type) {
        case AirSourceType::ExternalPassive:
            // A regulated external supply is always available: ExternalPassiveSource
            // ::ready() returns true unconditionally. Zero here is a MEASURED zero,
            // not an unknown.
            p.known = true; p.ms = 0; break;
        case AirSourceType::FanOnOff:
        case AirSourceType::FanPwm:
            // FanSource::ready() flips spinUpMs after prepare(): a real, configured
            // preparation component.
            p.known = true; p.ms = air.source.spinUpMs; break;
        case AirSourceType::PumpsDirect: {
            // PumpDirectSource::ready() waits for the cascade to have started every
            // pump, i.e. (pumpCount - 1) stagger delays.
            uint8_t n = clampv<uint8_t>(air.source.pumpCount, 1, MAX_PUMPS);
            p.known = true; p.ms = uint32_t(n - 1) * air.source.cascadeDelayMs; break;
        }
        case AirSourceType::PumpsTank:
            // Readiness depends on the tank's current pressure/level, so the wait
            // ranges from nothing to a full refill. There is no honest constant —
            // leave it unknown rather than invent one.
            p.known = false; break;
    }
    return p;
}

// True when a note's air is guaranteed to be cut while the slide moves. Only
// LegatoPolicy::AlwaysClose guarantees it: Glissando always holds the air open
// across the move, and every HoldWithin*/SafetyLargeMove policy holds it under
// a runtime condition, so the movement can be audible.
inline bool prepareIsSilent(LegatoPolicy legato) {
    return legato == LegatoPolicy::AlwaysClose;
}

// Does changing MIDI velocity materially change the generated air/output?
//
//   AirSystem::startNote() → src_->run(r) and flow_->setTarget(airNominal ? airNominal : velocity)
//
// so velocity reaches the hardware through two independent paths:
//   * the SOURCE — Fan/PumpsDirect lerp(min01, max01, velocity/127), which only
//     changes anything when min01 != max01;
//   * the FLOW  — but only for notes whose calibrated airNominal is 0, because a
//     non-zero airNominal overrides velocity entirely.
inline bool velocityEffective(const InstrumentConfig& ic, const GmbNoteSet& playable) {
    const AirConfig& a = ic.air;
    const bool sourceResponds =
        (a.source.type == AirSourceType::FanOnOff || a.source.type == AirSourceType::FanPwm ||
         a.source.type == AirSourceType::PumpsDirect) &&
        std::fabs(a.source.max01 - a.source.min01) > 1e-4f;
    if (sourceResponds) return true;

    const bool flowResponds = (a.flow.type != FlowControlType::None) && (a.flow.max > a.flow.min);
    if (!flowResponds) return false;
    // At least one advertised note must leave airNominal unset for velocity to
    // reach the flow controller at all.
    for (int n = 0; n < 128; ++n)
        if (playable.has(uint8_t(n)) && ic.map.entry(uint8_t(n)).airNominal == 0) return true;
    return false;
}

// The CC numbers this flute really consumes, from the ACTIVE CcMap and the
// hardware/functions that are actually enabled. A mapping of 0 means "disabled"
// and is ignored; a CC whose target hardware is absent is not advertised.
inline std::vector<uint8_t> expressionCcs(const InstrumentConfig& ic) {
    std::vector<uint8_t> out;
    auto push = [&out](uint8_t cc) {
        if (cc == 0 || cc > 127) return;
        for (uint8_t v : out) if (v == cc) return;
        out.push_back(cc);
    };
    const bool flowActive   = ic.air.flow.type != FlowControlType::None;
    const bool slideActive  = ic.motion.type != SlideDriveType::Disabled;
    // breath / expression / volume all land on AirSystem::updateExpression(),
    // which only does something when there is a flow controller to retarget.
    if (flowActive) { push(ic.cc.breath); push(ic.cc.expression); push(ic.cc.volume); }
    // vibrato modulates the slide position, so it needs a moving slide AND the
    // vibrato routing enabled (Instrument::controlChange checks both).
    if (slideActive && ic.cc.vibratoEnabled) push(ic.cc.vibrato);
    // sustain is pure sequencer logic — always available.
    push(ic.cc.sustain);
    // the jet-angle CC only reaches hardware when the angle servo is attached.
    if (ic.air.angle.enabled) push(ic.cc.angle);
    // ascending order, so the list is stable across rebuilds (the signature and
    // therefore the revision must not move just because a field order changed).
    for (size_t i = 1; i < out.size(); ++i) {
        uint8_t v = out[i]; size_t j = i;
        while (j > 0 && out[j - 1] > v) { out[j] = out[j - 1]; --j; }
        out[j] = v;
    }
    return out;
}

inline const char* slideDriveName(SlideDriveType t) {
    switch (t) {
        case SlideDriveType::StepDir:     return "step_dir";
        case SlideDriveType::SingleServo: return "single_servo";
        case SlideDriveType::DualServo:   return "dual_servo";
        case SlideDriveType::Disabled:    return "none";
    }
    return "none";
}
inline const char* airSourceName(AirSourceType t) {
    switch (t) {
        case AirSourceType::ExternalPassive: return "external_passive";
        case AirSourceType::FanOnOff:        return "fan_onoff";
        case AirSourceType::FanPwm:          return "fan_pwm";
        case AirSourceType::PumpsDirect:     return "pumps_direct";
        case AirSourceType::PumpsTank:       return "pumps_tank";
    }
    return "external_passive";
}
inline const char* airGateName(AirGateType t) {
    switch (t) {
        case AirGateType::None:             return "none";
        case AirGateType::SolenoidSimple:   return "solenoid";
        case AirGateType::SolenoidPwm:      return "solenoid_pwm";
        case AirGateType::ServoValve:       return "servo_valve";
        case AirGateType::ServoDiverter:    return "servo_diverter";
        case AirGateType::FlowServoAsValve: return "flow_servo_valve";
    }
    return "none";
}
inline const char* flowControlName(FlowControlType t) {
    switch (t) {
        case FlowControlType::None:                return "none";
        case FlowControlType::FlowServo:           return "flow_servo";
        case FlowControlType::ProportionalPwm:     return "proportional_pwm";
        case FlowControlType::FanPwm:              return "fan_pwm";
        case FlowControlType::PumpPwm:             return "pump_pwm";
        case FlowControlType::SourcePlusFlowServo: return "source_plus_flow_servo";
    }
    return "none";
}
inline const char* legatoName(LegatoPolicy p) {
    switch (p) {
        case LegatoPolicy::AlwaysClose:        return "always_close";
        case LegatoPolicy::HoldWithinTime:     return "hold_within_time";
        case LegatoPolicy::HoldWithinDistance: return "hold_within_distance";
        case LegatoPolicy::HoldWithinMoveTime: return "hold_within_move_time";
        case LegatoPolicy::Glissando:          return "glissando";
        case LegatoPolicy::SafetyLargeMove:    return "safety_large_move";
    }
    return "always_close";
}

// ---------------------------------------------------------------------------
// Snapshot builder
// ---------------------------------------------------------------------------

// Everything the builder reads. Plain references into the ACTIVE validated
// configuration, so the snapshot can only ever describe what is really running.
struct GmbBuildInput {
    const RuntimeConfig* config = nullptr;
    bool     configValid = false;   // the active config passed hardware validation
    GmbIdentity identity;           // instance id + firmware, supplied by the platform
    float    pitchBendRangeSemitones = 0.0f;   // the range the runtime really uses
};

namespace detail {

// One physical flute's contribution before merging.
struct Contribution {
    uint8_t index = 0;
    const InstrumentConfig* cfg = nullptr;
    GmbNoteSet notes;
    bool configured = false;
    bool omni = false;
};

// Keep `dst` only when every contributor agrees on it; otherwise the field is
// unknown for the merged instrument and must not be announced.
inline void agreeString(std::string& dst, bool& first, const char* value) {
    if (first) { dst = value; first = false; return; }
    if (dst != value) dst.clear();
}

} // namespace detail

inline GmbSnapshot buildSnapshot(const GmbBuildInput& in) {
    GmbSnapshot snap;
    snap.identity = in.identity;
    if (!snap.identity.deviceName.size()) snap.identity.deviceName = "Slide Whistle";
    snap.identity.model = DEVICE_MODEL;

    // A descriptor that carries no instrument is invalid (GMB's validator
    // rejects an empty `instruments` array), so a legal placeholder is emitted
    // whenever nothing real can be announced.
    auto placeholder = []() {
        GmbInstrumentCaps e;
        e.channel = 0;
        e.configured = false;
        return e;
    };

    if (!in.config) { snap.instruments.push_back(placeholder()); return snap; }
    const RuntimeConfig& c = *in.config;
    snap.identity.deviceName = std::string(c.device.name);
    const int transpose = int(c.midi.transpose);

    // --- collect the enabled physical flutes, bucketed by descriptor channel --
    std::vector<uint8_t> channels;                       // bucket order = first seen
    std::vector<std::vector<detail::Contribution>> buckets;
    uint8_t enabledCount = 0;
    for (uint8_t i = 0; i < c.instrumentCount && i < MAX_INSTRUMENTS; ++i) {
        const InstrumentConfig& ic = c.instruments[i];
        if (!ic.enabled) continue;                       // a disabled flute announces nothing
        ++enabledCount;
        detail::Contribution con;
        con.index = i;
        con.cfg   = &ic;
        con.omni  = (ic.midiChannel == 0);
        con.configured = instrumentConfigured(ic, in.configValid, transpose);
        if (con.configured) con.notes = playableNotes(ic, transpose);
        const uint8_t ch = descriptorChannel(ic.midiChannel);
        size_t b = channels.size();
        for (size_t k = 0; k < channels.size(); ++k) if (channels[k] == ch) { b = k; break; }
        if (b == channels.size()) { channels.push_back(ch); buckets.push_back({}); }
        buckets[b].push_back(con);
    }

    if (!in.configValid || enabledCount == 0) {
        // Factory default, invalid, or nothing enabled: one legal placeholder.
        // Nothing about the rest of the configuration is trustworthy, so no
        // notes, no polyphony and no timing are published.
        snap.instruments.push_back(placeholder());
        return snap;
    }

    // --- merge each bucket into ONE logical GMB instrument -------------------
    for (size_t b = 0; b < buckets.size(); ++b) {
        GmbInstrumentCaps e;
        e.channel = channels[b];
        e.physical.transposeSemitones = transpose;

        bool firstDrive = true, firstSource = true, firstGate = true,
             firstFlow = true, firstLegato = true, firstTravel = true;
        bool anyConfigured = false, firstCaps = true;

        for (const detail::Contribution& con : buckets[b]) {
            const InstrumentConfig& ic = *con.cfg;
            if (con.omni) e.physical.omni = true;
            if (!con.configured || con.notes.empty()) continue;   // adds no capability
            anyConfigured = true;

            GmbVoice v;
            v.id = "flute" + std::to_string((int)con.index);
            v.notes = con.notes;
            e.voices.push_back(v);
            e.notes.unite(con.notes);

            if (e.name.empty()) e.name = std::string(ic.name);

            // --- physical: announce a value only when every voice agrees -----
            detail::agreeString(e.physical.slideDrive,  firstDrive,  slideDriveName(ic.motion.type));
            detail::agreeString(e.physical.airSource,   firstSource, airSourceName(ic.air.source.type));
            detail::agreeString(e.physical.airGate,     firstGate,   airGateName(ic.air.gate.type));
            detail::agreeString(e.physical.flowControl, firstFlow,   flowControlName(ic.air.flow.type));
            detail::agreeString(e.physical.legato,      firstLegato, legatoName(ic.seq.legato));
            if (firstTravel) { e.physical.travelMm = ic.motion.travelMm; e.physical.hasTravelMm = true; firstTravel = false; }
            else if (std::fabs(e.physical.travelMm - ic.motion.travelMm) > 1e-4f) e.physical.hasTravelMm = false;
            // Homing is a per-voice fact; if ANY voice needs it, the host should
            // know the controller needs homing before it can play.
            if (ic.motion.type == SlideDriveType::StepDir) e.physical.requiresHoming = true;

            // --- timing: worst case over the merged voices -------------------
            GmbTiming t;
            const AirPrepare ap = airPrepareMs(ic.air);
            int lo = con.notes.min(), hi = con.notes.max();
            float spanMm = 0.0f;
            if (lo >= 0 && hi >= lo) {
                NoteResolution a = resolveNoteStrict(ic.map, uint8_t(lo));
                NoteResolution z = resolveNoteStrict(ic.map, uint8_t(hi));
                if (a.resolvable && z.resolvable) spanMm = std::fabs(z.positionMm - a.positionMm);
            }
            const uint32_t moveMs = slideTravelMs(spanMm, ic.motion.maxSpeedMmS, ic.motion.accelMmS2);
            if (ap.known) {
                // The sequencer starts air preparation and the slide move in the
                // SAME call (NoteSequencer::chooseActiveAndTrigger), so the two
                // overlap: the bound is the MAX of the two, never their sum.
                t.hasPrepare    = true;
                t.prepareBaseMs = uint16_t(ap.ms > 65535u ? 65535u : ap.ms);
                uint32_t worst  = ap.ms > moveMs ? ap.ms : moveMs;
                t.prepareMaxMs  = uint16_t(worst > 65535u ? 65535u : worst);
                t.prepareSilent = prepareIsSilent(ic.seq.legato);
            }
            // NOTE: SequencerConfig::prepareTimeoutMs is NEVER read here. It is
            // the give-up timeout for a note that never became ready — a failure
            // bound, not an acoustic latency.
            if (ic.seq.minNoteMs > 0) { t.hasMinNote = true; t.minNoteMs = uint16_t(ic.seq.minNoteMs); }

            // --- expression --------------------------------------------------
            GmbExpression ex;
            ex.cc = expressionCcs(ic);
            const bool slideActive = ic.motion.type != SlideDriveType::Disabled;
            // Pitch bend and channel aftertouch both work by MOVING the slide.
            ex.pitchBend = slideActive && in.pitchBendRangeSemitones > 0.0f;
            if (ex.pitchBend) {
                float r = in.pitchBendRangeSemitones;
                if (r > 127.0f) r = 127.0f;
                ex.pitchBendRangeSemitones = uint8_t(r + 0.5f);
            }
            // Instrument::aftertouch() only acts when vibrato routing is enabled.
            ex.channelAftertouch = slideActive && ic.cc.vibratoEnabled;
            ex.polyAftertouch = false;
            ex.velocity = velocityEffective(ic, con.notes);

            if (firstCaps) { e.timing = t; e.expression = ex; firstCaps = false; continue; }

            // --- conservative merge -----------------------------------------
            // timing: a bound is only publishable when EVERY voice has one.
            if (!t.hasPrepare) e.timing.hasPrepare = false;
            else if (e.timing.hasPrepare) {
                if (t.prepareBaseMs > e.timing.prepareBaseMs) e.timing.prepareBaseMs = t.prepareBaseMs;
                if (t.prepareMaxMs  > e.timing.prepareMaxMs)  e.timing.prepareMaxMs  = t.prepareMaxMs;
                e.timing.prepareSilent = e.timing.prepareSilent && t.prepareSilent;
            }
            if (t.hasMinNote) {
                if (!e.timing.hasMinNote) { e.timing.hasMinNote = true; e.timing.minNoteMs = t.minNoteMs; }
                else if (t.minNoteMs > e.timing.minNoteMs) e.timing.minNoteMs = t.minNoteMs;
            }
            // expression: promise only what EVERY merged voice implements.
            std::vector<uint8_t> common;
            for (uint8_t cc : e.expression.cc)
                for (uint8_t o : ex.cc) if (cc == o) { common.push_back(cc); break; }
            e.expression.cc = common;
            e.expression.pitchBend = e.expression.pitchBend && ex.pitchBend;
            if (!e.expression.pitchBend) e.expression.pitchBendRangeSemitones = 0;
            else if (ex.pitchBendRangeSemitones < e.expression.pitchBendRangeSemitones)
                e.expression.pitchBendRangeSemitones = ex.pitchBendRangeSemitones;
            e.expression.channelAftertouch = e.expression.channelAftertouch && ex.channelAftertouch;
            e.expression.velocity = e.expression.velocity && ex.velocity;
        }

        e.configured = anyConfigured;
        if (!e.configured) {
            // A channel whose flutes are enabled but not musically configured:
            // announce the channel and nothing else (§5.1). No notes, no
            // polyphony, no timing — GMB falls back to manual entry without
            // overwriting whatever the user set previously.
            e.voices.clear();
            e.notes.clear();
            snap.instruments.push_back(e);
            continue;
        }

        // --- polyphony ------------------------------------------------------
        // The engine BROADCASTS a channel's notes to every instrument that
        // accepts the channel and the note (RealtimeEngine::dispatch), and each
        // flute is intrinsically monophonic. Two voices therefore add real
        // polyphony only when their playable sets are DISJOINT; where they
        // overlap, one note reaches both flutes (unison) and a second note in
        // the overlap simply steals the first. Overlapping voices are grouped
        // and the group counts as ONE.
        const size_t nv = e.voices.size();
        std::vector<int> group(nv, -1);
        int groups = 0;
        for (size_t i = 0; i < nv; ++i) {
            if (group[i] >= 0) continue;
            const int g = groups++;
            group[i] = g;
            // transitive closure over "overlaps"
            bool grew = true;
            while (grew) {
                grew = false;
                for (size_t j = 0; j < nv; ++j) {
                    if (group[j] >= 0) continue;
                    for (size_t k = 0; k < nv; ++k) {
                        if (group[k] != g) continue;
                        if (e.voices[j].notes.intersects(e.voices[k].notes)) {
                            group[j] = g; grew = true; break;
                        }
                    }
                }
            }
        }
        e.polyphonyMax = uint8_t(groups < 1 ? 1 : (groups > 255 ? 255 : groups));
        e.oneNotePerVoice = (nv > 1);

        if (e.name.empty()) e.name = std::string(c.device.name);
        snap.instruments.push_back(e);
    }

    if (snap.instruments.empty()) snap.instruments.push_back(placeholder());
    return snap;
}

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBCAPABILITIES_H
