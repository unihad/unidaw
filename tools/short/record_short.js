#!/usr/bin/env node
// Short ohne Mensch aufnehmen (docs/shorts.md, Decision
// 2026-11-23-shorts-statt-demo-video): eigene unidaw-Instanz, ein Agent
// (`claude -p`) führt den Prompt des Drehbuchs über MCP aus, die Flow-UI wird
// dabei im Headless-Browser fotografiert, danach Play; ffmpeg setzt daraus
// das Hochformat 1080x1920 mit Titel, Prompt-Karte, UI und Endkarte, Ton aus
// dem Render des Agenten.
//
//   NODE_PATH=~/dev/agent-game/suite/node_modules node tools/short/record_short.js <short> [--fresh] [--out <mp4>]
//   node tools/short/record_short.js <short> --compose-only     # nur neu schneiden, kein Agent
//
// <short>: Name einer Datei tools/short/<short>.json (Drehbuch als Daten).
// Arbeitsordner: ~/.cache/unidaw-shorts/<short>/ (Bilder, Log, agent.json,
// manifest.json). Ausgabe: --out, sonst
// ~/StorageBox/agent-daw/agent-daw-short-<short>.mp4 (fehlt der Ordner: nur
// im Arbeitsordner). --fresh benennt einen schon vorhandenen Projektordner
// um (<ordner>.prev-<zeit>), sonst bricht das Skript ab — der Agent braucht
// einen neuen Ordner.
// Umgebung: UNIDAW_BIN (Binary oder AppImage; Standard: Build dieses Checkouts), CLAUDE_BIN
// (Standard: claude im PATH), SHORT_MODEL (Modell für claude -p, optional).

const fs = require('fs');
const os = require('os');
const net = require('net');
const path = require('path');
const crypto = require('crypto');
const { spawn, spawnSync } = require('child_process');

const REPO = path.resolve(__dirname, '../..');
const HOME = os.homedir();
const W = 1080, H = 1920;
// Bildaufbau (docs/shorts.md): Titel, Prompt-Karte, UI, Fußzeile
const TITLE_H = 200, CARD_H = 544, FOOT_H = 120;
const UI_Y = TITLE_H + CARD_H, UI_H = H - UI_Y - FOOT_H;
// UI im Browser: 960x939 CSS-Pixel, Faktor 1,125 → genau der UI-Bereich;
// ab 900 px Breite steht der Song-Baum als Spalte neben der Timeline
const VIEW = { width: 960, height: 939, scale: 1.125 };
const BG = '#0d0f14';

