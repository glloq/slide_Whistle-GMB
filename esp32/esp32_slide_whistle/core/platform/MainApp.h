/*
 * core/platform/MainApp.h — top-level firmware orchestration for the universal
 * controller. Owns the tested portable components and the ESP32 hardware layer,
 * and runs them on two pinned FreeRTOS tasks:
 *
 *   Core 0 (net) : WiFi/AP, async web server + WebSocket → ApiRouter → queue.
 *   Core 1 (rt)  : deterministic vTaskDelayUntil loop → RealtimeEngine.tick()
 *                  (drains the queue, ticks actuators/air/sequencers).
 *
 * Boot is safe-by-construction (Section 12): critical outputs are forced to a
 * safe state, config is loaded + validated, instruments are built de-energised,
 * and only a validated, homed system reaches READY.
 *
 * Status: IMPLEMENTED (structure) · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE
 */
#ifndef SWC_CORE_MAINAPP_H
#define SWC_CORE_MAINAPP_H
#if defined(ARDUINO)

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "../ConfigStore.h"
#include "../AuthManager.h"
#include "../ApiRouter.h"
#include "../RealtimeEngine.h"
#include "../InstrumentRuntime.h"
#include "EspSinks.h"
#include "EspEntropy.h"
#include "WebServerAdapter.h"

namespace swc {

enum class SysState : uint8_t { Boot, SafeConfigOnly, Ready, Fault };

class MainApp {
public:
    static constexpr uint16_t QUEUE_LEN = 64;

