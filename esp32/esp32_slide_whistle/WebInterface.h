/*
 * WebInterface.h — Serveur web + WebSocket (ESP32)
 *
 * Corrections vs v1 :
 *   - extern g_homingRequested et g_testAirRequested déclarés au niveau
 *     fichier (plus à l'intérieur d'une lambda — non-standard C++)
 *   - extern SystemState sysState déclaré ici pour l'inclure dans le JSON status
 *   - POST /api/test/air : positionne le flag g_testAirRequested au lieu
 *     d'appeler testSequence() directement depuis Core 0
 *   - POST /api/config : validation des bornes avant application
 *   - POST /api/config + POST /api/lut : appel setRuntimeLUT() sur le stepper
 *     → la calibration web est maintenant réellement appliquée au moteur
 *   - buildStatusJSON() inclut sys_state
 *
 * Bibliothèques : ESPAsyncWebServer, AsyncTCP, ArduinoJson, LittleFS, Preferences
 */

#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "settings.h"

// ---- Déclarations externes (définies dans .ino) --------------------------
// Note : 'extern' dans une lambda serait non-standard C++.
// On les déclare ici au niveau fichier, là où c'est valide.
extern volatile SystemState sysState;
extern volatile bool g_homingRequested;
extern volatile bool g_testAirRequested;

// ---- Forward declarations ------------------------------------------------
class StepperControl;
class AirControl;
class MIDIHandler;
class WiFiManager;

static StepperControl* _stepperPtr = nullptr;
static AirControl*     _airPtr     = nullptr;
static MIDIHandler*    _midiPtr    = nullptr;
static WiFiManager*    _wifiMgrPtr = nullptr;

// ---- Config runtime -------------------------------------------------------

struct RuntimeConfig {
  float speedMmS    = STEPPER_SPEED_MM_S;
  float accelMmS2   = STEPPER_ACCEL_MM_S2;
  int   waitDelayMs = POSITION_WAIT_DELAY;
  int   legatoMs    = LEGATO_THRESHOLD;
  int   pwmFull     = SOLENOID_PWM_FULL;
  int   pwmHold     = SOLENOID_PWM_HOLD;
  int   midiChannel = MIDI_CHANNEL;
  int   noteMin     = MIDI_NOTE_MIN;
  int   noteMax     = MIDI_NOTE_MAX;
  bool  useLUT      = USE_POSITION_LUT;
  float lutPositions[37];

  RuntimeConfig() {
    memcpy(lutPositions, NOTE_POSITION_LUT, sizeof(lutPositions));
  }
};

static RuntimeConfig runtimeCfg;

// ---- WebInterface ---------------------------------------------------------

class WebInterface {
private:
  AsyncWebServer server;
  AsyncWebSocket ws;
  Preferences    prefs;
  unsigned long  lastWsPush = 0;
  static constexpr unsigned long WS_INTERVAL = 200;

  // ---- JSON builders ----

