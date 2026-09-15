/*
 * core/gmb/GmbProtocol.h — General-Midi-Boop v2 wire constants and 7-bit codecs.
 *
 * This is the lowest layer of the GMB control plane: pure arithmetic and named
 * constants, no I/O, no configuration, no hardware. Everything above it (the
 * frame codec, the SysEx service, the runtime) depends on this header and on
 * nothing platform-specific, so the whole protocol is unit-tested natively.
 *
 * Reference: General-Midi-Boop docs/SYSEX_IDENTITY.md (protocol v2), verified
 * against the CURRENT consumer implementation in that repository
 * (src/midi/devices/DeviceManager.js parseGmbHandshake / parseDescriptorChunk /
 * parseChangeNotification and src/midi/instrument/DescriptorProtocol.js).
 *
 *   F0 7D 00 <block> <direction> ... F7
 *   7D = experimental / educational manufacturer id, 00 = GMB device id.
 *
 * GMB IS CONTROL-PLANE CODE. Nothing in this directory may touch a motor, a
 * servo, a valve, a pump or an air system, directly or indirectly.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBPROTOCOL_H
#define SWC_CORE_GMB_GMBPROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace swc {
namespace gmb {

// --- framing ---------------------------------------------------------------
static constexpr uint8_t SYSEX_START   = 0xF0;
static constexpr uint8_t SYSEX_END     = 0xF7;
static constexpr uint8_t MANUFACTURER  = 0x7D;   // experimental / educational
static constexpr uint8_t DEVICE_ID     = 0x00;   // General-Midi-Boop
static constexpr uint8_t PROTO_VERSION = 0x02;   // this document

// --- blocks ----------------------------------------------------------------
static constexpr uint8_t BLOCK_HANDSHAKE     = 0x01;
static constexpr uint8_t BLOCK_DESCRIPTOR    = 0x10;
static constexpr uint8_t BLOCK_CHANGE_NOTIFY = 0x11;

// --- directions ------------------------------------------------------------
static constexpr uint8_t DIR_REQUEST      = 0x00;
static constexpr uint8_t DIR_RESPONSE     = 0x01;
static constexpr uint8_t DIR_NOTIFICATION = 0x02;

// --- handshake flags (SYSEX_IDENTITY.md §2) --------------------------------
static constexpr uint8_t FLAG_HTTP_DESCRIPTOR = 1u << 0;   // GET /gmb/descriptor.json reachable
static constexpr uint8_t FLAG_PUSH_NOTIFY     = 1u << 1;   // block 0x11 can actually be delivered

// --- change flags (SYSEX_IDENTITY.md §4) -----------------------------------
static constexpr uint8_t CHANGE_IDENTITY         = 1u << 0;
static constexpr uint8_t CHANGE_INSTRUMENTS      = 1u << 1;
static constexpr uint8_t CHANGE_TIMING           = 1u << 2;
static constexpr uint8_t CHANGE_RESTART_REQUIRED = 1u << 3;

// --- sizes -----------------------------------------------------------------
// Exact size of the block-1 reply. The spec fixes the frame at 24 bytes and the
// GMB consumer rejects anything else (`bytes.length !== 24`).
static constexpr size_t HANDSHAKE_BYTES = 24;
// Exact size of a block 0x11 notification (the consumer checks `length !== 12`).
static constexpr size_t CHANGE_NOTIFY_BYTES = 12;
// Descriptor payload carried by ONE block 0x10 segment (§3): 200 bytes keeps the
// whole message at 210, below the BLE-MIDI reassembly limit.
static constexpr size_t DESCRIPTOR_CHUNK_PAYLOAD = 200;
// Longest GMB REQUEST we will even look at. Every request the protocol defines
// is 6 or 8 bytes; a longer frame is not a GMB request and is dropped before it
// is buffered, which caps what a SysEx flood can cost us.
static constexpr size_t MAX_REQUEST_BYTES = 16;
// Largest value the 21-bit `descriptor_size` field can carry.
static constexpr uint32_t MAX_DESCRIPTOR_SIZE_FIELD = 0x1FFFFFu;
// Largest descriptor this firmware will publish. A configuration that would
// render past this is rejected and the device falls back to level 0 (a
// descriptor_size of 0) rather than announcing a size it cannot serve.
static constexpr size_t MAX_DESCRIPTOR_BYTES = 8192;
// `total_chunks` / `chunk_index` are 14-bit fields (2 x 7 bits).
static constexpr uint16_t MAX_CHUNK_INDEX = 0x3FFF;

// ---------------------------------------------------------------------------
// 7-bit-safe little-endian integer codecs.
//
// A 32-bit value occupies 5 bytes: bytes 0..3 carry 7 bits each and byte 4
// carries bits 28..31 — a full NIBBLE. The GMB consumer masks that last byte
// with 0x0f, so the 3-bit variant used by the legacy v1 codec would silently
// drop bit 31 and halve the instance-id space.
// ---------------------------------------------------------------------------
inline void encode32Le7(uint32_t value, uint8_t out[5]) {
    out[0] = uint8_t(value & 0x7F);
    out[1] = uint8_t((value >> 7) & 0x7F);
    out[2] = uint8_t((value >> 14) & 0x7F);
    out[3] = uint8_t((value >> 21) & 0x7F);
    out[4] = uint8_t((value >> 28) & 0x0F);
}

inline uint32_t decode32Le7(const uint8_t in[5]) {
    return (uint32_t(in[0] & 0x7F)) |
           (uint32_t(in[1] & 0x7F) << 7) |
           (uint32_t(in[2] & 0x7F) << 14) |
           (uint32_t(in[3] & 0x7F) << 21) |
           (uint32_t(in[4] & 0x0F) << 28);
}

// 21-bit value over 3 bytes — the `descriptor_size` field.
inline void encode21Le7(uint32_t value, uint8_t out[3]) {
    out[0] = uint8_t(value & 0x7F);
    out[1] = uint8_t((value >> 7) & 0x7F);
    out[2] = uint8_t((value >> 14) & 0x7F);
}

inline uint32_t decode21Le7(const uint8_t in[3]) {
    return (uint32_t(in[0] & 0x7F)) |
           (uint32_t(in[1] & 0x7F) << 7) |
           (uint32_t(in[2] & 0x7F) << 14);
}

// 14-bit value over 2 bytes — `total_chunks` and `chunk_index`.
inline void encode14Le7(uint16_t value, uint8_t out[2]) {
    out[0] = uint8_t(value & 0x7F);
    out[1] = uint8_t((value >> 7) & 0x7F);
}

inline uint16_t decode14Le7(const uint8_t in[2]) {
    return uint16_t((uint16_t(in[0] & 0x7F)) | (uint16_t(in[1] & 0x7F) << 7));
}

// FNV-1a over a byte range. Deterministic across builds and boots, which is what
// makes it usable as the persisted capability signature.
inline uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    if (!data) return h;
    for (size_t i = 0; i < len; ++i) { h ^= uint32_t(data[i]); h *= 16777619u; }
    return h;
}

// Number of block 0x10 segments a document of `jsonSize` bytes is served in.
// Always at least one, so a (hypothetical) empty document is still addressable.
inline uint16_t chunkCount(size_t jsonSize) {
    size_t total = (jsonSize + DESCRIPTOR_CHUNK_PAYLOAD - 1) / DESCRIPTOR_CHUNK_PAYLOAD;
    if (total == 0) total = 1;
    if (total > MAX_CHUNK_INDEX) total = MAX_CHUNK_INDEX;
    return uint16_t(total);
}

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBPROTOCOL_H
