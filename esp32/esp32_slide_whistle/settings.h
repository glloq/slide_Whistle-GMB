/*
 * settings.h — Configuration ESP32 MIDI Slide Whistle
 */

#ifndef SETTINGS_H
#define SETTINGS_H

// ============================================================================
// ÉTAT SYSTÈME (partagé entre .ino et WebInterface.h)
// ============================================================================

enum SystemState { SYS_INIT, SYS_HOMING, SYS_READY, SYS_ERROR };

// ============================================================================
// PINS ESP32 — MOTEUR PAS À PAS
// ============================================================================

#define STEPPER_STEP_PIN      26
#define STEPPER_DIR_PIN       27
#define STEPPER_ENABLE_PIN    14   // LOW = actif

#define STEPS_PER_REVOLUTION  200
#define MICROSTEPS            16   // A4988 / DRV8825 / TMC2209

// Poulie GT2 — choisir STEPS_PER_MM selon la poulie montée
// 20 dents : 80.0 pas/mm   24 dents : 66.7   30 dents : 53.3
// 36 dents : 44.4 pas/mm (recommandé)   40 dents : 40.0
// Calcul : (STEPS_PER_REVOLUTION × MICROSTEPS) / (dents × 2)
#define STEPS_PER_MM          44.4f  // Poulie 36 dents GT2

#define STEPPER_SPEED_MM_S    45.0f
#define STEPPER_ACCEL_MM_S2   800.0f

#define SLIDER_TRAVEL_MM      300.0f
#define HOME_OFFSET_MM        5.0f
#define INVERT_MOTOR_DIR      false

// ============================================================================
// PINS ESP32 — CAPTEUR FIN DE COURSE
// ============================================================================

// GPIO34 est input-only → résistance pull-up 10 kΩ externe OBLIGATOIRE
#define ENDSTOP_PIN           34
#define ENDSTOP_ACTIVE_STATE  LOW

#define HOMING_SPEED_MM_S     10.0f
#define HOMING_BACKOFF_MM     2.0f
#define MAX_HOMING_TIME       12000  // ms

// ============================================================================
// PINS ESP32 — CONTRÔLE AIR
// ============================================================================

#define SOLENOID_PIN          25
#define SOLENOID_LEDC_CHANNEL 0
#define SOLENOID_LEDC_FREQ    1000
#define SOLENOID_LEDC_RES     8    // 8 bits → 0-255

#define SOLENOID_PWM_FULL     255
#define SOLENOID_PWM_HOLD     120

#define POSITION_WAIT_DELAY   150  // ms — attente moteur en position
#define SOLENOID_OPEN_DURATION 50  // ms — PWM pleine puissance
#define SOLENOID_CLOSE_DELAY  50   // ms — délai avant fermeture
#define LEGATO_THRESHOLD      300  // ms — seuil détection legato

#define SERVO_PIN             32
#define SERVO_CLOSED_ANGLE    30
#define SERVO_OPEN_ANGLE      150
#define SERVO_DEFAULT_ANGLE   90

// ============================================================================
// MIDI
// ============================================================================

#define MIDI_NOTE_MIN         48   // C3
#define MIDI_NOTE_MAX         84   // C6
#define MIDI_CHANNEL          1    // 1-16, 0 = omni

#define PITCHBEND_ENABLED     true
#define PITCHBEND_RANGE_SEMITONES 2.0f

#define AFTERTOUCH_ENABLED    true
#define VIBRATO_DEPTH_MM      2.0f
#define VIBRATO_SPEED_HZ      5.0f

#define MIDI_SOURCE_SERIAL    true   // DIN-5 via UART2 GPIO16
#define MIDI_SOURCE_BLE       true   // BLE MIDI
#define MIDI_SERIAL_BAUD      31250

// ============================================================================
// WIFI / WEB INTERFACE
// ============================================================================

#define WIFI_AP_SSID          "SlideWhistle"
#define WIFI_AP_PASSWORD      "midi1234"
#define WIFI_AP_CHANNEL       6

#define WIFI_STA_SSID         ""
#define WIFI_STA_PASSWORD     ""

#define WEB_SERVER_PORT       80
#define WS_PATH               "/ws"

// ============================================================================
// LED / DEBUG / SÉCURITÉ
// ============================================================================

#define STATUS_LED_PIN        2
#define LED_ENABLED           true

#define DEBUG_MODE            true
#define SERIAL_BAUD_RATE      115200

#define ENABLE_SOFT_LIMITS    true
#define MAX_HOMING_TIME       12000
#define WATCHDOG_TIMEOUT_MS   5000  // ms sans MIDI → couper air

// ============================================================================
// TABLE DE LOOKUP DES POSITIONS (LUT)
// ============================================================================
// 'static const' = linkage interne → pas d'ODR si inclus dans plusieurs TU.
// Utilisée uniquement comme valeur initiale de runtimeCfg.lutPositions.
// La LUT active est toujours celle de RuntimeConfig (modifiable via web).

#define USE_POSITION_LUT      false  // valeur par défaut au compile

static const float NOTE_POSITION_LUT[37] = {
    0.00f,  // 48 C3
    8.33f,  // 49 C#3
   16.67f,  // 50 D3
   25.00f,  // 51 D#3
   33.33f,  // 52 E3
   41.67f,  // 53 F3
   50.00f,  // 54 F#3
   58.33f,  // 55 G3
   66.67f,  // 56 G#3
   75.00f,  // 57 A3
   83.33f,  // 58 A#3
   91.67f,  // 59 B3
  100.00f,  // 60 C4
  108.33f,  // 61 C#4
  116.67f,  // 62 D4
  125.00f,  // 63 D#4
  133.33f,  // 64 E4
  141.67f,  // 65 F4
  150.00f,  // 66 F#4
  158.33f,  // 67 G4
  166.67f,  // 68 G#4
  175.00f,  // 69 A4
  183.33f,  // 70 A#4
  191.67f,  // 71 B4
  200.00f,  // 72 C5
  208.33f,  // 73 C#5
  216.67f,  // 74 D5
  225.00f,  // 75 D#5
  233.33f,  // 76 E5
  241.67f,  // 77 F5
  250.00f,  // 78 F#5
  258.33f,  // 79 G5
  266.67f,  // 80 G#5
  275.00f,  // 81 A5
  283.33f,  // 82 A#5
  291.67f,  // 83 B5
  300.00f   // 84 C6
};

#endif // SETTINGS_H
