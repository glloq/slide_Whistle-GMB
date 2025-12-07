# MIDI Slide Whistle - Guide d'Installation et d'Utilisation

## 📋 Table des matières

- [Vue d'ensemble](#vue-densemble)
- [Matériel requis](#matériel-requis)
- [Bibliothèques Arduino](#bibliothèques-arduino)
- [Câblage](#câblage)
- [Configuration](#configuration)
- [Installation du code](#installation-du-code)
- [Premier démarrage](#premier-démarrage)
- [Utilisation](#utilisation)
- [Dépannage](#dépannage)

---

## 🎵 Vue d'ensemble

Ce projet transforme un pipeau à coulisse en instrument MIDI contrôlé par ordinateur. Le système :

- **Reçoit** des notes MIDI via USB
- **Positionne** le slider avec un moteur pas à pas
- **Contrôle** le flux d'air avec un ventilateur/solénoïde
- **Ajuste** (optionnel) le débit avec un servomoteur

---

## 🛠️ Matériel requis

### Électronique

| Composant | Spécification | Exemple |
|-----------|---------------|---------|
| **Microcontrôleur** | Arduino compatible MIDIUSB | Leonardo, Micro, Due, Zero |
| **Driver moteur** | A4988, DRV8825, TB6600 | A4988 (recommandé) |
| **Moteur pas à pas** | NEMA 17, 200 pas/tour | NEMA 17 1.8° |
| **Capteur fin de course** | Normalement fermé (NF) | Micro-switch mécanique |
| **Ventilateur** | 12V radial ou solénoïde | Ventilateur radial 12V |
| **Servomoteur** | Standard 0-180° | SG90, MG996R |
| **Alimentation** | 12V 2A minimum | Adaptateur 12V 3A |
| **Régulateur** | 5V pour Arduino | Buck converter ou 7805 |

### Mécanique

- Rail linéaire ou tige filetée pour le slider
- Courroie ou vis sans fin pour transmission
- Support pour le pipeau
- Châssis (impression 3D ou profilés alu)

---

## 📚 Bibliothèques Arduino

### Installation via le Gestionnaire de bibliothèques Arduino

1. Ouvrir l'IDE Arduino
2. Aller dans **Outils → Gérer les bibliothèques**
3. Installer les bibliothèques suivantes :

| Bibliothèque | Version | Auteur |
|--------------|---------|--------|
| **AccelStepper** | ≥ 1.64 | Mike McCauley |
| **MIDIUSB** | ≥ 1.0.5 | Arduino |
| **Servo** | (incluse) | Arduino |

### Installation manuelle (si nécessaire)

```bash
cd ~/Documents/Arduino/libraries/
git clone https://github.com/waspinator/AccelStepper.git
git clone https://github.com/arduino-libraries/MIDIUSB.git
```

---

## 🔌 Câblage

### Schéma de connexion

```
ARDUINO LEONARDO/MICRO
┌─────────────────────┐
│                     │
│  D2 ────────────────┼──> STEP (Driver moteur)
│  D3 ────────────────┼──> DIR  (Driver moteur)
│  D4 ────────────────┼──> EN   (Driver moteur)
│                     │
│  D5 ────────────────┼──< Capteur fin de course (+ GND)
│                     │
│  D6 ────────────────┼──> Ventilateur (via MOSFET/relais)
│  D9 ────────────────┼──> Servomoteur (signal PWM)
│                     │
│  D13 (LED) ─────────┼──  LED de statut intégrée
│                     │
│  5V  ───────────────┼──> Alimentation servomoteur
│  GND ───────────────┼──> Masse commune
│                     │
└─────────────────────┘
```

### Driver moteur (A4988/DRV8825)

```
DRIVER MOTEUR
┌─────────────────┐
│      A4988      │
│                 │
│ STEP  ←─────────┼── D2 (Arduino)
│ DIR   ←─────────┼── D3 (Arduino)
│ EN    ←─────────┼── D4 (Arduino)
│                 │
│ 1A ─────────────┼──> Moteur (phase A)
│ 1B ─────────────┼──> Moteur (phase A)
│ 2A ─────────────┼──> Moteur (phase B)
│ 2B ─────────────┼──> Moteur (phase B)
│                 │
│ VMOT ←──────────┼── +12V
│ GND  ←──────────┼── GND
│                 │
└─────────────────┘

CONFIGURATION MICROSTEPPING (cavaliers MS1, MS2, MS3) :
MS1  MS2  MS3  | Microstepping
─────────────────────────────
OFF  OFF  OFF  | Pas entier
ON   OFF  OFF  | 1/2 pas
OFF  ON   OFF  | 1/4 pas
ON   ON   OFF  | 1/8 pas
ON   ON   ON   | 1/16 pas (recommandé)
```

### Contrôle du ventilateur (via MOSFET)

```
VENTILATEUR 12V
                         +12V
                          │
                          ├──> Ventilateur
                          │
D6 (Arduino) ──┬──[1kΩ]──┤ Gate
               │         MOSFET
              GND        (IRLZ44N)
                          │
                         GND
```

### Servomoteur

```
SERVOMOTEUR
┌─────────────┐
│    SG90     │
│             │
│ Signal ─────┼── D9 (Arduino)
│ VCC    ─────┼── +5V
│ GND    ─────┼── GND
│             │
└─────────────┘
```

### Capteur fin de course

```
CAPTEUR FIN DE COURSE (NF)
┌───────┐
│  NC   ├──── D5 (Arduino) + PULLUP interne
│       │
│  COM  ├──── GND
└───────┘
```

---

## ⚙️ Configuration

### Fichier `settings.h`

Tous les paramètres sont dans `settings.h`. Voici les principaux à ajuster :

#### 1. Pins Arduino

```cpp
#define STEPPER_STEP_PIN   2    // Votre pin STEP
#define STEPPER_DIR_PIN    3    // Votre pin DIR
#define ENDSTOP_PIN        5    // Votre capteur fin de course
#define FAN_PIN            6    // Votre ventilateur
#define SERVO_PIN          9    // Votre servomoteur
```

#### 2. Configuration moteur

```cpp
#define STEPS_PER_REVOLUTION  200  // Moteur standard 1.8°
#define MICROSTEPS            16   // Configuration de votre driver
#define STEPPER_SPEED         1000 // Vitesse max (ajuster selon besoin)
```

#### 3. Course du slider

**Important** : À calibrer selon votre mécanique !

```cpp
#define TOTAL_TRAVEL_STEPS   10000  // Course totale en pas
```

**Comment déterminer cette valeur :**

1. Compiler et uploader le code
2. Ouvrir le moniteur série (115200 bauds)
3. Déplacer manuellement le slider de bout en bout
4. Noter la distance parcourue
5. Calculer : `TOTAL_TRAVEL_STEPS = distance_mm × pas_par_mm`

#### 4. Plage de notes MIDI

```cpp
#define MIDI_NOTE_MIN   48  // C3 (Do3)
#define MIDI_NOTE_MAX   84  // C6 (Do6)
```

**Table des notes MIDI :**

| Note | MIDI | Fréquence |
|------|------|-----------|
| C3   | 48   | 130.81 Hz |
| C4   | 60   | 261.63 Hz |
| C5   | 72   | 523.25 Hz |
| C6   | 84   | 1046.50 Hz|

#### 5. Servomoteur (débit d'air)

```cpp
#define SERVO_MIN_ANGLE   30   // Débit minimum
#define SERVO_MAX_ANGLE   150  // Débit maximum
```

---

## 💾 Installation du code

### 1. Télécharger le projet

```bash
git clone https://github.com/glloq/slide_Whistle.git
cd slide_Whistle
```

### 2. Ouvrir dans Arduino IDE

- Ouvrir `slide_Whistle.ino`
- Sélectionner votre carte : **Outils → Type de carte → Arduino Leonardo** (ou Micro)
- Sélectionner le port COM

### 3. Configurer `settings.h`

Adapter les paramètres à votre montage (voir section précédente)

### 4. Compiler et téléverser

- Cliquer sur **Téléverser** (ou Ctrl+U)
- Attendre la fin du téléversement

---

## 🚀 Premier démarrage

### Séquence de démarrage

1. **LED allumée** : Initialisation
2. **LED clignotante** : Homing en cours
3. **Bip court** : Système prêt
4. **LED éteinte** : En attente de MIDI

### Moniteur série (Debug)

Ouvrir le moniteur série (115200 bauds) pour voir :

```
========================================
   MIDI Slide Whistle Controller
========================================

Initializing modules...
StepperControl: Initialized
MIDIHandler: Initialized (USB MIDI)
AirControl: Initialized

Running self-tests...
AirControl: Testing fan...
AirControl: Testing servo...

Starting homing sequence...
StepperControl: Endstop detected
StepperControl: Homing complete

========================================
   System READY!
   Waiting for MIDI input...
========================================
```

### Problèmes au démarrage

| Symptôme | Cause probable | Solution |
|----------|----------------|----------|
| LED clignote indéfiniment | Capteur fin de course non détecté | Vérifier câblage, inverser `ENDSTOP_ACTIVE_STATE` |
| Moteur ne bouge pas | Driver désactivé ou mal câblé | Vérifier STEP/DIR/EN, alimentation 12V |
| Pas de communication USB | Mauvaise carte sélectionnée | Vérifier Type de carte dans Arduino IDE |

---

## 🎹 Utilisation

### Configuration logiciel MIDI

#### Windows

1. Brancher l'Arduino
2. Il apparaît comme "Arduino Leonardo" dans les périphériques MIDI
3. Dans votre DAW (Reaper, Ableton, etc.), sélectionner "Arduino Leonardo" comme sortie MIDI

#### macOS

1. Brancher l'Arduino
2. Ouvrir **Configuration MIDI Audio** (dans Applications/Utilitaires)
3. L'Arduino doit apparaître
4. Sélectionner dans votre DAW

#### Linux

```bash
# Vérifier que l'Arduino est détecté
aconnect -l

# Connecter depuis un soft MIDI
aconnect [source] [destination]
```

### Test avec un clavier virtuel

**VMPK (Virtual MIDI Piano Keyboard)** - Gratuit et multiplateforme

1. Télécharger sur https://vmpk.sourceforge.io/
2. Lancer VMPK
3. **Edit → MIDI Connections → MIDI OUT → Sélectionner Arduino**
4. Jouer des notes entre C3 et C6

### Paramètres recommandés

- **Canal MIDI** : 1 (par défaut)
- **Vélocité** : 64-127 pour un bon débit d'air
- **Plage de notes** : C3-C6 (MIDI 48-84)

---

## 🔧 Dépannage

### Problèmes MIDI

| Problème | Solution |
|----------|----------|
| Aucune réponse aux notes | Vérifier le canal MIDI dans `settings.h` |
| Notes hors de portée ignorées | Ajuster `MIDI_NOTE_MIN/MAX` |
| Décalage de hauteur | Calibrer `TOTAL_TRAVEL_STEPS` |

### Problèmes mécaniques

| Problème | Solution |
|----------|----------|
| Moteur saute des pas | Réduire `STEPPER_SPEED`, vérifier alimentation 12V |
| Bruit excessif | Activer microstepping (1/16), réduire vitesse |
| Positionnement imprécis | Vérifier fixation courroie, réduire jeu mécanique |

### Problèmes d'air

| Problème | Solution |
|----------|----------|
| Ventilateur ne démarre pas | Vérifier MOSFET, alimentation 12V |
| Pas assez d'air | Augmenter `SERVO_MAX_ANGLE`, ventilateur plus puissant |
| Coupures brusques | Augmenter `AIR_OFF_DELAY` dans settings.h |

### Debug avancé

Activer le mode verbose :

```cpp
#define DEBUG_MODE  true
```

Ouvrir le moniteur série (115200) pour voir :
- Position du stepper en temps réel
- Notes MIDI reçues
- État du ventilateur
- Événements système

---

## 🎛️ Optimisations

### Calibration fine

1. **Courbe de réponse** : Modifier le mapping note→position dans `StepperControl.h:137`
2. **Vélocité** : Ajuster la réponse du servo selon vos préférences
3. **Vitesse** : Équilibrer entre précision et réactivité

### Extensions possibles

- **Aftertouch** : Vibrato via servomoteur
- **Pitch bend** : Glissando contrôlé
- **Control Change** : Volume, modulation
- **Polyphonie** : Plusieurs pipeaux (nécessite duplication hardware)

---

## 📝 Licence

Ce projet est open-source. Consultez LICENSE pour plus de détails.

---

## 🙏 Crédits

- **AccelStepper** : Mike McCauley
- **MIDIUSB** : Arduino Team
- **Concept** : Votre projet unique !

---

**Bon contrôle MIDI ! 🎵**
