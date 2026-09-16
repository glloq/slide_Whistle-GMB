/*
 * core/gmb/GmbDescriptor.h — renders a capability snapshot as the §5 descriptor.
 *
 * The output is ASCII-ONLY JSON: every byte is < 0x80, so the document is
 * already 7-bit safe and goes onto the SysEx wire with no packing (§3). A UTF-8
 * device name (an accented instrument name is the normal case here) is decoded
 * and re-emitted as \uXXXX escapes.
 *
 * There is ONE serializer and ONE published document. The SysEx block 0x10
 * transfer and GET /gmb/descriptor.json both read the very same std::string, so
 * a byte-for-byte divergence between the two is structurally impossible.
 *
 * "Absent = unknown" is implemented literally: a field whose `has*` flag is
 * false is not written at all. Nothing is ever emitted as 0 to mean "unknown".
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBDESCRIPTOR_H
#define SWC_CORE_GMB_GMBDESCRIPTOR_H

#include <cmath>
#include <cstdio>
#include <string>

#include "GmbCapabilities.h"

namespace swc {
namespace gmb {

namespace detail {

inline void appendHex4(std::string& out, unsigned cp) {
    static const char kHex[] = "0123456789abcdef";
    out += "\\u";
    out += kHex[(cp >> 12) & 0xF];
    out += kHex[(cp >> 8) & 0xF];
    out += kHex[(cp >> 4) & 0xF];
    out += kHex[cp & 0xF];
}

// JSON-escape a (possibly UTF-8) string into pure 7-bit ASCII. Quote, backslash
// and control characters use the short escapes; EVERY code point >= 0x80 — and
// 0x7F, which is a control character — becomes \uXXXX. A code point outside the
// BMP is emitted as a surrogate pair, and malformed UTF-8 degrades to U+FFFD
// rather than leaking a byte with bit 7 set into a SysEx payload.
inline std::string escAscii(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"')       { out += "\\\""; ++i; }
        else if (c == '\\') { out += "\\\\"; ++i; }
        else if (c == '\n') { out += "\\n";  ++i; }
        else if (c == '\r') { out += "\\r";  ++i; }
        else if (c == '\t') { out += "\\t";  ++i; }
        else if (c == '\b') { out += "\\b";  ++i; }
        else if (c == '\f') { out += "\\f";  ++i; }
        else if (c < 0x20 || c == 0x7F) { appendHex4(out, c); ++i; }
        else if (c < 0x80)  { out.push_back((char)c); ++i; }
        else {
            unsigned cp = 0xFFFD;
            size_t adv = 1;
            auto cont = [&](size_t k) { return i + k < n && (((unsigned char)in[i + k]) & 0xC0) == 0x80; };
            if ((c & 0xE0) == 0xC0 && cont(1)) {
                cp = ((c & 0x1Fu) << 6) | ((unsigned char)in[i + 1] & 0x3Fu);
                adv = 2;
                if (cp < 0x80) cp = 0xFFFD;                       // overlong
            } else if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
                cp = ((c & 0x0Fu) << 12) | (((unsigned char)in[i + 1] & 0x3Fu) << 6) |
                     ((unsigned char)in[i + 2] & 0x3Fu);
                adv = 3;
                if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
            } else if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
                cp = ((c & 0x07u) << 18) | (((unsigned char)in[i + 1] & 0x3Fu) << 12) |
                     (((unsigned char)in[i + 2] & 0x3Fu) << 6) | ((unsigned char)in[i + 3] & 0x3Fu);
                adv = 4;
                if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
            }
            if (cp > 0xFFFF) {                                     // surrogate pair
                const unsigned v = cp - 0x10000u;
                appendHex4(out, 0xD800u + (v >> 10));
                appendHex4(out, 0xDC00u + (v & 0x3FFu));
            } else {
                appendHex4(out, cp);
            }
            i += adv;
        }
    }
    return out;
}

// Compact decimal for a physical quantity: integral values print without a
// fractional part so `travel_mm` reads 100, not 100.000.
inline std::string num(float v) {
    if (!std::isfinite(v)) return "0";
    if (std::fabs(v - std::floor(v + 0.5f)) < 1e-4f) {
        char b[24];
        std::snprintf(b, sizeof(b), "%lld", (long long)std::floor(v + 0.5f));
        return std::string(b);
    }
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", (double)v);
    std::string s(b);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

inline void key(std::string& j, const char* k) { j += '"'; j += k; j += "\":"; }
inline void keyStr(std::string& j, const char* k, const std::string& v) {
    key(j, k); j += '"'; j += escAscii(v); j += '"';
}
inline void keyInt(std::string& j, const char* k, long v) { key(j, k); j += std::to_string(v); }
inline void keyBool(std::string& j, const char* k, bool v) { key(j, k); j += v ? "true" : "false"; }

inline void writeNotes(std::string& j, const GmbNoteSet& s) {
    if (s.contiguous()) {
        j += "{\"mode\":\"range\",\"min\":";
        j += std::to_string(s.min());
        j += ",\"max\":";
        j += std::to_string(s.max());
        j += '}';
        return;
    }
    j += "{\"mode\":\"discrete\",\"list\":[";
    bool first = true;
    for (uint8_t n : s.list()) { if (!first) j += ','; first = false; j += std::to_string((int)n); }
    j += "]}";
}

} // namespace detail

// Render the descriptor for `snap`, stamping `revision` into it.
//
// The revision is injected rather than read from the snapshot so the SAME
// function can produce the canonical, revision-independent form (revision 0)
// used to decide whether the capabilities actually moved. Anything that changes
// the published bytes therefore changes that signature — a field can never slip
// into the descriptor without also driving the revision.
inline std::string renderDescriptor(const GmbSnapshot& snap, uint32_t revision) {
    using namespace detail;
    std::string j;
    j.reserve(1024);
    j += "{\"gmb_descriptor\":2,\"revision\":";
    j += std::to_string((unsigned long)revision);
    j += ",\"device\":{";
    keyStr(j, "name", snap.identity.deviceName);
    j += ',';
    keyStr(j, "model", snap.identity.model);
    j += "},\"instruments\":[";

    for (size_t i = 0; i < snap.instruments.size(); ++i) {
        const GmbInstrumentCaps& e = snap.instruments[i];
        if (i) j += ',';
        j += '{';
        keyInt(j, "channel", e.channel);
        j += ',';
        keyBool(j, "configured", e.configured);
        // The musical identity is a property of the MODEL, not of the user's
        // configuration, so it is announced even for an unconfigured entry: GMB
        // can then pre-select "pipe / whistle / GM 78" for manual entry.
        j += ','; keyStr(j, "type", e.type);
        j += ','; keyStr(j, "subtype", e.subtype);
        j += ','; keyInt(j, "gm_program", e.gmProgram);

        if (!e.configured) { j += '}'; continue; }   // §5.1: nothing else is trustworthy

        if (!e.name.empty()) { j += ','; keyStr(j, "name", e.name); }

        j += ','; key(j, "notes"); writeNotes(j, e.notes);

        if (!e.voices.empty()) {
            j += ','; key(j, "voices"); j += '[';
            for (size_t v = 0; v < e.voices.size(); ++v) {
                if (v) j += ',';
                j += '{';
                keyStr(j, "id", e.voices[v].id);
                j += ','; key(j, "notes"); writeNotes(j, e.voices[v].notes);
                j += '}';
            }
            j += ']';
        }

        j += ','; key(j, "polyphony"); j += '{';
        keyInt(j, "max", e.polyphonyMax);
        if (e.oneNotePerVoice) j += ",\"constraints\":[{\"type\":\"one_note_per_voice\"}]";
        j += '}';

        const GmbTiming& t = e.timing;
        if (t.hasPrepare || t.hasMinNote) {
            j += ','; key(j, "timing"); j += '{';
            bool firstT = true;
            if (t.hasPrepare) {
                j += "\"prepare\":{";
                keyInt(j, "base_ms", t.prepareBaseMs);
                j += ','; keyInt(j, "max_ms", t.prepareMaxMs);
                j += ','; keyBool(j, "silent", t.prepareSilent);
                j += '}';
                firstT = false;
            }
            if (t.hasMinNote) {
                if (!firstT) j += ',';
                keyInt(j, "min_note_ms", t.minNoteMs);
            }
            j += '}';
            // excite / release / rearticulation are intentionally absent: no
            // acoustic onset measurement exists on this firmware yet.
        }

        const GmbExpression& x = e.expression;
        j += ','; key(j, "expression"); j += '{';
        key(j, "cc"); j += '[';
        for (size_t k = 0; k < x.cc.size(); ++k) { if (k) j += ','; j += std::to_string((int)x.cc[k]); }
        j += ']';
        j += ','; key(j, "pitch_bend"); j += '{';
        keyBool(j, "supported", x.pitchBend);
        if (x.pitchBend) { j += ','; keyInt(j, "range_semitones", x.pitchBendRangeSemitones); }
        j += '}';
        j += ','; keyBool(j, "channel_aftertouch", x.channelAftertouch);
        j += ','; keyBool(j, "poly_aftertouch", x.polyAftertouch);
        j += ','; keyBool(j, "velocity", x.velocity);
        j += '}';

        const GmbPhysical& p = e.physical;
        j += ','; key(j, "physical"); j += '{';
        keyStr(j, "family", p.family);
        j += ','; keyStr(j, "mechanism", p.mechanism);
        j += ','; keyBool(j, "continuous_pitch", p.continuousPitch);
        if (!p.slideDrive.empty())  { j += ','; keyStr(j, "slide_drive", p.slideDrive); }
        if (p.hasTravelMm)          { j += ','; key(j, "travel_mm"); j += num(p.travelMm); }
        j += ','; keyBool(j, "requires_homing", p.requiresHoming);
        if (!p.airSource.empty())   { j += ','; keyStr(j, "air_source", p.airSource); }
        if (!p.airGate.empty())     { j += ','; keyStr(j, "air_gate", p.airGate); }
        if (!p.flowControl.empty()) { j += ','; keyStr(j, "flow_control", p.flowControl); }
        if (!p.legato.empty())      { j += ','; keyStr(j, "legato", p.legato); }
        j += ','; keyInt(j, "transpose_semitones", p.transposeSemitones);
        if (p.omni) { j += ','; keyBool(j, "omni", true); }
        // No GPIO numbers, no Wi-Fi credentials, no tokens: `physical` describes
        // the MECHANISM, never the wiring or the secrets.
        j += '}';

        j += '}';
    }
    j += "]}";
    return j;
}

// The canonical, revision-independent form of a snapshot. Hashing THIS is what
// decides whether the advertised capabilities really moved.
inline uint32_t capabilityHash(const GmbSnapshot& snap) {
    const std::string canonical = renderDescriptor(snap, 0);
    return fnv1a32(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
}

// True when every byte of `s` is 7-bit (the invariant the SysEx transfer needs).
inline bool isAscii(const std::string& s) {
    for (unsigned char c : s) if (c & 0x80) return false;
    return true;
}

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBDESCRIPTOR_H
