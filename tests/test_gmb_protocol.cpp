/*
 * tests/test_gmb_protocol.cpp — GMB v2 wire protocol: codecs, handshake frame,
 * block 0x10 transfer semantics, block 0x11 notifications, and the robustness
 * rules that keep untrusted SysEx away from the note path.
 *
 * Every frame layout asserted here was checked against the CURRENT consumer in
 * General-Midi-Boop (src/midi/devices/DeviceManager.js).
 */
#include "test_framework.h"
#include "test_gmb_support.h"

using namespace swc;
using namespace swc::gmb;
using namespace gmbtest;

// ---------------------------------------------------------------------------
// 7-bit codecs
// ---------------------------------------------------------------------------

TEST(gmb_codec_32bit_roundtrip_and_boundaries) {
    const uint32_t values[] = {0u, 1u, 127u, 128u, 0x0FFFFFFFu, 0x80000000u,
                               0xFFFFFFFFu, 0x7FFFFFFFu, 0xDEADBEEFu};
    for (uint32_t v : values) {
        uint8_t enc[5];
        encode32Le7(v, enc);
        for (int i = 0; i < 5; ++i) CHECK((enc[i] & 0x80) == 0);   // 7-bit safe
        CHECK_EQ(decode32Le7(enc), v);
    }
    // Bit 31 MUST survive: the byte-4 nibble is 0x0f, not the 3 bits of the
    // legacy v1 codec, which would halve the instance-id space.
    uint8_t enc[5];
    encode32Le7(0xFFFFFFFFu, enc);
    CHECK_EQ(enc[4], 0x0F);
}

TEST(gmb_codec_21bit_and_14bit_boundaries) {
    for (uint32_t v : {0u, 1u, 127u, 128u, 16383u, 16384u, 0x1FFFFFu}) {
        uint8_t e[3];
        encode21Le7(v, e);
        for (int i = 0; i < 3; ++i) CHECK((e[i] & 0x80) == 0);
        CHECK_EQ(decode21Le7(e), v);
    }
    for (uint16_t v : {uint16_t(0), uint16_t(1), uint16_t(127), uint16_t(128), uint16_t(0x3FFF)}) {
        uint8_t e[2];
        encode14Le7(v, e);
        CHECK((e[0] & 0x80) == 0);
        CHECK((e[1] & 0x80) == 0);
        CHECK_EQ(decode14Le7(e), v);
    }
    // A value above the field width truncates rather than leaking a status byte.
    uint8_t e[3];
    encode21Le7(0x200000u, e);
    CHECK_EQ(decode21Le7(e), 0u);
}

TEST(gmb_instance_id_is_stable_unique_and_nonzero) {
    const uint64_t macA = 0x7C9EBD1234ABull;
    const uint64_t macB = 0x7C9EBD1234ACull;
    // Same hardware id → same instance id, every time (survives a reboot).
    CHECK_EQ(instanceIdFromHardwareId(macA), instanceIdFromHardwareId(macA));
    // Different boards → different identity.
    CHECK(instanceIdFromHardwareId(macA) != instanceIdFromHardwareId(macB));
    // Never the reserved 0, even for an all-zero hardware id.
    CHECK(instanceIdFromHardwareId(0) != 0u);
    // Differences confined to the bits a 7-bit truncation would drop still
    // separate two boards (this is why the MAC is hashed, not truncated).
    CHECK(instanceIdFromHardwareId(0x000000000080ull) != instanceIdFromHardwareId(0x000000000000ull));
}

// ---------------------------------------------------------------------------
// Block 1 — handshake
// ---------------------------------------------------------------------------

