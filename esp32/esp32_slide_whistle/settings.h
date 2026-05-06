/*
 * settings.h — Configuration ESP32 MIDI Slide Whistle
 *
 * Adaptations ESP32 vs Arduino Leonardo original :
 *   - Pins GPIO numérotées différemment
 *   - analogWrite via ledcWrite (ESP32 Arduino Core ≥ 2.x)
 *   - Servo via bibliothèque ESP32Servo
 *   - LUT en RAM normale (pas PROGMEM, assez de SRAM)
 *   - WiFi / BLE MIDI remplace MIDIUSB
 */

#ifndef SETTINGS_H
#define SETTINGS_H

// ============================================================================
// PINS ESP32 — MOTEUR PAS À PAS
// ============================================================================

#define STEPPER_STEP_PIN      26   // GPIO26 → STEP driver
#define STEPPER_DIR_PIN       27   // GPIO27 → DIR driver
#define STEPPER_ENABLE_PIN    14   // GPIO14 → ENABLE driver (LOW = actif)

// Paramètres mécaniques
#define STEPS_PER_REVOLUTION  200  // NEMA17 standard
#define MICROSTEPS            16   // Réglage driver (A4988 / DRV8825 / TMC2209)

// Sélection poulie GT2 — modifier STEPS_PER_MM selon votre poulie
// Poulie 20 dents : 80.0 pas/mm  (lent, précis)
// Poulie 24 dents : 66.7 pas/mm
// Poulie 30 dents : 53.3 pas/mm  (bon compromis)
// Poulie 36 dents : 44.4 pas/mm  (rapide — recommandé)
// Poulie 40 dents : 40.0 pas/mm
#define STEPS_PER_MM          44.4  // Poulie 36 dents GT2

// Vitesse et accélération (pour poulie 36 dents — adapter si autre poulie)
#define STEPPER_SPEED_MM_S    45.0
#define STEPPER_ACCEL_MM_S2   800.0

// Course totale du slider
#define SLIDER_TRAVEL_MM      300.0
#define HOME_OFFSET_MM        5.0

// Sens moteur (true = inverser direction vers endstop)
#define INVERT_MOTOR_DIR      false

// ============================================================================
// PINS ESP32 — CAPTEUR FIN DE COURSE
// ============================================================================

#define ENDSTOP_PIN           34   // GPIO34 — entrée seule (pas de pull-up interne !)
                                   // → Ajouter résistance pull-up 10kΩ externe
#define ENDSTOP_ACTIVE_STATE  LOW  // LOW = capteur normalement fermé (NC)

#define HOMING_SPEED_MM_S     10.0
#define HOMING_BACKOFF_MM     2.0
#define MAX_HOMING_TIME       12000  // ms — timeout sécurité

// ============================================================================
// PINS ESP32 — CONTRÔLE AIR
// ============================================================================

// Solénoïde (PWM via LEDC)
#define SOLENOID_PIN          25   // GPIO25 — sortie PWM
#define SOLENOID_LEDC_CHANNEL 0    // Canal LEDC (0-15)
#define SOLENOID_LEDC_FREQ    1000 // Hz
#define SOLENOID_LEDC_RES     8    // bits (0-255)

// Niveaux PWM solénoïde
#define SOLENOID_PWM_FULL     255  // 100% — ouverture rapide
#define SOLENOID_PWM_HOLD     120  // ~47% — maintien anti-chauffe

// Timings machine à états (ms)
#define POSITION_WAIT_DELAY   150  // Attente moteur en position avant ouverture
#define SOLENOID_OPEN_DURATION 50  // Durée PWM pleine puissance
#define SOLENOID_CLOSE_DELAY  50   // Délai avant fermeture

// Legato : si note suivante < N ms après la précédente → garder air ouvert
#define LEGATO_THRESHOLD      300

// Servomoteur (via ESP32Servo)
#define SERVO_PIN             32   // GPIO32 — signal PWM servo
#define SERVO_CLOSED_ANGLE    30   // Degrés — flux minimum
#define SERVO_OPEN_ANGLE      150  // Degrés — flux maximum
#define SERVO_DEFAULT_ANGLE   90

