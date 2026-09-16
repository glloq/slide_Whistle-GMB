/*
 * core/FirmwareVersion.h — semantic version of the UNIVERSAL firmware.
 *
 * The three numbers are announced verbatim in the General-Midi-Boop v2
 * handshake (block 1, `firmware[3]`), so each must stay inside 0..127: the
 * field is a 7-bit SysEx byte and a value above 127 could not be transmitted.
 *
 * This version describes the universal firmware only. The deprecated legacy
 * sketches (slide_Whistle_Fan_Servo, slide_Whistle_Solenoid_Servo and the v3
 * esp32_slide_whistle.ino) keep their own historic versioning and do not
 * participate in GMB discovery.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_FIRMWAREVERSION_H
#define SWC_CORE_FIRMWAREVERSION_H

#include <cstdint>

namespace swc {

static constexpr uint8_t FIRMWARE_VERSION_MAJOR = 1;
static constexpr uint8_t FIRMWARE_VERSION_MINOR = 0;
static constexpr uint8_t FIRMWARE_VERSION_PATCH = 0;

static_assert(FIRMWARE_VERSION_MAJOR < 128, "firmware major must fit a 7-bit SysEx byte");
static_assert(FIRMWARE_VERSION_MINOR < 128, "firmware minor must fit a 7-bit SysEx byte");
static_assert(FIRMWARE_VERSION_PATCH < 128, "firmware patch must fit a 7-bit SysEx byte");

} // namespace swc

#endif // SWC_CORE_FIRMWAREVERSION_H
