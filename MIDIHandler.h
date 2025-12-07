/*
 * MIDIHandler.h
 * Gestion des messages MIDI USB
 * Utilise la bibliothèque MIDIUSB pour Arduino
 */

#ifndef MIDI_HANDLER_H
#define MIDI_HANDLER_H

#include <MIDIUSB.h>
#include "settings.h"

// Définition des callbacks
typedef void (*NoteOnCallback)(byte note, byte velocity);
typedef void (*NoteOffCallback)(byte note);

class MIDIHandler {
private:
  NoteOnCallback onNoteOn;
  NoteOffCallback onNoteOff;
  byte lastNote;
  bool noteActive;

public:
  // Constructeur
  MIDIHandler()
    : onNoteOn(nullptr),
      onNoteOff(nullptr),
      lastNote(0),
      noteActive(false) {
  }

  // Initialisation
  void begin() {
    // MIDIUSB ne nécessite pas d'initialisation particulière

    #if DEBUG_MODE
    Serial.println(F("MIDIHandler: Initialized (USB MIDI)"));
    Serial.print(F("MIDIHandler: Listening on channel "));
    if (MIDI_CHANNEL == 0) {
      Serial.println(F("ALL"));
    } else {
      Serial.println(MIDI_CHANNEL);
    }
    Serial.print(F("MIDIHandler: Note range "));
    Serial.print(MIDI_NOTE_MIN);
    Serial.print(F(" - "));
    Serial.println(MIDI_NOTE_MAX);
    #endif
  }

  // Définir le callback pour Note On
  void setNoteOnCallback(NoteOnCallback callback) {
    onNoteOn = callback;
  }

  // Définir le callback pour Note Off
  void setNoteOffCallback(NoteOffCallback callback) {
    onNoteOff = callback;
  }

  // Lire et traiter les messages MIDI (à appeler dans loop)
  void update() {
    midiEventPacket_t rx;

    do {
      rx = MidiUSB.read();

      if (rx.header != 0) {
        byte messageType = rx.byte1 & 0xF0; // Type de message
        byte channel = (rx.byte1 & 0x0F) + 1; // Canal (1-16)
        byte note = rx.byte2;
        byte velocity = rx.byte3;

        // Filtrer par canal si spécifié
        if (MIDI_CHANNEL != 0 && channel != MIDI_CHANNEL) {
          continue;
        }

        // Traiter les messages
        switch (messageType) {
          case 0x90: // Note On
            if (velocity > 0) {
              handleNoteOn(note, velocity);
            } else {
              // Velocity 0 = Note Off
              handleNoteOff(note);
            }
            break;

          case 0x80: // Note Off
            handleNoteOff(note);
            break;

          case 0xB0: // Control Change
            handleControlChange(rx.byte2, rx.byte3);
            break;

          #if DEBUG_MODE
          default:
            Serial.print(F("MIDIHandler: Unknown message type 0x"));
            Serial.println(messageType, HEX);
            break;
          #endif
        }
      }
    } while (rx.header != 0);
  }

  // Obtenir la dernière note jouée
  byte getLastNote() {
    return lastNote;
  }

  // Vérifier si une note est active
  bool isNoteActive() {
    return noteActive;
  }

private:
  // Traiter Note On
  void handleNoteOn(byte note, byte velocity) {
    // Filtrer les notes hors de la plage
    if (note < MIDI_NOTE_MIN || note > MIDI_NOTE_MAX) {
      #if DEBUG_MODE
      Serial.print(F("MIDIHandler: Note "));
      Serial.print(note);
      Serial.println(F(" out of range, ignoring"));
      #endif
      return;
    }

    lastNote = note;
    noteActive = true;

    #if DEBUG_MODE
    Serial.print(F("MIDIHandler: Note ON - Note: "));
    Serial.print(note);
    Serial.print(F(", Velocity: "));
    Serial.println(velocity);
    #endif

    // Appeler le callback
    if (onNoteOn != nullptr) {
      onNoteOn(note, velocity);
    }
  }

  // Traiter Note Off
  void handleNoteOff(byte note) {
    // Ne traiter que si c'est la note active
    if (note == lastNote && noteActive) {
      noteActive = false;

      #if DEBUG_MODE
      Serial.print(F("MIDIHandler: Note OFF - Note: "));
      Serial.println(note);
      #endif

      // Appeler le callback
      if (onNoteOff != nullptr) {
        onNoteOff(note);
      }
    }
  }

  // Traiter Control Change (optionnel, pour extensions futures)
  void handleControlChange(byte controller, byte value) {
    #if DEBUG_MODE
    Serial.print(F("MIDIHandler: CC "));
    Serial.print(controller);
    Serial.print(F(" = "));
    Serial.println(value);
    #endif

    // Exemples d'utilisation :
    // CC 1 (Modulation) pourrait contrôler la vitesse du ventilateur
    // CC 7 (Volume) pourrait contrôler l'angle du servo
    // etc.
  }
};

#endif // MIDI_HANDLER_H
