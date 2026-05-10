/*
 * app.js — Slide Whistle Controller (ESP32 web UI)
 *
 * Logique frontend : WebSocket temps-réel, rendu cartes/dashboard,
 * piano (souris/touch/clavier), Web Audio synth, palette de commandes,
 * tour onboarding, MIDI Learn, presets, macros, OTA, etc.
 *
 * Conventions :
 *   - Variables globales préfixées _ (ex: _wsConnected, _learnTarget)
 *   - state = miroir local du JSON envoyé par /api/status (mis à jour
 *     toutes les 200 ms via WebSocket /ws)
 *   - Toutes les fonctions onclick="..." sont publiques au scope global
 *
 * Sections (recherche par "// ====" pour naviguer) :
 *   - PWA service worker
 *   - Macros (DSL + éditeur)
 *   - Web MIDI API (browser → ESP32)
 *   - Status bar
 *   - Quick mute / context menu
 *   - Latency, swipe
 *   - LUT presets, copy config, quick edit popover
 *   - Splash, theme, color palette, animated counters
 *   - SVG whistle, oscilloscope, heatmap
 *   - HTTP helpers, toasts, modals
 *   - WebSocket
 *   - Render (dashboard, flutes, MIDI strip, ...)
 *   - Pages : Play / Config / Pressure / Presets / MIDI log / Diag / WiFi
 *   - Onboarding tour, MIDI Learn, activity feed
 *   - Validation, auto-save
 *   - Compact mode, hash routing, filters, bulk edit
 *   - Notes per flute (localStorage)
 *   - Command palette (Ctrl+K)
 *   - Keyboard shortcuts
 *   - Init
 */

// ============================================================================
// State
// ============================================================================
let state = { sys_state: 'INIT', flutes: [], pressure: null };
let ws = null;

// ============================================================================
// PWA service worker
// ============================================================================
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/sw.js').catch(() => {});
}

// ============================================================================
// Macros — défauts + personnalisation (localStorage)
// ============================================================================
const DEFAULT_MACROS_BUILTIN = [
  {label: '🎬 Démarrage spectacle', desc: 'Homing + air ON + démo arpèges',
    actions: [
      ['POST', '/api/homing'],
      ['SLEEP', 1500],
      ['POST', '/api/pressure/start'],
      ['SLEEP', 500],
      ['POST', '/api/demo/play', {id: 2, loop: false}]
    ]},
  {label: '😴 Mode veille', desc: 'Panic + pompe stop + mute all',
    actions: [
      ['POST', '/api/panic'],
      ['POST', '/api/flutes/all', {action: 'muteAll'}],
      ['POST', '/api/pressure/stop']
    ]},
  {label: '🎯 Test complet', desc: 'Test air sur chaque flûte',
    actions: 'TEST_ALL'},
];

// User macros = stored in localStorage as plaintext lines (DSL)
function loadUserMacros() {
  const raw = localStorage.getItem('user_macros');
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (_) { return null; }
}
function saveUserMacros(arr) {
  localStorage.setItem('user_macros', JSON.stringify(arr));
}
function getMacros() {
  return loadUserMacros() || DEFAULT_MACROS_BUILTIN.map(m => ({
    label: m.label, desc: m.desc,
    script: macroToScript(m)
  }));
}

// Mini DSL : "homing", "panic", "sleep N", "pressure_start", "pressure_stop",
// "demo N [loop]", "mute_all", "unmute_all", "test_all", "homing_one N",
// "test N", "sweep N"
function parseScript(text) {
  const actions = [];
  text.split('\n').forEach(line => {
    line = line.trim();
    if (!line || line.startsWith('#')) return;
    const parts = line.split(/\s+/);
    const cmd = parts[0].toLowerCase();
    switch (cmd) {
      case 'sleep':         actions.push(['SLEEP', +parts[1] || 500]); break;
      case 'homing':        actions.push(['POST', '/api/homing']); break;
      case 'panic':         actions.push(['POST', '/api/panic']); break;
      case 'pressure_start':actions.push(['POST', '/api/pressure/start']); break;
      case 'pressure_stop': actions.push(['POST', '/api/pressure/stop']); break;
      case 'mute_all':      actions.push(['POST', '/api/flutes/all', {action:'muteAll'}]); break;
      case 'unmute_all':    actions.push(['POST', '/api/flutes/all', {action:'unmuteAll'}]); break;
      case 'demo':          actions.push(['POST', '/api/demo/play', {id:+parts[1]||0, loop:parts.includes('loop')}]); break;
      case 'demo_stop':     actions.push(['POST', '/api/demo/stop']); break;
      case 'test':          actions.push(['POST', '/api/flute/test',  {id:+parts[1]||0}]); break;
      case 'sweep':         actions.push(['POST', '/api/flute/sweep', {id:+parts[1]||0}]); break;
      case 'homing_one':    actions.push(['POST', '/api/flute/homing',{id:+parts[1]||0}]); break;
      case 'test_all':      actions.push(['TEST_ALL']); break;
    }
  });
  return actions;
}
// Réciproque pour pré-remplir les défauts en mode script
function macroToScript(m) {
  if (m.actions === 'TEST_ALL') return 'test_all';
  return (m.actions || []).map(a => {
    if (a[0] === 'SLEEP') return 'sleep ' + a[1];
    if (a[0] === 'POST' && a[1] === '/api/homing') return 'homing';
    if (a[0] === 'POST' && a[1] === '/api/panic') return 'panic';
    if (a[0] === 'POST' && a[1] === '/api/pressure/start') return 'pressure_start';
    if (a[0] === 'POST' && a[1] === '/api/pressure/stop')  return 'pressure_stop';
    if (a[0] === 'POST' && a[1] === '/api/flutes/all' && a[2]?.action === 'muteAll')   return 'mute_all';
    if (a[0] === 'POST' && a[1] === '/api/flutes/all' && a[2]?.action === 'unmuteAll') return 'unmute_all';
    if (a[0] === 'POST' && a[1] === '/api/demo/play') return 'demo ' + (a[2]?.id || 0) + (a[2]?.loop ? ' loop' : '');
    if (a[0] === 'POST' && a[1] === '/api/demo/stop') return 'demo_stop';
    return '# unknown';
  }).join('\n');
}

async function runMacro(macro) {
  toast(`Macro : ${macro.label}`, 'success');
  const actions = macro.actions || parseScript(macro.script || '');
  for (const a of actions) {
    if (a[0] === 'SLEEP') { await new Promise(r => setTimeout(r, a[1])); continue; }
    if (a[0] === 'TEST_ALL') {
      for (const f of (state.flutes || [])) {
        if (!f.enabled) continue;
        await postJson('/api/flute/test', {id: f.id});
        await new Promise(r => setTimeout(r, 2000));
      }
      continue;
    }
    if (a[0] === 'POST') {
      if (a[2]) await postJson(a[1], a[2]);
      else      await postNoBody(a[1]);
    }
  }
}
function renderMacros() {
  const bar = document.getElementById('macroBar');
  if (!bar) return;
  const macros = getMacros();
  bar.innerHTML = macros.map((m, i) =>
    `<button class="macro-chip" data-idx="${i}" title="${m.desc || ''}">${m.label}</button>`
  ).join('');
  bar.querySelectorAll('.macro-chip').forEach(c => c.onclick = () => runMacro(macros[+c.dataset.idx]));
}
function openMacroEditor() {
  document.getElementById('macroEditor').classList.add('show');
  renderMacroList();
}
function renderMacroList() {
  const macros = getMacros();
  const box = document.getElementById('macroList');
  box.innerHTML = macros.map((m, i) => `
    <div class="macro-list-item">
      <div style="flex:1;display:flex;flex-direction:column;gap:4px">
        <input value="${m.label}" data-idx="${i}" data-field="label" placeholder="Nom">
        <input value="${m.desc || ''}" data-idx="${i}" data-field="desc" placeholder="Description">
        <textarea rows="3" data-idx="${i}" data-field="script" placeholder="Script">${m.script || ''}</textarea>
      </div>
      <button class="btn btn-danger" onclick="deleteMacro(${i})" style="margin-left:8px">🗑</button>
    </div>
  `).join('');
  // Bind editing
  box.querySelectorAll('input, textarea').forEach(inp => {
    inp.addEventListener('input', () => {
      const arr = getMacros();
      arr[+inp.dataset.idx][inp.dataset.field] = inp.value;
      saveUserMacros(arr);
      renderMacros();
    });
  });
}
function addMacro() {
  const arr = getMacros();
  arr.push({label: '✨ Ma macro', desc: '', script: 'homing\nsleep 1000'});
  saveUserMacros(arr);
  renderMacroList();
  renderMacros();
}
function deleteMacro(i) {
  const arr = getMacros();
  arr.splice(i, 1);
  saveUserMacros(arr);
  renderMacroList();
  renderMacros();
}
function resetMacros() {
  localStorage.removeItem('user_macros');
  renderMacroList();
  renderMacros();
  toast('Macros réinitialisées');
}

// ============================================================================
// Web MIDI API (browser ↔ external USB controller → ESP32)
// ============================================================================
let _webMidi = null;
let _webMidiEnabled = false;
async function enableWebMidi() {
  if (!navigator.requestMIDIAccess) {
    toast('Web MIDI non supporté par ce navigateur', 'warn');
    return;
  }
  try {
    _webMidi = await navigator.requestMIDIAccess({sysex: false});
    _webMidiEnabled = true;
    document.getElementById('sbWebMidi').textContent = 'Web MIDI ✓';
    bindWebMidiInputs();
    _webMidi.onstatechange = bindWebMidiInputs;
    toast('Web MIDI activé');
  } catch (e) {
    toast('Permission Web MIDI refusée', 'error');
  }
}
function bindWebMidiInputs() {
  if (!_webMidi) return;
  const list = document.getElementById('webMidiDevices');
  const status = document.getElementById('webMidiStatus');
  const inputs = Array.from(_webMidi.inputs.values());
  if (!inputs.length) {
    list.innerHTML = '<div class="device-item"><span>Aucun contrôleur connecté</span><span class="led off"></span></div>';
    if (status) status.textContent = 'Branche un clavier USB MIDI puis clique Activer.';
    return;
  }
  list.innerHTML = inputs.map(inp =>
    `<div class="device-item">
       <span><strong>${inp.name}</strong> <span class="small">— ${inp.manufacturer || 'inconnu'}</span></span>
       <span class="led ${inp.state === 'connected' ? 'green' : 'red'}"></span>
     </div>`
  ).join('');
  inputs.forEach(inp => { inp.onmidimessage = onWebMidiMessage; });
  if (status) status.textContent = `${inputs.length} contrôleur(s) bindé(s).`;
}
function onWebMidiMessage(e) {
  const [status, d1, d2] = e.data;
  const cmd = status & 0xF0, ch = (status & 0x0F) + 1;
  // Synth navigateur immédiat (latence quasi-nulle)
  if (cmd === 0x90 && d2 > 0) synthNoteOn(d1);
  else if (cmd === 0x80 || (cmd === 0x90 && d2 === 0)) synthNoteOff(d1);
  // Relais ESP32 si activé
  if (!document.getElementById('webMidiForward').checked) return;
  // Choix flûte cible : la première qui accepte ce canal
  const target = (state.flutes || []).find(f =>
    f.enabled && (f.midi_channel === 0 || f.midi_channel === ch));
  if (!target) return;
  if (cmd === 0x90 && d2 > 0) {
    postJson('/api/flute/note', {id: target.id, note: d1, velocity: d2, on: true});
  } else if (cmd === 0x80 || (cmd === 0x90 && d2 === 0)) {
    postJson('/api/flute/note', {id: target.id, note: d1, on: false});
  }
  // CC, PB, AT : pour l'instant on laisse l'ESP32 les recevoir directement
  // via Serial/BLE MIDI si raccordé. Pour étendre, ajouter ici les endpoints.
}

// ============================================================================
// Status bar (footer)
// ============================================================================
function updateStatusBar() {
  document.getElementById('sbState').textContent = state.sys_state || '—';
  setAnimCounter(document.getElementById('sbMidi'), state.midi_count || 0);
  document.getElementById('sbUptime').textContent = fmtUptime(state.uptime_ms || 0);
  if (refreshHealth._heapFree) {
    document.getElementById('sbHeap').textContent = (refreshHealth._heapFree / 1024).toFixed(0) + 'KB';
  }
  // Reflète l'état WS du connDot dans la barre
  const dot = document.getElementById('sbConnDot');
  if (dot) {
    dot.textContent = _wsConnected ? '●' : '○';
    dot.style.color = _wsConnected ? 'var(--green)' : 'var(--orange)';
  }
}
setInterval(updateStatusBar, 1000);

// ============================================================================
// Latency indicator (ping)
// ============================================================================
let _lastPingMs = 0;
async function pingLatency() {
  const t0 = performance.now();
  try {
    await fetch('/api/status', {method: 'GET', cache: 'no-store'});
    const dt = performance.now() - t0;
    _lastPingMs = dt;
    const el = document.getElementById('sbLatency');
    if (el) {
      el.textContent = dt.toFixed(0) + 'ms';
      el.classList.toggle('high', dt > 200);
    }
  } catch (_) {
    document.getElementById('sbLatency').textContent = 'KO';
  }
}
setInterval(pingLatency, 4000);
setTimeout(pingLatency, 1500);

// ============================================================================
// Touch swipe entre pages (mobile)
// ============================================================================
(function() {
  let sx = 0, sy = 0, t0 = 0;
  document.addEventListener('touchstart', e => {
    if (e.touches.length !== 1) return;
    sx = e.touches[0].clientX; sy = e.touches[0].clientY; t0 = Date.now();
  }, {passive: true});
  document.addEventListener('touchend', e => {
    if (e.changedTouches.length !== 1) return;
    const dx = e.changedTouches[0].clientX - sx;
    const dy = e.changedTouches[0].clientY - sy;
    const dt = Date.now() - t0;
    if (dt > 500) return;
    if (Math.abs(dx) < 80 || Math.abs(dy) > 60) return;
    // Swipe horizontal
    const buttons = Array.from(document.querySelectorAll('nav button'));
    const cur = buttons.findIndex(b => b.classList.contains('active'));
    if (cur < 0) return;
    const next = dx < 0 ? Math.min(buttons.length - 1, cur + 1) : Math.max(0, cur - 1);
    if (next !== cur) {
      document.body.classList.add(dx < 0 ? 'swipe-left' : 'swipe-right');
      setTimeout(() => document.body.classList.remove('swipe-left', 'swipe-right'), 200);
      buttons[next].click();
    }
  }, {passive: true});
})();

