/*
 * core/NoteSequencer.h — central note/timeline coordinator.
 *
 * Every MIDI source (DIN, BLE, rtpMIDI, web keyboard, demo, file) funnels
 * through the exact same path: MidiRouter → CommandQueue → NoteSequencer →
 * ISlideActuator + IAirSystem. The sequencer owns all timing.
 *
 * Fixes the mission's correctness list:
 *   #1  NoteOff during positioning cancels the pending air open.
 *   #2  Legato policy is actually consulted.
 *  #12  Full monophonic note stack (not a single "last note").
 *  #15  panic() clears the stack, sustain, active note and closes the air.
 *   min-note-duration and millis() rollover handled with elapsed_u32.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_NOTESEQUENCER_H
#define SWC_CORE_NOTESEQUENCER_H

#include "ISlideActuator.h"
#include "IAirSystem.h"
#include "AirSystem.h"     // AirConfig (for per-note air window lookups via NoteMap)
#include "NoteMap.h"
#include <cmath>

namespace swc {

struct SequencerConfig {
    MonoPolicy   mono   = MonoPolicy::LastNote;
    LegatoPolicy legato = LegatoPolicy::AlwaysClose;
    float    legatoMaxDistanceMm = 8.0f;    // for HoldWithinDistance
    uint32_t legatoMaxTimeMs     = 120;     // for HoldWithinTime
    float    legatoMaxMoveTimeMs = 60.0f;   // for HoldWithinMoveTime
    uint32_t minNoteMs           = 0;
    // Max time to wait for the actuator AND the air source to be ready before
    // giving up on a note (so a stuck spin-up / unreached position can't hang).
    uint32_t prepareTimeoutMs    = 4000;
    // Absolute upper bound on how long ONE note may sound (ms, 0 = disabled).
    // A lost NoteOff, a stuck NoteOn or an unplugged MIDI cable would otherwise
    // hold the air open indefinitely — this watchdog force-releases everything
    // after the note has sounded this long without any change, independent of
    // incoming MIDI traffic (review #9 §4.5). Populated from InstrumentConfig
    // watchdogMs by the Instrument.
    uint32_t maxNoteMs           = 0;
    // vibrato
    VibratoUnit vibratoUnit = VibratoUnit::Cents;
    float    vibratoRateHz  = 5.0f;
};

enum class SeqPhase : uint8_t { Idle, Positioning, Playing, Releasing };

class NoteSequencer {
public:
    void begin(ISlideActuator* act, IAirSystem* air, NoteMap* map, const SequencerConfig& cfg) {
        act_ = act; air_ = air; map_ = map; cfg_ = cfg;
        stackN_ = 0; active_ = -1; phase_ = SeqPhase::Idle;
        sustainHeld_ = false; pendingRelease_ = false;
        pitchBend_ = 0.0f; vibratoDepth_ = 0.0f; noteOnMs_ = 0;
    }
    void setConfig(const SequencerConfig& cfg) { cfg_ = cfg; }   // dynamic (no restart)

    // ---- MIDI events (already routed to this instrument) ------------------
    void noteOn(uint8_t note, uint8_t vel, uint32_t nowMs) {
        if (vel == 0) { noteOff(note, nowMs); return; }          // running-status NoteOn/0
        // A deferred release (sustain or the min-note guard) belongs to the note
        // that was active when it was requested. Pressing a fresh key supersedes
        // that note, so the pending release is moot — clear it, otherwise update()
        // would later fire it against noteOnMs_ and tear down THIS new note, and a
        // pedal-up would release a note that is actually still held (review #6 §17).
        pendingRelease_ = false;
        watchdogTripped_ = false;   // a fresh key clears a prior stuck-note trip
        pushOrUpdate(note, vel);
        chooseActiveAndTrigger(nowMs, /*fromRelease=*/false);
    }

    void noteOff(uint8_t note, uint32_t nowMs) {
        int idx = find(note);
        if (idx < 0) return;                                     // stale/unknown → ignore (#stale)
        bool wasActive = (stack_[idx].note == activeNote());
        removeAt(idx);
        if (!wasActive) return;                                  // a background held note released
        if (sustainHeld_) { pendingRelease_ = true; return; }    // sustain defers the release
        // Active note released.
        if (phase_ == SeqPhase::Positioning) {
            // #1 — released before the air ever opened: cancel the pending open.
            cancelToStackOrRelease(nowMs);
            return;
        }
        // min-note duration: defer release if the note was too short. The wait
        // is measured from noteOnMs_ in update() (rollover-safe), so no absolute
        // release timestamp is stored.
        if (cfg_.minNoteMs && elapsed_u32(nowMs, noteOnMs_) < cfg_.minNoteMs) {
            pendingRelease_ = true;
            return;
        }
        cancelToStackOrRelease(nowMs);
    }

    void pitchBend(float semitones, uint32_t nowMs) {
        pitchBend_ = semitones;
        if (active_ >= 0) applyPosition(nowMs);
    }

    // Vibrato depth already reduced to the sequencer's configured unit's raw
    // amount by the CC layer; we convert to semitones via the NoteMap.
    void setVibrato(float amount, uint32_t nowMs) {
        vibratoDepth_ = amount;
        if (active_ >= 0) applyPosition(nowMs);
    }

    void setSustain(bool held, uint32_t nowMs) {
        bool was = sustainHeld_;
        sustainHeld_ = held;
        if (was && !held && pendingRelease_) {                   // pedal up → release deferred note
            pendingRelease_ = false;
            cancelToStackOrRelease(nowMs);
        }
    }

    void allNotesOff(uint32_t nowMs) {
        stackN_ = 0; active_ = -1; pendingRelease_ = false;
        release(nowMs);
    }

    void panic(uint32_t nowMs) {                                  // #15
        stackN_ = 0; active_ = -1;
        sustainHeld_ = false; pendingRelease_ = false;
        pitchBend_ = 0.0f; vibratoDepth_ = 0.0f;
        if (air_) air_->emergencyStop();
        if (act_) act_->emergencyStop();
        phase_ = SeqPhase::Idle;
        (void)nowMs;
    }

    // ---- real-time tick ---------------------------------------------------
    void update(uint32_t nowMs, uint32_t nowUs) {
        // Note watchdog: a single note that has been sounding longer than
        // maxNoteMs almost certainly means its NoteOff was lost (stuck key,
        // unplugged cable). Force EVERYTHING off so the air cannot stay open
        // forever waiting on a NoteOff that will never come (review #9 §4.5).
        // Measured from noteOnMs_ (rollover-safe), which every legato/glissando
        // note change refreshes — so genuine playing keeps resetting it and only
        // a truly static held note trips.
        if (phase_ == SeqPhase::Playing && cfg_.maxNoteMs &&
            elapsed_u32(nowMs, noteOnMs_) >= cfg_.maxNoteMs) {
            watchdogTripped_ = true;
            allNotesOff(nowMs);
            return;
        }
        // Continuous vibrato LFO ONLY while Playing. Applying it during
        // Positioning re-issued requestPositionMm() every tick, which flipped the
        // actuator back to Moving, so isReadyForAir() never became true and a note
        // pressed while a modulation-wheel vibrato was already active timed out
        // without ever opening the air (review #9 §4.1). The slide still moves to
        // the base note position during Positioning; the LFO modulates around it
        // only once the note is actually sounding.
        if (vibratoDepth_ > 1e-4f && phase_ == SeqPhase::Playing)
            applyPosition(nowMs);

        if (phase_ == SeqPhase::Positioning && act_ && air_) {
            // Open the air only once BOTH the slide is at position AND the air
            // source is actually ready (fan spun up / tank at pressure / sensor
            // valid) — never open early (correction #15).
            if (act_->isReadyForAir() && air_->isReady()) {
                AirNoteRequest r = airRequestFor(activeNote());
                air_->startNote(r);
                phase_ = SeqPhase::Playing;
                noteOnMs_ = nowMs;
            } else if (cfg_.prepareTimeoutMs &&
                       elapsed_u32(nowMs, positionStartMs_) > cfg_.prepareTimeoutMs) {
                // Gave up waiting: abandon THIS note safely, never open air late.
                // Drop it from the stack first so we don't immediately re-select
                // and retry the same unreachable note forever (#3 §5.2).
                int idx = find(activeNote());
                if (idx >= 0) removeAt(idx);
                active_ = -1;
                cancelToStackOrRelease(nowMs);
            }
        }
        // Rollover-safe: measure elapsed time from the note's onset with
        // elapsed_u32 (modular subtraction) rather than an absolute
        // `now >= releaseAt` comparison that breaks across the millis() wrap
        // (review #5 §14).
        if (pendingRelease_ && !sustainHeld_ && cfg_.minNoteMs &&
            elapsed_u32(nowMs, noteOnMs_) >= cfg_.minNoteMs) {
            pendingRelease_ = false;
            cancelToStackOrRelease(nowMs);
        }
        // Complete the release: once the air has actually closed (gate + source
        // idle) with no note pending, settle to Idle instead of lingering in
        // Releasing forever, so telemetry and future decisions are accurate
        // (review #5 §15).
        if (phase_ == SeqPhase::Releasing && active_ < 0 && !pendingRelease_ &&
            (!air_ || air_->state() == AirState::Idle))
            phase_ = SeqPhase::Idle;
        (void)nowUs;
    }

    // ---- introspection (for tests / telemetry snapshot) -------------------
    SeqPhase phase() const { return phase_; }
    int      activeNoteOr(int none = -1) const { return active_ >= 0 ? active_ : none; }
    uint8_t  heldCount() const { return stackN_; }
    // True once the note watchdog has force-released a stuck note; cleared by the
    // next fresh NoteOn (for telemetry / tests, review #9 §4.5).
    bool     watchdogTripped() const { return watchdogTripped_; }

