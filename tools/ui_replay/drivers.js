// UI-Wege je Op: so würde ein Mensch die Op im Flow-UI auslösen. Jeder
// Treiber bedient nur über ctx.m (Messung) und liest Lage/Zustand aus der
// Seite (globale Variablen des Flow-UI: lastState, tree, pps …).
// Kein UI-Weg → throw new NoUi(Grund); Teilweise (Parameter fehlen im UI)
// → rec.partial.push(Grund) und den erreichbaren Teil ausführen.

const fs = require('fs');
const os = require('os');
const path = require('path');

class NoUi extends Error {}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const q = (s) => s.replace(/["\\]/g, '\\$&');

// ── Lesen aus der Seite ───────────────────────────────────────────────
const ev = (ctx, fn, arg) => ctx.page.evaluate(fn, arg);
const shown = (ctx, sel) => ev(ctx, (s) => { const e = document.querySelector(s); return !!e && e.getClientRects().length > 0 && getComputedStyle(e).display !== 'none'; }, sel);
const stateOf = (ctx) => ev(ctx, () => lastState);
async function trackOf (ctx, idx) {
  return ev(ctx, (i) => (lastState.trackList || []).find(t => t.index === i) || null, idx);
}
// Die API nimmt die Hauptspur und meint ihre Lanes mit: Clip dort suchen
const LANE_JS = `(i, n) => { const ts = lastState.trackList || []; const t = ts.find(x => x.index === i);
  if (t && (t.clips || []).some(c => c.name === n)) return i;
  const l = ts.find(x => x.dest === i && (x.clips || []).some(c => c.name === n)); return l ? l.index : i; }`;
async function clipTrack (ctx, idx, name) { return ev(ctx, ([i, n, f]) => eval(f)(i, n), [idx, name, LANE_JS]); }
async function clipOf (ctx, idx, name) {
  const real = await clipTrack(ctx, idx, name);
  return ev(ctx, ([i, n]) => { const t = (lastState.trackList || []).find(x => x.index === i); return t ? (t.clips || []).find(c => c.name === n) || null : null; }, [real, name]);
}
async function xOfTime (ctx, t) { return ev(ctx, (s) => laneOffsetPx() + s * pps, t); }
async function timeOfSongBeat (ctx, b) { return ev(ctx, (x) => timeOfBeat(x, tempoSegs()), b); }

async function waitUi (ctx, ms = 30000) { await ctx.settle(ms); }

// Element nach Selektor und Text (ohne Warten wie bei locator)
async function byText (ctx, sel, text) {
  const h = await ctx.page.evaluateHandle(([s, t]) => [...document.querySelectorAll(s)].find(e => e.textContent.trim() === t)
    || [...document.querySelectorAll(s)].find(e => e.textContent.includes(t)) || null, [sel, text]);
  return h.asElement();
}

// ── Projekt, Strips, Routing-Ansicht ──────────────────────────────────
// Seit 2026-10-30 ohne Modus-Umschalter (alles immer sichtbar): nur ein
// offenes Projekt nötig
async function ensureProject (ctx) {
  if (!await ev(ctx, () => !!tree)) throw new NoUi('no project open yet (the UI shows only the folder picker)');
}

// Dock-Reiter (Variante B): Routing · Mixer · Notes · Recipes. Ein Klick auf den offenen Reiter klappt das Dock ein — darum
// erst den Stand lesen
async function ensureTab (ctx, tab) {
  if (!await ev(ctx, () => !!tree)) throw new NoUi('no project open yet (the UI shows only the folder picker)');
  const st = await ev(ctx, () => ({ tab: dockTab, folded: dockFolded }));
  if (st.tab === tab && !st.folded) return;
  await ctx.m.click('#dockTabs .docktab[data-tab="' + tab + '"]', { desc: 'tab ' + tab, view: 'tab ' + tab });
  await waitUi(ctx);
}

async function ensureRouting (ctx, mode = 'simple') {
  if (mode === 'mixer') return ensureTab(ctx, 'mixer');
  await ensureTab(ctx, 'routing');
  const cur = await ev(ctx, () => routingMode);
  if (cur === mode) return;
  const label = { simple: 'Simple', complex: 'Complex' }[mode];
  await ctx.m.click(() => byText(ctx, '#routing .rmodes button', label),
                    { desc: 'routing ' + label, view: 'routing ' + label });
  await waitUi(ctx);
}

// ── Song-Baum ─────────────────────────────────────────────────────────
// Reihenfolge der .node-Elemente wie renderTree/addLine
async function nodeHandle (ctx, id) {
  const h = await ctx.page.evaluateHandle((nid) => {
    lineDepth = new Map();
    const out = [];
    const walk = (start) => {
      const line = [start];
      for (let kids; (kids = byParentOf(line[line.length - 1].id).filter(kidVisible)).length;)
        line.push(kids.reduce((a, b) => chainLen(b.id) > chainLen(a.id) ? b : a));
      for (let i = line.length - 1; i >= 0; i--) {
        const n = line[i];
        const sides = byParentOf(n.id).filter(k => kidVisible(k) && k !== line[i + 1]);
        if (sides.length && !collapsed.has(n.id)) for (const s of sides.reverse()) walk(s);
        out.push(n.id);
      }
    };
    for (const n of [...byParentOf(0)].reverse()) if (kidVisible(n)) walk(n);
    const idx = out.indexOf(nid);
    return idx < 0 ? null : document.querySelectorAll('#tree .node')[idx] || null;
  }, id);
  return h.asElement();
}

async function nodeData (ctx, id) { return ev(ctx, (nid) => tree.nodes.find(n => n.id === nid) || null, id); }
async function currentNode (ctx) { return ev(ctx, () => tree && tree.current); }

// Node-Knopf klicken: versteckt und "⋯" da (seit 2026-10-30) → erst "⋯"
// (ein Klick statt Hover); ohne "⋯" (ältere UI) Hover wie bisher
async function clickNodeButton (ctx, id, pick, desc, opts = {}) {
  const hidden = await (await nodeButton(ctx, id, pick)).evaluate(b => {
    const cs = getComputedStyle(b.closest('.nbtns'));
    return parseFloat(cs.opacity) < 0.05 || cs.pointerEvents === 'none';
  });
  const more = hidden && await (await nodeHandle(ctx, id)).$('.nmore');
  if (more) {
    await ctx.m.click(async () => (await nodeHandle(ctx, id)).$('.nmore'), { desc: 'node actions ⋯' });
    return ctx.m.click(() => nodeButton(ctx, id, pick), { desc, ...opts });
  }
  return ctx.m.click(() => nodeButton(ctx, id, pick), { desc, hover: () => nodeHandle(ctx, id), ...opts });
}

async function nodeButton (ctx, id, pick) {
  const nh = await nodeHandle(ctx, id);
  if (!nh) return null;
  const b = await nh.evaluateHandle((el, how) => [...el.querySelectorAll('.nbtns button')].find(b =>
    how.title ? b.title.startsWith(how.title) : b.textContent === how.text) || null, pick);
  return b.asElement();
}

// ── Timeline ─────────────────────────────────────────────────────────
const rowSel = (i) => '#linearTracks .ltrow[data-track="' + i + '"]';

async function clipHandle (ctx, track, name) {
  track = await clipTrack(ctx, track, name);
  const h = await ctx.page.evaluateHandle(([t, n]) => {
    const row = document.querySelector('#linearTracks .ltrow[data-track="' + t + '"]');
    if (!row) return null;
    return [...row.querySelectorAll('.clip')].find(c => {
      const cn = c.querySelector('.cname');
      return cn && cn.textContent.replace(/^(\+\d+|⟲ |ƒx |⚠ )+/, '') === n;
    }) || null;
  }, [track, name]);
  return h.asElement();
}

async function clipButton (ctx, track, name, titleStart) {
  track = await clipTrack(ctx, track, name);
  const h = await ctx.page.evaluateHandle(([t, n, ti]) => {
    const row = document.querySelector('#linearTracks .ltrow[data-track="' + t + '"]');
    if (!row) return null;
    const clip = [...row.querySelectorAll('.clip')].find(c => c.querySelector('.cname').textContent.replace(/^(\+\d+|⟲ |ƒx |⚠ )+/, '') === n);
    if (!clip) return null;
    let bar = clip.querySelector('.clbtns');
    if (!bar && clip.nextElementSibling && clip.nextElementSibling.classList.contains('clbtns')) bar = clip.nextElementSibling;
    if (!bar) return null;
    return [...bar.querySelectorAll('button')].find(b => b.title.startsWith(ti)) || null;
  }, [track, name, titleStart]);
  return h.asElement();
}

// Clip greifbar machen wie eine Hand: zu schmal (< 24 px) → hineinzoomen;
// ganz verdeckt → die Marke +N am Clip darüber wählt ihn (holt ihn nach
// oben). Gibt einen freien Punkt im Clip zurück (relativ), sonst NoUi.
async function uncover (ctx, track, name) {
  // Suche von rechts (Hover am Ende); mit klebender Leiste von links — rechts
  // im sichtbaren Teil erscheint beim Anfahren die Leiste unter der Maus
  let fromLeft = false;
  // Die eigene Leiste wird beim Suchen gezeigt (sie erscheint beim Anfahren
  // unter der Maus); unten im Clip ist neben einer Leiste daneben noch Platz
  const freeAt = async () => (await clipHandle(ctx, track, name)).evaluate((el, left) => {
    const own = el.querySelector(':scope > .clbtns') || (el.nextElementSibling && el.nextElementSibling.classList.contains('clbtns') ? el.nextElementSibling : null);
    const was = own ? own.style.display : '';
    if (own) own.style.display = 'flex';
    const r = el.getBoundingClientRect();
    const xs = [];
    for (let x = r.right - 6; x > r.left + 2; x -= 4) xs.push(x);
    if (left) xs.reverse().splice(0, 3);   // nicht in die Trim-Zone links
    let hit = null;
    for (const x of xs) {
      for (const y of [r.height / 2, 6, r.height - 5]) {
        const px = x, py = r.top + y;
        if (px < 0 || px > innerWidth) continue;
        const t = document.elementFromPoint(px, py);
        if (t && (t === el || el.contains(t)) && !t.closest('.clbtns, .cwarp, .cunder')) { hit = { x: px - r.left, y }; break; }
      }
      if (hit) break;
    }
    if (own) own.style.display = was;
    return hit;
  }, fromLeft);
  for (let i = 0; i < 6 && (await (await clipHandle(ctx, track, name)).boundingBox()).width < 24; i++) await zoomStep(ctx, +1);
  // das rechte Ende ins Bild holen (gezählt), dann die freie Stelle suchen.
  // Seit 2026-10-30 klebt die Leiste am sichtbaren Teil (sticky): dann
  // reicht der sichtbare Teil des Clips, kein Weg zum Ende
  const sticky = await ev(ctx, () => { const b = document.querySelector('.clip > .clbtns'); return !!b && getComputedStyle(b).position === 'sticky'; });
  if (sticky) {
    fromLeft = true;
    await ctx.m.prepare(() => clipHandle(ctx, track, name), { desc: 'clip (for its buttons)', noSize: true });
  } else {
    const cl = await clipOf(ctx, track, name);
    if (cl) await reachTime(ctx, cl.start + cl.length);
    const w = (await (await clipHandle(ctx, track, name)).boundingBox()).width;
    await ctx.m.prepare(() => clipHandle(ctx, track, name), { desc: 'clip end (for its buttons)', at: { x: Math.max(4, w - 24), y: 8 }, noSize: true });
  }
  let free = await freeAt();
  if (!free && sticky) {
    // sichtbarer Teil zu schmal (die Leiste deckt ihn): Clip-Mitte ins Bild
    const w = (await (await clipHandle(ctx, track, name)).boundingBox()).width;
    await ctx.m.prepare(() => clipHandle(ctx, track, name), { desc: 'clip middle', at: { x: w / 2, y: 8 }, noSize: true });
    free = await freeAt();
  }
  for (let i = 0; !free && i < 6; i++) {
    const chip = await ctx.page.evaluateHandle(([t, n]) => {
      const row = document.querySelector('#linearTracks .ltrow[data-track="' + t + '"]');
      return row && [...row.querySelectorAll('.cunder')].find(b => b.title.includes('"' + n + '"')) || null;
    }, [await clipTrack(ctx, track, name), name]).then(x => x.asElement());
    if (!chip) break;
    await ctx.m.click(() => Promise.resolve(chip), { desc: 'covered clips +N' });
    await waitUi(ctx);
    if (await ev(ctx, (n) => selectedClip && selectedClip.name === n, name)) free = await freeAt();
  }
  if (!free) {
    // Wer deckt ihn? (Clips der Zeile, die den Clip überlappen, mit Lage)
    ctx.m.rec.ui.notes.push('covered by: ' + await (await clipHandle(ctx, track, name)).evaluate((el) => {
      const r = el.getBoundingClientRect();
      return [...el.parentElement.children].filter(c => c !== el && (c.classList.contains('clip') || c.classList.contains('clbtns'))).map(c => [c, c.getBoundingClientRect()])
        .filter(([, q]) => q.right > r.left && q.left < r.right && q.bottom > r.top && q.top < r.bottom)
        .map(([c, q]) => (c.querySelector('.cname') ? c.querySelector('.cname').textContent : c.className) + ' ' + Math.round(q.left - r.left) + '..' + Math.round(q.right - r.left) + '/' + Math.round(r.width)).join('; ');
    }));
    throw new NoUi('clip fully covered by an overlapping clip on its lane — not clickable');
  }
  return free;
}

async function clickClipButton (ctx, track, name, titleStart, desc, opts = {}) {
  const b = await clipButton(ctx, track, name, titleStart);
  if (!b) throw new Error('clip button not found: ' + desc + ' on ' + name);
  // Die Knopfleiste erscheint nur, solange die Maus auf dem Clip steht (oder
  // er gewählt ist); sie sitzt am rechten Ende. Also: Clip-Ende ins Bild,
  // dort hovern — an einer Stelle, die kein anderer Clip verdeckt.
  const free = await uncover(ctx, track, name);
  // frisch holen: der Klick auf +N zeichnet die Zeile neu (b hängt dann ab)
  const hid = await (await clipButton(ctx, track, name, titleStart)).evaluate(e => getComputedStyle(e.closest('.clbtns')).display === 'none');
  // Seit 2026-10-30 (Leiste am sichtbaren Teil): Clip wählen statt hovern —
  // die Leiste bleibt dann stehen; HOVER_CLIPS=1 = alter Weg (Hover)
  if (hid && !process.env.HOVER_CLIPS) {
    await ctx.m.click(() => clipHandle(ctx, track, name), { desc: 'clip (select)', at: free, wait: 320 });
  } else if (hid) {
    await ctx.m.hover(() => clipHandle(ctx, track, name), { at: free, desc: 'hover clip' });
    ctx.m.rec.ui.hoverOnly.push(desc);
    const still = await (await clipButton(ctx, track, name, titleStart)).evaluate(e => getComputedStyle(e.closest('.clbtns')).display === 'none');
    if (still) {
      ctx.m.rec.ui.notes.push('clip buttons need a selected clip (hover shows another clip): click it first');
      await ctx.m.click(() => clipHandle(ctx, track, name), { desc: 'clip (select)', at: free, wait: 320 });
    }
  }
  await ctx.m.click(() => clipButton(ctx, track, name, titleStart), { desc, ...opts });
}

async function selectTrack (ctx, idx) {
  const isSel = await ev(ctx, (s) => { const r = document.querySelector(s); return r ? r.classList.contains('sel') : null; }, rowSel(idx));
  if (isSel === null) throw new Error('track row not found: ' + idx);
  // Für Voice/Drums/Match/Tune zählt selectedTrack (Audio-Ziel) — der Klick
  // auf eine MIDI-Spur setzt nur selectedMidiTrack (gemessen, nicht korrigiert)
  if (isSel) return;
  await ctx.m.click(rowSel(idx) + ' .ltname', { desc: 'track header' });
}

async function selectSection (ctx, name) {
  const sel = await ev(ctx, () => selectedSection);
  if (sel === name) return;
  await ctx.m.click('#linearSections .section[data-name="' + q(name) + '"] .secname', { desc: 'section', wait: 420 });
  await waitUi(ctx);
}

async function deselectSection (ctx) {
  const sel = await ev(ctx, () => selectedSection);
  if (!sel) return;
  await ctx.m.click('#linearSections .section[data-name="' + q(sel) + '"] .secname', { desc: 'section (deselect)', wait: 420 });
  await waitUi(ctx);
}

// Cursor ins Lineal (rundet auf 1/20 s)
async function setCursor (ctx, t) {
  const cur = await ev(ctx, () => cursorPos);
  if (cur !== null && Math.abs(cur - t) < 0.026) return;
  await reachTime(ctx, t);
  const x0 = await ev(ctx, () => document.querySelector('#linearRuler').getBoundingClientRect().left);
  await ctx.m.click('#linearRuler', { desc: 'ruler (cursor)', at: { x: t * (await ev(ctx, () => pps)) + 1, y: 10 }, noSize: true });
}

// Popover-Feld nach seiner Beschriftung (span davor)
async function popField (ctx, label) {
  const h = await ctx.page.evaluateHandle((l) => {
    const pop = document.querySelector('#swatchPop');
    if (!pop) return null;
    const sp = [...pop.querySelectorAll(':scope > span')].find(s => s.textContent === l);
    return sp ? sp.nextElementSibling : null;
  }, label);
  return h.asElement();
}

async function fillPop (ctx, label, value) {
  const el = await popField(ctx, label);
  if (!el) throw new Error('popover field missing: ' + label);
  const kind = await el.evaluate(e => e.tagName + ':' + (e.type || ''));
  if (kind === 'INPUT:checkbox') {
    const on = await el.evaluate(e => e.checked);
    if (on !== !!value) await ctx.m.click(() => popField(ctx, label), { desc: 'popover ' + label });
  } else if (kind.startsWith('SELECT')) {
    await ctx.m.select(() => popField(ctx, label), value, { desc: 'popover ' + label });
  } else {
    await ctx.m.fill(() => popField(ctx, label), String(value), { desc: 'popover ' + label });
  }
}

// Range-Regler auf einen Wert ziehen (waagrecht oder senkrecht) — wie eine
// Hand, die den angezeigten Wert sieht: ziehen, ablesen, nachziehen (bis zu
// drei Korrekturen; ganze Pixel, darum bleibt bei kurzen Reglern ein Rest)
async function dragRange (ctx, target, value, desc) {
  for (let i = 0; i < 4; i++) {
    const h = await ctx.m.resolve(target);
    if (!h) throw new Error('slider missing: ' + desc);
    const info = await h.evaluate((el) => {
      const r = el.getBoundingClientRect();
      const cs = getComputedStyle(el);
      const vert = cs.writingMode.startsWith('vertical') || r.height > r.width * 1.5;
      return { min: +el.min, max: +el.max, step: +el.step || 0, value: +el.value, w: r.width, h: r.height, vert };
    });
    const tol = Math.max(info.step / 2, (info.max - info.min) * 0.002);
    if (Math.abs(info.value - value) <= tol) return;
    const thumb = info.vert ? 4.5 : 5;
    const len = (info.vert ? info.h : info.w) - 2 * thumb;
    const frac = (v) => Math.max(0, Math.min(1, (v - info.min) / (info.max - info.min)));
    const pos = (v) => info.vert ? { x: info.w / 2, y: info.h - thumb - frac(v) * len }
                                 : { x: thumb + frac(v) * len, y: info.h / 2 };
    const a = pos(info.value), b = pos(value);
    const dx = Math.round(b.x - a.x), dy = Math.round(b.y - a.y);
    if (!dx && !dy) { ctx.m.rec.ui.notes.push('slider resolution: ' + desc + ' ' + info.value + ' vs ' + value); return; }
    await ctx.m.drag(target, { desc: desc + (i ? ' (correct)' : ''), at: a, dx, dy, noSize: true, wait: 200 });
    await waitUi(ctx);
  }
}

// ── Referenz in der rechten Spalte (Variante B) ──────────────────────
// Referenz-Datei: aus der Auswahl #refPick, sonst Load… (Dateidialog);
// Quelle Mix | Vocals | Bass | Drums | Other; Bereich + Grid einmal setzen
// (Teil-Chip oder Felder). Stems des Agenten (<config>/stems/<Basis>[ a-b]/
// <stem>.wav) sind Stems der Referenz <Basis>: Quelle = Stem, Bereich um a
// verschoben (die UI nimmt die Stems des ganzen Songs aus dem Stem-Cache).
const STEM4 = ['vocals', 'bass', 'drums', 'other'];
const CFG_STEMS = path.join(os.homedir(), '.config/unidaw/stems');
const CFG_REFS = path.join(os.homedir(), '.config/unidaw/references');

// Woher eine Datei als Referenz-Quelle kommt: { ref, source, offset, end?, stems? }
function refSourceOf (ctx, file) {
  const m = /\/stems\/([^/]+)\/(vocals|bass|drums|other)\.wav$/.exec(file || '');
  if (m && !/ (6s|53)$/.test(m[1])) {
    const rm = / (\d+(?:\.\d+)?)-(\d+(?:\.\d+)?)$/.exec(m[1]);
    const base = rm ? m[1].slice(0, rm.index) : m[1];
    const whole = path.join(path.dirname(path.dirname(file)), base);
    const cands = [path.join(CFG_REFS, base + '.wav'), path.join(ctx.newProj, 'renders', base + '.wav'), path.join(ctx.newProj, 'media', base + '.wav')];
    const ref = cands.find(f => fs.existsSync(f));
    const stems = {};
    for (const k of STEM4) { const f = path.join(fs.existsSync(whole) ? whole : path.join(CFG_STEMS, base), k + '.wav'); if (fs.existsSync(f)) stems[k] = f; }
    if (ref && Object.keys(stems).length === 4) return { ref, source: m[2], offset: rm ? +rm[1] : 0, end: rm ? +rm[2] : undefined, stems };
  }
  return { ref: file, source: 'mix', offset: 0 };
}

// Referenz-Datei wählen: Auswahl (frühere des Projekts) oder Load…
async function ensureRefFile (ctx, file) {
  if (!file) throw new NoUi('no reference file');
  if (!fs.existsSync(file)) throw new NoUi('reference file missing: ' + file);
  const proj = await ev(ctx, () => tree && tree.projectDir);
  ctx.refUp = ctx.refUp || {};
  const key = proj + '|' + file;
  const up = ctx.refUp[key];
  const cur = await ev(ctx, () => refFile());
  if (up && cur === up) return up;
  const inList = up && await ev(ctx, (u) => [...document.querySelectorAll('#refPick option')].some(o => o.value === u), up);
  if (inList) {
    await ctx.m.select('#refPick', up, { desc: 'reference list' });
    await waitUi(ctx);
    return up;
  }
  await ctx.m.fileDialog('#refLoad', file, { desc: 'Load reference…' });
  await waitUi(ctx, 120000);
  const now = await ev(ctx, () => refFile());
  ctx.refUp[key] = now;
  return now;
}

// Quelle (Mix oder Stem) und Referenz; gibt den Versatz der Stem-Zeit zurück
async function ensureSource (ctx, file, opts = {}) {
  const s = refSourceOf(ctx, file);
  const up = await ensureRefFile(ctx, s.ref);
  if (s.stems) {
    // "aus dem Stem-Cache": in diesem Browser schon einmal getrennt (sonst
    // holte der Klick sie per separateStems aus dem Cache — derselbe Klick)
    await ev(ctx, ([u, st]) => { const k = 'dawd.refStems.' + u; if (!localStorage.getItem(k)) { lsSet(k, st); syncReference(); } }, [up, s.stems]);
  }
  if (!opts.noSource) {
    const cur = await ev(ctx, () => refState().source || 'mix');
    if (cur !== s.source) {
      await ctx.m.click('#refSrc button[data-src="' + s.source + '"]', { desc: 'reference source ' + s.source });
      await waitUi(ctx);
    }
  }
  return s;
}

// Bereich und Grid der Referenz setzen (nur was abweicht): Teil-Chip, Whole
// oder die Felder; r in Sekunden der Quelle des Agenten (offset = Stem-Start)
const near = (a, b) => a !== undefined && b !== undefined && Math.abs(a - b) < 0.006;
async function ensureRange (ctx, r, offset = 0, dflt = {}) {
  const want = {};
  if (r.start !== undefined || r.end !== undefined) {
    want.start = (r.start ?? 0) + offset;
    want.end = r.end !== undefined ? r.end + offset : dflt.end;
  } else if (offset || dflt.end !== undefined) { want.start = offset; want.end = dflt.end; }
  if (r.bpm !== undefined) want.bpm = r.bpm;
  if (r.firstDownbeat !== undefined) want.firstDownbeat = r.firstDownbeat + offset;
  const st = await ev(ctx, () => refState());
  if (want.start === undefined || want.end === undefined) {
    if (st.start !== undefined && want.start === undefined) {
      await ctx.m.click(() => byText(ctx, '#refParts button', 'Whole'), { desc: 'reference range Whole' });
      await waitUi(ctx);
    }
  } else if (!near(st.start, want.start) || !near(st.end, want.end)) {
    const parts = await ev(ctx, () => refParts());
    const i = parts.findIndex(x => near(x.start, want.start) && near(x.end, want.end));
    if (i >= 0) {
      await ctx.m.click(() => ctx.page.evaluateHandle((n) => document.querySelectorAll('#refParts button')[n], i).then(h => h.asElement()), { desc: 'reference part chip' });
      await waitUi(ctx);
    } else {
      const order = st.end !== undefined && want.start >= st.end ? [['#refEnd', 'end'], ['#refStart', 'start']] : [['#refStart', 'start'], ['#refEnd', 'end']];
      for (const [sel, k] of order) {
        if (near(st[k], want[k])) continue;
        await ctx.m.fill(sel, String(Math.round(want[k] * 1000) / 1000), { desc: 'reference range ' + k });
        await ctx.m.press('Enter');
        await waitUi(ctx);
      }
    }
  }
  const st2 = await ev(ctx, () => refState());
  for (const [sel, k] of [['#refBpm', 'bpm'], ['#refDown', 'firstDownbeat']]) {
    if (want[k] === undefined || near(st2[k], want[k])) continue;
    await ctx.m.fill(sel, String(Math.round(want[k] * 1000) / 1000), { desc: 'reference grid ' + k });
    await ctx.m.press('Enter');
    await waitUi(ctx);
  }
}

// Quelle + Bereich in einem (die meisten Referenz-Knöpfe)
async function ensureRef (ctx, file, r = {}, opts = {}) {
  const s = await ensureSource(ctx, file, opts);
  await ensureRange(ctx, r, s.offset, { end: s.end });
  return s;
}

// Parameter, die die Referenz-Leiste nicht kennt
function refPart (rec, ref, keys) {
  const miss = keys.filter(k => ref && ref[k] !== undefined);
  if (miss.length) rec.partial.push(miss.join('/') + ' not in UI');
}

async function openMenu (ctx, sel, name) {
  if (await ev(ctx, (s) => document.querySelector(s).nextElementSibling.classList.contains('open'), sel)) return;
  await ctx.m.click(sel, { desc: name + ' ▾', view: 'menu ' + name });
}

// ── Plugin-Karten ─────────────────────────────────────────────────────
async function cardHandle (ctx, pid) {
  const h = await ctx.page.evaluateHandle((id) => [...document.querySelectorAll('#routing .plugcard')].find(c => {
    const t = c.querySelector('h4'); return !!t && new RegExp('· ' + id + '(?![0-9])').test(t.textContent);
  }) || null, pid);
  return h.asElement();
}

async function moduleNameHandle (ctx, pid) {
  const h = await ctx.page.evaluateHandle((id) =>
    document.querySelector('#routing .slot[data-plugin="' + id + '"] .sname')
    || document.querySelector('#routing .chain[data-bus="' + id + '"] .sname') || null, pid);
  return h.asElement();
}

async function openCard (ctx, pid) {
  await ensureRouting(ctx, 'simple');
  if (await ev(ctx, (id) => openParams.has(id), pid)) return;
  if (!await moduleNameHandle(ctx, pid)) throw new Error('module not in routing view: ' + pid);
  await ctx.m.click(() => moduleNameHandle(ctx, pid), { desc: 'plugin name (card)', view: 'plugin card', wait: 300 });
  await waitUi(ctx);
}

async function paramRow (ctx, pid, name) {
  const card = await cardHandle(ctx, pid);
  if (!card) return null;
  const h = await card.evaluateHandle((c, n) => [...c.querySelectorAll('label.param')].find(l => l.firstChild && l.firstChild.textContent === n) || null, name);
  return h.asElement();
}

// Parameter in der Karte; fehlt er (Limit 80), sucht ihn die Parameter-Suche
async function findParamRow (ctx, pid, name) {
  let row = await paramRow(ctx, pid, name);
  if (row) return row;
  const inp = async () => (await cardHandle(ctx, pid)).$('.paramsearch input[type=search]');
  if (!await inp()) return null;
  await ctx.m.fill(inp, name, { desc: 'parameter search' });
  await sleep(700);
  await waitUi(ctx);
  return paramRow(ctx, pid, name);
}

// Inline-Bearbeitung: nach dem Doppelklick muss ein Eingabefeld den Fokus
// haben — sonst landen die Tasten in den Kürzeln der Seite (R = Record …)
async function editOpen (ctx) {
  return ev(ctx, () => { const a = document.activeElement; return !!a && (a.isContentEditable || a.tagName === 'INPUT' || a.tagName === 'TEXTAREA'); });
}
async function needEdit (ctx, what) {
  if (!await editOpen(ctx)) throw new Error('inline edit did not open: ' + what);
}

async function renameInline (ctx, targetFn, name, desc) {
  await ctx.m.click(targetFn, { desc, dbl: true, wait: 150 });
  await needEdit(ctx, desc);
  await ctx.m.press('Control+a');
  await ctx.m.type(name);
  await ctx.m.press('Enter');
  await waitUi(ctx);
}

// Antwort des UI auf eine Op (aus dem Mitschnitt)
const sentResult = (rec, op) => { const s = [...rec.sent].reverse().find(x => x.op === op && x.result); return s ? s.result : null; };

// ══ Treiber ═══════════════════════════════════════════════════════════
const D = {};

D.treeSeed = async (ctx, p, rec) => {
  const dir = p.dir.replace(/\/$/, '');
  const parent = path.dirname(dir), name = path.basename(dir);
  const open = await shown(ctx, '#seedPanel') ? '#seedPanel button.primary' : '#projBtn';
  await ctx.m.click(open, { desc: 'open project folder', view: 'folder dialog', wait: 400 });
  await waitUi(ctx);
  // Vom Startordner zum Elternordner klicken
  for (let guard = 0; guard < 20; guard++) {
    const at = await ev(ctx, () => browsePath);
    if (at === parent) break;
    if (!parent.startsWith(at.replace(/\/$/, '') + '/')) {
      await ctx.m.click('#dirPick .dirfoot button.ghost', { desc: 'folder up', wait: 300 });
    } else {
      const next = parent.slice(at.replace(/\/$/, '').length + 1).split('/')[0];
      const bh = () => ctx.page.evaluateHandle((n) => [...document.querySelectorAll('#dirList button')].find(b => b.lastChild && b.lastChild.textContent === n && !b.classList.contains('recentdir')) || null, next).then(x => x.asElement());
      if (!await bh()) throw new Error('folder not listed: ' + next);
      await ctx.m.click(bh, { desc: 'folder ' + next, wait: 450 });
    }
    await waitUi(ctx);
  }
  await ctx.m.click('#dirNewBtn', { desc: '+ New folder' });
  await ctx.m.click('#dirNewName', { desc: 'folder name' });
  await ctx.m.type(name);
  await ctx.m.click('#dirNewRow button', { desc: 'Create', wait: 400 });
  await waitUi(ctx);
  await ctx.m.click('#dirSelBtn', { desc: 'Start a song here', wait: 500 });
  await waitUi(ctx);
  if (p.label) {
    const id = await currentNode(ctx);
    await renameInline(ctx, async () => (await nodeHandle(ctx, id)).$('.nlabel'), p.label, 'node label');
  }
};

// Projekt wechseln: Ordnerdialog, zuletzt geöffnete Songs stehen oben
D.treeOpen = async (ctx, p, rec) => {
  const dir = p.dir.replace(/\/$/, '');
  if ((await ev(ctx, () => tree && tree.projectDir.replace(/\/$/, ''))) === dir) return;
  await ctx.m.click('#projBtn', { desc: 'project folder', view: 'folder dialog', wait: 400 });
  await waitUi(ctx);
  const rh = () => ctx.page.evaluateHandle((d) => [...document.querySelectorAll('#dirList .recentdir')].find(b => b.title.startsWith(d + ' ')) || null, dir).then(x => x.asElement());
  if (!await rh()) throw new NoUi('project not in the recent list');
  await ctx.m.click(rh, { desc: 'recent song', wait: 500 });
  await waitUi(ctx, 120000);
};

D.treeLabel = async (ctx, p) => {
  if (!await nodeHandle(ctx, p.node)) throw new Error('node not visible: ' + p.node);
  await renameInline(ctx, async () => (await nodeHandle(ctx, p.node)).$('.nlabel'), p.label, 'node label');
};

D.treeNote = async (ctx, p, rec) => {
  const id = p.node ?? await currentNode(ctx);
  const n = await nodeData(ctx, id);
  if (!n) throw new Error('node missing: ' + id);
  if (!await nodeHandle(ctx, id)) throw new NoUi('node hidden in the tree');
  if (n.note) {
    await ctx.m.click(async () => { const h = await nodeHandle(ctx, id); return (await h.evaluateHandle(e => e.nextElementSibling && e.nextElementSibling.classList.contains('nnote') ? e.nextElementSibling : null)).asElement(); },
                      { desc: 'node note', wait: 150 });
    if (p.append) { await ctx.m.press('Control+End'); await ctx.m.press('Enter'); }
    else await ctx.m.press('Control+a');
  } else {
    await clickNodeButton(ctx, id, { text: '✎' }, 'add note ✎', { wait: 150 });
  }
  await ctx.m.type(p.text, { free: true });
  await ctx.m.press('Control+Enter');
  await waitUi(ctx);
};

D.treeBranch = async (ctx, p, rec) => {
  const id = p.node ?? await currentNode(ctx);
  if (!await nodeButton(ctx, id, { title: 'Fork a variant' })) throw new NoUi('no fork button at node ' + id);
  await clickNodeButton(ctx, id, { title: 'Fork a variant' }, 'fork ⑂', { wait: 300 });
  await waitUi(ctx);
  if (p.label) {
    const nid = await currentNode(ctx);
    await renameInline(ctx, async () => (await nodeHandle(ctx, nid)).$('.nlabel'), p.label, 'node label');
  }
};

D.treeCheckout = async (ctx, p) => {
  if ((await currentNode(ctx)) === p.node) return;
  if (!await nodeHandle(ctx, p.node)) throw new NoUi('node hidden in the tree');
  await ctx.m.click(async () => (await nodeHandle(ctx, p.node)).$('.nlabel'), { desc: 'node (checkout)', wait: 450 });
  await waitUi(ctx, 120000);
};

D.treeTerminate = async (ctx, p) => {
  const pick = { title: p.on === false ? 'Activate this node' : 'Terminate this node' };
  if (!await nodeButton(ctx, p.node, pick)) throw new NoUi('terminate only on leaves without favorite');
  await clickNodeButton(ctx, p.node, pick, 'terminate ■ (arm)', { wait: 150 });
  await clickNodeButton(ctx, p.node, { title: 'Click again' }, 'terminate ■ (confirm)', { wait: 150 });
  await waitUi(ctx);
};

D.treeFavorite = async (ctx, p) => {
  const n = await nodeData(ctx, p.node);
  const pick = { title: n && n.favorite ? 'Unfavorite' : 'Favorite' };
  if (!await nodeButton(ctx, p.node, pick)) throw new NoUi('no favorite button');
  await clickNodeButton(ctx, p.node, pick, 'favorite ★');
  await waitUi(ctx);
};

// ── Sections ──
D.updateSection = async (ctx, p, rec) => {
  const secSel = (n) => '#linearSections .section[data-name="' + q(n) + '"]';
  let name = p.name;
  const tempo = ['bpm', 'timeSig', 'bars', 'ramp'].filter(k => p[k] !== undefined);
  if (tempo.length) {
    await ctx.m.click(secSel(name) + ' .sectempo', { desc: 'tempo tag', view: 'tempo popover', wait: 200 });
    if (p.bpm !== undefined) await fillPop(ctx, 'BPM', p.bpm);
    if (p.timeSig !== undefined) await fillPop(ctx, 'Time', p.timeSig);
    if (p.bars !== undefined) await fillPop(ctx, 'Bars', p.bars);
    if (p.ramp !== undefined) await fillPop(ctx, 'Ramp to next', p.ramp);
    await ctx.m.press('Enter');
    await waitUi(ctx);
  }
  if (p.label && p.label !== name) {
    await ctx.m.click(secSel(name) + ' .secname', { desc: 'section name', dbl: true, wait: 150 });
    await needEdit(ctx, 'section name');
    await ctx.m.press('Control+a');
    await ctx.m.type(p.label);
    await ctx.m.press('Enter');
    await waitUi(ctx);
  }
  if (p.color) {
    const sel = secSel(p.label || name);
    await ctx.m.click(sel + ' .secbtn', { desc: 'section colour', hover: sel, view: 'colour swatches' });
    const inPal = await ev(ctx, (c) => PALETTE.slice(0, 9).map(x => x.toLowerCase()).includes(c.toLowerCase()), p.color);
    if (inPal) await ctx.m.click('#swatchPop .swatch[title="' + p.color + '"]', { desc: 'swatch' });
    else {
      // eigener Farbton: System-Farbdialog (nicht bedienbar) — Wert wie dort gewählt
      await ctx.m.click('#swatchPop .swatch.custom', { desc: 'custom colour' });
      rec.ui.fileDialogs++;
      rec.partial.push('custom colour via the system colour dialog');
      await ev(ctx, (c) => { const i = document.querySelector('#swatchPop input[type=color]'); i.value = c; i.dispatchEvent(new Event('change')); }, p.color.toLowerCase());
    }
    await waitUi(ctx);
  }
  if (p.start !== undefined || p.length !== undefined || p.end !== undefined) {
    const s = (await stateOf(ctx)).sections.find(x => x.name === (p.label || name));
    const pps = await ev(ctx, () => pps);
    if (p.length !== undefined || p.end !== undefined) {
      const end = p.end !== undefined ? p.end : s.start + p.length;
      const box = await (await ctx.m.resolve(secSel(p.label || name))).boundingBox();
      await ctx.m.drag(secSel(p.label || name), { desc: 'section end edge', at: { x: box.width - 2, y: box.height / 2 }, dx: (end - s.end) * pps });
      await waitUi(ctx);
    }
    if (p.start !== undefined) {
      await ctx.m.drag(secSel(p.label || name), { desc: 'section start edge', at: { x: 2, y: 8 }, dx: (p.start - s.start) * pps });
      await waitUi(ctx);
    }
  }
};

// Favoriten (seit 2026-10-30): Stern an der Section bzw. im Clip-Popover;
// on = Zielzustand (ohne: umschalten)
D.favoriteSection = async (ctx, p) => {
  const sec = ((await stateOf(ctx)).sections || []).find(x => x.name === p.name);
  if (!sec) throw new Error('section missing ' + p.name);
  if (p.on !== undefined && !!sec.favorite === !!p.on) return;
  const sel = '#linearSections .section[data-name="' + q(p.name) + '"]';
  await ctx.m.click(sel + ' .secbtn[title*="avorite this section"]', { desc: 'section favorite ★', hover: sel });
  await waitUi(ctx);
};

D.deleteSection = async (ctx, p) => {
  const sel = '#linearSections .section[data-name="' + q(p.name) + '"]';
  for (let i = 0; i < 2; i++)
    await ctx.m.click(sel + ' .secbtn.danger', { desc: 'delete section (' + (i ? 'confirm' : 'arm') + ')', hover: sel, wait: 150 });
  await waitUi(ctx);
};

D.duplicateSection = async (ctx, p, rec) => {
  const sel = '#linearSections .section[data-name="' + q(p.name) + '"]';
  await ctx.m.click(() => byText(ctx, sel + ' .secbtn', '⧉'), { desc: 'duplicate section ⧉', hover: sel });
  await waitUi(ctx);
  const r = sentResult(rec, 'duplicateSection');
  if (r && r.ok && p.newName && r.name !== p.newName) await D.updateSection(ctx, { name: r.name, label: p.newName }, rec);
  if (p.after && p.after !== p.name) rec.partial.push('copy lands right after the original');
};

D.addSection = async (ctx, p, rec) => {
  const secs = (await stateOf(ctx)).sections || [];
  let anchor, edge;
  if (p.position === 'first' || (p.position === 'before' && p.before)) {
    anchor = p.before || secs[0].name; edge = 'left';
  } else {
    anchor = p.after || secs[secs.length - 1].name; edge = 'right';
  }
  const sel = '#linearSections .section[data-name="' + q(anchor) + '"]';
  const box = await (await ctx.m.resolve(sel)).boundingBox();
  await ctx.m.click(sel, { desc: 'section edge (insert)', dbl: true, at: { x: edge === 'left' ? 2 : box.width - 2, y: box.height / 2 } });
  await waitUi(ctx);
  const r = sentResult(rec, 'addSection');
  if (!r || !r.ok) return;
  if (p.name && r.name !== p.name) await D.updateSection(ctx, { name: r.name, label: p.name }, rec);
  const extra = {};
  for (const k of ['bpm', 'timeSig', 'bars']) if (p[k] !== undefined) extra[k] = p[k];
  if (p.length !== undefined) rec.partial.push('length: insert makes 30 s, then drag the edge');
  if (Object.keys(extra).length) await D.updateSection(ctx, { name: p.name || r.name, ...extra }, rec);
  if (p.length !== undefined && p.bars === undefined) await D.updateSection(ctx, { name: p.name || r.name, length: p.length }, rec);
};

D.moveSection = async (ctx, p, rec) => {
  const st = await stateOf(ctx);
  const secs = st.sections;
  const s = secs.find(x => x.name === p.name);
  let to;
  if (p.after) { const a = secs.find(x => x.name === p.after); to = a.end; }
  else if (p.before) { const b = secs.find(x => x.name === p.before); to = b.start; }
  else if (p.position === 'first') to = 0;
  else if (p.position === 'last') to = secs[secs.length - 1].end;
  else throw new NoUi('moveSection target not understood');
  const pps = await ev(ctx, () => pps);
  const sel = '#linearSections .section[data-name="' + q(p.name) + '"]';
  const box = await (await ctx.m.resolve(sel)).boundingBox();
  const dx = (to - s.start) * pps + (to > s.start ? -box.width / 2 + 10 : 10);
  await ctx.m.drag(sel, { desc: 'section body (move)', at: { x: box.width / 2, y: box.height / 2 }, dx });
  await waitUi(ctx);
};

// ── Spuren ──
D.createTrack = async (ctx, p, rec, op) => {
  if ((p.type || 'audio') === 'midi') {
    await ctx.m.click('#addMidiBtn', { desc: '+ MIDI', view: 'instrument popover', wait: 300 });
    await waitUi(ctx);
    for (let i = 0; i < 60 && await ev(ctx, () => { const b = document.querySelector('#swatchPop .instrlist button'); return !b || b.disabled; }); i++) await sleep(250);
    const want = p.instrument;
    if (want) {
      const fmt = ((op && op.result) || {}).format;
      const bh = () => ctx.page.evaluateHandle(([w, f]) => {
        const bs = [...document.querySelectorAll('#swatchPop .instrlist button')];
        return bs.find(b => b.textContent === w + (f && f !== 'builtin' ? ' (' + f + ')' : '')) || bs.find(b => b.textContent.startsWith(w)) || null;
      }, [want, fmt]).then(x => x.asElement());
      if (!await bh()) throw new NoUi('instrument not in the list: ' + want);
      await ctx.m.click(bh, { desc: 'instrument ' + want, wait: 300 });
    } else {
      await ctx.m.click(() => byText(ctx, '#swatchPop .instrlist button', 'No instrument'), { desc: 'no instrument' });
    }
  } else {
    await ctx.m.click('#addAudioBtn', { desc: '+ Audio', wait: 300 });
  }
  await waitUi(ctx);
  const r = sentResult(rec, 'createTrack');
  if (r && r.ok && p.name) await D.renameTrack(ctx, { track: r.index, name: p.name }, rec);
};

D.renameTrack = async (ctx, p) => {
  await ctx.m.click(rowSel(p.track) + ' .ltname', { desc: 'track name', dbl: true, wait: 150 });
  await needEdit(ctx, 'track name');
  await ctx.m.press('Control+a');
  await ctx.m.type(p.name);
  await ctx.m.press('Enter');
  await waitUi(ctx);
};

D.laneActive = async (ctx, p) => {
  await ctx.m.click(rowSel(p.track) + ' .ltlabel', { desc: 'lane header', at: { x: 30, y: 12 } });
  await waitUi(ctx);
};

D.trackMix = async (ctx, p, rec) => {
  const tr = await trackOf(ctx, p.track);
  if (!tr) throw new Error('track missing ' + p.track);
  if (tr.dest >= 0) throw new NoUi('lane: no S/M or fader on a take lane');
  for (const k of ['solo', 'mute']) if (p[k] !== undefined && p[k] !== tr[k]) {
    await ctx.m.click(rowSel(p.track) + ' .ltmixbtn.' + k, { desc: k + ' button' });
    await waitUi(ctx);
  }
  if (p.volumeDb !== undefined || p.pan !== undefined) {
    await ensureRouting(ctx, 'mixer');
    const strip = '#routing .mxstrip[data-mixtrack="' + p.track + '"]';
    if (p.pan !== undefined && Math.abs(p.pan - tr.pan) > 0.005) {
      await dragRange(ctx, strip + ' input.mxpan', p.pan, 'pan slider');
      await waitUi(ctx);
    }
    if (p.volumeDb !== undefined && Math.abs(p.volumeDb - tr.volumeDb) > 0.01) {
      await dragRange(ctx, strip + ' .mxfader input', Math.max(-60, p.volumeDb), 'fader');
      await waitUi(ctx);
    }
  }
};

// ── Clips ──
D.addClip = async (ctx, p, rec) => {
  if (!fs.existsSync(p.file)) throw new NoUi('file missing: ' + p.file);
  await ensureProject(ctx);
  const st = await stateOf(ctx);
  const sec = (st.sections || []).find(x => x.name === p.section) || (st.sections || [])[0];
  if (!sec) throw new NoUi('no section');
  const tr = (st.trackList || []).find(t => t.index === p.track);
  if (tr && tr.type === 'audio') await selectTrack(ctx, tr.dest >= 0 ? tr.dest : p.track);
  else if (p.track !== undefined && p.track >= 0) rec.partial.push('target track not selectable');
  if (await ev(ctx, () => selectedSection) && (await ev(ctx, () => selectedSection)) !== sec.name) await deselectSection(ctx);
  const start = sec.start + (p.start || 0);
  await setCursor(ctx, start);
  await ctx.m.fileDialog('#insertBtn', p.file, { desc: 'Insert file' });
  await waitUi(ctx, 120000);
};

D.deleteClip = async (ctx, p) => {
  await clickClipButton(ctx, p.track, p.name, 'Delete this clip', 'delete clip 🗑');
  await waitUi(ctx);
};

// Kopie an Position (seit 2026-10-30): Ziel auf dem Raster → Ctrl + Ziehen
// des Clips (rastet ein), sonst Feld „Copy at s“ im Clip-Popover; weitere
// Kopien (count) mit ⧉ direkt dahinter. startBeat zählt wie im Backend ab
// der Section (section, sonst die des Quell-Clips), nicht ab dem Songanfang.
D.duplicateClip = async (ctx, p, rec, op) => {
  const count = p.count || 1;
  let src = p.name;
  const cl = await clipOf(ctx, p.track, p.name);
  if (!cl) throw new Error('clip missing ' + p.name);
  let target = p.start;
  if (target === undefined && (p.startBeat !== undefined || p.section !== undefined))
    target = await ev(ctx, ([b, sn, cs]) => {
      const segs = tempoSegs();
      const sec = sn ? lastState.sections.find(x => x.name === sn) : sectionAt(cs + 0.001);
      return sec ? timeOfBeat(songBeat(sec.start, segs) + (b || 0), segs) : null;
    }, [p.startBeat, p.section || null, cl.start]);
  if (target === null) throw new NoUi('no section ' + p.section);
  for (let i = 0; i < count; i++) {
    const k = rec.sent.length;
    if (i === 0 && target !== undefined) {
      // an dieselbe Stelle lässt sich nicht ziehen (unter 3 px gilt nicht als Ziehen)
      if (Math.abs(target - cl.start) > 0.005 && await dragWorth(ctx, cl.start, target) && await uncoverIfHidden(ctx, p.track, src))
        await timeDrag(ctx, () => clipHandle(ctx, p.track, src), async () => (await clipOf(ctx, p.track, src)).start, target,
                       'clip body (Ctrl+drag: copy)', (pps) => Math.min(14, cl.length * pps / 2), { snap: true, modifiers: ['Control'], once: true, body: true });
      else await placeByFields(ctx, p.track, src, { 'Copy at s': target });
    } else {
      await clickClipButton(ctx, p.track, src, 'Duplicate right after', 'duplicate ⧉');
      await waitUi(ctx);
    }
    const s = rec.sent.slice(k).reverse().find(x => x.op === 'duplicateClip' && x.result);
    const r = s && s.result;
    if (!r || !r.ok) throw new Error('duplicate failed' + (r ? ': ' + r.error : ''));
    const nc = r.clips[0];
    src = nc.name;
    if (i === 0 && target !== undefined && Math.abs(nc.start - target) > 0.005) {
      rec.ui.notes.push('copy landed at ' + nc.start + ', moved to ' + target);
      await D.moveClip(ctx, { track: r.track ?? p.track, name: nc.name, start: target }, rec);
    }
  }
};

// Raster, auf das Clips einrasten (seit 2026-10-30 nach Zoom: Takt bis
// 1/16, clipSnap im UI): das gröbste, auf dem t liegt, und sein Schritt in
// Sekunden an der Stelle — null = t liegt neben jedem Raster. Ab
// CLIP_SNAP_PX / secs px/s rastet das UI auf dieses Raster oder feiner.
const gridOf = (ctx, t) => ev(ctx, (x) => {
  const segs = tempoSegs();
  for (const [g] of [...CLIP_GRIDS].reverse()) {
    if (Math.abs(clipSnap(x, false, segs, g).t - x) > 1e-3) continue;
    const beats = gridBeats(g, sectionAt(x));
    return { grid: g, secs: timeOfBeat(songBeat(x, segs) + beats, segs) - x, px: CLIP_SNAP_PX };
  }
  return null;
}, t);
const onGrid = async (ctx, t) => !!await gridOf(ctx, t);
// Ziehen lohnt (Ziel auf einem Raster und der Weg passt bei Einrast-Zoom in
// ein Bild)? Sonst nimmt eine Hand das Feld im Popover.
async function dragWorth (ctx, from, to) {
  const g = await gridOf(ctx, to);
  if (!g) return false;
  const v = await laneView(ctx);
  return Math.abs(to - from) * g.px / g.secs <= v.x1 - v.x0 - 40;
}

// Liegt ein anderer Clip über diesem (gleiche Lane, überlappend)? Dann erst
// greifbar machen (+N), sonst träfe das Ziehen den oberen Clip
async function uncoverIfHidden (ctx, track, name) {
  const over = await ev(ctx, ([i, n, f]) => {
    const t = (lastState.trackList || []).find(x => x.index === eval(f)(i, n));
    const cs = (t && t.clips) || [], k = cs.findIndex(c => c.name === n);
    if (k < 0 || (selectedClip && selectedClip.name === n)) return false;
    const c = cs[k];
    return cs.slice(k + 1).some(o => o.start < c.start + c.length - 0.001 && o.start + o.length > c.start + 0.001);
  }, [track, name, LANE_JS]);
  if (over) await uncover(ctx, track, name);
  return true;
}

// Lage genau über die Felder im Clip-Popover (Start s, Length s, Copy at s)
async function placeByFields (ctx, track, name, fields) {
  const cl = await clipOf(ctx, track, name);
  const midi = cl && cl.type === 'midi';
  await clickClipButton(ctx, track, name, midi ? 'Clip volume' : 'Clip gain', 'clip ± popover', { view: 'clip popover' });
  for (const [label, v] of Object.entries(fields)) await fillPop(ctx, label, Math.round(v * 1e6) / 1e6);
  await ctx.m.press('Enter');
  await waitUi(ctx);
}

// Ein Mensch zoomt heraus, bis Start und Ziel eines Ziehens zugleich im
// Bild sind, und scrollt die Timeline dorthin (gezählt)
async function laneView (ctx) {
  return ev(ctx, () => {
    const sc = document.querySelector('#linearScroll');
    const r = sc.getBoundingClientRect();
    const lab = document.querySelector('#linearRulerRow .ltlabel').getBoundingClientRect();
    const x0 = lab.right, x1 = Math.min(r.right, innerWidth);
    return { x0, x1, t0: (x0 - laneOffsetPx()) / pps, t1: (x1 - laneOffsetPx()) / pps, pps };
  });
}
// Weit entfernte Zeit: erst herauszoomen (wie eine Hand), statt lange zu scrollen
async function reachTime (ctx, t) {
  for (let i = 0; i < 8; i++) {
    const v = await laneView(ctx);
    const w = v.x1 - v.x0;
    const d = t < v.t0 ? (v.t0 - t) * v.pps : t > v.t1 ? (t - v.t1) * v.pps : 0;
    if (d < 1.5 * w || v.pps <= 20) return;
    await zoomStep(ctx, -1);
  }
}

async function zoomStep (ctx, dir) {
  // − ist der erste, + der zweite Knopf im Lineal-Kopf
  const idx = dir < 0 ? 0 : 1;
  await ctx.m.click(async () => (await ctx.page.$$('#linearRulerRow .ltlabel button'))[idx],
                    { desc: dir < 0 ? 'zoom out −' : 'zoom in +', wait: 150 });
}

// Ziehen in der Timeline, bis ein Merkmal (Clip-Start, -Ende …) auf der
// Zielzeit liegt — wie eine Hand: ganze Pixel, herauszoomen, bis der Weg ins
// Bild passt (kleinster Zoom 8 px/s, sonst Etappen mit Scrollen), zum
// genauen Ablegen hineinzoomen. Frei (Shift, Ziel neben dem Raster):
// ≥ 100 px/s, 1 px ≤ 0,01 s. Mit Einrasten (opts.snap, Ziel auf dem
// Raster): nur so weit, dass das UI auf das gröbste Raster des Ziels (oder
// feiner) rastet (gridOf). opts.modifiers: Tasten beim Ziehen (Shift = frei,
// Control = Kopie); opts.once: ein Ziehen.
const FINE_PPS = 100;
async function timeDrag (ctx, handleFn, featFn, target, desc, grabPx, opts = {}) {
  const g = opts.snap ? await gridOf(ctx, target) : null;
  const fine = g ? g.px / g.secs * 1.01 : FINE_PPS;
  for (let leg = 0; leg < 24; leg++) {
    const cur = await featFn();
    const left = target - cur;
    if (Math.abs(left) < 0.005) return;
    const v = await laneView(ctx);
    const visW = v.x1 - v.x0 - 40;
    if (Math.abs(left) * v.pps > visW && v.pps > 8.01) { await zoomStep(ctx, -1); continue; }
    if (v.pps < fine && Math.abs(left) * v.pps * 1.4 <= visW) { await zoomStep(ctx, +1); continue; }
    // Griff und Ziel ins Bild scrollen
    const off = await ev(ctx, () => laneOffsetPx());
    const sc = await ev(ctx, () => document.querySelector('#linearScroll').scrollLeft);
    let gx = off + cur * v.pps + grabPx(v.pps), tx = gx + left * v.pps;
    // ganz links (scrollLeft 0) liegt eine Kante bei 0 s direkt an der
    // Spurkopf-Spalte — dort reicht 1 px Abstand, weiter scrollen geht nicht
    const lo = v.x0 + (sc <= 0 ? 1 : 10), hi = v.x1 - 10;
    if (Math.min(gx, tx) < lo || Math.max(gx, tx) > hi) {
      let shift;
      if (Math.abs(tx - gx) <= hi - lo) shift = (gx + tx) / 2 - (lo + hi) / 2;
      else shift = left < 0 ? gx - (hi - 10) : gx - (lo + 10);
      await ctx.m.scrollTo('#linearScroll', Math.max(0, sc + shift));
      continue;
    }
    gx = Math.round(gx); tx = Math.round(Math.max(lo, Math.min(hi, tx)));
    if (process.env.DEBUG_DRAG) ctx.m.rec.ui.notes.push('leg cur=' + cur + ' target=' + target + ' pps=' + v.pps.toFixed(1) + ' gx=' + gx + ' tx=' + tx);
    // Das UI wertet erst mehr als 2 px als Ziehen: kleine Korrekturen brauchen mehr Zoom
    if (Math.abs(tx - gx) <= 2) {
      if (v.pps < 400) { await zoomStep(ctx, +1); continue; }
      return;
    }
    const h = await handleFn();
    if (!h) throw new Error('drag handle gone: ' + desc);
    const box = await h.boundingBox();
    // Körper in der Mitte greifen (oben in der Namenszeile sitzt die Marke
    // „+N“, oben rechts die Knopfleiste), Kanten oben
    let y = opts.body ? box.height / 2 : Math.min(8, box.height / 3);
    // Seit 2026-10-30 klebt die Knopfleiste am sichtbaren Teil: liegt sie
    // (beim Anfahren eingeblendet) über dem Griff, greift die Hand darunter
    if (opts.body) y = await h.evaluate((el, [px, y0]) => {
      const own = el.querySelector(':scope > .clbtns');
      if (!own) return y0;
      const was = own.style.display;
      own.style.display = 'flex';
      const r = el.getBoundingClientRect();
      const hit = (yy) => { const t = document.elementFromPoint(px, r.top + yy); return !!t && !!t.closest('.clbtns'); };
      const yy = hit(y0) && !hit(r.height - 5) ? r.height - 5 : y0;
      own.style.display = was;
      return yy;
    }, [gx, y]);
    await ctx.m.drag(handleFn, { desc, at: { x: gx - box.x, y }, dx: tx - gx, noSize: true, modifiers: opts.modifiers });
    await waitUi(ctx);
    if (opts.once) return;
  }
  ctx.m.rec.ui.notes.push('drag did not reach the target: ' + desc);
}

// Seit 2026-10-30: Ziel auf einem Raster und Weg in einem Bild → Ziehen mit
// Einrasten, sonst das Feld „Start s“ im Clip-Popover
D.moveClip = async (ctx, p) => {
  const cl0 = await clipOf(ctx, p.track, p.name);
  if (!cl0) throw new Error('clip missing ' + p.name);
  if (Math.abs(cl0.start - p.start) < 0.005) return;
  if (!await dragWorth(ctx, cl0.start, p.start)) return placeByFields(ctx, p.track, p.name, { 'Start s': p.start });
  await uncoverIfHidden(ctx, p.track, p.name);
  // Körper greifen: 14 px hinter dem Clip-Anfang (Kanten 8 px), in der Mitte (Marke +N und Knöpfe oben)
  await timeDrag(ctx, () => clipHandle(ctx, p.track, p.name), async () => (await clipOf(ctx, p.track, p.name)).start, p.start,
                 'clip body (move)', (pps) => Math.min(14, cl0.length * pps / 2), { snap: true, body: true });
};

D.trimClip = async (ctx, p) => {
  const cl = await clipOf(ctx, p.track, p.name);
  if (!cl) throw new Error('clip missing ' + p.name);
  const start = p.start !== undefined ? p.start : cl.start;
  const end = p.length !== undefined ? start + p.length : cl.start + cl.length;
  await uncoverIfHidden(ctx, p.track, p.name);
  // Kanten rasten ein (Ziel auf dem Raster); linke Kante daneben: Shift +
  // Ziehen (kein Feld), rechte Kante daneben: Feld „Length s“
  if (Math.abs(start - cl.start) > 0.005) {
    const snap = await onGrid(ctx, start);
    await timeDrag(ctx, () => clipHandle(ctx, p.track, p.name), async () => (await clipOf(ctx, p.track, p.name)).start, start,
                   'clip left edge (trim)', () => 2, snap ? { snap } : { modifiers: ['Shift'] });
  }
  const fe = async () => { const c = await clipOf(ctx, p.track, p.name); return c.start + c.length; };
  if (Math.abs(end - await fe()) > 0.005) {
    if (await dragWorth(ctx, await fe(), end))
      await timeDrag(ctx, () => clipHandle(ctx, p.track, p.name), fe, end, 'clip right edge (trim)', () => -2, { snap: true });
    else await placeByFields(ctx, p.track, p.name, { 'Length s': end - (await clipOf(ctx, p.track, p.name)).start });
  }
};

D.favoriteClip = async (ctx, p) => {
  const cl = await clipOf(ctx, p.track, p.name);
  if (!cl) throw new Error('clip missing ' + p.name);
  if (p.on !== undefined && !!cl.favorite === !!p.on) return;
  await clickClipButton(ctx, p.track, p.name, cl.type === 'midi' ? 'Clip volume' : 'Clip gain', 'clip ± popover', { view: 'clip popover' });
  await ctx.m.click('#swatchPop .cpfav', { desc: 'clip favorite ★ (popover)' });
  await waitUi(ctx);
};

D.setClip = async (ctx, p, rec) => {
  const cl = await clipOf(ctx, p.track, p.name);
  if (!cl) throw new Error('clip missing ' + p.name);
  const midi = cl.type === 'midi';
  await clickClipButton(ctx, p.track, p.name, midi ? 'Clip volume' : 'Clip gain', 'clip ± popover', { view: 'clip popover' });
  const map = { gainDb: midi ? 'Volume dB' : 'Gain dB', transpose: 'Transpose', fadeIn: 'Fade in s', fadeOut: 'Fade out s',
                reverse: 'Reverse', crossfade: 'Crossfade', fadeCurve: 'Curve', speed: 'Speed' };
  for (const [k, v] of Object.entries(p)) {
    if (['track', 'name'].includes(k)) continue;
    if (!map[k]) { rec.partial.push(k + ' not in the clip popover'); continue; }
    await fillPop(ctx, map[k], v);
  }
  await ctx.m.press('Enter');
  await waitUi(ctx, 120000);
};

D.warpClip = async (ctx, p, rec) => {
  await clickClipButton(ctx, p.track, p.name, 'Clip gain', 'clip ± popover', { view: 'clip popover' });
  if (p.clear) await fillPop(ctx, 'Warp', 'clear');
  else {
    await fillPop(ctx, 'Warp', 'detect');
    if (p.markers) rec.partial.push('marker list (' + p.markers.length + ' source→beat) only by double-click + drag per marker; popover offers "detect"');
    if (p.mode) rec.partial.push('mode ' + p.mode + ' not in UI');
  }
  await ctx.m.press('Enter');
  await waitUi(ctx, 180000);
};

D.splitClip = async (ctx, p) => {
  await setCursor(ctx, p.at);
  await clickClipButton(ctx, p.track, p.name, 'Split at the playhead', 'split ✂');
  await waitUi(ctx);
};

// Piano-Roll: Noten zeichnen (Raster 1/16, Tonhöhen 0–127, so lang wie
// Section/Clip plus ein Takt hinter der letzten Note; seit 2026-10-30).
// Velocity ≠ Feldwert: erst das Feld „Vel“ setzen — neue Noten nehmen es.
const PR_HI = 127, PR_ROW = 12, PR_COL = 12;
async function setVelField (ctx, v) {
  if (+(await ev(ctx, () => document.querySelector('#prVel').value)) === v) return;
  await ctx.m.fill('#prVel', String(v), { desc: 'velocity field', noSize: true });
  await ctx.m.press('Enter');
}

async function drawNotes (ctx, notes, rec, bpm) {
  let drawn = 0, outside = 0, offgrid = 0;
  const conv = notes.map(n => ({ n, beat: n.beat !== undefined ? n.beat : n.start * bpm / 60,
                                 beats: n.beats !== undefined ? n.beats : n.length * bpm / 60 }))
    .sort((a, b) => a.beat - b.beat);
  for (const { n, beat, beats } of conv) {
    if (Math.abs(beat * 4 - Math.round(beat * 4)) > 0.01 || Math.abs(beats * 4 - Math.round(beats * 4)) > 0.01) offgrid++;
    const col = Math.round(beat * 4), cols = Math.max(1, Math.round(beats * 4));
    // Die Rolle reicht einen Takt hinter die letzte Note — weiter weg geht nicht
    const width = await ev(ctx, () => parseFloat(document.querySelector('#prGrid').style.width) || 0);
    if ((col + 1) * PR_COL > width) { outside++; continue; }
    await setVelField(ctx, n.velocity || 100);
    const at = { x: col * PR_COL + 4, y: (PR_HI - n.pitch) * PR_ROW + 6 };
    await ctx.m.drag('#prGrid', { desc: 'piano roll note', at, dx: (cols - 1) * PR_COL + 1, noSize: true, wait: 10 });
    drawn++;
  }
  if (outside) rec.partial.push(outside + ' of ' + notes.length + ' notes past the end of the piano roll');
  if (offgrid) rec.partial.push(offgrid + ' notes off the 1/16 grid (quantized)');
  return { drawn, outside };
}

D.addMidiClip = async (ctx, p, rec) => {
  const notes = p.notes || [];
  await ensureProject(ctx);
  await ensureTab(ctx, 'notes');
  const st = await stateOf(ctx);
  const sec = (st.sections || []).find(x => x.name === p.section) || st.sections[0];
  await selectTrack(ctx, p.track);
  const sel = await ev(ctx, () => selectedSection);
  if (sel && sel !== sec.name) await deselectSection(ctx);
  let start = 0;
  if (p.startBeat !== undefined) start = (await timeOfSongBeat(ctx, (sec.start * sec.bpm / 60) + p.startBeat)) - sec.start;
  else if (p.start !== undefined) start = p.start;
  if (start > 0.001) {
    await setCursor(ctx, sec.start + start);
    if (Math.abs(Math.round((sec.start + start) * 20) / 20 - sec.start - start) > 0.001) rec.partial.push('start rounded to 1/20 s by the ruler cursor');
  } else {
    // Anfang der Section: sie anklicken (löscht den Cursor) — ohne Auswahl
    // nähme das UI die Section an der Abspielposition (stil-1 #3818), der
    // Cursor rastet auf 1/20 s neben den Anfang
    if (sel === sec.name && (await ev(ctx, () => cursorPos)) !== null) await deselectSection(ctx);
    await selectSection(ctx, sec.name);
  }
  if (!await ev(ctx, () => prOpen)) await ctx.m.click('#prToggle', { desc: 'Draw a MIDI clip', view: 'piano roll' });
  if (await ev(ctx, () => prNotes.length)) await ctx.m.click(() => byText(ctx, '#midiNewNotes .prhead button', 'Clear'), { desc: 'Clear' });
  await drawNotes(ctx, notes, rec, sec.bpm || 120);
  if (p.name) await ctx.m.fill('#prName', p.name, { desc: 'clip name', free: true });
  // Länge über das Feld „Length“ (Sekunden; Beats im Tempo der Section)
  if (p.lengthBeats !== undefined || p.length !== undefined) {
    const len = p.length !== undefined ? p.length : p.lengthBeats * 60 / (sec.bpm || 120);
    await ctx.m.fill('#prLen', String(Math.round(len * 1e6) / 1e6), { desc: 'clip length', noSize: true });
    await ctx.m.press('Enter');
  }
  await ctx.m.click('#prAddBtn', { desc: 'Add notes as clip' });
  await waitUi(ctx);
};

// Bearbeiten: Doppelklick auf den Clip öffnet ihn in der Rolle — an einer
// freien Stelle, nicht auf der Knopfleiste des gewählten Clips (stil-1
// #4062 traf Q, #4155 ⧉; die Rolle ging nie auf)
async function openClipInRoll (ctx, track, name, desc = 'MIDI clip (edit)') {
  const at = await uncover(ctx, track, name);
  await ctx.m.click(() => clipHandle(ctx, track, name), { desc, dbl: true, at, view: 'piano roll edit', wait: 300 });
  await waitUi(ctx);
}

// Note in der Rolle (Index in prNotes) nach Tonhöhe und Beat ab Clip-Anfang
const noteIndex = (ctx, pitch, beat, spb) => ev(ctx, ([pitch, beat, spb]) =>
  prNotes.findIndex(n => n.pitch === pitch && Math.abs(n.start / spb - beat) <= 1 / 32 + 1e-6), [pitch, beat, spb]);
const noteSel = (i) => '#prGrid .prnote[data-i="' + i + '"]';

D.setMidiNotes = async (ctx, p, rec) => {
  const cl = await clipOf(ctx, p.track, p.name);
  if (!cl) throw new Error('clip missing');
  const pk = await ctx.api('clipPeaks', { track: p.track, name: p.name });
  const bpm = ((await stateOf(ctx)).sections.find(s => cl.start >= s.start && cl.start < s.end) || {}).bpm || 120;
  const inRange = (pk.notes || []).filter(n => {
    const b = n.start * bpm / 60;
    return (p.fromBeat === undefined || b >= p.fromBeat - 1e-6) && (p.toBeat === undefined || b < p.toBeat - 1e-6);
  });
  if (p.copies) rec.partial.push('copies: only this clip');
  await openClipInRoll(ctx, p.track, p.name);
  // Bestehende Noten im Bereich: je ein Klick entfernt sie
  for (const n of inRange.sort((a, b) => b.start - a.start)) {
    const i = await noteIndex(ctx, n.pitch, n.start * bpm / 60, 60 / bpm);
    if (i < 0) continue;
    await ctx.m.click(noteSel(i), { desc: 'piano roll note (remove)', at: { x: 2, y: 5 }, noSize: true, wait: 30 });
  }
  if ((p.notes || []).length) await drawNotes(ctx, p.notes.map(n => ({ ...n, beat: (n.beat !== undefined ? n.beat : n.start * bpm / 60) + (p.fromBeat || 0) })), rec, bpm);
  await sleep(500);
  await waitUi(ctx);
  await ctx.m.click('#prDoneBtn', { desc: 'Done' });
  await waitUi(ctx);
};

// editNotes: dieselben Handgriffe in der Rolle — löschen (Klick), Tonhöhe/
// Lage (Note ziehen), Velocity (Spur „Vel“ ziehen, dann Feld + Enter genau),
// add (zeichnen); die Länge einer vorhandenen Note kennt die Rolle nicht
D.editNotes = async (ctx, p, rec) => {
  const cl = await clipOf(ctx, p.track, p.name);
  if (!cl) throw new Error('clip missing');
  const bpm = ((await stateOf(ctx)).sections.find(s => cl.start >= s.start && cl.start < s.end) || {}).bpm || 120;
  const spb = 60 / bpm;
  await openClipInRoll(ctx, p.track, p.name);
  let shared = 0, lengths = 0, offgrid = 0;
  for (const e of p.edits || []) {
    // Treffer wie im Backend: Tonhöhe, Beat ±1/32 bzw. Bereich beat..toBeat
    const b1 = e.toBeat !== undefined ? e.toBeat : e.beat;
    const hits = await ev(ctx, ([pitch, b0, b1, spb]) => prNotes.filter(n => n.pitch === pitch && n.start / spb >= b0 - 1 / 32 && n.start / spb <= b1 + 1 / 32)
      .map(n => n.start / spb), [e.pitch, e.beat, b1, spb]);
    if (!hits.length) throw new Error('note ' + e.pitch + ' at beat ' + e.beat + ' not in the roll');
    for (const beat of hits) {
      let i = await noteIndex(ctx, e.pitch, beat, spb);
      if (e.delete) { await ctx.m.click(noteSel(i), { desc: 'piano roll note (remove)', at: { x: 2, y: 5 }, noSize: true, wait: 30 }); continue; }
      if (e.velocity !== undefined) {
        const col = Math.floor(beat * 4 + 1e-6);
        if (await ev(ctx, (col) => prNotes.filter(n => Math.floor(n.start / prStep + 1e-6) === col).length, col) > 1) shared++;
        const y = Math.max(1, Math.min(39, Math.round((1 - e.velocity / 127) * 40)));
        await ctx.m.drag('#velLane', { desc: 'velocity lane', at: { x: col * PR_COL + 6, y: 1 }, dy: y - 1, noSize: true, wait: 30 });
        await ctx.m.fill('#prVel', String(e.velocity), { desc: 'velocity field', noSize: true });
        await ctx.m.press('Enter');
      }
      if (e.beats !== undefined) lengths++;
      if (e.toPitch !== undefined || e.moveTo !== undefined) {
        i = await noteIndex(ctx, e.pitch, beat, spb);
        const dc = e.moveTo !== undefined ? (e.moveTo - beat) * 4 : 0;
        if (Math.abs(dc - Math.round(dc)) > 0.01) offgrid++;
        const dp = e.toPitch !== undefined ? e.toPitch - e.pitch : 0;
        await ctx.m.drag(noteSel(i), { desc: 'piano roll note (move)', at: { x: 2, y: 5 }, dx: Math.round(dc) * PR_COL, dy: -dp * PR_ROW, noSize: true, wait: 30 });
      }
    }
  }
  if (Array.isArray(p.add) && p.add.length) {
    // Noten der letzten Velocity-Änderung sind fürs Feld markiert — Klick
    // neben die Noten in der Spur hebt das auf, sonst träfe das Feld sie
    const free = await ev(ctx, () => { if (!velTouched.size) return -1;
      const used = new Set(prNotes.map(n => Math.floor(n.start / prStep + 1e-6)));
      let c = 0; while (used.has(c)) c++; return c; });
    if (free >= 0) await ctx.m.click('#velLane', { desc: 'velocity lane (unmark)', at: { x: free * PR_COL + 6, y: 20 }, noSize: true });
    await drawNotes(ctx, p.add, rec, bpm);
  }
  if (shared) rec.partial.push(shared + ' velocity edits: other notes in the same column got the value too');
  if (lengths) rec.partial.push(lengths + ' note lengths not editable in the roll');
  if (offgrid) rec.partial.push(offgrid + ' moves off the 1/16 grid (snapped)');
  await sleep(500);
  await waitUi(ctx);
  await ctx.m.click('#prDoneBtn', { desc: 'Done' });
  await waitUi(ctx);
};

D.clipNotes = async (ctx, p) => {
  if (await ev(ctx, ([t, n]) => prEdit && prEdit.track === t && prEdit.name === n, [p.track, p.name])) return;
  await openClipInRoll(ctx, p.track, p.name, 'MIDI clip (show notes)');
  await ctx.m.click('#prDoneBtn', { desc: 'Done' });
};

// ── Plugins und Routing ──
async function pickPlugin (ctx, selTarget, name, uid, desc) {
  // Erster Fokus lädt den Katalog (listPlugins) und zeichnet neu
  await ctx.m.click(selTarget, { desc, wait: 200 });
  await waitUi(ctx, 60000);
  const h = await ctx.m.resolve(selTarget);
  const value = await h.evaluate((s, [n, u]) => {
    const o = [...s.options].find(o => { try { const v = JSON.parse(o.value); return v && (v.name === n || (u && v.uid === u)); } catch (e) { return false; } });
    return o ? o.value : null;
  }, [name, uid]);
  if (!value) throw new NoUi('plugin not in the picker: ' + name);
  const n = await h.evaluate(s => s.options.length);
  await h.selectOption(value);
  ctx.m.rec.ui.clicks++;
  if (n > 25) ctx.m.rec.ui.notes.push('long list (' + n + ' entries): ' + desc);
  await waitUi(ctx);
}

D.addPlugin = async (ctx, p, rec, op) => {
  await ensureRouting(ctx, 'simple');
  const uid = (op.result || {}).uid;
  if (p.at === 'master') {
    if (p.before) await pickPlugin(ctx, '#routing .chain[data-master="' + p.before + '"] select.inputsel', p.name, uid, '+ before (master)');
    else await pickPlugin(ctx, '#routing .rtop > select.inputsel:nth-of-type(2)', p.name, uid, '+ Before Main');
  } else if (p.track !== undefined) {
    const chain = '#routing .chain[data-chain="t' + p.track + '"]';
    if (!await ctx.m.resolve(chain)) throw new NoUi('track ' + p.track + ' has no chain row');
    await pickPlugin(ctx, chain + ' > select.inputsel', p.name, uid, '+ (chain end)');
    if (p.at === 'start') {
      const r = sentResult(rec, 'addPlugin');
      const first = await ev(ctx, (c) => { const s = document.querySelector(c + ' .slot'); return s && s.dataset.plugin; }, chain);
      if (r && first && first !== r.id) {
        await ctx.m.page.locator(chain + ' .slot[data-plugin="' + r.id + '"]').dragTo(ctx.page.locator(chain + ' .slot[data-plugin="' + first + '"]'));
        ctx.m.rec.ui.drags++;
        await waitUi(ctx);
      }
    }
  } else {
    await pickPlugin(ctx, '#routing .rtop > select.inputsel:nth-of-type(1)', p.name, uid, '+ Add plugin');
  }
  const r = sentResult(rec, 'addPlugin');
  if (r && r.ok && p.label) await D.renamePlugin(ctx, { plugin: r.id, name: p.label }, rec);
};

D.removePlugin = async (ctx, p) => {
  await ensureRouting(ctx, 'simple');
  const sel = '#routing .slot[data-plugin="' + p.plugin + '"] .rdel';
  const bus = '#routing .chain[data-bus="' + p.plugin + '"] .rdel';
  await ctx.m.click(await ctx.m.resolve(sel) ? sel : bus, { desc: 'remove plugin ×' });
  await waitUi(ctx);
};

D.renamePlugin = async (ctx, p) => {
  await ensureRouting(ctx, 'simple');
  if (!await moduleNameHandle(ctx, p.plugin)) throw new Error('module not visible ' + p.plugin);
  await ctx.m.click(() => moduleNameHandle(ctx, p.plugin), { desc: 'plugin name', dbl: true, wait: 150 });
  await needEdit(ctx, 'plugin name');
  await ctx.m.press('Control+a');
  await ctx.m.type(p.name);
  await ctx.m.press('Enter');
  await waitUi(ctx);
};

D.setParameter = async (ctx, p, rec, op) => {
  await openCard(ctx, p.plugin);
  const targets = [];   // [name, normalized]
  const setOf = ((op.result || {}).set || []);
  const norm = (name, fallback) => { const s = setOf.find(x => x.name === name); return s && typeof s.value === 'number' ? s.value : fallback; };
  if (p.name !== undefined) targets.push([p.name, p.value !== undefined ? p.value : norm(p.name)]);
  for (const [k, v] of Object.entries(p.values || {})) targets.push([k, v]);
  for (const k of Object.keys(p.reals || {})) targets.push([k, norm(k)]);
  for (const [k, v] of Object.entries(p.settings || {})) {
    const row = await paramRow(ctx, p.plugin, k);
    if (!row) throw new NoUi('setting not shown: ' + k);   // Einstellungen stehen immer in der Karte
    const inp = async () => (await paramRow(ctx, p.plugin, k)).$('select, input');
    const kind = await (await inp()).evaluate(e => e.tagName === 'SELECT' ? 'select' : e.type);
    if (kind === 'select') await ctx.m.select(inp, String(v), { desc: 'setting ' + k });
    else if (kind === 'checkbox') { if (await (await inp()).evaluate(e => e.checked) !== !!v) await ctx.m.click(inp, { desc: 'setting ' + k }); }
    else { await ctx.m.fill(inp, String(v), { desc: 'setting ' + k }); await ctx.m.press('Enter'); }
    await waitUi(ctx);
  }
  // Zahlenfeld je Parameter (Variante B): der Wert in der angezeigten Einheit
  // (real), Wörter als Text, ohne Einheit 0..1
  const want = (name, v) => {
    const s = setOf.find(x => x.name === name);
    if (s && s.real !== undefined && s.real !== null) return String(s.real);
    if (s && s.text && !/^[-+]?[0-9.]+/.test(s.text)) return s.text;
    return v === undefined ? undefined : String(Math.round(v * 10000) / 10000);
  };
  for (const [k, v] of Object.entries(p.texts || {})) targets.push([k, undefined, String(v)]);
  for (const [k, v, t] of targets) {
    const text = t !== undefined ? t : want(k, v);
    if (text === undefined) { rec.partial.push('no target value for ' + k); continue; }
    if (!await findParamRow(ctx, p.plugin, k)) throw new NoUi('parameter not found in the card: ' + k);
    await ctx.m.fill(async () => (await paramRow(ctx, p.plugin, k)).$('input.pval'), text, { desc: 'parameter value ' + k });
    await ctx.m.press('Enter');
    await sleep(150);
    await waitUi(ctx);
  }
};

D.setWet = async (ctx, p) => {
  await ensureRouting(ctx, 'simple');
  let sel = '#routing .slot[data-plugin="' + p.plugin + '"] input.rwet';
  if (!await ctx.m.resolve(sel)) sel = '#routing .chain[data-bus="' + p.plugin + '"] > input.rwet';
  if (!await ctx.m.resolve(sel)) throw new NoUi('no wet slider for ' + p.plugin);
  await dragRange(ctx, sel, p.wet, 'wet slider');
  await waitUi(ctx);
};

D.setChain = async (ctx, p, rec) => {
  await ensureRouting(ctx, 'simple');
  const chain = '#routing .chain[data-chain="t' + p.track + '"]';
  const cur = await ev(ctx, (c) => [...document.querySelectorAll(c + ' .slot')].map(s => s.dataset.plugin), chain);
  const want = p.plugins || [];
  if (want.some(id => !cur.includes(id)) || cur.some(id => !want.includes(id))) {
    // Module in die Kette holen: nur über die Matrix (Complex) — Kante für Kante
    rec.partial.push('chain with other modules: Complex matrix, one cell per edge');
    await ensureRouting(ctx, 'complex');
    const src = 't' + p.track;
    const nodes = [src, ...want, 'main'];
    const wantEdges = new Set(nodes.slice(0, -1).map((a, i) => a + '>' + nodes[i + 1]));
    const st = await stateOf(ctx);
    const edges = ((st.routing || {}).edges || []).filter(e => !e.send && !e.feedback && !e.sidechain);
    const chainSet = new Set([src, ...want, ...cur]);
    const drop = edges.filter(e => chainSet.has(e.from) && !wantEdges.has(e.from + '>' + e.to) && (e.from === src || want.includes(e.from) || cur.includes(e.from)));
    const have = new Set(edges.map(e => e.from + '>' + e.to));
    const cell = (f, t) => '#routing .rcell[data-from="' + f + '"][data-to="' + t + '"]';
    for (const e of drop) { await ctx.m.click(cell(e.from, e.to), { desc: 'matrix cell (disconnect)' }); await waitUi(ctx); }
    for (const k of wantEdges) if (!have.has(k)) {
      const [f, t] = k.split('>');
      if (!await ctx.m.resolve(cell(f, t))) throw new NoUi('matrix cell missing ' + k);
      await ctx.m.click(cell(f, t), { desc: 'matrix cell (connect)' });
      await waitUi(ctx);
    }
    return;
  }
  // Einsortieren: je Position das gewünschte Modul vor das aktuelle ziehen
  for (let i = 0; i < want.length; i++) {
    const now = await ev(ctx, (c) => [...document.querySelectorAll(c + ' .slot')].map(s => s.dataset.plugin), chain);
    if (now[i] === want[i]) continue;
    await ctx.page.locator(chain + ' .slot[data-plugin="' + want[i] + '"]').dragTo(ctx.page.locator(chain + ' .slot[data-plugin="' + now[i] + '"]'));
    ctx.m.rec.ui.drags++;
    await waitUi(ctx);
  }
};

async function presetFind (ctx, pid, query) {
  await openCard(ctx, pid);
  const inp = async () => (await cardHandle(ctx, pid)).$('.presetbar input[type=search]');
  await ctx.m.fill(inp, query, { desc: 'preset search' });
  await sleep(450);
  await waitUi(ctx);
}

async function moduleId (ctx, ref) {
  if (!ref) return null;
  if (/^p\d+$/.test(ref)) return ref;
  return ev(ctx, (r) => { const ps = ((lastState.routing || {}).plugins || []); const m = ps.find(x => x.label === r) || ps.find(x => x.name === r); return m ? m.id : null; }, ref);
}

D.findPresets = async (ctx, p, rec) => {
  const id = await moduleId(ctx, p.plugin);
  if (!id) throw new NoUi('preset search across plugins: only inside one module card');
  p = { ...p, plugin: id };
  await presetFind(ctx, p.plugin, p.query || p.class || p.text || '');
  if (p.class) rec.partial.push('class filter as search word');
};

D.loadPreset = async (ctx, p, rec, op) => {
  const pr = (op.result || {}).preset || {};
  const name = pr.name || p.name || (p.path ? path.basename(p.path).replace(/\.[^.]+$/, '') : '');
  if (!name) throw new NoUi('preset by number/sound only');
  if (p.sound) rec.partial.push('sound ' + p.sound + ' found by name search');
  await presetFind(ctx, p.plugin, name);
  const sel = async () => (await cardHandle(ctx, p.plugin)).$('.presetbar select');
  const h = await sel();
  const value = await h.evaluate((s, [n, pth, snd]) => {
    const o = [...s.options].find(o => { try { const v = JSON.parse(o.value); return (pth && v.path === pth) || (snd && v.id === snd) || v.name === n; } catch (e) { return false; } });
    return o ? o.value : null;
  }, [name, pr.path || p.path, p.sound]);
  if (!value) throw new NoUi('preset not found by search: ' + name);
  await ctx.m.select(sel, value, { desc: 'preset list' });
  await ctx.m.click(async () => (await (await cardHandle(ctx, p.plugin)).evaluateHandle(c => [...c.querySelectorAll('.presetbar button')].find(b => b.textContent === 'Load'))).asElement(), { desc: 'Load preset' });
  await waitUi(ctx, 60000);
};

async function samplerSearch (ctx, pid, query, note) {
  const card = () => cardHandle(ctx, pid);
  if (note !== undefined) {
    await ctx.m.fill(async () => (await card()).$('.samplesearch input[type=number]'), String(note), { desc: 'sample key' });
  }
  await ctx.m.fill(async () => (await card()).$('.samplesearch input:not([type])'), query, { desc: 'sample search' });
  await ctx.m.press('Enter');
  await sleep(300);
  await waitUi(ctx, 60000);
}

D.findSamples = async (ctx, p, rec) => {
  const st = await stateOf(ctx);
  const sampler = ((st.routing || {}).plugins || []).find(x => x.name === 'Sampler' || x.uid === 'sampler');
  if (!sampler) throw new NoUi('sample search lives in a sampler card — no sampler in the project yet');
  await openCard(ctx, sampler.id);
  await samplerSearch(ctx, sampler.id, p.query || p.text || '');
  if (p.oneShot !== undefined) rec.partial.push('oneShot filter not in UI');
};

D.setSamples = async (ctx, p, rec) => {
  await openCard(ctx, p.plugin);
  if (p.update) {
    for (const u of p.update) {
      const other = Object.keys(u).filter(k => !['name', 'index', 'choke', 'attack', 'release', 'velLow', 'velHigh', 'roundRobin'].includes(k));
      if (other.length) throw new NoUi('sample ' + other.join('/') + ' not in UI');
    }
    throw new NoUi('update by name: only choke/velocity/attack/release fields (⚙) — not driven here');
  }
  for (const s of p.samples || []) {
    const base = path.basename(s.file).replace(/\.[^.]+$/, '');
    await samplerSearch(ctx, p.plugin, base, s.note);
    const plus = async () => (await (await cardHandle(ctx, p.plugin)).evaluateHandle((c, f) => {
      const rows = [...c.querySelectorAll('.samplelist .samplerow')];
      const row = rows.find(r => { const n = r.querySelector('.sname'); return n && n.title.endsWith('\n' + f); });
      return row ? [...row.querySelectorAll('button')].find(b => b.textContent === '+') : null;
    }, s.file)).asElement();
    if (!await plus()) throw new NoUi('sample not found by name search: ' + base);
    await ctx.m.click(plus, { desc: 'sample + (to key)' });
    await waitUi(ctx);
    const extra = Object.keys(s).filter(k => !['file', 'note', 'name', 'oneShot'].includes(k));
    if (extra.length) rec.partial.push('sample ' + extra.join('/') + ' via ⚙ or not in UI');
    if (s.name) rec.partial.push('sample name not settable');
  }
};

// Automation-Lane einer Spur öffnen, Ziel wählen
async function openLane (ctx, param) {
  await ensureProject(ctx);
  const st = await stateOf(ctx);
  let track = null;
  const m = /^t(\d+)\./.exec(param);
  if (m) track = +m[1];
  else {
    const pm = /^(p\d+)\./.exec(param);
    const owner = pm && (st.trackList || []).find(t => (t.routes || []).some(r => r.id === pm[1]));
    track = owner ? owner.index : ((st.trackList || []).find(t => t.dest < 0) || {}).index;
  }
  const tr = (st.trackList || []).find(t => t.index === track);
  if (!tr) throw new NoUi('no track for ' + param);
  const open = await ev(ctx, (n) => autoOpen.has(n), tr.name);
  if (!open) {
    await ctx.m.click(rowSel(track) + ' .autobtn', { desc: 'A (automation lane)', view: 'automation lane' });
    await waitUi(ctx);
  }
  const lane = async () => ctx.page.evaluateHandle(([n, prm]) => {
    const rows = [...document.querySelectorAll('#linearTracks .autorow')];
    return rows.find(r => r.dataset.auto === prm) || null;
  }, [tr.name, param]).then(x => x.asElement());
  if (!await lane()) {
    // Ziel in der Auswahl der Lane dieser Spur
    const selH = async () => ctx.page.evaluateHandle((idx) => {
      const row = document.querySelector('#linearTracks .ltrow[data-track="' + idx + '"]');
      let r = row && row.nextElementSibling;
      while (r && !r.classList.contains('autorow')) r = r.nextElementSibling;
      return r ? r.querySelector('select.autosel') : null;
    }, track).then(x => x.asElement());
    await sleep(300);
    if (!await selH()) throw new NoUi('automation lane select missing');
    const has = await (await selH()).evaluate((s, prm) => [...s.options].some(o => o.value === prm), param);
    if (!has) throw new NoUi('parameter not automatable in the lane: ' + param);
    await ctx.m.select(selH, param, { desc: 'automation target' });
    await waitUi(ctx);
  }
  return lane;
}

D.setModifier = async (ctx, p, rec) => {
  const lane = await openLane(ctx, p.param);
  const lab = async () => (await lane()).$('.ltlabel');
  const has = await ev(ctx, (prm) => !!modifierOf(prm), p.param);
  if (!has) {
    await ctx.m.click(async () => (await lab()).$('.lfobtn'), { desc: '∿ LFO on' });
    await waitUi(ctx);
  }
  const sels = async (i) => (await (await lab()).$$('select.lfosel'))[i];
  if (p.type === 'step') {
    const cur = await ev(ctx, (prm) => modifierOf(prm).type, p.param);
    if (cur !== 'step') { await ctx.m.select(() => sels(0), JSON.stringify('step'), { desc: 'modifier type' }); await waitUi(ctx); }
    if (p.steps) { await ctx.m.fill(async () => (await lab()).$('input.lfosteps'), p.steps.join(' '), { desc: 'steps' }); await ctx.m.press('Enter'); await waitUi(ctx); }
  } else {
    const cur = await ev(ctx, (prm) => modifierOf(prm).type, p.param);
    if (cur === 'step') { await ctx.m.select(() => sels(0), JSON.stringify('lfo'), { desc: 'modifier type' }); await waitUi(ctx); }
    if (p.wave) { await ctx.m.select(() => sels(1), JSON.stringify(p.wave), { desc: 'LFO wave' }); await waitUi(ctx); }
  }
  if (p.rate !== undefined) {
    const idx = p.type === 'step' ? 1 : 2;
    const ok = await (await sels(idx)).evaluate((s, v) => [...s.options].some(o => o.value === v), JSON.stringify(p.rate));
    if (!ok) rec.partial.push('rate ' + p.rate + ' not in the list');
    else { await ctx.m.select(() => sels(idx), JSON.stringify(p.rate), { desc: 'rate' }); await waitUi(ctx); }
  }
  if (p.depth !== undefined) {
    await dragRange(ctx, async () => (await lab()).$('input.rwet:not([min="-60"]):not([min="-1"])') || (await lab()).$$('input.rwet').then(a => a[a.length - 1]), p.depth, 'modifier depth');
    await sleep(250);
    await waitUi(ctx);
  }
  if (p.phase !== undefined) rec.partial.push('phase not in UI');
};

D.setAutomation = async (ctx, p, rec) => {
  const lane = await openLane(ctx, p.param);
  if (await ev(ctx, (prm) => !!automationOf(prm), p.param)) throw new NoUi('existing curve: points one by one (double-click deletes)');
  const geo = await ev(ctx, (prm) => {
    const info = (autoTargets || []).find(t => t.param === prm) || automationOf(prm) || { min: 0, max: 1 };
    return autoRange(info);
  }, p.param);
  const pps = await ev(ctx, () => pps);
  for (const pt of p.points || []) {
    const laneEl = async () => (await lane()).$('.autolane');
    const h = await (await laneEl()).evaluate(e => e.clientHeight || 56);
    const f = (pt.value - geo[0]) / (geo[1] - geo[0]);
    const y = 6 + (1 - Math.max(0, Math.min(1, f))) * (h - 12);
    await ctx.m.click(laneEl, { desc: 'automation point', at: { x: pt.time * pps + 0.5, y }, noSize: true, wait: 150 });
    await waitUi(ctx);
  }
  if ((p.points || []).some(x => x.curve === 'step')) rec.partial.push('step points need shift-click');
  rec.partial.push('values by pointer height (lane ' + 'height px resolution)');
};

// ── Analyse, Referenz ──
// Analyze im Panel-Kopf: Ziel × Bereich (Variante B)
async function anaPick (ctx, target, range) {
  if (await ev(ctx, () => document.querySelector('#anaTarget').value) !== target)
    await ctx.m.select('#anaTarget', target, { desc: 'Analyze target' });
  if (await ev(ctx, () => document.querySelector('#anaRange').value) !== range)
    await ctx.m.select('#anaRange', range, { desc: 'Analyze range' });
}

// Auswahl im Lineal: Shift+Ziehen von start nach end (Songzeit)
async function rulerSelect (ctx, start, end) {
  const cur = await ev(ctx, () => rulerSel);
  if (cur && near(cur.start, start) && near(cur.end, end)) return;
  await reachTime(ctx, start);
  const pps0 = await ev(ctx, () => pps);
  await ctx.page.keyboard.down('Shift');
  ctx.m.rec.ui.keys++;
  try {
    await ctx.m.drag('#linearRuler', { desc: 'ruler selection (Shift+drag)', at: { x: start * pps0 + 1, y: 10 }, dx: Math.round((end - start) * pps0), dy: 0, noSize: true });
  } finally { await ctx.page.keyboard.up('Shift'); }
  await waitUi(ctx);
}

D.analyze = async (ctx, p, rec) => {
  let target = 'song';
  if (p.track !== undefined) {
    const tr = await trackOf(ctx, p.track);
    if (!tr || tr.dest >= 0) throw new NoUi('lane: Analyze takes main tracks (header click)');
    await selectTrack(ctx, p.track);
    target = 'track';
  }
  if (p.file) {
    const s = refSourceOf(ctx, p.file);
    const cur = await ev(ctx, () => refSourceFile());
    const up = (ctx.refUp || {})[(await ev(ctx, () => tree.projectDir)) + '|' + s.ref];
    if (up && up === await ev(ctx, () => refFile()) && (await ev(ctx, () => refState().source || 'mix')) === s.source && cur) {
      await ensureRange(ctx, { start: p.start, end: p.end }, s.offset, { end: s.end });
      await anaPick(ctx, 'reference', (await ev(ctx, () => refRange().start)) !== undefined ? 'refrange' : 'whole');
    } else {
      if (p.start !== undefined) rec.partial.push('file + time window: File… measures the whole file');
      await anaPick(ctx, 'file', 'whole');
      ctx.m.prompt(p.file);
    }
  } else if (p.start !== undefined || p.end !== undefined) {
    await rulerSelect(ctx, p.start || 0, p.end);
    await anaPick(ctx, target, 'ruler');
  } else if (p.section) {
    await selectSection(ctx, p.section);
    await anaPick(ctx, target, 'section');
  } else {
    await anaPick(ctx, target, 'whole');
  }
  if (p.tail !== undefined) rec.partial.push('tail not in UI');
  await ctx.m.click('#anaBtn', { desc: 'Analyze' });
  await waitUi(ctx, 120000);
};

async function refButton (ctx, sel, desc, prompts = [], opts = {}) {
  for (const t of prompts) ctx.m.prompt(t);
  await ctx.m.click(sel, { desc, ...opts });
  await waitUi(ctx, 300000);
}

D.detectTempo = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('tempo of the song/a track: no UI (only the reference)');
  await ensureRef(ctx, p.file, { start: p.start, end: p.end });
  refPart(rec, p, ['detail']);
  await refButton(ctx, '#refTempoBtn', 'Tempo');
};
D.detectKey = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('key of the song/a track: no UI');
  await ensureRef(ctx, p.file, p);
  refPart(rec, p, ['beatsPerBar']);
  await refButton(ctx, '#refKeyBtn', 'Key');
};
D.analyzeStructure = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('structure of the song: no UI');
  // Structure nimmt immer die ganze Quelle (mit dem Grid)
  await ensureRef(ctx, p.file, { bpm: p.bpm, firstDownbeat: p.firstDownbeat });
  refPart(rec, p, ['start', 'end', 'detail']);
  await refButton(ctx, '#refStructBtn', 'Structure');
};
D.findRepeats = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('repeats in the song: no UI');
  await ensureRef(ctx, p.file, p);
  refPart(rec, p, ['stems', 'limit', 'minSimilarity']);
  await openMenu(ctx, '#refPartMenu', 'Part');
  await refButton(ctx, '#refRepeatsBtn', 'Repeats');
};
D.describeLayers = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('layers of the song: no UI');
  await ensureRef(ctx, p.file, p);
  await openMenu(ctx, '#refPartMenu', 'Part');
  await refButton(ctx, '#refLayersBtn', 'Layers', p.exposed ? [p.exposed.start + '-' + p.exposed.end] : [], p.exposed ? { modifiers: ['Shift'] } : {});
};
D.compareClasses = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('compareClasses without a reference file');
  await ensureRef(ctx, p.file, { start: p.referenceStart || 0, end: p.referenceEnd ?? (p.referenceStart || 0) + 60 });
  if (p.section) await selectSection(ctx, p.section); else throw new NoUi('needs a selected section');
  await openMenu(ctx, '#refPartMenu', 'Part');
  const ign = (p.ignore || []).length;
  await refButton(ctx, '#refClassesBtn', 'Classes', ign ? [p.ignore.join(', ')] : [], ign ? { modifiers: ['Shift'] } : {});
};
D.compareSeparated = async (ctx, p, rec) => {
  const ref = p.reference || {};
  await ensureRef(ctx, ref.file, ref);
  await openMenu(ctx, '#refPartMenu', 'Part');
  const ex = ref.exposed;
  await refButton(ctx, '#refCrossBtn', 'Cross-check', (ex ? [ex.start + '-' + ex.end] : []).concat([p.file]), ex ? { modifiers: ['Shift'] } : {});
};
D.splitLayer = async (ctx, p, rec) => {
  await openMenu(ctx, '#refPartMenu', 'Part');
  const target = Array.isArray(p.target) ? p.target[0] : p.target;
  if (Array.isArray(p.target) && p.target.length > 1) rec.partial.push('one target file only');
  await refButton(ctx, '#refSplitBtn', 'Split', [target, (p.examples || []).map(x => x.name + '=' + x.file).join('; ')]);
};

