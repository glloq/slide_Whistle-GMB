/*
 * core/gmb/GmbMidiBridge.h — transport-neutral routing of GMB SysEx.
 *
 *   physical transport ──► channel voice messages ──► MidiRouter (note path)
 *                      └─► GMB SysEx frames       ──► GmbMidiBridge ──► GmbSysExService
 *                                                         │
 *                                                         └─► reply, back out the
 *                                                             SAME transport
 *
 * GMB is a SEPARATE CONTROL PLANE. It is deliberately NOT inside MidiRouter: a
 * SysEx parser bug must never be able to route bytes into NoteOn/CC handling,
 * and the two paths share no state at all.
 *
 * A request arriving in a transport callback is only STAGED here; the reply is
 * built and sent later from service(), on the control-plane task. That keeps
 * allocation and re-entrant transport writes out of the MIDI parse path.
 *
 * ONE pending request at a time: a second request arriving before the first is
 * answered is dropped, which bounds what a SysEx flood can cost to a single
 * fixed-size buffer.
 *
 * Any transport that can carry SysEx in BOTH directions — DIN with a MIDI OUT,
 * BLE-MIDI, RTP-MIDI, native USB MIDI on an S3 — implements IGmbMidiPort and
 * reuses this one service; no protocol logic is duplicated per transport.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBMIDIBRIDGE_H
#define SWC_CORE_GMB_GMBMIDIBRIDGE_H

#include <cstring>

#include "GmbSysExService.h"

namespace swc {
namespace gmb {

// What a MIDI transport must offer to take part in GMB discovery. A transport
// with no return path (a DIN IN with no MIDI OUT wired) simply never registers,
// and keeps working normally for note/CC traffic.
class IGmbMidiPort {
public:
    virtual ~IGmbMidiPort() = default;
    // True when a peer is connected and a SysEx reply can really be delivered.
    virtual bool canSendSysEx() const = 0;
    virtual void sendSysEx(const uint8_t* data, size_t len) = 0;
    virtual const char* portName() const = 0;
};

class GmbMidiBridge {
public:
    static constexpr size_t MAX_PORTS = 4;

    void begin(GmbSysExService* service) { service_ = service; }

    void registerPort(IGmbMidiPort* port) {
        if (!port) return;
        for (size_t i = 0; i < portCount_; ++i) if (ports_[i] == port) return;
        if (portCount_ >= MAX_PORTS) return;
        ports_[portCount_++] = port;
    }

    // Called from a transport's SysEx callback with one COMPLETE message.
    // Copies the frame and returns immediately; nothing is answered here.
    void onSysEx(IGmbMidiPort* from, const uint8_t* data, size_t len) {
        if (!service_ || !data) return;
        // Drop anything not shaped like a GMB request before it costs a copy. A
        // long or foreign SysEx (a sample dump, another vendor's message) is not
        // our business and must not displace a pending GMB request.
        if (len < 6 || len > MAX_REQUEST_BYTES) return;
        if (data[0] != SYSEX_START || data[1] != MANUFACTURER ||
            data[2] != DEVICE_ID || data[len - 1] != SYSEX_END) return;
        if (pendingLen_ != 0) { ++overruns_; return; }   // a reply is already owed
        std::memcpy(pending_, data, len);
        pendingLen_ = len;
        pendingPort_ = from;
        ++staged_;
    }

    // Called from the control-plane loop: answers a staged request, if any, and
    // drives the transfer idle timeout.
    void service(uint32_t nowMs) {
        if (!service_) return;
        if (pendingLen_ == 0) { service_->tick(nowMs); return; }
        // Take the request BEFORE answering, so a callback that fires while we
        // build the response can stage the next one.
        uint8_t frame[MAX_REQUEST_BYTES];
        std::memcpy(frame, pending_, pendingLen_);
        const size_t len = pendingLen_;
        IGmbMidiPort* port = pendingPort_;
        pendingLen_ = 0;
        pendingPort_ = nullptr;

        std::vector<uint8_t> response = service_->handleMessage(frame, len, nowMs);
        if (response.empty()) return;
        // Reply through the transport the request arrived on.
        if (!port || !port->canSendSysEx()) return;
        port->sendSysEx(response.data(), response.size());
    }

    // Send a block 0x11 change notification to every port that can carry it.
    void notifyCapabilitiesChanged(uint8_t changeFlags) {
        if (!service_ || changeFlags == 0) return;
        std::vector<uint8_t> msg = service_->notification(changeFlags);
        if (msg.empty()) return;
        for (size_t i = 0; i < portCount_; ++i)
            if (ports_[i] && ports_[i]->canSendSysEx())
                ports_[i]->sendSysEx(msg.data(), msg.size());
    }

    // True when at least one registered transport really has a return path —
    // the only honest basis for announcing the push-notification flag.
    bool anyPortCanSend() const {
        for (size_t i = 0; i < portCount_; ++i)
            if (ports_[i] && ports_[i]->canSendSysEx()) return true;
        return false;
    }

    size_t portCount() const { return portCount_; }
    uint32_t stagedCount() const { return staged_; }
    uint32_t overrunCount() const { return overruns_; }

private:
    GmbSysExService* service_ = nullptr;
    IGmbMidiPort* ports_[MAX_PORTS] = {nullptr, nullptr, nullptr, nullptr};
    size_t portCount_ = 0;

    uint8_t pending_[MAX_REQUEST_BYTES] = {0};
    size_t  pendingLen_ = 0;
    IGmbMidiPort* pendingPort_ = nullptr;
    uint32_t staged_ = 0, overruns_ = 0;
};

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBMIDIBRIDGE_H
