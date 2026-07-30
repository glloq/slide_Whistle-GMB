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
#include <AsyncJson.h>          // AsyncCallbackJsonWebHandler (esphome fork)
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include "NVSKeys.h"
#include "NVSStore.h"
#include "settings.h"
#include "Orchestra.h"
#include "PressureControl.h"
#include "MIDIHandler.h"
#include "WiFiManager.h"
#include "DemoPlayer.h"

extern volatile SystemState sysState;
extern volatile bool g_homingRequested;
extern volatile int  g_homingFluteId;     // -1 = aucun, sinon id flûte à re-homer
extern volatile int  g_testAirFluteId;
extern volatile bool g_pressureStartRequested;
extern volatile bool g_pressureStopRequested;
extern volatile bool g_pressureResetRequested;
extern volatile int  g_demoMelodyId;      // -1 = stop, sinon index de mélodie à lancer
extern volatile bool g_demoLoop;
extern volatile int  g_sweepFluteId;
extern volatile int  g_stressDurationSec;

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
  DemoPlayer*      _demo      = nullptr;

  // ---- JSON builders ------------------------------------------------------

  void appendFluteStatus(JsonObject& obj, Flute* f) {
    if (!f) return;
    obj["id"]           = f->id();
    obj["name"]         = f->name();
    obj["custom_name"]  = f->customName();
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
    obj["slider_travel_mm"] = f->config().sliderTravelMM;
  }

  String buildStatusJSON() {
    DynamicJsonDocument doc(2048);
    const char* names[] = {"INIT","HOMING","READY","ERROR"};
    doc["sys_state"]         = names[(int)sysState];
    doc["uptime_ms"]         = millis();
    doc["midi_count"]        = _midi ? _midi->getMessageCount() : 0;
    doc["midi_last_channel"] = _midi ? _midi->getLastChannel() : 0;
    if (_midi) {
      JsonArray ch = doc.createNestedArray("midi_count_by_channel");
      for (int i = 1; i <= 16; ++i) ch.add(_midi->getMessageCountForChannel(i));
    }

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
    doc["custom_name"]   = f->customName();
    doc["default_name"]  = f->config().name;
    doc["midi_channel"]  = f->midiChannel();
    doc["note_min"]      = f->noteMin();
    doc["note_max"]      = f->noteMax();
    doc["enabled"]       = f->isEnabled();
    doc["muted"]         = f->isMuted();
    doc["speed_mm_s"]    = f->stepper().getSpeedMmS();
    doc["accel_mm_s2"]   = f->stepper().getAccelMmS2();
    doc["pwm_full"]       = f->air().getPwmFull();
    doc["pwm_hold"]       = f->air().getPwmHold();
    doc["wait_delay_ms"]  = f->air().getWaitDelay();
    doc["legato_ms"]      = f->air().getLegatoMs();
    doc["velocity_curve"] = f->air().getVelocityCurve();
    doc["cc_breath"]      = f->getCcBreath();
    doc["cc_expression"]  = f->getCcExpr();
    doc["cc_volume"]      = f->getCcVolume();
    doc["cc_vibrato"]     = f->getCcVibrato();
    doc["cc_sustain"]     = f->getCcSustain();
    doc["transpose"]      = f->getTranspose();
    doc["use_lut"]        = f->stepper().getUseLUT();
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
  // Délégués au module NVSStore (cf. NVSStore.h). Les wrappers ci-dessous
  // existent pour ne pas modifier les call sites des routes.

  void saveFluteConfigNVS(Flute* f)        { NVSStore::saveFlute(prefs, f); }
  void loadFluteConfigNVS(Flute* f)        { NVSStore::loadFlute(prefs, f); }
  void savePressureNVS()                   { NVSStore::savePressure(prefs, _pressure); }
  void loadPressureNVS()                   { NVSStore::loadPressure(prefs, _pressure); }

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
        if (o.containsKey("velocity_curve")) {
          int v = o["velocity_curve"];
          if (v < 0 || v > 2) { req->send(400, "application/json", "{\"error\":\"velocity_curve\"}"); return; }
          f->air().setVelocityCurve((uint8_t)v);
        }
        if (o.containsKey("custom_name")) {
          f->setCustomName((const char*)o["custom_name"]);
        }
        // CC mapping (0 = désactivé)
        auto ccCheck = [&](const char* key, void(Flute::*setter)(uint8_t))->bool {
          if (!o.containsKey(key)) return true;
          int v = o[key];
          if (v < 0 || v > 127) {
            req->send(400, "application/json", "{\"error\":\"cc out of range\"}"); return false;
          }
          (f->*setter)((uint8_t)v); return true;
        };
        if (!ccCheck("cc_breath",     &Flute::setCcBreath))  return;
        if (!ccCheck("cc_expression", &Flute::setCcExpr))    return;
        if (!ccCheck("cc_volume",     &Flute::setCcVolume))  return;
        if (!ccCheck("cc_vibrato",    &Flute::setCcVibrato)) return;
        if (!ccCheck("cc_sustain",    &Flute::setCcSustain)) return;
        if (o.containsKey("transpose")) {
          int v = o["transpose"];
          if (v < -36 || v > 36) { req->send(400, "application/json", "{\"error\":\"transpose\"}"); return; }
          f->setTranspose((int8_t)v);
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

    // ---- POST /api/flute/sweep -------------------------------------------
    // Déplace le slide de 0 → travelMM → 0 (visualisation/test mécanique)

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/sweep",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        if (!f->stepper().isHomed()) {
          req->send(409, "application/json", "{\"error\":\"flute not homed\"}"); return;
        }
        g_sweepFluteId = (int)f->id();
        req->send(200, "application/json", "{\"ok\":true,\"message\":\"sweep scheduled\"}");
      }));

    // ---- POST /api/flutes/all --------------------------------------------
    // Commandes globales : {action: "muteAll"|"unmuteAll"|"enableAll"|"disableAll"|"panic"}

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flutes/all",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        String action = json["action"] | "";
        if (!_orchestra) { req->send(503, "application/json", "{}"); return; }
        for (uint8_t i = 0; i < _orchestra->count(); ++i) {
          Flute* f = _orchestra->get(i);
          if (!f) continue;
          if      (action == "muteAll")    f->setMuted(true);
          else if (action == "unmuteAll")  f->setMuted(false);
          else if (action == "enableAll")  f->setEnabled(true);
          else if (action == "disableAll") f->setEnabled(false);
          else if (action == "panic")      f->panic();
          else                             { req->send(400, "application/json", "{\"error\":\"unknown action\"}"); return; }
          if (action != "panic") saveFluteConfigNVS(f);
        }
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

    // POST /api/flute/homing — body {id}
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/homing",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        Flute* f = fluteFromJson(json, req);
        if (!f) return;
        g_homingFluteId = (int)f->id();
        req->send(200, "application/json", "{\"ok\":true,\"message\":\"homing scheduled\"}");
      }));

    // GET /api/midi/log.csv — export CSV téléchargeable
    server.on("/api/midi/log.csv", HTTP_GET, [this](AsyncWebServerRequest* req) {
      String csv = "t_rel_ms,type,channel,data1,data2,bend\n";
      if (_midi) {
        const char* types[] = {"NoteOn","NoteOff","CC","PitchBend","Aftertouch"};
        unsigned long now = millis();
        _midi->forEachLog([&](const auto& e) {
          csv += String((uint32_t)(now - e.ts)) + ",";
          csv += String(types[e.type < 5 ? e.type : 0]) + ",";
          csv += String(e.channel) + ",";
          csv += String(e.data1) + ",";
          csv += String(e.data2) + ",";
          csv += String(e.bend) + "\n";
        });
      }
      AsyncWebServerResponse* r = req->beginResponse(200, "text/csv", csv);
      r->addHeader("Content-Disposition", "attachment; filename=\"midi_log.csv\"");
      req->send(r);
    });

    // GET /api/midi/log — dernières activités MIDI (ring buffer)
    server.on("/api/midi/log", HTTP_GET, [this](AsyncWebServerRequest* req) {
      DynamicJsonDocument doc(4096);
      JsonArray arr = doc.to<JsonArray>();
      if (_midi) {
        const char* types[] = {"NoteOn","NoteOff","CC","PitchBend","Aftertouch"};
        unsigned long now = millis();
        _midi->forEachLog([&](const auto& e) {
          JsonObject o = arr.createNestedObject();
          o["t_rel_ms"] = (uint32_t)(now - e.ts);
          o["type"]     = types[e.type < 5 ? e.type : 0];
          o["channel"]  = e.channel;
          o["data1"]    = e.data1;
          o["data2"]    = e.data2;
          if (e.type == 3) o["bend"] = e.bend;
        });
      }
      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    // ---- PRESETS ---------------------------------------------------------
    // Presets globaux : snapshot complet de toutes les flûtes + pression.
    // Stockés en NVS namespace "presets" (clés = noms).

    server.on("/api/presets", HTTP_GET, [this](AsyncWebServerRequest* req) {
      DynamicJsonDocument doc(1024);
      JsonArray arr = doc.to<JsonArray>();
      prefs.begin("presets_idx", true);
      String list = prefs.getString("list", "");
      prefs.end();
      // list est CSV de noms
      int start = 0;
      while (start < (int)list.length()) {
        int sep = list.indexOf(',', start);
        String name = (sep < 0) ? list.substring(start) : list.substring(start, sep);
        if (name.length() > 0) arr.add(name);
        if (sep < 0) break;
        start = sep + 1;
      }
      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/presets/save",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        String name = json["name"] | "";
        if (name.length() == 0 || name.length() > 14) {
          req->send(400, "application/json", "{\"error\":\"name 1-14 chars\"}"); return;
        }
        // Construire le snapshot complet en JSON
        DynamicJsonDocument doc(8192);
        JsonArray flutes = doc.createNestedArray("flutes");
        for (uint8_t i = 0; i < _orchestra->count(); ++i) {
          Flute* f = _orchestra->get(i);
          if (!f) continue;
          JsonObject o = flutes.createNestedObject();
          o["id"]            = f->id();
          o["midi_channel"]  = f->midiChannel();
          o["note_min"]      = f->noteMin();
          o["note_max"]      = f->noteMax();
          o["enabled"]       = f->isEnabled();
          o["muted"]         = f->isMuted();
          o["speed"]         = f->stepper().getSpeedMmS();
          o["accel"]         = f->stepper().getAccelMmS2();
          o["pwm_full"]      = f->air().getPwmFull();
          o["pwm_hold"]      = f->air().getPwmHold();
          o["wait_delay_ms"] = f->air().getWaitDelay();
          o["legato_ms"]     = f->air().getLegatoMs();
          o["velocity_curve"] = f->air().getVelocityCurve();
          o["use_lut"]       = f->stepper().getUseLUT();
          JsonArray lut = o.createNestedArray("lut");
          const float* L = f->stepper().getLUT();
          for (int j = 0; j < MIDI_LUT_SIZE; ++j) lut.add(L[j]);
        }
        if (_pressure && _pressure->isEnabled()) {
          JsonObject p = doc.createNestedObject("pressure");
          p["target"]      = _pressure->getTarget();
          p["min"]         = _pressure->getMin();
          p["max"]         = _pressure->getMax();
          p["safety"]      = _pressure->getSafety();
          p["max_fill_ms"] = _pressure->getMaxFillMs();
        }
        String snapshot;
        serializeJson(doc, snapshot);

        // Stocker
        prefs.begin("presets", false);
        prefs.putString(name.c_str(), snapshot);
        prefs.end();
        // Mettre à jour l'index
        prefs.begin("presets_idx", false);
        String list = prefs.getString("list", "");
        if (list.indexOf(name) < 0) {
          if (list.length()) list += ",";
          list += name;
          prefs.putString("list", list);
        }
        prefs.end();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/presets/load",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        String name = json["name"] | "";
        if (name.length() == 0) { req->send(400, "application/json", "{\"error\":\"name\"}"); return; }
        prefs.begin("presets", true);
        String snapshot = prefs.getString(name.c_str(), "");
        prefs.end();
        if (snapshot.length() == 0) { req->send(404, "application/json", "{\"error\":\"not found\"}"); return; }

        DynamicJsonDocument doc(8192);
        DeserializationError err = deserializeJson(doc, snapshot);
        if (err) { req->send(500, "application/json", "{\"error\":\"corrupt preset\"}"); return; }

        for (JsonObject o : doc["flutes"].as<JsonArray>()) {
          int id = o["id"] | -1;
          Flute* f = _orchestra->get(id);
          if (!f) continue;
          if (o.containsKey("midi_channel")) f->setMidiChannel(o["midi_channel"]);
          if (o.containsKey("note_min") && o.containsKey("note_max")) {
            f->setNoteRange(o["note_min"], o["note_max"]);
          }
          if (o.containsKey("enabled"))  f->setEnabled(o["enabled"]);
          if (o.containsKey("muted"))    f->setMuted(o["muted"]);
          if (o.containsKey("speed"))    f->stepper().setSpeed(o["speed"]);
          if (o.containsKey("accel"))    f->stepper().setAcceleration(o["accel"]);
          if (o.containsKey("pwm_full")) f->air().setPwmFull(o["pwm_full"]);
          if (o.containsKey("pwm_hold")) f->air().setPwmHold(o["pwm_hold"]);
          if (o.containsKey("wait_delay_ms")) f->air().setWaitDelay(o["wait_delay_ms"]);
          if (o.containsKey("legato_ms"))     f->air().setLegatoMs(o["legato_ms"]);
          if (o.containsKey("velocity_curve")) f->air().setVelocityCurve(o["velocity_curve"]);
          if (o.containsKey("use_lut"))       f->stepper().setUseLUT(o["use_lut"]);
          if (o.containsKey("lut")) {
            float lut[MIDI_LUT_SIZE];
            int j = 0;
            for (float v : o["lut"].as<JsonArray>()) {
              if (j < MIDI_LUT_SIZE) lut[j++] = v;
            }
            if (j == MIDI_LUT_SIZE) f->stepper().setLUT(lut);
          }
          saveFluteConfigNVS(f);
        }
        if (doc.containsKey("pressure") && _pressure) {
          JsonObject p = doc["pressure"];
          if (p.containsKey("target"))      _pressure->setTarget(p["target"]);
          if (p.containsKey("min"))         _pressure->setMin(p["min"]);
          if (p.containsKey("max"))         _pressure->setMax(p["max"]);
          if (p.containsKey("safety"))      _pressure->setSafety(p["safety"]);
          if (p.containsKey("max_fill_ms")) _pressure->setMaxFillMs(p["max_fill_ms"]);
          savePressureNVS();
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/presets/delete",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        String name = json["name"] | "";
        if (name.length() == 0) { req->send(400, "application/json", "{\"error\":\"name\"}"); return; }
        prefs.begin("presets", false);
        prefs.remove(name.c_str());
        prefs.end();
        prefs.begin("presets_idx", false);
        String list = prefs.getString("list", "");
        // retirer le nom (avec virgules)
        String needle1 = name + ",";
        String needle2 = "," + name;
        if (list.startsWith(needle1)) list = list.substring(needle1.length());
        else if (list.indexOf(needle2) >= 0) list.replace(needle2, "");
        else if (list == name) list = "";
        prefs.putString("list", list);
        prefs.end();
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- SYSTEM / DIAGNOSTIC ---------------------------------------------

    server.on("/api/system", HTTP_GET, [this](AsyncWebServerRequest* req) {
      DynamicJsonDocument doc(2048);
      doc["chip_model"]   = ESP.getChipModel();
      doc["chip_revision"]= ESP.getChipRevision();
      doc["chip_cores"]   = ESP.getChipCores();
      doc["sdk_version"]  = ESP.getSdkVersion();
      doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
      doc["heap_free"]    = ESP.getFreeHeap();
      doc["heap_min"]     = ESP.getMinFreeHeap();
      doc["heap_size"]    = ESP.getHeapSize();
      doc["psram_free"]   = ESP.getFreePsram();
      doc["flash_size"]   = ESP.getFlashChipSize();
      doc["uptime_ms"]    = millis();
      doc["sys_state"]    = (int)sysState;
      doc["mac"]          = WiFi.macAddress();
      doc["fw_version"]   = "v3";

      // Diagnostic par flûte : état endstop + pins assignés
      JsonArray flutes = doc.createNestedArray("flutes");
      if (_orchestra) {
        for (uint8_t i = 0; i < _orchestra->count(); ++i) {
          Flute* f = _orchestra->get(i);
          if (!f) continue;
          const auto& c = f->config();
          JsonObject o = flutes.createNestedObject();
          o["id"]            = f->id();
          o["name"]          = f->name();
          o["pin_step"]      = c.stepperStepPin;
          o["pin_dir"]       = c.stepperDirPin;
          o["pin_en"]        = c.stepperEnablePin;
          o["pin_endstop"]   = c.endstopPin;
          o["pin_solenoid"]  = c.solenoidPin;
          o["pin_servo"]     = c.servoPin;
          o["ledc_solenoid"] = c.solenoidLedcChannel;
          o["servo_timer"]   = c.servoTimerIndex;
          o["endstop_active"] = (c.endstopActiveState == LOW) ? "LOW" : "HIGH";
          o["endstop_state"] = digitalRead(c.endstopPin);
          o["endstop_triggered"] =
            (digitalRead(c.endstopPin) == c.endstopActiveState);
          o["homed"]         = f->stepper().isHomed();
          o["position_mm"]   = f->stepper().getCurrentPositionMM();
        }
      }

      // Diagnostic pression : raw ADC
      if (_pressure && _pressure->isEnabled()) {
        JsonObject p = doc.createNestedObject("pressure");
        p["state"]   = _pressure->getStateName();
        p["pump_on"] = _pressure->getPumpOn();
        p["kpa"]     = _pressure->getPressure();
        p["has_sensor"] = _pressure->hasSensor();
        if (_pressure->hasSensor()) {
          // raw ADC du capteur — relire ici
          int rawAdc = analogRead(DEFAULT_PRESSURE_CONFIG.pressureSensorPin);
          p["adc_raw"] = rawAdc;
          p["adc_pin"] = DEFAULT_PRESSURE_CONFIG.pressureSensorPin;
        }
        p["reservoir_ok"] = _pressure->getReservoirOk();
        p["fault"]   = _pressure->getFaultName();
      }

      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    // POST /api/system/reboot
    server.on("/api/system/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
      req->send(200, "application/json", "{\"ok\":true,\"message\":\"reboot in 1s\"}");
      delay(100);
      // Programmer un reboot dans une tâche dédiée pour laisser la réponse partir
      xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
      }, "reboot", 2048, nullptr, 1, nullptr);
    });

    // ---- OTA firmware update --------------------------------------------
    // POST /api/system/update — multipart/form-data avec un fichier .bin
    // (compilé avec le même schéma de partition).
    // Le sketch flashe le binaire dans la partition OTA inactive puis reboot.

    server.on("/api/system/update", HTTP_POST,
      // Réponse finale après le upload
      [](AsyncWebServerRequest* req) {
        bool ok = !Update.hasError();
        AsyncWebServerResponse* r = req->beginResponse(
          ok ? 200 : 500,
          "application/json",
          ok ? "{\"ok\":true,\"message\":\"reboot in 2s\"}"
             : "{\"ok\":false,\"error\":\"update failed\"}");
        r->addHeader("Connection", "close");
        req->send(r);
        if (ok) {
          xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP.restart();
          }, "ota_reboot", 2048, nullptr, 1, nullptr);
        }
      },
      // Handler upload (appelé en chunks)
      [](AsyncWebServerRequest* req, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          Serial.printf("[OTA] début upload : %s\n", filename.c_str());
          // U_FLASH = firmware (sketch). U_SPIFFS = LittleFS.
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
          }
        }
        if (len) {
          if (Update.write(data, len) != len) Update.printError(Serial);
        }
        if (final) {
          if (Update.end(true)) {
            Serial.printf("[OTA] succès : %u bytes\n", (unsigned)(index + len));
          } else {
            Update.printError(Serial);
          }
        }
      });

    // ---- DEMO PLAYER ------------------------------------------------------

    server.on("/api/demo", HTTP_GET, [this](AsyncWebServerRequest* req) {
      DynamicJsonDocument doc(512);
      JsonArray arr = doc.createNestedArray("melodies");
      for (uint8_t i = 0; i < DemoPlayer::melodyCount(); ++i) {
        JsonObject o = arr.createNestedObject();
        o["id"]   = i;
        o["name"] = DemoPlayer::melodyName(i);
      }
      doc["playing"] = _demo ? _demo->isPlaying() : false;
      if (_demo && _demo->isPlaying()) {
        doc["current"] = _demo->melodyName();
        doc["channel"] = _demo->channel();
      }
      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/demo/play",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        int id   = json["id"]   | -1;
        bool loop = json["loop"] | false;
        if (id < 0 || id >= DemoPlayer::melodyCount()) {
          req->send(400, "application/json", "{\"error\":\"melody id\"}"); return;
        }
        g_demoMelodyId = id;
        g_demoLoop     = loop;
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    server.on("/api/demo/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
      g_demoMelodyId = -2;   // -2 = stop signal (différent de -1 = idle)
      req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/stress {duration_sec} — burn-in : notes aléatoires
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/stress",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        int sec = json["duration_sec"] | 30;
        if (sec < 0 || sec > 600) { req->send(400, "application/json", "{\"error\":\"0-600s\"}"); return; }
        g_stressDurationSec = sec;
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- SOLO -------------------------------------------------------------
    // Met en mute toutes les flûtes sauf {id}. Si id < 0, libère le solo.

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/flute/solo",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        int soloId = json["id"] | -1;
        for (uint8_t i = 0; i < _orchestra->count(); ++i) {
          Flute* f = _orchestra->get(i);
          if (!f) continue;
          if (soloId < 0)         f->setMuted(false);
          else                    f->setMuted((int)f->id() != soloId);
          saveFluteConfigNVS(f);
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

    // ---- BACKUP / RESTORE complet ----------------------------------------
    // GET  /api/backup → JSON snapshot complet (downloadable)
    // POST /api/restore → applique un snapshot uploadé

    server.on("/api/backup", HTTP_GET, [this](AsyncWebServerRequest* req) {
      DynamicJsonDocument doc(16384);
      doc["version"] = "v3";
      doc["ts_ms"]   = millis();
      JsonArray flutes = doc.createNestedArray("flutes");
      for (uint8_t i = 0; i < _orchestra->count(); ++i) {
        Flute* f = _orchestra->get(i);
        if (!f) continue;
        JsonObject o = flutes.createNestedObject();
        o["id"]            = f->id();
        o["midi_channel"]  = f->midiChannel();
        o["note_min"]      = f->noteMin();
        o["note_max"]      = f->noteMax();
        o["enabled"]       = f->isEnabled();
        o["muted"]         = f->isMuted();
        o["speed"]         = f->stepper().getSpeedMmS();
        o["accel"]         = f->stepper().getAccelMmS2();
        o["pwm_full"]      = f->air().getPwmFull();
        o["pwm_hold"]      = f->air().getPwmHold();
        o["wait_delay_ms"] = f->air().getWaitDelay();
        o["legato_ms"]     = f->air().getLegatoMs();
        o["velocity_curve"] = f->air().getVelocityCurve();
        o["use_lut"]       = f->stepper().getUseLUT();
        JsonArray lut = o.createNestedArray("lut");
        const float* L = f->stepper().getLUT();
        for (int j = 0; j < MIDI_LUT_SIZE; ++j) lut.add(L[j]);
      }
      if (_pressure && _pressure->isEnabled()) {
        JsonObject p = doc.createNestedObject("pressure");
        p["target"]      = _pressure->getTarget();
        p["min"]         = _pressure->getMin();
        p["max"]         = _pressure->getMax();
        p["safety"]      = _pressure->getSafety();
        p["max_fill_ms"] = _pressure->getMaxFillMs();
      }
      String out; serializeJson(doc, out);
      AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", out);
      resp->addHeader("Content-Disposition", "attachment; filename=\"slidewhistle_backup.json\"");
      req->send(resp);
    });

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/restore",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        // Réutilise la même logique que /api/presets/load (mais sans NVS de preset)
        for (JsonObject o : json["flutes"].as<JsonArray>()) {
          int id = o["id"] | -1;
          Flute* f = _orchestra->get(id);
          if (!f) continue;
          if (o.containsKey("midi_channel")) f->setMidiChannel(o["midi_channel"]);
          if (o.containsKey("note_min") && o.containsKey("note_max")) {
            f->setNoteRange(o["note_min"], o["note_max"]);
          }
          if (o.containsKey("enabled"))  f->setEnabled(o["enabled"]);
          if (o.containsKey("muted"))    f->setMuted(o["muted"]);
          if (o.containsKey("speed"))    f->stepper().setSpeed(o["speed"]);
          if (o.containsKey("accel"))    f->stepper().setAcceleration(o["accel"]);
          if (o.containsKey("pwm_full")) f->air().setPwmFull(o["pwm_full"]);
          if (o.containsKey("pwm_hold")) f->air().setPwmHold(o["pwm_hold"]);
          if (o.containsKey("wait_delay_ms")) f->air().setWaitDelay(o["wait_delay_ms"]);
          if (o.containsKey("legato_ms"))     f->air().setLegatoMs(o["legato_ms"]);
          if (o.containsKey("velocity_curve")) f->air().setVelocityCurve(o["velocity_curve"]);
          if (o.containsKey("use_lut"))       f->stepper().setUseLUT(o["use_lut"]);
          if (o.containsKey("lut")) {
            float lut[MIDI_LUT_SIZE]; int j = 0;
            for (float v : o["lut"].as<JsonArray>()) {
              if (j < MIDI_LUT_SIZE) lut[j++] = v;
            }
            if (j == MIDI_LUT_SIZE) f->stepper().setLUT(lut);
          }
          saveFluteConfigNVS(f);
        }
        if (json.as<JsonObject>().containsKey("pressure") && _pressure) {
          JsonObject p = json["pressure"];
          if (p.containsKey("target"))      _pressure->setTarget(p["target"]);
          if (p.containsKey("min"))         _pressure->setMin(p["min"]);
          if (p.containsKey("max"))         _pressure->setMax(p["max"]);
          if (p.containsKey("safety"))      _pressure->setSafety(p["safety"]);
          if (p.containsKey("max_fill_ms")) _pressure->setMaxFillMs(p["max_fill_ms"]);
          savePressureNVS();
        }
        req->send(200, "application/json", "{\"ok\":true}");
      }));

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
    // GET /api/wifi/scan — liste les réseaux disponibles
    server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
      if (!_wifi) { req->send(503, "application/json", "{}"); return; }
      req->send(200, "application/json", _wifi->scanNetworksJSON());
    });
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi",
      [this](AsyncWebServerRequest* req, JsonVariant& json) {
        String ssid = json["ssid"]     | "";
        String pass = json["password"] | "";
        if (!_wifi) { req->send(503, "application/json", "{\"error\":\"no wifi\"}"); return; }
        if (!_wifi->saveSTACredentials(ssid, pass)) {
          req->send(400, "application/json",
                    "{\"error\":\"ssid 1-32 chars, password 0 ou 8-63 chars\"}");
          return;
        }
        _wifi->connectSTA();
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
             MIDIHandler* midi, WiFiManager* wifi,
             DemoPlayer* demo = nullptr) {
    _orchestra = orchestra;
    _pressure  = pressure;
    _midi      = midi;
    _wifi      = wifi;
    _demo      = demo;

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