D.compareReference = async (ctx, p, rec) => {
  const file = p.file || (p.reference || {}).file;
  await ensureRef(ctx, file, { start: p.referenceStart, end: p.referenceEnd });
  if (p.section) await selectSection(ctx, p.section); else await deselectSection(ctx);
  if (p.track !== undefined) await selectTrack(ctx, p.track);
  refPart(rec, p, ['start', 'end', 'downbeat', 'articulation']);
  await refButton(ctx, '#refCompare', 'Compare');
};

async function trackCompare (ctx, p, rec, btn, desc) {
  const ref = p.reference || {};
  if (p.file) throw new NoUi('compare a file (render) with the reference: no UI — only a track (File… side not built)');
  if (!ref.file) throw new NoUi('reference is a track, not a file');
  await ensureRef(ctx, ref.file, ref);
  refPart(rec, p, ['timbre', 'limit', 'minPitch', 'maxPitch', 'detail']);
  if (p.section) await selectSection(ctx, p.section); else await deselectSection(ctx);
  await selectTrack(ctx, p.track);
  await refButton(ctx, btn, desc);
}
D.compareVoice = (ctx, p, rec) => trackCompare(ctx, p, rec, '#refVoiceBtn', 'Voice');
D.compareDrums = (ctx, p, rec) => trackCompare(ctx, p, rec, '#refDrumsBtn', 'Drums');

