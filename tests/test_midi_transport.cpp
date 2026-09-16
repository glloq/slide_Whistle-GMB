/*
 * tests/test_midi_transport.cpp — the transport-neutral MIDI/control split.
 *
 * The invariant under test: channel-voice bytes reach the command queue and
 * NOTHING else, GMB SysEx reaches the control plane and NOTHING else, and a
 * malformed or foreign SysEx can never synthesise a note.
 */
#include "test_framework.h"
#include "test_gmb_support.h"

#include "../esp32/esp32_slide_whistle/core/MidiTransportBridge.h"

using namespace swc;
using namespace swc::gmb;
using namespace gmbtest;

namespace {

struct RecordingSink : IMidiStreamSink {
    struct Ev { char kind; uint8_t ch, a, b; int16_t bend; };
    std::vector<Ev> events;
    std::vector<std::vector<uint8_t>> sysex;
    void midiNoteOn(uint8_t ch, uint8_t n, uint8_t v) override { events.push_back({'n', ch, n, v, 0}); }
    void midiNoteOff(uint8_t ch, uint8_t n) override { events.push_back({'o', ch, n, 0, 0}); }
    void midiControlChange(uint8_t ch, uint8_t cc, uint8_t v) override { events.push_back({'c', ch, cc, v, 0}); }
    void midiPitchBend(uint8_t ch, int16_t b) override { events.push_back({'b', ch, 0, 0, b}); }
    void midiChannelPressure(uint8_t ch, uint8_t v) override { events.push_back({'p', ch, v, 0, 0}); }
    void midiSysEx(const uint8_t* d, size_t n) override { sysex.push_back(std::vector<uint8_t>(d, d + n)); }
};

void feedAll(MidiStreamParser<64>& p, const std::vector<uint8_t>& bytes) {
    for (uint8_t b : bytes) p.feed(b);
}

} // namespace

// ---------------------------------------------------------------------------
// Stream parser
// ---------------------------------------------------------------------------

TEST(midi_parser_decodes_channel_voice_messages) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    feedAll(p, {0x90, 60, 100,            // NoteOn ch1
                0x80, 60, 64,             // NoteOff ch1
                0xB2, 7, 90,              // CC ch3
                0xE0, 0x00, 0x40,         // PitchBend centre ch1
                0xD4, 55});               // Channel pressure ch5
    CHECK_EQ((int)sink.events.size(), 5);
    CHECK_EQ(sink.events[0].kind, 'n'); CHECK_EQ(sink.events[0].ch, 1); CHECK_EQ(sink.events[0].a, 60); CHECK_EQ(sink.events[0].b, 100);
    CHECK_EQ(sink.events[1].kind, 'o'); CHECK_EQ(sink.events[1].a, 60);
    CHECK_EQ(sink.events[2].kind, 'c'); CHECK_EQ(sink.events[2].ch, 3); CHECK_EQ(sink.events[2].a, 7); CHECK_EQ(sink.events[2].b, 90);
    CHECK_EQ(sink.events[3].kind, 'b'); CHECK_EQ(sink.events[3].bend, 0);
    CHECK_EQ(sink.events[4].kind, 'p'); CHECK_EQ(sink.events[4].ch, 5); CHECK_EQ(sink.events[4].a, 55);
}

TEST(midi_parser_handles_running_status_and_note_on_velocity_zero) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    feedAll(p, {0x90, 60, 100, 62, 100, 60, 0});   // three events, one status byte
    CHECK_EQ((int)sink.events.size(), 3);
    CHECK_EQ(sink.events[0].kind, 'n');
    CHECK_EQ(sink.events[1].kind, 'n'); CHECK_EQ(sink.events[1].a, 62);
    CHECK_EQ(sink.events[2].kind, 'o'); CHECK_EQ(sink.events[2].a, 60);   // vel 0 == NoteOff
}

