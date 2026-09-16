/*
 * core/gmb/GmbRuntime.h — the portable facade that owns the GMB control plane.
 *
 * One object ties together the four pieces and enforces the order in which they
 * may move:
 *
 *   begin()                     once at boot, after the configuration has been
 *                               loaded and validated
 *   onConfigurationActivated()  after a new configuration has been validated,
 *                               committed and made ACTIVE
 *
 * onConfigurationActivated() is the ONLY capability-change entry point. It
 * rebuilds the snapshot, bumps the revision if and only if the advertised
 * capabilities really moved, republishes the descriptor and emits block 0x11. A
 * failed or rejected configuration write never reaches it, so the published
 * descriptor cannot move on a bad POST.
 *
 * Everything platform-specific is injected: the hardware instance id, the NVS
 * revision store, and the transports. This class itself is pure C++17 and is
 * unit-tested natively, end to end, against a fake store and a fake port.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBRUNTIME_H
#define SWC_CORE_GMB_GMBRUNTIME_H

#include "GmbMidiBridge.h"
#include "GmbRevision.h"

namespace swc {
namespace gmb {

class GmbRuntime {
public:
    // Boot-time initialisation from the ACTIVE configuration.
    void begin(const GmbBuildInput& in, IGmbRevisionStore* store) {
        store_ = store;
        bridge_.begin(&service_);
        snapshot_ = buildSnapshot(in);
        const CapabilitySignature sig = computeSignature(snapshot_);
        GmbRevisionRecord stored;
        if (!store_ || !store_->load(stored)) stored = GmbRevisionRecord{};
        if (tracker_.begin(stored, sig) && store_) store_->save(tracker_.record());
        republish();
        // No block 0x11 at boot: a controller that just saw us appear reads the
        // handshake anyway, and the notification would race the transport's own
        // connection setup.
    }

    // Report that a new configuration is validated, committed and ACTIVE.
    // `restartRequired` says the change needs a reboot to reach the hardware —
    // the descriptor still describes the PERSISTED musical configuration (§9:
    // `configured` is about the stored configuration, never a transient runtime
    // state), and the RESTART_REQUIRED flag tells the host a reboot is pending.
    //
    // Returns true when the revision actually moved.
    bool onConfigurationActivated(const GmbBuildInput& in, bool restartRequired) {
        GmbSnapshot next = buildSnapshot(in);
        const CapabilitySignature sig = computeSignature(next);
        if (!tracker_.onConfigurationActivated(sig, restartRequired)) {
            // Nothing GMB is told about changed: no revision bump, no flash
            // write, no descriptor churn and no notification.
            return false;
        }
        snapshot_ = next;
        if (store_) store_->save(tracker_.record());
        republish();
        bridge_.notifyCapabilitiesChanged(tracker_.changeFlags());
        return true;
    }

    // Drive the staged-request service and the transfer idle timeout. Called
    // from the control-plane loop, never from the real-time actuator task.
    void service(uint32_t nowMs) {
        // Only announce push when a transport can really deliver a notification.
        service_.setPushNotificationsAvailable(bridge_.anyPortCanSend());
        bridge_.service(nowMs);
    }

    void setHttpDescriptorAvailable(bool available) {
        service_.setHttpDescriptorAvailable(available);
    }

    GmbSysExService& service() { return service_; }
    GmbMidiBridge&   bridge()  { return bridge_; }
    const GmbSnapshot& snapshot() const { return snapshot_; }

    const std::string& descriptorJson() const { return service_.descriptorJson(); }
    uint32_t revision() const { return tracker_.revision(); }
    uint32_t instanceId() const { return snapshot_.identity.instanceId; }
    uint8_t  lastChangeFlags() const { return tracker_.changeFlags(); }

private:
    void republish() {
        snapshot_.revision = tracker_.revision();
        std::string json = renderDescriptor(snapshot_, tracker_.revision());
        // A descriptor we could not serve in full must not be announced: fall
        // back to level 0 (descriptor_size == 0) rather than advertise a size
        // the block 0x10 transfer cannot deliver.
        if (json.size() > MAX_DESCRIPTOR_BYTES || !isAscii(json)) json.clear();
        service_.publish(json, snapshot_.identity, tracker_.revision());
    }

    GmbSnapshot        snapshot_;
    RevisionTracker    tracker_;
    GmbSysExService    service_;
    GmbMidiBridge      bridge_;
    IGmbRevisionStore* store_ = nullptr;
};

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBRUNTIME_H
