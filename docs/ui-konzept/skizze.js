// Statische Skizzen zum Konzept Dock/Referenz — gemeinsamer Inhalt und
// Klick-Verhalten. Kein dawd, keine echten Daten: Werte sind erfunden.
// Bedienelemente tragen data-ctl (Schlüssel für tools/ui_replay/skizze_mess.js).
'use strict';
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const h = (html) => { const t = document.createElement('template'); t.innerHTML = html.trim(); return t.content.firstElementChild; };

const TRACKS = [
  { name: 'Drums', type: 'm', chain: ['Drum Kit', 'EQ', 'Compressor'] },
  { name: 'Bass', type: 'm', chain: ['Surge XT', 'EQ', 'Drive'] },
  { name: 'Pad', type: 'm', chain: ['4OSC', 'EQ', 'Limiter'] },
  { name: 'Lead', type: 'm', chain: ['Surge XT', 'Chorus', 'Reverb'] },
  { name: 'Guitar', type: 'm', chain: ['Surge XT', 'EQ', 'Compressor', 'Limiter'] },
  { name: 'Vox', type: 'a', chain: ['EQ', 'Reverb'] },
];
const MASTER = { name: 'Master', chain: ['EQ', 'Bus Comp', 'Limiter'] };
const SECTIONS = [
  { name: 'Intro', start: 0, len: 13.3, col: '#2b6b4f' },
  { name: 'Verse', start: 13.3, len: 40, col: '#4a3f8f' },
  { name: 'Chorus', start: 53.3, len: 32, col: '#7a3f6f' },
];
// Teile der Referenz (aus „Structure“), Sekunden
const PARTS = [
  { name: 'Intro', start: 0, end: 13.31 }, { name: 'Verse 1', start: 13.31, end: 53.27 },
  { name: 'Chorus 1', start: 53.27, end: 79.94 }, { name: 'Verse 2', start: 79.94, end: 112.74 },
  { name: 'Chorus 2', start: 112.74, end: 127.52 }, { name: 'Outro', start: 127.52, end: 160.0 },
];
const REF_LEN = 160;
const PARAMS = ['Volume', 'A Osc 1 Type', 'A Osc 1 Pitch', 'A Osc 1 Shape', 'A Osc 1 Width', 'A Osc 2 Pitch', 'A Osc 2 Detune',
  'A Osc 3 Pitch', 'A Noise Color', 'A Ring Mod', 'A Filter 1 Type', 'A Filter 1 Cutoff', 'A Filter 1 Resonance',
  'A Filter 1 Env Amount', 'A Filter 2 Cutoff', 'A Highpass', 'A Waveshaper Drive', 'A Amp EG Attack', 'A Amp EG Decay',
  'A Amp EG Sustain', 'A Amp EG Release', 'A Filter EG Attack', 'A Filter EG Decay', 'A Filter EG Sustain',
  'A Filter EG Release', 'A LFO 1 Rate', 'A LFO 1 Depth', 'A Portamento', 'A Pitch Bend Range', 'A Unison Voices',
  'A Unison Detune', 'A Width', 'A Pan', 'FX A1 Mix', 'FX A1 Param 1', 'FX A1 Param 2', 'FX A1 Param 3', 'FX A2 Mix',
  'Global Volume', 'Polyphony'];
const RECIPES = [
  ['Saw lead: 2 saws unison + chorus, LPF 8.5 kHz', 'sound', 'checked'],
  ['Indie drums v9: snare −1 st, drums EQ 1 kHz −6.5', 'chain', 'draft'],
  ['E-bass with drive, high shelf 420 Hz −13', 'sound', 'checked'],
  ['String ensemble pad: 2×4 unison, pad EQ, limiter', 'sound', 'draft'],
  ['Crunch guitar: distorted, wide, 2 EQs + comp', 'chain', 'draft'],
  ['Mix target: bus comp + limiter, short release, −6.9 LUFS', 'mixTarget', 'draft'],
  ['Vocal chops: warp to grid, slice per beat', 'workflow', 'checked'],
  ['Sub bass: sine + saw, mono below 120 Hz', 'sound', 'draft'],
];

const S = { track: 1, slot: null, source: 'Bass', part: 4, start: 112.74, end: 127.52, bpm: 130, down: 112.74, n: 0 };

