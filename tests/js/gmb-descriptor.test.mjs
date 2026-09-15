/**
 * tests/js/gmb-descriptor.test.mjs — check the descriptor this firmware
 * publishes against General-Midi-Boop's OWN consumer rules.
 *
 * The three functions below are faithful ports of the current implementations in
 * General-Midi-Boop `main`:
 *
 *   validateDescriptor   — src/midi/instrument/DescriptorProtocol.js
 *   assembleChunks       — src/midi/instrument/DescriptorProtocol.js
 *   parseGmbHandshake    — src/midi/devices/DeviceManager.js
 *
 * They are copied rather than imported because GMB is a separate repository;
 * keeping them here means CI fails the moment this firmware emits something the
 * real host would reject. The fixture itself is generated from the REAL C++
 * capability builder (`make -C tests fixture`), so the two ends cannot drift.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE = path.join(HERE, '..', 'fixtures', 'gmb_descriptor_reference.json');

const MAX_INSTRUMENTS = 16;
const SUPPORTED_DESCRIPTOR_VERSION = 2;
const NOTE_MODES = new Set(['range', 'discrete']);
const CHUNK_PAYLOAD = 200;

function _validateNotes(notes, at, errors) {
  if (notes == null) return;
  if (typeof notes !== 'object' || Array.isArray(notes)) {
    errors.push(`${at} must be an object`);
    return;
  }
  if (!NOTE_MODES.has(notes.mode)) {
    errors.push(`${at}.mode must be 'range' or 'discrete'`);
    return;
  }
  if (notes.mode === 'range') {
    const okMin = notes.min == null || (Number.isInteger(notes.min) && notes.min >= 0 && notes.min <= 127);
    const okMax = notes.max == null || (Number.isInteger(notes.max) && notes.max >= 0 && notes.max <= 127);
    if (!okMin) errors.push(`${at}.min must be an integer 0-127`);
    if (!okMax) errors.push(`${at}.max must be an integer 0-127`);
    if (Number.isInteger(notes.min) && Number.isInteger(notes.max) && notes.min > notes.max) {
      errors.push(`${at}.min must be <= ${at}.max`);
    }
  } else if (!Array.isArray(notes.list) || notes.list.length === 0) {
    errors.push(`${at}.list must be a non-empty array in discrete mode`);
  } else if (notes.list.some((n) => !(Number.isInteger(n) && n >= 0 && n <= 127))) {
    errors.push(`${at}.list must contain only integers 0-127`);
  }
}

function validateDescriptor(obj) {
  const errors = [];
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) {
    return { valid: false, errors: ['descriptor must be an object'] };
  }
  if (obj.gmb_descriptor !== SUPPORTED_DESCRIPTOR_VERSION) {
    errors.push(`gmb_descriptor must be ${SUPPORTED_DESCRIPTOR_VERSION}`);
  }
  if (obj.revision != null && !(Number.isInteger(obj.revision) && obj.revision >= 0)) {
    errors.push('revision must be a non-negative integer');
  }
  if (obj.device != null && (typeof obj.device !== 'object' || Array.isArray(obj.device))) {
    errors.push('device must be an object');
  }
  if (!Array.isArray(obj.instruments) || obj.instruments.length === 0) {
    errors.push('instruments must be a non-empty array');
  } else if (obj.instruments.length > MAX_INSTRUMENTS) {
    errors.push(`instruments must have at most ${MAX_INSTRUMENTS} entries`);
  } else {
    const seenChannels = new Set();
    obj.instruments.forEach((inst, i) => {
      const at = `instruments[${i}]`;
      if (!inst || typeof inst !== 'object' || Array.isArray(inst)) {
        errors.push(`${at} must be an object`);
        return;
      }
      if (!Number.isInteger(inst.channel) || inst.channel < 0 || inst.channel > 15) {
        errors.push(`${at}.channel must be an integer 0-15`);
      } else if (seenChannels.has(inst.channel)) {
        errors.push(`${at}.channel ${inst.channel} is duplicated`);
      } else {
        seenChannels.add(inst.channel);
      }
      if (inst.configured != null && typeof inst.configured !== 'boolean') {
        errors.push(`${at}.configured must be a boolean`);
      }
      if (inst.gm_program != null &&
          !(Number.isInteger(inst.gm_program) && inst.gm_program >= 0 && inst.gm_program <= 127)) {
        errors.push(`${at}.gm_program must be an integer 0-127`);
      }
      if (inst.type != null && typeof inst.type !== 'string') errors.push(`${at}.type must be a string`);
      if (inst.subtype != null && typeof inst.subtype !== 'string') errors.push(`${at}.subtype must be a string`);
      _validateNotes(inst.notes, `${at}.notes`, errors);
    });
  }
  return { valid: errors.length === 0, errors };
}

function assembleChunks(chunks) {
  if (!Array.isArray(chunks) || chunks.length === 0) {
    return { complete: false, total: null, missing: [], json: null };
  }
  const totals = new Set(chunks.map((c) => c && c.total).filter((t) => Number.isInteger(t) && t > 0));
  if (totals.size !== 1) return { complete: false, total: null, missing: [], json: null };
  const total = [...totals][0];
  const byIndex = new Map();
  for (const c of chunks) {
    if (!c || !Number.isInteger(c.index) || c.index < 0 || c.index >= total) continue;
    if (typeof c.payload !== 'string') continue;
    byIndex.set(c.index, c.payload);
  }
  const missing = [];
  for (let i = 0; i < total; i++) if (!byIndex.has(i)) missing.push(i);
  if (missing.length > 0) return { complete: false, total, missing, json: null };
  let json = '';
  for (let i = 0; i < total; i++) json += byIndex.get(i);
  return { complete: true, total, missing: [], json };
}

function parseGmbHandshake(bytes) {
  if (!Array.isArray(bytes) || bytes.length !== 24) return null;
  if (bytes[0] !== 0xf0 || bytes[1] !== 0x7d || bytes[2] !== 0x00) return null;
  if (bytes[3] !== 0x01 || bytes[4] !== 0x01 || bytes[23] !== 0xf7) return null;
  if (bytes[5] !== 0x02) return null;
  const dec32 = (b) =>
    (((b[0] & 0x7f) | ((b[1] & 0x7f) << 7) | ((b[2] & 0x7f) << 14) |
      ((b[3] & 0x7f) << 21) | ((b[4] & 0x0f) << 28)) >>> 0);
  return {
    protocolVersion: bytes[5],
    instanceId: dec32(bytes.slice(6, 11)),
    firmware: { major: bytes[11], minor: bytes[12], patch: bytes[13] },
    descriptorSize: (bytes[14] & 0x7f) | ((bytes[15] & 0x7f) << 7) | ((bytes[16] & 0x7f) << 14),
    revision: dec32(bytes.slice(17, 22)),
    flags: { httpAvailable: (bytes[22] & 0x01) !== 0, pushNotifications: (bytes[22] & 0x02) !== 0 },
  };
}

// --- the firmware side, as this repo implements it -------------------------

function encode32Le7(v) {
  return [v & 0x7f, (v >>> 7) & 0x7f, (v >>> 14) & 0x7f, (v >>> 21) & 0x7f, (v >>> 28) & 0x0f];
}
function encode21Le7(v) {
  return [v & 0x7f, (v >>> 7) & 0x7f, (v >>> 14) & 0x7f];
}
function encode14Le7(v) {
  return [v & 0x7f, (v >>> 7) & 0x7f];
}
/** Split a descriptor the way core/gmb/GmbSysEx.h does, into block 0x10 frames. */
function encodeChunks(json) {
  const total = Math.max(1, Math.ceil(json.length / CHUNK_PAYLOAD));
  const frames = [];
  for (let i = 0; i < total; i++) {
    const payload = json.slice(i * CHUNK_PAYLOAD, (i + 1) * CHUNK_PAYLOAD);
    frames.push([
      0xf0, 0x7d, 0x00, 0x10, 0x01,
      ...encode14Le7(total), ...encode14Le7(i),
      ...[...payload].map((ch) => ch.charCodeAt(0) & 0x7f),
      0xf7,
    ]);
  }
  return frames;
}
/** Decode one, the way DeviceManager.parseDescriptorChunk does. */
function parseDescriptorChunk(bytes) {
  if (!Array.isArray(bytes) || bytes.length < 10) return null;
  if (bytes[0] !== 0xf0 || bytes[1] !== 0x7d || bytes[2] !== 0x00) return null;
  if (bytes[3] !== 0x10 || bytes[4] !== 0x01 || bytes[bytes.length - 1] !== 0xf7) return null;
  let payload = '';
  for (let i = 9; i < bytes.length - 1; i++) payload += String.fromCharCode(bytes[i] & 0x7f);
  return {
    total: (bytes[5] & 0x7f) | ((bytes[6] & 0x7f) << 7),
    index: (bytes[7] & 0x7f) | ((bytes[8] & 0x7f) << 7),
    payload,
  };
}

