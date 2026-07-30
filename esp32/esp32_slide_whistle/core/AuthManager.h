/*
 * core/AuthManager.h — network authentication & abuse control (Section 15).
 *
 * The API must NOT let any device on the network move the slide, drive a pump,
 * open a valve, run a test, restore config, restart or reflash. This portable
 * manager provides the decision logic, unit-tested off-device:
 *   - an admin token GENERATED at first boot (never a fixed default) — #22
 *   - short-lived sessions with expiry and a connection cap
 *   - a Public/Protected criticality gate (panic stays public — safety)
 *   - an Origin allow-list (CSRF hardening)
 *   - a token-bucket rate limiter
 *
 * Entropy is injected by the caller (esp_random() on the ESP32; a seed in
 * tests) so the logic stays deterministic and testable.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_AUTHMANAGER_H
#define SWC_CORE_AUTHMANAGER_H

#include "Types.h"
#include "CommandQueue.h"   // CommandType
#include <string>
#include <cstdio>

namespace swc {

enum class Criticality : uint8_t { Public = 0, Protected };

// Endpoints/commands that touch hardware, config, restart or OTA are Protected.
// Panic is Public on purpose: stopping the machine must never need a token.
inline Criticality criticalityFor(CommandType t) {
    switch (t) {
        case CommandType::Panic: return Criticality::Public;
        default: return Criticality::Protected;   // NoteOn/Off/CC/Home/Jog/Test/ApplyConfig…
    }
}

class RateLimiter {
public:
    void configure(float capacity, float refillPerSec) { cap_ = capacity; refill_ = refillPerSec; tokens_ = capacity; }
    bool allow(uint32_t nowMs, float cost = 1.0f) {
        if (haveTime_) {
            float dt = float(elapsed_u32(nowMs, lastMs_)) * 1e-3f;
            tokens_ = tokens_ + dt * refill_; if (tokens_ > cap_) tokens_ = cap_;
        }
        haveTime_ = true; lastMs_ = nowMs;
        if (tokens_ >= cost) { tokens_ -= cost; return true; }
        return false;
    }
private:
    float cap_ = 20, refill_ = 5, tokens_ = 20;
    uint32_t lastMs_ = 0; bool haveTime_ = false;
};

class AuthManager {
public:
    static constexpr uint8_t MAX_SESSIONS = 4;

    void begin(uint32_t ttlMs = 30u * 60u * 1000u, uint8_t maxConnections = MAX_SESSIONS) {
        ttlMs_ = ttlMs; maxConn_ = maxConnections > MAX_SESSIONS ? MAX_SESSIONS : maxConnections;
        adminToken_.clear();
        for (auto& s : sessions_) s = Session{};
        limiter_.configure(20, 5);
        requireAuth_ = true;
        originStrict_ = false; allowedOrigin_.clear();
    }

    void setRequireAuth(bool on) { requireAuth_ = on; }
    bool requireAuth() const { return requireAuth_; }

    // --- admin token (first boot) ------------------------------------------
    void regenerateAdminToken(const uint32_t entropy[4]) { adminToken_ = hex(entropy, 4); }
    void setAdminToken(const std::string& t) { adminToken_ = t; }   // restore persisted token
    bool hasAdminToken() const { return !adminToken_.empty(); }
    const std::string& adminToken() const { return adminToken_; }
    bool checkAdmin(const std::string& t) const { return hasAdminToken() && ctEqual(t, adminToken_); }

    // AP password generated at first boot (alnum, avoids ambiguous chars).
    static std::string generateApPassword(const uint32_t entropy[2], uint8_t len = 10) {
        static const char* al = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::string out; uint64_t v = (uint64_t(entropy[0]) << 32) | entropy[1];
        for (uint8_t i = 0; i < len; ++i) { out.push_back(al[v & 31]); v = (v >> 5) | (v << 59); }
        return out;
    }

    // --- sessions ----------------------------------------------------------
    // Exchange the admin token for a session token (used by WS/REST clients).
    std::string openSession(const std::string& adminToken, const uint32_t entropy[2], uint32_t nowMs) {
        if (!checkAdmin(adminToken)) return "";
        pruneExpired(nowMs);
        if (activeSessions() >= maxConn_) return "";   // connection cap reached
        for (auto& s : sessions_) {
            if (!s.active) {
                s.active = true; s.token = hex(entropy, 2); s.lastMs = nowMs;
                return s.token;
            }
        }
        return "";
    }
    bool verifySession(const std::string& token, uint32_t nowMs) {
        pruneExpired(nowMs);
        for (auto& s : sessions_)
            if (s.active && ctEqual(s.token, token)) { s.lastMs = nowMs; return true; }
        return false;
    }
    void closeSession(const std::string& token) {
        for (auto& s : sessions_) if (s.active && ctEqual(s.token, token)) s = Session{};
    }
    uint8_t activeSessions() const {
        uint8_t n = 0; for (auto& s : sessions_) if (s.active) ++n; return n;
    }

    // --- the gate ----------------------------------------------------------
    // A request is authorized when it's Public, or when it presents a valid
    // admin token or session token (and auth is enabled).
    bool authorize(Criticality c, const std::string& token, uint32_t nowMs) {
        if (c == Criticality::Public) return true;
        if (!requireAuth_) return true;
        return checkAdmin(token) || verifySession(token, nowMs);
    }

    // --- Origin allow-list -------------------------------------------------
    void setAllowedOrigin(const std::string& o) { allowedOrigin_ = o; originStrict_ = !o.empty(); }
    bool originAllowed(const std::string& origin) const {
        if (!originStrict_) return true;         // not enforced
        return origin == allowedOrigin_;
    }

    // --- rate limiting -----------------------------------------------------
    bool allowRequest(uint32_t nowMs, float cost = 1.0f) { return limiter_.allow(nowMs, cost); }
    void configureRate(float capacity, float refillPerSec) { limiter_.configure(capacity, refillPerSec); }

private:
    struct Session { bool active = false; std::string token; uint32_t lastMs = 0; };

    void pruneExpired(uint32_t nowMs) {
        for (auto& s : sessions_)
            if (s.active && elapsed_u32(nowMs, s.lastMs) > ttlMs_) s = Session{};
    }
    static std::string hex(const uint32_t* w, int n) {
        std::string out; char b[9];
        for (int i = 0; i < n; ++i) { std::snprintf(b, sizeof(b), "%08x", w[i]); out += b; }
        return out;
    }
    // best-effort constant-time compare (length-independent branch on mismatch)
    static bool ctEqual(const std::string& a, const std::string& b) {
        if (a.size() != b.size() || a.empty()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
        return diff == 0;
    }

    std::string adminToken_;
    Session     sessions_[MAX_SESSIONS];
    uint32_t    ttlMs_ = 1800000;
    uint8_t     maxConn_ = MAX_SESSIONS;
    bool        requireAuth_ = true;
    bool        originStrict_ = false;
    std::string allowedOrigin_;
    RateLimiter limiter_;
};

} // namespace swc

#endif // SWC_CORE_AUTHMANAGER_H
