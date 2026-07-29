# Universal controller web UI

The new `/api/v1` frontend for the universal firmware. It is **additive** — the
legacy v3 UI under `esp32/esp32_slide_whistle/data/` is left untouched.

## Structure

```
webui/
├── index.html         responsive shell (theme-aware, PANIC always visible)
├── app.css            light/dark, mobile-first
├── app.js             entry: wires the modules below
└── js/
    ├── api.js         /api/v1 client — throws on 4xx/5xx & {ok:false} (#23)
    ├── dom.js         safe DOM (createElement/textContent only, no innerHTML #25) + diff helpers (#26)
    ├── notes.js       active-note registry, flushes NoteOff on blur/hide/disconnect (#27)
    ├── ws.js          one WebSocket for MIDI, NoteOff/panic prioritised (#28)
    ├── macros.js      run steps, stop at first failure (#24)
    ├── config.js      client mirror of dynamic-vs-restart + unsaved tracker
    ├── wizard.js      first-boot step machine with per-step gating (Section 13)
    └── presets-meta.js UI labels for the 11 mounting presets
```

## Design rules enforced

- **No false success:** every request goes through `api.js`, which raises
  `ApiError` (carrying `{code, message, field}`) on any non-2xx or `ok:false`.
- **No innerHTML:** all nodes are built with `dom.h()` (createElement +
  textContent + addEventListener). User strings can never inject markup.
- **No stuck notes:** `NoteRegistry` + `bindLifecycleFlush` send NoteOff on
  blur, tab-hide and WebSocket close.
- **One socket for MIDI:** `MidiSocket` replaces per-note HTTP; NoteOff/panic
  jump the queue and are flushed first on reconnect.
- **Differential updates:** status text is patched only when a value changed
  (`patchText` / `diffKeys`) — no full DOM rebuild on every frame.

## Tests

Pure logic is unit-tested with the node test runner (no browser needed):

```sh
node --test tests/js/ui.test.mjs
```

CI runs this plus `node --check` on every script.

## Serving

The universal firmware serves this directory from LittleFS root. Point the
PlatformIO `data_dir` at `esp32/webui` for the `esp32-universal` build (or copy
its contents into the active `data/` before `pio run -t uploadfs`).

## Status

- Logic modules: **IMPLEMENTED · TESTED IN SOFTWARE** (17 node cases).
- Visual shell (`index.html` / `app.css` / `app.js` glue) and the full wizard
  screens: **IMPLEMENTED · NOT TESTED — REQUIRES HARDWARE/BROWSER** (rendered
  against a live firmware).
