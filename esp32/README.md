# 🎼 ESP32 MIDI Slide Whistle — v3 (multi-flûtes + pression)

Version ESP32 du contrôleur, refacturée pour piloter **plusieurs flûtes** depuis
un seul micro-contrôleur, avec un module de **gestion pompe + réservoir +
capteur de pression** intégré, et une interface web complète.

> ⚠️ Cette version remplace l'API v2 (single-flûte). Voir [Migration](#migration-depuis-v2) ci-dessous.

> 🚧 **Refonte en cours — contrôleur universel.** Un cœur de contrôle modulaire,
> portable et testé unitairement (moteur pas-à-pas / 1 servo / 2 servos / air
> composable / séquenceur monophonique / validateur de GPIO) est en cours
> d'intégration sous [`esp32_slide_whistle/core/`](esp32_slide_whistle/core/).
> Voir **[ARCHITECTURE.md](ARCHITECTURE.md)** et l'état réel, fonction par
> fonction, dans **[HARDWARE_MATRIX.md](HARDWARE_MATRIX.md)**. Tests natifs :
> `make -C tests`.

---

## 🏗 Architecture

```
                ┌──────────────────────────┐
                │      MIDIHandler         │  Serial UART2 / BLE MIDI
                │  (broadcast canal-aware) │
                └──────────┬───────────────┘
                           │ (channel, msg)
                           ▼
                ┌──────────────────────────┐
                │        Orchestra         │  routage par canal
                └──┬─────────┬────────┬────┘
                   │         │        │
              ┌────▼──┐ ┌────▼──┐ ┌──▼────┐
              │Flute 0│ │Flute 1│ │Flute N│   chacune = StepperControl + AirControl
              └───────┘ └───────┘ └───────┘
                   │         │        │
                   ▼         ▼        ▼
              solénoïde ─── solénoïde ─── solénoïde
                   ▲         ▲        ▲
                   └───┬─────┴────────┘
                       │
                ┌──────┴───────────────┐
                │  PressureControl     │  pompe + capteur + FSM
                │   (pump → réservoir) │
                └──────────────────────┘
```

### Modules

| Fichier             | Rôle                                                                  |
|---------------------|------------------------------------------------------------------------|
| `settings.h`        | Configs matérielles par flûte (`DEFAULT_FLUTE_CONFIGS[]`) + pression   |
| `StepperControl.h`  | Pilotage moteur pas-à-pas, instanciable, LUT par instance              |
| `AirControl.h`      | Solénoïde PWM + servo, instanciable, courbes de vélocité               |
| `Flute.h`           | Encapsulation d'un instrument (stepper + air + état + watchdog + sustain) |
| `Orchestra.h`       | Tableau de flûtes, dispatch MIDI par canal                              |
| `PressureControl.h` | Pompe + capteur pression + FSM (FILLING/MAINTAIN/SAFETY)                |
| `MIDIHandler.h`     | Multi-source (Serial + BLE), callbacks `(channel, ...)` + ring buffer log |
| `DemoPlayer.h`      | Mélodies de démo non-bloquantes (start/stop/loop)                       |
| `StressTester.h`    | Burn-in : notes aléatoires sur les flûtes activées                      |
| `WiFiManager.h`     | AP + STA + mDNS + scan, credentials NVS, validation longueur            |
| `NVSKeys.h`         | Clés NVS centralisées (un seul endroit pour tout renommer)              |
| `NVSStore.h`        | save/load Flute + PressureControl (extrait de WebInterface)             |
| `WebInterface.h`    | API REST + WebSocket (orchestration des routes)                         |
| `data/index.html`   | Squelette HTML (686 lignes), modaux et pages                            |
| `data/app.css`      | Styles (750 lignes) — variables thème + composants + responsive         |
| `data/app.js`       | Logique frontend (2426 lignes) — WS, rendu, piano, presets, etc.        |
| `data/sw.js`        | Service worker PWA (cache shell, bypass /api/* et /ws)                  |
| `data/manifest.webmanifest` | Manifeste PWA (icônes SVG inline)                              |

---

## 🎺 Multi-flûtes

### Principe

Chaque flûte est définie par un `FluteHwConfig` (pins, canal MIDI, plage de
notes, pulley, canaux LEDC, etc.). On peut activer 1 à `MAX_FLUTES` (4 par
défaut) flûtes sur le même ESP32.

```cpp
// settings.h
#define MAX_FLUTES          4    // borne haute compile-time
#define DEFAULT_FLUTE_COUNT 1    // nombre de flûtes activées au boot

static const FluteHwConfig DEFAULT_FLUTE_CONFIGS[MAX_FLUTES] = {
  { "Flute 0", 1, /*…pins, plage MIDI…*/ },
  { "Flute 1", 2, /*…*/ },
  { "Flute 2", 3, /*…*/ },
  { "Flute 3", 4, /*…*/ }
};
```

### Routage MIDI

Chaque flûte a un canal MIDI :
- canal `0` (OMNI) → reçoit tous les canaux
- canal `1..16` → ne reçoit que ce canal

Une flûte peut aussi limiter sa **plage de notes** (`note_min`, `note_max`) pour
faire un split clavier : flûte grave 48-66, flûte aiguë 67-84, etc.

Tout est modifiable au runtime via l'API web (`POST /api/flute`) et persisté en
NVS.

### Limites matérielles

| Ressource           | Disponible ESP32   | Coût par flûte | Max         |
|---------------------|--------------------|----------------|-------------|
| GPIO output         | ~25                | 6 (step/dir/en/end/sol/servo) | ~4 |
| Canaux LEDC         | 16 (8 HS + 8 LS)   | 1 solénoïde    | 4 (LS 8,10,12,14) + pompe |
| Timers ESP32Servo   | 4                  | 1              | 4           |
| ADC                 | 18 canaux          | -              | 1 capteur pression |

→ Au-delà de **4 flûtes**, prévoir un PCA9685 (I²C) pour multiplier les sorties PWM.

---

## 💨 Module pression — pompe + réservoir

### Pourquoi ?

Sur la version solénoïde v2, l'air provient d'un compresseur externe avec
pression supposée constante. La v3 intègre la régulation : **pompe + réservoir
tampon + capteur de pression**, ce qui permet :

- Fonctionnement autonome (sans compresseur externe)
- Régulation par hystérésis ou seuils mesurés
- Coupure d'urgence (surpression / réservoir vide)
- Diagnostics via l'interface web

### FSM

```
   ┌─────┐        start()        ┌─────────┐
   │IDLE ├──────────────────────▶│FILLING  │
   └──┬──┘                       └────┬────┘
      ▲                               │ p ≥ target
      │ stop()                        ▼
      │                          ┌──────────┐
      │             ┌────────────┤MAINTAIN  │
      │             │   p < min  └────┬─────┘
      │             ▼                 │ p > safety
      │        FILLING ←─────┐        │ ou réservoir vide
      │                      │        ▼
   ┌──┴──┐                  ┌──────────┐
   │RESET│◀─── reset() ─────┤ SAFETY   │
   └─────┘                  └──────────┘
