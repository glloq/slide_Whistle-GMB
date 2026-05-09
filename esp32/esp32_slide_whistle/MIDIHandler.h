/*
 * MIDIHandler.h — MIDI multi-source, broadcast canal-aware (ESP32)
 *
 * Refactor v3 :
 *   - Plus de filtrage par canal au niveau handler. Le canal MIDI est passé
 *     aux callbacks ; c'est l'Orchestra qui décide quelle flûte consomme.
 *   - Signature unifiée : NoteOn(channel, note, velocity) etc.
 *
 * Sources :
 *   MIDI_SOURCE_SERIAL → DIN-5 via UART2 (GPIO16 RX2)
 *   MIDI_SOURCE_BLE    → BLE MIDI (lathoub/Arduino-BLE-MIDI)
 *
 * Bibliothèques :
 *   MIDI Library  — FortySevenEffects/Arduino-MIDI-Library
 *   BLE-MIDI      — lathoub/Arduino-BLE-MIDI
 */

#ifndef MIDI_HANDLER_H
#define MIDI_HANDLER_H

#include "settings.h"

#if MIDI_SOURCE_SERIAL
  #include <MIDI.h>
  MIDI_CREATE_INSTANCE(HardwareSerial, Serial2, MIDI_SERIAL);
#endif

#if MIDI_SOURCE_BLE
  #include <BLEMidi.h>
#endif

typedef void (*NoteOnCallback)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*NoteOffCallback)(uint8_t channel, uint8_t note);
typedef void (*PitchBendCallback)(uint8_t channel, float semitones);
typedef void (*AftertouchCallback)(uint8_t channel, bool active, uint8_t pressure);
typedef void (*CCCallback)(uint8_t channel, uint8_t cc, uint8_t value);

class MIDIHandler {
private:
  NoteOnCallback     _onNoteOn     = nullptr;
  NoteOffCallback    _onNoteOff    = nullptr;
  PitchBendCallback  _onPitchBend  = nullptr;
  AftertouchCallback _onAftertouch = nullptr;
  CCCallback         _onCC         = nullptr;

  // Statistiques (pour status web)
  uint8_t  _lastChannel = 0;
  uint8_t  _lastNote    = 0;
  int      _lastBend    = 0;
  uint32_t _msgCount    = 0;

  // Ring buffer d'activité MIDI (pour affichage UI)
  static constexpr uint8_t LOG_SIZE = 32;
  struct LogEntry {
    uint32_t  ts;        // millis()
    uint8_t   type;      // 0=NoteOn 1=NoteOff 2=CC 3=PB 4=AT
    uint8_t   channel;
    uint8_t   data1;
    uint8_t   data2;
    int16_t   bend;      // pour PB
  };
  LogEntry _log[LOG_SIZE];
  uint8_t  _logHead = 0;
  uint8_t  _logCount = 0;

  void pushLog(uint8_t type, uint8_t ch, uint8_t d1, uint8_t d2, int16_t bend = 0) {
    _log[_logHead] = { millis(), type, ch, d1, d2, bend };
    _logHead = (_logHead + 1) % LOG_SIZE;
    if (_logCount < LOG_SIZE) _logCount++;
  }

  inline static MIDIHandler* _instance = nullptr;

#if MIDI_SOURCE_SERIAL
  static void _onSerialNoteOn(byte ch, byte note, byte vel) {
    if (!_instance) return;
    if (vel > 0) _instance->dispatchNoteOn(ch, note, vel);
    else         _instance->dispatchNoteOff(ch, note);
  }
  static void _onSerialNoteOff(byte ch, byte note, byte vel) {
    (void)vel;
    if (_instance) _instance->dispatchNoteOff(ch, note);
  }
  static void _onSerialPitchBend(byte ch, int bend) {
    if (_instance) _instance->dispatchPitchBend(ch, bend);
  }
  static void _onSerialAftertouch(byte ch, byte pressure) {
    if (_instance) _instance->dispatchAftertouch(ch, pressure);
  }
  static void _onSerialCC(byte ch, byte num, byte val) {
    if (_instance) _instance->dispatchCC(ch, num, val);
  }
#endif

#if MIDI_SOURCE_BLE
  static void _onBleNoteOn(uint8_t ch, uint8_t note, uint8_t vel, uint16_t) {
    if (!_instance) return;
    if (vel > 0) _instance->dispatchNoteOn(ch, note, vel);
    else         _instance->dispatchNoteOff(ch, note);
  }
  static void _onBleNoteOff(uint8_t ch, uint8_t note, uint8_t, uint16_t) {
    if (_instance) _instance->dispatchNoteOff(ch, note);
  }
  static void _onBlePitchBend(uint8_t ch, int16_t val, uint16_t) {
    if (_instance) _instance->dispatchPitchBend(ch, (int)val);
  }
  static void _onBleAftertouch(uint8_t ch, uint8_t pressure, uint16_t) {
    if (_instance) _instance->dispatchAftertouch(ch, pressure);
  }
  static void _onBleCC(uint8_t ch, uint8_t num, uint8_t val, uint16_t) {
    if (_instance) _instance->dispatchCC(ch, num, val);
  }
#endif

  // ---- Dispatch interne ---------------------------------------------------

