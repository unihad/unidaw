# unidaw — Feature-Longlist

Vergleich mit Ableton Live 12, FL Studio 2025, Cubase 14, Logic Pro 11,
Bitwig Studio 5, REAPER 7 und Pro Tools 2025: welche Features unidaw noch
fehlt und was die Nachrüstung kostet. Zweck von unidaw: eine allgemeine DAW —
Songs aller Genres, Filmmusik und Games-Audio; die Liste bewertet auf diesen
Zweck, nicht nur auf Games. Stand 2026-10-13, Erledigt-Vermerke nachgetragen
2026-10-24 (gegen die Op-Liste in `dawd/source/OpRegistry.cpp`). Quellen: unidaw-Op-Liste
(Befehls-API), `tracktion_engine/FEATURES.md`, Produktseiten ableton.com,
image-line.com, steinberg.net, apple.com, bitwig.com, reaper.fm und avid.com
(Stand 2026-10-13).

## Annahmen und Aufwandsmaßstab

- Aufwand = fokussierte Entwicklungszeit mit Agenten (Op = Handler +
  Registry-Eintrag + Test + MCP-Zeile, per `ARCHITECTURE.md`), eine Person
  reviewt.
- **S** < 1 Tag, **M** 1–3 Tage, **L** ≈ 1 Woche, **XL** > 2 Wochen.
  Schätzwerte, ±50 %.
- „TE nativ" = Fähigkeit laut `tracktion_engine/FEATURES.md` schon in der
  Engine — unidaw muss nur die Op/Sichtbarkeit nachrüsten, kein Engine-Code.
  Das ist der größte Aufwandshebel.

## Arrangement & Clips

- **Clips bearbeiten (verschieben, trimmen, splitten, duplizieren, löschen)**
  — M — Erledigt: `moveClip`, `trimClip`, `deleteClip` (852fc45,
  2026-10-13), `splitClip`, `duplicateClip` (4b80b1b, 2026-10-15).
- **Fades/Crossfades an Clips** — S — TE nativ (audio fade curves). Erledigt
  (3ff22d8, 2026-10-15): `setClip` fadeIn/fadeOut mit fadeCurve.
  Crossfades zwischen Clips erledigt (2026-10-25): `setClip` crossfade
  (autoCrossfade der Engine, Überlappung = Blende, folgt Verschieben),
  Marke mit Umschalter in der Lane (Test crossfade_flow).
- **Clip-Gain/Normalisieren** — S — TE nativ (normalise peak/RMS). Gain
  erledigt (`setClip` gainDb, 3ff22d8). Normalisieren je Clip erledigt
  (2026-10-25): `setClip` normalize peak|rms mit targetDb setzt gainDb aus
  dem hörbaren Bereich (Test clip_normalize_flow).
- **Zeit-/Pitch-Stretching (Live Warp, FL Stretch)** — Erledigt
  (2026-10-15, Rubberband nach Decision): `stretchClip` (ratio,
  followTempo mit gemessenem Quelltempo), Audio-Transpose in `setClip`.
  Warp-Marker erledigt (2026-10-26, `warpClip`, nächste Zeile). Ursprünglich: M + Lizenzprüfung —
  TE nativ via Elastique/Rubberband/SoundTouch, aber FEATURES.md markiert
  externe Lizenz; Rubberband (GPL) passt zu dawds GPLv3, Elastique nur
  kommerziell.
- **Warp Time (Clip-Zeitverlauf biegen)** — M — TE nativ. Erledigt
  (2026-10-26): `warpClip` (Marker Quellsekunde → Beat ab Clip-Start,
  ersetzen = setzen/verschieben/löschen, `detect` aus den Beats von
  `detectTempo`, `clear`); seit 2026-10-31 in Echtzeit ohne Datei (ein
  Rubberband-Stretcher, Patch `tracktion-warp-realtime`, ±5 ms;
  `todos/2026-10-31-warp-echtzeit.md`), vorher offline gerendert; der
  Clip folgt danach dem Tempo; Striche im Clip (Tests warp_clip_flow,
  warp_realtime_flow).
- **Comping (Takes zusammenschneiden)** — L — Erledigt (Stand 2026-10-25):
  `compLane` (je Zeitbereich spielt eine Lane, Test comp_flow).
- **Tempo/Taktart pro Section** — M — TE nativ (tempo curves, time
  signatures). Erledigt (2026-10-14): bpm/timeSig je Section (eigen oder
  geerbt), Länge in Takten, Tempospur der Engine aus den Sections,
  Taktlinien im Band. Tempo-Rampen erledigt (2026-10-25): `updateSection`
  ramp — das Tempo gleitet zum Tempo der nächsten Section (Test
  tempo_ramp_flow). Offen: Raster/Snap.
- **Loop-Start/-Offset in Audio-Clips** — S — TE nativ. Erledigt
  (2026-10-26): `loopClip` (Bereich in Sekunden der Datei oder Beats des
  Materials, Offset im Loop, Durchgänge, aus), gestrichelte Linien und ⟲ im
  Clip (Test loop_clip_flow).
- **Marker neben Sections** — S — Erledigt (2026-10-25): `addMarker`/
  `updateMarker`/`deleteMarker` (Zeit oder Takt/Schlag, `state.markers`),
  folgen den Section-Ops, Fähnchen im Lineal (Test marker_flow).

## MIDI

- **Noten nachträglich ändern** — M — Erledigt (32ff8df, 2026-10-18):
  `editNotes` (einzelne Noten ändern, löschen, einfügen), dazu
  `setMidiNotes`.
- **Quantisieren/Groove** — M — TE nativ (groove templates, quantisation).
  Erledigt (25228e2, 2026-10-16): `quantizeClip`, `extractGroove`.
- **MIDI-CC/Controller-Events in Clips** — M — TE nativ (controller/sysex
  events). Erledigt (2026-10-24, Merge 7f612cb): `setControllers` (CC 0..127,
  Pitch-Bend), `clipNotes` liest sie, Controller-Spur in der Piano-Roll.
- **MIDI-Transform/Generate (Live 12)** — M — TE nativ (pattern generation
  bass/melody/chord/arp); für Agenten, die Noten selbst erzeugen, evtl.
  überflüssig.
- **MIDI-Export** — S — TE nativ (MIDI file exporting). Erledigt
  (2026-10-24, Merge 7f612cb): `exportMidi` (Song, Spur, Clip, Section).
- **MPE** — L — für expressive Instrumente in Songs/Filmmusik relevant; erst
  bei Bedarf.
- **Timecode-Sync (MTC, MIDI Clock) für Filmmusik** — S/M — TE nativ (MTC
  in/out, MIDI Clock); als Op nachrüsten.

## Mixer/Processing

- **Pan pro Track** — S — TE nativ (volume & pan Plugin). Erledigt
  (2026-10-14): `trackMix` volumeDb/pan, Regler in der Automations-Lane.
- **Sends/Returns (FX-Busse)** — M — TE nativ (aux send/return). Erledigt
  durch die Routing-Matrix (2026-10-14): Fan-out/Fan-in zwischen Spuren und
  Modulen, Pegel je Kante, Sends im Simple-Mode, Feedback-Schleifen.
- **Gruppen-/Submix-Spuren** — M — Erledigt (Stand 2026-10-25):
  `createGroup`/`setGroup`/`groupMix`, Bus `g<N>` in der Routing-Matrix
  (Test groups_flow).
- **Metering pro Track** — S/M — TE nativ (level meter). Erledigt
  (2026-10-14): `levels`-Poll (je Spur und Main, nicht geloggt), Meter im
  Spur-Header und im Transport.
- **Freeze/Flatten** — M — TE nativ (freeze point). Erledigt (2026-10-24,
  Merge 857f342): `freezeTrack`/`flattenTrack` über den eigenen Render-Weg
  (der Freeze Point der Engine passt nicht zu Ketten hinter AuxSends).
- **Plugin-Presets/State speichern** — M — Erledigt (8ba568c, 2026-10-16):
  `savePreset`, `loadPreset`, `listPresets`; Suche nach Klang mit
  `findPresets`.
- **Sampler-Spur (Live Simpler/Drum Sampler, FL DirectWave)** — M — TE nativ
  (sampler Plugin). Erledigt (2026-10-15): `setSamples`/`listSamples`
  (One-Shot je Taste oder Bereich mit Grundton, Ausschnitt), Built-in "Drum
  Kit" mit eigenem synthetisierten Kit in GM-Belegung. Ausgebaut
  (2026-10-24, Branch sampler): Velocity-Layer, Round Robin, Choke-Gruppen
  (Hi-Hat im Drum Kit), Attack/Release je Sample — Engine-Patch
  `tracktion-sampler-layers.patch`.
