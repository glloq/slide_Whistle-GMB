# General-Midi-Boop v2 — automatic recognition on the universal firmware

This document describes how the **universal slide-whistle firmware**
(`esp32/esp32_slide_whistle/universal_main.cpp` → `core/platform/MainApp.h` →
portable `core/*`) implements General-Midi-Boop's *Instrument Recognition &
Capability Protocol v2* (GMB `docs/SYSEX_IDENTITY.md`).

It implements **level 1**: the controller answers the handshake *and* serves a
capability descriptor rendered from its own live configuration.

> **Scope.** GMB support belongs to the universal firmware only. The deprecated
> sketches (`slide_Whistle_Fan_Servo`, `slide_Whistle_Solenoid_Servo`, the legacy
> v3 `esp32_slide_whistle.ino`) still build unchanged and do **not** take part in
> discovery.

> **GMB is control-plane code.** Nothing under `core/gmb/` may actuate a motor, a
> servo, a valve, a pump or an air system. Descriptor rendering, hashing,
> filesystem/NVS access and SysEx handling all run on the control-plane loop,
> never in the real-time actuator task.

---

## 1. Status

| Item | Status |
|---|---|
| 7-bit codecs, handshake / 0x10 / 0x11 frame codecs | IMPLEMENTED · TESTED IN SOFTWARE |
| Capability derivation from `RuntimeConfig` / `NoteMap` / `CcMap` | IMPLEMENTED · TESTED IN SOFTWARE |
| ASCII-only descriptor serializer | IMPLEMENTED · TESTED IN SOFTWARE |
| Pinned-snapshot block 0x10 transfer (retries, arbitrary order, timeout) | IMPLEMENTED · TESTED IN SOFTWARE |
| Revision tracker + change flags | IMPLEMENTED · TESTED IN SOFTWARE |
| Descriptor conformance against GMB's own validator rules | TESTED IN SOFTWARE (C++ and `node --test`) |
| Instance id fold (hash of a hardware unique id) | IMPLEMENTED · TESTED IN SOFTWARE |
| eFuse-MAC instance id on ESP32 | IMPLEMENTED · **NOT TESTED — REQUIRES HARDWARE** |
| NVS revision persistence (`Preferences`) | IMPLEMENTED · **NOT TESTED — REQUIRES HARDWARE** |
| DIN UART MIDI in/out transport | IMPLEMENTED (structure, compiles in CI) · **NOT TESTED — REQUIRES HARDWARE** |
| `GET /gmb/descriptor.json` | IMPLEMENTED (structure, compiles in CI) · **NOT TESTED — REQUIRES HARDWARE** |
| End-to-end discovery against a real General-Midi-Boop install | **NOT TESTED — REQUIRES HARDWARE** |
| BLE-MIDI / RTP-MIDI / native USB MIDI transports | **TODO** — the port interface exists, the bring-up does not |

No claim of hardware validation is made anywhere in this document.

---

## 2. Architecture

```
core/gmb/                       portable, Arduino-free, unit-tested
  GmbProtocol.h        wire constants + 7-bit little-endian codecs
  GmbIdentity.h        device model, firmware version, instance-id fold
  GmbCapabilities.h    RuntimeConfig/NoteMap/CcMap -> capability snapshot
  GmbDescriptor.h      snapshot -> ASCII-only JSON (the ONE serializer)
  GmbRevision.h        capability signature + persistent revision tracker
  GmbSysEx.h           frame encode / strict request parse
  GmbSysExService.h    published document + pinned-snapshot 0x10 transfer
  GmbMidiBridge.h      IGmbMidiPort, request staging, reply routing
  GmbRuntime.h         the facade: begin() / onConfigurationActivated()

core/MidiStreamParser.h     byte-wise MIDI 1.0 parser (running status, SysEx)
core/MidiTransportBridge.h  the two-plane split (notes vs GMB control)

core/platform/EspGmb.h      eFuse MAC, NVS revision store, DIN UART port
core/platform/MainApp.h     wiring + the control-plane loop
core/FirmwareVersion.h      semantic version announced in the handshake
```

The two planes never meet:

```
DIN UART bytes ─► MidiStreamParser ─┬─► channel voice ─► MidiRouter ─► CommandQueue ─► RealtimeEngine
                                    └─► SysEx         ─► GmbMidiBridge ─► GmbSysExService
```

GMB is deliberately **not** inside `MidiRouter`. A SysEx parser bug cannot route
bytes into NoteOn/CC handling, because the SysEx path has no reference to the
command queue at all.

---

## 3. Discovery flow

1. The host sends the block-1 request `F0 7D 00 01 00 F7`.
2. The firmware answers a 24-byte handshake carrying `instance_id`, the firmware
   version, the descriptor byte count, the revision and the flags.
3. If `revision` is unchanged, the host reuses its cached descriptor — nothing
   else happens.
4. Otherwise the host fetches the descriptor, either over
   `GET /gmb/descriptor.json` (handshake flag bit 0) or segment by segment over
   block 0x10.
5. When the advertised capabilities change at runtime, the firmware emits a
   block 0x11 notification. Hosts without a return path re-read block 1
   periodically instead; **the notification is an optimisation, never a
   dependency**.

---

## 4. Transports — a return path is mandatory

Automatic recognition cannot exist without a way to answer. The handshake is a
request/response exchange.

| Transport | Discovery | State in this firmware |
|---|---|---|
| DIN IN **+ OUT** | yes | implemented (`DinMidiPort`), **not hardware-tested** |
| DIN IN only | **no** | notes work; no reply is possible, so the device must be picked by hand in GMB |
| BLE-MIDI | yes (once implemented) | `IGmbMidiPort` is ready; the BLE bring-up is TODO |
| RTP-MIDI | yes (once implemented) | same; prefer the HTTP descriptor flag on Wi-Fi |
| Native USB MIDI (S2/S3) | yes (once implemented) | same |

Adding a transport means implementing `IGmbMidiPort` (three methods) and feeding
its bytes to `MidiStreamParser`. **No protocol logic is duplicated per
transport**, and the push-notification flag is only announced when a registered
port actually reports a usable return path.

### Configuring the DIN UART

The DIN pins are part of the versioned `RuntimeConfig` and go through the normal
migration/validation path — nothing is hardcoded:

```json
"midi": { "din": true, "dinRxPin": 4, "dinTxPin": 17 }
```

* Both default to `-1` (**unassigned**), like every actuator pin, so a fresh or
  migrated configuration never drives a board-specific GPIO by surprise.
* `dinRxPin` alone gives a playable MIDI IN. `dinTxPin` is what makes discovery
  possible.
* Both pins are claimed by `HardwareResourceValidator`, so they collide-check
  against every actuator pin. RX may sit on an input-only GPIO (34–39 on a
  WROOM); TX may not. RX and TX must differ.
* Changing either pin sets `restart_required`: the UART is opened once at boot.
* The port is opened **only on a validated configuration**. `buildClaims()`
  claims the DIN pins, so a valid config is what proves the TX pin is not also an
  actuator pin — driving a MIDI byte stream onto a solenoid gate or a stepper
  STEP input would be a real hazard. On an invalid configuration the controller
  stays reachable over the AP, where the descriptor still reports
  `configured: false`.

Wiring is the usual DIN MIDI arrangement: an opto-isolator (6N138/H11L1) on the
input, a pair of 220 Ω resistors on the output. UART2 is used.

---

## 5. `instance_id` — the pivot of the protocol

`instance_id` is what reattaches a configuration stored in GMB to the right
physical controller, so it must survive reboots and configuration changes and
must differ between two boards flashed with the same binary.

It is derived from the **ESP32 eFuse MAC** (`ESP.getEfuseMac()`), FNV-1a folded
to 32 bits (`gmb::instanceIdFromHardwareId`). It is therefore **never** derived
from the device name, the Wi-Fi settings or anything else a user can edit.

The MAC is *hashed*, not truncated: the wire format carries 7 bits per byte, and
truncating would throw away one bit in eight and could collapse two boards onto
one identity. `0` is reserved as "no identity" and is never returned.

---

## 6. Revision behaviour

`revision` is an ETag. The rule the firmware implements:

```
config change -> validated -> persisted -> ACTIVE
   -> revision++ -> descriptor rebuilt -> block 0x11 emitted
```

