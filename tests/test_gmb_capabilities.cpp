/*
 * tests/test_gmb_capabilities.cpp — the capability layer: what the slide
 * whistle is allowed to claim, and (more importantly) what it must not.
 *
 * Every assertion is made against a real RuntimeConfig, so a change to the
 * firmware's own configuration semantics shows up here rather than in a
 * hand-maintained parallel profile.
 */
#include "test_framework.h"
#include "test_gmb_support.h"
#include "../esp32/esp32_slide_whistle/core/Json.h"

#include <fstream>
#include <sstream>

using namespace swc;
using namespace swc::gmb;
using namespace gmbtest;

// ---------------------------------------------------------------------------
// Playable-note derivation
// ---------------------------------------------------------------------------

TEST(gmb_notes_outside_the_mapped_span_are_not_advertised) {
    // The instrument range is 36..96 but only 60..72 is mapped.
    RuntimeConfig c = makeConfig({ makeFlute(1, 36, 96) });
    InstrumentConfig& ic = c.instruments[0];
    ic.map.clear();
    ic.map.setTravelMm(100.0f);
    ic.map.generateLinear(60, 72, 0.0f, 50.0f);

    // NoteMap::positionForNote() happily answers for note 36 — it CLAMPS to the
    // nearest mapped endpoint. That is right for driving the slide and wrong as
    // a capability claim, which is exactly why resolveNoteStrict() exists.
    float mm = -1.0f;
    CHECK(ic.map.positionForNote(36.0f, mm));      // clamping path says "yes"
    CHECK_NEAR(mm, 0.0f, 1e-3);
    CHECK(!resolveNoteStrict(ic.map, 36).resolvable);   // capability path says "no"
    CHECK(!resolveNoteStrict(ic.map, 96).resolvable);
    CHECK(resolveNoteStrict(ic.map, 60).resolvable);
    CHECK(resolveNoteStrict(ic.map, 66).resolvable);
    CHECK(resolveNoteStrict(ic.map, 72).resolvable);

    const GmbNoteSet s = playableNotes(ic, 0);
    CHECK_EQ(s.min(), 60);
    CHECK_EQ(s.max(), 72);
    CHECK_EQ((int)s.count(), 13);
    const std::string json = descriptorFor(c);
    CHECK(contains(json, "\"notes\":{\"mode\":\"range\",\"min\":60,\"max\":72}"));
    CHECK(!contains(json, "\"min\":36"));
}

TEST(gmb_provisional_enabled_map_points_are_playable) {
    // generateLinear() marks points enabled but NOT hardware-calibrated.
    // "not calibrated" must not mean "unplayable".
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    const InstrumentConfig& ic = c.instruments[0];
    CHECK_EQ(ic.map.calibratedCount(), 0);              // nothing hand-calibrated
    for (uint8_t n = 60; n <= 72; ++n) {
        CHECK(ic.map.entry(n).enabled);
        CHECK(!ic.map.entry(n).calibrated);
    }
    CHECK_EQ((int)playableNotes(ic, 0).count(), 13);
}

TEST(gmb_soft_limits_clip_the_playable_set) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) });   // 60..84 over 0..90 mm
    InstrumentConfig& ic = c.instruments[0];
    // 3.75 mm per semitone; a 45 mm soft ceiling stops at note 72.
    ic.motion.softMaxMm = 45.0f;
    const GmbNoteSet s = playableNotes(ic, 0);
    CHECK_EQ(s.min(), 60);
    CHECK_EQ(s.max(), 72);
    CHECK(!s.has(73));
}

TEST(gmb_disabled_slide_drive_is_not_musically_configured) {
    // No slide movement means no pitch control: the persisted configuration
    // cannot produce notes, so the entry is announced as unconfigured.
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    c.instruments[0].motion.type = SlideDriveType::Disabled;
    CHECK(playableNotes(c.instruments[0], 0).empty());
    CHECK(!instrumentConfigured(c.instruments[0], true, 0));
    const std::string json = descriptorFor(c);
    CHECK(contains(json, "\"configured\":false"));
    CHECK(!contains(json, "\"notes\""));
}

