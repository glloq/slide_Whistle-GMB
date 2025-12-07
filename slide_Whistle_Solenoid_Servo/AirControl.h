/*
 * AirControl.h
 * Gestion du flux d'air - VERSION SOLÉNOÏDE
 *
 * Le solénoïde ouvre/ferme l'arrivée d'air de manière binaire
 * Le servomoteur module le débit selon la vélocité
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <Servo.h>
#include "settings.h"

class AirControl {
private:
  Servo airServo;
  bool solenoidOpen;
  byte currentVelocity;
  unsigned long closeTime;
  bool delayedClosePending;

public:
  // Constructeur
  AirControl()
    : solenoidOpen(false),
      currentVelocity(0),
      closeTime(0),
      delayedClosePending(false) {
  }

  // Initialisation
  void begin() {
    // Configuration du solénoïde
    pinMode(SOLENOID_PIN, OUTPUT);
    setSolenoidState(false);

    // Configuration du servomoteur
    airServo.attach(SERVO_PIN);
    airServo.write(SERVO_CLOSED_ANGLE); // Fermé au départ
    delay(500); // Laisser le servo se positionner

    #if DEBUG_MODE
    Serial.println(F("AirControl: Initialized (SOLENOID mode)"));
    Serial.print(F("AirControl: Solenoid pin "));
    Serial.println(SOLENOID_PIN);
    Serial.print(F("AirControl: Servo pin "));
    Serial.println(SERVO_PIN);
    Serial.println(F("AirControl: Solenoid ON/OFF, servo modulates airflow"));
    #endif
  }

  // Démarrer l'air (appelé lors d'un Note On)
  void startAir(byte velocity) {
    currentVelocity = velocity;
    delayedClosePending = false;

    // Positionner le servo selon la vélocité AVANT d'ouvrir le solénoïde
    setServoFromVelocity(velocity);
    delay(SOLENOID_OPEN_DELAY); // Laisser le servo se positionner

    // Ouvrir le solénoïde
    setSolenoidState(true);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Air ON, velocity "));
    Serial.println(velocity);
    #endif
  }

  // Arrêter l'air (appelé lors d'un Note Off)
  void stopAir() {
    // Programmer la fermeture avec délai
    if (SOLENOID_CLOSE_DELAY > 0) {
      delayedClosePending = true;
      closeTime = millis() + SOLENOID_CLOSE_DELAY;
    } else {
      closeAirImmediate();
    }

    #if DEBUG_MODE
    Serial.println(F("AirControl: Air OFF scheduled"));
    #endif
  }

  // Fermer l'air immédiatement
  void closeAirImmediate() {
    setSolenoidState(false);
    airServo.write(SERVO_CLOSED_ANGLE);
    delayedClosePending = false;

    #if DEBUG_MODE
    Serial.println(F("AirControl: Air closed"));
    #endif
  }

  // Mettre à jour (à appeler dans loop)
  void update() {
    // Gérer la fermeture différée
    if (delayedClosePending && millis() >= closeTime) {
      closeAirImmediate();
    }
  }

  // Définir l'état du solénoïde
  void setSolenoidState(bool state) {
    solenoidOpen = state;
    digitalWrite(SOLENOID_PIN, state ? SOLENOID_ACTIVE_STATE : !SOLENOID_ACTIVE_STATE);

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, state ? HIGH : LOW);
    #endif

    #if DEBUG_MODE
    Serial.print(F("AirControl: Solenoid "));
    Serial.println(state ? "OPEN" : "CLOSED");
    #endif
  }

  // Définir manuellement l'angle du servo (0-180)
  void setServoAngle(byte angle) {
    angle = constrain(angle, SERVO_CLOSED_ANGLE, SERVO_OPEN_ANGLE);
    airServo.write(angle);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Servo angle set to "));
    Serial.println(angle);
    #endif
  }

  // Définir l'angle du servo depuis la vélocité MIDI (0-127)
  void setServoFromVelocity(byte velocity) {
    // Mapper la vélocité MIDI (0-127) vers l'angle du servo
    byte angle = map(velocity,
                    0, 127,
                    SERVO_CLOSED_ANGLE, SERVO_OPEN_ANGLE);
    setServoAngle(angle);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Velocity "));
    Serial.print(velocity);
    Serial.print(F(" -> Servo "));
    Serial.print(angle);
    Serial.println(F("°"));
    #endif
  }

  // Obtenir l'état du solénoïde
  bool isSolenoidOpen() {
    return solenoidOpen;
  }

  // Obtenir la vélocité actuelle
  byte getCurrentVelocity() {
    return currentVelocity;
  }

  // Test du servomoteur (balayage)
  void testServo() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing servo..."));
    #endif

    for (int angle = SERVO_CLOSED_ANGLE; angle <= SERVO_OPEN_ANGLE; angle += 5) {
      airServo.write(angle);
      delay(50);
    }
    for (int angle = SERVO_OPEN_ANGLE; angle >= SERVO_CLOSED_ANGLE; angle -= 5) {
      airServo.write(angle);
      delay(50);
    }
    airServo.write(SERVO_CLOSED_ANGLE);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Servo test complete"));
    #endif
  }

  // Test du solénoïde (marche/arrêt)
  void testSolenoid() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing solenoid..."));
    #endif

    setSolenoidState(true);
    delay(500);
    setSolenoidState(false);
    delay(300);
    setSolenoidState(true);
    delay(300);
    setSolenoidState(false);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Solenoid test complete"));
    #endif
  }
};

#endif // AIR_CONTROL_H
