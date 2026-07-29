/*
 * dom.js — safe DOM construction. No innerHTML anywhere (correction #25):
 * text goes through textContent / createTextNode, handlers through
 * addEventListener, so instrument names, SSIDs, preset/macro names, MIDI file
 * names and diagnostics can never inject markup.
 */

export function h(tag, props = {}, children = [], doc = document) {
  const el = doc.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null) continue;
    if (k === "text") el.textContent = v;                          // safe text
    else if (k === "class") el.className = v;
    else if (k === "dataset") for (const [dk, dv] of Object.entries(v)) el.dataset[dk] = dv;
    else if (k.startsWith("on") && typeof v === "function") el.addEventListener(k.slice(2).toLowerCase(), v);
    else el.setAttribute(k, String(v));                            // never innerHTML
  }
  for (const c of [].concat(children)) {
    if (c == null || c === false) continue;
    el.appendChild(typeof c === "object" ? c : doc.createTextNode(String(c)));
  }
  return el;
}

export function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); }

// Diff two keyed lists → what to add / remove / keep (for differential DOM
// updates instead of full rebuilds, correction #26).
export function diffKeys(oldKeys, newKeys) {
  const oldSet = new Set(oldKeys);
  const newSet = new Set(newKeys);
  return {
    add: newKeys.filter((k) => !oldSet.has(k)),
    remove: oldKeys.filter((k) => !newSet.has(k)),
    keep: newKeys.filter((k) => oldSet.has(k)),
  };
}

// Update only the text nodes whose value changed (avoids touching the DOM when
// nothing moved — e.g. a 5 Hz status frame where 2 values changed).
export function patchText(node, value) {
  const s = String(value);
  if (node.textContent !== s) { node.textContent = s; return true; }
  return false;
}
