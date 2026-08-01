/*
 * core/ConfigHandoff.h — atomic, generation-tracked config handoff between the
 * NETWORK core (writer of the DESIRED config) and the RT core (reader/applier of
 * the APPLIED config).
 *
 * The old path wrote the whole RuntimeConfig struct in place on the network core
 * (`*live_ = cand`) while the RT core read the same struct inside
 * ApplyDynamicConfig — a ~10 KB non-atomic copy racing a concurrent reader on the
 * other core, i.e. a torn read of pins/limits/calibration mid-apply (review #9
 * §4.2/§4.3). This class serialises that handoff:
 *
 *   - a single-writer seqlock brackets the struct write with an odd→even sequence
 *     so the RT core copies either the whole old or the whole new config, never a
 *     mix;
 *   - a monotonic GENERATION lets the RT core tell whether a config has actually
 *     been applied, and lets telemetry expose desired-vs-applied (the honest
 *     "your save has landed" signal, and the basis for §4.4).
 *
 * One writer (network core), one reader (RT core). Only ONE extra RuntimeConfig
 * lives here; the reader copies into its own buffer. Portable / unit-tested.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_CONFIGHANDOFF_H
#define SWC_CORE_CONFIGHANDOFF_H

#include "RuntimeConfig.h"
#include <atomic>

namespace swc {

class ConfigHandoff {
public:
    // Writer = network core. Publishes a new DESIRED config atomically w.r.t. the
    // reader and returns its generation (1, 2, 3, …). The odd intermediate seq
    // marks "write in progress" so a concurrent load() retries instead of copying
    // a half-written struct.
    uint32_t publish(const RuntimeConfig& c) {
        uint32_t s = seq_.load(std::memory_order_relaxed) + 1;   // → odd: writing
        seq_.store(s, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        buf_ = c;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 1, std::memory_order_release);            // → even: published
        return (s + 1) >> 1;
    }

    // Last generation the writer published (0 = never). Cheap atomic read; usable
    // from either core.
    uint32_t desiredGen() const {
        uint32_t s = seq_.load(std::memory_order_acquire);
        return s >> 1;   // odd (writing) rounds down to the last stable generation
    }

    // Reader = RT core. Copies the currently-published config into `out` and
    // returns its generation, or 0 if nothing has been published yet or the copy
    // could not be verified consistent within the retry budget (a config apply is
    // rare and uncontended, so the retry effectively never trips).
    uint32_t load(RuntimeConfig& out) const {
        for (int attempt = 0; attempt < 16; ++attempt) {
            uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0) return 0;          // never published
            if (s1 & 1u) continue;          // writer mid-publish → retry
            out = buf_;
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) return s1 >> 1;   // no republish during the copy
        }
        return 0;
    }

private:
    RuntimeConfig         buf_{};
    std::atomic<uint32_t> seq_{0};
};

} // namespace swc

#endif // SWC_CORE_CONFIGHANDOFF_H
