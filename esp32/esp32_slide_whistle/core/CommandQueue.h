/*
 * core/CommandQueue.h — bounded, priority-aware command queue.
 *
 * Replaces the volatile-global pseudo-queue (correction #7). Web/REST/WS
 * handlers *enqueue* structured commands; only the real-time task dequeues and
 * touches hardware (Section 9). NoteOff and Panic jump the queue so a release
 * is never starved by a backlog of NoteOn/CC (Section 8, correction #27).
 *
 * Lock-free is not required here — on the MCU a short critical section / mutex
 * wraps push/pop. The container itself does no dynamic allocation.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_COMMANDQUEUE_H
#define SWC_CORE_COMMANDQUEUE_H

#include "Types.h"

namespace swc {

enum class CommandType : uint8_t {
    NoteOn, NoteOff, ControlChange, PitchBend, Panic,
    Home, Jog, TestActuator, TestAir,
    ApplyDynamicConfig, StartCalibration, CancelCalibration,
};

struct Command {
    CommandType type = CommandType::Panic;
    uint8_t  instrument = 0;
    uint8_t  channel    = 0;
    uint8_t  a          = 0;   // note / cc number / target id
    uint8_t  b          = 0;   // velocity / cc value
    int16_t  i16        = 0;   // pitch bend raw / jog delta
    uint32_t seq        = 0;   // command id for long-op tracking
};

inline bool isPriority(CommandType t) {
    return t == CommandType::NoteOff || t == CommandType::Panic;
}

template <uint16_t N>
class CommandQueue {
public:
    // Priority commands are pushed to the front; the queue is bounded and
    // reports overflow instead of allocating.
    bool push(const Command& c) {
        if (count_ >= N) { ++dropped_; return false; }
        if (isPriority(c.type)) {
            head_ = dec(head_);
            buf_[head_] = c;
        } else {
            buf_[tail_] = c;
            tail_ = inc(tail_);
        }
        ++count_;
        return true;
    }

    bool pop(Command& out) {
        if (count_ == 0) return false;
        out = buf_[head_];
        head_ = inc(head_);
        --count_;
        return true;
    }

    uint16_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    uint32_t dropped() const { return dropped_; }
    void clear() { head_ = tail_ = count_ = 0; }

private:
    uint16_t inc(uint16_t i) const { return uint16_t((i + 1) % N); }
    uint16_t dec(uint16_t i) const { return uint16_t((i + N - 1) % N); }
    Command  buf_[N];
    uint16_t head_ = 0, tail_ = 0, count_ = 0;
    uint32_t dropped_ = 0;
};

} // namespace swc

#endif // SWC_CORE_COMMANDQUEUE_H