// ============================================================================
// LUT presets (linear / 1/f / exp / reverse)
// ============================================================================
async function applyLutPreset() {
  const sel = document.getElementById('lutPresetSel');
  const preset = sel.value;
  if (!preset) return;
  if (!await confirmDialog(`Remplacer la LUT actuelle par le preset "${preset}" ?`,
                            'Preset LUT', 'Appliquer')) {
    sel.value = ''; return;
  }
  const id = +document.getElementById('cfgFluteSel').value;
  const f  = (state.flutes || []).find(x => x.id === id);
  if (!f) return;
  const travel = f.slider_travel_mm || 300;
  const span = f.note_max - f.note_min;
  const lut = [];
  for (let i = 0; i <= span; i++) {
    let pos;
    const r = i / span;
    if (preset === 'linear')      pos = r * travel;
    else if (preset === 'real')   pos = travel * (1 - 1 / (1 + r * 1.5));   // approche 1/f
    else if (preset === 'exp')    pos = travel * Math.pow(r, 0.6);
    else if (preset === 'reverse')pos = (1 - r) * travel;
    else                          pos = r * travel;
    lut.push({note: f.note_min + i, position: pos});
  }
  await postJson('/api/flute/lut', {id, lut});
  toast(`Preset LUT "${preset}" appliqué`);
  loadFluteCfg();
  sel.value = '';
}

// ============================================================================
// Copy config from another flute
// ============================================================================
function refreshCopyFromOptions() {
  const sel = document.getElementById('cfgCopyFrom');
  if (!sel) return;
  const cur = +document.getElementById('cfgFluteSel').value;
  sel.innerHTML = '<option value="">Copier config depuis...</option>' +
    (state.flutes || []).filter(f => f.id !== cur).map(f =>
      `<option value="${f.id}">#${f.id} ${f.name}</option>`).join('');
}
async function copyConfigFrom() {
  const src = +document.getElementById('cfgCopyFrom').value;
  const dst = +document.getElementById('cfgFluteSel').value;
  document.getElementById('cfgCopyFrom').value = '';
  if (!src && src !== 0) return;
  if (src === dst) return;
  if (!await confirmDialog(`Copier la config de #${src} sur #${dst} ?\n(ne copie pas le canal MIDI ni la plage de notes ni le custom_name)`,
                            'Copier config', 'Copier')) return;
  showLoading();
  try {
    const c = await getJson(`/api/flute?id=${src}`);
    if (!c) return;
    // On copie tout sauf : id, midi_channel, note_min, note_max, custom_name
    const body = {
      id: dst,
      speed_mm_s:    c.speed_mm_s,
      accel_mm_s2:   c.accel_mm_s2,
      pwm_full:      c.pwm_full,
      pwm_hold:      c.pwm_hold,
      wait_delay_ms: c.wait_delay_ms,
      legato_ms:     c.legato_ms,
      velocity_curve: c.velocity_curve,
      cc_breath:     c.cc_breath,
      cc_expression: c.cc_expression,
      cc_volume:     c.cc_volume,
      cc_vibrato:    c.cc_vibrato,
      cc_sustain:    c.cc_sustain,
      transpose:     c.transpose,
      use_lut:       c.use_lut
    };
    await postJson('/api/flute', body);
    // Copier la LUT séparément
    if (c.lut) await postJson('/api/flute/lut', {id: dst, lut: c.lut});
    toast(`Config copiée de #${src} vers #${dst}`);
    loadFluteCfg();
  } finally { hideLoading(); }
}

// ============================================================================
// Quick edit popover sur les cartes de flûte
// ============================================================================
function quickEditFlute(id, ev) {
  ev.stopPropagation();
  const pop = document.getElementById('quickPopover');
  const f = (state.flutes || []).find(x => x.id === id);
  if (!f) return;
  pop.style.display = 'block';
  pop.innerHTML = `
    <h5>#${f.id} ${f.name} <span style="float:right;cursor:pointer;color:var(--muted)" onclick="closeQuickEdit()">✕</span></h5>
    <label>Canal MIDI</label>
    <input type="number" min="0" max="16" id="qpChan" value="${f.midi_channel}">
    <div class="row2" style="margin-top:6px">
      <div><label>Note min</label><input type="number" id="qpNmin" min="0" max="127" value="${f.note_min}"></div>
      <div><label>Note max</label><input type="number" id="qpNmax" min="0" max="127" value="${f.note_max}"></div>
    </div>
    <div class="btn-row" style="margin-top:8px">
      <button class="btn btn-primary" onclick="saveQuickEdit(${f.id})">💾 Appliquer</button>
      <button class="btn" onclick="navigateTo('config'); document.getElementById('cfgFluteSel').value=${f.id}; loadFluteCfg(); closeQuickEdit()">↗ Détails</button>
    </div>
  `;
  // Position popover proche de la cible
  const r = ev.target.getBoundingClientRect();
  const top  = Math.min(window.innerHeight - 240, r.bottom + window.scrollY + 6);
  const left = Math.min(window.innerWidth - 320, r.left + window.scrollX);
  pop.style.top  = top + 'px';
  pop.style.left = left + 'px';
}
function closeQuickEdit() {
  document.getElementById('quickPopover').style.display = 'none';
}
async function saveQuickEdit(id) {
  await postJson('/api/flute', {
    id,
    midi_channel: +document.getElementById('qpChan').value,
    note_min:     +document.getElementById('qpNmin').value,
    note_max:     +document.getElementById('qpNmax').value
  });
  toast('Modifications appliquées');
  closeQuickEdit();
}
// Click extérieur ferme
document.addEventListener('click', e => {
  const pop = document.getElementById('quickPopover');
  if (pop?.style.display === 'block' && !pop.contains(e.target)) closeQuickEdit();
});

// ============================================================================
// Quick mute par canal (click droit sur cellule du strip)
// ============================================================================
function setupChannelStripContextMenu() {
  document.addEventListener('contextmenu', e => {
    const cell = e.target.closest('.ch-cell');
    if (!cell) return;
    e.preventDefault();
    const ch = +cell.querySelector('.ch-num')?.textContent;
    if (!ch) return;
    const f = (state.flutes || []).find(x => x.midi_channel === ch);
    if (!f) { toast(`Aucune flûte n'écoute le canal ${ch}`, 'warn'); return; }
    const newMuted = !f.muted;
    postJson('/api/flute', {id: f.id, muted: newMuted});
    toast(`${f.name} : ${newMuted ? 'mutée' : 'unmutée'}`);
  });
}

// ============================================================================
// Navigation
// ============================================================================
function updateTabIndicator() {
  const nav = document.querySelector('nav');
  const active = document.querySelector('nav button.active');
  if (!nav || !active) return;
  const navRect = nav.getBoundingClientRect();
  const r = active.getBoundingClientRect();
  nav.style.setProperty('--ind-left', (r.left - navRect.left + nav.scrollLeft) + 'px');
  nav.style.setProperty('--ind-width', r.width + 'px');
  nav.style.cssText = nav.style.cssText;       // force reflow
}
// Sync via CSS variables (custom selector)
const _styleSheet = document.createElement('style');
_styleSheet.textContent = 'nav::after { left: var(--ind-left, 0); width: var(--ind-width, 0); }';
document.head.appendChild(_styleSheet);
window.addEventListener('resize', updateTabIndicator);

document.querySelectorAll('nav button').forEach(b => b.onclick = () => {
  document.querySelectorAll('nav button').forEach(x => x.classList.remove('active'));
  document.querySelectorAll('.page').forEach(x => x.classList.remove('active'));
  b.classList.add('active');
  document.getElementById('page-' + b.dataset.page).classList.add('active');
  updateTabIndicator();
  // Sync URL hash (sans déclencher hashchange en boucle)
  if (location.hash !== '#' + b.dataset.page) {
    history.replaceState(null, '', '#' + b.dataset.page);
  }
  if (b.dataset.page === 'config')   loadFluteCfg();
  if (b.dataset.page === 'pressure') loadPressureCfg();
  if (b.dataset.page === 'wifi')     loadWifi();
  if (b.dataset.page === 'midi')     loadMidiLog();
  if (b.dataset.page === 'presets')  loadPresets();
  if (b.dataset.page === 'diag')     loadDiag();
  if (b.dataset.page === 'play')     loadDemoMelodies();
});
// Au chargement, restaurer la page depuis le hash
window.addEventListener('load', () => {
  const p = location.hash.replace(/^#/, '');
  if (p && document.getElementById('page-' + p)) navigateTo(p);
});

// ============================================================================
// Onboarding tour (first-run, désactivable)
// ============================================================================
// Tour court (5 étapes) — focus sur les actions essentielles d'un nouvel
// utilisateur. Chaque étape indique aussi quoi faire ensuite.
const TOUR_STEPS = [
  {
    target: 'header h1',
    title: '🎼 Bienvenue',
    msg: 'Cette interface pilote tes slide whistles MIDI. En 5 étapes, tu auras tout en main. Rappel : Ctrl+K à tout moment pour la recherche, ⌨ pour les raccourcis.'
  },
  {
    target: '#sysBadge',
    title: '1. État système',
    msg: 'Doit afficher READY (vert). Si HOMING : patiente. Si ERROR : vérifie les endstops via la page Diag.'
  },
  {
    target: 'nav button[data-page="play"]',
    title: '2. Jouer',
    msg: 'Démarre ici : active la preview audio (🔊 dans le header), puis clique sur le piano. Tu peux aussi lancer une démo pré-enregistrée.'
  },
  {
    target: 'nav button[data-page="config"]',
    title: '3. Régler',
    msg: 'Pour ajuster une flûte : calibration LUT (drag les points), mapping CC (avec MIDI Learn 🎯), transpose, vélocité curve. Active "mode avancé" pour révéler les paramètres techniques.'
  },
  {
    target: 'nav button[data-page="help"]',
    title: '4. Aide à tout moment',
    msg: 'Cette page contient le guide complet, la table des CC standards, le dépannage. Tu peux aussi imprimer la doc.'
  }
];
let _tourStep = 0;
function startTour() {
  _tourStep = 0;
  document.getElementById('tourRoot').classList.add('show');
  showTourStep();
}
function endTour() {
  document.getElementById('tourRoot').classList.remove('show');
  localStorage.setItem('tour_done', '1');
}
function showTourStep() {
  const step = TOUR_STEPS[_tourStep];
  if (!step) { endTour(); return; }
  const target = document.querySelector(step.target);
  if (!target) { tourNext(); return; }

  document.getElementById('tourTitle').textContent = step.title;
  document.getElementById('tourMsg').textContent   = step.msg;
  document.getElementById('tourStepLabel').textContent = `${_tourStep + 1}/${TOUR_STEPS.length}`;
  const btn = document.getElementById('tourNext');
  btn.textContent = _tourStep === TOUR_STEPS.length - 1 ? 'Terminer' : 'Suivant →';

  // Position popover près de la cible
  const r = target.getBoundingClientRect();
  const pop = document.getElementById('tourPopover');
  const top = Math.min(window.innerHeight - 200, r.bottom + 8);
  const left = Math.min(window.innerWidth - 340, Math.max(8, r.left));
  pop.style.top  = top + 'px';
  pop.style.left = left + 'px';

  // Découpe le mask pour révéler la cible
  const mask = document.getElementById('tourMask');
  const pad = 6;
  const x = r.left - pad, y = r.top - pad, w = r.width + 2*pad, h = r.height + 2*pad;
  mask.style.clipPath =
    `polygon(0 0, 0 100%, ${x}px 100%, ${x}px ${y}px, ${x+w}px ${y}px, ${x+w}px ${y+h}px, ${x}px ${y+h}px, ${x}px 100%, 100% 100%, 100% 0)`;
}
function tourNext() {
  _tourStep++;
  if (_tourStep >= TOUR_STEPS.length) { endTour(); toast('Bon voyage ! 🎵'); return; }
  showTourStep();
}
window.addEventListener('resize', () => {
  if (document.getElementById('tourRoot').classList.contains('show')) showTourStep();
});

// ============================================================================
// MIDI Learn — clique un champ CC, joue un CC sur ton contrôleur, le numéro
// est rempli automatiquement
// ============================================================================
let _learnTarget = null;
function midiLearn(inputId) {
  const inp = document.getElementById(inputId);
  if (!inp) return;
  if (_learnTarget) _learnTarget.classList.remove('learning');
  _learnTarget = inp;
  inp.classList.add('learning');
  // Badge global persistant tant que learn actif
  const badge = document.getElementById('learnBadge');
  if (badge) badge.style.display = 'inline-block';
  inp.scrollIntoView({behavior: 'smooth', block: 'center'});
  toast('🎯 Envoie un CC sur ton contrôleur (Esc pour annuler)', 'warn', 8000);
}
function endMidiLearn(reason) {
  if (!_learnTarget) return;
  _learnTarget.classList.remove('learning');
  _learnTarget = null;
  const badge = document.getElementById('learnBadge');
  if (badge) badge.style.display = 'none';
  if (reason) toast(reason);
}
// Pris en compte dans onWsMessage : si _learnTarget actif et message CC reçu,
// on remplit le champ. On utilise l'event log MIDI.
let _lastSeenLogIndex = 0;
async function pollForLearn() {
  if (!_learnTarget) return;
  const log = await getJson('/api/midi/log');
  if (!log || !log.length) return;
  // Cherche un CC plus récent que _lastSeenLogIndex (en pratique : t_rel_ms le plus petit > 0 = le plus récent)
  const recent = log.filter(e => e.type === 'CC').sort((a, b) => a.t_rel_ms - b.t_rel_ms);
  if (recent.length && recent[0].t_rel_ms < 4000) {
    const cc = recent[0].data1;
    const target = _learnTarget;
    target.value = cc;
    target.dispatchEvent(new Event('input', {bubbles: true}));
    endMidiLearn(`CC${cc} appris pour ${target.id.replace('cfgCc','')}`);
  }
}
setInterval(pollForLearn, 600);

// Échap = annuler le learn
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') {
    if (_learnTarget) endMidiLearn('MIDI Learn annulé');
    document.getElementById('shortcutsModal')?.classList.remove('show');
    document.getElementById('bulkModal')?.classList.remove('show');
    document.getElementById('macroEditor')?.classList.remove('show');
  }
});

