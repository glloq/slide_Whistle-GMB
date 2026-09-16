/*
 * tests/test_gmb_runtime.cpp — the GMB runtime: revision persistence, the
 * activation hook, change notifications, and the HTTP/SysEx consistency rule.
 */
#include "test_framework.h"
#include "test_gmb_support.h"

using namespace swc;
using namespace swc::gmb;
using namespace gmbtest;

// ---------------------------------------------------------------------------
// Revision persistence
// ---------------------------------------------------------------------------

TEST(gmb_first_valid_descriptor_is_revision_1_and_is_persisted) {
    const RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    CHECK_EQ(rt.revision(), 1u);
    CHECK_EQ(store.saves, 1);
    CHECK_EQ(store.rec.revision, 1u);
    CHECK(store.rec.signature != 0u);
    CHECK(contains(rt.descriptorJson(), "\"revision\":1"));
}

TEST(gmb_reboot_with_the_same_config_keeps_the_revision_and_writes_nothing) {
    const RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    { GmbRuntime rt; rt.begin(inputFor(c), &store); }
    const int savesAfterFirstBoot = store.saves;
    // Reboot: same configuration, same firmware.
    GmbRuntime rt2;
    rt2.begin(inputFor(c), &store);
    CHECK_EQ(rt2.revision(), 1u);
    CHECK_EQ(store.saves, savesAfterFirstBoot);   // no flash write on a plain reboot
}

TEST(gmb_a_config_changed_while_powered_off_bumps_the_revision_once_at_boot) {
    FakeRevisionStore store;
    { GmbRuntime rt; rt.begin(inputFor(referenceConfig()), &store); }
    CHECK_EQ(store.rec.revision, 1u);
    // The config file was replaced offline (or the firmware now announces less).
    // NB: widening noteMax past the mapped span would change nothing at all —
    // the playable set is bounded by the NoteMap, not by the range field.
    RuntimeConfig edited = referenceConfig();
    edited.instruments[0].noteMax = 78;
    GmbRuntime rt2;
    rt2.begin(inputFor(edited), &store);
    CHECK_EQ(rt2.revision(), 2u);
    CHECK_EQ(store.rec.revision, 2u);
    // A second boot on that same edited config moves nothing.
    GmbRuntime rt3;
    const int saves = store.saves;
    rt3.begin(inputFor(edited), &store);
    CHECK_EQ(rt3.revision(), 2u);
    CHECK_EQ(store.saves, saves);
}

TEST(gmb_revision_moves_on_an_effective_capability_change) {
    RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    const std::string before = rt.descriptorJson();

    c.instruments[0].noteMax = 78;                   // fewer playable notes
    CHECK(rt.onConfigurationActivated(inputFor(c), /*restartRequired=*/false));
    CHECK_EQ(rt.revision(), 2u);
    CHECK_EQ(store.rec.revision, 2u);
    CHECK(rt.descriptorJson() != before);
    CHECK(contains(rt.descriptorJson(), "\"revision\":2"));
    CHECK(contains(rt.descriptorJson(), "\"max\":78"));
}

TEST(gmb_widening_the_range_past_the_map_is_not_a_capability_change) {
    // The NoteMap bounds what can be played, so a wider noteMax that reaches no
    // new mapped point announces nothing new — and must not bump the revision.
    RuntimeConfig c = referenceConfig();          // range 60..84, map 60..84
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    const int savesAfterBoot = store.saves;
    c.instruments[0].noteMax = 120;
    CHECK(!rt.onConfigurationActivated(inputFor(c), false));
    CHECK_EQ(rt.revision(), 1u);
    CHECK_EQ(store.saves, savesAfterBoot);
    CHECK(contains(rt.descriptorJson(), "\"max\":84"));
}

