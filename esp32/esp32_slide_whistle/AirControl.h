/*
 * AirControl.h — Solénoïde + Servo (ESP32)
 *
 * Machine à états identique à la version Arduino, adaptations ESP32 :
 *   - analogWrite() disponible (ESP32 Arduino Core ≥ 2.x via LEDC)
 *   - Servo via bibliothèque ESP32Servo
 *
 * États :
 *   IDLE             → solénoïde fermé, au repos
 *   WAITING_POSITION → attente moteur en position
 *   OPENING          → PWM 100% pendant SOLENOID_OPEN_DURATION ms
 *   HOLDING          → PWM réduit (anti-chauffe)
 *   CLOSING          → délai avant fermeture
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
  SolenoidState currentState;
  unsigned long stateChangeTime;
  unsigned long lastNoteOnTime;
  byte currentVelocity;

  void changeState(SolenoidState s) {
    currentState = s;
    stateChangeTime = millis();
#if DEBUG_MODE
    const char* names[] = {"IDLE","WAITING","OPENING","HOLDING","CLOSING"};
    Serial.printf("[Air] → %s\n", names[s]);
#endif
  }

  void setSolenoidPWM(uint8_t val) {
    analogWrite(SOLENOID_PIN, val);
#if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, val > 0 ? HIGH : LOW);
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
  AirControl()
    : currentState(IDLE),
      stateChangeTime(0),
      lastNoteOnTime(0),
      currentVelocity(0) {}

  void begin() {
    // LEDC init pour analogWrite sur GPIO solénoïde
    ledcSetup(SOLENOID_LEDC_CHANNEL, SOLENOID_LEDC_FREQ, SOLENOID_LEDC_RES);
    ledcAttachPin(SOLENOID_PIN, SOLENOID_LEDC_CHANNEL);
    setSolenoidPWM(0);

    // Servo
    ESP32PWM::allocateTimer(0);
    airServo.setPeriodHertz(50);
    airServo.attach(SERVO_PIN, 500, 2400);
    airServo.write(SERVO_CLOSED_ANGLE);
    delay(300);

#if DEBUG_MODE
    Serial.println(F("[Air] Init OK (solénoïde + servo)"));
    Serial.printf("[Air] PWM full=%d hold=%d  wait=%dms legato=%dms\n",
                  SOLENOID_PWM_FULL, SOLENOID_PWM_HOLD,
                  POSITION_WAIT_DELAY, LEGATO_THRESHOLD);
#endif
  }

  void startAir(byte velocity) {
    currentVelocity = velocity;
    lastNoteOnTime = millis();
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
        // Legato — air reste ouvert, juste le servo bouge
#if DEBUG_MODE
        Serial.println(F("[Air] Legato — air maintenu"));
#endif
        break;

      case CLOSING:
        // Annuler fermeture
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

  void update() {
    unsigned long elapsed = millis() - stateChangeTime;

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

  // Activer l'air directement (mode test web / calibration)
  void forceOpen(byte velocity = 100) {
    setServoFromVelocity(velocity);
    setSolenoidPWM(SOLENOID_PWM_FULL);
    delay(SOLENOID_OPEN_DURATION);
    setSolenoidPWM(SOLENOID_PWM_HOLD);
    currentState = HOLDING;
    stateChangeTime = millis();
  }

  void forceClose() {
    setSolenoidPWM(0);
    airServo.write(SERVO_CLOSED_ANGLE);
    currentState = IDLE;
  }

  void testSequence() {
    Serial.println(F("[Air] Test solénoïde..."));
    setSolenoidPWM(SOLENOID_PWM_FULL); delay(300);
    setSolenoidPWM(SOLENOID_PWM_HOLD); delay(700);
    setSolenoidPWM(0);

    Serial.println(F("[Air] Test servo..."));
    for (int a = SERVO_CLOSED_ANGLE; a <= SERVO_OPEN_ANGLE; a += 10) {
      airServo.write(a); delay(80);
    }
    for (int a = SERVO_OPEN_ANGLE; a >= SERVO_CLOSED_ANGLE; a -= 10) {
      airServo.write(a); delay(80);
    }
    airServo.write(SERVO_CLOSED_ANGLE);
    Serial.println(F("[Air] Test terminé"));
  }

  SolenoidState getState()  { return currentState; }
  bool isSolenoidOpen()     { return currentState == OPENING || currentState == HOLDING; }
  byte getCurrentVelocity() { return currentVelocity; }

  const char* getStateName() {
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
