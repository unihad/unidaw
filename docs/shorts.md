# Shorts-Reihe: je ein Short pro Feature

Plan und Drehbücher für die Shorts über unidaw (Decision
[2026-11-23-shorts-statt-demo-video](decisions/2026-11-23-shorts-statt-demo-video.md),
Ziel `agent-daw-bereit-fuer-den-vertrieb-machen`). Löst
[`docs/demo-video.md`](demo-video.md) ab. Agents nehmen die Shorts
automatisch auf (Todos `2026-11-23-ersten-short-automatisch-aufnehmen`,
`2026-11-23-restliche-shorts-aufnehmen`); niemand spielt etwas ein.

## Die Reihe

| # | Short | Feature | Länge | ML-Modelle |
|---|---|---|---|---|
| 1 | One prompt, one song | Agent baut über MCP einen Song, die UI zeigt es live — **Release-Ankündigung** | 40–45 s | nein |
| 2 | Try it, keep both | Song-Baum: Variante im Branch, beide vergleichen | 30–40 s | nein |
| 3 | The agent listens | Tonart einer Aufnahme hören, passende Akkorde dazu | 30–35 s | nein |
| 4 | Longer song, one sentence | Sections: Aufbau des Songs per Prompt umbauen | 30–40 s | nein |
| 5 | Ready for streaming | Mix auf −14 LUFS, Limiter, Messung | 20–30 s | nein |
| 6 | Melody to MIDI | Audio → MIDI, ein Synth spielt die Melodie | 25–35 s | basic-pitch |
| 7 | Split a mix | Stem-Trennung eines Mixes in eigene Spuren | 25–35 s | HT-Demucs |

**Warum diese Reihenfolge:** Short 1 erklärt unidaw als Ganzes (Agent +
DAW + gemeinsamer Song). Short 2 zeigt danach das, was andere DAWs nicht
haben, den Song-Baum. Shorts 3–5 sind Alltag beim Songbau (hören,
arrangieren, mischen). Die ML-Shorts 6 und 7 kommen zuletzt: sie brauchen
die Modelle (`fetch-models`, etwa 1,2 GB, README) und haben das größte
Risiko in der automatischen Kette.

**Warum diese Features:** jedes kann unidaw heute, und jedes prüft die
Suite an der API (Testnamen je Drehbuch unter „Beleg“). Quellen: README
(„Song tree“, „Analysis for agents“), Szenen 2, 4, 5 und 6 aus
`docs/demo-video.md` (dort einmal ohne Fehler durchgespielt, Trockenlauf
2026-11-21), Op-Liste in `dawd/source/OpRegistry.cpp`.

**Bewusst nicht in der Reihe:**

- Live-Aufnahme eines Menschen (Szene 3 des Demo-Videos): niemand spielt
  ein (Decision).
- Suche in der Sample-Bibliothek nach Klang: die Bibliothek besteht aus
  Samples fremder Hersteller ohne Lizenzkette für ein Video
  (`docs/sample-bibliothek.md`).
- Plugin-Sandbox (Plugin stürzt ab, Song läuft weiter): braucht ein
  fremdes Plugin im Bild und einen künstlich erzeugten Absturz — gut für
  später, wenn die Kette steht.
- Referenz-Vergleich mit einem echten Song: fremdes Material.

## Gemeinsame Regeln für jeden Short

**Format:** 1080×1920 (9:16), 30 fps, H.264 + AAC, `.mp4`. Prüfen mit
`ffprobe`. Ablage zum Ansehen:
`~/StorageBox/agent-daw/agent-daw-short-<feature>.mp4` (`<feature>` wie in
der Überschrift je Drehbuch).

**Bildaufbau im Hochformat** (gleich in allen Shorts, damit die Reihe
wiedererkennbar ist):