TEST(gmb_range_vs_discrete_note_modes) {
    // One flute: a contiguous set is announced as a range.
    CHECK(contains(descriptorFor(makeConfig({ makeFlute(1, 60, 72) })),
                   "\"notes\":{\"mode\":\"range\",\"min\":60,\"max\":72}"));
    // Two flutes merged on one channel with a gap: a discrete list, never a
    // range that would falsely promise the notes in between.
    const std::string merged = descriptorFor(makeConfig({ makeFlute(1, 60, 64), makeFlute(1, 70, 74) }));
    CHECK(contains(merged, "\"notes\":{\"mode\":\"discrete\",\"list\":[60,61,62,63,64,70,71,72,73,74]}"));
    CHECK(!contains(merged, "\"mode\":\"range\",\"min\":60,\"max\":74"));
}

// ---------------------------------------------------------------------------
// Channels, merging, polyphony
// ---------------------------------------------------------------------------

TEST(gmb_config_channel_1_16_maps_to_descriptor_0_15) {
    CHECK_EQ(descriptorChannel(1), 0);
    CHECK_EQ(descriptorChannel(16), 15);
    CHECK_EQ(descriptorChannel(0), 0);     // omni answers on channel 0
    CHECK_EQ(descriptorChannel(99), 0);    // out of range degrades safely
    CHECK(contains(descriptorFor(makeConfig({ makeFlute(16, 60, 72) })), "\"channel\":15"));
}

TEST(gmb_one_slide_whistle_descriptor) {
    const std::string json = descriptorFor(makeConfig({ makeFlute(1, 60, 84) }));
    CHECK(contains(json, "\"gmb_descriptor\":2"));
    CHECK(contains(json, "\"model\":\"Slide-Whistle-GMB\""));
    CHECK(contains(json, "\"type\":\"pipe\""));
    CHECK(contains(json, "\"subtype\":\"whistle\""));
    CHECK(contains(json, "\"gm_program\":78"));
    CHECK(contains(json, "\"channel\":0"));
    CHECK(contains(json, "\"configured\":true"));
    // A single physical slide whistle is intrinsically monophonic.
    CHECK(contains(json, "\"polyphony\":{\"max\":1}"));
    CHECK(contains(json, "\"voices\":[{\"id\":\"flute0\""));
}

TEST(gmb_four_flutes_on_four_channels) {
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72), makeFlute(2, 60, 72),
                                         makeFlute(3, 60, 72), makeFlute(4, 60, 72) });
    const GmbSnapshot s = buildSnapshot(inputFor(c));
    CHECK_EQ((int)s.instruments.size(), 4);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(s.instruments[i].channel, i);
        CHECK_EQ((int)s.instruments[i].voices.size(), 1);
        CHECK_EQ(s.instruments[i].polyphonyMax, 1);
        CHECK(!s.instruments[i].oneNotePerVoice);   // a lone voice needs no constraint
    }
    const std::string json = descriptorFor(c);
    for (const char* ch : {"\"channel\":0", "\"channel\":1", "\"channel\":2", "\"channel\":3"})
        CHECK(contains(json, ch));
}

TEST(gmb_same_channel_flutes_merge_into_one_entry_with_voices) {
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 64), makeFlute(1, 70, 74) });
    const GmbSnapshot s = buildSnapshot(inputFor(c));
    // ONE entry, not two — duplicate channels are invalid in GMB.
    CHECK_EQ((int)s.instruments.size(), 1);
    CHECK_EQ(s.instruments[0].channel, 0);
    CHECK_EQ((int)s.instruments[0].voices.size(), 2);
    CHECK_EQ_STR(s.instruments[0].voices[0].id, "flute0");
    CHECK_EQ_STR(s.instruments[0].voices[1].id, "flute1");
    // Disjoint ranges: the two flutes really can sound two different notes.
    CHECK_EQ(s.instruments[0].polyphonyMax, 2);
    CHECK(s.instruments[0].oneNotePerVoice);
    CHECK(contains(descriptorFor(c), "\"constraints\":[{\"type\":\"one_note_per_voice\"}]"));
}

