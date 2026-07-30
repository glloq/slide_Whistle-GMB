/*
 * settings.h — Configuration ESP32 MIDI Slide Whistle (multi-flûte)
 *
 * Refactor v3 :
 *   - Architecture multi-instances : tableau DEFAULT_FLUTE_CONFIGS[]
 *   - Pins / LEDC channels passés par struct (plus en #define hardcodé)
 *   - Module pression / pompe / réservoir (PressureControl)
 *   - Les anciennes #define deviennent les valeurs par défaut de la flûte 0
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// ============================================================================
// COMPILATION FLAGS
// ============================================================================

#define MAX_FLUTES                 4      // borne haute compile-time
#define DEFAULT_FLUTE_COUNT        1      // nombre de flûtes activées au boot
#define ENABLE_PRESSURE_CONTROL    true   // module pompe + capteur pression
#define DEBUG_MODE                 true
#define SERIAL_BAUD_RATE           115200

// ============================================================================
// ÉTAT SYSTÈME (partagé)
// ============================================================================

enum SystemState { SYS_INIT, SYS_HOMING, SYS_READY, SYS_ERROR };

// ============================================================================
// VALEURS PAR DÉFAUT — STEPPER (par flûte)
// ============================================================================

#define STEPS_PER_REVOLUTION  200
#define MICROSTEPS            16

// Poulie GT2 — choisir STEPS_PER_MM selon la poulie montée
// 20 dents : 80.0 pas/mm   24 dents : 66.7   30 dents : 53.3
// 36 dents : 44.4 pas/mm (recommandé)   40 dents : 40.0
#define DEFAULT_STEPS_PER_MM       44.4f
#define DEFAULT_STEPPER_SPEED      45.0f
#define DEFAULT_STEPPER_ACCEL      800.0f
#define DEFAULT_SLIDER_TRAVEL      300.0f
#define DEFAULT_HOME_OFFSET        5.0f

#define DEFAULT_HOMING_SPEED       10.0f
#define DEFAULT_HOMING_BACKOFF     2.0f
#define DEFAULT_MAX_HOMING_TIME    12000

// ============================================================================
// VALEURS PAR DÉFAUT — AIR (par flûte)
// ============================================================================

#define DEFAULT_PWM_FULL           255
#define DEFAULT_PWM_HOLD           120
#define DEFAULT_LEDC_FREQ          1000
#define DEFAULT_LEDC_RES           8

#define DEFAULT_POSITION_WAIT      150
#define DEFAULT_OPEN_DURATION      50
#define DEFAULT_CLOSE_DELAY        50
#define DEFAULT_LEGATO_THRESHOLD   300

#define DEFAULT_SERVO_CLOSED_ANGLE 30
#define DEFAULT_SERVO_OPEN_ANGLE   150

// ============================================================================
// VALEURS PAR DÉFAUT — MIDI
// ============================================================================

#define DEFAULT_MIDI_NOTE_MIN      48     // C3
#define DEFAULT_MIDI_NOTE_MAX      84     // C6
#define MIDI_LUT_SIZE              (DEFAULT_MIDI_NOTE_MAX - DEFAULT_MIDI_NOTE_MIN + 1)

#define PITCHBEND_RANGE_SEMITONES  2.0f
#define VIBRATO_DEPTH_MM           2.0f
#define VIBRATO_SPEED_HZ           5.0f

// These two sources can be overridden from the build system (e.g. a
// PlatformIO `-DMIDI_SOURCE_BLE=0` flag) so a build can drop BLE without
// editing this file. The #ifndef guards make the -D flag win.
#ifndef MIDI_SOURCE_SERIAL
#define MIDI_SOURCE_SERIAL         true
#endif
#ifndef MIDI_SOURCE_BLE
#define MIDI_SOURCE_BLE            true
#endif
#define MIDI_SERIAL_BAUD           31250

// ============================================================================
// CONFIG MATÉRIELLE PAR FLÛTE — pins + plage MIDI
// ============================================================================

struct FluteHwConfig {
  // Identité
  const char* name;          // "Flute 0", "Soprano"...
  uint8_t     midiChannel;   // 1-16, 0 = OMNI

  // Plage MIDI
  uint8_t noteMin;           // ex. 48 (C3)
  uint8_t noteMax;           // ex. 84 (C6)

  // Stepper
  int8_t  stepperStepPin;
  int8_t  stepperDirPin;
  int8_t  stepperEnablePin;
  bool    invertMotorDir;

  // Endstop
  int8_t  endstopPin;
  uint8_t endstopActiveState;  // LOW ou HIGH

  // Mécanique
  float   stepsPerMM;
  float   sliderTravelMM;
  float   homeOffsetMM;

  // Air — solénoïde PWM (canal LEDC unique par flûte)
  int8_t  solenoidPin;
  uint8_t solenoidLedcChannel;

  // Air — servo direction (timer ESP32Servo unique par flûte)
  int8_t  servoPin;
  uint8_t servoTimerIndex;     // 0..3 — un par flûte

  // Profil de vélocité (mapping vel→servo angle)
  // 0 = linéaire, 1 = quadratique (musical), 2 = exponentielle
  uint8_t velocityCurve;
};

// ----- Configs par défaut --------------------------------------------------
// NB : ESP32 a 16 canaux LEDC + 4 timers ESP32Servo.
//      - Canaux LEDC 0..7 = groupe haute-vitesse (HS, partagent 4 timers HW)
//      - Canaux LEDC 8..15 = groupe basse-vitesse (LS, partagent 4 timers HW)
//      - ESP32Servo utilise les timers HS pour le 50 Hz
//      → On met les solénoïdes en LS (canaux 8,10,12,14) et la pompe en LS 15
//        pour éviter tout conflit de fréquence avec les servos.
//      Au-delà de 4 flûtes, prévoir un PCA9685 (I²C) ou shift register.

static const FluteHwConfig DEFAULT_FLUTE_CONFIGS[MAX_FLUTES] = {
  // ---- Flûte 0 (compatible v2 single-flûte) ----
  { "Flute 0", 1,
    DEFAULT_MIDI_NOTE_MIN, DEFAULT_MIDI_NOTE_MAX,
    /*step*/26, /*dir*/27, /*en*/14, /*invert*/false,
    /*endstop*/34, /*active*/LOW,
    DEFAULT_STEPS_PER_MM, DEFAULT_SLIDER_TRAVEL, DEFAULT_HOME_OFFSET,
    /*solenoid*/25, /*ledc ch*/8,
    /*servo*/32,    /*servo timer*/0,
    /*velCurve*/1
  },
  // ---- Flûte 1 (exemple — désactivée par défaut) ----
  { "Flute 1", 2,
    DEFAULT_MIDI_NOTE_MIN, DEFAULT_MIDI_NOTE_MAX,
    /*step*/13, /*dir*/12, /*en*/15, /*invert*/false,
    /*endstop*/35, /*active*/LOW,
    DEFAULT_STEPS_PER_MM, DEFAULT_SLIDER_TRAVEL, DEFAULT_HOME_OFFSET,
    /*solenoid*/19, /*ledc ch*/10,
    /*servo*/18,    /*servo timer*/1,
    /*velCurve*/1
  },
  // ---- Flûte 2 ----
  { "Flute 2", 3,
    DEFAULT_MIDI_NOTE_MIN, DEFAULT_MIDI_NOTE_MAX,
    /*step*/5,  /*dir*/17, /*en*/16, /*invert*/false,
    /*endstop*/39, /*active*/LOW,
    DEFAULT_STEPS_PER_MM, DEFAULT_SLIDER_TRAVEL, DEFAULT_HOME_OFFSET,
    /*solenoid*/4,  /*ledc ch*/12,
    /*servo*/2,     /*servo timer*/2,
    /*velCurve*/1
  },
  // ---- Flûte 3 ----
  { "Flute 3", 4,
    DEFAULT_MIDI_NOTE_MIN, DEFAULT_MIDI_NOTE_MAX,
    /*step*/33, /*dir*/21, /*en*/22, /*invert*/false,
    /*endstop*/36, /*active*/LOW,
    DEFAULT_STEPS_PER_MM, DEFAULT_SLIDER_TRAVEL, DEFAULT_HOME_OFFSET,
    /*solenoid*/23, /*ledc ch*/14,
    /*servo*/3,     /*servo timer*/3,    // GPIO0 strap → on prend 3 par défaut
    /*velCurve*/1
  }
};