const raw = fs.readFileSync(FIXTURE, 'latin1').replace(/[\r\n]+$/, '');

test('the published descriptor is ASCII-only, so it needs no 7-bit packing', () => {
  for (let i = 0; i < raw.length; i++) {
    assert.ok(raw.charCodeAt(i) < 0x80, `byte ${i} is not 7-bit: ${raw.charCodeAt(i)}`);
  }
});

test('the published descriptor passes GMB validateDescriptor()', () => {
  const parsed = JSON.parse(raw);
  const result = validateDescriptor(parsed);
  assert.deepEqual(result, { valid: true, errors: [] });
});

test('the descriptor carries the slide whistle musical identity', () => {
  const d = JSON.parse(raw);
  assert.equal(d.gmb_descriptor, 2);
  assert.equal(d.device.model, 'Slide-Whistle-GMB');
  assert.ok(d.instruments.length >= 1, 'instruments is never empty');
  const inst = d.instruments[0];
  assert.equal(inst.gm_program, 78);
  assert.equal(inst.type, 'pipe');
  assert.equal(inst.subtype, 'whistle');
  assert.equal(inst.physical.family, 'winds');
  // A single physical slide whistle is monophonic.
  assert.equal(inst.polyphony.max, 1);
  // No invented timing: absent means unknown, never zero.
  assert.equal(inst.timing.excite, undefined);
  assert.equal(inst.timing.release_ms, undefined);
  assert.equal(inst.timing.rearticulation_ms, undefined);
  // prepareTimeoutMs (4000) is a failure timeout and must not appear anywhere.
  assert.ok(!raw.includes('4000'), 'prepareTimeoutMs leaked into the descriptor');
  // No wiring, no secrets.
  for (const forbidden of ['gpio', 'pin', 'ssid', 'password', 'token'])
    assert.ok(!raw.toLowerCase().includes(forbidden), `descriptor leaks "${forbidden}"`);
});

