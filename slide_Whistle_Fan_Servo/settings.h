/*
 * MIDI Slide Whistle - Configuration Settings
 * VERSION: VENTILATEUR + SERVOMOTEUR
 *
 * Tous les paramètres de l'instrument sont centralisés ici
 * Le ventilateur tourne en continu, le servo module le débit
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
#define STEPS_PER_MM         40.0  // Nombre de pas par millimètre (à calibrer)
                                   // Exemple: poulie 20 dents GT2 = (200*16)/(20*2) = 80 pas/mm

// Vitesse et accélération
#define STEPPER_SPEED_MM_S   25.0  // Vitesse en mm/seconde
#define STEPPER_ACCEL_MM_S2  15.0  // Accélération en mm/seconde²

// Course du slider
#define SLIDER_TRAVEL_MM     300.0 // Course totale du slider en millimètres
#define HOME_OFFSET_MM       5.0   // Offset depuis le capteur fin de course (mm)

// Direction du moteur
#define INVERT_MOTOR_DIR     false // true = inverser le sens vers le capteur FDC

// ============================================================================
// CONFIGURATION CAPTEUR FIN DE COURSE
// ============================================================================

#define ENDSTOP_PIN          5     // Pin du capteur fin de course
#define ENDSTOP_ACTIVE_STATE LOW   // État actif du capteur (LOW = normalement fermé)

// Paramètres de homing
#define HOMING_SPEED_MM_S    10.0  // Vitesse de retour à l'origine (mm/sec)
#define HOMING_BACKOFF_MM    2.0   // Recul après détection du capteur (mm)

// ============================================================================
// CONFIGURATION MIDI
// ============================================================================

// Note MIDI de référence et plage
#define MIDI_NOTE_MIN        48    // Note MIDI la plus basse (C3)
#define MIDI_NOTE_MAX        84    // Note MIDI la plus haute (C6)
#define MIDI_CHANNEL         1     // Canal MIDI à écouter (1-16, 0 = tous)

// Pitch Bend
#define PITCHBEND_ENABLED    true  // Activer le pitch bend
#define PITCHBEND_RANGE_SEMITONES 2.0  // Plage en demi-tons (+/- 2 = 4 demi-tons total)

// Aftertouch (vibrato)
#define AFTERTOUCH_ENABLED   true  // Activer l'aftertouch
#define VIBRATO_DEPTH_MM     2.0   // Profondeur du vibrato en mm
#define VIBRATO_SPEED_HZ     5.0   // Fréquence du vibrato en Hz

// ============================================================================
// CONFIGURATION CONTRÔLE D'AIR - VERSION VENTILATEUR
// ============================================================================

// Ventilateur (tourne en continu)
#define FAN_PIN              6     // Pin de contrôle du ventilateur
#define FAN_ACTIVE_STATE     HIGH  // État actif (HIGH ou LOW)
#define FAN_STARTUP_DELAY    500   // Délai de démarrage du ventilateur (ms)

// Servomoteur (contrôle du débit)
#define SERVO_PIN            9     // Pin PWM pour le servomoteur

// Positions servomoteur (en degrés, 0-180)
#define SERVO_CLOSED_ANGLE   30    // Angle fermé (pas d'air)
#define SERVO_OPEN_ANGLE     150   // Angle ouvert (débit maximum)
#define SERVO_DEFAULT_ANGLE  90    // Position par défaut

// Mapping vélocité MIDI -> angle servo
// Vélocité MIDI: 0-127
// La vélocité MIDI contrôle l'angle du servo pour varier le débit

// Temps de réponse du servo
#define SERVO_CLOSE_DELAY    100   // Délai avant fermeture après note off (ms)

// ============================================================================
// CONFIGURATION SYSTÈME
// ============================================================================

// LED de statut (optionnel)
#define STATUS_LED_PIN       13    // LED intégrée Arduino
#define LED_ENABLED          true  // Activer/désactiver la LED de statut

// Mode debug (affiche les informations sur Serial)
#define DEBUG_MODE           true  // true = affiche les messages de debug
#define SERIAL_BAUD_RATE     115200 // Vitesse de communication série

// ============================================================================
// CALIBRATION AVANCÉE
// ============================================================================

// Limites de sécurité
#define ENABLE_SOFT_LIMITS   true  // Active les limites logicielles
#define MAX_HOMING_TIME      10000 // Timeout pour le homing (ms)

// Lissage du pitch bend (ms)
#define PITCHBEND_SMOOTH_TIME 20   // Temps de lissage des mouvements

#endif // SETTINGS_H
