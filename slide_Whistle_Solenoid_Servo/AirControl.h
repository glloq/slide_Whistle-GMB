/*
 * AirControl.h
 * Gestion du flux d'air - VERSION SOLÉNOÏDE AVANCÉE
 *
 * MACHINE À ÉTATS avec PWM intelligent et gestion legato
 *
 * ÉTATS :
 *   IDLE              → Au repos, solénoïde fermé
 *   WAITING_POSITION  → Attente que moteur + servo soient en position
 *   OPENING           → Ouverture avec PWM max (courte durée)
 *   HOLDING           → Maintien ouvert avec PWM réduit (économie énergie)
 *   CLOSING           → Délai avant fermeture
 *
 * PWM :
 *   - Ouverture : 100% (12V) pendant 50ms → ouverture rapide
 *   - Maintien  : 47% (5.6V) continu → évite chauffe
 *
 * LEGATO :
 *   - Si note rapide (< 300ms) → pas de coupure
 *   - Solénoïde reste ouvert, juste moteur bouge
 */

#ifndef AIR_CONTROL_H
#define AIR_CONTROL_H

#include <Servo.h>
#include "settings.h"

// États de la machine
enum SolenoidState {
  IDLE,                // Repos, fermé
  WAITING_POSITION,    // Attente position moteur
  OPENING,             // Ouverture PWM max
  HOLDING,             // Maintien PWM réduit
  CLOSING              // Fermeture différée
};

class AirControl {
private:
  Servo airServo;

  // Machine à états
  SolenoidState currentState;
  unsigned long stateChangeTime;    // Timestamp du dernier changement d'état
  unsigned long lastNoteOnTime;     // Timestamp de la dernière note ON

  // Gestion notes
  bool noteQueued;                  // Note en attente pendant transition
  byte queuedVelocity;              // Vélocité de la note en attente

  byte currentVelocity;             // Vélocité actuelle

public:
  // Constructeur
  AirControl()
    : currentState(IDLE),
      stateChangeTime(0),
      lastNoteOnTime(0),
      noteQueued(false),
      queuedVelocity(0),
      currentVelocity(0) {
  }

  // Initialisation
  void begin() {
    // Configuration du solénoïde (PWM)
    pinMode(SOLENOID_PIN, OUTPUT);
    setSolenoidPWM(0); // Fermé au départ

    // Configuration du servomoteur
    airServo.attach(SERVO_PIN);
    airServo.write(SERVO_CLOSED_ANGLE);
    delay(500);

    currentState = IDLE;

    #if DEBUG_MODE
    Serial.println(F("AirControl: Initialized (SOLENOID mode - Advanced)"));
    Serial.print(F("AirControl: Solenoid pin "));
    Serial.print(SOLENOID_PIN);
    Serial.println(F(" (PWM)"));
    Serial.print(F("  PWM Full: "));
    Serial.print(SOLENOID_PWM_FULL);
    Serial.print(F("  Hold: "));
    Serial.println(SOLENOID_PWM_HOLD);
    Serial.print(F("AirControl: Position wait delay: "));
    Serial.print(POSITION_WAIT_DELAY);
    Serial.println(F(" ms"));
    Serial.print(F("AirControl: Legato threshold: "));
    Serial.print(LEGATO_THRESHOLD);
    Serial.println(F(" ms"));
    #endif
  }

  // Démarrer l'air (appelé lors d'un Note On)
  void startAir(byte velocity) {
    currentVelocity = velocity;
    unsigned long now = millis();

    // Détecter si c'est une note legato (rapide après la précédente)
    bool isLegato = (now - lastNoteOnTime) < LEGATO_THRESHOLD;
    lastNoteOnTime = now;

    #if DEBUG_MODE
    if (isLegato) {
      Serial.println(F("AirControl: LEGATO detected"));
    }
    #endif

    // Positionner le servo selon la vélocité
    setServoFromVelocity(velocity);

    // Gestion selon l'état actuel
    switch (currentState) {
      case IDLE:
        // Démarrage normal
        changeState(WAITING_POSITION);
        break;

      case WAITING_POSITION:
        // Note rapide pendant attente → annuler et recommencer
        #if DEBUG_MODE
        Serial.println(F("AirControl: New note during WAITING, restarting timer"));
        #endif
        changeState(WAITING_POSITION); // Reset timer
        break;

      case OPENING:
      case HOLDING:
        // Note rapide pendant ouvert → juste ajuster servo, garder ouvert (legato)
        #if DEBUG_MODE
        Serial.println(F("AirControl: New note while open, keeping open (legato)"));
        #endif
        // Rester dans l'état actuel, juste le servo bouge
        break;

      case CLOSING:
        // Note pendant fermeture → annuler fermeture, retourner HOLDING
        #if DEBUG_MODE
        Serial.println(F("AirControl: New note during CLOSING, cancelling close"));
        #endif
        changeState(HOLDING);
        setSolenoidPWM(SOLENOID_PWM_HOLD);
        break;
    }
  }

  // Arrêter l'air (appelé lors d'un Note Off)
  void stopAir() {
    // Seulement fermer si on est ouvert
    if (currentState == OPENING || currentState == HOLDING) {
      changeState(CLOSING);

      #if DEBUG_MODE
      Serial.println(F("AirControl: Note OFF, scheduling close"));
      #endif
    }
  }

