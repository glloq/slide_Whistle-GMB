# Feature & Hardware Validation Matrix

Honest status of every subsystem. A montage is **never** marked validated just
because the code compiles.

Markers:

- **IMPLEMENTED** — code written and integrated.
- **TESTED IN SOFTWARE** — covered by native unit tests (`make -C tests`).
- **NOT TESTED — REQUIRES HARDWARE** — logic exists but needs a physical rig.
- **EXPERIMENTAL** — provisional; interface may change.
- **BLOCKED / TODO** — not yet started.

## Control core (`core/`)

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Domain types / units | IMPLEMENTED · TESTED IN SOFTWARE | |
| `NoteMap` LUT + interpolation | IMPLEMENTED · TESTED IN SOFTWARE | 128 notes, neighbour interp, monotonic check |
| Pitch bend / vibrato | IMPLEMENTED · TESTED IN SOFTWARE | fractional-note interpolation, 3 vibrato units |
| `StepDirSlideActuator` | IMPLEMENTED · TESTED IN SOFTWARE (logic) · NOT TESTED — REQUIRES HARDWARE (step timing) | non-blocking homing FSM + timeout |
| `SingleServoSlideActuator` | IMPLEMENTED · TESTED IN SOFTWARE (mm↔µs) · NOT TESTED — REQUIRES HARDWARE | multi-point calibration |
| `DualServoSlideActuator` | IMPLEMENTED · TESTED IN SOFTWARE · NOT TESTED — REQUIRES HARDWARE | open-loop, same-cycle command |
| `DisabledSlideActuator` | IMPLEMENTED · TESTED IN SOFTWARE | air-only bench mode |
| Air sources (ext/fan/pump/tank) | IMPLEMENTED · TESTED IN SOFTWARE · NOT TESTED — REQUIRES HARDWARE | tank regulation logic tested with a simulated sensor |
| Air gates (solenoid/pwm/servo) | IMPLEMENTED · TESTED IN SOFTWARE · NOT TESTED — REQUIRES HARDWARE | peak/hold, timeouts, true-close on panic |
| Flow controller + curves | IMPLEMENTED · TESTED IN SOFTWARE | min≤nominal≤max, slew, velocity curves |
| Jet-angle servo | IMPLEMENTED · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE | CC74 hook |
| Air sensor (range/stale/hyst) | IMPLEMENTED · TESTED IN SOFTWARE | stale + out-of-range + absent detection |
| `AirSafetyController` | IMPLEMENTED · TESTED IN SOFTWARE | valve/pump timeout, overpressure, stale sensor |
| `NoteSequencer` | IMPLEMENTED · TESTED IN SOFTWARE | mono stack, legato, sustain, panic, min-note, rollover |
| `CommandQueue` | IMPLEMENTED · TESTED IN SOFTWARE | bounded, NoteOff/Panic priority |
| `HardwareResourceValidator` | IMPLEMENTED · TESTED IN SOFTWARE | GPIO/ADC/LEDC/PCA/range, WROOM + S3 profiles |
| `RuntimeConfig` + defaults | IMPLEMENTED · TESTED IN SOFTWARE | versioned, collision-free default |
| 11 presets | IMPLEMENTED · TESTED IN SOFTWARE | each validates clean |
| `Instrument` aggregate | IMPLEMENTED · TESTED IN SOFTWARE | CC map, live range change |
| `MidiRouter` | IMPLEMENTED · TESTED IN SOFTWARE | single entry point, transpose |
| `RealtimeEngine` | IMPLEMENTED · TESTED IN SOFTWARE | queue drain, routing, per-flute test scope |
| `PwmOutput` LEDC wrapper | IMPLEMENTED · TESTED IN SOFTWARE (math/polarity) · NOT TESTED — REQUIRES HARDWARE (LEDC) | 2.x + 3.x API isolated, both syntax-checked in CI |
| `EspMotionSink` / `EspAirSink` | IMPLEMENTED (structure) · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE | Arduino-guarded; both LEDC branches syntax-checked in CI; not yet wired into the sketch |
| Portable JSON (`Json.h`) | IMPLEMENTED · TESTED IN SOFTWARE | parser+writer, escaping, UTF-8, FNV-1a checksum |
| `ConfigCodec` + v3 migration | IMPLEMENTED · TESTED IN SOFTWARE | round-trip, structural validation, legacy NVS-key mapping |
| `ConfigStore` (atomic/backup/recovery) | IMPLEMENTED · TESTED IN SOFTWARE (logic) | transactional import, factory reset |
| LittleFS backend | IMPLEMENTED (structure) · NOT TESTED — REQUIRES HARDWARE | Arduino-guarded |
| API envelope (`ApiResponse`) | IMPLEMENTED · TESTED IN SOFTWARE | ok/error{code,message,field}, validator→envelope |
| `AuthManager` | IMPLEMENTED · TESTED IN SOFTWARE | generated admin token, sessions+expiry, criticality gate, Origin allow-list, rate limiter, connection cap, AP password |
| `ApiRouter` (/api/v1 dispatch) | IMPLEMENTED · TESTED IN SOFTWARE | auth/origin/rate gate, size/content-type, transactional apply, restart_required, enqueue-only control |
| `InstrumentRuntime` (config→objects) | IMPLEMENTED · TESTED IN SOFTWARE | selects actuator+air from config, rebuild switches mechanism, safe state |