```text
┌──────────────────────┐
│ Titel (1 Zeile)      │  ~8 %   Text-Einblendung, bleibt stehen
├──────────────────────┤
│ Prompt-Karte         │  ~30 %  Prompt wörtlich, wird „getippt“;
│ → Antwort-Zeile      │         danach 1–2 Zeilen aus der Antwort
├──────────────────────┤
│ unidaw-UI            │  ~55 %  Ausschnitt der UI, gezoomt auf den
│ (Ausschnitt)         │         Bereich, der sich gerade ändert
├──────────────────────┤
│ Fußzeile             │  ~7 %   „8× speed“ während beschleunigt,
└──────────────────────┘         sonst leer
```

- **UI:** im Headless-Browser aufgenommen (Querformat, z. B. 1600×1000),
  dann je Szene auf den wichtigen Bereich zugeschnitten und skaliert:
  Song-Baum, Timeline mit Sections und Spuren oder Routing-Kette.
  Klicks in der UI (Play, Node wählen, Solo) macht die Kette per Skript.
- **Prompt-Karte statt Terminal:** der Prompt steht als Text im Bild,
  Schrift groß (lesbar am Handy), dunkler Grund wie die UI. Die
  Antwort-Zeile ist **wörtlich** aus der Ausgabe des Agenten (`claude -p`)
  genommen — gekürzt, nie umformuliert. Kein echter Terminal-Mitschnitt
  nötig.
- **Wartezeit des Agenten:** beschleunigt (4–8×), mit Marke „8× speed“
  (Faktor wie verwendet) in der Fußzeile. So bleibt sichtbar, dass die
  Arbeit länger dauert.

**Ton:** der Render des Songs aus dem Short (`render` am Ende des
Laufs), keine Live-Aufnahme. Während die UI sich aufbaut leise, beim
Play am Ende voll. Kein Sprecher, keine Stimme.

**Text-Einblendungen:** Englisch, kurz, ohne Hype und Superlative (Ton der
Kanäle: `unihad-social`, `docs/strategie.md`, B3). Jeder Short endet mit
derselben Endkarte (3 s):

```text
unidaw — a DAW for people and AI agents
Free and open source (AGPLv3) · Linux
github.com/unihad/unidaw
```

**Ausgangsprojekt:** Short 1 baut den Song aus dem Nichts. Shorts 2, 4
und 5 starten von einer **Kopie** des Projekts, das Short 1 hinterlässt
(„Basis-Song“: Intro + Verse, Drums, Bass, 96 BPM) — so klingt die Reihe
gleich. Shorts 3, 6 und 7 bringen ihr eigenes Material mit (siehe dort).

**Material und Lizenz:** nur, was unidaw mitbringt oder die Kette selbst
erzeugt:

- eingebautes Drum Kit (synthetisiert von `dawd/kits/make_basic_kit.py`,
  `THIRD_PARTY_LICENSES.md`), eingebaute Plugins (4OSC, Reverb, Limiter);
- keine Fremd-Plugins, keine Samples aus der Sample-Bibliothek, keine
  fremden Songs als Referenz;
- Audio-Eingaben (Melodie, Mix) erzeugt die Kette selbst: Melodie per
  Skript (`tools/short/make_melody.py`: a-Moll, additiver Zupfton — der
  Karplus-Strong-Ton des Trockenlaufs gab `detectKey` E-Moll und
  `audioToMidi` Geisternoten, Todos `2026-11-23-detectkey-gezupfte-melodie`
  und `2026-11-23-audiotomidi-obertoene-gezupft`), Mix als Render von
  unidaw (`song.wav` aus Short 1).
- Die Prompts der Shorts 3 und 4 nennen 4OSC und den eingebauten Hall:
  ohne das griff der Agent zu installierten Fremd-Plugins (Lauf 2026-11-23:
  Helm, Dragonfly Reverb). Die Kette bricht ab, wenn der Song am Ende ein
  VST3/LV2 enthält.

**Keine echten Daten:** frische Projektordner, eigene Config-Ordner und
eigener Port (wie im Trockenlauf); Pfade im Bild nur unter
`~/music/unidaw-short-…`; keine Namen, Mails, Firmenpfade. Claude Code
mit `--setting-sources project,local` und nur den Tools
`mcp__unidaw__unidaw_command mcp__unidaw__unidaw_start` (Fallstricke in
`docs/demo-video.md`).

