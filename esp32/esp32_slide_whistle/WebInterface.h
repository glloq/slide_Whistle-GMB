/*
 * WebInterface.h — Serveur web + WebSocket (ESP32)
 *
 * Bibliothèques requises :
 *   ESPAsyncWebServer  (me-no-dev/ESPAsyncWebServer)
 *   AsyncTCP           (me-no-dev/AsyncTCP)
 *   ArduinoJson        (bblanchon/ArduinoJson)
 *   LittleFS           (incluse ESP32 Arduino Core)
 *
 * L'UI est servie depuis LittleFS (/data/index.html).
 *
 * API REST :
 *   GET  /api/status          → JSON état système
 *   GET  /api/config          → JSON configuration
 *   POST /api/config          → modifier config (JSON body)
 *   POST /api/note            → {note, velocity, on:bool}
 *   POST /api/jog             → {mm: float}
 *   POST /api/homing          → lance homing
 *   POST /api/test/air        → test solénoïde+servo
 *   GET  /api/lut             → JSON table LUT
 *   POST /api/lut             → sauvegarder LUT entière
 *   POST /api/lut/point       → {note, position_mm}
 *   POST /api/wifi            → {ssid, password}
 *   DELETE /api/wifi          → oublier credentials STA
 *   GET  /api/wifi            → état WiFi
 *
 * WebSocket /ws :
 *   Push JSON status toutes les 200 ms (depuis loop)
 */

#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "settings.h"

// Forward declarations (objets globaux définis dans .ino)
class StepperControl;
class AirControl;
class MIDIHandler;
class WiFiManager;

// Pointeurs vers les modules globaux — assignés dans setup()
static StepperControl* _stepperPtr = nullptr;
static AirControl*     _airPtr     = nullptr;
static MIDIHandler*    _midiPtr    = nullptr;
static WiFiManager*    _wifiMgrPtr = nullptr;

// Copies modifiables des paramètres runtime
struct RuntimeConfig {
  float speedMmS     = STEPPER_SPEED_MM_S;
  float accelMmS2    = STEPPER_ACCEL_MM_S2;
  int   waitDelayMs  = POSITION_WAIT_DELAY;
  int   legatoMs     = LEGATO_THRESHOLD;
  int   pwmFull      = SOLENOID_PWM_FULL;
  int   pwmHold      = SOLENOID_PWM_HOLD;
  int   midiChannel  = MIDI_CHANNEL;
  int   noteMin      = MIDI_NOTE_MIN;
  int   noteMax      = MIDI_NOTE_MAX;
  bool  useLUT       = USE_POSITION_LUT;
  float lutPositions[37];  // note 48-84

  RuntimeConfig() {
    memcpy(lutPositions, NOTE_POSITION_LUT, sizeof(lutPositions));
  }
};

static RuntimeConfig runtimeCfg;

class WebInterface {
private:
  AsyncWebServer server;
  AsyncWebSocket ws;
  Preferences prefs;
  unsigned long lastWsPush = 0;
  const unsigned long WS_INTERVAL = 200; // ms

