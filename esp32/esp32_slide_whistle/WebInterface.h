/*
 * WebInterface.h — API web multi-flûtes + pression (ESP32)
 *
 * Refactor v3 :
 *   - API REST à chemins fixes (pas de regex), id dans le body JSON ou en
 *     query string (?id=N). Compatible AsyncWebServer sans flag spécial.
 *   - WebSocket diffuse l'état complet (toutes flûtes + pression)
 *   - NVS : namespace "flute0", "flute1"... + "pressure"
 *
 * Routes :
 *   GET  /api/status                — global
 *   GET  /api/flutes                — liste résumée
 *   GET  /api/flute?id=N            — config détaillée
 *   POST /api/flute                 — body {id, ...champs}
 *   POST /api/flute/lut             — body {id, lut:[{note,position}…]}
 *   POST /api/flute/lut_point       — body {id, note, position_mm}
 *   POST /api/flute/note            — body {id, note, velocity, on}
 *   POST /api/flute/jog             — body {id, mm}
 *   POST /api/flute/test            — body {id}
 *   POST /api/flute/panic           — body {id}
 *   POST /api/homing                — homing global
 *   POST /api/panic                 — panic global
 *   GET  /api/pressure
 *   POST /api/pressure/config       — body {target,min,max,safety,max_fill_ms}
 *   POST /api/pressure/start|stop|reset
 *   GET  /api/wifi    POST /api/wifi    DELETE /api/wifi
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
#include "Orchestra.h"
#include "PressureControl.h"
#include "MIDIHandler.h"
#include "WiFiManager.h"

extern volatile SystemState sysState;
extern volatile bool g_homingRequested;
extern volatile int  g_testAirFluteId;
extern volatile bool g_pressureStartRequested;
extern volatile bool g_pressureStopRequested;
extern volatile bool g_pressureResetRequested;

class WebInterface {
private:
  AsyncWebServer server;
  AsyncWebSocket ws;
  Preferences    prefs;
  unsigned long  lastWsPush = 0;
  static constexpr unsigned long WS_INTERVAL = 200;

  Orchestra*       _orchestra = nullptr;
  PressureControl* _pressure  = nullptr;
  MIDIHandler*     _midi      = nullptr;
  WiFiManager*     _wifi      = nullptr;

  // ---- JSON builders ------------------------------------------------------

  void appendFluteStatus(JsonObject& obj, Flute* f) {
    if (!f) return;
    obj["id"]           = f->id();
    obj["name"]         = f->name();
    obj["enabled"]      = f->isEnabled();
    obj["muted"]        = f->isMuted();
    obj["midi_channel"] = f->midiChannel();
    obj["note_min"]     = f->noteMin();
    obj["note_max"]     = f->noteMax();
    obj["position_mm"]  = f->stepper().getCurrentPositionMM();
    obj["moving"]       = f->stepper().isMoving();
    obj["homed"]        = f->stepper().isHomed();
    obj["air_state"]    = f->air().getStateName();
    obj["air_open"]     = f->air().isSolenoidOpen();
    obj["pwm"]          = f->air().getCurrentPwm();
    obj["last_note"]    = f->lastNote();
    obj["note_active"]  = f->isNoteActive();
  }

  String buildStatusJSON() {
    DynamicJsonDocument doc(2048);
    const char* names[] = {"INIT","HOMING","READY","ERROR"};
    doc["sys_state"]         = names[(int)sysState];
    doc["uptime_ms"]         = millis();
    doc["midi_count"]        = _midi ? _midi->getMessageCount() : 0;
    doc["midi_last_channel"] = _midi ? _midi->getLastChannel() : 0;

    JsonArray flutes = doc.createNestedArray("flutes");
    if (_orchestra) {
      for (uint8_t i = 0; i < _orchestra->count(); ++i) {
        JsonObject o = flutes.createNestedObject();
        appendFluteStatus(o, _orchestra->get(i));
      }
    }

    if (_pressure && _pressure->isEnabled()) {
      JsonObject p        = doc.createNestedObject("pressure");
      p["state"]          = _pressure->getStateName();
      p["fault"]          = _pressure->getFaultName();
      p["pump_on"]        = _pressure->getPumpOn();
      p["pump_duty"]      = _pressure->getPumpDuty();
      p["pressure_kpa"]   = _pressure->getPressure();
      p["reservoir_ok"]   = _pressure->getReservoirOk();
      p["has_sensor"]     = _pressure->hasSensor();
    }

    String out;
    serializeJson(doc, out);
    return out;
  }

  String buildFluteConfigJSON(Flute* f) {
    DynamicJsonDocument doc(1536);
    doc["id"]            = f->id();
    doc["name"]          = f->name();
    doc["midi_channel"]  = f->midiChannel();
    doc["note_min"]      = f->noteMin();
    doc["note_max"]      = f->noteMax();
    doc["enabled"]       = f->isEnabled();
    doc["muted"]         = f->isMuted();
    doc["speed_mm_s"]    = f->stepper().getSpeedMmS();
    doc["accel_mm_s2"]   = f->stepper().getAccelMmS2();
    doc["pwm_full"]      = f->air().getPwmFull();
    doc["pwm_hold"]      = f->air().getPwmHold();
    doc["wait_delay_ms"] = f->air().getWaitDelay();
    doc["legato_ms"]     = f->air().getLegatoMs();
    doc["use_lut"]       = f->stepper().getUseLUT();
    doc["slider_travel_mm"] = f->config().sliderTravelMM;
    JsonArray lut = doc.createNestedArray("lut");
    const float* L = f->stepper().getLUT();
    for (int i = 0; i < MIDI_LUT_SIZE; ++i) {
      JsonObject pt = lut.createNestedObject();
      pt["note"]     = f->config().noteMin + i;
      pt["position"] = L[i];
    }
    String out; serializeJson(doc, out);
    return out;
  }

  String buildPressureJSON() {
    DynamicJsonDocument doc(512);
    if (!_pressure || !_pressure->isEnabled()) {
      doc["enabled"] = false;
    } else {
      doc["enabled"]      = true;
      doc["state"]        = _pressure->getStateName();
      doc["fault"]        = _pressure->getFaultName();
      doc["pump_on"]      = _pressure->getPumpOn();
      doc["pump_duty"]    = _pressure->getPumpDuty();
      doc["pressure_kpa"] = _pressure->getPressure();
      doc["reservoir_ok"] = _pressure->getReservoirOk();
      doc["has_sensor"]   = _pressure->hasSensor();
      doc["target"]       = _pressure->getTarget();
      doc["min"]          = _pressure->getMin();
      doc["max"]          = _pressure->getMax();
      doc["safety"]       = _pressure->getSafety();
      doc["max_fill_ms"]  = _pressure->getMaxFillMs();
    }
    String out; serializeJson(doc, out);
    return out;
  }

  // ---- NVS ---------------------------------------------------------------

  String fluteNs(uint8_t id) {
    char buf[16];
    snprintf(buf, sizeof(buf), "flute%u", id);
    return String(buf);
  }

  void saveFluteConfigNVS(Flute* f) {
    if (!f) return;
    prefs.begin(fluteNs(f->id()).c_str(), false);
    prefs.putUChar("ch",       f->midiChannel());
    prefs.putUChar("nMin",     f->noteMin());
    prefs.putUChar("nMax",     f->noteMax());
    prefs.putBool("enabled",   f->isEnabled());
    prefs.putBool("muted",     f->isMuted());
    prefs.putFloat("speed",    f->stepper().getSpeedMmS());
    prefs.putFloat("accel",    f->stepper().getAccelMmS2());
    prefs.putUChar("pwmFull",  f->air().getPwmFull());
    prefs.putUChar("pwmHold",  f->air().getPwmHold());
    prefs.putUShort("wait",    f->air().getWaitDelay());
    prefs.putUShort("legato",  f->air().getLegatoMs());
    prefs.putBool("useLUT",    f->stepper().getUseLUT());
    prefs.putBytes("lut",      f->stepper().getLUT(),
                   sizeof(float) * MIDI_LUT_SIZE);
    prefs.end();
  }

  void loadFluteConfigNVS(Flute* f) {
    if (!f) return;
    prefs.begin(fluteNs(f->id()).c_str(), true);
    f->setMidiChannel(prefs.getUChar("ch", f->midiChannel()));
    uint8_t lo = prefs.getUChar("nMin", f->noteMin());
    uint8_t hi = prefs.getUChar("nMax", f->noteMax());
    f->setNoteRange(lo, hi);
    f->setEnabled(prefs.getBool("enabled", true));
    f->setMuted(prefs.getBool("muted", false));
    f->stepper().setSpeed(prefs.getFloat("speed", DEFAULT_STEPPER_SPEED));
    f->stepper().setAcceleration(prefs.getFloat("accel", DEFAULT_STEPPER_ACCEL));
    f->air().setPwmFull(prefs.getUChar("pwmFull", DEFAULT_PWM_FULL));
    f->air().setPwmHold(prefs.getUChar("pwmHold", DEFAULT_PWM_HOLD));
    f->air().setWaitDelay(prefs.getUShort("wait", DEFAULT_POSITION_WAIT));
    f->air().setLegatoMs(prefs.getUShort("legato", DEFAULT_LEGATO_THRESHOLD));
    f->stepper().setUseLUT(prefs.getBool("useLUT", false));
    if (prefs.getBytesLength("lut") == sizeof(float) * MIDI_LUT_SIZE) {
      float buf[MIDI_LUT_SIZE];
      prefs.getBytes("lut", buf, sizeof(buf));
      f->stepper().setLUT(buf);
    }
    prefs.end();
  }

  void savePressureNVS() {
    if (!_pressure) return;
    prefs.begin("pressure", false);
    prefs.putFloat("target", _pressure->getTarget());
    prefs.putFloat("min",    _pressure->getMin());
    prefs.putFloat("max",    _pressure->getMax());
    prefs.putFloat("safety", _pressure->getSafety());
    prefs.putUShort("fillMs", _pressure->getMaxFillMs());
    prefs.end();
  }

  void loadPressureNVS() {
    if (!_pressure) return;
    prefs.begin("pressure", true);
    _pressure->setTarget(prefs.getFloat("target", _pressure->getTarget()));
    _pressure->setMin(prefs.getFloat("min", _pressure->getMin()));
    _pressure->setMax(prefs.getFloat("max", _pressure->getMax()));
    _pressure->setSafety(prefs.getFloat("safety", _pressure->getSafety()));
    _pressure->setMaxFillMs(prefs.getUShort("fillMs", _pressure->getMaxFillMs()));
    prefs.end();
  }

  // ---- Helpers ------------------------------------------------------------

  Flute* fluteFromJson(JsonVariant& json, AsyncWebServerRequest* req) {
    int id = json["id"] | -1;
    if (id < 0 || id >= (int)_orchestra->count()) {
      req->send(400, "application/json", "{\"error\":\"id invalide\"}");
      return nullptr;
    }
    return _orchestra->get(id);
  }

  // ---- Routes -------------------------------------------------------------

  void setupRoutes() {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // ---- STATUS -----------------------------------------------------------

    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildStatusJSON());
    });

    server.on("/api/flutes", HTTP_GET, [this](AsyncWebServerRequest* req) {
      DynamicJsonDocument doc(2048);
      JsonArray arr = doc.to<JsonArray>();
      for (uint8_t i = 0; i < _orchestra->count(); ++i) {
        JsonObject o = arr.createNestedObject();
        appendFluteStatus(o, _orchestra->get(i));
      }
      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    // GET /api/flute?id=N
    server.on("/api/flute", HTTP_GET, [this](AsyncWebServerRequest* req) {
      if (!req->hasParam("id")) { req->send(400, "application/json", "{\"error\":\"missing id\"}"); return; }
      int id = req->getParam("id")->value().toInt();
      Flute* f = _orchestra->get(id);
      if (!f) { req->send(404, "application/json", "{\"error\":\"flute not found\"}"); return; }
      req->send(200, "application/json", buildFluteConfigJSON(f));
    });

    // ---- POST /api/flute (config) ----------------------------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        JsonObject o = json.as<JsonObject>();

        if (o.containsKey("midi_channel")) {
          int v = o["midi_channel"];
          if (v < 0 || v > 16) { req->send(400, "application/json", "{\"error\":\"midi_channel\"}"); return; }
          f->setMidiChannel((uint8_t)v);
        }
        if (o.containsKey("note_min") && o.containsKey("note_max")) {
          int lo = o["note_min"], hi = o["note_max"];
          if (lo < 0 || hi > 127 || hi <= lo) { req->send(400, "application/json", "{\"error\":\"note range\"}"); return; }
          f->setNoteRange((uint8_t)lo, (uint8_t)hi);
        }
        if (o.containsKey("enabled")) f->setEnabled(o["enabled"].as<bool>());
        if (o.containsKey("muted"))   f->setMuted(o["muted"].as<bool>());
        if (o.containsKey("speed_mm_s")) {
          float v = o["speed_mm_s"];
          if (v < 5.0f || v > 500.0f) { req->send(400, "application/json", "{\"error\":\"speed\"}"); return; }
          f->stepper().setSpeed(v);
        }
        if (o.containsKey("accel_mm_s2")) {
          float v = o["accel_mm_s2"];
          if (v < 50.0f || v > 5000.0f) { req->send(400, "application/json", "{\"error\":\"accel\"}"); return; }
          f->stepper().setAcceleration(v);
        }
        if (o.containsKey("pwm_full")) {
          int v = o["pwm_full"];
          if (v < 0 || v > 255) { req->send(400, "application/json", "{\"error\":\"pwm_full\"}"); return; }
          f->air().setPwmFull((uint8_t)v);
        }
        if (o.containsKey("pwm_hold")) {
          int v = o["pwm_hold"];
          if (v < 0 || v > 255) { req->send(400, "application/json", "{\"error\":\"pwm_hold\"}"); return; }
          if (o.containsKey("pwm_full") && v >= (int)o["pwm_full"]) {
            req->send(400, "application/json", "{\"error\":\"pwm_hold doit < pwm_full\"}"); return;
          }
          f->air().setPwmHold((uint8_t)v);
        }
        if (o.containsKey("wait_delay_ms")) {
          int v = o["wait_delay_ms"];
          if (v < 0 || v > 2000) { req->send(400, "application/json", "{\"error\":\"wait\"}"); return; }
          f->air().setWaitDelay((uint16_t)v);
        }
        if (o.containsKey("legato_ms")) {
          int v = o["legato_ms"];
          if (v < 0 || v > 2000) { req->send(400, "application/json", "{\"error\":\"legato\"}"); return; }
          f->air().setLegatoMs((uint16_t)v);
        }
        if (o.containsKey("use_lut")) f->stepper().setUseLUT(o["use_lut"].as<bool>());

        saveFluteConfigNVS(f);
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- POST /api/flute/lut (table complète) ----------------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/lut",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        JsonArray arr = json["lut"].as<JsonArray>();
        float lut[MIDI_LUT_SIZE];
        memcpy(lut, f->stepper().getLUT(), sizeof(lut));
        for (JsonObject pt : arr) {
          int   note = pt["note"]     | -1;
          float pos  = pt["position"] | -1.0f;
          int idx = note - f->config().noteMin;
          if (idx >= 0 && idx < MIDI_LUT_SIZE
              && pos >= 0.0f && pos <= f->config().sliderTravelMM) {
            lut[idx] = pos;
          }
        }
        f->stepper().setLUT(lut);
        saveFluteConfigNVS(f);
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- POST /api/flute/lut_point (un seul point) -----------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/lut_point",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        int   note = json["note"]        | -1;
        float pos  = json["position_mm"] | -1.0f;
        int idx = note - f->config().noteMin;
        if (idx < 0 || idx >= MIDI_LUT_SIZE
            || pos < 0.0f || pos > f->config().sliderTravelMM) {
          req->send(400, "application/json", "{\"error\":\"note ou position invalide\"}"); return;
        }
        float lut[MIDI_LUT_SIZE];
        memcpy(lut, f->stepper().getLUT(), sizeof(lut));
        lut[idx] = pos;
        f->stepper().setLUT(lut);
        saveFluteConfigNVS(f);
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- POST /api/flute/note --------------------------------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/note",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        int  note = json["note"]     | 60;
        int  vel  = json["velocity"] | 100;
        bool on   = json["on"]       | true;
        if (_midi) {
          uint8_t ch = f->midiChannel() ? f->midiChannel() : 1;
          if (on) _midi->triggerNoteOn(ch, (uint8_t)note, (uint8_t)vel);
          else    _midi->triggerNoteOff(ch, (uint8_t)note);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- POST /api/flute/jog ---------------------------------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/jog",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        float mm = json["mm"] | 0.0f;
        if (f->stepper().isHomed()) {
          f->stepper().moveToMM(f->stepper().getCurrentPositionMM() + mm);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- POST /api/flute/test --------------------------------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/test",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        g_testAirFluteId = (int)f->id();
        req->send(200, "application/json", "{\"ok\":true,\"message\":\"test scheduled\"}");
      }));

    // ---- POST /api/flute/panic -------------------------------------------

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/panic",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        f->panic();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- Globaux ---------------------------------------------------------

    server.on("/api/homing", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_homingRequested = true;
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/panic", HTTP_POST, [this](AsyncWebServerRequest* req) {
      if (_orchestra) _orchestra->panicAll();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // ---- PRESSION ---------------------------------------------------------

    server.on("/api/pressure", HTTP_GET, [this](AsyncWebServerRequest* req) {
      req->send(200, "application/json", buildPressureJSON());
    });

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/pressure/config",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!_pressure || !_pressure->isEnabled()) {
          req->send(503, "application/json", "{\"error\":\"disabled\"}"); return;
        }
        JsonObject o = json.as<JsonObject>();
        if (o.containsKey("target"))      _pressure->setTarget(o["target"].as<float>());
        if (o.containsKey("min"))         _pressure->setMin(o["min"].as<float>());
        if (o.containsKey("max"))         _pressure->setMax(o["max"].as<float>());
        if (o.containsKey("safety"))      _pressure->setSafety(o["safety"].as<float>());
        if (o.containsKey("max_fill_ms")) _pressure->setMaxFillMs(o["max_fill_ms"].as<uint16_t>());
        savePressureNVS();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    server.on("/api/pressure/start", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_pressureStartRequested = true;
      req->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/pressure/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_pressureStopRequested = true;
      req->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/pressure/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_pressureResetRequested = true;
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // ---- WIFI ------------------------------------------------------------

    server.on("/api/wifi", HTTP_GET, [this](AsyncWebServerRequest* req) {
      if (_wifi) req->send(200, "application/json", _wifi->getStatusJSON());
      else       req->send(503, "application/json", "{\"error\":\"no wifi\"}");
    });
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        String ssid = json["ssid"]     | "";
        String pass = json["password"] | "";
        if (ssid.length() == 0) { req->send(400, "application/json", "{\"error\":\"ssid vide\"}"); return; }
        if (_wifi) {
          _wifi->saveSTACredentials(ssid, pass);
          _wifi->connectSTA();
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));
    server.on("/api/wifi", HTTP_DELETE, [this](AsyncWebServerRequest* req) {
      if (_wifi) _wifi->clearSTACredentials();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // ---- 404 + WS ---------------------------------------------------------

    server.onNotFound([](AsyncWebServerRequest* req) {
      req->send(404, "text/plain", "Not found");
    });

    ws.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType type, void*, uint8_t*, size_t) {
      if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] client #%u connecté\n", client->id());
        client->text(buildStatusJSON());
      }
    });
    server.addHandler(&ws);
  }

public:
  WebInterface() : server(WEB_SERVER_PORT), ws(WS_PATH) {}

  void begin(Orchestra* orchestra, PressureControl* pressure,
             MIDIHandler* midi, WiFiManager* wifi) {
    _orchestra = orchestra;
    _pressure  = pressure;
    _midi      = midi;
    _wifi      = wifi;

    if (!LittleFS.begin(true)) {
      Serial.println(F("[Web] LittleFS mount failed — reformatage..."));
      LittleFS.format();
      LittleFS.begin();
    }

    if (_orchestra) {
      for (uint8_t i = 0; i < _orchestra->count(); ++i) {
        loadFluteConfigNVS(_orchestra->get(i));
      }
    }
    loadPressureNVS();

    setupRoutes();
    server.begin();
    Serial.printf("[Web] serveur démarré sur :%d (%u flûtes)\n",
                  WEB_SERVER_PORT, _orchestra ? _orchestra->count() : 0);
  }

  void update() {
    ws.cleanupClients();
    if (millis() - lastWsPush >= WS_INTERVAL && ws.count() > 0) {
      lastWsPush = millis();
      ws.textAll(buildStatusJSON());
    }
  }
};

#endif // WEB_INTERFACE_H
