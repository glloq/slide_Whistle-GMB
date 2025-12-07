# 📐 Analyse : Diamètre de poulie GT2 optimal

## 🎯 Objectif

Trouver le **meilleur compromis** entre :
- **Vitesse** de déplacement (réactivité)
- **Précision** de positionnement (qualité du son)
- **Délai minimal** avant ouverture air

---

## 🔧 Poulies GT2 standards disponibles

Courroie **GT2** (pas de 2mm) :

| Dents | Diamètre primitif | Circonférence | Prix |
|-------|-------------------|---------------|------|
| **16** | 10.2 mm | 32.0 mm | ~2€ |
| **20** | 12.7 mm | 40.0 mm | ~2€ |
| **24** | 15.3 mm | 48.0 mm | ~3€ |
| **30** | 19.1 mm | 60.0 mm | ~4€ |
| **36** | 23.0 mm | 72.0 mm | ~5€ |

**Formule** : `Circonférence = Nb_dents × 2mm`

---

## 📊 Calculs de performance

### Configuration moteur

```cpp
STEPS_PER_REVOLUTION  = 200   // Moteur NEMA 17
MICROSTEPS            = 16    // Driver A4988/DRV8825
STEPPER_SPEED_MM_S    = 25.0  // Vitesse max (mm/s)
```

### Pas par mm selon poulie

```
STEPS_PER_MM = (STEPS_PER_REVOLUTION × MICROSTEPS) / Circonférence

Poulie 16 dents : (200 × 16) / 32  = 100 pas/mm
Poulie 20 dents : (200 × 16) / 40  = 80 pas/mm  ← STANDARD
Poulie 24 dents : (200 × 16) / 48  = 66.7 pas/mm
Poulie 30 dents : (200 × 16) / 60  = 53.3 pas/mm
Poulie 36 dents : (200 × 16) / 72  = 44.4 pas/mm
```

---

## ⚡ Vitesse vs Précision

### 1. Vitesse de déplacement

**Vitesse max du moteur** : Limité par `STEPPER_SPEED` en pas/seconde

```
Vitesse linéaire (mm/s) = STEPPER_SPEED_PAS_S / STEPS_PER_MM

Avec STEPPER_SPEED = 2000 pas/s (vitesse réaliste) :

Poulie 16 dents : 2000 / 100   = 20 mm/s
Poulie 20 dents : 2000 / 80    = 25 mm/s  ← STANDARD
Poulie 24 dents : 2000 / 66.7  = 30 mm/s
Poulie 30 dents : 2000 / 53.3  = 37.5 mm/s
Poulie 36 dents : 2000 / 44.4  = 45 mm/s
```

**⚡ Plus la poulie est grande, plus c'est RAPIDE**

---

### 2. Précision (résolution)

**Résolution** = Distance minimale par pas

```
Résolution (µm) = Circonférence / (STEPS × MICROSTEPS)

Poulie 16 dents : 32 / 3200  = 0.010 mm = 10 µm
Poulie 20 dents : 40 / 3200  = 0.0125 mm = 12.5 µm  ← STANDARD
Poulie 24 dents : 48 / 3200  = 0.015 mm = 15 µm
Poulie 30 dents : 60 / 3200  = 0.01875 mm = 18.75 µm
Poulie 36 dents : 72 / 3200  = 0.0225 mm = 22.5 µm
```

**🎯 Plus la poulie est petite, plus c'est PRÉCIS**

---

## ⏱️ Temps de déplacement entre notes

### Distance entre notes

```
Course totale : 300 mm
Plage MIDI : 48-84 (36 demi-tons)

Distance par demi-ton = 300 / 36 = 8.33 mm
```

### Temps de déplacement (1 demi-ton)

**Avec accélération** (profil trapézoïdal) :

```
Temps ≈ Distance / (Vitesse × 0.7)  [coefficient accélération]

Poulie 16 dents : 8.33 / (20 × 0.7)  = 0.60 s  ❌ TROP LENT
Poulie 20 dents : 8.33 / (25 × 0.7)  = 0.48 s  ⚠️ Lent
Poulie 24 dents : 8.33 / (30 × 0.7)  = 0.40 s  ⚠️ Acceptable
Poulie 30 dents : 8.33 / (37.5 × 0.7) = 0.32 s  ✅ Bon
Poulie 36 dents : 8.33 / (45 × 0.7)  = 0.26 s  ✅ Rapide
```

### Temps de déplacement (octave = 12 demi-tons)

```
Distance octave = 12 × 8.33 = 100 mm

Poulie 16 dents : 100 / 14  = 7.1 s   ❌❌❌
Poulie 20 dents : 100 / 17.5 = 5.7 s   ❌❌
Poulie 24 dents : 100 / 21  = 4.8 s   ❌
Poulie 30 dents : 100 / 26.25 = 3.8 s   ⚠️
Poulie 36 dents : 100 / 31.5 = 3.2 s   ✅
```

---

## 🎯 Délai optimal POSITION_WAIT_DELAY

Le délai doit être **au moins égal** au temps de déplacement max :

```
POSITION_WAIT_DELAY = Temps_déplacement_max + Marge_sécurité

Distance max entre 2 notes = 100 mm (octave)
```

### Calcul selon poulie