  // ---- Helpers JSON ----
  String buildStatusJSON() {
    StaticJsonDocument<512> doc;
    if (_stepperPtr) {
      doc["position_mm"] = _stepperPtr->getCurrentPositionMM();
      doc["moving"]      = _stepperPtr->isMoving();
      doc["homed"]       = _stepperPtr->isHomed();
    }
    if (_airPtr) {
      doc["air_state"]   = _airPtr->getStateName();
      doc["air_open"]    = _airPtr->isSolenoidOpen();
      doc["velocity"]    = _airPtr->getCurrentVelocity();
    }
    if (_midiPtr) {
      doc["last_note"]   = _midiPtr->getLastNote();
      doc["note_active"] = _midiPtr->isNoteActive();
      doc["pitchbend"]   = _midiPtr->getPitchBendValue();
    }
    doc["uptime_ms"]     = millis();
    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildConfigJSON() {
    StaticJsonDocument<512> doc;
    doc["speed_mm_s"]   = runtimeCfg.speedMmS;
    doc["accel_mm_s2"]  = runtimeCfg.accelMmS2;
    doc["wait_delay_ms"]= runtimeCfg.waitDelayMs;
    doc["legato_ms"]    = runtimeCfg.legatoMs;
    doc["pwm_full"]     = runtimeCfg.pwmFull;
    doc["pwm_hold"]     = runtimeCfg.pwmHold;
    doc["midi_channel"] = runtimeCfg.midiChannel;
    doc["note_min"]     = runtimeCfg.noteMin;
    doc["note_max"]     = runtimeCfg.noteMax;
    doc["use_lut"]      = runtimeCfg.useLUT;
    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildLUTJSON() {
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.createNestedArray("lut");
    for (int i = 0; i < 37; i++) {
      JsonObject pt = arr.createNestedObject();
      pt["note"]     = MIDI_NOTE_MIN + i;
      pt["position"] = runtimeCfg.lutPositions[i];
    }
    String out;
    serializeJson(doc, out);
    return out;
  }

  void saveConfigToNVS() {
    prefs.begin("config", false);
    prefs.putFloat("speed",      runtimeCfg.speedMmS);
    prefs.putFloat("accel",      runtimeCfg.accelMmS2);
    prefs.putInt("wait",         runtimeCfg.waitDelayMs);
    prefs.putInt("legato",       runtimeCfg.legatoMs);
    prefs.putInt("pwmFull",      runtimeCfg.pwmFull);
    prefs.putInt("pwmHold",      runtimeCfg.pwmHold);
    prefs.putInt("midiCh",       runtimeCfg.midiChannel);
    prefs.putInt("noteMin",      runtimeCfg.noteMin);
    prefs.putInt("noteMax",      runtimeCfg.noteMax);
    prefs.putBool("useLUT",      runtimeCfg.useLUT);
    prefs.putBytes("lut", runtimeCfg.lutPositions, sizeof(runtimeCfg.lutPositions));
    prefs.end();
  }

  void loadConfigFromNVS() {
    prefs.begin("config", true);
    runtimeCfg.speedMmS    = prefs.getFloat("speed",   STEPPER_SPEED_MM_S);
    runtimeCfg.accelMmS2   = prefs.getFloat("accel",   STEPPER_ACCEL_MM_S2);
    runtimeCfg.waitDelayMs = prefs.getInt("wait",      POSITION_WAIT_DELAY);
    runtimeCfg.legatoMs    = prefs.getInt("legato",    LEGATO_THRESHOLD);
    runtimeCfg.pwmFull     = prefs.getInt("pwmFull",   SOLENOID_PWM_FULL);
    runtimeCfg.pwmHold     = prefs.getInt("pwmHold",   SOLENOID_PWM_HOLD);
    runtimeCfg.midiChannel = prefs.getInt("midiCh",    MIDI_CHANNEL);
    runtimeCfg.noteMin     = prefs.getInt("noteMin",   MIDI_NOTE_MIN);
    runtimeCfg.noteMax     = prefs.getInt("noteMax",   MIDI_NOTE_MAX);
    runtimeCfg.useLUT      = prefs.getBool("useLUT",   USE_POSITION_LUT);
    if (prefs.getBytesLength("lut") == sizeof(runtimeCfg.lutPositions)) {
      prefs.getBytes("lut", runtimeCfg.lutPositions, sizeof(runtimeCfg.lutPositions));
    }
    prefs.end();
  }

  // ---- Routes ----
  void setupRoutes() {
    // Fichiers statiques LittleFS
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Status
    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildStatusJSON());
    });