- **Sidechain** — M/L — Routing in TE erst prüfen. Erledigt (2026-10-24, Merge
  b70f879): `connect {sidechain: true}` über ein eigenes Plugin-Paar um das
  Rack, ohne Engine-Patch.
- **Stock-Effekte nutzbar machen (EQ, Delay, Compressor, Chorus …)** — S/M —
  Erledigt mit der Routing-Matrix (2026-10-14): Built-ins stehen in
  `listPlugins` (format builtin) und sind als Module routbar.
- **Musik-Analyse (Tempo, Tonart/Akkorde, Aufbau)** — L — Erledigt
  (2026-10-15): `detectTempo`, `detectKey`, `analyzeStructure` — Datei oder
  Mix; der Aufbau liefert fertige `addSection`-Befehle für `batch`; Noten
  aus Audio über basic-pitch (`audioToMidi`, `addMidiFromAudio`, ONNX
  Runtime), Stem-Trennung (`separateStems`). Offen: Drum-Transkription.
- **Analyse-Ops (Referenzsong-Vergleich)** — M — Pegel, Spektrum, Lautheit
  des eigenen Materials gegen einen Referenztrack (Todo 2026-10-11,
  verschoben). Erledigt (2026-10-14): `analyze` misst integrierte LUFS,
  LRA, True Peak, Peak, RMS und die Spektral-Balance (Oktavbänder) für
  Song, Section oder Spur; `compareReference {file}` misst eine Referenz
  (wav/aiff/flac/ogg/mp3) gleich und liefert Differenz und Hinweise.
  Offen: feinere Spektral-Auflösung, Stereobreite.
- **Rack-Patching (FL Patcher)** — L — TE nativ; erstmal nicht bauen.

## Automation

- **Automationskurven für Parameter** — M/L — TE nativ (Bezier-Kurven).
  Erledigt (2026-10-14): Spur (Volume/Pan), Plugin-Parameter, Wet, Kanten-
  Pegel und Feedback über die Songzeit, linear + Stufe, Lane je Spur im
  Flow-UI. Offen: Bezier-Formen, Raster an Section-Grenzen.
- **LFO/Modifier pro Parameter (FL Automation-Editor)** — M — Erledigt
  (Stand 2026-10-25): `setModifier`/`clearModifier` (LFO, Step; Tests
  lfo_flow, step_modifier_flow).
- **Automationsaufnahme** — M — TE nativ; für Agent-first zweitrangig,
  Agenten schreiben Kurven direkt.

## Session/Performance (Live)

- **Session View: Clip-Launching, Scenes, Follow Actions** — L — TE nativ
  (clip launching, scenes, follow actions); widerspricht dem linearen
  Flow-Modell — erstmal nicht bauen.

## Render/Export

- **Node-Bounce als Zweig-Seed** — S — flow-spezifisch: Ausgang eines Knotens
  rendern und als Seed-Audio eines neuen Zweigs nutzen. Erledigt
  (2026-10-25, Branch render-bounce): `bounceNode {node?, section?, tail?,
  label?}` — Kind-Node mit einer Spur "Bounce" und den Sections, collect-
  Aktion mit Datei, Undo; UI: Bounce-Knopf am Node.

- **Render-Optionen (Format, Sample-Rate, Dither)** — S/M — Erledigt
  (2026-10-14): WAV/AIFF/FLAC/OGG, 44,1–96 kHz, 16/24/32 Bit, Dither bei
  16 Bit. MP3 braucht den LAME-Encoder (nicht installiert).
- **Stem-Export** — M — Erledigt (2026-10-14): `renderStems`, je Spur eine
  Datei durch ihr Routing (über allowedClips gefiltert — Klangerzeuger ohne
  Eingang klingen in jedem Stem).
- **Section-/Clip-Render** — S — Section-Render erledigt (2026-10-14:
  `section`, `start`/`end`, Ausklang `tail`). Clip-Render erledigt
  (2026-10-25, Branch render-bounce): `render {file, track, name}` — der
  Clip durch das Routing seiner Spur, ohne Master-Kette (`master: true`
  mit); UI: Clip-Knopf "⤓".
- **Normalisieren/Loudness beim Render** — S — Erledigt (2026-10-14): Peak
  oder LUFS mit True-Peak-Grenze −1 dB.
- **Sound Similarity Search (Live 12, KI)** — XL — nicht bauen.
- **Video im Timeline-Kontext (Filmmusik: Score to Picture)** — L–XL —
  TE bietet kein Video (nicht in FEATURES.md), JUCE-Bausteine vorhanden —
  Engine-Rückstand; nötig, sobald Musik auf Bild produziert wird. Video-Export
  (Trailer) bleibt extern.

## Flow-UI (Mensch-Seite)

- **Waveform-Anzeige/Thumbnails** — M — Erledigt (7d511f4, 2026-10-14):
  `clipPeaks`, Waveform und Noten im Clip.
- **Piano-Roll-Anzeige** — L — Erledigt (7d511f4, 2026-10-14): Piano-Roll
  für neue MIDI-Clips im Flow-UI.
- **Mixer-Ansicht** — M — Erledigt (Stand 2026-10-25): Modus „Mixer“ der
  Routing-Ansicht (Kanalzug je Spur mit Fader, Pan, Solo/Mute, Meter und
  Sends).
- **Library/Browser mit Suche und Preview** — M — Erledigt (2026-10-15):
  `sampleLibraries` + `findSamples` (Index im Hintergrund, Suche über
  Ordner- und Dateinamen), Vorhören und Belegen in der Sampler-Karte.

## Fundament

- **Undo/Redo-Op** — S — Erledigt (2026-10-14): `undo`/`redo {steps?}`
  über Schnappschüsse (Edit + Aktionen des Nodes), nicht über den
  UndoManager der Engine — der kennt die Node-Aktionen und Auto-Nodes nicht.
- **Engine-Bug: Render-Precheck bei Synth-only-Graphen** — kein Feature, ein
  Tracktion-Bug (Todo 2026-10-11, verschoben): Edits mit nur MIDI-Clip +
  Instrument brechen mit „Didn't find any audio to render" ab, obwohl der
  Graph Audio erzeugt. Verdacht: ChannelRemappingNode/DynamicOffsetNode
  überschreiben `hasAudio` statt zu OR-en. Workaround eingebaut: `render`
  mit `force:true`. Erst angehen, wenn der Workaround stört.
- **Engine-Einstellungen (Latenz, Samplerate)** — S — TE nativ. Erledigt
  (aa54c56, 2026-10-17): `audioSettings`.
- **Mehrere Projekte gleichzeitig offen** — S/M.

## Zusätzlich bei Cubase 14, Logic Pro 11, Bitwig Studio 5, REAPER 7, Pro Tools 2025

Nur Ergänzungen gegenüber den Abschnitten oben; das Deckungsgleiche
(Clips, Warping, Comping, Automation, Stems) steht bereits.

**Cubase 14 (Steinberg)**

- **Score/Notensatz (Score Editor)** — L — für Filmmusik relevant (Noten
  für Session-Musiker, Cue Sheets); erst bei Bedarf.
- **VariAudio (Pitch-Korrektur im Audio)** — XL — für Songs mit Gesang
  relevant; erst wenn Gesang in die Song-Arbeit kommt.
- **Chord Track/Chord Pads** — M — TE nativ (chord and scale utilities);
  Agenten erzeugen Harmonien selbst — eher nicht bauen. Erkennen erledigt
  (2026-10-15): `detectKey` hört Tonart und Akkordfolge aus Audio (Datei
  oder Mix) — die Grundlage, um Harmonien einer Referenz aufzugreifen.
- **MixConsole-Snapshots** — M — Verlauf gibt es über events.jsonl;
  Snapshots nur auf Bedarf.
- **Direct Offline Processing** — M — TE nativ (render specific clips); als
  Op nachrüsten. Erledigt (2026-10-26, Branch clip-fx): `processClip
  {track, name, plugins, tail?}` rechnet einen Audio-Clip durch Module in
  eine neue Datei, `clear` holt das Original (`ARCHITECTURE.md`, Clip-FX).
- **Expression Maps (Articulations)** — M/L — für Orchester-/Filmmusik
  relevant (Strings, Bläser); setzt Sampler-Spur voraus.