// ============================================================================
// Activity feed (timeline sur dashboard)
// ============================================================================
const _activityRing = [];
function pushActivity(icon, text, level='') {
  _activityRing.unshift({t: Date.now(), icon, text, level});
  if (_activityRing.length > 30) _activityRing.pop();
}
function renderActivity() {
  const box = document.getElementById('activityFeed');
  if (!box) return;
  if (!_activityRing.length) return;
  const fmt = ts => {
    const d = (Date.now() - ts);
    if (d < 1000) return 'à l\'instant';
    if (d < 60000) return Math.floor(d/1000) + 's';
    if (d < 3600000) return Math.floor(d/60000) + 'm';
    return Math.floor(d/3600000) + 'h';
  };
  box.innerHTML = _activityRing.map(a =>
    `<div class="activity-row">
       <span class="activity-time">${fmt(a.t)}</span>
       <span class="activity-icon">${a.icon}</span>
       <span class="${a.level==='error'?'health-error':a.level==='warn'?'health-warn':''}">${a.text}</span>
     </div>`
  ).join('');
}
setInterval(renderActivity, 1000);

// Détection events depuis state diff (note edges, sys state changes, fault transitions)
let _prevSysState = null;
let _prevFault    = null;
function detectActivity() {
  if (state.sys_state !== _prevSysState && _prevSysState !== null) {
    pushActivity('⚙', `État système : ${state.sys_state}`,
                 state.sys_state === 'ERROR' ? 'error' : '');
  }
  _prevSysState = state.sys_state;
  if (state.pressure?.fault !== _prevFault && _prevFault !== null) {
    if (state.pressure.fault !== 'none') {
      pushActivity('⚠', `Défaut pression : ${state.pressure.fault}`, 'error');
    } else {
      pushActivity('✓', 'Défaut pression résolu', '');
    }
  }
  if (state.pressure) _prevFault = state.pressure.fault;
}

// Hook : appelé dans render() → trackNoteEdges() suit déjà les note ON
const _origTrackNoteEdges = trackNoteEdges;
trackNoteEdges = function() {
  (state.flutes||[]).forEach(f => {
    const prev = _lastNoteState[f.id] || {note: 0, active: false};
    if (f.note_active && (!prev.active || prev.note !== f.last_note)) {
      pushActivity('♪', `${f.name} → ${noteName(f.last_note)} (${noteFreq(f.last_note).toFixed(0)}Hz)`);
    }
  });
  _origTrackNoteEdges();
  detectActivity();
};

// ============================================================================
// Nav badges
// ============================================================================
function updateNavBadges() {
  // Compte les flûtes en fault (non homé tout en étant enabled)
  const faultCount = (state.flutes||[]).filter(f => f.enabled && !f.homed).length;
  setBadge('flutes', faultCount, faultCount ? 'warn' : '');
  // Pression en SAFETY → badge sur Pression
  const pErr = state.pressure && state.pressure.fault && state.pressure.fault !== 'none';
  setBadge('pressure', pErr ? '!' : '', pErr ? '' : '');
  // Sys ERROR
  setBadge('diag', state.sys_state === 'ERROR' ? '!' : '', '');
}
function setBadge(page, text, type='') {
  const btn = document.querySelector(`nav button[data-page="${page}"]`);
  if (!btn) return;
  let b = btn.querySelector('.nav-badge');
  if (!text) { if (b) b.remove(); return; }
  if (!b) { b = document.createElement('span'); b.className = 'nav-badge'; btn.appendChild(b); }
  b.textContent = text;
  b.className = 'nav-badge ' + (type || 'info');
}
setInterval(updateNavBadges, 1000);

// ============================================================================
// Inline form validation (limites client-side avant POST)
// ============================================================================
const VALIDATION_RULES = {
  cfgChan:     {min:0,  max:16,  msg:'Canal MIDI 0-16'},
  cfgNmin:     {min:0,  max:127, msg:'Note 0-127'},
  cfgNmax:     {min:0,  max:127, msg:'Note 0-127'},
  cfgSpeed:    {min:5,  max:500, msg:'Vitesse 5-500 mm/s'},
  cfgAccel:    {min:50, max:5000,msg:'Accel 50-5000 mm/s²'},
  cfgPwmFull:  {min:0,  max:255, msg:'PWM 0-255'},
  cfgPwmHold:  {min:0,  max:255, msg:'PWM 0-255'},
  cfgWait:     {min:0,  max:2000,msg:'Wait 0-2000 ms'},
  cfgLegato:   {min:0,  max:2000,msg:'Legato 0-2000 ms'},
  cfgCcBreath: {min:0,  max:127, msg:'CC 0-127'},
  cfgCcExpr:   {min:0,  max:127, msg:'CC 0-127'},
  cfgCcVolume: {min:0,  max:127, msg:'CC 0-127'},
  cfgCcVibrato:{min:0,  max:127, msg:'CC 0-127'},
  cfgCcSustain:{min:0,  max:127, msg:'CC 0-127'},
  cfgTranspose:{min:-36,max:36,  msg:'Transpose ±36'},
};
function validateField(id) {
  const inp = document.getElementById(id);
  const rule = VALIDATION_RULES[id];
  if (!inp || !rule) return true;
  const v = +inp.value;
  const ok = !isNaN(v) && v >= rule.min && v <= rule.max;
  inp.classList.toggle('invalid', !ok);
  // Hint inline
  let hint = inp.parentNode.querySelector('.field-hint');
  if (!ok) {
    if (!hint) {
      hint = document.createElement('div'); hint.className = 'field-hint';
      inp.parentNode.insertBefore(hint, inp.nextSibling);
    }
    hint.textContent = '⚠ ' + rule.msg;
  } else if (hint) hint.remove();
  return ok;
}
function validateAllConfigFields() {
  let allOk = true;
  Object.keys(VALIDATION_RULES).forEach(id => { if (!validateField(id)) allOk = false; });
  return allOk;
}

// ============================================================================
// Auto-save (debounced) — toggle avec checkbox
// ============================================================================
let _autoSaveTimer = null;
function maybeAutoSave() {
  const cb = document.getElementById('cfgAutoSave');
  if (!cb || !cb.checked) return;
  clearTimeout(_autoSaveTimer);
  _autoSaveTimer = setTimeout(() => {
    if (validateAllConfigFields()) saveFluteCfg();
  }, 800);
}

// ============================================================================
// Compact mode
// ============================================================================
function toggleCompact() {
  document.body.classList.toggle('compact');
  const on = document.body.classList.contains('compact');
  localStorage.setItem('compact', on ? '1' : '0');
  toast(on ? 'Vue compacte' : 'Vue détaillée');
}
if (localStorage.getItem('compact') === '1') document.body.classList.add('compact');

// ============================================================================
// URL hash routing
// ============================================================================
function navigateTo(page) {
  const btn = document.querySelector(`nav button[data-page="${page}"]`);
  if (btn) btn.click();
}
window.addEventListener('hashchange', () => {
  const p = location.hash.replace(/^#/, '');
  if (p && document.getElementById('page-' + p)) navigateTo(p);
});

// ============================================================================
// Filter chips (Flutes page)
// ============================================================================
let _fluteFilter = 'all';
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('#fluteFilters .chip[data-filter]').forEach(c => {
    c.addEventListener('click', () => {
      document.querySelectorAll('#fluteFilters .chip[data-filter]').forEach(x => x.classList.remove('active'));
      c.classList.add('active');
      _fluteFilter = c.dataset.filter;
      render();
    });
  });
});
function fluteMatchesFilter(f) {
  switch (_fluteFilter) {
    case 'active':    return f.enabled && !f.muted;
    case 'muted':     return f.muted;
    case 'disabled':  return !f.enabled;
    case 'not_homed': return f.enabled && !f.homed;
    case 'playing':   return f.note_active || f.air_open;
    default:          return true;
  }
}

// ============================================================================
// Bulk edit
// ============================================================================
async function applyBulk() {
  const field = document.getElementById('bulkField').value;
  const value = +document.getElementById('bulkValue').value;
  if (!await confirmDialog(`Appliquer ${field}=${value} à toutes les flûtes activées ?`,
                            'Édition groupée', 'Appliquer')) return;
  showLoading();
  try {
    for (const f of (state.flutes || [])) {
      if (!f.enabled) continue;
      const body = {id: f.id};
      body[field] = value;
      await postJson('/api/flute', body);
    }
    toast(`${field} mis à ${value} pour toutes les flûtes`, 'success');
    document.getElementById('bulkModal').classList.remove('show');
  } finally { hideLoading(); }
}

// ============================================================================
// Notes per flute (localStorage)
// ============================================================================
function getFluteNote(id) { return localStorage.getItem(`flute_notes_${id}`) || ''; }
function setFluteNote(id, text) {
  if (text) localStorage.setItem(`flute_notes_${id}`, text);
  else      localStorage.removeItem(`flute_notes_${id}`);
}

// ============================================================================
// Command palette (Ctrl+K)
// ============================================================================
const CMD_ITEMS = [
  // Navigation
  {label: 'Aller au Dashboard',     desc: 'Vue d\'ensemble',                      ctx: 'page',   action: () => navigateTo('dashboard')},
  {label: 'Aller aux Flûtes',       desc: 'Gestion par instrument',               ctx: 'page',   action: () => navigateTo('flutes')},
  {label: 'Aller à Jouer',          desc: 'Piano + démo',                         ctx: 'page',   action: () => navigateTo('play')},
  {label: 'Aller à Pression',       desc: 'Pompe / réservoir',                    ctx: 'page',   action: () => navigateTo('pressure')},
  {label: 'Aller à MIDI log',       desc: 'Activité MIDI temps-réel',             ctx: 'page',   action: () => navigateTo('midi')},
  {label: 'Aller aux Réglages',     desc: 'Config détaillée par flûte',           ctx: 'page',   action: () => navigateTo('config')},
  {label: 'Aller aux Presets',      desc: 'Snapshots de config',                  ctx: 'page',   action: () => navigateTo('presets')},
  {label: 'Aller à Diag',           desc: 'Système, OTA, backup',                 ctx: 'page',   action: () => navigateTo('diag')},
  {label: 'Aller à WiFi',           desc: 'Réseaux',                              ctx: 'page',   action: () => navigateTo('wifi')},
  {label: 'Aller à Aide',           desc: 'Documentation',                        ctx: 'page',   action: () => navigateTo('help')},
  // Actions globales
  {label: 'Homing global',          desc: 'Re-calibre toutes les flûtes',         ctx: 'action', action: () => { postNoBody('/api/homing'); toast('Homing'); }},
  {label: 'Panic',                  desc: 'Coupure d\'urgence',                   ctx: 'action', action: () => { postNoBody('/api/panic');  toast('Panic !', 'warn'); }},
  {label: 'Mute toutes',            desc: 'Silence sur toutes les flûtes',        ctx: 'action', action: () => { postJson('/api/flutes/all', {action:'muteAll'}); }},
  {label: 'Unmute toutes',          desc: '',                                     ctx: 'action', action: () => { postJson('/api/flutes/all', {action:'unmuteAll'}); }},
  {label: 'Activer toutes',         desc: '',                                     ctx: 'action', action: () => { postJson('/api/flutes/all', {action:'enableAll'}); }},
  {label: 'Démarrer pompe',         desc: 'Démarre la régulation pression',       ctx: 'action', action: () => { postNoBody('/api/pressure/start'); }},
  {label: 'Arrêter pompe',          desc: '',                                     ctx: 'action', action: () => { postNoBody('/api/pressure/stop');  }},
  {label: 'Reset défaut pression',  desc: 'Sortir de SAFETY',                     ctx: 'action', action: () => { postNoBody('/api/pressure/reset'); }},
  {label: 'Bascule preview audio',  desc: 'Synth navigateur',                     ctx: 'action', action: () => toggleSynth()},
  {label: 'Bascule thème',          desc: 'Clair/sombre',                         ctx: 'action', action: () => toggleTheme()},
  {label: 'Vue compacte/détaillée', desc: 'Réduit la taille des cartes',          ctx: 'action', action: () => toggleCompact()},
  {label: 'Édition groupée',        desc: 'Applique un réglage à toutes',         ctx: 'action', action: () => document.getElementById('bulkModal').classList.add('show')},
  {label: 'Télécharger backup JSON',desc: 'Export complet',                       ctx: 'action', action: () => window.open('/api/backup', '_blank')},
  {label: 'Redémarrer ESP32',       desc: 'Reboot logiciel',                      ctx: 'action', action: () => rebootSystem()},
];
let _cmdSel = 0;
function openCmdPalette() {
  const root = document.getElementById('cmdRoot');
  root.classList.add('show');
  const inp = document.getElementById('cmdInput');
  inp.value = '';
  renderCmdResults('');
  setTimeout(() => inp.focus(), 30);
}
function closeCmdPalette() {
  document.getElementById('cmdRoot').classList.remove('show');
}
function renderCmdResults(q) {
  q = q.toLowerCase().trim();
  // Fuzzy : tous les chars de q dans label ou desc dans l'ordre
  const items = CMD_ITEMS.filter(it => {
    if (!q) return true;
    const txt = (it.label + ' ' + it.desc).toLowerCase();
    let i = 0;
    for (const ch of q) { i = txt.indexOf(ch, i); if (i < 0) return false; i++; }
    return true;
  });
  _cmdSel = 0;
  const box = document.getElementById('cmdResults');
  if (!items.length) { box.innerHTML = '<div class="cmd-item"><span class="desc">Aucun résultat</span></div>'; return; }
  box.innerHTML = items.map((it, i) => `
    <div class="cmd-item ${i===0?'sel':''}" data-idx="${i}">
      <span><strong>${it.label}</strong> <span class="desc">${it.desc}</span></span>
      <span class="ctx">${it.ctx}</span>
    </div>
  `).join('');
  box._items = items;
  box.querySelectorAll('.cmd-item').forEach(el => {
    el.addEventListener('click', () => { items[+el.dataset.idx].action(); closeCmdPalette(); });
  });
}
document.addEventListener('keydown', e => {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault(); openCmdPalette();
    return;
  }
  if (document.getElementById('cmdRoot').classList.contains('show')) {
    const box = document.getElementById('cmdResults');
    const items = box._items || [];
    if (e.key === 'Escape') { closeCmdPalette(); return; }
    if (e.key === 'ArrowDown') { _cmdSel = Math.min(items.length - 1, _cmdSel + 1); }
    else if (e.key === 'ArrowUp') { _cmdSel = Math.max(0, _cmdSel - 1); }
    else if (e.key === 'Enter') {
      e.preventDefault();
      if (items[_cmdSel]) { items[_cmdSel].action(); closeCmdPalette(); }
      return;
    } else return;
    box.querySelectorAll('.cmd-item').forEach((el, i) => el.classList.toggle('sel', i === _cmdSel));
    e.preventDefault();
  }
});
document.getElementById('cmdInput')?.addEventListener('input', e => renderCmdResults(e.target.value));
document.getElementById('cmdRoot')?.addEventListener('click', e => {
  if (e.target.id === 'cmdRoot') closeCmdPalette();
});

// ============================================================================
// Splash screen
// ============================================================================
function hideSplash() {
  const s = document.getElementById('splash');
  if (s) { s.classList.add('hide'); setTimeout(() => s.remove(), 500); }
}