The decision is driven by a **signature of the advertised capabilities**, not by
"something was saved". The overall signature is the FNV-1a hash of the canonical
descriptor (rendered at revision 0), which makes the invariant airtight: if the
published bytes change, the revision moves; if they do not, it does not.

Consequences, all covered by tests:

* first valid descriptor → revision **1**;
* a plain reboot with an unchanged configuration → **same revision, no flash
  write**;
* an effective capability change → `revision++`, one NVS write, one block 0x11;
* a **no-op save**, a cosmetic setting that is not in the descriptor (AP SSID,
  `prepareTimeoutMs`, `watchdogMs`, a DIN pin) → **no increment, no NVS write, no
  notification**;
* a configuration replaced while the firmware was off → the counter advances
  once at boot, then stays put.

The record (`revision` + `signature`) lives in the NVS namespace `swgmb`, never
in `config.json`: a configuration save never rewrites the counter and a counter
bump never rewrites the configuration.

**Change flags** come from three separate structural digests, so block 0x11 can
say *what* moved:

| Flag | Driven by |
|---|---|
| `IDENTITY_CHANGED` (bit 0) | device name/model/firmware, per-instrument channel, name, GM identity |
| `INSTRUMENTS_CHANGED` (bit 1) | playable notes, voices, polyphony, expression, `physical` |
| `TIMING_CHANGED` (bit 2) | the two-phase timing block |
| `RESTART_REQUIRED` (bit 3) | the activated change still needs a reboot to reach the hardware |

### What triggers a rebuild

The hook is the **existing successful configuration-activation path**:
`ApiRouter::applyCandidate()` and the factory reset publish a monotonic
`configActivationSeq()`, which the control-plane loop watches. A rejected POST
(bad JSON, bad checksum, failed structural or hardware validation, failed
persist) never bumps it, so **a failed config write cannot move the published
descriptor**. Everything that changes an advertised capability therefore triggers
a rebuild: device name, enabled/configured state, instrument name, MIDI channel,
note range, the effective `NoteMap`, motion parameters that move the advertised
timing, air source/gate/flow, the CC map, vibrato/expression, pitch-bend range,
and the timing fields.

---

## 7. HTTP descriptor

```
GET /gmb/descriptor.json      →  200 application/json
```

* Serves a **copy of the very bytes** block 0x10 transfers. There is one
  serializer and one published document, so `descriptor_size`,
  `Content-Length` and the reassembled block-0x10 payload always agree.
* Read-only capability metadata: `GET` only, no token, no side effect, no path
  into the command queue. It cannot move hardware.
* Deliberately outside `/api/v1`, so it inherits none of the control endpoints'
  authentication surface and **weakens none of it**.
* Handshake flag bit 0 is announced **only** when the endpoint is genuinely
  reachable. Station mode is not implemented yet, so it follows the AP being
  enabled; with no network there is no flag.
* Returns `404` when no descriptor is published (level 0). Block 0x10 likewise
  answers a segment request with silence in that case, rather than an empty
  payload that would reassemble to `""` and fail to parse.

---

## 8. Descriptor fields for the slide whistle

### Device and musical identity

| Field | Value |
|---|---|
| `device.name` | the configured device name |
| `device.model` | `"Slide-Whistle-GMB"` (a display label; GMB derives no capability from it) |
| `gm_program` | `78` |
| `type` / `subtype` | `"pipe"` / `"whistle"` |

These match General-Midi-Boop's own `InstrumentTypeConfig.js`
(`pipe → whistle → GM 78`).

### `notes` — only what is genuinely playable

The descriptor does **not** simply echo `noteMin..noteMax`.
`NoteMap::positionForNote()` clamps beyond the outermost mapped point, so it
answers "yes" for notes that have no mapping at all — right for driving the
slide, wrong as a capability claim. `gmb::resolveNoteStrict()` is the
side-effect-free capability helper that refuses to clamp.

A MIDI note is advertised only when **all** of these hold:

1. it is inside the configured instrument MIDI range;
2. it is resolvable from the *active* map points **without endpoint clamping**
   (an enabled point at or below it **and** one at or above it);
3. the resulting position lies inside the configured soft limits and travel;
4. the configuration itself is valid;
5. some legal MIDI note can reach it given the global transpose (see §10).