function topbar (note) {
  $('#topbar').innerHTML = `
    <div class="brand"><i></i><span>dawd</span></div>
    <button class="ghost">▤</button><button class="ghost">📁 demo-song</button>
    <button class="play" data-ctl="top.play">▶</button>
    <button class="ghost">↶</button><span class="time">0:00.0</span>
    <span class="dim mono" data-ctl="top.lufs" style="cursor:pointer" title="Click: analyze the song">I −9.8 LUFS</span>
    <span class="sketchnote">${note}</span>
    <span class="spacer"></span>
    <div class="seg"><button>Collect</button><button>Select</button><button class="on">Process</button></div>
    <button class="ghost" title="Export">⇪</button>`;
}

function tree () {
  const nodes = [['Reference analysis', 'process', 'Tempo 130, key A minor, chorus 2 at 112.74 s'], ['Drums: notes', 'collect', ''],
    ['Bass: notes', 'collect', 'Root notes bar 1–8'], ['Bass: sound', 'process', 'Surge XT preset, EQ, drive'],
    ['Pad: notes', 'collect', ''], ['Pad: sound', 'process', ''], ['Lead: notes', 'collect', ''],
    ['Lead: sound A', 'process', 'Recipe saw lead'], ['Lead: sound B', 'process', ''], ['Song: mix', 'process', 'Bus comp + limiter']];
  $('#tree').innerHTML = '<h2>SONG TREE</h2>' + nodes.map((n, i) =>
    `<div class="node${i === 3 ? ' cur' : ''}">${n[0]}<span class="tag">${n[1]}</span>${n[2] ? `<div class="note">${n[2]}</div>` : ''}</div>`).join('');
}

function timeline () {
  const pps = 22;
  let html = `<div class="ltrow ruler"><div class="ltlabel"><button class="ghost" style="width:24px;padding:0">−</button><button class="ghost" style="width:24px;padding:0">+</button><span class="dim" style="margin-left:auto">6 tracks · 21 clips</span></div><div class="ltlane">${
    Array.from({ length: 10 }, (_, i) => `<span style="left:${i * 10 * pps + 4}px">0:${String(i * 10).padStart(2, '0')}</span>`).join('')}</div></div>`;
  html += `<div class="ltrow secrow"><div class="ltlabel">Sections</div><div class="ltlane">${SECTIONS.map((s, i) =>
    `<div class="sec${i === 2 ? ' sel' : ''}" data-ctl="tl.section" style="left:${s.start * pps}px;width:${s.len * pps - 4}px;background:${s.col}">${s.name} <span class="dim">130 · 4/4</span></div>`).join('')}</div></div>`;
  TRACKS.forEach((t, i) => {
    let clips = '';
    for (let x = 0; x < 85; x += 13.3) clips += `<div class="clip ${t.type}" style="left:${x * pps}px;width:${13.3 * pps - 4}px">${t.name} ${Math.round(x / 13.3) + 1}</div>`;
    html += `<div class="ltrow trk${i === S.track ? ' sel' : ''}" data-track="${i}"><div class="ltlabel"><span class="dot" style="background:var(--${t.type === 'a' ? 'audio' : 'midi'})"></span><span class="tname" data-ctl="tl.track">${t.name}</span><span class="smb"><button>S</button><button>M</button><button>A</button></span></div><div class="ltlane">${clips}</div></div>`;
  });
  $('#timeline').innerHTML = html;
  $$('#timeline .trk .ltlabel').forEach(l => l.onclick = () => selectTrack(+l.parentElement.dataset.track));
}

function selectTrack (i) {
  S.track = i;
  $$('#timeline .trk').forEach(r => r.classList.toggle('sel', +r.dataset.track === i));
  $$('.chain').forEach(c => c.classList.toggle('sel', +c.dataset.track === i));
  $$('.tgtname').forEach(e => e.textContent = i < 0 ? 'Master' : TRACKS[i].name);
  toast('Selected ' + (i < 0 ? 'Master' : TRACKS[i].name) + ' — Voice, Drums, Match and Tune use this track');
}

