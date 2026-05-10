/*
 * StressTester.h — Burn-in : génère des notes aléatoires sur les flûtes
 *
 * Permet de vérifier la stabilité mécanique/thermique en simulant un usage
 * intensif. Ne bloque pas la tâche temps-réel : l'implémentation est
 * non-bloquante avec un état interne.
 *
 * Usage :
 *   tester.start(30);          // 30 secondes
 *   loop: tester.update();     // appelé depuis taskMotion
 *   tester.isRunning();        // true tant que actif
 *   tester.stop();
 *
 * Stratégie : choisit aléatoirement une flûte enabled, joue une note dans
 * sa plage, durée 200-600 ms, puis enchaîne.
 */

#ifndef STRESS_TESTER_H
#define STRESS_TESTER_H

#include <Arduino.h>
#include "Orchestra.h"
#include "MIDIHandler.h"

class StressTester {
private:
  Orchestra*    _orchestra = nullptr;
  MIDIHandler*  _midi      = nullptr;

  unsigned long _endAt        = 0;   // 0 = inactif, sinon millis() de fin
  unsigned long _nextNoteAt   = 0;   // prochaine programmation de note
  uint8_t       _activeNote   = 0;   // dernière note jouée (0 = aucune)
  uint8_t       _activeChannel = 1;

public:
  void begin(Orchestra* orch, MIDIHandler* midi) {
    _orchestra = orch;
    _midi      = midi;
  }

  void start(uint16_t durationSec) {
    if (!_orchestra || !_midi || durationSec == 0) return;
    _endAt      = millis() + (unsigned long)durationSec * 1000UL;
    _nextNoteAt = millis();
    _activeNote = 0;
    Serial.printf("[Stress] démarrage %us\n", durationSec);
  }

  void stop() {
    releaseActive();
    _endAt = 0;
    Serial.println(F("[Stress] arrêt manuel"));
  }

  bool isRunning() const { return _endAt != 0; }

  // Appel non bloquant — à invoquer dans la boucle taskMotion
  void update() {
    if (_endAt == 0) return;
    unsigned long now = millis();
    if (now > _endAt) {
      releaseActive();
      _endAt = 0;
      Serial.println(F("[Stress] terminé"));
      return;
    }
    if (now < _nextNoteAt) return;

    // Libère la note précédente avant d'en jouer une nouvelle
    releaseActive();

    // Cherche une flûte activée au hasard (max N essais pour éviter une
    // boucle infinie si aucune n'est activée).
    Flute* target = nullptr;
    for (uint8_t tries = 0; tries < _orchestra->count(); ++tries) {
      uint8_t i = esp_random() % _orchestra->count();
      Flute* f = _orchestra->get(i);
      if (f && f->isEnabled()) { target = f; break; }
    }
    if (!target) {
      // Aucune flûte activée — on attend un peu avant de réessayer
      _nextNoteAt = now + 500;
      return;
    }

    uint8_t lo = target->noteMin();
    uint8_t hi = target->noteMax();
    uint8_t note = lo + (esp_random() % (hi - lo + 1));
    uint8_t vel  = 60 + (esp_random() % 60);
    _activeChannel = target->midiChannel() ? target->midiChannel() : 1;
    _midi->triggerNoteOn(_activeChannel, note, vel);
    _activeNote  = note;
    _nextNoteAt  = now + 200 + (esp_random() % 400);    // 200..600 ms
  }

private:
  void releaseActive() {
    if (_activeNote && _midi) {
      _midi->triggerNoteOff(_activeChannel, _activeNote);
    }
    _activeNote = 0;
  }
};

#endif // STRESS_TESTER_H