const args = process.argv.slice(2);
const name = args.find((a) => !a.startsWith('--') && args[args.indexOf(a) - 1] !== '--out');
const flag = (f) => args.includes(f);
const outAt = args.indexOf('--out');
if (!name) {
  console.error('usage: node tools/short/record_short.js <short> [--fresh] [--out <mp4>] [--compose-only]');
  process.exit(2);
}
const tilde = (p) => p.replace(/^~(?=\/|$)/, HOME);
const shot = JSON.parse(fs.readFileSync(path.join(__dirname, name + '.json'), 'utf8'));
const work = path.join(HOME, '.cache', 'unidaw-shorts', shot.id);
const out = outAt >= 0 ? path.resolve(args[outAt + 1]) : path.join(HOME, 'StorageBox', 'agent-daw', `agent-daw-short-${shot.id}.mp4`);
const log = (m) => console.error(`[short ${shot.id}] ${m}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// Abbruch: wirft, damit finally die Instanz beendet; Ausgabe oben im catch
class Fail extends Error {}
function die (m) { throw new Fail(m); }

function loadPlaywright () {
  const tries = [undefined, path.join(HOME, 'dev', 'agent-game', 'suite', 'node_modules')];
  for (const p of tries) {
    try { return require(p ? require.resolve('playwright', { paths: [p] }) : 'playwright'); } catch (e) { /* weiter */ }
  }
  die('playwright nicht gefunden (NODE_PATH auf ein node_modules mit playwright setzen)');
}

function freePort (port) {
  return new Promise((resolve) => {
    const s = net.createServer();
    s.once('error', () => resolve(false));
    s.listen(port, '127.0.0.1', () => s.close(() => resolve(true)));
  });
}

// ── Aufnahme ─────────────────────────────────────────────────────────────

async function record () {
  const project = tilde(shot.project);
  // Render des Agenten (der Prompt nennt ihn); ohne: die Kette rendert selbst
  const render = shot.render ? tilde(shot.render) : null;
  if (fs.existsSync(project)) {
    if (!flag('--fresh')) die(`${project} gibt es schon — mit --fresh wird er umbenannt`);
    const prev = `${project}.prev-${new Date().toISOString().replace(/[:.]/g, '-')}`;
    fs.renameSync(project, prev);
    log(`alter Projektordner → ${prev}`);
  }
  fs.mkdirSync(path.dirname(project), { recursive: true });
  const bin = process.env.UNIDAW_BIN || path.join(REPO, 'dawd/build/unidaw_artefacts/Release/unidaw');
  // Ausgangsprojekt: Kopie des Basis-Songs (ohne Renders), dazu Eingabe-Dateien
  let startNode = null;
  if (shot.base) {
    const base = tilde(shot.base);
    if (!fs.existsSync(path.join(base, 'tree.json'))) die(`Basis-Song ${base} fehlt — erst Short 1 aufnehmen`);
    fs.cpSync(base, project, { recursive: true, filter: (f) => !/\.wav$/i.test(f) });
    startNode = JSON.parse(fs.readFileSync(path.join(project, 'tree.json'), 'utf8')).current;
    log(`Basis-Song ${base} → ${project} (Node ${startNode})`);
  }
  for (const inp of shot.inputs || []) {
    fs.mkdirSync(project, { recursive: true });
    const dest = path.join(project, inp.file);
    if (inp.make === 'melody') {
      const r = spawnSync('python3', [path.join(__dirname, 'make_melody.py'), dest], { encoding: 'utf8' });
      if (r.status !== 0) die(`make_melody.py: ${r.stderr}`);
    } else if (inp.copy) {
      if (!fs.existsSync(tilde(inp.copy))) die(`Eingabe ${inp.copy} fehlt`);
      fs.copyFileSync(tilde(inp.copy), dest);
    } else die(`Eingabe ${inp.file}: make oder copy fehlt`);
    log(`Eingabe ${dest}`);
  }
  fs.rmSync(work, { recursive: true, force: true });
  for (const d of ['config', 'recipes', 'frames']) fs.mkdirSync(path.join(work, d), { recursive: true });

  let port = 7490;
  while (!(await freePort(port)) || !(await freePort(port + 1000))) port += 3;
  const token = crypto.randomBytes(16).toString('hex');
  const env = { ...process.env, UNIDAW_TOKEN: token, DISPLAY: '', WAYLAND_DISPLAY: '' };
  // Das AppImage startet ohne Unterbefehl den Starter mit Browser; nur der Daemon: "unidaw"
  const sub = /\.appimage$/i.test(bin) ? ['unidaw'] : [];
  const daw = spawn(bin, [...sub, '--port', String(port), '--config', 'config', '--recipes', 'recipes', '--log', 'events.jsonl',
    '--session', 'session.json', '--no-audio', '--interface-sim'],
  { cwd: work, env, stdio: ['ignore', fs.openSync(path.join(work, 'unidaw.log'), 'w'), fs.openSync(path.join(work, 'unidaw.err'), 'w')] });
  const url = `http://127.0.0.1:${port}`;
  const api = async (op, p = {}) => (await fetch(url + '/command', { method: 'POST',
    headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + token },
    body: JSON.stringify({ ...p, op }) })).json();

  let browser;
  try {
    let up = false;
    for (let i = 0; i < 60 && !up; i++) { try { up = (await api('ping')).ok; } catch (e) { await sleep(500); } }
    if (!up) die(`unidaw antwortet nicht auf ${url} (${path.join(work, 'unidaw.err')})`);
    log(`unidaw auf Port ${port}`);

    // MCP-Server als Kopie mit diesem Port (wie im Trockenlauf von docs/demo-video.md)
    const src = fs.readFileSync(path.join(REPO, 'mcp/unidaw_mcp.py'), 'utf8');
    const mcp = src.replace('UNIDAW_URL = "http://127.0.0.1:7400"', `UNIDAW_URL = "${url}"`);
    if (mcp === src) die('UNIDAW_URL in mcp/unidaw_mcp.py nicht gefunden');
    fs.writeFileSync(path.join(work, 'unidaw_mcp.py'), mcp);
    const mcpConfig = path.join(work, 'mcp.json');
    fs.writeFileSync(mcpConfig, JSON.stringify({ mcpServers: { unidaw: { command: 'python3',
      args: [path.join(work, 'unidaw_mcp.py')], env: { UNIDAW_TOKEN: token } } } }, null, 2));

    const { chromium } = loadPlaywright();
    browser = await chromium.launch();
    const page = await browser.newPage({ viewport: { width: VIEW.width, height: VIEW.height }, deviceScaleFactor: VIEW.scale });
    // Ansicht wie von Hand eingestellt (Merker der UI im Browser): Song-Baum und
    // Timeline, rechte Spalte und Dock zu, Zoom so, dass der Song passt. Die
    // Eingangszeile zeigt den Gerätenamen dieses Rechners — weg damit.
    // ui.dockH: Höhe des Docks, wenn eine Szene es öffnet (notes: Piano-Roll)
    await page.addInitScript(({ pps, dockH }) => {
      localStorage.setItem('dawd.noright', '1');
      localStorage.setItem('dawd.dockFolded', '1');
      localStorage.setItem('dawd.pps', String(pps));
      if (dockH) localStorage.setItem('dawd.dockH', String(dockH));
      document.addEventListener('DOMContentLoaded', () => {
        const st = document.createElement('style');
        st.textContent = '#audioStrip { display: none !important; }';
        document.head.appendChild(st);
      });
    }, { pps: shot.ui.pps, dockH: shot.ui.dockH || 0 });
    // Ohne Audiogerät (--no-audio) läuft die Transportuhr nicht: beim Play
    // meldet die Kette der UI die Position nach der Wanduhr — so läuft der
    // Playhead wie beim Abspielen, der Ton kommt ohnehin aus dem Render
    let clock = null;
    await page.route('**/command', async (route) => {
      let body = {};
      try { body = route.request().postDataJSON() || {}; } catch (e) { /* kein JSON */ }
      // Uhr beim Eingang merken: endet die Szene während fetch, ist clock schon null
      const now = clock;
      if (now === null || body.op !== 'position') return route.continue();
      const resp = await route.fetch();
      const j = await resp.json();
      return route.fulfill({ response: resp, json: { ...j, playing: true, position: now() } });
    });
    await page.goto(url + '/ui');
    await sleep(2500);
    const still = path.join(work, 'frames', 'start.jpg');
    await page.screenshot({ path: still, type: 'jpeg', quality: 90 });

    // Fotos in Abständen, bis stop() wahr ist
    const shoot = async (prefix, gapMs, stop) => {
      const frames = [];
      const t0 = Date.now();
      for (let i = 0; !stop(); i++) {
        const at = Date.now();
        const file = path.join(work, 'frames', `${prefix}${String(i).padStart(5, '0')}.jpg`);
        await page.screenshot({ path: file, type: 'jpeg', quality: 90 });
        frames.push({ file, t: (at - t0) / 1000 });
        const wait = gapMs - (Date.now() - at);
        if (wait > 0) await sleep(wait);
      }
      return frames;
    };

    // Agent: claude -p ohne unihad-Kontext, nur die beiden unidaw-Tools
    const claudeArgs = ['-p', shot.prompt, '--setting-sources', 'project,local', '--mcp-config', mcpConfig,
      '--strict-mcp-config', '--allowedTools', 'mcp__unidaw__unidaw_command mcp__unidaw__unidaw_start',
      '--output-format', 'json'];
    if (process.env.SHORT_MODEL) claudeArgs.push('--model', process.env.SHORT_MODEL);
    const agentEnv = { ...process.env };
    delete agentEnv.CLAUDECODE;
    delete agentEnv.UNIDAW_TOKEN;
    log('Agent startet');
    const agentStart = Date.now();
    let agentDone = false, agentOut = '';
    // Leerer Ordner außerhalb von $HOME: darunter fände Claude Code die
    // CLAUDE.md des Rechners (erster Lauf: der Agent bot an, die Datei auf die
    // Storage Box zu legen) — --setting-sources hält sie nicht ab
    const agentCwd = fs.mkdtempSync(path.join(os.tmpdir(), 'unidaw-short-'));
    const agent = spawn(process.env.CLAUDE_BIN || 'claude', claudeArgs,
      { cwd: agentCwd, env: agentEnv, stdio: ['ignore', 'pipe', fs.openSync(path.join(work, 'agent.err'), 'w')] });
    agent.stdout.on('data', (d) => { agentOut += d; });
    const agentExit = new Promise((r) => agent.on('close', (code) => { agentDone = true; r(code); }));
    const killer = setTimeout(() => agent.kill('SIGTERM'), 20 * 60 * 1000);
    let doneAt = null;
    // Die leere UI zeigt den Song, sobald der Agent ihn anlegt — ohne Neuladen
    const agentFrames = await shoot('a', 250, () => {
      if (agentDone && doneAt === null) doneAt = Date.now();
      return doneAt !== null && Date.now() - doneAt > 2000; // UI zieht den letzten Stand nach
    });
    clearTimeout(killer);
    const code = await agentExit;
    const agentSeconds = (doneAt - agentStart) / 1000;
    fs.writeFileSync(path.join(work, 'agent.json'), agentOut);
    let result;
    try { result = JSON.parse(agentOut); } catch (e) { die(`Agent ohne JSON-Antwort (Exit ${code}, ${path.join(work, 'agent.err')})`); }
    if (result.is_error) die(`Agent meldet Fehler: ${result.result}`);
    log(`Agent fertig nach ${agentSeconds.toFixed(0)} s, ${result.total_cost_usd?.toFixed(2)} USD`);
    if (render && !fs.existsSync(render)) die(`kein Render ${render} — Lauf wiederholen oder Drehbuch anpassen`);
    const ask = async (op, p = {}) => {
      const r = await api(op, p);
      if (!r.ok) die(`${op} ${JSON.stringify(p)}: ${JSON.stringify(r)}`);
      return r;
    };
    const endNode = (await ask('treeState')).current;
    // Nur eingebaute Plugins im Bild (docs/shorts.md, Material und Lizenz):
    // greift der Agent zu einem installierten VST3/LV2, gibt es kein Video
    const edit = path.join(project, `node-${endNode}.tracktionedit`);
    const foreign = fs.existsSync(edit)
      ? [...fs.readFileSync(edit, 'utf8').matchAll(/<PLUGIN type="(?:vst|lv2)"[^>]*?filename="([^"]*)"/g)].map((x) => x[1]) : [];
    if (foreign.length) die(`fremde Plugins im Song: ${foreign.join(', ')} — Prompt auf eingebaute Plugins lenken`);
    // Ganzer Song als Ton: Render des Agenten, sonst rendert die Kette den Stand am Ende
    let song = render;
    if (!song) {
      song = path.join(work, 'song.wav');
      // Spitze auf -1 dB: ein Song, den der Agent übersteuert lässt, clippt sonst im Video
      await ask('render', { file: song, normalize: 'peak', level: -1 });
    }
    let stemFiles = null;
    const stemOf = async (track) => {
      if (!stemFiles) {
        const dir = path.join(work, 'stems');
        await ask('renderStems', { dir });
        stemFiles = fs.readdirSync(dir).map((f) => path.join(dir, f));
      }
      const f = stemFiles.find((s) => path.basename(s).toLowerCase().includes(track.toLowerCase()));
      if (!f) die(`kein Stem für Spur ${track} in ${stemFiles.map((s) => path.basename(s))}`);
      return f;
    };
    // Spur und Section nach Muster (der Agent wählt die Namen)
    const trackNamed = async (re) => {
      const list = (await ask('state', { only: ['trackList'] })).trackList || [];
      const tr = list.find((x) => new RegExp(re, 'i').test(x.name));
      if (!tr) die(`keine Spur /${re}/ in ${list.map((x) => x.name)}`);
      return tr.name;
    };
    const sectionStart = async (re, nth = 0) => {
      const list = (await ask('state', { only: ['sections'] })).sections || [];
      const hits = list.filter((x) => new RegExp(re, 'i').test(x.label || x.name));
      if (!hits[nth]) die(`keine Section /${re}/ Nr. ${nth} in ${list.map((x) => x.label || x.name)}`);
      return { start: hits[nth].start, name: hits[nth].name };
    };

    // Play-Szenen: je Szene Node wählen, Solo, Play ab Section; Ton je Szene
    const t = shot.timing;
    const scenes = [];
    let soloed = null;
    for (const sc of shot.scenes || [{ from: t.playFrom, seconds: t.play, audio: 'render' }]) {
      if (sc.node) {
        const id = sc.node === 'start' ? startNode : endNode;
        if (id === null || id === undefined) die(`Szene ohne Node ${sc.node} (Basis-Song fehlt im Drehbuch)`);
        await ask('treeCheckout', { node: id });
        await sleep(1500); // UI zieht den Node nach
      }
      if (soloed) { await ask('trackMix', { track: soloed, solo: false }); soloed = null; }
      if (sc.solo) { soloed = await trackNamed(sc.solo); await ask('trackMix', { track: soloed, solo: true }); }
      // Noten zeigen: Doppelklick auf den ersten MIDI-Clip öffnet die Piano-Roll im Dock
      if (sc.notes) {
        const clip = page.locator('.clip.midi').first();
        if (!(await clip.count())) die('Szene notes: kein MIDI-Clip in der UI');
        await clip.dblclick({ position: { x: 20, y: 20 } });
        await sleep(1000);
      }
      const sec = sc.section ? await sectionStart(sc.section, sc.nth || 0) : { start: 0, name: null };
      const from = sec.start + (sc.from || 0);
      let audio;
      if (sc.audio === 'render') audio = { file: song, offset: from };
      else if (sc.audio === 'node') {
        const r = await ask('renderNode', sec.name ? { node: (await ask('treeState')).current, section: sec.name }
          : { node: (await ask('treeState')).current });
        const keep = path.join(work, `node-${r.node}${sec.name ? '-' + sec.name : ''}.wav`);
        fs.copyFileSync(r.file, keep);
        audio = { file: keep, offset: sec.name ? (sc.from || 0) : from };
      } else if (sc.audio && sc.audio.stem) audio = { file: await stemOf(await trackNamed(sc.audio.stem)), offset: from };
      else if (sc.audio && sc.audio.file) audio = { file: path.join(project, sc.audio.file), offset: sc.audio.offset || 0 };
      else die(`Szene: unbekannter Ton ${JSON.stringify(sc.audio)}`);
      // Play in der UI (Playhead läuft), so lange wie die Szene
      const p = await api('play', { on: true, from });
      if (!p.ok) die(`play: ${JSON.stringify(p)}`);
      const playStart = Date.now();
      clock = () => from + (Date.now() - playStart) / 1000;
      const frames = await shoot(`p${scenes.length}-`, 66, () => Date.now() - playStart > sc.seconds * 1000 + 300);
      clock = null;
      await api('play', { on: false });
      scenes.push({ frames, seconds: sc.seconds, audio, caption: sc.caption || null });
      log(`Szene ${scenes.length}: ab ${from.toFixed(1)} s, Ton ${path.basename(audio.file)} ab ${audio.offset.toFixed(1)} s`);
    }
    if (soloed) await api('trackMix', { track: soloed, solo: false });

    const manifest = { still, agentFrames, scenes, agentSeconds, answer: result.result,
      costUsd: result.total_cost_usd, song };
    fs.writeFileSync(path.join(work, 'manifest.json'), JSON.stringify(manifest, null, 2));
    return manifest;
  } finally {
    if (browser) await browser.close();
    try { await api('quit'); } catch (e) { /* schon weg */ }
    await Promise.race([new Promise((r) => daw.on('exit', r)), sleep(5000)]);
    if (daw.exitCode === null) daw.kill('SIGKILL');
  }
}