// ============================================================================
// MODULE PRESSION — POMPE + RÉSERVOIR + CAPTEUR
// ============================================================================
//
// Architecture supportée :
//  - Pompe 12V/24V via MOSFET ou relais (pin digital ou PWM)
//  - Capteur pression analogique (ex. MPX5010 0–10 kPa, ou 0–100 PSI ratiometric)
//  - Capteur niveau réservoir optionnel (flotteur tout-ou-rien ou analog)
//  - Soupape de sécurité logicielle : coupure pompe si > max
//
// FSM : IDLE → FILLING → PRESSURIZED → MAINTAIN ↔ FILLING → SAFETY (fault)

struct PressureHwConfig {
  bool    enabled;

  // Pompe
  int8_t  pumpPin;
  bool    pumpPwm;             // false = relais (digital), true = PWM (MOSFET)
  uint8_t pumpLedcChannel;     // si pumpPwm, canal LEDC unique
  uint8_t pumpPwmMax;          // 0..255 (vitesse max, utile si silencieux)

  // Capteur pression — analog ESP32 (ADC1 = GPIO32-39, ADC2 = GPIO0,2,4,12-15,25-27)
  int8_t  pressureSensorPin;   // -1 = pas de capteur (mode hystérésis temporelle)
  float   pressureSensorMin;   // kPa @ ADC=0
  float   pressureSensorMax;   // kPa @ ADC=4095
  uint8_t pressureSensorAdcAtten;  // ADC_ATTEN_DB_11 = 0..3.3V plage