TEST(gmb_handshake_is_exactly_24_bytes_with_proto_2) {
    GmbIdentity id;
    id.instanceId = 0xDEADBEEFu;
    id.firmware[0] = 1; id.firmware[1] = 2; id.firmware[2] = 3;
    std::vector<uint8_t> m = GmbSysEx::encodeHandshake(id, 42u, 1234u,
                                                       FLAG_HTTP_DESCRIPTOR | FLAG_PUSH_NOTIFY);
    // The consumer rejects anything that is not exactly 24 bytes.
    CHECK_EQ((int)m.size(), 24);
    CHECK_EQ(m[0], 0xF0); CHECK_EQ(m[1], 0x7D); CHECK_EQ(m[2], 0x00);
    CHECK_EQ(m[3], 0x01); CHECK_EQ(m[4], 0x01);
    CHECK_EQ(m[5], 0x02);                              // proto_ver
    CHECK_EQ(decode32Le7(&m[6]), 0xDEADBEEFu);         // instance_id[5]
    CHECK_EQ(m[11], 1); CHECK_EQ(m[12], 2); CHECK_EQ(m[13], 3);
    CHECK_EQ(decode21Le7(&m[14]), 1234u);              // descriptor_size[3]
    CHECK_EQ(decode32Le7(&m[17]), 42u);                // revision[5]
    CHECK_EQ(m[22], 0x03);                             // flags
    CHECK_EQ(m[23], 0xF7);
    for (size_t i = 1; i < m.size() - 1; ++i) CHECK((m[i] & 0x80) == 0);
}

TEST(gmb_handshake_reports_level_0_when_no_descriptor) {
    GmbSysExService svc;
    svc.publish("", GmbIdentity{}, 7u);
    uint32_t now = 1000;
    std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    std::vector<uint8_t> rep = svc.handleMessage(req.data(), req.size(), now);
    CHECK_EQ((int)rep.size(), 24);
    CHECK_EQ(decode21Le7(&rep[14]), 0u);   // descriptor_size 0 == level 0
}

TEST(gmb_handshake_flags_follow_real_availability) {
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 1u);
    CHECK_EQ(svc.handshakeFlags(), 0);            // nothing announced by default
    svc.setHttpDescriptorAvailable(true);
    CHECK_EQ(svc.handshakeFlags(), FLAG_HTTP_DESCRIPTOR);
    svc.setPushNotificationsAvailable(true);
    CHECK_EQ(svc.handshakeFlags(), FLAG_HTTP_DESCRIPTOR | FLAG_PUSH_NOTIFY);
    svc.setHttpDescriptorAvailable(false);
    CHECK_EQ(svc.handshakeFlags(), FLAG_PUSH_NOTIFY);
}

// ---------------------------------------------------------------------------
// Block 0x11 — change notification
// ---------------------------------------------------------------------------

TEST(gmb_change_notification_is_exactly_12_bytes_with_flags) {
    const uint8_t flags = CHANGE_IDENTITY | CHANGE_TIMING | CHANGE_RESTART_REQUIRED;
    std::vector<uint8_t> m = GmbSysEx::encodeChangeNotification(0x0ABCDEF1u, flags);
    CHECK_EQ((int)m.size(), 12);
    CHECK_EQ(m[0], 0xF0); CHECK_EQ(m[1], 0x7D); CHECK_EQ(m[2], 0x00);
    CHECK_EQ(m[3], 0x11); CHECK_EQ(m[4], 0x02);
    CHECK_EQ(decode32Le7(&m[5]), 0x0ABCDEF1u);
    CHECK_EQ(m[10], flags);
    CHECK_EQ(m[11], 0xF7);
    // Individual flag bits match the spec table.
    CHECK_EQ(CHANGE_IDENTITY, 1);
    CHECK_EQ(CHANGE_INSTRUMENTS, 2);
    CHECK_EQ(CHANGE_TIMING, 4);
    CHECK_EQ(CHANGE_RESTART_REQUIRED, 8);
}

// ---------------------------------------------------------------------------
// Robustness — untrusted input
// ---------------------------------------------------------------------------

