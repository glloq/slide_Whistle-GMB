/*
 * MIDI Slide Whistle - Calibration Tool
 *
 * Outil de calibration pour créer la table de lookup (LUT) des positions
 *
 * WORKFLOW :
 * 1. Homing automatique au démarrage
 * 2. Commence à la note la plus aiguë (C6 - note 84)
 * 3. Ajuster position avec +/- jusqu'à justesse parfaite
 * 4. Sauvegarder avec S
 * 5. Passer à note suivante avec n (descend vers les graves)
 * 6. Répéter jusqu'à la note la plus grave (C3 - note 48)
 * 7. Copier-coller le tableau LUT généré dans settings.h
 *
 * COMMANDES SÉRIE :
 * + <valeur>  : avancer de X mm (ex: +5 ou +0.5)
 * - <valeur>  : reculer de X mm (ex: -5 ou -0.5)
 * n           : passer à la note suivante (descendre d'un demi-ton)
 * p           : passer à la note précédente (monter d'un demi-ton)
 * T           : tester la note actuelle (activer air 1 sec)
 * S           : sauvegarder position actuelle
 * L           : lister toutes les positions calibrées
 * H           : refaire homing
 * ?           : afficher l'aide
 */

#include <AccelStepper.h>
#include "settings.h"

// ============================================================================
// VARIABLES GLOBALES
// ============================================================================

AccelStepper stepper(AccelStepper::DRIVER, STEPPER_STEP_PIN, STEPPER_DIR_PIN);

// Table de lookup des positions (initialisée à -1 = non calibrée)
float positionLUT[MIDI_NOTE_MAX - MIDI_NOTE_MIN + 1];
const int LUT_SIZE = MIDI_NOTE_MAX - MIDI_NOTE_MIN + 1;

// État de calibration
byte currentNote = MIDI_NOTE_MAX;  // Commence par la note la plus aiguë
float currentPositionMM = 0.0;
bool isHomed = false;

// Buffer série
String serialBuffer = "";

// ============================================================================
// CONVERSION MM <-> STEPS
// ============================================================================

long mmToSteps(float mm) {
  int directionMultiplier = INVERT_MOTOR_DIR ? -1 : 1;
  return (long)(mm * STEPS_PER_MM * directionMultiplier);
}

float stepsToMM(long steps) {
  int directionMultiplier = INVERT_MOTOR_DIR ? -1 : 1;
  return (float)steps / STEPS_PER_MM / directionMultiplier;
}

// ============================================================================
// FONCTIONS MOTEUR
// ============================================================================

bool performHoming() {
  Serial.println(F("\n=== HOMING ==="));
  Serial.println(F("Recherche du capteur fin de course..."));

  stepper.setMaxSpeed(HOMING_SPEED_MM_S * STEPS_PER_MM);
  stepper.setAcceleration(STEPPER_ACCEL_MM_S2 * STEPS_PER_MM);

  // Phase 1 : Recherche rapide du capteur
  int directionMultiplier = INVERT_MOTOR_DIR ? 1 : -1;
  stepper.move(directionMultiplier * 999999);  // Mouvement infini vers le capteur

  unsigned long startTime = millis();
  while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
    stepper.run();

    if (millis() - startTime > MAX_HOMING_TIME) {
      Serial.println(F("ERREUR: Timeout homing!"));
      return false;
    }
  }

  stepper.stop();
  stepper.setCurrentPosition(0);

  // Phase 2 : Recul
  Serial.println(F("Capteur détecté, recul..."));
  long backoffSteps = mmToSteps(HOMING_BACKOFF_MM);
  stepper.moveTo(-backoffSteps);
  while (stepper.distanceToGo() != 0) {
    stepper.run();
  }

  // Phase 3 : Recherche lente
  stepper.setMaxSpeed(HOMING_SPEED_MM_S * STEPS_PER_MM * 0.3);
  stepper.move(directionMultiplier * 999999);

  while (digitalRead(ENDSTOP_PIN) != ENDSTOP_ACTIVE_STATE) {
    stepper.run();
  }

  stepper.stop();
  stepper.setCurrentPosition(0);

  // Phase 4 : Aller à l'offset de départ
  Serial.println(F("Position zéro trouvée, déplacement offset..."));
  stepper.setMaxSpeed(STEPPER_SPEED_MM_S * STEPS_PER_MM);
  long homeOffsetSteps = mmToSteps(HOME_OFFSET_MM);
  stepper.moveTo(-homeOffsetSteps);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
  }

  stepper.setCurrentPosition(0);
  currentPositionMM = 0.0;

  Serial.println(F("✓ Homing terminé!\n"));
  return true;
}