TEST(gmb_overlapping_same_channel_ranges_do_not_overstate_polyphony) {
    // Both flutes accept a note in the overlap, so it reaches BOTH (unison) and
    // a second note in the overlap simply steals the first. Polyphony is 1.
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72), makeFlute(1, 66, 78) });
    const GmbSnapshot s = buildSnapshot(inputFor(c));
    CHECK_EQ((int)s.instruments.size(), 1);
    CHECK_EQ((int)s.instruments[0].voices.size(), 2);
    CHECK_EQ(s.instruments[0].polyphonyMax, 1);
    CHECK(s.instruments[0].oneNotePerVoice);
}

TEST(gmb_partially_overlapping_voices_count_groups_not_voices) {
    // flute0 and flute1 overlap (one group); flute2 is disjoint from both.
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 70), makeFlute(1, 65, 75),
                                         makeFlute(1, 90, 100) });
    const GmbSnapshot s = buildSnapshot(inputFor(c));
    CHECK_EQ((int)s.instruments.size(), 1);
    CHECK_EQ((int)s.instruments[0].voices.size(), 3);
    CHECK_EQ(s.instruments[0].polyphonyMax, 2);
}

TEST(gmb_never_emits_duplicate_descriptor_channels) {
    // Four flutes all on MIDI channel 1, plus the omni case, must still yield
    // one entry per distinct descriptor channel.
    for (const RuntimeConfig& c : std::vector<RuntimeConfig>{
             makeConfig({ makeFlute(1, 60, 64), makeFlute(1, 66, 70),
                          makeFlute(1, 72, 76), makeFlute(1, 78, 82) }),
             makeConfig({ makeFlute(0, 60, 64), makeFlute(1, 70, 74) }),
             makeConfig({ makeFlute(0, 60, 64), makeFlute(0, 70, 74), makeFlute(3, 60, 64) }) }) {
        const GmbSnapshot s = buildSnapshot(inputFor(c));
        for (size_t i = 0; i < s.instruments.size(); ++i)
            for (size_t j = i + 1; j < s.instruments.size(); ++j)
                CHECK(s.instruments[i].channel != s.instruments[j].channel);
    }
}

TEST(gmb_omni_flute_is_announced_on_channel_zero_and_flagged) {
    const RuntimeConfig c = makeConfig({ makeFlute(0, 60, 72) });
    const GmbSnapshot s = buildSnapshot(inputFor(c));
    CHECK_EQ((int)s.instruments.size(), 1);
    CHECK_EQ(s.instruments[0].channel, 0);
    CHECK(s.instruments[0].physical.omni);
    CHECK(contains(descriptorFor(c), "\"omni\":true"));
}

TEST(gmb_disabled_flute_contributes_nothing) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72), makeFlute(2, 60, 72) });
    c.instruments[1].enabled = false;
    const GmbSnapshot s = buildSnapshot(inputFor(c));
    CHECK_EQ((int)s.instruments.size(), 1);
    CHECK_EQ(s.instruments[0].channel, 0);
    CHECK(!contains(descriptorFor(c), "\"channel\":1"));
}

// ---------------------------------------------------------------------------
// The "never empty / configured=false" rules
// ---------------------------------------------------------------------------

TEST(gmb_descriptor_never_has_zero_instruments) {
    RuntimeConfig noneEnabled = makeConfig({ makeFlute(1, 60, 72) });
    noneEnabled.instruments[0].enabled = false;
    RuntimeConfig zeroCount = defaultConfig();
    zeroCount.instrumentCount = 0;
    for (const RuntimeConfig& c : { defaultConfig(), noneEnabled, zeroCount }) {
        for (bool valid : { true, false }) {
            const GmbSnapshot s = buildSnapshot(inputFor(c, valid));
            CHECK(s.instruments.size() >= 1);
            const std::string json = renderDescriptor(s, 1);
            CHECK(!contains(json, "\"instruments\":[]"));
        }
    }
    // A null config still produces a legal document.
    GmbBuildInput empty;
    CHECK_EQ((int)buildSnapshot(empty).instruments.size(), 1);
}

