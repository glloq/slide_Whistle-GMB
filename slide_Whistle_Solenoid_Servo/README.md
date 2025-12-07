# ⚡ MIDI Slide Whistle - Version SOLÉNOÏDE + SERVO (Avancé)

## 🎯 Principe de fonctionnement

Cette version utilise un **solénoïde/valve** pour ouvrir/fermer l'air et un **servomoteur** pour moduler le débit selon la vélocité MIDI.

```
Compresseur externe
      │
      ↓
Solénoïde (PWM intelligent)
      │
      ├─→ OUVERTURE : PWM 100% (12V) pendant 50ms
      ├─→ MAINTIEN   : PWM  47% (5.6V) continu
      └─→ FERMETURE  : PWM   0%
      │
      ↓
Servomoteur (module débit)
      │
      ↓
  Pipeau à coulisse
```

### ✨ Caractéristiques avancées

- 🔥 **Machine à états** : 5 états (IDLE → WAITING → OPENING → HOLDING → CLOSING)
- ⚡ **PWM intelligent** : Ouverture forte (12V), maintien économique (5.6V)
- 🎵 **Legato** : Notes rapides sans coupure (< 300ms)
- 🛡️ **Buffer de position** : Délai fixe 200ms avant ouverture
- 🎚️ **Contrôle du débit** : Servo ajusté selon vélocité MIDI

---

## 🔥 Machine à états

### Diagramme d'états

```
     ┌─────────────┐
     │    IDLE     │ ← Repos, solénoïde fermé
     └──────┬──────┘
            │ Note ON
            ↓
┌───────────────────┐
│ WAITING_POSITION  │ ← Délai 200ms (moteur en position)
└────────┬──────────┘
         │ Timer écoulé
         ↓
    ┌─────────┐
    │ OPENING │ ← PWM 100% pendant 50ms
    └────┬────┘
         │ Timer écoulé
         ↓
    ┌─────────┐
    │ HOLDING │ ← PWM 47% continu (anti-chauffe)
    └────┬────┘
         │ Note OFF
         ↓
    ┌─────────┐
    │ CLOSING │ ← Délai 50ms
    └────┬────┘
         │ Timer écoulé
         ↓
     (retour IDLE)
```

### Description des états

| État | PWM | Durée | Rôle |
|------|-----|-------|------|
| **IDLE** | 0% | ∞ | Repos, solénoïde fermé |
| **WAITING_POSITION** | 0% | 200ms | Attente position moteur + servo |
| **OPENING** | 100% | 50ms | Ouverture rapide pleine puissance |
| **HOLDING** | 47% | ∞ | Maintien avec PWM réduit |
| **CLOSING** | 0% | 50ms | Délai avant fermeture complète |

---

## ⚡ PWM intelligent

### Pourquoi le PWM ?

Un solénoïde a besoin de :
- **Forte puissance** pour **ouvrir** (vaincre le ressort)
- **Faible puissance** pour **maintenir** (juste retenir)

### Phases PWM

```
Phase 1 - OUVERTURE (courte, puissante)
┌──────┐
│ 100% │  ← PWM 255 (12V plein)
│      │     Durée: 50ms
└──────┘     Force max

Phase 2 - MAINTIEN (longue, économique)
         ┌─────────────────────────────┐
         │ 47% PWM (5.6V)             │
         │ Juste assez pour tenir     │
         └─────────────────────────────┘
         Réduit chauffe + économie
```

### Avantages

| Avantage | Description |
|----------|-------------|
| ✅ **Moins de chauffe** | Solénoïde ne surchauffe plus |
| ✅ **Économie d'énergie** | ~50% de réduction |
| ✅ **Durée de vie** | Solénoïde préservé |
| ✅ **Fiabilité** | Moins de fatigue mécanique |

---

## 🎵 Gestion Legato

### Détection automatique

```cpp
if (nouvelle_note < 300ms après précédente) {
  // LEGATO détecté
  → Solénoïde reste OUVERT
  → Seul le moteur bouge
  → Son continu et fluide
}
```

### Comportement

**Notes détachées (> 300ms)** :
```
Note1      OFF         Note2
  │─200ms─│Ouvert│─────│─200ms─│Ouvert│
          └Fermé┘              └Fermé┘
```

**Notes liées (< 300ms)** :
```
Note1              Note2              Note3
  │─200ms─│────────│────────│─────────│
          └──────Ouvert en continu─────┘
```

---

## 🛡️ Buffer de position

### Problème résolu

Sans buffer :
```
Note ON → Solénoïde OUVERT immédiatement
       → Air arrive pendant que moteur bouge
       → Son imprécis, bruit pendant déplacement
```

Avec buffer :
```
Note ON → Moteur commence à bouger
       → Servo se positionne
       → DÉLAI 200ms
       → Solénoïde s'ouvre
       → SON PROPRE dès l'ouverture !
```

### Paramètres

```cpp
#define POSITION_WAIT_DELAY  200  // Délai fixe en ms
```

**Ajustement** :
- Moteur rapide → Réduire (150ms)
- Moteur lent → Augmenter (250ms)

---

## 🛠️ Hardware requis

| Composant | Spécification | Exemple |
|-----------|---------------|---------|
| **Solénoïde** | 12V, normalement fermé | Valve pneumatique 12V |
| **Servomoteur** | Standard 180°, couple moyen | MG996R |
| **Compresseur** | 6-12 bar | Compresseur d'air |
| **Alimentation** | 12V 2A minimum | Adaptateur secteur |
| **Arduino** | Leonardo/Micro (PWM sur pin 6!) | Leonardo |

⚠️ **IMPORTANT** : Le `SOLENOID_PIN` (pin 6) **DOIT supporter le PWM** !

