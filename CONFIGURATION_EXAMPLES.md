# Exemples de configuration - MIDI Slide Whistle

Ce document présente différentes configurations du fichier `settings.h` pour différents cas d'usage.

---

## 📐 Configuration 1 : Setup standard (recommandé)

**Caractéristiques** :
- Arduino Leonardo
- Driver A4988 en 1/16 microstepping
- NEMA 17 200 steps
- Course de 300mm avec courroie GT2 (20 dents)
- Plage C3-C6

### Calcul de la course

```
Poulie : 20 dents
Pas courroie GT2 : 2mm
Circonférence poulie : 20 × 2mm = 40mm

Course souhaitée : 300mm
Tours moteur : 300mm / 40mm = 7.5 tours
Pas moteur : 7.5 × 200 = 1500 pas
Avec microstepping 1/16 : 1500 × 16 = 24000 pas
```

### settings.h

```cpp
// MOTEUR
#define STEPPER_STEP_PIN      2
#define STEPPER_DIR_PIN       3
#define STEPPER_ENABLE_PIN    4

#define STEPS_PER_REVOLUTION  200
#define MICROSTEPS            16
#define STEPPER_SPEED         1200
#define STEPPER_ACCELERATION  600

#define TOTAL_TRAVEL_STEPS    24000  // 300mm de course
#define HOME_OFFSET_STEPS     800    // 5mm de marge

// CAPTEUR
#define ENDSTOP_PIN           5
#define ENDSTOP_ACTIVE_STATE  LOW

#define HOMING_SPEED          500
#define HOMING_BACKOFF_STEPS  160    // 1mm

// MIDI
#define MIDI_NOTE_MIN         48     // C3
#define MIDI_NOTE_MAX         84     // C6
#define MIDI_CHANNEL          1

// AIR
#define FAN_PIN               6
#define FAN_ACTIVE_STATE      HIGH

#define SERVO_PIN             9
#define SERVO_ENABLED         true
#define SERVO_MIN_ANGLE       40
#define SERVO_MAX_ANGLE       140
#define SERVO_DEFAULT_ANGLE   90

#define AIR_OFF_DELAY         80

// SYSTÈME
#define DEBUG_MODE            true
#define LED_ENABLED           true
```

---

## 🎵 Configuration 2 : Flûte soprano (haute)

**Caractéristiques** :
- Plage étendue : C4-C7
- Course réduite : 200mm
- Vitesse élevée pour réactivité

### settings.h

```cpp
// MOTEUR (course réduite)
#define TOTAL_TRAVEL_STEPS    16000  // 200mm

// MIDI (plage haute)
#define MIDI_NOTE_MIN         60     // C4
#define MIDI_NOTE_MAX         96     // C7

// VITESSE (réactivité)
#define STEPPER_SPEED         1500
#define STEPPER_ACCELERATION  800

// AIR (soufflerie réduite)
#define SERVO_MIN_ANGLE       30
#define SERVO_MAX_ANGLE       120
```

---

## 🎶 Configuration 3 : Basse (grave)

**Caractéristiques** :
- Plage basse : C2-C5
- Course longue : 500mm
- Ventilateur puissant

### settings.h

```cpp
// MOTEUR (course longue)
#define TOTAL_TRAVEL_STEPS    40000  // 500mm

// MIDI (plage basse)
#define MIDI_NOTE_MIN         36     // C2
#define MIDI_NOTE_MAX         72     // C5

// VITESSE (plus lent pour précision)
#define STEPPER_SPEED         900
#define STEPPER_ACCELERATION  400

// AIR (débit max)
#define SERVO_MIN_ANGLE       50
#define SERVO_MAX_ANGLE       160
#define AIR_OFF_DELAY         150    // Coupure plus douce
```

---

## ⚡ Configuration 4 : Vitesse maximale

**Pour morceaux rapides (musique électronique, jazz)**

### settings.h

```cpp
// VITESSE (performance)
#define STEPPER_SPEED         2000
#define STEPPER_ACCELERATION  1200

// AIR (réactivité)
#define AIR_OFF_DELAY         30     // Coupure rapide

// HOMING (rapide)
#define HOMING_SPEED          800
```

**⚠️ Attention** : Risque de perte de pas si alimentation insuffisante !

---

## 🔇 Configuration 5 : Silencieux

**Pour studio d'enregistrement**

### settings.h

```cpp
// MICROSTEPPING (silence)
#define MICROSTEPS            32     // Driver compatible requis

// VITESSE (douce)
#define STEPPER_SPEED         600
#define STEPPER_ACCELERATION  300

// DEBUG (désactiver)
#define DEBUG_MODE            false
```

---

## 💡 Configuration 6 : Sans servomoteur

**Version simplifiée**

### settings.h

```cpp
// SERVO (désactivé)
#define SERVO_ENABLED         false

// Ventilateur ON/OFF uniquement
// La vélocité MIDI n'affecte pas le débit
```

---

## 🎛️ Configuration 7 : Contrôle avancé

**Pour utilisateurs expérimentés**

### Ajout dans settings.h

```cpp
// COURBE DE RÉPONSE
#define POSITION_CURVE        LOGARITHMIC  // Plus naturel

// COMPENSATION VÉLOCITÉ
#define VELOCITY_COMPENSATION true
#define VELOCITY_OFFSET_MAX   500  // Ajustement position selon vélocité

// LIMITES DE SÉCURITÉ
#define ENABLE_SOFT_LIMITS    true
#define SOFT_LIMIT_MIN        100
#define SOFT_LIMIT_MAX        (TOTAL_TRAVEL_STEPS - 100)
```

