/*
 * core/RealtimeEngine.h — the owner of all hardware objects (RT core).
 *
 * Drains the command queue, routes MIDI events to the matching instruments,
 * dispatches direct (non-MIDI) commands to a single instrument by index, and
 * ticks actuators / air / sequencers on a deterministic period.
 *
 * Direct test/home/jog commands carry an explicit instrument index so a
 * "test this flute" action can never reach another instrument through MIDI
 * routing (correction #13).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_REALTIMEENGINE_H
#define SWC_CORE_REALTIMEENGINE_H

#include "Instrument.h"
#include "CommandQueue.h"

namespace swc {

template <uint16_t QN>
class RealtimeEngine {
public:
    void begin(Instrument** instruments, uint8_t count, CommandQueue<QN>* queue) {
        inst_ = instruments; count_ = count; q_ = queue;
        bendRangeSemis_ = 2.0f;
    }
    void setPitchBendRange(float semis) { bendRangeSemis_ = semis; }

    // Drain up to `budget` commands, then tick every instrument. Called from a
    // periodic RT task (vTaskDelayUntil) — no blocking, no allocation.
    void tick(uint32_t nowMs, uint32_t nowUs, uint16_t budget = 32) {
        Command c;
        while (budget-- && q_ && q_->pop(c)) dispatch(c, nowMs);

        // Server-side test-air timeout: a TestAir never stays open waiting on
        // the browser — it closes itself after its duration (correction #16).
        if (testAirInst_ >= 0 && nowMs >= testAirStopMs_) {
            if (testAirInst_ < count_ && inst_[testAirInst_] && inst_[testAirInst_]->air())
                inst_[testAirInst_]->air()->stopNote();
            testAirInst_ = -1;
        }

        for (uint8_t i = 0; i < count_; ++i) {
            Instrument* in = inst_[i];
            if (!in) continue;
            if (in->actuator()) in->actuator()->update(nowUs);
            if (in->air())      in->air()->update(nowMs);
            in->update(nowMs, nowUs);
        }
    }

    void panicAll(uint32_t nowMs) {
        if (q_) q_->clear();
        testAirInst_ = -1;                 // cancel any running test
        for (uint8_t i = 0; i < count_; ++i) if (inst_[i]) inst_[i]->panic(nowMs);
    }

    bool testAirActive() const { return testAirInst_ >= 0; }

private:
    void dispatch(const Command& c, uint32_t nowMs) {
        switch (c.type) {
            case CommandType::NoteOn:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel) && inst_[i]->acceptsNote(c.a))
                        inst_[i]->noteOn(c.a, c.b, nowMs);
                break;
            case CommandType::NoteOff:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel))
                        inst_[i]->noteOff(c.a, nowMs);
                break;
            case CommandType::ControlChange:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel)) {
                        if (c.a == 0xFF) inst_[i]->aftertouch(c.b, nowMs);
                        else             inst_[i]->controlChange(c.a, c.b, nowMs);
                    }
                break;
            case CommandType::PitchBend: {
                float semis = (float(c.i16) / 8192.0f) * bendRangeSemis_;
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel))
                        inst_[i]->pitchBend(semis, nowMs);
                break;
            }
            case CommandType::Panic:
                panicAll(nowMs);
                break;
            case CommandType::Home:
                if (c.instrument < count_ && inst_[c.instrument] && inst_[c.instrument]->actuator())
                    inst_[c.instrument]->actuator()->requestHoming();
                break;
            case CommandType::Jog:
            case CommandType::TestActuator:
                // single-instrument only — never broadcast (correction #13)
                if (c.instrument < count_ && inst_[c.instrument] && inst_[c.instrument]->actuator())
                    inst_[c.instrument]->actuator()->requestPositionMm(float(c.a));
                break;
            case CommandType::TestAir:
                if (c.instrument < count_ && inst_[c.instrument] && inst_[c.instrument]->air()) {
                    AirNoteRequest r; r.velocity = c.b ? c.b : 100;
                    inst_[c.instrument]->air()->prepareNote(r);
                    inst_[c.instrument]->air()->startNote(r);
                    // schedule an automatic stop (i16 = ms, default 3 s)
                    uint32_t dur = c.i16 > 0 ? (uint32_t)c.i16 : 3000u;
                    testAirInst_ = c.instrument;
                    testAirStopMs_ = nowMs + dur;
                }
                break;
            default: break;   // ApplyDynamicConfig / calibration handled elsewhere
        }
    }

    Instrument**       inst_ = nullptr;
    uint8_t            count_ = 0;
    CommandQueue<QN>*  q_ = nullptr;
    float              bendRangeSemis_ = 2.0f;
    int                testAirInst_ = -1;      // instrument index of a running TestAir, or -1
    uint32_t           testAirStopMs_ = 0;
};

} // namespace swc

#endif // SWC_CORE_REALTIMEENGINE_H
