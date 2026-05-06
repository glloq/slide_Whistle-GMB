/*
 * AirControl.h — Solénoïde + Servo (ESP32)
 *
 * Corrections vs v1 :
 *   - PWM : ledcWrite() seul (plus d'analogWrite — cohérence API)
 *   - delay() → vTaskDelay() partout (compatibilité FreeRTOS)
 *   - setSolenoidPWM() public (accès depuis taskMotion pour le test)
 *   - setAirflow(byte) ajouté → CC2/CC11 expression
 *   - runTestFromTask() : version vTaskDelay sans delay() bloquant
 *   - testSequence() supprimée (remplacée par runTestFromTask)
 *   - stateChangeTime correctement mis à jour dans forceClose()
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <ESP32Servo.h>
#include "settings.h"

enum SolenoidState {
  IDLE,
  WAITING_POSITION,
  OPENING,
  HOLDING,
  CLOSING
};

class AirControl {
private:
  Servo airServo;
  SolenoidState currentState   = IDLE;
  unsigned long stateChangeTime = 0;
  unsigned long lastNoteOnTime  = 0;
  byte currentVelocity          = 0;

  void changeState(SolenoidState s) {
    currentState    = s;
    stateChangeTime = millis();
#if DEBUG_MODE
    const char* names[] = {"IDLE","WAITING","OPENING","HOLDING","CLOSING"};
    Serial.printf("[Air] → %s\n", names[s]);
#endif
  }

  void setServoFromVelocity(byte vel) {
    int angle = map(vel, 0, 127, SERVO_CLOSED_ANGLE, SERVO_OPEN_ANGLE);
    airServo.write(angle);
#if DEBUG_MODE
    Serial.printf("[Air] Servo %d° (vel %d)\n", angle, vel);
#endif
  }

public:
  // ---- Init ---------------------------------------------------------------

  void begin() {
    // PWM solénoïde via LEDC (API explicite — ne pas mélanger avec analogWrite)
    ledcSetup(SOLENOID_LEDC_CHANNEL, SOLENOID_LEDC_FREQ, SOLENOID_LEDC_RES);
    ledcAttachPin(SOLENOID_PIN, SOLENOID_LEDC_CHANNEL);
    setSolenoidPWM(0);

    // Servo
    ESP32PWM::allocateTimer(0);
    airServo.setPeriodHertz(50);
    airServo.attach(SERVO_PIN, 500, 2400);
    airServo.write(SERVO_CLOSED_ANGLE);
    vTaskDelay(pdMS_TO_TICKS(300));  // attente servo — vTaskDelay, pas delay()

#if DEBUG_MODE
    Serial.println(F("[Air] Init OK"));
    Serial.printf("[Air] PWM full=%d hold=%d  wait=%dms legato=%dms\n",
                  SOLENOID_PWM_FULL, SOLENOID_PWM_HOLD,
                  POSITION_WAIT_DELAY, LEGATO_THRESHOLD);
#endif
  }

  // ---- Contrôle air (appelé depuis callbacks MIDI) -----------------------

  void startAir(byte velocity) {
    currentVelocity = velocity;
    lastNoteOnTime  = millis();
    setServoFromVelocity(velocity);

    switch (currentState) {
      case IDLE:
        changeState(WAITING_POSITION);
        break;
      case WAITING_POSITION:
        changeState(WAITING_POSITION);  // reset timer
        break;
      case OPENING:
      case HOLDING:
#if DEBUG_MODE
        Serial.println(F("[Air] Legato — air maintenu"));
#endif
        break;
      case CLOSING:
        changeState(HOLDING);
        setSolenoidPWM(SOLENOID_PWM_HOLD);
        break;
    }
  }

  void stopAir() {
    if (currentState == OPENING || currentState == HOLDING) {
      changeState(CLOSING);
    }
  }

  // CC2 / CC11 expression → débit air direct (servo uniquement, sans changer état)
  void setAirflow(byte ccValue) {
    setServoFromVelocity(ccValue);
  }

  // ---- Machine à états (appeler depuis taskMotion) -----------------------

  void update() {
    const unsigned long elapsed = millis() - stateChangeTime;

    switch (currentState) {
      case IDLE:
        break;

      case WAITING_POSITION:
        if (elapsed >= POSITION_WAIT_DELAY) {
          changeState(OPENING);
          setSolenoidPWM(SOLENOID_PWM_FULL);
        }
        break;

      case OPENING:
        if (elapsed >= SOLENOID_OPEN_DURATION) {
          changeState(HOLDING);
          setSolenoidPWM(SOLENOID_PWM_HOLD);
        }
        break;

      case HOLDING:
        break;

      case CLOSING:
        if (elapsed >= SOLENOID_CLOSE_DELAY) {
          changeState(IDLE);
          setSolenoidPWM(0);
          airServo.write(SERVO_CLOSED_ANGLE);
        }
        break;
    }
  }

  // ---- Accès direct (calibration / watchdog) -----------------------------

  // Ouverture immédiate (utilisé dans taskMotion uniquement — vTaskDelay inside)
  void forceOpen(byte velocity = 100) {
    setServoFromVelocity(velocity);
    setSolenoidPWM(SOLENOID_PWM_FULL);
    vTaskDelay(pdMS_TO_TICKS(SOLENOID_OPEN_DURATION));
    setSolenoidPWM(SOLENOID_PWM_HOLD);
    currentState    = HOLDING;
    stateChangeTime = millis();
  }

  void forceClose() {
    setSolenoidPWM(0);
    airServo.write(SERVO_CLOSED_ANGLE);
    currentState    = IDLE;
    stateChangeTime = millis();
  }

  // Test complet (appelé depuis taskMotion via flag g_testAirRequested)
  // Utilise vTaskDelay → ne bloque pas le scheduler
  void runTestFromTask() {
    Serial.println(F("[Air] Test solénoïde..."));
    setSolenoidPWM(SOLENOID_PWM_FULL); vTaskDelay(pdMS_TO_TICKS(300));
    setSolenoidPWM(SOLENOID_PWM_HOLD); vTaskDelay(pdMS_TO_TICKS(700));
    setSolenoidPWM(0);

    Serial.println(F("[Air] Test servo..."));
    for (int a = SERVO_CLOSED_ANGLE; a <= SERVO_OPEN_ANGLE; a += 10) {
      airServo.write(a); vTaskDelay(pdMS_TO_TICKS(80));
    }
    for (int a = SERVO_OPEN_ANGLE; a >= SERVO_CLOSED_ANGLE; a -= 10) {
      airServo.write(a); vTaskDelay(pdMS_TO_TICKS(80));
    }
    airServo.write(SERVO_CLOSED_ANGLE);
    currentState    = IDLE;
    stateChangeTime = millis();
    Serial.println(F("[Air] Test terminé"));
  }

  // ---- PWM solénoïde (public pour accès depuis taskMotion) ---------------

  void setSolenoidPWM(uint8_t val) {
    ledcWrite(SOLENOID_LEDC_CHANNEL, val);
#if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, val > 0 ? HIGH : LOW);
#endif
  }

  // ---- Getters ------------------------------------------------------------

  SolenoidState getState()      const { return currentState; }
  bool isSolenoidOpen()         const { return currentState == OPENING || currentState == HOLDING; }
  byte getCurrentVelocity()     const { return currentVelocity; }

  const char* getStateName() const {
    switch (currentState) {
      case IDLE:             return "IDLE";
      case WAITING_POSITION: return "WAITING";
      case OPENING:          return "OPENING";
      case HOLDING:          return "HOLDING";
      case CLOSING:          return "CLOSING";
      default:               return "UNKNOWN";
    }
  }
};

#endif // AIR_CONTROL_H
