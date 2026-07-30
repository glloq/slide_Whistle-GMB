/*
 * tests/test_snapshot.cpp — lock-free double-buffered status snapshot (#37).
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/StatusSnapshot.h"
using namespace swc;

TEST(snapshot_publish_then_read) {
    SnapshotPublisher pub;
    // writer fills the back buffer, then publishes
    StatusSnapshot& b = pub.back();
    b.systemState = 5; b.instrumentCount = 1; b.restartRequired = true;
    b.instruments[0].id = 0; b.instruments[0].posMm = 42.0f; b.instruments[0].homed = true;
    pub.publish();
    const StatusSnapshot& r = pub.read();
    CHECK_EQ(r.systemState, 5);
    CHECK(r.restartRequired);
    CHECK_NEAR(r.instruments[0].posMm, 42.0f, 1e-3);
    CHECK_EQ(r.seq, 1u);
}

TEST(snapshot_reader_sees_only_published) {
    SnapshotPublisher pub;
    pub.back().systemState = 1; pub.publish();
    const StatusSnapshot& first = pub.read();
    CHECK_EQ(first.systemState, 1);
    // start writing a NEW back buffer but do NOT publish yet
    pub.back().systemState = 99;
    CHECK_EQ(pub.read().systemState, 1);   // reader still sees the published one
    pub.publish();
    CHECK_EQ(pub.read().systemState, 99);  // now visible
    CHECK_EQ(pub.read().seq, 2u);
}

// Review #3 §3: readCopy() yields a self-consistent snapshot (seqlock). In a
// single-threaded test we can't force a mid-copy race, but we verify the copy
// semantics: it reflects the last publish, reports success, and is a value copy
// independent of subsequent writes to the back buffer.
TEST(snapshot_readcopy_is_consistent_value_copy) {
    SnapshotPublisher pub;
    pub.back().systemState = 7; pub.back().instrumentCount = 1;
    pub.back().instruments[0].posMm = 3.5f;
    pub.publish();
    StatusSnapshot c;
    CHECK(pub.readCopy(c));
    CHECK_EQ(c.systemState, 7);
    CHECK_EQ(c.seq, 1u);
    CHECK_NEAR(c.instruments[0].posMm, 3.5f, 1e-3);
    // Mutating the (unpublished) back buffer must not disturb the taken copy.
    pub.back().systemState = 200;
    pub.back().instruments[0].posMm = 99.0f;
    CHECK_EQ(c.systemState, 7);
    CHECK_NEAR(c.instruments[0].posMm, 3.5f, 1e-3);
    // After a fresh publish, a new copy tracks it.
    pub.publish();
    CHECK(pub.readCopy(c));
    CHECK_EQ(c.systemState, 200);
    CHECK_EQ(c.seq, 2u);
}