### Pins PWM sur Arduino Leonardo/Micro

Pins PWM disponibles : **3, 5, 6, 9, 10, 11, 13**

---

## ⚙️ Configuration (`settings.h`)

### Pins

```cpp
#define SOLENOID_PIN  6  // DOIT être PWM !
#define SERVO_PIN     9  // PWM
```

### PWM du solénoïde

```cpp
// AJUSTER selon votre solénoïde
#define SOLENOID_PWM_FULL  255  // Ouverture (100%)
#define SOLENOID_PWM_HOLD  120  // Maintien (47%)
```

**Comment calibrer** :
1. Commencer avec `SOLENOID_PWM_HOLD = 100`
2. Tester si le solénoïde **reste ouvert**
3. Si ça ferme : augmenter (120, 140...)
4. Si ça chauffe : réduire (100, 80...)

### Délais

```cpp
#define POSITION_WAIT_DELAY     200  // Attente position
#define SOLENOID_OPEN_DURATION  50   // Durée PWM max
#define SOLENOID_CLOSE_DELAY    50   // Délai fermeture
#define LEGATO_THRESHOLD        300  // Seuil legato
```

### Servomoteur

```cpp
#define SERVO_CLOSED_ANGLE  30   // Débit min
#define SERVO_OPEN_ANGLE    150  // Débit max
```

La **vélocité MIDI** (0-127) contrôle l'angle entre ces 2 valeurs.

---

## 🎹 Comportement MIDI

| Message MIDI | Action Solénoïde | Action Servo | Effet |
|--------------|-----------------|--------------|-------|
| **Note ON (vel 64)** | WAITING → OPENING → HOLDING | Angle ~90° | Son moyen |
| **Note ON (vel 127)** | WAITING → OPENING → HOLDING | Angle 150° | Son fort |
| **Note OFF** | CLOSING → IDLE | Angle 30° | Silence |
| **Pitch Bend** | Pas d'effet | Pas d'effet | Agit sur moteur |
| **Aftertouch** | Pas d'effet | Pas d'effet | Agit sur moteur |

---

## 📊 Cas d'usage

### Cas 1 : Note simple

```
Note ON (C4, velocity 100)
  ↓
IDLE → WAITING_POSITION (200ms)
  ↓
OPENING (PWM 255, 50ms)
  ↓
HOLDING (PWM 120)
  ↓ Note OFF
CLOSING (50ms)
  ↓
IDLE
```

### Cas 2 : Notes rapides (legato)

```
Note ON (C4)
  → WAITING (200ms) → OPENING → HOLDING

Note ON (D4) ← 150ms après C4
  → LEGATO détecté !
  → Reste HOLDING
  → Juste moteur bouge

Note ON (E4) ← 100ms après D4
  → Toujours LEGATO
  → Toujours HOLDING
```

### Cas 3 : Note pendant attente

```
Note ON (C4)
  → WAITING (100ms écoulé...)

Note ON (D4) ← Nouvelle note
  → Annule WAITING
  → Moteur vers D4
  → Nouveau WAITING (200ms)
```

---

## 🔧 Calibration

### Étape 1 : Test du solénoïde

Au démarrage, le test s'exécute :
```
Opening (PWM FULL)  → Doit s'ouvrir
Holding (PWM reduced) → Doit rester ouvert
Closing             → Doit se fermer
```

### Étape 2 : Ajuster PWM HOLD

Si le solénoïde :
- **Se ferme** pendant HOLDING → Augmenter `SOLENOID_PWM_HOLD`
- **Chauffe** trop → Réduire `SOLENOID_PWM_HOLD`

### Étape 3 : Tester legato

Jouer des notes rapides (< 300ms) → Pas de coupure audible

---

## 💡 Avantages de cette version

| Avantage | Description |
|----------|-------------|
| **Son précis** | Air arrive quand position atteinte |
| **Économie** | PWM réduit = moins de chauffe |
| **Legato fluide** | Notes rapides sans coupure |
| **Contrôle du volume** | Vélocité MIDI → angle servo |
| **Robustesse** | Gestion intelligente cas limites |

---

## ⚠️ Inconvénients

- ❌ **Complexité** : Machine à états, PWM
- ❌ **Compresseur requis** : Source d'air externe
- ❌ **Délai 200ms** : Légère latence (mais justifiée)

---

## 🐛 Dépannage

### Le solénoïde ne s'ouvre pas

- Vérifier alimentation 12V
- Vérifier que pin 6 est bien PWM
- Augmenter `SOLENOID_PWM_FULL` si nécessaire

### Le solénoïde chauffe

- Réduire `SOLENOID_PWM_HOLD` (tester 100, 80...)
- Vérifier que le solénoïde supporte 12V continu

### Le solénoïde se ferme pendant HOLDING

- Augmenter `SOLENOID_PWM_HOLD`
- Vérifier pression du compresseur (pas trop forte)

### Pas de son ou son trop faible

- Augmenter `SERVO_OPEN_ANGLE`
- Vérifier pression du compresseur

### Notes coupées en legato

- Augmenter `LEGATO_THRESHOLD` (400, 500ms)
- Vérifier logs de debug (LEGATO détecté ?)

---

## 📝 Fichiers du projet

```
slide_Whistle_Solenoid_Servo/
├── slide_Whistle_Solenoid_Servo.ino  # Programme principal
├── settings.h                         # Configuration
├── StepperControl.h                  # Moteur pas à pas
├── MIDIHandler.h                     # Gestion MIDI
├── AirControl.h                      # Solénoïde PWM + Machine à états
└── README.md                         # Ce fichier
```

---

## 🎵 Bon jeu ! ⚡
