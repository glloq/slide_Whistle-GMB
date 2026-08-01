/*
 * tests/test_confighandoff.cpp — the cross-core config handoff (review #9
 * §4.2/§4.3). Verifies the value fidelity + generation semantics the RT engine
 * relies on: a load before any publish is skipped, a publish round-trips the
 * whole struct, and the generation is monotonic so the RT core can tell a saved
 * config has actually landed. (True cross-core torn-read avoidance is the
 * seqlock's job and cannot be exercised single-threaded; here we pin down the
 * contract the engine depends on.)
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/ConfigHandoff.h"

using namespace swc;

// A load() from a never-published handoff must return 0 so the engine leaves its
// objects — and its applied generation — untouched.
TEST(confighandoff_load_before_publish_is_zero) {
    ConfigHandoff h;
    RuntimeConfig out;
    CHECK_EQ((int)h.desiredGen(), 0);
    CHECK_EQ((int)h.load(out), 0);
}

// A publish round-trips the whole config and load() returns generation 1.
TEST(confighandoff_publish_load_roundtrip) {
    ConfigHandoff h;
    RuntimeConfig in = defaultConfig();
    in.midi.transpose = 7;
    in.instrumentCount = 2;
    in.instruments[0].noteMin = 55;
    in.instruments[1].noteMax = 90;

    uint32_t g = h.publish(in);
    CHECK_EQ((int)g, 1);
    CHECK_EQ((int)h.desiredGen(), 1);

    RuntimeConfig out;
    uint32_t gl = h.load(out);
    CHECK_EQ((int)gl, 1);
    CHECK_EQ((int)out.midi.transpose, 7);
    CHECK_EQ((int)out.instrumentCount, 2);
    CHECK_EQ((int)out.instruments[0].noteMin, 55);
    CHECK_EQ((int)out.instruments[1].noteMax, 90);
}

// Generations are monotonic and load() always yields the LATEST published config,
// so a second save supersedes the first (last-write-wins, one apply).
TEST(confighandoff_generation_monotonic_latest_wins) {
    ConfigHandoff h;
    RuntimeConfig a = defaultConfig(); a.midi.transpose = 3;
    RuntimeConfig b = defaultConfig(); b.midi.transpose = -5;

    CHECK_EQ((int)h.publish(a), 1);
    CHECK_EQ((int)h.publish(b), 2);
    CHECK_EQ((int)h.desiredGen(), 2);

    RuntimeConfig out;
    uint32_t g = h.load(out);
    CHECK_EQ((int)g, 2);
    CHECK_EQ((int)out.midi.transpose, -5);   // the newer publish won
}

// Re-loading without a new publish returns the same generation (idempotent) — the
// engine uses this to avoid re-applying an unchanged config.
TEST(confighandoff_reload_same_generation) {
    ConfigHandoff h;
    RuntimeConfig in = defaultConfig(); in.midi.transpose = 9;
    h.publish(in);
    RuntimeConfig o1, o2;
    CHECK_EQ((int)h.load(o1), 1);
    CHECK_EQ((int)h.load(o2), 1);
    CHECK_EQ((int)o2.midi.transpose, 9);
}