```

### Modes

- **Avec capteur** (`pressureSensorPin >= 0`) : régulation par seuils kPa.
- **Sans capteur** : alternance temporelle ON/OFF programmable.

### Sécurités

- Timeout de remplissage (`maxFillTimeMs`)
- Surpression (`pressureSafety`)
- Réservoir vide (capteur flotteur optionnel)
- Anti-cyclage pompe (`pumpMinOffMs`)

---

## 🌐 API Web

### Status global

```
GET  /api/status
GET  /api/flutes                   — liste résumée (status court par flûte)
GET  /api/flute?id=N               — config détaillée flûte N
```

### Flûte (id dans le body JSON)

```
POST /api/flute            {id, midi_channel, note_min, note_max,
                            speed_mm_s, accel_mm_s2,
                            pwm_full, pwm_hold, wait_delay_ms, legato_ms,
                            enabled, muted, use_lut}
POST /api/flute/lut        {id, lut: [{note, position}…]}
POST /api/flute/lut_point  {id, note, position_mm}
POST /api/flute/note       {id, note, velocity, on}
POST /api/flute/jog        {id, mm}
POST /api/flute/test       {id}                ─ séquence de test air
POST /api/flute/panic      {id}                ─ coupure immédiate
```

### Globaux

```
POST /api/homing                 — re-homing toutes flûtes
POST /api/flute/homing           — body {id} — homing d'une flûte précise
POST /api/panic                  — panic toutes flûtes
GET  /api/midi/log               — ring buffer des 32 derniers messages MIDI
```

### Presets (snapshot complet)

```
GET    /api/presets              — liste des noms enregistrés
POST   /api/presets/save         {name}   — sauvegarde la config courante
POST   /api/presets/load         {name}   — restaure la config
POST   /api/presets/delete       {name}
```

### Sauvegarde / restauration JSON (téléchargement)

```
GET  /api/backup                  — JSON complet (Content-Disposition: attachment)
POST /api/restore                 — body = JSON déjà téléchargé, applique tout
```

### Système / diagnostic / OTA

```
GET  /api/system                  — chip, heap, MAC, état endstop par flûte,
                                    raw ADC capteur pression, etc.
POST /api/system/reboot           — redémarrage logiciel
POST /api/system/update           — multipart/form-data avec un .bin :
                                    flashe la partition OTA inactive et reboot
GET  /api/midi/log.csv            — export CSV téléchargeable du ring buffer
```

### Démo (mélodies pré-enregistrées)

```
GET  /api/demo                    — liste des mélodies + état lecteur
POST /api/demo/play               {id, loop}
POST /api/demo/stop
```

### Solo flûte

```
POST /api/flute/solo              {id}    — mute toutes sauf {id} (id<0 libère)
```

### Sweep mécanique + commandes globales

```
POST /api/flute/sweep             {id}    — déplace 0 → fond → 0 (test mécanique)
POST /api/flutes/all              {action} — muteAll | unmuteAll | enableAll
                                            | disableAll | panic
