/*
 * core/HardwareValidator.h — central hardware-resource validator.
 *
 * Validates the WHOLE configuration at once (not one form at a time,
 * Section 11): GPIO conflicts across instruments, flash-reserved pins,
 * input-only pins used as outputs, strapping pins, ADC2/WiFi contention, LEDC
 * channel / PCA9685 channel+address exhaustion, and min/max range sanity.
 *
 * The default configuration must produce zero errors (correction #21).
 *
 * Uses std::vector/std::string — invoked only on config apply, never on the
 * real-time hot path.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_HARDWAREVALIDATOR_H
#define SWC_CORE_HARDWAREVALIDATOR_H

#include "Types.h"
#include <vector>
#include <string>
#include <utility>

namespace swc {

enum class Severity : uint8_t { Error = 0, Warning };

struct ValidationIssue {
    std::string code;      // e.g. "GPIO_CONFLICT"
    std::string message;   // human readable
    std::string field;     // e.g. "instruments[1].air.pump.pin"
    Severity    severity = Severity::Error;
};

enum class BoardKind : uint8_t { Esp32Wroom = 0, Esp32S3, Custom };

struct BoardProfile {
    BoardKind kind = BoardKind::Esp32Wroom;
    int  maxGpio = 39;
    // membership predicates implemented via small tables
    const int* inputOnly; size_t inputOnlyN;
    const int* flashReserved; size_t flashReservedN;
    const int* strapping; size_t strappingN;
    const int* adc2; size_t adc2N;
    uint8_t ledcChannels = 16;   // ESP32 has 16 LEDC channels

    static BoardProfile wroom() {
        static const int inOnly[]  = {34, 35, 36, 37, 38, 39};
        static const int flash[]   = {6, 7, 8, 9, 10, 11};
        static const int strap[]   = {0, 2, 5, 12, 15};
        static const int adc2p[]   = {0, 2, 4, 12, 13, 14, 15, 25, 26, 27};
        BoardProfile b; b.kind = BoardKind::Esp32Wroom; b.maxGpio = 39;
        b.inputOnly = inOnly; b.inputOnlyN = 6;
        b.flashReserved = flash; b.flashReservedN = 6;
        b.strapping = strap; b.strappingN = 5;
        b.adc2 = adc2p; b.adc2N = 10; b.ledcChannels = 16;
        return b;
    }
    static BoardProfile s3() {
        static const int inOnly[]  = {};             // S3 has no input-only in the usual range
        static const int flash[]   = {26, 27, 28, 29, 30, 31, 32};
        static const int strap[]   = {0, 3, 45, 46};
        static const int adc2p[]   = {};
        BoardProfile b; b.kind = BoardKind::Esp32S3; b.maxGpio = 48;
        b.inputOnly = inOnly; b.inputOnlyN = 0;
        b.flashReserved = flash; b.flashReservedN = 7;
        b.strapping = strap; b.strappingN = 4;
        b.adc2 = adc2p; b.adc2N = 0; b.ledcChannels = 8;
        return b;
    }

    bool in(const int* t, size_t n, int pin) const { for (size_t i=0;i<n;++i) if (t[i]==pin) return true; return false; }
    bool isInputOnly(int p) const { return in(inputOnly, inputOnlyN, p); }
    bool isFlash(int p) const { return in(flashReserved, flashReservedN, p); }
    bool isStrapping(int p) const { return in(strapping, strappingN, p); }
    bool isAdc2(int p) const { return in(adc2, adc2N, p); }
};

// A single hardware claim collected from the whole config.
struct PinClaim {
    int  pin = -1;
    bool output = false;    // used as an output
    bool adc = false;       // used as an analog input
    std::string field;
    std::string owner;      // e.g. "instrument 1 / air pump"
};

struct RangeClaim { long lo, hi; std::string field; };
struct BoundClaim { long v, lo, hi; std::string field; };
struct PcaClaim   { uint8_t addr, channel; std::string field; };
struct LedcClaim  { std::string field; };

class HardwareResourceValidator {
public:
    explicit HardwareResourceValidator(BoardProfile b = BoardProfile::wroom())
        : board_(b) {}

    void reset() { pins_.clear(); ranges_.clear(); bounds_.clear(); pcas_.clear(); ledc_.clear(); required_.clear(); unsupported_.clear(); wifiOn_ = true; }
    void setBoard(const BoardProfile& b) { board_ = b; }
    void setWifi(bool on) { wifiOn_ = on; }

    void claimPin(int pin, bool output, bool adc, const std::string& field, const std::string& owner = "") {
        if (pin < 0) return;   // unassigned OPTIONAL pin — not claimed
        pins_.push_back({pin, output, adc, field, owner});
    }
    // Like claimPin, but the pin is MANDATORY for the selected mode: an
    // unassigned (-1) pin is a hard error (review item #24).
    void requirePin(int pin, bool output, bool adc, const std::string& field, const std::string& owner = "") {
        if (pin < 0) { required_.push_back(field); return; }
        pins_.push_back({pin, output, adc, field, owner});
    }
    void claimRange(long lo, long hi, const std::string& field) { ranges_.push_back({lo, hi, field}); }
    // Require lo <= v <= hi, else BOUND_INVALID (e.g. soft limits within travel,
    // a calibration point inside the course) — review #8 §19.
    void claimBound(long v, long lo, long hi, const std::string& field) { bounds_.push_back({v, lo, hi, field}); }
    void claimPca(uint8_t addr, uint8_t ch, const std::string& field) { pcas_.push_back({addr, ch, field}); }
    void claimLedc(const std::string& field) { ledc_.push_back({field}); }
    // Flag a configuration that selects a backend the firmware does not yet
    // drive (PCA9685 outputs, ToF / digital sensors). A config that reaches
    // Ready while selecting one of these would silently move no hardware, so it
    // is a hard error, not a TODO (review #7 §7).
    void markUnsupported(const std::string& field, const std::string& what) {
        unsupported_.push_back({field, what});
    }

    std::vector<ValidationIssue> validate() {
        std::vector<ValidationIssue> out;
        // 1. per-pin checks
        for (const auto& c : pins_) {
            if (c.pin > board_.maxGpio)
                add(out, "GPIO_OUT_OF_RANGE", "GPIO" + s(c.pin) + " does not exist on this board", c.field);
            if (board_.isFlash(c.pin))
                add(out, "GPIO_RESERVED_FLASH", "GPIO" + s(c.pin) + " is reserved for the SPI flash", c.field);
            if (c.output && board_.isInputOnly(c.pin))
                add(out, "GPIO_INPUT_ONLY", "GPIO" + s(c.pin) + " is input-only and cannot drive an output", c.field);
            if (board_.isStrapping(c.pin))
                add(out, "GPIO_STRAPPING", "GPIO" + s(c.pin) + " is a strapping pin; use with care", c.field, Severity::Warning);
            if (c.adc && wifiOn_ && board_.isAdc2(c.pin))
                add(out, "ADC2_WIFI", "GPIO" + s(c.pin) + " is on ADC2 and is unusable while WiFi is active", c.field);
        }
        // 2. cross-instrument GPIO conflicts
        for (size_t i = 0; i < pins_.size(); ++i)
            for (size_t j = i + 1; j < pins_.size(); ++j)
                if (pins_[i].pin == pins_[j].pin)
                    add(out, "GPIO_CONFLICT",
                        "GPIO" + s(pins_[i].pin) + " is used by " + owner(pins_[i]) +
                        " and " + owner(pins_[j]), pins_[j].field);
        // 3. LEDC channel exhaustion
        if (ledc_.size() > board_.ledcChannels)
            add(out, "LEDC_EXHAUSTED",
                "needs " + s((long)ledc_.size()) + " LEDC channels, board has " + s(board_.ledcChannels),
                ledc_.empty() ? "" : ledc_.back().field);
        // 4. PCA9685 channel conflicts (same addr+channel twice)
        for (size_t i = 0; i < pcas_.size(); ++i) {
            if (pcas_[i].channel > 15)
                add(out, "PCA_CHANNEL_RANGE", "PCA9685 channel must be 0..15", pcas_[i].field);
            for (size_t j = i + 1; j < pcas_.size(); ++j)
                if (pcas_[i].addr == pcas_[j].addr && pcas_[i].channel == pcas_[j].channel)
                    add(out, "PCA_CONFLICT",
                        "PCA9685 @0x" + hx(pcas_[i].addr) + " channel " + s(pcas_[i].channel) + " used twice",
                        pcas_[j].field);
        }
        // 5. min/max ranges
        for (const auto& r : ranges_)
            if (r.lo > r.hi)
                add(out, "RANGE_INVALID", "min (" + s(r.lo) + ") exceeds max (" + s(r.hi) + ")", r.field);
        // 5b. value-within-bounds
        for (const auto& b : bounds_)
            if (b.v < b.lo || b.v > b.hi)
                add(out, "BOUND_INVALID", s(b.v) + " is outside [" + s(b.lo) + ".." + s(b.hi) + "]", b.field);
        // 6. mandatory pins left unassigned for the selected mode (#24)
        for (const auto& field : required_)
            add(out, "PIN_REQUIRED", "a pin is required for the selected mode but is unassigned", field);
        // 7. backends the firmware cannot actually drive yet (#7)
        for (const auto& u : unsupported_)
            add(out, "UNSUPPORTED_BACKEND", u.second + " is selected but not implemented in this firmware", u.first);
        return out;
    }

    static bool hasErrors(const std::vector<ValidationIssue>& v) {
        for (const auto& i : v) if (i.severity == Severity::Error) return true;
        return false;
    }

private:
    static std::string s(long v) { return std::to_string(v); }
    static std::string hx(uint8_t v) { char b[8]; std::snprintf(b, sizeof(b), "%02X", v); return b; }
    static std::string owner(const PinClaim& c) { return c.owner.empty() ? c.field : c.owner; }
    void add(std::vector<ValidationIssue>& v, const char* code, const std::string& msg,
             const std::string& field, Severity sev = Severity::Error) {
        v.push_back({code, msg, field, sev});
    }

    BoardProfile board_;
    bool wifiOn_ = true;
    std::vector<PinClaim>  pins_;
    std::vector<RangeClaim> ranges_;
    std::vector<BoundClaim> bounds_;
    std::vector<PcaClaim>   pcas_;
    std::vector<LedcClaim>  ledc_;
    std::vector<std::string> required_;   // fields of mandatory-but-unassigned pins
    std::vector<std::pair<std::string,std::string>> unsupported_;   // {field, what}
};

} // namespace swc

#endif // SWC_CORE_HARDWAREVALIDATOR_H
