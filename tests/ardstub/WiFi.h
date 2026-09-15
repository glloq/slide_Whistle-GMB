#pragma once
#include <Arduino.h>
struct WiFiClass {
    bool softAP(const char*, const char* = nullptr) { return true; }
    void begin(const char*, const char*) {}
    int  status() { return 0; }
};
extern WiFiClass WiFi;