  String buildStatusJSON() {
    StaticJsonDocument<512> doc;

    // sys_state
    const char* stateNames[] = {"INIT","HOMING","READY","ERROR"};
    doc["sys_state"] = stateNames[(int)sysState];

    if (_stepperPtr) {
      doc["position_mm"] = _stepperPtr->getCurrentPositionMM();
      doc["moving"]      = _stepperPtr->isMoving();
      doc["homed"]       = _stepperPtr->isHomed();
    }
    if (_airPtr) {
      doc["air_state"] = _airPtr->getStateName();
      doc["air_open"]  = _airPtr->isSolenoidOpen();
      doc["velocity"]  = _airPtr->getCurrentVelocity();
    }
    if (_midiPtr) {
      doc["last_note"]   = _midiPtr->getLastNote();
      doc["note_active"] = _midiPtr->isNoteActive();
      doc["pitchbend"]   = _midiPtr->getPitchBendValue();
    }
    doc["uptime_ms"] = millis();

    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildConfigJSON() {
    StaticJsonDocument<512> doc;
    doc["speed_mm_s"]    = runtimeCfg.speedMmS;
    doc["accel_mm_s2"]   = runtimeCfg.accelMmS2;
    doc["wait_delay_ms"] = runtimeCfg.waitDelayMs;
    doc["legato_ms"]     = runtimeCfg.legatoMs;
    doc["pwm_full"]      = runtimeCfg.pwmFull;
    doc["pwm_hold"]      = runtimeCfg.pwmHold;
    doc["midi_channel"]  = runtimeCfg.midiChannel;
    doc["note_min"]      = runtimeCfg.noteMin;
    doc["note_max"]      = runtimeCfg.noteMax;
    doc["use_lut"]       = runtimeCfg.useLUT;
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

  // ---- NVS ----

  void saveConfigToNVS() {
    prefs.begin("config", false);
    prefs.putFloat("speed",    runtimeCfg.speedMmS);
    prefs.putFloat("accel",    runtimeCfg.accelMmS2);
    prefs.putInt("wait",       runtimeCfg.waitDelayMs);
    prefs.putInt("legato",     runtimeCfg.legatoMs);
    prefs.putInt("pwmFull",    runtimeCfg.pwmFull);
    prefs.putInt("pwmHold",    runtimeCfg.pwmHold);
    prefs.putInt("midiCh",     runtimeCfg.midiChannel);
    prefs.putInt("noteMin",    runtimeCfg.noteMin);
    prefs.putInt("noteMax",    runtimeCfg.noteMax);
    prefs.putBool("useLUT",    runtimeCfg.useLUT);
    prefs.putBytes("lut", runtimeCfg.lutPositions, sizeof(runtimeCfg.lutPositions));
    prefs.end();
  }

  void loadConfigFromNVS() {
    prefs.begin("config", true);
    runtimeCfg.speedMmS    = prefs.getFloat("speed",  STEPPER_SPEED_MM_S);
    runtimeCfg.accelMmS2   = prefs.getFloat("accel",  STEPPER_ACCEL_MM_S2);
    runtimeCfg.waitDelayMs = prefs.getInt("wait",     POSITION_WAIT_DELAY);
    runtimeCfg.legatoMs    = prefs.getInt("legato",   LEGATO_THRESHOLD);
    runtimeCfg.pwmFull     = prefs.getInt("pwmFull",  SOLENOID_PWM_FULL);
    runtimeCfg.pwmHold     = prefs.getInt("pwmHold",  SOLENOID_PWM_HOLD);
    runtimeCfg.midiChannel = prefs.getInt("midiCh",   MIDI_CHANNEL);
    runtimeCfg.noteMin     = prefs.getInt("noteMin",  MIDI_NOTE_MIN);
    runtimeCfg.noteMax     = prefs.getInt("noteMax",  MIDI_NOTE_MAX);
    runtimeCfg.useLUT      = prefs.getBool("useLUT",  USE_POSITION_LUT);
    if (prefs.getBytesLength("lut") == sizeof(runtimeCfg.lutPositions)) {
      prefs.getBytes("lut", runtimeCfg.lutPositions, sizeof(runtimeCfg.lutPositions));
    }
    prefs.end();
  }

  // Notifie StepperControl de la nouvelle LUT / flag useLUT
  void syncStepperLUT() {
    if (_stepperPtr) {
      _stepperPtr->setRuntimeLUT(runtimeCfg.lutPositions, runtimeCfg.useLUT);
    }
  }

  // ---- Routes ----

  void setupRoutes() {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // GET /api/status
    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildStatusJSON());
    });

