/*
 * StepperControl.h — Contrôle moteur pas à pas, instanciable par flûte
 *
 * Refactor v3 :
 *   - Pins / mécanique reçues via FluteHwConfig au moment de begin()
 *   - LUT propre à chaque instance (plus de pointeur partagé global)
 *   - currentPositionMM volatile (lecture cross-core)
 *   - homing protégé par timeouts sur chaque phase
 */

#ifndef STEPPER_CONTROL_H
#define STEPPER_CONTROL_H

#include <AccelStepper.h>
#include "settings.h"

class StepperControl {
private:
  AccelStepper* _stepper = nullptr;
  FluteHwConfig _cfg;
  bool          _isHomed = false;
  int           _dirMult = 1;

  volatile float currentPositionMM = 0.0f;
  float baseNoteMM         = 0.0f;
  float pitchBendOffsetMM  = 0.0f;
  float vibratoOffsetMM    = 0.0f;
  unsigned long vibratoStartTime = 0;
  bool  vibratoActive      = false;
  byte  lastVibratoIntensity = 0;

  // LUT propre à l'instance
  float _lut[MIDI_LUT_SIZE];
  bool  _useLUT = false;

  // Paramètres runtime modifiables
  float _maxSpeedMmS = DEFAULT_STEPPER_SPEED;
  float _accelMmS2   = DEFAULT_STEPPER_ACCEL;

  long mmToSteps(float mm) const {
    return (long)(mm * _cfg.stepsPerMM);
  }

  float stepsToMM(long steps) const {
    return (float)steps / _cfg.stepsPerMM;
  }

  // Lecture endstop avec debounce logiciel (3 lectures concordantes sur 1ms)
  bool endstopActive() {
    int s1 = digitalRead(_cfg.endstopPin);
    if (s1 != _cfg.endstopActiveState) return false;
    delayMicroseconds(300);
    int s2 = digitalRead(_cfg.endstopPin);
    if (s2 != _cfg.endstopActiveState) return false;
    delayMicroseconds(300);
    int s3 = digitalRead(_cfg.endstopPin);
    return s3 == _cfg.endstopActiveState;
  }

  void applyMove(float positionMM) {
#if ENABLE_SOFT_LIMITS
    positionMM = constrain(positionMM, 0.0f, _cfg.sliderTravelMM);
#endif
    if (_stepper) _stepper->moveTo(mmToSteps(positionMM) * _dirMult);
  }

  void updateTarget() {
    applyMove(baseNoteMM + pitchBendOffsetMM + vibratoOffsetMM);
  }

public:
  StepperControl() {
    // initialiser la LUT par défaut
    memcpy(_lut, DEFAULT_NOTE_POSITION_LUT, sizeof(_lut));
  }

  ~StepperControl() {
    delete _stepper;
  }

  // ---- Init avec config matérielle ---------------------------------------

  void begin(const FluteHwConfig& cfg) {
    _cfg     = cfg;
    _dirMult = cfg.invertMotorDir ? -1 : 1;

    delete _stepper;
    _stepper = new AccelStepper(AccelStepper::DRIVER,
                                cfg.stepperStepPin, cfg.stepperDirPin);

    pinMode(cfg.stepperStepPin,   OUTPUT);
    pinMode(cfg.stepperDirPin,    OUTPUT);
    pinMode(cfg.stepperEnablePin, OUTPUT);
    enableMotor(true);

    pinMode(cfg.endstopPin, INPUT);  // pull-up externe sur GPIO34-39

    _stepper->setMaxSpeed(_maxSpeedMmS * cfg.stepsPerMM);
    _stepper->setAcceleration(_accelMmS2 * cfg.stepsPerMM);
    _stepper->setCurrentPosition(0);

#if DEBUG_MODE
    Serial.printf("[Stepper:%s] init step=%d dir=%d en=%d endstop=%d  %.1fmm/s  %.1fmm course\n",
                  cfg.name, cfg.stepperStepPin, cfg.stepperDirPin,
                  cfg.stepperEnablePin, cfg.endstopPin,
                  _maxSpeedMmS, cfg.sliderTravelMM);
#endif
  }

  // ---- LUT ---------------------------------------------------------------

  void setLUT(const float* values) {
    if (values) memcpy(_lut, values, sizeof(_lut));
  }
  const float* getLUT() const { return _lut; }
  void  setUseLUT(bool use)   { _useLUT = use; }
  bool  getUseLUT() const     { return _useLUT; }

  // ---- Homing ------------------------------------------------------------