// ============================================================================
// Color palette per flute (consistent across UI, override par localStorage)
// ============================================================================
const FLUTE_COLORS = ['#00d084','#5b9bf2','#ff7e5f','#c779e8','#ffaa00','#34d4c5','#e91e63','#7bd131'];
function fluteColor(id) {
  const override = localStorage.getItem(`flute_color_${id}`);
  return override || FLUTE_COLORS[id % FLUTE_COLORS.length];
}
function setFluteColor(id, hex) {
  if (hex) localStorage.setItem(`flute_color_${id}`, hex);
  else     localStorage.removeItem(`flute_color_${id}`);
  render();
}

// ============================================================================
// Animated counter — flash quand la valeur change
// ============================================================================
function setAnimCounter(el, val) {
  if (!el) return;
  const prev = el.textContent;
  const cur = String(val);
  if (prev !== cur) {
    el.textContent = cur;
    el.classList.add('flash');
    setTimeout(() => el.classList.remove('flash'), 250);
  }
}

// ============================================================================
// Slide-whistle SVG (inline, animée par CSS variable)
// ============================================================================
function whistleSvg(color, posPct, playing) {
  const cx = 22 + posPct * 1.50;     // x position du curseur (22..172)
  const glow = playing ? 'filter:drop-shadow(0 0 4px ' + color + ')' : '';
  return `
    <svg class="whistle-svg" viewBox="0 0 200 36" xmlns="http://www.w3.org/2000/svg" style="${glow}">
      <!-- corps tube -->
      <rect x="20" y="14" width="160" height="8" rx="4" fill="var(--border)" stroke="${color}" stroke-width="1"/>
      <!-- embouchure -->
      <path d="M2 14 L20 12 L20 24 L2 22 Z" fill="${color}" opacity="0.55"/>
      <circle cx="6" cy="18" r="2" fill="var(--bg)"/>
      <!-- piston / curseur (bouge avec posPct) -->
      <rect x="${cx - 3}" y="10" width="6" height="16" rx="2" fill="${color}"/>
      <line x1="${cx}" y1="6" x2="${cx}" y2="30" stroke="${color}" stroke-width="0.5" opacity="0.5"/>
      <!-- pommeau -->
      <circle cx="180" cy="18" r="4" fill="${color}"/>
      ${playing ? `
        <!-- ondes sonores quand actif -->
        <circle cx="6" cy="18" r="6" fill="none" stroke="${color}" stroke-width="0.5" opacity="0.6">
          <animate attributeName="r" from="6" to="14" dur="1s" repeatCount="indefinite"/>
          <animate attributeName="opacity" from="0.6" to="0" dur="1s" repeatCount="indefinite"/>
        </circle>
      ` : ''}
    </svg>`;
}

// ============================================================================
// Position oscilloscope par flûte (rolling history)
// ============================================================================
const _posHistory = {};   // {fluteId: [{t, pos}, ...]}
function pushFlutePos(id, pos) {
  if (!_posHistory[id]) _posHistory[id] = [];
  _posHistory[id].push({t: Date.now(), pos});
  if (_posHistory[id].length > 80) _posHistory[id].shift();
}
function drawScope(canvas, id, travelMm) {
  if (!canvas) return;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (w === 0) return;
  if (canvas.width !== w) canvas.width = w;
  if (canvas.height !== h) canvas.height = h;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  const hist = _posHistory[id] || [];
  if (hist.length < 2) return;
  // Grille
  ctx.strokeStyle = 'rgba(128,128,128,.15)'; ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = (i / 4) * h;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }
  // Trace
  const color = fluteColor(id);
  ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.beginPath();
  hist.forEach((s, i) => {
    const x = (i / (hist.length - 1)) * w;
    const y = h - (s.pos / travelMm) * h;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
  // Dernier point
  const last = hist[hist.length - 1];
  const lx = w, ly = h - (last.pos / travelMm) * h;
  ctx.fillStyle = color;
  ctx.beginPath(); ctx.arc(lx - 2, ly, 2.5, 0, 2*Math.PI); ctx.fill();
}

// ============================================================================
// Note heatmap par flûte
// ============================================================================
const _noteHits = {};     // {fluteId: {note: count}}
function bumpNoteHit(id, note) {
  if (!_noteHits[id]) _noteHits[id] = {};
  _noteHits[id][note] = (_noteHits[id][note] || 0) + 1;
}
function buildHeatmap(id, noteMin, noteMax) {
  const hits = _noteHits[id] || {};
  let max = 0;
  for (let n = noteMin; n <= noteMax; n++) max = Math.max(max, hits[n] || 0);
  if (max === 0) max = 1;
  const color = fluteColor(id);
  let html = '<div style="display:flex;gap:1px;height:14px">';
  for (let n = noteMin; n <= noteMax; n++) {
    const c = hits[n] || 0;
    const opacity = c ? (0.2 + 0.8 * (c / max)) : 0.05;
    html += `<div style="flex:1;background:${color};opacity:${opacity}" title="${noteName(n)} : ${c}"></div>`;
  }
  return html + '</div>';
}

// ============================================================================
// Loading overlay
// ============================================================================
let _loadingDepth = 0;
function showLoading() {
  _loadingDepth++;
  document.getElementById('loading-overlay').classList.add('show');
}
function hideLoading() {
  _loadingDepth = Math.max(0, _loadingDepth - 1);
  if (_loadingDepth === 0) document.getElementById('loading-overlay').classList.remove('show');
}

// ============================================================================
// Modal confirm dialog (replaces native confirm)
// ============================================================================
function confirmDialog(message, title = 'Confirmer', okLabel = 'OK') {
  return new Promise(resolve => {
    const root  = document.getElementById('modalRoot');
    const tEl   = document.getElementById('modalTitle');
    const mEl   = document.getElementById('modalMsg');
    const okBtn = document.getElementById('modalOk');
    const caBtn = document.getElementById('modalCancel');
    tEl.textContent = title;
    mEl.textContent = message;
    okBtn.textContent = okLabel;
    root.classList.add('show');
    const close = (val) => {
      root.classList.remove('show');
      okBtn.onclick = caBtn.onclick = root.onclick = null;
      resolve(val);
    };
    okBtn.onclick = () => close(true);
    caBtn.onclick = () => close(false);
    // Cliquer en dehors = annuler
    root.onclick = e => { if (e.target === root) close(false); };
  });
}

// ============================================================================
// Toast notifications
// ============================================================================
function toast(msg, type = 'success', timeout = 3000) {
  const t = document.createElement('div');
  t.className = 'toast ' + type;
  t.textContent = msg;
  document.getElementById('toasts').appendChild(t);
  setTimeout(() => { t.style.opacity = '0'; setTimeout(() => t.remove(), 200); }, timeout);
}

// toast avec bouton "Annuler" — appelle undoFn si l'utilisateur clique
// avant l'expiration. Utile pour les actions destructives (delete preset,
// disable flute, etc.) — donne 5 s pour annuler.
function toastUndo(msg, undoFn, timeout = 5000) {
  const t = document.createElement('div');
  t.className = 'toast warn';
  t.textContent = msg + ' ';
  const btn = document.createElement('button');
  btn.className = 'toast-undo';
  btn.textContent = '↶ Annuler';
  btn.onclick = async () => {
    try { await undoFn(); toast('Annulé', 'success'); }
    catch (e) { toast('Échec annulation', 'error'); }
    t.remove();
  };
  t.appendChild(btn);
  document.getElementById('toasts').appendChild(t);
  setTimeout(() => { t.style.opacity = '0'; setTimeout(() => t.remove(), 200); }, timeout);
}

// ============================================================================
// HTTP helpers
// ============================================================================
async function postNoBody(url) {
  const r = await fetch(url, {method:'POST'});
  if (!r.ok) toast(`Erreur ${r.status} sur ${url}`, 'error');
  return r;
}
async function postJson(url, body) {
  const r = await fetch(url, {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(body)
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) toast(`Erreur: ${j.error || r.status}`, 'error');
  return j;
}
async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) { toast(`GET ${url} → ${r.status}`, 'error'); return null; }
  return r.json();
}

// ============================================================================
// WebSocket
// ============================================================================
let _wsConnected = false;
let _wsReconnectCount = 0;
function setConnState(ok) {
  const dot = document.getElementById('connDot');
  const lbl = document.getElementById('connLabel');
  if (dot) dot.className = ok ? 'ok' : 'lost';
  if (lbl) lbl.textContent = ok ? 'connecté' : 'reconnexion...';
  // Toast on reconnect (skip first connect)
  if (ok && !_wsConnected && _wsReconnectCount > 0) toast('Reconnecté au serveur', 'success');
  if (!ok && _wsConnected) toast('Connexion WebSocket perdue', 'warn');
  _wsConnected = ok;
}
function connectWs() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { setConnState(true); };
  ws.onmessage = e => {
    try { state = JSON.parse(e.data); render(); } catch(_) {}
  };
  ws.onclose = () => {
    setConnState(false);
    _wsReconnectCount++;
    setTimeout(connectWs, 2000);
  };
  ws.onerror = () => setConnState(false);
}

// ============================================================================
// Render
// ============================================================================
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const noteName = n => `${NOTE_NAMES[n%12]}${Math.floor(n/12)-1}`;
const noteFreq = n => 440 * Math.pow(2, (n - 69) / 12);
const noteWithFreq = n => `${noteName(n)} (${noteFreq(n).toFixed(1)}Hz)`;

function fmtUptime(ms) {
  const s = Math.floor(ms/1000);
  const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), ss = s%60;
  return h ? `${h}h${String(m).padStart(2,'0')}` : `${m}m${String(ss).padStart(2,'0')}s`;
}

function badgeClass(s) {
  return ({READY:'b-ready', HOMING:'b-homing', ERROR:'b-error', INIT:'b-init'})[s] || 'b-init';
}

// Suivi notes activées (pour heatmap) — déclenché à chaque frame WS
const _lastNoteState = {};
function trackNoteEdges() {
  (state.flutes||[]).forEach(f => {
    const prev = _lastNoteState[f.id] || {note: 0, active: false};
    // edge rising
    if (f.note_active && (!prev.active || prev.note !== f.last_note)) {
      bumpNoteHit(f.id, f.last_note);
    }
    _lastNoteState[f.id] = {note: f.last_note, active: f.note_active};
    pushFlutePos(f.id, f.position_mm || 0);
  });
}