// ── Routing: eine Zeile je Kette, Plugin-Karte als Popover oder daneben ──
function routing (cardMode) {
  const row = (t, i) => `<div class="chain${i === S.track ? ' sel' : ''}" data-track="${i}"><span class="cname">${t.name}</span>${
    t.chain.map((p, j) => `<span class="arrow">›</span><button class="slot" data-slot="${i}:${j}"${i === 1 && j === 0 ? ' data-ctl="route.chain"' : ''}>${p}</button>`).join('')
  }<span class="arrow">›</span><button class="add"${i === 1 ? ' data-ctl="route.add"' : ''} title="Add a plugin at the end">+</button></div>`;
  const html = `<div class="chains">${TRACKS.map(row).join('')}${row(MASTER, -1)}</div>`;
  $$('[data-slot-routing]').forEach(host => {
    host.innerHTML = cardMode === 'inline'
      ? `<div style="display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.15fr);gap:.7rem;height:100%;min-height:0">${html}<div data-cardhost style="min-height:0;display:flex"></div></div>`
      : html;
    $$('.slot', host).forEach(b => b.onclick = () => openCard(b, cardMode));
  });
  if (cardMode === 'inline') openCard($('[data-slot="1:0"]'), cardMode);
  if (cardMode === 'stack') {
    // heute: alle offenen Karten untereinander unter allen Ketten
    $$('[data-slot-routing]').forEach(host => host.insertAdjacentHTML('beforeend',
      [['Drum Kit', 'Drums'], ['Surge XT', 'Bass'], ['EQ', 'Bass']].map(([n, t]) => `<div style="margin-top:.6rem;display:flex">${cardHtml(n, t)}</div>`).join('')));
    const cards = $$('[data-slot-routing] .card');
    cards.forEach((c, i) => { if (i !== 1) $$('[data-ctl]', c).forEach(e => e.removeAttribute('data-ctl')); $('.params', c).style.overflow = 'visible'; });
  }
}

function cardHtml (name, track) {
  return `<div class="card" style="flex:1"><h4>${name} · ${track}<span class="dim">40 of 312 parameters</span><button class="ghost" style="margin-left:auto" onclick="closeCard(this)">×</button></h4>
    <div class="cardbar"><input data-ctl="route.preset" placeholder="Presets (bass, bright …)" style="width:12rem"><button>Load</button>
      <input class="psearch" placeholder="Find parameter" style="width:11rem" oninput="filterParams(this)"></div>
    <div class="params">${PARAMS.map((p, i) => `<label class="param"${i === 11 ? ' data-ctl="route.card"' : i === 0 ? ' data-ctl="route.card0"' : ''}><span>${p}</span><input type="range" min="0" max="1" step="0.001" value="${((i * 37) % 100) / 100}"><input type="text" value="${((i * 37) % 100) / 100}"></label>`).join('')}</div></div>`;
}

function openCard (btn, mode) {
  if (mode === 'stack') return;
  $$('.slot.open').forEach(b => b.classList.remove('open'));
  btn.classList.add('open');
  const [ti, j] = btn.dataset.slot.split(':').map(Number);
  const t = ti < 0 ? MASTER : TRACKS[ti];
  const html = cardHtml(t.chain[j], t.name);
  if (mode === 'inline') {
    const host = btn.closest('[data-slot-routing]').querySelector('[data-cardhost]');
    host.innerHTML = html;
  } else {
    let pop = $('#popcard');
    if (!pop) { pop = h('<div id="popcard" class="popcard"></div>'); document.body.appendChild(pop); }
    pop.innerHTML = html;
    pop.firstElementChild.style.height = '100%';
    // über dem Dock (die Reiterleiste bleibt frei), also über der Timeline
    const r = btn.getBoundingClientRect(), top = $('#dock').getBoundingClientRect().top;
    const H = Math.min(420, top - 70);
    pop.style.height = H + 'px';
    pop.style.top = (top - H - 6) + 'px';
    pop.style.left = Math.min(innerWidth - 470, Math.max(8, r.left)) + 'px';
    pop.style.display = 'block';
  }
}
function closeCard (b) { const pop = b.closest('#popcard'); if (pop) pop.style.display = 'none'; else b.closest('.card').remove(); $$('.slot.open').forEach(x => x.classList.remove('open')); }
function filterParams (inp) {
  const q = inp.value.toLowerCase();
  $$('.param', inp.closest('.card')).forEach(l => { const hit = q && l.textContent.toLowerCase().includes(q); l.style.display = !q || hit ? '' : 'none'; l.classList.toggle('hit', !!hit); });
}

