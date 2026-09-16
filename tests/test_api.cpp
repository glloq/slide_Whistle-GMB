/*
 * tests/test_api.cpp — portable ApiRouter request dispatch.
 * Covers Section 18 "Web/API": HTTP errors, max size, content-type, auth,
 * forbidden commands, transactional import, restart_required, enqueue-only.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/ApiRouter.h"
#include <map>

using namespace swc;

struct FakeFs2 : IConfigFs {
    std::map<std::string, std::string> files;
    bool read(const char* p, std::string& out) override { auto it=files.find(p); if(it==files.end())return false; out=it->second; return true; }
    bool write(const char* p, const std::string& d) override { files[p]=d; return true; }
    bool remove(const char* p) override { files.erase(p); return true; }
    bool exists(const char* p) override { return files.count(p)>0; }
    bool rename(const char* a, const char* b) override { auto it=files.find(a); if(it==files.end())return false; files[b]=it->second; files.erase(it); return true; }
};
struct CountEntropy : IEntropy { uint32_t n=1000; uint32_t next() override { return ++n; } };
struct StatusStub : IStatusSource { JsonValue statusJson() override { JsonValue v=JsonValue::makeObj(); v.set("state","ready"); return v; } };
struct RecSink : ICommandSink { std::vector<Command> cmds; bool full=false;
    bool push(const Command& c) override { if(full) return false; cmds.push_back(c); return true; } };

struct Rig {
    AuthManager auth; ConfigStore store; FakeFs2 fs; RuntimeConfig live;
    CountEntropy ent; StatusStub st; RecSink sink; ApiRouter api;
    std::string adminTok;
    void begin() {
        auth.begin(); uint32_t e[4]={1,2,3,4}; auth.regenerateAdminToken(e); adminTok=auth.adminToken();
        store.begin(&fs);
        live = defaultConfig(); store.save(live);
        api.begin(&auth, &store, &live, &sink, &ent, &st);
    }
    ApiReply req(const std::string& m, const std::string& p, const std::string& body="",
                 const std::string& tok="", const std::string& ct="application/json", const std::string& origin="") {
        ApiRequest r; r.method=m; r.path=p; r.body=body; r.token=tok; r.contentType=ct; r.origin=origin;
        return api.handle(r, now_++);
    }
    uint32_t now_ = 0;
    static JsonValue parse(const std::string& s){ JsonValue v; jsonParse(s,v,nullptr); return v; }
};

TEST(api_status_public) {
    Rig g; g.begin();
    ApiReply r = g.req("GET", "/api/v1/status");
    CHECK_EQ(r.status, 200);
    CHECK(Rig::parse(r.body).bool_or("ok", false));
}

TEST(api_protected_requires_auth) {
    Rig g; g.begin();
    ApiReply r = g.req("GET", "/api/v1/config");     // no token
    CHECK_EQ(r.status, 401);
    CHECK(Rig::parse(r.body).find("error")->str_or("code","") == "UNAUTHORIZED");
}

TEST(api_login_then_access) {
    Rig g; g.begin();
    std::string loginBody = std::string("{\"token\":\"") + g.adminTok + "\"}";
    ApiReply r = g.req("POST", "/api/v1/session", loginBody, "", "application/json", "");
    CHECK_EQ(r.status, 200);
    std::string session = Rig::parse(r.body).find("data")->str_or("session","");
    CHECK(!session.empty());
    ApiReply c = g.req("GET", "/api/v1/config", "", session);
    CHECK_EQ(c.status, 200);
}

TEST(api_login_bad_token) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/session", "{\"token\":\"wrong\"}");
    CHECK_EQ(r.status, 401);
}

TEST(api_body_too_large) {
    Rig g; g.begin(); g.api.setMaxBodyBytes(64);
    std::string big(200, 'x');
    ApiReply r = g.req("POST", "/api/v1/config", big, g.adminTok);
    CHECK_EQ(r.status, 413);
}

// Review #9 §6: the platform web adapter must be able to refuse an oversized
// upload BEFORE buffering it into heap. That decision is the shared
// httpBodyExceedsLimit() rule (fed the declared Content-Length), and the cap is
// exposed via maxBodyBytes(); both are exercised here so the boundary (a body
// exactly at the cap is allowed, one byte over is refused) can't silently drift.
TEST(api_body_limit_rule_and_accessor) {
    Rig g; g.begin(); g.api.setMaxBodyBytes(64);
    CHECK_EQ((int)g.api.maxBodyBytes(), 64);
    CHECK(!httpBodyExceedsLimit(0, 64));      // empty ok
    CHECK(!httpBodyExceedsLimit(64, 64));     // exactly at the cap ok
    CHECK(httpBodyExceedsLimit(65, 64));      // one over → refuse
    CHECK(httpBodyExceedsLimit(200000, 64));  // a huge declared length → refuse early
}

TEST(api_bad_content_type) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/config", "{}", g.adminTok, "text/plain");
    CHECK_EQ(r.status, 415);
}

TEST(api_origin_enforced) {
    Rig g; g.begin();
    g.auth.setAllowedOrigin("http://slide.local");
    // a NON-safety command from a bad Origin is refused (safety cmds are exempt)
    ApiReply bad = g.req("POST", "/api/v1/command",
                         "{\"type\":\"noteOn\",\"note\":60}", g.adminTok, "application/json", "http://evil");
    CHECK_EQ(bad.status, 403);
    ApiReply ok = g.req("POST", "/api/v1/command",
                        "{\"type\":\"noteOn\",\"note\":60}", g.adminTok, "application/json", "http://slide.local");
    CHECK_EQ(ok.status, 202);
    // but panic bypasses Origin (safety must always pass)
    ApiReply panic = g.req("POST", "/api/v1/command", "{\"type\":\"panic\"}", "", "application/json", "http://evil");
    CHECK_EQ(panic.status, 202);
}

TEST(api_command_panic_public_enqueues) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/command", "{\"type\":\"panic\"}");   // no token
    CHECK_EQ(r.status, 202);
    CHECK_EQ((long)g.sink.cmds.size(), 1);
    CHECK(g.sink.cmds[0].type == CommandType::Panic);
}

TEST(api_command_stamps_incrementing_seq) {
    Rig g; g.begin();
    ApiReply a = g.req("POST", "/api/v1/command", "{\"type\":\"panic\"}");
    ApiReply b = g.req("POST", "/api/v1/command", "{\"type\":\"panic\"}");
    CHECK_EQ(a.status, 202); CHECK_EQ(b.status, 202);
    // enqueued commands carry a monotonic seq (#3 §7) …
    CHECK(g.sink.cmds[0].seq >= 1u);
    CHECK_EQ(g.sink.cmds[1].seq, g.sink.cmds[0].seq + 1);
    // … which the client also gets back to match against /status lastAck.
    CHECK(Rig::parse(a.body).find("data")->num_or("seq", 0) >= 1.0);
}

TEST(api_command_rejects_out_of_range_fields) {
    Rig g; g.begin();
    // channel 256 would wrap to 0 (OMNI) if narrowed before validation (#4 §P1).
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"channel\":256,\"note\":60}", g.adminTok).status, 400);
    // note 300 would wrap to 44.
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":300}", g.adminTok).status, 400);
    // instrument 256 would wrap to 0.
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"home\",\"instrument\":256}", g.adminTok).status, 400);
    // testAir duration beyond int16 would wrap negative.
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"testAir\",\"instrument\":0,\"ms\":40000}", g.adminTok).status, 400);
    // a valid one still passes.
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"channel\":1,\"note\":60}", g.adminTok).status, 202);
}

TEST(api_command_protected_needs_auth) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/command", "{\"type\":\"home\",\"instrument\":0}");
    CHECK_EQ(r.status, 401);
    CHECK_EQ((long)g.sink.cmds.size(), 0);            // never enqueued without auth
    ApiReply ok = g.req("POST", "/api/v1/command", "{\"type\":\"home\",\"instrument\":0}", g.adminTok);
    CHECK_EQ(ok.status, 202);
    CHECK(g.sink.cmds.back().type == CommandType::Home);
}

TEST(api_command_noteon_enqueues_only) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/command",
                       "{\"type\":\"noteOn\",\"channel\":1,\"note\":60,\"velocity\":100}", g.adminTok);
    CHECK_EQ(r.status, 202);
    CHECK(g.sink.cmds.back().type == CommandType::NoteOn);
    CHECK_EQ(g.sink.cmds.back().a, 60);
    CHECK_EQ(g.sink.cmds.back().b, 100);
}

TEST(api_config_invalid_rejected) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/config", "{ not json", g.adminTok);
    CHECK_EQ(r.status, 400);
    CHECK(Rig::parse(r.body).find("error")->str_or("code","") == "CONFIG_INVALID");
}

TEST(api_config_gpio_conflict_rejected) {
    Rig g; g.begin();
    // build a config where two enabled instruments share a preset (same pins)
    RuntimeConfig c = defaultConfig(); c.instrumentCount = 2;
    c.instruments[0].enabled = true; applyPreset(c.instruments[0], PresetId::StepperSolenoidOnly);
    c.instruments[1].enabled = true; applyPreset(c.instruments[1], PresetId::StepperSolenoidOnly);
    std::string json = g.store.exportJson(c);
    ApiReply r = g.req("POST", "/api/v1/config", json, g.adminTok);
    CHECK_EQ(r.status, 400);
    CHECK(Rig::parse(r.body).find("error")->str_or("code","") == "GPIO_CONFLICT");
}

TEST(api_config_restart_required_flag) {
    Rig g; g.begin();
    // dynamic-only change (speed) → applied live, no restart
    RuntimeConfig dyn = g.live; dyn.instruments[0].motion.maxSpeedMmS += 10;
    ApiReply r1 = g.req("POST", "/api/v1/config", g.store.exportJson(dyn), g.adminTok);
    CHECK_EQ(r1.status, 200);
    CHECK(!Rig::parse(r1.body).find("data")->bool_or("restart_required", true));
    CHECK(!g.api.restartRequired());
    // hardware change (enable a stepper instrument + pins) → restart_required
    RuntimeConfig hw = g.live; hw.instruments[0].enabled = true;
    applyPreset(hw.instruments[0], PresetId::StepperSolenoidOnly);
    ApiReply r2 = g.req("POST", "/api/v1/config", g.store.exportJson(hw), g.adminTok);
    CHECK_EQ(r2.status, 200);
    CHECK(Rig::parse(r2.body).find("data")->bool_or("restart_required", false));
    CHECK(g.api.restartRequired());
}

TEST(api_preset_applies_and_persists) {
    Rig g; g.begin();
    ApiReply r = g.req("POST", "/api/v1/preset", "{\"index\":2,\"instrument\":0}", g.adminTok);
    CHECK_EQ(r.status, 200);
    // persisted config reloads with the stepper preset
    RuntimeConfig chk; g.store.load(chk);
    CHECK(chk.instruments[0].enabled);
    CHECK(chk.instruments[0].motion.type == SlideDriveType::StepDir);
}

TEST(api_factory_reset) {
    Rig g; g.begin();
    g.req("POST", "/api/v1/preset", "{\"index\":2,\"instrument\":0}", g.adminTok);
    ApiReply r = g.req("POST", "/api/v1/factory-reset", "{}", g.adminTok);
    CHECK_EQ(r.status, 200);
    CHECK(!g.live.instruments[0].enabled);   // back to safe default
}

// Review #4 §P1: a dynamic apply that can't be queued (queue full) becomes a
// REAL pending state that servicePending() retries — not a cosmetic flag.
TEST(api_dynamic_apply_pending_is_retried) {
    Rig g; g.begin();
    g.sink.full = true;                             // queue full
    std::string body = configToJson(g.live);        // valid, dynamic-only (no diff)
    ApiReply r = g.req("POST", "/api/v1/config", body, g.adminTok);
    CHECK_EQ(r.status, 200);
    CHECK(g.api.applyPending());                     // genuinely pending
    CHECK(g.sink.cmds.empty());                      // nothing applied yet
    // queue drains; the retry actually enqueues the ApplyDynamicConfig.
    g.sink.full = false;
    g.api.servicePending();
    CHECK(!g.api.applyPending());
    CHECK(!g.sink.cmds.empty());
    CHECK(g.sink.cmds.back().type == CommandType::ApplyDynamicConfig);
}

// Review #4 §P1: configNeedsRestart must flag network/auth/MIDI-transport and
// angle/flow-type/endstop-max/pump-count changes (applyDynamic can't do those),
// while a pure tuning change (transpose) stays dynamic.
TEST(config_needs_restart_covers_more_fields) {
    RuntimeConfig a = defaultConfig();
    CHECK(!configNeedsRestart(a, a));                       // identical
    { RuntimeConfig b = a; b.network.requireAuth = !a.network.requireAuth; CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.network.apEnabled = !a.network.apEnabled;     CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; std::snprintf(b.network.allowedOrigin, sizeof(b.network.allowedOrigin), "http://x"); CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.midi.ble = !a.midi.ble;        CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.angle.enabled = !a.instruments[0].air.angle.enabled; CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.flow.type = FlowControlType::FanPwm; CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.source.pumpCount = (uint8_t)(a.instruments[0].air.source.pumpCount + 1); CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.stepper.endstopMax.pin = 39; CHECK(configNeedsRestart(a, b)); }
    // transpose is a live tuning parameter — no restart.
    { RuntimeConfig b = a; b.midi.transpose = (int8_t)(a.midi.transpose + 1); CHECK(!configNeedsRestart(a, b)); }
}

// Review #7 §12: fields applyDynamic() cannot push to the built objects must
// force a restart — otherwise a "dynamic" apply would run against hardware
// constructed from the previous values.
TEST(config_needs_restart_for_unapplied_params) {
    RuntimeConfig a = defaultConfig();
    a.instrumentCount = 1; a.instruments[0].enabled = true;
    // --- these are NOT applied dynamically → must require restart ---
    { RuntimeConfig b = a; b.instruments[0].motion.travelMm += 5.0f;                 CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.stepper.stepsPerMm += 1.0f;       CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.stepper.invertDir ^= 1;           CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.stepper.homeTowardZero ^= 1;      CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.stepper.idleDisableMs += 100;     CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.servoA.trimUs += 10;              CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.servoA.cal[0].us += 5;            CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.source.spinUpMs += 10;               CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.source.pidKp += 0.01f;               CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.gate.servoMinUs += 10;               CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.flow.servoMaxUs += 10;               CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.sensor.staleTimeoutMs += 50;         CHECK(configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].watchdogMs += 100;                       CHECK(configNeedsRestart(a, b)); }
    // --- genuinely dynamic fields stay dynamic → NO restart ---
    { RuntimeConfig b = a; b.instruments[0].motion.maxSpeedMmS += 5.0f;              CHECK(!configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].motion.softMaxMm -= 1.0f;                CHECK(!configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.flow.nominal = (uint8_t)(a.instruments[0].air.flow.nominal + 1); CHECK(!configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.valveOpenTimeoutMs += 100;           CHECK(!configNeedsRestart(a, b)); }
    { RuntimeConfig b = a; b.instruments[0].air.minNoteMs += 10;                     CHECK(!configNeedsRestart(a, b)); }
}

// Review #5 §22: POST /config must reject a wrapped config whose integrity
// checksum no longer matches its content (the normal path checked only dec.ok).
TEST(api_config_rejects_bad_checksum) {
    Rig g; g.begin();
    std::string body = configToJson(g.live);      // wrapped {checksum, config}
    // Tamper the CONTENT (valid value) so the recomputed checksum diverges from
    // the stored one — an unwrapped/consistent config would still pass.
    auto pos = body.find("\"transpose\":0");
    CHECK(pos != std::string::npos);
    body.replace(pos, std::string("\"transpose\":0").size(), "\"transpose\":5");
    ApiReply r = g.req("POST", "/api/v1/config", body, g.adminTok);
    CHECK_EQ(r.status, 400);
    CHECK(Rig::parse(r.body).find("error")->str_or("code", "") == "BAD_CHECKSUM");
}

TEST(api_restart_flag) {
    Rig g; g.begin();
    CHECK(!g.api.restartRequested());
    ApiReply r = g.req("POST", "/api/v1/restart", "{}", g.adminTok);
    CHECK_EQ(r.status, 200);
    CHECK(g.api.restartRequested());
}

TEST(api_unknown_route_404) {
    Rig g; g.begin();
    ApiReply r = g.req("GET", "/api/v1/nope");
    CHECK_EQ(r.status, 404);
}

// Review #4 network security: an attacker cannot escape the rate limit by
// rotating a fresh (invalid) token per request — unverified tokens share one
// bucket instead of each minting its own.
TEST(api_rate_limit_not_bypassed_by_fake_tokens) {
    Rig g; g.begin();
    g.auth.configureRate(2, 0.0001);   // ~2 tokens, negligible refill
    // Two requests with distinct bogus tokens consume the shared unauth budget
    // (invalid token → 401, but the rate bucket is charged first).
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":60}", "faketok1").status, 401);
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":60}", "faketok2").status, 401);
    // A third distinct fake token is RATE-LIMITED, not handed a fresh bucket.
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":60}", "faketok3").status, 429);
}

// Review #7: safety commands bypass the rate limiter; notes do not.
TEST(api_rate_limit_exempts_safety) {
    Rig g; g.begin();
    g.auth.configureRate(2, 0.0001);   // ~2 tokens, negligible refill
    // two protected notes consume the budget
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":60}", g.adminTok).status, 202);
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":61}", g.adminTok).status, 202);
    // third note is rate-limited
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOn\",\"note\":62}", g.adminTok).status, 429);
    // but a Note Off and a Panic still get through (no token needed either)
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"noteOff\",\"note\":60}").status, 202);
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"panic\"}").status, 202);
    // All Sound Off (cc 120) too
    CHECK_EQ(g.req("POST", "/api/v1/command", "{\"type\":\"cc\",\"a\":120,\"b\":0}").status, 202);
}
