/*
 * StepperControl.h — Contrôle moteur pas à pas (ESP32)
 *
 * Corrections vs v1 :
 *   - LUT lue depuis runtimeCfg (pointer), pas la constante compilée
 *   - useLUT modifiable au runtime (setRuntimeUseLUT)
 *   - currentPositionMM volatile → visible depuis Core 0 (WebSocket)
 *   - Homing phase 3 : timeout ajouté
 *   - mmToSteps / stepsToMM cohérents (STEPS_PER_MM inclut déjà MICROSTEPS)
 */

#ifndef STEPPER_CONTROL_H
#define STEPPER_CONTROL_H

#include <AccelStepper.h>
#include "settings.h"

class StepperControl {
private:
  AccelStepper stepper;
  bool  _isHomed           = false;
  volatile float currentPositionMM = 0.0f;  // volatile : lu par Core 0 (WS)
  float baseNoteMM         = 0.0f;
  float pitchBendOffsetMM  = 0.0f;
  float vibratoOffsetMM    = 0.0f;
  unsigned long vibratoStartTime  = 0;
  bool  vibratoActive      = false;
  byte  lastVibratoIntensity = 0;
  int   dirMult;

  // Pointeur vers la LUT runtime (pointe sur runtimeCfg.lutPositions)
  float* _runtimeLUT       = nullptr;
  bool   _runtimeUseLUT    = false;

  long mmToSteps(float mm) const {
    return (long)(mm * STEPS_PER_MM);
  }

  float stepsToMM(long steps) const {
    return (float)steps / STEPS_PER_MM;
  }

  void applyMove(float positionMM) {
#if ENABLE_SOFT_LIMITS
    positionMM = constrain(positionMM, 0.0f, SLIDER_TRAVEL_MM);
#endif
    stepper.moveTo(mmToSteps(positionMM) * dirMult);
  }

  void updateTarget() {
    applyMove(baseNoteMM + pitchBendOffsetMM + vibratoOffsetMM);
  }

public:
  StepperControl()
    : stepper(AccelStepper::DRIVER, STEPPER_STEP_PIN, STEPPER_DIR_PIN),
      dirMult(INVERT_MOTOR_DIR ? -1 : 1) {}