- **Session-Interchange AAF/OMF** — M/L — für Zusammenarbeit mit externen
  Tonstudios (Film/Post); erstmal nicht bauen.

**Logic Pro 11 (Apple)**

- **Session Players/Drummer (KI-Session-Player, adaptiert zu Akkorden)** — L —
  dawds Agenten sind der Session-Player; nicht bauen.
- **Stem Splitter (KI-Stems trennen)** — XL — Erledigt (2026-10-15):
  `separateStems` (HT-Demucs über ONNX Runtime, Hintergrund-Job mit
  `jobStatus`), vier Stems; Lizenz der Gewichte vor einem Vertrieb klären.
- **Smart Tempo (Tempo aus Audio erkennen/adaptieren)** — L — nützlich, wenn
  Briefings Referenz-Audio mitliefern; Tempo-Erkennung fehlt auch der TE
  (nicht in FEATURES.md) — Engine-Rückstand. Erkennen erledigt
  (2026-10-15): `detectTempo` (Datei oder Mix — BPM, Kandidaten, Beats,
  3/4 oder 4/4, Takt-Eins, Drift), eigene Signalverarbeitung ohne neue
  Abhängigkeit (`MusicAnalysis.cpp`). Audio ans erkannte Tempo anpassen
  erledigt: `stretchClip followTempo` (ein Tempo, 2026-10-15), `warpClip
  detect` (Beat für Beat aufs Raster, 2026-10-26). Offen: Tempo-Verlauf
  (Rampen).
- **Flex Pitch** — XL — nicht bauen.
- **Scripter (skriptbare MIDI-FX)** — M — durch dawds Befehls-API und TEs
  Plugin-API im Wesentlichen abgedeckt.

**Bitwig Studio 5 (Bitwig)**

- **The Grid (modulare Synthese/FX; Poly, FX und Note Grid)** — XL — nicht
  bauen; VST-Hosting deckt modulare Synths ab.
- **Plugin-Sandboxing (Plugins in isolierten Prozessen)** — M/L —
  Infra-relevant: gecrashtes Plugin reißt den Daemon nicht mit; TE hostet
  in-process. Steht als Paket 5 in `docs/vertrieb-und-verkauf.md`.

**REAPER 7 (Cockos)**

- **Routing-Matrix (alles auf alles)** — M/L — TE-Routing erst prüfen.
- **Per-Item-FX / Free Item Positioning** — M — Processing auf Clip-Ebene;
  TE kann Clips einzeln rendern, Item-FX als Op nachrüsten. Gerendert
  erledigt (2026-10-26): `processClip` (siehe Direct Offline Processing).
  Offen: eine Live-Kette je Clip (Engine-Clip-Plugins, L) — nicht gebaut;
  Matthias 2026-10-26: später prüfen (Todo `todos/2026-10-26-clip-fx.md`).
- **ReaScript/Custom Actions (skriptbare Befehle)** — S/M — dawds
  Befehls-API ist die Skript-Ebene. Batch-Op erledigt (2026-10-14): `batch
  {commands, stopOnError?}` — mehrere Befehle in einem Aufruf, je Befehl
  über executeOp, nicht transaktional.
- **Batch-Converter/Bounce im Schwung (auch Pro Tools Bounce Factory)** — M —
  viele Varianten in einem Lauf rendern (SFX, Stems, Alternaten). Erledigt
  (2026-10-25, Branch render-bounce): `renderBatch {dir, nodes?, sections?,
  song?, formats?, stems?, …}` — Nodes x Bereiche x Formate, Mix oder Stems,
  synchron mit Fortschritt in /progress; UI: "Batch" im Export-Popover.
- **OSC-Fernsteuerung** — M — steht schon als Plan in der DAW-Decision
  (Live-Performances per OSC). Erledigt (2026-10-26, Branch
  osc-fernsteuerung): `oscSettings {enabled, port, address}`, standardmäßig
  aus, nur 127.0.0.1; feste Adressen `/unidaw/...` für Transport, Section/
  Marker, Spur-Mix und Plugin-Parameter über die Op-Registry; UI im
  Audio-Popover (`ARCHITECTURE.md`, OSC-Fernsteuerung).

**Pro Tools 2025 (Avid)**

- **Audio Vivid/Atmos (immersive Mischung)** — XL — Stereo-Auslieferung
  zuerst (Portale, Streaming); Atmos erst bei Bedarf (Film,
  Spiele-Streaming).
- **Speech-to-Text im Timeline-Kontext** — XL — KI-Anbieterfrage; nicht
  bauen.
- **Loudness-Metering (LUFS)** — M — Erledigt (2026-10-14): `analyze`
  (EBU R128 über den LoudnessMeter der Engine), LUFS-Normalisieren beim
  Render. Erledigt (2026-10-26): Live-LUFS im `levels`-Poll (Momentary,
  Short-term, Integrated seit Play-Start) am Main-Ausgang, Anzeige im
  Transport (Todo `2026-10-26-lufs-live.md`, Test live_lufs_flow).

Keine der Ergänzungen verdrängt die Top 5; am ehesten agent-nah sind das
Batch-/Makro-Op, der Batch-Render und die Plugin-Sandboxing-Frage.

## Wenn nur etwas kommt: Top 5

1. Clip-Edit-Ops
2. Tempo/Takt pro Section
3. Automation-API
4. Pan + Sends + Metering
5. Render-Optionen + Stems

Zusammen grob 20–30 Agententage (±50 %). Stand 2026-10-24: alle fünf
erledigt. L/XL-Positionen kommen dazu, wenn
der Bedarf da ist: Comping mit Gesang, Video-Sync bei Filmmusik, Warping für
Referenz-Audio, Score-Editor und Expression Maps für Orchester-Arbeit.

## Als Nächstes (2026-10-24)

Ausgewählt von Matthias aus den offenen Punkten oben; je ein Todo:

1. MIDI-CC in Clips und MIDI-Export — `todos/2026-10-24-midi-cc-und-export.md` (erledigt)
2. Sampler ausbauen (Velocity-Layer, Round Robin, Choke, Hüllkurven) —
   `todos/2026-10-24-sampler-ausbauen.md` (erledigt)
3. Sidechain — `todos/2026-10-24-sidechain.md` (erledigt)
4. Freeze/Flatten — `todos/2026-10-24-freeze-flatten.md` (erledigt)

## Als Nächstes (2026-10-25)

Ausgewählt von Matthias; je ein Todo, je ein Agent:

1. Clips & Arrangement (Crossfades zwischen Clips, Normalisieren je Clip,
   Marker neben Sections, Tempo-Rampen) —
   `todos/2026-10-25-clips-arrangement.md`
2. Render & Bounce (Node-Bounce als Zweig-Seed, Clip-Render, Batch-Render) —
   `todos/2026-10-25-render-bounce.md`
3. Plugin-Sandboxing (erst Konzept und Decision-Vorschlag) —
   `todos/2026-10-25-plugin-sandboxing.md` (Konzept erledigt; Matthias:
   Option C, Umsetzung `todos/2026-10-25-plugin-sandbox-umsetzung.md`, XL)

## Eingang (seit 2026-10-29)

Funde während der Todo-Runde (Matthias, 2026-10-29: erst die offenen Todos
auf 0, Neues hier sammeln). Noch nicht bewertet, noch kein Todo.

Bewertet 2026-10-29 (Matthias): Einträge mit Marke sind in Todos
übernommen — [A] `todos/2026-10-29-klang-werkzeuge-pegelschutz.md`,
[B] `todos/2026-10-29-clip-fallen.md`, [D]
`todos/2026-10-29-analyse-messfehler.md`, [E]/[F]
`todos/2026-10-29-api-kleinkram-anzeigen.md`. Ohne Marke: bleibt hier
(u. a. Chop-Werkzeuge, Kataloge/Pflege). Kleinkram-Zeilen gehören zu [F]
bzw. [B] laut Todo.

Bewertet 2026-10-30 (Matthias): [G] `todos/2026-10-30-ui-clips-genau-setzen.md`
(Vorschlag 5 und „[UI] Reste nach U1“), [H]
`todos/2026-10-30-ui-piano-roll-erweitern.md` (Vorschlag 8), [I]
`todos/2026-10-30-ui-knoepfe-sichtbar.md` (Vorschlag 9), [J]
`todos/2026-10-30-ui-export-kleinkram.md` (Vorschlag 11, ohne Projekt,
„-2“-Namen) — alle vier nach dem Bau von Variante B; [K]
`todos/2026-10-30-lfo-wert-im-fader.md`, [L]
`todos/2026-10-30-chords-echte-quinte.md`, [M]
`todos/2026-10-30-test-stabilitaet.md`. Übrige Befunde der Agents A/B/F
und D bleiben hier.