TEST(gmb_malformed_sysex_is_ignored_silently) {
    GmbSysExService svc;
    svc.publish(std::string(500, 'x'), GmbIdentity{}, 1u);
    uint32_t now = 0;
    struct Frame { const char* what; std::vector<uint8_t> bytes; };
    const std::vector<Frame> bad = {
        {"empty",                {}},
        {"too short",            {0xF0, 0x7D, 0x00, 0xF7}},
        {"no F0",                {0x7D, 0x7D, 0x00, 0x01, 0x00, 0xF7}},
        {"no F7",                {0xF0, 0x7D, 0x00, 0x01, 0x00, 0x00}},
        {"wrong manufacturer",   {0xF0, 0x41, 0x00, 0x01, 0x00, 0xF7}},
        {"wrong device id",      {0xF0, 0x7D, 0x01, 0x01, 0x00, 0xF7}},
        {"8-bit payload byte",   {0xF0, 0x7D, 0x00, 0x01, 0x80, 0xF7}},
        {"handshake wrong len",  {0xF0, 0x7D, 0x00, 0x01, 0x00, 0x00, 0xF7}},
        {"chunk req wrong len",  {0xF0, 0x7D, 0x00, 0x10, 0x00, 0x00, 0xF7}},
        {"unknown block",        {0xF0, 0x7D, 0x00, 0x42, 0x00, 0xF7}},
        {"response echoed back", {0xF0, 0x7D, 0x00, 0x01, 0x01, 0xF7}},
        {"notification back",    {0xF0, 0x7D, 0x00, 0x11, 0x02, 0xF7}},
        // A frame that is far too long is not a GMB request and must not even be
        // parsed as one.
        {"over-long request",    std::vector<uint8_t>(64, 0x00)},
    };
    uint32_t expectDropped = 0;
    for (const Frame& f : bad) {
        std::vector<uint8_t> copy = f.bytes;
        if (std::string(f.what) == "over-long request") {
            copy.front() = 0xF0; copy[1] = 0x7D; copy[2] = 0x00; copy[3] = 0x01; copy[4] = 0x00;
            copy.back() = 0xF7;
        }
        std::vector<uint8_t> rep = svc.handleMessage(copy.data(), copy.size(), now);
        CHECK(rep.empty());
        ++expectDropped;
        now += 10;
    }
    CHECK_EQ(svc.droppedRequests(), expectDropped);
    CHECK_EQ(svc.handledRequests(), 0u);
    // Not one malformed frame started a transfer.
    CHECK(!svc.transferInFlight());
}

TEST(gmb_out_of_range_chunk_is_silent_and_starts_no_transfer) {
    GmbSysExService svc;
    svc.publish(std::string(250, 'a'), GmbIdentity{}, 1u);   // 2 chunks
    uint32_t now = 100;
    for (uint16_t idx : {uint16_t(2), uint16_t(3), uint16_t(1000), uint16_t(0x3FFF)}) {
        std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(idx);
        std::vector<uint8_t> rep = svc.handleMessage(req.data(), req.size(), now);
        CHECK(rep.empty());
        CHECK(!svc.transferInFlight());   // no pin, no buffer growth
        now += 10;
    }
    // A valid index still works afterwards.
    std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(0);
    CHECK(!svc.handleMessage(req.data(), req.size(), now).empty());
}

TEST(gmb_rate_limit_bounds_a_sysex_flood) {
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 1u);
    std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    const uint32_t now = 5000;
    int answered = 0;
    for (int i = 0; i < 200; ++i)
        if (!svc.handleMessage(req.data(), req.size(), now).empty()) ++answered;
    // The discovery burst gets through; a sustained flood at one instant does not.
    CHECK_EQ(answered, GmbSysExService::MAX_TOKENS);
    // Tokens come back with time.
    CHECK(!svc.handleMessage(req.data(), req.size(), now + 100).empty());
}

