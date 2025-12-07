/*
 * MIDIHandler.h
 * Gestion des messages MIDI USB
 * Utilise la bibliothèque MIDIUSB pour Arduino
 * Support: Note On/Off, Pitch Bend, Aftertouch (vibrato)
 */

#ifndef MIDI_HANDLER_H
#define MIDI_HANDLER_H

#include <MIDIUSB.h>
#include "settings.h"

// Définition des callbacks
typedef void (*NoteOnCallback)(byte note, byte velocity);
typedef void (*NoteOffCallback)(byte note);
typedef void (*PitchBendCallback)(float semitones);
typedef void (*AftertouchCallback)(bool active, byte pressure);

class MIDIHandler {
private:
  NoteOnCallback onNoteOn;
  NoteOffCallback onNoteOff;
  PitchBendCallback onPitchBend;
  AftertouchCallback onAftertouch;

  byte lastNote;
  bool noteActive;
  int pitchBendValue;       // -8192 à +8191
  byte aftertouchPressure;  // 0-127

public:
  // Constructeur
  MIDIHandler()
    : onNoteOn(nullptr),
      onNoteOff(nullptr),
      onPitchBend(nullptr),
      onAftertouch(nullptr),
      lastNote(0),
      noteActive(false),
      pitchBendValue(0),
      aftertouchPressure(0) {
  }

  // Initialisation
  void begin() {
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

    #if PITCHBEND_ENABLED
    Serial.print(F("MIDIHandler: Pitch bend enabled (+/- "));
    Serial.print(PITCHBEND_RANGE_SEMITONES);
    Serial.println(F(" semitones)"));
    #endif

    #if AFTERTOUCH_ENABLED
    Serial.println(F("MIDIHandler: Aftertouch enabled (vibrato)"));
    #endif
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

  // Définir le callback pour Pitch Bend
  void setPitchBendCallback(PitchBendCallback callback) {
    onPitchBend = callback;
  }

  // Définir le callback pour Aftertouch
  void setAftertouchCallback(AftertouchCallback callback) {
    onAftertouch = callback;
  }

  // Lire et traiter les messages MIDI (à appeler dans loop)
  void update() {
    midiEventPacket_t rx;

    do {
      rx = MidiUSB.read();

      if (rx.header != 0) {
        byte messageType = rx.byte1 & 0xF0; // Type de message
        byte channel = (rx.byte1 & 0x0F) + 1; // Canal (1-16)

        // Filtrer par canal si spécifié
        if (MIDI_CHANNEL != 0 && channel != MIDI_CHANNEL) {
          continue;
        }

        // Traiter les messages
        switch (messageType) {
          case 0x90: // Note On
            {
              byte note = rx.byte2;
              byte velocity = rx.byte3;

              if (velocity > 0) {
                handleNoteOn(note, velocity);
              } else {
                // Velocity 0 = Note Off
                handleNoteOff(note);
              }
            }
            break;

          case 0x80: // Note Off
            handleNoteOff(rx.byte2);
            break;

          case 0xE0: // Pitch Bend
            handlePitchBend(rx.byte2, rx.byte3);
            break;

          case 0xD0: // Channel Pressure (Aftertouch)
            handleAftertouch(rx.byte2);
            break;

          case 0xA0: // Polyphonic Aftertouch
            // Pour cette implémentation, on utilise la pression de la note active
            if (rx.byte2 == lastNote) {
              handleAftertouch(rx.byte3);
            }
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

  // Obtenir la valeur du pitch bend
  int getPitchBendValue() {
    return pitchBendValue;
  }

  // Obtenir la pression aftertouch
  byte getAftertouchPressure() {
    return aftertouchPressure;
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
      aftertouchPressure = 0; // Reset aftertouch

      #if DEBUG_MODE
      Serial.print(F("MIDIHandler: Note OFF - Note: "));
      Serial.println(note);
      #endif

      // Arrêter le vibrato
      #if AFTERTOUCH_ENABLED
      if (onAftertouch != nullptr) {
        onAftertouch(false, 0);
      }
      #endif

      // Appeler le callback
      if (onNoteOff != nullptr) {
        onNoteOff(note);
      }
    }
  }

  // Traiter Pitch Bend
  void handlePitchBend(byte lsb, byte msb) {
    #if PITCHBEND_ENABLED
    // Combiner LSB et MSB (14 bits)
    pitchBendValue = ((int)msb << 7) | lsb;
    pitchBendValue -= 8192; // Centrer à 0 (-8192 à +8191)

    // Convertir en demi-tons
    float semitones = (pitchBendValue / 8192.0) * PITCHBEND_RANGE_SEMITONES;

    #if DEBUG_MODE
    Serial.print(F("MIDIHandler: Pitch Bend = "));
    Serial.print(pitchBendValue);
    Serial.print(F(" ("));
    Serial.print(semitones);
    Serial.println(F(" semitones)"));
    #endif

    // Appeler le callback
    if (onPitchBend != nullptr) {
      onPitchBend(semitones);
    }
    #endif
  }

  // Traiter Aftertouch (Channel Pressure)
  void handleAftertouch(byte pressure) {
    #if AFTERTOUCH_ENABLED
    aftertouchPressure = pressure;

    // Activer le vibrato si pression > seuil
    bool active = (pressure > 10); // Seuil de 10/127

    #if DEBUG_MODE
    Serial.print(F("MIDIHandler: Aftertouch = "));
    Serial.print(pressure);
    Serial.print(F(" -> Vibrato "));
    Serial.println(active ? "ON" : "OFF");
    #endif

    // Appeler le callback
    if (onAftertouch != nullptr) {
      onAftertouch(active, pressure);
    }
    #endif
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
    // CC 1 (Modulation) pourrait aussi contrôler le vibrato
    // CC 7 (Volume) pourrait contrôler l'angle du servo
    // etc.

    // CC 1 (Modulation Wheel) -> Vibrato alternatif
    if (controller == 1 && AFTERTOUCH_ENABLED) {
      bool active = (value > 10);
      if (onAftertouch != nullptr) {
        onAftertouch(active, value);
      }
    }
  }
};

#endif // MIDI_HANDLER_H
