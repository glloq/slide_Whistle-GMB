/*
 * Orchestra.h — Routeur MIDI multi-flûtes
 *
 * Reçoit les messages MIDI du MIDIHandler avec le canal d'origine,
 * et les dispatche aux flûtes qui acceptent ce canal.
 *
 * Stratégie de routage :
 *   - canal 0 (OMNI) sur la flûte → reçoit tous les canaux MIDI
 *   - canal N (1..16) sur la flûte → ne reçoit que les messages du canal N
 *   - une flûte est ignorée si !enabled
 *
 * Chaque flûte filtre ensuite par sa plage [noteMin, noteMax].
 */

#ifndef ORCHESTRA_H
#define ORCHESTRA_H

#include "settings.h"
#include "Flute.h"

class Orchestra {
private:
  Flute*  _flutes[MAX_FLUTES];
  uint8_t _count = 0;

public:
  Orchestra() {
    for (uint8_t i = 0; i < MAX_FLUTES; ++i) _flutes[i] = nullptr;
  }

  ~Orchestra() {
    for (uint8_t i = 0; i < MAX_FLUTES; ++i) delete _flutes[i];
  }

  // ---- Construction --------------------------------------------------------

  bool addFlute(const FluteHwConfig& cfg) {
    if (_count >= MAX_FLUTES) return false;
    _flutes[_count] = new Flute(_count, cfg);
    ++_count;
    return true;
  }

  void beginAll() {
    for (uint8_t i = 0; i < _count; ++i) _flutes[i]->begin();
  }

  // Homing séquentiel — évite les pics de courant simultanés
  bool homingAll() {
    bool allOk = true;
    for (uint8_t i = 0; i < _count; ++i) {
      if (!_flutes[i]->isEnabled()) continue;
#if DEBUG_MODE
      Serial.printf("[Orchestra] homing flûte %u/%u\n", i + 1, _count);
#endif
      if (!_flutes[i]->homing()) allOk = false;
    }
    return allOk;
  }

  // ---- Dispatch MIDI -------------------------------------------------------

  void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    for (uint8_t i = 0; i < _count; ++i) {
      Flute* f = _flutes[i];
      if (f && f->acceptsChannel(channel)) f->handleNoteOn(note, velocity);
    }
  }

  void onNoteOff(uint8_t channel, uint8_t note) {
    for (uint8_t i = 0; i < _count; ++i) {
      Flute* f = _flutes[i];
      if (f && f->acceptsChannel(channel)) f->handleNoteOff(note);
    }
  }

  void onPitchBend(uint8_t channel, float semitones) {
    for (uint8_t i = 0; i < _count; ++i) {
      Flute* f = _flutes[i];
      if (f && f->acceptsChannel(channel)) f->handlePitchBend(semitones);
    }
  }

  void onAftertouch(uint8_t channel, bool active, uint8_t pressure) {
    for (uint8_t i = 0; i < _count; ++i) {
      Flute* f = _flutes[i];
      if (f && f->acceptsChannel(channel)) f->handleAftertouch(active, pressure);
    }
  }

  void onCC(uint8_t channel, uint8_t cc, uint8_t value) {
    for (uint8_t i = 0; i < _count; ++i) {
      Flute* f = _flutes[i];
      if (f && f->acceptsChannel(channel)) f->handleCC(cc, value);
    }
  }

  // ---- Update temps-réel ---------------------------------------------------

  void updateAll() {
    for (uint8_t i = 0; i < _count; ++i) {
      if (_flutes[i]) _flutes[i]->update();
    }
  }

  void watchdogTickAll(unsigned long timeoutMs) {
    unsigned long now = millis();
    for (uint8_t i = 0; i < _count; ++i) {
      if (_flutes[i]) _flutes[i]->watchdogTick(now, timeoutMs);
    }
  }

  void panicAll() {
    for (uint8_t i = 0; i < _count; ++i) {
      if (_flutes[i]) _flutes[i]->panic();
    }
  }

  // ---- Accès ---------------------------------------------------------------

  uint8_t count() const { return _count; }

  Flute* get(uint8_t id) {
    return id < _count ? _flutes[id] : nullptr;
  }

  bool anyAirOpen() const {
    for (uint8_t i = 0; i < _count; ++i) {
      if (_flutes[i] && _flutes[i]->air().isSolenoidOpen()) return true;
    }
    return false;
  }
};

#endif // ORCHESTRA_H