async function soundJob (ctx, p, rec, btn, desc) {
  const ref = p.reference || {};
  if (!ref.file) throw new NoUi('reference is a track, not a file');
  await ensureRef(ctx, ref.file, { start: ref.start, end: ref.end });
  const st = await stateOf(ctx);
  const tr = (st.trackList || []).find(t => (t.routes || []).some(r => r.id === p.plugin));
  if (!tr) throw new NoUi('no track plays ' + p.plugin);
  if (p.notes && p.notes.section) await selectSection(ctx, p.notes.section); else await deselectSection(ctx);
  await selectTrack(ctx, tr.index);
  for (const k of ['limit', 'candidates', 'seconds', 'params', 'compare']) if (p[k] !== undefined) rec.partial.push(k + ' not in UI');
  await refButton(ctx, btn, desc);
}
D.matchSound = (ctx, p, rec) => soundJob(ctx, p, rec, '#refMatchBtn', 'Match sound');
D.tuneSound = (ctx, p, rec) => soundJob(ctx, p, rec, '#refTuneBtn', 'Tune');

// Describe ▾: Referenz-Quelle im Bereich oder die gewählte Spur
async function describe (ctx, p, rec, op) {
  const btn = { describeVoice: '#refDescVoice', describeTimbre: '#refDescTimbre', describeDrums: '#refDescDrums' }[op];
  const side = p.file ? 'reference' : 'track';
  if (p.file) await ensureRef(ctx, p.file, p);
  else {
    if (p.track === undefined) throw new NoUi(op + ' of the whole mix: no UI (Reference or a track)');
    const tr = await trackOf(ctx, p.track);
    if (!tr || tr.dest >= 0) throw new NoUi('lane: Describe takes main tracks');
    if (p.section) await selectSection(ctx, p.section); else await deselectSection(ctx);
    await selectTrack(ctx, p.track);
    if (p.start !== undefined) rec.partial.push('time window on a track: section only');
  }
  refPart(rec, p, ['timeline', 'voice', 'detail', 'minPitch', 'maxPitch', 'beatsPerBar']);
  await openMenu(ctx, '#refDescMenu', 'Describe');
  if (await ev(ctx, () => descSide) !== side)
    await ctx.m.click('#descSide button[data-side="' + side + '"]', { desc: 'Describe ' + side });
  await refButton(ctx, btn, op);
}
for (const op of ['describeVoice', 'describeTimbre', 'describeDrums']) D[op] = (ctx, p, rec) => describe(ctx, p, rec, op);