Provisional generated points (`enabled = true`, `calibrated = false`) stay
playable: "not hardware calibrated" is not "unplayable".

Notes 0..127 are each tested. If every semitone between the extremes is playable
the set is announced as `{"mode":"range",...}`; otherwise as
`{"mode":"discrete","list":[...]}`. No false range is ever announced.

### `voices` / `polyphony`

One `voices[]` entry per contributing physical flute, with a stable id
(`"flute0"`, `"flute1"`, …) and that flute's own playable set.

Polyphony is derived, not assumed. The real-time engine **broadcasts** a
channel's notes to every instrument that accepts the channel and the note, and
each slide whistle is intrinsically monophonic, so:

* voices whose playable sets are **disjoint** genuinely add polyphony;
* voices whose sets **overlap** do not — a note in the overlap reaches both
  flutes (unison) and a second note in the overlap simply steals the first.

Overlapping voices are therefore grouped, and `polyphony.max` is the number of
groups. `constraints: [{"type":"one_note_per_voice"}]` is added whenever the
entry has more than one voice.

### `expression`

Derived from the active `CcMap` and the hardware that is actually enabled — never
from the defaults:

| CC | Advertised when |
|---|---|
| breath / expression / volume | a flow controller exists (they retarget it) |
| vibrato | the slide can move **and** vibrato routing is enabled |
| sustain | always (pure sequencer logic) |
| jet angle | the angle servo is attached |

A mapping of `0` means disabled and is ignored. `pitch_bend.range_semitones`
comes from `swc::DEFAULT_PITCH_BEND_RANGE_SEMITONES`, the constant the engine
itself applies, so the two cannot drift. `channel_aftertouch` is true only when
the configured runtime really uses channel pressure (it drives vibrato).
`poly_aftertouch` is `false`. `velocity` is true only when changing MIDI velocity
materially changes the generated air: through the source
(`lerp(min01, max01, velocity/127)`, so only when `min01 != max01`) or through
the flow controller (only for notes whose calibrated `airNominal` is 0, because a
non-zero `airNominal` overrides velocity entirely).

For a logical entry formed by **merging** several physical flutes, capabilities
are intersected: the CC list is the common subset, booleans are ANDed, and the
pitch-bend range is the minimum. A feature only one voice implements is not
promised.

### `physical`

The free GMB namespace, describing the **mechanism** — never the wiring:

```json
"physical": {
  "family": "winds",
  "mechanism": "variable_length_slide",
  "continuous_pitch": true,
  "slide_drive": "step_dir",
  "travel_mm": 100,
  "requires_homing": true,
  "air_source": "fan_pwm",
  "air_gate": "solenoid",
  "flow_control": "flow_servo",
  "legato": "always_close",
  "transpose_semitones": 0
}
```

No GPIO numbers, no Wi-Fi credentials, no tokens, no internal secrets. A string
the merged voices cannot agree on is omitted rather than guessed. `omni: true`
appears when a contributing flute listens on every channel.

`resources` is deliberately **not** emitted: the firmware cannot express a real
consumable capacity in GMB's units. A pump or a tank existing is not enough to
invent `ms_of_sound`.

---

## 9. Multi-flute and channel behaviour

The repository's musician-facing channel numbering is `1..16`, with `0` meaning
omni. The descriptor uses `0..15` and **requires unique channels**.

| Configuration | Descriptor |
|---|---|
| `midiChannel = 1..16` | `channel = midiChannel - 1` |
| `midiChannel = 0` (omni) | `channel = 0` plus `physical.omni = true` — GMB has no omni representation, and 0 is a channel the flute really does answer on |
| several flutes on **different** channels | one entry per channel |
| several flutes on the **same** channel | **merged** into one entry with several `voices` — never duplicate channels, which GMB's validator rejects outright |
| a disabled flute | contributes nothing |

Duplicate-channel prevention is covered by its own test, including the case
where an omni flute and a channel-1 flute both land on descriptor channel 0.

### `configured = false`

A descriptor **always** carries at least one instrument; `"instruments":[]` is
never emitted. When the controller is at factory default, invalid, or not
musically configured, a legal placeholder is emitted:

```json
{ "channel": 0, "configured": false, "type": "pipe", "subtype": "whistle", "gm_program": 78 }
```

No notes, no polyphony, no timing — nothing that cannot be vouched for. GMB then
falls back to manual entry **without overwriting** an earlier configuration.

`configured` describes the **persistent musical configuration**. It is *not*
toggled by a transient runtime state — unhomed, moving, faulted, muted,
mid-calibration. Structurally so: the capability builder has no access to a
runtime object at all. The one configuration-level case that does make an
instrument unconfigured is `motion.type = Disabled`, because a slide that cannot
move has no pitch control.

---

## 10. Known limitations

### Timing — absent means unknown

The slide whistle genuinely has the two-phase model (move the slide and prepare
the air, then admit air), but **the existing timeout values are not acoustic
measurements** and are not presented as such.

* `SequencerConfig::prepareTimeoutMs` is a **failure timeout** — the point at
  which a note that never became ready is abandoned. It is never serialized as
  `prepare.max_ms`, and a test asserts the value cannot appear in the document.
* `excite.latency_ms` is **omitted**. There is no acoustic onset measurement or
  calibration yet, and a servo valve's `openDelayMs` is a mechanism delay, not a
  measured acoustic latency. Zero is never emitted to mean "unknown".
* `release_ms` and `rearticulation_ms` are **omitted** for the same reason.

`prepare` is emitted only when it can be defended from the actual configuration:

| Air source | Preparation bound |
|---|---|
| external passive | `0` — a *measured* zero (`ready()` is unconditionally true) |
| fan on/off, fan PWM | `spinUpMs` — a real configured component |
| pumps direct | `(pumpCount - 1) x cascadeDelayMs` — when the cascade has started every pump |
| pumps + tank | **unknown** — readiness depends on current pressure/level, so `prepare` is omitted entirely rather than faked with a constant |

The slide move bound comes from the symmetric trapezoidal profile the actuator
integrator really runs (`accel == decel`, capped at `maxSpeedMmS`) over the span
of the advertised notes. The sequencer starts the air source and the slide move
in the *same* call, so the two overlap and the bound is their **MAX, never their
sum**.

`prepare.silent` is `true` only for `LegatoPolicy::AlwaysClose`. Every other
policy can hold the air open across a move — `Glissando` always does, the
`HoldWithin*`/`SafetyLargeMove` policies do under a runtime condition — so the
movement can be audible and is not described as silent.

`min_note_ms` is emitted only from the effective runtime minimum-note rule
(`SequencerConfig::minNoteMs`), and only when it is non-zero.

### Transpose

The firmware supports a **global MIDI transpose**: the engine plays
`incoming note + transpose`, and `noteMin`/`noteMax` and the `NoteMap` are both
expressed in the resulting *physical* pitch space.

GMB assumes plain MIDI pitch semantics and has no transpose-aware compensation
today. This firmware does not hide the problem:

* playback behaviour is unchanged;
* the descriptor advertises the **physical / effective playable pitch set** — the
  pitches the instrument can actually sound — so it never claims a pitch the
  device cannot produce;
* a physical pitch that no legal MIDI note could reach (`pitch - transpose`
  outside `0..127`) is not advertised;
* the active value is published as `physical.transpose_semitones` so a host can
  compensate;
* tests cover a non-zero transpose in both directions.

**Limitation that cannot be solved here:** with a non-zero transpose, the set the
descriptor advertises and the set of MIDI notes a host should *send* differ by
`transpose`. Closing that gap needs General-Midi-Boop to read
`physical.transpose_semitones` and offset accordingly — a host-side change that
is deliberately out of scope for this firmware change. **Until then, use
`midi.transpose = 0` when the controller is driven by GMB.**

### Descriptor size

A descriptor larger than `MAX_DESCRIPTOR_BYTES` (8192) is not published: the
firmware falls back to level 0 (`descriptor_size = 0`) rather than announce a
size the block 0x10 transfer could not deliver. A realistic four-flute
configuration is far below that.

---

## 11. Robustness

Every GMB request is untrusted input. The service enforces:

* `F0 … F7` framing, the manufacturer id (`7D`) and the GMB device id (`00`);
* every payload byte below `0x80`;
* the **exact** length of each known block (6 bytes for a handshake request,
  8 for a segment request) — anything else is dropped;
