/*
 * MIDI Slide Whistle Controller
 *
 * Contrôle un pipeau à coulisse via MIDI USB
 * - Moteur pas à pas pour le slider (position de la note)
 * - Ventilateur/solénoïde pour l'air
 * - Servomoteur optionnel pour contrôler le débit
 *
 * Hardware requis :
 * - Arduino compatible MIDIUSB (Leonardo, Micro, Due, etc.)
 * - Driver moteur pas à pas (A4988, DRV8825, etc.)
 * - Moteur pas à pas NEMA
 * - Capteur fin de course
 * - Ventilateur ou solénoïde
 * - Servomoteur (optionnel)
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
    return; // Ignorer si le système n'est pas prêt
  }

  // Déplacer le slider vers la note
  stepper.moveToMidiNote(note);

  // Activer l'air
  air.startAir(velocity);
}

// Callback appelé lors d'un Note Off
void onMidiNoteOff(byte note) {
  if (currentState != STATE_READY) {
    return;
  }

  // Arrêter l'air
  air.stopAir();

  // Option : retourner à la position centrale après la note
  // stepper.moveToCenter();
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  #if DEBUG_MODE
  Serial.begin(SERIAL_BAUD_RATE);
  while (!Serial && millis() < 3000); // Attendre Serial max 3s
  Serial.println(F("\n========================================"));
  Serial.println(F("   MIDI Slide Whistle Controller"));
  Serial.println(F("========================================\n"));
  #endif

  // LED de statut
  #if LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH); // Allumer pendant l'init
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

  // Tests optionnels au démarrage
  #if DEBUG_MODE
  Serial.println(F("\nRunning self-tests..."));
  air.testFan();
  #if SERVO_ENABLED
  air.testServo();
  #endif
  #endif

  // Procédure de homing
  #if DEBUG_MODE
  Serial.println(F("\nStarting homing sequence..."));
  #endif

  currentState = STATE_HOMING;
  #if LED_ENABLED
  // Clignoter pendant le homing
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
    digitalWrite(STATUS_LED_PIN, LOW); // Éteindre la LED
    #endif

    // Signal sonore optionnel (bip court)
    air.setFanState(true);
    delay(100);
    air.setFanState(false);

  } else {
    currentState = STATE_ERROR;

    #if DEBUG_MODE
    Serial.println(F("\n========================================"));
    Serial.println(F("   ERROR: Homing failed!"));
    Serial.println(F("   Check endstop connection"));
    Serial.println(F("========================================\n"));
    #endif

    #if LED_ENABLED
    // Clignoter rapidement en cas d'erreur
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
  // Vérifier l'état du système
  if (currentState != STATE_READY) {
    return;
  }

  // Mettre à jour les modules
  midi.update();      // Lire les messages MIDI
  stepper.update();   // Mettre à jour le moteur
  air.update();       // Gérer les délais d'arrêt de l'air

  // Affichage de debug périodique (optionnel)
  #if DEBUG_MODE
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 5000) { // Toutes les 5 secondes
    lastDebugTime = millis();

    Serial.println(F("\n--- Status ---"));
    Serial.print(F("Stepper position: "));
    Serial.println(stepper.getCurrentPosition());
    Serial.print(F("Fan active: "));
    Serial.println(air.isFanActive() ? "YES" : "NO");
    Serial.print(F("Last MIDI note: "));
    Serial.println(midi.getLastNote());
    Serial.println();
  }
  #endif
}

// ============================================================================
// FONCTIONS UTILITAIRES (optionnel)
// ============================================================================

// Reset du système (peut être appelé via Serial ou bouton)
void resetSystem() {
  #if DEBUG_MODE
  Serial.println(F("Resetting system..."));
  #endif

  air.setFanState(false);
  stepper.resetHoming();
  currentState = STATE_INIT;

  // Redémarrer le setup
  setup();
}
