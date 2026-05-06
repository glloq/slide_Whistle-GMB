/*
 * MIDIHandler.h — MIDI multi-source (ESP32)
 *
 * Corrections vs v1 :
 *   - _instance déclaré comme 'inline static' (C++17) → une seule définition,
 *     quel que soit le nombre d'unités de compilation (fix ODR + BLE guard)
 *   - La déclaration n'est plus protégée par #if MIDI_SOURCE_SERIAL :
 *     les callbacks BLE en ont aussi besoin
 *   - La définition externe au bas du fichier est supprimée (inline static suffit)
 *
 * Sources :
 *   MIDI_SOURCE_SERIAL → DIN-5 via UART2 (GPIO16 RX2)
 *   MIDI_SOURCE_BLE    → BLE MIDI (bibliothèque lathoub/Arduino-BLE-MIDI)
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

typedef void (*NoteOnCallback)(byte note, byte velocity);
typedef void (*NoteOffCallback)(byte note);
typedef void (*PitchBendCallback)(float semitones);
typedef void (*AftertouchCallback)(bool active, byte pressure);
typedef void (*CCCallback)(byte cc, byte value);

class MIDIHandler {
private:
  NoteOnCallback     _onNoteOn     = nullptr;
  NoteOffCallback    _onNoteOff    = nullptr;
  PitchBendCallback  _onPitchBend  = nullptr;
  AftertouchCallback _onAftertouch = nullptr;
  CCCallback         _onCC         = nullptr;

  byte lastNote       = 0;
  bool noteActive     = false;
  int  pitchBendValue = 0;
  byte aftertouchPres = 0;

  // Singleton pour les callbacks statiques (Serial + BLE)
  // 'inline static' = défini ici, une seule instance (C++17)
  inline static MIDIHandler* _instance = nullptr;

  // ---- Callbacks statiques Serial MIDI ------------------------------------
#if MIDI_SOURCE_SERIAL
  static void _onSerialNoteOn(byte ch, byte note, byte vel) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    if (vel > 0) _instance->dispatchNoteOn(note, vel);
    else         _instance->dispatchNoteOff(note);
  }
  static void _onSerialNoteOff(byte ch, byte note, byte vel) {
    (void)vel;
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchNoteOff(note);
  }
  static void _onSerialPitchBend(byte ch, int bend) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchPitchBend(bend);
  }
  static void _onSerialAftertouch(byte ch, byte pressure) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchAftertouch(pressure);
  }
  static void _onSerialCC(byte ch, byte num, byte val) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchCC(num, val);
  }
#endif // MIDI_SOURCE_SERIAL

  // ---- Callbacks statiques BLE MIDI ---------------------------------------
#if MIDI_SOURCE_BLE
  static void _onBleNoteOn(uint8_t ch, uint8_t note, uint8_t vel, uint16_t) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    if (vel > 0) _instance->dispatchNoteOn(note, vel);
    else         _instance->dispatchNoteOff(note);
  }
  static void _onBleNoteOff(uint8_t ch, uint8_t note, uint8_t, uint16_t) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchNoteOff(note);
  }
  static void _onBlePitchBend(uint8_t ch, int16_t val, uint16_t) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchPitchBend((int)val);
  }
  static void _onBleAftertouch(uint8_t ch, uint8_t pressure, uint16_t) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchAftertouch(pressure);
  }
  static void _onBleCC(uint8_t ch, uint8_t num, uint8_t val, uint16_t) {
    if (!_instance || (MIDI_CHANNEL && ch != MIDI_CHANNEL)) return;
    _instance->dispatchCC(num, val);
  }
#endif // MIDI_SOURCE_BLE

  // ---- Dispatch interne ---------------------------------------------------

  void dispatchNoteOn(byte note, byte velocity) {
    if (note < MIDI_NOTE_MIN || note > MIDI_NOTE_MAX) return;
    lastNote   = note;
    noteActive = true;
#if DEBUG_MODE
    Serial.printf("[MIDI] NoteOn  %d vel=%d\n", note, velocity);
#endif
    if (_onNoteOn) _onNoteOn(note, velocity);
  }

  void dispatchNoteOff(byte note) {
    if (note != lastNote || !noteActive) return;
    noteActive    = false;
    aftertouchPres = 0;
#if DEBUG_MODE
    Serial.printf("[MIDI] NoteOff %d\n", note);
#endif
#if AFTERTOUCH_ENABLED
    if (_onAftertouch) _onAftertouch(false, 0);
#endif
    if (_onNoteOff) _onNoteOff(note);
  }

  void dispatchPitchBend(int raw) {
#if PITCHBEND_ENABLED
    pitchBendValue = raw;
    float semitones = (raw / 8192.0f) * PITCHBEND_RANGE_SEMITONES;
#if DEBUG_MODE
    Serial.printf("[MIDI] PitchBend %d → %.2f st\n", raw, semitones);
#endif
    if (_onPitchBend) _onPitchBend(semitones);
#endif
  }

  void dispatchAftertouch(byte pressure) {
#if AFTERTOUCH_ENABLED
    aftertouchPres = pressure;
    bool active    = pressure > 10;
#if DEBUG_MODE
    Serial.printf("[MIDI] Aftertouch %d → vibrato %s\n", pressure, active ? "ON" : "OFF");
#endif
    if (_onAftertouch) _onAftertouch(active, pressure);
#endif
  }

  void dispatchCC(byte number, byte value) {
#if DEBUG_MODE
    Serial.printf("[MIDI] CC %d = %d\n", number, value);
#endif
    if (_onCC) _onCC(number, value);
    // CC1 modulation wheel → vibrato alternatif
    if (number == 1 && AFTERTOUCH_ENABLED) {
      dispatchAftertouch(value);
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
    // BLE MIDI : event-driven, pas de polling
  }

  // Déclenchement direct depuis l'interface web (même chemin callbacks)
  void triggerNoteOn(byte note, byte velocity) { dispatchNoteOn(note, velocity); }
  void triggerNoteOff(byte note)               { dispatchNoteOff(note); }
  void triggerCC(byte number, byte value)      { dispatchCC(number, value); }

  // Enregistrement callbacks
  void setNoteOnCallback(NoteOnCallback cb)        { _onNoteOn     = cb; }
  void setNoteOffCallback(NoteOffCallback cb)      { _onNoteOff    = cb; }
  void setPitchBendCallback(PitchBendCallback cb)  { _onPitchBend  = cb; }
  void setAftertouchCallback(AftertouchCallback cb){ _onAftertouch  = cb; }
  void setCCCallback(CCCallback cb)                { _onCC         = cb; }

  // Getters
  byte getLastNote()           const { return lastNote; }
  bool isNoteActive()          const { return noteActive; }
  int  getPitchBendValue()     const { return pitchBendValue; }
  byte getAftertouchPressure() const { return aftertouchPres; }
};

#endif // MIDI_HANDLER_H
