# 🎵 MIDI Slide Whistle Controller

Transformez un pipeau à coulisse en instrument MIDI contrôlable par ordinateur !

<img src="https://github.com/glloq/slide_Whistle/blob/main/img/schemas%20principe.png" alt="Schéma de principe" width=70% height=70%/>

## 🎯 Description

Ce projet Arduino permet de contrôler automatiquement un pipeau à coulisse via des messages MIDI USB. Le système positionne précisément le slider pour chaque note et gère le flux d'air de manière synchronisée.

### ✨ Fonctionnalités

- ✅ **Réception MIDI USB** - Contrôle direct depuis votre DAW ou clavier MIDI
- ✅ **Positionnement précis** - Moteur pas à pas avec homing automatique
- ✅ **Contrôle d'air** - Ventilateur/solénoïde avec servomoteur optionnel
- ✅ **Configuration facile** - Tous les paramètres dans `settings.h`
- ✅ **Modulaire** - Code organisé en modules réutilisables
- ✅ **Debug complet** - Moniteur série avec informations détaillées

## 🛠️ Choix techniques

- **Moteur pas à pas** NEMA 17 avec driver A4988/DRV8825
- **Capteur fin de course** normalement fermé (NF) pour le homing
- **Ventilateur radial** 12V ou solénoïde pour l'air
- **Servomoteur** pour moduler le débit (optionnel)
- **Arduino Leonardo/Micro** pour support MIDI USB natif

## 📚 Documentation

### Guides d'utilisation

- **[Guide d'installation](INSTALLATION.md)** - Installation complète, câblage, premier démarrage
- **[Liste des composants](COMPONENTS_LIST.md)** - Tous les composants nécessaires avec prix
- **[Exemples de configuration](CONFIGURATION_EXAMPLES.md)** - Configurations pour différents cas d'usage

### Fichiers du projet

```
slide_Whistle/
├── slide_Whistle.ino      # Fichier principal Arduino
├── settings.h             # Configuration centralisée ⚙️
├── StepperControl.h       # Gestion moteur pas à pas
├── MIDIHandler.h          # Gestion messages MIDI
├── AirControl.h           # Gestion ventilateur et servo
├── INSTALLATION.md        # Guide d'installation complet
├── COMPONENTS_LIST.md     # Liste d'achat
└── CONFIGURATION_EXAMPLES.md  # Exemples de configs
```

## 🚀 Démarrage rapide

### 1. Installer les bibliothèques Arduino

- **AccelStepper** (≥ 1.64)
- **MIDIUSB** (≥ 1.0.5)
- **Servo** (incluse)

### 2. Configurer `settings.h`

```cpp
#define STEPPER_STEP_PIN   2
#define STEPPER_DIR_PIN    3
#define ENDSTOP_PIN        5
#define FAN_PIN            6
#define SERVO_PIN          9

#define TOTAL_TRAVEL_STEPS 10000  // À calibrer selon votre montage
#define MIDI_NOTE_MIN      48     // C3
#define MIDI_NOTE_MAX      84     // C6
```

### 3. Téléverser le code

1. Ouvrir `slide_Whistle.ino` dans Arduino IDE
2. Sélectionner **Arduino Leonardo** (ou Micro)
3. Téléverser

### 4. Premier test

Au démarrage, le système :
1. Effectue un homing automatique
2. Se positionne au centre
3. Attend les messages MIDI

Connectez votre clavier MIDI et jouez !

## 🎹 Utilisation

Le système reçoit des **Note On** MIDI et :
- Positionne le slider selon la hauteur de la note
- Active le ventilateur
- Ajuste le débit selon la vélocité (si servo activé)

Sur **Note Off** :
- Arrête le ventilateur (avec délai configurable)

## 🔧 Calibration

La calibration principale se fait dans `settings.h` :

```cpp
// Ajuster selon votre mécanique
#define TOTAL_TRAVEL_STEPS 24000

// Formule :
// TOTAL_TRAVEL_STEPS = course_mm × (steps × microsteps) / circonférence_poulie
```

Voir [CONFIGURATION_EXAMPLES.md](CONFIGURATION_EXAMPLES.md) pour des exemples détaillés.

## 💡 Améliorations futures

- [ ] Courbes de réponse personnalisables
- [ ] Vibrato via aftertouch
- [ ] Pitch bend pour glissando
- [ ] Control Change pour effets
- [ ] Interface web de configuration

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
