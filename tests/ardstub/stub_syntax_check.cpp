/*
 * tests/ardstub/stub_syntax_check.cpp — compile the ESP32-guarded platform layer
 * against the stub above, so a typo in MainApp / EspGmb / WebServerAdapter is
 * caught by the fast native job instead of only by the PlatformIO build.
 *
 * This is a COMPILE check, not a behavioural test: the stub does nothing.
 */
#include "../../esp32/esp32_slide_whistle/core/platform/MainApp.h"

// Instantiate the template transports so their bodies are actually compiled
// (a template member that is never instantiated is barely checked).
template class swc::DinMidiPort<swc::MainApp::QUEUE_LEN>;
template class swc::MidiTransportBridge<swc::MainApp::QUEUE_LEN>;

HardwareSerial Serial;
EspClass ESP;
WiFiClass WiFi;
LittleFSClass LittleFS;

static swc::MainApp g_app;

int main() {
    (void)&g_app;
    return 0;
}
