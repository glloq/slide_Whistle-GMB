/*
 * tests/test_auth.cpp — API envelope + AuthManager.
 * Covers Section 18 "Web/API": auth, forbidden commands, error responses,
 * and Section 15 network-security logic.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/ApiResponse.h"
#include "../esp32/esp32_slide_whistle/core/AuthManager.h"

using namespace swc;

static JsonValue parse(const std::string& s) { JsonValue v; jsonParse(s, v, nullptr); return v; }

TEST(api_envelope_ok_and_error) {
    JsonValue ok = parse(apiOk());
    CHECK(ok.bool_or("ok", false));
    JsonValue er = parse(apiErr("GPIO_CONFLICT", "GPIO22 already used", "instruments[1].air.pump.pin"));
    CHECK(!er.bool_or("ok", true));
    const JsonValue* e = er.find("error");
    CHECK(e && e->str_or("code", "") == "GPIO_CONFLICT");
    CHECK(e->str_or("field", "") == "instruments[1].air.pump.pin");
}

TEST(api_from_validation) {
    std::vector<ValidationIssue> issues;
    issues.push_back({"GPIO_STRAPPING", "gpio0 strapping", "x", Severity::Warning});
    // only warnings → ok envelope carrying warnings
    JsonValue r = parse(apiFromValidation(issues));
    CHECK(r.bool_or("ok", false));
    // add a hard error → error envelope
    issues.push_back({"GPIO_CONFLICT", "dup", "y", Severity::Error});
    JsonValue r2 = parse(apiFromValidation(issues));
    CHECK(!r2.bool_or("ok", true));
    CHECK(r2.find("error")->str_or("code", "") == "GPIO_CONFLICT");
}

TEST(auth_admin_token_generated_not_fixed) {
    AuthManager a; a.begin();
    CHECK(!a.hasAdminToken());
    uint32_t e1[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    a.regenerateAdminToken(e1);
    CHECK(a.hasAdminToken());
    std::string t1 = a.adminToken();
    // a different device/seed yields a different token (not a fixed default)
    AuthManager b; b.begin();
    uint32_t e2[4] = {0xdeadbeef, 0x0badf00d, 0xfeedface, 0x8badf00d};
    b.regenerateAdminToken(e2);
    CHECK(b.adminToken() != t1);
    CHECK_EQ((long)t1.size(), 32);
}

TEST(auth_gate_public_vs_protected) {
    AuthManager a; a.begin();
    uint32_t e[4] = {1,2,3,4}; a.regenerateAdminToken(e);
    // public (panic) always allowed, even with no token
    CHECK(a.authorize(Criticality::Public, "", 0));
    CHECK(criticalityFor(CommandType::Panic) == Criticality::Public);
    // protected denied without a token
    CHECK(!a.authorize(Criticality::Protected, "", 0));
    CHECK(!a.authorize(Criticality::Protected, "wrongtoken", 0));
    // protected allowed with the admin token
    CHECK(a.authorize(Criticality::Protected, a.adminToken(), 0));
    // hardware-moving commands are Protected
    CHECK(criticalityFor(CommandType::Home) == Criticality::Protected);
    CHECK(criticalityFor(CommandType::TestAir) == Criticality::Protected);
    CHECK(criticalityFor(CommandType::NoteOn) == Criticality::Protected);
}

TEST(auth_sessions_and_expiry) {
    AuthManager a; a.begin(/*ttlMs=*/1000, /*maxConn=*/2);
    uint32_t e[4] = {5,6,7,8}; a.regenerateAdminToken(e);
    uint32_t se[2] = {0xA, 0xB};
    std::string s = a.openSession(a.adminToken(), se, 0);
    CHECK(!s.empty());
    CHECK(a.authorize(Criticality::Protected, s, 100));   // session grants access
    CHECK(a.verifySession(s, 500));                        // refreshes last-use
    CHECK(a.verifySession(s, 1400));                       // still within ttl since refresh
    CHECK(!a.verifySession(s, 3000));                      // expired after idle > ttl
}

TEST(auth_bad_admin_token_no_session) {
    AuthManager a; a.begin();
    uint32_t e[4] = {1,1,1,1}; a.regenerateAdminToken(e);
    uint32_t se[2] = {2,2};
    CHECK(a.openSession("nope", se, 0).empty());           // wrong admin token → no session
}

TEST(auth_connection_cap) {
    AuthManager a; a.begin(60000, /*maxConn=*/2);
    uint32_t e[4] = {9,9,9,9}; a.regenerateAdminToken(e);
    uint32_t s1[2] = {1,1}, s2[2] = {2,2}, s3[2] = {3,3};
    CHECK(!a.openSession(a.adminToken(), s1, 0).empty());
    CHECK(!a.openSession(a.adminToken(), s2, 0).empty());
    CHECK(a.openSession(a.adminToken(), s3, 0).empty());   // 3rd refused (cap=2)
    CHECK_EQ((long)a.activeSessions(), 2);
}

TEST(auth_origin_allowlist) {
    AuthManager a; a.begin();
    CHECK(a.originAllowed("http://anything"));             // not enforced by default
    a.setAllowedOrigin("http://slidewhistle.local");
    CHECK(a.originAllowed("http://slidewhistle.local"));
    CHECK(!a.originAllowed("http://evil.example"));
}

TEST(auth_rate_limiter) {
    AuthManager a; a.begin();
    a.configureRate(/*capacity=*/3, /*refillPerSec=*/1);
    CHECK(a.allowRequest(0));
    CHECK(a.allowRequest(0));
    CHECK(a.allowRequest(0));
    CHECK(!a.allowRequest(0));      // bucket empty
    CHECK(a.allowRequest(1100));    // ~1 token refilled after 1.1 s
}

TEST(auth_can_disable_for_trusted_lan) {
    AuthManager a; a.begin();
    a.setRequireAuth(false);
    CHECK(a.authorize(Criticality::Protected, "", 0));     // opt-out honoured
    a.setRequireAuth(true);
    CHECK(!a.authorize(Criticality::Protected, "", 0));
}

TEST(ap_password_generated) {
    uint32_t e[2] = {0x12345678, 0x9abcdef0};
    std::string p = AuthManager::generateApPassword(e, 10);
    CHECK_EQ((long)p.size(), 10);
    uint32_t e2[2] = {0x0, 0x1};
    CHECK(AuthManager::generateApPassword(e2, 10) != p);   // seed-dependent
}

TEST(auth_per_client_rate_limit) {
    // Review #31: one client exhausting its bucket does not block another.
    AuthManager a; a.begin();
    a.configureRate(2, 0.0001);
    CHECK(a.allowRequestFor("clientA", 0));
    CHECK(a.allowRequestFor("clientA", 0));
    CHECK(!a.allowRequestFor("clientA", 0));   // A exhausted
    CHECK(a.allowRequestFor("clientB", 0));    // B has its own bucket
    CHECK(a.allowRequestFor("clientB", 0));
}
