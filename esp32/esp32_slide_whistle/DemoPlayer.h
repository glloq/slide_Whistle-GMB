/*
 * DemoPlayer.h — Lecteur de mélodies de démonstration
 *
 * Permet de tester le système sans source MIDI externe. Une mélodie est une
 * suite de pas {note, durée_ms, velocité}. Le lecteur appelle des callbacks
 * comme un vrai flux MIDI, donc il passe par toute la chaîne (Orchestra,
 * routage canal, etc.).
 *
 * Le lecteur tourne dans taskMotion (non bloquant — état + tick).
 */

#ifndef DEMO_PLAYER_H
#define DEMO_PLAYER_H

#include <Arduino.h>

struct DemoStep {
  uint8_t  note;       // 0 = silence (note off seulement)
  uint16_t durationMs; // durée jusqu'à la note suivante
  uint8_t  velocity;
};

struct DemoMelody {
  const char*     name;
  uint8_t         channel;
  uint8_t         length;
  const DemoStep* steps;
};

// ---- Mélodies prédéfinies -------------------------------------------------

// Frère Jacques (do majeur, octave centrale)
static const DemoStep MELODY_FRERE_JACQUES[] = {
  {60, 500, 90}, {62, 500, 90}, {64, 500, 90}, {60, 500, 90},
  {60, 500, 90}, {62, 500, 90}, {64, 500, 90}, {60, 500, 90},
  {64, 500, 90}, {65, 500, 90}, {67, 1000, 90},
  {64, 500, 90}, {65, 500, 90}, {67, 1000, 90},
  {0,  100, 0}
};

// Gamme chromatique ascendante puis descendante (test de toutes les notes)
static const DemoStep MELODY_CHROMATIC[] = {
  {48,250,80},{49,250,80},{50,250,80},{51,250,80},{52,250,80},{53,250,80},
  {54,250,80},{55,250,80},{56,250,80},{57,250,80},{58,250,80},{59,250,80},
  {60,250,80},{61,250,80},{62,250,80},{63,250,80},{64,250,80},{65,250,80},
  {66,250,80},{67,250,80},{68,250,80},{69,250,80},{70,250,80},{71,250,80},
  {72,250,80},{71,250,80},{70,250,80},{69,250,80},{68,250,80},{67,250,80},
  {66,250,80},{65,250,80},{64,250,80},{63,250,80},{62,250,80},{61,250,80},
  {60,250,80},{0,100,0}
};

// Arpèges Do majeur (test de sauts)
static const DemoStep MELODY_ARPEGGIO[] = {
  {60, 300, 100}, {64, 300, 100}, {67, 300, 100}, {72, 600, 110},
  {67, 300, 100}, {64, 300, 100}, {60, 800, 90},  {0, 100, 0}
};

static const DemoMelody DEMO_MELODIES[] = {
  { "Frère Jacques", 1, sizeof(MELODY_FRERE_JACQUES)/sizeof(DemoStep), MELODY_FRERE_JACQUES },
  { "Chromatique",   1, sizeof(MELODY_CHROMATIC)/sizeof(DemoStep),    MELODY_CHROMATIC    },
  { "Arpège Do",     1, sizeof(MELODY_ARPEGGIO)/sizeof(DemoStep),     MELODY_ARPEGGIO     }
};
static constexpr uint8_t DEMO_MELODY_COUNT = sizeof(DEMO_MELODIES)/sizeof(DemoMelody);

// ---- Player ---------------------------------------------------------------

typedef void (*DemoNoteOn)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*DemoNoteOff)(uint8_t channel, uint8_t note);

class DemoPlayer {
private:
  const DemoMelody* _melody = nullptr;
  uint8_t           _step = 0;
  uint32_t          _stepStart = 0;
  bool              _playing = false;
  bool              _noteOnPending = false;
  uint8_t           _activeNote = 0;
  uint8_t           _channel = 1;
  bool              _loop = false;

  DemoNoteOn  _onNoteOn  = nullptr;
  DemoNoteOff _onNoteOff = nullptr;

  void releaseActive() {
    if (_noteOnPending && _activeNote && _onNoteOff) {
      _onNoteOff(_channel, _activeNote);
    }
    _noteOnPending = false;
    _activeNote = 0;
  }

public:
  void setCallbacks(DemoNoteOn on, DemoNoteOff off) { _onNoteOn = on; _onNoteOff = off; }

  void play(uint8_t melodyIdx, uint8_t channel = 0, bool loop = false) {
    if (melodyIdx >= DEMO_MELODY_COUNT) return;
    releaseActive();
    _melody    = &DEMO_MELODIES[melodyIdx];
    _channel   = channel ? channel : _melody->channel;
    _step      = 0;
    _stepStart = millis();
    _playing   = true;
    _loop      = loop;
    triggerStep();
  }

  void stop() {
    releaseActive();
    _playing = false;
    _melody  = nullptr;
  }

  void update() {
    if (!_playing || !_melody) return;
    uint32_t elapsed = millis() - _stepStart;
    const DemoStep& s = _melody->steps[_step];
    if (elapsed >= s.durationMs) {
      releaseActive();
      _step++;
      if (_step >= _melody->length) {
        if (_loop) { _step = 0; }
        else       { _playing = false; _melody = nullptr; return; }
      }
      _stepStart = millis();
      triggerStep();
    }
  }

  void triggerStep() {
    if (!_melody) return;
    const DemoStep& s = _melody->steps[_step];
    if (s.note > 0 && _onNoteOn) {
      _onNoteOn(_channel, s.note, s.velocity);
      _activeNote    = s.note;
      _noteOnPending = true;
    }
  }

  bool isPlaying()        const { return _playing; }
  uint8_t currentStep()   const { return _step; }
  const char* melodyName() const { return _melody ? _melody->name : ""; }
  uint8_t channel()       const { return _channel; }

  static uint8_t melodyCount() { return DEMO_MELODY_COUNT; }
  static const char* melodyName(uint8_t i) { return i < DEMO_MELODY_COUNT ? DEMO_MELODIES[i].name : ""; }
};

#endif // DEMO_PLAYER_H