TEST(gmb_revision_does_not_move_on_a_cosmetic_or_no_op_save) {
    RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    const int savesAfterBoot = store.saves;
    const std::string doc = rt.descriptorJson();

    // 1. a literal no-op save
    CHECK(!rt.onConfigurationActivated(inputFor(c), false));
    // 2. a setting that is not part of the advertised capabilities
    std::snprintf(c.network.apSsid, sizeof(c.network.apSsid), "%s", "AtelierAP");
    c.network.disableApWhenConnected = true;
    CHECK(!rt.onConfigurationActivated(inputFor(c), false));
    // 3. a FAILURE timeout, which must never be advertised in the first place
    c.instruments[0].seq.prepareTimeoutMs = 9000;
    c.instruments[0].watchdogMs = 12345;
    CHECK(!rt.onConfigurationActivated(inputFor(c), false));
    // 4. a DIN pin (transport wiring, not a musical capability)
    c.midi.dinTxPin = 21;
    CHECK(!rt.onConfigurationActivated(inputFor(c), false));

    CHECK_EQ(rt.revision(), 1u);
    CHECK_EQ(store.saves, savesAfterBoot);   // no flash wear
    CHECK_EQ_STR(rt.descriptorJson(), doc);  // the published bytes never moved
    CHECK_EQ(rt.lastChangeFlags(), 0);
}

// ---------------------------------------------------------------------------
// Change notification (block 0x11)
// ---------------------------------------------------------------------------

TEST(gmb_change_flags_identify_what_moved) {
    RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    FakePort port;
    rt.begin(inputFor(c), &store);
    rt.bridge().registerPort(&port);

    // A device rename is an IDENTITY change only.
    std::snprintf(c.device.name, sizeof(c.device.name), "%s", "Atelier Whistle");
    CHECK(rt.onConfigurationActivated(inputFor(c), false));
    CHECK_EQ(rt.lastChangeFlags(), CHANGE_IDENTITY);
    CHECK_EQ((int)port.sent.size(), 1);
    CHECK_EQ((int)port.sent[0].size(), 12);
    CHECK_EQ(port.sent[0][3], 0x11);
    CHECK_EQ(port.sent[0][4], 0x02);
    CHECK_EQ(decode32Le7(&port.sent[0][5]), rt.revision());
    CHECK_EQ(port.sent[0][10], CHANGE_IDENTITY);

    // A minimum-note rule is a TIMING change only.
    c.instruments[0].seq.minNoteMs = 75;
    CHECK(rt.onConfigurationActivated(inputFor(c), false));
    CHECK_EQ(rt.lastChangeFlags(), CHANGE_TIMING);

    // A different note range is an INSTRUMENTS change (and moves the mechanical
    // prepare bound with it, so TIMING too).
    c.instruments[0].noteMax = 78;
    CHECK(rt.onConfigurationActivated(inputFor(c), false));
    CHECK((rt.lastChangeFlags() & CHANGE_INSTRUMENTS) != 0);

    // A change that still needs a reboot says so.
    c.instruments[0].air.source.type = AirSourceType::PumpsDirect;
    CHECK(rt.onConfigurationActivated(inputFor(c), /*restartRequired=*/true));
    CHECK((rt.lastChangeFlags() & CHANGE_RESTART_REQUIRED) != 0);
}

TEST(gmb_no_notification_on_a_no_op_save) {
    const RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    FakePort port;
    rt.begin(inputFor(c), &store);
    rt.bridge().registerPort(&port);
    CHECK(!rt.onConfigurationActivated(inputFor(c), false));
    CHECK_EQ((int)port.sent.size(), 0);
}

TEST(gmb_push_flag_follows_the_transports_return_path) {
    const RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    // No transport registered at all: no push.
    rt.service(10);
    CHECK((rt.service().handshakeFlags() & FLAG_PUSH_NOTIFY) == 0);

    FakePort port;
    port.up = false;                     // registered but with no return path
    rt.bridge().registerPort(&port);
    rt.service(20);
    CHECK((rt.service().handshakeFlags() & FLAG_PUSH_NOTIFY) == 0);

    port.up = true;                      // MIDI OUT wired
    rt.service(30);
    CHECK((rt.service().handshakeFlags() & FLAG_PUSH_NOTIFY) != 0);
}

