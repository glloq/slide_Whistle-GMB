/*
 * MIDI Slide Whistle Controller - VERSION SOLÉNOÏDE + SERVO (Avancé)
 *
 * Contrôle un pipeau à coulisse via MIDI USB
 * - Moteur pas à pas pour le slider (position de la note)
 * - Solénoïde/valve avec PWM intelligent :
 *     → Ouverture : PWM 100% (12V) pendant 50ms
 *     → Maintien : PWM 47% (5.6V) → réduit chauffe
 * - Buffer/délai avant ouverture (moteur en position)
 * - Gestion legato (notes rapides sans coupure)
 * - Servomoteur pour moduler le débit selon la vélocité
 * - Pitch bend pour glissando
 * - Aftertouch pour vibrato
 *
 * Hardware requis :
 * - Arduino compatible MIDIUSB (Leonardo, Micro, Due, etc.)
 * - Driver moteur pas à pas (A4988, DRV8825, etc.)
 * - Moteur pas à pas NEMA
 * - Capteur fin de course
 * - Solénoïde/valve 12V (Pin PWM requise!)
 * - Servomoteur
 *
 * Bibliothèques requises :
 * - AccelStepper
 * - MIDIUSB
 * - Servo (incluse)
 */

#include "settings.h"
#include "StepperControl.h"
#include "MIDIHandler.h"
#include "AirControl.h"

// Instances globales
StepperControl stepper;
MIDIHandler midi;
AirControl air;

// États du système
enum SystemState {
  STATE_INIT,
  STATE_HOMING,
  STATE_READY,
  STATE_ERROR
};

SystemState currentState = STATE_INIT;

// ============================================================================
// CALLBACKS MIDI
// ============================================================================

// Callback appelé lors d'un Note On
void onMidiNoteOn(byte note, byte velocity) {
  if (currentState != STATE_READY) {
    return;
  }

  // Déplacer le slider vers la note
  stepper.moveToMidiNote(note);

  // Activer l'air avec vélocité
  air.startAir(velocity);
}

// Callback appelé lors d'un Note Off
void onMidiNoteOff(byte note) {
  if (currentState != STATE_READY) {
    return;
  }

  // Arrêter l'air
  air.stopAir();

  // Arrêter le vibrato si actif
  stepper.setVibrato(false, 0);
}

// Callback appelé lors d'un Pitch Bend
void onMidiPitchBend(float semitones) {
  if (currentState != STATE_READY) {
    return;
  }

  stepper.setPitchBend(semitones);
}

// Callback appelé lors d'un Aftertouch
void onMidiAftertouch(bool active, byte pressure) {
  if (currentState != STATE_READY) {
    return;
  }

  stepper.setVibrato(active, pressure);
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  #if DEBUG_MODE
  Serial.begin(SERIAL_BAUD_RATE);
  while (!Serial && millis() < 3000);
  Serial.println(F("\n========================================"));
  Serial.println(F("   MIDI Slide Whistle Controller"));
  Serial.println(F("   VERSION: SOLÉNOÏDE + SERVO"));
  Serial.println(F("   (Advanced: PWM + Legato)"));
  Serial.println(F("========================================\n"));
  #endif

  // LED de statut
  #if LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);
  #endif

  currentState = STATE_INIT;

  // Initialiser les modules
  #if DEBUG_MODE
  Serial.println(F("Initializing modules..."));
  #endif

  stepper.begin();
  midi.begin();
  air.begin();

  // Enregistrer les callbacks MIDI
  midi.setNoteOnCallback(onMidiNoteOn);
  midi.setNoteOffCallback(onMidiNoteOff);
  midi.setPitchBendCallback(onMidiPitchBend);
  midi.setAftertouchCallback(onMidiAftertouch);

  // Tests optionnels au démarrage
  #if DEBUG_MODE
  Serial.println(F("\nRunning self-tests..."));
  air.testSolenoid();
  air.testServo();
  #endif

  // Procédure de homing
  #if DEBUG_MODE
  Serial.println(F("\nStarting homing sequence..."));
  #endif

  currentState = STATE_HOMING;
  #if LED_ENABLED
  for (int i = 0; i < 5; i++) {
    digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    delay(100);
  }
  #endif

  bool homingSuccess = stepper.performHoming();

  if (homingSuccess) {
    currentState = STATE_READY;

    #if DEBUG_MODE
    Serial.println(F("\n========================================"));
    Serial.println(F("   System READY!"));
    Serial.println(F("   Waiting for MIDI input..."));
    Serial.println(F("========================================\n"));
    #endif

    #if LED_ENABLED
    digitalWrite(STATUS_LED_PIN, LOW);
    #endif

    // Signal sonore (bip court)
    air.startAir(100);
    delay(100);
    air.stopAir();
    delay(200);
    air.update();

  } else {
    currentState = STATE_ERROR;

    #if DEBUG_MODE
    Serial.println(F("\n========================================"));
    Serial.println(F("   ERROR: Homing failed!"));
    Serial.println(F("   Check endstop connection"));
    Serial.println(F("========================================\n"));
    #endif

    #if LED_ENABLED
    while (true) {
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
      delay(200);
    }
    #endif
  }
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
  if (currentState != STATE_READY) {
    return;
  }

  // Mettre à jour les modules
  midi.update();      // Lire les messages MIDI
  stepper.update();   // Mettre à jour le moteur (inclut vibrato)
  air.update();       // Gérer les délais de fermeture

  // Debug périodique
  #if DEBUG_MODE
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 5000) {
    lastDebugTime = millis();

    Serial.println(F("\n--- Status ---"));
    Serial.print(F("Position: "));
    Serial.print(stepper.getCurrentPositionMM());
    Serial.println(F(" mm"));
    Serial.print(F("Solenoid state: "));
    Serial.print(air.getStateName());
    Serial.print(F(" ("));
    Serial.print(air.isSolenoidOpen() ? "OPEN" : "CLOSED");
    Serial.println(F(")"));
    Serial.print(F("Last note: "));
    Serial.println(midi.getLastNote());
    Serial.print(F("Pitch bend: "));
    Serial.println(midi.getPitchBendValue());
    Serial.print(F("Aftertouch: "));
    Serial.println(midi.getAftertouchPressure());
    Serial.println();
  }
  #endif
}
