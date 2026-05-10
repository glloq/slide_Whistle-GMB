/*
 * PressureControl.h — Gestion pompe + réservoir + capteur pression
 *
 * Architecture :
 *
 *   Pompe → Réservoir → Distributeur → Solénoïdes (AirControl par flûte)
 *
 *   ┌─────────┐   ┌──────────┐   ┌────────────┐
 *   │  Pompe  ├──▶│Réservoir ├──▶│ Solénoïdes │ (1 par flûte)
 *   └─────────┘   └──────────┘   └────────────┘
 *        ▲              │
 *        │   ┌──────────▼─────────┐
 *        └───┤ PressureControl    │← capteur pression analogique
 *            │  FSM + hystérésis  │
 *            └────────────────────┘
 *
 * États :
 *   IDLE        — pompe arrêtée, attente
 *   FILLING     — pompe ON, montée vers pressureTarget
 *   MAINTAIN    — pression dans [min, max], pompe régulée à la demande
 *   SAFETY      — défaut (timeout / surpression / réservoir vide) → pompe OFF, alerte
 *
 * Modes :
 *   - avec capteur analogique (pressureSensorPin >= 0) → régulation kPa
 *   - sans capteur → mode hystérésis temporelle (pump_on_time / pump_off_time)
 */

#ifndef PRESSURE_CONTROL_H
#define PRESSURE_CONTROL_H

#include "settings.h"

enum PressureState {
  PRES_IDLE,
  PRES_FILLING,
  PRES_MAINTAIN,
  PRES_SAFETY
};

enum PressureFault {
  PFAULT_NONE,
  PFAULT_FILL_TIMEOUT,
  PFAULT_OVERPRESSURE,
  PFAULT_RESERVOIR_EMPTY
};

class PressureControl {
private:
  PressureHwConfig _cfg;
  bool _begun = false;

  PressureState _state    = PRES_IDLE;
  PressureFault _fault    = PFAULT_NONE;
  unsigned long _stateAt  = 0;
  unsigned long _pumpOffAt = 0;

  bool     _pumpRunning   = false;
  uint8_t  _pumpDuty      = 0;        // 0..255
  float    _lastPressure  = 0.0f;
  bool     _reservoirOk   = true;

  // Mode "sans capteur" : alternance ON/OFF
  uint16_t _noSensorOnMs   = 2000;
  uint16_t _noSensorOffMs  = 4000;

  // Setters runtime (NVS)
  float _target, _min, _max, _safety;
  uint16_t _maxFillMs;

  void setPump(bool on, uint8_t duty = 0) {
    _pumpRunning = on;
    _pumpDuty    = on ? duty : 0;

    if (_cfg.pumpPin < 0) return;
    if (_cfg.pumpPwm) {
      ledcWrite(_cfg.pumpLedcChannel, _pumpDuty);
    } else {
      digitalWrite(_cfg.pumpPin, on ? HIGH : LOW);
    }
#if DEBUG_MODE
    Serial.printf("[Pressure] pump %s duty=%u\n", on ? "ON" : "OFF", _pumpDuty);
#endif
  }

  void changeState(PressureState s) {
    _state   = s;
    _stateAt = millis();
#if DEBUG_MODE
    const char* names[] = {"IDLE","FILLING","MAINTAIN","SAFETY"};
    Serial.printf("[Pressure] → %s\n", names[s]);
#endif
  }

  bool readReservoirOk() const {
    if (_cfg.reservoirLevelPin < 0) return true;
    int v = digitalRead(_cfg.reservoirLevelPin);
    return v != _cfg.reservoirLowState;
  }

  float readPressureKpa() {
    if (_cfg.pressureSensorPin < 0) return -1.0f;
    int raw = analogRead(_cfg.pressureSensorPin);   // 0..4095
    float ratio = raw / 4095.0f;
    float kpa   = _cfg.pressureSensorMin
                + ratio * (_cfg.pressureSensorMax - _cfg.pressureSensorMin);
    _lastPressure = kpa;
    return kpa;
  }

public:
  PressureControl() {}

  // ---- Init ---------------------------------------------------------------

  void begin(const PressureHwConfig& cfg) {
    _cfg       = cfg;
    _target    = cfg.pressureTarget;
    _min       = cfg.pressureMin;
    _max       = cfg.pressureMax;
    _safety    = cfg.pressureSafety;
    _maxFillMs = cfg.maxFillTimeMs;

    if (!cfg.enabled || cfg.pumpPin < 0) {
#if DEBUG_MODE
      Serial.println(F("[Pressure] désactivé"));
#endif
      return;
    }

    if (cfg.pumpPwm) {
      ledcSetup(cfg.pumpLedcChannel, DEFAULT_LEDC_FREQ, DEFAULT_LEDC_RES);
      ledcAttachPin(cfg.pumpPin, cfg.pumpLedcChannel);
      ledcWrite(cfg.pumpLedcChannel, 0);
    } else {
      pinMode(cfg.pumpPin, OUTPUT);
      digitalWrite(cfg.pumpPin, LOW);
    }

    if (cfg.pressureSensorPin >= 0) {
      analogSetPinAttenuation(cfg.pressureSensorPin,
                              (adc_attenuation_t)cfg.pressureSensorAdcAtten);
      analogReadResolution(12);
    }

    if (cfg.reservoirLevelPin >= 0) {
      pinMode(cfg.reservoirLevelPin, INPUT_PULLUP);
    }

    _begun = true;
#if DEBUG_MODE
    Serial.printf("[Pressure] init pump=%d(pwm=%d) sensor=%d reservoir=%d\n",
                  cfg.pumpPin, cfg.pumpPwm, cfg.pressureSensorPin,
                  cfg.reservoirLevelPin);
    Serial.printf("[Pressure] target=%.1f min=%.1f max=%.1f safety=%.1f kPa\n",
                  _target, _min, _max, _safety);
#endif
  }

