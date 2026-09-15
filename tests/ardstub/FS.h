#pragma once
#include <Arduino.h>
class File {
public:
    explicit operator bool() const { return false; }
    size_t size() const { return 0; }
    int    available() { return 0; }
    int    read() { return -1; }
    size_t write(const uint8_t*, size_t) { return 0; }
    void   close() {}
};
class FS {
public:
    File open(const char*, const char* = "r") { return File(); }
    bool exists(const char*) { return false; }
    bool remove(const char*) { return true; }
    bool rename(const char*, const char*) { return true; }
};
