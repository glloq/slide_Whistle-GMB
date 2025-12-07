# Liste des composants - MIDI Slide Whistle

## 🛒 Liste d'achat complète

### Électronique principale

| Qté | Composant | Spécification | Prix estimé | Lien exemple |
|-----|-----------|---------------|-------------|--------------|
| 1 | Arduino Leonardo | Compatible MIDIUSB | ~15€ | [Arduino Store](https://store.arduino.cc) |
| 1 | Driver moteur A4988 | Avec dissipateur | ~3€ | Amazon, AliExpress |
| 1 | Moteur NEMA 17 | 1.8°, 200 steps/rev, 42mm | ~12€ | Amazon, Robotshop |
| 1 | Capteur fin de course | Micro-switch mécanique NF | ~2€ | Amazon, AliExpress |
| 1 | Ventilateur radial 12V | 2-3A, pour soufflerie | ~8€ | Amazon |
| 1 | Servomoteur MG996R | Standard, 180°, couple élevé | ~6€ | Amazon, AliExpress |

### Alimentation

| Qté | Composant | Spécification | Prix estimé |
|-----|-----------|---------------|-------------|
| 1 | Alimentation 12V 3A | Adaptateur secteur | ~8€ |
| 1 | Buck converter | 12V → 5V, 3A | ~3€ |
| 1 | Condensateur 100µF | 25V, électrolytique | ~0.50€ |

### Composants électroniques

| Qté | Composant | Spécification | Prix estimé |
|-----|-----------|---------------|-------------|
| 1 | MOSFET IRLZ44N | Logic-level, TO-220 | ~1€ |
| 1 | Diode 1N4007 | Protection flyback | ~0.20€ |
| 3 | Résistances 1kΩ | 1/4W | ~0.30€ |
| 1 | Plaque de prototypage | PCB ou breadboard | ~3€ |
| - | Fils dupont | M-M, M-F, F-F | ~5€ |

### Mécanique

| Qté | Composant | Spécification | Prix estimé |
|-----|-----------|---------------|-------------|
| 1 | Rail linéaire | MGN12H, 300-500mm | ~15€ |
| 1 | Chariot linéaire | Compatible rail | (inclus) |
| 1 | Poulie GT2 20 dents | Alésage 5mm (moteur) | ~2€ |
| 1 | Poulie GT2 20 dents | Alésage 8mm (slider) | ~2€ |
| 2m | Courroie GT2 | Largeur 6mm | ~3€ |
| 1 | Pipeau à coulisse | Instrument musical | ~5-20€ |
| - | Vis M3, M4, M5 | Assortiment | ~5€ |
| - | Profilés alu 20x20 | Pour châssis (optionnel) | ~15€ |

### Outillage nécessaire

- Fer à souder + étain
- Pince coupante
- Tournevis cruciforme
- Multimètre
- Imprimante 3D (ou service d'impression)

---

## 💰 Budget total

| Catégorie | Prix estimé |
|-----------|-------------|
| **Électronique** | ~55€ |
| **Alimentation** | ~12€ |
| **Mécanique** | ~50€ |
| **Divers** | ~10€ |
| **TOTAL** | **~130€** |

*Prix indicatifs, variables selon fournisseurs et localisation*

---

## 🔗 Fournisseurs recommandés

### Europe

- **Amazon.fr** : Livraison rapide, large choix
- **AliExpress** : Prix bas, délai 2-4 semaines
- **Robotshop.fr** : Spécialisé robotique
- **GoTronic** : Composants électroniques
- **RS Components** : Pro, catalogue complet

### Alternatives Arduino

Si Arduino Leonardo trop cher :

| Carte | Prix | Avantage |
|-------|------|----------|
| Arduino Micro | ~18€ | Plus compact |
| Arduino Leonardo clone | ~6€ | Économique |
| Teensy LC | ~12€ | Performant, petit |

---

## 📦 Kits recommandés

### Kit driver + moteur NEMA 17

Souvent vendus ensemble (~15€) :
- Driver A4988 ou DRV8825
- Moteur NEMA 17
- Dissipateur thermique
- Cavaliers microstepping

Recherche : "NEMA 17 kit A4988"

### Kit capteurs

Pack de micro-switches (~5€ pour 10 pièces)

---

## 🎯 Version minimale (sans servo)

Pour tester le concept à moindre coût :

| Composant | Prix |
|-----------|------|
| Arduino Leonardo clone | ~6€ |
| Kit NEMA17 + A4988 | ~15€ |
| Micro-switch | ~0.50€ |
| Ventilateur 12V | ~8€ |
| Alimentation | ~8€ |
| Buck converter | ~3€ |
| MOSFET + composants | ~2€ |
| Mécanique basique | ~10€ |
| **TOTAL** | **~53€** |

---

## 🧰 Fichiers 3D à imprimer

*(À créer selon votre design)*

Pièces suggérées :
- Support Arduino
- Boîtier driver moteur
- Support capteur fin de course
- Adaptateur moteur → courroie
- Support servo
- Buse ventilateur → pipeau
- Châssis général

**Format** : STL, STEP
**Matériau** : PLA ou PETG

---

## 📋 Checklist avant commande

- [ ] Vérifier tension alimentation (12V pour moteur/ventilateur)
- [ ] Vérifier compatibilité Arduino (doit supporter MIDIUSB)
- [ ] Mesurer dimensions du pipeau
- [ ] Calculer course nécessaire du slider
- [ ] Vérifier type de courroie (GT2 recommandé)
- [ ] Prévoir dissipateur thermique pour driver
- [ ] Câbles USB pour Arduino (Micro-USB ou USB-C)

---

## 🔄 Composants réutilisables

Si vous avez déjà un Arduino ou imprimante 3D :

**Déjà possédé** :
- Arduino → -15€
- Alimentation 12V → -8€
- Fils/outils → -5€

**Nouveau total** : ~100€

---

**Bon shopping ! 🛍️**