Nachtrag 2026-10-30: [K] erledigt — `moveFader` war richtig
(`settleModifierBase` bereinigt den LFO-Stand), behoben wurde der
Rückfallwert in `syncFaders`. Die −76 dB kamen vom Render ohne Vorlauf →
[N] `todos/2026-10-30-analyse-vorlauf.md` (Entscheidung Matthias: fester
Vorlauf). Fund nebenbei: in der Test-Config gibt es kein Modul „High Pass“
(`unknown plugin: High Pass`).

Nachtrag 2026-10-30: [L] erledigt (Grenze Aktivierung 0,45, nur an
synthetischen Klängen gemessen: Obertöne ≤ 0,44, gespielt ab 0,47 — an
einem echten Song gegenprüfen). Funde des Agents, noch nicht bewertet
(Zahlen und Skripte: Agent Log `todos/2026-10-30-chords-echte-quinte.md`,
`/tmp/claude-1000/chords-quinte/`):
- **Gespielte Oktave C4 über C3 fällt in `describeVoice` weg** (saw12,
  soft3, square): Geister-Filter in `VoiceAnalysis.cpp` greift unter
  Verhältnis 0,6.
- **C3 E4 G4 C5 bekommt ein falsches C4** (Aktivierung 0,41–0,49).
- **Später einsetzende gespielte Oktave/Duodezime über gehaltenem C3
  verschwindet in `shape chords`** — der Filter für späte Obertöne in
  `Voice.cpp` prüft nur das Verhältnis; Vorschlag: dieselbe Grenze 0,45
  (späte Obertöne gemessen 0,31–0,38).
- **`audioToMidi` ohne shape verliert G4 über C3 im Zweiklang:**
  Geister-Filter in `ops/Analysis.cpp`, Grenze 0,75 × Bass, gemessen ~0,72.
- **Klangfarbe mit schwachem Grundton** („lowpiano“) gibt falsche hohe
  Noten 82/87 über C3/F3 in `shape chords`.

Nachtrag 2026-10-30: [M] erledigt (Xvfb-Zeile ganz lesen, liegengebliebene
X-Ereignisse im Plugin-Fenster, Presets aufräumen; unter Last 0/5 → 5/5).
Funde des Agents, noch nicht bewertet (Agent Log
`todos/2026-10-30-test-stabilitaet.md`):
- **Tests lassen Presets im `<config>` der Suite liegen:** `limiter`-Test
  („laut“), `sound_level_guard_flow` (`LevelGuard/*`), `match_tune_flow`
  (`MatchTest/*`) — heute ohne Folgen, aber dieselbe Falle wie [M].
  Vorschlag: Aufräumen als Regel für Tests, die Presets anlegen.
- **150-ms-Nachtrag im Mini-WM** ist vermutlich ein Notbehelf für dieselbe
  Ursache wie die versetzte Fensterlage; entfernen macht den Test strenger,
  braucht eigene Läufe unter Last.
- Ob der X-Ereignis-Fix die Handprüfung „x/y um die Titelleiste versetzt“
  (GNOME) erklärt, ist offen — am Desktop prüfen
  (`todos/2026-10-29-handpruefungen-matthias.md`).

Nachtrag 2026-10-30: [N] erledigt (Vorlauf `min(2 s, start)` in
`renderMixRange`, Limiter messen ab `start`; Decision
`docs/decisions/2026-10-30-analyse-vorlauf.md`). Funde des Agents, noch
nicht bewertet (Agent Log `todos/2026-10-30-analyse-vorlauf.md`):
- **Fenster vor 2 s erben weiter den Plugin-Zustand** (Vorlauf ab 0 s):
  Surge „Cyber Pad“ 0,50–0,56 s: erste Messung −33,6 dB, zweite −27,1 dB;
  `flushAllPlugins` hilft bei Surge nicht. Mögliches Todo: Plugins vor
  jedem Analyse-Render auf Stille setzen; Zusammenhang mit
  `surge-render-schwankt` prüfen.
- **Hüllkurven länger als 2 s** weichen tief in langen Noten weiter ab
  (Noten setzen am Vorlauf-Beginn neu ein); Surge bei 5 s ~−25,4 dB gegen
  −27,6 dB voller Render, im Rahmen der Streuung.

Nachtrag 2026-10-30: Variante B gebaut und gemergt (23c90e5); Nachmessung
stil-1 1366×768: Rasten 8.458 → 1.356, Dock + Referenz 6.275 → 596,
Klicks + Ziehen 1.274 → 1.249, Dateidialoge 39 → 12, Ergebnis im Bild
9 → 85. Dock-Höhe → `todos/2026-10-30-ui-dock-hoehe-ziehen.md`
(Entscheidung Matthias). Funde des Agents, noch nicht bewertet (Agent Log
`todos/2026-10-29-ui-konzept-dock-referenz.md`):
- **Nachmessung unvollständig:** nur stil-1 in 1366×768; 1920×1080,
  nachbau-14 und remix-1 fehlen.
- **Voice/Drums mit eigener Render-Datei** statt Spur („File…“ aus dem
  Konzept) nicht gebaut — 3 Ops im Lauf ohne UI-Weg.
- **Ladeleiste `#busy` oben mittig liegt über der LUFS-Anzeige** (auch vor
  Variante B).
- **Web Awesome macht `label` zeilenbreit und Bedienelemente ~40 px hoch**
  — im Dock behoben, Popover und Baum prüfen.
- **Structure nennt viele Teile „Bridge“** (Testsong: 5 von 12) — Rollen
  prüfen (vgl. `analyzeStructure`-Rollen in Praxistest 3).
- **„Referenz-Bereich einmal setzen“ ungeprüft:** die Treiber setzen ihn
  je Op; erst ein Lauf mit Agenten, die das neue UI kennen, zeigt es.

- **Kurznotizen fürs Kernset ergänzen** — S — ohne Kurznotiz
  (`dawd/plugin-profiles/notes/`): Helm, MDA Overdrive, MDA Bandisto,
  MDA Transient, LSP Clipper Stereo, eingebaute Effekte (Reverb, Delay,
  Compressor, Equaliser, Low Pass, Pitch Shifter). Kurznotizen braucht
  Matthias' Abnicken (Todo Plugin-Steckbriefe, Abschnitt 4).
- **Steckbrief-Abschnitt „Erfahrungen“** — S — Erfahrungen aus den Runs
  (Baum-Notizen, Auswertungen) in den Steckbrief übernehmen, nur zwischen
  den Tests (Todo Plugin-Steckbriefe, „Später“).
- **ML-Schichten und mega53** — M — Pegel `describeLayers` gegen
  `compareClasses` (1n P9), split-Schichten 1,6 LU zu leise (1k P4),
  mega53-Drums schwach, Chunk 10 gegen 20 s, Surge `format ""` beobachten.
  Zuerst die Lizenz der mega53-Gewichte klären. Lange CPU-Läufe, ~10 GB RAM
  je ML-Test (Paket 14 aus `todos/2026-10-24-offene-befunde-praxistests.md`).
- [A] **Schwellen und Gewichte kalibrieren** — M — matchSound/tuneSound und die
  Artikulations-Hinweise in compareReference sind gewählt, nicht kalibriert;
  braucht echte Läufe (Praxistests) und Hörurteile
  (`todos/2026-10-18-analyse-musikalitaet-und-sounddesign.md`).
- **Klangkatalog für Diva** — M — die Erkundung braucht eine Lösung für die
  Modulwahl: das Oszillator- und Filtermodell ändert die Bedeutung vieler
  Parameter (Ideen im Todo: Modulwechsel als eigene, seltene Sprünge).
  Erst Gestaltungsidee, dann bauen (`todos/2026-10-17-klangkatalog.md`).
- [D] **`audioToMidi shape chords`: Reste** — S–M — (Agent voice-reste,
  2026-10-29, Repro in kurzen Testdateien) ohne `bpm` meldet eine kurze
  Datei (≤ 4 s) „no notes found in the source“, eigentlich fehlt das
  Taktraster; ein ausklingender Klavierton gibt einen zweiten Anschlag; ein
  G-Akkord nach F rutscht eine 16tel zu spät (basic-pitch setzt den Einsatz
  früher); die Oktave C3+C4 wird zu einem C (Oberton-Filter nimmt C4).
