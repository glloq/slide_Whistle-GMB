/*
 * MIDI Slide Whistle - Configuration Settings
 * Tous les paramètres de l'instrument sont centralisés ici
 */

#ifndef SETTINGS_H
#define SETTINGS_H

// ============================================================================
// CONFIGURATION MOTEUR PAS À PAS
// ============================================================================

// Pins du driver moteur (compatible A4988, DRV8825, etc.)
#define STEPPER_STEP_PIN      2    // Pin STEP
#define STEPPER_DIR_PIN       3    // Pin DIR
#define STEPPER_ENABLE_PIN    4    // Pin ENABLE (optionnel)

// Paramètres mécaniques
#define STEPS_PER_REVOLUTION  200  // Nombre de pas par tour (moteur NEMA standard)
#define MICROSTEPS           16    // Microstepping du driver (1, 2, 4, 8, 16, 32)
#define STEPPER_SPEED        1000  // Vitesse en pas/seconde
#define STEPPER_ACCELERATION 500   // Accélération en pas/seconde²

// Course du slider (en pas)
#define TOTAL_TRAVEL_STEPS   10000 // Course totale du slider en pas
#define HOME_OFFSET_STEPS    500   // Offset depuis le capteur fin de course

// ============================================================================
// CONFIGURATION CAPTEUR FIN DE COURSE
// ============================================================================

#define ENDSTOP_PIN          5     // Pin du capteur fin de course
#define ENDSTOP_ACTIVE_STATE LOW   // État actif du capteur (LOW = normalement fermé)

// Paramètres de homing
#define HOMING_SPEED         400   // Vitesse de retour à l'origine (pas/sec)
#define HOMING_BACKOFF_STEPS 100   // Recul après détection du capteur

// ============================================================================
// CONFIGURATION MIDI
// ============================================================================

// Note MIDI de référence et plage
#define MIDI_NOTE_MIN        48    // Note MIDI la plus basse (C3)
#define MIDI_NOTE_MAX        84    // Note MIDI la plus haute (C6)
#define MIDI_CHANNEL         1     // Canal MIDI à écouter (1-16, 0 = tous)

// Mapping notes -> positions
// Position calculée linéairement entre 0 et TOTAL_TRAVEL_STEPS
// Vous pouvez ajuster la courbe dans le code si nécessaire

// ============================================================================
// CONFIGURATION CONTRÔLE D'AIR
// ============================================================================

// Ventilateur / Solénoïde
#define FAN_PIN              6     // Pin de contrôle du ventilateur/solénoïde
#define FAN_ACTIVE_STATE     HIGH  // État actif (HIGH ou LOW)

// Servomoteur (contrôle du débit)
#define SERVO_PIN            9     // Pin PWM pour le servomoteur
#define SERVO_ENABLED        true  // Activer/désactiver le servomoteur

// Positions servomoteur (en degrés, 0-180)
#define SERVO_MIN_ANGLE      30    // Angle minimum (débit minimum)
#define SERVO_MAX_ANGLE      150   // Angle maximum (débit maximum)
#define SERVO_DEFAULT_ANGLE  90    // Position par défaut

// Mapping vélocité MIDI -> angle servo
// Vélocité MIDI: 0-127
// La vélocité MIDI contrôle l'angle du servo pour varier le débit

// ============================================================================
// CONFIGURATION SYSTÈME
// ============================================================================

// Délai pour l'arrêt du ventilateur après note off (ms)
#define AIR_OFF_DELAY        50    // Petit délai pour éviter les coupures brusques

// LED de statut (optionnel)
#define STATUS_LED_PIN       13    // LED intégrée Arduino
#define LED_ENABLED          true  // Activer/désactiver la LED de statut

// Mode debug (affiche les informations sur Serial)
#define DEBUG_MODE           true  // true = affiche les messages de debug
#define SERIAL_BAUD_RATE     115200 // Vitesse de communication série

// ============================================================================
// CALIBRATION AVANCÉE
// ============================================================================

// Courbe de réponse du slider (linéaire par défaut)
// Options: LINEAR, LOGARITHMIC, EXPONENTIAL
#define POSITION_CURVE       LINEAR

// Compensation de la vélocité
#define VELOCITY_COMPENSATION true // Ajuste la position selon la vélocité

// Limites de sécurité
#define ENABLE_SOFT_LIMITS   true  // Active les limites logicielles
#define MAX_HOMING_TIME      10000 // Timeout pour le homing (ms)

#endif // SETTINGS_H
