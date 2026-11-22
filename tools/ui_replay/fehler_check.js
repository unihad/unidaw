#!/usr/bin/env node
// Gegenprobe der sechs UI-Fehler aus dem Nachstell-Lauf 1 (Todo
// todos/2026-10-29-ui-fehler-nachstell-lauf.md): baut per API einen kleinen
// Song und bedient das Flow-UI wie eine Hand (Playwright, headless). Je
// Fehler eine Zahl (Versuche/Treffer) und ok/FEHLT. Exit 1, wenn ein Check
// fehlt. Start über fehler_check.sh (eigene dawd-Instanz im tmpfs).
//
//   NODE_PATH=~/dev/unspill/node_modules node tools/ui_replay/fehler_check.js \
//     --port 7950 --dir /tmp/…/song [--size 1920x1080] [--json out.json] [--shots <ordner>]

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
const PORT = +args.port || 7950;
const [W, H] = (args.size || '1920x1080').split('x').map(Number);
const DIR = path.resolve(args.dir);

async function api (op, params = {}) {
  const r = await fetch('http://127.0.0.1:' + PORT + '/command', { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + (process.env.UNIDAW_TOKEN || '') },
    body: JSON.stringify({ ...params, op }) });
  return r.json();
}

// Mono-WAV, 16 bit: Sägezahn, damit der Clip eine Wellenform hat
function writeWav (file, seconds, rate = 44100) {
  const n = Math.round(seconds * rate);
  const b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVE', 8); b.write('fmt ', 12);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) b.writeInt16LE(Math.round(8000 * (((i * 220 / rate) % 1) * 2 - 1)), 44 + i * 2);
  fs.writeFileSync(file, b);
}

const results = [];
function report (fehler, name, ok, detail) {
  results.push({ fehler, name, ok: !!ok, detail });
  console.log((ok ? 'ok    ' : 'FEHLT ') + (typeof fehler === 'number' ? 'F' + fehler : fehler) + ' ' + name + (detail !== undefined ? ' — ' + (typeof detail === 'string' ? detail : JSON.stringify(detail)) : ''));
}
const need = (r, what) => { if (!r || r.ok === false) throw new Error(what + ': ' + JSON.stringify(r)); return r; };

