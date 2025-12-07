/*
 * AirControl.h
 * Gestion du flux d'air - VERSION VENTILATEUR
 *
 * Le ventilateur tourne EN CONTINU (toujours allumé)
 * Le servomoteur DIRIGE le flux d'air :
 *   - Note ON  : Servo pointe vers le bec du pipeau → son produit
 *   - Note OFF : Servo pointe ailleurs → pas de son
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <Servo.h>
#include "settings.h"

class AirControl {
private:
  Servo airServo;
  bool airDirectedToBeak;  // true si l'air est dirigé vers le bec

public:
  // Constructeur
  AirControl()
    : airDirectedToBeak(false) {
  }

  // Initialisation
  void begin() {
    // Configuration du ventilateur
    pinMode(FAN_PIN, OUTPUT);

    #if FAN_ALWAYS_ON
    // Démarrer le ventilateur immédiatement et le laisser tourner
    digitalWrite(FAN_PIN, FAN_ACTIVE_STATE);
    #if DEBUG_MODE
    Serial.println(F("AirControl: Fan started (runs continuously)"));
    #endif
    #endif

    // Configuration du servomoteur
    airServo.attach(SERVO_PIN);
    airServo.write(SERVO_NOTE_OFF_ANGLE); // Air dirigé AILLEURS au départ
    delay(500); // Laisser le servo se positionner

    #if DEBUG_MODE
    Serial.println(F("AirControl: Initialized (FAN mode)"));
    Serial.print(F("AirControl: Fan pin "));
    Serial.print(FAN_PIN);
    Serial.println(F(" - Always ON"));
    Serial.print(F("AirControl: Servo pin "));
    Serial.println(SERVO_PIN);
    Serial.println(F("AirControl: Servo directs airflow to beak (ON) or away (OFF)"));
    Serial.print(F("  Note ON angle:  "));
    Serial.print(SERVO_NOTE_ON_ANGLE);
    Serial.println(F("° (towards beak)"));
    Serial.print(F("  Note OFF angle: "));
    Serial.print(SERVO_NOTE_OFF_ANGLE);
    Serial.println(F("° (away from beak)"));
    #endif
  }

  // Diriger l'air vers le bec (appelé lors d'un Note On)
  // La vélocité n'a pas d'effet sur l'angle (juste 2 positions)
  void startAir(byte velocity) {
    // Ignorer la vélocité, juste positionner le servo vers le bec
    (void)velocity; // Éviter warning unused parameter

    airServo.write(SERVO_NOTE_ON_ANGLE);
    airDirectedToBeak = true;

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, HIGH);
    #endif

    #if DEBUG_MODE
    Serial.print(F("AirControl: Air directed TO BEAK ("));
    Serial.print(SERVO_NOTE_ON_ANGLE);
    Serial.println(F("°)"));
    #endif
  }

  // Diriger l'air ailleurs (appelé lors d'un Note Off)
  void stopAir() {
    airServo.write(SERVO_NOTE_OFF_ANGLE);
    airDirectedToBeak = false;

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, LOW);
    #endif

    #if DEBUG_MODE
    Serial.print(F("AirControl: Air directed AWAY ("));
    Serial.print(SERVO_NOTE_OFF_ANGLE);
    Serial.println(F("°)"));
    #endif
  }

  // Mettre à jour (à appeler dans loop)
  // Pas de logique complexe ici pour cette version
  void update() {
    // Rien à faire en continu
  }

  // Obtenir l'état de la direction de l'air
  bool isAirDirectedToBeak() {
    return airDirectedToBeak;
  }

  // Obtenir l'état du ventilateur (toujours ON dans cette version)
  bool isFanRunning() {
    #if FAN_ALWAYS_ON
    return true;
    #else
    return false;
    #endif
  }

  // Définir manuellement l'angle du servo (pour tests)
  void setServoAngle(byte angle) {
    angle = constrain(angle, 0, 180);
    airServo.write(angle);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Servo angle set to "));
    Serial.println(angle);
    #endif
  }

  // Test du servomoteur (balayage entre les 2 positions)
  void testServo() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing servo..."));
    Serial.println(F("  Moving to NOTE_OFF position"));
    #endif

    airServo.write(SERVO_NOTE_OFF_ANGLE);
    delay(1000);

    #if DEBUG_MODE
    Serial.println(F("  Moving to NOTE_ON position"));
    #endif

    airServo.write(SERVO_NOTE_ON_ANGLE);
    delay(1000);

    #if DEBUG_MODE
    Serial.println(F("  Back to NOTE_OFF position"));
    #endif

    airServo.write(SERVO_NOTE_OFF_ANGLE);
    delay(500);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Servo test complete"));
    #endif
  }

  // Test du ventilateur (déjà en marche)
  void testFan() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Fan is running continuously"));
    Serial.print(F("  Fan state: "));
    Serial.println(digitalRead(FAN_PIN) == FAN_ACTIVE_STATE ? "ON" : "OFF");
    #endif
  }
};

#endif // AIR_CONTROL_H