TEST(gmb_chunk_payload_never_exceeds_200_bytes_and_is_7bit) {
    const std::string doc(1000, 'Z');
    CHECK_EQ(chunkCount(doc.size()), 5);
    for (uint16_t i = 0; i < 5; ++i) {
        std::vector<uint8_t> m = GmbSysEx::encodeDescriptorChunk(doc, i);
        CHECK(m.size() >= 10);
        CHECK(m.size() <= 10 + DESCRIPTOR_CHUNK_PAYLOAD);
        CHECK_EQ(m[3], 0x10);
        CHECK_EQ(m[4], 0x01);
        CHECK_EQ(decode14Le7(&m[5]), 5);
        CHECK_EQ(decode14Le7(&m[7]), i);
        for (size_t k = 1; k < m.size() - 1; ++k) CHECK((m[k] & 0x80) == 0);
    }
    CHECK(GmbSysEx::encodeDescriptorChunk(doc, 5).empty());
    // chunkCount always advertises at least one segment.
    CHECK_EQ(chunkCount(0), 1);
    CHECK_EQ(chunkCount(200), 1);
    CHECK_EQ(chunkCount(201), 2);
}

// ---------------------------------------------------------------------------
// Block 0x10 — the pinned-snapshot rule
// ---------------------------------------------------------------------------

TEST(gmb_transfer_serves_chunks_in_arbitrary_order) {
    GmbSysExService svc;
    std::string doc;
    for (int i = 0; i < 700; ++i) doc += char('a' + (i % 26));
    svc.publish(doc, GmbIdentity{}, 1u);
    uint32_t now = 1000;
    // 4 chunks requested back-to-front, then the middle ones.
    std::string got = reassemble(svc, {3, 0, 2, 1}, now);
    CHECK(got == doc);
    // Once every distinct segment has gone out, the transfer is over.
    CHECK(!svc.transferInFlight());
}

TEST(gmb_repeated_chunks_do_not_end_the_transfer_early) {
    GmbSysExService svc;
    std::string doc(700, 'q');
    svc.publish(doc, GmbIdentity{}, 1u);
    const uint16_t total = chunkCount(doc.size());
    CHECK_EQ(total, 4);
    uint32_t now = 1000;

    // Ask for the LAST segment first, then retry it repeatedly. Counting raw
    // deliveries (rather than unique indices) would end the transfer here.
    for (int i = 0; i < 6; ++i) {
        std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(uint16_t(total - 1));
        CHECK(!svc.handleMessage(req.data(), req.size(), now).empty());
        now += 5;
        CHECK(svc.transferInFlight());   // still pinned: segments 0..2 are missing
    }
    // Repeated chunk 0 likewise does not finish it.
    for (int i = 0; i < 6; ++i) {
        std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(0);
        CHECK(!svc.handleMessage(req.data(), req.size(), now).empty());
        now += 5;
    }
    CHECK(svc.transferInFlight());
    // Only the genuinely missing segments end it.
    for (uint16_t i = 1; i <= 2; ++i) {
        std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(i);
        CHECK(!svc.handleMessage(req.data(), req.size(), now).empty());
        now += 5;
    }
    CHECK(!svc.transferInFlight());
}

TEST(gmb_transfer_is_frozen_across_a_config_change) {
    GmbSysExService svc;
    const std::string docA(700, 'A');
    const std::string docB(700, 'B');
    svc.publish(docA, GmbIdentity{}, 1u);
    uint32_t now = 1000;

    auto chunk = [&](uint16_t idx) {
        std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(idx);
        std::vector<uint8_t> rep = svc.handleMessage(req.data(), req.size(), now);
        now += 5;
        return std::string(rep.begin() + 9, rep.end() - 1);
    };

    // 1. request chunk 0 of revision A
    CHECK(chunk(0) == docA.substr(0, 200));
    // 2. configuration becomes revision B, mid-transfer
    svc.publish(docB, GmbIdentity{}, 2u);
    // 3. retry chunk 0 → STILL revision A (a retry must never re-pin)
    CHECK(chunk(0) == docA.substr(0, 200));
    // 4. arbitrary other chunks → still revision A
    CHECK(chunk(3) == docA.substr(600));
    CHECK(chunk(2) == docA.substr(400, 200));
    CHECK(chunk(1) == docA.substr(200, 200));
    // 5. the transfer is complete, so the pin is released
    CHECK(!svc.transferInFlight());
    // 6. the NEXT fresh transfer sees revision B
    CHECK(chunk(0) == docB.substr(0, 200));
}