**Kennzeichnung:** Die Shorts zeigen einen Bildschirm-Mitschnitt und
Musik aus eingebauten Synths; es gibt keine KI-Stimme und kein
realistisch wirkendes KI-Bild. Nach der Kanal-Regel (`unihad-social`,
`docs/strategie.md`, B4; `docs/ki-kennzeichnung.md`) ist dafür kein
Plattform-Label nötig. Weil ein Agent den Song macht, sagt der
Begleittext jedes Posts es offen, z. B. „An AI agent built this song in
unidaw over MCP.“ Die Bio der Kanäle nennt die Agenten ohnehin (B4).

**Prompts:** wörtlich wie unten, Englisch. Antworten weichen je Lauf ab;
gezeigt wird, was der Lauf liefert. Stimmt das erwartete Ergebnis nicht
(z. B. andere Tonart), wird der Short nicht veröffentlicht, sondern der
Lauf wiederholt oder das Drehbuch angepasst.

## Drehbücher

### 1 — One prompt, one song (`intro`)

- **Feature:** ein Agent arbeitet über MCP mit unidaw; der Song erscheint
  live in der UI. Trägt die Release-Ankündigung.
- **Länge:** 40–45 s.
- **Ausgangsprojekt:** keins; leere UI, Projektordner
  `~/music/unidaw-short-intro` existiert noch nicht.
- **Prompt:**

  ```text
  Start a new song in ~/music/unidaw-short-intro at 96 BPM. Make two sections: Intro (4 bars) and Verse (8 bars). Add a drum beat with the built-in drum kit and a simple bass line in A minor with the 4OSC synth, both playing through both sections. Then render the song to ~/music/unidaw-short-intro/song.wav.
  ```

- **Erwartet:** Sections Intro/Verse bei 96 BPM, Spuren Drums (Drum Kit)
  und Bass (4OSC, a-Moll), Render `song.wav` (gleich Prompt 1 des
  Demo-Videos, dort ohne die Spur Take; Trockenlauf 2026-11-21).
- **Bild:**
  1. 0–4 s: leere UI, Titel „unidaw 0.1 is out“.
  2. 4–10 s: Prompt wird getippt.
  3. 10–28 s: UI-Ausschnitt Timeline + Song-Baum, beschleunigt: Sections,
     Spuren, Clips, Nodes erscheinen.
  4. 28–40 s: Play in der UI, Playhead läuft über Intro und Verse; Ton voll.
  5. Endkarte.
- **Ton:** `song.wav`.
- **Text-Einblendungen:** „unidaw 0.1 is out“ · „A DAW for people and AI
  agents“ (bei 4 s) · „The agent works over MCP. You see every change.“
  (bei 12 s) · Endkarte.
- **Beleg:** Demo-Video Szene 2 (Trockenlauf), Suite `batch_flow`,
  `auto_branch_flow`, `render_options_flow`.

### 2 — Try it, keep both (`song-tree`)

- **Feature:** Song-Baum — eine Variante in einem neuen Branch, die alte
  bleibt; beide vergleichen.
- **Länge:** 30–40 s.
- **Ausgangsprojekt:** Kopie des Basis-Songs als
  `~/music/unidaw-short-song-tree`.
- **Prompt:**

  ```text
  Open the song in ~/music/unidaw-short-song-tree. Try a more energetic version of the Verse in a new branch: sixteenth-note hi-hats and a busier bass line. Keep the current version as it is, so I can compare both.
  ```

- **Erwartet:** der aktuelle Node bekommt einen Namen („Verse calm …“),
  ein neuer Branch („Verse energetic …“) mit 16tel-Hi-Hats und
  bewegterem Bass (gleich Prompt 3 des Demo-Videos, Trockenlauf).
- **Bild:**
  1. 0–6 s: Prompt.
  2. 6–16 s: Song-Baum groß, beschleunigt: neuer Zweig wächst.
  3. 16–24 s: Skript klickt Node „calm“, Play in der Verse (4 s).
  4. 24–32 s: Skript klickt Node „energetic“, Play (4 s).
  5. Endkarte.
