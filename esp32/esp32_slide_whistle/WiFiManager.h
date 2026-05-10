/*
 * WiFiManager.h — Gestion WiFi double mode (ESP32)
 *
 * Mode AP  (hotspot) : démarré par défaut, SSID "SlideWhistle"
 * Mode STA (client)  : connexion à un réseau externe si configuré
 * Les credentials STA sont stockés en NVS via Preferences.
 *
 * Bibliothèques : WiFi (incluse ESP32), Preferences (incluse ESP32)
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "settings.h"

#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "slidewhistle"
#endif

class WiFiManager {
private:
  Preferences prefs;
  String staSSID;
  String staPassword;
  bool staConnected = false;
  bool apActive     = false;

public:
  void begin() {
    prefs.begin("wifi", false);
    staSSID     = prefs.getString("ssid",     WIFI_STA_SSID);
    staPassword = prefs.getString("password", WIFI_STA_PASSWORD);
    prefs.end();

    WiFi.mode(WIFI_AP_STA);

    // Toujours démarrer l'AP
    startAP();

    // Tenter connexion STA si credentials disponibles
    if (staSSID.length() > 0) {
      connectSTA();
    }

    // mDNS — http://slidewhistle.local (et variante par hostname personnalisé)
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", WEB_SERVER_PORT);
#if DEBUG_MODE
      Serial.printf("[mDNS] hôte : http://%s.local\n", MDNS_HOSTNAME);
#endif
    } else {
#if DEBUG_MODE
      Serial.println(F("[mDNS] échec initialisation"));
#endif
    }
  }

  void startAP() {
    String apPass = String(WIFI_AP_PASSWORD);
    if (apPass.length() < 8) apPass = "";

    WiFi.softAP(WIFI_AP_SSID, apPass.length() > 0 ? apPass.c_str() : nullptr,
                WIFI_AP_CHANNEL);

    IPAddress apIP(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apIP, subnet);

    apActive = true;
#if DEBUG_MODE
    Serial.printf("[WiFi] AP démarré — SSID: %s  IP: %s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
#endif
  }

  bool connectSTA() {
    if (staSSID.length() == 0) return false;

#if DEBUG_MODE
    Serial.printf("[WiFi] Connexion STA à '%s'...\n", staSSID.c_str());
#endif
    WiFi.begin(staSSID.c_str(), staPassword.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
      delay(200);
    }

    staConnected = (WiFi.status() == WL_CONNECTED);
#if DEBUG_MODE
    if (staConnected) {
      Serial.printf("[WiFi] STA connecté — IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println(F("[WiFi] STA: échec connexion"));
    }
#endif
    return staConnected;
  }

  // Sauvegarde des credentials STA en NVS.
  // Validation : SSID 1-32 octets (norme 802.11), password 0 ou 8-63 caractères
  // (WPA/WPA2). Renvoie false si invalide.
  bool saveSTACredentials(const String& ssid, const String& password) {
    if (ssid.length() == 0 || ssid.length() > 32) {
#if DEBUG_MODE
      Serial.printf("[WiFi] SSID invalide (longueur %u, attendu 1-32)\n", ssid.length());
#endif
      return false;
    }
    // password vide = réseau ouvert ; sinon WPA/WPA2 = 8-63
    if (password.length() != 0 && (password.length() < 8 || password.length() > 63)) {
#if DEBUG_MODE
      Serial.printf("[WiFi] mot de passe invalide (longueur %u, attendu 0 ou 8-63)\n",
                    password.length());
#endif
      return false;
    }
    staSSID = ssid;
    staPassword = password;
    prefs.begin("wifi", false);
    prefs.putString("ssid",     ssid);
    prefs.putString("password", password);
    prefs.end();
    return true;
  }

  void clearSTACredentials() {
    staSSID = "";
    staPassword = "";
    prefs.begin("wifi", false);
    prefs.remove("ssid");
    prefs.remove("password");
    prefs.end();
    WiFi.disconnect();
    staConnected = false;
  }

  bool isSTAConnected() { return WiFi.status() == WL_CONNECTED; }
  bool isAPActive()     { return apActive; }

  String getAPIP()  { return WiFi.softAPIP().toString(); }
  String getSTAIP() { return WiFi.localIP().toString(); }
  String getSTASSID() { return staSSID; }

  // Scan synchrone des réseaux WiFi (renvoie JSON array)
  String scanNetworksJSON() {
    int n = WiFi.scanNetworks(false, true);    // sync, show hidden
    String out = "[";
    for (int i = 0; i < n; ++i) {
      if (i) out += ",";
      out += "{\"ssid\":\"" + WiFi.SSID(i) +
             "\",\"rssi\":" + String(WiFi.RSSI(i)) +
             ",\"enc\":" + String((int)WiFi.encryptionType(i)) +
             ",\"ch\":" + String(WiFi.channel(i)) + "}";
    }
    out += "]";
    WiFi.scanDelete();
    return out;
  }

  String getStatusJSON() {
    bool sta = isSTAConnected();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"ap\":{\"ssid\":\"%s\",\"ip\":\"%s\",\"clients\":%d},"
             "\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},"
             "\"mdns\":\"%s.local\"}",
             WIFI_AP_SSID, getAPIP().c_str(), WiFi.softAPgetStationNum(),
             sta ? "true" : "false",
             sta ? staSSID.c_str() : "",
             sta ? getSTAIP().c_str() : "",
             MDNS_HOSTNAME);
    return String(buf);
  }
};

#endif // WIFI_MANAGER_H