TEST(midi_parser_ignores_realtime_bytes_anywhere) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    // A clock byte between the status and its data, and inside a SysEx.
    feedAll(p, {0x90, 0xF8, 60, 0xFE, 100});
    CHECK_EQ((int)sink.events.size(), 1);
    CHECK_EQ(sink.events[0].a, 60);
    CHECK_EQ(sink.events[0].b, 100);
    feedAll(p, {0xF0, 0x7D, 0x00, 0xF8, 0x01, 0x00, 0xF7});
    CHECK_EQ((int)sink.sysex.size(), 1);
    CHECK_EQ((int)sink.sysex[0].size(), 6);   // the clock byte is not part of the frame
}

TEST(midi_parser_cancels_running_status_on_system_common) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    feedAll(p, {0x90, 60, 100});
    feedAll(p, {0xF1, 0x20});          // MIDI Time Code quarter frame
    feedAll(p, {62, 100});             // orphan data bytes: no running status left
    CHECK_EQ((int)sink.events.size(), 1);
}

TEST(midi_parser_drops_an_oversized_sysex_and_resynchronises) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    std::vector<uint8_t> big;
    big.push_back(0xF0);
    for (int i = 0; i < 400; ++i) big.push_back(0x11);
    big.push_back(0xF7);
    feedAll(p, big);
    CHECK_EQ((int)sink.sysex.size(), 0);      // truncating into a plausible short frame would be worse
    CHECK_EQ((int)sink.events.size(), 0);     // and it certainly must not become a note
    // The parser is usable again straight away.
    feedAll(p, {0x90, 64, 100});
    CHECK_EQ((int)sink.events.size(), 1);
}

TEST(midi_parser_abandons_an_unterminated_sysex) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    feedAll(p, {0xF0, 0x7D, 0x00, 0x01});     // no F7
    feedAll(p, {0x90, 60, 100});              // a new status byte arrives
    CHECK_EQ((int)sink.sysex.size(), 0);
    CHECK_EQ((int)sink.events.size(), 1);
    CHECK_EQ(sink.events[0].a, 60);
}

TEST(midi_parser_never_turns_sysex_into_notes) {
    RecordingSink sink;
    MidiStreamParser<64> p;
    p.begin(&sink);
    // A SysEx body full of bytes that would be a NoteOn's data if misparsed.
    feedAll(p, {0xF0, 0x7D, 0x00, 0x10, 0x00, 60, 100, 0xF7});
    CHECK_EQ((int)sink.events.size(), 0);
    CHECK_EQ((int)sink.sysex.size(), 1);
}

// ---------------------------------------------------------------------------
// The two-plane split
// ---------------------------------------------------------------------------

TEST(midi_bridge_splits_notes_from_gmb_control_traffic) {
    CommandQueue<32> queue;
    MidiRouter<32> router(queue);
    GmbSysExService svc;
    svc.publish("{\"gmb_descriptor\":2}", GmbIdentity{}, 5u);
    GmbMidiBridge gmb;
    gmb.begin(&svc);
    FakePort port;
    gmb.registerPort(&port);

    MidiTransportBridge<32> bridge;
    bridge.begin(&router, &gmb, &port);
    MidiStreamParser<64> parser;
    parser.begin(&bridge);

    // A note goes to the command queue...
    feedAll(parser, {0x91, 64, 120});
    Command c{};
    CHECK(queue.pop(c));
    CHECK(c.type == CommandType::NoteOn);
    CHECK_EQ(c.channel, 2);
    CHECK_EQ(c.a, 64);
    CHECK_EQ(c.b, 120);
    CHECK(queue.empty());

    // ... and a GMB handshake goes to the control plane, not the queue.
    const std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    feedAll(parser, req);
    CHECK(queue.empty());                   // nothing enqueued
    CHECK_EQ(gmb.stagedCount(), 1u);
    CHECK_EQ((int)port.sent.size(), 0);     // staged, not answered in the callback
    gmb.service(100);                       // answered from the control loop
    CHECK_EQ((int)port.sent.size(), 1);
    CHECK_EQ((int)port.sent[0].size(), 24);
    CHECK_EQ(port.sent[0][3], 0x01);
    CHECK(queue.empty());
}

