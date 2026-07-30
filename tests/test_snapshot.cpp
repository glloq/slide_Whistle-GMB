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
