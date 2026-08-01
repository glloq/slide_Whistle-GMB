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
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include "../ConfigStore.h"

#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 1
#endif
#include "../AuthManager.h"
#include "../ApiRouter.h"
#include "../RealtimeEngine.h"
#include "../InstrumentRuntime.h"
#include "../StatusSnapshot.h"
#include "EspSinks.h"
#include "EspEntropy.h"
#include "WebServerAdapter.h"

namespace swc {

enum class SysState : uint8_t { Boot, SafeConfigOnly, Initializing, NeedsHoming, Homing, Ready, Fault };

class MainApp {
public:
    static constexpr uint16_t QUEUE_LEN = 64;

    void setup() {
#if DEBUG_SERIAL
        Serial.begin(115200); delay(200);
#endif
        // The ESP32-S3 exposes 8 LEDC channels, not the 16 of a classic WROOM —
        // cap the allocator so we never hand out a channel the chip lacks
        // (review #3 §9.3). Only matters on the 2.x LEDC API path.
#if defined(BOARD_ESP32_S3)
        LedcAllocator::global().setCapacity(8);
#endif
        fsOk_ = mountFs();
        LoadOutcome lo = LoadOutcome::Default;
        if (fsOk_) { store_.begin(&fs_); lo = store_.load(config_); }
        else       { config_ = defaultConfig(); }
        firstBoot_ = (lo == LoadOutcome::Default);

        // 1. drive every critical output to its inactive state BEFORE anything
        //    else, even if the config turns out invalid (#8).
        forceSafeOutputs(config_);

        // 2. admin secret: persisted in NVS so it survives reboots and can be
        //    shown; generated only on first boot, never a fixed default (#5/#22).
        auth_.begin();
        auth_.setRequireAuth(config_.network.requireAuth);   // honour the config field (review #33)
        if (config_.network.allowedOrigin[0])                // enforce Origin only if set (#32)
            auth_.setAllowedOrigin(config_.network.allowedOrigin);
        loadOrCreateAdminToken();

        // 3. validate the whole config before energising anything
        HardwareResourceValidator v; buildClaims(v, config_);
        bool cfgOk = !HardwareResourceValidator::hasErrors(v.validate());

        // In FS recovery, never energise actuators — serve the config/recovery UI
        // only until the operator explicitly reformats (review #5 §23).
        if (fsRecovery_) cfgOk = false;

        // 4. build ONLY on a valid config; invalid → serve config UI only (#8)
        uint8_t enabledCount = 0;
        for (uint8_t i = 0; i < config_.instrumentCount && i < MAX_INSTRUMENTS; ++i)
            if (config_.instruments[i].enabled) ++enabledCount;
        if (cfgOk) buildInstruments();
        else       instCount_ = 0;
        // Ready requires at least one active instrument AND that EVERY enabled
        // instrument actually built — a config of only disabled instruments, or a
        // partial build failure, must stay SafeConfigOnly, never reach Ready
        // (review #6 §6/§5).
        bool runnable = cfgOk && instCount_ > 0 && instCount_ == enabledCount;
        state_ = runnable ? SysState::Initializing : SysState::SafeConfigOnly;

        engine_.begin(instPtrs_, instCount_, &queue_);
        engine_.setLiveConfig(&config_);    // so ApplyDynamicConfig actually applies (#5)
        engine_.setTranspose(config_.midi.transpose);   // global transpose actually applied (#5 §12)
        router_.begin(&auth_, &store_, &config_, &sink_, &entropy_, &status_);

        startNetwork();

        // 5. runnable config → home before declaring READY (#10). If not
        // runnable we stay SafeConfigOnly (no actuators) — never home nothing.
        if (state_ == SysState::Initializing) { requestHomingAll(); state_ = SysState::NeedsHoming; }

        // Bring up the RT task (sole actuator owner) BEFORE opening the network
        // server, so a WebSocket/HTTP command cannot be enqueued and observed
        // before the command gate is being managed (review #7 §4). The engine
        // also starts blocked, so even a command that slips in early is refused
        // until the lifecycle reaches Ready.
        // If the RT task cannot be created, NOTHING drives the actuators to a safe
        // state or manages the command gate — refuse Ready and fall back to
        // config-only so the outputs stay in the safe state forced above (#9 §3.8).
        if (xTaskCreatePinnedToCore(rtTaskThunk, "rt", 16384, this, 5, nullptr, 1) != pdPASS)
            state_ = SysState::SafeConfigOnly;

        startWebServer();
        printCredentials();                 // Serial: AP password + admin token (#5)
    }