TEST(midi_bridge_replies_through_the_originating_transport) {
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 1u);
    GmbMidiBridge gmb;
    gmb.begin(&svc);
    FakePort a, b;
    gmb.registerPort(&a);
    gmb.registerPort(&b);
    const std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    gmb.onSysEx(&b, req.data(), req.size());
    gmb.service(10);
    CHECK_EQ((int)a.sent.size(), 0);
    CHECK_EQ((int)b.sent.size(), 1);
}

TEST(midi_bridge_bounds_a_flood_to_one_pending_request) {
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 1u);
    GmbMidiBridge gmb;
    gmb.begin(&svc);
    FakePort port;
    gmb.registerPort(&port);
    const std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    for (int i = 0; i < 50; ++i) gmb.onSysEx(&port, req.data(), req.size());
    CHECK_EQ(gmb.stagedCount(), 1u);
    CHECK_EQ(gmb.overrunCount(), 49u);
    gmb.service(10);
    CHECK_EQ((int)port.sent.size(), 1);
}

TEST(midi_bridge_ignores_foreign_and_malformed_sysex_without_buffering) {
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 1u);
    GmbMidiBridge gmb;
    gmb.begin(&svc);
    FakePort port;
    gmb.registerPort(&port);
    const std::vector<std::vector<uint8_t>> junk = {
        {0xF0, 0x41, 0x10, 0x42, 0x12, 0xF7},              // another vendor
        {0xF0, 0x7D, 0x01, 0x01, 0x00, 0xF7},              // wrong GMB device id
        {0xF0, 0x7D, 0x00, 0x01, 0x00},                    // no F7
        std::vector<uint8_t>(40, 0x00),                    // far too long
    };
    for (const auto& j : junk) gmb.onSysEx(&port, j.data(), j.size());
    CHECK_EQ(gmb.stagedCount(), 0u);
    gmb.service(10);
    CHECK_EQ((int)port.sent.size(), 0);
}

TEST(midi_bridge_notifies_only_transports_with_a_return_path) {
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 9u);
    GmbMidiBridge gmb;
    gmb.begin(&svc);
    FakePort up, down;
    down.up = false;
    gmb.registerPort(&up);
    gmb.registerPort(&down);
    CHECK(gmb.anyPortCanSend());
    gmb.notifyCapabilitiesChanged(CHANGE_INSTRUMENTS);
    CHECK_EQ((int)up.sent.size(), 1);
    CHECK_EQ((int)down.sent.size(), 0);
    CHECK_EQ(decode32Le7(&up.sent[0][5]), 9u);
    // An empty flag set is not a notification.
    gmb.notifyCapabilitiesChanged(0);
    CHECK_EQ((int)up.sent.size(), 1);
    // With no usable return path at all, push must not be advertised.
    up.up = false;
    CHECK(!gmb.anyPortCanSend());
}

TEST(midi_bridge_without_a_return_path_still_plays_notes) {
    // A DIN IN with no MIDI OUT: notes work, discovery does not. That is exactly
    // what the protocol says about a one-way bus.
    CommandQueue<32> queue;
    MidiRouter<32> router(queue);
    GmbSysExService svc;
    svc.publish("{}", GmbIdentity{}, 1u);
    GmbMidiBridge gmb;
    gmb.begin(&svc);
    FakePort port;
    port.up = false;                       // no TX pin wired
    gmb.registerPort(&port);
    MidiTransportBridge<32> bridge;
    bridge.begin(&router, &gmb, &port);
    MidiStreamParser<64> parser;
    parser.begin(&bridge);

    feedAll(parser, {0x90, 60, 100});
    Command c{};
    CHECK(queue.pop(c));
    CHECK(c.type == CommandType::NoteOn);

    const std::vector<uint8_t> req = GmbSysEx::encodeHandshakeRequest();
    feedAll(parser, req);
    gmb.service(10);
    CHECK_EQ((int)port.sent.size(), 0);    // nothing to answer through
}
