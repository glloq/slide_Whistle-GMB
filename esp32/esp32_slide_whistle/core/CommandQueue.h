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
    Rearm,        // acknowledge fault + re-arm actuator/air (and re-home) after panic
    SafeRestart,  // RT task brings everything to a safe state, then the system reboots
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
    //
    // Safety guarantee (review #3 §2.2): a priority command (NoteOff / Panic)
    // is NEVER dropped just because the queue is full — as long as any
    // non-priority command is queued, the newest one is evicted to make room.
    // Losing a queued NoteOn/CC to guarantee a release or an e-stop is the
    // correct trade. Only a queue that is full of priority commands can reject
    // another priority command (a redundant Panic behind a Panic is harmless).
    bool push(const Command& c) {
        SWC_Q_ENTER;
        bool ok;
        if (count_ < N) {
            insertLocked(c);
            ok = true;
        } else if (isPriority(c.type) && pcount_ < N) {
            // Full, but at least one non-priority command exists at the tail —
            // evict the newest non-priority to admit this safety command.
            tail_ = dec(tail_);
            --count_;
            ++dropped_;            // the evicted non-priority command is lost
            insertLocked(c);
            ok = true;
        } else {
            ++dropped_;
            ok = false;
        }
        SWC_Q_EXIT;
        return ok;
    }

    bool pop(Command& out) {
        SWC_Q_ENTER;
        bool ok = (count_ != 0);
        if (ok) {
            out = buf_[head_];
            if (isPriority(out.type) && pcount_ > 0) --pcount_;
            head_ = inc(head_);
            --count_;
        }
        SWC_Q_EXIT;
        return ok;
    }

    uint16_t size() const {
        SWC_Q_ENTER;
        uint16_t n = count_;
        SWC_Q_EXIT;
        return n;
    }
    bool empty() const { return size() == 0; }
    uint32_t dropped() const {
        SWC_Q_ENTER;
        uint32_t d = dropped_;
        SWC_Q_EXIT;
        return d;
    }
    void clear() {
        SWC_Q_ENTER;
        head_ = tail_ = count_ = pcount_ = 0;
        SWC_Q_EXIT;
    }

private:
    // Caller holds the lock. Priority commands go to the head, others to the
    // tail, keeping the priority block contiguous at the front so that the
    // element just before tail_ is always the newest non-priority command.
    void insertLocked(const Command& c) {
        if (isPriority(c.type)) {
            head_ = dec(head_);
            buf_[head_] = c;
            ++pcount_;
        } else {
            buf_[tail_] = c;
            tail_ = inc(tail_);
        }
        ++count_;
    }

    uint16_t inc(uint16_t i) const { return uint16_t((i + 1) % N); }
    uint16_t dec(uint16_t i) const { return uint16_t((i + N - 1) % N); }
    Command  buf_[N];
    uint16_t head_ = 0, tail_ = 0, count_ = 0, pcount_ = 0;
    uint32_t dropped_ = 0;
#if defined(ARDUINO)
    // mutable so the const observers (size/empty/dropped) can take the lock.
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

} // namespace swc

#endif // SWC_CORE_COMMANDQUEUE_H
