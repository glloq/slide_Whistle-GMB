/*
 * AirControl.h
 * Gestion du flux d'air via ventilateur/solénoïde et servomoteur
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <Servo.h>
#include "settings.h"

class AirControl {
private:
  Servo airServo;
  bool fanActive;
  unsigned long fanOffTime;
  bool delayedOffPending;
  byte currentVelocity;

public:
  // Constructeur
  AirControl()
    : fanActive(false),
      fanOffTime(0),
      delayedOffPending(false),
      currentVelocity(0) {
  }

  // Initialisation
  void begin() {
    // Configuration du ventilateur/solénoïde
    pinMode(FAN_PIN, OUTPUT);
    setFanState(false);

    // Configuration du servomoteur
    #if SERVO_ENABLED
    airServo.attach(SERVO_PIN);
    airServo.write(SERVO_DEFAULT_ANGLE);
    delay(500); // Laisser le servo se positionner
    #endif

    #if DEBUG_MODE
    Serial.println(F("AirControl: Initialized"));
    Serial.print(F("AirControl: Fan pin "));
    Serial.println(FAN_PIN);
    #if SERVO_ENABLED
    Serial.print(F("AirControl: Servo pin "));
    Serial.println(SERVO_PIN);
    #endif
    #endif
  }

  // Activer l'air (appelé lors d'un Note On)
  void startAir(byte velocity) {
    currentVelocity = velocity;
    delayedOffPending = false;

    // Activer le ventilateur
    setFanState(true);

    // Ajuster le servomoteur en fonction de la vélocité
    #if SERVO_ENABLED
    setServoFromVelocity(velocity);
    #endif

    #if DEBUG_MODE
    Serial.print(F("AirControl: Air ON, velocity "));
    Serial.println(velocity);
    #endif
  }

  // Désactiver l'air (appelé lors d'un Note Off)
  void stopAir() {
    // Programmer l'arrêt avec délai
    if (AIR_OFF_DELAY > 0) {
      delayedOffPending = true;
      fanOffTime = millis() + AIR_OFF_DELAY;
    } else {
      setFanState(false);
      #if SERVO_ENABLED
      airServo.write(SERVO_DEFAULT_ANGLE);
      #endif
    }

    #if DEBUG_MODE
    Serial.println(F("AirControl: Air OFF scheduled"));
    #endif
  }

  // Mettre à jour (à appeler dans loop)
  void update() {
    // Gérer l'arrêt différé du ventilateur
    if (delayedOffPending && millis() >= fanOffTime) {
      setFanState(false);
      #if SERVO_ENABLED
      airServo.write(SERVO_DEFAULT_ANGLE);
      #endif
      delayedOffPending = false;

      #if DEBUG_MODE
      Serial.println(F("AirControl: Air OFF executed"));
      #endif
    }
  }

  // Définir l'état du ventilateur
  void setFanState(bool state) {
    fanActive = state;
    digitalWrite(FAN_PIN, state ? FAN_ACTIVE_STATE : !FAN_ACTIVE_STATE);

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, state ? HIGH : LOW);
    #endif
  }

  // Définir manuellement l'angle du servo (0-180)
  void setServoAngle(byte angle) {
    #if SERVO_ENABLED
    angle = constrain(angle, 0, 180);
    airServo.write(angle);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Servo angle set to "));
    Serial.println(angle);
    #endif
    #endif
  }

  // Définir l'angle du servo depuis la vélocité MIDI (0-127)
  void setServoFromVelocity(byte velocity) {
    #if SERVO_ENABLED
    // Mapper la vélocité MIDI (0-127) vers l'angle du servo
    byte angle = map(velocity,
                    0, 127,
                    SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
    setServoAngle(angle);
    #endif
  }

  // Obtenir l'état du ventilateur
  bool isFanActive() {
    return fanActive;
  }

  // Obtenir la vélocité actuelle
  byte getCurrentVelocity() {
    return currentVelocity;
  }

  // Test du servomoteur (balayage)
  void testServo() {
    #if SERVO_ENABLED
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing servo..."));
    #endif

    for (int angle = SERVO_MIN_ANGLE; angle <= SERVO_MAX_ANGLE; angle += 5) {
      airServo.write(angle);
      delay(50);
    }
    for (int angle = SERVO_MAX_ANGLE; angle >= SERVO_MIN_ANGLE; angle -= 5) {
      airServo.write(angle);
      delay(50);
    }
    airServo.write(SERVO_DEFAULT_ANGLE);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Servo test complete"));
    #endif
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