function render() {
  trackNoteEdges();
  // Adaptive : masque la navigation Pression si module non activé
  const pressureNav = document.querySelector('nav button[data-page="pressure"]');
  if (pressureNav) pressureNav.style.display = state.pressure ? '' : 'none';

  // Header
  const sb = document.getElementById('sysBadge');
  sb.textContent = state.sys_state || 'INIT';
  sb.className = 'badge ' + badgeClass(state.sys_state);
  document.getElementById('fluteCount').textContent =
    `${(state.flutes||[]).length} flûte${(state.flutes||[]).length>1?'s':''}`;

  const pb = document.getElementById('pressBadge');
  if (state.pressure) {
    pb.style.display = 'inline-block';
    pb.textContent = `${state.pressure.state} ${state.pressure.pressure_kpa?.toFixed?.(0)||'?'}kPa`;
    pb.className = 'pill ' + (state.pressure.fault !== 'none' ? 'fault' : 'ok');
  } else pb.style.display = 'none';

  // Dashboard
  document.getElementById('dashState').textContent = state.sys_state;
  document.getElementById('dashUptime').textContent = fmtUptime(state.uptime_ms||0);
  document.getElementById('dashMidiCount').textContent = state.midi_count||0;

  const dp = document.getElementById('dashPressure');
  if (state.pressure) {
    const p = state.pressure;
    const pct = Math.max(0, Math.min(100, ((p.pressure_kpa||0) / 100) * 100));
    let bar = 'progress-fill';
    if (p.state === 'SAFETY') bar += ' danger';
    else if (p.state === 'FILLING') bar += ' warn';
    dp.innerHTML = `
      <div class="stat"><span class="stat-label">État</span><span class="stat-value">${p.state}</span></div>
      <div class="stat"><span class="stat-label">Pression</span><span class="stat-value">${p.pressure_kpa?.toFixed?.(1)||'—'} kPa</span></div>
      <div class="progress-bar"><div class="${bar}" style="width:${pct}%"></div></div>
      <div class="stat"><span class="stat-label">Pompe</span><span class="stat-value ${p.pump_on?'':'muted'}">${p.pump_on?'ON ('+p.pump_duty+')':'OFF'}</span></div>
      ${p.fault!=='none'?`<div class="stat"><span class="stat-label">Défaut</span><span class="stat-value red">${p.fault}</span></div>`:''}
    `;
  } else {
    dp.innerHTML = '<div class="stat-value muted">module désactivé</div>';
  }

  // Dashboard flûtes
  const df = document.getElementById('dashFlutes');
  df.innerHTML = (state.flutes||[]).map(f => {
    let cls = 'card flute-card';
    if (!f.enabled) cls += ' disabled';
    else if (f.muted) cls += ' muted';
    if (f.air_open || f.note_active) cls += ' active';
    const travel = f.slider_travel_mm || 300;
    const posPct = Math.max(0, Math.min(100, ((f.position_mm||0)/travel)*100));
    const pwmPct = Math.max(0, Math.min(100, ((f.pwm||0)/255)*100));
    return `
      <div class="${cls}" style="--flute-color:${fluteColor(f.id)}">
        <h3><span><span class="flute-id">#${f.id}</span> ${f.name}</span>
            <span class="pill">ch${f.midi_channel||'omni'}</span></h3>
        ${whistleSvg(fluteColor(f.id), posPct, f.air_open || f.note_active)}
        <div class="slider-label"><span>0</span><span>${f.position_mm?.toFixed?.(1)||'—'} mm</span><span>${travel}</span></div>
        <div class="stat" style="margin-top:6px"><span class="stat-label">Note</span>
          <span class="stat-value">${f.note_active?noteWithFreq(f.last_note):'—'}</span></div>
        <div class="mini-piano">
          ${Array.from({length: f.note_max - f.note_min + 1}, (_, i) => {
            const n = f.note_min + i;
            const cls = (f.note_active && n === f.last_note) ? 'playing' : 'in-range';
            return `<div class="mini-piano-key ${cls}" title="${noteName(n)}"></div>`;
          }).join('')}
        </div>
        <div class="stat"><span class="stat-label">Air</span>
          <span class="stat-value ${f.air_open?'':'muted'}">${f.air_state}</span></div>
        <div class="vu" title="PWM solénoïde ${f.pwm}/255"><div class="vu-fill" style="width:${pwmPct}%"></div></div>
      </div>`;
  }).join('') || '<div class="small">Aucune flûte configurée.</div>';

  // Page Flûtes (gestion) — applique le filtre actif
  const fl = document.getElementById('flutesList');
  const filtered = (state.flutes||[]).filter(fluteMatchesFilter);
  if (!filtered.length) {
    fl.innerHTML = `<div class="empty-state" style="grid-column:1/-1">
      <div class="ico">🎺</div>
      <div class="msg">Aucune flûte ne correspond au filtre "${_fluteFilter}".</div>
      <button class="btn" onclick="document.querySelector('#fluteFilters .chip[data-filter=&quot;all&quot;]').click()">Voir toutes</button>
    </div>`;
    populateFluteSelectors();
    setAnimCounter(document.getElementById('dashMidiCount'), state.midi_count || 0);
    return;
  }
  fl.innerHTML = filtered.map(f => {
    let cls = 'card flute-card';
    if (!f.enabled) cls += ' disabled';
    else if (f.muted) cls += ' muted';
    if (f.air_open || f.note_active) cls += ' active';
    const travel = f.slider_travel_mm || 300;
    const posPct = Math.max(0, Math.min(100, ((f.position_mm||0)/travel)*100));
    const pwmPct = Math.max(0, Math.min(100, ((f.pwm||0)/255)*100));
    const span = (f.note_max - f.note_min) || 1;
    return `
      <div class="${cls}" style="--flute-color:${fluteColor(f.id)}">
        <h3>
          <span>
            <span class="led ${f.homed?'green':'red'}" title="${f.homed?'Homé':'Non homé'}"></span>
            <span class="flute-id">#${f.id}</span> ${f.name}
          </span>
          <span>
            <span class="led ${f.air_open?'green':'off'}" title="Air ${f.air_open?'ouvert':'fermé'}"></span>
            <span class="led ${f.muted?'yellow':(f.enabled?'green':'red')}" title="${f.muted?'mute':(f.enabled?'active':'désactivée')}"></span>
          </span>
        </h3>
        ${whistleSvg(fluteColor(f.id), posPct, f.air_open || f.note_active)}
        <div class="slider-label"><span>0 mm</span><span>${f.position_mm?.toFixed?.(1)||'—'} mm</span><span>${travel} mm</span></div>
        <canvas class="scope-canvas" data-flute-id="${f.id}" data-travel="${travel}" style="width:100%;height:48px;background:var(--bg);border-radius:4px;margin:6px 0"></canvas>
        <div class="vu" title="PWM solénoïde ${f.pwm}/255"><div class="vu-fill" style="width:${pwmPct}%"></div></div>
        <div style="margin-top:6px"><div class="small">Heatmap notes</div>${buildHeatmap(f.id, f.note_min, f.note_max)}</div>
        <div class="stat" style="margin-top:8px"><span class="stat-label">Plage MIDI</span>
          <span class="stat-value">${noteName(f.note_min)}–${noteName(f.note_max)} (ch ${f.midi_channel||'omni'})</span></div>
        <div class="stat"><span class="stat-label">Note jouée</span>
          <span class="stat-value">${f.note_active?noteName(f.last_note):'—'}</span></div>
        <div class="stat"><span class="stat-label">Air</span>
          <span class="stat-value ${f.air_open?'':'muted'}">${f.air_state} (pwm ${f.pwm})</span></div>
        <div class="btn-row">
          <button class="btn" onclick="quickEditFlute(${f.id}, event)">✎ Éditer</button>
          <button class="btn" onclick="toggleEnable(${f.id}, ${!f.enabled})">${f.enabled?'Désactiver':'Activer'}</button>
          <button class="btn" onclick="toggleMute(${f.id}, ${!f.muted})">${f.muted?'Unmute':'Mute'}</button>
          <button class="btn" onclick="soloFlute(${f.id})">🎯 Solo</button>
          <button class="btn btn-warn" onclick="homeOne(${f.id})" title="Homing">⌂ Homing</button>
          <button class="btn btn-warn" onclick="testAir(${f.id})" title="Test ouverture solénoïde + balayage servo">Test air</button>
          <button class="btn btn-warn" onclick="sweepOne(${f.id})" title="Test mécanique : 0 → fond → 0">⇄ Sweep</button>
          <button class="btn btn-danger" onclick="panic(${f.id})">Panic</button>
        </div>
        <details style="margin-top:8px">
          <summary class="small" style="cursor:pointer">📝 Notes</summary>
          <textarea oninput="setFluteNote(${f.id}, this.value)" rows="2"
                    placeholder="Notes locales (sauvées dans le navigateur)"
                    style="width:100%;margin-top:4px;padding:6px;background:var(--bg);
                    border:1px solid var(--border);border-radius:4px;color:var(--text);font-size:12px">${getFluteNote(f.id)}</textarea>
        </details>
      </div>`;
  }).join('') || '<div class="small">Aucune flûte configurée.</div>';
  // Bouton "Désactiver solo" en bas
  if ((state.flutes||[]).some(f => f.muted)) {
    fl.innerHTML += `<div class="card" style="text-align:center"><button class="btn btn-primary" onclick="clearSolo()">Libérer le solo (unmute toutes)</button></div>`;
  }

  // MIDI channels strip avec sparkline d'activité
  const strip = document.getElementById('midiChStrip');
  if (strip) {
    const counts = state.midi_count_by_channel || [];
    const last = state.midi_last_channel;
    const usedChannels = new Set();
    (state.flutes||[]).forEach(f => { if (f.midi_channel) usedChannels.add(f.midi_channel); });
    // Push delta dans l'historique par canal
    if (!_chHistory) window._chHistory = Array.from({length:17}, () => []);
    if (!_prevChCounts) window._prevChCounts = Array(17).fill(0);
    for (let ch = 1; ch <= 16; ch++) {
      const cur = counts[ch-1] || 0;
      const delta = Math.max(0, cur - _prevChCounts[ch]);
      _chHistory[ch].push(delta);
      if (_chHistory[ch].length > 20) _chHistory[ch].shift();
      _prevChCounts[ch] = cur;
    }
    let html = '';
    for (let ch = 1; ch <= 16; ch++) {
      const cnt = counts[ch-1] || 0;
      const isLast = ch === last;
      const isUsed = usedChannels.has(ch);
      let style = '', barColor = 'var(--green)';
      if (isUsed) {
        const f = (state.flutes||[]).find(x => x.midi_channel === ch);
        if (f) { style = `border-color:${fluteColor(f.id)}`; barColor = fluteColor(f.id); }
      }
      // Sparkline SVG en bas de la cellule
      const hist = _chHistory[ch];
      const maxH = Math.max(1, ...hist);
      let bars = '';
      for (let i = 0; i < hist.length; i++) {
        const h = (hist[i] / maxH) * 6;
        bars += `<rect x="${i*1.2}" y="${6-h}" width="1" height="${h}" fill="${barColor}"/>`;
      }
      html += `<div class="ch-cell ${isLast?'active':''}" style="${style}" title="${cnt} messages · cliquer pour filtrer log" onclick="filterLogByChannel(${ch})">
        <span class="ch-num">${ch}</span>
        <span class="ch-cnt">${cnt > 999 ? Math.round(cnt/100)/10+'k' : cnt}</span>
        <svg class="ch-spark" viewBox="0 0 24 6" preserveAspectRatio="none">${bars}</svg>
      </div>`;
    }
    strip.innerHTML = html;
  }

  // Selectors
  populateFluteSelectors();

  // Compteurs animés
  const mc = document.getElementById('dashMidiCount');
  if (mc) {
    if (!mc.classList.contains('anim-counter')) mc.classList.add('anim-counter');
    setAnimCounter(mc, state.midi_count || 0);
  }

  // Dessin oscilloscopes (sur la page Flûtes)
  document.querySelectorAll('canvas.scope-canvas').forEach(c => {
    drawScope(c, +c.dataset.fluteId, +c.dataset.travel);
  });
}

function populateFluteSelectors() {
  const opts = (state.flutes||[]).map(f => `<option value="${f.id}">#${f.id} ${f.name}</option>`).join('');
  ['playFluteSel','cfgFluteSel'].forEach(id => {
    const el = document.getElementById(id);
    if (el && el.options.length !== (state.flutes||[]).length) el.innerHTML = opts;
  });
}

// ============================================================================
// Play page
// ============================================================================
const playNote = document.getElementById('playNote');
const playVel  = document.getElementById('playVel');
playNote.oninput = () => document.getElementById('playNoteLabel').textContent = `${playNote.value} (${noteName(+playNote.value)})`;
playVel.oninput  = () => document.getElementById('playVelLabel').textContent  = playVel.value;

async function playStart() {
  const id = +document.getElementById('playFluteSel').value;
  await postJson('/api/flute/note', {id, note:+playNote.value, velocity:+playVel.value, on:true});
}
async function playStop() {
  const id = +document.getElementById('playFluteSel').value;
  await postJson('/api/flute/note', {id, note:+playNote.value, on:false});
}

// Piano (multi-touch + souris + labels)
function buildPiano() {
  const piano = document.getElementById('piano');
  piano.innerHTML = '';
  for (let n = 48; n <= 84; n++) {
    const isBlack = [1,3,6,8,10].includes(n%12);
    const k = document.createElement('div');
    k.className = 'key ' + (isBlack ? 'black' : 'white');
    k.dataset.note = n;
    k.title = noteName(n) + ' / ' + n;
    // Label sur les Do uniquement (ou toutes les notes blanches si on veut)
    if ((n % 12) === 0 || (n % 12) === 7) {
      const lbl = document.createElement('span');
      lbl.className = 'label';
      lbl.textContent = noteName(n);
      k.appendChild(lbl);
    }
    // Souris
    k.onmousedown = e => { e.preventDefault(); keyPress(n, true); };
    k.onmouseup   = () => keyPress(n, false);
    k.onmouseleave= () => keyPress(n, false);
    // Touch (multi-touch)
    k.ontouchstart = e => { e.preventDefault(); keyPress(n, true); };
    k.ontouchend   = e => { e.preventDefault(); keyPress(n, false); };
    k.ontouchcancel = () => keyPress(n, false);
    piano.appendChild(k);
  }
}
const _activeKeys = new Set();
async function keyPress(note, on) {
  // Visuel local immédiat + ripple à la frappe
  const k = document.querySelector(`#piano .key[data-note="${note}"]`);
  if (k) {
    k.classList.toggle('active', on);
    if (on) {
      const r = document.createElement('span');
      r.className = 'ripple';
      r.style.background = `rgba(0,208,132,.7)`;
      k.appendChild(r);
      setTimeout(() => r.remove(), 700);
    }
  }
  // Eviter de spam si pas de changement
  if (on && _activeKeys.has(note)) return;
  if (!on && !_activeKeys.has(note)) return;
  if (on) _activeKeys.add(note); else _activeKeys.delete(note);
  // Audio preview
  if (on) synthNoteOn(note); else synthNoteOff(note);
  // API
  const id = +(document.getElementById('playFluteSel').value || 0);
  await postJson('/api/flute/note', {id, note, velocity:100, on});
}

// Mapping clavier ordinateur → notes (rangée du clavier QWERTY/AZERTY)
// Z S X D C V G B H N J M , L . ; / → C4 .. E5 (chromatique)
const KEYBOARD_MAP = {
  'a':60,'w':61,'s':62,'e':63,'d':64,'f':65,'t':66,'g':67,'y':68,'h':69,'u':70,'j':71,
  'k':72,'o':73,'l':74,'p':75,';':76,
  // AZERTY fallback
  'q':60,'z':61,'w':62
};
function isPlayPageActive() {
  return document.getElementById('page-play')?.classList.contains('active');
}
document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (e.repeat) return;
  if (!isPlayPageActive()) return;            // notes seulement sur la page Jouer
  const n = KEYBOARD_MAP[e.key.toLowerCase()];
  if (n != null) { e.preventDefault(); keyPress(n, true); }
});
document.addEventListener('keyup', e => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (!isPlayPageActive()) return;
  const n = KEYBOARD_MAP[e.key.toLowerCase()];
  if (n != null) { e.preventDefault(); keyPress(n, false); }
});

// ============================================================================
// Web Audio synth preview (pour tester sans matériel)
// ============================================================================
let _audioCtx = null, _synthEnabled = false;
const _activeOsc = {};

function ensureCtx() {
  if (!_audioCtx) _audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  if (_audioCtx.state === 'suspended') _audioCtx.resume();
}
function midiToFreq(n) { return 440 * Math.pow(2, (n - 69) / 12); }
function synthNoteOn(note) {
  if (!_synthEnabled) return;
  ensureCtx();
  if (_activeOsc[note]) return;
  // Mini synthé : sinus + filtre passe-bas + enveloppe AD
  const osc = _audioCtx.createOscillator();
  const gain = _audioCtx.createGain();
  const filter = _audioCtx.createBiquadFilter();
  osc.type = 'triangle';
  osc.frequency.value = midiToFreq(note);
  filter.type = 'lowpass';
  filter.frequency.value = 2000;
  filter.Q.value = 1.5;
  gain.gain.setValueAtTime(0, _audioCtx.currentTime);
  gain.gain.linearRampToValueAtTime(0.18, _audioCtx.currentTime + 0.02);
  osc.connect(filter); filter.connect(gain); gain.connect(_audioCtx.destination);
  osc.start();
  _activeOsc[note] = {osc, gain};
}
function synthNoteOff(note) {
  const o = _activeOsc[note];
  if (!o) return;
  const t = _audioCtx.currentTime;
  o.gain.gain.cancelScheduledValues(t);
  o.gain.gain.setValueAtTime(o.gain.gain.value, t);
  o.gain.gain.linearRampToValueAtTime(0, t + 0.08);
  o.osc.stop(t + 0.1);
  delete _activeOsc[note];
}
function toggleSynth() {
  _synthEnabled = !_synthEnabled;
  if (_synthEnabled) ensureCtx();
  else {
    Object.keys(_activeOsc).forEach(n => synthNoteOff(+n));
  }
  const btn = document.getElementById('synthToggleBtn');
  btn.style.background = _synthEnabled ? 'var(--green)' : 'transparent';
  btn.style.color = _synthEnabled ? '#000' : 'var(--text)';
  toast(_synthEnabled ? 'Preview audio activée' : 'Preview audio désactivée');
}