function mixer () {
  const strip = (t, i) => `<div class="strip"><b>${t.name}</b><span class="val">${(-6 - i).toFixed(1)} dB</span><input type="range" class="fader"${i === 2 ? ' data-ctl="route.mixer"' : ''} min="-60" max="6" value="${-6 - i}"><input type="range" min="-1" max="1" step="0.01" value="0" style="width:60px"><div style="display:flex;gap:2px"><button style="width:24px;padding:0;justify-content:center">S</button><button style="width:24px;padding:0;justify-content:center">M</button></div></div>`;
  $$('[data-slot-mixer]').forEach(e => e.innerHTML = `<div class="mixer">${TRACKS.map(strip).join('')}${strip(MASTER, -1)}</div>`);
}

function recipes () {
  $$('[data-slot-recipes]').forEach(e => e.innerHTML = `<div class="recbar"><input data-ctl="recipe.search" type="search" placeholder="Search recipes — title, goal, tags, plugins" style="width:18rem"><select><option>any kind</option></select><select><option>not rejected</option></select><button>Save…</button><span class="dim">8 recipes</span></div>
    ${RECIPES.map((r, i) => `<div class="recipe"><b>${r[0]}</b><span class="tagx">${r[1]}</span><span class="tagx">${r[2]}</span><span class="spacer"></span><button${i === 2 ? ' data-ctl="recipe.apply"' : ''} onclick="toast('Recipe applied to <b>'+(S.track<0?'Master':TRACKS[S.track].name)+'</b> in a new node')">Apply to <span class="tgtname">${TRACKS[S.track].name}</span></button></div>`).join('')}`);
}

function notes () {
  $$('[data-slot-notes]').forEach(e => {
    let n = ''; for (let i = 0; i < 18; i++) n += `<i style="left:${i * 40 + 2}px;top:${12 * (8 + ((i * 5) % 7))}px;width:${36 + (i % 3) * 40}px"></i>`;
    e.innerHTML = `<div style="display:flex;gap:.4rem;margin-bottom:.4rem;align-items:center"><b>Bass 3</b><span class="dim">click: add · drag: move · range 0–127, scroll for more</span><span class="spacer"></span><button class="primary">Done</button></div><div class="roll" data-ctl="notes.roll">${n}</div>`;
  });
}

// ── Referenz: Quelle, Bereich, Knöpfe ──
const BTN = {
  song: [['Compare', 'Compare the mix (selected section) with the reference range'], ['Tempo'], ['Key'], ['Structure']],
  track: [['Voice', 'Compare the selected track with the reference range'], ['Drums'], ['Match sound'], ['Tune']],
  describe: [['Describe voice', 'Notes, articulation, glide, register — of the reference range or of the selected track in the selected section'], ['Describe timbre', 'Brightness, detune, envelope'], ['Describe drums', 'Kick, snare, hats per bar and their sound']],
  part: [['Repeats'], ['Layers'], ['Classes'], ['Cross-check'], ['Split']],
  make: [['To MIDI', 'Transcribe the reference range into a MIDI clip in the selected section'], ['Notes only', 'Transcribe the reference range and show the notes in the analysis panel — no clip'], ['Stems'], ['Compare stems', 'Each of your tracks against a stem of the reference, in the reference range']],
};
const DESC_OF = () => `<span class="srcseg" title="What to describe"><button class="on" onclick="descOf(this)">Reference</button><button onclick="descOf(this)" class="tgtname">${TRACKS[S.track].name}</button></span>`;
function descOf (btn) { event.stopPropagation(); $$('button', btn.parentElement).forEach(x => x.classList.toggle('on', x === btn)); }
const b = (n, cls = '') => `<button class="${cls}" data-ctl="ref:${n[0]}" title="${n[1] || n[0]}" onclick="runRef('${n[0]}')">${n[0]}</button>`;

