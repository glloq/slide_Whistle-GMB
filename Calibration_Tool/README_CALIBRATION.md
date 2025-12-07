# 🎵 Outil de Calibration - MIDI Slide Whistle

Cet outil permet de créer la **table de lookup (LUT)** des positions pour obtenir une **justesse parfaite** sur toute la tessiture de l'instrument.

---

## 📋 Matériel Nécessaire

### **Hardware :**
- Arduino avec le système monté (moteur + slider + capteur fin de course)
- Connexion USB à l'ordinateur
- Pipeau à coulisse installé
- Source d'air (ventilateur ou compresseur + solénoïde)

### **Mesure :**
- **Tuner chromatique** (application smartphone ou hardware)
- OU **Microphone + logiciel d'analyse** (Audacity, TuneLab, etc.)
- OU **Oreille absolue** (pour musiciens expérimentés)

---

## 🚀 Installation

### **1. Matériel**
Assurez-vous que votre montage est **identique** à celui de votre version principale (Fan ou Solenoid).

### **2. Configuration**
Vérifiez que `settings.h` dans `Calibration_Tool/` a les **mêmes paramètres** que votre version principale :
- `STEPS_PER_MM`
- `SLIDER_TRAVEL_MM`
- `INVERT_MOTOR_DIR`
- Pins moteur et capteur

### **3. Upload**
1. Ouvrir `Calibration_Tool.ino` dans Arduino IDE
2. Sélectionner votre carte Arduino
3. Téléverser le programme

### **4. Moniteur Série**
- Ouvrir le **Moniteur Série** (Ctrl+Shift+M)
- Régler sur **115200 baud**
- Régler sur **Newline** ou **Both NL & CR**

---

## 🎯 Processus de Calibration

### **Workflow Complet**

Le programme commence automatiquement par un **homing**, puis se positionne à la **note la plus aiguë** (C6 - note 84).

```
C6 → B5 → A#5 → A5 → ... → D3 → C#3 → C3
(Note 84 → 83 → 82 → ... → 50 → 49 → 48)
```

### **Pour chaque note :**

