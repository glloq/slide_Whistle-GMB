/*
 * StepperControl.h — Contrôle moteur pas à pas (ESP32)
 *
 * Différences vs version Arduino :
 *   - LUT en RAM normale (plus de pgm_read_float)
 *   - AccelStepper fonctionne identiquement sur ESP32
 *   - getDistanceToTarget() ajouté (utilisé par WebInterface)
 */

#ifndef STEPPER_CONTROL_H
#define STEPPER_CONTROL_H

#include <AccelStepper.h>
#include "settings.h"

class StepperControl {
private:
  AccelStepper stepper;
  bool _isHomed;
  float currentPositionMM;
  float baseNoteMM;
  float pitchBendOffsetMM;
  float vibratoOffsetMM;
  unsigned long vibratoStartTime;
  bool vibratoActive;
  byte lastVibratoIntensity;
  int dirMult;  // +1 ou -1

  long mmToSteps(float mm) {
    return (long)(mm * STEPS_PER_MM);
  }

  float stepsToMM(long steps) {
    return (float)steps / STEPS_PER_MM;
  }

  void applyMove(float positionMM) {
#if ENABLE_SOFT_LIMITS
    positionMM = constrain(positionMM, 0.0f, SLIDER_TRAVEL_MM);
#endif
    stepper.moveTo(mmToSteps(positionMM) * dirMult);
  }

public:
  StepperControl()
    : stepper(AccelStepper::DRIVER, STEPPER_STEP_PIN, STEPPER_DIR_PIN),
      _isHomed(false),
      currentPositionMM(0),
      baseNoteMM(0),
      pitchBendOffsetMM(0),
      vibratoOffsetMM(0),
      vibratoStartTime(0),
      vibratoActive(false),
      lastVibratoIntensity(0),
      dirMult(INVERT_MOTOR_DIR ? -1 : 1) {}

  void begin() {
    pinMode(STEPPER_STEP_PIN, OUTPUT);
    pinMode(STEPPER_DIR_PIN,  OUTPUT);
    pinMode(STEPPER_ENABLE_PIN, OUTPUT);
    enableMotor(true);

    // GPIO34 est input-only sur ESP32 — pas de INPUT_PULLUP interne disponible
    // → câbler résistance pull-up 10kΩ externe sur ENDSTOP_PIN
    pinMode(ENDSTOP_PIN, INPUT);

    stepper.setMaxSpeed(STEPPER_SPEED_MM_S * STEPS_PER_MM);
    stepper.setAcceleration(STEPPER_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.setCurrentPosition(0);

#if DEBUG_MODE
    Serial.printf("[Stepper] Init — %.1f mm/s, %.1f mm/s², %.1f mm course\n",
                  STEPPER_SPEED_MM_S, STEPPER_ACCEL_MM_S2, SLIDER_TRAVEL_MM);
#endif
  }

  // Homing 3 phases : rapide → recul → lent
  bool performHoming() {
#if DEBUG_MODE
    Serial.println(F("[Stepper] Homing..."));
#endif
    unsigned long t0 = millis();
    float fastSteps = HOMING_SPEED_MM_S * STEPS_PER_MM;

    // Phase 1 — recherche rapide
    stepper.setSpeed(-fastSteps * dirMult);
    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();
      if (millis() - t0 > MAX_HOMING_TIME) {
#if DEBUG_MODE
        Serial.println(F("[Stepper] Homing timeout!"));
#endif
        return false;
      }
    }

    // Phase 2 — recul
    stepper.setAcceleration(STEPPER_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.setMaxSpeed(STEPPER_SPEED_MM_S * STEPS_PER_MM);
    stepper.move(mmToSteps(HOMING_BACKOFF_MM) * dirMult);
    while (stepper.distanceToGo() != 0) stepper.run();

    // Phase 3 — approche lente
    stepper.setSpeed(-fastSteps * 0.25f * dirMult);
    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();
    }

    // Zéro avec offset
    long offsetSteps = -mmToSteps(HOME_OFFSET_MM) * dirMult;
    stepper.setCurrentPosition(offsetSteps);
    stepper.moveTo(0);
    while (stepper.distanceToGo() != 0) stepper.run();

    _isHomed = true;
    currentPositionMM = 0.0f;

#if DEBUG_MODE
    Serial.println(F("[Stepper] Homing OK"));
#endif
    return true;
  }

  void moveToMM(float mm) {
    applyMove(mm);
  }

  void moveToMidiNote(byte note) {
    if (!_isHomed) return;
    note = constrain(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX);
    baseNoteMM = getPositionForNote(note);
    updateTarget();
#if DEBUG_MODE
    Serial.printf("[Stepper] Note %d → %.2f mm\n", note, baseNoteMM);
#endif
  }

  float getPositionForNote(byte note) {
#if USE_POSITION_LUT
    int idx = constrain(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX) - MIDI_NOTE_MIN;
    return NOTE_POSITION_LUT[idx];
#else
    return map((long)(constrain(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX) * 100),
               MIDI_NOTE_MIN * 100L, MIDI_NOTE_MAX * 100L,
               0, (long)(SLIDER_TRAVEL_MM * 100)) / 100.0f;
#endif
  }

  void setPitchBend(float semitones) {
#if PITCHBEND_ENABLED
    float mmPerSemitone = SLIDER_TRAVEL_MM / (float)(MIDI_NOTE_MAX - MIDI_NOTE_MIN);
    pitchBendOffsetMM = semitones * mmPerSemitone;
    updateTarget();
#endif
  }

  void setVibrato(bool active, byte intensity) {
#if AFTERTOUCH_ENABLED
    vibratoActive = active;
    lastVibratoIntensity = intensity;
    if (active) {
      vibratoStartTime = millis();
    } else {
      vibratoOffsetMM = 0;
      updateTarget();
    }
#endif
  }

  void update() {
#if AFTERTOUCH_ENABLED
    if (vibratoActive) {
      float t = (millis() - vibratoStartTime) / 1000.0f;
      float depth = VIBRATO_DEPTH_MM * (lastVibratoIntensity / 127.0f);
      vibratoOffsetMM = sinf(t * VIBRATO_SPEED_HZ * 2.0f * PI) * depth;
      updateTarget();
    }
#endif
    stepper.run();
    currentPositionMM = stepsToMM(stepper.currentPosition() * dirMult);
  }

  void stop() {
    stepper.stop();
    vibratoActive = false;
  }

  void enableMotor(bool en) {
    digitalWrite(STEPPER_ENABLE_PIN, en ? LOW : HIGH);
  }

  bool isMoving()    { return stepper.distanceToGo() != 0; }
  bool isHomed()     { return _isHomed; }
  void resetHomed()  { _isHomed = false; }

  float getCurrentPositionMM() { return currentPositionMM; }

  float getDistanceToTarget() {
    return stepsToMM(abs(stepper.distanceToGo()));
  }

  void setSpeed(float mmps) {
    stepper.setMaxSpeed(mmps * STEPS_PER_MM);
  }

  void setAcceleration(float mmps2) {
    stepper.setAcceleration(mmps2 * STEPS_PER_MM);
  }

private:
  void updateTarget() {
    applyMove(baseNoteMM + pitchBendOffsetMM + vibratoOffsetMM);
  }
};

#endif // STEPPER_CONTROL_H
