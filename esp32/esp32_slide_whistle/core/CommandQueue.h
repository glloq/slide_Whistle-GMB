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

// On the ESP32 the queue is written from the network core and read from the
// real-time core, so every push/pop runs inside a cross-core portMUX critical
// section (review item #2). Natively these macros are no-ops.
#if defined(ARDUINO)
  #include <Arduino.h>
  #define SWC_Q_ENTER portENTER_CRITICAL(&mux_)
  #define SWC_Q_EXIT  portEXIT_CRITICAL(&mux_)
#else
  #define SWC_Q_ENTER ((void)0)
  #define SWC_Q_EXIT  ((void)0)
#endif

namespace swc {

enum class CommandType : uint8_t {
    NoteOn, NoteOff, ControlChange, PitchBend, Panic,
    Home, Jog, TestActuator, TestAir,
    ApplyDynamicConfig, StartCalibration, CancelCalibration,
    Rearm,   // acknowledge fault + re-arm actuator/air (and re-home) after panic
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
        SWC_Q_ENTER;
        bool ok = true;
        if (count_ >= N) { ++dropped_; ok = false; }
        else {
            if (isPriority(c.type)) { head_ = dec(head_); buf_[head_] = c; }
            else                    { buf_[tail_] = c; tail_ = inc(tail_); }
            ++count_;
        }
        SWC_Q_EXIT;
        return ok;
    }

    bool pop(Command& out) {
        SWC_Q_ENTER;
        bool ok = (count_ != 0);
        if (ok) { out = buf_[head_]; head_ = inc(head_); --count_; }
        SWC_Q_EXIT;
        return ok;
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
#if defined(ARDUINO)
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

} // namespace swc

#endif // SWC_CORE_COMMANDQUEUE_H