* direction — a response or notification arriving back in is never echoed, so two
  instruments on one bus cannot talk to each other;
* chunk-index bounds — an out-of-range index is answered with **silence** and
  neither starts a transfer nor keeps one alive;
* a token bucket (32 tokens, +1 every 5 ms) that admits the whole discovery burst
  but caps a sustained flood;
* a 5 s idle timeout that releases an abandoned transfer;
* a maximum descriptor size;
* no unbounded heap growth: the only per-transfer allocation is a delivered-index
  bitmap sized from the *published* document, never from a request, and the MIDI
  bridge stages **one** request in a fixed-size buffer (a second one is dropped,
  counted as an overrun).

Unknown GMB blocks are ignored silently. A malformed or foreign SysEx cannot
produce actuator activity: it is dropped by the parser or the codec, and the
SysEx path has no reference to the command queue.

### The block 0x10 pinned-snapshot rule

A transfer picks its document **exactly once**, when it starts; every later
segment of that transfer is cut from that same document. A retry — of segment 0
as much as of any other — belongs to the transfer in flight and does **not**
re-pin, because re-pinning is exactly what would let a host reassemble segment 0
of revision A with segment 1 of revision B.

The pin is released only when:

* every **distinct** segment has been delivered — unique indices are tracked, so
  a transfer never ends merely because the highest-numbered segment happened to
  be asked for first, and N retries of segment 0 never finish it;
* the host stops asking (idle timeout); or
* a handshake announces a document the pinned one no longer matches.

The exact sequence from the protocol notes is a test:

1. request chunk 0 of revision A;
2. the configuration becomes revision B;
3. retry chunk 0 → **still revision A**;
4. request arbitrary other chunks → **still revision A**;
5. the transfer completes, releasing the pin;
6. the next fresh transfer gets revision B.

---

## 12. Wire examples

### Handshake

Request:

```
F0 7D 00 01 00 F7
```

Reply — exactly 24 bytes (instance id `0x1234ABCD`, firmware `1.0.0`,
descriptor 859 bytes, revision 1, HTTP + push flags):

```
F0 7D 00 01 01 02 4D 57 52 11 01 01 00 00 5B 06 00 01 00 00 00 00 03 F7
│  │  │  │  │  │  └──────────┘ └──────┘ └──────┘ └──────────┘ │  └─ F7
│  │  │  │  │  │   instance_id firmware  desc     revision     flags
│  │  │  │  │  └─ proto_ver = 02
│  │  │  │  └─ direction = response
│  │  │  └─ block 01
│  │  └─ GMB device id
│  └─ manufacturer 7D
└─ F0
```

`descriptor_size` is 21 bits over 3 bytes: `5B 06 00` = `0x5B + (0x06 << 7)` =
859. `instance_id` and `revision` are 32 bits over 5 bytes, the last byte
carrying bits 28–31 as a full nibble.

### Descriptor transfer

```
request  F0 7D 00 10 00 00 00 F7                  (chunk 0)
reply    F0 7D 00 10 01 05 00 00 00 <200 bytes> F7 (5 chunks total, chunk 0)
```

### Change notification

```
F0 7D 00 11 02 <revision[5]> <change_flags> F7
```

### Descriptor

The example below is generated by the real capability builder from a
representative configuration (one stepper-driven slide whistle on MIDI channel 1,
100 mm travel at 120 mm/s and 800 mm/s², notes 60–84 mapped over 0–90 mm, PWM fan
with a 150 ms spin-up, simple solenoid gate, flow servo, `minNoteMs = 40`). It is
checked in as `tests/fixtures/gmb_descriptor_reference.json` and compared
byte-for-byte by the test suite, so this document cannot drift from what the
firmware serves. Regenerate with `make -C tests fixture`.