TEST(gmb_factory_default_emits_the_legal_placeholder) {
    const std::string json = descriptorFor(defaultConfig());
    CHECK(contains(json, "\"instruments\":[{\"channel\":0,\"configured\":false,"
                         "\"type\":\"pipe\",\"subtype\":\"whistle\",\"gm_program\":78}]"));
    // An unconfigured instrument publishes NO capability it cannot vouch for.
    CHECK(!contains(json, "\"notes\""));
    CHECK(!contains(json, "\"polyphony\""));
    CHECK(!contains(json, "\"timing\""));
    CHECK(!contains(json, "\"expression\""));
    CHECK(!contains(json, "\"voices\""));
}

TEST(gmb_invalid_config_is_announced_as_unconfigured) {
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    const std::string json = descriptorFor(c, 1, /*valid=*/false);
    CHECK(contains(json, "\"configured\":false"));
    CHECK(!contains(json, "\"notes\""));
}

TEST(gmb_configured_ignores_transient_runtime_state) {
    // `configured` describes the PERSISTED musical configuration. Nothing about
    // homing, motion, faults or muting is an input to it — buildSnapshot() has
    // no access to a runtime object at all, which is the structural guarantee.
    const RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    const std::string a = descriptorFor(c);
    const std::string b = descriptorFor(c);
    CHECK_EQ_STR(a, b);
    CHECK(contains(a, "\"configured\":true"));
}

// ---------------------------------------------------------------------------
// Timing — "absent = unknown"
// ---------------------------------------------------------------------------

TEST(gmb_prepare_timeout_is_never_serialized_as_a_latency) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) });
    c.instruments[0].seq.prepareTimeoutMs = 4000;    // a FAILURE timeout
    const std::string json = descriptorFor(c);
    CHECK(!contains(json, "4000"));
    CHECK(!contains(json, "\"max_ms\":4000"));
    // Changing only the timeout must not change one byte of the descriptor.
    RuntimeConfig d = c;
    d.instruments[0].seq.prepareTimeoutMs = 9000;
    CHECK_EQ_STR(descriptorFor(c), descriptorFor(d));
}

TEST(gmb_unknown_timing_fields_are_omitted_not_zero) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) });
    // A pressure tank's readiness is state-dependent: no honest constant exists.
    c.instruments[0].air.source.type = AirSourceType::PumpsTank;
    c.instruments[0].seq.minNoteMs = 0;
    const std::string json = descriptorFor(c);
    CHECK(!contains(json, "\"timing\""));
    CHECK(!contains(json, "\"prepare\""));
    // Never emitted at all — no acoustic onset measurement exists yet.
    CHECK(!contains(json, "excite"));
    CHECK(!contains(json, "latency_ms"));
    CHECK(!contains(json, "release_ms"));
    CHECK(!contains(json, "rearticulation_ms"));
    // And certainly never as a zero that would read as "instantaneous".
    CHECK(!contains(json, "\"base_ms\":0"));
    CHECK(!contains(json, "\"min_note_ms\":0"));
}

TEST(gmb_prepare_bound_is_the_max_of_air_and_slide_not_their_sum) {
    // The sequencer starts the air source and the slide move in the same call,
    // so the two overlap.
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) });   // 90 mm span
    InstrumentConfig& ic = c.instruments[0];
    ic.air.source.spinUpMs = 150;
    // 90 mm at 120 mm/s with 800 mm/s^2: 90/120 + 120/800 = 0.9 s.
    CHECK_EQ(slideTravelMs(90.0f, 120.0f, 800.0f), 900u);
    CHECK(contains(descriptorFor(c), "\"prepare\":{\"base_ms\":150,\"max_ms\":900,\"silent\":true}"));
    CHECK(!contains(descriptorFor(c), "\"max_ms\":1050"));   // never the sum

    // A very slow fan dominates instead.
    ic.air.source.spinUpMs = 3000;
    CHECK(contains(descriptorFor(c), "\"prepare\":{\"base_ms\":3000,\"max_ms\":3000,\"silent\":true}"));
}