void moveToPosition(float targetMM) {
  if (targetMM < 0) targetMM = 0;
  if (targetMM > SLIDER_TRAVEL_MM) targetMM = SLIDER_TRAVEL_MM;

  long targetSteps = mmToSteps(targetMM);
  stepper.moveTo(targetSteps);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
  }

  currentPositionMM = targetMM;
}

void adjustPosition(float deltaMM) {
  float newPosition = currentPositionMM + deltaMM;

  if (newPosition < 0) newPosition = 0;
  if (newPosition > SLIDER_TRAVEL_MM) newPosition = SLIDER_TRAVEL_MM;

  moveToPosition(newPosition);
  Serial.print(F("Position: "));
  Serial.print(currentPositionMM, 2);
  Serial.println(F(" mm"));
}

// ============================================================================
// FONCTIONS AIR (pour test)
// ============================================================================

void testNote() {
  Serial.println(F("🎵 Test de la note pendant 1 seconde..."));
  digitalWrite(AIR_CONTROL_PIN, AIR_ACTIVE_STATE);
  delay(TEST_DURATION_MS);
  digitalWrite(AIR_CONTROL_PIN, !AIR_ACTIVE_STATE);
  Serial.println(F("✓ Test terminé"));
}

// ============================================================================
// FONCTIONS LUT
// ============================================================================

void initLUT() {
  for (int i = 0; i < LUT_SIZE; i++) {
    positionLUT[i] = -1.0;  // -1 = non calibré
  }
}

void saveCurrentPosition() {
  int index = currentNote - MIDI_NOTE_MIN;
  positionLUT[index] = currentPositionMM;

  Serial.println(F("✓ Position sauvegardée!"));
  printCurrentNoteStatus();
}

void printCurrentNoteStatus() {
  Serial.print(F("\n>>> Note "));
  Serial.print(currentNote);
  Serial.print(F(" ("));
  Serial.print(getMIDINoteName(currentNote));
  Serial.print(F(") - Position: "));

  int index = currentNote - MIDI_NOTE_MIN;
  if (positionLUT[index] >= 0) {
    Serial.print(positionLUT[index], 2);
    Serial.println(F(" mm [CALIBRÉE ✓]"));
  } else {
    Serial.print(currentPositionMM, 2);
    Serial.println(F(" mm [NON CALIBRÉE]"));
  }
  Serial.println();
}

void nextNote() {
  if (currentNote > MIDI_NOTE_MIN) {
    currentNote--;

    // Si la note suivante est déjà calibrée, aller à sa position
    int index = currentNote - MIDI_NOTE_MIN;
    if (positionLUT[index] >= 0) {
      Serial.print(F("Note suivante déjà calibrée, déplacement à "));
      Serial.print(positionLUT[index], 2);
      Serial.println(F(" mm"));
      moveToPosition(positionLUT[index]);
    }

    printCurrentNoteStatus();
  } else {
    Serial.println(F("⚠ Déjà à la note la plus grave!"));
  }
}

void previousNote() {
  if (currentNote < MIDI_NOTE_MAX) {
    currentNote++;

    // Si la note précédente est déjà calibrée, aller à sa position
    int index = currentNote - MIDI_NOTE_MIN;
    if (positionLUT[index] >= 0) {
      Serial.print(F("Note précédente déjà calibrée, déplacement à "));
      Serial.print(positionLUT[index], 2);
      Serial.println(F(" mm"));
      moveToPosition(positionLUT[index]);
    }

    printCurrentNoteStatus();
  } else {
    Serial.println(F("⚠ Déjà à la note la plus aiguë!"));
  }
}

