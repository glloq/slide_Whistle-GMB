/*
 * StepperControl.h
 * Gestion du moteur pas à pas pour le slider du pipeau
 * Utilise la bibliothèque AccelStepper pour un contrôle précis
 */

#ifndef STEPPER_CONTROL_H
#define STEPPER_CONTROL_H

#include <AccelStepper.h>
#include "settings.h"

class StepperControl {
private:
  AccelStepper stepper;
  bool isHomed;
  long currentPosition;
  long targetPosition;

public:
  // Constructeur
  StepperControl()
    : stepper(AccelStepper::DRIVER, STEPPER_STEP_PIN, STEPPER_DIR_PIN),
      isHomed(false),
      currentPosition(0),
      targetPosition(0) {
  }

  // Initialisation
  void begin() {
    // Configuration des pins
    pinMode(STEPPER_STEP_PIN, OUTPUT);
    pinMode(STEPPER_DIR_PIN, OUTPUT);

    #ifdef STEPPER_ENABLE_PIN
    pinMode(STEPPER_ENABLE_PIN, OUTPUT);
    enableMotor(true);
    #endif

    pinMode(ENDSTOP_PIN, INPUT_PULLUP);

    // Configuration du moteur
    stepper.setMaxSpeed(STEPPER_SPEED * MICROSTEPS);
    stepper.setAcceleration(STEPPER_ACCELERATION * MICROSTEPS);
    stepper.setCurrentPosition(0);

    #if DEBUG_MODE
    Serial.println(F("StepperControl: Initialized"));
    #endif
  }

  // Procédure de homing
  bool performHoming() {
    #if DEBUG_MODE
    Serial.println(F("StepperControl: Starting homing sequence..."));
    #endif

    unsigned long startTime = millis();

    // Phase 1: Avancer lentement vers le capteur
    stepper.setSpeed(-HOMING_SPEED * MICROSTEPS);

    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();

      // Timeout de sécurité
      if (millis() - startTime > MAX_HOMING_TIME) {
        #if DEBUG_MODE
        Serial.println(F("StepperControl: Homing timeout!"));
        #endif
        return false;
      }
    }

    #if DEBUG_MODE
    Serial.println(F("StepperControl: Endstop detected"));
    #endif

    // Phase 2: Reculer légèrement
    stepper.move(HOMING_BACKOFF_STEPS * MICROSTEPS);
    while (stepper.distanceToGo() != 0) {
      stepper.run();
    }

    // Phase 3: Approche lente finale
    stepper.setSpeed(-HOMING_SPEED * MICROSTEPS / 4);
    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();
    }

    // Définir la position zéro avec offset
    stepper.setCurrentPosition(-HOME_OFFSET_STEPS * MICROSTEPS);
    stepper.moveTo(0);
    while (stepper.distanceToGo() != 0) {
      stepper.run();
    }

    isHomed = true;

    #if DEBUG_MODE
    Serial.println(F("StepperControl: Homing complete, moving to center"));
    #endif

    // Aller à la position centrale
    moveToCenter();

    return true;
  }

  // Déplacer vers la position centrale
  void moveToCenter() {
    long centerPos = (TOTAL_TRAVEL_STEPS * MICROSTEPS) / 2;
    moveTo(centerPos);
  }

  // Déplacer vers une position absolue (en pas)
  void moveTo(long position) {
    #if ENABLE_SOFT_LIMITS
    // Limiter la position dans la plage valide
    position = constrain(position, 0, TOTAL_TRAVEL_STEPS * MICROSTEPS);
    #endif

    targetPosition = position;
    stepper.moveTo(position);

    #if DEBUG_MODE
    Serial.print(F("StepperControl: Moving to position "));
    Serial.println(position);
    #endif
  }

  // Déplacer vers une note MIDI
  void moveToMidiNote(byte note) {
    if (!isHomed) {
      #if DEBUG_MODE
      Serial.println(F("StepperControl: Not homed, ignoring command"));
      #endif
      return;
    }

    // Limiter la note dans la plage définie
    note = constrain(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX);

    // Calculer la position en fonction de la note
    long position = map(note,
                       MIDI_NOTE_MIN,
                       MIDI_NOTE_MAX,
                       0,
                       TOTAL_TRAVEL_STEPS * MICROSTEPS);

    moveTo(position);

    #if DEBUG_MODE
    Serial.print(F("StepperControl: MIDI Note "));
    Serial.print(note);
    Serial.print(F(" -> Position "));
    Serial.println(position);
    #endif
  }

  // Mettre à jour le moteur (à appeler dans loop)
  void update() {
    stepper.run();
  }

  // Vérifier si le moteur est en mouvement
  bool isMoving() {
    return stepper.distanceToGo() != 0;
  }

  // Obtenir la position actuelle
  long getCurrentPosition() {
    return stepper.currentPosition();
  }

  // Arrêt d'urgence
  void stop() {
    stepper.stop();
    #if DEBUG_MODE
    Serial.println(F("StepperControl: Emergency stop"));
    #endif
  }

  // Activer/désactiver le moteur
  void enableMotor(bool enable) {
    #ifdef STEPPER_ENABLE_PIN
    digitalWrite(STEPPER_ENABLE_PIN, enable ? LOW : HIGH); // La plupart des drivers sont actifs à LOW
    #endif
  }

  // Vérifier si le système est initialisé
  bool isSystemHomed() {
    return isHomed;
  }

  // Réinitialiser le homing
  void resetHoming() {
    isHomed = false;
  }
};

#endif // STEPPER_CONTROL_H
