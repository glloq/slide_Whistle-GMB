# 🌀 MIDI Slide Whistle - Version VENTILATEUR + SERVO

## 🎯 Principe de fonctionnement

Cette version utilise un **ventilateur tournant en continu** et un **servomoteur qui dirige le flux d'air**.

```
Ventilateur (12V)
      │
      │ Flux d'air constant
      ↓
Servomoteur ──→ Dirige le flux
      │
      ├─→ Note ON  : Vers le bec du pipeau → SON
      └─→ Note OFF : Ailleurs (à côté)    → PAS DE SON
```

### ✨ Caractéristiques

- ✅ **Ventilateur** : Toujours allumé (FAN_ALWAYS_ON = true)
- ✅ **Servo** : 2 positions fixes uniquement
  - `SERVO_NOTE_ON_ANGLE` (90°) : Air dirigé **vers le bec**
  - `SERVO_NOTE_OFF_ANGLE` (30°) : Air dirigé **ailleurs**
- ✅ **Vélocité MIDI** : Ignorée (pas de modulation)
- ✅ **Réactivité** : Instantanée (pas de délai d'ouverture)

---

## 🛠️ Hardware requis

| Composant | Spécification | Exemple |
|-----------|---------------|---------|
| **Ventilateur** | 12V radial, 2-3A | Ventilateur turbo 12V |
| **Servomoteur** | Standard 180°, couple moyen | SG90, MG90S |
| **Alimentation** | 12V 3A minimum | Adaptateur secteur |
| **MOSFET** | Logic-level (pour ventilateur) | IRLZ44N |

### Schéma de principe

```
       Ventilateur 12V
            ┃
   ╔════════╩════════╗
   ║   Flux d'air   ║
   ╚═══════╤═════════╝
           │
      Servomoteur
      (oriente le flux)
           │
     ┌─────┴─────┐
     │           │
  Vers bec    Ailleurs
   (SON)     (silence)
```

---

## ⚙️ Configuration (`settings.h`)

### Pins

```cpp
#define FAN_PIN              6     // Ventilateur (via MOSFET)
#define SERVO_PIN            9     // Servomoteur (PWM)
```

### Angles du servo

```cpp
// AJUSTER selon votre montage mécanique
#define SERVO_NOTE_ON_ANGLE  90    // Vers le bec
#define SERVO_NOTE_OFF_ANGLE 30    // Ailleurs
```

**Comment calibrer** :
1. Téléverser le code
2. Ouvrir le moniteur série
3. Jouer une note MIDI
4. Observer la direction du flux d'air
5. Ajuster les angles si nécessaire

### Ventilateur

```cpp
#define FAN_ALWAYS_ON        true  // Toujours allumé
#define FAN_ACTIVE_STATE     HIGH  // Selon votre MOSFET
```

---

## 🎹 Comportement MIDI

| Message MIDI | Action Ventilateur | Action Servo |
|--------------|-------------------|--------------|
| **Note ON** | Reste allumé | → `SERVO_NOTE_ON_ANGLE` (vers bec) |
| **Note OFF** | Reste allumé | → `SERVO_NOTE_OFF_ANGLE` (ailleurs) |
| **Pitch Bend** | Pas d'effet | Pas d'effet |
| **Aftertouch** | Pas d'effet | Pas d'effet |

**Notes** :
- La **vélocité** n'affecte **pas** l'angle du servo (juste 2 positions)
- Le **pitch bend** et l'**aftertouch** agissent sur le **moteur du slider** (vibrato, glissando)

---

## 🔧 Calibration mécanique

### Étape 1 : Position du servo

Positionner le servo de façon à ce que :
- À `SERVO_NOTE_ON_ANGLE` : Le flux d'air entre **directement dans le bec**
- À `SERVO_NOTE_OFF_ANGLE` : Le flux d'air passe **à côté** du bec

### Étape 2 : Test

```cpp
void testServo() {
  // Dans AirControl.h, fonction de test disponible
  // Alterne entre les 2 positions
}
```

Depuis le moniteur série, le test s'exécute au démarrage.

---

## 💡 Avantages de cette version

| Avantage | Description |
|----------|-------------|
| **Simplicité** | Pas de PWM, pas de machine à états |
| **Fiabilité** | Ventilateur simple, servo robuste |
| **Réactivité** | Changement instantané (pas de délai) |
| **Autonome** | Ventilateur intégré, pas de compresseur |
| **Économique** | Composants standards peu coûteux |

---

## ⚠️ Inconvénients

- ❌ **Pas de contrôle du volume** (vélocité MIDI ignorée)
- ❌ **Consommation continue** (ventilateur toujours en marche)
- ❌ **Bruit** du ventilateur constant

---

## 📊 Comparaison Fan vs Solenoid

| Critère | Version Fan | Version Solenoid |
|---------|-------------|------------------|
| **Source air** | Ventilateur intégré | Compresseur externe |
| **Réactivité** | Instantanée | Buffer 200ms |
| **Contrôle volume** | ❌ Non | ✅ Via vélocité |
| **Consommation** | Continue | PWM variable |
| **Complexité** | Simple | Avancée |
| **Legato** | N/A | ✅ Intelligent |

---

## 🚀 Démarrage rapide

### 1. Câblage

```
Arduino D6  ──→ MOSFET Gate ──→ Ventilateur 12V
Arduino D9  ──→ Servo Signal
Arduino 5V  ──→ Servo VCC
Arduino GND ──→ Servo GND + MOSFET Source + Alim GND
12V         ──→ Ventilateur + (via MOSFET Drain)
```

### 2. Configuration

Éditer `settings.h` :
- Ajuster `SERVO_NOTE_ON_ANGLE` et `SERVO_NOTE_OFF_ANGLE`
- Vérifier `FAN_PIN` et `SERVO_PIN`

### 3. Upload

```bash
cd slide_Whistle_Fan_Servo/
# Ouvrir slide_Whistle_Fan_Servo.ino
# Compiler et téléverser sur Arduino Leonardo/Micro
```

### 4. Test

1. Connecter clavier MIDI via USB
2. Jouer une note → Servo dirige vers le bec
3. Relâcher la note → Servo dirige ailleurs

---

## 🐛 Dépannage

### Le ventilateur ne démarre pas

- Vérifier alimentation 12V
- Vérifier MOSFET (gate à 5V quand actif)
- Vérifier `FAN_ACTIVE_STATE` (HIGH ou LOW)

### Le servo ne bouge pas

- Vérifier alimentation servo (5V)
- Vérifier pin PWM (D9 sur Leonardo)
- Tester avec `air.testServo()` au démarrage

### L'air ne va pas dans le bec

- Ajuster `SERVO_NOTE_ON_ANGLE`
- Vérifier positionnement mécanique du servo
- Repositionner le bec du pipeau

---

## 📝 Fichiers du projet

```
slide_Whistle_Fan_Servo/
├── slide_Whistle_Fan_Servo.ino   # Programme principal
├── settings.h                     # Configuration
├── StepperControl.h              # Moteur pas à pas
├── MIDIHandler.h                 # Gestion MIDI
├── AirControl.h                  # Ventilateur + Servo
└── README.md                     # Ce fichier
```

---

## 🎵 Bon jeu ! 🌀