    void loop() {
        ws_.cleanupClients();
        router_.servicePending();   // retry a queued-full dynamic apply (#4 §P1)
        // Restart is owned by the RT task: enqueue a SafeRestart command, then
        // wait for the RT task to reach a safe state before rebooting — the
        // network task never touches the actuators directly (review #4 §P0).
        // Only latch "enqueued" when push() actually succeeds; retry otherwise so
        // a momentarily-full queue can't strand the restart forever. SafeRestart
        // is a priority command, so it is never dropped for lack of space
        // (review #5 §P0.5).
        if (router_.restartRequested() && !safeRestartEnqueued_) {
            Command c{}; c.type = CommandType::SafeRestart;
            if (queue_.push(c)) safeRestartEnqueued_ = true;
        }
        if (safeRestartEnqueued_ && engine_.safeRestartDone()) {
            delay(200);          // let the safe-state writes settle / clients flush
            ESP.restart();
        }
        delay(5);
    }

private:
    // Mount LittleFS WITHOUT ever auto-formatting: a transient mount error must
    // never wipe config/backup/UI (review #30, hardened per #5 §23). On failure
    // we enter recovery — run from defaults in RAM with NO actuators — and wait
    // for an explicit operator format command; we never call fs_.begin(true).
    bool mountFs() {
        for (int i = 0; i < 3; ++i) { if (fs_.begin(false)) return true; delay(50); }
        fsRecovery_ = true;                  // surfaced in status; operator is warned
        return false;
    }

    // Drive every GPIO-backed actuator/air output to a known-inactive level
    // BEFORE any driver is configured, so a floating pin can't energise a pump,
    // open a valve, or step a motor at boot (#8, extended per #3 §8 to the
    // stepper enable/step/dir and the servo/flow/angle pins). Runs on the loaded
    // config regardless of validity. PCA9685 channels come up off after the
    // chip's power-on reset and stay off until we init I2C, so only the direct
    // GPIO (backend == Gpio) pins need forcing here.
    void forceSafeOutputs(const RuntimeConfig& c) {
        // Validate each pin NUMBER against the board profile before ever calling
        // pinMode()/digitalWrite() on it, so a corrupt config cannot drive a
        // flash-reserved, out-of-range or input-only pin while forcing outputs
        // safe (review #9 §3.7). This is a per-pin guard, not the full validator —
        // the full HardwareResourceValidator still runs before anything is built.
        const BoardProfile b = (c.device.board == BoardKind::Esp32S3)
                                   ? BoardProfile::s3() : BoardProfile::wroom();
        for (uint8_t i = 0; i < c.instrumentCount && i < MAX_INSTRUMENTS; ++i) {
            const InstrumentConfig& ic = c.instruments[i];
            const SlideMotionConfig& m = ic.motion;
            // Stepper: hold the driver DISABLED and keep step/dir quiet.
            if (m.type == SlideDriveType::StepDir) {
                safeDisableStepper(b, m.stepper.enablePin, m.stepper.enableActiveHigh);
                safeLow(b, m.stepper.stepPin, true);
                safeLow(b, m.stepper.dirPin, true);
            }
            // Servos on direct GPIO: no pulse train = no commanded motion.
            if (m.type == SlideDriveType::SingleServo || m.type == SlideDriveType::DualServo) {
                if (m.servoA.backend == PwmBackend::Gpio) safeLow(b, m.servoA.pin, true);
                if (m.servoBEnabled && m.servoB.backend == PwmBackend::Gpio) safeLow(b, m.servoB.pin, true);
            }
            const AirConfig& a = ic.air;
            safeLow(b, a.gate.pin, a.gate.activeHigh);
            for (uint8_t p = 0; p < MAX_PUMPS; ++p) safeLow(b, a.source.pin[p], true);
            if (a.flow.backend == PwmBackend::Gpio)  safeLow(b, a.flow.pin, true);
            if (a.angle.enabled && a.angle.backend == PwmBackend::Gpio) safeLow(b, a.angle.pin, true);
        }
    }
    // A pin may be safely driven as an output only if it exists on the board and
    // is neither flash-reserved nor input-only (review #9 §3.7).
    static bool drivableOutput(const BoardProfile& b, int pin) {
        return pin >= 0 && pin <= b.maxGpio && !b.isFlash(pin) && !b.isInputOnly(pin);
    }
    static void safeLow(const BoardProfile& b, int pin, bool activeHigh) {
        if (!drivableOutput(b, pin)) return;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, activeHigh ? LOW : HIGH);   // inactive
    }
    static void safeDisableStepper(const BoardProfile& b, int enablePin, bool enableActiveHigh) {
        if (!drivableOutput(b, enablePin)) return;
        pinMode(enablePin, OUTPUT);
        // Inactive = driver DISABLED (opposite of the enable-active polarity).
        digitalWrite(enablePin, enableActiveHigh ? LOW : HIGH);
    }

