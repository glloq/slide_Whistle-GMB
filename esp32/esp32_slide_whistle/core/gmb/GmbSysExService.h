/*
 * core/gmb/GmbSysExService.h — transport-independent GMB SysEx endpoint.
 *
 * Holds ONE published descriptor (built once per configuration activation) and
 * answers incoming requests from it. Serving a segment costs a substring copy,
 * never a JSON render.
 *
 * THE INVARIANT OF BLOCK 0x10 — a transfer pins ONE immutable document:
 *
 *   A transfer picks its document exactly once, when it starts, and every later
 *   segment of that transfer is cut out of that same document. A RETRY — of
 *   segment 0 as much as of any other — belongs to the transfer in flight and
 *   must NOT re-pin, because re-pinning is precisely what would let a controller
 *   reassemble segment 0 of revision A with segment 1 of revision B.
 *
 *   The pin is released only when
 *     * every distinct segment of the document has been delivered (UNIQUE
 *       indices are tracked — a transfer must never end merely because the
 *       highest-numbered segment happened to be asked for first), or
 *     * the controller stops asking (idle timeout), or
 *     * a handshake announces a document the pinned one no longer matches.
 *
 * Robustness: every byte that arrives here is untrusted. Frames are length- and
 * framing-checked, payloads must be 7-bit, chunk indices are bounds-checked, a
 * token bucket caps a sustained flood, and an abandoned transfer times out. The
 * only heap this path can grow is the per-transfer "delivered" bitmap, which is
 * sized from the PUBLISHED document — never from the request.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBSYSEXSERVICE_H
#define SWC_CORE_GMB_GMBSYSEXSERVICE_H

#include <memory>
#include <string>
#include <vector>

#include "GmbSysEx.h"

namespace swc {
namespace gmb {

class GmbSysExService {
public:
    // A transfer is considered abandoned after this long without a segment
    // request, which releases the pinned document.
    static constexpr uint32_t TRANSFER_IDLE_MS = 5000;
    // Token bucket: allows the whole discovery burst (handshake + every segment
    // back to back) but caps a sustained flood, so repeated SysEx traffic can
    // never keep the control plane building responses forever.
    static constexpr int      MAX_TOKENS = 32;
    static constexpr uint32_t REFILL_MS  = 5;      // +1 token every 5 ms

    GmbSysExService()
        : descriptor_(std::make_shared<const std::string>(std::string())) {}

    // Replace the published descriptor. Called ONLY after a configuration has
    // been validated, committed and activated. Published atomically: a segment
    // is never cut from a half-built document, and a transfer already in flight
    // keeps the document it started on because that one is held alive by the pin.
    void publish(const std::string& json, const GmbIdentity& identity, uint32_t revision) {
        descriptor_ = std::make_shared<const std::string>(json);
        identity_   = identity;
        revision_   = revision;
    }

    const std::string& descriptorJson() const { return *descriptor_; }
    uint32_t descriptorSize() const { return uint32_t(descriptor_->size()); }
    uint32_t revision() const { return revision_; }
    const GmbIdentity& identity() const { return identity_; }

    // Handshake flags. Bit 0 follows the web server (it must never be announced
    // when there is no HTTP route to fetch from); bit 1 follows the transports
    // (never announce push for a bus with no return path).
    void setHttpDescriptorAvailable(bool available) { httpAvailable_ = available; }
    void setPushNotificationsAvailable(bool available) { pushAvailable_ = available; }
    uint8_t handshakeFlags() const {
        uint8_t f = 0;
        if (httpAvailable_) f |= FLAG_HTTP_DESCRIPTOR;
        if (pushAvailable_) f |= FLAG_PUSH_NOTIFY;
        return f;
    }

    // Handle one complete incoming SysEx message. Returns the bytes to send
    // back, or an empty vector when nothing should be sent (malformed frame,
    // unknown block, out-of-range segment, rate limited).
    std::vector<uint8_t> handleMessage(const uint8_t* data, size_t len, uint32_t nowMs) {
        const SysExRequest req = GmbSysEx::parseRequest(data, len);
        if (!req.valid) { ++dropped_; return {}; }
        if (!allow(nowMs)) { ++dropped_; return {}; }
        ++handled_;

        if (req.block == BLOCK_HANDSHAKE) {
            expireStaleTransfer(nowMs);
            // The handshake announces the CURRENT revision and descriptor_size.
            // A transfer still pinned to a document that is no longer current
            // would answer every following segment with bytes that contradict
            // the frame we are about to send, so it is dropped here rather than
            // five seconds later on the idle timeout. When the pinned document
            // IS the current one this changes nothing.
            if (serving_ && serving_ != descriptor_) endTransfer();
            return GmbSysEx::encodeHandshake(identity_, revision_, descriptorSize(),
                                             handshakeFlags());
        }
        if (req.block == BLOCK_DESCRIPTOR) return serveDescriptorChunk(req.chunkIndex, nowMs);
        return {};
    }

    // Block 0x11 notification for the current revision.
    std::vector<uint8_t> notification(uint8_t changeFlags) const {
        return GmbSysEx::encodeChangeNotification(revision_, changeFlags);
    }

    // Let a caller drive the idle timeout without traffic (the main loop does).
    void tick(uint32_t nowMs) { expireStaleTransfer(nowMs); }

    // --- diagnostics / tests ----------------------------------------------
    uint32_t handledRequests() const { return handled_; }
    uint32_t droppedRequests() const { return dropped_; }
    bool transferInFlight() const { return (bool)serving_; }
    // The document a transfer in flight is pinned to (empty when none).
    std::string pinnedJson() const { return serving_ ? *serving_ : std::string(); }

private:
    std::shared_ptr<const std::string> descriptor_;
    std::shared_ptr<const std::string> serving_;   // pinned for the transfer in flight
    std::vector<uint8_t> delivered_;               // bitmap of UNIQUE indices served
    uint16_t  deliveredCount_ = 0;
    uint32_t  servingLastMs_ = 0;

    GmbIdentity identity_;
    uint32_t revision_ = 0;
    bool     httpAvailable_ = false;
    bool     pushAvailable_ = false;
    uint32_t handled_ = 0, dropped_ = 0;

    int      tokens_ = MAX_TOKENS;
    uint32_t lastRefillMs_ = 0;
    bool     haveRefillTime_ = false;

    bool allow(uint32_t nowMs) {
        if (!haveRefillTime_) { lastRefillMs_ = nowMs; haveRefillTime_ = true; }
        const uint32_t elapsed = elapsed_u32(nowMs, lastRefillMs_);
        if (elapsed >= REFILL_MS) {
            const uint32_t add = elapsed / REFILL_MS;
            const long grown = (long)tokens_ + (long)add;
            tokens_ = int(grown > MAX_TOKENS ? MAX_TOKENS : grown);
            lastRefillMs_ += add * REFILL_MS;
        }
        if (tokens_ <= 0) return false;
        --tokens_;
        return true;
    }

    void endTransfer() {
        serving_.reset();
        delivered_.clear();
        deliveredCount_ = 0;
    }

    void expireStaleTransfer(uint32_t nowMs) {
        // The controller gave up mid-way: release the pinned document so the next
        // transfer starts on the current one instead of a stale snapshot forever.
        if (serving_ && elapsed_u32(nowMs, servingLastMs_) > TRANSFER_IDLE_MS) endTransfer();
    }

    bool markDelivered(uint16_t index) {
        const size_t byteIdx = size_t(index) >> 3;
        if (byteIdx >= delivered_.size()) return false;
        const uint8_t bit = uint8_t(1u << (index & 7));
        if (delivered_[byteIdx] & bit) return false;   // a retry: already counted
        delivered_[byteIdx] |= bit;
        ++deliveredCount_;
        return true;
    }

    std::vector<uint8_t> serveDescriptorChunk(uint16_t index, uint32_t nowMs) {
        expireStaleTransfer(nowMs);

        const bool starting = !serving_;
        const std::shared_ptr<const std::string> doc = starting ? descriptor_ : serving_;
        // Level 0: the handshake announced descriptor_size 0, so there is nothing
        // to transfer. Answering with an empty payload would hand the controller
        // a document that reassembles to "" and then fails to parse; silence
        // leaves it on level 0, which is the truth.
        if (doc->empty()) return {};

        std::vector<uint8_t> out = GmbSysEx::encodeDescriptorChunk(*doc, index);
        if (out.empty()) {
            // Out-of-range segment: answered with silence. It neither starts a
            // transfer nor keeps an existing one alive, so a flood of bogus
            // indices can never pin a document or grow any buffer.
            return out;
        }

        const uint16_t total = chunkCount(doc->size());
        if (starting) {
            serving_ = doc;
            delivered_.assign(size_t((total + 7) / 8), 0);
            deliveredCount_ = 0;
        }
        servingLastMs_ = nowMs;
        markDelivered(index);

        // Released only once EVERY distinct segment has gone out. Ending on "the
        // last index was requested" would cut a transfer short for a controller
        // that fetches out of order, and ending on a raw delivery count would let
        // N retries of segment 0 finish the transfer.
        if (deliveredCount_ >= total) endTransfer();
        return out;
    }
};

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBSYSEXSERVICE_H