// Make ▾ → Notes only (audioToMidi einer Datei)
D.audioToMidi = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('notes of the song/a track: ♪ on a clip (whole clips only)');
  const mode = p.shape === 'chords' ? 'chords' : (p.mode || 'polyphonic');
  if (!['melody', 'bass', 'polyphonic', 'chords'].includes(mode)) throw new NoUi('mode ' + mode + ' not in UI');
  await ensureRef(ctx, p.file, p);
  refPart(rec, p, ['minPitch', 'maxPitch', 'legato', 'bends', 'keepOctaves']);
  await openMenu(ctx, '#refMakeMenu', 'Make');
  if (await ev(ctx, () => document.querySelector('#refMidiMode').value) !== mode) await ctx.m.select('#refMidiMode', mode, { desc: 'notes mode' });
  await refButton(ctx, '#refNotesBtn', 'Notes only');
};

// Make ▾ → Compare stems… (Dialog mit Paaren)
D.compareStems = async (ctx, p, rec) => {
  const pairs = p.pairs || [];
  if (!pairs.length) throw new NoUi('no pairs');
  const s0 = refSourceOf(ctx, pairs[0].file);
  if (!s0.stems) throw new NoUi('pairs not stems of a reference: ' + pairs[0].file);
  await ensureSource(ctx, pairs[0].file, { noSource: true });
  const r = p.reference || {};
  await ensureRange(ctx, { start: r.start ?? pairs[0].start, end: r.end ?? pairs[0].end, bpm: r.bpm, firstDownbeat: r.firstDownbeat ?? p.firstDownbeat }, s0.offset, { end: s0.end });
  if (p.section) await selectSection(ctx, p.section); else await deselectSection(ctx);
  refPart(rec, p, ['timbre', 'limit']);
  await openMenu(ctx, '#refMakeMenu', 'Make');
  await ctx.m.click('#refCmpStemsBtn', { desc: 'Compare stems…', view: 'compare stems dialog', wait: 200 });
  const st = await stateOf(ctx);
  const want = new Map();
  for (const pr of pairs) {
    const t = typeof pr.track === 'number' ? pr.track : ((st.trackList || []).find(x => x.name === pr.track) || {}).index;
    const s = refSourceOf(ctx, pr.file);
    if (t === undefined || !s.stems) { rec.partial.push('pair not possible: ' + pr.track + ' ↔ ' + pr.file); continue; }
    want.set(t, s.source);
  }
  const rows = await ev(ctx, () => [...document.querySelectorAll('#stemDlg .pair')].map(r => r.firstChild.textContent));
  for (let i = 0; i < rows.length; i++) {
    const tr = (st.trackList || []).find(x => x.name === rows[i] && !(x.dest >= 0));
    const v = tr && want.has(tr.index) ? want.get(tr.index) : '';
    const sel = async () => (await ctx.page.$$('#stemDlg .pair select'))[i];
    if (await (await sel()).evaluate(e => e.value) !== v) await ctx.m.select(sel, v, { desc: 'stem pair ' + rows[i] });
  }
  await ctx.m.click('#stemDlg button.primary', { desc: 'Compare (stems)' });
  await waitUi(ctx, 300000);
};

