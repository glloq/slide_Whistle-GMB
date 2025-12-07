/*
 * MIDI Slide Whistle - Calibration Tool Configuration
 *
 * Configuration matérielle pour l'outil de calibration
 * DOIT être identique à la configuration de votre version principale (Fan ou Solenoid)
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

// IMPORTANT : Choisir la poulie selon vitesse/précision souhaitée (voir POULIE_ANALYSIS.md)
#define STEPS_PER_MM         44.4  // Poulie 36 dents GT2 (RECOMMANDÉ pour rapidité)
                                   // Calcul: (200 × 16) / (36 × 2) = 44.4 pas/mm
                                   // Autres options :
                                   // - 30 dents: 53.3 pas/mm (compromis)
                                   // - 20 dents: 80.0 pas/mm (trop lent)

// Vitesse et accélération (optimisées pour poulie 36 dents)
#define STEPPER_SPEED_MM_S   45.0  // Vitesse maximale en mm/seconde
#define STEPPER_ACCEL_MM_S2  30.0  // Accélération en mm/seconde²

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

// ============================================================================
// CONFIGURATION CONTRÔLE D'AIR (pour test avec T)
// ============================================================================

// Pin de contrôle air (solénoïde ou ventilateur selon votre montage)
#define AIR_CONTROL_PIN      6     // Pin de contrôle d'air
#define AIR_ACTIVE_STATE     HIGH  // État actif (HIGH ou LOW)
#define TEST_DURATION_MS     1000  // Durée du test en millisecondes

// ============================================================================
// CONFIGURATION SYSTÈME
// ============================================================================

// LED de statut (optionnel)
#define STATUS_LED_PIN       13    // LED intégrée Arduino
#define LED_ENABLED          true  // Activer/désactiver la LED de statut

// Mode debug
#define SERIAL_BAUD_RATE     115200 // Vitesse de communication série

// Limites de sécurité
#define MAX_HOMING_TIME      10000 // Timeout pour le homing (ms)

#endif // SETTINGS_H