```json
{
    "gmb_descriptor": 2,
    "revision": 1,
    "device": {
        "name": "Slide Whistle",
        "model": "Slide-Whistle-GMB"
    },
    "instruments": [
        {
            "channel": 0,
            "configured": true,
            "type": "pipe",
            "subtype": "whistle",
            "gm_program": 78,
            "name": "Slide Whistle",
            "notes": {
                "mode": "range",
                "min": 60,
                "max": 84
            },
            "voices": [
                {
                    "id": "flute0",
                    "notes": {
                        "mode": "range",
                        "min": 60,
                        "max": 84
                    }
                }
            ],
            "polyphony": {
                "max": 1
            },
            "timing": {
                "prepare": {
                    "base_ms": 150,
                    "max_ms": 900,
                    "silent": true
                },
                "min_note_ms": 40
            },
            "expression": {
                "cc": [
                    1,
                    2,
                    7,
                    11,
                    64
                ],
                "pitch_bend": {
                    "supported": true,
                    "range_semitones": 2
                },
                "channel_aftertouch": true,
                "poly_aftertouch": false,
                "velocity": true
            },
            "physical": {
                "family": "winds",
                "mechanism": "variable_length_slide",
                "continuous_pitch": true,
                "slide_drive": "step_dir",
                "travel_mm": 100,
                "requires_homing": true,
                "air_source": "fan_pwm",
                "air_gate": "solenoid",
                "flow_control": "flow_servo",
                "legato": "always_close",
                "transpose_semitones": 0
            }
        }
    ]
}
```

On the wire it is 859 ASCII bytes with no whitespace — the value the handshake
announces as `descriptor_size`, the length `Content-Length` reports, and the
length GMB reassembles from block 0x10.

---

## 13. Tests

`make -C tests` builds and runs the whole native suite (it also syntax-checks the
ESP32-guarded platform layer against an arduino-esp32 API stub, for both API
generations and both board profiles).

| File | Covers |
|---|---|
| `tests/test_gmb_protocol.cpp` | codecs and their boundaries, the exact 24-byte handshake, proto version 2, the 12-byte notification and its flags, malformed/foreign/echoed SysEx, out-of-range chunks, rate limiting, arbitrary chunk order, repeated chunk 0, repeated arbitrary chunks, the frozen snapshot, the idle timeout, handshake/pin interaction |
| `tests/test_gmb_capabilities.cpp` | strict note resolution vs the clamping path, soft-limit clipping, provisional points, range vs discrete, channel mapping, four channels, same-channel merge, overlap-aware polyphony, duplicate-channel prevention, omni, disabled flute, never-zero-instruments, the `configured=false` placeholder, timing omission, `prepareTimeoutMs` non-exposure, prepare = max(air, slide), glissando not silent, CC derivation, velocity true/false, pitch bend, aftertouch, transpose, ASCII-only output, non-ASCII escaping, the reference fixture, and a port of GMB's validator rules |
| `tests/test_gmb_runtime.cpp` | revision 1 on first boot, unchanged across a reboot, bumped by an effective change, unchanged by a cosmetic or no-op save, offline edits, change flags, no notification on a no-op, the push flag, and HTTP == SysEx byte-for-byte |
| `tests/test_midi_transport.cpp` | the MIDI parser (running status, velocity 0, real-time bytes, oversized/unterminated SysEx) and the two-plane split, including that SysEx never becomes a note |
| `tests/test_config.cpp` | DIN pin round-trip, range rejection, RX≠TX, GPIO collision checks, v3 migration leaving the pins unassigned |
| `tests/test_api.cpp` | the activation counter: it moves only on a successful, persisted, activated config |
| `tests/js/gmb-descriptor.test.mjs` | the published descriptor against faithful ports of GMB's own `validateDescriptor`, `assembleChunks` and `parseGmbHandshake` |

---

## 14. Remaining hardware-validation items

Everything below is implemented and compiles, but has **not** run on a physical
controller:

1. the eFuse-MAC instance id (uniqueness across two real boards, stability across
   reboots);
2. NVS revision persistence across power cycles and its flash-wear behaviour;
3. the DIN UART MIDI input and output (opto-isolator wiring, 31250 baud, byte
   timing, the 1 ms control-plane poll cadence);
4. a real end-to-end discovery against a running General-Midi-Boop install:
   handshake, descriptor fetch, block 0x11 on a configuration save;
5. `GET /gmb/descriptor.json` served by ESPAsyncWebServer;
6. the acoustic onset measurement that would let `excite.latency_ms` exist at
   all — the reason it is omitted today.