TEST(gmb_air_prepare_bounds_per_source_type) {
    AirConfig a;
    a.source.type = AirSourceType::ExternalPassive;
    CHECK(airPrepareMs(a).known);
    CHECK_EQ(airPrepareMs(a).ms, 0u);          // a MEASURED zero, not an unknown
    a.source.type = AirSourceType::FanPwm; a.source.spinUpMs = 220;
    CHECK(airPrepareMs(a).known);
    CHECK_EQ(airPrepareMs(a).ms, 220u);
    a.source.type = AirSourceType::PumpsDirect; a.source.pumpCount = 3; a.source.cascadeDelayMs = 120;
    CHECK_EQ(airPrepareMs(a).ms, 240u);        // (3 - 1) stagger delays
    a.source.type = AirSourceType::PumpsTank;
    CHECK(!airPrepareMs(a).known);             // state-dependent: unknown
}

TEST(gmb_glissando_is_not_advertised_as_a_silent_move) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) });
    c.instruments[0].seq.legato = LegatoPolicy::Glissando;
    const std::string json = descriptorFor(c);
    CHECK(contains(json, "\"silent\":false"));
    CHECK(contains(json, "\"legato\":\"glissando\""));
    // Every policy other than AlwaysClose can hold the air open across a move.
    CHECK(prepareIsSilent(LegatoPolicy::AlwaysClose));
    for (LegatoPolicy p : { LegatoPolicy::Glissando, LegatoPolicy::HoldWithinTime,
                            LegatoPolicy::HoldWithinDistance, LegatoPolicy::HoldWithinMoveTime,
                            LegatoPolicy::SafetyLargeMove })
        CHECK(!prepareIsSilent(p));
}

TEST(gmb_min_note_ms_comes_from_the_effective_runtime_rule) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    c.instruments[0].seq.minNoteMs = 0;
    CHECK(!contains(descriptorFor(c), "min_note_ms"));
    c.instruments[0].seq.minNoteMs = 55;
    CHECK(contains(descriptorFor(c), "\"min_note_ms\":55"));
}

// ---------------------------------------------------------------------------
// Expression
// ---------------------------------------------------------------------------

TEST(gmb_cc_list_is_derived_from_the_active_ccmap) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    // Default map, flow servo present, angle servo absent.
    CHECK(contains(descriptorFor(c), "\"cc\":[1,2,7,11,64]"));

    // No flow controller: breath/expression/volume reach no hardware.
    InstrumentConfig& ic = c.instruments[0];
    ic.air.flow.type = FlowControlType::None;
    CHECK(contains(descriptorFor(c), "\"cc\":[1,64]"));

    // Vibrato routing disabled: CC1 no longer drives anything.
    ic.cc.vibratoEnabled = false;
    CHECK(contains(descriptorFor(c), "\"cc\":[64]"));

    // A mapping of 0 means disabled and is ignored.
    ic.cc.sustain = 0;
    CHECK(contains(descriptorFor(c), "\"cc\":[]"));

    // The jet-angle CC only appears when the angle servo is attached.
    ic.air.flow.type = FlowControlType::FlowServo;
    ic.cc.vibratoEnabled = true;
    ic.cc.sustain = 64;
    ic.air.angle.enabled = true;
    ic.air.angle.pin = 18;
    CHECK(contains(descriptorFor(c), "\"cc\":[1,2,7,11,64,74]"));
}

