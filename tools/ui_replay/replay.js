#!/usr/bin/env node
// UI-Nachstell-Lauf: spielt die Op-Folge eines Agent-Laufs über das Flow-UI
// nach (Playwright, headless Chromium) und misst den Bedienaufwand.
//
// Aufruf (dawd läuft schon auf --port, frischer --config, --no-audio):
//   NODE_PATH=~/dev/unspill/node_modules node tools/ui_replay/replay.js \
//     --ops <lauf>.ops.jsonl --run remix-1 --orig ~/dev/agent-daw-runs/remix-1/projekt \
//     --out <ordner> --port 8100 --size 1920x1080 [--limit N] [--from SEQ] [--only SEQ,SEQ]
//
// Je Op: UI-Weg (Treiber in drivers.js) mit Messung (meter.js), danach
// Abgleich mit dem API-Ergebnis. Ops ohne UI-Weg laufen per API (markiert).
// Ergebnis: <out>/records.jsonl (je Op) und <out>/shots/*.png.

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');
const { Meter } = require('./meter');
const drivers = require('./drivers');

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
const PORT = +args.port || 8100;
const [W, H] = (args.size || '1920x1080').split('x').map(Number);
const OUT = path.resolve(args.out);
const ORIG = args.orig.replace(/\/$/, '');
const NEWPROJ = path.join(OUT, 'projekt');
fs.mkdirSync(path.join(OUT, 'shots'), { recursive: true });

// ── Op-Klassen ────────────────────────────────────────────────────────
// agent: Meta-Abfragen des Agenten (Uhr, Befehlsliste, Zustand als Text) —
// ein Mensch sieht das im UI ohnehin; zählen nicht als Bedienung
const AGENT_ONLY = new Set(['ping', 'listOps', 'treeState', 'state', 'routing', 'quit', 'audioSettings',
                            'captureStatus', 'listDir', 'jobStatus', 'clipPeaks', 'listPlugins']);
// Lese-Ops: das Ergebnis ist eine Anzeige, kein Zustand
const READ_ONLY = new Set(['analyze', 'detectTempo', 'detectKey', 'analyzeStructure', 'findRepeats', 'compareReference',
  'compareVoice', 'compareDrums', 'compareStems', 'compareClasses', 'compareSeparated', 'describeVoice', 'describeTimbre',
  'describeDrums', 'describeLayers', 'audioToMidi', 'findRecipes', 'recipeInfo', 'findPresets', 'findSamples',
  'listParameters', 'listAutomatable', 'listSamples', 'pluginInfo', 'clipNotes', 'matchSound', 'detectSongBoundaries', 'play']);
// Ops, die Dateien schreiben, aber den Song nicht ändern
const FILE_OPS = new Set(['render', 'renderStems', 'renderBatch', 'exportMidi', 'separateStems', 'saveRecipe', 'rateRecipe']);
// Lange Jobs: die Rechenzeit wird nicht abgesessen — das UI bekommt das
// Original-Ergebnis aus dem Protokoll (Fortschritt/Toast werden geprüft)
const CANNED = new Set(['separateStems', 'matchSound', 'tuneSound', 'compareClasses', 'describeLayers', 'compareSeparated', 'splitLayer']);
// Vom UI gesendet, aber nur Anzeige/Abfrage
const UI_POLL = new Set(['position', 'levels', 'inputLevel', 'interfaceMeters', 'interfaceState', 'captureStatus', 'jobStatus',
  'state', 'treeState', 'clipPeaks', 'listAutomatable', 'listParameters', 'listSamples', 'pluginInfo', 'listPlugins',
  'sampleLibraries', 'listDir', 'findPresets', 'findSamples', 'findRecipes', 'recipeInfo', 'loop', 'play']);
const UNDOABLE = new Set(['addSection', 'updateSection', 'moveSection', 'addClip', 'addMidiClip', 'createTrack',
  'pickInstrument', 'favoriteClip', 'favoriteSection', 'deleteClip', 'undoDelete', 'trimClip', 'moveClip', 'setMidiNotes',
  'editNotes', 'setControllers', 'quantizeClip', 'addPlugin', 'removePlugin', 'renamePlugin', 'renameTrack', 'linkPlugin',
  'setChain', 'connect', 'disconnect', 'setAutomation', 'clearAutomation', 'setModifier', 'clearModifier', 'setParameter',
  'loadPreset', 'setPluginFile', 'setEdge', 'setWet', 'trackMix', 'laneActive', 'setSamples', 'splitClip', 'duplicateClip',
  'duplicateSection', 'setClip', 'deleteSection', 'addMidiFromAudio', 'stretchClip', 'warpClip', 'loopClip', 'processClip',
  'freezeTrack', 'flattenTrack', 'compLane', 'createGroup', 'setGroup', 'removeGroup', 'groupMix', 'addMarker',
  'updateMarker', 'deleteMarker', 'bounceNode']);