- [D] **Glide bei einzelnen Tonschritten nicht erkannt** — S — E4→G4 (im
  Bericht „Ganzton“ genannt, ist eine kleine Terz; nachprüfen) mit 80 ms Legato: weder
  `pitchShape` noch `legatoGlideMs` erkennt den Glide, `glideShare` 0,71
  statt 1 (Agent voice-reste, 2026-10-29).
- [D] **detectTempo: Reste** — M — (Agent tempo-ohne-snare, 2026-10-29) nur
  Kick ohne Bass und Akkorde ergibt meterConfidence 0,89, obwohl keine
  Takt-Information da ist; auf einem Referenzsong (130 BPM) haben 6 von 34
  Fenstern à 15 s ein falsches Tempo (87–90, 101 oder 151 BPM); die
  Hochband-Stimme in Schritt 3b setzt Hats auf den Vierteln voraus und
  folgt ohne Drums den Anschlägen tonaler Instrumente; Drums ohne Bass
  (`make_drums` bass=False) setzen die Takt-Eins 1–2 Schläge daneben (Kick
  1/3, Four-on-the-floor, Standard-Kick; schon vor dem Fix vom 2026-10-29);
  der Phasen-Hinweis greift seit der Verschärfung nur ohne Drums, echte
  Halb-Zählzeit-Fehler mit Drums (3 im Drum-Stem) bleiben ohne Hinweis.
- [M] **Test-Abhängigkeit `match_tune_flow` nach `preset_flow`** — S — läuft
  `preset_flow` vorher im selben Lauf, findet `match_tune_flow` 7 statt 6
  Kandidaten (vermutlich ein übrig gebliebenes 4OSC-Preset); allein grün,
  auch mit dem Binary von main (Agent surge-werte, 2026-10-29).
- **`PluginRandomness` liest das Patch-XML** — S — Grund war, dass
  `listParameters` nach `loadPreset` alte Werte zeigte; seit 2026-10-29
  (Abgleich Controller/Komponente) behoben. Umstellen auf die Parameter
  prüfen, nur wenn es etwas vereinfacht.
- **Klang-Kataloge weiter erkunden** — S + Läufe — Stand 2026-10-29: Surge
  1046 Klänge (zuletzt 20 % neu), Helm 2745 (12 % neu), gesättigt erst
  unter 2 %, geschätzt je ≥ 1 h (`nice -n 15 taskset -c 8-15`, nicht
  während Praxistests). JX10 und Dexed noch nicht mit dem Fix vom
  2026-10-29 fortgesetzt (`todos/2026-10-17-klangkatalog.md`).
- **Stabile Klang-Ids** — S — ein Klang verliert seinen Id, wenn ein anderer
  ihn aus dem vollen Fach verdrängt; ein gemerkter Id gibt dann „no such
  sound“. Falls Ids halten sollen: `loadPreset {sound}` nimmt auch
  Basis-Preset plus Parameter.
- [F] **`findPresets` über 500 Einträge** — S — `limit` 5000 lieferte nur 500
  (Kappung vermutet, nicht geprüft); Surge hat jetzt 4054 Einträge. Seiten
  oder klare Grenze in der Doku.
- **Befunde Praxistest 2 (remix-1, 2026-10-29)** — aus dem Event-Log von
  dawd-7400 und dem Bericht des Lauf-Agents, Lauf
  `$AGENT_DAW_RUNS/remix-1`; noch nicht bewertet:
  - [A] **`tuneSound`/`matchSound` belohnen einen fast stummen Bass** (wichtig):
    `tuneSound` am Surge-Bass gegen den Original-Bass-Stem stellte Hochpass
    73 → 1046 Hz bzw. Cutoff 161 → 13,75 Hz ein, Bass solo −51 LUFS statt
    −18,6, Distanz trotzdem „besser“ (1,95 → 1,84 / 1,65); `matchSound`
    schlug einen Bass mit −50,5 LUFS vor. Die Distanz bestraft fehlenden
    Pegel/Bassbereich nicht, und `tuneSound` wendet das Ergebnis
    automatisch an — Schutz gegen Pegelverlust fehlt.
  - [E] **LFO-Bereich falsch angezeigt?** LFO-Tiefe 0,3 auf der Pad-Lautstärke:
    auf dem Schlag −76 dB RMS, angezeigtes Minimum −15,7 dB.
  - [F] **`saveRecipe`-Steps:** verlangt je Step `{text}` (oder `op`), Strings
    gehen nicht; erster Versuch scheiterte mit „step 1 needs op or text“,
    die Beschreibung sagt es nicht (zweiter Versuch gespeichert).
  - [A] **`matchSound` im Remix:** absichtlich anderer Rhythmus sättigt die
    Hüllkurven-Distanz (3,0 bei allen Kandidaten) und überdeckt den Klang;
    Option „nur Klang“ oder Hinweis. Gegen einen gemischten Stem (4-Stem
    other) schlägt es eine Bass-Gitarre für ein Pad vor.
  - [D] **detectTempo bei House** (Kick auf allen Vierteln, Clap 2/4,
    Offbeat-Hats/-Bass): Schlag auf den Offbeats (Hinweis kam); gehört zu
    „detectTempo: Reste“ oben.
  - [B] **Auto-Section beim Choppen:** `addClip` mit einem ganzen Stem legt
    jedes Mal „Section 1“ (32–220 s) an, der Agent löschte sie zweimal.
  - [B] **Zweites `addClip` auf derselben Spur** legte eine Lane FX.2 an und
    schaltete FX.1 stumm, gewollt war ein zweiter Clip.
  - **Rückwärts an gewarpten Clips** geht nicht („clear the warp markers
    first“) — Lücke beim Choppen.
  - [E] **`trackMix` meldet den Fader mit LFO-Stand** (−5,6 bzw. −4,71 dB statt
    −1 dB). Der Step-Modifier hat keine Glättung (Klicks).
  - **Chops sind umständlich:** `addClip` hat keinen Quell-Ausschnitt; jeder
    Chop braucht `duplicateClip` + `trimClip` + `moveClip`.
  - [D] **`compareDrums {file}` rastert die eigene Datei im Tempo der
    Referenz** (90 statt 120 BPM, Muster verzerrt); mit `section`/`track`
    stimmt es. Doku sagt es nicht.
  - [F] **`listAutomatable`:** Spurname als `target` gibt eine leere Liste ohne
    Fehler (nötig `t1`); ohne `target` 119 000 Zeichen.
  - **`setSamples update` kann die Datei nicht tauschen** — für eine andere
    Kick das ganze Kit neu senden.
  - [F] **`jobStatus {wait}` flutet das Event-Log** (~10 Einträge/s, 1123 in
    2 min `separateStems`).
  - [D] **`analyzeStructure` auf einem Vocal-Stem** liefert 139 BPM, 3/4, 149
    Takte ohne Warnung; auf dem Mix nennt es den Refrain bei 21,4–58,6 s,
    der Agent wählte nach `findRepeats` und Gesangspegel 58,6–79,9 s (wer
    recht hat: Matthias hört); erste Takt-Eins Mix 26,7 s (detectTempo) gegen
    0,18 s (analyzeStructure).
  - [D] **`findRepeats` über den ganzen Song** (0–219 s) liefert nichts — Hinweis
    „Teil = ganzer Song“ fehlt.
  - Kleinkram: `treeNote {note}` → „no text“ (Feld nennen oder Alias);
    `addPlugin at` ohne `after`, mit `track` ohne `at` Fehler; `warpClip
    mode` nennt seine Werte nicht in der Beschreibung; `setMidiNotes copies`
    ist keine Wiederholungszahl (verwechselbar); `sampleLibraries add` bei
    schon angemeldeter Bibliothek `ok:false`; fünf `trackMix` in einem
    `batch` ergeben fünf Nodes; `jobStatus` ohne `job` liefert alle Jobs
    (sehr lang); ein `addPlugin` landete ohne neuen Node im schon
    gerenderten Endstand-Node.
