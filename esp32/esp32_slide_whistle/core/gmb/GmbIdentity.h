/*
 * core/gmb/GmbIdentity.h — stable per-exemplar identity for GMB discovery.
 *
 * `instance_id` is the pivot of the whole protocol: it is what reattaches a
 * configuration stored in General-Midi-Boop to the right physical controller.
 * It therefore MUST
 *   - stay identical across reboots,
 *   - stay identical across configuration changes,
 *   - differ between two boards flashed with the same binary.
 *
 * It is consequently derived from a HARDWARE identity (the ESP32 eFuse/MAC),
 * never from the device name, the Wi-Fi settings or anything else a user can
 * edit. The fold from the hardware id to 32 bits lives here, pure, so it is
 * unit-tested natively; reading the eFuse is the platform layer's job.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_GMB_GMBIDENTITY_H
#define SWC_CORE_GMB_GMBIDENTITY_H

#include <string>

#include "GmbProtocol.h"
#include "../FirmwareVersion.h"

namespace swc {
namespace gmb {

// The `device.model` label of §5. GMB derives NO capability from it — it is a
// display string only, and must stay stable for this firmware family.
static constexpr const char* DEVICE_MODEL = "Slide-Whistle-GMB";

struct GmbIdentity {
    std::string deviceName = "Slide Whistle";
    std::string model      = DEVICE_MODEL;
    uint8_t     firmware[3] = { FIRMWARE_VERSION_MAJOR,
                                FIRMWARE_VERSION_MINOR,
                                FIRMWARE_VERSION_PATCH };
    uint32_t    instanceId = 0;   // 0 means "no hardware identity available"
};

// Fold a hardware unique id (the ESP32 48-bit eFuse MAC) into the 32-bit
// instance id announced in the handshake.
//
// The MAC is HASHED rather than truncated: the wire format carries 7 bits per
// byte, so truncating would throw away one bit in eight and could collapse two
// boards whose MACs differ only in those bits onto one identity.
//
// 0 is reserved as "no identity" (the non-conformant `{0,0,0,0,0}` the spec
// calls out), so the returned value is never 0.
inline uint32_t instanceIdFromHardwareId(uint64_t hardwareId) {
    uint8_t bytes[6];
    for (int i = 0; i < 6; ++i) bytes[i] = uint8_t((hardwareId >> (8 * i)) & 0xFF);
    uint32_t id = fnv1a32(bytes, sizeof(bytes));
    return id == 0 ? 1u : id;
}

} // namespace gmb
} // namespace swc

#endif // SWC_CORE_GMB_GMBIDENTITY_H
