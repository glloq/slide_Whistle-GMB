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
    explicit LedcAllocator(uint8_t capacity = 16) : cap_(clampCap(capacity)) {}
    // Returns the lowest free channel index, or -1 when the board's channels are
    // used up. A -1 must be treated as a hard failure by the caller — never
    // silently reused as channel 0 (review #3 §9.1).
    int allocate() {
        for (uint8_t i = 0; i < cap_; ++i)
            if (!(mask_ & chMask(i))) { mask_ |= chMask(i); return int(i); }
        return -1;
    }
    // Return a channel to the pool so a later reconfigure can reuse it rather
    // than leaking it (review #3 §9.2). Out-of-range / double-free is a no-op.
    void release(int ch) {
        if (ch >= 0 && ch < int(cap_)) mask_ &= ~chMask(uint8_t(ch));
    }
    bool isAllocated(uint8_t ch) const { return ch < cap_ && (mask_ & chMask(ch)); }
    void reset() { mask_ = 0; }
    uint8_t used() const { return uint8_t(__builtin_popcount(mask_)); }
    uint8_t capacity() const { return cap_; }
    void setCapacity(uint8_t c) { cap_ = clampCap(c); }

    // Process-wide default allocator (WROOM has 16 channels; call setCapacity(8)
    // on an S3 during bring-up).
    static LedcAllocator& global() { static LedcAllocator a(16); return a; }

private:
    static uint32_t chMask(uint8_t i) { return 1u << i; }
    static uint8_t  clampCap(uint8_t c) { return c > 32 ? 32 : c; }   // mask_ is 32-bit
    uint8_t  cap_;
    uint32_t mask_ = 0;   // bit i set = channel i in use
};

} // namespace swc

#endif // SWC_CORE_LEDCALLOCATOR_H