    // Config GET
    server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildConfigJSON());
    });

    // Config POST
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/config",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonObject obj = json.as<JsonObject>();
        if (obj.containsKey("speed_mm_s"))    runtimeCfg.speedMmS    = obj["speed_mm_s"];
        if (obj.containsKey("accel_mm_s2"))   runtimeCfg.accelMmS2   = obj["accel_mm_s2"];
        if (obj.containsKey("wait_delay_ms")) runtimeCfg.waitDelayMs = obj["wait_delay_ms"];
        if (obj.containsKey("legato_ms"))     runtimeCfg.legatoMs    = obj["legato_ms"];
        if (obj.containsKey("pwm_full"))      runtimeCfg.pwmFull     = obj["pwm_full"];
        if (obj.containsKey("pwm_hold"))      runtimeCfg.pwmHold     = obj["pwm_hold"];
        if (obj.containsKey("midi_channel"))  runtimeCfg.midiChannel = obj["midi_channel"];
        if (obj.containsKey("note_min"))      runtimeCfg.noteMin     = obj["note_min"];
        if (obj.containsKey("note_max"))      runtimeCfg.noteMax     = obj["note_max"];
        if (obj.containsKey("use_lut"))       runtimeCfg.useLUT      = obj["use_lut"];

        if (_stepperPtr) {
          _stepperPtr->setSpeed(runtimeCfg.speedMmS);
          _stepperPtr->setAcceleration(runtimeCfg.accelMmS2);
        }
        saveConfigToNVS();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // Note on/off depuis web
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/note",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonObject obj = json.as<JsonObject>();
        byte note = obj["note"]     | 60;
        byte vel  = obj["velocity"] | 100;
        bool on   = obj["on"]       | true;
        if (_midiPtr) {
          if (on) _midiPtr->triggerNoteOn(note, vel);
          else    _midiPtr->triggerNoteOff(note);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // Jog moteur
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/jog",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        float mm = json["mm"] | 0.0f;
        if (_stepperPtr && _stepperPtr->isHomed()) {
          float newPos = _stepperPtr->getCurrentPositionMM() + mm;
          _stepperPtr->moveToMM(newPos);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // Homing
    server.on("/api/homing", HTTP_POST, [](AsyncWebServerRequest* req) {
      // Le homing bloque le moteur — on le signale seulement, le .ino le déclenche
      req->send(200, "application/json", "{\"ok\":true,\"message\":\"homing scheduled\"}");
      extern volatile bool g_homingRequested;
      g_homingRequested = true;
    });

    // Test air
    server.on("/api/test/air", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (_airPtr) _airPtr->testSequence();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // LUT GET
    server.on("/api/lut", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildLUTJSON());
    });

    // LUT POST (table complète)
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/lut",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonArray arr = json["lut"].as<JsonArray>();
        for (JsonObject pt : arr) {
          int  note = pt["note"] | -1;
          float pos = pt["position"] | -1.0f;
          if (note >= MIDI_NOTE_MIN && note <= MIDI_NOTE_MAX && pos >= 0) {
            runtimeCfg.lutPositions[note - MIDI_NOTE_MIN] = pos;
          }
        }
        saveConfigToNVS();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // LUT point unique
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/lut/point",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        int   note = json["note"]        | -1;
        float pos  = json["position_mm"] | -1.0f;
        if (note >= MIDI_NOTE_MIN && note <= MIDI_NOTE_MAX && pos >= 0) {
          runtimeCfg.lutPositions[note - MIDI_NOTE_MIN] = pos;
          saveConfigToNVS();
          req->send(200, "application/json", "{\"ok\":true}");
        } else {
          req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid note/position\"}");
        }
      }));

    // WiFi status
    server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (_wifiMgrPtr) req->send(200, "application/json", _wifiMgrPtr->getStatusJSON());
      else             req->send(503, "application/json", "{\"error\":\"no wifi manager\"}");
    });

    // WiFi credentials STA
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        String ssid = json["ssid"]     | "";
        String pass = json["password"] | "";
        if (_wifiMgrPtr) {
          _wifiMgrPtr->saveSTACredentials(ssid, pass);
          _wifiMgrPtr->connectSTA();
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    server.on("/api/wifi", HTTP_DELETE, [](AsyncWebServerRequest* req) {
      if (_wifiMgrPtr) _wifiMgrPtr->clearSTACredentials();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // 404
    server.onNotFound([](AsyncWebServerRequest* req) {
      req->send(404, "text/plain", "Not found");
    });

    // WebSocket
    ws.onEvent([this](AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
      (void)srv; (void)arg; (void)data; (void)len;
      if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] Client #%u connecté\n", client->id());
        client->text(buildStatusJSON());
      }
    });
    server.addHandler(&ws);
  }

public:
  WebInterface() : server(WEB_SERVER_PORT), ws(WS_PATH) {}

  void begin(StepperControl* stepper, AirControl* air,
             MIDIHandler* midi, WiFiManager* wifiMgr) {
    _stepperPtr = stepper;
    _airPtr     = air;
    _midiPtr    = midi;
    _wifiMgrPtr = wifiMgr;

    if (!LittleFS.begin(true)) {
      Serial.println(F("[Web] LittleFS mount failed — formatage..."));
      LittleFS.format();
      LittleFS.begin();
    }

    loadConfigFromNVS();
    setupRoutes();
    server.begin();

    Serial.printf("[Web] Serveur démarré sur port %d\n", WEB_SERVER_PORT);
  }

  // Appeler dans loop() — push WebSocket périodique
  void update() {
    ws.cleanupClients();
    if (millis() - lastWsPush >= WS_INTERVAL) {
      lastWsPush = millis();
      if (ws.count() > 0) {
        ws.textAll(buildStatusJSON());
      }
    }
  }
};

#endif // WEB_INTERFACE_H