    // GET /api/config
    server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildConfigJSON());
    });

    // POST /api/config — avec validation des bornes
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/config",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonObject obj = json.as<JsonObject>();

        if (obj.containsKey("speed_mm_s")) {
          float v = obj["speed_mm_s"];
          if (v < 5.0f || v > 500.0f) { req->send(400, "application/json", "{\"error\":\"speed hors bornes [5-500]\"}"); return; }
          runtimeCfg.speedMmS = v;
          if (_stepperPtr) _stepperPtr->setSpeed(v);
        }
        if (obj.containsKey("accel_mm_s2")) {
          float v = obj["accel_mm_s2"];
          if (v < 50.0f || v > 5000.0f) { req->send(400, "application/json", "{\"error\":\"accel hors bornes [50-5000]\"}"); return; }
          runtimeCfg.accelMmS2 = v;
          if (_stepperPtr) _stepperPtr->setAcceleration(v);
        }
        if (obj.containsKey("wait_delay_ms")) {
          int v = obj["wait_delay_ms"];
          if (v < 0 || v > 2000) { req->send(400, "application/json", "{\"error\":\"wait hors bornes [0-2000]\"}"); return; }
          runtimeCfg.waitDelayMs = v;
        }
        if (obj.containsKey("legato_ms")) {
          int v = obj["legato_ms"];
          if (v < 0 || v > 2000) { req->send(400, "application/json", "{\"error\":\"legato hors bornes [0-2000]\"}"); return; }
          runtimeCfg.legatoMs = v;
        }
        if (obj.containsKey("pwm_full")) {
          int v = obj["pwm_full"];
          if (v < 0 || v > 255) { req->send(400, "application/json", "{\"error\":\"pwm_full hors bornes [0-255]\"}"); return; }
          runtimeCfg.pwmFull = v;
        }
        if (obj.containsKey("pwm_hold")) {
          int v = obj["pwm_hold"];
          if (v < 0 || v > 255) { req->send(400, "application/json", "{\"error\":\"pwm_hold hors bornes [0-255]\"}"); return; }
          if (obj.containsKey("pwm_full") && v >= runtimeCfg.pwmFull) {
            req->send(400, "application/json", "{\"error\":\"pwm_hold doit être < pwm_full\"}"); return;
          }
          runtimeCfg.pwmHold = v;
        }
        if (obj.containsKey("midi_channel")) {
          int v = obj["midi_channel"];
          if (v < 0 || v > 16) { req->send(400, "application/json", "{\"error\":\"canal MIDI hors bornes [0-16]\"}"); return; }
          runtimeCfg.midiChannel = v;
        }
        if (obj.containsKey("note_min")) {
          int v = obj["note_min"];
          if (v < 0 || v > 127) { req->send(400, "application/json", "{\"error\":\"note_min hors bornes\"}"); return; }
          runtimeCfg.noteMin = v;
        }
        if (obj.containsKey("note_max")) {
          int v = obj["note_max"];
          if (v < 0 || v > 127 || v <= runtimeCfg.noteMin) {
            req->send(400, "application/json", "{\"error\":\"note_max invalide\"}"); return;
          }
          runtimeCfg.noteMax = v;
        }
        if (obj.containsKey("use_lut")) {
          runtimeCfg.useLUT = (bool)obj["use_lut"];
          syncStepperLUT();
        }

        saveConfigToNVS();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // POST /api/note
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/note",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        byte note = json["note"]     | 60;
        byte vel  = json["velocity"] | 100;
        bool on   = json["on"]       | true;
        if (_midiPtr) {
          if (on) _midiPtr->triggerNoteOn(note, vel);
          else    _midiPtr->triggerNoteOff(note);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // POST /api/jog
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/jog",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        float mm = json["mm"] | 0.0f;
        if (_stepperPtr && _stepperPtr->isHomed()) {
          _stepperPtr->moveToMM(_stepperPtr->getCurrentPositionMM() + mm);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // POST /api/homing — flag lu par taskMotion (pas d'extern dans lambda)
    server.on("/api/homing", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_homingRequested = true;
      req->send(200, "application/json", "{\"ok\":true,\"message\":\"homing scheduled\"}");
    });

    // POST /api/test/air — flag lu par taskMotion (plus d'appel direct depuis Core 0)
    server.on("/api/test/air", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_testAirRequested = true;
      req->send(200, "application/json", "{\"ok\":true,\"message\":\"test scheduled\"}");
    });

    // GET /api/lut
    server.on("/api/lut", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildLUTJSON());
    });

    // POST /api/lut — table complète
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/lut",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonArray arr = json["lut"].as<JsonArray>();
        for (JsonObject pt : arr) {
          int   note = pt["note"]     | -1;
          float pos  = pt["position"] | -1.0f;
          if (note >= MIDI_NOTE_MIN && note <= MIDI_NOTE_MAX && pos >= 0.0f && pos <= SLIDER_TRAVEL_MM) {
            runtimeCfg.lutPositions[note - MIDI_NOTE_MIN] = pos;
          }
        }
        saveConfigToNVS();
        syncStepperLUT();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // POST /api/lut/point — un seul point
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/lut/point",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        int   note = json["note"]        | -1;
        float pos  = json["position_mm"] | -1.0f;
        if (note >= MIDI_NOTE_MIN && note <= MIDI_NOTE_MAX
            && pos >= 0.0f && pos <= SLIDER_TRAVEL_MM) {
          runtimeCfg.lutPositions[note - MIDI_NOTE_MIN] = pos;
          saveConfigToNVS();
          syncStepperLUT();
          req->send(200, "application/json", "{\"ok\":true}");
        } else {
          req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"note ou position invalide\"}");
        }
      }));

    // GET /api/wifi
    server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (_wifiMgrPtr) req->send(200, "application/json", _wifiMgrPtr->getStatusJSON());
      else             req->send(503, "application/json", "{\"error\":\"no wifi manager\"}");
    });

    // POST /api/wifi
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        String ssid = json["ssid"]     | "";
        String pass = json["password"] | "";
        if (ssid.length() == 0) { req->send(400, "application/json", "{\"error\":\"ssid vide\"}"); return; }
        if (_wifiMgrPtr) {
          _wifiMgrPtr->saveSTACredentials(ssid, pass);
          _wifiMgrPtr->connectSTA();
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // DELETE /api/wifi
    server.on("/api/wifi", HTTP_DELETE, [](AsyncWebServerRequest* req) {
      if (_wifiMgrPtr) _wifiMgrPtr->clearSTACredentials();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // 404
    server.onNotFound([](AsyncWebServerRequest* req) {
      req->send(404, "text/plain", "Not found");
    });

    // WebSocket
    ws.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType type, void*, uint8_t*, size_t) {
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
      Serial.println(F("[Web] LittleFS mount failed — reformatage..."));
      LittleFS.format();
      LittleFS.begin();
    }

    loadConfigFromNVS();
    syncStepperLUT();  // propager la LUT chargée au StepperControl

    setupRoutes();
    server.begin();
    Serial.printf("[Web] Serveur démarré sur port %d\n", WEB_SERVER_PORT);
  }

  // Push WebSocket périodique — appeler depuis taskWeb loop
  void update() {
    ws.cleanupClients();
    if (millis() - lastWsPush >= WS_INTERVAL && ws.count() > 0) {
      lastWsPush = millis();
      ws.textAll(buildStatusJSON());
    }
  }
};

#endif // WEB_INTERFACE_H