    void buildInstruments() {
        instCount_ = 0;
        for (uint8_t i = 0; i < config_.instrumentCount && i < MAX_INSTRUMENTS; ++i) {
            InstrumentConfig& ic = config_.instruments[i];
            if (!ic.enabled) continue;          // ignore disabled instruments (#8)
            // A hardware-init failure (servo/pump LEDC attach, or a missing STEP
            // timer) must NOT reach Ready: skip the instrument so instCount_ ends
            // below enabledCount and setup() stays SafeConfigOnly (review #9 §3.8).
            if (!configureSinks(i, ic)) continue;
            rt_[i] = new InstrumentRuntime(&motion_[i], &air_[i]);
            if (!rt_[i]->build(i, ic)) continue;
            instPtrs_[instCount_++] = &rt_[i]->instrument();
        }
    }

    // Returns false if any required output (STEP timer, servo/pump/gate/flow LEDC)
    // failed to initialise (review #9 §3.8).
    bool configureSinks(uint8_t i, const InstrumentConfig& ic) {
        bool motionOk = motion_[i].begin(ic.motion);
        const auto& a = ic.air;
        // source
        if (a.source.type == AirSourceType::FanOnOff || a.source.type == AirSourceType::FanPwm)
            air_[i].configureSourcePwm(0, a.source.pin[0], 25000, a.source.activeHigh);
        else if (a.source.type == AirSourceType::PumpsDirect || a.source.type == AirSourceType::PumpsTank)
            for (uint8_t p = 0; p < a.source.pumpCount && p < MAX_PUMPS; ++p)
                air_[i].configureSourcePwm(p, a.source.pin[p], 25000, a.source.activeHigh);
        // gate — solenoid uses digital / PWM, servo gates use a µs pulse (#13)
        switch (a.gate.type) {
            case AirGateType::SolenoidSimple: air_[i].configureSolenoid(a.gate.pin, a.gate.activeHigh); break;
            case AirGateType::SolenoidPwm:    air_[i].configureSolenoidPwm(a.gate.pin, 20000, a.gate.activeHigh); break;
            case AirGateType::None:           break;
            default: air_[i].configureGateServo(a.gate.pin, a.gate.servoMinUs, a.gate.servoMaxUs); break;
        }
        // flow
        if (a.flow.type == FlowControlType::FlowServo)      air_[i].configureFlowServo(a.flow.pin, a.flow.servoMinUs, a.flow.servoMaxUs);
        else if (a.flow.type != FlowControlType::None)      air_[i].configureFlowPwm(a.flow.pin, 20000, a.flow.activeHigh);
        // angle
        if (a.angle.enabled) air_[i].configureAngleServo(a.angle.pin, a.angle.servoMinUs, a.angle.servoMaxUs);
        // sensor
        air_[i].configureSensor(a.sensor.pin);
        return motionOk && air_[i].configOk();
    }

