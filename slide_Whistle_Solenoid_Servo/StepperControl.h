/*
 * StepperControl.h
 * Gestion du moteur pas à pas pour le slider du pipeau
 * Utilise la bibliothèque AccelStepper pour un contrôle précis
 * Support: pitch bend, vibrato, calculs en mm
 */

#ifndef STEPPER_CONTROL_H
#define STEPPER_CONTROL_H

#include <AccelStepper.h>
#include "settings.h"

class StepperControl {
private:
  AccelStepper stepper;
  bool isHomed;
  float currentPositionMM;
  float targetPositionMM;
  float baseNoteMM;         // Position de base de la note
  float pitchBendOffsetMM;  // Offset dû au pitch bend
  float vibratoOffsetMM;    // Offset dû au vibrato
  unsigned long vibratoStartTime;
  bool vibratoActive;
  int directionMultiplier;  // +1 ou -1 selon INVERT_MOTOR_DIR

  // Conversion mm -> pas
  long mmToSteps(float mm) {
    return (long)(mm * STEPS_PER_MM * MICROSTEPS);
  }

  // Conversion pas -> mm
  float stepsToMM(long steps) {
    return (float)steps / (STEPS_PER_MM * MICROSTEPS);
  }

public:
  // Constructeur
  StepperControl()
    : stepper(AccelStepper::DRIVER, STEPPER_STEP_PIN, STEPPER_DIR_PIN),
      isHomed(false),
      currentPositionMM(0),
      targetPositionMM(0),
      baseNoteMM(0),
      pitchBendOffsetMM(0),
      vibratoOffsetMM(0),
      vibratoStartTime(0),
      vibratoActive(false),
      directionMultiplier(INVERT_MOTOR_DIR ? -1 : 1) {
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

    // Configuration du moteur (conversion mm/s -> pas/s)
    float speedStepsPerSec = STEPPER_SPEED_MM_S * STEPS_PER_MM * MICROSTEPS;
    float accelStepsPerSec2 = STEPPER_ACCEL_MM_S2 * STEPS_PER_MM * MICROSTEPS;

    stepper.setMaxSpeed(speedStepsPerSec);
    stepper.setAcceleration(accelStepsPerSec2);
    stepper.setCurrentPosition(0);

    #if DEBUG_MODE
    Serial.println(F("StepperControl: Initialized"));
    Serial.print(F("  Steps per mm: "));
    Serial.println(STEPS_PER_MM);
    Serial.print(F("  Max speed: "));
    Serial.print(STEPPER_SPEED_MM_S);
    Serial.println(F(" mm/s"));
    Serial.print(F("  Travel: "));
    Serial.print(SLIDER_TRAVEL_MM);
    Serial.println(F(" mm"));
    Serial.print(F("  Direction: "));
    Serial.println(INVERT_MOTOR_DIR ? "INVERTED" : "NORMAL");
    #endif
  }

  // Procédure de homing
  bool performHoming() {
    #if DEBUG_MODE
    Serial.println(F("StepperControl: Starting homing sequence..."));
    #endif

    unsigned long startTime = millis();
    float homingSpeedSteps = HOMING_SPEED_MM_S * STEPS_PER_MM * MICROSTEPS;

    // Phase 1: Avancer vers le capteur
    stepper.setSpeed(-homingSpeedSteps * directionMultiplier);

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
    long backoffSteps = mmToSteps(HOMING_BACKOFF_MM) * directionMultiplier;
    stepper.move(backoffSteps);
    while (stepper.distanceToGo() != 0) {
      stepper.run();
    }

    // Phase 3: Approche lente finale
    stepper.setSpeed(-homingSpeedSteps * directionMultiplier / 4);
    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();
    }