```

### Personnalisation par flûte

Le POST `/api/flute` accepte aussi :

- `custom_name` (string, vide = nom par défaut)
- `cc_breath`, `cc_expression`, `cc_volume`, `cc_vibrato`, `cc_sustain`
  — numéros CC (0 = désactivé) routés vers les fonctions correspondantes
- `transpose` (int -36..+36) — décale les notes entrantes de N demi-tons,
  notes hors plage [note_min, note_max] sont ignorées

### Pression

```
GET  /api/pressure
POST /api/pressure/config        {target, min, max, safety, max_fill_ms}
POST /api/pressure/start
POST /api/pressure/stop
POST /api/pressure/reset
```

### WiFi

```
GET    /api/wifi
POST   /api/wifi                 {ssid, password}
DELETE /api/wifi                 — oublier credentials
```

### WebSocket

`ws://<ip>/ws` diffuse l'état complet (toutes flûtes + pression) toutes les
200 ms, et envoie un push immédiat à la connexion.

### Statistiques MIDI

Le JSON `/api/status` inclut `midi_count_by_channel` : compteurs par canal
1-16 pour visualiser quel canal reçoit du trafic (utile pour le debug et
le routage multi-flûte).

### UX navigateur

- **Preview audio** (Web Audio synth) — bouton 🔊 dans l'en-tête : permet
  de jouer dans le navigateur ce qui est commandé, pour tester sans matériel.
- **Piano multi-touch** + **clavier ordinateur** (sur la page Jouer) :
  A,W,S,E,D,F,T,G,Y,H,U,J,K... → C4-C5 chromatique
- **Couleur par flûte** appliquée aux bordures, slider et badge canal
- **Animation pulse** quand une flûte est active
- **VU bar PWM** sur chaque carte flûte
- **Modal confirm** + **overlay loading** pour les opérations longues
- **Bandeau MIDI 16 canaux** sur le dashboard avec compteurs live

---

## 🧰 Pins par défaut

| Flûte    | Step | Dir | En  | Endstop | Solénoïde (LEDC) | Servo (timer) |
|----------|------|-----|-----|---------|-------------------|----------------|
| Flûte 0  | 26   | 27  | 14  | 34      | 25 (ch 8)         | 32 (t 0)       |
| Flûte 1  | 13   | 12  | 15  | 35      | 19 (ch 10)        | 18 (t 1)       |
| Flûte 2  | 5    | 17  | 16  | 39      | 4 (ch 12)         | 2 (t 2)        |
| Flûte 3  | 33   | 21  | 22  | 36      | 23 (ch 14)        | 3 (t 3)        |
| Pompe    | 22   | -   | -   | -       | (LEDC ch 15)      | -              |
| Capteur  | 36   | -   | -   | -       | -                 | -              |

> Endstops sur GPIO 34/35/36/39 = input-only → pull-up externe 10 kΩ obligatoire.
>
> ⚠️ Le pin pompe (22) collide avec la flûte 3 — à remapper si on active 4 flûtes.

---

## 🔧 Compilation

Bibliothèques requises (Arduino IDE) :

- AccelStepper
- ESP32Servo
- ESPAsyncWebServer + AsyncTCP
- ArduinoJson
- MIDI Library (FortySevenEffects)
- Arduino-BLE-MIDI (lathoub)

Cible : **ESP32 (Wrover/DevKitC v4 ou compatible)**, partition incluant LittleFS.

```
Outils → Carte : ESP32 Dev Module
Outils → Partition Scheme : Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)
Outils → Téléverser le contenu LittleFS (data/index.html)
```

---

## ↪️ Migration depuis v2

| v2                                     | v3                                            |
|----------------------------------------|-----------------------------------------------|
| `stepper`, `air`, `midi` globaux       | `orchestra.get(id)` puis `f->stepper()` etc.  |
| `STEPPER_STEP_PIN` `#define`           | `DEFAULT_FLUTE_CONFIGS[0].stepperStepPin`     |
| `runtimeCfg` partagé                   | NVS namespace `flute0`, `flute1`...           |
| Endpoints `/api/config`, `/api/lut`    | `/api/flute`, `/api/flute/lut`                |
| `MIDI_CHANNEL` global                  | par flûte (`midi_channel`)                    |
| Pas de pompe                           | `PressureControl` complet (FSM + capteur)     |

Pour rester en single-flûte (équivalent v2) : `DEFAULT_FLUTE_COUNT = 1`.

---

## 🚀 Démarrage rapide

1. Cloner et ouvrir `esp32/esp32_slide_whistle/esp32_slide_whistle.ino` dans Arduino IDE
2. Ajuster `DEFAULT_FLUTE_COUNT` et les pins dans `settings.h`
3. Téléverser le sketch
4. Téléverser LittleFS (Outils → ESP32 Sketch Data Upload) avec le dossier `data/`
5. Se connecter au WiFi `SlideWhistle` (mot de passe `midi1234`)
6. Ouvrir `http://192.168.4.1` ou `http://slidewhistle.local` (mDNS) → l'interface web pilote tout