### Modification dans StepperControl.h (ligne ~137)

```cpp
// Courbe logarithmique au lieu de linéaire
void moveToMidiNote(byte note) {
  note = constrain(note, MIDI_NOTE_MIN, MIDI_NOTE_MAX);

  // Calcul logarithmique
  float normalizedNote = (float)(note - MIDI_NOTE_MIN) / (MIDI_NOTE_MAX - MIDI_NOTE_MIN);
  float logPosition = pow(normalizedNote, 1.5); // Courbe exponentielle

  long position = (long)(logPosition * TOTAL_TRAVEL_STEPS * MICROSTEPS);

  moveTo(position);
}
```

---

## 🛠️ Configuration 8 : Driver DRV8825

**Si vous utilisez un DRV8825 au lieu de A4988**

### Différences

- Microstepping max : 1/32
- Tension max : 45V (vs 35V pour A4988)
- Plus silencieux

### settings.h

```cpp
// MICROSTEPPING (1/32 possible)
#define MICROSTEPS            32

// Recalculer TOTAL_TRAVEL_STEPS
#define TOTAL_TRAVEL_STEPS    48000  // Double de la config 1/16
```

---

## 🔍 Tableau comparatif

| Config | Course | Plage MIDI | Vitesse | Usage |
|--------|--------|------------|---------|-------|
| **Standard** | 300mm | C3-C6 | 1200 | Général |
| **Soprano** | 200mm | C4-C7 | 1500 | Aigu |
| **Basse** | 500mm | C2-C5 | 900 | Grave |
| **Rapide** | 300mm | C3-C6 | 2000 | Performance |
| **Silencieux** | 300mm | C3-C6 | 600 | Studio |
| **Simple** | 300mm | C3-C6 | 1200 | Sans servo |

---

## 📊 Guide de calibration

### Étape 1 : Mesurer la course mécanique

```cpp
// Temporairement, activer le mode manuel
void loop() {
  stepper.moveTo(100000); // Valeur arbitraire grande
  stepper.update();

  // Observer le moteur jusqu'à ce qu'il atteigne le bout
  // Noter le nombre de pas dans le Serial Monitor
}
```

### Étape 2 : Test des notes

```cpp
// Dans setup(), après homing
for (byte note = MIDI_NOTE_MIN; note <= MIDI_NOTE_MAX; note += 12) {
  stepper.moveToMidiNote(note);
  while (stepper.isMoving()) stepper.update();

  Serial.print("Note ");
  Serial.print(note);
  Serial.print(" = position ");
  Serial.println(stepper.getCurrentPosition());

  delay(2000); // Observer la position
}
```

### Étape 3 : Ajustement fin

Si les notes ne correspondent pas aux bonnes hauteurs :

1. **Trop grave** → Réduire `TOTAL_TRAVEL_STEPS`
2. **Trop aigu** → Augmenter `TOTAL_TRAVEL_STEPS`
3. **Non linéaire** → Implémenter courbe personnalisée

---

## 🧪 Configuration debug complète

**Pour développement et tests**

### settings.h

```cpp
#define DEBUG_MODE            true
#define SERIAL_BAUD_RATE      115200

// Ajouter dans slide_Whistle.ino, dans loop() :
static unsigned long lastDebugTime = 0;
if (millis() - lastDebugTime > 1000) {
  lastDebugTime = millis();

  Serial.print("Pos: ");
  Serial.print(stepper.getCurrentPosition());
  Serial.print(" | Target: ");
  Serial.print(stepper.getTargetPosition());
  Serial.print(" | Fan: ");
  Serial.print(air.isFanActive() ? "ON" : "OFF");
  Serial.print(" | Note: ");
  Serial.println(midi.getLastNote());
}
```

---

## 📝 Notes importantes

### Calcul de TOTAL_TRAVEL_STEPS

```
TOTAL_TRAVEL_STEPS = course_mm × (STEPS_PER_REVOLUTION × MICROSTEPS) / circonférence_poulie_mm

Exemple :
300mm × (200 × 16) / 40mm = 24000 pas
```

### Limites de vitesse

| Microstepping | Vitesse max recommandée |
|---------------|-------------------------|
| 1 (full step) | 3000 pas/s |
| 1/2 | 2000 pas/s |
| 1/4 | 1500 pas/s |
| 1/8 | 1200 pas/s |
| 1/16 | 1000 pas/s |
| 1/32 | 800 pas/s |

**Au-delà** : Risque de perte de pas

---

## ✅ Checklist de configuration

- [ ] Pins correctement définies (STEP, DIR, EN, ENDSTOP, FAN, SERVO)
- [ ] `STEPS_PER_REVOLUTION` correspond au moteur (généralement 200)
- [ ] `MICROSTEPS` correspond aux cavaliers du driver
- [ ] `TOTAL_TRAVEL_STEPS` calibré selon la mécanique
- [ ] `MIDI_NOTE_MIN/MAX` adaptés à l'instrument
- [ ] `STEPPER_SPEED` testé sans perte de pas
- [ ] `ENDSTOP_ACTIVE_STATE` vérifié (LOW ou HIGH)
- [ ] `FAN_ACTIVE_STATE` vérifié (HIGH ou LOW selon transistor)
- [ ] Servo testé avec `testServo()`
- [ ] Homing réussi au démarrage

---

**Bonne configuration ! 🎛️**