## Firmware / web integration (next phases)

| Item | Status |
|------|--------|
| Wire core into `esp32_slide_whistle.ino` (RT task owns `Instrument`) | BLOCKED / TODO |
| `PwmOutput` LEDC wrapper (old/new Arduino-ESP32 API) | IMPLEMENTED (see core table) |
| Platform `IMotionSink` / `IAirSink` | IMPLEMENTED (skeleton) · EXPERIMENTAL — PCA9685 servo backend + RMT stepping TODO |
| Deterministic RT task (`vTaskDelayUntil`) — correction #3 | BLOCKED / TODO |
| Web handlers → CommandQueue only — correction #6 | BLOCKED / TODO |
| LittleFS `/config.json` atomic save + migration from v3 | IMPLEMENTED (logic tested; LittleFS backend needs hardware) |
| Auth/session/rate-limit logic — Section 15 | IMPLEMENTED · TESTED IN SOFTWARE |
| `/api/v1` dispatch logic (auth, transactional apply, restart) — Section 14 | IMPLEMENTED · TESTED IN SOFTWARE |
| Config→objects builder (`InstrumentRuntime`) | IMPLEMENTED · TESTED IN SOFTWARE |
| Firmware orchestration (`MainApp`, 2 tasks, boot-safe) | IMPLEMENTED (structure) · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE |
| Async web-server adapter (`WebServerAdapter`) + universal sketch | IMPLEMENTED (structure) · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE — CI build is informational |
| WebSocket keyboard + differential status push — Section 13 | PARTIAL (WS command path scaffolded) · TODO diff push |
| forceSafeOutputs pin map, BLE/rtpMIDI bring-up, lock-free status snapshot | TODO |

## Universal web UI (`esp32/webui/`)

