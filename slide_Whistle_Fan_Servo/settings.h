/*
 * MIDI Slide Whistle - Configuration Settings
 * VERSION: VENTILATEUR + SERVOMOTEUR
 *
 * Tous les paramètres de l'instrument sont centralisés ici
 * Le ventilateur tourne EN CONTINU (toujours allumé)
 * Le servo dirige le flux d'air vers le bec (Note ON) ou ailleurs (Note OFF)
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

// Ventilateur (tourne EN CONTINU, toujours allumé)
#define FAN_PIN              6     // Pin de contrôle du ventilateur
#define FAN_ACTIVE_STATE     HIGH  // État actif (HIGH ou LOW)
#define FAN_ALWAYS_ON        true  // Le ventilateur tourne en permanence

// Servomoteur (direction du flux d'air)
// Le servo DIRIGE le flux vers le bec du pipeau (Note ON) ou ailleurs (Note OFF)
#define SERVO_PIN            9     // Pin PWM pour le servomoteur

// Positions servomoteur (en degrés, 0-180)
// Seulement 2 positions : vers le bec ou à côté
#define SERVO_NOTE_ON_ANGLE  90    // Angle dirigeant l'air VERS le bec du pipeau
#define SERVO_NOTE_OFF_ANGLE 30    // Angle dirigeant l'air AILLEURS (à côté du bec)

// Temps de transition du servo
#define SERVO_TRANSITION_DELAY 50  // Délai pour laisser le servo se positionner (ms)

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

// ============================================================================
// TABLE DE LOOKUP DES POSITIONS (LUT)
// ============================================================================

// Activer l'utilisation de la LUT (mettre false pour mapping linéaire)
#define USE_POSITION_LUT     false  // Mettre true après calibration

// Table de lookup des positions (à calibrer avec Calibration_Tool)
// Plage: Note 48 (C3) à Note 84 (C6) - 37 notes
//
// VALEURS PAR DÉFAUT (mapping linéaire) :
// Remplacer ces valeurs par celles générées par le Calibration_Tool
// pour obtenir une justesse parfaite !
//
// Pour calibrer :
// 1. Téléverser Calibration_Tool/Calibration_Tool.ino
// 2. Suivre le processus de calibration (voir README_CALIBRATION.md)
// 3. Copier-coller le code généré ici
// 4. Mettre USE_POSITION_LUT à true
// 5. Recompiler et téléverser cette version

const float NOTE_POSITION_LUT[] PROGMEM = {
  // Valeurs par défaut (linéaires) - À REMPLACER après calibration
    0.00,  // 48 - C3
    8.33,  // 49 - C#3
   16.67,  // 50 - D3
   25.00,  // 51 - D#3
   33.33,  // 52 - E3
   41.67,  // 53 - F3
   50.00,  // 54 - F#3
   58.33,  // 55 - G3
   66.67,  // 56 - G#3
   75.00,  // 57 - A3
   83.33,  // 58 - A#3
   91.67,  // 59 - B3
  100.00,  // 60 - C4
  108.33,  // 61 - C#4
  116.67,  // 62 - D4
  125.00,  // 63 - D#4
  133.33,  // 64 - E4
  141.67,  // 65 - F4
  150.00,  // 66 - F#4
  158.33,  // 67 - G4
  166.67,  // 68 - G#4
  175.00,  // 69 - A4
  183.33,  // 70 - A#4
  191.67,  // 71 - B4
  200.00,  // 72 - C5
  208.33,  // 73 - C#5
  216.67,  // 74 - D5
  225.00,  // 75 - D#5
  233.33,  // 76 - E5
  241.67,  // 77 - F5
  250.00,  // 78 - F#5
  258.33,  // 79 - G5
  266.67,  // 80 - G#5
  275.00,  // 81 - A5
  283.33,  // 82 - A#5
  291.67,  // 83 - B5
  300.00   // 84 - C6
};

#endif // SETTINGS_H