function sourceRow (lbl = true) {
  return `<div class="refrow">${lbl ? '<span class="lbl">Source</span>' : ''}<select data-ctl="ref.file" style="max-width:11rem"><optgroup label="Reference"><option>reference.wav</option></optgroup><optgroup label="Layers of Chorus 2 (from Layers)"><option>synth.wav</option><option>guitar.wav</option><option>keys.wav</option></optgroup><optgroup label="Renders"><option>render-node-13.wav</option></optgroup><option>Load file…</option><option>Capture…</option></select>
    <span class="srcseg" data-ctl="ref.source">${['Mix', 'Vocals', 'Bass', 'Drums', 'Other'].map(s => `<button class="${s === S.source ? 'on' : ''}" onclick="setSource('${s}')">${s}</button>`).join('')}</span></div>`;
}
function rangeRow (lbl = true) {
  return `<div class="refrow">${lbl ? '<span class="lbl">Range</span>' : ''}${PARTS.map((p, i) => `<button class="part${i === S.part ? ' on' : ''}"${i === 4 ? ' data-ctl="ref.part"' : ''} onclick="setPart(${i})">${p.name}</button>`).join('')}<button class="part" onclick="setPart(-1)">Whole</button></div>
    <div class="refrow">${lbl ? '<span class="lbl"></span>' : ''}<input class="rngfld" data-ctl="ref.range" data-k="start" value="${S.start}" onchange="setField(this)"><span class="dim">–</span><input class="rngfld" data-k="end" value="${S.end}" onchange="setField(this)"><span class="dim">s</span>
      <input class="rngfld" data-k="bpm" value="${S.bpm}" style="width:3.6rem" onchange="setField(this)"><span class="dim">BPM · 1 at</span><input class="rngfld" data-k="down" value="${S.down}" onchange="setField(this)">
      <button class="ghost" title="Set the range from the selected section of the song">From section</button></div>`;
}
function chip () { return `<span class="rangechip" data-ctl="ref.chip" title="Reference range — every reference button uses it">Ref <b class="chipsrc">${S.source}</b> <span class="mono chiprng">${S.start.toFixed(2)}–${S.end.toFixed(2)} s · ${S.bpm} BPM</span></span>`; }
function menu (label, items, ctl, up) {
  return `<span class="menuwrap"><button data-ctl="${ctl}" onclick="this.nextElementSibling.classList.toggle('open')">${label} ▾</button><span class="menu${up ? ' up' : ''}" data-menu="${ctl}">${items}</span></span>`;
}

function refPanel (layout) {
  const host = $('[data-slot-ref]');
  const grp = (k, label, cls) => `<span class="btngrp"><span class="gl">${label}</span>${BTN[k].map((n, i) => b(n, i === 0 && cls ? cls : '')).join('')}</span>`;
  if (layout === 'wide') {
    host.innerHTML = `<div class="refpanel">${sourceRow()}${rangeRow()}
      <div class="refrow"><span class="lbl">Song</span>${grp('song', '', 'primary')}<span class="lbl" style="margin-left:.5rem;width:auto">Track <b class="tgtname" style="color:var(--text)">${TRACKS[S.track].name}</b></span>${grp('track', '')}</div>
      <div class="refrow"><span class="lbl">Describe</span>${DESC_OF()}${grp('describe', '')}</div>
      <div class="refrow"><span class="lbl">Part</span>${grp('part', '')}</div>
      <div class="refrow"><span class="lbl">Make</span><select style="width:6.5rem"><option>melody</option><option>bass</option><option>all notes</option><option>chords</option></select>${grp('make', '')}</div></div>`;
  } else if (layout === 'side') {
    host.innerHTML = `<div class="refpanel">
      <div class="refrow" style="justify-content:space-between"><b style="font-size:11px;letter-spacing:.12em;color:var(--warn)">REFERENCE</b>${chip()}</div>
      ${sourceRow(false)}${rangeRow(false)}
      <div class="refrow">${b(BTN.song[0], 'primary')}${BTN.song.slice(1).map(n => b(n)).join('')}</div>
      <div class="refrow"><span class="dim">Track <b class="tgtname">${TRACKS[S.track].name}</b>:</span>${BTN.track.map(n => b(n)).join('')}</div>
      <div class="refrow">${menu('Describe', '<span class="mh">Describe</span>' + DESC_OF() + BTN.describe.map(n => b(n)).join(''), 'ref.describe')}
        ${menu('Part', BTN.part.map(n => b(n)).join(''), 'ref.partmenu')}${menu('Make', BTN.make.map(n => b(n)).join(''), 'ref.make')}</div></div>`;
  } else if (layout === 'lane') {
    host.innerHTML = `<div class="ltlabel" style="position:static;height:auto;flex-direction:column;align-items:stretch;gap:4px;padding:.35rem .5rem">
        <div style="display:flex;gap:.3rem;align-items:center"><b style="font-size:11px;letter-spacing:.12em;color:var(--warn)">REFERENCE</b><select data-ctl="ref.file" style="flex:1;min-width:0"><optgroup label="Reference"><option>reference.wav</option></optgroup><optgroup label="Layers of Chorus 2 (from Layers)"><option>synth.wav</option><option>guitar.wav</option><option>keys.wav</option></optgroup><optgroup label="Renders"><option>render-node-13.wav</option></optgroup><option>Load file…</option><option>Capture…</option></select></div>
        <span class="srcseg" data-ctl="ref.source" style="align-self:flex-start">${['Mix', 'Vox', 'Bass', 'Drm', 'Oth'].map((s, i) => `<button class="${['Mix', 'Vocals', 'Bass', 'Drums', 'Other'][i] === S.source ? 'on' : ''}" style="padding:0 .4rem" onclick="setSource('${['Mix', 'Vocals', 'Bass', 'Drums', 'Other'][i]}')">${s}</button>`).join('')}</span></div>
      <div style="flex:1;min-width:0;display:flex;flex-direction:column;gap:4px;padding:4px .6rem">
        <div class="refrow">${chip()}${b(BTN.song[0], 'primary')}${b(BTN.track[0])}${b(BTN.track[1])}<span class="dim">→ <b class="tgtname">${TRACKS[S.track].name}</b></span>
          ${menu('Detect', BTN.song.slice(1).map(n => b(n)).join(''), 'ref.detect')}${menu('Sound', b(BTN.track[2]) + b(BTN.track[3]), 'ref.sound')}
          ${menu('Describe', '<span class="mh">Describe</span>' + DESC_OF() + BTN.describe.map(n => b(n)).join(''), 'ref.describe')}
          ${menu('Part', BTN.part.map(n => b(n)).join(''), 'ref.partmenu')}${menu('Make', BTN.make.map(n => b(n)).join(''), 'ref.make')}</div>
        <div class="wave" data-ctl="ref.part" title="Drag to set the reference range · click a part to take it" style="position:relative;height:40px;border-radius:6px;background:var(--bg);border:1px solid var(--line);cursor:crosshair;overflow:hidden">${waveSvg()}</div></div>`;
    lane();
  }
  syncRange();
}