- **Befunde Praxistest 3 (stil-1, 2026-10-29)** — aus dem Event-Log von
  dawd-7400 und dem Bericht des Lauf-Agents, Lauf
  `$AGENT_DAW_RUNS/stil-1`; noch nicht bewertet:
  - [B] **Überlappender Clip legt still eine Lane an** und schaltet die alte
    stumm — zweites Mal nach remix-1 (Bass.2); echte Falle, mindestens ein
    Hinweis in der Antwort.
  - [D] **`detectTempo`/`analyzeStructure` auf dem eigenen Render:** „beats drift
    59–99 ms“ und „bar start uncertain“ bei quantisiertem MIDI mit 108 BPM —
    Messfehler, nachstellbar mit einem eigenen Render.
  - [D] **`analyzeStructure`-Rollen:** auf der Referenz alles „chorus“ (A/A/A),
    auf dem eigenen Song Refrain und Bridge zeitweise zu einem Teil (A16)
    verschmolzen.
  - [B] **`startBeat` zählt je Op verschieden:** `addMidiClip` ab dem Start der
    Section, `duplicateClip` ab der Section des Quell-Clips — zwei Clips
    landeten falsch (mit Auto-Section), danach „start lies outside every
    section“; Beschreibung sagt es nicht. `addMidiClip {section: "Refrain",
    startBeat: 0}` in einem `batch` → „start must be >= 0“, ohne `startBeat`
    ging es.
  - [B] **`addSection`/`moveSection {after}`** wirkt nur mit `position: "after"`,
    sonst landet die Section am Ende; `moveSection` gab alte Zeiten zurück,
    danach überlappten „Refrain 2“ und „Outro“ samt Clips (erst `undo 8`
    half).
  - [B] **Clip-Gain auf MIDI-Clips wirkt kaum** (−8 dB an Pad/Bass/Drums ≈ 3 LU,
    Pad −13 dB → Outro nur −0,3 LU; Ausweg Automation der Kante
    `edge:p14>main`)
    — vermutlich gleichen Limiter in Spur-Ketten und Master aus; prüfen,
    sonst Hinweis.
  - [F] **`compareReference {articulation: true}` über den ganzen Song ~2 min**
    und blockiert die API so lange.
  - [D] **`describeDrums` und `compareDrums` widersprechen sich** an derselben
    Spur (Snare-Rauschen 0,74 gegen 0,02; Hats wechseln die Klasse).
  - [B] **`duplicateClip`-Kopien** zeigen ein gemeinsames `pattern`, sind aber
    unabhängig: `setMidiNotes` am Original ändert sie nicht.
  - [E] **MDA Image auf dem Master** machte die Tiefen 15–25 dB breiter
    (Korrelation 0,68 → 0,52), auf Spuren normal — Ursache unklar.
  - [B] **Baum:** `deleteClip`/`setClip` legen eigene Nodes „…: Noten“ an; ein
    `trackMix` landete ohne `autoNode`-Hinweis im bestehenden Node, die
    nächste Notiz überschrieb die vorige.
  - [A] **`compareVoice` empfiehlt Glide über „Legato“/„Voice Mode“ bei 4OSC**,
    das keinen Portamento-Parameter hat.
  - Kleinkram: `addSection` ohne Position per Index („first, last or
    after“); `setSamples gainDb` −60 abgelehnt (−48..48), ein `mute` fehlt,
    zwei Samples auf einer Taste schichten nicht; `listParameters
    {paramQuery: "a|b|c"}` leer ohne Hinweis; `trackMix` meldet als `fader`
    das letzte Plugin der Kette; `jobStatus wait: 240` kehrt nach ~60 s
    zurück (Grenze nicht beschrieben); `state {detail}` ignoriert den
    Parameter, eine kompakte Abfrage nur der Sections fehlt;
    der Agent rief `detectSongBoundaries {file: "/nonexistent.wav"}`, um eine
    Op auszuprobieren (Beschreibung lückenhaft?); `treeNote {}` → „no text“.
- **Befunde der Agents A/B/F (2026-10-29)** — noch nicht bewertet:
  - **`matchSound {sameRole}`** — Kandidaten nur aus der Klasse des eigenen
    Klangs; erst nach einem Praxislauf (Entscheidung Matthias 2026-10-29:
    jetzt nur Hinweise), CLAP-Klassen streuen bei Synths.
  - **4OSC-Steckbrief „Legato“** (= Gleitzeit 0–500 ms) steht als „no
    audible effect“, weil im Poly-Modus gemessen; im Mono-/Legato-Modus neu
    messen. `paramQuery "glide"` findet den Regler nicht — Synonym
    glide/portamento → Legato.
  - **Gemischte Referenz:** der Hauptbereich folgt dem lautesten Instrument,
    Bass-Kandidaten bekommen dann die `level`-Strafe; Schwellen nicht
    kalibriert. CLAP vergleicht den ganzen Render — bei anderem Rhythmus
    verlängert `tuneSound` das Release, um Pausen zu füllen.
  - **Glättung für Step-Modifier an Plugin-Parametern** — erst bauen, wenn
    ein hörbarer Klick belegt ist (am Fader glättet die Engine 15 ms).
  - [K] **LFO-Ausreißer nach `treeCheckout`:** `analyze {track}` maß einmal
    −76 dB statt ~−30 dB, danach stimmte es; vermutlich Einschwingen im
    Sandbox-Host. `Routing.cpp:774` kopiert beim Umziehen des Faders
    `getVolumeDb()` mit LFO-Ausschlag — prüfen.
  - **Baum-Regel „Node mit Notiz ist abgeschlossen“** fehlt: gleiche Sorte
    und Fokus landen nach einer Abschluss-Notiz trotzdem im selben Node.
  - **Pegel je MIDI-Clip** fehlt (Clip-Gain skaliert nur Velocity; MIDI-Lanes
    ohne eigenen Fader) — heute nur über Automation.
- **Befunde des Agents D, Runde 1 (2026-10-29)** — noch nicht bewertet:
  - **Drum-Detektor:** eine tiefer gestimmte, laute geschlossene Hat (+8 dB)
    zählt als Snare (90 Schläge, Rauschen 0,74).
  - **`detectTempo` auf einem Vocal-Stem:** Hinweis sagt „4 beats per bar“,
    `timeSig` ist 3/4 — Widerspruch.
  - **Exporte nachbau-10 bis -14:** erkanntes Raster ~0,27 Schläge neben dem
    Projekt-Raster; Ursache nicht geprüft.
- **Reste nach Todo `2026-10-29-analyse-messfehler` (Runde 2, Agent D)** —
  ehrlich als nicht sauber lösbar dokumentiert, Zahlen im Agent Log:
  - Takt-Eins bei Drums ohne Bass (auch Four-on-the-floor mit Clap, nur
    Kick) oft 2 Schläge daneben (Drum-Stem 9 von 27); kein Zweifels-Maß
    trennt sauber.
  - House mit lauten Offbeat-Hats + Offbeat-Bass: Schlagerkennung rastet
    auf die Offbeats ein, keine Stimme widerspricht (remix-1
    zwischenstand-2/3).
  - Falsches Tempo in 3 Fenstern des Referenzsongs (120, 232,5, 240 s).
  - Halb-Zählzeit-Fehler mit Drums ohne Hinweis (3 von 5; ein Hinweis bei
    schwachem Rückschlag träfe auch 9 von 50 richtigen).
  - [L] **Neu:** `shape: "chords"` entfernt mit dem Duodezim-Filter eine
    wirklich gespielte G4 über C3 (C3 C4 E4 G4 → C E).
- [M] **Fenster-Tests wackeln unter Last** — S — `plugin_window_real_flow`
  (Dexed, Auto-Branch: Lage 355/210 statt 360/240) und
  `plugin_window_x_error_flow` (`XOpenDisplay :90`) waren am 2026-10-29
  bei Load ~11 rot, allein je 3/3 grün; Mini-WM auf Xvfb zu langsam?
  Hängt evtl. mit der Handprüfung „x/y um die Titelleiste versetzt“
  zusammen.