- **Ton:** zwei Renders der Verse, je Node einer (`renderNode`), passend
  zum gewählten Node geschnitten.
- **Text-Einblendungen:** „Every idea is a branch“ · „calm“ / „energetic“
  über dem jeweiligen Node · „Keep both. Pick later.“
- **Beleg:** Demo-Video Szene 5 (Trockenlauf), Suite `treeSeed_fields`,
  `auto_branch_flow`, `render_batch_flow`.

### 3 — The agent listens (`listen`)

- **Feature:** der Agent hört die Tonart einer Aufnahme und legt passende
  Akkorde darunter.
- **Länge:** 30–35 s.
- **Ausgangsprojekt:** Kopie des Basis-Songs als
  `~/music/unidaw-short-listen`, dazu `melody.wav` im selben Ordner:
  8 Takte Melodie in a-Moll bei 96 BPM, gezupfter Ton, von der Kette
  erzeugt (wie im Trockenlauf des Demo-Videos).
- **Prompt:**

  ```text
  Open the song in ~/music/unidaw-short-listen. Put melody.wav from that folder on a new audio track called Melody in the Verse. What key is it in? Then add a soft pad with the 4OSC synth that plays chords in that key under the Verse, and put a little of the built-in reverb on the melody.
  ```

- **Erwartet:** Spur Melody mit dem Clip in der Verse; Antwort nennt
  „A minor“; Spur Pad (4OSC, weich) mit Akkorden in a-Moll; Reverb auf
  Melody (gleich Prompt 2 des Demo-Videos, dort von einer Aufnahme statt
  einer Datei; Trockenlauf mit derselben Art Datei).
- **Bild:**
  1. 0–6 s: Prompt.
  2. 6–14 s: Timeline, Spur Melody mit Wellenform erscheint.
  3. 14–18 s: Antwort-Zeile „… A minor …“ groß.
  4. 18–26 s: Spur Pad und Modul „Melody Reverb“ in der Routing-Kette.
  5. 26–32 s: Play der Verse. Endkarte.
- **Ton:** Render der Verse mit Melodie und Pad.
- **Text-Einblendungen:** „The agent listens“ · „Key: A minor“ (nur wenn
  die Antwort das sagt) · „Chords in the same key“.
- **Beleg:** Demo-Video Szene 4 (Trockenlauf), Suite `detect_key_flow`.

### 4 — Longer song, one sentence (`sections`)

- **Feature:** Sections sind Zeitbereiche; der Agent baut den Aufbau des
  Songs um (neue Section, wiederholen, Schluss).
- **Länge:** 30–40 s.
- **Ausgangsprojekt:** Kopie des Basis-Songs als
  `~/music/unidaw-short-sections`.
- **Prompt:**

  ```text
  Open the song in ~/music/unidaw-short-sections. Make it a full song: add a Chorus (8 bars) after the Verse with a simple lead melody on the 4OSC synth, then repeat Verse and Chorus once, and end with a 4-bar Outro where only the bass plays.
  ```

- **Erwartet:** Sections Intro, Verse, Chorus, Verse, Chorus, Outro; Spur
  Lead im Chorus; im Outro nur Bass. **Nicht trocken getestet.**
- **Bild:**
  1. 0–6 s: Prompt.
  2. 6–24 s: Timeline ganz im Bild, beschleunigt: Section-Band wächst von
     zwei auf sechs Abschnitte, Clips folgen.
  3. 24–34 s: Play ab dem zweiten Chorus bis ins Outro.
  4. Endkarte.
- **Ton:** Render des ganzen Songs, ab dem zweiten Chorus.
- **Text-Einblendungen:** „From 12 bars to a full song“ · Namen der
  Sections stehen schon in der UI, keine Einblendung nötig.
- **Beleg:** Suite `section_span_flow`, `section_move_copy_flow`,
  `clip_split_duplicate_flow`.

### 5 — Ready for streaming (`mix`)

- **Feature:** Mix auf eine Ziel-Lautheit: Limiter auf Master, Fader,
  Render, Messung.