function waveSvg () {
  let p = ''; for (let i = 0; i < 400; i++) { const a = 4 + 14 * Math.abs(Math.sin(i * 0.37) * Math.cos(i * 0.051)) * (i > 210 && i < 320 ? 1.3 : 0.8); p += `M${i / 4} ${20 - a}V${20 + a}`; }
  const parts = PARTS.map((q, i) => `<div style="position:absolute;top:0;bottom:0;left:${q.start / REF_LEN * 100}%;width:${(q.end - q.start) / REF_LEN * 100}%;border-left:1px solid var(--line-2);font-size:10px;color:var(--muted);padding:1px 3px;pointer-events:none">${q.name}</div>`).join('');
  return `<svg viewBox="0 0 100 40" preserveAspectRatio="none" style="position:absolute;inset:0;width:100%;height:100%"><path d="${p}" stroke="#38d4f0" stroke-opacity=".55" stroke-width=".18"/></svg>${parts}<div class="rngshade" style="position:absolute;top:0;bottom:0;background:color-mix(in oklab,var(--ok) 22%,transparent);border:1px solid var(--ok);pointer-events:none"></div>`;
}
function lane () {
  const w = $('.wave'); let x0 = null;
  const t = (e) => Math.max(0, Math.min(REF_LEN, (e.clientX - w.getBoundingClientRect().left) / w.clientWidth * REF_LEN));
  w.onmousedown = (e) => { x0 = t(e); };
  w.onmousemove = (e) => { if (x0 === null) return; const x1 = t(e); S.start = +Math.min(x0, x1).toFixed(2); S.end = +Math.max(x0, x1).toFixed(2); S.part = -2; syncRange(); };
  w.onmouseup = (e) => { if (x0 !== null && Math.abs(t(e) - x0) < 0.5) { const i = PARTS.findIndex(p => x0 >= p.start && x0 < p.end); setPart(i); } x0 = null; };
}