// ── Protokoll lesen, Schritte bilden ─────────────────────────────────
const all = fs.readFileSync(args.ops, 'utf8').trim().split('\n').map(l => JSON.parse(l));
const jobsDone = new Map();   // Job-Id → letzter jobStatus (done)
const jobPolls = new Map();
for (const e of all) if (e.op === 'jobStatus') {
  const j = e.params.job;
  jobPolls.set(j, (jobPolls.get(j) || 0) + 1);
  if (e.result && e.result.status && e.result.status !== 'running') jobsDone.set(j, e);
}

// applyRecipe protokolliert seine Teil-Ops vor sich selbst (gleiche Sekunde):
// die `steps` Ops davor, bei branched dazu der treeBranch — sie laufen im
// Nachstell-Lauf nur über applyRecipe, nicht doppelt
const recipeSub = new Set();
{
  const seqOps = all.filter(e => e.op !== 'jobStatus');
  seqOps.forEach((e, i) => {
    if (e.op !== 'applyRecipe' || !e.result || !e.result.ok) return;
    let n = e.result.steps || 0, j = i - 1;
    const t0 = Date.parse(e.ts);
    for (; j >= 0 && n > 0; j--) {
      if (['ping', 'state', 'treeState'].includes(seqOps[j].op)) continue;
      if (t0 - Date.parse(seqOps[j].ts) > 5000) break;
      recipeSub.add(seqOps[j].seq); n--;
    }
    if (e.result.branched) for (; j >= 0; j--) {
      if (seqOps[j].op === 'treeBranch') { if (t0 - Date.parse(seqOps[j].ts) < 5000) recipeSub.add(seqOps[j].seq); break; }
      if (!['ping', 'state', 'treeState'].includes(seqOps[j].op)) break;
    }
  });
}

// Schritt = Node, an dem die Op wirkt (Auto-Node, Branch, Checkout; Notizen
// an den genannten Node); Label aus dem Original-Baum
let origTree = {};
try { origTree = JSON.parse(fs.readFileSync(path.join(ORIG, 'tree.json'), 'utf8')); } catch (e) {}
const nodeLabel = id => ((origTree.nodes || []).find(n => n.id === id) || {}).label || ('#' + id);
let curNode = null, segment = 0, lastKey = null;
const plan = [];
for (const e of all) {
  if (e.op === 'jobStatus') continue;
  const r = e.result || {};
  let node = curNode;
  if (e.op === 'treeNote' && e.params.node !== undefined) node = e.params.node;
  if (['treeSeed', 'treeOpen', 'treeCheckout', 'treeBranch'].includes(e.op) && r.ok) curNode = node = r.node;
  if (r.autoNode) curNode = node = r.autoNode.node;
  if (e.op === 'applyRecipe' && r.ok && r.node) curNode = node = r.node;
  const key = String(node);
  if (key !== lastKey) { segment++; lastKey = key; }
  const job = r.job ? jobsDone.get(r.job) : null;
  const t0 = Date.parse(e.ts), t1 = job ? Date.parse(job.ts) : null;
  plan.push({ seq: e.seq, ts: e.ts, op: e.op, params: e.params, result: r, node, step: segment,
              stepLabel: node === null ? 'Start' : nodeLabel(node) + ' (#' + node + ')',
              job, jobSec: t1 ? (t1 - t0) / 1000 : null, jobPolls: r.job ? jobPolls.get(r.job) || 0 : 0 });
}