  void dispatchNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    _lastChannel = channel;
    _lastNote    = note;
    _msgCount++;
    pushLog(0, channel, note, velocity);
#if DEBUG_MODE
    Serial.printf("[MIDI] ch%u NoteOn %u vel=%u\n", channel, note, velocity);
#endif
    if (_onNoteOn) _onNoteOn(channel, note, velocity);
  }

  void dispatchNoteOff(uint8_t channel, uint8_t note) {
    _lastChannel = channel;
    _msgCount++;
    pushLog(1, channel, note, 0);
#if DEBUG_MODE
    Serial.printf("[MIDI] ch%u NoteOff %u\n", channel, note);
#endif
    if (_onNoteOff) _onNoteOff(channel, note);
  }

  void dispatchPitchBend(uint8_t channel, int raw) {
    _lastChannel = channel;
    _lastBend    = raw;
    _msgCount++;
    pushLog(3, channel, 0, 0, (int16_t)raw);
    float semitones = (raw / 8192.0f) * PITCHBEND_RANGE_SEMITONES;
    if (_onPitchBend) _onPitchBend(channel, semitones);
  }

  void dispatchAftertouch(uint8_t channel, uint8_t pressure) {
    _lastChannel = channel;
    _msgCount++;
    pushLog(4, channel, pressure, 0);
    bool active = pressure > 10;
    if (_onAftertouch) _onAftertouch(channel, active, pressure);
  }

  void dispatchCC(uint8_t channel, uint8_t number, uint8_t value) {
    _lastChannel = channel;
    _msgCount++;
    pushLog(2, channel, number, value);
#if DEBUG_MODE
    Serial.printf("[MIDI] ch%u CC%u=%u\n", channel, number, value);
#endif
    if (_onCC) _onCC(channel, number, value);
    // CC1 (modulation wheel) → traduit en aftertouch pour vibrato
    if (number == 1 && _onAftertouch) {
      _onAftertouch(channel, value > 10, value);
    }
  }

public:
  void begin() {
    _instance = this;

#if MIDI_SOURCE_SERIAL
    Serial2.begin(MIDI_SERIAL_BAUD, SERIAL_8N1, 16, 17);
    MIDI_SERIAL.begin(MIDI_CHANNEL_OMNI);
    MIDI_SERIAL.setHandleNoteOn(_onSerialNoteOn);
    MIDI_SERIAL.setHandleNoteOff(_onSerialNoteOff);
    MIDI_SERIAL.setHandlePitchBend(_onSerialPitchBend);
    MIDI_SERIAL.setHandleAfterTouchChannel(_onSerialAftertouch);
    MIDI_SERIAL.setHandleControlChange(_onSerialCC);
    Serial.println(F("[MIDI] Serial MIDI init (RX2=GPIO16)"));
#endif

#if MIDI_SOURCE_BLE
    BLEMidiServer.begin("SlideWhistle");
    BLEMidiServer.setOnConnectCallback([]() {
      Serial.println(F("[MIDI] BLE connecté"));
    });
    BLEMidiServer.setOnDisconnectCallback([]() {
      Serial.println(F("[MIDI] BLE déconnecté"));
    });
    BLEMidiServer.setNoteOnCallback(_onBleNoteOn);
    BLEMidiServer.setNoteOffCallback(_onBleNoteOff);
    BLEMidiServer.setPitchBendCallback(_onBlePitchBend);
    BLEMidiServer.setAfterTouchCallback(_onBleAftertouch);
    BLEMidiServer.setControlChangeCallback(_onBleCC);
    Serial.println(F("[MIDI] BLE MIDI init"));
#endif
  }

  void update() {
#if MIDI_SOURCE_SERIAL
    MIDI_SERIAL.read();
#endif
  }

  // ---- Déclenchement direct depuis le web (passe par le même chemin) ------

  void triggerNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    dispatchNoteOn(channel, note, velocity);
  }
  void triggerNoteOff(uint8_t channel, uint8_t note) {
    dispatchNoteOff(channel, note);
  }
  void triggerCC(uint8_t channel, uint8_t cc, uint8_t value) {
    dispatchCC(channel, cc, value);
  }

  // ---- Enregistrement callbacks ------------------------------------------

  void setNoteOnCallback(NoteOnCallback cb)         { _onNoteOn     = cb; }
  void setNoteOffCallback(NoteOffCallback cb)       { _onNoteOff    = cb; }
  void setPitchBendCallback(PitchBendCallback cb)   { _onPitchBend  = cb; }
  void setAftertouchCallback(AftertouchCallback cb) { _onAftertouch = cb; }
  void setCCCallback(CCCallback cb)                 { _onCC         = cb; }

  // ---- Getters -----------------------------------------------------------

  uint8_t  getLastChannel() const { return _lastChannel; }
  uint8_t  getLastNote()    const { return _lastNote; }
  int      getPitchBendValue() const { return _lastBend; }
  uint32_t getMessageCount() const { return _msgCount; }

  // ---- Activity log -------------------------------------------------------
  // Itère du plus ancien au plus récent, appelle cb(entry) pour chaque
  template <typename Fn>
  void forEachLog(Fn cb) const {
    uint8_t start = (_logCount < LOG_SIZE)
                  ? 0
                  : _logHead;  // wrap : oldest = head
    for (uint8_t i = 0; i < _logCount; ++i) {
      uint8_t idx = (start + i) % LOG_SIZE;
      cb(_log[idx]);
    }
  }
};

#endif // MIDI_HANDLER_H