TEST(gmb_velocity_is_true_only_when_it_changes_the_output) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    InstrumentConfig& ic = c.instruments[0];
    // Fan with a real drive span: velocity scales the source.
    CHECK(velocityEffective(ic, playableNotes(ic, 0)));
    CHECK(contains(descriptorFor(c), "\"velocity\":true"));

    // A fan whose min and max are identical produces the same air whatever the
    // velocity; the flow controller still responds, so velocity stays true.
    ic.air.source.min01 = ic.air.source.max01 = 0.8f;
    CHECK(velocityEffective(ic, playableNotes(ic, 0)));

    // ... but with no flow controller either, nothing reads velocity at all.
    ic.air.flow.type = FlowControlType::None;
    CHECK(!velocityEffective(ic, playableNotes(ic, 0)));
    CHECK(contains(descriptorFor(c), "\"velocity\":false"));

    // A calibrated per-note airNominal OVERRIDES velocity on the flow path
    // (AirSystem::startNote uses airNominal when it is non-zero).
    ic.air.flow.type = FlowControlType::FlowServo;
    for (uint8_t n = 60; n <= 72; ++n) ic.map.entry(n).airNominal = 90;
    CHECK(!velocityEffective(ic, playableNotes(ic, 0)));
    // One note left on velocity is enough for the capability to be real.
    ic.map.entry(66).airNominal = 0;
    CHECK(velocityEffective(ic, playableNotes(ic, 0)));
}

TEST(gmb_pitch_bend_and_aftertouch_follow_the_runtime) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) });
    std::string json = descriptorFor(c);
    // Derived from the engine's own constant, not a hardcoded 2.
    CHECK_NEAR(DEFAULT_PITCH_BEND_RANGE_SEMITONES, 2.0f, 1e-6);
    CHECK(contains(json, "\"pitch_bend\":{\"supported\":true,\"range_semitones\":2}"));
    CHECK(contains(json, "\"channel_aftertouch\":true"));
    CHECK(contains(json, "\"poly_aftertouch\":false"));

    // Channel pressure only does something when vibrato routing is enabled.
    c.instruments[0].cc.vibratoEnabled = false;
    CHECK(contains(descriptorFor(c), "\"channel_aftertouch\":false"));

    // A different configured range flows straight through.
    GmbBuildInput in = inputFor(c);
    in.pitchBendRangeSemitones = 12.0f;
    CHECK(contains(renderDescriptor(buildSnapshot(in), 1),
                   "\"pitch_bend\":{\"supported\":true,\"range_semitones\":12}"));
    // No slide, no bend.
    in.pitchBendRangeSemitones = 0.0f;
    CHECK(contains(renderDescriptor(buildSnapshot(in), 1), "\"pitch_bend\":{\"supported\":false}"));
}

TEST(gmb_merged_voices_promise_only_common_capabilities) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 64), makeFlute(1, 70, 74) });
    // Only ONE of the two flutes has a jet-angle servo and vibrato routing.
    c.instruments[0].air.angle.enabled = true;
    c.instruments[0].air.angle.pin = 18;
    c.instruments[1].cc.vibratoEnabled = false;
    c.instruments[1].air.source.min01 = c.instruments[1].air.source.max01 = 0.5f;
    c.instruments[1].air.flow.type = FlowControlType::None;
    const std::string json = descriptorFor(c);
    // CC74 (angle) and CC1 (vibrato) are dropped: only one voice implements them.
    CHECK(contains(json, "\"cc\":[64]"));
    CHECK(contains(json, "\"channel_aftertouch\":false"));
    CHECK(contains(json, "\"velocity\":false"));
}

// ---------------------------------------------------------------------------
// Transpose
// ---------------------------------------------------------------------------

TEST(gmb_transpose_is_published_and_does_not_invent_pitches) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) });
    c.midi.transpose = 3;
    const std::string json = descriptorFor(c);
    // The advertised set is the PHYSICAL playable pitch set, unchanged by the
    // global transpose, and the transpose itself is published so a host can
    // compensate.
    CHECK(contains(json, "\"notes\":{\"mode\":\"range\",\"min\":60,\"max\":84}"));
    CHECK(contains(json, "\"transpose_semitones\":3"));
    c.midi.transpose = -5;
    CHECK(contains(descriptorFor(c), "\"transpose_semitones\":-5"));
    CHECK(contains(descriptorFor(c), "\"notes\":{\"mode\":\"range\",\"min\":60,\"max\":84}"));
}

