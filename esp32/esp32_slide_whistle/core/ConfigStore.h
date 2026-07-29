/*
 * core/ConfigStore.h — atomic, recoverable configuration persistence.
 *
 * Filesystem access is abstracted behind IConfigFs so the save/restore logic
 * (atomic temp-file write, validate-before-swap, previous-version backup,
 * corruption recovery, factory reset) is unit-tested natively. A LittleFS
 * backend is provided for the ESP32 (guarded).
 *
 * Save flow (Section 10):
 *   1. serialize + checksum
 *   2. write to /config.tmp
 *   3. re-read /config.tmp and validate it decodes  (never swap junk in)
 *   4. copy current /config.json to /config.bak
 *   5. atomically replace /config.json with /config.tmp
 *
 * Load flow: try /config.json → on parse/checksum failure fall back to
 * /config.bak → on failure return the collision-free default (never brick).
 *
 * Status: IMPLEMENTED · store logic TESTED IN SOFTWARE ·
 *         LittleFS backend NOT TESTED — REQUIRES HARDWARE
 */
#ifndef SWC_CORE_CONFIGSTORE_H
#define SWC_CORE_CONFIGSTORE_H

#include "ConfigCodec.h"

namespace swc {

class IConfigFs {
public:
    virtual bool read(const char* path, std::string& out) = 0;
    virtual bool write(const char* path, const std::string& data) = 0;
    virtual bool remove(const char* path) = 0;
    virtual bool exists(const char* path) = 0;
    virtual bool rename(const char* from, const char* to) = 0;
    virtual ~IConfigFs() = default;
};

enum class LoadOutcome : uint8_t { Primary, Backup, Default };

class ConfigStore {
public:
    static constexpr const char* MAIN = "/config.json";
    static constexpr const char* TMP  = "/config.tmp";
    static constexpr const char* BAK  = "/config.bak";

    void begin(IConfigFs* fs) { fs_ = fs; }

    // Returns false only if the serialized config fails to re-decode (bug/OOM);
    // in that case the existing config is left untouched.
    bool save(const RuntimeConfig& c) {
        if (!fs_) return false;
        std::string data = configToJson(c);
        if (!fs_->write(TMP, data)) return false;
        // validate the temp file decodes before swapping it in
        std::string back; RuntimeConfig probe;
        if (!fs_->read(TMP, back)) return false;
        if (!configFromJson(back, probe).ok) { fs_->remove(TMP); return false; }
        if (fs_->exists(MAIN)) {
            std::string cur;
            if (fs_->read(MAIN, cur)) { fs_->remove(BAK); fs_->write(BAK, cur); }
        }
        fs_->remove(MAIN);
        return fs_->rename(TMP, MAIN);
    }

    LoadOutcome load(RuntimeConfig& out) {
        if (fs_) {
            std::string data;
            if (fs_->read(MAIN, data)) { auto r = configFromJson(data, out); if (r.ok && r.checksumOk) return LoadOutcome::Primary; }
            if (fs_->read(BAK, data))  { auto r = configFromJson(data, out); if (r.ok && r.checksumOk) return LoadOutcome::Backup; }
        }
        out = defaultConfig();
        return LoadOutcome::Default;
    }

    // Import from an external JSON string transactionally: validate first, then
    // (only on success) persist. Never applies a partially-decoded config.
    bool importJson(const std::string& json, RuntimeConfig& out, std::string& err) {
        RuntimeConfig probe;
        ConfigDecodeResult r = configFromJson(json, probe);
        if (!r.ok) { err = r.error; return false; }
        if (!save(probe)) { err = "persist failed"; return false; }
        out = probe;
        return true;
    }

    std::string exportJson(const RuntimeConfig& c) { return configToJson(c); }

    bool factoryReset(RuntimeConfig& out) {
        out = defaultConfig();
        if (fs_) { fs_->remove(MAIN); fs_->remove(BAK); }   // discard everything stale
        return save(out);                                    // MAIN gone → no backup created
    }

private:
    IConfigFs* fs_ = nullptr;
};

// --- ESP32 LittleFS backend (guarded) --------------------------------------
#if defined(ARDUINO)
} // namespace swc
#include <LittleFS.h>
namespace swc {
class LittleFsConfigFs : public IConfigFs {
public:
    bool begin(bool formatOnFail = true) { return LittleFS.begin(formatOnFail); }
    bool read(const char* path, std::string& out) override {
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        out.clear(); out.reserve(f.size());
        while (f.available()) out.push_back((char)f.read());
        f.close(); return true;
    }
    bool write(const char* path, const std::string& data) override {
        File f = LittleFS.open(path, "w");
        if (!f) return false;
        size_t n = f.write((const uint8_t*)data.data(), data.size());
        f.close(); return n == data.size();
    }
    bool remove(const char* path) override { return LittleFS.exists(path) ? LittleFS.remove(path) : true; }
    bool exists(const char* path) override { return LittleFS.exists(path); }
    bool rename(const char* from, const char* to) override { return LittleFS.rename(from, to); }
};
#endif // ARDUINO

} // namespace swc

#endif // SWC_CORE_CONFIGSTORE_H