D.separateStems = async (ctx, p, rec) => {
  if (!p.file) throw new NoUi('stems of a track: use the clip ⧈ button (not driven)');
  if (p.insertSection) {
    // Make ▾ → Stems to tracks (Mix im Bereich als neue Spuren)
    await ensureRef(ctx, p.file, { start: p.start, end: p.end });
    await selectSection(ctx, p.insertSection);
    for (const k of ['dir', 'stems', 'insertStart']) if (p[k] !== undefined) rec.partial.push(k + ' not in UI');
    await openMenu(ctx, '#refMakeMenu', 'Make');
    await refButton(ctx, '#refStemsBtn', 'Stems to tracks');
    return;
  }
  // Analysematerial: Stems der Referenz als Quelle (ganzer Song aus dem
  // Stem-Cache, ein Klick auf einen Stem)
  for (const k of ['dir', 'start', 'end', 'stems']) if (p[k] !== undefined) rec.partial.push(k + ': the UI takes the stems of the whole reference');
  const s = refSourceOf(ctx, p.file);
  await ensureRefFile(ctx, s.ref);
  const base = path.basename(s.ref).replace(/\.[^.]+$/, '');
  const whole = path.join(CFG_STEMS, base);
  const stems = {};
  for (const k of STEM4) if (fs.existsSync(path.join(whole, k + '.wav'))) stems[k] = path.join(whole, k + '.wav');
  const up = await ev(ctx, () => refFile());
  if (await ev(ctx, (u) => !!localStorage.getItem('dawd.refStems.' + u), up)) return;   // die UI kennt sie schon
  if (Object.keys(stems).length === 4) await ev(ctx, ([u, st]) => { lsSet('dawd.refStems.' + u, st); }, [up, stems]);
  else rec.partial.push('no whole-song stems in the cache: the click separates (canned job)');
  await ctx.m.click('#refSrc button[data-src="vocals"]', { desc: 'reference source vocals (stems)' });
  await waitUi(ctx, 120000);
};