private:
    struct Held { uint8_t note; uint8_t vel; };

    // active_ stores the active NOTE VALUE (-1 = none), not a stack index, so
    // it stays valid when the stack shifts on release.
    uint8_t activeNote() const { return uint8_t(active_); }

    int find(uint8_t note) const {
        for (uint8_t i = 0; i < stackN_; ++i) if (stack_[i].note == note) return i;
        return -1;
    }

    void pushOrUpdate(uint8_t note, uint8_t vel) {
        int idx = find(note);
        if (idx >= 0) { stack_[idx].vel = vel; moveToTop(idx); return; } // duplicate → refresh
        if (stackN_ >= MAX_NOTE_STACK) { removeAt(0); }                  // drop oldest
        stack_[stackN_++] = {note, vel};
    }

    void moveToTop(int idx) {
        Held h = stack_[idx];
        for (int i = idx; i < stackN_ - 1; ++i) stack_[i] = stack_[i + 1];
        stack_[stackN_ - 1] = h;
    }

    void removeAt(int idx) {
        for (int i = idx; i < stackN_ - 1; ++i) stack_[i] = stack_[i + 1];
        if (stackN_ > 0) --stackN_;
    }

    // Select the active NOTE VALUE per the monophonic priority policy.
    int selectActive() const {
        if (stackN_ == 0) return -1;
        switch (cfg_.mono) {
            case MonoPolicy::LastNote: return stack_[stackN_ - 1].note;   // most recent
            case MonoPolicy::HighestNote: {
                int best = 0; for (int i = 1; i < stackN_; ++i) if (stack_[i].note > stack_[best].note) best = i;
                return stack_[best].note;
            }
            case MonoPolicy::LowestNote: {
                int best = 0; for (int i = 1; i < stackN_; ++i) if (stack_[i].note < stack_[best].note) best = i;
                return stack_[best].note;
            }
        }
        return stack_[stackN_ - 1].note;
    }

    void chooseActiveAndTrigger(uint32_t nowMs, bool fromRelease) {
        for (;;) {
            int sel = selectActive();
            if (sel < 0) { active_ = -1; release(nowMs); return; }

            // Refuse a note that has no usable position mapping: proceeding to
            // Positioning would let the actuator report "ready for air" at a
            // stale slide position and open air on the WRONG note (#3 §5.1).
            // Drop the unmapped note and fall back to the next one in the stack.
            float posMm;
            if (!map_ || !map_->positionForNote(float(sel), posMm)) {
                int idx = find(uint8_t(sel));
                if (idx >= 0) removeAt(idx);
                if (active_ == sel) active_ = -1;
                continue;
            }

            bool sameNote = (active_ == sel);
            active_ = sel;
            if (sameNote && phase_ == SeqPhase::Playing && !fromRelease) return;

            // Decide legato: keep the air across the move or cut it.
            bool hold = legatoHold(nowMs);
            if (air_) {
                if (phase_ == SeqPhase::Playing && !hold) air_->stopNote();   // #2 close between notes
                air_->prepareNote(airRequestFor(uint8_t(sel)));
            }
            // If the actuator REFUSES the move (out of soft-limits, not homed,
            // faulted), never open air — close it and abandon this note, even
            // under a legato-hold policy (review #5 §P0.3 / §9).
            if (!applyPosition(nowMs)) {
                if (air_) air_->stopNote();
                int idx = find(uint8_t(sel));
                if (idx >= 0) removeAt(idx);
                active_ = -1;
                continue;
            }
            if (air_ && hold && phase_ == SeqPhase::Playing) {
                // glissando / legato-hold: air stays open, just move — stay Playing
                noteOnMs_ = nowMs;
            } else {
                phase_ = SeqPhase::Positioning;   // wait for actuator + air ready in update()
                positionStartMs_ = nowMs;
            }
            return;
        }
    }

    void cancelToStackOrRelease(uint32_t nowMs) {
        if (stackN_ > 0) {
            // Fall back to the previous held note. chooseActiveAndTrigger owns
            // the air: it keeps it open for a legato-hold, or closes+reopens via
            // Positioning otherwise. We must NOT pre-close here — a legato-hold
            // fallback would then keep phase Playing with the air shut, i.e. a
            // silent note (review #4 §P1).
            chooseActiveAndTrigger(nowMs, /*fromRelease=*/true);
        } else {
            if (air_) air_->stopNote();
            active_ = -1;
            release(nowMs);
        }
    }

    void release(uint32_t nowMs) {
        if (air_) air_->stopNote();
        phase_ = SeqPhase::Releasing;
        // actuator stays where it is; source strategy handles spin-down.
        if (act_ && act_->state() != MotionState::EStopped) { /* hold position */ }
        (void)nowMs;
    }

    float fractionalActiveNote(uint32_t nowMs) const {
        float n = float(activeNote()) + pitchBend_;
        if (vibratoDepth_ > 1e-4f && map_) {
            float semi = map_->vibratoSemitones(n, vibratoDepth_, cfg_.vibratoUnit);
            float phase = 2.0f * 3.14159265f * cfg_.vibratoRateHz * (nowMs * 1e-3f);
            n += semi * std::sin(phase);
        }
        return n;
    }

    // Returns true if a target position was accepted by the actuator. A false
    // means either no mapping or the actuator refused the move (soft-limit /
    // fault / not homed) — callers on the trigger path must then NOT open air.
    bool applyPosition(uint32_t nowMs) {
        if (active_ < 0 || !act_ || !map_) return false;
        float mm;
        if (!map_->positionForNote(fractionalActiveNote(nowMs), mm)) return false;
        return act_->requestPositionMm(mm);
    }

    AirNoteRequest airRequestFor(uint8_t note) const {
        AirNoteRequest r;
        int idx = find(note);
        r.velocity = (idx >= 0) ? stack_[idx].vel : 100;
        if (map_) {
            const NoteEntry& e = map_->entry(note);
            r.airMin = e.airMin; r.airNominal = e.airNominal; r.airMax = e.airMax;
        }
        return r;
    }

    bool legatoHold(uint32_t nowMs) {
        if (phase_ != SeqPhase::Playing) return false;    // nothing to hold from
        switch (cfg_.legato) {
            case LegatoPolicy::AlwaysClose:  return false;
            case LegatoPolicy::Glissando:    return true;
            case LegatoPolicy::HoldWithinTime:
                return elapsed_u32(nowMs, noteOnMs_) <= cfg_.legatoMaxTimeMs;
            case LegatoPolicy::HoldWithinDistance: {
                float from = act_ ? act_->currentPositionMm() : 0.0f;
                float mm; if (!map_ || !map_->positionForNote(fractionalActiveNote(nowMs), mm)) return false;
                return std::fabs(mm - from) <= cfg_.legatoMaxDistanceMm;
            }
            case LegatoPolicy::HoldWithinMoveTime: {
                float from = act_ ? act_->currentPositionMm() : 0.0f;
                float mm; if (!map_ || !map_->positionForNote(fractionalActiveNote(nowMs), mm)) return false;
                float dist = std::fabs(mm - from);
                float est = dist / 120.0f * 1000.0f;      // rough mm→ms at 120 mm/s
                return est <= cfg_.legatoMaxMoveTimeMs;
            }
            case LegatoPolicy::SafetyLargeMove: {
                float from = act_ ? act_->currentPositionMm() : 0.0f;
                float mm; if (!map_ || !map_->positionForNote(fractionalActiveNote(nowMs), mm)) return true;
                return std::fabs(mm - from) <= cfg_.legatoMaxDistanceMm; // large move ⇒ close
            }
        }
        return false;
    }

    ISlideActuator* act_ = nullptr;
    IAirSystem*     air_ = nullptr;
    NoteMap*        map_ = nullptr;
    SequencerConfig cfg_;

    Held    stack_[MAX_NOTE_STACK];
    uint8_t stackN_ = 0;
    int     active_ = -1;
    SeqPhase phase_ = SeqPhase::Idle;

    bool     sustainHeld_ = false, pendingRelease_ = false, watchdogTripped_ = false;
    uint32_t noteOnMs_ = 0, positionStartMs_ = 0;
    float    pitchBend_ = 0.0f, vibratoDepth_ = 0.0f;
};

} // namespace swc

#endif // SWC_CORE_NOTESEQUENCER_H
