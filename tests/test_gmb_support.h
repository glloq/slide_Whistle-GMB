/*
 * tests/test_gmb_support.h — shared fixtures for the GMB test suite.
 *
 * Builds realistic slide-whistle configurations so every capability assertion is
 * made against the SAME RuntimeConfig the firmware would actually run, never
 * against a hand-written "expected" profile.
 */
#ifndef SWC_TESTS_GMB_SUPPORT_H
#define SWC_TESTS_GMB_SUPPORT_H

#include "../esp32/esp32_slide_whistle/core/RealtimeEngine.h"
#include "../esp32/esp32_slide_whistle/core/gmb/GmbRuntime.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gmbtest {

using namespace swc;

// A fully configured stepper-driven slide whistle: fan source, solenoid gate,
// flow servo, and a provisional (generated, enabled, NOT hardware-calibrated)
// linear map over [noteLo, noteHi] spanning 0..90 mm of a 100 mm slide.
inline InstrumentConfig makeFlute(uint8_t channel, uint8_t noteLo, uint8_t noteHi,
                                  float posLo = 0.0f, float posHi = 90.0f) {
    InstrumentConfig ic;
    ic.enabled = true;
    std::snprintf(ic.name, sizeof(ic.name), "Flute %u", (unsigned)channel);
    ic.midiChannel = channel;
    ic.noteMin = noteLo;
    ic.noteMax = noteHi;

    ic.motion.type        = SlideDriveType::StepDir;
    ic.motion.travelMm    = 100.0f;
    ic.motion.softMinMm   = 0.0f;
    ic.motion.softMaxMm   = 100.0f;
    ic.motion.maxSpeedMmS = 120.0f;
    ic.motion.accelMmS2   = 800.0f;
    ic.motion.stepper.stepPin = 25; ic.motion.stepper.dirPin = 26;
    ic.motion.stepper.enablePin = 27; ic.motion.stepper.endstopMin.pin = 34;

    ic.map.clear();
    ic.map.setTravelMm(100.0f);
    ic.map.generateLinear(noteLo, noteHi, posLo, posHi);

    ic.air.source.type     = AirSourceType::FanPwm;
    ic.air.source.pin[0]   = 13;
    ic.air.source.spinUpMs = 150;
    ic.air.source.min01    = 0.15f;
    ic.air.source.max01    = 1.0f;
    ic.air.gate.type       = AirGateType::SolenoidSimple;
    ic.air.gate.pin        = 14;
    ic.air.flow.type       = FlowControlType::FlowServo;
    ic.air.flow.pin        = 16;
    ic.air.flow.min = 0; ic.air.flow.nominal = 64; ic.air.flow.max = 127;
    ic.air.angle.enabled   = false;

    ic.seq.legato    = LegatoPolicy::AlwaysClose;
    ic.seq.minNoteMs = 40;
    // Deliberately NOT an acoustic measurement: a failure timeout that must
    // never reach the descriptor.
    ic.seq.prepareTimeoutMs = 4000;
    return ic;
}

inline RuntimeConfig makeConfig(const std::vector<InstrumentConfig>& flutes,
                                const char* deviceName = "Slide Whistle") {
    RuntimeConfig c = defaultConfig();
    std::snprintf(c.device.name, sizeof(c.device.name), "%s", deviceName);
    c.instrumentCount = (uint8_t)(flutes.size() > MAX_INSTRUMENTS ? MAX_INSTRUMENTS : flutes.size());
    for (uint8_t i = 0; i < c.instrumentCount; ++i) c.instruments[i] = flutes[i];
    return c;
}

// The canonical single-flute controller used by the reference fixture.
inline RuntimeConfig referenceConfig() {
    RuntimeConfig c = makeConfig({ makeFlute(1, 60, 84) }, "Slide Whistle");
    std::snprintf(c.instruments[0].name, sizeof(c.instruments[0].name), "%s", "Slide Whistle");
    c.midi.din = true;
    c.midi.dinRxPin = 4;
    c.midi.dinTxPin = 17;
    return c;
}

inline gmb::GmbBuildInput inputFor(const RuntimeConfig& c, bool valid = true,
                                   uint32_t instanceId = 0x1234ABCDu) {
    gmb::GmbBuildInput in;
    in.config = &c;
    in.configValid = valid;
    in.identity.instanceId = instanceId;
    in.identity.deviceName = std::string(c.device.name);
    in.pitchBendRangeSemitones = DEFAULT_PITCH_BEND_RANGE_SEMITONES;
    return in;
}

inline std::string descriptorFor(const RuntimeConfig& c, uint32_t revision = 1,
                                 bool valid = true) {
    return gmb::renderDescriptor(gmb::buildSnapshot(inputFor(c, valid)), revision);
}

// --- test doubles ---------------------------------------------------------

struct FakeRevisionStore : gmb::IGmbRevisionStore {
    gmb::GmbRevisionRecord rec;
    int saves = 0;
    bool load(gmb::GmbRevisionRecord& out) override {
        if (rec.revision == 0) return false;
        out = rec;
        return true;
    }
    bool save(const gmb::GmbRevisionRecord& r) override { rec = r; ++saves; return true; }
};

struct FakePort : gmb::IGmbMidiPort {
    bool up = true;
    std::vector<std::vector<uint8_t>> sent;
    bool canSendSysEx() const override { return up; }
    void sendSysEx(const uint8_t* d, size_t n) override { sent.push_back(std::vector<uint8_t>(d, d + n)); }
    const char* portName() const override { return "fake"; }
};

// --- helpers --------------------------------------------------------------

inline bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Reassemble a descriptor by asking the service for every segment, in the given
// order. Returns the reassembled document.
inline std::string reassemble(gmb::GmbSysExService& svc, const std::vector<uint16_t>& order,
                              uint32_t& nowMs) {
    std::vector<std::string> parts(order.size());
    uint16_t total = 0;
    for (uint16_t idx : order) {
        std::vector<uint8_t> req = gmb::GmbSysEx::encodeDescriptorRequest(idx);
        std::vector<uint8_t> rep = svc.handleMessage(req.data(), req.size(), nowMs);
        nowMs += 5;
        if (rep.size() < 10) continue;
        total = gmb::decode14Le7(&rep[5]);
        const uint16_t got = gmb::decode14Le7(&rep[7]);
        if (got >= parts.size()) parts.resize(got + 1);
        parts[got].assign(rep.begin() + 9, rep.end() - 1);
    }
    std::string out;
    for (uint16_t i = 0; i < total && i < parts.size(); ++i) out += parts[i];
    return out;
}

} // namespace gmbtest

#endif // SWC_TESTS_GMB_SUPPORT_H