1. **Positionner le tuner** près de la sortie du pipeau
2. **Tester le son** avec `T` (active l'air pendant 1 seconde)
3. **Ajuster la position** avec `+` et `-` jusqu'à justesse parfaite :
   ```
   +5      → avancer de 5 mm
   +0.5    → avancer de 0.5 mm (ajustement fin)
   -2      → reculer de 2 mm
   -0.1    → reculer de 0.1 mm (ajustement très fin)
   ```
4. **Vérifier** avec `T` plusieurs fois
5. **Sauvegarder** avec `S`
6. **Note suivante** avec `n`
7. **Répéter** jusqu'à la dernière note (C3)

---

## ⌨️ Commandes Disponibles

| Commande | Description | Exemple |
|----------|-------------|---------|
| `+ <valeur>` | Avancer de X mm | `+5` ou `+0.5` |
| `- <valeur>` | Reculer de X mm | `-2` ou `-0.1` |
| `n` | Passer à la note **suivante** (descendre) | `n` |
| `p` | Passer à la note **précédente** (monter) | `p` |
| `T` | **Tester** la note (air 1 sec) | `T` |
| `S` | **Sauvegarder** position actuelle | `S` |
| `L` | **Lister** toutes les positions calibrées | `L` |
| `H` | Refaire **homing** | `H` |
| `?` | Afficher **aide** | `?` |

---

## 📊 Exemple de Session

```
========================================
MIDI SLIDE WHISTLE - CALIBRATION TOOL
========================================

=== HOMING ===
Recherche du capteur fin de course...
Capteur détecté, recul...
Position zéro trouvée, déplacement offset...
✓ Homing terminé!

✓ Initialisation terminée!

Calibration commence par la note la plus AIGUË
et descend progressivement vers les GRAVES.

>>> Note 84 (C6) - Position: 300.00 mm [NON CALIBRÉE]

Commandes: +/- <val>, n (suivant), p (précédent), T (test), S (save), L (list), ? (aide)

> T
🎵 Test de la note pendant 1 seconde...
✓ Test terminé

> -5
Position: 295.00 mm

> T
🎵 Test de la note pendant 1 seconde...
✓ Test terminé

> -0.5
Position: 294.50 mm

> T
🎵 Test de la note pendant 1 seconde...
✓ Test terminé
[La note est juste sur le tuner]

> S
✓ Position sauvegardée!

>>> Note 84 (C6) - Position: 294.50 mm [CALIBRÉE ✓]

> n
Note suivante déjà calibrée, déplacement à 294.50 mm

>>> Note 83 (B5) - Position: 294.50 mm [NON CALIBRÉE]

> +1.2
Position: 295.70 mm

> T
🎵 Test de la note pendant 1 seconde...
✓ Test terminé

...
[Continuer jusqu'à la dernière note]

> L

========================================
POSITIONS CALIBRÉES
========================================
Note 48 (C3)  : 0.50 mm ✓
Note 49 (C#3) : 2.80 mm ✓
...
Note 84 (C6)  : 294.50 mm ✓
========================================
Progrès: 37/37 notes (100%)
========================================

🎉 CALIBRATION COMPLÈTE! 🎉

========================================
CODE À COPIER-COLLER DANS settings.h
========================================

// Table de lookup des positions (calibrée)
// Plage: Note 48 (C3) à Note 84 (C6)
const float NOTE_POSITION_LUT[] PROGMEM = {
    0.50,  // 48 - C3
    2.80,  // 49 - C#3
    5.20,  // 50 - D3
    ...
  294.50   // 84 - C6
};

========================================
```

---

## 🎯 Conseils de Calibration

### **Précision**
- Calibrer dans un **environnement silencieux**
- Utiliser un **bon tuner** (précision ±1 cent minimum)
- Laisser le son **se stabiliser** avant de lire le tuner (0.5 sec)
- **Tester plusieurs fois** chaque note avant de sauvegarder

### **Ajustements Fins**
- Commencer par ajustements **grossiers** (`+5`, `-5`)
- Affiner avec **petits incréments** (`+0.5`, `-0.2`)
- Pour précision ultime : `+0.1`, `-0.1`

### **Ordre de Calibration**
Le programme commence par les **aigus** et descend vers les **graves** car :
- Les notes aiguës sont plus faciles à caler (fréquence élevée)
- Position de départ = fin de course (position haute)
- Cohérence mécanique (évite retours en arrière)

### **Dépannage**
- **Son instable** → Vérifier pression d'air constante
- **Position incorrecte après `n`** → La note n'était pas calibrée
- **Homing échoue** → Vérifier câblage capteur fin de course
- **Moteur ne bouge pas** → Vérifier alimentation driver moteur

---

## 📥 Utilisation du Code Généré

### **1. Copier le code**
Lorsque vous tapez `L` après avoir calibré toutes les notes, le code LUT est affiché.

### **2. Ouvrir settings.h**
Ouvrir le fichier `settings.h` de votre version principale (Fan ou Solenoid).

### **3. Ajouter la LUT**
Ajouter le code copié **avant** le `#endif` final :

```cpp
// ============================================================================
// TABLE DE LOOKUP DES POSITIONS (LUT)
// ============================================================================

// Table de lookup des positions (calibrée)
// Plage: Note 48 (C3) à Note 84 (C6)
const float NOTE_POSITION_LUT[] PROGMEM = {
    0.50,  // 48 - C3
    2.80,  // 49 - C#3
    // ... (copier tout le tableau généré)
  294.50   // 84 - C6
};

#define USE_POSITION_LUT  true  // Activer l'utilisation de la LUT

#endif // SETTINGS_H
```

### **4. Recompiler et téléverser**
Le système utilisera maintenant la LUT pour une justesse parfaite !

---

## 🔧 Recalibration Partielle

Si vous voulez **recalibrer seulement quelques notes** :

1. Lancer l'outil de calibration
2. Utiliser `n` et `p` pour naviguer vers la note souhaitée
3. Ajuster avec `+` / `-`
4. Sauvegarder avec `S`
5. Utiliser `L` pour régénérer le code complet

Le programme conserve les positions déjà calibrées en mémoire.

---

## 📐 Informations Techniques

### **Précision Théorique**
Avec poulie 36 dents GT2 :
- Résolution : **0.0225 mm/pas** (1/16 microstepping)
- Ajustement fin ±0.1 mm = **±4 pas**
- Précision largement suffisante pour justesse musicale

### **Mémoire**
- LUT complète : **37 notes × 4 bytes = 148 octets**
- Stockage en **PROGMEM** (Flash) pour économiser la RAM

### **Format MIDI**
- Note 48 = C3 (130.81 Hz)
- Note 84 = C6 (1046.50 Hz)
- 3 octaves complètes

---

## 🎵 Bon Calibrage !

Prenez votre temps, la qualité de la calibration détermine la justesse finale de l'instrument. Comptez **15-25 minutes** pour une calibration complète soignée.
