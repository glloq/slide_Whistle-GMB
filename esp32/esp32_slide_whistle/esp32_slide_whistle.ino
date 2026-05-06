/*
 * esp32_slide_whistle.ino — MIDI Slide Flute Controller (ESP32)
 *
 * Corrections vs v1 :
 *   - portMUX_TYPE → SemaphoreHandle_t (mutex FreeRTOS)
 *     · portENTER_CRITICAL désactivait les interruptions Core 0 (WiFi!)
 *     · xSemaphoreTake/Give cède le CPU correctement entre tâches
 *   - Watchdog MIDI : si aucun message pendant WATCHDOG_TIMEOUT_MS, air coupé
 *   - CC2 / CC11 implémentés → débit air (air.setAirflow)
 *   - g_testAirRequested : test lancé depuis taskMotion (plus depuis Core 0)
 *   - stepper.setRuntimeLUT() appelé dans taskMotion après begin()
 *     (la LUT est chargée par taskWeb, mais le pointeur est valide dès le début)
 *   - Stack sizes augmentés : 16384 taskMotion, 12288 taskWeb
 *   - SystemState maintenant défini dans settings.h (partagé avec WebInterface)
 *
 * Bibliothèques requises :
 *   AccelStepper, ESP32Servo, ESPAsyncWebServer, AsyncTCP,
 *   ArduinoJson, MIDI Library, BLE-MIDI
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
// ÉTAT PARTAGÉ
// ============================================================================

volatile SystemState sysState        = SYS_INIT;
volatile bool g_homingRequested      = false;
volatile bool g_testAirRequested     = false;
volatile unsigned long lastMidiActMs = 0;  // watchdog MIDI

// Mutex FreeRTOS — protège stepper/air contre accès concurrent inter-core.
// Utilisé dans les callbacks MIDI (Core 1 Serial, Core 0 BLE) et taskMotion.
SemaphoreHandle_t motionMutex = nullptr;

// ============================================================================
// CALLBACKS MIDI
// Appelés depuis Core 1 (MIDI Serial) ou Core 0 (BLE MIDI).
// Le mutex garantit l'exclusion mutuelle avec taskMotion.
// ============================================================================

void onMidiNoteOn(byte note, byte velocity) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  stepper.moveToMidiNote(note);
  air.startAir(velocity);
  xSemaphoreGive(motionMutex);
  lastMidiActMs = millis();
}

void onMidiNoteOff(byte note) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  air.stopAir();
  stepper.setVibrato(false, 0);
  xSemaphoreGive(motionMutex);
  lastMidiActMs = millis();
}

void onMidiPitchBend(float semitones) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  stepper.setPitchBend(semitones);
  xSemaphoreGive(motionMutex);
  lastMidiActMs = millis();
}

void onMidiAftertouch(bool active, byte pressure) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  stepper.setVibrato(active, pressure);
  xSemaphoreGive(motionMutex);
  lastMidiActMs = millis();
}

// CC2 (Breath) / CC11 (Expression) → débit air
// CC1 (Modulation) → vibrato (géré dans MIDIHandler.dispatchCC)
void onMidiCC(byte cc, byte value) {
  if (sysState != SYS_READY) return;
  if (cc == 2 || cc == 11) {
    xSemaphoreTake(motionMutex, portMAX_DELAY);
    air.setAirflow(value);
    xSemaphoreGive(motionMutex);
    lastMidiActMs = millis();
  }
}

// ============================================================================
// TÂCHE CORE 0 — WEB / WIFI (priorité basse)
// ============================================================================

void taskWeb(void* param) {
  (void)param;
  wifiMgr.begin();
  webIface.begin(&stepper, &air, &midi, &wifiMgr);
  // webIface.begin() appelle loadConfigFromNVS() + syncStepperLUT()
  // → les valeurs de runtimeCfg.lutPositions sont mises à jour ;
  //   stepper lit à travers le pointeur, donc il utilise la LUT correcte
  //   dès que taskMotion émerge du homing.

  for (;;) {
    webIface.update();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// TÂCHE CORE 1 — MIDI + MOTION (priorité haute, temps-réel)
// ============================================================================

void taskMotion(void* param) {
  (void)param;

  stepper.begin();
  air.begin();
  midi.begin();

  // Pointer vers la LUT runtime (runtimeCfg.lutPositions dans WebInterface.h).
  // Les valeurs seront mises à jour par taskWeb/webIface.begin() via NVS.
  // Le pointeur est stable pour toute la durée du programme.
  stepper.setRuntimeLUT(runtimeCfg.lutPositions, runtimeCfg.useLUT);

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
  bool ok  = stepper.performHoming();

  if (ok) {
    sysState = SYS_READY;
    lastMidiActMs = millis();  // init watchdog
#if DEBUG_MODE
    Serial.println(F("[System] READY — en attente MIDI..."));
#endif
    // Signal sonore bref
    air.forceOpen(80);
    vTaskDelay(pdMS_TO_TICKS(120));
    air.forceClose();
  } else {
    sysState = SYS_ERROR;
    Serial.println(F("[System] ERREUR homing! Vérifier endstop."));
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

  // ---- Boucle principale temps-réel ----
  for (;;) {

    // --- 1. Lire MIDI (appelle callbacks si message disponible) ---
    midi.update();

    // --- 2. Mettre à jour motion + air (sous mutex) ---
    xSemaphoreTake(motionMutex, portMAX_DELAY);
    stepper.update();
    air.update();
    xSemaphoreGive(motionMutex);

    // --- 3. Watchdog MIDI ---
    if (sysState == SYS_READY
        && millis() - lastMidiActMs > WATCHDOG_TIMEOUT_MS
        && air.isSolenoidOpen()) {
      Serial.println(F("[WDG] Timeout MIDI — air coupé"));
      xSemaphoreTake(motionMutex, portMAX_DELAY);
      air.forceClose();
      stepper.setVibrato(false, 0);
      xSemaphoreGive(motionMutex);
      lastMidiActMs = millis();  // évite déclenchements répétés
    }

    // --- 4. Homing demandé depuis l'interface web ---
    if (g_homingRequested) {
      g_homingRequested = false;
      sysState = SYS_HOMING;
      xSemaphoreTake(motionMutex, portMAX_DELAY);
      air.forceClose();
      xSemaphoreGive(motionMutex);

      bool rehome = stepper.performHoming();
      sysState    = rehome ? SYS_READY : SYS_ERROR;
      if (rehome) lastMidiActMs = millis();
    }

    // --- 5. Test air demandé depuis l'interface web ---
    if (g_testAirRequested) {
      g_testAirRequested = false;
      xSemaphoreTake(motionMutex, portMAX_DELAY);
      air.runTestFromTask();   // vTaskDelay interne, libère le CPU
      xSemaphoreGive(motionMutex);
    }

#if DEBUG_MODE
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
      lastDebug = millis();
      Serial.printf("[Status] pos=%.1fmm  air=%s  note=%d  uptime=%lus\n",
                    stepper.getCurrentPositionMM(),
                    air.getStateName(),
                    midi.getLastNote(),
                    millis() / 1000UL);
    }
#endif

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
  Serial.println(F("   ESP32 MIDI Slide Whistle v2"));
  Serial.println(F("========================================"));
#endif

#if LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif

  // Créer le mutex avant de lancer les tâches
  motionMutex = xSemaphoreCreateMutex();
  configASSERT(motionMutex);

  // Core 0 — web / WiFi  (stack 12 KB, priorité 1)
  xTaskCreatePinnedToCore(taskWeb,    "Web",    12288, nullptr, 1, nullptr, 0);

  // Core 1 — MIDI + motion (stack 16 KB, priorité 5)
  xTaskCreatePinnedToCore(taskMotion, "Motion", 16384, nullptr, 5, nullptr, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);  // tout est dans les tâches FreeRTOS
}