function setSource (s) { S.source = s; $$('.srcseg button').forEach(x => x.classList.toggle('on', x.textContent === s || ({ Vox: 'Vocals', Drm: 'Drums', Oth: 'Other' })[x.textContent] === s)); syncRange(); toast('Reference source: ' + (s === 'Mix' ? 'the whole mix' : 'the ' + s.toLowerCase() + ' stem of the reference (no upload)')); }
function setPart (i) { S.part = i; if (i === -1) { S.start = 0; S.end = REF_LEN; } else if (i >= 0) { S.start = PARTS[i].start; S.end = PARTS[i].end; S.down = PARTS[i].start; } syncRange(); }
function setField (inp) { S[inp.dataset.k] = +inp.value; S.part = -2; syncRange(); }
function syncRange () {
  $$('.part').forEach((p, i) => p.classList.toggle('on', i === S.part || (S.part === -1 && p.textContent === 'Whole')));
  $$('.rngfld').forEach(f => { const v = S[f.dataset.k]; if (document.activeElement !== f) f.value = v; });
  $$('.chipsrc').forEach(e => e.textContent = S.source);
  $$('.chiprng').forEach(e => e.textContent = `${S.start.toFixed(2)}–${S.end.toFixed(2)} s · ${S.bpm} BPM`);
  $$('.rngshade').forEach(e => { e.style.left = (S.start / REF_LEN * 100) + '%'; e.style.width = ((S.end - S.start) / REF_LEN * 100) + '%'; });
}

// ── Analyse-Panel: Ergebnisse bleiben stehen (statt Toast 3,2 s) ──
const RESULTS = {
  Compare: ['Compare · Chorus vs reference', [['Loudness', '−9.8 vs −7.1 LUFS'], ['True peak', '−0.4 vs −0.1 dB'], ['Low (20–120 Hz)', '+2.1 dB'], ['Highs (6–16 kHz)', '−3.4 dB']], 'Highs are 3.4 dB under the reference.'],
  Voice: ['Voice · Bass vs reference', [['Articulation', 'legato vs detached'], ['Register', 'A1–E2 vs A1–A2'], ['Brightness', '−18 %'], ['Rhythm', '8ths, 2 notes missing in bar 3']], 'Shorter notes: lower Amp EG Release.'],
  Drums: ['Drums · Drums vs reference', [['Kick', 'missing on 3 (bars 2, 4)'], ['Snare body', '190 vs 210 Hz'], ['Hats', 'open on 2+ in the reference']], ''],
  'Describe voice': ['Describe voice · reference', [['Notes', 'A1 C2 E2 G2 …'], ['Articulation', 'detached, 80 % gate'], ['Glide', 'none']], ''],
  'Describe timbre': ['Describe timbre · reference', [['Brightness', '1.9 kHz'], ['Detune', '12 cents (unison)'], ['Envelope', 'fast attack, 280 ms release']], ''],
  'Describe drums': ['Describe drums · reference', [['Kick', 'X . . . x . X . | 1 bar'], ['Snare', '2 and 4, ghost on 4e'], ['Kick pitch', '54 Hz']], ''],
  Analyze: ['Analyze · Bass in Chorus', [['Integrated', '−18.2 LUFS'], ['True peak', '−4.1 dB'], ['LRA', '3.2 LU']], ''],
};
function runRef (name) {
  $$('.menu.open').forEach(m => m.classList.remove('open'));
  if (name === 'Compare stems') { $('#stemsDlg').classList.add('open'); return; }
  const r = RESULTS[name] || [name + ' · reference', [['Range', `${S.start.toFixed(2)}–${S.end.toFixed(2)} s`], ['Source', S.source]], ''];
  addResult(r[0].replace('Bass', TRACKS[S.track]?.name || 'Master'), r[1], r[2], name === 'Analyze' ? 'track ' + (TRACKS[S.track]?.name || 'Master') + ' · section Chorus' : '');
  toast(r[0] + ' — result in the analysis panel');
}
function addResult (title, kv, hint, meta) {
  S.n++;
  $$('.alist').forEach(list => {
    $$('.res.new', list).forEach(x => x.classList.remove('new'));
    list.prepend(h(`<div class="res new" data-ctl="analysis.result"><div class="rh">${title}<span class="dim">#${S.n} · ${meta || 'ref ' + S.source + ' ' + S.start.toFixed(2) + '–' + S.end.toFixed(2)}</span></div>
      <div class="kv">${kv.map(([k, v]) => `<span>${k}</span><span>${v}</span>`).join('')}</div>${hint ? `<div class="hint">${hint}</div>` : ''}</div>`));
  });
}
function analysisPanel () {
  $$('[data-slot-analysis]').forEach(e => {
    e.innerHTML = `<div class="apanel" style="height:100%"><div class="ahead"><h3>Analysis</h3><span class="spacer"></span>
      <select data-ctl="analysis.scope" title="What to analyze"><option>Selected track</option><option>Song</option><option>Reference source</option><option>File…</option></select>
      <select title="Which part"><option>Selected section</option><option>Whole</option><option>Ruler selection</option><option>Reference range</option></select>
      <button data-ctl="analysis.analyze" onclick="runRef('Analyze')">Analyze</button></div><div class="alist"></div></div>`;
  });
  addResult('Analyze · Song', [['Integrated', '−9.8 LUFS'], ['True peak', '−0.4 dB']], '', 'song · whole');
  addResult('Tempo · reference', [['Tempo', '130.0 BPM, 4/4'], ['First downbeat', '0.42 s']], '', 'ref Mix · whole');
  addResult('Voice · Bass vs reference', RESULTS.Voice[1], RESULTS.Voice[2]);
  $$('.res.new').forEach(x => x.classList.remove('new'));
  $$('.alist .res').forEach((x, i) => { if (i) x.removeAttribute('data-ctl'); });
}

