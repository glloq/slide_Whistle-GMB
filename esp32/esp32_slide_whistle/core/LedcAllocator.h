/*
 * core/LedcAllocator.h — hands out distinct LEDC channels (review item #3).
 *
 * On Arduino-ESP32 2.x every PWM output must sit on its OWN channel; without an
 * allocator they all defaulted to channel 0 and stomped on each other. This
 * portable allocator (unit-tested) assigns unique channels and reports
 * exhaustion. On 3.x the core auto-manages channels, so the allocator is unused
 * there but harmless.
 */
#ifndef SWC_CORE_LEDCALLOCATOR_H
#define SWC_CORE_LEDCALLOCATOR_H

#include <cstdint>

namespace swc {

class LedcAllocator {
public:
    explicit LedcAllocator(uint8_t capacity = 16) : cap_(capacity) {}
    // Returns a fresh channel index, or -1 when the board's channels are used up.
    int allocate() { return used_ < cap_ ? int(used_++) : -1; }
    void reset() { used_ = 0; }
    uint8_t used() const { return used_; }
    uint8_t capacity() const { return cap_; }
    void setCapacity(uint8_t c) { cap_ = c; }

    // Process-wide default allocator (WROOM has 16 channels; call setCapacity(8)
    // on an S3 during bring-up).
    static LedcAllocator& global() { static LedcAllocator a(16); return a; }

private:
    uint8_t cap_;
    uint8_t used_ = 0;
};

} // namespace swc

#endif // SWC_CORE_LEDCALLOCATOR_H