    // Définir la position zéro avec offset
    long offsetSteps = -mmToSteps(HOME_OFFSET_MM) * directionMultiplier;
    stepper.setCurrentPosition(offsetSteps);
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
    float centerMM = SLIDER_TRAVEL_MM / 2.0;
    moveToMM(centerMM);
  }

  // Déplacer vers une position absolue (en mm)
  void moveToMM(float positionMM) {
    #if ENABLE_SOFT_LIMITS
    // Limiter la position dans la plage valide
    positionMM = constrain(positionMM, 0, SLIDER_TRAVEL_MM);
    #endif

    targetPositionMM = positionMM;
    long targetSteps = mmToSteps(positionMM) * directionMultiplier;
    stepper.moveTo(targetSteps);

    #if DEBUG_MODE
    Serial.print(F("StepperControl: Moving to "));
    Serial.print(positionMM);
    Serial.println(F(" mm"));
    #endif
  }

  // Obtenir la position pour une note depuis la LUT
  float getPositionForNote(byte note) {
    #if USE_POSITION_LUT
    // Utiliser la table de lookup calibrée
    if (note < MIDI_NOTE_MIN || note > MIDI_NOTE_MAX) {
      return 0.0;
    }
    int index = note - MIDI_NOTE_MIN;
    return pgm_read_float(&NOTE_POSITION_LUT[index]);
    #else
    // Mapping linéaire (par défaut)
    note = constrain(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX);
    float positionMM = map(note,
                          MIDI_NOTE_MIN,
                          MIDI_NOTE_MAX,
                          0,
                          (long)(SLIDER_TRAVEL_MM * 100)) / 100.0;
    return positionMM;
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

    // Obtenir la position (LUT ou linéaire selon config)
    float positionMM = getPositionForNote(note);

    baseNoteMM = positionMM;
    updateTargetPosition();

    #if DEBUG_MODE
    Serial.print(F("StepperControl: MIDI Note "));
    Serial.print(note);
    Serial.print(F(" -> "));
    Serial.print(positionMM);
    Serial.print(F(" mm"));
    #if USE_POSITION_LUT
    Serial.println(F(" [LUT]"));
    #else
    Serial.println(F(" [LINEAR]"));
    #endif
    #endif
  }

  // Définir l'offset du pitch bend (en demi-tons)
  void setPitchBend(float semitones) {
    #if PITCHBEND_ENABLED
    // Calculer la plage en mm par demi-ton
    float mmPerSemitone = SLIDER_TRAVEL_MM / (MIDI_NOTE_MAX - MIDI_NOTE_MIN);
    pitchBendOffsetMM = semitones * mmPerSemitone;

    updateTargetPosition();

    #if DEBUG_MODE
    Serial.print(F("StepperControl: Pitch bend "));
    Serial.print(semitones);
    Serial.print(F(" semitones = "));
    Serial.print(pitchBendOffsetMM);
    Serial.println(F(" mm"));
    #endif
    #endif
  }

  // Activer/désactiver le vibrato
  void setVibrato(bool active, byte intensity) {
    #if AFTERTOUCH_ENABLED
    vibratoActive = active;
    if (active) {
      vibratoStartTime = millis();
      // L'intensité (0-127) module la profondeur
      // On garde VIBRATO_DEPTH_MM comme maximum
    } else {
      vibratoOffsetMM = 0;
      updateTargetPosition();
    }

    #if DEBUG_MODE
    Serial.print(F("StepperControl: Vibrato "));
    Serial.print(active ? "ON" : "OFF");
    if (active) {
      Serial.print(F(" intensity: "));
      Serial.println(intensity);
    } else {
      Serial.println();
    }
    #endif
    #endif
  }

  // Mettre à jour la position cible combinée
  void updateTargetPosition() {
    float totalPositionMM = baseNoteMM + pitchBendOffsetMM + vibratoOffsetMM;
    moveToMM(totalPositionMM);
  }

  // Mettre à jour le moteur (à appeler dans loop)
  void update() {
    // Calculer le vibrato si actif
    #if AFTERTOUCH_ENABLED
    if (vibratoActive) {
      unsigned long elapsed = millis() - vibratoStartTime;
      float phase = (elapsed / 1000.0) * VIBRATO_SPEED_HZ * 2.0 * PI;
      vibratoOffsetMM = sin(phase) * VIBRATO_DEPTH_MM;
      updateTargetPosition();
    }
    #endif

    stepper.run();
    currentPositionMM = stepsToMM(stepper.currentPosition() * directionMultiplier);
  }

  // Vérifier si le moteur est en mouvement
  bool isMoving() {
    return stepper.distanceToGo() != 0;
  }

  // Obtenir la position actuelle (mm)
  float getCurrentPositionMM() {
    return currentPositionMM;
  }

  // Arrêt d'urgence
  void stop() {
    stepper.stop();
    vibratoActive = false;
    #if DEBUG_MODE
    Serial.println(F("StepperControl: Emergency stop"));
    #endif
  }

  // Activer/désactiver le moteur
  void enableMotor(bool enable) {
    #ifdef STEPPER_ENABLE_PIN
    digitalWrite(STEPPER_ENABLE_PIN, enable ? LOW : HIGH);
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
