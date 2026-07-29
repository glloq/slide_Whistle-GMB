# ESP32 Universal Slide-Whistle — Architecture

> Status of this document: describes the **portable control core** introduced
> under `esp32/esp32_slide_whistle/core/`. This core is fully implemented and
> unit-tested in software. Its integration into the running Arduino sketch
> (`esp32_slide_whistle.ino`) and the web layer is **in progress** — see
> [HARDWARE_MATRIX.md](HARDWARE_MATRIX.md) for the per-feature status.

## Goal

A single ESP32 firmware that drives one or several slide whistles, each
configured **entirely from the web UI** — no code edit, no recompile — for both
the slide-drive mechanism and the air path.

## Layered design

```
            ┌────────────────────────── Network core (Core 0) ──────────────────────────┐
            │  WiFi · REST /api/v1 · WebSocket · JSON parsing · HardwareResourceValidator │
            └───────────────────────────────────┬───────────────────────────────────────┘
                                                 │  (enqueue only — never touches a GPIO)
                                                 ▼
                                   swc::CommandQueue<N>  (bounded, NoteOff/Panic first)
                                                 │
            ┌────────────────────────── Real-time core (Core 1) ─────────────────────────┐
            │  NoteSequencer → ISlideActuator + IAirSystem   · GlobalSafety · watchdog     │
            └─────────────────────────────────────────────────────────────────────────────┘
```

Only the real-time core owns hardware. The network side deposits **structured
commands** into a bounded queue (`swc::CommandQueue`). This replaces the former
`volatile` global flags (`g_homingRequested`, `g_testAirFluteId`, …) and the
blocking `while` loops in `taskMotion`.

## Domain model (`swc::` namespace, `core/`)

```
InstrumentConfig
├── ISlideActuator                (canonical unit: millimetres)
│   ├── StepDirSlideActuator      A4988 / DRV8825 / TMC step-dir
│   ├── SingleServoSlideActuator  GPIO or PCA9685, mm↔µs calibration
│   ├── DualServoSlideActuator    two servos, same command cycle
│   └── DisabledSlideActuator     air-only bench testing
│
├── NoteMap                       dynamic 128-note LUT + interpolation
│
├── AirSystem (IAirSystem)        composed of, not an enum of whole systems:
│   ├── IAirSource                ExternalPassive · Fan · PumpDirect · PumpTank
│   ├── IAirGate                  None · SolenoidSimple · SolenoidPwm · ServoValve
│   ├── IFlowController           servo / PWM, velocity curves, slew
│   ├── IAirSensor                pressure/ToF/Hall/endstop/level
│   └── AirSafetyController       valve & pump timeouts, overpressure, stale sensor
│
├── NoteSequencer                 monophonic stack, legato, sustain, panic
└── CcMap / SequencerConfig / safety
```

### Why interfaces everywhere

The rest of the engine speaks **only millimetres** and **only note events**. It
never learns whether a stepper or a servo moves the slide, nor whether the air
comes from a fan, a solenoid or a pump+tank. Swapping mechanism = swapping which
concrete class the config selects — no code change, exactly the acceptance
criteria #2 and #3.

### Hardware boundary

`IMotionSink` and `IAirSink` are the only seam that touches a GPIO / servo /
PCA9685. The core computes *what* to output; a thin platform layer performs the
write. Native unit tests substitute recording fakes, which is how the whole
timeline is tested without hardware.

## Note timeline (Section 7)

`NoteSequencer::noteOn` →
1. push to the mono stack, choose the active note by policy;
2. compute mm from `NoteMap` (incl. pitch bend / vibrato);
3. `air.prepareNote`, apply legato decision (keep/cut air);
4. `actuator.requestPositionMm`, enter **Positioning**;
5. when `actuator.isReadyForAir()` → `air.startNote`, enter **Playing**.

A `noteOff` received while still **Positioning** cancels the pending air open
(correction #1): the gate never opens late. `panic()` clears the stack,
sustain, pitch bend and e-stops both actuator and air, and no deferred command
can reopen the air (correction #15).

## Concurrency rules honoured by the core

- no `delay()` / `vTaskDelay()` inside a hardware sequence;
- no long `while` in homing/sweep — homing is a state machine with per-phase
  timeouts;
- no dynamic allocation on the play path (all blocks are members; the queue and
  note stack are fixed arrays);
- `millis()`/`micros()` rollover handled via `elapsed_u32`.

## Mapping to the mission's correction list

| # | Issue | Where fixed |
|---|-------|-------------|
| 1 | Note Off ignored during positioning | `NoteSequencer::noteOff` + `update` |
| 2 | Legato stored but unused | `NoteSequencer::legatoHold` |
| 4 | Infinite loop after homing failure | `StepDirSlideActuator` FSM → `Fault` |
| 5 | Blocking homing/sweep/tests | non-blocking FSM, timeouts |
| 7 | Volatile globals as a queue | `CommandQueue` |
| 8 | Constant mm/semitone pitch bend | `NoteMap::positionForNote` |
| 10 | LUT sized to default MIDI range | fixed 128-entry `NoteMap` |
| 12 | Mono limited to one last note | full `NoteSequencer` stack |
| 15 | Incomplete panic | `NoteSequencer::panic` + `AirSystem::emergencyStop` |
| 21 | Colliding default pins | `defaultConfig` (pins unassigned) + validator |

Remaining corrections (#3 real-time period, #6 web→hardware, #9 CC routing,
#11 range propagation, #13 test scope, #14 watchdog, #16–20 config/API, #22–30
network/UI/CI) are tracked in [HARDWARE_MATRIX.md](HARDWARE_MATRIX.md); several
are already satisfied structurally by this core and complete once it is wired
into the sketch and web layer.
