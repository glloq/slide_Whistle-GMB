# ESP32 MIDI Slide Whistle Controller

Version ESP32 complète, combinant le meilleur des deux versions Arduino
(Fan+Servo et Solénoïde+Servo) avec une interface web intégrée.

## Fonctionnalités

| Fonction | Détail |
|---|---|
| Moteur | AccelStepper, homing 3 phases, LUT calibrée |
| Air | Machine à états 5 états, PWM solénoïde, legato |
| MIDI | DIN-5 (Serial2) + BLE MIDI simultanés |
| Web UI | Hotspot WiFi + réseau externe, WebSocket temps-réel |
| Config | Persistance NVS (Preferences), pas de recompilation |
| Calibration | Interface web assistée, table LUT note par note |

## Structure

```
esp32/
└── esp32_slide_whistle/
    ├── esp32_slide_whistle.ino   ← programme principal (FreeRTOS)
    ├── settings.h                ← tous les paramètres (pins, mécanique, MIDI)
    ├── StepperControl.h          ← moteur AccelStepper (adapté ESP32)
    ├── AirControl.h              ← solénoïde PWM + servo (machine à états)
    ├── MIDIHandler.h             ← MIDI multi-source (Serial + BLE)
    ├── WiFiManager.h             ← hotspot AP + client STA + NVS
    ├── WebInterface.h            ← AsyncWebServer + WebSocket + API REST
    └── data/
        └── index.html            ← interface web (LittleFS)
```

## Bibliothèques requises

Installer via Arduino Library Manager :

| Bibliothèque | Auteur | Utilisation |
|---|---|---|
| AccelStepper | Mike McCauley | Moteur pas à pas |
| ESP32Servo | Kevin Harrington | Servomoteur |
| ESPAsyncWebServer | me-no-dev | Serveur web async |
| AsyncTCP | me-no-dev | Requis par ESPAsyncWebServer |
| ArduinoJson | bblanchon | JSON API |
| MIDI Library | FortySevenEffects | MIDI DIN-5 série |
| BLE-MIDI | lathoub | BLE MIDI |

## Pins ESP32 (modifiables dans settings.h)

| Signal | GPIO | Notes |
|---|---|---|
| STEP driver | 26 | |
| DIR driver | 27 | |
| ENABLE driver | 14 | LOW = actif |
| Endstop | 34 | Input-only ! Pull-up 10kΩ externe obligatoire |
| Solénoïde | 25 | PWM LEDC |
| Servo | 32 | ESP32Servo |
| LED status | 2 | LED intégrée |
| MIDI RX (DIN) | 16 | Serial2 RX |

## Upload de l'interface web

L'interface web est stockée sur LittleFS (flash ESP32).

**Arduino IDE :**
1. Installer le plugin "ESP32 LittleFS Data Upload"  
2. `Outils → ESP32 LittleFS Data Upload`

**PlatformIO :**
```ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
board_build.filesystem = littlefs
```
puis : `pio run --target uploadfs`

## Accès interface web

Par défaut, l'ESP32 crée un hotspot WiFi :
- **SSID** : `SlideWhistle`
- **Mot de passe** : `midi1234`
- **URL** : http://192.168.4.1

Depuis l'onglet WiFi, configurer un réseau externe pour accès depuis votre réseau local.

## Architecture FreeRTOS

```
Core 0 (priorité basse)    Core 1 (priorité haute)
─────────────────────────  ──────────────────────────
WiFi (AP + STA)            MIDI parsing (Serial + BLE)
Web server (async)         Stepper control (AccelStepper)
WebSocket push (200ms)     Air control (machine à états)
Config NVS load/save       Homing séquentiel
```

## Différences vs versions Arduino

| Aspect | Arduino | ESP32 |
|---|---|---|
| MIDI | MIDIUSB (USB) | Serial2 (DIN-5) + BLE |
| Servo | Servo standard | ESP32Servo |
| PWM | analogWrite Arduino | ledcWrite (LEDC) |
| LUT | PROGMEM | RAM normale |
| Config | Recompiler | NVS (Preferences) |
| Interface | Série (calibration) | Web (temps-réel) |
| Multi-tâches | Loop unique | FreeRTOS dual-core |

## Calibration

1. Connecter au hotspot `SlideWhistle`
2. Ouvrir http://192.168.4.1
3. Onglet **Calibration**
4. Sélectionner note la plus aiguë (C6)
5. Ajuster position avec les boutons jog
6. Tester la note (bouton "Tester note")
7. Sauvegarder → répéter pour chaque note
8. Activer "Utiliser LUT calibrée" dans Configuration

## Paramètres MIDI

| Message | Action |
|---|---|
| Note On | Position coulisse + ouverture air |
| Note Off | Fermeture air |
| Pitch Bend | Micro-ajustement position |
| Aftertouch | Vibrato (profondeur + vitesse configurables) |
| CC1 | Vibrato (alternative modulation wheel) |
| CC2 / CC11 | Débit air (expression) |