TEST(gmb_transpose_drops_pitches_no_midi_note_could_reach) {
    // The engine plays (incoming note + transpose) and DROPS a note transposed
    // out of 0..127, so a physical pitch below `transpose` is unreachable.
    RuntimeConfig c = makeConfig({ makeFlute(1, 0, 20) });
    c.midi.transpose = 10;
    const GmbNoteSet s = playableNotes(c.instruments[0], 10);
    CHECK_EQ(s.min(), 10);       // pitch 9 would need host note -1
    CHECK_EQ(s.max(), 20);
    // A NEGATIVE transpose clips the top instead: the engine plays
    // (incoming - 10), so pitch 127 would need host note 137.
    RuntimeConfig d = makeConfig({ makeFlute(1, 110, 127) });
    d.midi.transpose = -10;
    const GmbNoteSet t = playableNotes(d.instruments[0], -10);
    CHECK_EQ(t.min(), 110);
    CHECK_EQ(t.max(), 117);
    // With a positive transpose the whole top of the map stays reachable.
    RuntimeConfig e = makeConfig({ makeFlute(1, 110, 127) });
    e.midi.transpose = 10;
    const GmbNoteSet u = playableNotes(e.instruments[0], 10);
    CHECK_EQ(u.min(), 110);
    CHECK_EQ(u.max(), 127);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST(gmb_descriptor_is_ascii_only) {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) }, "Atelier");
    const std::string json = descriptorFor(c);
    CHECK(isAscii(json));
    for (unsigned char ch : json) CHECK(ch < 0x80);
}

TEST(gmb_non_ascii_names_are_escaped_as_uXXXX) {
    // A UTF-8 device name is the normal case for this project.
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 72) }, "Atelier \xC3\xA9t\xC3\xA9");
    const std::string json = descriptorFor(c);
    CHECK(isAscii(json));
    CHECK(contains(json, "\"name\":\"Atelier \\u00e9t\\u00e9\""));

    // Control characters, quotes and backslashes.
    CHECK_EQ_STR(detail::escAscii("a\"b\\c"), "a\\\"b\\\\c");
    CHECK_EQ_STR(detail::escAscii(std::string("a\x01" "b")), "a\\u0001b");
    CHECK_EQ_STR(detail::escAscii("\x7f"), "\\u007f");
    // A 3-byte sequence, and one outside the BMP (a surrogate pair).
    CHECK_EQ_STR(detail::escAscii("\xE2\x99\xAA"), "\\u266a");
    CHECK_EQ_STR(detail::escAscii("\xF0\x9F\x8E\xB5"), "\\ud83c\\udfb5");
    // Malformed UTF-8 degrades to U+FFFD rather than leaking a high byte.
    CHECK_EQ_STR(detail::escAscii("\xC3"), "\\ufffd");
    CHECK(isAscii(detail::escAscii("\xFF\xFE")));
}

// ---------------------------------------------------------------------------
// Reference fixture — the documented example, generated by the real builder
// ---------------------------------------------------------------------------

TEST(gmb_reference_fixture_matches_the_generated_descriptor) {
    std::ifstream f("fixtures/gmb_descriptor_reference.json", std::ios::binary);
    CHECK(f.good());
    if (!f.good()) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string fixture = ss.str();
    while (!fixture.empty() && (fixture.back() == '\n' || fixture.back() == '\r')) fixture.pop_back();
    // Regenerate with:  make -C tests fixture
    CHECK_EQ_STR(descriptorFor(referenceConfig(), 1), fixture);
}

