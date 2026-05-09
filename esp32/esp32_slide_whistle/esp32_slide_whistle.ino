/*
 * esp32_slide_whistle.ino — MIDI Slide Flute Controller (ESP32) v3
 *
 * Architecture multi-flûtes + module pression :
 *   - Orchestra  : N flûtes (chacune = stepper + air + config), routage MIDI
 *   - Pressure   : pompe + capteur + FSM régulation
 *   - MIDIHandler: source série/BLE → callbacks broadcast canal-aware
 *   - WebInterface: API REST /api/flute(/lut|/note|/jog|/test|/panic) et /api/pressure
 *
 * Ports / canaux LEDC : voir settings.h DEFAULT_FLUTE_CONFIGS[]
 *
 * Bibliothèques requises :
 *   AccelStepper, ESP32Servo, ESPAsyncWebServer, AsyncTCP,
 *   ArduinoJson, MIDI Library, BLE-MIDI
 */

#include "settings.h"
#include "Orchestra.h"
#include "PressureControl.h"
#include "MIDIHandler.h"
#include "WiFiManager.h"
#include "WebInterface.h"

// ============================================================================
// INSTANCES GLOBALES
// ============================================================================

Orchestra        orchestra;
PressureControl  pressure;
MIDIHandler      midi;
WiFiManager      wifiMgr;
WebInterface     webIface;

// ============================================================================
// ÉTAT PARTAGÉ ENTRE TÂCHES
// ============================================================================

volatile SystemState sysState        = SYS_INIT;
volatile bool g_homingRequested      = false;
volatile int  g_testAirFluteId       = -1;
volatile bool g_pressureStartRequested = false;
volatile bool g_pressureStopRequested  = false;
volatile bool g_pressureResetRequested = false;

// Mutex FreeRTOS — protège orchestre/pression contre accès concurrent inter-core.
SemaphoreHandle_t motionMutex = nullptr;

// ============================================================================
// CALLBACKS MIDI — appelés depuis Core 1 (Serial) ou Core 0 (BLE)
// ============================================================================

void onMidiNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  orchestra.onNoteOn(ch, note, vel);
  xSemaphoreGive(motionMutex);
}

void onMidiNoteOff(uint8_t ch, uint8_t note) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  orchestra.onNoteOff(ch, note);
  xSemaphoreGive(motionMutex);
}

void onMidiPitchBend(uint8_t ch, float semitones) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  orchestra.onPitchBend(ch, semitones);
  xSemaphoreGive(motionMutex);
}

void onMidiAftertouch(uint8_t ch, bool active, uint8_t pressure_v) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  orchestra.onAftertouch(ch, active, pressure_v);
  xSemaphoreGive(motionMutex);
}

void onMidiCC(uint8_t ch, uint8_t cc, uint8_t value) {
  if (sysState != SYS_READY) return;
  xSemaphoreTake(motionMutex, portMAX_DELAY);
  orchestra.onCC(ch, cc, value);
  xSemaphoreGive(motionMutex);
}

// ============================================================================
// TÂCHE CORE 0 — WEB / WIFI
// ============================================================================