- **Länge:** 20–30 s.
- **Ausgangsprojekt:** Kopie des Basis-Songs als
  `~/music/unidaw-short-mix`.
- **Prompt:**

  ```text
  Open the song in ~/music/unidaw-short-mix. Mix it for streaming: about -14 LUFS and no clipping. Render the whole song to ~/music/unidaw-short-mix/mix.wav and tell me the loudness.
  ```

- **Erwartet:** Limiter auf Master („Master Limiter“), Fader angepasst,
  Render; Antwort nennt etwa −14 LUFS und True Peak unter −1 dB (gleich
  Prompt 4 des Demo-Videos ohne den Favoriten; Trockenlauf: −14,0 LUFS,
  −1,1 dB TP).
- **Bild:**
  1. 0–6 s: Prompt.
  2. 6–14 s: Routing-Kette: Limiter erscheint in der Master-Kette.
  3. 14–20 s: Antwort-Zeile mit LUFS und True Peak groß.
  4. 20–26 s: Play. Endkarte.
- **Ton:** `mix.wav`.
- **Text-Einblendungen:** „Mix to a target“ · die gemessenen Werte aus
  der Antwort (z. B. „−14.0 LUFS · −1.1 dB true peak“).
- **Beleg:** Demo-Video Szene 6 (Trockenlauf), Suite `mix_bus_flow`,
  `metering_flow`, `render_options_flow`.

### 6 — Melody to MIDI (`audio-to-midi`)

- **Feature:** eine Melodie als Audio wird zu MIDI-Noten; ein Synth
  spielt sie.
- **Länge:** 25–35 s.
- **Ausgangsprojekt:** leer; im Ordner `~/music/unidaw-short-midi` liegt
  `melody.wav` (wie in Short 3, von der Kette erzeugt). Modelle geladen
  (`fetch-models`).
- **Prompt:**

  ```text
  Start a new song in ~/music/unidaw-short-midi at 96 BPM with one 8-bar section called Verse. Put melody.wav from that folder on an audio track called Melody. Then turn the melody (a single line) into MIDI notes on a new track called Lead and let the 4OSC synth play it with a bright lead sound. Mute the Melody track.
  ```

- **Erwartet:** Spur Melody (Audio), Spur Lead mit MIDI-Clip, dessen
  Noten der Melodie folgen (Piano-Roll), 4OSC als Lead; Melody stumm.
  **Nicht trocken getestet.**
- **Bild:**
  1. 0–6 s: Prompt.
  2. 6–12 s: Wellenform der Spur Melody; kurz Play (Originalton).
  3. 12–22 s: Spur Lead mit Noten erscheint; Zoom auf die Piano-Roll.
  4. 22–30 s: Play — derselbe Verlauf, jetzt vom Synth. Endkarte.
- **Ton:** 4 s `melody.wav`, dann Render mit Lead.
- **Text-Einblendungen:** „Audio in“ · „MIDI out“ · „Same melody, new
  sound“.
- **Beleg:** Suite `audio_to_midi_flow`, `audio_to_midi_legato_flow`
  (ohne ONNX skip).

### 7 — Split a mix (`stems`)

- **Feature:** Stem-Trennung — ein fertiger Mix wird in Drums, Bass und
  Rest getrennt, jeder Teil auf eigener Spur.
- **Länge:** 25–35 s.
- **Ausgangsprojekt:** leer; im Ordner `~/music/unidaw-short-stems` liegt
  `mix.wav` = der Render `song.wav` aus Short 1 (eigenes Material).
  Modelle geladen.
- **Prompt:**

  ```text
  Start a new song in ~/music/unidaw-short-stems at 96 BPM with one section called Song that is as long as mix.wav from that folder. Split mix.wav into stems and put each stem on its own track in that section. Then tell me which stems have sound.
  ```

- **Erwartet:** Job läuft im Hintergrund; danach Spuren für die Stems
  (drums, bass, other, vocals) mit Wellenformen; Antwort: Drums und Bass
  klingen, Vocals still (der Song hat keinen Gesang). **Nicht trocken
  getestet.**
