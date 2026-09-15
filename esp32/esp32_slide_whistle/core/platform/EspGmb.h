/*
 * core/platform/EspGmb.h — the ESP32-specific half of the GMB control plane.
 *
 * Everything the portable core/gmb/ layer deliberately does not know about:
 *
 *   - the STABLE instance id, folded from the eFuse MAC;
 *   - the persistent revision record, in NVS (Preferences) — never in
 *     config.json, so a configuration save never rewrites the counter and a
 *     counter bump never rewrites the configuration;
 *   - a genuinely bidirectional DIN MIDI transport on a hardware UART.
 *
 * Nothing here actuates anything. The DIN port pushes channel-voice messages
 * onto the existing MidiRouter -> CommandQueue path (the real-time task remains
 * the sole owner of the hardware) and hands SysEx frames to the GMB bridge.
 *
 * Status: IMPLEMENTED (structure) - NOT TESTED - REQUIRES HARDWARE
 *         (the pure protocol/capability layers it serves ARE tested natively)
 */
#ifndef SWC_CORE_PLATFORM_ESPGMB_H
#define SWC_CORE_PLATFORM_ESPGMB_H
#if defined(ARDUINO)

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>

#include "../MidiStreamParser.h"
#include "../MidiTransportBridge.h"
#include "../gmb/GmbRuntime.h"

namespace swc {

// ---------------------------------------------------------------------------
// Stable per-exemplar hardware identity.
//
// ESP.getEfuseMac() reads the factory-programmed eFuse MAC: unique per chip,
// identical across reboots, and completely independent of the device name, the
// Wi-Fi settings and everything else a user can edit. Exactly what GMB's
// instance_id contract asks for.
// ---------------------------------------------------------------------------
inline uint32_t espGmbInstanceId() {
    return gmb::instanceIdFromHardwareId((uint64_t)ESP.getEfuseMac());
}

// ---------------------------------------------------------------------------
// Persistent revision record (NVS).
// ---------------------------------------------------------------------------
class EspGmbRevisionStore : public gmb::IGmbRevisionStore {
public:
    bool load(gmb::GmbRevisionRecord& out) override {
        Preferences p;
        if (!p.begin(kNamespace, /*readOnly=*/true)) return false;
        out.revision  = p.getUInt("rev", 0);
        out.signature = p.getUInt("sig", 0);
        p.end();
        return out.revision != 0;
    }
    bool save(const gmb::GmbRevisionRecord& rec) override {
        Preferences p;
        if (!p.begin(kNamespace, /*readOnly=*/false)) return false;
        // Written only when the tracker says the advertised capabilities really
        // moved, and putUInt is itself a no-op when the value is unchanged, so
        // flash wear stays proportional to real capability changes.
        p.putUInt("rev", rec.revision);
        p.putUInt("sig", rec.signature);
        p.end();
        return true;
    }
private:
    static constexpr const char* kNamespace = "swgmb";
};

// ---------------------------------------------------------------------------
// DIN MIDI on a hardware UART - the baseline BIDIRECTIONAL transport.
//
// RX alone is enough to play notes; automatic GMB discovery additionally needs
// a MIDI OUT, so canSendSysEx() is true only when a TX pin is configured. That
// is the honest signal for the push-notification handshake flag: a DIN IN with
// no OUT is explicitly listed by the protocol as "no automatic recognition".
// ---------------------------------------------------------------------------
template <uint16_t QueueLen>
class DinMidiPort : public gmb::IGmbMidiPort {
public:
    static constexpr uint32_t MIDI_BAUD = 31250;
    // UART2 is present on both the classic WROOM and the S3, and the universal
    // firmware uses no other hardware UART (Serial0 is the console).
    static constexpr uint8_t UART_NUM = 2;

    DinMidiPort() : uart_(UART_NUM) {}

    // Returns false when DIN is disabled or no RX pin is assigned, in which case
    // nothing is opened and no pin is touched.
    bool begin(const MidiConfig& cfg, MidiRouter<QueueLen>* router, gmb::GmbMidiBridge* bridge) {
        if (!cfg.din || cfg.dinRxPin < 0) return false;
        rxPin_ = cfg.dinRxPin;
        txPin_ = cfg.dinTxPin;
        uart_.begin(MIDI_BAUD, SERIAL_8N1, rxPin_, txPin_);
        bridge_.begin(router, bridge, this);
        parser_.begin(&bridge_);
        open_ = true;
        return true;
    }

    // Drain whatever the UART has. Called from the control-plane loop; the
    // bytes only ever become queue commands, never direct actuation.
    void poll() {
        if (!open_) return;
        int budget = 256;   // bounded work per call so one loop pass stays short
        while (budget-- > 0 && uart_.available() > 0) {
            const int b = uart_.read();
            if (b < 0) break;
            parser_.feed((uint8_t)b);
        }
    }

    // --- IGmbMidiPort ------------------------------------------------------
    bool canSendSysEx() const override { return open_ && txPin_ >= 0; }
    void sendSysEx(const uint8_t* data, size_t len) override {
        if (!canSendSysEx() || !data || len == 0) return;
        uart_.write(data, len);
    }
    const char* portName() const override { return "din"; }

    bool isOpen() const { return open_; }

private:
    HardwareSerial uart_;
    MidiStreamParser<64>      parser_;
    MidiTransportBridge<QueueLen> bridge_;
    int8_t rxPin_ = -1, txPin_ = -1;
    bool   open_ = false;
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_PLATFORM_ESPGMB_H
