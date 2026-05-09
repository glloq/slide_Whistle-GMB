/*
 * Flute.h — un instrument complet : stepper + air + état + LUT
 *
 * Encapsule un slide whistle individuel. Plusieurs instances coexistent
 * sur le même ESP32 via Orchestra (cf. Orchestra.h).
 *
 * Cycle de vie :
 *   - construire avec id + FluteHwConfig
 *   - begin()    → init matériel (depuis taskMotion)
 *   - homing()   → chercher endstop
 *   - handleNoteOn / handleNoteOff / handleCC / handlePitchBend / handleAftertouch
 *   - update()   → polling temps-réel (taskMotion)
 */

#ifndef FLUTE_H
#define FLUTE_H

#include "settings.h"
#include "StepperControl.h"
#include "AirControl.h"

class Flute {
private:
  uint8_t        _id;
  FluteHwConfig  _cfg;
  StepperControl _stepper;
  AirControl     _air;

  // État runtime modifiable
  uint8_t  _midiChannel;
  uint8_t  _noteMin;
  uint8_t  _noteMax;
  bool     _enabled        = true;
  bool     _muted          = false;
  byte     _lastNote       = 0;
  bool     _noteActive     = false;
  unsigned long _lastMidiActMs = 0;

public:
  Flute(uint8_t id, const FluteHwConfig& cfg)
    : _id(id), _cfg(cfg),
      _midiChannel(cfg.midiChannel),
      _noteMin(cfg.noteMin),
      _noteMax(cfg.noteMax) {}

  // ---- Init ---------------------------------------------------------------

  void begin() {
    _stepper.begin(_cfg);
    _air.begin(_cfg);
  }

  bool homing() {
    bool ok = _stepper.performHoming();
    if (ok) _lastMidiActMs = millis();
    return ok;
  }

  // ---- Routage MIDI -------------------------------------------------------
  // Renvoie true si la flûte a consommé le message (canal correspond + plage).

  bool acceptsChannel(uint8_t channel) const {
    if (!_enabled) return false;
    return (_midiChannel == 0) || (_midiChannel == channel);
  }

  bool acceptsNote(uint8_t note) const {
    return note >= _noteMin && note <= _noteMax;
  }

  void handleNoteOn(uint8_t note, uint8_t velocity) {
    if (!_enabled || _muted) return;
    if (!acceptsNote(note)) return;
    _lastNote      = note;
    _noteActive    = true;
    _lastMidiActMs = millis();
    _stepper.moveToMidiNote(note);
    _air.startAir(velocity);
  }

  void handleNoteOff(uint8_t note) {
    if (note != _lastNote || !_noteActive) return;
    _noteActive    = false;
    _lastMidiActMs = millis();
    _air.stopAir();
    _stepper.setVibrato(false, 0);
  }

  void handlePitchBend(float semitones) {
    _lastMidiActMs = millis();
    _stepper.setPitchBend(semitones);
  }

  void handleAftertouch(bool active, uint8_t pressure) {
    _lastMidiActMs = millis();
    _stepper.setVibrato(active, pressure);
  }

  void handleCC(uint8_t cc, uint8_t value) {
    _lastMidiActMs = millis();
    if (cc == 2 || cc == 11) {           // breath / expression
      _air.setAirflow(value);
    } else if (cc == 1) {                // modulation wheel → vibrato
      _stepper.setVibrato(value > 10, value);
    } else if (cc == 7) {                // volume
      _air.setAirflow(value);
    } else if (cc == 123 || cc == 120) { // all notes / sound off
      panic();
    }
  }

  // ---- Boucle temps-réel --------------------------------------------------

  void update() {
    _stepper.update();
    _air.update();
  }

  // Watchdog : appelé périodiquement par Orchestra. Coupe l'air si silence
  // MIDI prolongé alors que l'air est encore ouvert.
  void watchdogTick(unsigned long now, unsigned long timeoutMs) {
    if (now - _lastMidiActMs > timeoutMs && _air.isSolenoidOpen()) {
      Serial.printf("[Flute %u:%s] WDG: timeout MIDI → air coupé\n", _id, _cfg.name);
      panic();
      _lastMidiActMs = now;
    }
  }

  // Coupure air + reset vibrato
  void panic() {
    _air.forceClose();
    _stepper.setVibrato(false, 0);
    _noteActive = false;
  }

  // ---- Accesseurs / setters -----------------------------------------------

  uint8_t id() const { return _id; }
  const char* name() const { return _cfg.name; }
  const FluteHwConfig& config() const { return _cfg; }

  StepperControl& stepper() { return _stepper; }
  AirControl&     air()     { return _air; }

  uint8_t  midiChannel() const { return _midiChannel; }
  void     setMidiChannel(uint8_t c) { _midiChannel = constrain((int)c, 0, 16); }
  uint8_t  noteMin() const { return _noteMin; }
  uint8_t  noteMax() const { return _noteMax; }
  void     setNoteRange(uint8_t lo, uint8_t hi) {
    if (hi > lo) { _noteMin = lo; _noteMax = hi; }
  }

  bool isEnabled() const { return _enabled; }
  void setEnabled(bool en) { _enabled = en; if (!en) panic(); }

  bool isMuted() const { return _muted; }
  void setMuted(bool m) { _muted = m; if (m) panic(); }

  byte lastNote() const     { return _lastNote; }
  bool isNoteActive() const { return _noteActive; }

  unsigned long lastMidiActMs() const { return _lastMidiActMs; }
  void noteActivity(unsigned long t) { _lastMidiActMs = t; }
};

#endif // FLUTE_H