- **Bild:**
  1. 0–6 s: Prompt.
  2. 6–16 s: Timeline, beschleunigt: vier Spuren mit Wellenform
     erscheinen.
  3. 16–22 s: Skript schaltet Solo auf Drums, Play (3 s).
  4. 22–28 s: Solo auf Bass, Play (3 s). Endkarte.
- **Ton:** Stem-Datei Drums, dann Bass (aus `media/stems` des Projekts).
- **Text-Einblendungen:** „One mix in“ · „Drums“ / „Bass“ beim Solo ·
  „Stems on their own tracks“.
- **Beleg:** Suite `stems_flow` (ohne Modell skip).

## Aufnahme-Kette

`tools/short/record_short.js` nimmt einen Short ohne Mensch auf. Das
Drehbuch steht als Daten in `tools/short/<short>.json` (Titel, Prompt
wörtlich, Projektordner, Render, Einblendungen mit Zeit, Längen der
Szenen, Zoom der Timeline), je Short eine Datei (`intro.json` …
`stems.json`).

Felder über Short 1 hinaus:

- `base`: Projektordner, den die Kette vor dem Lauf kopiert (ohne
  `.wav`), z. B. der Basis-Song `~/music/unidaw-short-intro`.
- `inputs`: Dateien im Projektordner vor dem Lauf — `{file, make:
  "melody"}` (`make_melody.py`) oder `{file, copy: <pfad>}`.
- `render`: optional; ohne rendert die Kette den Stand am Ende selbst
  (Spitze −1 dB, damit ein übersteuerter Song im Video nicht clippt).
- `scenes`: die Play-Szenen nach dem Agenten, je Szene `seconds`, `audio`
  und wahlweise `node` (`start` = Node der Kopie, `end` = Node am Ende,
  Ton per `renderNode`), `section` (+ `nth`, `from` in Sekunden), `solo`
  (Spurname als Muster), `notes` (Piano-Roll des ersten MIDI-Clips öffnen)
  und `caption`. `audio`: `render`, `node`, `{stem: <spur>}`
  (`renderStems`) oder `{file, offset}`. Ohne `scenes` gilt wie bei
  Short 1 `timing.play`/`timing.playFrom`.
- `answerPick`: Muster für die erste gezeigte Zeile der Antwort;
  `answerCaptions`: Einblendungen aus Werten der Antwort (`{re, text}` mit
  `$1`), nur wenn die Antwort sie nennt.
- `ui.dockH`: Höhe des Docks in px, wenn eine Szene es öffnet.

```bash
NODE_PATH=~/dev/agent-game/suite/node_modules node tools/short/record_short.js intro
node tools/short/record_short.js intro --compose-only   # nur neu schneiden
```

Ablauf:

1. **unidaw isoliert:** eigene Instanz ohne Audiogerät (`--no-audio`,
   `--interface-sim`), freier Port ab 7490, Config, Rezepte und Log im
   Arbeitsordner `~/.cache/unidaw-shorts/<short>/`, Token je Lauf.
   `UNIDAW_BIN` nimmt auch das AppImage; Standard ist der Build des
   Checkouts.
2. **Agent:** `claude -p` mit dem Prompt, `--setting-sources
   project,local`, `--strict-mcp-config` mit einer Kopie des MCP-Servers
   auf diesem Port, nur die beiden unidaw-Tools erlaubt, Start in einem
   leeren Ordner. `SHORT_MODEL` wählt das Modell. Antwort, Dauer und
   Kosten landen in `agent.json`.
3. **UI:** Headless-Chromium (Playwright) mit 960×939 CSS-Pixeln,
   Faktor 1,125 — das ist genau der UI-Bereich des Hochformats. Rechte
   Spalte und Dock sind zu, die Eingangszeile (Gerätename des Rechners)
   ist ausgeblendet. Während der Agent arbeitet, alle 250 ms ein Bild.
4. **Prüfung des Songs:** enthält der End-Node ein VST3/LV2, bricht die
   Kette ab (nur eingebaute Plugins im Bild). Ein kopierter Basis-Song
   zeigt mit absolutem Pfad auf das Drum Kit der Binary, die ihn gebaut
   hat; unidaw biegt die Pfade beim Laden auf das Kit der laufenden Binary
   um (`relinkKitSamples`).