(async () => {
  // ── Song per API ──
  fs.rmSync(DIR, { recursive: true, force: true });
  fs.mkdirSync(DIR, { recursive: true });
  const media = path.join(DIR, '..', path.basename(DIR) + '-media');
  fs.mkdirSync(media, { recursive: true });
  writeWav(path.join(media, 'tone.wav'), 4);
  writeWav(path.join(media, 'loop.wav'), 2);
  writeWav(path.join(media, 'chop.wav'), 2);
  writeWav(path.join(media, 'free.wav'), 1);
  await api('treeClose');
  need(await api('treeSeed', { dir: DIR, label: 'check' }), 'treeSeed');
  need(await api('treeSettings', { autoBranch: false }), 'treeSettings');
  need(await api('updateSection', { name: 'Section 1', length: 8, bpm: 120 }), 'updateSection');
  const a = need(await api('addClip', { track: -1, file: path.join(media, 'tone.wav'), section: 'Section 1' }), 'addClip tone').track;
  const b = need(await api('addClip', { track: -1, file: path.join(media, 'loop.wav'), section: 'Section 1' }), 'addClip loop').track;
  const m = need(await api('createTrack', { type: 'midi', instrument: '4OSC' }), 'createTrack midi').index;
  const fx = need(await api('addPlugin', { name: 'Compressor', track: a }), 'addPlugin').id;
  // Marker an Anfang und Ende des Clips (0 s und 4 s) — dort sitzen Griffe
  const warp = await api('warpClip', { track: a, name: 'tone', markers: [{ source: 0, beat: 0 }, { source: 2, beat: 4 }, { source: 4, beat: 8 }] });
  need(await api('duplicateClip', { track: b, name: 'loop', start: 0 }), 'duplicateClip');
  // Clip unter zwei Nachbarn (stil-1): "chop 3" 1..3 unter "chop 4" 1..2 und "chop 2" 2..4
  const c = need(await api('addClip', { track: -1, file: path.join(media, 'chop.wav'), section: 'Section 1' }), 'addClip chop').track;
  need(await api('duplicateClip', { track: c, name: 'chop', start: 2 }), 'dup chop 2');
  need(await api('duplicateClip', { track: c, name: 'chop', start: 1 }), 'dup chop 3');
  need(await api('duplicateClip', { track: c, name: 'chop', start: 1 }), 'dup chop 4');
  need(await api('trimClip', { track: c, name: 'chop 4', length: 1 }), 'trim chop 4');
  // freier Clip für Einrasten, Kopie und Felder (120 BPM: 1/16 = 0,125 s)
  const d = need(await api('addClip', { track: -1, file: path.join(media, 'free.wav'), section: 'Section 1' }), 'addClip free').track;
  let st = await api('state', {});
  // addClip legt Spur + Lane an: Kopfzeile = Hauptspur, Clips in der Lane
  const head = (i) => { const t = st.trackList.find(x => x.index === i); return t && t.dest >= 0 ? t.dest : i; };
  const aHead = head(a);
  const inst = ((st.trackList.find(t => t.index === m) || {}).routes || [])[0];

  // ── Browser ──
  const exe = path.join(process.env.HOME, '.cache/ms-playwright/chromium_headless_shell-1234/chrome-linux/headless_shell');
  const browser = await chromium.launch({ executablePath: fs.existsSync(exe) ? exe : undefined });
  const page = await (await browser.newContext({ viewport: { width: W, height: H } })).newPage();
  const pageErrors = [];
  page.on('pageerror', e => pageErrors.push(e.message));
  // Mitschnitt der Befehle; Referenz-/Rezept-Ops werden abgefangen (nur die
  // Anfrage zählt, gerechnet wird nichts)
  const sent = [];
  const FAKE = { compareVoice: 1, compareDrums: 1, matchSound: 1, tuneSound: 1, applyRecipe: 1 };
  const FAKE_OK = {};   // Antwort ok ohne Wirkung (Record-Check: armTrack)
  // Export-Checks „E“: Render-Ops nicht rechnen, nur die Anfrage prüfen
  const FAKE_RESP = { render: b => ({ ok: true, file: b.file }), renderStems: b => ({ ok: true, stems: [{ file: b.dir + '/x.wav' }] }),
                      renderBatch: b => ({ ok: true, dir: b.dir, renders: [], renderSec: 0 }) };
  let recipeTracks = [{ type: 'midi' }];
  await page.route('**/command', async (route) => {
    let body = {};
    try { body = JSON.parse(route.request().postData() || '{}'); } catch (e) {}
    if (!['position', 'levels', 'inputLevel', 'interfaceMeters', 'interfaceState', 'captureStatus', 'progress'].includes(body.op)) sent.push(body);
    if (FAKE_OK[body.op]) return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: true, armed: true, input: 'check' }) });
    if (FAKE[body.op]) return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: false, error: 'check: not run' }) });
    if (FAKE_RESP[body.op]) return route.fulfill({ contentType: 'application/json', body: JSON.stringify(FAKE_RESP[body.op](body)) });
    if (body.op === 'recipeInfo' && body.id === 'check-recipe')
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: true, recipe: { id: 'check-recipe', tracks: recipeTracks, steps: [{ op: 'trackMix' }] } }) });
    // Antwort mitschreiben (ok?) — nur gelungene Änderungen werden zurückgenommen
    const resp = await route.fetch();
    try { body.resultOk = (await resp.json()).ok !== false; } catch (e) {}
    return route.fulfill({ response: resp });
  });
  page.on('dialog', d => d.accept(d.type() === 'prompt' ? '0-4' : undefined));
  const quiet = async (ms = 350) => { await page.waitForTimeout(ms); };
  const ev = (fn, x) => page.evaluate(fn, x);
  const since = (k, op) => sent.slice(k).filter(s => s.op === op);
  const toast = () => ev(() => { const s = document.querySelector('#status'); return s ? s.textContent : ''; });

  await page.goto('http://127.0.0.1:' + PORT + '/');
  await page.waitForFunction(() => typeof tree !== 'undefined' && tree && document.querySelector('#linearTracks .ltrow[data-track]'), null, { timeout: 30000 });
  await quiet(800);
  const rowSel = (i) => '#linearTracks .ltrow[data-track="' + i + '"]';
  const editOpen = () => ev(() => { const x = document.activeElement; return !!x && x.isContentEditable; });
  // Knopf des gewählten Clips: Leiste im Clip oder (schmaler Clip) daneben
  const selBtn = (t, title) => page.locator(rowSel(t) + ' .clip.sel .clbtns button[title^="' + title + '"], '
                                            + rowSel(t) + ' .clip.sel + .clbtns.beside button[title^="' + title + '"]');
  const center = async (sel) => { const bb = await page.locator(sel).first().boundingBox(); return { x: bb.x + bb.width / 2, y: bb.y + bb.height / 2 }; };

  // ── Dock-Höhe: Kante ziehen, Grenzen, gemerkt über Neuladen, Einklappen
  //    per Reiter, Doppelklick = Standard (Todo 2026-10-30-ui-dock-hoehe-ziehen)
  {
    const geo = () => ev(() => ({ dock: Math.round(document.querySelector('#dock').getBoundingClientRect().height),
                                   lin: Math.round(document.querySelector('#linearPane').getBoundingClientRect().height),
                                   work: Math.round(document.querySelector('#work').getBoundingClientRect().height),
                                   rem: parseFloat(getComputedStyle(document.documentElement).fontSize) }));
    const dragEdge = async (dy) => {
      const bb = await page.locator('#dockEdge').boundingBox();
      const x = bb.x + bb.width / 2, y = bb.y + bb.height / 2;
      await page.mouse.move(x, y); await page.mouse.down();
      for (let s = 1; s <= 8; s++) await page.mouse.move(x, y + dy * s / 8);
      await page.mouse.up(); await quiet(200);
    };
    const g0 = await geo();
    if (!await page.locator('#dockEdge').count()) report('D', 'Kante zum Ziehen (#dockEdge)', false, 'fehlt');
    else {
    await dragEdge(-150);
    const g1 = await geo();
    report('D', 'Kante 150 px nach oben: Dock höher, Timeline kleiner', Math.abs(g1.dock - g0.dock - 150) <= 2 && g1.lin < g0.lin, g0.dock + ' → ' + g1.dock + ' px');
    await page.reload();
    await page.waitForFunction(() => typeof tree !== 'undefined' && tree && document.querySelector('#linearTracks .ltrow[data-track]'), null, { timeout: 30000 });
    await quiet(800);
    const g2 = await geo();
    report('D', 'Höhe nach Neuladen gemerkt', Math.abs(g2.dock - g1.dock) <= 1, g2.dock + ' px');
    await dragEdge(-2000);
    const g3 = await geo();
    report('D', 'Timeline behält 10 rem', g3.lin >= 10 * g3.rem - 1, 'Timeline ' + g3.lin + ' px, Dock ' + g3.dock);
    await dragEdge(2000);
    const g4 = await geo();
    report('D', 'Dock behält 7 rem', g4.dock >= 7 * g4.rem - 1 && g4.dock <= 7 * g4.rem + 2, 'Dock ' + g4.dock + ' px');
    await page.locator('#dockTabs .docktab.on').click(); await quiet(300);
    const g5 = await geo();
    const edgeGone = !(await page.locator('#dockEdge').isVisible());
    const paneGone = await ev(() => ![...document.querySelectorAll('#dock .dockpane')].some(x => x.offsetHeight > 0));
    await page.locator('#dockTabs .docktab.on').click(); await quiet(300);
    const g6 = await geo();
    report('D', 'Reiter-Klick klappt ein (Kante weg) und wieder auf mit der gezogenen Höhe',
           paneGone && edgeGone && Math.abs(g6.dock - g4.dock) <= 1, g5.dock + ' / ' + g6.dock + ' px');
    await page.locator('#dockEdge').dblclick(); await quiet(200);
    const g7 = await geo();
    report('D', 'Doppelklick auf die Kante: Standardhöhe 42 %', Math.abs(g7.dock - g0.dock) <= 1 && !(await ev(() => localStorage.getItem('dawd.dockH'))), g7.dock + ' px');
    }
  }

  // ── F1: Spur umbenennen per Doppelklick ──
  {
    let opened = 0;
    for (let i = 0; i < 10; i++) {
      const p = await center(rowSel(aHead) + ' .ltname');
      await page.mouse.dblclick(p.x, p.y);
      await quiet(200);
      if (await editOpen()) opened++;
      await page.keyboard.press('Escape');
      await quiet(250);
    }
    report(1, 'Doppelklick auf den Spurnamen öffnet das Feld', opened === 10, opened + '/10');
  }

  // ── F1 + F3: Name mit Leerzeichen, R und M im Feld ──
  {
    const marks0 = ((await api('state', {})).markers || []).length;
    const p = await center(rowSel(aHead) + ' .ltname');
    await page.mouse.dblclick(p.x, p.y);
    await quiet(200);
    const open = await editOpen();
    if (open) { await page.keyboard.press('Control+a'); await page.keyboard.type('Bass Synth R M'); await page.keyboard.press('Enter'); }
    await quiet(700);
    st = await api('state', {});
    const name = (st.trackList.find(t => t.index === aHead) || {}).name;
    report(3, 'Spurname mit Leerzeichen, R, M bleibt ganz', name === 'Bass Synth R M', name);
    report(3, 'Tippen im Spurnamen: keine Aufnahme, kein Marker, kein Play',
           !st.recording && (st.markers || []).length === marks0 && !(await ev(() => playOn)),
           { recording: !!st.recording, markers: (st.markers || []).length - marks0 });
  }

  // ── F2: Plugin umbenennen (Routing Simple) ──
  {
    if (await ev(() => routingMode) !== 'simple') { await page.locator('#routing .rmodes button', { hasText: 'Simple' }).click(); await quiet(600); }
    const sn = '#routing .slot[data-plugin="' + fx + '"] .sname';
    let opened = 0;
    for (let i = 0; i < 10; i++) {
      const p = await center(sn);
      await page.mouse.dblclick(p.x, p.y);
      await quiet(200);
      if (await editOpen()) opened++;
      await page.keyboard.press('Escape');
      await quiet(450);
    }
    report(2, 'Doppelklick auf den Modulnamen öffnet das Feld', opened === 10, opened + '/10');
    const marks0 = ((await api('state', {})).markers || []).length;
    const p = await center(sn);
    await page.mouse.dblclick(p.x, p.y);
    await quiet(200);
    if (await editOpen()) { await page.keyboard.press('Control+a'); await page.keyboard.type('Drum Kit R M'); await page.keyboard.press('Enter'); }
    await quiet(700);
    st = await api('state', { detail: true });
    const label = ((st.routing || {}).plugins || []).find(x => x.id === fx) || {};
    report(3, 'Modulname mit Leerzeichen, R, M bleibt ganz', label.label === 'Drum Kit R M', label.label || label.name);
    report(3, 'Tippen im Modulnamen: keine Aufnahme, kein Marker', !st.recording && (st.markers || []).length === marks0,
           { recording: !!st.recording, markers: (st.markers || []).length - marks0 });
    // ein einfacher Klick öffnet die Karte weiterhin
    const was = await ev((id) => openParams.has(id), fx);
    await page.locator(sn).click();
    await quiet(900);
    report(2, 'Einfacher Klick auf den Modulnamen öffnet/schließt die Karte', (await ev((id) => openParams.has(id), fx)) !== was);
  }

  // ── F4: Referenz-Knöpfe und Rezept-Apply nehmen die gewählte MIDI-Spur ──
  {
    await ev(() => { localStorage.setItem(refKey(), '/nonexistent/ref.wav'); syncReference(); });
    // Referenz-Bereich einmal setzen (Variante B: Felder statt Prompt je Knopf)
    await page.locator('#refStart').fill('0');
    await page.locator('#refEnd').fill('4');
    await page.locator('#refEnd').press('Enter');
    await quiet(200);
    const clickHeader = async (i) => { const p = await center(rowSel(i) + ' .ltname'); await page.mouse.click(p.x, p.y); await quiet(400); };
    const run = async (fn) => { const k = sent.length; await ev(fn); await quiet(500); return k; };
    await clickHeader(m);
    let k = await run(() => doCompareVoice());
    let r = since(k, 'compareVoice');
    report(4, 'Voice nach Klick auf MIDI-Spur', r.length === 1 && r[0].track === m, r.length ? 'track ' + r[0].track : 'keine Anfrage: ' + await toast());
    k = await run(() => doCompareDrums());
    r = since(k, 'compareDrums');
    report(4, 'Drums nach Klick auf MIDI-Spur', r.length === 1 && r[0].track === m, r.length ? 'track ' + r[0].track : 'keine Anfrage: ' + await toast());
    k = await run(() => doMatchSound());
    r = since(k, 'matchSound');
    report(4, 'Match sound nach Klick auf MIDI-Spur', r.length === 1 && inst && r[0].plugin === inst.id, r.length ? 'plugin ' + r[0].plugin : 'keine Anfrage: ' + await toast());
    k = await run(() => doTuneSound());
    r = since(k, 'tuneSound');
    report(4, 'Tune nach Klick auf MIDI-Spur', r.length === 1 && inst && r[0].plugin === inst.id, r.length ? 'plugin ' + r[0].plugin : 'keine Anfrage: ' + await toast());
    recipeTracks = [{ type: 'midi' }];
    k = await run(() => applyRecipeUi({ id: 'check-recipe', title: 'Check' }));
    r = since(k, 'applyRecipe');
    report(4, 'Rezept (MIDI-Spur) auf die gewählte MIDI-Spur', r.length === 1 && r[0].track === m, r.length ? 'track ' + r[0].track : 'keine Anfrage');
    // beide gewählt: zuletzt geklickt = Audio → Voice nimmt Audio, Match bleibt MIDI
    await clickHeader(aHead);
    k = await run(() => doCompareVoice());
    r = since(k, 'compareVoice');
    report(4, 'Voice nach Klick auf Audio-Spur (MIDI vorher gewählt)', r.length === 1 && r[0].track === aHead, r.length ? 'track ' + r[0].track : 'keine Anfrage');
    k = await run(() => doMatchSound());
    r = since(k, 'matchSound');
    report(4, 'Match sound nimmt die gewählte MIDI-Spur, auch nach Klick auf Audio', r.length === 1 && inst && r[0].plugin === inst.id, r.length ? 'plugin ' + r[0].plugin : 'keine Anfrage: ' + await toast());
    recipeTracks = [{ type: 'midi' }];
    k = await run(() => applyRecipeUi({ id: 'check-recipe', title: 'Check' }));
    r = since(k, 'applyRecipe');
    report(4, 'Rezept (MIDI-Spur) nach Klick auf Audio: gewählte MIDI-Spur', r.length === 1 && r[0].track === m, r.length ? 'track ' + r[0].track : 'keine Anfrage');
    recipeTracks = [{ type: 'audio' }];
    k = await run(() => applyRecipeUi({ id: 'check-recipe', title: 'Check' }));
    r = since(k, 'applyRecipe');
    report(4, 'Rezept (Audio-Spur): gewählte Audio-Spur', r.length === 1 && r[0].track === aHead, r.length ? 'track ' + r[0].track : 'keine Anfrage');
  }

  // ── F5: Trimmen an den Kanten eines gewarpten Clips ──
  if (warp.ok === false) report(5, 'warpClip (Rubberband) fehlt', false, warp.error);
  else {
    // genug Zoom, damit der Clip breit ist
    for (let i = 0; i < 3; i++) { await ev(() => zoomBy(1.4)); await quiet(200); }
    await ev(() => refresh()); await quiet(600);
    const clipSel = rowSel(a) + ' .clip';
    let trims = 0, tries = 0;
    const where = [];
    for (const side of ['left', 'right']) for (const yPos of ['top', 'middle', 'bottom']) {
      // Kante ins Bild scrollen wie eine Hand (die Timeline ist mit der
      // Seitenleiste schmaler; in 1366×768 läge die rechte Kante sonst
      // hinter dem Rand)
      await ev(([sel, right]) => {
        const c = document.querySelector(sel).getBoundingClientRect(), sc = document.querySelector('#linearScroll');
        const v = sc.getBoundingClientRect(), x = right ? c.right : c.left;
        const lo = v.left + parseFloat(getComputedStyle(document.documentElement).fontSize) * 16, hi = v.right - 60;
        if (x > hi) sc.scrollLeft += x - hi; else if (x < lo) sc.scrollLeft -= lo - x;
      }, [clipSel, side === 'right']);
      await quiet(150);
      const bb = await page.locator(clipSel).first().boundingBox();
      const x = side === 'left' ? bb.x + 3 : bb.x + bb.width - 4;
      const y = yPos === 'top' ? bb.y + 5 : yPos === 'middle' ? bb.y + bb.height / 2 : bb.y + bb.height - 4;
      const k = sent.length;
      await page.mouse.move(x, y);
      await quiet(150);
      await page.mouse.down();
      for (let s = 1; s <= 6; s++) await page.mouse.move(x + (side === 'left' ? 5 : -5) * s, y);
      await page.mouse.up();
      await quiet(900);
      tries++;
      const done = sent.slice(k).filter(s => ['trimClip', 'warpClip', 'moveClip'].includes(s.op));
      if (done.some(s => s.op === 'trimClip' && s.resultOk)) trims++;
      where.push(side + '/' + yPos + ':' + (done.map(s => s.op + (s.resultOk ? '' : '!')).join('+') || 'nichts'));
      const undo = done.filter(s => s.resultOk).length;
      if (undo) { await api('undo', { steps: undo }); await ev(() => refresh()); await quiet(500); }
    }
    report(5, 'Ziehen an der Kante des gewarpten Clips trimmt', trims === tries, trims + '/' + tries + ' ' + where.join(' '));
  }

  // ── F6: ganz überdeckter Clip ist erreichbar ──
  {
    await ev(() => { selectedClip = null; renderLinear(lastState); });
    await quiet(300);
    // unten liegt, was in der Lane zuerst gezeichnet wird (Undo kann die
    // Reihenfolge ändern — darum frisch aus dem Zustand)
    st = await api('state', {});
    const [lower] = st.trackList.find(t => t.index === b).clips.map(c => c.name);
    const hits = () => ev(([sel, n]) => {
      const el = [...document.querySelectorAll(sel)].find(c => c.querySelector('.cname').lastChild.textContent === n);
      if (!el) return -1;
      const r = el.getBoundingClientRect();
      let hit = 0;
      for (let x = r.left + 1; x < r.right - 1; x += 4) for (let y = r.top + 1; y < r.bottom - 1; y += 4) {
        const t = document.elementFromPoint(x, y);
        if (t && t.closest('.clip') === el) hit++;
      }
      return hit;
    }, [rowSel(b) + ' .clip', lower]);
    const before = await hits();
    const shot = async (name) => { if (args.shots) await page.locator(rowSel(b)).screenshot({ path: path.join(args.shots, name + '.png') }); };
    await page.mouse.move(5, 5);
    await shot('verdeckt-vorher');
    const chip = await page.locator(rowSel(b) + ' .cunder').count();
    if (chip) { await page.locator(rowSel(b) + ' .cunder').first().click(); await quiet(400); }
    const sel = await ev(() => selectedClip && selectedClip.name);
    const after = await hits();
    await shot('verdeckt-gewaehlt');
    report(6, 'Verdeckter Clip: Marke +N am Clip darüber, Klick wählt ihn und holt ihn nach oben',
           chip > 0 && sel === lower && after > 0, { chip, selected: sel, freiePunkteVorher: before, nachher: after,
             lane: st.trackList.find(t => t.index === b).clips.map(c => c.name + '@' + c.start + '+' + c.length) });
    // gewählt und oben: am rechten Rand trimmen
    if (after > 0) {
      const el = page.locator(rowSel(b) + ' .clip.sel');
      const bb = await el.boundingBox();
      const k = sent.length;
      await page.mouse.move(bb.x + bb.width - 4, bb.y + bb.height / 2);
      await page.mouse.down();
      for (let s = 1; s <= 6; s++) await page.mouse.move(bb.x + bb.width - 4 - 5 * s, bb.y + bb.height / 2);
      await page.mouse.up();
      await quiet(900);
      const t = sent.slice(k).find(s => s.op === 'trimClip');
      report(6, 'Verdeckten Clip trimmen', !!t && t.name === lower, t ? t.name : 'kein trimClip');
      await api('undo');
      // Knopfleiste des gewählten (verdeckten) Clips: Kopie dahinter
      await quiet(600);
      const k2 = sent.length;
      const dup = selBtn(b, 'Duplicate');
      const vis = await dup.isVisible().catch(() => false);
      if (vis) { await dup.click({ timeout: 3000 }).catch((e) => console.log("dup click:", e.message.split("\n").slice(0, 12).join(" | "))); await quiet(900); }
      const d = sent.slice(k2).find(s => s.op === 'duplicateClip');
      report(6, 'Verdeckten Clip kopieren (Knopf ⧉)', !!d && d.name === lower,
             d ? d.name : { sichtbar: vis, gewaehlt: await ev(() => selectedClip && selectedClip.name) });
      if (d) await api('undo');
    } else report(6, 'Verdeckten Clip trimmen', false, 'nicht erreichbar');
  }

  // ── C: Clips genau setzen (Todo 2026-10-30-ui-clips-genau-setzen) ──
  {
    await page.keyboard.press('Escape');
    await ev(() => { selectedClip = null; closeSwatches(); renderLinear(lastState); });
    await quiet(300);
    const clipEl = (t, n) => page.evaluateHandle(([sel, n]) => [...document.querySelectorAll(sel)]
      .find(c => !c.classList.contains('cghost') && c.querySelector('.cname').lastChild.textContent === n) || null, [rowSel(t) + ' .clip', n]).then(h => h.asElement());
    const clipOf = async (t, n) => ((await api('state', {})).trackList.find(x => x.index === t).clips || []).find(x => x.name === n);
    const onGrid = (v) => Math.abs(v * 8 - Math.round(v * 8)) < 1e-6;   // 1/16 bei 120 BPM
    const ppsNow = () => ev(() => pps);
    // Zeit t der Lane ins Bild holen (Spurkopf-Spalte bleibt frei)
    // Zeile senkrecht ins Bild (nur scrollTop — die Zeile ist breiter als die Ansicht)
    const rowInView = async (track) => {
      await ev((sel) => { const sc = document.querySelector('#linearScroll'), v = sc.getBoundingClientRect(),
                               r = document.querySelector(sel).getBoundingClientRect();
                          if (r.bottom > v.bottom - 20) sc.scrollTop += r.bottom - v.bottom + 40; else if (r.top < v.top + 60) sc.scrollTop -= v.top + 60 - r.top; }, rowSel(track));
      await quiet(150);
    };
    const show = async (t, track) => {
      await ev((t) => { document.querySelector('#linearScroll').scrollLeft = Math.max(0, t * pps - 200); }, t);
      await rowInView(track);
    };
    // Ziehen von Clip-x (px ab Clip-Anfang) um dx px, mit Tasten
    const dragClip = async (t, n, atX, dx, mods = []) => {
      await rowInView(t);
      const h = await clipEl(t, n);
      const bb = await h.boundingBox();
      const x = bb.x + atX, y = bb.y + bb.height - 6;
      for (const k of mods) await page.keyboard.down(k);
      await page.mouse.move(x, y); await page.mouse.down();
      for (let i = 1; i <= 8; i++) await page.mouse.move(x + dx * i / 8, y);
      await page.mouse.up();
      for (const k of mods) await page.keyboard.up(k);
      await quiet(900);
    };
    const last = (k, op) => sent.slice(k).filter(x => x.op === op).pop();
    await show(0, d);
    let pp = await ppsNow();
    // C1: Körper ziehen rastet ein — Ziel 2,06 s (nicht auf dem Raster)
    let k = sent.length;
    await dragClip(d, 'free', 20, Math.round(2.06 * pp));
    let r = last(k, 'moveClip');
    report('C', 'Clip verschieben rastet auf 1/16 ein', r && r.resultOk && onGrid(r.start), r ? 'start ' + r.start : 'kein moveClip');
    if (r && r.resultOk) await api('undo');
    await ev(() => refresh()); await quiet(500);
    // C2: mit Shift frei (auf 1/100 s)
    k = sent.length;
    await dragClip(d, 'free', 20, Math.round(2.06 * pp), ['Shift']);
    r = last(k, 'moveClip');
    report('C', 'Shift + Ziehen: frei (kein Einrasten)', r && r.resultOk && !onGrid(r.start) && Math.abs(r.start - 2.06) < 2 / pp + 0.011, r ? 'start ' + r.start : 'kein moveClip');
    if (r && r.resultOk) await api('undo');
    await ev(() => refresh()); await quiet(500);
    // C3: rechte Kante rastet ein — Ende 1,0 → ~1,56 s
    k = sent.length;
    let bb = await (await clipEl(d, 'free')).boundingBox();
    await dragClip(d, 'free', bb.width - 3, Math.round(0.56 * pp));
    r = last(k, 'trimClip');
    report('C', 'Rechte Kante trimmen rastet ein', r && r.resultOk && onGrid(r.length), r ? 'length ' + r.length : 'kein trimClip');
    if (r && r.resultOk) await api('undo');
    // C3b: linke Kante rastet ein — Start 0 → ~0,31 s
    await ev(() => refresh()); await quiet(500);
    k = sent.length;
    await dragClip(d, 'free', 2, Math.round(0.31 * pp));
    r = last(k, 'trimClip');
    report('C', 'Linke Kante trimmen rastet ein', r && r.resultOk && onGrid(r.start), r ? 'start ' + r.start + ' length ' + r.length : 'kein trimClip');
    if (r && r.resultOk) await api('undo');
    await ev(() => refresh()); await quiet(500);
    // C4: Ctrl + Ziehen = Kopie an der Stelle (Original bleibt)
    k = sent.length;
    await dragClip(d, 'free', 20, Math.round(4.06 * pp), ['Control']);
    r = last(k, 'duplicateClip');
    const orig = await clipOf(d, 'free');
    report('C', 'Ctrl + Ziehen kopiert an die Stelle (gerastet), Original bleibt',
           r && r.resultOk && onGrid(r.start) && orig.start === 0 && !last(k, 'moveClip'), r ? 'start ' + r.start + ', Original ' + orig.start : 'kein duplicateClip');
    if (r && r.resultOk) await api('undo');
    await ev(() => refresh()); await quiet(500);
    // C4b: schmaler Clip (1 s bei 110 px/s): beim Hovern deckt die
    // Knopfleiste den Körper nicht — Ziehen 14 px hinter dem Anfang verschiebt
    const pps0 = await ppsNow();
    await ev(() => { pps = 110; renderLinear(lastState); });
    await show(0, d); await quiet(300);
    bb = await (await clipEl(d, 'free')).boundingBox();
    await page.mouse.move(bb.x + 14, bb.y + 8); await quiet(300);
    const hitBody = await ev(([x, y]) => { const t = document.elementFromPoint(x, y); return !!t && !t.closest('.clbtns, .cunder') && !!t.closest('.clip'); }, [bb.x + 14, bb.y + 8]);
    k = sent.length;
    await dragClip(d, 'free', 14, 220);
    r = last(k, 'moveClip');
    report('C', 'Schmaler Clip: Knopfleiste deckt den Körper nicht, Ziehen verschiebt', hitBody && r && r.resultOk && onGrid(r.start),
           { breite: Math.round(bb.width), koerperFrei: hitBody, start: r ? r.start : 'kein moveClip' });
    if (r && r.resultOk) await api('undo');
    await ev((p) => { pps = p; renderLinear(lastState); }, pps0);
    await ev(() => refresh()); await quiet(500);
    // C5: Felder im Popover — Start, Länge, Kopie bei
    await page.mouse.move(5, 5);
    const pop = async (t, n) => {
      await ev(([t, n]) => { selectedClip = { track: t, name: n }; renderLinear(lastState); }, [t, n]);
      await quiet(300);
      await selBtn(t, 'Clip gain').click();
      await quiet(300);
    };
    const field = (label) => page.locator('#swatchPop > span', { hasText: new RegExp('^' + label + '$') }).locator('xpath=following-sibling::*[1]');
    await pop(d, 'free');
    k = sent.length;
    const fill = (l, v) => field(l).fill(v, { timeout: 3000 }).catch(() => console.log('Feld fehlt: ' + l));
    await fill('Copy at s', '6');
    await fill('Start s', '3.25');
    await fill('Length s', '0.75');
    await page.keyboard.press('Enter');
    await quiet(1200);
    const fd = last(k, 'duplicateClip'), fm = last(k, 'moveClip'), ft = last(k, 'trimClip');
    const fr = await clipOf(d, 'free');
    report('C', 'Popover: Start, Länge, Kopie bei → moveClip/trimClip/duplicateClip genau',
           fd && fd.start === 6 && fm && fm.start === 3.25 && ft && ft.length === 0.75 && fr.start === 3.25 && fr.length === 0.75,
           { copy: fd && fd.start, start: fm && fm.start, length: ft && ft.length, clip: fr && fr.start + '+' + fr.length });
    const nUndo = [fd, fm, ft].filter(x => x && x.resultOk).length;
    if (nUndo) await api('undo', { steps: nUndo });
    await ev(() => refresh()); await quiet(500);
    // C6: Popover im Bild, jedes Feld erreichbar (niedriges Fenster)
    await page.setViewportSize({ width: W, height: 560 });
    await quiet(500);
    await ev(() => { const sc = document.querySelector('#linearScroll'); sc.scrollTop = sc.scrollHeight; });
    await pop(d, 'free');
    const reach = await ev(() => {
      const p = document.querySelector('#swatchPop');
      const out = { inView: p.getBoundingClientRect().top >= 0 && p.getBoundingClientRect().bottom <= innerHeight, miss: [] };
      for (const el of p.querySelectorAll('input, select')) {
        el.scrollIntoView({ block: 'nearest' });
        const r = el.getBoundingClientRect();
        const t = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
        if (t !== el && !el.contains(t)) out.miss.push(el.previousElementSibling ? el.previousElementSibling.textContent : el.type);
      }
      return out;
    });
    report('C', 'Clip-Popover im Bild, jedes Feld erreichbar (auch Fade out s)', reach.inView && !reach.miss.length, reach);
    await page.keyboard.press('Escape');
    await page.setViewportSize({ width: W, height: H });
    await quiet(500);
    // C7: Clip unter zwei Nachbarn — Marke +N am oberen, Klick wählt ihn, ziehen geht
    await ev(() => { selectedClip = null; renderLinear(lastState); });
    await show(0, c); await quiet(200);
    const chip = page.locator(rowSel(c) + ' .cunder');
    const chips = await chip.count();
    const chipFor = chips ? await chip.first().getAttribute('title') : '';
    if (chips) { await chip.first().click(); await quiet(400); }
    const sel3 = await ev(() => selectedClip && selectedClip.name);
    k = sent.length;
    if (sel3 === 'chop 3') await dragClip(c, 'chop 3', 30, Math.round(1.0 * pp));
    r = last(k, 'moveClip');
    report('C', 'Clip unter zwei Nachbarn: +N, Klick wählt ihn, Ziehen verschiebt ihn',
           chips === 1 && /"chop 3"/.test(chipFor) && sel3 === 'chop 3' && r && r.name === 'chop 3' && r.resultOk,
           { chips, chipFor, selected: sel3, move: r ? r.name + '→' + r.start : 'kein moveClip' });
    if (r && r.resultOk) await api('undo');
    await ev(() => refresh()); await quiet(500);
    // C8: Kopie des verdeckten Clips (⧉) lässt sich verschieben
    await ev(([t]) => { selectedClip = { track: t, name: 'chop 3' }; renderLinear(lastState); }, [c]);
    await quiet(300);
    k = sent.length;
    await selBtn(c, 'Duplicate').click({ timeout: 5000 }).catch(() => {});
    await quiet(900);
    const dup = last(k, 'duplicateClip');
    let mv = null;
    if (dup && dup.resultOk) {
      const copyName = (await api('state', {})).trackList.find(x => x.index === c).clips.map(x => x.name).find(n => !['chop', 'chop 2', 'chop 3', 'chop 4'].includes(n));
      await show(3, c);
      const k2 = sent.length;
      if (copyName) await dragClip(c, copyName, 30, Math.round(1.5 * pp));
      mv = last(k2, 'moveClip');
      report('C', 'Kopie eines verdeckten Clips verschieben', mv && mv.resultOk && mv.name === copyName, { copy: copyName, move: mv ? mv.name + '→' + mv.start : 'kein moveClip' });
      await api('undo', { steps: 1 + (mv && mv.resultOk ? 1 : 0) });
    } else report('C', 'Kopie eines verdeckten Clips verschieben', false, 'kein duplicateClip');
    await ev(() => { selectedClip = null; renderLinear(lastState); });

    // C9: Raster nach Zoom (Todo 2026-10-30-ui-clips-raster-zoom-takt-felder):
    // Ziel 2,83 s bei 120 BPM — Takt (2 s) → 2, Viertel → 3, Achtel → 2,75,
    // 1/16 → 2,875; das Label beim Ziehen nennt das Raster
    for (const [zoom, grid, want] of [[8, 'bar', 2], [30, '1/4', 3], [50, '1/8', 2.75], [200, '1/16', 2.875]]) {
      await ev((p) => { pps = p; renderLinear(lastState); }, zoom);
      await show(0, d); await quiet(300);
      const h = await clipEl(d, 'free');
      const box = await h.boundingBox();
      const x = box.x + Math.min(14, box.width / 2), y = box.y + box.height - 6, dx = Math.round(2.83 * zoom);
      k = sent.length;
      await page.mouse.move(x, y); await page.mouse.down();
      for (let i = 1; i <= 8; i++) await page.mouse.move(x + dx * i / 8, y);
      const lab = await ev(() => { const l = document.querySelector('.cdraglab'); return l ? l.textContent : ''; });
      await page.mouse.up(); await quiet(900);
      r = last(k, 'moveClip');
      report('C', 'Raster bei ' + zoom + ' px/s: ' + grid + ', Label nennt es',
             r && r.resultOk && Math.abs(r.start - want) < 1e-6 && lab.endsWith('snap ' + grid), { start: r ? r.start : 'kein moveClip', label: lab });
      if (r && r.resultOk) await api('undo');
      await ev(() => refresh()); await quiet(500);
    }
    await ev((p) => { pps = p; renderLinear(lastState); }, pp);

    // C10: Felder nehmen Takt.Schlag, Tempo/Takt je Section: Section 2 mit
    // 90 BPM in 3/4 hinter Section 1 (4 Takte 4/4 bei 120 BPM = 8 s).
    // bar 6.2 = Section 2, Takt 2, Schlag 2 = 4 Viertel bei 90 BPM → 8 + 2,667 s;
    // Länge bar 7.1 = Ende bei 8 + 6 Viertel = 12 s; Kopie bar 3.2 →
    // duplicateClip section "Section 1", startBeat 9 (das Backend rechnet) → 4,5 s
    const sec2 = await api('addSection', { name: 'Section 2', bars: 4, bpm: 90, timeSig: '3/4' });
    await ev(() => refresh()); await quiet(600);
    await show(0, d);
    await pop(d, 'free');
    k = sent.length;
    await fill('Copy at s', 'bar 3.2');
    await fill('Start s', 'bar 6.2');
    await fill('Length s', 'bar 7.1');
    await page.keyboard.press('Enter');
    await quiet(1200);
    {
      const fd = last(k, 'duplicateClip'), fm = last(k, 'moveClip'), ft = last(k, 'trimClip');
      const fr = await clipOf(d, 'free');
      const copy = fd && fd.resultOk && (await api('state', {})).trackList.find(x => x.index === d).clips.find(x => x.name !== 'free' && Math.abs(x.start - 4.5) < 1e-3);
      report('C', 'Felder mit Takt.Schlag: bar 6.2 / bar 7.1 / Kopie bar 3.2 (Tempo/Takt je Section)',
             sec2.ok && fd && fd.section === 'Section 1' && fd.startBeat === 9 && !!copy && fm && Math.abs(fm.start - 32 / 3) < 1e-4
             && ft && Math.abs(fr.start + fr.length - 12) < 1e-3,
             { section2: sec2.ok, copy: fd && { section: fd.section, startBeat: fd.startBeat, at: copy && copy.start }, start: fm && fm.start, end: fr && Math.round((fr.start + fr.length) * 1000) / 1000 });
      const n = [fd, fm, ft].filter(x => x && x.resultOk).length;
      if (n) await api('undo', { steps: n });
    }
    // unlesbare Eingabe: nichts geht raus, Meldung nennt das Format, Popover bleibt
    await ev(() => refresh()); await quiet(500);
    await pop(d, 'free');
    k = sent.length;
    await fill('Start s', 'soon');
    await page.keyboard.press('Enter');
    await quiet(500);
    const msg = await toast();
    const still = await page.locator('#swatchPop').count();
    report('C', 'Feld mit unlesbarer Lage: keine Op, Meldung, Popover bleibt',
           !last(k, 'moveClip') && /bar 3\.2/.test(msg) && still === 1, { msg, popover: still });
    await page.keyboard.press('Escape');
    if (sec2.ok) await api('undo');
    await ev(() => refresh()); await quiet(500);

    // ── M: ohne Modus-Umschalter (Decision 2026-10-30-modus-umschalter-weg) ──
    await ev(() => { selectedClip = null; closeSwatches(); refresh(); });
    await quiet(600);
    // M1: kein Umschalter, alle Bedienelemente da (Record/Monitor/Insert,
    // Arm, Automation, Freeze je Spur, Stern an der Section)
    const vis = await ev(([a, mi]) => {
      const shown = (sel) => { const e = document.querySelector(sel); return !!e && e.offsetParent !== null && getComputedStyle(e).visibility !== 'hidden'; };
      const row = (i) => '#linearTracks .ltrow[data-track="' + i + '"] ';
      return { modeSeg: !!document.querySelector('#modeSeg'), rec: shown('#recBtn'), mon: shown('#monBtn'), insert: shown('#insertBtn'),
               input: shown('#audioStrip'), arm: shown(row(a) + '.armbtn') && shown(row(mi) + '.armbtn'),
               auto: shown(row(a) + '.autobtn'), freeze: shown(row(mi) + '.frzbtn'),
               secStar: shown('#linearSections .section .secbtn[title*="avorite this section"]') };
    }, [aHead, m]);
    report('M', 'Kein Modus-Umschalter, Record/Monitor/Insert, Arm, A, Freeze, Section-Stern sichtbar',
           !vis.modeSeg && Object.entries(vis).every(([k, v]) => k === 'modeSeg' || v), vis);
    // M2: Stern im Clip-Popover setzt den Favoriten (favoriteClip), Popover zu
    await show(0, d);
    await pop(d, 'free');
    k = sent.length;
    const star = page.locator('#swatchPop .cpfav');
    const starTxt = await star.count() ? await star.textContent() : '';
    if (await star.count()) { await star.click(); await quiet(800); }
    const fc = last(k, 'favoriteClip');
    const favNow = (await clipOf(d, 'free')).favorite;
    report('M', 'Stern im Clip-Popover: favoriteClip, Clip markiert (★)',
           fc && fc.resultOk && favNow && !(await page.locator('#swatchPop').count())
           && await page.locator(rowSel(d) + ' .clip.favorite').count() === 1, { star: starTxt, sent: !!fc, favorite: favNow });
    if (fc && fc.resultOk) await api('undo');
    // M3: Stern an der Section
    await ev(() => refresh()); await quiet(500);
    k = sent.length;
    const sstar = page.locator('#linearSections .section[data-name="Section 1"] .secbtn[title*="avorite this section"]');
    if (await sstar.count()) { await page.locator('#linearSections .section[data-name="Section 1"]').hover(); await sstar.click(); await quiet(800); }
    const fs1 = last(k, 'favoriteSection');
    const secFav = ((await api('state', {})).sections.find(x => x.name === 'Section 1') || {}).favorite;
    report('M', 'Stern an der Section: favoriteSection', fs1 && fs1.resultOk && secFav === true, { sent: !!fs1, favorite: secFav });
    if (fs1 && fs1.resultOk) await api('undo');
    // M4: Record ohne Umschalter in einem process-Node: Tooltip nennt, wohin
    // der Take geht (Auto-Branch an/aus), R schickt record (abgefangen)
    const home = (await api('treeState')).current;
    const br = await api('treeBranch', { node: home });
    await api('setClip', { track: d, name: 'free', gainDb: -1 });   // erste Aktion: process
    await ev(() => refresh()); await quiet(700);
    const tipOff = await page.locator('#recBtn').getAttribute('title');
    await api('treeSettings', { autoBranch: true });
    await ev(() => refresh()); await quiet(700);
    const tipOn = await page.locator('#recBtn').getAttribute('title');
    FAKE_OK.armTrack = 1; FAKE.record = 1;
    k = sent.length;
    await page.mouse.click(5, H - 5);   // Fokus weg aus Feldern
    await page.keyboard.press('r');
    await quiet(800);
    const rc = last(k, 'record');
    const recMsg = await toast();
    delete FAKE_OK.armTrack; delete FAKE.record;
    report('M', 'Record im process-Node: Tooltip nach Auto-Branch, R schickt record',
           br.ok && /lands in this process node/.test(tipOff) && /new collect node/.test(tipOn) && rc && rc.on === true && !/Collect/.test(recMsg),
           { off: tipOff, on: tipOn, record: rc ? { on: rc.on, section: rc.section } : 'kein record', msg: recMsg });
    await api('treeSettings', { autoBranch: false });
    await api('treeCheckout', { node: home });
    await ev(() => refresh()); await quiet(500);
  }

  // ── K: Knöpfe ohne Hover, 24 px (Todo 2026-10-30-ui-knoepfe-sichtbar) ──
  {
    const away = async () => { await page.mouse.move(W - 3, Math.round(H / 2)); await quiet(250); };
    // K1: langer Clip (Ende außerhalb des Bildes): Klick wählt, Maus weg —
    // die Leiste steht im sichtbaren Teil, Knöpfe ≥ 24 px
    await ev(() => { selectedClip = null; closeSwatches(); refresh(); });
    await quiet(600);
    await ev(() => { pps = 400; renderLinear(lastState); document.querySelector('#linearScroll').scrollLeft = 0; });
    await quiet(300);
    await ev((sel) => { const sc = document.querySelector('#linearScroll'), r = document.querySelector(sel).getBoundingClientRect(),
                        v = sc.getBoundingClientRect(); if (r.bottom > v.bottom - 20 || r.top < v.top + 60) sc.scrollTop += r.top - v.top - 120; },
             rowSel(a) + ' .clip');
    await quiet(200);
    const tone = page.locator(rowSel(a) + ' .clip').first();
    const before = await ev((sel) => getComputedStyle(document.querySelector(sel)).display, rowSel(a) + ' .clip .clbtns');
    const tb = await tone.boundingBox();
    const hdrR = await ev(() => document.querySelector('#linearRulerRow .ltlabel').getBoundingClientRect().right);
    await page.mouse.click(Math.max(tb.x, hdrR) + 60, tb.y + tb.height - 8);
    await quiet(350);   // der Klick wählt ≈ 0,26 s verzögert (Decision 2026-10-31-clip-leiste-nur-gewaehlt)
    await away();
    const bar = await ev((sel) => {
      const b = document.querySelector(sel + ' .clip.sel .clbtns');
      if (!b) return { found: false };
      const r = b.getBoundingClientRect(), v = document.querySelector('#linearScroll').getBoundingClientRect(),
            c = b.closest('.clip').getBoundingClientRect();
      const btns = [...b.querySelectorAll('button')].map(x => x.getBoundingClientRect());
      return { found: true, shown: getComputedStyle(b).display !== 'none', inView: r.left >= v.left && r.right <= v.right && r.top >= v.top && r.bottom <= v.bottom,
               clipEndOut: c.right > v.right, min: Math.round(Math.min(...btns.map(x => Math.min(x.width, x.height)))), n: btns.length };
    }, rowSel(a));
    report('K', 'Langer Clip: Klick wählt, Maus weg — Leiste im Bild am sichtbaren Teil, Knöpfe ≥ 24 px',
           bar.found && bar.shown && bar.inView && bar.clipEndOut && bar.min >= 24, { ...bar, ohneWahl: before });
    // ⧉ der Leiste geht ohne Hover (Klick) — Kopie per API zurück
    let k = sent.length;
    await page.locator(rowSel(a) + ' .clip.sel .clbtns button[title^="Duplicate"]').click({ timeout: 3000 }).catch(() => {});
    await quiet(800);
    const dupK = sent.slice(k).find(x => x.op === 'duplicateClip');
    report('K', 'Knopf ⧉ der gewählten Leiste ohne Hover', dupK && dupK.resultOk, dupK ? dupK.name : 'kein duplicateClip');
    if (dupK && dupK.resultOk) await api('undo');
    await ev(() => { selectedClip = null; pps = 60; refresh(); });
    await quiet(600);

    // K2: Node-Knöpfe: am aktuellen Node da (✎), an den anderen "⋯" (24 px);
    // Klick auf "⋯", Maus weg — Knöpfe offen und klickbar; ✎ öffnet die Notiz
    await ev(() => refresh()); await quiet(600);
    const nodes = await ev(() => [...document.querySelectorAll('#tree .node')].map(n => {
      const b = n.querySelector('.nbtns'), m = n.querySelector('.nmore');
      const cs = b && getComputedStyle(b);
      const mr = m && m.getBoundingClientRect();
      return { cur: n.classList.contains('current'), shown: !!cs && parseFloat(cs.opacity) > 0.05 && cs.pointerEvents !== 'none',
               note: !!b && [...b.querySelectorAll('button')].some(x => x.textContent === '✎'),
               more: mr ? Math.round(Math.min(mr.width, mr.height)) : 0,
               min: b ? Math.round(Math.min(...[...b.querySelectorAll('button')].map(x => { const r = x.getBoundingClientRect(); return Math.min(r.width, r.height); }))) : 0 };
    }));
    const cur = nodes.find(n => n.cur), others = nodes.filter(n => !n.cur);
    report('K', 'Aktueller Node: Knöpfe und ✎ ohne Hover, ≥ 24 px', cur && cur.shown && cur.note && cur.min >= 24, cur);
    report('K', 'Andere Nodes: "⋯" sichtbar, ≥ 24 px', others.length > 0 && others.every(n => n.more >= 24), others.map(n => n.more));
    let opened = null, noteOpen = false;
    if (others.length && await page.locator('#tree .node:not(.current) .nmore').count()) {
      const node = page.locator('#tree .node:not(.current)').first();
      await node.locator('.nmore').click();
      await away();
      opened = await node.evaluate(n => { const b = n.querySelector('.nbtns'), cs = getComputedStyle(b);
        return { open: parseFloat(cs.opacity) > 0.05 && cs.pointerEvents !== 'none', fork: !!b.querySelector('button[title^="Fork"]'),
                 note: [...b.querySelectorAll('button')].some(x => x.textContent === '✎'),
                 min: Math.round(Math.min(...[...b.querySelectorAll('button')].map(x => { const r = x.getBoundingClientRect(); return Math.min(r.width, r.height); }))) }; });
      const pen = node.locator('.nbtns button', { hasText: '✎' });
      if (await pen.count()) { await pen.click(); await quiet(300); noteOpen = await page.locator('#tree .nnoteedit').count() === 1; }
      await page.keyboard.press('Escape'); await quiet(200);
      await page.mouse.click(W / 2, Math.round(H / 2)); await quiet(200);
    }
    report('K', 'Klick auf "⋯", Maus weg: Knöpfe offen (≥ 24 px), ✎ öffnet die Notiz', opened && opened.open && opened.note && opened.min >= 24 && noteOpen, { ...opened, noteOpen });
    const closed = await ev(() => document.querySelectorAll('#tree .node.btnsopen').length);
    report('K', 'Klick daneben schließt die Knöpfe wieder', closed === 0, closed);

    // K3: Spurkopf ohne Abschnitt — eingerückt in einer Gruppe und gefroren
    // (sechs Knöpfe), Knöpfe ≥ 24 px; Gruppen-Kopfzeile ebenso
    const fz = await api('freezeTrack', { track: m, on: true });
    const gr = await api('createGroup', { name: 'Check group', tracks: [b] });
    await ev(() => refresh()); await quiet(900);
    const heads = await ev(() => [...document.querySelectorAll('#linearTracks .ltrow')].filter(r => r.querySelector('.mixbtns')).map(row => {
      const lab = row.querySelector('.ltlabel'), lr = lab.getBoundingClientRect();
      const inner = lr.right - parseFloat(getComputedStyle(lab).paddingRight) + 0.5;
      const els = [...lab.querySelectorAll('.mixbtns button, .tmeter, input.gvol, .gdel')];
      const cut = els.filter(e => { const r = e.getBoundingClientRect(); return r.right > inner || r.width < 4; }).map(e => e.className);
      const bs = [...lab.querySelectorAll('.mixbtns button')].map(e => { const r = e.getBoundingClientRect(); return Math.min(r.width, r.height); });
      return { name: (row.querySelector('.ltname') || {}).textContent, group: row.classList.contains('grouprow'), ingroup: row.classList.contains('ingroup'),
               btns: bs.length, min: Math.round(Math.min(...bs)), cut };
    }));
    const six = heads.find(h => h.btns === 6), ing = heads.find(h => h.ingroup), grp = heads.find(h => h.group);
    report('K', 'Spurkopf: Knöpfe + Pegel ohne Abschnitt (gefroren: 6 Knöpfe, in Gruppe, Gruppen-Kopf), ≥ 24 px',
           fz.ok && gr.ok && six && ing && grp && heads.every(h => !h.cut.length && h.min >= 24),
           { freeze: fz.ok, group: gr.ok, cut: heads.filter(h => h.cut.length).map(h => h.name + ': ' + h.cut.join(',')),
             min: Math.min(...heads.map(h => h.min)), six: !!six, ingroup: !!ing });
  }

  // ── P: Piano-Roll mit Tonumfang 0–127, Velocity je Note, Name beim
  //    Anlegen (Todo 2026-10-30-ui-piano-roll-erweitern) ──
  {
    const pshot = async (name) => { if (args.shots) await page.screenshot({ path: path.join(args.shots, name + '-' + W + 'x' + H + '.png') }); };
    const dr = need(await api('createTrack', { type: 'midi', instrument: '4OSC', name: 'Drums' }), 'createTrack Drums').index;
    need(await api('addSection', { name: 'Long', length: 48, bpm: 120 }), 'addSection Long');
    await ev(() => { selectedClip = null; selectedSection = ''; cursorPos = null; setDockTab('notes', true); refresh(); });
    await quiet(900);
    const clickHeader = async (i) => {
      await page.locator(rowSel(i) + ' .ltname').scrollIntoViewIfNeeded();
      const p = await center(rowSel(i) + ' .ltname'); await page.mouse.click(p.x, p.y); await quiet(500);
    };
    if (!await ev(() => prOpen)) { await page.locator('#prToggle').click(); await quiet(400); }
    await page.locator('#prWrap').scrollIntoViewIfNeeded();
    // sichtbare Tonhöhen der Rolle (Zeile 12 px, oben 127)
    const view = () => ev(() => { const w = document.querySelector('#prWrap'), hi = 127 - Math.floor(w.scrollTop / 12);
      return { keys: document.querySelectorAll('#prKeys .prkey').length, hi, lo: 127 - Math.floor((w.scrollTop + w.clientHeight - 1) / 12) }; });
    await clickHeader(m);
    const vSynth = await view();
    await clickHeader(dr);
    const vDrums = await view();
    report('P', 'Tonumfang 0–127, Start um C4, auf "Drums" ab 36', vSynth.keys === 128 && vSynth.lo <= 60 && vSynth.hi >= 60
           && vDrums.lo <= 36 && vDrums.lo >= 30 && vDrums.hi > 36, { synth: vSynth, drums: vDrums });

    // Noten setzen: wie eine Hand, Zeile ins Bild per Mausrad; neue Noten
    // nehmen die Velocity aus dem Feld
    const gridAt = async (pitch, col) => {
      // sichtbarer Teil der Rolle: im Reiter (scrollt selbst) und im Fenster
      const wb = await ev(() => { const a = document.querySelector('#prWrap').getBoundingClientRect(), b = document.querySelector('#midiStrip').getBoundingClientRect();
        const y = Math.max(a.top, b.top, 0), y2 = Math.min(a.bottom, b.bottom, innerHeight); return { x: a.left, width: a.width, y, height: y2 - y }; });
      for (let i = 0; i < 40; i++) {
        const gb = await page.locator('#prGrid').boundingBox();
        const y = gb.y + (127 - pitch) * 12 + 6;
        if (y > wb.y + 6 && y < wb.y + wb.height - 18) return { x: gb.x + col * 12 + 4, y };
        await page.mouse.move(wb.x + wb.width / 2, wb.y + wb.height / 2);
        const step = Math.min(100, Math.max(12, Math.round(wb.height / 2 / 12) * 12));
        await page.mouse.wheel(0, y < wb.y + 6 ? -step : step); await quiet(80);
      }
      throw new Error('pitch ' + pitch + ' not reachable');
    };
    const note = async (pitch, col) => { const p = await gridAt(pitch, col); await page.mouse.click(p.x, p.y); await quiet(120); };
    const setVel = async (v) => { await page.locator('#prVel').fill(String(v)); await page.locator('#prVel').press('Enter'); await quiet(150); };
    await note(36, 0); await note(38, 4);
    await setVel(70);
    await note(42, 2);
    await page.locator('#prName').fill('Kick A');
    await pshot('piano-anlegen-name');
    await note(96, 6);
    let k = sent.length;
    await page.locator('#prAddBtn').click();
    await quiet(1000);
    const add = since(k, 'addMidiClip')[0];
    const got = add ? add.notes.map(n => n.pitch + ':' + (n.velocity || 100)).join(' ') : '';
    const kn = await api('clipNotes', { track: dr, name: 'Kick A' });
    const have = kn.ok ? kn.notes.map(n => n.pitch + ':' + n.velocity).sort().join(' ') : kn.error;
    report('P', 'Anlegen: Noten < 48 und > 84, Velocity aus dem Feld, Name "Kick A"',
           add && add.resultOk && add.name === 'Kick A' && got === '36:100 42:70 38:100 96:70' && have === '36:100 38:100 42:70 96:70',
           { sent: add ? add.name + ' ' + got : 'kein addMidiClip', clip: have });

    // Velocity-Spur: Doppelklick öffnet den Clip, Ziehen am Balken der
    // Note 36 (Spalte 0) setzt sie, Feld + Enter setzt sie genau
    await ev(() => refresh()); await quiet(600);
    const clipEl = page.locator('#linearTracks .clip', { hasText: 'Kick A' }).first();
    await clipEl.scrollIntoViewIfNeeded();
    await clipEl.dblclick();
    await quiet(900);
    await page.locator('#velLane').scrollIntoViewIfNeeded();
    const lb = await page.locator('#velLane').boundingBox();
    k = sent.length;
    await page.mouse.move(lb.x + 4, lb.y + 4); await page.mouse.down();
    for (let s = 1; s <= 6; s++) await page.mouse.move(lb.x + 4, lb.y + 4 + 26 * s / 6);
    await page.mouse.up();
    await quiet(900);
    const v1 = await api('clipNotes', { track: dr, name: 'Kick A' });
    const vel = (r, p) => ((r.notes || []).find(n => n.pitch === p) || {}).velocity;
    const dragged = vel(v1, 36), field1 = await page.locator('#prVel').inputValue();
    await pshot('piano-velocity');
    await setVel(64);
    await quiet(900);
    const v2 = await api('clipNotes', { track: dr, name: 'Kick A' });
    report('P', 'Velocity-Spur: Ziehen setzt die Note (≈ 32), Feld zeigt den Wert, Enter setzt genau 64',
           since(k, 'setMidiNotes').length >= 2 && Math.abs(dragged - 32) <= 3 && +field1 === dragged
           && vel(v2, 36) === 64 && vel(v2, 38) === 100 && vel(v2, 96) === 70,
           { gezogen: dragged, feld: field1, danach: { 36: vel(v2, 36), 38: vel(v2, 38), 96: vel(v2, 96) } });
    await page.locator('#prDoneBtn').click(); await quiet(500);

    // Clip länger als 32 s: Section "Long" (48 s) — die Rolle reicht bis
    // 48 s, eine Note bei 40 s geht
    await ev(() => { selectedSection = 'Long'; refresh(); }); await quiet(700);
    const wGrid = await ev(() => parseFloat(document.querySelector('#prGrid').style.width));
    await page.locator('#prWrap').evaluate(e => { e.scrollLeft = 320 * 12 - 200; });
    await quiet(150);
    await note(60, 320);
    const at40 = await ev(() => prNotes.some(n => Math.abs(n.start - 40) < 1e-6));
    await pshot('piano-lang');
    await page.locator('#midiNewNotes .prhead button', { hasText: 'Clear' }).click();
    await ev(() => { selectedSection = ''; refresh(); }); await quiet(400);
    report('P', 'Section 48 s: Rolle 48 s breit, Note bei 40 s', wGrid >= 48 / 0.125 * 12 && at40, { breite: wGrid, note40: at40 });

    // Clip-Länge (Entscheidung 2026-10-30): neu über das Feld „Length“;
    // beim Bearbeiten auf einem Node mit Kind (Auto-Branch) bleibt die Rolle
    // nach dem ersten Speichern offen (Nachstell-Lauf remix-1 #1484), dann
    // Ende ziehen (rastet auf 1/16) und Feld
    const clipLen = async (name) => ((await api('state', {})).trackList.flatMap(t => t.clips || []).find(c => c.name === name) || {}).length;
    await ev(() => { selectedSection = 'Long'; cursorPos = null; refresh(); }); await quiet(700);
    await note(60, 8);
    await page.locator('#prName').fill('Len A');
    await page.locator('#prLen').fill('6'); await page.locator('#prLen').press('Enter'); await quiet(200);
    const endNew = await ev(() => { const e = document.querySelector('#prGrid .prend'); return e ? parseFloat(e.style.left) : -1; });
    k = sent.length;
    await page.locator('#prAddBtn').click(); await quiet(1000);
    const addL = since(k, 'addMidiClip')[0];
    const len0 = await clipLen('Len A');
    const home2 = (await api('treeState')).current;
    await api('treeBranch', { node: home2 });
    await api('treeCheckout', { node: home2 });
    await api('treeSettings', { autoBranch: true });
    await ev(() => { selectedSection = ''; refresh(); }); await quiet(700);
    const lc = page.locator('#linearTracks .clip', { hasText: 'Len A' }).first();
    await lc.scrollIntoViewIfNeeded(); await lc.dblclick(); await quiet(900);
    await page.locator('#prWrap').scrollIntoViewIfNeeded();
    await note(62, 16); await quiet(1200);
    const afterSave = { node: await ev(() => tree.current), open: await ev(() => !!prEdit), done: await page.locator('#prDoneBtn').isVisible() };
    await page.locator('#prWrap').evaluate(e => { e.scrollLeft = 0; });
    const hb = await page.locator('#prGrid .prend').boundingBox();
    const ym = await ev(() => { const a = document.querySelector('#prWrap').getBoundingClientRect(), b = document.querySelector('#midiStrip').getBoundingClientRect();
      return (Math.max(a.top, b.top, 0) + Math.min(a.bottom, b.bottom, innerHeight)) / 2; });
    k = sent.length;
    await page.mouse.move(hb.x + 1, ym); await page.mouse.down();
    for (let s = 1; s <= 8; s++) await page.mouse.move(hb.x + 1 - 16 * 12 * s / 8, ym);
    await page.mouse.up(); await quiet(1000);
    const trDrag = since(k, 'trimClip')[0];
    const len1 = await clipLen('Len A'), field1L = await page.locator('#prLen').inputValue();
    await pshot('piano-laenge');
    await page.locator('#prLen').fill('3'); await page.locator('#prLen').press('Enter'); await quiet(1000);
    const len2 = await clipLen('Len A');
    const stillOpen = await page.locator('#prDoneBtn').isVisible();
    await page.locator('#prDoneBtn').click(); await quiet(500);
    await api('treeSettings', { autoBranch: false });
    report('P', 'Clip-Länge: Feld beim Anlegen (6 s), Bearbeiten im Auto-Node bleibt offen, Ende ziehen (4 s), Feld (3 s)',
           endNew === 6 / 0.125 * 12 && addL && addL.length === 6 && Math.abs(len0 - 6) < 1e-3
           && afterSave.node !== home2 && afterSave.open && afterSave.done
           && trDrag && trDrag.length === 4 && Math.abs(len1 - 4) < 1e-3 && field1L === '4' && Math.abs(len2 - 3) < 1e-3 && stillOpen,
           { griffNeu: endNew, gesendet: addL ? addL.length : 'kein addMidiClip', neu: len0, nachSpeichern: afterSave,
             gezogen: trDrag ? trDrag.length : 'kein trimClip', nachZiehen: len1, feld: field1L, nachFeld: len2, offen: stillOpen });
  }

  // ── E: Export frei — Tail, Dateiname, Batch-Nodes ankreuzen (Todo
  // 2026-10-30-ui-export-kleinkram; Render-Ops abgefangen) ──
  {
    await page.keyboard.press('Escape'); await quiet(200);
    const openExport = async () => { await page.locator('#renderBtn').click(); await quiet(300); };
    const field = (label) => page.locator('#swatchPop span').filter({ hasText: new RegExp('^' + label + '$') }).locator('xpath=following-sibling::*[1]');
    const label = await ev(() => tree.nodes.find(n => n.id === tree.current).label.replace(/[^\w.-]+/g, '_'));
    await openExport();
    const popIn = await ev(() => { const r = document.querySelector('#swatchPop').getBoundingClientRect(); return r.top >= 0 && r.bottom <= innerHeight && r.left >= 0 && r.right <= innerWidth; });
    const tailType = await field('Tail s').evaluate(e => e.type);
    await field('Tail s').fill('1.5');
    await field('File').fill('renders/zwischenstand-1');
    let k = sent.length;
    await page.locator('#swatchPop button.primary').click(); await quiet(500);
    const r1 = since(k, 'render')[0];
    report('E', 'Tail 1,5 s und Datei "renders/zwischenstand-1" → render im Projekt', tailType === 'number' && r1 && r1.tail === 1.5
           && r1.file === DIR + '/renders/zwischenstand-1.wav', r1 ? { file: r1.file, tail: r1.tail } : 'kein render');
    await openExport();
    const kept = await field('File').inputValue();
    await field('File').fill('');
    const ph = await field('File').getAttribute('placeholder');
    await field('Range').selectOption({ index: 1 });
    const phSec = await field('File').getAttribute('placeholder');
    await field('Range').selectOption('song');
    k = sent.length;
    await page.locator('#swatchPop button.primary').click(); await quiet(500);
    const r2 = since(k, 'render')[0];
    report('E', 'Feld bleibt stehen; leer = exports/<Node>, Platzhalter mit Section', kept === 'renders/zwischenstand-1'
           && ph === 'exports/' + label && /^exports\/.+-.+/.test(phSec) && r2 && r2.file === DIR + '/exports/' + label + '.wav',
           { kept, ph, phSec, file: r2 ? r2.file : 'kein render' });
    await openExport();
    await field('Batch nodes').selectOption('pick');
    const boxes = page.locator('#swatchPop .batchpick input');
    const nBoxes = await boxes.count();
    const pre = await page.locator('#swatchPop .batchpick input:checked').count();
    const ids = await ev(() => tree.nodes.map(n => n.id));
    const curId = await ev(() => tree.current);
    const other = ids.find(id => id !== curId);
    if (other !== undefined) await boxes.nth(ids.indexOf(other)).check();
    const popIn2 = await ev(() => { const r = document.querySelector('#swatchPop').getBoundingClientRect(); return r.top >= 0 && r.bottom <= innerHeight; });
    await field('File').fill('renders/varianten');
    k = sent.length;
    await page.locator('#swatchPop button', { hasText: 'Batch' }).click(); await quiet(500);
    const b1 = since(k, 'renderBatch')[0];
    const want = [curId, other];
    report('E', 'Batch "Pick nodes…": aktueller Node vorbelegt, zweiter angekreuzt, Ordner aus "File"', nBoxes === ids.length && pre === 1 && other !== undefined
           && b1 && JSON.stringify([...b1.nodes].sort()) === JSON.stringify(want.sort()) && b1.dir === DIR + '/renders/varianten' && b1.tail === 1.5,
           { boxes: nBoxes, vorbelegt: pre, nodes: b1 ? b1.nodes : 'kein renderBatch', want, dir: b1 ? b1.dir : '' });
    await openExport();
    await field('File').fill('');
    k = sent.length;
    await page.locator('#swatchPop button', { hasText: 'Batch' }).click(); await quiet(500);
    const b2 = since(k, 'renderBatch')[0];
    report('E', 'Batch ohne "File" nach exports/batch-<Zeit>; Popover im Bild', b2 && b2.dir.startsWith(DIR + '/exports/batch-') && popIn && popIn2,
           { dir: b2 ? b2.dir : 'kein renderBatch', imBild: popIn, mitListe: popIn2 });
    await page.keyboard.press('Escape'); await quiet(200);
  }

  // ── W: Doppelklick auf einen nicht gewählten Audio-Clip trifft den Clip,
  // nie die Knopfleiste (Decision 2026-10-31-clip-leiste-nur-gewaehlt;
  // warpClip abgefangen) ──
  {
    await page.keyboard.press('Escape'); await quiet(200);
    await ev(() => { selectedClip = null; renderLinear(lastState); });
    const tgt = await ev(() => { for (const t of lastState.trackList) for (const c of t.clips || [])
      if (c.type === 'audio' && !t.clips.some(o => o !== c && o.start < c.start + c.length && o.start + o.length > c.start)) return { t: t.index, name: c.name };
      return null; });
    const csel = () => page.locator(rowSel(tgt.t) + ' .clip').filter({ has: page.locator('.cname', { hasText: tgt.name }) }).first();
    // hineinzoomen, bis die Leiste im Clip läge (nicht daneben)
    for (let i = 0; i < 12 && (await csel().boundingBox()).width < 320; i++) { await ev(() => zoomBy(1.4)); await quiet(250); }
    // Zeile und Clip ins Bild der Timeline (sie scrollt in sich; unten liegt das Dock)
    await csel().evaluate((c) => { const sc = document.querySelector('#linearScroll'), v = sc.getBoundingClientRect();
      let r = c.getBoundingClientRect(); if (r.bottom > v.bottom - 20 || r.top < v.top + 60) sc.scrollTop += r.top - v.top - 120;
      const hdr = document.querySelector('#linearRulerRow .ltlabel').getBoundingClientRect().right;
      r = c.getBoundingClientRect(); sc.scrollLeft += r.left - hdr - 40; });
    await quiet(300);
    const bb = await csel().boundingBox();
    // ein Punkt im sichtbaren Clip-Körper neben den Markern (nicht auf einem
    // Beat, dort läge schon einer), von der Mitte aus gesucht
    const mid = await csel().evaluate((c) => { const r = c.getBoundingClientRect();
      for (const f of [0.47, 0.41, 0.53, 0.35, 0.59, 0.29, 0.65]) { const x = r.left + r.width * f, y = r.top + r.height / 2;
        const e = document.elementFromPoint(x, y); if (e && c.contains(e) && !e.closest('.clbtns, .cwarp, .cloopbar, .cunder')) return { x, y }; }
      return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; });
    await page.mouse.move(mid.x, mid.y); await quiet(300);
    const hoverBar = await csel().evaluate(c => { const b = c.querySelector('.clbtns') || c.nextElementSibling; return b ? getComputedStyle(b).display : 'none'; });
    FAKE.warpClip = 1;
    const k = sent.length;
    await page.mouse.dblclick(mid.x, mid.y); await quiet(900);
    delete FAKE.warpClip;
    const ops = sent.slice(k).map(x => x.op).filter(o => !['clipPeaks', 'state', 'treeState', 'routing'].includes(o));
    await page.mouse.click(mid.x, mid.y); await quiet(450);
    const selBar = await csel().evaluate(c => { const b = c.querySelector('.clbtns') || c.nextElementSibling; return b ? getComputedStyle(b).display : 'none'; });
    report('W', 'kurzer Audio-Clip groß gezoomt: keine Leiste beim Überfahren, Doppelklick = warpClip, Klick zeigt die Leiste',
           hoverBar === 'none' && ops.length && ops.every(o => o === 'warpClip') && selBar === 'flex',
           { breite: Math.round(bb.width), leisteHover: hoverBar, gesendet: ops, leisteNachKlick: selBar, toast: (await toast()).slice(0, 80) });
    await page.keyboard.press('Escape'); await quiet(200);
  }

  // ── R: Referenz + Analyse ohne Projekt (Todo 2026-10-30-ui-export-
  // kleinkram): laden landet in <config>/references/, Tempo und Analyze
  // laufen, Projekt-Knöpfe aus; das Projekt danach übernimmt die Referenz ──
  {
    await api('treeClose');
    await page.evaluate(() => { for (const k of Object.keys(localStorage)) if (k.startsWith('dawd.ref')) localStorage.removeItem(k); });
    await page.reload(); await quiet(1500);
    const view = await ev(() => ({ side: getComputedStyle(document.querySelector('#side')).display !== 'none'
                                         && getComputedStyle(document.querySelector('#side')).visibility === 'visible',
                                   seed: getComputedStyle(document.querySelector('#seedPanel')).display !== 'none',
                                   sideW: document.querySelector('#side').getBoundingClientRect().width,
                                   seedR: document.querySelector('#seedPanel').getBoundingClientRect().right,
                                   sideL: document.querySelector('#side').getBoundingClientRect().left }));
    await page.locator('#refFile').setInputFiles(path.join(media, 'tone.wav')); await quiet(1500);
    const ref = await ev(() => refFile());
    const btns = await ev(() => Object.fromEntries(['#refTempoBtn', '#refKeyBtn', '#refStructBtn', '#refCompare', '#refVoiceBtn', '#refMidiBtn']
                                                     .map(id => [id, document.querySelector(id).disabled])));
    report('R', 'ohne Projekt: Start-Panel und Referenz nebeneinander, Load nach <config>/references/, Projekt-Knöpfe aus',
           view.side && view.seed && view.sideW > 200 && view.seedR <= view.sideL + 1 && /\/config\/references\/tone\.wav$/.test(ref)
           && !btns['#refTempoBtn'] && !btns['#refKeyBtn'] && !btns['#refStructBtn'] && btns['#refCompare'] && btns['#refVoiceBtn'] && btns['#refMidiBtn'],
           { view, ref, disabled: btns });
    let k = sent.length;
    await page.locator('#refTempoBtn').click(); await quiet(2500);
    const tempo = since(k, 'detectTempo')[0];
    const tgt = await page.locator('#anaTarget').inputValue();
    const songOff = await ev(() => [...document.querySelector('#anaTarget').options].find(o => o.value === 'song').disabled);
    k = sent.length;
    await page.locator('#anaBtn').click(); await quiet(1500);
    const ana = since(k, 'analyze')[0];
    const card = await ev(() => { const c = document.querySelector('#aList > *'); return c ? c.textContent.slice(0, 80) : ''; });
    report('R', 'ohne Projekt: Tempo und Analyze (Ziel Referenz, Song gesperrt) laufen', tempo && tempo.file === ref && tempo.resultOk
           && tgt === 'reference' && songOff && ana && ana.file === ref && ana.resultOk && /LUFS|silent/.test(card),
           { tempo: tempo ? tempo.resultOk : 'kein detectTempo', ziel: tgt, songAus: songOff, analyze: ana ? ana.resultOk : 'kein analyze', card });
    need(await api('treeOpen', { dir: DIR }), 'treeOpen');
    await page.reload(); await quiet(1500);
    const after = await ev(() => ({ ref: refFile(), noproj: document.querySelector('#app').classList.contains('noproject'),
                                    compare: document.querySelector('#refCompare').disabled }));
    report('R', 'Projekt öffnen übernimmt die Referenz von vorher', after.ref === ref && !after.noproj && !after.compare, after);
  }

  report(0, 'keine JS-Fehler auf der Seite', pageErrors.length === 0, pageErrors.slice(0, 3).join(' | '));
  if (args.json) fs.writeFileSync(args.json, JSON.stringify(results, null, 1));
  await page.unrouteAll({ behavior: 'ignoreErrors' });
  await browser.close();
  await api('treeClose');
  const bad = results.filter(r => !r.ok).length;
  console.log(bad ? bad + ' Check(s) fehlen' : 'alle ' + results.length + ' Checks ok');
  process.exit(bad ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