// A faithful port of the CURRENT structural rules in General-Midi-Boop's
// src/midi/instrument/DescriptorProtocol.js validateDescriptor(). The fixture
// was additionally run through the real JS validator; this keeps the invariant
// enforced in CI without a Node dependency in the C++ suite.
static bool validatesLikeGmb(const std::string& json, std::string& why) {
    JsonValue d;
    if (!jsonParse(json, d, nullptr)) { why = "not parseable"; return false; }
    if (d.type != JsonValue::Obj) { why = "not an object"; return false; }
    if (d.int_or("gmb_descriptor", 0) != 2) { why = "gmb_descriptor must be 2"; return false; }
    const JsonValue* rev = d.find("revision");
    if (rev && (rev->type != JsonValue::Num || rev->asNum() < 0)) { why = "bad revision"; return false; }
    const JsonValue* dev = d.find("device");
    if (dev && dev->type != JsonValue::Obj) { why = "device must be an object"; return false; }
    const JsonValue* arr = d.find("instruments");
    if (!arr || arr->type != JsonValue::Arr || arr->arr.empty()) { why = "instruments must be non-empty"; return false; }
    if (arr->arr.size() > 16) { why = "at most 16 instruments"; return false; }
    std::vector<long> seen;
    for (const JsonValue& inst : arr->arr) {
        if (inst.type != JsonValue::Obj) { why = "instrument must be an object"; return false; }
        const JsonValue* chv = inst.find("channel");
        if (!chv || chv->type != JsonValue::Num) { why = "channel must be an integer"; return false; }
        const long ch = chv->asInt();
        if (ch < 0 || ch > 15) { why = "channel out of 0..15"; return false; }
        for (long s : seen) if (s == ch) { why = "duplicate channel"; return false; }
        seen.push_back(ch);
        const JsonValue* cfg = inst.find("configured");
        if (cfg && cfg->type != JsonValue::Bool) { why = "configured must be a boolean"; return false; }
        const JsonValue* gp = inst.find("gm_program");
        if (gp && (gp->type != JsonValue::Num || gp->asInt() < 0 || gp->asInt() > 127)) { why = "bad gm_program"; return false; }
        for (const char* k : {"type", "subtype"}) {
            const JsonValue* v = inst.find(k);
            if (v && v->type != JsonValue::Str) { why = std::string(k) + " must be a string"; return false; }
        }
        const JsonValue* notes = inst.find("notes");
        if (!notes) continue;
        if (notes->type != JsonValue::Obj) { why = "notes must be an object"; return false; }
        const std::string mode = notes->str_or("mode", "");
        if (mode == "range") {
            const long mn = notes->int_or("min", 0), mx = notes->int_or("max", 0);
            if (mn < 0 || mn > 127 || mx < 0 || mx > 127 || mn > mx) { why = "bad range"; return false; }
        } else if (mode == "discrete") {
            const JsonValue* list = notes->find("list");
            if (!list || list->type != JsonValue::Arr || list->arr.empty()) { why = "discrete list empty"; return false; }
            for (const JsonValue& n : list->arr)
                if (n.type != JsonValue::Num || n.asInt() < 0 || n.asInt() > 127) { why = "bad note"; return false; }
        } else { why = "notes.mode must be range or discrete"; return false; }
    }
    return true;
}

TEST(gmb_descriptors_satisfy_the_gmb_validator_rules) {
    std::string why;
    const std::vector<RuntimeConfig> cases = {
        referenceConfig(),
        defaultConfig(),
        makeConfig({ makeFlute(1, 60, 72), makeFlute(2, 60, 72), makeFlute(3, 60, 72), makeFlute(4, 60, 72) }),
        makeConfig({ makeFlute(1, 60, 64), makeFlute(1, 70, 74) }),
        makeConfig({ makeFlute(0, 60, 64), makeFlute(1, 70, 74) }),
        makeConfig({ makeFlute(16, 40, 50) }),
    };
    for (const RuntimeConfig& c : cases) {
        for (bool valid : { true, false }) {
            why.clear();
            const std::string json = descriptorFor(c, 7, valid);
            const bool ok = validatesLikeGmb(json, why);
            if (!ok) std::printf("    invalid descriptor: %s\n      %s\n", why.c_str(), json.c_str());
            CHECK(ok);
        }
    }
}
