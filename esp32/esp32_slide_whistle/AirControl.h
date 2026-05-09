/*
 * AirControl.h — Solénoïde + Servo, instanciable par flûte
 *
 * Refactor v3 :
 *   - Pins / canaux LEDC reçus via FluteHwConfig
 *   - Plus de référence à STATUS_LED_PIN (la LED est centralisée ailleurs)
 *   - Courbe de vélocité paramétrable (linéaire / quadratique / exponentielle)
 *   - Paramètres temporels (wait, open, close, legato) modifiables au runtime
 *   - PWM full / hold modifiables au runtime
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <ESP32Servo.h>
#include "settings.h"

enum SolenoidState {
  AIR_IDLE,
  AIR_WAITING_POSITION,
  AIR_OPENING,
  AIR_HOLDING,
  AIR_CLOSING
};

class AirControl {
private:
  Servo         airServo;
  FluteHwConfig _cfg;
  bool          _begun = false;

  SolenoidState currentState   = AIR_IDLE;
  unsigned long stateChangeTime = 0;
  unsigned long lastNoteOnTime  = 0;
  byte          currentVelocity = 0;
  uint8_t       currentPwm      = 0;

  // Paramètres runtime modifiables via web
  uint8_t  _pwmFull       = DEFAULT_PWM_FULL;
  uint8_t  _pwmHold       = DEFAULT_PWM_HOLD;
  uint16_t _waitDelayMs   = DEFAULT_POSITION_WAIT;
  uint16_t _openDurationMs = DEFAULT_OPEN_DURATION;
  uint16_t _closeDelayMs  = DEFAULT_CLOSE_DELAY;
  uint16_t _legatoMs      = DEFAULT_LEGATO_THRESHOLD;
  uint8_t  _servoClosed   = DEFAULT_SERVO_CLOSED_ANGLE;
  uint8_t  _servoOpen     = DEFAULT_SERVO_OPEN_ANGLE;

  void changeState(SolenoidState s) {
    currentState    = s;
    stateChangeTime = millis();
#if DEBUG_MODE
    const char* names[] = {"IDLE","WAITING","OPENING","HOLDING","CLOSING"};
    Serial.printf("[Air:%s] → %s\n", _cfg.name, names[s]);
#endif
  }

  // Courbe de vélocité runtime (initialisée depuis cfg, modifiable)
  uint8_t _velocityCurve = 1;

  // Mapping vélocité → angle servo selon la courbe choisie
  int velocityToAngle(byte vel) const {
    float v = vel / 127.0f;
    switch (_velocityCurve) {
      case 1: v = v * v;        break;       // quadratique (musical)
      case 2: v = v * v * v;    break;       // cubique
      default: /* linéaire */   break;
    }
    return _servoClosed + (int)(v * (_servoOpen - _servoClosed));
  }

  void setServoFromVelocity(byte vel) {
    int angle = velocityToAngle(vel);
    airServo.write(angle);
  }

