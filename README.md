# 🎵 MIDI Slide Whistle Controller

Transformez un pipeau à coulisse en instrument MIDI contrôlable par ordinateur !

<img src="https://github.com/glloq/slide_Whistle/blob/main/img/schemas%20principe.png" alt="Schéma de principe" width=70% height=70%/>

## 🎯 Description

Ce projet Arduino permet de contrôler automatiquement un pipeau à coulisse via des messages MIDI USB. Le système positionne précisément le slider pour chaque note et gère le flux d'air de manière synchronisée.

### ✨ Fonctionnalités

- ✅ **Réception MIDI USB** - Contrôle direct depuis votre DAW ou clavier MIDI
- ✅ **Positionnement précis** - Moteur pas à pas avec homing automatique (calculs en mm)
- ✅ **Pitch Bend** - Glissando fluide entre les notes
- ✅ **Aftertouch** - Vibrato expressif en temps réel
- ✅ **2 versions disponibles** - Ventilateur continu ou solénoïde ON/OFF
- ✅ **Contrôle du débit** - Servomoteur modulant selon la vélocité MIDI
- ✅ **Configuration facile** - Tous les paramètres dans `settings.h`
- ✅ **Modulaire** - Code organisé en modules réutilisables
- ✅ **Debug complet** - Moniteur série avec informations détaillées

## 🛠️ Choix techniques

- **Moteur pas à pas** NEMA 17 avec driver A4988/DRV8825
- **Capteur fin de course** normalement fermé (NF) pour le homing
- **Contrôle d'air** : 2 versions disponibles
  - **Version Fan** : Ventilateur radial 12V (tourne en continu)
  - **Version Solenoid** : Solénoïde/valve 12V (ouverture ON/OFF)
- **Servomoteur** pour moduler le débit selon la vélocité
- **Arduino Leonardo/Micro** pour support MIDI USB natif

## 📂 Deux versions disponibles

### Version Ventilateur + Servo (`slide_Whistle_Fan_Servo/`)
Le ventilateur tourne en continu pendant les notes. Le servomoteur module le débit d'air selon la vélocité MIDI.
- **Avantages** : Démarrage instantané, contrôle fluide
- **Usage** : Instruments nécessitant un flux d'air constant

### Version Solénoïde + Servo (`slide_Whistle_Solenoid_Servo/`)
Le solénoïde ouvre/ferme l'arrivée d'air de façon binaire. Le servomoteur module le débit.
- **Avantages** : Économie d'énergie, contrôle précis ON/OFF
- **Usage** : Instruments avec source d'air externe (compresseur)

## 📚 Documentation

### Guides d'utilisation