  void begin() {
    pinMode(STEPPER_STEP_PIN,   OUTPUT);
    pinMode(STEPPER_DIR_PIN,    OUTPUT);
    pinMode(STEPPER_ENABLE_PIN, OUTPUT);
    enableMotor(true);

    // GPIO34 est input-only — pull-up 10 kΩ externe obligatoire
    pinMode(ENDSTOP_PIN, INPUT);

    stepper.setMaxSpeed(STEPPER_SPEED_MM_S * STEPS_PER_MM);
    stepper.setAcceleration(STEPPER_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.setCurrentPosition(0);

#if DEBUG_MODE
    Serial.printf("[Stepper] Init — %.1f mm/s  %.1f mm/s²  %.1f mm course\n",
                  STEPPER_SPEED_MM_S, STEPPER_ACCEL_MM_S2, SLIDER_TRAVEL_MM);
#endif
  }

  // ---- LUT runtime --------------------------------------------------------

  // Appeler après begin() avec runtimeCfg.lutPositions comme pointeur.
  // Le pointeur reste valide pour toute la durée du programme.
  void setRuntimeLUT(float* lut, bool use) {
    _runtimeLUT    = lut;
    _runtimeUseLUT = use;
  }

  void setRuntimeUseLUT(bool use) {
    _runtimeUseLUT = use;
  }

  // ---- Homing -------------------------------------------------------------

  bool performHoming() {
#if DEBUG_MODE
    Serial.println(F("[Stepper] Homing..."));
#endif
    const float fastSteps = HOMING_SPEED_MM_S * STEPS_PER_MM;
    unsigned long t0 = millis();

    // Phase 1 — recherche rapide
    stepper.setSpeed(-fastSteps * dirMult);
    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();
      if (millis() - t0 > MAX_HOMING_TIME) {
#if DEBUG_MODE
        Serial.println(F("[Stepper] Homing timeout (phase 1)"));
#endif
        return false;
      }
    }

    // Phase 2 — recul
    stepper.setMaxSpeed(STEPPER_SPEED_MM_S * STEPS_PER_MM);
    stepper.setAcceleration(STEPPER_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.move(mmToSteps(HOMING_BACKOFF_MM) * dirMult);
    while (stepper.distanceToGo() != 0) stepper.run();

    // Phase 3 — approche lente (avec timeout)
    stepper.setSpeed(-fastSteps * 0.25f * dirMult);
    unsigned long t3 = millis();
    while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
      stepper.runSpeed();
      if (millis() - t3 > 5000) {  // 5 s max pour la phase lente
#if DEBUG_MODE
        Serial.println(F("[Stepper] Homing timeout (phase 3)"));
#endif
        return false;
      }
    }

    // Position zéro + offset
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

  // ---- Déplacements -------------------------------------------------------

  void moveToMM(float mm) {
    applyMove(mm);
  }

  void moveToMidiNote(byte note) {
    if (!_isHomed) return;
    note = constrain(note, (byte)MIDI_NOTE_MIN, (byte)MIDI_NOTE_MAX);
    baseNoteMM = getPositionForNote(note);
    updateTarget();
#if DEBUG_MODE
    Serial.printf("[Stepper] Note %d → %.2f mm%s\n",
                  note, baseNoteMM, _runtimeUseLUT ? " [LUT]" : " [lin]");
#endif
  }

  float getPositionForNote(byte note) const {
    byte n = constrain(note, (byte)MIDI_NOTE_MIN, (byte)MIDI_NOTE_MAX);
    if (_runtimeUseLUT && _runtimeLUT) {
      return _runtimeLUT[n - MIDI_NOTE_MIN];
    }
    // Mapping linéaire
    return ((float)(n - MIDI_NOTE_MIN) / (float)(MIDI_NOTE_MAX - MIDI_NOTE_MIN))
           * SLIDER_TRAVEL_MM;
  }

  // ---- Pitch bend / vibrato -----------------------------------------------

  void setPitchBend(float semitones) {
#if PITCHBEND_ENABLED
    const float mmPerSemitone = SLIDER_TRAVEL_MM / (float)(MIDI_NOTE_MAX - MIDI_NOTE_MIN);
    pitchBendOffsetMM = semitones * mmPerSemitone;
    updateTarget();
#endif
  }

  void setVibrato(bool active, byte intensity) {
#if AFTERTOUCH_ENABLED
    vibratoActive       = active;
    lastVibratoIntensity = intensity;
    if (active) {
      vibratoStartTime = millis();
    } else {
      vibratoOffsetMM = 0.0f;
      updateTarget();
    }
#endif
  }

  // ---- Boucle principale (appeler depuis taskMotion) ----------------------

  void update() {
#if AFTERTOUCH_ENABLED
    if (vibratoActive) {
      float t     = (millis() - vibratoStartTime) / 1000.0f;
      float depth = VIBRATO_DEPTH_MM * (lastVibratoIntensity / 127.0f);
      vibratoOffsetMM = sinf(t * VIBRATO_SPEED_HZ * 2.0f * PI) * depth;
      updateTarget();
    }
#endif
    stepper.run();
    currentPositionMM = stepsToMM(stepper.currentPosition() * dirMult);
  }

  // ---- Contrôles divers ---------------------------------------------------

  void stop() {
    stepper.stop();
    vibratoActive = false;
  }

  void enableMotor(bool en) {
    digitalWrite(STEPPER_ENABLE_PIN, en ? LOW : HIGH);
  }

  void setSpeed(float mmps) {
    stepper.setMaxSpeed(mmps * STEPS_PER_MM);
  }

  void setAcceleration(float mmps2) {
    stepper.setAcceleration(mmps2 * STEPS_PER_MM);
  }

  // ---- Getters ------------------------------------------------------------

  bool  isMoving()              const { return stepper.distanceToGo() != 0; }
  bool  isHomed()               const { return _isHomed; }
  void  resetHomed()                  { _isHomed = false; }
  float getCurrentPositionMM()  const { return currentPositionMM; }
  float getDistanceToTarget()   const { return stepsToMM(abs(stepper.distanceToGo())); }
};

#endif // STEPPER_CONTROL_H