- **[UI] Befunde UI-Nachstell-Lauf 1 (2026-10-29)** — drei Agent-Läufe im
  Flow-UI nachgespielt (1920×1080 und 1366×768), Bericht und Zahlen in
  `$AGENT_DAW_RUNS/ui-replay-1/BERICHT.md` und `TABELLEN.md`, Werkzeug
  `tools/ui_replay/`, Todo `todos/2026-10-29-ui-nachstell-lauf.md`. Kern:
  73–77 % der Ops gehen im UI, 15–17 % ohne UI-Weg, 7–12 % scheitern;
  85 % des Scrollens im Dock (Ø 41 Rasten je Dock-Op); nur 9–17 von
  77–109 Schritten ohne Scrollen/Wechsel/Hover. Bewertet 2026-10-29
  (Matthias): Fehler (U1) → `todos/2026-10-29-ui-fehler-nachstell-lauf.md`;
  Vorschläge 1, 6, 7, 10 und „ohne UI-Weg“ (U3/U4) → erst Konzept,
  `todos/2026-10-29-ui-konzept-dock-referenz.md`; Vorschläge 4, 5, 8
  (U2/U5) und 9, 11 sowie „ohne Projekt“ (U6) bleiben hier:
  - Fehler und Sackgassen:
    - **Spur umbenennen geht nicht** (Doppelklick trifft nach Neuzeichnen
      ins Leere, 20 von 21); **Plugin umbenennen kaum** (31 von 39).
    - **Tastenkürzel feuern in Namensfeldern** (globaler Handler nimmt
      `contentEditable` nicht aus: Leerzeichen kürzt „Bass Synth“ zu „Bass“;
      ohne Feld startet `R` eine Aufnahme, `M` setzt einen Marker).
    - **Voice, Drums, Match, Tune und Rezept-Apply ignorieren MIDI-Spuren**
      (lesen `selectedTrack`, ein MIDI-Spurkopf setzt nur
      `selectedMidiTrack`): 50 Ops kamen nicht zum Befehl, Match/Tune im UI
      praktisch tot, Rezepte legen neue Spuren an.
    - Warp-Griffe sperren das Trimmen an Clip-Kanten; ganz überdeckte Clips
      sind nicht anklickbar; Parameterkarte zeigt höchstens 80 Parameter
      (Surge XT: über 600 unerreichbar); [J] ohne Projekt keine Analyse (nur
      Ordnerdialog); [J] dieselbe Datei zweimal → „vocals-2“.
  - Vorschläge nach Nutzen: (1) Dock entzerren, (2) MIDI-Ziel für
    Referenz-Knöpfe und Rezepte, (3) Umbenennen reparieren, (4) Parameter als
    Zahl mit Suche in der Karte, (5) [G] Clips einrasten, Start/Länge als
    Felder, (6) Analyse-Panel statt 3,2-s-Toast, mit Bereich, (7)
    Referenz-Bereich einmal setzen für alle Knöpfe (Merklast: 112.74
    sechsmal getippt), (8) [H] Piano-Roll mit 0–127 und Velocity, (9) [I]
    Hover-Knöpfe sichtbar und ≥ 24 px (Spur-Meter als Analyse-Knopf 4 px
    hoch), (10) Toast weg vom Dock (1366×768: 161 verdeckte Ziele), (11) [J]
    Export: Tail nur 0/2/5/10 s, Dateiname fest.
  - Ohne UI-Weg: `describeVoice`, `describeTimbre`, `describeDrums`,
    `editNotes`, `compareStems`, `audioToMidi`, `analyze` mit Zeitfenster
    oder Datei, Sample-Gain/-Pan/-Name.
  - Werkzeug: Läufe laden jede eingefügte Datei nach `media/` hoch — Lauf 1
    füllte am 2026-10-29 ~17:10 die Platte; nächstes Mal Projekte im tmpfs
    oder aufräumen.