// ============================================================================
// Flute control actions
// ============================================================================
async function toggleEnable(id, en) {
  await postJson('/api/flute', {id, enabled: en});
  if (!en) toastUndo(`Flûte #${id} désactivée.`,
    () => postJson('/api/flute', {id, enabled: true}));
}
async function toggleMute(id, m) {
  await postJson('/api/flute', {id, muted: m});
  if (m) toastUndo(`Flûte #${id} mutée.`,
    () => postJson('/api/flute', {id, muted: false}));
}
async function testAir(id)          { await postJson('/api/flute/test',  {id}); }
async function panic(id)            { await postJson('/api/flute/panic', {id}); }
async function homeOne(id)          { await postJson('/api/flute/homing', {id}); }

// ============================================================================
// Config page
// ============================================================================
async function loadFluteCfg() {
  const sel = document.getElementById('cfgFluteSel');
  if (!sel.value && (state.flutes||[]).length) sel.value = state.flutes[0].id;
  if (sel.value === '' || sel.value === undefined) return;
  const c = await getJson(`/api/flute?id=${sel.value}`);
  document.getElementById('cfgChan').value     = c.midi_channel;
  document.getElementById('cfgNmin').value     = c.note_min;
  document.getElementById('cfgNmax').value     = c.note_max;
  document.getElementById('cfgSpeed').value    = c.speed_mm_s?.toFixed?.(1) || c.speed_mm_s;
  document.getElementById('cfgAccel').value    = c.accel_mm_s2?.toFixed?.(0) || c.accel_mm_s2;
  document.getElementById('cfgPwmFull').value  = c.pwm_full;
  document.getElementById('cfgPwmHold').value  = c.pwm_hold;
  document.getElementById('cfgWait').value     = c.wait_delay_ms;
  document.getElementById('cfgLegato').value   = c.legato_ms;
  document.getElementById('cfgVelCurve').value  = c.velocity_curve ?? 1;
  document.getElementById('cfgCustomName').value = c.custom_name || '';
  document.getElementById('cfgCustomName').placeholder = c.default_name || '';
  document.getElementById('cfgCcBreath').value   = c.cc_breath ?? 2;
  document.getElementById('cfgCcExpr').value     = c.cc_expression ?? 11;
  document.getElementById('cfgCcVolume').value   = c.cc_volume ?? 7;
  document.getElementById('cfgCcVibrato').value  = c.cc_vibrato ?? 1;
  document.getElementById('cfgCcSustain').value  = c.cc_sustain ?? 64;
  document.getElementById('cfgTranspose').value  = c.transpose ?? 0;
  document.getElementById('cfgEnabled').checked = c.enabled;
  document.getElementById('cfgMuted').checked   = c.muted;
  document.getElementById('cfgUseLut').checked  = c.use_lut;
  // Range editor sync
  syncRangeFromInputs();
  drawVelCurve();
  buildLutBox(c.lut || []);
  buildColorSwatches(c.id);
  refreshCopyFromOptions();
  clearCfgDirty();
}

function buildColorSwatches(id) {
  const box = document.getElementById('cfgColorSwatches');
  if (!box) return;
  const current = fluteColor(id);
  const palette = [...FLUTE_COLORS, '#7e57c2','#26c6da','#ec407a','#9ccc65','#ff5722'];
  box.innerHTML = palette.map(c =>
    `<span class="color-swatch ${c.toLowerCase()===current.toLowerCase()?'selected':''}" style="background:${c}" onclick="setFluteColor(${id},'${c}')"></span>`
  ).join('') + ` <button class="btn small" onclick="setFluteColor(${id},null)" style="padding:2px 8px;font-size:11px">défaut</button>`;
}

// ---- Dirty state for config ------------------------------------------------
let _cfgDirty = false;
function markCfgDirty() {
  _cfgDirty = true;
  const bar = document.getElementById('cfgDirtyBar');
  if (bar) bar.classList.add('show');
}
function clearCfgDirty() {
  _cfgDirty = false;
  const bar = document.getElementById('cfgDirtyBar');
  if (bar) bar.classList.remove('show');
}
// Attacher des écouteurs aux champs de config (une fois le DOM chargé)
function setupDirtyTracking() {
  const ids = ['cfgCustomName','cfgChan','cfgNmin','cfgNmax','cfgSpeed','cfgAccel',
    'cfgPwmFull','cfgPwmHold','cfgWait','cfgLegato','cfgVelCurve',
    'cfgCcBreath','cfgCcExpr','cfgCcVolume','cfgCcVibrato','cfgCcSustain',
    'cfgTranspose','cfgEnabled','cfgMuted','cfgUseLut'];
  const onChange = (id) => {
    markCfgDirty();
    validateField(id);
    maybeAutoSave();
  };
  ids.forEach(id => {
    const el = document.getElementById(id);
    if (el) el.addEventListener('input',  () => onChange(id));
    if (el) el.addEventListener('change', () => onChange(id));
  });
  // Persistance auto-save toggle
  const cb = document.getElementById('cfgAutoSave');
  if (cb) {
    cb.checked = localStorage.getItem('autosave') === '1';
    cb.addEventListener('change', () => localStorage.setItem('autosave', cb.checked ? '1' : '0'));
  }
  // Mode avancé toggle (affiche transpose, ouvre le panneau Moteur & Air)
  const adv = document.getElementById('cfgAdvanced');
  if (adv) {
    const restore = localStorage.getItem('advanced') === '1';
    adv.checked = restore;
    if (restore) document.body.classList.add('advanced-mode');
    adv.addEventListener('change', () => {
      document.body.classList.toggle('advanced-mode', adv.checked);
      localStorage.setItem('advanced', adv.checked ? '1' : '0');
      // Auto-ouvrir/fermer le panneau Moteur & Air
      const panel = document.querySelector('details.cfg-collapse:nth-of-type(2)');
      if (panel) panel.open = adv.checked;
    });
  }
}

async function saveFluteCfg() {
  const id = +document.getElementById('cfgFluteSel').value;
  await postJson('/api/flute', {
    id,
    custom_name:  document.getElementById('cfgCustomName').value,
    midi_channel: +document.getElementById('cfgChan').value,
    note_min:     +document.getElementById('cfgNmin').value,
    note_max:     +document.getElementById('cfgNmax').value,
    speed_mm_s:   +document.getElementById('cfgSpeed').value,
    accel_mm_s2:  +document.getElementById('cfgAccel').value,
    pwm_full:     +document.getElementById('cfgPwmFull').value,
    pwm_hold:     +document.getElementById('cfgPwmHold').value,
    wait_delay_ms:+document.getElementById('cfgWait').value,
    legato_ms:    +document.getElementById('cfgLegato').value,
    velocity_curve: +document.getElementById('cfgVelCurve').value,
    cc_breath:     +document.getElementById('cfgCcBreath').value,
    cc_expression: +document.getElementById('cfgCcExpr').value,
    cc_volume:     +document.getElementById('cfgCcVolume').value,
    cc_vibrato:    +document.getElementById('cfgCcVibrato').value,
    cc_sustain:    +document.getElementById('cfgCcSustain').value,
    transpose:    +document.getElementById('cfgTranspose').value,
    enabled:      document.getElementById('cfgEnabled').checked,
    muted:        document.getElementById('cfgMuted').checked,
    use_lut:      document.getElementById('cfgUseLut').checked,
  });
  toast('Configuration sauvegardée');
  clearCfgDirty();
}

async function testFluteAir() {
  const id = +document.getElementById('cfgFluteSel').value;
  await postJson('/api/flute/test', {id});
}

let _currentLut = [];   // [{note, position}, …]
let _currentTravel = 300;

function buildLutBox(lut) {
  _currentLut = lut.slice();
  const sel = document.getElementById('cfgFluteSel');
  const f = (state.flutes||[]).find(x => x.id == sel.value);
  _currentTravel = f?.slider_travel_mm || 300;
  // Table textuelle (collapsée)
  const box = document.getElementById('lutBox');
  box.innerHTML =
    '<div class="lut-grid"><div><b>Note</b></div><div></div><div><b>Position (mm)</b></div><div></div></div>'
    + lut.map(p => `
      <div class="lut-grid">
        <div>${noteName(p.note)} (${p.note})</div>
        <div></div>
        <div><input type="number" step="0.1" id="lut_${p.note}" value="${p.position.toFixed(2)}"></div>
        <div><button class="btn" onclick="saveLutPoint(${p.note})">💾</button></div>
      </div>
    `).join('');
  drawLutCanvas();
}

function drawLutCanvas() {
  const c = document.getElementById('lutCanvas');
  if (!c || !_currentLut.length) return;
  const w = c.clientWidth, h = c.clientHeight;
  if (w === 0) return;
  if (c.width !== w) c.width = w;
  if (c.height !== h) c.height = h;
  const ctx = c.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  const padL = 32, padR = 8, padT = 12, padB = 22;
  const plotW = w - padL - padR, plotH = h - padT - padB;
  const n = _currentLut.length;
  const noteMin = _currentLut[0].note;
  const noteMax = _currentLut[n-1].note;
  const x = i => padL + (i / (n - 1)) * plotW;
  const yPos = mm => padT + plotH - (mm / _currentTravel) * plotH;
  // Grid Y (4 lignes)
  ctx.strokeStyle = '#2a2a4a'; ctx.lineWidth = 1;
  ctx.fillStyle = '#888'; ctx.font = '10px sans-serif';
  for (let i = 0; i <= 4; i++) {
    const v = (i/4) * _currentTravel;
    const y = yPos(v);
    ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - padR, y); ctx.stroke();
    ctx.fillText(v.toFixed(0), 2, y + 3);
  }
  // X axis labels (every 12 semitones = 1 octave)
  for (let i = 0; i < n; i += 12) {
    const note = _currentLut[i].note;
    ctx.fillText(noteName(note), x(i) - 8, h - 6);
  }
  // Référence linéaire (gris pointillé)
  ctx.strokeStyle = '#555'; ctx.setLineDash([3,3]); ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const linMm = (i / (n - 1)) * _currentTravel;
    const xx = x(i), yy = yPos(linMm);
    if (i === 0) ctx.moveTo(xx, yy); else ctx.lineTo(xx, yy);
  }
  ctx.stroke();
  ctx.setLineDash([]);
  // Courbe LUT (vert)
  ctx.strokeStyle = '#00d084'; ctx.lineWidth = 2; ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const xx = x(i), yy = yPos(_currentLut[i].position);
    if (i === 0) ctx.moveTo(xx, yy); else ctx.lineTo(xx, yy);
  }
  ctx.stroke();
  // Points cliquables
  ctx.fillStyle = '#00d084';
  for (let i = 0; i < n; i++) {
    const xx = x(i), yy = yPos(_currentLut[i].position);
    ctx.beginPath(); ctx.arc(xx, yy, 3, 0, 2*Math.PI); ctx.fill();
  }
}

// Drag sur le canvas LUT pour modifier les points (souris + tactile).
// Sauvegarde au mouseup/touchend, tooltip crosshair au survol souris.
(function setupLutDrag() {
  const c = document.getElementById('lutCanvas');
  if (!c) return;
  let dragIdx = -1;

  // Helpers : coordonnées d'un évènement, recherche du point le plus proche
  function eventXY(e) {
    const t = e.touches?.[0] || e.changedTouches?.[0] || e;
    return { x: t.clientX, y: t.clientY };
  }
  function findNearestPoint(xRel) {
    const padL = 32, padR = 8;
    const plotW = c.clientWidth - padL - padR;
    const n = _currentLut.length;
    let best = 0, bestD = 1e9;
    for (let i = 0; i < n; i++) {
      const xx = padL + (i / (n - 1)) * plotW;
      const d = Math.abs(xx - xRel);
      if (d < bestD) { bestD = d; best = i; }
    }
    return best;
  }

  // Hover : afficher tooltip (souris seulement)
  c.addEventListener('mousemove', e => {
    if (!_currentLut.length) return;
    const rect = c.getBoundingClientRect();
    const xRel = e.clientX - rect.left;
    const p = _currentLut[findNearestPoint(xRel)];
    const tip = document.getElementById('lutTooltip');
    if (tip) {
      tip.style.display = 'block';
      tip.style.left = (xRel + 12) + 'px';
      tip.style.top  = (e.clientY - rect.top - 32) + 'px';
      tip.textContent = `${noteName(p.note)} (${p.note}) → ${p.position.toFixed(1)} mm · ${noteFreq(p.note).toFixed(1)}Hz`;
    }
  });
  c.addEventListener('mouseleave', () => {
    const tip = document.getElementById('lutTooltip');
    if (tip) tip.style.display = 'none';
  });

  // Drag start (souris ou touch)
  function startDrag(e) {
    if (!_currentLut.length) return;
    const rect = c.getBoundingClientRect();
    const { x } = eventXY(e);
    dragIdx = findNearestPoint(x - rect.left);
    e.preventDefault();
  }
  // Drag move : ajuste position et redessine
  function moveDrag(e) {
    if (dragIdx < 0) return;
    const rect = c.getBoundingClientRect();
    const padT = 12, padB = 22;
    const plotH = c.clientHeight - padT - padB;
    const { y } = eventXY(e);
    const yRel = y - rect.top - padT;
    const ratio = Math.max(0, Math.min(1, 1 - yRel / plotH));
    _currentLut[dragIdx].position = ratio * _currentTravel;
    drawLutCanvas();
    e.preventDefault();
  }
  // Drag end : POST + toast
  async function endDrag() {
    if (dragIdx < 0) return;
    const note = _currentLut[dragIdx].note;
    const pos  = _currentLut[dragIdx].position;
    const id = +document.getElementById('cfgFluteSel').value;
    await postJson('/api/flute/lut_point', { id, note, position_mm: pos });
    const inp = document.getElementById('lut_' + note);
    if (inp) inp.value = pos.toFixed(2);
    toast(`Note ${noteName(note)} → ${pos.toFixed(1)} mm`);
    dragIdx = -1;
  }

  c.addEventListener('mousedown',  startDrag);
  c.addEventListener('touchstart', startDrag, { passive: false });
  c.addEventListener('mousemove',  moveDrag);
  c.addEventListener('touchmove',  moveDrag,  { passive: false });
  document.addEventListener('mouseup', endDrag);
  c.addEventListener('touchend',  endDrag);
  c.addEventListener('touchcancel', () => { dragIdx = -1; });
})();
window.addEventListener('resize', drawLutCanvas);

async function saveLutPoint(note) {
  const id = +document.getElementById('cfgFluteSel').value;
  const pos = +document.getElementById('lut_' + note).value;
  await postJson('/api/flute/lut_point', {id, note, position_mm: pos});
}