    void requestHomingAll() {
        for (uint8_t i = 0; i < instCount_; ++i)
            if (instPtrs_[i] && instPtrs_[i]->actuator()) instPtrs_[i]->actuator()->requestHoming();
    }
    bool allHomed() const {
        for (uint8_t i = 0; i < instCount_; ++i)
            if (instPtrs_[i] && instPtrs_[i]->actuator() && !instPtrs_[i]->actuator()->isHomed()) return false;
        return true;
    }
    bool anyActuatorFault() const {
        for (uint8_t i = 0; i < instCount_; ++i) {
            auto* a = instPtrs_[i] ? instPtrs_[i]->actuator() : nullptr;
            if (a && a->fault() != FaultCode::None) return true;
        }
        return false;
    }
    bool anyAirFault() const {
        for (uint8_t i = 0; i < instCount_; ++i) {
            auto* air = instPtrs_[i] ? instPtrs_[i]->air() : nullptr;
            if (air && air->fault() != FaultCode::None) return true;
        }
        return false;
    }

    // Persist the admin token in NVS so it survives reboots and can be shown.
    void loadOrCreateAdminToken() {
        Preferences p; p.begin("swauth", false);
        String t = p.getString("admin", "");
        if (t.length() == 0) {
            uint32_t e[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
            auth_.regenerateAdminToken(e);
            p.putString("admin", auth_.adminToken().c_str());
        } else {
            auth_.setAdminToken(std::string(t.c_str()));
        }
        p.end();
    }

    // Show the AP password + admin token on Serial so the operator can log in
    // (they are otherwise unknowable). Regeneratable physically via factory reset.
    void printCredentials() {
#if DEBUG_SERIAL
        Serial.println(F("=== Slide Whistle — access credentials ==="));
        if (config_.network.apEnabled)
            Serial.printf("AP SSID : %s\nAP pass : %s\n", config_.network.apSsid, apPassword_.c_str());
        Serial.printf("Admin token (X-Auth-Token): %s\n", auth_.adminToken().c_str());
        Serial.println(F("==========================================="));
#endif
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
        web_.begin(&server_, &ws_, &router_, &auth_, &MainApp::millisNow);
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
            // Refresh the command gate BEFORE draining the queue, using the state
            // settled at the end of the previous cycle. Updating it only after
            // the drain (as before) let one batch execute against a stale gate —
            // e.g. a command drained while the system had already left Ready
            // (review #7 §4). Actuation is allowed ONLY in Ready.
            engine_.setCommandsBlocked(state_ != SysState::Ready);
            // Single owner of updates: engine_.tick() ticks each instrument's
            // actuator + air + sequencer exactly once — no second pass (#9).
            engine_.tick(ms, us, /*budget=*/32);
            // Lifecycle: home before declaring READY (#10); a homing fault must
            // move to Fault, never hang in Homing (review #12).
            SysState prev = state_;
            if (state_ == SysState::NeedsHoming) state_ = SysState::Homing;
            else if (state_ == SysState::Homing) {
                // A homing OR air fault must move to Fault, never hang (#12, §7.2).
                if (anyActuatorFault() || anyAirFault()) state_ = SysState::Fault;
                else if (allHomed())                     state_ = SysState::Ready;
            }
            else if (state_ == SysState::Ready) {
                // A fault developing while playing (endstop trip, air overpressure,
                // sensor lost) drops the system out of Ready (#3 §7.2).
                if (anyActuatorFault() || anyAirFault()) state_ = SysState::Fault;
                // A Home command issued from Ready sets an actuator back to
                // Homing — the lifecycle must follow it out of Ready (#6 §7).
                else if (!allHomed()) state_ = SysState::Homing;
            }
            else if (state_ == SysState::Fault) {
                // Recover once a Rearm command has cleared every latched fault:
                // re-home before returning to Ready (#3 §7.3). The Rearm handler
                // in the RT engine clears the faults and requests homing.
                if (!anyActuatorFault() && !anyAirFault()) state_ = SysState::NeedsHoming;
            }
            // Entering Fault is a GLOBAL physical stop, not just a command gate:
            // panicAll() closes every gate, zeroes the sources and stops the
            // motors so an already-playing note cannot continue (review #5 §P0.3).
            if (state_ == SysState::Fault && prev != SysState::Fault)
                engine_.panicAll(ms);
            // The command gate for the NEXT drain is applied at the top of the
            // loop from this settled state_ (review #7 §4). Safety/recovery
            // commands (Panic/NoteOff/Home/Rearm) bypass the gate regardless.
            publishSnapshot();                        // lock-free telemetry (#37)
            vTaskDelayUntil(&last, period);            // deterministic cadence
        }
    }