| Item | Status |
|------|--------|
| `api.js` (throws on 4xx/5xx & ok:false — #23) | IMPLEMENTED · TESTED IN SOFTWARE |
| `dom.js` (no innerHTML #25, diff helpers #26) | IMPLEMENTED · TESTED IN SOFTWARE |
| `notes.js` (flush stuck NoteOff #27) | IMPLEMENTED · TESTED IN SOFTWARE |
| `ws.js` (one socket, NoteOff/panic priority #28) | IMPLEMENTED · TESTED IN SOFTWARE |
| `macros.js` (stop on error #24) | IMPLEMENTED · TESTED IN SOFTWARE |
| `config.js` (restart mirror, unsaved tracker) | IMPLEMENTED · TESTED IN SOFTWARE |
| `wizard.js` (first-boot step gating) | IMPLEMENTED · TESTED IN SOFTWARE |
| Visual shell (index.html/app.css/app.js glue) | IMPLEMENTED · NOT TESTED — REQUIRES BROWSER+FIRMWARE |
| Full wizard screens / expert-mode blocks / calibration UI | PARTIAL · TODO |
| First-boot wizard + expert mode UI — Section 13 | BLOCKED / TODO |
| OTA safe-state + rollback — Section 15 | BLOCKED / TODO |
| Calibration assistant + INMP441 auto-cal — Section 4 | BLOCKED / TODO |

## ESP32-WROOM default preset pin plan (validator-clean)

| Signal | GPIO | Notes |
|--------|------|-------|
| Stepper STEP | 32 | output |
| Stepper DIR | 33 | output |
| Stepper ENABLE | 25 | output |
| Endstop | 34 | input-only (OK as input) |
| Flow servo | 26 | LEDC |
| Gate (solenoid/servo) | 27 | LEDC/output |
| Fan PWM | 14 | LEDC |
| Jet-angle servo | 13 | LEDC |
| Pumps 1–3 | 16 / 17 / 18 | LEDC |
| Analog sensor | 36 | ADC1 (WiFi-safe), input-only |
| Single/dual servo A / B | 26 / 13 | LEDC |

Pins avoided by default: **6–11** (flash), **0/2/5/12/15** (strapping) as
outputs, **ADC2** pins for analog input while WiFi is on.

## External review response (P0/P1)

| # | Item | Status |
|---|------|--------|
| 1 | Permanent E-stop at boot | FIXED · TESTED (clearFault/rearm/arm) |
| 2 | CI BLE-MIDI dep + split jobs | FIXED (git tag dep; 4 CI jobs) — build not verifiable here |
| 3 | New UI not in LittleFS | FIXED (esp32-universal/S3 data_dir → esp32/webui) |
| 4 | REST routes not registered | FIXED (each route + 404) — REQUIRES HARDWARE |
| 5 | Credentials unknowable | FIXED (NVS persist + Serial print + login page) — REQUIRES HARDWARE |
| 6 | WS auth not implemented | FIXED (per-client session map) — REQUIRES HARDWARE |
| 7 | Rate limiter can block panic/NoteOff | FIXED · TESTED (safety bypass origin/rate/auth) |
| 8 | Outputs not safe at boot | FIXED (forceSafeOutputs + solenoid-closed + skip invalid/disabled) — REQUIRES HARDWARE |
| 9 | Double update per cycle | FIXED (engine is single updater) |
| 10 | No homing before READY | FIXED (NeedsHoming/Homing/Ready states) — REQUIRES HARDWARE |
| 11 | Homing clamp + logical offset | FIXED · TESTED (generous seek bound + real MoveToOffset) |
| 12 | Virtual position outpaces motor | PARTIAL — needs RMT/timer stepping + step-count position (REQUIRES HARDWARE) |
| 13 | Air servo normalization wrong | FIXED · TESTED (ServoOutput → µs) |
| 14 | Backends not wired (PCA/ToF/BLE/rtp/STA) | TODO — REQUIRES HARDWARE |
| 15 | Air opens before source ready | FIXED · TESTED (wait actuator AND air isReady + timeout) |
| 16 | Test air can stay active | FIXED · TESTED (server-side auto-stop) |
| 17 | Dynamic config not applied live | FIXED · TESTED (ApplyDynamicConfig → live objects) |
| 18 | JSON codec incomplete | FIXED · TESTED (full field-by-field round-trip) |
| UI | Wizard not wired, no login | FIXED (login/session + wizard rendered); full screens still PARTIAL |

Remaining hardware-bound work: RMT/timer step generation with step-count
feedback (#12), PCA9685 / ToF / digital-sensor / BLE-MIDI / rtpMIDI / WiFi-STA
bring-up (#14), and physical validation of every mount.

## Second review response (45 items)

FIXED·TESTED = code changed + native/node test proves it. FIXED = code changed,
correct-by-construction but needs hardware to validate. TODO = not yet done
(hardware-bound unless noted).

| # | Item | Status |
|---|------|--------|
| 1 | BLE-MIDI dep breaks CI clone | FIXED (dep off common; legacy-only) |
| 2 | CommandQueue not thread-safe | FIXED (portMUX critical sections) |
| 3 | All LEDC on channel 0 | FIXED·TESTED (LedcAllocator) |
| 4 | Stepper not position-servoed | TODO — RMT/timer + step-count feedback (hardware) |
| 5 | liveConfig never connected; push ignored | FIXED·TESTED (setLiveConfig + truthful `applied`) |
| 6 | applyDynamic incomplete (motor/air params) | FIXED·TESTED — speed/accel/soft-limits + flow params now applied live; pins/type stay restart-only |
| 7 | Presets lose musical table | FIXED·TESTED (enabled entries kept) |
| 8 | Null deref for disabled-then-enabled | FIXED (status iterates compacted ptrs) |
| 9 | Panic permanent, no rearm | FIXED·TESTED (Rearm command) |
| 10 | MIDI transports not integrated | TODO — DIN/BLE/rtp/STA bring-up (hardware) |
| 11 | Concurrent air tests orphan a flute | FIXED·TESTED (per-instrument sessions) |
| 12 | Homing hangs in Homing on fault | FIXED (→ Fault state) |
| 13 | Air rearm leaves subcomponent faults | FIXED·TESTED (resetFault) |
| 14 | Stable sensor flagged stale | FIXED·TESTED (freshness = new sample) |
| 15 | NaN → permanently absent | FIXED·TESTED (recovers; boot timeout) |
| 16 | Pump cascade never advances | FIXED·TESTED (update-driven) |
| 17 | Tank modes/PID not implemented | FIXED·TESTED (target-aware PI + hysteresis); level/position modes still share the sensor value |
| 18 | Out-of-range sensor keeps pumping | FIXED·TESTED (stops pumps) |
| 19 | FlowServoAsValve drives one servo twice | TODO — unified controller (hardware) |
| 20 | PCA9685 configurable but not driven | TODO (hardware) |
| 21 | ToF/digital sensors not implemented | TODO (hardware) |
| 22 | Air servo µs hardcoded | FIXED·TESTED (configurable window) |
| 23 | Endstops not watched while playing | TODO (hardware) |
| 24 | Validator accepts missing required pins | FIXED·TESTED (PIN_REQUIRED) |
| 25 | Resources not validated (angle/UART/I2C/I2S…) | PARTIAL — angle servo + servo-gate LEDC + pumps/PCA now claimed; UART/I2C/I2S buses TODO |
| 26 | Enum values unvalidated | FIXED·TESTED |
| 27 | Structural validation thin | FIXED·TESTED (ranges/monotonic/thresholds) |
| 28 | Future schema accepted | FIXED·TESTED (refused) |
| 29 | Bad checksum imports accepted | FIXED·TESTED (rejected) |
| 30 | LittleFS auto-format wipes data | FIXED (mount no-format first) |
| 31 | Rate limiter global | FIXED·TESTED (per-client buckets) |
| 32 | Origin off by default | FIXED — network.allowedOrigin config drives real enforcement |
| 33 | requireAuth field not applied | FIXED (MainApp applies it) |
| 34 | WS confirms auth without verifying | FIXED (verifySession on auth frame) |
| 35 | WS fragmented frames unhandled | TODO (hardware/transport) |
| 36 | HTTP body allocated before size check | TODO (transport) |
| 37 | Status snapshot unsynchronized | FIXED·TESTED — lock-free double-buffer snapshot |
| 38 | Wizard steps mostly empty | PARTIAL — instrument/motion/air real; others TODO |
| 39 | finishWizard ignores most choices | FIXED·TESTED (buildConfigPatch) |
| 40 | Play works w/o login, no WS errors shown | FIXED (onMessage → toast) |
| 41 | Config not reloaded after login | FIXED |
| 42 | Double WS reconnect | FIXED·TESTED |
| 43 | Unbounded browser queue | FIXED·TESTED (bounded + coalesce) |
| 44 | Expert mode is raw JSON | TODO — per-block forms |
| 45 | Calibration UI / OTA missing | TODO |

Net: the real-time/config/air correctness defects that can be proven off-device
are fixed and covered by tests (139 automated cases total). The remainder are
hardware-transport bound (RMT stepping, PCA9685, ToF, BLE/rtp/DIN, WS transport
edge cases) or larger UI build-out, and are listed here honestly rather than
marked done.

## How to run the software tests

```sh
make -C tests            # builds with g++/clang++, runs all cases
```

CI (`.github/workflows/ci.yml`) runs the same suite plus JSON/JS validation and
a PlatformIO firmware build on every push.