- [G] **[UI] Reste nach U1: Gegenprobe Clips (2026-10-30)** — `replay.js --only`
  mit den Ops von Fehler 6, 1920×1080, vorher/nachher (Todo
  `todos/2026-10-29-ui-fehler-nachstell-lauf.md`). Kopie an derselben Stelle
  (remix-1) jetzt erreichbar (0/7 → 7/7). Offen, noch nicht bewertet:
  - **Clip unter zwei Nachbarn** (stil-1, 0/5): ein Clip liegt unter zwei
    späteren Clips zusammen (z. B. „Bass Refrain“ unter „Bass Strophe 2“,
    14 px versetzt, und „Bass Strophe 3“) — kein einzelner deckt ihn, also
    keine Marke „+N“. Ops 3822, 3924, 3926, 4010, 4165.
  - **Fade-Popover nicht erreichbar** (remix-1 #1358 `setClip`): Feld
    „Fade out s“ lässt sich nicht ins Bild scrollen.
  - **Kopie lässt sich nicht verschieben** (remix-1 #1360–1362): nach ⧉ auf
    dem verdeckten Clip findet der Treiber den Griff der Kopie nicht mehr
    („drag handle gone“); Ursache offen (UI oder Treiber).
- **Katalog-`glideMs` hochrechnen** — S + Neumessung — seit 2026-10-29
  rechnet unidaw `glideMs` auf den ganzen Tonschritt hoch (= Portamento-Knopf),
  Python `legato_glide` (`tools/profile_plugin.py`) misst weiter nur den
  Mittelteil (4OSC 80 ms → ~45 ms). Betroffen 9 Preset-/Klang-Kataloge;
  heute liest kein Code diese Werte. Mitziehen, wenn die Kataloge ohnehin
  neu gemessen werden (Todo `2026-10-27-describevoice-glidems-zu-lang.md`).
- **[UI] Funde aus Todo [G] Clips genau setzen (2026-10-30)** — noch nicht
  bewertet (Agent Log `todos/2026-10-30-ui-clips-genau-setzen.md`):
  - **Lineal-Cursor rastet auf 1/20 s** (13.35 statt 13.333) — im Beleg von
    [G] genannt, aber nicht gebaut.
  - **Start und Länge zugleich im Popover = zwei Undo-Schritte**
    (moveClip + trimClip).
  - **Tempo-, Export- und Instrument-Popover bleiben nicht sicher im Bild**
    — gelöst nur Clip- und Rezept-Popover.
  - **Fragen an Matthias aus [G]** — entschieden 2026-10-30: Raster nach
    Zoom, Kopie bleibt, Felder auch „bar 3.2“ →
    `todos/2026-10-30-ui-clips-raster-zoom-takt-felder.md`.
- **[UI] Funde aus Raster/Felder und Modus-Umschalter (2026-10-30)** —
  noch nicht bewertet (Agent Logs
  `todos/2026-10-30-ui-clips-raster-zoom-takt-felder.md`,
  `todos/2026-10-30-ui-modus-umschalter-weg.md`):
  - **Checkboxen im Clip-Popover** (Follow tempo, Reverse, Loop …) sehen
    wie leere Balken aus (auch vorher).
  - **Input-Strip fragt jetzt in jedem Node den Eingangspegel ab**
    (~6,7×/s, reine Lese-Op), vorher nur in Collect.
  - **Leerer Auto-Node nach Record:** ein Take ohne Noten bucht keine
    Aktion, der Kind-Node „collect N“ aus Auto-Branch bleibt leer im Baum.
  - **Kein ganzer Replay mit den neuen Treibern** (Raster nach Zoom) —
    Zahlen für gesparte Zoom-Klicks fehlen.
- **[UI] Funde aus [I] Knöpfe sichtbar (2026-10-30)** — noch nicht bewertet
  (Agent Log `todos/2026-10-30-ui-knoepfe-sichtbar.md`):
  - **Leiste neben einem gewählten schmalen Clip deckt beim Nachbarn oben
    24 px ab** — in einer Lane-Zeile bleiben 8 px greifbar.
  - **Node-Name und Marke „+N“ sind Klickziele < 24 px** (Node-Name 9× im
    Nachstell-Lauf).
  - **Schmale Seitenleiste (1366):** „⋯“ kürzt die Namen eingerückter Nodes
    früher („Varian…“).
  - **`UI_FILE` auf eine Kopie ohne `vendor/`** gibt falsche Fehler im
    Fehler-Check (README ergänzt; ein Hinweis im Skript fehlt).
  - **Nachstell-Lauf nur remix-1 1920×1080** — stil-1 und nachbau-14 fehlen.
- **[UI] Plugin-Suche** — Matthias, 2026-10-30 — Plugin hinzufügen geht nur
  über ein Auswahlmenü nach Klasse (`flow.html`, Plugin-Auswahl im Routing),
  das Instrument einer Spur über eine Liste (`openInstrumentPop`); kein
  Suchfeld, nur Sprung über den Anfangsbuchstaben. Wunsch: Suchfeld statt
  Menü. Daten liegen schon vor (`listPlugins`). Mit Vorhören je Treffer,
  bevor das Plugin ins Projekt kommt (Matthias) — offen, worauf: ein
  Instrument mit ein paar Tönen, ein Effekt auf einem Beispiel oder der
  gewählten Spur; eine Vorhör-Op für Plugins gibt es noch nicht (vgl. die
  Rezept-Hörprobe).
- **[UI] Sample-Suche außerhalb des Samplers** — Matthias, 2026-10-30 — die
  Bibliotheks-Suche (`findSamples` nach Name oder Klang, ≈ ähnliche) gibt es
  nur in der Plugin-Karte eines Samplers/Drum Kits (`samplerSection`); ein
  Sample direkt in die Timeline zu holen geht nur über eine Datei vom
  Rechner (Drop/Insert), ohne Suche in der Bibliothek. Mit Vorhören je
  Treffer (Matthias) — im Sampler gibt es das schon (▶, `previewSample`).
- **[UI] [API] Track löschen** — Matthias, 2026-10-30 — es gibt weder im
  Flow-UI noch als Op einen Weg, eine Spur zu löschen (`OpRegistry.cpp`:
  nur `createTrack`, `renameTrack`, `armTrack`, `freezeTrack`,
  `flattenTrack`). Heute nur Undo direkt nach dem Anlegen, einen älteren
  Node auschecken oder die Spur stumm schalten. Gelöscht wird bisher nur
  der Clip (Entf, Toast mit Undo).
- **[UI] Zoom in der Piano-Roll** — Matthias, 2026-10-30 — die Rolle hat
  einen festen Maßstab (12 px je Sechzehntel und je Zeile); in einen Clip
  hineinzoomen geht nur über den Zoom der Timeline (− / +, Ctrl + Mausrad,
  Tasten +/−). In der Rolle fehlen waagrechter und senkrechter Zoom (vgl.
  [H]-Fund: in 1366×768 nur ~7 Zeilen sichtbar).
- **[UI] Funde aus [H] Piano-Roll (2026-10-31)** — noch nicht bewertet
  (Agent Log `todos/2026-10-30-ui-piano-roll-erweitern.md`):
  - **In 1366×768 nur ~7 Zeilen Rolle** — die INPUT-Leiste über dem Dock
    nimmt ~70 px.
  - **Rolle nur Raster 1/16** — Noten daneben werden beim Zeichnen
    quantisiert (stil-1: 12 von 24 `addMidiClip` teilweise).
  - **Notenlänge einer vorhandenen Note nicht änderbar** (`editNotes`
    `beats`; nur löschen und neu zeichnen).
  - **Doppelklick auf einen gewählten Clip kann die Knopfleiste treffen** —
    Q quantisiert dann sofort, ⧉ kopiert (im Nachstell-Lauf so passiert,
    Treiber jetzt angepasst).
  - **MIDI-Clip hinter dem Ende seiner Section** (stil-1 #3808: Break,
    Beat 64) geht im UI nur über den Cursor; der rastet auf 1/20 s, das UI
    zählt den Clip zur nächsten Section.
  - **Checks „C“ (Raster je Zoom) in `fehler_check` einmal rot**, danach
    grün — vielleicht wackelig. Am 2026-10-31 wieder: ein Lauf 1920×1080
    7× rot (Einrasten, Shift, Ctrl-Kopie, Raster je Zoom), zwei
    Wiederholungen grün. Dazu einmal Abbruch in F2 (Routing-Zeile noch
    nicht gezeichnet, `center` ohne Box).
- **[UI] Funde aus der Handprüfung 2026-10-31** — noch nicht bewertet
  (Agent Log `todos/2026-10-29-handpruefungen-matthias.md`):
  - **Clip-Popover verwirft Änderungen ohne Hinweis**: Häkchen „Loop“
    gesetzt, Popover per Klick daneben geschlossen → nichts übernommen
    (nur „Apply“ oder Enter); Matthias verlängerte danach den Clip ohne
    Loop.
  - **Loop-Leiste nur 10 px hoch** (unter der 24-px-Regel): Ctrl + Ziehen
    knapp darüber trifft den Clip-Körper und kopiert den Clip, statt den
    Loop-Start zu setzen (so passiert).
  - **Echtzeit-Warp hinter einem Abschnitt an der 4×-Grenze ~9 ms spät**
    (gemessen 2026-10-31, `warp_realtime_flow`): nach einem stark
    gestauchten Abschnitt liegen der Marker und alle folgenden Einsätze
    konstant ~9 ms hinter dem Beat, davor ±1 ms; bei ×1,33 ±5 ms. Der
    Test lässt dahinter ±12 ms zu. Zusammen mit dem Fund des Agenten
    (gedehnte Clips 5–15 ms zu früh, Agent Log
    `todos/2026-10-31-warp-echtzeit.md`) erst angehen, wenn man es hört.
  - **Surge-Namen nach Host-Neustart** (Sandbox-Prüfung): 29 Parameter
    zeigen bis zum ersten Audio-Block Namen aus dem Standard-Patch
    („M1: -“ statt „M1: Release“), Werte gleich; nach `treeOpen` richtig.
    Nur ohne Gerät gemessen. Skript `…/werkzeuge/sandbox-check/
    probe_surge_names.py` (handpruefung-1).
  - **Toast nach „Reload“ überschrieben**: „Diva reloaded“ wird nach 65 ms
    vom alten Absturz-Text ersetzt (`sandboxChanged` nach `sandboxSeq`).
  - **Fortschritt bei Match/Tune springt** (Matthias): die Leiste oben
    zeigt jeden einzelnen Render und fängt immer wieder von vorn an — man
    sieht nicht, ob der ganze Job noch läuft oder etwas anderes startete.
    Vorschlag: eine Leiste für den Job (Render i von n, Sekunden), die
    Render-Schritte darunter.
  - **Match lädt zu Schlechterem ein**: Referenz Bass-Stem „Irgendwo“
    58,6–79,9 s, Surge XT — der aktuelle Klang war am nächsten (1,51), der
    geladene Kandidat schlechter (1,82) und 24 dB leiser. Vorschlag: ist
    „current sound“ der beste, sagt das UI es deutlich und bietet „Load“
    nur an Besseren an (oder markiert Schlechtere).
  - **Pegel der Treffer**: Kandidaten 24–29 dB leiser als die Referenz im
    Hauptbereich stehen in der Liste; Tune endete −25,5 dB (vorher −23,7)
    bei 7 % Abstand-Gewinn (1,60 → 1,49). Musikalitäts-Prüfung mit diesem
    Fall nicht aussagekräftig (Referenz klingt laut CLAP nach „brass
    section“ 0,09, 5 von 20 Noten fielen weg).
  - **Solo geht beim Node-Wechsel verloren**: Rezept auf eine Spur
    angewendet (neuer Zweig) → Solo war aus. Absicht im Code (Solo/Abhör-
    Mute gehören nicht in den Node, `listeningMutes` in `Dawd.cpp` verwirft
    den Stand beim Laden eines anderen Edits), aber unpraktisch beim Hören.
    Vorschlag: Abhör-Stand (Solo/Mute je Spurname) über Node-Wechsel,
    Auto-Branch und Rezepte mitnehmen, nicht speichern.
- **[API] Funde aus dem SFX-Auftrag Unspill (2026-11-01)** — noch nicht
  bewertet (Lauf `$AGENT_DAW_RUNS/unspill-sfx-1`, sechs One-Shots mit
  4OSC, Abgabe mono 32-bit float):
  - **`render` kann kein Mono**: die Datei ist immer Stereo; Mono ging nur
    über ffmpeg danach (L/R gemittelt). Vorschlag: Render-Option
    `channels: 1` (auch für `renderBatch`/`renderStems`).
  - **4OSC ohne Mod-Matrix**: keine Tonhöhen-Hüllkurve erreichbar — die
    Engine hat die Matrix (`setModulationDepth`, Quellen Mod-Env/LFO in
    `tracktion_FourOscPlugin.h`), unidaw reicht sie nicht durch. Notlösung
    im Lauf: `setAutomation` auf `Tune 1..n` (Fall 7–12 st in 50–80 ms,
    im Render messbar). Für Drums/SFX (Kick, Plop, Zap) fehlt das sonst.
  - **`analyze` rundet `duration` auf 0,1 s**: eine 60-ms-Datei zeigt 0.1,
    eine 320-ms-Datei 0.3 — bei One-Shots zu grob (`findSamples` rundet
    auf ms).

## Eher nicht bauen (agent-first)

- Session View/Performance, Rack-Patching, Similarity Search,
  Hardware-Controller (TE hat Control Surfaces, unidaw braucht sie nicht).
- Mensch-UI-Hilfen wie Scale-Highlighting, Ghost Notes, Loop Starter —
  Agenten erzeugen Noten selbst.
- KI-Assistent à la FL „Gopher" — unidaw ist agent-first, die Agenten sind der
  Assistent.
- Verschoben statt gestrichen: VariAudio/Flex Pitch, Score-Editor,
  Expression Maps, Atmos — erst, wenn Gesang, Orchester- oder Film-Arbeit
  tatsächlich anstehen.