    static uint32_t millisNow() { return millis(); }

    // Fill + publish the telemetry snapshot from the RT task (single writer).
    void publishSnapshot() {
        StatusSnapshot& s = snap_.back();
        s.systemState = uint8_t(state_);
        s.restartRequired = router_.restartRequired();
        s.instrumentCount = instCount_;
        const ExecAck& ack = engine_.lastExec();
        s.lastAckSeq = ack.seq;
        s.lastAckResult = uint8_t(ack.result);
        for (uint8_t i = 0; i < instCount_ && i < MAX_INSTRUMENTS; ++i) {
            Instrument* in = instPtrs_[i];
            InstrumentStatus& is = s.instruments[i];
            if (!in) { is = InstrumentStatus{}; continue; }
            auto* a = in->actuator();
            is.id = in->id();
            is.homed = a && a->isHomed();
            is.moving = a && a->isMoving();
            is.posMm = a ? a->currentPositionMm() : 0.0f;
            is.targetMm = a ? a->targetPositionMm() : 0.0f;
            is.motionState = a ? uint8_t(a->state()) : 0;
            is.fault = a ? uint8_t(a->fault()) : 0;
            is.airState = in->air() ? uint8_t(in->air()->state()) : 0;
            is.activeNote = int16_t(in->sequencer().activeNoteOr(-1));
            is.softLimitPending = a && a->softLimitApplyPending();   // §4.4
        }
        snap_.publish();
    }

    // Web reader: builds JSON from the PUBLISHED snapshot, never from live
    // objects the RT task is mutating (review #37 — no torn reads / no lock).
    struct StatusSrc : IStatusSource {
        MainApp* app = nullptr;
        JsonValue statusJson() override {
            // Consistent copy: the RT task may publish while we serialize, so
            // read into a local (seqlock) rather than off the live buffer.
            StatusSnapshot s;
            app->snap_.readCopy(s);
            JsonValue v = JsonValue::makeObj();
            v.set("state", int(s.systemState));
            v.set("seq", (double)s.seq);
            // Execution ack for the last direct command (#3 §7): a client that
            // POSTed a command reads back its seq here with accepted/rejected.
            JsonValue ack = JsonValue::makeObj();
            ack.set("seq", (double)s.lastAckSeq);
            ack.set("result", int(s.lastAckResult));   // 1 accepted, 2 rejected
            v.set("lastAck", ack);
            v.set("firstBoot", app->firstBoot_);
            v.set("fsFormatted", app->fsFormatted_);
            v.set("fsRecovery", app->fsRecovery_);   // FS unmountable → recovery, no actuators
            JsonValue arr = JsonValue::makeArr();
            for (uint8_t i = 0; i < s.instrumentCount && i < MAX_INSTRUMENTS; ++i) {
                const InstrumentStatus& is = s.instruments[i];
                JsonValue o = JsonValue::makeObj();
                o.set("id", (int)is.id);
                o.set("pos_mm", (double)is.posMm);
                o.set("homed", is.homed);
                o.set("moving", is.moving);
                o.set("note", (int)is.activeNote);
                o.set("fault", (int)is.fault);
                o.set("soft_limit_pending", is.softLimitPending);   // §4.4: saved but deferred
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
    SnapshotPublisher snap_;

    EspMotionSink    motion_[MAX_INSTRUMENTS];
    EspAirSink       air_[MAX_INSTRUMENTS];
    InstrumentRuntime* rt_[MAX_INSTRUMENTS] = {nullptr};
    Instrument*      instPtrs_[MAX_INSTRUMENTS] = {nullptr};
    uint8_t          instCount_ = 0;

    AsyncWebServer   server_{80};
    AsyncWebSocket   ws_{"/ws"};
    WebServerAdapter web_;
    std::string      apPassword_;
    bool             fsOk_ = false, firstBoot_ = true, fsFormatted_ = false, fsRecovery_ = false;
    volatile SysState state_ = SysState::Boot;
    bool     safeRestartEnqueued_ = false;

public:
    MainApp() { status_.app = this; }
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_MAINAPP_H