// ── Schnitt ──────────────────────────────────────────────────────────────

// 1–2 Zeilen aus der Antwort, wörtlich, nur Markdown-Zeichen weg und gekürzt
// pick: Muster für die erste gezeigte Zeile (z. B. die mit der Tonart)
function answerLine (text, pick) {
  // # und > nur am Zeilenanfang (Überschrift, Zitat): "G#" bleibt "G#"
  let lines = String(text || '').split('\n').map((l) => l.replace(/^\s*[#>]+\s*/, '').replace(/[*_`]/g, '')
    .replace(/^\s*[-•]\s*/, '').trim()).filter((l) => l && !l.startsWith('|'));   // Tabellen nicht
  const at = pick ? lines.findIndex((l) => new RegExp(pick, 'i').test(l)) : -1;
  if (at > 0) lines = lines.slice(at);
  let s = '';
  for (const l of lines) { s = s ? `${s} ${l}` : l; if (s.length >= 60) break; }
  if (s.length > 170) s = s.slice(0, s.lastIndexOf(' ', 168)) + ' …';
  return s.replace(/:$/, '');   // Doppelpunkt vor einer weggelassenen Liste
}

const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));

function chromeHtml () {
  return `<!doctype html><html><head><meta charset="utf-8"><style>
  * { box-sizing: border-box; margin: 0; }
  html, body { width: ${W}px; height: ${H}px; background: transparent; overflow: hidden;
    font-family: "Noto Sans", "DejaVu Sans", sans-serif; color: #e8eaf0; }
  .band { position: absolute; left: 0; width: ${W}px; background: ${BG}; }
  #title { top: 0; height: ${TITLE_H}px; padding: 34px 48px 0; }
  #title h1 { font-size: 62px; font-weight: 700; letter-spacing: -0.5px; }
  #title p { font-size: 34px; color: #8b7bff; margin-top: 10px; min-height: 46px; }
  #card { top: ${TITLE_H}px; height: ${CARD_H}px; padding: 14px 36px 30px; }
  #card .box { height: 100%; border: 2px solid #2a2f3d; border-radius: 22px; background: #161922; padding: 26px 30px; }
  #card .lbl { font-size: 24px; color: #8b92a5; text-transform: uppercase; letter-spacing: 2px; margin-bottom: 12px; }
  #prompt { font-size: 31px; line-height: 1.38; white-space: pre-wrap; }
  #prompt .caret { display: inline-block; width: 14px; height: 34px; background: #8b7bff; vertical-align: -5px; margin-left: 3px; }
  #answer { font-size: 29px; line-height: 1.35; color: #3ddc97; margin-top: 18px; }
  #foot { top: ${H - FOOT_H}px; height: ${FOOT_H}px; text-align: center; font-size: 38px; font-weight: 700;
    color: #ffb547; padding-top: 34px; }
  #ui { position: absolute; top: ${UI_Y}px; left: 0; width: ${W}px; height: ${UI_H}px;
    border-top: 2px solid #2a2f3d; border-bottom: 2px solid #2a2f3d; }
  /* unten 2 px durchsichtig: sonst speichert Chromium die Endkarte ohne Alpha,
     ffmpeg baut beim Formatwechsel den Filter neu und verliert die Text-Ebene */
  #end { position: absolute; inset: 0 0 2px 0; background: ${BG}; display: none; flex-direction: column;
    justify-content: center; align-items: center; text-align: center; gap: 34px; padding: 0 60px; }
  #end h2 { font-size: 64px; font-weight: 700; }
  #end p { font-size: 40px; color: #8b92a5; }
  #end .url { color: #8b7bff; font-size: 44px; }
  </style></head><body>
  <div class="band" id="title"><h1></h1><p></p></div>
  <div class="band" id="card"><div class="box"><div class="lbl">Prompt to the agent</div><div id="prompt"></div><div id="answer"></div></div></div>
  <div id="ui"></div>
  <div class="band" id="foot"></div>
  <div id="end"><h2>unidaw</h2><p>a DAW for people and AI agents</p><p>Free and open source (AGPLv3) · Linux</p><p class="url">github.com/unihad/unidaw</p></div>
  <script>
  window.draw = (s) => {
    document.getElementById('end').style.display = s.end ? 'flex' : 'none';
    document.querySelector('#title h1').textContent = s.title;
    document.querySelector('#title p').textContent = s.caption || '';
    const pr = document.getElementById('prompt');
    pr.textContent = s.prompt;
    if (s.caret) { const c = document.createElement('span'); c.className = 'caret'; pr.appendChild(c); }
    document.getElementById('answer').textContent = s.answer ? '→ ' + s.answer : '';
    document.getElementById('foot').textContent = s.foot || '';
    document.getElementById('foot').style.display = s.end ? 'none' : '';
  };
  </script></body></html>`;
}