test('block 0x10 segments reassemble byte-for-byte, in any order', () => {
  const frames = encodeChunks(raw);
  assert.ok(frames.length > 1, 'the reference descriptor spans several segments');
  for (const f of frames) {
    assert.ok(f.length <= 210, 'a segment must stay under the BLE-MIDI reassembly limit');
    for (let i = 1; i < f.length - 1; i++) assert.ok(f[i] < 0x80, 'payload must be 7-bit');
  }
  // Reverse order, with chunk 0 retried twice — the transfer must survive both.
  const shuffled = [...frames].reverse();
  const chunks = [...shuffled, frames[0], frames[0]].map(parseDescriptorChunk);
  const asm = assembleChunks(chunks);
  assert.equal(asm.complete, true);
  assert.equal(asm.json, raw);
});

test('the handshake announces the descriptor byte count GMB will reassemble', () => {
  const frame = [
    0xf0, 0x7d, 0x00, 0x01, 0x01, 0x02,
    ...encode32Le7(0x1234abcd),
    1, 0, 0,
    ...encode21Le7(raw.length),
    ...encode32Le7(1),
    0x03,
    0xf7,
  ];
  assert.equal(frame.length, 24, 'the handshake is exactly 24 bytes');
  const hs = parseGmbHandshake(frame);
  assert.ok(hs, 'GMB must accept the frame');
  assert.equal(hs.protocolVersion, 2);
  assert.equal(hs.instanceId, 0x1234abcd);
  assert.equal(hs.descriptorSize, raw.length);
  assert.equal(hs.revision, 1);
  assert.deepEqual(hs.flags, { httpAvailable: true, pushNotifications: true });
  // descriptor_size == the reassembled length == what HTTP would return.
  const asm = assembleChunks(encodeChunks(raw).map(parseDescriptorChunk));
  assert.equal(asm.json.length, hs.descriptorSize);
});