// ============================================================================
// Pressure config
// ============================================================================
async function loadPressureCfg() {
  const c = await getJson('/api/pressure');
  if (!c.enabled) return;
  document.getElementById('pCfgTarget').value = c.target;
  document.getElementById('pCfgMin').value    = c.min;
  document.getElementById('pCfgMax').value    = c.max;
  document.getElementById('pCfgSafety').value = c.safety;
  document.getElementById('pCfgFill').value   = c.max_fill_ms;
}
async function savePressureCfg() {
  await postJson('/api/pressure/config', {
    target: +document.getElementById('pCfgTarget').value,
    min:    +document.getElementById('pCfgMin').value,
    max:    +document.getElementById('pCfgMax').value,
    safety: +document.getElementById('pCfgSafety').value,
    max_fill_ms: +document.getElementById('pCfgFill').value,
  });
  toast('Paramètres pression sauvegardés');
}

// Update pressure stats from WS state
setInterval(() => {
  const p = state.pressure;
  if (!p) return;
  document.getElementById('pState').textContent = p.state;
  document.getElementById('pVal').textContent   = (p.pressure_kpa?.toFixed?.(1)||'—') + ' kPa';
  document.getElementById('pPump').textContent  = p.pump_on ? `ON (${p.pump_duty})` : 'OFF';
  document.getElementById('pReservoir').textContent = p.reservoir_ok ? 'OK' : 'VIDE';
  document.getElementById('pFault').textContent = p.fault;
  document.getElementById('pFault').className = 'stat-value ' + (p.fault!=='none'?'red':'muted');
  const pct = Math.max(0, Math.min(100, ((p.pressure_kpa||0)/100)*100));
  const bar = document.getElementById('pBar');
  bar.style.width = pct + '%';
  bar.className = 'progress-fill' + (p.state==='SAFETY'?' danger':p.state==='FILLING'?' warn':'');
}, 200);

// ============================================================================
// WiFi
// ============================================================================
async function scanWifi() {
  const list = document.getElementById('wifiScanList');
  list.style.display = 'block';
  list.innerHTML = '<div class="scan-item"><span class="skeleton">scan en cours…</span></div>';
  showLoading();
  try {
    const networks = await getJson('/api/wifi/scan');
    if (!networks || !networks.length) {
      list.innerHTML = '<div class="scan-item small">Aucun réseau détecté.</div>';
      return;
    }
    networks.sort((a, b) => b.rssi - a.rssi);
    list.innerHTML = networks.map(n => {
      const lock = n.enc !== 0 ? '🔒' : '🔓';
      const bars = n.rssi > -50 ? '▮▮▮▮' : n.rssi > -65 ? '▮▮▮▯' : n.rssi > -75 ? '▮▮▯▯' : '▮▯▯▯';
      return `<div class="scan-item" onclick="document.getElementById('wSsid').value='${n.ssid.replace(/'/g, "\\'")}'; document.getElementById('wPass').focus()">
        <span><span class="scan-lock">${lock}</span>${n.ssid || '<em>(caché)</em>'}</span>
        <span class="scan-rssi">${bars} ${n.rssi}dBm · ch${n.ch}</span>
      </div>`;
    }).join('');
  } catch(e) { list.innerHTML = '<div class="scan-item">Erreur scan.</div>'; }
  finally { hideLoading(); }
}

// ============================================================================
// Stress test (Diag page)
// ============================================================================
async function startStress() {
  const sec = +document.getElementById('stressDuration').value || 30;
  if (!await confirmDialog(`Lancer un stress test pendant ${sec}s ?`, 'Stress test', 'Démarrer')) return;
  await postJson('/api/stress', {duration_sec: sec});
  toast(`Stress test démarré (${sec}s)`, 'warn');
}
async function stopStress() {
  await postJson('/api/stress', {duration_sec: 0});
  toast('Stress test stoppé');
}

async function loadWifi() {
  const w = await getJson('/api/wifi');
  if (!w) return;
  document.getElementById('wifiStatus').innerHTML = `
    <div class="stat"><span class="stat-label">AP</span><span class="stat-value">${w.ap?.ssid} · ${w.ap?.ip} · ${w.ap?.clients} clients</span></div>
    <div class="stat"><span class="stat-label">STA</span><span class="stat-value ${w.sta?.connected?'':'muted'}">${w.sta?.connected?w.sta.ssid+' · '+w.sta.ip:'non connecté'}</span></div>
    <div class="stat"><span class="stat-label">mDNS</span><span class="stat-value">http://${w.mdns}</span></div>
    <div class="small" style="margin-top:6px">Accès navigateur : <code>http://${w.mdns}</code> (depuis n'importe quel appareil sur le même réseau)</div>
  `;
}
async function saveWifi() {
  const ssid = document.getElementById('wSsid').value;
  const pass = document.getElementById('wPass').value;
  if (!ssid) { toast('SSID requis', 'warn'); return; }
  await postJson('/api/wifi', {ssid, password: pass});
  setTimeout(loadWifi, 1000);
}
async function clearWifi() {
  await fetch('/api/wifi', {method:'DELETE'});
  setTimeout(loadWifi, 500);
}

// ============================================================================
// MIDI activity log
// ============================================================================
function filterLogByChannel(ch) {
  navigateTo('midi');
  const inp = document.getElementById('midiFilterCh');
  if (inp) { inp.value = ch; loadMidiLog(); }
}
async function loadMidiLog() {
  const box = document.getElementById('midiLog');
  if (!box) return;
  // Skeleton pendant le fetch
  if (!box._loaded) box.innerHTML = '<div class="small skeleton skeleton-block">Chargement…</div>';
  const log = await getJson('/api/midi/log');
  if (!log) { box.innerHTML = '<div class="empty-state"><div class="ico">📡</div><div class="msg">Erreur de chargement.</div></div>'; return; }
  box._loaded = true;
  const filterType = document.getElementById('midiFilterType')?.value || '';
  const filterCh   = +document.getElementById('midiFilterCh')?.value || 0;
  const filtered = log.filter(e =>
    (!filterType || e.type === filterType) &&
    (!filterCh   || e.channel === filterCh)
  );
  if (!filtered.length) {
    box.innerHTML = '<div class="empty-state"><div class="ico">🎵</div>' +
      '<div class="msg">' + (log.length ? 'Aucune activité ne correspond aux filtres.' : 'Aucune activité MIDI reçue récemment. Branchez un contrôleur ou lancez une démo depuis Jouer.') + '</div></div>';
    return;
  }
  box.innerHTML = filtered.slice().reverse().map(e => {
    const age = e.t_rel_ms < 1000 ? `${e.t_rel_ms}ms` : `${(e.t_rel_ms/1000).toFixed(1)}s`;
    let body = '';
    if (e.type === 'NoteOn')        body = `${noteName(e.data1)} (${e.data1}) vel=${e.data2}`;
    else if (e.type === 'NoteOff')  body = `${noteName(e.data1)} (${e.data1})`;
    else if (e.type === 'CC')       body = `CC${e.data1}=${e.data2}`;
    else if (e.type === 'PitchBend')body = `bend=${e.bend}`;
    else if (e.type === 'Aftertouch')body= `pressure=${e.data1}`;
    return `<div class="log-line log-${e.type}">-${age.padStart(6)} ch${String(e.channel).padStart(2)} ${e.type.padEnd(10)} ${body}</div>`;
  }).join('');
}
// Auto-refresh MIDI log toutes 1.5s si page ouverte ET checkbox cochée
setInterval(() => {
  if (document.getElementById('page-midi')?.classList.contains('active') &&
      document.getElementById('midiAutoRefresh')?.checked) loadMidiLog();
}, 1500);

// ============================================================================
// OTA upload
// ============================================================================
async function otaUpload() {
  const f = document.getElementById('otaFile').files[0];
  if (!f) { toast('Sélectionnez un fichier .bin', 'warn'); return; }
  if (!await confirmDialog(
        `Flasher "${f.name}" (${(f.size/1024).toFixed(1)} KB) ?\n\n` +
        `⚠ ATTENTION :\n` +
        `• L'ESP32 sera indisponible pendant ~30 secondes\n` +
        `• Toutes les notes en cours seront coupées\n` +
        `• Si le flash échoue, débranchez/rebranchez et flashez à nouveau`,
        'Mise à jour OTA', 'Flasher')) return;
  const btn = document.getElementById('otaBtn');
  const prog = document.getElementById('otaProgress');
  btn.disabled = true;
  // Spinner persistant + barre de progression visuelle
  prog.innerHTML = '<div class="spinner" style="width:20px;height:20px;border-width:2px;display:inline-block;vertical-align:middle"></div> Upload 0%' +
    '<div class="progress-bar" style="margin-top:8px"><div id="otaBar" class="progress-fill" style="width:0%"></div></div>';
  try {
    const fd = new FormData();
    fd.append('firmware', f, f.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/system/update');
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) {
        const pct = (e.loaded / e.total) * 100;
        const bar = document.getElementById('otaBar');
        if (bar) bar.style.width = pct + '%';
        prog.firstChild.nextSibling.textContent = ` Upload ${pct.toFixed(0)}% (${(e.loaded/1024).toFixed(1)}/${(e.total/1024).toFixed(1)} KB)`;
      }
    };
    xhr.onload = () => {
      btn.disabled = false;
      if (xhr.status === 200) {
        prog.innerHTML = '<span class="health-ok">✓ Mise à jour OK — redémarrage en cours… patientez 30 s puis rechargez la page</span>';
        toast('Firmware flashé, reboot...', 'success', 10000);
      } else {
        prog.innerHTML = '<span class="health-error">✗ Échec : ' + xhr.responseText + '</span>';
        toast('Échec OTA', 'error', 6000);
      }
    };
    xhr.onerror = () => {
      btn.disabled = false;
      prog.innerHTML = '<span class="health-error">✗ Erreur réseau pendant l\'upload</span>';
      toast('Erreur réseau', 'error');
    };
    xhr.send(fd);
  } catch (e) {
    btn.disabled = false;
    prog.innerHTML = '<span class="health-error">✗ ' + e.message + '</span>';
  }
}

// ============================================================================
// Presets
// ============================================================================
async function loadPresets() {
  const list = await getJson('/api/presets');
  const box = document.getElementById('presetsList');
  if (!list || !list.length) {
    box.innerHTML = '<div class="empty-state"><div class="ico">💾</div>' +
      '<div class="msg">Aucun preset enregistré.</div>' +
      '<div class="small">Tape un nom à gauche puis clique <em>Enregistrer la config courante</em> pour créer ton premier snapshot.</div>' +
      '</div>';
    return;
  }
  box.innerHTML = list.map(name => `
    <div class="stat" style="margin:4px 0">
      <span style="color:var(--text); font-weight:600">${name}</span>
      <span>
        <button class="btn btn-primary" style="padding:4px 10px" onclick="loadPreset('${name}')">↑ Charger</button>
        <button class="btn btn-danger" style="padding:4px 10px" onclick="deletePreset('${name}')">🗑</button>
      </span>
    </div>
  `).join('');
}
async function savePreset() {
  const name = document.getElementById('presetName').value.trim();
  if (!name) return alert('Nom requis');
  await postJson('/api/presets/save', {name});
  document.getElementById('presetName').value = '';
  loadPresets();
}
async function loadPreset(name) {
  if (!await confirmDialog(`Charger le preset "${name}" ? La config courante sera écrasée.`,
                            'Charger un preset', 'Charger')) return;
  showLoading();
  try { await postJson('/api/presets/load', {name}); toast('Preset chargé', 'success'); }
  finally { hideLoading(); }
}
async function deletePreset(name) {
  if (!await confirmDialog(`Supprimer le preset "${name}" ?`, 'Supprimer', 'Supprimer')) return;
  // Sauvegarde le snapshot avant delete pour pouvoir annuler
  let snapshot = null;
  try { snapshot = await getJson(`/api/presets`).then(arr =>
    fetch('/api/presets/load', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({name, dryRun:true})}).catch(() => null));
  } catch (_) {}
  await postJson('/api/presets/delete', {name});
  loadPresets();
  // Note : la restauration ne peut pas régénérer le contenu exact si l'utilisateur
  // ne l'avait pas backupé. On propose tout de même l'undo en re-sauvegardant la
  // config courante sous le même nom.
  toastUndo(`Preset "${name}" supprimé.`, async () => {
    await postJson('/api/presets/save', {name});
    loadPresets();
  });
}

