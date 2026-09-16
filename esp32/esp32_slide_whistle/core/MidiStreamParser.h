/*
 * core/MidiStreamParser.h — portable byte-wise MIDI 1.0 stream parser.
 *
 * Turns a raw serial byte stream (DIN, and any other byte-oriented MIDI
 * transport) into structured events, with no Arduino dependency and no external
 * library, so it is unit-tested natively.
 *
 * It keeps the two planes strictly apart, which is the whole point:
 *
 *   channel voice messages ──► IMidiStreamSink::noteOn/noteOff/cc/bend/pressure
 *   SysEx (F0 … F7)        ──► IMidiStreamSink::sysEx  (a COMPLETE frame)
 *
 * The caller wires the first to MidiRouter and the second to the GMB bridge; a
 * bug on one side can never reach the other, because this parser hands SysEx
 * bytes to a different callback and never synthesises a channel message from
 * them.
 *
 * Handled correctly because a real MIDI bus needs it:
 *   - running status (and its cancellation by System Common);
 *   - real-time bytes (F8..FF) interleaved ANYWHERE, including inside a SysEx;
 *   - a SysEx longer than the buffer — the frame is dropped, not truncated into
 *     a plausible-looking short one, and the parser resynchronises on F7;
 *   - a new status byte arriving mid-SysEx (an unterminated frame) — abandoned.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_MIDISTREAMPARSER_H
#define SWC_CORE_MIDISTREAMPARSER_H

#include "Types.h"

namespace swc {

class IMidiStreamSink {
public:
    virtual ~IMidiStreamSink() = default;
    virtual void midiNoteOn(uint8_t channel1, uint8_t note, uint8_t velocity) = 0;
    virtual void midiNoteOff(uint8_t channel1, uint8_t note) = 0;
    virtual void midiControlChange(uint8_t channel1, uint8_t cc, uint8_t value) = 0;
    virtual void midiPitchBend(uint8_t channel1, int16_t bend14) = 0;   // -8192..8191
    virtual void midiChannelPressure(uint8_t channel1, uint8_t value) = 0;
    // One COMPLETE SysEx frame, F0 … F7 included.
    virtual void midiSysEx(const uint8_t* data, size_t len) = 0;
};

// `SysExCapacity` bounds what one frame may cost. GMB requests are 6 or 8 bytes;
// 64 leaves room for another vendor's short message without ever letting a
// malformed stream grow memory.
template <size_t SysExCapacity = 64>
class MidiStreamParser {
public:
    void begin(IMidiStreamSink* sink) { sink_ = sink; reset(); }

    void reset() {
        status_ = 0; dataCount_ = 0; expected_ = 0;
        inSysEx_ = false; sysExLen_ = 0; sysExOverflow_ = false;
    }

    void feed(const uint8_t* bytes, size_t len) { for (size_t i = 0; i < len; ++i) feed(bytes[i]); }

    void feed(uint8_t b) {
        // System real-time: valid anywhere, even between the bytes of another
        // message and inside a SysEx. Never disturbs the running state.
        if (b >= 0xF8) return;

        if (b & 0x80) {                       // a status byte
            if (b == 0xF0) {                  // SysEx start
                inSysEx_ = true; sysExLen_ = 0; sysExOverflow_ = false;
                pushSysEx(b);
                status_ = 0;                  // System Common cancels running status
                dataCount_ = 0;
                return;
            }
            if (b == 0xF7) {                  // SysEx end
                if (inSysEx_) {
                    pushSysEx(b);
                    if (!sysExOverflow_ && sink_) sink_->midiSysEx(sysEx_, sysExLen_);
                    inSysEx_ = false; sysExLen_ = 0; sysExOverflow_ = false;
                }
                return;
            }
            // Any other status byte abandons an unterminated SysEx.
            inSysEx_ = false; sysExLen_ = 0; sysExOverflow_ = false;
            if (b < 0xF0) {                   // channel voice / mode
                status_ = b;
                dataCount_ = 0;
                expected_ = expectedDataBytes(b);
            } else {                          // System Common: cancels running status
                status_ = 0;
                dataCount_ = 0;
            }
            return;
        }

        // A data byte.
        if (inSysEx_) { pushSysEx(b); return; }
        if (status_ == 0) return;             // data with no status: ignored
        data_[dataCount_++] = b;
        if (dataCount_ < expected_) return;
        dispatch();
        dataCount_ = 0;                       // running status: keep `status_`
    }

private:
    static uint8_t expectedDataBytes(uint8_t status) {
        const uint8_t type = uint8_t(status & 0xF0);
        // Program Change (0xC0) and Channel Pressure (0xD0) carry one data byte;
        // everything else in the channel-voice range carries two.
        return (type == 0xC0 || type == 0xD0) ? 1 : 2;
    }

    void pushSysEx(uint8_t b) {
        if (sysExLen_ < SysExCapacity) sysEx_[sysExLen_++] = b;
        else sysExOverflow_ = true;           // too long for us: drop the whole frame
    }

    void dispatch() {
        if (!sink_) return;
        const uint8_t type = uint8_t(status_ & 0xF0);
        const uint8_t ch1  = uint8_t((status_ & 0x0F) + 1);   // 1..16, the repo's convention
        switch (type) {
            case 0x80: sink_->midiNoteOff(ch1, data_[0]); break;
            case 0x90:
                if (data_[1] == 0) sink_->midiNoteOff(ch1, data_[0]);
                else               sink_->midiNoteOn(ch1, data_[0], data_[1]);
                break;
            case 0xB0: sink_->midiControlChange(ch1, data_[0], data_[1]); break;
            case 0xD0: sink_->midiChannelPressure(ch1, data_[0]); break;
            case 0xE0: {
                const int raw = int(data_[0] & 0x7F) | (int(data_[1] & 0x7F) << 7);
                sink_->midiPitchBend(ch1, int16_t(raw - 8192));
                break;
            }
            default: break;   // Poly aftertouch (0xA0) and Program Change (0xC0): not used
        }
    }

    IMidiStreamSink* sink_ = nullptr;
    uint8_t status_ = 0;
    uint8_t data_[2] = {0, 0};
    uint8_t dataCount_ = 0, expected_ = 0;
    bool    inSysEx_ = false, sysExOverflow_ = false;
    uint8_t sysEx_[SysExCapacity] = {0};
    size_t  sysExLen_ = 0;
};

} // namespace swc

#endif // SWC_CORE_MIDISTREAMPARSER_H
