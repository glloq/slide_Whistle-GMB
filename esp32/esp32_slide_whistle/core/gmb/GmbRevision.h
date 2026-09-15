/*
 * core/gmb/GmbRevision.h — the persistent capability revision counter.
 *
 * `revision` is the ETag General-Midi-Boop uses to decide whether it has to
 * re-download the descriptor (§2/§7 step 4). The contract:
 *
 *     config change → validated → committed → ACTIVE
 *        → revision++ → descriptor rebuilt → block 0x11 emitted
 *
 * and, just as importantly: a plain reboot must NOT move it, and a save that
 * changes nothing GMB is told about must write no flash at all.
 *
 * The decision is driven by a SIGNATURE of the advertised capabilities, not by
 * "something was saved". The overall signature is the hash of the canonical
 * descriptor (rendered at revision 0), which makes the invariant airtight: if
 * the published bytes change, the revision moves; if they do not, it does not.
 * Three structural sub-signatures then say WHAT moved, so block 0x11 can carry
 * meaningful change flags without a second source of truth.
 *
 * Pure: persistence is the caller's job (NVS on the ESP32), so every decision
 * here is unit-tested natively.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBREVISION_H
#define SWC_CORE_GMB_GMBREVISION_H

#include "GmbDescriptor.h"

namespace swc {
namespace gmb {

struct CapabilitySignature {
    uint32_t identity    = 0;   // device name/model/firmware + per-instrument musical identity
    uint32_t instruments = 0;   // playable notes, voices, polyphony, expression, physical
    uint32_t timing      = 0;   // the two-phase timing model
    uint32_t all         = 0;   // hash of the canonical descriptor — the persisted value
};

namespace detail {

// Field-by-field FNV-1a accumulator: no intermediate buffer, stable layout.
struct Hasher {
    uint32_t h = 2166136261u;
    void byte(uint8_t v) { h ^= uint32_t(v); h *= 16777619u; }
    void u16(uint16_t v) { byte(uint8_t(v & 0xFF)); byte(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v & 0xFFFF)); u16(uint16_t(v >> 16)); }
    void boolean(bool v) { byte(v ? 1 : 0); }
    void f32(float v) {
        // Quantised so a bit-identical float is not required, but a change of
        // 0.001 mm still registers.
        long q = (long)std::floor((double)v * 1000.0 + 0.5);
        u32((uint32_t)q);
    }
    void text(const std::string& s) {
        for (char c : s) byte((uint8_t)c);
        byte(0);   // terminator: "ab"+"c" must differ from "a"+"bc"
    }
    void notes(const GmbNoteSet& n) {
        for (int i = 0; i < 128; ++i) if (n.has(uint8_t(i))) byte(uint8_t(i));
        byte(0xFF);
    }
};

} // namespace detail

// Signature of the capability-relevant parts of a snapshot. The instance id and
// the revision itself are excluded on purpose: they are not capabilities, and
// folding the revision in would make the signature move every time it is bumped.
inline CapabilitySignature computeSignature(const GmbSnapshot& s) {
    detail::Hasher identity, instruments, timing;

    identity.text(s.identity.deviceName);
    identity.text(s.identity.model);
    identity.byte(s.identity.firmware[0]);
    identity.byte(s.identity.firmware[1]);
    identity.byte(s.identity.firmware[2]);
    identity.byte(uint8_t(s.instruments.size()));

    for (const GmbInstrumentCaps& e : s.instruments) {
        identity.byte(e.channel);
        identity.boolean(e.configured);
        identity.text(e.name);
        identity.byte(e.gmProgram);
        identity.text(e.type);
        identity.text(e.subtype);

        instruments.byte(e.channel);
        instruments.boolean(e.configured);
        instruments.notes(e.notes);
        instruments.byte(uint8_t(e.voices.size()));
        for (const GmbVoice& v : e.voices) { instruments.text(v.id); instruments.notes(v.notes); }
        instruments.byte(e.polyphonyMax);
        instruments.boolean(e.oneNotePerVoice);
        instruments.byte(uint8_t(e.expression.cc.size()));
        for (uint8_t cc : e.expression.cc) instruments.byte(cc);
        instruments.boolean(e.expression.pitchBend);
        instruments.byte(e.expression.pitchBendRangeSemitones);
        instruments.boolean(e.expression.channelAftertouch);
        instruments.boolean(e.expression.polyAftertouch);
        instruments.boolean(e.expression.velocity);
        instruments.text(e.physical.family);
        instruments.text(e.physical.mechanism);
        instruments.boolean(e.physical.continuousPitch);
        instruments.text(e.physical.slideDrive);
        instruments.boolean(e.physical.hasTravelMm);
        instruments.f32(e.physical.travelMm);
        instruments.boolean(e.physical.requiresHoming);
        instruments.text(e.physical.airSource);
        instruments.text(e.physical.airGate);
        instruments.text(e.physical.flowControl);
        instruments.text(e.physical.legato);
        instruments.u32(uint32_t(int32_t(e.physical.transposeSemitones)));
        instruments.boolean(e.physical.omni);

        timing.byte(e.channel);
        timing.boolean(e.timing.hasPrepare);
        timing.u16(e.timing.prepareBaseMs);
        timing.u16(e.timing.prepareMaxMs);
        timing.boolean(e.timing.prepareSilent);
        timing.boolean(e.timing.hasMinNote);
        timing.u16(e.timing.minNoteMs);
    }

    CapabilitySignature sig;
    sig.identity    = identity.h;
    sig.instruments = instruments.h;
    sig.timing      = timing.h;
    // The authoritative value: the hash of the bytes we would actually publish.
    sig.all = capabilityHash(s);
    return sig;
}

// What the caller must persist alongside the counter.
struct GmbRevisionRecord {
    uint32_t revision  = 0;   // 0 = nothing persisted yet
    uint32_t signature = 0;
};

class RevisionTracker {
public:
    // Seed from persisted state at boot. Returns true when the caller must write
    // the new state back — i.e. on a first boot, or when the configuration
    // changed while the firmware was not running (a config file replaced
    // offline, a firmware upgrade that changes what is announced). A plain
    // reboot with an unchanged configuration returns false and writes nothing.
    bool begin(const GmbRevisionRecord& stored, const CapabilitySignature& current) {
        signature_ = current;
        seeded_ = true;
        changeFlags_ = 0;
        if (stored.revision == 0) {
            revision_ = 1;              // first valid descriptor → revision 1
            return true;
        }
        revision_ = stored.revision;
        if (stored.signature == current.all) return false;   // same config: no write, no bump
        revision_++;
        changeFlags_ = CHANGE_IDENTITY | CHANGE_INSTRUMENTS | CHANGE_TIMING;
        return true;
    }

    // Report that a new configuration has been validated, committed and made
    // ACTIVE. Returns true only when the advertised capabilities really moved —
    // the single case in which the caller persists the counter, rebuilds the
    // descriptor and emits block 0x11.
    bool onConfigurationActivated(const CapabilitySignature& next, bool restartRequired) {
        if (!seeded_) return begin(GmbRevisionRecord{}, next);
        if (next.all == signature_.all) {
            // A save that changes nothing GMB is told about (Wi-Fi settings, a
            // re-save of identical values, a cosmetic field that is not part of
            // the descriptor): no increment, no flash write, no notification.
            changeFlags_ = 0;
            return false;
        }
        uint8_t flags = 0;
        if (next.identity    != signature_.identity)    flags |= CHANGE_IDENTITY;
        if (next.instruments != signature_.instruments) flags |= CHANGE_INSTRUMENTS;
        if (next.timing      != signature_.timing)      flags |= CHANGE_TIMING;
        // The canonical bytes moved, so SOMETHING changed even if no structural
        // sub-signature caught it. Never emit a notification with no flag set.
        if (flags == 0) flags = CHANGE_INSTRUMENTS;
        if (restartRequired) flags |= CHANGE_RESTART_REQUIRED;

        signature_ = next;
        revision_++;
        changeFlags_ = flags;
        return true;
    }

    uint32_t revision() const { return revision_; }
    uint32_t signature() const { return signature_.all; }
    uint8_t  changeFlags() const { return changeFlags_; }
    GmbRevisionRecord record() const { return GmbRevisionRecord{revision_, signature_.all}; }

private:
    uint32_t revision_ = 0;
    CapabilitySignature signature_;
    uint8_t  changeFlags_ = 0;
    bool     seeded_ = false;
};

// Persistence boundary. The ESP32 implementation stores the record in NVS
// (Preferences) — never in config.json, so a configuration save never rewrites
// the counter and a counter bump never rewrites the configuration.
class IGmbRevisionStore {
public:
    virtual bool load(GmbRevisionRecord& out) = 0;
    virtual bool save(const GmbRevisionRecord& rec) = 0;
    virtual ~IGmbRevisionStore() = default;
};

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBREVISION_H