void listCalibration() {
  Serial.println(F("\n========================================"));
  Serial.println(F("POSITIONS CALIBRÉES"));
  Serial.println(F("========================================"));

  int calibratedCount = 0;
  for (int i = 0; i < LUT_SIZE; i++) {
    byte note = MIDI_NOTE_MIN + i;
    Serial.print(F("Note "));
    Serial.print(note);
    Serial.print(F(" ("));
    Serial.print(getMIDINoteName(note));
    Serial.print(F(")\t: "));

    if (positionLUT[i] >= 0) {
      Serial.print(positionLUT[i], 2);
      Serial.println(F(" mm ✓"));
      calibratedCount++;
    } else {
      Serial.println(F("--- NON CALIBRÉE"));
    }
  }

  Serial.println(F("========================================"));
  Serial.print(F("Progrès: "));
  Serial.print(calibratedCount);
  Serial.print(F("/"));
  Serial.print(LUT_SIZE);
  Serial.print(F(" notes ("));
  Serial.print((calibratedCount * 100) / LUT_SIZE);
  Serial.println(F("%)"));
  Serial.println(F("========================================\n"));

  // Si toutes les notes sont calibrées, générer le code
  if (calibratedCount == LUT_SIZE) {
    Serial.println(F("\n🎉 CALIBRATION COMPLÈTE! 🎉\n"));
    generateLUTCode();
  }
}

void generateLUTCode() {
  Serial.println(F("\n========================================"));
  Serial.println(F("CODE À COPIER-COLLER DANS settings.h"));
  Serial.println(F("========================================\n"));

  Serial.println(F("// Table de lookup des positions (calibrée)"));
  Serial.print(F("// Plage: Note "));
  Serial.print(MIDI_NOTE_MIN);
  Serial.print(F(" ("));
  Serial.print(getMIDINoteName(MIDI_NOTE_MIN));
  Serial.print(F(") à Note "));
  Serial.print(MIDI_NOTE_MAX);
  Serial.print(F(" ("));
  Serial.print(getMIDINoteName(MIDI_NOTE_MAX));
  Serial.println(F(")"));
  Serial.println(F("const float NOTE_POSITION_LUT[] PROGMEM = {"));

  for (int i = 0; i < LUT_SIZE; i++) {
    byte note = MIDI_NOTE_MIN + i;

    Serial.print(F("  "));

    if (positionLUT[i] >= 0) {
      // Formater avec 2 décimales, alignement
      if (positionLUT[i] < 10) Serial.print(F(" "));
      if (positionLUT[i] < 100) Serial.print(F(" "));
      Serial.print(positionLUT[i], 2);
    } else {
      Serial.print(F("  0.00"));  // Valeur par défaut pour notes non calibrées
    }

    if (i < LUT_SIZE - 1) {
      Serial.print(F(","));
    }

    Serial.print(F("  // "));
    Serial.print(note);
    Serial.print(F(" - "));
    Serial.println(getMIDINoteName(note));
  }

  Serial.println(F("};"));
  Serial.println(F("\n========================================\n"));
}

// ============================================================================
// UTILITAIRES
// ============================================================================

const char* getMIDINoteName(byte note) {
  static const char* noteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
  };

  static char buffer[6];
  int octave = (note / 12) - 1;
  int noteIndex = note % 12;

  sprintf(buffer, "%s%d", noteNames[noteIndex], octave);
  return buffer;
}