public:
  // ---- Init --------------------------------------------------------------

  void begin(const FluteHwConfig& cfg) {
    _cfg = cfg;
    _velocityCurve = cfg.velocityCurve;

    // PWM solénoïde via LEDC (canal unique par flûte)
    ledcSetup(cfg.solenoidLedcChannel, DEFAULT_LEDC_FREQ, DEFAULT_LEDC_RES);
    ledcAttachPin(cfg.solenoidPin, cfg.solenoidLedcChannel);
    setSolenoidPWM(0);

    // Servo (timer unique par flûte)
    if (cfg.servoTimerIndex < 4) {
      ESP32PWM::allocateTimer(cfg.servoTimerIndex);
    }
    airServo.setPeriodHertz(50);
    airServo.attach(cfg.servoPin, 500, 2400);
    airServo.write(_servoClosed);
    vTaskDelay(pdMS_TO_TICKS(300));

    _begun = true;
#if DEBUG_MODE
    Serial.printf("[Air:%s] init solenoid=%d(ch%u) servo=%d(t%u) full=%u hold=%u\n",
                  cfg.name, cfg.solenoidPin, cfg.solenoidLedcChannel,
                  cfg.servoPin, cfg.servoTimerIndex, _pwmFull, _pwmHold);
#endif
  }

  // ---- Contrôle (callbacks MIDI) -----------------------------------------

  void startAir(byte velocity) {
    currentVelocity = velocity;
    lastNoteOnTime  = millis();
    setServoFromVelocity(velocity);

    switch (currentState) {
      case AIR_IDLE:
        changeState(AIR_WAITING_POSITION);
        break;
      case AIR_WAITING_POSITION:
        changeState(AIR_WAITING_POSITION);
        break;
      case AIR_OPENING:
      case AIR_HOLDING:
        // Legato : air maintenu
        break;
      case AIR_CLOSING:
        changeState(AIR_HOLDING);
        setSolenoidPWM(_pwmHold);
        break;
    }
  }

  void stopAir() {
    if (currentState == AIR_OPENING || currentState == AIR_HOLDING) {
      changeState(AIR_CLOSING);
    }
  }

  void setAirflow(byte ccValue) {
    setServoFromVelocity(ccValue);
  }

  // ---- Boucle update -----------------------------------------------------

  void update() {
    if (!_begun) return;
    const unsigned long elapsed = millis() - stateChangeTime;

    switch (currentState) {
      case AIR_IDLE:
        break;
      case AIR_WAITING_POSITION:
        if (elapsed >= _waitDelayMs) {
          changeState(AIR_OPENING);
          setSolenoidPWM(_pwmFull);
        }
        break;
      case AIR_OPENING:
        if (elapsed >= _openDurationMs) {
          changeState(AIR_HOLDING);
          setSolenoidPWM(_pwmHold);
        }
        break;
      case AIR_HOLDING:
        break;
      case AIR_CLOSING:
        if (elapsed >= _closeDelayMs) {
          changeState(AIR_IDLE);
          setSolenoidPWM(0);
          airServo.write(_servoClosed);
        }
        break;
    }
  }

  // ---- Accès direct ------------------------------------------------------

  void forceOpen(byte velocity = 100) {
    setServoFromVelocity(velocity);
    setSolenoidPWM(_pwmFull);
    vTaskDelay(pdMS_TO_TICKS(_openDurationMs));
    setSolenoidPWM(_pwmHold);
    currentState    = AIR_HOLDING;
    stateChangeTime = millis();
  }

  void forceClose() {
    setSolenoidPWM(0);
    airServo.write(_servoClosed);
    currentState    = AIR_IDLE;
    stateChangeTime = millis();
  }

  void runTestFromTask() {
    Serial.printf("[Air:%s] test solénoïde\n", _cfg.name);
    setSolenoidPWM(_pwmFull); vTaskDelay(pdMS_TO_TICKS(300));
    setSolenoidPWM(_pwmHold); vTaskDelay(pdMS_TO_TICKS(700));
    setSolenoidPWM(0);

    Serial.printf("[Air:%s] test servo\n", _cfg.name);
    for (int a = _servoClosed; a <= _servoOpen; a += 10) {
      airServo.write(a); vTaskDelay(pdMS_TO_TICKS(80));
    }
    for (int a = _servoOpen; a >= _servoClosed; a -= 10) {
      airServo.write(a); vTaskDelay(pdMS_TO_TICKS(80));
    }
    airServo.write(_servoClosed);
    currentState    = AIR_IDLE;
    stateChangeTime = millis();
  }

  void setSolenoidPWM(uint8_t val) {
    currentPwm = val;
    ledcWrite(_cfg.solenoidLedcChannel, val);
  }

  // ---- Setters runtime ---------------------------------------------------

  void setPwmFull(uint8_t v)    { _pwmFull = v; }
  void setPwmHold(uint8_t v)    { _pwmHold = v; }
  void setWaitDelay(uint16_t v) { _waitDelayMs = v; }
  void setOpenDuration(uint16_t v) { _openDurationMs = v; }
  void setCloseDelay(uint16_t v){ _closeDelayMs = v; }
  void setLegatoMs(uint16_t v)  { _legatoMs = v; }
  void setServoClosedAngle(uint8_t a) { _servoClosed = a; }
  void setServoOpenAngle(uint8_t a)   { _servoOpen   = a; }
  void setVelocityCurve(uint8_t c)    { _velocityCurve = (c <= 2) ? c : 0; }
  uint8_t getVelocityCurve() const    { return _velocityCurve; }

  // ---- Getters -----------------------------------------------------------

  SolenoidState getState() const { return currentState; }
  bool isSolenoidOpen() const    { return currentState == AIR_OPENING || currentState == AIR_HOLDING; }
  byte getCurrentVelocity() const { return currentVelocity; }
  uint8_t getCurrentPwm() const  { return currentPwm; }
  uint8_t getPwmFull() const     { return _pwmFull; }
  uint8_t getPwmHold() const     { return _pwmHold; }
  uint16_t getWaitDelay() const  { return _waitDelayMs; }
  uint16_t getLegatoMs() const   { return _legatoMs; }

  const char* getStateName() const {
    switch (currentState) {
      case AIR_IDLE:             return "IDLE";
      case AIR_WAITING_POSITION: return "WAITING";
      case AIR_OPENING:          return "OPENING";
      case AIR_HOLDING:          return "HOLDING";
      case AIR_CLOSING:          return "CLOSING";
      default:                   return "UNKNOWN";
    }
  }
};

#endif // AIR_CONTROL_H
