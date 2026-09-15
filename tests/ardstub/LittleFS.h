#pragma once
#include <FS.h>
class LittleFSClass : public FS {
public:
    bool begin(bool = false) { return true; }
    bool format() { return true; }
};
extern LittleFSClass LittleFS;