5. **Play:** je Szene Node wählen, Solo, Piano-Roll, dann `play` ab der
   Stelle, 15 Bilder/s. Ohne
   Audiogerät läuft die Transportuhr von unidaw nicht; die Kette meldet der
   UI in dieser Zeit die Position nach der Wanduhr. Der Ton kommt ohnehin
   aus dem Render, Bild und Ton laufen so gleich.
6. **Schnitt (ffmpeg):** Titel und Einblendungen, Prompt-Karte (wird
   getippt), UI-Ausschnitt, Fußzeile „N× speed“; die Wartezeit des
   Agenten auf `timing.agent` Sekunden beschleunigt (Faktor gerundet).
   Ton: ganzer Song leise bis zum ersten Play, dann je Szene ihr Ton
   voll (kurze Blenden an den Schnitten), am Ende ausgeblendet. Endkarte
   3 s. Die Antwort-Zeile ist der Anfang der Antwort des Agenten (oder ab
   `answerPick`), nur ohne Markdown-Zeichen, Tabellen und gekürzt.
   `--compose-only` nimmt Einblendungen und Antwort-Muster aus dem
   aktuellen Drehbuch.
7. **Prüfung:** `ffprobe` muss H.264, AAC und 1080×1920 melden, sonst
   Exit 1. Ablage: `~/StorageBox/agent-daw/agent-daw-short-<short>.mp4`
   (fehlt der Ordner, nur im Arbeitsordner).

Ein vorhandener Projektordner bricht den Lauf ab (der Agent braucht einen
neuen); `--fresh` benennt ihn vorher um. Fehlt der Render am Ende, gibt es
kein Video — Lauf wiederholen oder Drehbuch anpassen.

Die leere UI zeigt den Song, sobald der Agent ihn anlegt — ohne Neuladen
(Todo `2026-11-23-ui-uebernimmt-von-aussen-geoeffneten-song`).

## Stand der Aufnahmen

Alle sieben Shorts sind aufgenommen (Short 1 am 2026-11-23 früh, 2–7 am
2026-11-23 im Todo `2026-11-23-restliche-shorts-aufnehmen`) und liegen als
`~/StorageBox/agent-daw/agent-daw-short-<feature>.mp4`. Abnahme durch
Matthias steht aus.

Kosten des Agenten je Short (Opus): intro 0,39 USD, song-tree 0,49,
listen 0,54, sections 0,39, mix 0,30, audio-to-midi 0,39, stems 0,28.

Abweichungen von den Drehbüchern oben:

- Die UI steht in allen Shorts ganz im Bild (Song-Baum + Timeline), ohne
  Zoom je Szene; die Routing-Kette (Shorts 3 und 5) ist nicht zu sehen.
  Short 6 öffnet die Piano-Roll im Dock.
- Short 2: die Einblendungen „calm“ / „energetic“ stehen oben, nicht über
  dem Node; der Node ist in der UI markiert.
- Short 3: die Einblendung „Chords in the same key“ fehlt; während des
  Plays steht „Key: A minor“ aus der Antwort.
- Short 4: Play ab 15 s im zweiten Chorus bis ins Outro.

## Offen für die Aufnahme-Kette

- Die Kette braucht eine Anmeldung von `claude` ohne Rückfrage. Eine
  Anmeldung im Browser läuft nach einiger Zeit ab (Warnung von `claude
  -p`); für Läufe ohne Mensch ist `claude setup-token` gedacht.
- Short 1 zeigt den Namen der ersten Section („Intro“) nicht — die UI
  blendet ihn aus (Todo `2026-11-23-name-der-ersten-section-fehlt`).
- Antworten weichen je Lauf ab: ein neuer Lauf kann eine andere Tonart,
  andere Spurnamen oder ein Fremd-Plugin bringen. Vor dem Veröffentlichen
  Antwort-Zeile und Einblendungen ansehen.