// ── Pfade und Ids umschreiben ────────────────────────────────────────
// Schreibziele des Agenten außerhalb des Projekts (z. B. <lauf>/stems,
// <lauf>/analyse-…/exports) landen im Ergebnisordner — die Original-Läufe
// bleiben unberührt; spätere Lese-Ops folgen der Umleitung
const RUNROOT = path.dirname(ORIG);
const WRITE_KEYS = { render: 'file', exportMidi: 'file', renderStems: 'dir', renderBatch: 'dir', separateStems: 'dir', treeSeed: 'dir' };
const writeMap = [];
function noteWrite (op) {
  const k = WRITE_KEYS[op.op];
  const t = k && op.params[k];
  if (!t || t.startsWith(ORIG + '/') || !t.startsWith(RUNROOT + '/')) return;
  if (!writeMap.some(([a]) => a === t)) writeMap.push([t, path.join(OUT, 'extern', path.relative(RUNROOT, t))]);
}
const nodeMap = new Map();     // Original-Node → Node im Nachstell-Lauf
const plugMap = new Map();     // Original-Modul → Modul im Nachstell-Lauf
function mapPlug (s) {
  if (typeof s !== 'string') return s;
  return s.replace(/\bp(\d+)\b/g, (m, n) => plugMap.get('p' + n) || m);
}
function rewrite (v, key) {
  if (typeof v === 'string') {
    let s = v.split(ORIG).join(NEWPROJ);
    for (const [a, b] of writeMap) if (s === a || s.startsWith(a + '/')) s = b + s.slice(a.length);
    if (['plugin', 'before', 'link', 'param', 'target', 'from', 'to'].includes(key)) s = mapPlug(s);
    return s;
  }
  if (typeof v === 'number' && (key === 'node' || key === 'parent')) return nodeMap.get(v) ?? v;
  if (Array.isArray(v)) return v.map(x => key === 'nodes' && typeof x === 'number' ? (nodeMap.get(x) ?? x) : rewrite(x, key));
  if (v && typeof v === 'object') {
    const o = {};
    for (const [k, x] of Object.entries(v)) o[k] = rewrite(x, k);
    return o;
  }
  return v;
}
function learnIds (orig, now) {
  if (!orig || !now) return;
  if (orig.autoNode && now.autoNode) nodeMap.set(orig.autoNode.node, now.autoNode.node);
  if (orig.node !== undefined && now.node !== undefined && typeof orig.node === 'number') nodeMap.set(orig.node, now.node);
  if (typeof orig.id === 'string' && typeof now.id === 'string' && /^p\d+$/.test(orig.id)) plugMap.set(orig.id, now.id);
}

// ── API direkt (Abkürzung, Folge-Dateien, Abgleich) ──────────────────
async function api (op, params = {}) {
  const r = await fetch('http://127.0.0.1:' + PORT + '/command', { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + (process.env.UNIDAW_TOKEN || '') },
    body: JSON.stringify({ ...params, op }) });
  return r.json();
}

// Fingerabdruck des Songs für den Zustandsvergleich (ohne Ids, Pfade, Version)
async function fingerprint () {
  const s = await api('state', { detail: true });
  if (!s.ok) return { error: s.error };
  const plugs = new Map(((s.routing || {}).plugins || []).map(p => [p.id, p]));
  const pn = id => { const p = plugs.get(id); return p ? (p.label || p.name) + (p.master ? '@master' : '') : id; };
  const r1 = x => typeof x === 'number' ? Math.round(x * 100) / 100 : x;
  const modded = new Set((s.modifiers || []).map(m => m.param));
  return {
    tracks: (s.trackList || []).map(t => ({ name: t.name, type: t.type, dest: t.dest, mute: t.mute, solo: t.solo,
      vol: modded.has('t' + t.index + '.volume') ? 'lfo' : r1(t.volumeDb), pan: modded.has('t' + t.index + '.pan') ? 'lfo' : r1(t.pan), routes: (t.routes || []).map(x => pn(x.id)),
      clips: (t.clips || []).map(c => ({ name: c.name, start: r1(c.start), len: r1(c.length), gain: r1(c.gainDb),
        fin: r1(c.fadeIn), fout: r1(c.fadeOut), rev: c.reversed, tp: r1(c.transpose), speed: r1(c.speed),
        warp: c.warp ? c.warp.markerCount : 0, loop: !!c.loop, mute: c.mute })) })),
    sections: (s.sections || []).map(x => ({ name: x.name, start: r1(x.start), end: r1(x.end), bpm: r1(x.bpm), sig: x.timeSig, color: x.color })),
    plugins: [...plugs.values()].map(p => ({ n: pn(p.id), cls: p.class, wet: r1(p.wet) })).sort((a, b) => a.n < b.n ? -1 : 1),
    chains: ((s.routing || {}).chains || []).map(c => ({ id: c.id, plugins: c.plugins.map(pn), toMain: c.toMain })),
    automation: (s.automation || []).map(a => ({ p: mapBack(a.param, pn), n: (a.points || []).length })),
    modifiers: (s.modifiers || []).map(a => ({ p: mapBack(a.param, pn), m: JSON.stringify(a.modifier && { ...a.modifier, low: undefined, high: undefined }) })),
  };
}
function mapBack (param, pn) { return String(param).replace(/\bp\d+\b/g, m => pn(m)); }

