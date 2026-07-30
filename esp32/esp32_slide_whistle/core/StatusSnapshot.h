/*
 * core/StatusSnapshot.h — lock-free double-buffered telemetry (review item #37).
 *
 * The real-time core writes instrument state into the BACK buffer and publishes
 * it with a single atomic index swap; the network core reads the FRONT buffer.
 * No mutex, no torn reads of individual fields, and the reader never blocks the
 * RT task. Portable and unit-tested.
 */
#ifndef SWC_CORE_STATUSSNAPSHOT_H
#define SWC_CORE_STATUSSNAPSHOT_H

#include "Types.h"
#include <atomic>

namespace swc {

struct InstrumentStatus {
    uint8_t  id = 0;
    bool     homed = false;
    bool     moving = false;
    float    posMm = 0.0f;
    float    targetMm = 0.0f;
    int16_t  activeNote = -1;
    uint8_t  motionState = 0;   // MotionState
    uint8_t  airState = 0;      // AirState
    uint8_t  fault = 0;         // FaultCode of the actuator (0 = none)
};

struct StatusSnapshot {
    uint32_t seq = 0;           // increments on each publish
    uint8_t  systemState = 0;
    uint8_t  instrumentCount = 0;
    bool     restartRequired = false;
    InstrumentStatus instruments[MAX_INSTRUMENTS];
};

// One writer (RT core), many readers (network core). The writer fills back(),
// then publish() flips the index; readers always see a fully-written buffer.
class SnapshotPublisher {
public:
    StatusSnapshot& back() { return buf_[1 - front_.load(std::memory_order_relaxed)]; }

    void publish() {
        int back = 1 - front_.load(std::memory_order_relaxed);
        buf_[back].seq = ++seq_;
        // release so a reader that observes the new index also sees the writes
        front_.store(back, std::memory_order_release);
    }

    const StatusSnapshot& read() const {
        int f = front_.load(std::memory_order_acquire);
        return buf_[f];
    }

private:
    StatusSnapshot buf_[2];
    std::atomic<int> front_{0};
    uint32_t seq_ = 0;
};

} // namespace swc

#endif // SWC_CORE_STATUSSNAPSHOT_H