// ============================================================================
// MIDI
// ============================================================================

#define MIDI_NOTE_MIN         48   // C3
#define MIDI_NOTE_MAX         84   // C6
#define MIDI_CHANNEL          1    // Canal MIDI (1-16, 0 = tous)

#define PITCHBEND_ENABLED     true
#define PITCHBEND_RANGE_SEMITONES 2.0

#define AFTERTOUCH_ENABLED    true
#define VIBRATO_DEPTH_MM      2.0
#define VIBRATO_SPEED_HZ      5.0

// Source MIDI active — choisir une seule (ou les combiner dans MIDIHandler)
#define MIDI_SOURCE_SERIAL    true   // DIN-5 via UART2 (GPIO16 RX2)
#define MIDI_SOURCE_BLE       true   // BLE MIDI (ex: connexion iPad)
#define MIDI_SERIAL_BAUD      31250  // Baud rate MIDI standard

// ============================================================================
// WIFI / WEB INTERFACE
// ============================================================================

// Hotspot (mode AP — défaut au démarrage)
#define WIFI_AP_SSID          "SlideWhistle"
#define WIFI_AP_PASSWORD      "midi1234"   // min 8 chars ou "" pour ouvert
#define WIFI_AP_CHANNEL       6
#define WIFI_AP_IP            "192.168.4.1"

// Client (mode STA — réseau externe, optionnel)
// Ces valeurs peuvent être surchargées via l'interface web (stockage NVS)
#define WIFI_STA_SSID         ""
#define WIFI_STA_PASSWORD     ""

#define WEB_SERVER_PORT       80
#define WS_PATH               "/ws"

// ============================================================================
// LED / DEBUG
// ============================================================================

#define STATUS_LED_PIN        2    // GPIO2 = LED intégrée ESP32
#define LED_ENABLED           true

#define DEBUG_MODE            true
#define SERIAL_BAUD_RATE      115200

// ============================================================================
// SÉCURITÉ
// ============================================================================

#define ENABLE_SOFT_LIMITS    true
#define WATCHDOG_TIMEOUT_MS   5000  // Chien de garde MIDI (ms sans message)

// ============================================================================
// TABLE DE LOOKUP DES POSITIONS (LUT)
// ============================================================================
// Sur ESP32, pas besoin de PROGMEM — stockée en RAM normale.
// Remplacer par les valeurs issues de la calibration web.

#define USE_POSITION_LUT      false  // true = utiliser LUT ; false = linéaire

// 37 positions : note 48 (C3) → note 84 (C6)
// Valeurs par défaut linéaires — calibrer via interface web !
const float NOTE_POSITION_LUT[37] = {
    0.00,  // 48 C3
    8.33,  // 49 C#3
   16.67,  // 50 D3
   25.00,  // 51 D#3
   33.33,  // 52 E3
   41.67,  // 53 F3
   50.00,  // 54 F#3
   58.33,  // 55 G3
   66.67,  // 56 G#3
   75.00,  // 57 A3
   83.33,  // 58 A#3
   91.67,  // 59 B3
  100.00,  // 60 C4
  108.33,  // 61 C#4
  116.67,  // 62 D4
  125.00,  // 63 D#4
  133.33,  // 64 E4
  141.67,  // 65 F4
  150.00,  // 66 F#4
  158.33,  // 67 G4
  166.67,  // 68 G#4
  175.00,  // 69 A4
  183.33,  // 70 A#4
  191.67,  // 71 B4
  200.00,  // 72 C5
  208.33,  // 73 C#5
  216.67,  // 74 D5
  225.00,  // 75 D#5
  233.33,  // 76 E5
  241.67,  // 77 F5
  250.00,  // 78 F#5
  258.33,  // 79 G5
  266.67,  // 80 G#5
  275.00,  // 81 A5
  283.33,  // 82 A#5
  291.67,  // 83 B5
  300.00   // 84 C6
};

#endif // SETTINGS_H
