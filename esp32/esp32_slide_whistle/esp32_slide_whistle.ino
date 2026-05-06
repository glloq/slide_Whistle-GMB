/*
 * esp32_slide_whistle.ino — MIDI Slide Flute Controller (ESP32)
 *
 * Architecture FreeRTOS :
 *   Core 0 : Web server, WiFi, WebSocket push
 *   Core 1 : MIDI parsing, stepper, air control (temps-réel)
 *
 * Sources MIDI supportées (voir settings.h) :
 *   - DIN-5 UART (Serial2 RX GPIO16)
 *   - BLE MIDI
 *   - Interface web (API /api/note)
 *
 * Bibliothèques requises (Arduino Library Manager) :
 *   AccelStepper           → moteur pas à pas
 *   ESP32Servo             → servomoteur
 *   ESPAsyncWebServer      → serveur web async
 *   AsyncTCP               → requis par ESPAsyncWebServer
 *   ArduinoJson            → JSON API
 *   MIDI Library           → MIDI DIN série (FortySevenEffects)
 *   BLE-MIDI               → BLE MIDI (lathoub/Arduino-BLE-MIDI)
 *
 * Upload données web :
 *   Installer "ESP32 LittleFS Data Upload" plugin Arduino IDE ou PlatformIO
 *   Placer index.html dans le dossier data/ du sketch
 */

#include "settings.h"
#include "StepperControl.h"
#include "AirControl.h"
#include "MIDIHandler.h"
#include "WiFiManager.h"
#include "WebInterface.h"

// ============================================================================
// INSTANCES GLOBALES
// ============================================================================

StepperControl stepper;
AirControl     air;
MIDIHandler    midi;
WiFiManager    wifiMgr;
WebInterface   webIface;

// ============================================================================
// ÉTAT SYSTÈME
// ============================================================================

enum SystemState { SYS_INIT, SYS_HOMING, SYS_READY, SYS_ERROR };
volatile SystemState sysState = SYS_INIT;

// Flag homing demandé depuis l'interface web (WebInterface.h le positionne)
volatile bool g_homingRequested = false;

// Mutex pour protéger stepper/air partagés entre les deux cores
portMUX_TYPE motionMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// CALLBACKS MIDI
// ============================================================================

void onMidiNoteOn(byte note, byte velocity) {
  if (sysState != SYS_READY) return;
  portENTER_CRITICAL(&motionMux);
  stepper.moveToMidiNote(note);
  air.startAir(velocity);
  portEXIT_CRITICAL(&motionMux);
}

void onMidiNoteOff(byte note) {
  if (sysState != SYS_READY) return;
  portENTER_CRITICAL(&motionMux);
  air.stopAir();
  stepper.setVibrato(false, 0);
  portEXIT_CRITICAL(&motionMux);
}

void onMidiPitchBend(float semitones) {
  if (sysState != SYS_READY) return;
  portENTER_CRITICAL(&motionMux);
  stepper.setPitchBend(semitones);
  portEXIT_CRITICAL(&motionMux);
}

void onMidiAftertouch(bool active, byte pressure) {
  if (sysState != SYS_READY) return;
  portENTER_CRITICAL(&motionMux);
  stepper.setVibrato(active, pressure);
  portEXIT_CRITICAL(&motionMux);
}

void onMidiCC(byte cc, byte value) {
  if (sysState != SYS_READY) return;
  // CC2 / CC11 → débit air (servo direct)
  if (cc == 2 || cc == 11) {
    portENTER_CRITICAL(&motionMux);
    // Mapper 0-127 → vitesse servo (contrôle expression)
    // Pour l'instant : vibrato via CC1 géré dans MIDIHandler
    portEXIT_CRITICAL(&motionMux);
  }
}

// ============================================================================
// TÂCHE CORE 0 — WEB / WIFI
// ============================================================================

void taskWeb(void* param) {
  (void)param;
  wifiMgr.begin();
  webIface.begin(&stepper, &air, &midi, &wifiMgr);

  for (;;) {
    webIface.update();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// TÂCHE CORE 1 — MIDI + MOTION (temps-réel)
// ============================================================================

void taskMotion(void* param) {
  (void)param;

  // Initialiser les modules temps-réel
  stepper.begin();
  air.begin();
  midi.begin();

  midi.setNoteOnCallback(onMidiNoteOn);
  midi.setNoteOffCallback(onMidiNoteOff);
  midi.setPitchBendCallback(onMidiPitchBend);
  midi.setAftertouchCallback(onMidiAftertouch);
  midi.setCCCallback(onMidiCC);

#if LED_ENABLED
  digitalWrite(STATUS_LED_PIN, HIGH);
#endif

  // Homing initial
  sysState = SYS_HOMING;
  bool ok = stepper.performHoming();

  if (ok) {
    sysState = SYS_READY;
#if DEBUG_MODE
    Serial.println(F("[System] READY — en attente MIDI..."));
#endif
    // Signal sonore bref
    air.forceOpen(80);
    vTaskDelay(pdMS_TO_TICKS(120));
    air.forceClose();
  } else {
    sysState = SYS_ERROR;
#if DEBUG_MODE
    Serial.println(F("[System] ERREUR homing! Vérifier endstop."));
#endif
    // LED clignote indéfiniment en cas d'erreur
    for (;;) {
#if LED_ENABLED
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
#endif
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }

#if LED_ENABLED
  digitalWrite(STATUS_LED_PIN, LOW);
#endif

  // Boucle principale temps-réel
  for (;;) {
    midi.update();

    portENTER_CRITICAL(&motionMux);
    stepper.update();
    air.update();
    portEXIT_CRITICAL(&motionMux);

    // Homing demandé depuis interface web
    if (g_homingRequested) {
      g_homingRequested = false;
      sysState = SYS_HOMING;
      air.forceClose();
      portENTER_CRITICAL(&motionMux);
      bool rehome = stepper.performHoming();
      portEXIT_CRITICAL(&motionMux);
      sysState = rehome ? SYS_READY : SYS_ERROR;
    }

#if DEBUG_MODE
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
      lastDebug = millis();
      Serial.printf("[Status] pos=%.1fmm  air=%s  note=%d  uptime=%lus\n",
                    stepper.getCurrentPositionMM(),
                    air.getStateName(),
                    midi.getLastNote(),
                    millis() / 1000);
    }
#endif

    // Pas de delay — laisser le scheduler FreeRTOS gérer
    taskYIELD();
  }
}

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup() {
#if DEBUG_MODE
  Serial.begin(SERIAL_BAUD_RATE);
  delay(500);
  Serial.println(F("\n========================================"));
  Serial.println(F("   ESP32 MIDI Slide Whistle Controller"));
  Serial.println(F("========================================"));
#endif

#if LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif

  // Tâche web sur Core 0 (priorité basse)
  xTaskCreatePinnedToCore(taskWeb,    "Web",    8192, nullptr, 1, nullptr, 0);

  // Tâche motion sur Core 1 (priorité haute)
  xTaskCreatePinnedToCore(taskMotion, "Motion", 8192, nullptr, 5, nullptr, 1);
}

void loop() {
  // Tout est dans les tâches FreeRTOS — loop() inutilisé
  vTaskDelay(portMAX_DELAY);
}
