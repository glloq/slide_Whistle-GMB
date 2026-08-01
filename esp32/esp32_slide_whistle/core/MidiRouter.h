/*
 * core/MidiRouter.h — the single entry point every MIDI source funnels through.
 *
 * DIN, BLE, rtpMIDI, the web keyboard, the demo player and MIDI-file playback
 * all call these methods; the router pushes structured commands onto the shared
 * bounded queue. Nothing here touches hardware — the real-time engine drains the
 * queue (Section 8/9).
 *
 * Transpose is applied by the RealtimeEngine on dequeue, NOT here: it is the
 * single owner so a live transpose change can release the notes it would strand,
 * and so notes routed through this class are never transposed twice once several
 * MIDI transports feed it (review #9 §4.6 / review #5 §12).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_MIDIROUTER_H
#define SWC_CORE_MIDIROUTER_H

#include "CommandQueue.h"

namespace swc {

template <uint16_t N>
class MidiRouter {
public:
    explicit MidiRouter(CommandQueue<N>& q) : q_(q) {}

    bool noteOn(uint8_t ch, uint8_t note, uint8_t vel) {
        if (note > 127) return false;
        if (vel == 0) return noteOff(ch, note);
        // Raw note — the engine applies transpose on dequeue (review #9 §4.6).
        return push(CommandType::NoteOn, ch, note, vel);
    }
    bool noteOff(uint8_t ch, uint8_t note) {
        if (note > 127) return false;
        return push(CommandType::NoteOff, ch, note, 0);
    }
    bool controlChange(uint8_t ch, uint8_t cc, uint8_t value) {
        return push(CommandType::ControlChange, ch, cc, value);
    }
    bool pitchBend(uint8_t ch, int16_t raw14) {   // -8192..8191, 0 = centre
        Command c{CommandType::PitchBend}; c.channel = ch; c.i16 = raw14; c.seq = ++seq_;
        return q_.push(c);
    }
    bool aftertouch(uint8_t ch, uint8_t value) {
        return push(CommandType::ControlChange, ch, 0xFF, value);   // 0xFF cc = channel pressure
    }
    bool panic() { return push(CommandType::Panic, 0, 0, 0); }

private:
    bool push(CommandType t, uint8_t ch, uint8_t a, uint8_t b) {
        Command c{t}; c.channel = ch; c.a = a; c.b = b; c.seq = ++seq_;
        return q_.push(c);
    }
    CommandQueue<N>& q_;
    uint32_t seq_ = 0;
};

} // namespace swc

#endif // SWC_CORE_MIDIROUTER_H