void taskWeb(void* param) {
  (void)param;
  wifiMgr.begin();
  webIface.begin(&orchestra, &pressure, &midi, &wifiMgr);

  for (;;) {
    webIface.update();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// TÂCHE CORE 1 — MIDI + MOTION + PRESSION (priorité haute, temps-réel)
// ============================================================================

void taskMotion(void* param) {
  (void)param;

  // Les flûtes ont déjà été créées dans setup() — ici on initialise le matériel.
  orchestra.beginAll();

#if ENABLE_PRESSURE_CONTROL
  pressure.begin(DEFAULT_PRESSURE_CONFIG);
#endif

  midi.begin();
  midi.setNoteOnCallback(onMidiNoteOn);
  midi.setNoteOffCallback(onMidiNoteOff);
  midi.setPitchBendCallback(onMidiPitchBend);
  midi.setAftertouchCallback(onMidiAftertouch);
  midi.setCCCallback(onMidiCC);

#if LED_ENABLED
  digitalWrite(STATUS_LED_PIN, HIGH);
#endif

  // Homing initial — toutes flûtes (séquentiel)
  sysState = SYS_HOMING;
  bool ok  = orchestra.homingAll();

  if (ok) {
    sysState = SYS_READY;
#if DEBUG_MODE
    Serial.printf("[System] READY — %u flûte(s) opérationnelle(s)\n", orchestra.count());
#endif
    // Bip de bienvenue sur la flûte 0
    Flute* f0 = orchestra.get(0);
    if (f0) {
      f0->air().forceOpen(80);
      vTaskDelay(pdMS_TO_TICKS(120));
      f0->air().forceClose();
    }

#if ENABLE_PRESSURE_CONTROL
    if (pressure.isEnabled()) {
      pressure.start();   // démarrer le remplissage automatiquement
    }
#endif
  } else {
    sysState = SYS_ERROR;
    Serial.println(F("[System] ERREUR homing — vérifier endstops"));
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

  // ---- Boucle temps-réel ----
  for (;;) {

    // 1. MIDI
    midi.update();

    // 2. Motion + air (sous mutex)
    xSemaphoreTake(motionMutex, portMAX_DELAY);
    orchestra.updateAll();
#if ENABLE_PRESSURE_CONTROL
    pressure.update();
#endif
    xSemaphoreGive(motionMutex);

    // 3. Watchdog MIDI par flûte
    if (sysState == SYS_READY) {
      xSemaphoreTake(motionMutex, portMAX_DELAY);
      orchestra.watchdogTickAll(WATCHDOG_TIMEOUT_MS);
      xSemaphoreGive(motionMutex);
    }

    // 4. Homing demandé depuis le web
    if (g_homingRequested) {
      g_homingRequested = false;
      sysState = SYS_HOMING;
      xSemaphoreTake(motionMutex, portMAX_DELAY);
      orchestra.panicAll();
      xSemaphoreGive(motionMutex);
      bool rehome = orchestra.homingAll();
      sysState    = rehome ? SYS_READY : SYS_ERROR;
    }

    // 5. Test air sur une flûte particulière
    if (g_testAirFluteId >= 0) {
      int id = g_testAirFluteId;
      g_testAirFluteId = -1;
      Flute* f = orchestra.get(id);
      if (f) {
        xSemaphoreTake(motionMutex, portMAX_DELAY);
        f->air().runTestFromTask();
        xSemaphoreGive(motionMutex);
      }
    }

#if ENABLE_PRESSURE_CONTROL
    // 6. Commandes pression
    if (g_pressureStartRequested) { g_pressureStartRequested = false; pressure.start(); }
    if (g_pressureStopRequested)  { g_pressureStopRequested  = false; pressure.stop();  }
    if (g_pressureResetRequested) { g_pressureResetRequested = false; pressure.reset(); }
#endif

#if DEBUG_MODE
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
      lastDebug = millis();
      Serial.printf("[Status] flutes=%u  uptime=%lus", orchestra.count(), millis()/1000UL);
      for (uint8_t i = 0; i < orchestra.count(); ++i) {
        Flute* f = orchestra.get(i);
        if (!f) continue;
        Serial.printf("  | F%u:%.1fmm/%s", i,
                      f->stepper().getCurrentPositionMM(),
                      f->air().getStateName());
      }
#if ENABLE_PRESSURE_CONTROL
      if (pressure.isEnabled()) {
        Serial.printf("  | P:%s %.1fkPa", pressure.getStateName(), pressure.getPressure());
      }
#endif
      Serial.println();
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
  Serial.println(F("  ESP32 MIDI Slide Whistle v3"));
  Serial.println(F("  multi-flute + pressure control"));
  Serial.println(F("========================================"));
#endif

#if LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif

  motionMutex = xSemaphoreCreateMutex();
  configASSERT(motionMutex);

  // Construire les flûtes ici (avant les tâches) pour éviter une course
  // entre taskMotion (begin matériel) et taskWeb (loadFluteConfigNVS).
  for (uint8_t i = 0; i < DEFAULT_FLUTE_COUNT && i < MAX_FLUTES; ++i) {
    orchestra.addFlute(DEFAULT_FLUTE_CONFIGS[i]);
  }

  // Core 0 — web (stack 12 KB, prio 1)
  xTaskCreatePinnedToCore(taskWeb,    "Web",    12288, nullptr, 1, nullptr, 0);

  // Core 1 — MIDI + motion (stack 16 KB, prio 5)
  xTaskCreatePinnedToCore(taskMotion, "Motion", 16384, nullptr, 5, nullptr, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
