/*
 * core/MidiTransportBridge.h — the transport-neutral MIDI/control split.
 *
 *   physical transport ──► MidiStreamParser ──► MidiTransportBridge
 *                                                  ├─► channel voice → MidiRouter
 *                                                  └─► GMB SysEx     → GmbMidiBridge
 *
 * This is the single place where a transport's byte stream fans out into the
 * two planes. The note path keeps going through MidiRouter → CommandQueue →
 * RealtimeEngine exactly as before; GMB SysEx goes to its own service and never
 * touches the command queue.
 *
 * Any transport — DIN UART, BLE-MIDI, RTP-MIDI, native USB MIDI on an S3 —
 * reuses this class by feeding it bytes (or, for packet transports, by calling
 * the sink methods directly), so the GMB service is shared, never duplicated.
 *
 * Portable and Arduino-free, so the whole split is unit-tested natively.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_MIDITRANSPORTBRIDGE_H
#define SWC_CORE_MIDITRANSPORTBRIDGE_H

#include "MidiRouter.h"
#include "MidiStreamParser.h"
#include "gmb/GmbMidiBridge.h"

namespace swc {

template <uint16_t QueueLen>
class MidiTransportBridge : public IMidiStreamSink {
public:
    // `port` is the transport this bridge reads from; it is what a GMB reply is
    // sent back through. It may be null for a receive-only transport, in which
    // case SysEx is parsed and then dropped (no return path, no discovery).
    void begin(MidiRouter<QueueLen>* router, gmb::GmbMidiBridge* gmbBridge,
               gmb::IGmbMidiPort* port) {
        router_ = router; gmb_ = gmbBridge; port_ = port;
    }

    void midiNoteOn(uint8_t channel1, uint8_t note, uint8_t velocity) override {
        if (router_) router_->noteOn(channel1, note, velocity);
    }
    void midiNoteOff(uint8_t channel1, uint8_t note) override {
        if (router_) router_->noteOff(channel1, note);
    }
    void midiControlChange(uint8_t channel1, uint8_t cc, uint8_t value) override {
        if (router_) router_->controlChange(channel1, cc, value);
    }
    void midiPitchBend(uint8_t channel1, int16_t bend14) override {
        if (router_) router_->pitchBend(channel1, bend14);
    }
    void midiChannelPressure(uint8_t channel1, uint8_t value) override {
        if (router_) router_->aftertouch(channel1, value);
    }
    void midiSysEx(const uint8_t* data, size_t len) override {
        // Control plane only. Note the total absence of any path back into
        // router_: a SysEx frame can never become a NoteOn.
        if (gmb_) gmb_->onSysEx(port_, data, len);
    }

private:
    MidiRouter<QueueLen>* router_ = nullptr;
    gmb::GmbMidiBridge*   gmb_ = nullptr;
    gmb::IGmbMidiPort*    port_ = nullptr;
};

} // namespace swc

#endif // SWC_CORE_MIDITRANSPORTBRIDGE_H