  // ---- Boucle update ------------------------------------------------------

  void update() {
    if (!_begun) return;

    unsigned long now = millis();
    _reservoirOk = readReservoirOk();
    bool hasSensor = (_cfg.pressureSensorPin >= 0);
    float p = hasSensor ? readPressureKpa() : -1.0f;

    // ----- Sécurité globale -----
    if (!_reservoirOk) {
      if (_state != PRES_SAFETY) {
        _fault = PFAULT_RESERVOIR_EMPTY;
        setPump(false);
        changeState(PRES_SAFETY);
      }
      return;
    }

    if (hasSensor && p > _safety) {
      _fault = PFAULT_OVERPRESSURE;
      setPump(false);
      changeState(PRES_SAFETY);
      return;
    }

    // ----- FSM -----
    switch (_state) {
      case PRES_IDLE:
        // démarrage explicite via start()
        break;

      case PRES_FILLING:
        if (hasSensor) {
          if (p >= _target) {
            setPump(false);
            _pumpOffAt = now;
            changeState(PRES_MAINTAIN);
          } else if (now - _stateAt > _maxFillMs) {
            _fault = PFAULT_FILL_TIMEOUT;
            setPump(false);
            changeState(PRES_SAFETY);
          } else {
            setPump(true, _cfg.pumpPwmMax);
          }
        } else {
          // mode temporel : ON _noSensorOnMs puis OFF _noSensorOffMs
          unsigned long e = now - _stateAt;
          if (e < _noSensorOnMs) setPump(true, _cfg.pumpPwmMax);
          else { setPump(false); _pumpOffAt = now; changeState(PRES_MAINTAIN); }
        }
        break;

      case PRES_MAINTAIN:
        if (hasSensor) {
          if (p < _min && now - _pumpOffAt > _cfg.pumpMinOffMs) {
            setPump(true, _cfg.pumpPwmMax);
          } else if (p > _max && _pumpRunning) {
            setPump(false);
            _pumpOffAt = now;
          }
        } else {
          // alternance temporelle
          unsigned long e = now - _stateAt;
          if (!_pumpRunning && e > _noSensorOffMs) {
            setPump(true, _cfg.pumpPwmMax);
            _stateAt = now;
          } else if (_pumpRunning && e > _noSensorOnMs) {
            setPump(false);
            _pumpOffAt = now;
            _stateAt = now;
          }
        }
        break;

      case PRES_SAFETY:
        setPump(false);
        // Reset uniquement par appel manuel à reset()
        break;
    }
  }

  // ---- Commandes ----------------------------------------------------------

  void start() {
    if (!_begun) return;
    if (_state == PRES_SAFETY) return;          // reset d'abord
    _fault = PFAULT_NONE;
    changeState(PRES_FILLING);
  }

  void stop() {
    if (!_begun) return;
    setPump(false);
    changeState(PRES_IDLE);
  }

  void reset() {
    _fault = PFAULT_NONE;
    setPump(false);
    changeState(PRES_IDLE);
  }

  // ---- Setters runtime ----------------------------------------------------

  void setTarget(float kpa)   { _target = kpa; }
  void setMin(float kpa)      { _min = kpa; }
  void setMax(float kpa)      { _max = kpa; }
  void setSafety(float kpa)   { _safety = kpa; }
  void setMaxFillMs(uint16_t ms) { _maxFillMs = ms; }

  // ---- Getters ------------------------------------------------------------

  bool          isEnabled()    const { return _begun; }
  PressureState getState()     const { return _state; }
  PressureFault getFault()     const { return _fault; }
  float         getPressure()  const { return _lastPressure; }
  bool          getPumpOn()    const { return _pumpRunning; }
  uint8_t       getPumpDuty()  const { return _pumpDuty; }
  bool          getReservoirOk() const { return _reservoirOk; }
  bool          hasSensor()    const { return _cfg.pressureSensorPin >= 0; }

  float getTarget() const { return _target; }
  float getMin()    const { return _min; }
  float getMax()    const { return _max; }
  float getSafety() const { return _safety; }
  uint16_t getMaxFillMs() const { return _maxFillMs; }

  const char* getStateName() const {
    switch (_state) {
      case PRES_IDLE:     return "IDLE";
      case PRES_FILLING:  return "FILLING";
      case PRES_MAINTAIN: return "MAINTAIN";
      case PRES_SAFETY:   return "SAFETY";
      default:            return "UNKNOWN";
    }
  }

  const char* getFaultName() const {
    switch (_fault) {
      case PFAULT_NONE:             return "none";
      case PFAULT_FILL_TIMEOUT:     return "fill_timeout";
      case PFAULT_OVERPRESSURE:     return "overpressure";
      case PFAULT_RESERVOIR_EMPTY:  return "reservoir_empty";
      default:                      return "unknown";
    }
  }
};

#endif // PRESSURE_CONTROL_H