// Einblendung aus Werten der Antwort (answerCaptions: [{re, text mit $1…}]);
// nur, was die Antwort wirklich sagt — sonst keine
function answerCaption (text) {
  return (shot.answerCaptions || []).map((c) => {
    const m = String(text || '').match(new RegExp(c.re, 'i'));
    return m ? c.text.replace(/\$(\d)/g, (_, i) => m[+i]) : null;
  }).filter(Boolean).join(' · ');
}

async function compose (m) {
  const t = shot.timing;
  const fps = 30;
  // Manifest von vor den Szenen (Short 1): ein Play ab playFrom
  if (!m.scenes) {
    m.scenes = [{ frames: m.playFrames, seconds: t.play, audio: { file: m.render, offset: t.playFrom }, caption: null }];
    m.song = m.render;
  }
  const factor = Math.max(2, Math.round(m.agentSeconds / t.agent));
  // Agent-Szene: alle Bilder bis kurz nach dem Ende (die UI zieht nach), beschleunigt
  const agentSpan = m.agentFrames[m.agentFrames.length - 1].t + 0.25;
  const agentOut = agentSpan / factor;
  const a0 = t.intro, a1 = a0 + t.typing, c1 = a1 + agentOut;
  const starts = [];
  let d1 = c1;
  for (const sc of m.scenes) { starts.push(d1); d1 += sc.seconds; }
  const total = d1 + t.end;
  const answer = answerLine(m.answer, shot.answerPick);
  const fromAnswer = answerCaption(m.answer);
  log(`Faktor ${factor}×, Länge ${total.toFixed(1)} s, Antwort: ${answer}${fromAnswer ? ' | ' + fromAnswer : ''}`);

  // Zustände der Text-Ebene je 1/15 s, gleiche zusammengefasst
  const stateAt = (x) => {
    let caption = shot.captions.filter((c) => c.at <= x).pop()?.text || '';
    if (x >= c1) {
      const i = starts.filter((s) => s <= x).length - 1;
      // Einblendung der Szene aus dem Drehbuch (--compose-only nimmt Änderungen mit)
      const sc = i >= 0 ? (shot.scenes ? shot.scenes[i] : m.scenes[i]) : null;
      caption = (sc && sc.caption) || fromAnswer || caption;
    }
    const s = { title: shot.title, caption, prompt: '', caret: false, answer: '', foot: '', end: false };
    if (x >= d1) return { ...s, end: true };
    if (x >= a0) {
      const k = Math.min(1, (x - a0) / (t.typing * 0.85));
      s.prompt = shot.prompt.slice(0, Math.round(shot.prompt.length * k));
      s.caret = x < a1;
    }
    if (x >= a1 && x < c1) s.foot = `${factor}× speed`;
    if (x >= c1) s.answer = answer;
    return s;
  };
  const step = 1 / 15;
  const states = [];
  for (let x = 0; x < total - 1e-9; x += step) {
    const s = stateAt(x);
    const key = JSON.stringify(s);
    const last = states[states.length - 1];
    if (last && last.key === key) last.dur += step; else states.push({ key, s, dur: step });
  }

  const { chromium } = loadPlaywright();
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: W, height: H } });
  await page.setContent(chromeHtml());
  const cdir = path.join(work, 'chrome');
  fs.rmSync(cdir, { recursive: true, force: true });
  fs.mkdirSync(cdir);
  const files = new Map();
  for (const st of states) {
    if (!files.has(st.key)) {
      const f = path.join(cdir, `c${String(files.size).padStart(4, '0')}.png`);
      await page.evaluate((s) => window.draw(s), st.s);
      await page.screenshot({ path: f, omitBackground: true });
      files.set(st.key, f);
    }
  }
  await browser.close();

  const list = (items) => items.map((i) => `file '${i.file}'\nduration ${i.dur.toFixed(4)}`).join('\n') +
    `\nfile '${items[items.length - 1].file}'\n`;
  const chromeList = states.map((st) => ({ file: files.get(st.key), dur: st.dur }));
  // Bilder mit ihrer Dauer bis zum nächsten; das letzte bis zum Ende der Szene (span, Quellzeit)
  const seq = (frames, speed, span) => frames.map((f, i) => ({ file: f.file,
    dur: Math.max(0.001, ((i + 1 < frames.length ? frames[i + 1].t : span) - f.t) / speed) }));
  const sceneFrames = m.scenes.map((sc) => sc.frames.filter((f) => f.t < sc.seconds));
  const lastScene = sceneFrames[sceneFrames.length - 1];
  const uiList = [{ file: m.still, dur: a1 }, ...seq(m.agentFrames, factor, agentSpan),
    ...sceneFrames.flatMap((fr, i) => seq(fr, 1, m.scenes[i].seconds)), { file: lastScene[lastScene.length - 1].file, dur: t.end }];
  fs.writeFileSync(path.join(work, 'ui.txt'), list(uiList));
  fs.writeFileSync(path.join(work, 'chrome.txt'), list(chromeList));

  // Ton: ganzer Song leise bis zum ersten Play, dann je Szene ihr Ton voll
  // (kurze Blenden an den Schnitten), die letzte läuft unter der Endkarte aus
  const fmt = 'aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo';
  const filter = [
    `[0:v]scale=${W}:${UI_H}:flags=lanczos,setsar=1,fps=${fps},pad=${W}:${H}:0:${UI_Y}:color=${BG}[u]`,
    `[1:v]fps=${fps},format=rgba[c]`,
    `[u][c]overlay=0:0,format=yuv420p[v]`,
    `[2:a]${fmt},atrim=0:${c1.toFixed(3)},apad=whole_dur=${c1.toFixed(3)},volume=0.22[q]`,
    ...m.scenes.map((sc, i) => {
      const last = i === m.scenes.length - 1;
      const dur = sc.seconds + (last ? t.end : 0);
      const o = sc.audio.offset;
      return `[${3 + i}:a]${fmt},atrim=${o.toFixed(3)}:${(o + dur).toFixed(3)},asetpts=PTS-STARTPTS,` +
        `apad=whole_dur=${dur.toFixed(3)},afade=t=in:d=0.03,` +
        (last ? `afade=t=out:st=${(dur - 2).toFixed(3)}:d=2` : `afade=t=out:st=${(dur - 0.05).toFixed(3)}:d=0.05`) + `[s${i}]`;
    }),
    `[q]${m.scenes.map((_, i) => `[s${i}]`).join('')}concat=n=${1 + m.scenes.length}:v=0:a=1[a]`,
  ].join(';');
  const mp4 = path.join(work, `agent-daw-short-${shot.id}.mp4`);
  const ff = spawnSync('ffmpeg', ['-y', '-loglevel', 'warning', '-f', 'concat', '-safe', '0', '-i', path.join(work, 'ui.txt'),
    '-f', 'concat', '-safe', '0', '-i', path.join(work, 'chrome.txt'), '-i', m.song,
    ...m.scenes.flatMap((sc) => ['-i', sc.audio.file]),
    '-filter_complex', filter, '-map', '[v]', '-map', '[a]', '-t', total.toFixed(3), '-r', String(fps),
    '-c:v', 'libx264', '-preset', 'medium', '-crf', '20', '-pix_fmt', 'yuv420p', '-profile:v', 'high',
    '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', mp4], { encoding: 'utf8' });
  fs.writeFileSync(path.join(work, 'ffmpeg.log'), ff.stderr);
  if (ff.status !== 0) die(`ffmpeg: ${ff.stderr}`);
  return { mp4, total, factor, answer };
}