// ============================================================================
// Pressure sparkline
// ============================================================================
const pressHistory = [];
function pushPressure(p) {
  if (p == null) return;
  pressHistory.push({t: Date.now(), p});
  if (pressHistory.length > 120) pressHistory.shift();
}
function drawSpark() {
  const c = document.getElementById('pressSpark');
  if (!c) return;
  // Auto-resize buffer to displayed CSS size
  const w = c.clientWidth, h = c.clientHeight;
  if (w === 0) return;
  if (c.width !== w) c.width = w;
  if (c.height !== h) c.height = h;
  const ctx = c.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  if (!pressHistory.length) {
    ctx.fillStyle = '#888'; ctx.font = '11px sans-serif';
    ctx.fillText('aucune donnée', 8, h/2 + 4);
    return;
  }
  // Range: 0..safety threshold (or 100 by default)
  const max = Math.max(100, ...pressHistory.map(x=>x.p));
  const min = 0;
  const n = pressHistory.length;
  // Grid line at target if available
  if (state.pressure?.target) {
    const y = h - ((state.pressure.target - min) / (max - min)) * (h - 4) - 2;
    ctx.strokeStyle = 'rgba(0,208,132,.25)';
    ctx.setLineDash([3,3]);
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    ctx.setLineDash([]);
  }
  // Curve
  ctx.strokeStyle = '#00d084';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  pressHistory.forEach((s, i) => {
    const x = (i / (n - 1 || 1)) * w;
    const y = h - ((s.p - min) / (max - min)) * (h - 4) - 2;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
  // Last value label
  const last = pressHistory[pressHistory.length-1];
  ctx.fillStyle = '#00d084'; ctx.font = '10px ui-monospace, monospace';
  ctx.fillText(`${last.p.toFixed(1)} kPa`, w - 60, 12);
}
setInterval(() => {
  if (state.pressure?.has_sensor) pushPressure(state.pressure.pressure_kpa);
  drawSpark();
}, 500);

// ============================================================================
// Solo / Mute helpers
// ============================================================================
async function soloFlute(id)  { await postJson('/api/flute/solo', {id}); toast(`Solo flûte #${id}`); }
async function clearSolo()    { await postJson('/api/flute/solo', {id:-1}); toast('Solo libéré'); }

// ============================================================================
// Demo player
// ============================================================================
async function loadDemoMelodies() {
  const d = await getJson('/api/demo');
  if (!d) return;
  const sel = document.getElementById('demoMelodySel');
  if (sel.options.length !== d.melodies.length) {
    sel.innerHTML = d.melodies.map(m => `<option value="${m.id}">${m.name}</option>`).join('');
  }
  document.getElementById('demoState').textContent = d.playing ? `▶ ${d.current}` : 'arrêté';
  document.getElementById('demoState').className = 'pill ' + (d.playing ? 'ok' : '');
  const hb = document.getElementById('demoBadge');
  if (hb) {
    hb.style.display = d.playing ? 'inline-block' : 'none';
    hb.className = 'pill ok';
    hb.textContent = '▶ ' + (d.current || 'demo');
  }
}
async function demoPlay() {
  const id = +document.getElementById('demoMelodySel').value;
  const loop = document.getElementById('demoLoop').checked;
  await postJson('/api/demo/play', {id, loop});
  toast('Démo lancée');
  setTimeout(loadDemoMelodies, 200);
}
async function demoStop() {
  await postNoBody('/api/demo/stop');
  toast('Démo arrêtée');
  setTimeout(loadDemoMelodies, 200);
}
setInterval(() => {
  if (document.getElementById('page-play')?.classList.contains('active')) loadDemoMelodies();
}, 1000);

// ============================================================================
// Diagnostics
// ============================================================================
async function loadDiag() {
  const d = await getJson('/api/system');
  if (!d) return;
  // System info
  document.getElementById('diagSys').innerHTML = `
    <div class="stat"><span class="stat-label">Chip</span><span class="stat-value">${d.chip_model} rev${d.chip_revision} · ${d.chip_cores}c · ${d.cpu_freq_mhz}MHz</span></div>
    <div class="stat"><span class="stat-label">SDK</span><span class="stat-value">${d.sdk_version}</span></div>
    <div class="stat"><span class="stat-label">Heap libre / total</span><span class="stat-value">${(d.heap_free/1024).toFixed(1)} / ${(d.heap_size/1024).toFixed(1)} KB</span></div>
    <div class="stat"><span class="stat-label">Heap min observé</span><span class="stat-value ${d.heap_min < 20000 ? 'red' : ''}">${(d.heap_min/1024).toFixed(1)} KB</span></div>
    <div class="stat"><span class="stat-label">Flash</span><span class="stat-value">${(d.flash_size/1048576).toFixed(1)} MB</span></div>
    <div class="stat"><span class="stat-label">PSRAM libre</span><span class="stat-value">${d.psram_free ? (d.psram_free/1024).toFixed(1)+' KB' : 'absente'}</span></div>
    <div class="stat"><span class="stat-label">MAC</span><span class="stat-value">${d.mac}</span></div>
    <div class="stat"><span class="stat-label">Firmware</span><span class="stat-value">${d.fw_version}</span></div>
  `;
  // Flutes
  document.getElementById('diagFlutes').innerHTML = (d.flutes||[]).map(f => `
    <div class="card flute-card" style="margin-bottom:8px">
      <div style="font-weight:700; color:var(--green)">#${f.id} ${f.name}</div>
      <div class="row3">
        <div>Step: <b>${f.pin_step}</b> · Dir: <b>${f.pin_dir}</b> · En: <b>${f.pin_en}</b></div>
        <div>Solenoid: <b>${f.pin_solenoid}</b> (LEDC ${f.ledc_solenoid}) · Servo: <b>${f.pin_servo}</b> (T${f.servo_timer})</div>
        <div>Endstop: <b>${f.pin_endstop}</b> (${f.endstop_active}) ·
             ${f.endstop_triggered ? '<span class="pill ok">déclenché</span>' : '<span class="pill">libre</span>'}</div>
      </div>
      <div class="stat" style="margin-top:6px">
        <span class="stat-label">Position / Homé ?</span>
        <span class="stat-value">${f.position_mm?.toFixed?.(1) ?? '—'} mm · ${f.homed ? '✓' : '✗'}</span>
      </div>
    </div>
  `).join('') || '<div>Aucune flûte.</div>';
  // Pressure
  if (d.pressure) {
    const p = d.pressure;
    document.getElementById('diagPressure').innerHTML = `
      <div class="stat"><span class="stat-label">État</span><span class="stat-value">${p.state}</span></div>
      <div class="stat"><span class="stat-label">Pompe</span><span class="stat-value">${p.pump_on ? 'ON' : 'OFF'}</span></div>
      <div class="stat"><span class="stat-label">Capteur ?</span><span class="stat-value">${p.has_sensor ? 'oui (pin ' + p.adc_pin + ')' : 'non'}</span></div>
      ${p.has_sensor ? `<div class="stat"><span class="stat-label">Pression / ADC raw</span><span class="stat-value">${p.kpa.toFixed(1)} kPa · ${p.adc_raw} (sur 4095)</span></div>` : ''}
      <div class="stat"><span class="stat-label">Réservoir</span><span class="stat-value">${p.reservoir_ok ? 'OK' : 'VIDE'}</span></div>
      <div class="stat"><span class="stat-label">Défaut</span><span class="stat-value ${p.fault!=='none'?'red':''}">${p.fault}</span></div>
    `;
  } else {
    document.getElementById('diagPressure').innerHTML = '<div>Module pression désactivé.</div>';
  }
}
// Auto-refresh diag toutes les 1s si page ouverte
setInterval(() => {
  if (document.getElementById('page-diag')?.classList.contains('active')) loadDiag();
}, 1500);

// ============================================================================
// Restore / reboot
// ============================================================================
async function restoreFile(ev) {
  const file = ev.target.files[0];
  if (!file) return;
  if (!await confirmDialog(`Restaurer la configuration depuis "${file.name}" ? Toute la config courante sera écrasée.`,
                            'Restaurer', 'Restaurer')) return;
  showLoading();
  try {
    const text = await file.text();
    const json = JSON.parse(text);
    await postJson('/api/restore', json);
    toast('Configuration restaurée', 'success');
  } catch (e) { toast('JSON invalide ou erreur réseau', 'error'); }
  finally { hideLoading(); }
}
async function rebootSystem() {
  if (!await confirmDialog('Redémarrer l\'ESP32 ? Toutes les notes en cours seront coupées.',
                            'Redémarrer', 'Redémarrer')) return;
  await postNoBody('/api/system/reboot');
  toast('Redémarrage en cours...', 'warn', 5000);
}

async function sweepOne(id) { await postJson('/api/flute/sweep', {id}); toast('Sweep en cours'); }

// ============================================================================
// Theme
// ============================================================================
function applyTheme(t) {
  document.documentElement.setAttribute('data-theme', t);
  if (t === 'auto') localStorage.removeItem('theme');
  else              localStorage.setItem('theme', t);
}
function effectiveTheme() {
  const stored = localStorage.getItem('theme');
  if (stored === 'light' || stored === 'dark') return stored;
  // auto → suit le système
  return window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches
    ? 'dark' : 'light';
}
function toggleTheme() {
  // Cycle dark → light → auto
  const cur = localStorage.getItem('theme') || 'auto';
  const next = cur === 'dark' ? 'light' : cur === 'light' ? 'auto' : 'dark';
  if (next === 'auto') {
    localStorage.removeItem('theme');
    document.documentElement.setAttribute('data-theme', effectiveTheme());
    toast('Thème : auto (suit le système)');
  } else {
    applyTheme(next);
    toast('Thème : ' + (next === 'dark' ? 'sombre' : 'clair'));
  }
}
// Init thème (suit système si pas de préférence stockée)
document.documentElement.setAttribute('data-theme',
  localStorage.getItem('theme') || effectiveTheme());
// Réagit au changement système si en mode auto
if (window.matchMedia) {
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', e => {
    if (!localStorage.getItem('theme')) {
      document.documentElement.setAttribute('data-theme', e.matches ? 'dark' : 'light');
    }
  });
}

// ============================================================================
// Range editor (note range double slider)
// ============================================================================
function setupRangeEditor() {
  const a = document.getElementById('rangeMin');
  const b = document.getElementById('rangeMax');
  const sync = () => {
    let lo = +a.value, hi = +b.value;
    if (lo >= hi) { if (a === document.activeElement) lo = hi - 1; else hi = lo + 1; }
    a.value = lo; b.value = hi;
    document.getElementById('cfgNmin').value = lo;
    document.getElementById('cfgNmax').value = hi;
    document.getElementById('rngLblMin').textContent = noteName(lo);
    document.getElementById('rngLblMax').textContent = noteName(hi);
    document.getElementById('rangeLabel').textContent = `${noteName(lo)} – ${noteName(hi)} (${hi-lo+1} notes)`;
    const left  = (lo / 127) * 100;
    const right = ((127 - hi) / 127) * 100;
    document.getElementById('rangeSel').style.left  = left + '%';
    document.getElementById('rangeSel').style.right = right + '%';
  };
  a.addEventListener('input', sync);
  b.addEventListener('input', sync);
  // Aussi quand l'utilisateur édite les inputs numériques
  document.getElementById('cfgNmin').addEventListener('input', () => { a.value = document.getElementById('cfgNmin').value; sync(); });
  document.getElementById('cfgNmax').addEventListener('input', () => { b.value = document.getElementById('cfgNmax').value; sync(); });
}
function syncRangeFromInputs() {
  document.getElementById('rangeMin').value = document.getElementById('cfgNmin').value;
  document.getElementById('rangeMax').value = document.getElementById('cfgNmax').value;
  // déclencher sync
  document.getElementById('rangeMin').dispatchEvent(new Event('input'));
}

// ============================================================================
// Velocity curve preview
// ============================================================================
function drawVelCurve() {
  const c = document.getElementById('velCurveCanvas');
  if (!c) return;
  const w = c.clientWidth, h = c.clientHeight;
  if (w === 0) return;
  if (c.width !== w) c.width = w;
  if (c.height !== h) c.height = h;
  const ctx = c.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  const curve = +document.getElementById('cfgVelCurve').value;
  // Grille
  ctx.strokeStyle = 'rgba(128,128,128,.2)';
  for (let i = 1; i < 4; i++) {
    const y = (i / 4) * h;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }
  // Linéaire de référence
  ctx.strokeStyle = 'rgba(128,128,128,.4)'; ctx.setLineDash([3,3]);
  ctx.beginPath(); ctx.moveTo(0, h); ctx.lineTo(w, 0); ctx.stroke(); ctx.setLineDash([]);
  // Courbe
  ctx.strokeStyle = '#00d084'; ctx.lineWidth = 2; ctx.beginPath();
  for (let i = 0; i <= 127; i++) {
    let v = i / 127;
    if (curve === 1) v = v*v;
    else if (curve === 2) v = v*v*v;
    const x = (i / 127) * w;
    const y = h - v * h;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
  // Légende
  ctx.fillStyle = '#00d084'; ctx.font = '10px ui-monospace';
  const labels = ['linéaire', 'quadratique', 'cubique'];
  ctx.fillText(labels[curve], 4, 12);
}
window.addEventListener('resize', drawVelCurve);

// ============================================================================
// Health check (dashboard)
// ============================================================================
async function refreshHealth() {
  const box = document.getElementById('dashHealth');
  if (!box) return;
  const issues = [];
  // sys_state
  if (state.sys_state === 'ERROR')   issues.push({lvl:'error', msg:'Système en ERREUR — vérifier endstops'});
  if (state.sys_state === 'HOMING')  issues.push({lvl:'warn',  msg:'Homing en cours...'});
  // flutes
  (state.flutes||[]).forEach(f => {
    if (f.enabled && !f.homed) issues.push({lvl:'warn', msg:`Flûte #${f.id} ${f.name} : non homée`});
    if (f.muted)               issues.push({lvl:'warn', msg:`Flûte #${f.id} ${f.name} : mutée`});
  });
  // pressure
  if (state.pressure?.fault && state.pressure.fault !== 'none') {
    issues.push({lvl:'error', msg:`Pression : défaut ${state.pressure.fault}`});
  }
  if (state.pressure && !state.pressure.reservoir_ok) {
    issues.push({lvl:'error', msg:'Réservoir vide'});
  }
  // heap (via /api/system, fetch occasionnel)
  if (refreshHealth._lastSysFetch == null || Date.now() - refreshHealth._lastSysFetch > 5000) {
    refreshHealth._lastSysFetch = Date.now();
    try {
      const sys = await getJson('/api/system');
      if (sys) {
        refreshHealth._heapMin = sys.heap_min;
        refreshHealth._heapFree = sys.heap_free;
      }
    } catch(_) {}
  }
  if (refreshHealth._heapFree && refreshHealth._heapFree < 30000) {
    issues.push({lvl:'warn', msg:`Heap libre faible : ${(refreshHealth._heapFree/1024).toFixed(1)} KB`});
  }
  if (!issues.length) {
    box.innerHTML = '<div class="health-ok" style="font-size:14px">✓ Tout fonctionne normalement</div>';
  } else {
    box.innerHTML = issues.map(i =>
      `<div class="health-${i.lvl}" style="margin:3px 0">${i.lvl==='error'?'⚠':'•'} ${i.msg}</div>`
    ).join('');
  }
}
setInterval(refreshHealth, 1500);

// ============================================================================
// Keyboard shortcuts
// ============================================================================
function showShortcuts() {
  document.getElementById('shortcutsModal').classList.add('show');
}
document.addEventListener('keydown', e => {
  // Ctrl+S = sauvegarder la config si page Réglages ouverte et dirty
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') {
    if (document.getElementById('page-config')?.classList.contains('active') && _cfgDirty) {
      e.preventDefault(); saveFluteCfg();
    }
    return;
  }
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (e.repeat) return;
  if (isPlayPageActive() && KEYBOARD_MAP[e.key.toLowerCase()] != null) return;  // page Jouer = piano
  switch (e.key.toLowerCase()) {
    case 'h': postNoBody('/api/homing'); toast('Homing global'); break;
    case 'p': postNoBody('/api/panic');  toast('Panic !', 'warn'); break;
    case 'm': postJson('/api/flutes/all', {action:'muteAll'}); toast('Toutes mutées', 'warn'); break;
    case 'u': postJson('/api/flutes/all', {action:'unmuteAll'}); toast('Toutes démutées'); break;
    case 't': toggleTheme(); break;
    case 's': toggleSynth(); break;
    case '?': showShortcuts(); break;
  }
});

// ============================================================================
// Init
// ============================================================================
buildPiano();
setupRangeEditor();
setupDirtyTracking();
setupChannelStripContextMenu();
renderMacros();
bindWebMidiInputs();   // si Web MIDI n'est pas activé, juste un message d'invite
connectWs();
setTimeout(updateTabIndicator, 50);
// premier fetch immédiat
getJson('/api/status').then(s => {
  state = s; render(); refreshHealth();
  setTimeout(hideSplash, 400);
  // Première visite ? Lancer le tour après le splash
  if (!localStorage.getItem('tour_done')) {
    setTimeout(startTour, 900);
  }
});
// Hide splash après 3 s max si jamais le fetch traîne
setTimeout(hideSplash, 3000);