  // Seuils de régulation (kPa)
  float   pressureTarget;      // pression cible en MAINTAIN
  float   pressureMin;         // re-démarrage pompe (hystérésis basse)
  float   pressureMax;         // arrêt pompe (hystérésis haute)
  float   pressureSafety;      // > coupure d'urgence + alerte

  // Limites temporelles
  uint16_t maxFillTimeMs;      // si non atteint en X ms → SAFETY
  uint16_t pumpMinOffMs;       // anti-cyclage pompe

  // Réservoir
  int8_t  reservoirLevelPin;   // -1 = absent, sinon flotteur (digital pull-up)
  uint8_t reservoirLowState;   // niveau LOW signifie réservoir vide
};

// ATTENTION : avec 4 flûtes activées, GPIO 22 collide avec la flûte 3.
// Avec 1-3 flûtes (cas usuel), pas de collision. À remapper sinon.
static const PressureHwConfig DEFAULT_PRESSURE_CONFIG = {
  /*enabled*/        true,
  /*pumpPin*/        22,
  /*pumpPwm*/        true,
  /*pumpLedcCh*/     15,
  /*pumpPwmMax*/     230,
  /*sensorPin*/      36,    // ADC1_CH0 (input-only, OK)
  /*sensorMin kPa*/  0.0f,
  /*sensorMax kPa*/  100.0f,
  /*adcAtten*/       3,     // ADC_ATTEN_DB_11
  /*target kPa*/     50.0f,
  /*min kPa*/        40.0f,
  /*max kPa*/        60.0f,
  /*safety kPa*/     90.0f,
  /*maxFill ms*/     8000,
  /*pumpMinOff ms*/  500,
  /*levelPin*/       -1,
  /*levelLow*/       LOW
};

// ============================================================================
// WIFI / WEB
// ============================================================================

#define WIFI_AP_SSID          "SlideWhistle"
#define WIFI_AP_PASSWORD      "midi1234"
#define WIFI_AP_CHANNEL       6
#define WIFI_STA_SSID         ""
#define WIFI_STA_PASSWORD     ""
#define WEB_SERVER_PORT       80
#define WS_PATH               "/ws"

// ============================================================================
// LED / SÉCURITÉ
// ============================================================================

#define STATUS_LED_PIN        2
#define LED_ENABLED           true
#define ENABLE_SOFT_LIMITS    true
#define WATCHDOG_TIMEOUT_MS   5000     // ms sans MIDI → coupure air/pompe

// ============================================================================
// LUT PAR DÉFAUT (positions linéaires sur SLIDER_TRAVEL)
// Chaque flûte garde ensuite sa propre LUT en NVS.
// ============================================================================

static const float DEFAULT_NOTE_POSITION_LUT[MIDI_LUT_SIZE] = {
    0.00f,   8.33f,  16.67f,  25.00f,  33.33f,  41.67f,  50.00f,  58.33f,
   66.67f,  75.00f,  83.33f,  91.67f, 100.00f, 108.33f, 116.67f, 125.00f,
  133.33f, 141.67f, 150.00f, 158.33f, 166.67f, 175.00f, 183.33f, 191.67f,
  200.00f, 208.33f, 216.67f, 225.00f, 233.33f, 241.67f, 250.00f, 258.33f,
  266.67f, 275.00f, 283.33f, 291.67f, 300.00f
};

#endif // SETTINGS_H
