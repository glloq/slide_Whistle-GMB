/*
 * MIDI Slide Whistle - Configuration Settings
 * VERSION: SOLÉNOÏDE + SERVOMOTEUR (Avancé)
 *
 * Tous les paramètres de l'instrument sont centralisés ici
 *
 * FONCTIONNALITÉS AVANCÉES :
 * - Buffer/délai avant ouverture (moteur en position)
 * - PWM intelligent (ouverture forte, maintien économique)
 * - Gestion legato (notes rapides sans coupure)
 * - Machine à états complète
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

// IMPORTANT : Choisir la poulie selon vitesse/précision souhaitée
// Voir POULIE_ANALYSIS.md pour analyse complète
#define STEPS_PER_MM         44.4  // Poulie 36 dents GT2 (RECOMMANDÉ - rapide)
                                   // Calcul: (200 × 16) / (36 × 2) = 44.4 pas/mm
                                   //
                                   // Autres poulies disponibles :
                                   // - 20 dents : 80 pas/mm (lent, déconseillé)
                                   // - 24 dents : 66.7 pas/mm (acceptable)
                                   // - 30 dents : 53.3 pas/mm (bon compromis)
                                   // - 36 dents : 44.4 pas/mm (optimal vitesse)

// Vitesse et accélération
#define STEPPER_SPEED_MM_S   45.0  // Vitesse en mm/seconde (adapté poulie 36 dents)
#define STEPPER_ACCEL_MM_S2  30.0  // Accélération en mm/seconde² (plus rapide)

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
// CONFIGURATION CONTRÔLE D'AIR - VERSION SOLÉNOÏDE AVANCÉE
// ============================================================================

// Solénoïde/Valve (ouverture/fermeture avec PWM)
#define SOLENOID_PIN         6     // Pin de contrôle du solénoïde (DOIT être PWM!)

// PWM du solénoïde (0-255)
#define SOLENOID_PWM_FULL    255   // PWM ouverture (100% = 12V)
#define SOLENOID_PWM_HOLD    120   // PWM maintien (47% = 5.6V, réduit chauffe)

// Délais machine à états (ms)
// OPTIMISÉ pour poulie 36 dents (voir POULIE_ANALYSIS.md)
#define POSITION_WAIT_DELAY     150  // Délai avant ouverture (moteur + servo en position)
                                     // Poulie 36 dents : 150ms (notes rapprochées)
                                     // Poulie 30 dents : 180ms
                                     // Poulie 20 dents : 500ms (déconseillé)
                                     //
                                     // Peut descendre à 100ms si notes < 3 demi-tons
                                     // Augmenter si sauts d'octave fréquents (300ms)

#define SOLENOID_OPEN_DURATION  50   // Durée PWM pleine puissance (ouverture rapide)
#define SOLENOID_CLOSE_DELAY    50   // Délai avant fermeture (éviter coupure brusque)

// Gestion notes rapides (legato)
#define LEGATO_THRESHOLD        300  // Si note < 300ms après précédente = legato (pas de coupure)

// Servomoteur (contrôle du débit)
#define SERVO_PIN            9     // Pin PWM pour le servomoteur

// Positions servomoteur (en degrés, 0-180)
#define SERVO_CLOSED_ANGLE   30    // Angle fermé (débit minimum)
#define SERVO_OPEN_ANGLE     150   // Angle ouvert (débit maximum)
#define SERVO_DEFAULT_ANGLE  90    // Position par défaut

// Mapping vélocité MIDI -> angle servo
// Vélocité MIDI: 0-127 contrôle l'angle du servo

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
