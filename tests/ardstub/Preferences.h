#pragma once
#include <Arduino.h>
class Preferences {
public:
    bool begin(const char*, bool = false) { return true; }
    void end() {}
    String   getString(const char*, const char* = "") { return String(""); }
    size_t   putString(const char*, const char*) { return 0; }
    uint32_t getUInt(const char*, uint32_t = 0) { return 0; }
    size_t   putUInt(const char*, uint32_t) { return 0; }
    bool     clear() { return true; }
};