function diff (a, b, pre = '') {
  if (JSON.stringify(a) === JSON.stringify(b)) return [];
  if (typeof a !== 'object' || typeof b !== 'object' || !a || !b) return [pre + ': ' + JSON.stringify(a) + ' ≠ ' + JSON.stringify(b)];
  const out = [];
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const k of keys) out.push(...diff(a[k], b[k], pre ? pre + '.' + k : k));
  return out;
}

// Gleiche Parameter? Zahlen mit Toleranz; nur Schlüssel des Agenten zählen
function sameParams (agent, ui, missing = [], pre = '', lenient = false) {
  let ok = true;
  for (const [k, v] of Object.entries(agent)) {
    if (k === 'op') continue;
    const u = ui ? ui[k] : undefined;
    if (u === undefined) { missing.push(pre + k); ok = false; continue; }
    // hochgeladene Kopie (media/<name>[-2]) gilt als dieselbe Datei
    if (lenient && k === 'file' && typeof v === 'string' && typeof u === 'string'
        && u.split('/').pop().replace(/-\d+(\.\w+)$/, '$1') === v.split('/').pop()) continue;
    if (typeof v === 'number') { if (typeof u !== 'number' || Math.abs(u - v) > Math.max(0.011, Math.abs(v) * 0.005)) { missing.push(pre + k + '≈'); ok = false; } }
    else if (v && typeof v === 'object') { if (!sameParams(v, u, missing, pre + k + '.', lenient)) ok = false; }
    else if (v !== u) { missing.push(pre + k + '='); ok = false; }
  }
  return ok;
}

