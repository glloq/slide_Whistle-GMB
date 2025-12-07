/*
 * AirControl.h
 * Gestion du flux d'air - VERSION VENTILATEUR
 *
 * Le ventilateur tourne en continu pendant les notes
 * Le servomoteur module le débit selon la vélocité
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <Servo.h>
#include "settings.h"

class AirControl {
private:
  Servo airServo;
  bool fanRunning;
  byte currentVelocity;
  unsigned long servoCloseTime;
  bool delayedClosePending;

public:
  // Constructeur
  AirControl()
    : fanRunning(false),
      currentVelocity(0),
      servoCloseTime(0),
      delayedClosePending(false) {
  }

  // Initialisation
  void begin() {
    // Configuration du ventilateur
    pinMode(FAN_PIN, OUTPUT);
    setFanState(false);

    // Configuration du servomoteur
    airServo.attach(SERVO_PIN);
    airServo.write(SERVO_CLOSED_ANGLE); // Fermé au départ
    delay(500); // Laisser le servo se positionner

    #if DEBUG_MODE
    Serial.println(F("AirControl: Initialized (FAN mode)"));
    Serial.print(F("AirControl: Fan pin "));
    Serial.println(FAN_PIN);
    Serial.print(F("AirControl: Servo pin "));
    Serial.println(SERVO_PIN);
    Serial.println(F("AirControl: Fan runs continuously, servo modulates airflow"));
    #endif
  }

  // Démarrer l'air (appelé lors d'un Note On)
  void startAir(byte velocity) {
    currentVelocity = velocity;
    delayedClosePending = false;

    // Démarrer le ventilateur
    if (!fanRunning) {
      setFanState(true);
      delay(FAN_STARTUP_DELAY); // Laisser le ventilateur démarrer
    }

    // Ouvrir le servo selon la vélocité
    setServoFromVelocity(velocity);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Air ON, velocity "));
    Serial.println(velocity);
    #endif
  }

  // Arrêter l'air (appelé lors d'un Note Off)
  void stopAir() {
    // Programmer la fermeture du servo avec délai
    if (SERVO_CLOSE_DELAY > 0) {
      delayedClosePending = true;
      servoCloseTime = millis() + SERVO_CLOSE_DELAY;
    } else {
      closeAir();
    }

    #if DEBUG_MODE
    Serial.println(F("AirControl: Air OFF scheduled"));
    #endif
  }

  // Fermer l'air immédiatement
  void closeAir() {
    airServo.write(SERVO_CLOSED_ANGLE);
    setFanState(false);
    delayedClosePending = false;

    #if DEBUG_MODE
    Serial.println(F("AirControl: Air closed"));
    #endif
  }

  // Mettre à jour (à appeler dans loop)
  void update() {
    // Gérer la fermeture différée du servo
    if (delayedClosePending && millis() >= servoCloseTime) {
      closeAir();
    }
  }

  // Définir l'état du ventilateur
  void setFanState(bool state) {
    fanRunning = state;
    digitalWrite(FAN_PIN, state ? FAN_ACTIVE_STATE : !FAN_ACTIVE_STATE);

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, state ? HIGH : LOW);
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

  // Obtenir l'état du ventilateur
  bool isFanRunning() {
    return fanRunning;
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

  // Test du ventilateur (marche/arrêt)
  void testFan() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing fan..."));
    #endif

    setFanState(true);
    delay(1000);
    setFanState(false);
    delay(500);
    setFanState(true);
    delay(500);
    setFanState(false);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Fan test complete"));
    #endif
  }
};

#endif // AIR_CONTROL_H