void printHelp() {
  Serial.println(F("\n========================================"));
  Serial.println(F("COMMANDES DE CALIBRATION"));
  Serial.println(F("========================================"));
  Serial.println(F("+ <valeur>  : Avancer de X mm (ex: +5 ou +0.5)"));
  Serial.println(F("- <valeur>  : Reculer de X mm (ex: -5 ou -0.5)"));
  Serial.println(F("n           : Note suivante (descendre)"));
  Serial.println(F("p           : Note précédente (monter)"));
  Serial.println(F("T           : Tester note (air 1 sec)"));
  Serial.println(F("S           : Sauvegarder position"));
  Serial.println(F("L           : Lister calibration"));
  Serial.println(F("H           : Refaire homing"));
  Serial.println(F("?           : Afficher aide"));
  Serial.println(F("========================================\n"));
}

// ============================================================================
// TRAITEMENT COMMANDES SÉRIE
// ============================================================================

void processCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  char firstChar = cmd.charAt(0);

  // Commandes avec argument numérique
  if (firstChar == '+' || firstChar == '-') {
    float value = cmd.substring(1).toFloat();
    if (value == 0 && cmd.length() > 1) {
      Serial.println(F("⚠ Valeur invalide"));
      return;
    }

    if (firstChar == '+') {
      adjustPosition(value);
    } else {
      adjustPosition(-value);
    }
    return;
  }

  // Commandes simples
  switch (firstChar) {
    case 'n':
    case 'N':
      nextNote();
      break;

    case 'p':
    case 'P':
      previousNote();
      break;

    case 'T':
    case 't':
      testNote();
      break;

    case 'S':
    case 's':
      saveCurrentPosition();
      break;

    case 'L':
    case 'l':
      listCalibration();
      break;

    case 'H':
    case 'h':
      if (performHoming()) {
        isHomed = true;
        // Aller à la position de la note aiguë si calibrée
        int index = currentNote - MIDI_NOTE_MIN;
        if (positionLUT[index] >= 0) {
          moveToPosition(positionLUT[index]);
        }
        printCurrentNoteStatus();
      }
      break;

    case '?':
      printHelp();
      printCurrentNoteStatus();
      break;

    default:
      Serial.println(F("⚠ Commande inconnue. Tapez ? pour l'aide"));
      break;
  }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  // Configuration série
  Serial.begin(SERIAL_BAUD_RATE);
  while (!Serial) { ; }  // Attendre connexion USB

  delay(500);

  Serial.println(F("\n\n========================================"));
  Serial.println(F("MIDI SLIDE WHISTLE - CALIBRATION TOOL"));
  Serial.println(F("========================================\n"));

  // Configuration pins
  pinMode(ENDSTOP_PIN, INPUT_PULLUP);
  pinMode(AIR_CONTROL_PIN, OUTPUT);
  digitalWrite(AIR_CONTROL_PIN, !AIR_ACTIVE_STATE);

  if (LED_ENABLED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);
  }

  // Configuration moteur
  stepper.setMaxSpeed(STEPPER_SPEED_MM_S * STEPS_PER_MM);
  stepper.setAcceleration(STEPPER_ACCEL_MM_S2 * STEPS_PER_MM);

  if (STEPPER_ENABLE_PIN >= 0) {
    pinMode(STEPPER_ENABLE_PIN, OUTPUT);
    digitalWrite(STEPPER_ENABLE_PIN, LOW);  // Activer le driver
  }

  // Initialiser LUT
  initLUT();

  // Homing automatique
  if (performHoming()) {
    isHomed = true;

    // Aller à la position de départ (note la plus aiguë)
    // Estimer position initiale (sera ajustée lors de la calibration)
    currentPositionMM = SLIDER_TRAVEL_MM;
    moveToPosition(currentPositionMM);

    Serial.println(F("✓ Initialisation terminée!"));
    Serial.println(F("\nCalibration commence par la note la plus AIGUË"));
    Serial.println(F("et descend progressivement vers les GRAVES.\n"));
    printHelp();
    printCurrentNoteStatus();
  } else {
    Serial.println(F("\n⚠ ERREUR: Échec du homing!"));
    Serial.println(F("Vérifiez le capteur fin de course et redémarrez.\n"));
  }

  if (LED_ENABLED) {
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
  // Lire commandes série
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }

  // Continuer mouvement si en cours
  stepper.run();
}