  // Mettre à jour (à appeler dans loop) - MACHINE À ÉTATS
  void update() {
    unsigned long now = millis();
    unsigned long elapsed = now - stateChangeTime;

    // Transitions automatiques selon les timers
    switch (currentState) {
      case IDLE:
        // Rien à faire
        break;

      case WAITING_POSITION:
        // Attendre que moteur soit en position
        if (elapsed >= POSITION_WAIT_DELAY) {
          changeState(OPENING);
          setSolenoidPWM(SOLENOID_PWM_FULL); // Ouverture pleine puissance

          #if DEBUG_MODE
          Serial.println(F("AirControl: Position reached, OPENING solenoid (PWM FULL)"));
          #endif
        }
        break;

      case OPENING:
        // Après ouverture, passer en mode maintien avec PWM réduit
        if (elapsed >= SOLENOID_OPEN_DURATION) {
          changeState(HOLDING);
          setSolenoidPWM(SOLENOID_PWM_HOLD); // Maintien économique

          #if DEBUG_MODE
          Serial.println(F("AirControl: Opened, switching to HOLD (PWM reduced)"));
          #endif
        }
        break;

      case HOLDING:
        // Rester en maintien jusqu'à Note OFF
        break;

      case CLOSING:
        // Délai avant fermeture complète
        if (elapsed >= SOLENOID_CLOSE_DELAY) {
          changeState(IDLE);
          setSolenoidPWM(0); // Fermer
          airServo.write(SERVO_CLOSED_ANGLE);

          #if DEBUG_MODE
          Serial.println(F("AirControl: CLOSED"));
          #endif
        }
        break;
    }
  }

  // Définir le PWM du solénoïde (0-255)
  void setSolenoidPWM(byte pwmValue) {
    analogWrite(SOLENOID_PIN, pwmValue);

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, pwmValue > 0 ? HIGH : LOW);
    #endif
  }

  // Définir l'angle du servo depuis la vélocité MIDI (0-127)
  void setServoFromVelocity(byte velocity) {
    byte angle = map(velocity,
                    0, 127,
                    SERVO_CLOSED_ANGLE, SERVO_OPEN_ANGLE);
    airServo.write(angle);

    #if DEBUG_MODE
    Serial.print(F("AirControl: Servo "));
    Serial.print(angle);
    Serial.print(F("° (velocity "));
    Serial.print(velocity);
    Serial.println(F(")"));
    #endif
  }

  // Changer d'état (avec logging)
  void changeState(SolenoidState newState) {
    currentState = newState;
    stateChangeTime = millis();

    #if DEBUG_MODE
    Serial.print(F("AirControl: State -> "));
    switch (newState) {
      case IDLE:              Serial.println(F("IDLE")); break;
      case WAITING_POSITION:  Serial.println(F("WAITING_POSITION")); break;
      case OPENING:           Serial.println(F("OPENING")); break;
      case HOLDING:           Serial.println(F("HOLDING")); break;
      case CLOSING:           Serial.println(F("CLOSING")); break;
    }
    #endif
  }

  // Obtenir l'état actuel
  SolenoidState getState() {
    return currentState;
  }

  // Vérifier si le solénoïde est ouvert
  bool isSolenoidOpen() {
    return (currentState == OPENING || currentState == HOLDING);
  }

  // Obtenir la vélocité actuelle
  byte getCurrentVelocity() {
    return currentVelocity;
  }

  // Test du servomoteur
  void testServo() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing servo..."));
    #endif

    for (int angle = SERVO_CLOSED_ANGLE; angle <= SERVO_OPEN_ANGLE; angle += 10) {
      airServo.write(angle);
      delay(100);
    }
    for (int angle = SERVO_OPEN_ANGLE; angle >= SERVO_CLOSED_ANGLE; angle -= 10) {
      airServo.write(angle);
      delay(100);
    }
    airServo.write(SERVO_CLOSED_ANGLE);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Servo test complete"));
    #endif
  }

  // Test du solénoïde (PWM)
  void testSolenoid() {
    #if DEBUG_MODE
    Serial.println(F("AirControl: Testing solenoid PWM..."));
    Serial.println(F("  Opening (PWM FULL)"));
    #endif

    setSolenoidPWM(SOLENOID_PWM_FULL);
    delay(500);

    #if DEBUG_MODE
    Serial.println(F("  Holding (PWM reduced)"));
    #endif

    setSolenoidPWM(SOLENOID_PWM_HOLD);
    delay(1000);

    #if DEBUG_MODE
    Serial.println(F("  Closing"));
    #endif

    setSolenoidPWM(0);
    delay(500);

    #if DEBUG_MODE
    Serial.println(F("AirControl: Solenoid test complete"));
    #endif
  }

  // Obtenir le nom de l'état actuel (pour debug)
  const char* getStateName() {
    switch (currentState) {
      case IDLE:              return "IDLE";
      case WAITING_POSITION:  return "WAITING";
      case OPENING:           return "OPENING";
      case HOLDING:           return "HOLDING";
      case CLOSING:           return "CLOSING";
      default:                return "UNKNOWN";
    }
  }
};

#endif // AIR_CONTROL_H