- **[Guide d'installation](INSTALLATION.md)** - Installation complète, câblage, premier démarrage
- **[Liste des composants](COMPONENTS_LIST.md)** - Tous les composants nécessaires avec prix
- **[Exemples de configuration](CONFIGURATION_EXAMPLES.md)** - Configurations pour différents cas d'usage

### Fichiers du projet

```
slide_Whistle/
├── slide_Whistle_Fan_Servo/          # Version Ventilateur
│   ├── slide_Whistle_Fan_Servo.ino   # Programme principal
│   ├── settings.h                    # Configuration
│   ├── StepperControl.h              # Moteur + vibrato + pitch bend
│   ├── MIDIHandler.h                 # MIDI avec aftertouch
│   └── AirControl.h                  # Ventilateur + servo
│
├── slide_Whistle_Solenoid_Servo/     # Version Solénoïde
│   ├── slide_Whistle_Solenoid_Servo.ino
│   ├── settings.h
│   ├── StepperControl.h              # (identique)
│   ├── MIDIHandler.h                 # (identique)
│   └── AirControl.h                  # Solénoïde + servo
│
├── INSTALLATION.md                   # Guide d'installation
├── COMPONENTS_LIST.md                # Liste d'achat
└── CONFIGURATION_EXAMPLES.md         # Exemples de configs
```

## 🚀 Démarrage rapide

### 1. Installer les bibliothèques Arduino

- **AccelStepper** (≥ 1.64)
- **MIDIUSB** (≥ 1.0.5)
- **Servo** (incluse)

### 2. Choisir votre version

- **`slide_Whistle_Fan_Servo/`** si vous utilisez un ventilateur
- **`slide_Whistle_Solenoid_Servo/`** si vous utilisez un solénoïde

### 3. Configurer `settings.h`

```cpp
// Pins
#define STEPPER_STEP_PIN   2
#define STEPPER_DIR_PIN    3
#define ENDSTOP_PIN        5

// Mécanique (en millimètres !)
#define STEPS_PER_MM       40.0   // À calibrer
#define SLIDER_TRAVEL_MM   300.0  // Course totale
#define INVERT_MOTOR_DIR   false  // Inverser direction si nécessaire

// MIDI
#define MIDI_NOTE_MIN      48     // C3
#define MIDI_NOTE_MAX      84     // C6
#define PITCHBEND_ENABLED  true   // Pitch bend
#define AFTERTOUCH_ENABLED true   // Vibrato
```

### 4. Téléverser le code

1. Ouvrir `slide_Whistle_Fan_Servo.ino` (ou `slide_Whistle_Solenoid_Servo.ino`)
2. Sélectionner **Arduino Leonardo** (ou Micro)
3. Téléverser

### 5. Premier test

Au démarrage, le système :
1. Effectue un homing automatique
2. Se positionne au centre
3. Attend les messages MIDI

Connectez votre clavier MIDI et jouez !

## 🎹 Utilisation

Le système reçoit des messages MIDI et réagit :

### Messages MIDI supportés

| Message | Action |
|---------|--------|
| **Note On** | Positionne le slider + ouvre l'air + ajuste débit (vélocité) |
| **Note Off** | Ferme l'air (avec délai) + arrête vibrato |
| **Pitch Bend** | Glissando fluide (±2 demi-tons par défaut) |
| **Aftertouch** | Vibrato (profondeur et vitesse configurables) |
| **CC 1 (Modulation)** | Vibrato alternatif (si aftertouch non disponible) |

### Expressions musicales

- **Glissando** : Utilisez la molette de pitch bend
- **Vibrato** : Appuyez sur les touches après le Note On (aftertouch)
- **Variations de débit** : Jouez avec différentes vélocités (pianissimo à fortissimo)

## 🔧 Calibration

La calibration se fait en **millimètres** dans `settings.h` :

```cpp
// 1. Calculer les pas par mm
// Pour une poulie GT2 20 dents : (200 × 16) / (20 × 2) = 80 pas/mm
#define STEPS_PER_MM      80.0

// 2. Mesurer la course totale du slider
#define SLIDER_TRAVEL_MM  300.0  // En millimètres

// 3. Inverser la direction si le moteur va dans le mauvais sens
#define INVERT_MOTOR_DIR  false  // true pour inverser

// 4. Ajuster vibrato et pitch bend
#define VIBRATO_DEPTH_MM  2.0    // Profondeur du vibrato (mm)
#define PITCHBEND_RANGE_SEMITONES 2.0  // Plage pitch bend (±2 demi-tons)
```

Voir [CONFIGURATION_EXAMPLES.md](CONFIGURATION_EXAMPLES.md) pour des exemples détaillés.

## 🎼 Nouvelles fonctionnalités v2.0

- ✅ **Pitch Bend** - Glissando fluide et expressif
- ✅ **Vibrato via Aftertouch** - Expression naturelle en appuyant sur les touches
- ✅ **Calculs en mm** - Configuration intuitive avec `STEPS_PER_MM`
- ✅ **Inversion moteur** - Variable `INVERT_MOTOR_DIR` pour faciliter le câblage
- ✅ **Deux versions** - Fan et Solenoid pour différents usages

## 💡 Améliorations futures

- [ ] Courbes de réponse personnalisables (exponentielle, logarithmique)
- [ ] Polyphonie (plusieurs pipeaux)
- [ ] Enregistrement/replay de séquences
- [ ] Interface web de configuration
- [ ] Support d'autres protocoles MIDI (DIN, Bluetooth)

## 🤝 Contribution

Les contributions sont les bienvenues ! N'hésitez pas à :
- Signaler des bugs
- Proposer des améliorations
- Partager vos configurations
- Ajouter des schémas 3D

## 📄 Licence

Ce projet est open-source sous licence MIT.

## 🙏 Remerciements

- **AccelStepper** par Mike McCauley
- **MIDIUSB** par Arduino Team
- La communauté maker et Arduino

---

**Bon contrôle MIDI ! 🎵**