function probe (file) {
  const r = spawnSync('ffprobe', ['-v', 'error', '-show_entries', 'stream=codec_type,codec_name,width,height',
    '-show_entries', 'format=duration', '-of', 'json', file], { encoding: 'utf8' });
  const j = JSON.parse(r.stdout);
  const v = j.streams.find((s) => s.codec_type === 'video');
  const a = j.streams.find((s) => s.codec_type === 'audio');
  return { video: v && v.codec_name, audio: a && a.codec_name, width: v && v.width, height: v && v.height,
    duration: +j.format.duration };
}

(async () => {
  const m = flag('--compose-only') ? JSON.parse(fs.readFileSync(path.join(work, 'manifest.json'), 'utf8')) : await record();
  const c = await compose(m);
  const p = probe(c.mp4);
  if (p.video !== 'h264' || p.audio !== 'aac' || p.width !== W || p.height !== H) die(`ffprobe: ${JSON.stringify(p)}`);
  let copied = null;
  if (fs.existsSync(path.dirname(out))) { fs.copyFileSync(c.mp4, out); copied = out; } else log(`${path.dirname(out)} fehlt — Video nur im Arbeitsordner`);
  console.log(JSON.stringify({ ok: true, short: shot.id, file: c.mp4, copiedTo: copied, ...p,
    speed: c.factor, agentSeconds: Math.round(m.agentSeconds), costUsd: m.costUsd, answer: c.answer }, null, 2));
})().catch((e) => {
  console.error(`[short ${shot.id}] FEHLER: ${e instanceof Fail ? e.message : e.stack}`);
  process.exit(1);
});
