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

  // Nom personnalisé (vide = utiliser cfg.name)
  char     _customName[24] = {0};

  // Mapping CC configurable. 0 = désactivé. Défauts musicalement standards :
  //   CC1  = mod wheel  → vibrato
  //   CC2  = breath     → débit air
  //   CC11 = expression → débit air
  //   CC7  = volume     → débit air
  //   CC64 = sustain    → maintien (TODO)
  uint8_t  _ccBreath  = 2;     // contrôle débit principal
  uint8_t  _ccExpr    = 11;    // expression (additif)
  uint8_t  _ccVolume  = 7;     // volume
  uint8_t  _ccVibrato = 1;     // mod wheel
  uint8_t  _ccSustain = 64;    // pédale sustain (placeholder)

  // Transpose : ajouté à chaque note entrante. Borne automatiquement à
  // [_noteMin, _noteMax]. Permet par exemple de jouer ch1 en C major mais
  // que la flûte sonne une octave plus haut.
  int8_t   _transpose = 0;     // demi-tons (-24 à +24 typiquement)

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

  // Applique transpose puis clamp à la plage. Renvoie 0xFF si hors plage
  // (et donc la note doit être ignorée).
  uint8_t applyTranspose(uint8_t note) const {
    int t = (int)note + (int)_transpose;
    if (t < _noteMin || t > _noteMax) return 0xFF;
    return (uint8_t)t;
  }

  void handleNoteOn(uint8_t note, uint8_t velocity) {
    if (!_enabled || _muted) return;
    uint8_t effective = applyTranspose(note);
    if (effective == 0xFF) return;             // hors plage après transpose
    _lastNote      = effective;
    _noteActive    = true;
    _lastMidiActMs = millis();
    _stepper.moveToMidiNote(effective);
    _air.startAir(velocity);
  }

  void handleNoteOff(uint8_t note) {
    uint8_t effective = applyTranspose(note);
    if (effective == 0xFF) return;
    if (effective != _lastNote || !_noteActive) return;
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
    if (cc == 0) return;
    if (cc == _ccBreath || cc == _ccExpr || cc == _ccVolume) {
      _air.setAirflow(value);                        // débit air
    } else if (cc == _ccVibrato) {
      _stepper.setVibrato(value > 10, value);        // vibrato
    } else if (cc == 123 || cc == 120) {             // all notes / sound off
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
  const char* name() const {
    return _customName[0] ? _customName : _cfg.name;
  }
  const char* customName()      const { return _customName; }
  void setCustomName(const char* n) {
    if (!n) { _customName[0] = 0; return; }
    strncpy(_customName, n, sizeof(_customName) - 1);
    _customName[sizeof(_customName) - 1] = 0;
  }
  const FluteHwConfig& config() const { return _cfg; }

  // CC mapping
  uint8_t getCcBreath()  const { return _ccBreath;  }
  uint8_t getCcExpr()    const { return _ccExpr;    }
  uint8_t getCcVolume()  const { return _ccVolume;  }
  uint8_t getCcVibrato() const { return _ccVibrato; }
  uint8_t getCcSustain() const { return _ccSustain; }
  void    setCcBreath(uint8_t c)  { _ccBreath  = c; }
  void    setCcExpr(uint8_t c)    { _ccExpr    = c; }
  void    setCcVolume(uint8_t c)  { _ccVolume  = c; }
  void    setCcVibrato(uint8_t c) { _ccVibrato = c; }
  void    setCcSustain(uint8_t c) { _ccSustain = c; }
  int8_t  getTranspose() const { return _transpose; }
  void    setTranspose(int8_t t) {
    _transpose = constrain((int)t, -36, 36);
    if (_noteActive) panic();              // évite des notes coincées
  }

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