function stemsDialog () {
  document.body.appendChild(h(`<div class="dlg" id="stemsDlg"><div><h3>Compare stems</h3><p class="dim">Each of your tracks against a stem of the reference, in the reference range (${'${range}'}). Pairs are guessed from the names.</p>
    ${[['Vox', 'Vocals'], ['Bass', 'Bass'], ['Drums', 'Drums'], ['Pad', 'Other']].map(p => `<div class="pair"><select><option>${p[0]}</option></select><span>↔</span><select><option>${p[1]} stem</option></select></div>`).join('')}
    <div style="display:flex;gap:.4rem;justify-content:flex-end;margin-top:.6rem"><button onclick="this.closest('.dlg').classList.remove('open')">Cancel</button><button class="primary" onclick="this.closest('.dlg').classList.remove('open');addResult('Compare stems · 4 pairs',[['Vox ↔ vocals','level −2.1 dB, brighter'],['Bass ↔ bass','register ok, legato vs detached'],['Drums ↔ drums','kick missing on 3'],['Pad ↔ other','−4 dB, darker']],'');toast('Compare stems — result in the analysis panel')">Compare</button></div></div></div>`.replace('${range}', 'Chorus 2, 112.74–127.52 s')));
}

let toastTimer;
function toast (msg) { const t = $('#toast'); t.innerHTML = msg; t.classList.remove('hide'); clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.add('hide'), 3200); }

function tabs () {
  $$('.tabs').forEach(bar => $$('button.tab', bar).forEach(btn => btn.onclick = () => {
    const dock = bar.parentElement;
    $$('button.tab', bar).forEach(x => x.classList.toggle('on', x === btn));
    $$(':scope > .tabpane, :scope > div > .tabpane', dock).forEach(p => p.classList.toggle('on', p.dataset.tab === btn.dataset.tabbtn));
    const pop = $('#popcard'); if (pop) pop.style.display = 'none';
  }));
}

function build ({ note, ref, card }) {
  document.body.appendChild(h('<div id="toast" class="hide">Ready</div>'));
  topbar(note); tree(); timeline(); $$('[data-slot-chip]').forEach(e => e.innerHTML = chip()); routing(card); mixer(); recipes(); notes(); refPanel(ref); analysisPanel(); stemsDialog(); tabs();
  document.addEventListener('click', (e) => { if (!e.target.closest('.menuwrap')) $$('.menu.open').forEach(m => m.classList.remove('open')); });
  // Für Screenshots: ?toast=1 zeigt einen Toast, ?card=1 öffnet die Karte (Popover), ?tab=X wählt einen Reiter
  const q = new URLSearchParams(location.search);
  if (q.get('tab')) { const t = $(`button.tab[data-tabbtn="${q.get('tab')}"]`); if (t) t.click(); }
  if (q.get('card') && card !== 'inline') openCard($('[data-slot="1:0"]'), card);
  if (q.get('toast')) { toast('"Saw lead" applied in node #13'); clearTimeout(toastTimer); }
}
