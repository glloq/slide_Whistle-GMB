/*
 * core/NoteMap.h — Dynamic note→position mapping and musical interpolation.
 *
 * Fixes correction #10 (LUT sized to the default MIDI range): the table is a
 * fixed 128-entry array indexed by MIDI note, and only *enabled/calibrated*
 * entries participate in mapping, so changing the MIDI range never resizes or
 * invalidates the table (correction #11).
 *
 * Fixes correction #8 (constant mm-per-semitone pitch bend): a fractional note
 * is mapped by interpolating between the two nearest *calibrated* points, so
 * pitch bend / vibrato stay musically correct despite the non-linear
 * length↔frequency relationship.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_NOTEMAP_H
#define SWC_CORE_NOTEMAP_H

#include "Types.h"

namespace swc {

struct NoteEntry {
    float   positionMm          = 0.0f;
    float   positionToleranceMm = 0.8f;
    uint8_t airMin              = 0;
    uint8_t airNominal          = 0;
    uint8_t airMax              = 0;
    bool    calibrated          = false;   // a real calibrated point
    bool    enabled             = false;   // participates in mapping
};

// Vibrato amplitude can be expressed three ways; all reduce to a fractional
// number of semitones before hitting the same interpolation path as pitch bend.
enum class VibratoUnit : uint8_t { Cents = 0, Semitone, Millimetre };

class NoteMap {
public:
    NoteMap() { clear(); }

    void clear() {
        for (uint16_t i = 0; i < MIDI_NOTE_COUNT; ++i) entries_[i] = NoteEntry{};
        travelMm_ = 100.0f;
    }

    void setTravelMm(float mm) { travelMm_ = mm > 0.0f ? mm : 0.0f; }
    float travelMm() const { return travelMm_; }

    // Define a calibrated point (marks it calibrated + enabled).
    void setPoint(uint8_t note, float positionMm, uint8_t airNominal = 0) {
        if (note >= MIDI_NOTE_COUNT) return;
        NoteEntry& e = entries_[note];
        e.positionMm  = positionMm;
        e.airNominal  = airNominal;
        e.calibrated  = true;
        e.enabled     = true;
    }

    NoteEntry&       entry(uint8_t n)       { return entries_[n < MIDI_NOTE_COUNT ? n : MIDI_NOTE_COUNT - 1]; }
    const NoteEntry& entry(uint8_t n) const { return entries_[n < MIDI_NOTE_COUNT ? n : MIDI_NOTE_COUNT - 1]; }

    // Build a provisional linear table between two MIDI notes (mapping mode 3
    // / 6: generate an initial table). Marks entries enabled but NOT calibrated
    // so the UI can flag them "not hardware-validated".
    void generateLinear(uint8_t noteLo, uint8_t noteHi, float posLo, float posHi) {
        if (noteHi <= noteLo) return;
        for (uint16_t n = noteLo; n <= noteHi && n < MIDI_NOTE_COUNT; ++n) {
            float t = float(n - noteLo) / float(noteHi - noteLo);
            NoteEntry& e = entries_[n];
            e.positionMm = lerp(posLo, posHi, t);
            e.enabled    = true;
            e.calibrated = false;
        }
    }

    int calibratedCount() const {
        int c = 0;
        for (uint16_t i = 0; i < MIDI_NOTE_COUNT; ++i)
            if (entries_[i].calibrated && entries_[i].enabled) ++c;
        return c;
    }

    // Map a fractional MIDI note to a slide position in mm by interpolating
    // between the nearest calibrated+enabled neighbours. Returns false if no
    // usable calibration exists.
    bool positionForNote(float fractionalNote, float& outMm) const {
        int lo = -1, hi = -1;
        // nearest calibrated at or below
        for (int n = int(fractionalNote); n >= 0; --n) {
            if (usable(n)) { lo = n; break; }
        }
        // nearest calibrated strictly above
        for (int n = int(fractionalNote) + 1; n < MIDI_NOTE_COUNT; ++n) {
            if (usable(n)) { hi = n; break; }
        }
        if (lo < 0 && hi < 0) return false;
        if (lo < 0) { outMm = entries_[hi].positionMm; return true; }   // below range: clamp
        if (hi < 0) { outMm = entries_[lo].positionMm; return true; }   // above range: clamp
        float t = (fractionalNote - float(lo)) / float(hi - lo);
        outMm = lerp(entries_[lo].positionMm, entries_[hi].positionMm, t);
        return true;
    }

    // Convert a vibrato amount (in its chosen unit) to fractional semitones at a
    // given note. Millimetre mode uses the local mm/semitone slope so vibrato
    // depth stays musically even across the non-linear slide.
    float vibratoSemitones(float note, float amount, VibratoUnit unit) const {
        switch (unit) {
            case VibratoUnit::Cents:     return amount / 100.0f;
            case VibratoUnit::Semitone:  return amount;
            case VibratoUnit::Millimetre: {
                float p0, p1;
                if (!positionForNote(note, p0) || !positionForNote(note + 1.0f, p1))
                    return 0.0f;
                float mmPerSemi = p1 - p0;
                if (mmPerSemi > -1e-6f && mmPerSemi < 1e-6f) return 0.0f;
                return amount / mmPerSemi;
            }
        }
        return 0.0f;
    }

    // Detect non-monotonic calibration (safety check, correction/Section 12).
    // Returns the first offending MIDI note, or -1 when monotonic.
    int firstNonMonotonic() const {
        int prev = -1;
        for (uint16_t n = 0; n < MIDI_NOTE_COUNT; ++n) {
            if (!usable(n)) continue;
            if (prev >= 0 && entries_[n].positionMm < entries_[prev].positionMm)
                return int(n);
            prev = int(n);
        }
        return -1;
    }

    bool isMonotonic() const { return firstNonMonotonic() < 0; }

private:
    bool usable(int n) const {
        return n >= 0 && n < MIDI_NOTE_COUNT &&
               entries_[n].enabled && entries_[n].calibrated;
    }

    NoteEntry entries_[MIDI_NOTE_COUNT];
    float     travelMm_ = 100.0f;
};

} // namespace swc

#endif // SWC_CORE_NOTEMAP_H