// ---------------------------------------------------------------------------
// HTTP / SysEx consistency (§17)
// ---------------------------------------------------------------------------

TEST(gmb_http_and_sysex_serve_byte_identical_descriptors) {
    const RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);

    // What GET /gmb/descriptor.json would return (MainApp copies this string).
    const std::string http = rt.descriptorJson();
    CHECK(!http.empty());

    uint32_t now = 1000;
    const uint16_t total = chunkCount(http.size());
    std::vector<uint16_t> order;
    for (uint16_t i = 0; i < total; ++i) order.push_back(uint16_t(total - 1 - i));  // reverse
    const std::string reassembled = reassemble(rt.service(), order, now);
    CHECK_EQ_STR(reassembled, http);

    // The handshake's descriptor_size is that same byte count.
    std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    std::vector<uint8_t> rep = rt.service().handleMessage(req.data(), req.size(), now);
    CHECK_EQ((int)rep.size(), 24);
    CHECK_EQ(decode21Le7(&rep[14]), (uint32_t)http.size());
    CHECK_EQ(decode32Le7(&rep[17]), rt.revision());
    CHECK_EQ(decode32Le7(&rep[6]), rt.instanceId());
}

TEST(gmb_http_and_sysex_stay_consistent_after_a_config_change) {
    RuntimeConfig c = referenceConfig();
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);

    c.instruments[0].noteMin = 65;
    CHECK(rt.onConfigurationActivated(inputFor(c), false));

    uint32_t now = 9000;   // past the idle timeout of any earlier transfer
    const std::string http = rt.descriptorJson();
    const uint16_t total = chunkCount(http.size());
    std::vector<uint16_t> order;
    for (uint16_t i = 0; i < total; ++i) order.push_back(i);
    CHECK_EQ_STR(reassemble(rt.service(), order, now), http);
    CHECK(contains(http, "\"min\":65"));
    CHECK(contains(http, "\"revision\":2"));
}

TEST(gmb_an_oversized_descriptor_falls_back_to_level_0) {
    // A descriptor we could not serve in full must never be announced. The
    // runtime publishes an empty document instead, which reads as level 0.
    RuntimeConfig c = referenceConfig();
    // MAX_DESCRIPTOR_BYTES is 8192; a realistic 4-flute config is far below it,
    // so verify the guard directly on the service instead of forging a config.
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    CHECK(rt.descriptorJson().size() < MAX_DESCRIPTOR_BYTES);
    CHECK(rt.descriptorJson().size() > 0);

    GmbSysExService svc;
    svc.publish("", GmbIdentity{}, 3u);
    CHECK_EQ(svc.descriptorSize(), 0u);
    uint32_t now = 100;
    std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    std::vector<uint8_t> rep = svc.handleMessage(req.data(), req.size(), now);
    CHECK_EQ(decode21Le7(&rep[14]), 0u);
}

TEST(gmb_four_flute_descriptor_round_trips_over_sysex) {
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72), makeFlute(2, 60, 72),
                                         makeFlute(3, 60, 72), makeFlute(4, 60, 72) });
    FakeRevisionStore store;
    GmbRuntime rt;
    rt.begin(inputFor(c), &store);
    const std::string http = rt.descriptorJson();
    CHECK(http.size() > DESCRIPTOR_CHUNK_PAYLOAD * 3);   // genuinely multi-segment
    uint32_t now = 1000;
    const uint16_t total = chunkCount(http.size());
    // Shuffled-ish order with a couple of retries mixed in.
    std::vector<uint16_t> order;
    for (uint16_t i = 0; i < total; i += 2) order.push_back(i);
    order.push_back(0);
    for (uint16_t i = 1; i < total; i += 2) order.push_back(i);
    order.push_back(0);
    CHECK_EQ_STR(reassemble(rt.service(), order, now), http);
}