TEST(gmb_transfer_timeout_releases_the_old_snapshot) {
    GmbSysExService svc;
    const std::string docA(700, 'A');
    const std::string docB(700, 'B');
    svc.publish(docA, GmbIdentity{}, 1u);
    uint32_t now = 1000;

    auto chunkAt = [&](uint16_t idx, uint32_t at) {
        std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(idx);
        std::vector<uint8_t> rep = svc.handleMessage(req.data(), req.size(), at);
        return std::string(rep.begin() + 9, rep.end() - 1);
    };

    CHECK(chunkAt(0, now) == docA.substr(0, 200));
    svc.publish(docB, GmbIdentity{}, 2u);
    CHECK(svc.transferInFlight());
    // The controller goes away for longer than the idle timeout.
    now += GmbSysExService::TRANSFER_IDLE_MS + 1;
    CHECK(chunkAt(0, now) == docB.substr(0, 200));
}

TEST(gmb_handshake_drops_a_pin_that_no_longer_matches) {
    GmbSysExService svc;
    const std::string docA(700, 'A');
    const std::string docB(700, 'B');
    svc.publish(docA, GmbIdentity{}, 1u);
    uint32_t now = 1000;
    std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(0);
    CHECK(!svc.handleMessage(req.data(), req.size(), now).empty());
    CHECK(svc.transferInFlight());
    svc.publish(docB, GmbIdentity{}, 2u);
    // A fresh handshake announces revision B; keeping a pin on A would make
    // every following segment contradict the frame just sent.
    std::vector<uint8_t> hs = GmbSysEx::encodeHandshakeRequest();
    std::vector<uint8_t> rep = svc.handleMessage(hs.data(), hs.size(), now + 10);
    CHECK_EQ((int)rep.size(), 24);
    CHECK_EQ(decode32Le7(&rep[17]), 2u);
    CHECK(!svc.transferInFlight());
}

TEST(gmb_handshake_keeps_a_pin_that_still_matches) {
    GmbSysExService svc;
    const std::string doc(700, 'A');
    svc.publish(doc, GmbIdentity{}, 1u);
    uint32_t now = 1000;
    std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(0);
    CHECK(!svc.handleMessage(req.data(), req.size(), now).empty());
    std::vector<uint8_t> hs = GmbSysEx::encodeHandshakeRequest();
    CHECK(!svc.handleMessage(hs.data(), hs.size(), now + 10).empty());
    CHECK(svc.transferInFlight());
}

TEST(gmb_level_0_never_serves_a_descriptor_segment) {
    // descriptor_size is 0, so there is nothing to transfer. An empty payload
    // would reassemble to "" and then fail to parse on the host; silence leaves
    // the controller honestly on level 0.
    GmbSysExService svc;
    svc.publish("", GmbIdentity{}, 4u);
    uint32_t now = 500;
    std::vector<uint8_t> req = GmbSysEx::encodeDescriptorRequest(0);
    CHECK(svc.handleMessage(req.data(), req.size(), now).empty());
    CHECK(!svc.transferInFlight());
    // And it still answers the handshake.
    std::vector<uint8_t> hs = GmbSysEx::encodeHandshakeRequest();
    CHECK_EQ((int)svc.handleMessage(hs.data(), hs.size(), now + 10).size(), 24);
}