// ── Lauf ─────────────────────────────────────────────────────────────
(async () => {
  const exe = args.browser || path.join(process.env.HOME, '.cache/ms-playwright/chromium_headless_shell-1234/chrome-linux/headless_shell');
  const browser = await chromium.launch({ executablePath: fs.existsSync(exe) ? exe : undefined });
  const ctxB = await browser.newContext({ viewport: { width: W, height: H } });
  const page = await ctxB.newPage();
  const meter = new Meter(page, { width: W, height: H });
  const recOut = fs.createWriteStream(path.join(OUT, 'records.jsonl'), { flags: args.from ? 'a' : 'w' });

  // Mitschnitt der Befehle, die das UI sendet
  let rec = null;
  let inflight = new Map();
  let lastActivity = Date.now();
  page.on('request', (rq) => {
    if (rq.url().includes('/upload')) { inflight.set(rq, Date.now()); lastActivity = Date.now(); return; }
    if (!rq.url().endsWith('/command') || rq.method() !== 'POST') return;
    let body = {};
    try { body = JSON.parse(rq.postData() || '{}'); } catch (e) {}
    // Warten auf alles außer den Dauer-Polls (Position, Pegel …)
    const poll = ['position', 'levels', 'inputLevel', 'interfaceMeters', 'interfaceState', 'captureStatus', 'sampleLibraries'].includes(body.op);
    if (!poll) { inflight.set(rq, Date.now()); lastActivity = Date.now(); }
    if (rec && !['position', 'levels', 'inputLevel', 'interfaceMeters', 'captureStatus', 'progress'].includes(body.op))
      rec.sent.push({ op: body.op, params: body, req: rq });
  });
  const done = async (rq) => {
    if (inflight.delete(rq)) lastActivity = Date.now();
    if (!rec) return;
    const s = rec.sent.find(x => x.req === rq);
    if (s) { try { s.result = await (await rq.response()).json(); } catch (e) { s.result = null; } }
  };
  page.on('requestfinished', done);
  page.on('requestfailed', done);

  // Lange Jobs: Original-Antworten aus dem Protokoll
  let canned = null;   // { op, startResult, final }
  // Nicht rückgängig zu machende Ops (applyRecipe): die Anfrage des UI wird
  // mitgeschnitten, ausgeführt wird sie mit den Parametern des Agenten —
  // sonst liefe der Lauf auseinander (andere Spur, neue Spuren …)
  let swap = null;     // { op, params, uiParams, result }
  await page.route('**/command', async (route) => {
    let body = {};
    try { body = JSON.parse(route.request().postData() || '{}'); } catch (e) {}
    if (swap && body.op === swap.op && !swap.result) {
      swap.uiParams = body;
      swap.result = await api(swap.op, swap.params);
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify(swap.result) });
    }
    if (canned && body.op === canned.op && !canned.started) {
      canned.started = Date.now();
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ...canned.startResult, job: 'replay-job' }) });
    }
    if (canned && body.op === 'jobStatus' && body.job === 'replay-job') {
      // Fortschritt wie im Original: drei Zwischenstände, dann das Ergebnis
      canned.polls = (canned.polls || 0) + 1;
      if (canned.polls <= 3) return route.fulfill({ contentType: 'application/json',
        body: JSON.stringify({ ok: true, status: 'running', progress: canned.polls / 4, version: 0 }) });
      const fin = { ...canned.final };
      if (canned.op === 'separateStems') fin.addClipCommands = [];   // Stems liegen schon im Ordner
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify(fin) });
    }
    return route.continue();
  });

  // Warten, bis das UI fertig ist; Fortschrittsanzeige mitschreiben
  async function settle (maxMs = 120000) {
    const t0 = Date.now();
    const w = { ms: 0, busyShown: false, progress: false, toastPct: false, samples: 0 };
    await page.waitForTimeout(120);
    while (Date.now() - t0 < maxMs) {
      if (inflight.size === 0 && Date.now() - lastActivity > 300) break;
      const b = await page.evaluate(() => {
        const busy = document.querySelector('#busy');
        const bar = document.querySelector('#busyBar');
        const st = document.querySelector('#status');
        return { busy: busy && !busy.classList.contains('hide'), indet: bar && bar.classList.contains('indet'),
                 toast: st && !st.classList.contains('hide') ? st.textContent : '',
                 bg: !document.querySelector('#bgLoad').classList.contains('hide') };
      }).catch(() => ({}));
      w.samples++;
      if (b.busy) { w.busyShown = true; if (!b.indet) w.progress = true; }
      if (/\d+ ?%/.test(b.toast || '') || b.bg) w.toastPct = true;
      await page.waitForTimeout(250);
    }
    w.ms = Date.now() - t0;
    return w;
  }

  const ctx = { page, m: meter, api, settle, W, H, newProj: NEWPROJ, orig: ORIG, rewrite, nodeMap, plugMap, out: OUT,
                shot: async (name) => { const f = path.join(OUT, 'shots', name + '.png'); await page.screenshot({ path: f }); return f; },
                plan, pageRefresh: () => page.evaluate(() => refresh()).catch(() => {}) };

  await page.goto('http://127.0.0.1:' + PORT + '/');
  await page.waitForTimeout(1500);

  const from = +args.from || 0;
  const limit = +args.limit || Infinity;
  let n = 0;

  // --only SEQ,SEQ,…: nur diese Ops über das UI (Gegenprobe einzelner
  // Befunde); alle anderen laufen schnell per API, damit der Zustand passt —
  // Lese-Ops und lange Jobs ohne Wirkung auf den Song fallen dabei weg
  const only = args.only ? new Set(String(args.only).split(',').map(Number)) : null;
  let stale = false;
  const fastFail = [];
  async function fastApi (op, p) {
    if (recipeSub.has(op.seq) || AGENT_ONLY.has(op.op) || op.op === 'treeClose'
        || (op.op === 'treeOpen' && op.params.restored) || (op.result && op.result.ok === false)) return;
    if (FILE_OPS.has(op.op) || op.op === 'separateStems') { await drivers.fileFixup(ctx, op, p, { sent: [] }); return; }
    if (READ_ONLY.has(op.op) || CANNED.has(op.op)) return;
    // Dateien des Original-Projekts: das UI lädt sie hoch, die API nimmt sie direkt
    if (typeof p.file === 'string' && !fs.existsSync(p.file) && fs.existsSync(op.params.file)) p.file = op.params.file;
    const nu = drivers.nonUndoableFix[op.op];
    const r = op.op.startsWith('tree') && nu ? await nu(ctx, op, p, { sent: [] }, []) : await api(op.op, p);
    if (r) learnIds(op.result, r);
    if (r && r.ok === false) fastFail.push(op.seq + ' ' + op.op + ': ' + r.error);
    stale = true;
  }
  const earlierResults = [];
  let shots = 0;   // Messwerte für die Merklast

  for (const op of plan) {
    if (op.seq < from) continue;
    if (n++ >= limit) break;
    noteWrite(op);
    const p = rewrite(op.params);
    delete p.op;
    for (const k of ['file', 'dir']) if (WRITE_KEYS[op.op] === k && p[k]) fs.mkdirSync(k === 'dir' && op.op !== 'treeSeed' ? p[k] : path.dirname(p[k]), { recursive: true });
    if (only && !only.has(op.seq)) { await fastApi(op, p); continue; }
    // Neu laden; Anfragen, die das Neuladen abbrach, melden sich nicht mehr ab
    if (stale) {
      await page.reload({ waitUntil: 'domcontentloaded' }).catch(() => {});
      await page.waitForTimeout(1500); inflight.clear(); stale = false;
    }
    rec = { seq: op.seq, ts: op.ts, op: op.op, params: p, step: op.step, stepLabel: op.stepLabel, sent: [], partial: [] };
    meter.start(rec);

    const origOk = op.result.ok !== false;
    if (recipeSub.has(op.seq)) rec.mode = 'recipeStep';
    else if (AGENT_ONLY.has(op.op) || op.op === 'treeClose' || (op.op === 'treeOpen' && op.params.restored)) rec.mode = 'agent';
    else if (!origOk) rec.mode = 'agentError';

    let S0 = null, uiErr = null;
    const tStart = Date.now();
    if (!rec.mode) {
      const drv = drivers[op.op];
      const mutating = !READ_ONLY.has(op.op) && !FILE_OPS.has(op.op);
      if (mutating) S0 = await fingerprint();
      if (CANNED.has(op.op) && op.job) canned = { op: op.op, startResult: op.result, final: op.job.result };
      if (op.op === 'applyRecipe') swap = { op: op.op, params: p };
      try {
        if (!drv) throw new drivers.NoUi('no UI driver for ' + op.op);
        await drv(ctx, p, rec, op);
        rec.mode = rec.mode || (rec.partial.length ? 'uiPartial' : 'ui');
      } catch (e) {
        if (e instanceof drivers.NoUi) { rec.mode = 'noUi'; rec.reason = e.message; }
        else { rec.mode = 'uiFailed'; uiErr = e.message.split('\n')[0]; rec.error = uiErr; }
      }
      rec.wait = await settle(CANNED.has(op.op) ? 30000 : 180000);
      canned = null;
      rec.toast = await page.evaluate(() => { const s = document.querySelector('#status'); return s && !s.classList.contains('hide') ? s.textContent.slice(0, 200) : ''; }).catch(() => '');
      if (drivers.feedback[op.op] && rec.mode !== 'noUi') {
        try { rec.feedback = await drivers.feedback[op.op](ctx, p, rec); } catch (e) { rec.feedback = { error: e.message }; }
      }
      // Bildschirmfoto der aufwendigen Stellen (vor dem Schließen der Popover)
      const uu = rec.ui;
      if ((uu.wheel >= 15 || uu.views >= 3 || uu.offscreenDrag || uu.drags >= 12 || rec.mode === 'uiFailed') && shots < 80) {
        shots++;
        rec.shot = 'shots/' + op.seq + '-' + op.op + '.png';
        await page.screenshot({ path: path.join(OUT, rec.shot) }).catch(() => {});
      }
      // Escape schließt offene Popover (gehört nicht zur Messung)
      await page.keyboard.press('Escape').catch(() => {});

      // ── Abgleich und Angleichen ──
      const sentMut = rec.sent.filter(s => !UI_POLL.has(s.op));
      rec.sentOps = sentMut.map(s => s.op + (s.result && s.result.ok === false ? '!' : ''));
      const okMut = sentMut.filter(s => s.result && s.result.ok !== false);
      const same = (READ_ONLY.has(op.op) || FILE_OPS.has(op.op) ? rec.sent : sentMut).filter(s => s.op === op.op);
      const missing = [];
      const sameReq = same.length === 1 && okMut.length === 1 && sameParams(p, same[0].params, missing);
      rec.paramGap = [...new Set(missing)];

      if (swap) {
        const sw = swap; swap = null;
        if (sw.result) {
          const miss = [];
          rec.check = sameParams(p, sw.uiParams, miss) ? 'same request' : 'partial request';
          rec.paramGap = miss;
          rec.uiSent = sw.uiParams;
          learnIds(op.result, sw.result);
        } else {
          const r = await api(op.op, p);
          learnIds(op.result, r);
          rec.check = 'no request';
        }
      } else if (rec.mode === 'noUi') {
        const r = await api(op.op, p);
        rec.apiOk = r.ok !== false;
        learnIds(op.result, r);
        rec.check = 'api';
      } else if (!mutating) {
        rec.check = same.length ? (sameParams(p, same[same.length - 1].params, [], '', true) ? 'same request' : 'partial request') : 'no request';
        if (FILE_OPS.has(op.op) || op.op === 'separateStems') {
          // Folge-Ops lesen die Datei unter dem Pfad des Agenten
          await drivers.fileFixup(ctx, op, p, rec);
        }
      } else if (sameReq) {
        rec.check = 'same request';
        learnIds(op.result, same[0].result);
      } else {
        const Sui = await fingerprint();
        const undoable = okMut.filter(s => UNDOABLE.has(s.op)).length;
        if (undoable) {
          const u = await api('undo', { steps: undoable });
          rec.undone = u.ok ? (u.undone || []).length : 'undo failed: ' + u.error;
        }
        const Sback = await fingerprint();
        rec.revertOk = !!(S0 && S0.error) || JSON.stringify(Sback) === JSON.stringify(S0);
        const r = drivers.nonUndoableFix[op.op] ? await drivers.nonUndoableFix[op.op](ctx, op, p, rec, okMut) : await api(op.op, p);
        rec.apiOk = !r || r.ok !== false;
        if (r) learnIds(op.result, r);
        const Sapi = await fingerprint();
        const d = diff(Sui, Sapi);
        rec.check = d.length ? 'differs' : 'same state';
        rec.diff = d.slice(0, 12);
        if (!rec.revertOk) rec.diff.unshift('revert incomplete: ' + diff(Sback, S0).slice(0, 3).join(' | '));
      }
      // Werte, die der Fingerabdruck nicht kennt (Plugin-Parameter)
      if (drivers.verify[op.op] && rec.mode !== 'noUi' && ['same request', 'same state'].includes(rec.check)) {
        try {
          const d = await drivers.verify[op.op](ctx, p, op, rec);
          if (d && d.length) { rec.check = 'differs'; rec.diff = d; await api(op.op, p); rec.fixup = 'API ' + op.op + ' (values)'; }
        } catch (e) { rec.verifyError = e.message; }
      }
      await ctx.pageRefresh();
      await page.waitForTimeout(150);
    }
    rec.ms = Date.now() - tStart;

    // Merklast: getippte Zahlen, die vorher nur in einer Antwort standen
    rec.memo = [];
    for (const t of rec.ui.typed || [])
      for (const num of String(t).match(/-?\d+\.\d+|\d{3,}/g) || [])
        if (earlierResults.some(s => s.includes(num)) && !/^(100|120|1000)$/.test(num)) rec.memo.push(num);
    if (READ_ONLY.has(op.op) && op.result) earlierResults.push(JSON.stringify(op.result));

    const out = { ...rec };
    out.sent = undefined;
    recOut.write(JSON.stringify(out) + '\n');
    const u = rec.ui;
    console.log(op.seq, op.op.padEnd(16), (rec.mode || '').padEnd(10), (rec.check || '').padEnd(14),
                'c' + u.clicks + ' d' + u.dblclicks + ' g' + u.drags + ' k' + u.keys + ' w' + u.wheel + ' v' + u.views,
                rec.error || rec.reason || (rec.diff ? rec.diff.slice(0, 2).join(' | ') : '') || (rec.paramGap || []).join(','));
  }
  if (fastFail.length) console.log('API (schnell) abgelehnt:', fastFail.length, '\n  ' + fastFail.slice(0, 30).join('\n  '));
  recOut.end();
  await browser.close();
})().catch(e => { console.error(e); process.exit(1); });
