/*
 * core/Instrument.h — one slide-whistle instrument (SlideWhistleInstrument).
 *
 * Aggregates the injected actuator + air system + calibration and owns a
 * NoteSequencer. It is the single object the real-time engine ticks and to
 * which the MidiRouter delivers already-routed events. Everything here is
 * portable and unit-tested with fakes.
 *
 * CC routing goes strictly through the configured CcMap: if CC1 (or whatever
 * is mapped to vibrato) is disabled, it no longer implicitly drives vibrato
 * (correction #9). No CC bypasses the map.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_INSTRUMENT_H
#define SWC_CORE_INSTRUMENT_H

#include "ISlideActuator.h"
#include "IAirSystem.h"
#include "NoteMap.h"
#include "NoteSequencer.h"
#include "RuntimeConfig.h"   // CcMap

namespace swc {

class Instrument {
public:
    void begin(uint8_t id, ISlideActuator* act, IAirSystem* air, NoteMap* map,
               const InstrumentConfig& cfg) {
        id_ = id; act_ = act; air_ = air; map_ = map;
        channel_ = cfg.midiChannel; noteMin_ = cfg.noteMin; noteMax_ = cfg.noteMax;
        enabled_ = cfg.enabled; cc_ = cfg.cc; transpose_ = 0;
        seq_.begin(act, air, map, withWatchdog(cfg));
    }

    // ---- dynamic (no-restart) reconfiguration -----------------------------
    void applyDynamic(const InstrumentConfig& cfg) {   // corrections #11, #17
        // Range + policy + CC map + calibration table can change live; propagate
        // to the real-time objects so the UI's "applied" claim is truthful.
        noteMin_ = cfg.noteMin; noteMax_ = cfg.noteMax;
        channel_ = cfg.midiChannel; cc_ = cfg.cc;
        seq_.setConfig(withWatchdog(cfg));
        if (map_) *map_ = cfg.map;                    // refresh note→position table
        if (act_) act_->applyDynamic(cfg.motion);     // live speed/accel/limits (#6)
        if (air_) air_->applyDynamic(cfg.air);        // live flow/expression params (#6)
        if (seq_.activeNoteOr(-1) >= 0) {
            int n = seq_.activeNoteOr(-1);
            if (n < noteMin_ || n > noteMax_) allNotesOff(lastNow_);
        }
    }

    void setTranspose(int8_t t) { transpose_ = t; }
    void setMuted(bool m) { muted_ = m; if (m) seq_.allNotesOff(lastNow_); }
    void setEnabled(bool e) { enabled_ = e; if (!e) panic(lastNow_); }

    // ---- routing acceptance (used by MidiRouter) --------------------------
    bool acceptsChannel(uint8_t ch) const { return enabled_ && (channel_ == 0 || channel_ == ch); }
    bool acceptsNote(uint8_t note) const {
        int e = int(note) + transpose_;
        return e >= noteMin_ && e <= noteMax_;
    }
    uint8_t id() const { return id_; }

    // ---- events (already routed to this instrument) -----------------------
    void noteOn(uint8_t note, uint8_t vel, uint32_t nowMs) {
        if (!enabled_ || muted_) return;
        int e = int(note) + transpose_;
        if (e < noteMin_ || e > noteMax_) return;
        seq_.noteOn(uint8_t(e), vel, nowMs);
    }
    void noteOff(uint8_t note, uint32_t nowMs) {
        int e = int(note) + transpose_;
        if (e < 0 || e > 127) return;
        seq_.noteOff(uint8_t(e), nowMs);
    }
    void pitchBend(float semitones, uint32_t nowMs) { seq_.pitchBend(semitones, nowMs); }
    void aftertouch(uint8_t value, uint32_t nowMs) {
        // channel pressure → vibrato depth, but only if vibrato is enabled
        if (cc_.vibratoEnabled) seq_.setVibrato(value, nowMs);
    }

    void controlChange(uint8_t cc, uint8_t value, uint32_t nowMs) {
        if (cc == 120 || cc == 123) { allNotesOff(nowMs); return; }  // all sound/notes off
        AirExpression e;
        bool touched = false;
        if (cc_.breath     && cc == cc_.breath)     { e.breath = value; touched = true; }
        if (cc_.expression && cc == cc_.expression) { e.expression = value; touched = true; }
        if (cc_.volume     && cc == cc_.volume)     { e.volume = value; touched = true; }
        if (cc_.angle      && cc == cc_.angle)      { e.angleCc = value; touched = true; }
        if (touched && air_) air_->updateExpression(e);
        // vibrato: only if enabled AND mapped (correction #9)
        if (cc_.vibratoEnabled && cc_.vibrato && cc == cc_.vibrato)
            seq_.setVibrato(float(value), nowMs);
        if (cc_.sustain && cc == cc_.sustain)
            seq_.setSustain(value >= 64, nowMs);
    }

    void allNotesOff(uint32_t nowMs) { seq_.allNotesOff(nowMs); }
    void panic(uint32_t nowMs) { seq_.panic(nowMs); }

    // ---- real-time tick ---------------------------------------------------
    void update(uint32_t nowMs, uint32_t nowUs) {
        lastNow_ = nowMs;
        seq_.update(nowMs, nowUs);
    }

    // ---- introspection ----------------------------------------------------
    NoteSequencer& sequencer() { return seq_; }
    ISlideActuator* actuator() { return act_; }
    IAirSystem*     air() { return air_; }
    bool enabled() const { return enabled_; }

private:
    // The per-note watchdog lives on the sequencer (it owns note timing), but the
    // duration is an instrument-level field (watchdogMs). Fold it into the
    // SequencerConfig so begin()/applyDynamic() carry it through (review #9 §4.5).
    static SequencerConfig withWatchdog(const InstrumentConfig& cfg) {
        SequencerConfig s = cfg.seq;
        s.maxNoteMs = cfg.watchdogMs;
        return s;
    }

    uint8_t id_ = 0;
    ISlideActuator* act_ = nullptr;
    IAirSystem*     air_ = nullptr;
    NoteMap*        map_ = nullptr;
    NoteSequencer   seq_;
    CcMap    cc_;
    uint8_t  channel_ = 1, noteMin_ = 0, noteMax_ = 127;
    int8_t   transpose_ = 0;
    bool     enabled_ = true, muted_ = false;
    uint32_t lastNow_ = 0;
};

} // namespace swc

#endif // SWC_CORE_INSTRUMENT_H