  bool performHoming() {
    if (!_stepper) return false;
#if DEBUG_MODE
    Serial.printf("[Stepper:%s] homing...\n", _cfg.name);
#endif
    const float fastSteps = DEFAULT_HOMING_SPEED * _cfg.stepsPerMM;
    unsigned long t0 = millis();

    // Phase 1 — recherche rapide
    _stepper->setSpeed(-fastSteps * _dirMult);
    while (!endstopActive()) {
      _stepper->runSpeed();
      if (millis() - t0 > DEFAULT_MAX_HOMING_TIME) {
#if DEBUG_MODE
        Serial.printf("[Stepper:%s] homing timeout phase1\n", _cfg.name);
#endif
        return false;
      }
    }

    // Phase 2 — recul
    _stepper->setMaxSpeed(_maxSpeedMmS * _cfg.stepsPerMM);
    _stepper->setAcceleration(_accelMmS2 * _cfg.stepsPerMM);
    _stepper->move(mmToSteps(DEFAULT_HOMING_BACKOFF) * _dirMult);
    while (_stepper->distanceToGo() != 0) _stepper->run();

    // Phase 3 — approche lente
    _stepper->setSpeed(-fastSteps * 0.25f * _dirMult);
    unsigned long t3 = millis();
    while (!endstopActive()) {
      _stepper->runSpeed();
      if (millis() - t3 > 5000) {
#if DEBUG_MODE
        Serial.printf("[Stepper:%s] homing timeout phase3\n", _cfg.name);
#endif
        return false;
      }
    }

    long offsetSteps = -mmToSteps(_cfg.homeOffsetMM) * _dirMult;
    _stepper->setCurrentPosition(offsetSteps);
    _stepper->moveTo(0);
    while (_stepper->distanceToGo() != 0) _stepper->run();

    _isHomed = true;
    currentPositionMM = 0.0f;
#if DEBUG_MODE
    Serial.printf("[Stepper:%s] homing OK\n", _cfg.name);
#endif
    return true;
  }

  // ---- Déplacements ------------------------------------------------------

  void moveToMM(float mm) { applyMove(mm); }

  void moveToMidiNote(byte note) {
    if (!_isHomed) return;
    note = constrain(note, _cfg.noteMin, _cfg.noteMax);
    baseNoteMM = getPositionForNote(note);
    updateTarget();
#if DEBUG_MODE
    Serial.printf("[Stepper:%s] note %d → %.2fmm%s\n",
                  _cfg.name, note, baseNoteMM, _useLUT ? " [LUT]" : " [lin]");
#endif
  }

  float getPositionForNote(byte note) const {
    byte n = constrain(note, _cfg.noteMin, _cfg.noteMax);
    int  idx = n - _cfg.noteMin;
    int  span = _cfg.noteMax - _cfg.noteMin;
    if (span <= 0) return 0.0f;
    if (_useLUT && idx >= 0 && idx < MIDI_LUT_SIZE) {
      return _lut[idx];
    }
    return ((float)idx / (float)span) * _cfg.sliderTravelMM;
  }

  // ---- Pitch bend / vibrato ----------------------------------------------

  void setPitchBend(float semitones) {
    int span = _cfg.noteMax - _cfg.noteMin;
    if (span <= 0) return;
    const float mmPerSemitone = _cfg.sliderTravelMM / (float)span;
    pitchBendOffsetMM = semitones * mmPerSemitone;
    updateTarget();
  }

  void setVibrato(bool active, byte intensity) {
    vibratoActive        = active;
    lastVibratoIntensity = intensity;
    if (active) {
      vibratoStartTime = millis();
    } else {
      vibratoOffsetMM = 0.0f;
      updateTarget();
    }
  }

  // ---- Boucle update -----------------------------------------------------

  void update() {
    if (!_stepper) return;
    if (vibratoActive) {
      float t     = (millis() - vibratoStartTime) / 1000.0f;
      float depth = VIBRATO_DEPTH_MM * (lastVibratoIntensity / 127.0f);
      vibratoOffsetMM = sinf(t * VIBRATO_SPEED_HZ * 2.0f * PI) * depth;
      updateTarget();
    }
    _stepper->run();
    currentPositionMM = stepsToMM(_stepper->currentPosition() * _dirMult);
  }

  // ---- Contrôles divers --------------------------------------------------

  void stop() { if (_stepper) _stepper->stop(); vibratoActive = false; }

  void enableMotor(bool en) {
    digitalWrite(_cfg.stepperEnablePin, en ? LOW : HIGH);
  }

  void setSpeed(float mmps) {
    _maxSpeedMmS = mmps;
    if (_stepper) _stepper->setMaxSpeed(mmps * _cfg.stepsPerMM);
  }

  void setAcceleration(float mmps2) {
    _accelMmS2 = mmps2;
    if (_stepper) _stepper->setAcceleration(mmps2 * _cfg.stepsPerMM);
  }

  // ---- Getters -----------------------------------------------------------

  bool   isMoving()          const { return _stepper && _stepper->distanceToGo() != 0; }
  bool   isHomed()           const { return _isHomed; }
  void   resetHomed()              { _isHomed = false; }
  float  getCurrentPositionMM() const { return currentPositionMM; }
  float  getDistanceToTarget() const { return _stepper ? stepsToMM(abs(_stepper->distanceToGo())) : 0.0f; }
  const FluteHwConfig& getConfig() const { return _cfg; }
  float  getSpeedMmS() const { return _maxSpeedMmS; }
  float  getAccelMmS2() const { return _accelMmS2; }
};

#endif // STEPPER_CONTROL_H