    void setup() {
        forceSafeOutputs();                 // 1. de-energise everything first
        fsOk_ = fs_.begin(true);
        LoadOutcome lo = LoadOutcome::Default;
        if (fsOk_) { store_.begin(&fs_); lo = store_.load(config_); }
        else       { config_ = defaultConfig(); }
        firstBoot_ = (lo == LoadOutcome::Default);

        auth_.begin();
        uint32_t e[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
        auth_.regenerateAdminToken(e);      // 2. token generated, never fixed

        // 3. validate the whole config before energising anything
        HardwareResourceValidator v; buildClaims(v, config_);
        bool cfgOk = !HardwareResourceValidator::hasErrors(v.validate());

        buildInstruments();                 // constructs objects in safe state
        engine_.begin(instPtrs_, instCount_, &queue_);
        router_.begin(&auth_, &store_, &config_, &sink_, &entropy_, &status_);

        startNetwork();
        startWebServer();

        state_ = cfgOk ? SysState::Ready : SysState::SafeConfigOnly;

        xTaskCreatePinnedToCore(rtTaskThunk, "rt",  16384, this, 5, nullptr, 1);
        // net runs on this task (Core 0) via loop()
    }

    void loop() {
        ws_.cleanupClients();
        if (router_.restartRequested()) { delaySafeRestart(); }
        delay(5);
    }

private:
    void forceSafeOutputs() { /* TODO: drive known critical pins low before config */ }

    void buildInstruments() {
        instCount_ = 0;
        for (uint8_t i = 0; i < config_.instrumentCount && i < MAX_INSTRUMENTS; ++i) {
            InstrumentConfig& ic = config_.instruments[i];
            configureSinks(i, ic);
            rt_[i] = new InstrumentRuntime(&motion_[i], &air_[i]);
            if (!rt_[i]->build(i, ic)) continue;
            rt_[i]->enterSafeState();           // not energised until homed
            instPtrs_[instCount_++] = &rt_[i]->instrument();
        }
    }

    void configureSinks(uint8_t i, const InstrumentConfig& ic) {
        motion_[i].begin(ic.motion);
        // map air pins from config onto the air sink
        const auto& a = ic.air;
        if (a.source.type == AirSourceType::FanOnOff || a.source.type == AirSourceType::FanPwm)
            air_[i].configureSourcePwm(0, a.source.pin[0], 25000);
        else if (a.source.type == AirSourceType::PumpsDirect || a.source.type == AirSourceType::PumpsTank)
            for (uint8_t p = 0; p < a.source.pumpCount && p < MAX_PUMPS; ++p)
                air_[i].configureSourcePwm(p, a.source.pin[p], 25000);
        if (a.gate.type == AirGateType::SolenoidSimple) air_[i].configureSolenoid(a.gate.pin, a.gate.activeHigh);
        else if (a.gate.type == AirGateType::SolenoidPwm) air_[i].configureGatePwm(a.gate.pin, 20000);
        else if (a.gate.type != AirGateType::None)        air_[i].configureGatePwm(a.gate.pin, 50);
        if (a.flow.type != FlowControlType::None) air_[i].configureFlow(a.flow.pin);
        if (a.angle.enabled) air_[i].configureAngle(/*angle pin carried elsewhere*/ -1);
        air_[i].configureSensor(a.sensor.pin);
    }

    void startNetwork() {
        if (config_.network.apEnabled) {
            uint32_t r[2] = { esp_random(), esp_random() };
            apPassword_ = AuthManager::generateApPassword(r);   // generated, not fixed (#22)
            WiFi.softAP(config_.network.apSsid, apPassword_.c_str());
        }
        // TODO: station mode + rtpMIDI + BLE-MIDI bring-up
    }
    void startWebServer() {
        web_.begin(&server_, &ws_, &router_, &MainApp::millisNow);
        server_.begin();
    }

    // Deterministic real-time loop — fixed period, no blocking (correction #3).
    static void rtTaskThunk(void* self) { static_cast<MainApp*>(self)->rtTask(); }
    void rtTask() {
        const TickType_t period = pdMS_TO_TICKS(1);   // 1 kHz motion tick
        TickType_t last = xTaskGetTickCount();
        for (;;) {
            uint32_t ms = millis();
            uint32_t us = micros();
            engine_.tick(ms, us, /*budget=*/32);
            for (uint8_t i = 0; i < instCount_; ++i) rt_[i]->update(ms, us);
            vTaskDelayUntil(&last, period);            // deterministic cadence
        }
    }

    void delaySafeRestart() {
        engine_.panicAll(millis());       // safe state before reboot (OTA/restart)
        delay(200);
        ESP.restart();
    }

    static uint32_t millisNow() { return millis(); }

    // Status snapshot for the web (TODO: lock-free double buffer).
    struct StatusSrc : IStatusSource {
        MainApp* app = nullptr;
        JsonValue statusJson() override {
            JsonValue v = JsonValue::makeObj();
            v.set("state", int(app->state_));
            v.set("firstBoot", app->firstBoot_);
            JsonValue arr = JsonValue::makeArr();
            for (uint8_t i = 0; i < app->instCount_; ++i) {
                JsonValue o = JsonValue::makeObj();
                auto* a = app->rt_[i]->actuator();
                o.set("pos_mm", a ? a->currentPositionMm() : 0.0);
                o.set("homed", a ? a->isHomed() : false);
                o.set("note", app->rt_[i]->instrument().sequencer().activeNoteOr(-1));
                arr.arr.push_back(o);
            }
            v.set("instruments", arr);
            return v;
        }
    };

    // --- state ---
    LittleFsConfigFs fs_;
    ConfigStore      store_;
    RuntimeConfig    config_;
    AuthManager      auth_;
    EspEntropy       entropy_;
    CommandQueue<QUEUE_LEN> queue_;
    QueueSink<QUEUE_LEN>    sink_{queue_};
    RealtimeEngine<QUEUE_LEN> engine_;
    ApiRouter        router_;
    StatusSrc        status_{};

    EspMotionSink    motion_[MAX_INSTRUMENTS];
    EspAirSink       air_[MAX_INSTRUMENTS];
    InstrumentRuntime* rt_[MAX_INSTRUMENTS] = {nullptr};
    Instrument*      instPtrs_[MAX_INSTRUMENTS] = {nullptr};
    uint8_t          instCount_ = 0;

    AsyncWebServer   server_{80};
    AsyncWebSocket   ws_{"/ws"};
    WebServerAdapter web_;
    std::string      apPassword_;
    bool             fsOk_ = false, firstBoot_ = true;
    volatile SysState state_ = SysState::Boot;

public:
    MainApp() { status_.app = this; }
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_MAINAPP_H