| Poulie | Temps 1 demi-ton | Temps octave | Délai recommandé |
|--------|------------------|--------------|------------------|
| **16 dents** | 600 ms | 7100 ms | ❌ Non recommandé |
| **20 dents** | 480 ms | 5700 ms | ❌ Trop lent |
| **24 dents** | 400 ms | 4800 ms | 500 ms |
| **30 dents** | 320 ms | 3800 ms | **350 ms** ✅ |
| **36 dents** | 260 ms | 3200 ms | **280 ms** ✅ |

**⚡ Mais en pratique** : Les notes consécutives sont rarement à 1 octave d'écart !

---

## 🎼 Analyse réaliste (musique)

### Distance moyenne entre notes

Musique typique : **90% des intervalles < 5 demi-tons**

```
Distance moyenne ≈ 3 demi-tons = 25 mm

Temps déplacement 25 mm :

Poulie 20 dents : 25 / 17.5 ≈ 1.4 s  ❌
Poulie 24 dents : 25 / 21   ≈ 1.2 s  ⚠️
Poulie 30 dents : 25 / 26.25 ≈ 950 ms ⚠️
Poulie 36 dents : 25 / 31.5 ≈ 800 ms ✅
```

**Délai optimal pour usage musical** :

| Poulie | Délai pour 3 demi-tons | Délai pour octave |
|--------|------------------------|-------------------|
| **20 dents** | 1400 ms + 100 ms = **1500 ms** ❌ |
| **24 dents** | 1200 ms + 100 ms = **1300 ms** ❌ |
| **30 dents** | 950 ms + 100 ms = **1050 ms** ⚠️ |
| **36 dents** | 800 ms + 100 ms = **900 ms** ✅ |

---

## 🏆 Recommandations

### ✅ OPTIMAL : Poulie 36 dents

**Avantages** :
- ✅ Vitesse max : **45 mm/s**
- ✅ Temps 1 demi-ton : **260 ms**
- ✅ Temps octave : **3.2 s**
- ✅ Délai optimal : **100-150 ms** (notes rapprochées)
- ✅ Réactivité musicale acceptable

**Inconvénients** :
- ⚠️ Précision : 22.5 µm (largement suffisant)
- ⚠️ Prix légèrement plus élevé (~5€)

### ⚠️ COMPROMIS : Poulie 30 dents

**Avantages** :
- ✅ Bon compromis vitesse/précision
- ✅ Temps 1 demi-ton : **320 ms**
- ✅ Prix raisonnable (~4€)

**Inconvénients** :
- ⚠️ Délai optimal : **150-200 ms**

### ❌ DÉCONSEILLÉ : Poulie 20 dents

**Raison** :
- ❌ Trop lent : 480 ms par demi-ton
- ❌ Latence perceptible
- ❌ Inadapté à la musique

---

## 🔢 Calculateur pour votre configuration

### Vos paramètres

```cpp
// À mettre dans settings.h

// Pour poulie 36 dents
#define STEPS_PER_MM      44.4   // (200 × 16) / 72
#define STEPPER_SPEED_MM_S 45.0   // Vitesse optimale
#define POSITION_WAIT_DELAY 150   // Délai optimisé (ms)

// Pour poulie 30 dents
#define STEPS_PER_MM      53.3   // (200 × 16) / 60
#define STEPPER_SPEED_MM_S 37.5   // Vitesse optimale
#define POSITION_WAIT_DELAY 180   // Délai optimisé (ms)
```

---

## 📈 Tableau récapitulatif

| Critère | 16 dents | 20 dents | 24 dents | 30 dents | **36 dents** |
|---------|----------|----------|----------|----------|--------------|
| **Vitesse (mm/s)** | 20 | 25 | 30 | 37.5 | **45** ✅ |
| **Précision (µm)** | 10 ✅ | 12.5 | 15 | 18.75 | 22.5 |
| **Temps 1 demi-ton** | 600 ms | 480 ms | 400 ms | 320 ms | **260 ms** ✅ |
| **Délai optimal** | ❌ | ❌ | 500 ms | 350 ms | **150 ms** ✅ |
| **Prix** | 2€ | 2€ | 3€ | 4€ | 5€ |
| **Verdict** | ❌ | ❌ | ⚠️ | ✅ Bon | ✅ **Optimal** |

---

## 💡 Conclusion

### Pour la version Solenoid (délai important)

**Choix optimal** : **Poulie 36 dents**

```cpp
#define STEPS_PER_MM         44.4
#define STEPPER_SPEED_MM_S   45.0
#define POSITION_WAIT_DELAY  150   // Au lieu de 200 !
```

**Gain** :
- 200 ms → **150 ms** = -25% de latence
- Plus réactif musicalement

### Pour la version Fan (pas de délai)

**Choix optimal** : **Poulie 30 ou 36 dents**

Pas de délai avant ouverture, donc la vitesse améliore juste la réactivité générale.

---

## ⚙️ Tests recommandés

1. **Démarrer avec poulie 36 dents**
2. **Mesurer temps réel** entre notes (moniteur série)
3. **Ajuster POSITION_WAIT_DELAY** :
   - Trop court → Air arrive avant position
   - Trop long → Latence perceptible
4. **Affiner avec STEPPER_SPEED** si perte de pas

---

**🎵 La poulie 36 dents réduit la latence de 50 ms et améliore grandement la jouabilité !**
