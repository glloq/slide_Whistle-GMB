/*
 * core/gmb/GmbSysEx.h — GMB v2 SysEx frame codec.
 *
 * Transport-independent: this file only ever deals in complete MIDI byte
 * buffers. Every frame it produces is verified against the CURRENT General-
 * Midi-Boop consumer (DeviceManager.parseGmbHandshake / parseDescriptorChunk /
 * parseChangeNotification), which rejects a handshake that is not exactly 24
 * bytes and a notification that is not exactly 12.
 *
 * Robustness: parseRequest() is the only entry point for untrusted bytes. It
 * checks the F0/F7 framing, the manufacturer and device ids, that every payload
 * byte is 7-bit, the exact length of each known block, and the direction (a
 * response or a notification arriving back in is NEVER echoed — that would let
 * two instruments on one bus talk to each other). An unknown block is ignored
 * silently. Nothing here can reach a NoteOn/CC path: this codec has no access
 * to the command queue at all.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBSYSEX_H
#define SWC_CORE_GMB_GMBSYSEX_H

#include <string>
#include <vector>

#include "GmbCapabilities.h"
#include "GmbProtocol.h"

namespace swc {
namespace gmb {

struct SysExRequest {
    bool     valid = false;
    uint8_t  block = 0;
    uint8_t  direction = 0;
    bool     hasChunkIndex = false;
    uint16_t chunkIndex = 0;
};

class GmbSysEx {
public:
    // --- framing checks ----------------------------------------------------
    static bool isWellFormed(const uint8_t* data, size_t len) {
        if (!data || len < 6) return false;
        if (data[0] != SYSEX_START || data[len - 1] != SYSEX_END) return false;
        if (data[1] != MANUFACTURER || data[2] != DEVICE_ID) return false;
        for (size_t i = 1; i < len - 1; ++i) if (data[i] & 0x80) return false;
        return true;
    }

    static SysExRequest parseRequest(const uint8_t* data, size_t len) {
        SysExRequest r;
        if (!isWellFormed(data, len)) return r;
        if (len > MAX_REQUEST_BYTES) return r;      // not a GMB request frame
        r.block = data[3];
        r.direction = data[4];
        if (r.direction != DIR_REQUEST) return r;   // only requests are answered
        if (r.block == BLOCK_HANDSHAKE) {
            if (len != 6) return r;                 // F0 7D 00 01 00 F7
            r.valid = true;
            return r;
        }
        if (r.block == BLOCK_DESCRIPTOR) {
            if (len != 8) return r;                 // F0 7D 00 10 00 <idx lo> <idx hi> F7
            r.hasChunkIndex = true;
            r.chunkIndex = decode14Le7(&data[5]);
            r.valid = true;
            return r;
        }
        return r;                                   // unknown block: ignored silently
    }

    // --- encoders ----------------------------------------------------------

    // Block 1 reply — EXACTLY 24 bytes:
    //   F0 7D 00 01 01 02 instance_id[5] firmware[3] descriptor_size[3]
    //   revision[5] flags F7
    static std::vector<uint8_t> encodeHandshake(const GmbIdentity& id, uint32_t revision,
                                                uint32_t descriptorSize, uint8_t flags) {
        std::vector<uint8_t> m;
        m.reserve(HANDSHAKE_BYTES);
        header(m, BLOCK_HANDSHAKE, DIR_RESPONSE);
        m.push_back(PROTO_VERSION);
        pushLe32(m, id.instanceId);
        for (int i = 0; i < 3; ++i) m.push_back(uint8_t(id.firmware[i] & 0x7F));
        if (descriptorSize > MAX_DESCRIPTOR_SIZE_FIELD) descriptorSize = MAX_DESCRIPTOR_SIZE_FIELD;
        uint8_t sz[3];
        encode21Le7(descriptorSize, sz);
        for (int i = 0; i < 3; ++i) m.push_back(sz[i]);
        pushLe32(m, revision);
        m.push_back(uint8_t(flags & 0x7F));
        m.push_back(SYSEX_END);
        return m;
    }

    // One block 0x10 segment. Returns an EMPTY vector for an out-of-range index:
    // a malformed request is answered with silence rather than with a frame a
    // controller could misassemble.
    static std::vector<uint8_t> encodeDescriptorChunk(const std::string& json, uint16_t index) {
        const uint16_t total = chunkCount(json.size());
        if (index >= total) return std::vector<uint8_t>();
        std::vector<uint8_t> m;
        m.reserve(10 + DESCRIPTOR_CHUNK_PAYLOAD);
        header(m, BLOCK_DESCRIPTOR, DIR_RESPONSE);
        pushLe14(m, total);
        pushLe14(m, index);
        const size_t start = size_t(index) * DESCRIPTOR_CHUNK_PAYLOAD;
        for (size_t i = start; i < json.size() && i < start + DESCRIPTOR_CHUNK_PAYLOAD; ++i) {
            // The descriptor is ASCII by construction; masking anyway guarantees
            // that no serializer bug can ever put a status byte inside a payload.
            m.push_back(uint8_t(uint8_t(json[i]) & 0x7F));
        }
        m.push_back(SYSEX_END);
        return m;
    }

    // Block 0x11 spontaneous change notification — EXACTLY 12 bytes.
    static std::vector<uint8_t> encodeChangeNotification(uint32_t revision, uint8_t changeFlags) {
        std::vector<uint8_t> m;
        m.reserve(CHANGE_NOTIFY_BYTES);
        header(m, BLOCK_CHANGE_NOTIFY, DIR_NOTIFICATION);
        pushLe32(m, revision);
        m.push_back(uint8_t(changeFlags & 0x7F));
        m.push_back(SYSEX_END);
        return m;
    }

    // The outgoing handshake REQUEST, for loopback tests and diagnostics.
    static std::vector<uint8_t> encodeHandshakeRequest() {
        return { SYSEX_START, MANUFACTURER, DEVICE_ID, BLOCK_HANDSHAKE, DIR_REQUEST, SYSEX_END };
    }
    static std::vector<uint8_t> encodeDescriptorRequest(uint16_t index) {
        std::vector<uint8_t> m = { SYSEX_START, MANUFACTURER, DEVICE_ID, BLOCK_DESCRIPTOR, DIR_REQUEST };
        pushLe14(m, index);
        m.push_back(SYSEX_END);
        return m;
    }

private:
    static void header(std::vector<uint8_t>& m, uint8_t block, uint8_t direction) {
        m.push_back(SYSEX_START);
        m.push_back(MANUFACTURER);
        m.push_back(DEVICE_ID);
        m.push_back(block);
        m.push_back(direction);
    }
    static void pushLe32(std::vector<uint8_t>& m, uint32_t v) {
        uint8_t e[5]; encode32Le7(v, e);
        for (int i = 0; i < 5; ++i) m.push_back(e[i]);
    }
    static void pushLe14(std::vector<uint8_t>& m, uint16_t v) {
        uint8_t e[2]; encode14Le7(v, e);
        m.push_back(e[0]); m.push_back(e[1]);
    }
};

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBSYSEX_H