// ── Render ──
async function exportPop (ctx, p, rec, opts = {}) {
  await ctx.m.click('#renderBtn', { desc: 'Export', view: 'export popover', wait: 200 });
  await fillPop(ctx, 'Range', p.section ? 'sec:' + p.section : 'song');
  // Seit 2026-10-31 frei: Tail als Zahl, Feld „File“ (relativ zum Projekt,
  // ohne Endung; Stems und Batch: der Ordner)
  if (p.tail !== undefined) await fillPop(ctx, 'Tail s', String(p.tail));
  // Stems-Häkchen wie verlangt (es bleibt vom letzten Export stehen)
  const cb = async () => (await popField(ctx, 'Stems')).$('input');
  if (await (await cb()).evaluate(e => e.checked) !== !!opts.stems) await ctx.m.click(cb, { desc: 'stems checkbox' });
  const proj = await ev(ctx, () => tree.projectDir.replace(/\/$/, ''));
  let f = p.file || p.dir || '';
  if (f.startsWith(proj + '/')) f = f.slice(proj.length + 1);
  if (p.file) f = f.replace(/\.(wav|flac|aiff|ogg|mid)$/i, '');
  if ((await (await popField(ctx, 'File')).inputValue()) !== f)
    await ctx.m.fill(() => popField(ctx, 'File'), f, { desc: 'popover File', free: true });
}
D.render = async (ctx, p, rec) => {
  if (p.track !== undefined && p.name) {
    await clickClipButton(ctx, p.track, p.name, 'Render this clip', 'render clip ⤓');
    await waitUi(ctx, 300000);
    return;
  }
  await exportPop(ctx, p, rec);
  await ctx.m.click(() => byText(ctx, '#swatchPop .tpbtns button', 'Export'), { desc: 'Export' });
  await waitUi(ctx, 300000);
};
D.renderStems = async (ctx, p, rec) => {
  await exportPop(ctx, p, rec, { stems: true });
  await ctx.m.click(() => byText(ctx, '#swatchPop .tpbtns button', 'Export'), { desc: 'Export' });
  await waitUi(ctx, 300000);
};
D.renderBatch = async (ctx, p, rec) => {
  await exportPop(ctx, { ...p, section: (p.sections || [])[0] }, rec, { stems: !!p.stems });
  // Nodes: „Pick nodes…“ und ankreuzen (der aktuelle ist vorbelegt)
  if (p.nodes) {
    await fillPop(ctx, 'Batch nodes', 'pick');
    const want = new Set(p.nodes);
    for (const [id, on] of await ev(ctx, () => [...document.querySelectorAll('#swatchPop .batchpick label')]
      .map(l => [+l.textContent.match(/^#(\d+)/)[1], l.querySelector('input').checked]))) {
      if (want.has(id) === on) continue;
      await ctx.m.click(() => ctx.page.evaluateHandle((i) => [...document.querySelectorAll('#swatchPop .batchpick label')]
        .find(l => l.textContent.startsWith('#' + i + ' ')).querySelector('input'), id).then(h => h.asElement()),
        { desc: 'batch node #' + id, noSize: true });
    }
  }
  await fillPop(ctx, 'Batch ranges', 'range');
  await ctx.m.click(() => byText(ctx, '#swatchPop .tpbtns button', 'Batch'), { desc: 'Batch' });
  await waitUi(ctx, 600000);
};
D.exportMidi = async (ctx, p, rec) => {
  await ctx.m.click('#renderBtn', { desc: 'Export', view: 'export popover', wait: 200 });
  await fillPop(ctx, 'Format', 'mid');
  if (p.section) await fillPop(ctx, 'Range', 'sec:' + p.section);
  if (p.track !== undefined) rec.partial.push('one track: only via a clip in the piano roll (Export .mid)');
  await ctx.m.click(() => byText(ctx, '#swatchPop .tpbtns button', 'Export'), { desc: 'Export' });
  await waitUi(ctx);
};

D.undo = async (ctx, p) => {
  for (let i = 0; i < (p.steps || 1); i++) {
    await ctx.m.click('#undoBtn', { desc: 'Undo' });
    await waitUi(ctx);
  }
};

D.play = async (ctx, p) => {
  await ctx.m.click('#playBtn', { desc: 'Play/Stop' });
  await waitUi(ctx);
};

// ── Rezepte ──
async function recipeSearch (ctx, query) {
  await ensureTab(ctx, 'recipes');
  await ctx.m.fill('#recipeQuery', query, { desc: 'recipe search' });
  await ctx.m.press('Enter');
  await sleep(400);
  await waitUi(ctx);
}
D.findRecipes = async (ctx, p, rec) => {
  await recipeSearch(ctx, p.query || '');
  for (const k of ['tags', 'plugin']) if (p[k]) rec.partial.push(k + ' filter: click a chip in a row');
};
async function recipeRowTitle (ctx, id) {
  const h = await ctx.page.evaluateHandle((rid) => [...document.querySelectorAll('#recipeList .rrow b')].find(b => b.title.includes(rid)) || null, id);
  return h.asElement();
}
async function findRecipeRow (ctx, id, rec) {
  await ensureTab(ctx, 'recipes');
  if (await recipeRowTitle(ctx, id)) return;
  const info = await ctx.api('recipeInfo', { id });
  await recipeSearch(ctx, (info.recipe && info.recipe.title) || id);
  if (!await recipeRowTitle(ctx, id)) throw new NoUi('recipe not found in the list: ' + id);
}
D.recipeInfo = async (ctx, p, rec) => {
  await findRecipeRow(ctx, p.id, rec);
  if (await ev(ctx, (rid) => recipeOpen.has(rid), p.id)) return;
  await ctx.m.click(() => recipeRowTitle(ctx, p.id), { desc: 'recipe title (details)' });
  await waitUi(ctx);
};
D.applyRecipe = async (ctx, p, rec) => {
  await findRecipeRow(ctx, p.id, rec);
  if (p.track !== undefined) await selectTrack(ctx, p.track);
  if (p.params) for (const v of Object.values(p.params)) ctx.m.prompt(String(v));
  const btn = async () => (await (await recipeRowTitle(ctx, p.id)).evaluateHandle(b => [...b.parentElement.querySelectorAll('button')].find(x => x.textContent === 'Apply'))).asElement();
  await ctx.m.click(btn, { desc: 'recipe Apply' });
  await waitUi(ctx, 120000);
};
D.rateRecipe = async (ctx, p, rec) => {
  await findRecipeRow(ctx, p.id, rec);
  if (p.stars) {
    await ctx.m.click(async () => (await (await recipeRowTitle(ctx, p.id)).evaluateHandle((b, n) => b.parentElement.querySelectorAll('.rstars button')[n - 1], p.stars)).asElement(), { desc: 'recipe stars' });
    await waitUi(ctx);
  }
  if (p.used !== undefined || p.comment) {
    if (!await ev(ctx, (rid) => recipeOpen.has(rid), p.id)) { await ctx.m.click(() => recipeRowTitle(ctx, p.id), { desc: 'recipe title (details)' }); await waitUi(ctx); }
    const form = async () => (await (await recipeRowTitle(ctx, p.id)).evaluateHandle(b => b.closest('.rrow').parentElement.querySelector('.rfb'))).asElement();
    await ctx.m.click(async () => (await form()).$('input'), { desc: 'recipe feedback note' });
    await ctx.m.type(p.note || p.comment || '', { free: true });
    const label = p.comment ? 'Comment' : p.ok === false ? 'Did not work' : 'Worked';
    await ctx.m.click(async () => (await (await form()).evaluateHandle((f, l) => [...f.querySelectorAll('button')].find(b => b.textContent === l), label)).asElement(), { desc: 'recipe ' + label });
    await waitUi(ctx);
  }
  if (p.status) rec.partial.push('status via Approve/Reject');
};
D.saveRecipe = async (ctx, p, rec) => {
  await ensureTab(ctx, 'recipes');
  await ctx.m.click('#recipeSaveBtn', { desc: 'recipe Save…', view: 'recipe popover', wait: 200 });
  const field = (i) => async () => (await ctx.page.$$('#swatchPop input[type=text]'))[i];
  await ctx.m.fill(field(0), p.title, { desc: 'recipe title', free: true });
  if (p.goal) await ctx.m.fill(field(1), p.goal, { desc: 'recipe goal', free: true });
  if (p.kind) await ctx.m.select(async () => (await ctx.page.$$('#swatchPop select'))[0], p.kind, { desc: 'recipe kind' });
  if (p.tags) await ctx.m.fill(field(2), p.tags.join(', '), { desc: 'recipe tags', free: true });
  if (p.steps && p.steps.length) {
    await ctx.m.click('#swatchPop textarea', { desc: 'recipe steps' });
    await ctx.m.type(p.steps.map(s => s.text || s.op || '').join('\n'), { free: true });
    if (p.steps.some(s => s.op)) rec.partial.push('op steps: only text steps in the UI');
  }
  for (const k of ['notes', 'tracks', 'modules', 'plugins', 'measure', 'mixTarget']) if (p[k] !== undefined) rec.partial.push(k + ' not in the recipe popover');
  await ctx.m.click(() => ctx.page.locator('#swatchPop button.primary').first().elementHandle(), { desc: 'Save recipe' });
  await waitUi(ctx);
};

// ── Ansicht (Lese-Ops) ──
D.listParameters = async (ctx, p) => {
  const id = await moduleId(ctx, p.plugin);
  if (!id) throw new NoUi('plugin info only for modules in the project');
  await openCard(ctx, id);
};
D.listSamples = D.listParameters;
D.pluginInfo = D.listParameters;
D.listAutomatable = async (ctx, p) => {
  const m = /^(t\d+|p\d+)/.exec(p.target || '');
  if (!m) { await ensureProject(ctx); return; }
  await openLane(ctx, m[1].startsWith('t') ? m[1] + '.volume' : m[1] + '.wet').catch(() => {});
};

// Nicht im UI
for (const op of ['sampleLibraries', 'detectSongBoundaries'])
  D[op] = async () => { throw new NoUi(op + ': no UI path'); };

D.pickInstrument = async (ctx, p, rec) => {
  await ctx.m.click(rowSel(p.track) + ' .tplug.pickinstr', { desc: 'pick an instrument', view: 'instrument popover', wait: 300 });
  await waitUi(ctx);
  const bh = () => ctx.page.evaluateHandle((w) => [...document.querySelectorAll('#swatchPop .instrlist button')].find(b => b.textContent.startsWith(w)) || null, p.instrument).then(x => x.asElement());
  if (!await bh()) throw new NoUi('instrument not in list');
  await ctx.m.click(bh, { desc: 'instrument' });
  await waitUi(ctx);
};

D.addMidiFromAudio = async (ctx, p, rec) => {
  if (p.file) {
    const mode = p.shape === 'chords' ? 'chords' : (p.mode || 'melody');
    if (!['melody', 'bass', 'polyphonic', 'chords'].includes(mode)) throw new NoUi('To MIDI mode ' + mode + ' not in UI');
    await ensureRef(ctx, p.file, p);
    if (p.insertSection) await selectSection(ctx, p.insertSection);
    await openMenu(ctx, '#refMakeMenu', 'Make');
    if (await ev(ctx, () => document.querySelector('#refMidiMode').value) !== mode) await ctx.m.select('#refMidiMode', mode, { desc: 'To MIDI mode' });
    await refButton(ctx, '#refMidiBtn', 'To MIDI');
    return;
  }
  const st = await stateOf(ctx);
  const tr = (st.trackList || []).find(t => t.index === p.track);
  const cl = tr && (tr.clips || []).find(c => Math.abs(c.start - (p.start || 0)) < 0.05);
  if (!cl) throw new NoUi('transcribe a range: only whole clips (♪ on a clip)');
  await clickClipButton(ctx, p.track, cl.name, p.shape === 'chords' ? 'Transcribe as played chords' : 'Transcribe to MIDI', 'transcribe ♪');
  await waitUi(ctx, 120000);
};

// ── Rückmeldung (Kriterium 6) ─────────────────────────────────────────
async function inView (ctx, sel) {
  return ev(ctx, (s) => {
    const e = document.querySelector(s);
    if (!e || !e.textContent.trim()) return { shown: false };
    const r = e.getBoundingClientRect();
    return { shown: true, inView: r.bottom > 0 && r.top < innerHeight && r.height > 0, top: Math.round(r.top) };
  }, sel);
}
const toastFb = async (ctx, p, rec, re) => ({ where: 'status line', visible: re.test(rec.toast || ''), transient: true, text: (rec.toast || '').slice(0, 120) });
// Ergebnisse stehen im Analyse-Panel (rechte Spalte): neuer Eintrag oben, im Bild?
const panelFb = async (ctx, p, rec) => {
  const r = await ev(ctx, () => {
    const e = document.querySelector('#aList .res.new');
    if (!e) return { shown: false };
    const b = e.getBoundingClientRect(), l = document.querySelector('#aList').getBoundingClientRect();
    return { shown: true, id: e.querySelector('.rh .dim').textContent.split(' ')[0], inView: b.top < Math.min(innerHeight, l.bottom) - 20 && b.bottom > l.top && b.height > 0 };
  });
  const fresh = r.shown && r.id !== ctx.lastResultId;
  if (fresh) ctx.lastResultId = r.id;
  return { where: 'analysis panel', visible: fresh && !!r.inView, persistent: true, shownOffscreen: fresh && !r.inView, error: fresh ? undefined : 'no new result' };
};
const F = {};
for (const op of ['analyze', 'detectTempo', 'detectKey', 'analyzeStructure', 'findRepeats', 'compareVoice', 'compareDrums', 'matchSound', 'tuneSound',
                  'describeLayers', 'compareReference', 'describeVoice', 'describeTimbre', 'describeDrums', 'compareStems', 'audioToMidi',
                  'compareClasses', 'compareSeparated', 'splitLayer'])
  F[op] = panelFb;
F.render = (ctx, p, rec) => toastFb(ctx, p, rec, /rendered|exported/);
F.renderStems = F.render;
F.renderBatch = (ctx, p, rec) => toastFb(ctx, p, rec, /batch/);
F.separateStems = (ctx, p, rec) => toastFb(ctx, p, rec, /stem|separat/);

// ── Folge-Dateien: was der Agent schrieb, liegt danach unter seinem Pfad ──
async function fileFixup (ctx, op, p, rec) {
  if (op.op === 'separateStems') {
    // Stems aus dem Original-Lauf (Rechenzeit gespart)
    const src = op.params.dir;
    if (src && fs.existsSync(src) && p.dir) {
      fs.mkdirSync(p.dir, { recursive: true });
      for (const f of fs.readdirSync(src)) {
        const a = path.join(src, f);
        if (fs.statSync(a).isFile() && !fs.existsSync(path.join(p.dir, f))) fs.copyFileSync(a, path.join(p.dir, f));
      }
      rec.fixup = 'stems copied from the original run';
    }
    return;
  }
  if (['render', 'renderStems', 'renderBatch', 'exportMidi'].includes(op.op) && (p.file || p.dir)) {
    const r = await ctx.api(op.op, p);
    rec.fixup = 'API ' + op.op + ' to the agent path: ' + (r.ok !== false ? 'ok' : r.error);
  }
  if (op.op === 'saveRecipe' || op.op === 'rateRecipe') {
    const ui = rec.sent.find(s => s.op === op.op && s.result && s.result.ok);
    if (!ui) { const r = await ctx.api(op.op, p); rec.fixup = 'API ' + op.op + ': ' + (r.ok !== false ? 'ok' : r.error); }
  }
}

// Nicht rückgängig zu machende Ops (Baum): prüfen, sonst per API angleichen
const NU = {};
NU.treeNote = async (ctx, op, p, rec) => {
  const id = p.node ?? (await ctx.api('treeState', {})).current;
  const t = await ctx.api('treeState', {});
  const n = (t.nodes || []).find(x => x.id === id) || {};
  // Erwartung wie im Backend: append hängt mit Zeilenumbruch an
  const before = rec.noteBefore;
  if (n.note && (n.note === p.text || (p.append && n.note.endsWith('\n' + p.text)))) return null;
  rec.fixup = 'treeNote via API';
  return ctx.api('treeNote', p);
};
NU.treeOpen = async (ctx, op, p) => {
  const t = await ctx.api('treeState', {});
  if (t.ok && t.projectDir && t.projectDir.replace(/\/$/, '') === p.dir.replace(/\/$/, '')) return { ok: true, node: t.current };
  await ctx.api('treeClose', {});
  return ctx.api('treeOpen', p);
};
NU.treeLabel = async (ctx, op, p) => {
  const t = await ctx.api('treeState', {});
  const n = (t.nodes || []).find(x => x.id === p.node) || {};
  return n.label === p.label ? null : ctx.api('treeLabel', p);
};
NU.treeSeed = async (ctx, op, p, rec) => {
  let t = await ctx.api('treeState', {});
  if (t.ok && t.projectDir && t.projectDir.replace(/\/$/, '') !== p.dir.replace(/\/$/, '')) { await ctx.api('treeClose', {}); t = { ok: false }; }
  if (!t.ok) return ctx.api('treeSeed', p);
  const n = (t.nodes || []).find(x => x.id === t.current) || {};
  if (p.label && n.label !== p.label) await ctx.api('treeLabel', { node: t.current, label: p.label });
  return { ok: true, node: t.current };
};
NU.treeBranch = async (ctx, op, p, rec, okMut) => {
  const b = okMut.find(s => s.op === 'treeBranch');
  if (!b) return ctx.api('treeBranch', p);
  const t = await ctx.api('treeState', {});
  const n = (t.nodes || []).find(x => x.id === b.result.node) || {};
  if (p.label && n.label !== p.label) await ctx.api('treeLabel', { node: b.result.node, label: p.label });
  return b.result;
};
NU.treeCheckout = async (ctx, op, p) => {
  const t = await ctx.api('treeState', {});
  return t.current === p.node ? { ok: true, node: p.node } : ctx.api('treeCheckout', p);
};
NU.treeTerminate = async (ctx, op, p) => {
  const t = await ctx.api('treeState', {});
  const n = (t.nodes || []).find(x => x.id === p.node) || {};
  return (n.status === 'terminated') === (p.on !== false) ? { ok: true } : ctx.api('treeTerminate', p);
};
NU.treeFavorite = async (ctx, op, p) => {
  const t = await ctx.api('treeState', {});
  const n = (t.nodes || []).find(x => x.id === p.node) || {};
  return n.favorite ? { ok: true } : ctx.api('treeFavorite', p);
};

module.exports = Object.assign(D, { NoUi, feedback: F, fileFixup, nonUndoableFix: NU });

// Werte prüfen, die der Zustand nicht zeigt: Parameter nach dem Ziehen
module.exports.verify = {
  setParameter: async (ctx, p, op) => {
    const lp = await ctx.api('listParameters', { plugin: p.plugin });
    const out = [];
    for (const want of ((op.result || {}).set || [])) {
      if (want.kind === 'choice') {
        const st = (lp.settings || []).find(x => x.name === want.name);
        if (st && String(st.value) !== String(want.value)) out.push(want.name + ': ' + st.value + ' ≠ ' + want.value);
        continue;
      }
      const got = (lp.parameters || []).find(x => x.name === want.name);
      if (got && typeof want.value === 'number' && Math.abs(got.value - want.value) > 0.004)
        out.push(want.name + ': ' + (got.text || got.value) + ' ≠ ' + (want.text || want.value) + ' (slider ' + got.value.toFixed(4) + ' vs ' + want.value.toFixed(4) + ')');
    }
    return out;
  },
};

// applyRecipe ist kein Undo-Schritt (legt Zweig/Module an): gelang er im UI,
// gilt dessen Ergebnis — sonst einmal per API
module.exports.nonUndoableFix.applyRecipe = async (ctx, op, p, rec, okMut) => {
  const u = okMut.find(s => s.op === 'applyRecipe');
  return u ? u.result : ctx.api('applyRecipe', p);
};
