# 🎵 MIDI Slide Whistle Controller

Transformez un pipeau à coulisse en instrument MIDI contrôlable par ordinateur.


---

## 🎯 Deux versions disponibles

### 🌀 [Version VENTILATEUR + SERVO](slide_Whistle_Fan_Servo/)

Le ventilateur tourne en continu, le servomoteur dirige le flux d'air.
<img src="https://github.com/glloq/slide_Whistle/blob/main/img/schemas%20principe.png" alt="Schéma de principe" width=70% height=70%/>

**Principe** :
- Ventilateur : Toujours allumé
- Servo : 2 positions (vers le bec / ailleurs)

**Avantages** :
- ✅ Simple et fiable
- ✅ Réactif instantané
- ✅ Autonome (pas de compresseur)

👉 **[Documentation complète →](slide_Whistle_Fan_Servo/README.md)**

---

### ⚡ [Version SOLÉNOÏDE + SERVO](slide_Whistle_Solenoid_Servo/)

Solénoïde avec PWM intelligent et machine à états avancée.
<img src="https://github.com/glloq/slide_Whistle/blob/main/img/solenoid.png" alt="Schéma de principe" width=70% height=70%/>

**Principe** :
- Solénoïde : PWM 100% ouverture, 47% maintien
- Buffer 200ms avant ouverture
- Legato intelligent (notes rapides)

**Avantages** :
- ✅ Contrôle du volume (vélocité)
- ✅ Legato fluide
- ✅ Économie d'énergie
- ✅ Son précis

👉 **[Documentation complète →](slide_Whistle_Solenoid_Servo/README.md)**

---

## 🎼 Fonctionnalités communes

| Fonctionnalité | Description |
|----------------|-------------|
| **Positionnement précis** | Moteur pas à pas avec calculs en mm |
| **Pitch Bend** | Glissando fluide (±2 demi-tons) |
| **Aftertouch** | Vibrato expressif en temps réel |
| **Homing automatique** | Initialisation au démarrage |
| **MIDI USB** | Arduino Leonardo/Micro |

---

## 📊 Comparaison rapide

| Critère | 🌀 Ventilateur | ⚡ Solénoïde |
|---------|----------------|--------------|
| **Complexité** | Simple | Avancée |
| **Source air** | Ventilateur intégré | Compresseur externe |
| **Contrôle volume** | ❌ Non | ✅ Oui (vélocité) |
| **Legato** | N/A | ✅ Intelligent |
| **Latence** | Instantané | Buffer 200ms |
| **Budget** | ~100€ | ~130€ |

---

## 🚀 Démarrage rapide

### 1. Choisir votre version

- Ventilateur intégré → **Version Fan**
- Compresseur externe → **Version Solenoid**

### 2. Installer les bibliothèques

```
Arduino IDE → Gestionnaire de bibliothèques
```

- **AccelStepper** (≥ 1.64)
- **MIDIUSB** (≥ 1.0.5)
- **Servo** (incluse)

### 3. Configurer `settings.h`

```cpp
// Pins
#define STEPPER_STEP_PIN   2
#define STEPPER_DIR_PIN    3
#define ENDSTOP_PIN        5

// Mécanique
#define STEPS_PER_MM       80.0   // À calibrer
#define SLIDER_TRAVEL_MM   300.0  // Course en mm
#define INVERT_MOTOR_DIR   false  // Inverser si besoin

// MIDI
#define MIDI_NOTE_MIN      48     // C3
#define MIDI_NOTE_MAX      84     // C6
```

### 4. Téléverser et tester

```
Arduino IDE → Téléverser
Moniteur série → 115200 bauds
```

---

## 📚 Documentation complète

- **[Installation](INSTALLATION.md)** - Câblage, premier démarrage, calibration
- **[Composants](COMPONENTS_LIST.md)** - Liste d'achat complète avec prix
- **[Configuration](CONFIGURATION_EXAMPLES.md)** - Exemples pour différents usages

---

## 🛠️ Hardware minimal

| Composant | Spécification |
|-----------|---------------|
| Arduino | Leonardo/Micro (MIDIUSB) |
| Driver moteur | A4988 ou DRV8825 |
| Moteur | NEMA 17, 200 pas/tour |
| Capteur | Fin de course NF |
| Alimentation | 12V 2-3A |

**+ Version Fan** : Ventilateur 12V, Servo
**+ Version Solenoid** : Solénoïde 12V, Servo, Compresseur

---

## 🎹 Messages MIDI supportés

| Message | Action |
|---------|--------|
| **Note On/Off** | Position + Air |
| **Pitch Bend** | Glissando (±2 demi-tons) |
| **Aftertouch** | Vibrato (profondeur/vitesse configurables) |
| **CC 1 (Modulation)** | Vibrato alternatif |

---

## 📂 Structure du projet

```
slide_Whistle/
│
├── slide_Whistle_Fan_Servo/          🌀 Version Ventilateur
│   ├── README.md                      ← Documentation détaillée
│   ├── settings.h
│   ├── StepperControl.h
│   ├── MIDIHandler.h
│   ├── AirControl.h
│   └── slide_Whistle_Fan_Servo.ino
│
├── slide_Whistle_Solenoid_Servo/     ⚡ Version Solénoïde
│   ├── README.md                      ← Documentation détaillée
│   ├── settings.h
│   ├── StepperControl.h
│   ├── MIDIHandler.h
│   ├── AirControl.h
│   └── slide_Whistle_Solenoid_Servo.ino
│
├── INSTALLATION.md                    📖 Guide installation
├── COMPONENTS_LIST.md                 🛒 Liste d'achat
├── CONFIGURATION_EXAMPLES.md          ⚙️ Exemples configs
└── README.md                          📄 Ce fichier
```

---

## 💡 Pour commencer

1. **Lire** le README de la version choisie
2. **Acheter** les composants (voir COMPONENTS_LIST.md)
3. **Câbler** selon le schéma (voir README de la version)
4. **Configurer** settings.h
5. **Téléverser** le code
6. **Jouer** ! 🎵

---

## 🤝 Contribution

Les contributions sont bienvenues :
- Signaler des bugs
- Proposer des améliorations
- Partager vos configurations
- Ajouter des schémas 3D

---

## 📄 Licence

MIT License - Open source

---

## 🙏 Remerciements

- **AccelStepper** par Mike McCauley
- **MIDIUSB** par Arduino Team
- La communauté maker Arduino

---

**🎵 Bon contrôle MIDI !**
