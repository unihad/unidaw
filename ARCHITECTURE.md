# unidaw — Architektur

Dieses Dokument ist verbindlich für Änderungen am Repo. Die Kurzfassung der
Regeln steht in `AGENTS.md` (wird von Agenten automatisch gelesen); hier
stehen Schichten, Ablauf und Erweiterungswege im Detail.

unidaw ist die eigenständige DAW von unihad: ein headless Daemon auf der
Tracktion Engine. Menschen (Flow-UI im Browser) und Agenten (MCP) sprechen
dieselbe Befehls-API; jede Mutation ist ein Event. Der Song ist ein Baum aus
Varianten (jeder Knoten ein eigener Edit-Stand mit Aktionen).

## Schichten

```
Browser (/ui) ─┐
MCP-Agent ─────┼─> POST /command ─> executeOp ─> Guards ─> Handler ─> Edit
Skripte/Tests ─┘                      │              │
                                      │              └─> events.jsonl
                                      └─ Version (stateVersion)
```

1. **HTTP-Einstieg** — `source/main.cpp`, `source/HttpServer.h`.
   Routen: `POST /command`, `GET /ui`, `GET /health`, `GET /audio?path=`,
   `POST /upload?name=`, `GET /progress`. Der Server-Thread nimmt die
   Anfragen eine nach der anderen an und liest sie; Befehle stellt er in
   Eingangsreihenfolge per `callAsync` in die Queue des
   MessageManager-Threads (Tracktion-Ebene ist nicht thread-safe), die
   Antwort wartet auf einem eigenen Thread je Verbindung. So antworten
   `/progress`, `/health`, `/audio` und `/ui`, während ein langer Befehl den
   Message-Thread hält. `GET /progress` (`source/LoadStatus.h`, kein Op, kein
   Log) ist der Ladestand für den Ladebalken der UI: laufende Op mit Phase
   (saving, node edit, plugins i/n mit Plugin-Name, routing, samples, audio
   graph), Dauer, `last` (zuletzt fertige Op mit Dauer und geladenen
   Plugins) und `background` (Jobs, Klang-Analyse). `executeOp` meldet jede
   Op außer den Anzeige-Polls, `loadNodeEdit` die Phasen, die Engine jedes
   externe Plugin (`DawdEngineBehaviour`). SIGPIPE ist aus: ein Client, der
   während seines Befehls abbricht, beendet unidaw nicht mehr. Vor jeder Route
   prüft der Server Host, Origin und beim POST den Token (Abschnitt
   localhost-API absichern).
2. **Dispatcher** — `source/OpRegistry.{h,cpp}`. Eine Tabelle mit allen Ops:
   Name, Familie, `mutating` (zählt Version, wird geloggt), Guards
   (braucht Edit/Tree, nicht während Aufnahme), Handler. `executeOp` prüft
   Guards, zählt die Version, ruft den Handler, loggt das Event.
   `listOps` liefert die Tabelle an Agenten.
3. **Ops je Familie** — `source/ops/`:
   - `Session.cpp` — Projekt auf/zu, Verzeichnisse, Daemon-Lebenszyklus,
     `batch` (mehrere Befehle in einem Aufruf: jeder über `executeOp`, also
     Guards, Auto-Branch, Version und Log je Befehl; `stopOnError` als
     Default; nicht transaktional; `quit`/`batch` darin verboten; der Batch
     selbst wird nicht geloggt)
   - `Tree.cpp` — Song-Baum (Seed, Branch, Checkout, Reset, Status,
     `bounceNode`: Mix eines Nodes als Kind-Node wie ein Seed — eine Spur
     "Bounce" mit `bounces/node-<id>.wav`, die Sections samt Tempo/Takt
     und die Marker (Teil-Bounce: die der Section; gleitet sie per Rampe,
     kommt die Folge-Section als Ziel mit), collect-Aktion `bounceNode` mit Datei; legt den Node selbst an, darum
     nicht in `actionKindOf`; Undo nimmt ihn weg),
     Einstellungen (`treeSettings`), Undo/Redo
   - `Compact.cpp` — Projekt komprimieren (Familie tree):
     `treeCompactPreview` (Trockenlauf, read-only, liefert Plan-Token),
     `treeCompact {plan}` (läuft nur mit dem Token des aktuellen Plans)
   - `Sections.cpp` — Songstruktur (anlegen, anpassen, verschieben,
     kopieren, löschen — `deleteSection` seit 2026-10-15, rückholbar über
     Undo); alle Ops arbeiten auf dem Zeitbereich (Abschnitt „Sections als
     Zeitbereiche“)
   - `Media.cpp` — Audio/MIDI: record (Multi-Track), Insert, Monitor, Take,
     Inputs (listInputs, assignInput, clearInput), Clip-Edit
     (deleteClip, trimClip, moveClip)
   - `Select.cpp` — Favoriten auf Material (favoriteClip, favoriteSection)
   - `Processing.cpp` — Plugin-Module, Routing-Matrix (addPlugin,
     removePlugin, connect, disconnect, routing), Parameter,
     Render (Optionen: Format, Samplerate, Bittiefe, Bereich, Ausklang,
     Normalisieren; `renderStems` filtert über allowedClips, weil der
     Renderer Kind-Spuren der Void-Spur immer mitnimmt; Clip-Render
     `render {track, name}` ebenso über allowedClips = der eine Clip,
     ohne Master-Kette wie die Stems, eine stumme Lane klingt für den
     Render; `renderBatch`: Nodes x Bereiche x Formate in einem Aufruf,
     andere Nodes über `loadEditForRender` nur zum Rendern geladen, Mix über
     `renderEditMix`, Stems über `renderStemsOf`; synchron auf dem
     Message-Thread — ein Hintergrund-Job ginge nur mit Render außerhalb
     des Message-Threads; `matchSound`/`tuneSound` rendern stattdessen in
     Schritten, Abschnitt „Klang nachbauen“)/Analyse
     (`analyze`, `compareReference`: Bereich als Float-WAV rendern und mit
     `measureReader` messen; `renderMixRange` rendert dafür bis 2 s
     Vorlauf vor `start` mit und schreibt nur das Fenster, für alle
     Aufrufer — Decision
     [2026-10-30-analyse-vorlauf](docs/decisions/2026-10-30-analyse-vorlauf.md);
     gemessen wird mit `measureReader` — EBU R128, RMS, Spektral-Balance in
     Oktavbändern; Mono-Referenzen als Dual-Mono; `compareReference` dazu
     Artikulation und Lage beider Seiten über `voice::describeVoice`
     (`articulationOf`, Standard bis 60 s Vergleich); vor jedem Render wartet
     `waitForClipRenders`, bis rückwärts gerenderte Clip-Kopien fertig
     sind); hier landen
     kommende KI-Verarbeitungsschritte
     Plugin-Fenster: `pluginWindow` {plugin, open?, x?, y?} zeigt den
     Editor eines Fremd-Plugins in einem nativen Fenster
     (`source/PluginWindows.{h,cpp}`, Abschnitt „Plugin-Fenster“)
   - `Freeze.cpp` — Freeze/Flatten (Familie processing): `freezeTrack`
     {track, on?}, `flattenTrack` {track} (Abschnitt „Freeze/Flatten“)
   - `ClipFx.cpp` — Clip-FX (Familie processing): `processClip` {track,
     name, plugins, tail? | clear} rechnet einen Audio-Clip offline durch
     Module (Abschnitt „Clip-FX“)
   - `Analysis.cpp` — Musik-Analyse für Agenten (Familie processing,
     read-only): `detectTempo` (Tempo, Beats, Takt, Takt-Eins), `detectKey`
     (Tonart, Tonleiter, Akkorde je Beat mit Takt/Schlag),
     `analyzeStructure` (Abschnitte mit Buchstabe, Rolle, Takten, Energie,
     Akkordfolge und fertigen `addSection`-Befehlen für `batch`); Quelle ist
     eine Datei (`start`/`end` in Dateisekunden, ohne Projekt) oder der Mix
     über `renderMixRange` (aus `Processing.cpp`, wie `analyze`). Die
     Signalverarbeitung (Onsets über eine Terzband-Filterbank, Chroma,
     Beat-Tracking, Krumhansl-Profile, Akkord-Vorlagen mit Viterbi,
     Merkmale je Takt und Foote-Neuheit für den Aufbau) liegt ohne
     Engine-Bezug in `source/MusicAnalysis.{h,cpp}`
   - Noten aus Audio (`audioToMidi`, `addMidiFromAudio` in `Analysis.cpp`):
     `source/Transcription.{h,cpp}` — basic-pitch (Apache 2.0) über ONNX
     Runtime (MIT): Resampling auf 22,05 kHz, 2-s-Fenster mit Überlappung,
     Port der basic-pitch-Nachverarbeitung (Onsets, Noten-Tracking,
     Melodia-Trick); Oberton-Geister und Stimmen-Filter im Op, Velocity
     aus dem Pegel beim Einsatz; `legato` (Fragmente verschmelzen, an den
     nächsten Einsatz binden) und `bends` (Verlauf aus der Contour) über
     Funktionen in `VoiceAnalysis.cpp` (`mergedFragments`, `joinLegato`,
     `onsetVelocities`, `pitchBends`). ONNX
     Runtime und Modelle liegen nicht im Repo: `tools/fetch_ml.sh` lädt
     sie nach `dawd/third_party/` und `dawd/models/`, CMake schaltet
     `UNIDAW_WITH_ONNX` nur ein, wenn die Bibliothek da ist
   - `Stems.cpp` — Stem-Trennung als Hintergrund-Job: `separateStems` lädt
     die Quelle (Datei oder Mix-Render) auf dem Message-Thread, trennt auf
     dem Job-Thread (`source/Jobs.h`: JobList im Kontext, ein Job nach dem
     anderen, Fortschritt/Abbruch atomar) mit `source/Separation.{h,cpp}`
     (HT-Demucs als ONNX: 7,8-s-Segmente, 25 % Überlappung, lineare Rampen)
     und schreibt nur Dateien; `jobStatus {wait}` liest das Ergebnis —
     Änderungen am Edit macht danach der Agent (addClipCommands → batch).
     Ein Treffer im Cache (`<config>/stems/…`) rechnet nichts und geht
     nicht in den Pool: der Job ist schon in der Antwort `done`
     (`cached: true`), auch wenn eine andere Trennung den Job-Thread belegt.
     `wait` wartet seit 2026-10-26 auf dem Thread der Verbindung
     (`main.cpp` fragt den Stand alle 100 ms auf dem Message-Thread ab),
     nicht mehr schlafend auf dem Message-Thread — Jobs in Schritten
     (unten, Klang nachbauen) liefen sonst nicht weiter.
     Dazu MVSep Mega 53 (BS-RoFormer, 53 Instrumentenklassen): kein
     fertiger ONNX-Export, `MEGA53=1 tools/fetch_ml.sh` lädt Checkpoint,
     Config und MSST-Code und exportiert mit `tools/export_mega53.py`
     (Python mit torch; ~2,8 GB). Ohne Graph-Optimierung laden; ab ONNX
     Runtime 1.31 dauert das 7 s (1.20.1: 5,4 min je Lauf). `compareClasses`
     trennt eigenen Mix und Referenz und ordnet eigene Spuren über
     Leistungsspektrogramme zu (Anteil der Klasse an der Mix-Energie je
     Zelle, gewichtet mit der Spur) — ein Skalarprodukt der Wellenformen
     scheitert an freilaufenden Oszillatoren
   - Timestretch (`stretchClip`, Audio-`transpose` in `setClip`, Media.cpp):
     Rubberband (GPL) als Single-File-Build der Engine
     (`TRACKTION_BUILD_RUBBERBAND`, Quellen per `tools/fetch_ml.sh` in
     `dawd/third_party/rubberband`, CMake schaltet es nur bei vorhandenen
     Quellen ein). Gestreckte Clips laufen mit Echtzeit-Stretching
     (`setUsesProxy(false)`): die Proxy-Datei rendert die Engine über Timer
     des Message-Threads, ein synchroner Render davor wartete darauf und
     blockierte den Daemon. Nach jedem Neustart des Stretchers (Clip- und
     Loop-Anfang) fehlten der Engine die ersten 256 Samples — Patch
     `tracktion-rubberband-start-delay.patch` (`docs/engine-bewertung.md`)
   - Warp-Marker (`warpClip`, Media.cpp, 2026-10-26): Marker heften
     Quellsekunden an Beats ab dem Clip-Start. Der Clip spielt die
     Originaldatei mit followTempo — Tempo der LoopInfo = Mittel vom ersten
     zum letzten Marker, dort liegen Quelle und Warp gleich —, die Engine
     führt sie beim Abspielen und im Render in Echtzeit durch die Marker
     (seit 2026-10-31, Todo `todos/2026-10-31-warp-echtzeit.md`): unidaw gibt
     die Marker über `EngineBehaviour::getWarpPointsForClip`
     (`realtimeWarpPoints`) an die Engine, Patch
     `tracktion-warp-realtime.patch` rechnet sie mit dem einen
     Tempo-Stretcher (Verhältnis je eingespeistem Stück, ±5 ms). `warpClip`
     antwortet sofort (0,01 s), legt keine Datei an und baut nur den
     Wiedergabe-Graphen neu (`restartPlayback`). Bis 2026-10-30 renderte
     es die ganze Datei offline (Rubberband mit Keyframe-Map,
     `media/<name>-warp<N>.wav`, 1,7–5,6 s bei 200 s Quelle, eine Datei je
     Änderung); `playWarpSources` (EngineHelpers, beim Laden) stellt solche
     Clips auf die Originaldatei um, `treeCompact` räumt die alten Dateien.
     Das Warp Time der Engine (`WarpTimeManager`) bleibt aus. Am Clip: Kind
     `DAWDWARP` {source (Originaldatei), length, realtime, MARKER {source, warp}}
     (Decision `docs/decisions/2026-10-31-warp-echtzeit.md`);
     `unwarpedFile`, `warpToSource`/`sourceToWarp`, `clipSourceTime`
     (EngineHelpers) rechnen Clip-Zeit → Sekunde der Originaldatei — auch
     für `clipPeaks`. Kein Rückwärts, kein `stretchClip`, kein `trimClip`
     vor den Dateianfang, solange Marker gesetzt sind
   - Loop im Audio-Clip (`loopClip`, Media.cpp): Loop-Bereich der Engine
     (`setLoopRange` in Sekunden der Datei bzw. `setLoopRangeBeats` in Beats
     des Materials bei followTempo), Offset = Stelle im Loop, `count` =
     Länge in Durchgängen. Zeit-Weg nur mit speed 1 (die Engine liest den
     Loop-Start dort als Dateisekunde, rechnet ihn im Modell aber mit dem
     Tempo um). `trimClip` nach links lässt den Offset im Loop weiterlaufen.
     `state` zeigt `loop` {start, length, offset, …Beat, restarts}
   - Parameter (`listParameters`/`setParameter`, Processing.cpp) über
     `source/PluginSettings.{h,cpp}`: eindeutige Namen je Plugin (Doppelte
     über die Parameter-Id), Realwerte mit Text/Einheit, Einstellungen, die
     keine Parameter sind (4OSC: Wellenform, Filtertyp, Effekte an).
     `real` ist überall die angezeigte Zahl in der gemeldeten Einheit —
     Zeiten in ms (auch wo der Text zwischen ms und s wechselt, z. B. alle
     4OSC-Hüllkurven), Pegel in dB, Anteile in % 0..100; Fehlermeldungen
     nennen die Einheit; Rezepte (`changedParameters`) und `setAutomation`
     (`real` je Punkt) nutzen dieselbe Umrechnung
     (`docs/decisions/2026-10-24-einheiten-real.md`).
     Pegel einer Spur: `faderTrackOf` (Routing) liefert den Fader am Ende
     ihrer eigenen Kette (nach den Effekten, vor der Summe; ohne eigene
     Kette die Spur selbst bzw. das Instrument einer MIDI-Spur; ein Send
     hinter dem letzten Modul ist keine Verzweigung, er geht hinter dem
     Fader ab — Post-Fader-Send, `docs/decisions/2026-10-24-post-fader-sends.md`) — trackMix,
     state (`fader`), Automation und der Pegel in `levels` nutzen ihn. `syncFaders` zieht Pegel, Pan und
     Automation nach jeder verändernden Op und beim Laden an den neuen Ort
     (Marke `dawdFaderOf` am Träger, letzter Wert an der Spur)
   - Summe/Splices (Processing.cpp, `planSplice`): `at` start/end/master,
     master mit `before` vor ein Modul der Master-Kette (sonst hinter die
     vorhandenen; UI: „+ before“ am Master-Modul in Simple), Pegel ersetzter Kanten bleiben erhalten; Audio-Effekt mit start auf einer
     MIDI-Spur beginnt am Instrument. `end` folgt keinen Sends (markierte
     Kanten, Post-Sends des letzten Moduls); die Post-Sends ziehen ans neue
     Kettenende mit wie bei `setChain` (Matthias 2026-10-24, R1). Eingebauter Limiter
     `source/Limiter.{h,cpp}` (eigenes Plugin wie das Feedback-Paar, über
     `registerLimiterPlugin`): 4x überabgetastet, Lookahead-Minimum per
     Deque, Release, gleitender Mittelwert — True Peak an der Decke. Im
     Render (`isRendering`, nicht beim Abspielen) zählt er Gain-Reduction
     und Eingang mit (`resetStats`/`renderStats`, ab `start` des Fensters
     — der Vorlauf zählt nicht); `analyze` und
     `compareReference` melden je Limiter im Weg `limiters` (Reduktion
     Mittel/Maximum, Crest am Eingang) samt Hinweisen
   - `SoundMatch.cpp` — Klang nachbauen (Familie processing, Abschnitt
     „Klang nachbauen“): `matchSound`, `tuneSound` als Jobs in Schritten
     auf dem Message-Thread; Messung aus `Voice.cpp` (`SoundMatch.h`)
   - `Presets.cpp` — Presets (Familie processing): `listPresets`,
     `loadPreset` (undo-fähig, keine Node-Aktion), `savePreset`. Quellen in
     `source/Presets.{h,cpp}`: unidaw-Presets (`<config>/presets/<Plugin>/`,
     Plugin-Zustand base64 bzw. ValueTree für eingebaute), benannte
     Programme, Surge-XT-.fxp (Patch ab Byte 60), u-he-.h2p (`#pgm=` + Text),
     .vstpreset (Comp/Cont-Chunks), DX7-Cartridges für Dexed (je Stimme ein
     Eintrag; Cartridge im Zustand tauschen, Stimme selbst in den
     Edit-Puffer entpacken). Rohe Komponenten-Zustände gehen bei VST3
     in die XML-Hülle von JUCEs Host (`VST3PluginState/IComponent`); danach
     läuft die Message-Loop kurz, bis die Parameter nachgezogen sind
   - `Recipes.cpp` — Rezept-Katalog (Familie recipes): `saveRecipe` liest
     Module samt Plugin-Zustand (`writePresetFile`), Ketten und Kanten aus
     dem geladenen Node-Edit (`routingState`) — nicht aus den Node-Aktionen,
     dort fehlen setParameter/loadPreset/setWet — und macht daraus
     Op-Schritte mit Platzhaltern (`$track`, `$p1`, `$recipe`, eigene wie
     `$tempo`); mit `track` auch die Module, die nur diese Spur speist
     (Parallelwege, Sends), dazu Fader (`trackMix`) und Automation
     (`setAutomation`, alle Punkte; ab `version` 2 in Schlägen `beat` ab
     Beginn der Quell-Section — `section` oder die, in der die Automation
     anfängt; mit `section` zugeschnitten —, version 1 in Songzeit `time`);
     gleicher Titel aktualisiert das Rezept (Id und Bewertung bleiben,
     Status draft). `applyRecipe` spielt sie
     über `executeOp` in einem neuen Zweig ab (nicht bei `treeSettings
     autoBranch` aus; Auto-Branch dabei aus, Rückbau des Zweigs bei
     Fehlschlag, Node-Aktion `applyRecipe`), Schläge ab Beginn der
     Ziel-Section (`section`, sonst die unter dem Playhead), Fader des
     Rezepts samt Lautstärke-/Pan-Automation nur auf neuen Spuren; `findRecipes` (Worte, Art,
     Tags, Plugin; Geprüfte, Sterne, Erfolge zuerst), `recipeInfo`,
     `rateRecipe` (Nutzung/Bewertung; Status checked verschiebt ins Repo);
     `pluginInfo` nennt die Rezepte eines Plugins (`recipesWithPlugin`)
   - `Library.cpp` — Sample-Bibliotheken (Familie media): `sampleLibraries`
     (Ordner an-/abmelden, Index-Stand), `findSamples` (Suche über Pfade);
     der Index steht in `source/SampleLibrary.{h,cpp}` (je Wurzel im
     Hintergrund über `std::filesystem` — folgt keinen Symlinks/Junctions —,
     als Textdatei in `<config>/index/` zwischengespeichert; versteckte und
     Vorhör-Ordner sowie AIFFs im Ableton-Codec fallen weg). Die Kits unter
     `dawd/kits/` sind immer dabei (`make_basic_kit.py` erzeugt das Kit
     "basic"); das Built-in "Drum Kit" ist ein Sampler mit diesem Kit,
     `setSamples`/`listSamples` (Processing.cpp) belegen Sampler-Module und
     kopieren fremde Samples nach `media/samples/`; Velocity-Layer,
     Round-Robin-/Choke-Gruppen und Attack/Release je Sample kommen aus dem
     Engine-Patch `dawd/patches/tracktion-sampler-layers.patch` (SOUND-
     Properties nur, wenn gesetzt — alte Projekte laden unverändert)
     Suche nach Klang: `source/SoundIndex.{h,cpp}` haelt je Datei
     CLAP-Vektoren (je 10-s-Fenster, hoechstens 6), gemessene Merkmale
     (`source/SoundFeatures.{h,cpp}`: Attack, Punch, Tiefenanteil,
     Helligkeit, Tonhoehe/Vibrato per YIN, bpm fuer Loops) und eine
     Kategorie (Pfadworte, sonst CLAP gegen Kategorie-Texte).
     `sampleLibraries {analyze}` baut ihn im Hintergrund: ein Manager-Thread,
     bis zu 8 Worker mit je einer ONNX-Session und einem Thread (mehrere
     kleine Sessions nutzen die Kerne besser als eine grosse), nice 19;
     inkrementell ueber Groesse + Aenderungszeit, jede fertige Datei wird an
     `<config>/index/<hash>.sound` angehaengt (Abbruch verliert nichts,
     ueberholte Datensaetze raeumt das Ende des Laufs). `findSamples {text |
     like}` vergleicht im Speicher (Brute Force). Ordner ohne Analyse
     (Lizenz verbietet Data Mining/KI; Decision
     `docs/decisions/2026-11-18-klang-analyse-je-hersteller.md`):
     `sampleLibraries {noAnalysis | allowAnalysis}`, Liste in
     `<config>/libraries.json`; `SampleLibrary::items` laesst sie weg,
     `SoundIndex::forgetUnder` loescht ihre Datensaetze (auch beim Start),
     `loadReference` lehnt eine Datei darin als `like` ab. Dieselbe Sperre
     gilt fuer die Analyse-Ops (Decision
     `docs/decisions/2026-11-18-analyse-ops-auf-gesperrten-samples.md`):
     `source/Origins.*` notiert die Herkunft abgeleiteter Dateien
     (`.dawd-origins.json` im Ordner der Datei: Kopie in `setSamples`,
     dekodierte WAV, Clip-FX, Trim-Kopie, Freeze, Bounce, Stems) und
     listet das Material im Bereich (`soundingFiles`: Audio-Clips, Samples
     der Sampler unter MIDI-Clips); `blockedFileError`/`checkMixLicence`
     (`ops/AnalysisSource.*`) pruefen bei jeder Analyse gegen die Liste —
     `loadAnalysisSource` (Flag `ml` = ML-Op lehnt auch den Mix ab) plus
     die Ops, die selbst laden. Hinweise der Messungen sammelt
     `ctx.licenceNotes`, `executeOp` setzt sie als `licenceNote` in die
     Antwort, das Flow-UI zeigt sie in der Statuszeile. CLAP selbst:
     `source/Clap.{h,cpp}` (Log-Mel wie ClapFeatureExtractor, Audio-Encoder
     int8, Text-Encoder fp32, RoBERTa-BPE-Tokenizer in C++), Modelle per
     `tools/fetch_ml.sh`
   - `Automation.cpp` — Automation über die Songzeit (Familie processing):
     `listAutomatable`, `automation`, `setAutomation`, `clearAutomation`
   - `View.cpp` — Zustand, Transport, Mix (mute/solo, volumeDb, pan)
   - `Audio.cpp` — Audio-Einstellungen (`audioSettings`): Puffergröße global
     (`audio.json` im `--config`-Ordner), Rate und Aufnahme-Bit-Tiefe je Song
     (`tree.json`, beim Öffnen angewendet). Unter JACK per `pw-metadata`
     `clock.force-rate`/`force-quantum` und Neu-Öffnen des Geräts (JUCE liest Rate
     und Block nur beim Öffnen); unidaw löst nur, was es selbst gesetzt hat, beim
     Beenden alles. Unter ALSA nur das eigene Geräte-Setup (0 = Startwert).
   - `Osc.cpp` — OSC-Fernsteuerung (Familie session): `oscSettings`
     {enabled?, port?, address?}; Empfang und Adressen in
     `source/Osc.{h,cpp}` (Abschnitt „OSC-Fernsteuerung“)
   - `Interface.cpp` — Audio-Interface (Familie interface): Hardware-
     Monitoring des RME Fireface UC (`interfaceState`, `setInterface`,
     Pegelanzeigen `interfaceMeters`; `interfaceSim` nur mit `--interface-sim`). `source/Interface.{h,cpp}`
     liest und schreibt die ALSA-Mixer-Elemente des snd-usb-audio-Quirks
     (`~/dev/fireface-uc`) über alsa-lib — dB je Stufe aus der TLV des
     Treibers, einmal je Einstecken gelesen — oder eine Simulation mit
     denselben Namen, Bereichen und Startwerten (Test-Suite). Das Modell
     leitet Ausgänge, Quellen und Paare aus den Reglernamen ab
     ("<Ausgang> from <Quelle> Playback Volume", "<Ausgang> Mix Playback
     Volume"). Kein Teil des Edits: kein Undo, keine Node-Aktion, kein
     Auto-Branch; ein Fehler prüft vor dem Schreiben und ändert nichts
4. **Domäne** — `source/Dawd.h` (Kontext, Logger, Edit-/Aufnahme-Zustand),
   `source/TreeState.h` (Song-Baum + Node-Aktionen), `EngineHelpers.{h,cpp}`
   (Tracks, Lanes, Sections, Inserts, Zeitbereiche, Auto-Section), `Routing.{h,cpp}` (Routing-Matrix:
   Module, Kanten, Typ-Regeln, Migration), `Automation.{h,cpp}` (Ziel-Ids →
   Engine-Parameter, Einheiten, Kurven schreiben/lesen), `RecordSession.cpp` (Aufnahme-Ende,
   Auto-Section beim Stopp, MIDI-Take-Render).
5. **Flow-UI** — `ui/flow.html`, ausgeliefert unter `GET /ui`. Bewusst
   ohne Framework und Build-Schritt: der Daemon liest die Datei pro
   Request (Pfad per `--ui`, Default `ui/flow.html` neben der Binary, siehe
   Datenablage, Mitgelieferte Ressourcen) — UI-Änderungen brauchen
   nur einen Browser-Reload, keinen Build und Neustart.
   App-Layout: Transport-Leiste oben (Play/Loop/Zeit, ein Record/Monitor/
   Insert für Audio + MIDI, Export — kein Modus-Umschalter, alle
   Bedienelemente sind in jedem Node da; die Sorte eines Nodes kommt aus
   seinen Aktionen, Decision
   [2026-10-30-modus-umschalter-weg](docs/decisions/2026-10-30-modus-umschalter-weg.md)),
   Song-Baum als Sidebar links,
   Timeline in der Mitte (Lineal = Playhead, Sections + Bänder, Spuren mit
   Waveform/Noten aus `clipPeaks`, Datei-Drop, Arm-Schalter je Spur —
   `armTrack`, Record nimmt alle geärmten Spuren parallel auf;
   Doppelklick auf MIDI-Clip = `setMidiNotes`, Löschen mit Undo-Toast =
   `undoDelete`), Dock unten (Input,
   Piano-Roll, Routing-Matrix — Simple, Complex und Mixer mit Kanalzügen
   je Spur, alle auf demselben Graphen). Design-Tokens als CSS-Variablen in `:root`.
   `applyNodeUi()` ist die eine Stelle, die aus Node, Action-Wahl,
   + -Zustand und Songzustand alle Sichtbarkeiten setzt.

## Datenablage

Projektordner (per `treeSeed`/`treeOpen`):

- `tree.json` — Baumzustand, Schema v3: Knoten mit `note`/`noteUpdated`
  (Notiz, `treeNote` — Baum-Daten wie das Label, kein Undo) und `actions[]`
  (`kind` collect|select|process, `op`, `params`, `file`). v1-/v2-Dateien
  migriert der Loader (alte Kinds nach dem Op gemappt).
- `node-<id>.tracktionedit` — der Edit-Stand des Knotens.
- `node-<id>.base.tracktionedit` — Snapshot bei Erzeugung (`treeReset`
  stellt ihn wieder her).
- `media/` — hochgeladene Dateien (`/upload`), gerenderte Clip-Dateien
  (`<Clip>-warp.wav` von `warpClip`, `<Clip>-fx.wav` von `processClip`,
  32-Bit-Float).
- Fehlt eine Mediendatei (gelöscht, verschoben, von `treeCompact`
  geräumt), lädt das Projekt trotzdem: `state` markiert den Clip
  (`missing`, `missingFile`) und nennt alle fehlenden Dateien
  (`missingFiles`: Clips, Freeze-Audio, Sampler-Samples), `treeOpen` auch.
  `runRender` (`ops/Processing.cpp`) lehnt jeden Offline-Render ab, der
  eine fehlende Datei lesen würde (Spuren, `allowedClips`, Zeitbereich;
  `missingMedia` in `EngineHelpers`) — sonst wartet die Engine je Block
  5 s (`docs/engine-bewertung.md`). Sampler laden in den Speicher und
  rendern weiter (fehlende Taste stumm, `listSamples` zeigt `missing`).
- `bounces/` — Mix-Bounces von `bounceNode` (`node-<id>.wav`, 32-Bit-Float,
  Rate des Geräts), Datei der collect-Aktion des Bounce-Nodes.
- `interface.json` — Monitor-Mix des Audio-Interfaces für diesen Song
  (Matrix-Kreuzpunkte und Fader als Stufen, `device`). Eingangs-Gains/-Schalter
  und Ausgangspegel gehören dem Gerät: der Drehregler vorn stellt sie auch
  (der Treiber meldet das), ein Song soll die Kopfhörerlautstärke nicht
  überschreiben; ältere Dateien mit diesen Werten werden ohne sie geladen. `setInterface` schreibt ihn nach
  jeder Änderung (`save` übernimmt das Gerät, wie es ist); `treeOpen`/
  `treeSeed` und das Einstecken (Timer alle 2 s, USB-Gerätenummer als
  Kennung) laden ihn aufs Gerät, nur abweichende Werte. Ohne Datei bleibt
  das Gerät, wie es ist. Gilt je Projekt, nicht je Node — der
  Kopfhörer-Mix gehört zur Aufnahme-Situation, nicht zu einer Variante.
- `windows.json` — Lage der Plugin-Fenster je Modul-Id (`{"p3": {x, y}}`,
  `pluginWindow`). Gilt je Projekt, nicht je Node, und ist kein Teil des
  Edits: Fensterlage ist Arbeitsplatz, keine Klangentscheidung.
- Take-WAVs (Record-Stop) — Audio neben dem Edit, der MIDI-Take-Render
  genauso im Projektordner (nicht im CWD des Daemons).

Nutzer-Einstellungen (`--config`, Default `~/.config/unidaw`):
`libraries.json` (angemeldete Sample-Ordner), `index/` (Datei-Index je
Ordner), `osc.json` (OSC-Fernsteuerung: enabled, address, port, allow), `presets/`, `recipes/` (Rezept-Entwürfe) und die Engine-Einstellungen `Settings.xml` (Audio-Gerät,
Gerätetyp, MIDI; `DawdPropertyStorage` in `Dawd.cpp`). Die Test-Suite und die
Mess-Werkzeuge geben eigene Ordner mit — keine Instanz teilt die
Engine-Einstellungen einer anderen (ein JACK-Probelauf aus einem anderen
Zweig hatte so am 2026-10-16 allen das Gerät genommen). Plugins findet unidaw
ohne gespeicherte Liste (Ordner-Scan bei `listPlugins` und unbekanntem Namen;
je Plugin-Datei bleibt das Ergebnis bis Prozessende, neu gescannt werden nur
neue oder geänderte Dateien — der volle Scan hielt den Message-Thread 12–15 s). Wo die Samples
auf diesem Rechner liegen: `docs/sample-bibliothek.md`.

Plugin-Wissen (`dawd/plugin-profiles/`, im Repo, zur Laufzeit gelesen):
`coreset.json` (Kernset mit Kategorie/Zweck — listPlugins), Steckbriefe
`<format>-<uid>.json` (Wirkung je Parameter, `tools/profile_plugin.py` —
pluginInfo/listParameters), Preset-Kataloge `<format>-<uid>-presets.json`
(Klasse und Klangworte je Preset, `tools/profile_presets.py`, Schwellen
ändern ohne neu zu rendern: `--reclassify` — findPresets, Suchworte →
Klasse/Klangwort über `presetSynonyms()` in `ops/Presets.cpp`; je Preset auch
der CLAP-Vektor für findPresets `text`/`like`), Klang-Kataloge
`<format>-<uid>-sounds.json` (erkundete Klänge: Basis-Preset + geänderte
Parameter, Merkmale, Vektor — `tools/explore_sounds.py`, findPresets mit
kind "sound", loadPreset `sound`) und Kurznotizen
`notes/<format>-<uid>.md`. Die Werkzeuge starten unidaw mit `--no-audio`
unter bwrap, auf freien Ports ab 7700 (74xx gehört den Suiten).
Vektoren stehen als base64 von 512 int8 im JSON (`clap::encode`/`decode`);
`analyze {file, embed}` liefert sie.

Rezepte: geprüfte im Repo unter `recipes/<kind>/<id>/` (`--recipes`,
Default `recipes` neben der Binary, siehe unten; die Suite gibt einen eigenen Ordner mit),
Entwürfe und verworfene unter `<config>/recipes/<kind>/<id>/` — je Rezept
`recipe.json` und `state/<ref>.dawdpreset`. `tools/recipes.py index` erzeugt
`recipes/README.md` und je Rezept `recipe.md`, `freeze` hält vor einem
Praxistest den Katalogstand im Lauf-Ordner fest (`recipes-version.json`).
Im Flow-UI: Recipes-Strip (Process) mit Suche und Filtern (`findRecipes`),
Details (`recipeInfo`), Sternen, Rückmeldung, Freigabe (`rateRecipe`) und
Apply (`applyRecipe`); der Rezept-Knopf am aktuellen Node und „Save…“ im
Strip speichern den Node (`saveRecipe`).

Run-Verzeichnis (Daemon-Start): `events.jsonl` — ein Eintrag je Befehl
und Zeile (seq, ts, op, params, result; wie alle Antworten einzeilig, seit
2026-10-19 — vorher eingerückt über viele Zeilen und doppelt so groß). Die Anzeige-Polls (`position`,
`inputLevel`, `levels`) werden nicht geloggt; `jobStatus {wait}` fragt alle
100 ms intern ab und loggt nur die letzte Antwort (mit `wait`, Marker
`waitPoll`/`waitLast` aus `main.cpp`). Mit `--session session.json` liegt dort
auch die Session-Datei (`{dir}` = offener Projektordner, leer nach
`treeClose`).

Mitgelieferte Ressourcen (`ui`, `kits`, `models`, `plugin-profiles`,
`recipes`) findet unidaw relativ zur Binary, nie über einen Pfad in den
Quellordner (`source/Resources.*`, `resourceDir`). Reihenfolge: `<bin>/<name>`
(Build-Ordner — CMake legt dort Links in den Quellordner an, UI-Änderungen
wirken weiter mit einem Reload; portabler Ordner), dann
`<bin>/../share/unidaw/<name>` (Installation, AppImage), dann
`${XDG_DATA_HOME:-~/.local/share}/dawd/<name>` (Nutzerordner, z. B. die
Modelle, die nicht mitkommen). Fehlt ein Ordner überall, gilt der
Nutzerordner. Links auf die Binary löst JUCE zum echten Ort auf. `--ui` und
`--recipes` überschreiben das für Entwicklung und Suite.

Starter `dawd/launcher/unidaw-start` (im Build-Ordner als Link neben der
Binary): startet unidaw, wenn auf dem Port (`--port`, Standard 7400) keiner
antwortet, in eigener Sitzung mit Laufordner
`${XDG_STATE_HOME:-~/.local/state}/dawd/<port>` (`events.jsonl`,
`session.json`, `unidaw.err`), und öffnet `/ui` im Browser (`--no-open` lässt
das weg; Argumente nach `--` gehen an unidaw). unidaw sucht er neben sich, dann
im `PATH`; `UNIDAW_BIN` überschreibt. Test: `resources_relocated_flow`.
`--foreground` lässt unidaw im Vordergrund laufen und endet mit ihm (Ctrl+C
beendet beide) — das AppImage braucht das, sein Dateisystem verschwindet
mit dem Starter. Daneben `dawd/launcher/unidaw-fetch-models`: lädt die
Modelle mit Lizenz und Prüfsumme in den Nutzerordner. AppImage (Aufbau,
Bauen, Prüfen): `docs/appimage.md`.

## Absturzschutz

- **Autosave**: `executeOp` plant nach jeder erfolgreichen mutierenden Op
  (außer `play`, das den Edit nicht ändert) ein Speichern des aktuellen Node-Edits (gebündelt, ~0,4 s nach der letzten
  Mutation, während einer Aufnahme verschoben). Ein Absturz — z. B. durch
  ein Plugin, das in-process läuft — verliert höchstens dieses Fenster.
- **Sauberes Beenden**: `quit`, SIGTERM und SIGINT schreiben einen
  ausstehenden Autosave sofort (Signal setzt nur ein Flag, ein Timer
  beendet den Dispatch-Loop).
- **Wiederherstellung**: mit `--session <datei>` folgt die Datei dem offenen
  Projekt; beim Start öffnet unidaw es als ersten Befehl wieder
  (`treeOpen` mit `restored: true` im Event-Log). Ohne Flag (Test-Suite)
  merkt sich unidaw nichts.
- Der Entwickler-Dienst startet mit `Restart=on-failure` neu (Werkzeuge).
- Plugin-Isolation: Fremd-Plugins kommen in eigene Prozesse (Decision
  `docs/decisions/2026-10-25-plugin-sandboxing.md`, Option C). Stürzt ein
  Host ab oder hängt, lädt unidaw das Plugin mit dem letzten Zustand neu
  (Sperrliste gegen Schleifen); der Plugin-Scan läuft in einem Kindprozess.
  Standard an seit Phase 3, `UNIDAW_PLUGIN_SANDBOX=0` schaltet ab —
  Abschnitt Plugin-Sandbox.

## Node-Aktionen (das Kernmodell)

Jeder Knoten sammelt Aktionen als Datensätze:
`{kind: collect|select|process, op, params, file, created}`.

- `acted` ist abgeleitet (Aktionen vorhanden), ebenso die Take-Daten für den
  + Button (`takeType`, `takeSection`, `takeTrack` aus der letzten
  Collect-Aktion außer einer Auto-Section — der Take-Typ reist in deren params), `actedBy`/`actionFile`
  (erste Aktion) und `hasFavorites` (favorisiertes Material).
- Die drei Kinds: **collect** = Material reinholen (`addSection`,
  `updateSection`, `moveSection`, `addMarker`, `updateMarker`, Record-Stop,
  `addClip`, `addMidiClip`),
  **select** = Favoriten (`favoriteClip`, `favoriteSection` — liegen als
  select-Aktionen am Node), **process** = Bearbeitung (`deleteClip`,
  `trimClip`, `moveClip`, `addPlugin`, `removePlugin`, `connect`,
  `disconnect`, `freezeTrack`, `flattenTrack`, `processClip`, `compLane`, `createGroup`, `setGroup`, `removeGroup`) und Feinschliff (`setParameter`, `loadPreset`,
  `setPluginFile`, `setWet`, `setEdge`, `trackMix`, `groupMix`, `pluginWindow`
  beim Schließen nach einer Änderung im Editor — eine Aktion je
  Modul/Spur/Gruppe, zusammengefasst; Decision 2026-10-18). Ausnahme Solo und
  Mute (Decision 2026-10-20): ein `trackMix` nur mit `solo`/`mute` ist
  Abhör-Hilfe — keine Aktion, kein Auto-Branch (der lud den Edit neu und
  stoppte die Wiedergabe), kein Undo-Schritt, kein Autosave. unidaw merkt
  sich je Spur den gespeicherten Mute-Wert (`ctx.savedMute`, gilt nur für
  den offenen Edit); `saveCurrentEdit` und Undo-Schnappschüsse schreiben ihn
  statt des Abhör-Werts und lassen `solo` weg (`saveCurrentEdit` dann als
  bereinigte Kopie des Edit-Stands in einem Schritt, `XmlElement::writeTo`
  per Rename — nie erst mit Solo auf der Platte), Renders und `peak` schalten
  beides für ihre Dauer ab (`ScopedListeningOff`). Lane-Wahl (`laneActive`,
  `activateLane`) setzt Mute als Struktur und vergisst den Abhör-Wert der
  Gruppe (`forgetLaneListening`). `deleteSection` (process, Decision 2026-10-15) löscht
  eine Section samt ihrem Zeitbereich auf dem aktuellen Node (Clips über
  ihren Grenzen werden dort abgeschnitten), Undo holt sie zurück; nicht
  favorisiertes Material projektweit wirft `treeCompact` raus (keine
  Node-Aktion).
- Komprimieren (`treeCompact`): Nodes bleiben, wenn sie Seed, Node-Favorit,
  Node mit Material-Favoriten oder Vorfahr einer solchen sind — die anderen
  fliegen samt Edit-Dateien. In Nodes mit Material-Favoriten (ohne
  Node-Favorit, der schützt alles) fliegen nicht favorisierte Clips (außer
  solchen, die eine favorisierte Section berühren), Sections ohne Favorit,
  die kein bleibender Clip berührt (Entscheidung Matthias 2026-10-24: auch
  ein nur durch eine favorisierte Section geschützter Clip hält sie, kein
  Abschneiden; samt Zeitbereich wie `deleteSection`, keine Gaps: der Rest
  rückt nach) und Tracks ohne bleibenden Clip
  und ohne bleibende Lane. Gefrorene Spuren überspringt es (Entscheidung
  Matthias 2026-10-24): ihre Clips und Lanes bleiben, sonst passte das
  gefrorene Audio nicht mehr; fällt eine Section, schneidet die Zeit
  Clips und gefrorenes Audio gleich. Track-Indizes in den Aktionen werden
  nachgezogen. Base-Snapshots und Mediendateien bleiben — bis auf
  Warp- und Clip-FX-Dateien (`media/<name>-warp<n>.wav` aus `warpClip`,
  `media/<Clip>-fx<n>.wav` aus `processClip`), deren Name danach in keiner
  Zeichenkette mehr steht (auch das Original eines gerechneten Clips,
  `DAWDFX`/`ORIGINAL`, ist ein Verweis): bleibende Edit-Dateien samt
  Base-Schnappschüssen und jeder anderen `*tracktionedit*` im Projektordner,
  gestutzte Edits ohne ihr fallendes Material, `tree.json`. Die
  Undo-Historie zählt nicht, `treeCompact` löscht sie. Vorschau und Lauf
  nennen die Dateien (`removeFiles`); Teilstring-Treffer und unlesbare
  Dateien halten im Zweifel fest (Todos `2026-10-26-warp-dateien-aufraeumen`,
  `2026-10-26-fx-dateien-aufraeumen`).
- Branch braucht eine Aktion am Knoten (`acted`); `treeReset` räumt sie.
- **Der Baum baut sich aus den Aktionen (Auto-Branch).** `executeOp` kennt
  die Sorte jeder Op vorab (`actionKindOf` in `OpRegistry.cpp`; `record`
  nur beim Start, `createTrack` nur mit Instrument) und ihren **Fokus**
  (`focusOf`): `t<i>` = die Gruppenspur, an der sie arbeitet (Noten,
  Clips, Pegel, Module ihrer Kette — ein Modul gehört der Spur, die es als
  einzige erreicht), `song` = Sections, Master-Kette, Module mehrerer
  Spuren, `new` = eine Spur, die erst entsteht. Ist der aktuelle Node acted
  und unterscheidet sich die Sorte (die seiner ersten Aktion) oder der
  Fokus (`TreeNode::focus`, von seiner ersten Aktion gesetzt), legt
  `executeOp` vor dem Handler einen Kind-Node an (`treeBranch`, Label
  "<Spur>: Notes|Sound|Mix" bzw. "Song: Arrangement|Sound|Mix", Decision
  2026-10-18 nach Praxistest 1d: ein Node hielt 24 Aktionen über alle
  Spuren) und führt die Op dort aus; die Antwort trägt `autoNode` {node,
  parent, kind, label}. Schlägt die Op fehl, fliegt der leere Kind-Node
  wieder raus (`treeDelete`). Ein loser Edit (`newEdit`, nicht die Datei
  des Nodes) branched nicht. Clip-
  Bearbeitung in einem collect-Node (`isArrangementEdit`: duplicateClip,
  splitClip, trimClip, moveClip, setMidiNotes, setClip, stretchClip,
  warpClip, loopClip, deleteClip, deleteSection, deleteMarker) gilt als Arrangieren und bleibt im Node
  (Praxistest 1). Umgekehrt nimmt ein Node aus reinen Clip-Edits ein
  folgendes `addClip`/`addMidiClip` derselben Spur auf (Clip ersetzen =
  ein Node). **Mix-Ebene** (`isMixEdit`: `trackMix` mit volumeDb/pan,
  `setClip` nur mit gainDb): Fokus `song`, Label "Song: Mix" — Pegel
  mehrerer Spuren sind ein Schritt, auch im batch; nur im Klang-Node
  derselben Spur (erste Aktion process, kein Clip-Edit) bleibt ein
  Fader-Wert dort. Der Freeze-Guard prüft weiter die Spur (`focusOf`).
  `treeNote`, das eine andere Notiz ersetzt, meldet `replaced` (Todo
  `2026-10-29-clip-fallen`, Praxistest 2/3).
  Abschaltbar je Projekt: `treeSettings {autoBranch}` (in `tree.json`).
  Bestehende gemischte Nodes bleiben, wie sie sind.
- **Instrument im Collect:** `createTrack {type: midi, instrument}` und
  `pickInstrument {track, instrument}` verdrahten Spur → Instrument → Main
  und buchen eine collect-Aktion — Material hörbar machen gehört zum
  Reinholen, kein Sorten-Wechsel.
- Favoriten-Pfad- und Terminierungs-Regeln: ältere Decision
  `decision.2026-10-11-song-baum-fuer-dawd-seed-pfad-favoriten-teilbaum-terminierung`
  (Liste in `docs/decisions/README.md`).

Aufnahme/Insert-Konventionen (stehen in den Decisons vom 2026-10-12, die
Begrenzung auf die Section ersetzt durch
`docs/decisions/2026-10-24-sections-als-zeitbereiche.md`):

- Eine Section ist Pflicht, sie ist nur der Startpunkt: die Aufnahme
  beginnt an ihrem Anfang und läuft bis zum Stopp (kein Punch, kein
  Auto-Stop), Inserts behalten ihre ganze Länge. Reicht ein Take oder
  Insert hinter die letzte Section, entsteht eine Auto-Section (Abschnitt
  „Sections als Zeitbereiche“).
- Mit zugewiesenen Inputs (`assignInput`) nimmt EIN record alle Spuren mit
  Zuweisung gleichzeitig auf (Multi-Track-Record); ohne Zuweisung gilt der
  Fallback (Default-Gerät, ein Take, `type`/`track`-Param).
- Jede Aufnahme wird eine eigene Lane des Zieltracks
  (Track 2 → Track 2.1, 2.2 …), die durch dessen Routing läuft.
  `addClip`/`addMidiClip` gehen in die spielende (oder genannte) Lane, wenn
  dort Platz ist; eine neue Lane nur für den ersten Clip, bei anderem Typ
  oder mit `newLane: true` (Take; die Flow-UI übergibt es beim Insert);
  Überlappung ist ein Fehler, der den Clip nennt
  ([Decision](docs/decisions/2026-10-29-ueberlappung-fehler.md)). Die
  Gruppenspur bleibt leerer Bus — Seed-Audio spielt immer weiter. Die
  Zuweisung (assignInput) bleibt nach dem Take auf dem Zieltrack.
- Nur eine Lane pro Gruppe spielt (`laneActive`); die Lane erbt den
  Spurtyp der Gruppe (`dawdType` am Track, audio | midi). Je Zeitbereich
  eine andere Lane: der Comp (Abschnitt „Comping“).

## Plugin-Fenster

`pluginWindow` {plugin, open?, x?, y?} (ohne `open` umschalten) zeigt die
eigene Oberfläche eines Fremd-Plugins (VST3/LV2-Editor) in einem nativen
Fenster; `routing.plugins[].window` sagt, welche offen sind. Eingebaute
Plugins (4OSC, Sampler, Limiter …) haben keinen Editor — sie laufen über
`listParameters`/`setParameter` (Fehler `no editor: …`).

- Das Fenster hängt an der Engine: `te::Plugin::windowState` hält es,
  `HeadlessUIBehaviour::createPluginWindow` baut es über die Fabrik in
  `source/PluginWindows.cpp`. Die Engine schließt es, bevor das Plugin
  geht (removePlugin, Edit-Wechsel, Beenden) — kein Editor zeigt auf ein
  gelöschtes Plugin.
- Edit-Wechsel im selben Projekt (Auto-Branch, Checkout, Undo —
  `loadNodeEdit`) öffnet die Fenster der Module, die es weiter gibt, im
  neuen Edit wieder; `treeClose`/`treeOpen` schließen sie.
- Lage je Modul in `windows.json` (Datenablage); `x`/`y` legen das
  Fenster hin, Verschieben auf dem Desktop merkt sich die Lage, beim
  Schließen landet sie in der Datei. `x`/`y` sind die linke obere Ecke des
  ganzen Fensters samt Titelleiste des Desktops — so legt der
  Fenstermanager ein neues Fenster hin (vorher wanderte es je Öffnen um die
  Titelleiste nach unten, `todos/2026-10-25-plugin-fenster-lage-und-ui-stand.md`).
- Öffnen und Schließen ohne Änderung sind kein Teil des Songs: read-only
  in der Registry (keine Version), kein Undo, keine Node-Aktion, kein
  Auto-Branch.
- Änderungen im Editor sind eine Node-Aktion (Decision
  `docs/decisions/2026-10-24-plugin-fenster-als-aktion.md`): unidaw merkt
  sich beim Öffnen den Zustand des Plugins (Chunk, `openedState`); weicht
  er beim Schließen ab, zählt `executeOp` das Schließen als Mutation —
  zusammengefasste process-Aktion `pluginWindow` am Node, Version, Undo,
  Auto-Branch wie bei `setParameter`, Antwort `changed: true`. Weil die
  Änderung beim Schließen schon im Plugin steckt, legt `executeOp` vor
  Schnappschuss und Auto-Branch den Zustand beim Öffnen zurück ins Plugin
  und merkt sich den neuen (`beginPluginWindowChange`); der Handler setzt
  ihn im (evtl. neuen) Edit wieder ein — so bekommt der Eltern-Node den
  alten Stand, der Kind-Node die Änderung, und Undo holt den alten zurück.
  Auto-Branch greift erst beim Schließen. Eine Op mit Node-Aktion am Modul
  (`setParameter`, `loadPreset`, Automation …) bucht sich selbst: danach
  beginnt der Vergleich neu (`pluginWindowSettled`). Ein Edit-Wechsel
  (Auto-Branch einer anderen Op, Checkout, Undo) öffnet das Fenster neu
  und nimmt den Zustand dort als neuen Anfang.
- Schließen über die Titelleiste des Fensters läuft über denselben Weg:
  `EditorWindow::closeButtonPressed` ruft `executeOp` mit `pluginWindow
  {open: false}` (danach, per `callAsync`) — so stimmen Aktion,
  Auto-Branch, Undo und Version, das Event-Log zeigt das Schließen, und
  das UI sieht die neue Version.
- Anzeige: JUCE zeichnet über X11 (unter Wayland XWayland) und braucht
  `DISPLAY` (der Dienst `unidaw-7400` bekommt es per `-E DISPLAY=:0`). Ohne
  `DISPLAY` meldet die Op `no display: …` — JUCE nähme bei leerem
  `DISPLAY` still `:0`, unidaw nicht. `--window-sim` ersetzt das Fenster
  durch ein Component ohne Desktop-Fenster (Test-Suite; die Suite leert
  zusätzlich `DISPLAY` für alle Instanzen). `pluginWindowSim` {plugin,
  param, value 0..1} dreht dort einen Regler wie der Editor (nur mit
  `--window-sim`, wie `interfaceSim`).
- X-Fehler (`source/XErrors.cpp`): JUCE setzt seinen X-Fehler-Handler nur
  für eine `JUCEApplication` (`isStandaloneApp()` in
  `juce_XWindowSystem_linux.cpp`); unidaw startet mit
  `ScopedJuceInitialiser_GUI`, ohne eigenen Handler beendete jeder
  X-Fehler den Prozess (Xlib: exit 1). `main` ruft gleich nach dem
  Initialiser `installXErrorHandler` — gilt für unidaw, Plugin-Host und
  Scan-Kind. Mit `DISPLAY` lädt es libX11 über JUCEs `X11Symbols`
  (`JUCE_GUI_BASICS_INCLUDE_XHEADERS`, wie `juce_gui_extra`; nichts neu
  gelinkt), ruft `XInitThreads` und setzt den Handler, wie JUCE es für
  eine App tut. Der Handler loggt
  `[x11 <pid>] X error: <Fehler> (code n), request <Request> (m), ignored`
  — derselbe Fehler beim 1., 10., 100. … Mal — und lässt den Aufruf
  scheitern (JUCE prüft das Ergebnis, z. B. `isHidden`). Ohne `DISPLAY`
  tut es nichts. IO-Fehler (Verbindung zum X-Server weg) bleiben beim
  Standard von Xlib (Meldung, exit 1): JUCE verbindet sich nicht neu und
  beendet auch eine App; der Dienst startet neu (`Restart=on-failure`).
- Liegengebliebene X-Ereignisse (`dispatchQueuedXEvents`, ebenfalls
  `XErrors.cpp`): JUCE arbeitet X-Ereignisse nur ab, wenn die Verbindung
  lesbar wird. Eine Anfrage mit Antwort von außerhalb (`isWindowShowing`
  bei jeder Op `pluginWindow`) liest ein ConfigureNotify mit in die
  Warteschlange von Xlib — dann blieb es liegen, JUCE behielt die alte
  Lage (Lage um den Rahmen versetzt, Test unter Last 2026-10-30).
  `NativeEditorWindow` sieht deshalb alle 50 ms nach (`XEventsQueued`
  ohne Server-Anfrage) und reicht Liegengebliebenes an JUCEs Callback der
  X-Verbindung.
- Plugin-Sandbox (Abschnitt unten, Phase 2): das Fenster gehört dem
  Host-Prozess des Plugins. Die Op und die Funktionen in `PluginWindows.h`
  (Modul-Id rein, Fehlertext raus) bleiben; für einen Stellvertreter baut
  `createPluginWindow` statt `EditorWindow` ein `RemoteWindow` (Component
  ohne Desktop-Fenster, wie die Simulation), das den Host das Fenster
  öffnen, verschieben und schließen lässt. Der Host nutzt dieselbe
  Lage-Logik (`NativeEditorWindow`, aus `EditorWindow` herausgezogen:
  Lage samt Titelleiste) und meldet Lage und Klick auf Schließen über den
  Speicher; `RemoteWindow` trägt beides nach (`windows.json`, Op
  `pluginWindow {open: false}` wie `closeButtonPressed`). `--window-sim`
  bleibt in unidaw (`SimWindow`); `pluginWindowSim` dreht den Regler dann
  im Host, die Änderung kommt über den Rückweg. Stirbt der Host, geht das
  Fenster in unidaw zu (ohne Aktion; Änderungen im Editor seit dem letzten
  gelesenen Zustand sind mit dem Host verloren) und nicht von selbst
  wieder auf.

## Plugin-Sandbox

Fremd-Plugins in eigenen Prozessen (Decision
`docs/decisions/2026-10-25-plugin-sandboxing.md`, Konzept und Messungen
`docs/plugin-sandboxing.md`, Phasen im Todo
`todos/2026-10-25-plugin-sandbox-umsetzung.md`). Stand: **Phase 4**, VST3
und LV2, Standard an.

- **Schalter:** Standard an (nach Phase 3, laut Decision).
  Umgebungsvariable `UNIDAW_PLUGIN_SANDBOX=0` beim Start (Dienst:
  `Environment=`) schaltet ab: dann ersetzt unidaw die Engine-Fabrik nicht und
  scannt im Prozess — alles wie vor der Sandbox. Eine Umgebungsvariable
  statt einer Einstellung: ein Notausgang für den ganzen Daemon, kein
  Zustand in `--config`, der in andere Instanzen wandert. Die Gruppierung
  je Plugin ist dagegen eine Einstellung (unten). Die Suite prüft beides
  (`UNIDAW_PLUGIN_SANDBOX=0` für die Suite-Instanz); Referenzen „ohne
  Sandbox“ lädt sie per `pluginHosting dawd` im Prozess.
- **Einhängepunkt ohne Engine-Patch:** `installPluginSandbox` ersetzt
  `PluginManager::createPluginInstance`. VST3 und LV2 (seit Phase 4) →
  `RemotePluginInstance` (Stellvertreter, `source/PluginSandbox.cpp`),
  eingebaute Plugins wie bisher. Für die Engine bleibt es ein `te::ExternalPlugin` mit
  derselben `PluginDescription`: Parameter-Ids (Index), Automation,
  Edit-Datei, Undo, Rezepte, Presets sehen keinen Unterschied.
- **Host-Prozess:** dieselbe Binary, `unidaw --plugin-host <socket-fd> --parent
  <pid>` (`source/PluginHost.cpp`) — gleiche JUCE-Patches (auch die
  Run-Loop für VST3-Editoren, Diva), gleiche Zustands-Blobs, derselbe
  X-Fehler-Handler (Abschnitt „Plugin-Fenster“). Start per
  `posix_spawn`, nur ein Socket (SOCK_SEQPACKET) als fd 3 (alle anderen fds
  zu). Je Instanz schickt unidaw einen memfd über den Socket (SCM_RIGHTS);
  der Host legt dafür eine Instanz mit eigenem Steuer- und RT-Thread an
  (beide lesen ihren Start-Zähler, bevor sie starten — 0a6b1d1), der
  Message-Thread ist gemeinsam. Op `quit` entfernt nur die Instanz; der
  Prozess endet, wenn unidaw den Socket schließt (letzte Instanz weg; sonst
  SIGKILL nach 2 s) oder unidaw weg ist (Eltern-PID alle 0,5 s geprüft).
- **Gruppierung** (Phase 3): Schlüssel Instanz → Prozess je Plugin
  (Format, Name, uid). `plugin` (Standard): alle Instanzen eines Plugins in
  einem Prozess; `single`: je Instanz einer; `dawd`: im Prozess von unidaw.
  Op `pluginHosting {plugin, mode?}` (Modul-Id rein, gilt für alle Module
  dieses Plugins), Datei `<config>/plugin-hosting.json` (nur Abweichungen),
  `routing.plugins[].hosting`. Gilt für Instanzen, die danach entstehen
  (Projekt öffnen, Node-Wechsel, Undo); ein Neustart nach Absturz nimmt
  `single`/`plugin` aus der Einstellung, `dawd` erst beim nächsten Laden.
- **Speicher je Instanz:** ein memfd (`source/PluginSandboxShared.h`,
  Version 4, nicht in `/dev/shm`, nichts zum Aufräumen).
  - RT-Kanal, synchron im selben Block: unidaw schreibt Audio (in place,
    bis 32 Kanäle × 8192 Frames, größere Blöcke in Teilen), MIDI, Playhead
    (`PositionInfo`), Flags (nonRealtime, bypass, reset), zählt
    `rtRequest` hoch (Futex); der RT-Thread des Hosts (SCHED_FIFO mit
    `TRACKTION_RT_PRIORITY`) rechnet und meldet `rtDone`. Keine
    zusätzliche Latenz; die Latenz des Plugins meldet der Stellvertreter
    der Engine wie bisher (`setLatencySamples`).
  - Parameter unidaw → Host: Wert und Zähler je Parameter, aus jedem Thread
    ohne Nachricht. Der Host übernimmt geänderte Werte vor jedem Block und
    vor Zustand/Text/Info und schreibt den übernommenen Zähler zurück
    (`paramApplied`).
  - Rückweg Host → unidaw (Phase 2): ein `AudioProcessorListener` am Plugin
    im Host schreibt, was das Plugin selbst meldet — Werte aus dem Editor
    oder aus dem Block (VST3-Ausgangs-Parameter), Gesten (Zähler Anfang/
    Ende), neue Namen/Programme (`infoSeq`) — und zählt `hostEvents` hoch.
    Der Stellvertreter holt das auf dem Message-Thread ab
    (`syncFromHost`): vor jeder Op (`executeOp` → `syncSandboxedPlugins`),
    beim Zustand für den Fenster-Vergleich und sonst per Timer alle 50 ms.
    Werte gehen wie im Prozess an die Listener (`ExternalAutomatableParameter`,
    `ProcessorChangedManager`), Gesten als `begin/endChangeGesture`. Ein
    Wert, den unidaw gesetzt und der Host noch nicht übernommen hat
    (`paramSeq ≠ paramApplied`), gewinnt — der Host übernimmt ihn gleich,
    wie die Automation im Prozess. Jede Meldung trägt den unidaw-Wert, den
    der Host da schon übernommen hatte (`outBase`, Phase 4); setzte unidaw
    seitdem einen neuen Wert, ist die Meldung veraltet und fällt weg
    (LV2 meldet bei `prepare`/`setState` alle Werte aus den Ports, bevor
    der Host den offenen unidaw-Wert übernimmt — der Wert sprang sonst
    zurück). Nach `infoSeq` holt der Stellvertreter
    Namen, Einheiten, Werte, Programme (Op `info`) und ruft bei einer
    Änderung `updateHostDisplay` (die Engine liest neu; Surge XT Effects
    benennt seine Parameter nach dem ersten Block um). Mit `syncController`
    gleicht der Host vorher den VST3-Controller mit der Komponente ab
    (`syncPluginParameters`; warum: `docs/engine-bewertung.md`, VST3 mit
    dynamischen Parametern).
  - Fenster: der Host meldet die Lage samt Titelleiste und Klicks auf
    Schließen (Zähler); `RemoteWindow` in `PluginWindows.cpp` fragt das
    alle 100 ms ab.
  - Steuerkanal (Futex, JSON + Blob, aus jedem unidaw-Thread außer dem
    Audio-Thread): `load`, `prepare`, `release`, `getState`, `setState`,
    `setProgram`, `getText`, `getValueForText`, `info`,
    `setProgramName`, `setLayout` (prüfen/übernehmen), `hasEditor`,
    `openWindow`, `moveWindow`, `closeWindow`, `editParam` (nur
    `--window-sim`), `quit`; im Host auf dem Message-Thread (VST3 verlangt
    ihn). `hasEditor` fragt erst, wenn unidaw ein Fenster öffnen will (nach
    der Prüfung auf ein Display): JUCE legt dafür eine VST3-View an, LSP
    beendet ohne Display dabei den Prozess.
- **Bus-Layout:** `isBusesLayoutSupported` fragt den Host,
  `processorLayoutsChanged` übernimmt das neue Layout dort (die Engine gibt
  das Plugin vorher frei und bereitet es danach neu vor). So wirkt ein
  Layout aus der Edit-Datei (z. B. Sidechain-Bus aus) wie im Prozess.
- **LV2** (Phase 4): dieselben Wege wie VST3. Die JUCE-Patches `juce-lv2-*`
  wirken im Host (dieselbe Binary). Dazu:
  - Der Host lädt einmal je Prozess alle LV2-Klassen
    (`searchPathsForPlugins`, wie das Scan-Kind) — sonst sagt die
    Beschreibung der Instanz Kategorie „Plugin“, `isInstrument` false, und
    die Engine übernimmt sie (`ExternalPlugin::initialise` → `isSynth`).
  - Werte, die erst ein Block zum Plugin bringt (Patch-Parameter, Werte ohne
    Graph): `passPendingValuesToPlugin` (Patch
    `tracktion-flush-pending-values`) gibt dem Stellvertreter 1 Sample vom
    Message-Thread. Ein solcher Block wartet wie ein schwerer Aufruf (10 s
    statt einen Block) und übernimmt danach sofort, was das Plugin darin
    meldete (`syncFromHost`) — im Prozess gehen diese Meldungen im Block
    selbst an die Listener.
  - Editor-Warteschlange: ein offener LV2-Editor im Host stellt seine
    Änderungen für den nächsten Block an; die Engine zählt nur ihre eigenen
    Editoren (`numOpenEditors`). `markLv2EditorPending` setzt darum vor
    jeder Op und alle 100 ms (`RemoteWindow`) `markValuesPending` — das
    Sichern ohne Graph gibt dann 1 Sample wie im Prozess.
  - Neustart nach Absturz: nach `setState` und `prepare` 1 Sample im neuen
    Host, bevor der Audio-Thread ihn nimmt — LV2 meldet wiederhergestellte
    Patch-Parameter erst aus einem Block (sonst zeigte unidaw danach die
    Standardwerte).
- **Fristen** (Phase 3):
  - Steuerkanal: `load` 60 s; schwere Aufrufe (`prepare`, `release`,
    Zustand, Programm, Layout übernehmen, Fenster auf/zu) 10 s, dann wird
    der Host beendet — ihr Ergebnis muss stimmen; leichte (Texte, `info`,
    `hasEditor`, Layout prüfen, Fenster verschieben, Programmname) 2 s, dann
    Ersatz (Text = Zahl, kein Editor, Layout nein) — der Host darf
    nachreichen, bis dahin geben weitere leichte Aufrufe sofort auf, schwere
    warten höchstens bis 10 s nach der offenen Anfrage.
  - RT-Kanal: live ein Block (mindestens 5 ms), Render 30 s (dann beendet).
    Verpasst → Stille in diesem Block, Zähler `missedBlocks`; ist der Host
    noch am alten Block, bleibt der nächste ohne Warten still. Ein toter
    Host fällt über `pidfd` sofort auf (Render: in 50-ms-Scheiben geprüft).
  - Wächter (Timer des Stellvertreters, 50 ms): Antwort 10 s offen, ohne
    dass jemand selbst wartet, oder Echtzeit-Block 5 s im Host → Host
    beenden. Nicht für Render-Blöcke (die wartet der Render selbst).
  - Ein Render, in dem ein Stellvertreter einen Block nicht bekam, liefert
    keine Datei, sondern `plugin host failed during render (output
    incomplete): <Namen>` (`runRender`).
- **Absturz und Neustart** (Phase 3): tot = Prozess beendet (`pidfd`) oder
  von unidaw beendet (Frist, Wächter). Der Stellvertreter merkt es im Timer
  oder vor jeder Op (`syncSandboxedPlugin` → `checkHost`), einmal je Kanal:
  Grund festhalten (Signal, Exit-Code, „not responding …“), Absturz je
  Plugin zählen (einmal je Prozess), offenes Fenster des Moduls in unidaw zu
  (`closeWindowOfLostHost`, ohne Aktion, nicht wieder auf). Dann neu laden
  (Message-Thread, synchron): Host der Gruppe holen oder starten, `load`,
  Bus-Layout, letzter Zustand, `prepare` wie zuletzt; erst danach setzt er
  den neuen Kanal für den Audio-Thread (`live`, atomar); alte Kanäle
  bleiben bis zum Ende des Stellvertreters stehen (der Audio-Thread könnte
  sie noch halten). Werte, die unidaw setzte, während der Host weg war,
  gehen hinterher. Stirbt der neue Host beim Laden, zählt das als
  Absturz.
- **Sperrliste:** 2 Abstürze eines Plugins in 60 s → gesperrt (`blocked`,
  bleibt stumm, kein Neuladen) bis Op `reloadPlugin {plugin}` (hebt die
  Sperre für das Plugin auf, lädt das Modul neu; andere Module desselben
  Plugins folgen über ihren Timer). Scheitert das Neuladen ohne Absturz
  (z. B. Plugin fehlt), bleibt das Modul `failed` bis `reloadPlugin`.
  Neue Instanzen (Projekt öffnen, Undo) laden auch bei Sperre — das ist ein
  Schritt des Nutzers, keine Schleife.
- **Zustand:** über den Steuerkanal, byteweise wie im Prozess. Der
  Stellvertreter merkt sich den letzten Zustand (jedes `getState`/
  `setState`; Undo liest ihn nach jeder Op) und liefert ihn, wenn der Host
  weg ist — Speichern löscht ihn dann nicht, der Neustart nimmt ihn.
- **Status:** `routing.plugins[].sandbox` {pid, missedBlocks, failed, group,
  state running|restarting|blocked|failed, crashes, restarts, message?} nur
  bei Modulen im Host-Prozess; `message` ist der Hinweis für Nutzer und
  Agent (Englisch). `position.sandboxSeq` zählt bei jedem Ausfall,
  Neustart, Sperren — das UI lädt dann den Stand (Zeichen im Ketten-Slot,
  Hinweis mit „Reload“ in der Parameter-Karte, Toast). `pluginWindow` auf
  einem Modul ohne Host: `plugin host not running: <Name> (<Hinweis>)`.
- **Plugin-Scan im Kindprozess** (Sandbox an): `scanPlugins`
  (`ops/Processing.cpp`) gibt neue/geänderte Dateien je Format an
  `scanPluginFilesInChild`; das Kind (`unidaw --plugin-scan <fd> <format>
  <dateien>`) schreibt je Datei eine JSON-Zeile auf fd 3 (nicht stdout —
  dort schreiben Plugins selbst). Stirbt es, gilt die Datei in Arbeit als
  kaputt (Frist 120 s je Datei), ein neues Kind macht mit der nächsten
  weiter. Kaputte Dateien stehen mit Grund im Scan-Cache (bis sie sich
  ändern) und in `listPlugins` `scanErrors`.
- **Messung** (Phase 4, `docs/plugin-sandboxing.md`, Abschnitt 0): Render
  mit echten VST3/LV2 ~14 µs je Plugin und Block mehr, CPU +8 bis +19 %,
  PSS ~21 MB je Host; live nicht gemessen (Handaufgabe im Todo).
- **Fehlt nach Phase 4** (Todo): Live-Messung mit echten Plugins; ein Hänger
  im Render kostet weiter einmal 30 s; jeder Parameter-Text ist ein Rundlauf
  (~0,4 ms); Gesten und `changeProgramName` ohne API-Test; ein Wechsel der
  Gruppierung wirkt erst beim nächsten Laden. Ein Edit-Wechsel startet neue
  Host-Instanzen (bei `plugin` im laufenden Host des Plugins), ein offenes
  Fenster geht dabei im neuen Host wieder auf (wie im Prozess neu erzeugt).

## Routing-Matrix

Plugins hängen nicht an Spuren. Jedes Plugin ist ein **Modul** im Projekt;
die Spuren laufen über Kanten frei durch die Module bis **Main**
(`source/Routing.{h,cpp}`, Ops in `ops/Processing.cpp`):

- Knoten: Gruppenspuren `t<i>` (Index wie in `state.trackList`; Lanes sind
  keine Knoten, sie laufen in ihre Gruppe), Module `p<N>`, Gruppen-Busse
  `g<N>` (Submix, Abschnitt „Gruppen (Submix)“), `main`.
- Klassen: `midiFx` (MIDI → MIDI), `instrument` (MIDI → Audio), `audioFx`
  (Audio → Audio). MIDI geht nur in midiFx/Instrumente, Audio nur in
  audioFx und Main; MIDI erreicht Main nur über ein Instrument. Keine
  Schleifen, Fan-in summiert, Fan-out erlaubt. Die Fehlertexte sind
  API-Vertrag; `routing.matrix` liefert je Zelle den Grund — das UI
  dupliziert die Regeln nicht.
- Kein Default-Instrument: eine neue MIDI-Spur ist stumm, bis sie geroutet
  wird; eine neue Audio-Spur hängt an Main.
- Engine-Abbildung (alles im Edit, also je Baum-Knoten; Branches erben):
  ein Modul ist eine versteckte Bus-Spur `[AuxReturn Bus N][Rack(Plugin)]`
  (`dawdRole=module`, solo-isolated) — das Rack bringt Dry/Wet für jedes
  Plugin (`setWet`). Jede Kante ist ein AuxSend am Ende der Kette der
  Quelle, sein Gain ist der Pegel der Kante (`setEdge`); Main ist ein Bus
  wie die Module (`dawdRole=main`, `[AuxReturn Bus 0]` → Master). Der
  Track-Output jeder Spur und jedes Moduls geht in die Void-Spur
  (Lautstärke −∞, **nicht** gemutet — `isMuted (true)` erbt das Mute des
  Ziels und würde die Sends der Quelle mit stummschalten). Send/Return
  tragen MIDI mit.
- **Feedback** (`Feedback.{h,cpp}`): der Tracktion-Graph ist azyklisch
  (`ReturnNode` wirft rückgekoppelte Sends still raus). Feedback-Kanten
  laufen über ein eigenes Built-in-Paar: `dawdFeedbackSend` am Ende der
  Quelle schreibt in einen Ringpuffer (je Edit + Schlüssel),
  `dawdFeedbackReturn` im Ziel-Modul vor dem Rack liest verzögert
  (mindestens ein Audio-Block, bis 2 s), Pegel, Soft-Clip (tanh). Die
  Position zählt in Blöcken — beide laufen einmal je Block, die
  Mindestverzögerung liest nur fertig geschriebene Blöcke (deterministisch,
  auch im Render). Nur Audio, Ziel immer ein Modul (auch es selbst).
  Feedback-Kanten zählen nicht für den Schleifen-Check.
- **Sidechain** (`Sidechain.{h,cpp}`): `connect {sidechain: true}` legt
  das Signal einer Quelle (Spur oder Modul, hinter ihrer Kette) als
  Schlüssel in den zweiten Eingang eines Audio-Effekt-Moduls (Ducking).
  Die Engine kennt Sidechain nur für Plugins direkt auf einer Spur
  (`Plugin::sidechainSourceID`: Send hinter der Quell-Spur, Return mit
  Kanal-Drähten in den Plugin-Eingang), Plugins im Rack bekommen keinen
  (`Plugin::canSidechain` ist im Rack false). Deshalb trägt ein eigenes
  Built-in-Paar auf der Modul-Spur den Schlüssel: `dawdSidechainIn` vor
  dem Rack (Quelle, Drähte 1/2 → 1/2 und Schlüssel → 3/4), die RackInstance
  nimmt vier Kanäle und gibt 3/4 auf die Rack-Pins, die das Wrapper-Rack
  schon mit den Sidechain-Pins des Plugins verbunden hat (eingebauter
  Compressor: "Sidechain Trigger", VST3/LV2: zweiter Eingangsbus);
  `dawdSidechainOut` hinter dem Rack leert 3/4 (sonst trüge der
  Dry-Anteil den Schlüssel weiter). Kein Engine-Patch, keine Verzögerung.
  Ein Schlüssel je Modul, ohne Pegel (Sidechain-Gain des Plugins); der
  Compressor schaltet `useSidechainTrigger` mit. Sidechain-Kanten zählen
  für den Schleifen-Check (beide Richtungen), die Simple-Sicht markiert
  sie als complex; fällt die Quelle weg, räumt `pruneSidechains` (nach
  jeder verändernden Op und beim Laden) den Schlüssel.
- **Simple-Mode**: je Spur eine Kette (`setChain`, Reihenfolge, Rollback
  bei Fehlern), verlinkte Instanzen (`linkPlugin`: Kopie mit gleichen
  Einstellungen, eigenes Signal; `setParameter` folgt der Link-Gruppe
  `dawdLink`, Wet bleibt je Instanz), Sends als markierte Kanten
  (`dawdSend` am AuxSend). `routing.chains` leitet die Simple-Sicht aus
  dem Graphen ab und markiert, was nur Complex darstellt (Feedback,
  Verzweigungen, Pegel ≠ 100 % in der Kette). Kanten hinter dem letzten
  Modul sind Post-Fader-Sends (`postSends`, Decision
  `docs/decisions/2026-10-24-post-fader-sends.md`), keine Verzweigung: ein
  markierter Send immer, eine normale Kante in ein Modul, wenn die Kette
  daneben ein Ende hat (Main, Master-Kette, Gruppe — diese Kanten sind
  Enden, nie Sends; ohne Ende ist die einzige Kante in ein geteiltes Modul
  das Ende; eine Spur ohne eigene Kette direkt in die Master-Kette endet
  dort, `toMaster`, kein Send). Der Fader bleibt am Kettenende, der AuxSend sitzt hinter ihm
  (`[Rack][Fader][Meter][Sends]`). `setChain` nimmt sie ans neue
  Kettenende mit (Art und Pegel bleiben). Ein Graph, zwei Ansichten.
- Lese-Sicht `routingState`: ohne `compact` mit der Zell-Matrix fürs UI
  (je Zelle Grund, Feedback- und Sidechain-Grund); `compact` ohne sie,
  `compact` + `reasons` dieselben Gründe je Quelle gruppiert (`allowed`,
  `blocked {Grund: [Ziele]}`, je für Kanten, Feedback, Sidechain) — für
  Agenten (MCP `detail: true`; Praxistest 1l: die Matrix hatte 126 000
  Zeichen).
- Spur-Indizes der API zählen nur Nutzer-Spuren (`userTracks`), Module und
  Void sind unsichtbar; neue Nutzer-Spuren landen vor den Routing-Spuren
  (automatische Namen "Track N" bleiben richtig). Lanes hängen hinten an
  und belegen die nächsten Indizes — stabile Adresse ist der Spurname
  (`docs/decisions/2026-10-24-spurnamen-als-adresse.md`).
- `ensureRouting` läuft bei jedem Laden: legt Void- und Main-Spur an,
  räumt halbe Feedback-Kanten und migriert alte Edits — v0 (Plugins der
  Spur → Modulkette bis Main, 4OSC der MIDI-Spur → Instrument-Modul,
  Lane-Synths raus), v1 (Track-Output auf Master → Main-Send, Modul-Plugin
  → Rack-Wrap); Edit-Feld `dawdRouting=2`.
- `treeCompact` zieht die Spur-Ids in `connect`/`disconnect`-Aktionen nach.

## Freeze/Flatten

Seit 2026-10-24 (`ops/Freeze.cpp`): eine Spur samt eigener Kette als Audio
spielen — die Plugins der Kette ruhen (CPU). Gemessen (Renderzeit, 32 s
Song, 4OSC mit 4 x 8 Unison-Stimmen und Vierklängen plus Delay, Ryzen 7
4800H): live 2,4 s (7,5 % der Songlänge), gefroren 0,7 s (2,1 %). Ein
solches Instrument kostet also rund 5 % Echtzeit; spürbar erst bei vielen
schweren Spuren.

- **Eigener Weg statt Freeze Point der Engine**: der friert Plugins auf der
  Spur selbst ein; bei unidaw liegt die Kette auf Modul-Spuren hinter
  AuxSends (die Engine lehnt das ab: „connected to other tracks through a
  Rack or aux send“), und er rendert über `renderToFile`, das headless die
  Progressbar braucht. unidaw rendert mit dem vorhandenen Render-Weg
  (`renderWithParams`, Clips der Spur über `allowedClips` wie die Stems).
- **Was eingefroren wird**: die eigene Kette in der Simple-Sicht
  (`ownChainOf` = `deriveChain`: Module, die nur ihr Vorgänger speist, der
  Reihe nach). Die Kette muss der einzige Ausgang der Spur sein; davor
  keine Verzweigung, kein Send, kein Feedback, kein Pegel ≠ 100 % — hinter
  dem letzten Modul ist alles frei (Sends, geteilte Module wie ein Hall,
  Kantenpegel). Geteilte Module werden nie eingefroren; eine Spur ohne
  eigenes Modul lehnt `freezeTrack` mit Grund ab, ebenso eine geärmte Spur
  (Entscheidung Matthias 2026-10-24; nichts ändert sich).
- **Abgriff**: hinter dem Rack des letzten Moduls, vor Fader und Sends —
  für die Dauer des Renders ein AuxSend von dort auf Main, die Kanten des
  Moduls und der Fader der Spur aus (sitzt er vor der Kette, wirkt er
  gefroren danach). 32-Bit-Float-WAV in `freeze/` neben dem Edit, Rate des
  Geräts.
- **Gefroren**: das Audio liegt auf einer versteckten Spur (Rolle
  `freeze`, nicht in `userTracks`), deren Ausgang die Gruppenspur ist — wie
  eine Lane: Mute und Solo der Spur gelten mit (die Engine erbt beides über
  das Ausgangsziel). Die Clips der Spur und ihrer Lanes sind stumm, die
  Racks der Kette aus (Marke `dawdFrozenOff` = ItemID der Gruppe, nur
  diese schaltet Unfreeze wieder an). Das Audio läuft durch die Kette —
  abgeschaltete Racks lassen es durch, Busse, Fader und Sends bleiben live.
  Gespeichert wird das im Edit (Unfreeze/Undo/treeOpen). `state` zeigt
  `frozen` je Spur, `renderStems` und `analyze {track}` nehmen das
  gefrorene Audio statt der stummen Clips.
- **Sperre**: `executeOp` lehnt Ops ab, deren Fokus eine gefrorene Spur ist
  (`frozenGuard` über `focusOf`: Noten, Clips, Lanes, Arm, die Module der
  Kette) — „track N (Name) is frozen; unfreeze it first“. Frei bleiben
  `trackMix`, Automation von `t<i>.volume/pan`, `setEdge` ab dem letzten
  Modul, Favoriten und alles an anderen Spuren und geteilten Modulen.
  Section-Ops schneiden das gefrorene Audio wie Clips (Ausklänge über eine
  Schnittstelle hinweg bleiben, wie sie gerendert wurden).
- **Flatten** (`flattenTrack`): endgültig — das Audio wird ein normaler
  Clip der Spur (jetzt Audio), Module der Kette und das alte Material (auch
  auf den Lanes) gehen, die Spur geht dorthin, wohin das letzte Modul ging
  (gleiche Pegel, Sends bleiben Sends); der Fader zieht mit. Automation der
  Kanten des letzten Moduls geht dabei verloren.
- Beide sind Node-Aktionen (process, Fokus die Spur) und Undo-Schritte.

## Clip-FX

Seit 2026-10-26 (`ops/ClipFx.cpp`, Todo `todos/2026-10-26-clip-fx.md`):
`processClip {track, name, plugins, tail?}` rechnet einen Audio-Clip
offline durch Module — Effekte auf einem Clip, ohne Modul in der Kette der
Spur (Direct Offline Processing, Per-Item-FX der Longlist).

- **Kette = Module**: `plugins` sind Modul-Ids oder -Namen, nur
  Audio-Effekte, jedes einmal, in dieser Reihenfolge, mit ihren
  Einstellungen jetzt (Dry/Wet des Moduls gilt). Ein Agent legt ein
  unverdrahtetes Modul an (`addPlugin {name}` ohne Spur — still im Mix),
  stellt es ein und entfernt es danach; ein Modul in einer Kette geht auch.
  So gelten Parameter, Presets, Plugin-Fenster und Sandbox wie überall.
- **Render**: der vorhandene Weg (`renderWithParams`, nur dieser Clip über
  `allowedClips` wie Clip-Render und Freeze), 32-Bit-Float, Rate des
  Geräts (`freezeSampleRate`), Bereich = Clip + `tail` (0..30 s, Ausklang).
  Abgriffe wie beim Freeze für die Dauer des Renders: ein AuxSend am Anfang
  der Gruppenspur (eine Lane läuft dort hinein) in den Bus des ersten
  Moduls, hinter dem Rack jedes Moduls ein Send in das nächste, hinter dem
  letzten auf Main (Bus 0, an der Master-Kette vorbei); alles Übrige auf
  der Spur und hinter den Racks (Sends, Fader, Feedback) ist so lange aus.
  Ein stummer Clip oder eine stumme Lane klingt für den Render. VST3/LV2
  laufen in der Sandbox wie im Mix; fällt ein Block aus, liefert der Render
  den Fehler und keine Datei (`runRender`).
- **Was eingerechnet wird**: der Clip, wie er spielt (Offset, Länge, Gain,
  Pan, Tempo/followTempo, Warp, Loop, rückwärts, Transpose) — ohne seine
  Blenden und den automatischen Crossfade: sie bleiben am Clip und wirken
  danach hinter den Effekten (sonst doppelt bzw. vor dem Hall). Danach
  spielt der Clip `media/<Clip>-fx.wav` 1:1: Gain 0 dB, Tempo 1, kein
  followTempo/Loop/Warp, rückwärts aus, Offset 0, Länge + `tail`. Er folgt
  dem Song-Tempo also nicht mehr (Tempo ändern: `clear`, ändern, neu
  rechnen).
- **Rückweg**: Kind `DAWDFX` am Clip — `ORIGINAL` (Kopie des Clip-Zustands
  vor dem ersten `processClip`, samt `DAWDWARP`, `LOOPINFO`), `source`
  (dessen Datei), `file` (die gerechnete), je Schritt `STEP {plugins,
  names, tail}`. Ein weiterer `processClip` rechnet die gerechnete Datei
  weiter (Schritte stapeln, `ORIGINAL` bleibt das erste). `clear: true`
  setzt die Eigenschaften und Kinder aus `ORIGINAL` zurück (Name, Mute und
  Blenden bleiben; erst die Datei, am Ende Lage und Loop noch einmal, weil
  die Engine beim Setzen von rückwärts/followTempo Loop-Punkte umrechnet)
  und meldet die Datei der Engine (`validateFile`). Lage: Sekunde x der
  gerechneten Datei = Sekunde x des Originals ab seinem Start — ein
  verschobener, gekürzter oder geteilter gerechneter Clip kommt an derselben
  Stelle mit demselben Ausschnitt zurück (nur solange er 1:1 spielt; sonst
  das ganze Original an seinem Start). `trimClip` vor den Dateianfang ist
  gesperrt (die Kopie mit Stille davor verschöbe den Rückweg).
- **Sichtbar**: `state` trägt `fx {steps, original, file}` je Clip;
  `clipPeaks` zeigt die gerechnete Datei. Node-Aktion process je Schritt
  (auch `clear`), Undo, Fokus die Spur (gesperrt auf gefrorenen Spuren),
  sperrt während der Aufnahme. Rezepte: die Modul-Ids in den params lassen
  `saveRecipe` am Node die Module mitnehmen.
- **Engine-Befund**: gerechnete Dateien sind Stereo; ein Mono-Clip auf
  derselben Spur klang danach +6 dB (CombiningNode: Mono-Hochmischung
  addierte ihren Eingang auf sich) — Patch
  `tracktion-channel-remap-shared-buffer.patch`.
- **Keine Live-Kette am Clip**: die Engine kann Plugins je Clip
  (`AudioClipBase::getPluginList`, im Graph vor den Blenden), unidaw nutzt
  sie nicht — Plugins sind Module (Routing-Matrix), und Parameter, Presets,
  Fenster, Automation, Sandbox-Status, Rezepte und UI kennen nur Module.
  Ein Clip-Plugin bräuchte all das ein zweites Mal; der Hall endete
  außerdem am Clip-Ende (der Clip-Node läuft nur über die Clip-Länge).

## Gruppen (Submix)

Seit 2026-10-24 (`ops/Groups.cpp`, Graph-Teil in `Routing.cpp`): mehrere
Spuren teilen Fader, Mute, Solo und eine eigene Kette.

- **Was die Matrix schon konnte**: Fan-in mehrerer Spuren in ein Modul
  (Bus-Kompressor, geteilter Hall), Pegel je Kante, Sends. Es fehlte ein
  Bus ohne Plugin, ein Fader/Mute/Solo dafür (ein geteiltes Modul hat einen
  Fader, die API erreichte ihn nicht; Module sind solo-isolated, ohne
  Mute), ein Name/Ordner in der Timeline, Stems je Gruppe — und `deriveChain`
  las eine Kante in ein geteiltes Modul als Verzweigung (der Fader wäre vor
  die Kette gewandert).
- **Gruppe = Knoten der Matrix** `g<N>`, ein Bus wie Main: versteckte Spur
  `[AuxReturn Bus N][Fader][Meter][Sends]`, Rolle `group`, solo-isolated,
  Ausgang in die Void-Spur, Name in `dawdLabel` (Adresse wie ein Modul-Name,
  eindeutig über Module und Gruppen). Die Bus-Nummer kommt aus derselben
  Folge wie die Module (`allocateBus`). Mitglied ist, wessen Weg nach Main
  (normale Kanten, ohne Sends) in die Gruppe läuft (`groupPathOf`,
  `groupMembers`) — keine zweite Datenhaltung. Sends gehen an der Gruppe
  vorbei wie in jedem Pult. Verschachteln geht über eine Kante Gruppe →
  Gruppe (Matrix).
- **Kein Folder/Submix der Engine**: der summiert die Track-Ausgänge seiner
  Kinder; bei unidaw parken alle Ausgänge in der Void-Spur und das Signal
  läuft über AuxSends — ein Folder hätte nichts zu summieren, und die
  Mitglieder müssten im ValueTree umziehen (Spur-Indizes, Lanes über das
  Ausgangsziel). Das Mute der Engine wirkt trotzdem: Mute des Gruppen-Busses
  schaltet seine Sends stumm (`TrackMuteState` der AuxSends).
- **Kette und Fader**: eine Gruppe ist für `deriveChain` ein Ende wie Main
  (`toGroup`), keine Verzweigung — Fader nach der Kette, Freeze und
  `setChain` der Mitglieder bleiben, wie sie waren; `addPlugin at: end` einer
  Spur landet vor der Gruppe. Die eigene Kette der Gruppe: `addPlugin {after:
  g3}`, `setChain {group}`; ihr Fader sitzt wie bei Spuren hinter der Kette
  (`groupFaderOf`, `syncFaders` zieht ihn mit, Marke `dawdFaderOf` = `g3`).
- **Ops**: `createGroup {name?, tracks?}` (Bus in den Eingang von Main bzw.
  der Master-Kette, End-Kanten der Spuren in die Gruppe, Pegel bleiben),
  `setGroup {track, group|""}` (rein, umziehen, heraus in den Main-Eingang),
  `removeGroup {group}` (Mitglieder dorthin, wohin die Gruppe lief, Pegel =
  rein × raus × Gruppen-Fader, höchstens 100 %; `warning` nennt Kurven
  und Modifier auf `g<N>.volume/pan`, die mit dem Bus verloren gehen), `groupMix {group, volumeDb,
  pan, mute, solo, name}`. Ein Modul auf dem Weg, das auch eine andere Spur
  speist (geteiltes Instrument), zieht mit — beide müssen zusammen in die
  Gruppe, sonst Fehler vor jeder Änderung. Alle sind Node-Aktionen (process,
  `groupMix` zusammengefasst je Gruppe), Undo-Schritte; Fokus `song`,
  `setGroup` die Spur (frei auch bei gefrorener Spur — die Kante liegt
  hinter der Kette).
- **Mute/Solo** sind Abhör-Hilfen wie bei `trackMix` (Decision 2026-10-20):
  Mute über `listenMute` am Gruppen-Bus, Solo schaltet alle Mitglieder solo
  (auch die verschachtelter Gruppen; `solo` der Gruppe = alle Mitglieder
  solo). Renders, `peak` und `analyze` ignorieren beides.
- **Lesen**: `state.groups` (id, name, members, volumeDb, pan, mute, solo,
  chain, targets, fader — die Felder für Timeline und Mixer),
  `trackList[i].group` (innerste Gruppe), `routing.groups`, Zeilen/Spalten
  der Matrix, `chains[].toGroup` und Ketten der Gruppen (`group`),
  `levels.groups` (Pegel am Fader der Gruppe).
- **Stems**: `renderStems {groups: true}` — ein Stem je äußerster Gruppe
  (ihre Spuren samt Lanes und Freeze-Audio, durch die Gruppe), die übrigen
  Spuren einzeln.
- **Freeze/Sections**: Freeze eines Mitglieds friert seine Kette ein, die
  Kante in die Gruppe liegt dahinter (bleibt live); eine Gruppe selbst hat
  kein Material und wird nicht eingefroren. Section-Ops schneiden und
  verschieben die Automation des Gruppen-Faders wie jede Kurve.
- **Automation**: `g<N>.volume` / `g<N>.pan` (Entscheidung G2), der Fader
  hinter der Kette der Gruppe — die Kurve (und ein Modifier) wandert mit
  ihm (Abschnitt „Automation“). Flow-UI: „A“ in der Kopfzeile öffnet die
  Lane unter der Gruppe.
- Flow-UI: Gruppe als Ordner in der Timeline (Kopfzeile mit Fader, S/M,
  Kette, „+“ für ein Plugin in Process, × löst auf; Mitglieder eingerückt,
  einklappbar), Spur-Header auf die Kopfzeile ziehen = hinein, auf „+ Group“
  = neue Gruppe, auf die „+“-Zeile = heraus; Simple-Sicht zeigt die Kette der
  Gruppe, Complex die Gruppe als Zeile und Spalte; Export „groups as one“.

## Comping

Seit 2026-10-24 (`compLane`, `ops/Comp.cpp`, Helfer `compRange`/
`compRanges` in `EngineHelpers`): je Zeitbereich spielt eine andere Lane.

- **Der Comp ist eine Lane** der Gruppe („<Spur>.comp“, Marke `dawdComp`
  am Track) aus Kopien der Take-Clips; jeder Teil merkt seine Quell-Lane
  (`dawdCompFrom` = ItemID). Der erste `compLane` legt ihn als Kopie der
  Lane an, die gerade spielt; jeder weitere ersetzt den Bereich. Er wird
  die aktive Lane — `laneActive` bleibt der Ganz-Song-Fall (ein ganzer
  Take, oder der Comp zurück). Neue Takes zählen ihn bei der Nummer nicht
  mit.
- **Warum Clips und keine eigenen Daten oder Mute-Automation**: Section-
  Ops (Zeit-Helfer schneiden und verschieben Clips), Undo (Schnappschuss
  des Edits), Freeze (rendert die spielenden Lanes), Render und
  `exportMidi` (gemutete Lanes fehlen) arbeiten ohne eine Zeile Comp-Code;
  die Takes bleiben unberührt. Mute-Automation ginge nur für Audio (der
  Fader einer MIDI-Lane skaliert höchstens Anschläge, gehaltene Noten
  klängen weiter), eigene Bereichs-Daten
  müssten alle Zeit-Helfer nachziehen.
- **Blende** (`fadeMs`, Default 10 ms): mittig auf der Grenze — der alte
  Teil läuft eine halbe Blende hinein, der neue beginnt eine halbe davor.
  Die Überlappung ist ein Crossfade wie jede andere: Crossfade
  standardmäßig an, auch für Clips älterer Projekte beim Laden, Kurve
  convex, solange keine gesetzt ist (`refreshAutoCrossfades`, Merker
  `dawdAutoCurve`; Decision `docs/decisions/2026-10-25-crossfade-standard.md`).
  MIDI schneidet hart: eine Note gehört dem Teil, in dem sie beginnt.
- `state`: die Comp-Lane trägt `comp` [{lane, name, start, end}] (Mitte
  der Blende als Grenze, gleiche Lane am Stück als ein Bereich), ihre
  Clips `compFrom`. Node-Aktion process (Fokus die Gruppe; im collect-Node
  Arrangieren wie `splitClip`), Undo-Schritt; an einer gefrorenen Spur
  gesperrt.
- UI: jeder Take einer Gruppe mit mindestens zwei Takes hat unten eine
  Leiste — farbig, wo er im Comp spielt; Ziehen darin = `compLane`. Die
  Comp-Teile tragen oben die Farbe ihres Takes.

## Undo/Redo

`undo`/`redo {steps?}` (`source/Undo.{h,cpp}`, Ops in `ops/Tree.cpp`) arbeiten
über Schnappschüsse, nicht über den UndoManager der Engine — der kennt weder
die Node-Aktionen in `tree.json` noch Auto-Nodes. `executeOp` merkt sich vor
und nach jeder rückgängig machbaren Op (`isUndoableOp`: alle mit Node-Aktion
außer `record`, dazu der Feinschliff `setParameter`, `setEdge`, `setWet`,
`trackMix`, `groupMix`, `laneActive`, `pluginWindow` beim Schließen nach
einer Änderung im Editor) den Edit des Nodes (ValueTree nach `flushState`)
und seine Aktionen. Undo schreibt den Vorher-Stand in die Node-Datei und
lädt ihn; legte die Op per Auto-Branch einen Node an (oder `bounceNode`
seinen Bounce-Node — Redo stellt ihn mit dem Bounce als Basis her),
verschwindet er (nicht,
wenn von ihm inzwischen von Hand verzweigt wurde). Trug er eine Notiz,
sagt die Antwort es (`removedNotes`, `warning`); Tree-Ops auf ihn
hängen den Grund an „no such node“ an. Redo stellt den
Nachher-Stand wieder her, samt Node (gleiche Id, Basis = Stand des
Elternknotens; Notiz und Label wie beim Undo). Nur die Aktionen des betroffenen Nodes werden
zurückgedreht — Labels, Favoriten, Terminate am Baum bleiben. Historie: 50
Schritte je Projekt, in-memory; eine neue Op verwirft Redo; `treeClose`,
`treeOpen`, `treeSeed`, `treeReset`, `treeDelete`, `treeCompact` und
`newEdit` löschen sie (`clearsUndo`). `batch` erzeugt einen Schritt je
Befehl. `undoDelete` bleibt für den Toast nach dem Löschen und räumt einen
dabei leer gewordenen Auto-Node weg (Node-Feld `auto`).
Fader-Gesten über OSC (Befehl mit `osc`-Adresse, `UndoStep::mergeKey`):
folgt ein Schritt mit derselben Adresse auf demselben Node in weniger als
1 s, zieht `pushUndo` nur den Nachher-Stand des letzten Schritts nach — eine
Geste ist ein Schritt (Abschnitt „OSC-Fernsteuerung“).

## Sections als Zeitbereiche

Seit 2026-10-24 (Wunsch Matthias): Sections sind Zeitbereiche ohne Lücken
(Ripple bleibt), das Material ist frei — Clips, Aufnahmen und Automation
dürfen über Section-Grenzen laufen. Die Helfer in `EngineHelpers`
(`splitClipAt`, `insertTime`, `removeTime`, `duplicateTime`, `reorderTime`,
`autoSection`) tragen alle Section-Ops; keine Op schneidet selbst. Sie
schneiden auch das gefrorene Audio (`clipTracks`: Nutzer- und
Freeze-Spuren, Abschnitt „Freeze/Flatten“).

- **Kein Kürzen am Section-Ende**: `addClip`, `addMidiClip`, Aufnahme,
  `moveClip`, `trimClip`, `duplicateClip`, `setMidiNotes` (wächst bis zum
  nächsten Clip der Lane), `stretchClip`, `warpClip` und `loopClip`.
- **Auto-Section**: reicht Material hinter die letzte Section, hängt
  `autoSection` eine neue an — bis zum Materialende in ganzen Takten
  (aufgerundet), Name „Section N“, Tempo/Takt geerbt; die Antwort trägt
  `autoSection`. Bei der Aufnahme beim Stopp. Am Node steht sie als eigene
  collect-Aktion `addSection {name, auto: true}` hinter der Aktion der
  auslösenden Op (`recordAutoSection`, Entscheidung Matthias 2026-10-24),
  im selben Node — kein Auto-Branch mitten in einer Op; Sorte des Nodes und
  Take-Daten bleiben, Undo nimmt beides zurück. Gegenweg
  (`retractAutoSection`, Todo `2026-10-29-clip-fallen`): zieht sich das
  Material im selben Node wieder zurück (`trimClip`, `deleteClip`,
  `moveClip`, `stretchClip`, `warpClip`, `loopClip` — Chops aus einem ganzen
  Stem), schrumpft die letzte Section, wenn sie eine Auto-Section dieses
  Nodes ist (Marke `dawdAutoSection`), auf das Materialende in ganzen Takten
  oder fällt samt Aktion weg (`autoSectionShrunk`/`autoSectionRemoved`);
  `updateSection` nimmt die Marke weg — angefasst bleibt sie.
- **Zeit einfügen** (`addSection` mitten im Song, Section verlängern,
  Kopie bei `duplicateSection`): ein Clip über der Einfügestelle wird
  geteilt (Entscheidung Matthias 2026-10-24), der hintere Teil rückt mit;
  die Automation hält in der Lücke den Wert der Stelle.
- **Zeit entfernen** (`deleteSection`, Section kürzen, `treeCompact`):
  Clips darin fallen weg, Clips über den Grenzen werden dort
  abgeschnitten; Automation darin fällt weg, an der Naht springt sie.
- **`moveSection`**: Clips werden an jeder Grenze geteilt, an der sich die
  Verschiebung ändert; jeder Teil wandert mit seiner Section, die
  Automation stückweise auch.
- **`duplicateSection`**: kopiert die Clip-Teile (auch von Clips, die
  davor beginnen), die Automation und die Marker des Bereichs.
- Teilen/Schneiden hält das Material an seiner Songzeit: MIDI-Noten, die
  ein Teil nur verdecken würde, fallen weg (wie bei `splitClip`), der
  Audio-Offset wächst nur (`setStart` mit preserveSync, nie unter 0 — siehe
  `trimClip`). Ein abgetrennter Teil heißt frei („long6 2“) und ist keine
  Pattern-Kopie mehr.
- Automation wird stückweise umgelegt: an jeder Stückgrenze hält ein
  Punkt den Wert der alten Kurve; Punkte, die nichts ändern, fallen wieder
  weg — eine Kurve außerhalb des Bereichs behält genau ihre Punkte.
- **Marker** (2026-10-25): benannte Zeitpunkte neben den Sections, im Edit
  unter `DAWDMARKERS` (also je Node, Undo über die Schnappschüsse). Die
  Zeit-Helfer nehmen sie mit wie Clips: Einfügen schiebt, Entfernen löscht
  die im Bereich, Umordnen zieht sie mit ihrem Stück (Schalter
  `markersFollowTime` in `EngineHelpers.cpp`; Empfehlung, Matthias
  2026-10-25), `duplicateTime` kopiert sie mit eindeutigem Suffix („Drop
  2“, Matthias 2026-10-25).

## Tempo und Takt je Section

Sections tragen optional Tempo und Takt (`dawdBpm`, `dawdTimeSig` am Marker,
`EngineHelpers` — `sectionTempos`, `applySectionTempos`). Ohne eigene Angabe
gilt die Vorgängerin, die erste Section ohne Angabe 120 BPM 4/4. Die
Tempospur der Engine wird nach jeder Section-Op und beim Laden aus den
Sections neu gebaut (Wechsel als Sprung am Section-Start, Takt auf den
nächsten Beat gerundet). **Tempo-Rampe** (2026-10-25, `dawdRamp`,
`updateSection {ramp}`; Voreinstellung — Frage an Matthias im Todo
clips-arrangement): der Punkt am Start einer Section mit Rampe hat Kurve 0
(linear in Beats bis zum nächsten Punkt) statt 1 (Sprung); ihr Tempo gleitet
zum Tempo der nächsten Section. Die Section behält ihre Sekunden; der Beat
des nächsten Punkts hängt von der Rampe ab — `applySectionTempos` führt ihn
nach, bis die Engine die Sekunde des Section-Starts trifft. Takte einer
Section mit Rampe zählt die Tempospur (`sectionBars`), `bars` setzen rechnet
die Stufen der Engine nach (`secondsForBars`: 4 Stufen je Beat, 1..100, je
mit dem Tempo am Stufenanfang); `exportMidi` schreibt dieselben Stufen. **Immer ohne Remap**: `TempoSetting::setBpm`
rechnet alle Positionen im Edit auf die Beat-Lage um — unidaw hält Sections und
Clips in Sekunden (`set (…, false)`). MIDI-Noten liegen in Beats und folgen
dem Tempo. Noten kommen über `mergeNotesRelative` in den Clip (die Engine
liest Zeitstempel als absolute Edit-Zeit). Audio-Clips mit followTempo
(auch gewarpt) behalten Start und Länge in Sekunden, ihr Material liegt in
Beats; den Offset (getrimmt, gewarpt, Stelle im Loop) hält die Engine aber
in Sekunden. Darum merken sich alle Section-Ops und `treeCompact` vor der
Änderung den Offset in Beats (`followTempoOffsets`) und stellen ihn nach
`applySectionTempos` wieder her (`keepOffsetBeats`, EngineHelpers) — auch bei
Rampe und geerbtem Tempo (Todo `2026-10-26-tempowechsel-haelt-clip-offset`).

**MIDI-Controller und Export** (2026-10-24): Controller-Events (CC,
Pitch-Bend) liegen in der `MidiList` des Clips in Beats ab dem
Inhaltsanfang, Werte in 14 Bit (CC `<< 7`, Pitch-Bend 0..16383). Die API
spricht Beats ab dem Clip-Anfang und MIDI-Werte (CC 0..127, `pitchBend`
−8192..8191): `setControllers` ersetzt die Kurve eines Controllers,
`clipNotes` liest sie (`clipControllers` in `EngineHelpers`), `copies: true`
ändert alle Clips des Patterns wie bei `setMidiNotes`. Teilen und
Schneiden lassen die Events an ihrer Songzeit wie die Noten. Die Engine
schickt beim Start eines Clips (und bei jedem Sprung) den letzten Wert
jedes Controllers vor der Position; steht dort kein Pitch-Bend, schickt
sie die Mitte (Patch `tracktion-midi-pitchbend-reset.patch`, Entscheidung
Matthias 2026-10-24) — sonst hielte das Instrument den Bend eines früheren
Clips oder einer gelöschten Kurve. `exportMidi`
schreibt dasselbe in die Datei (je Clip am Anfang des exportierten Bereichs
die Bend-Mitte, wenn davor und dort kein Bend steht — Entscheidung M6
2026-10-24, damit die Datei klingt wie die Wiedergabe) und
baut die .mid-Datei selbst aus den Clips (nicht über den MIDI-Render der
Engine, der durch den Graphen liefe): Typ 1, 960 Ticks je Viertel, Songzeit
→ Ticks über die Tempospur (`getPPQTime`), Tempo und Takt aus ihr (also aus
den Sections), eine Datei-Spur je MIDI-Spur; Material über Section-Grenzen
wird nur am Bereich des Exports abgeschnitten.

## Automation

Parameter über die Songzeit (`source/Automation.{h,cpp}`, Ops in
`ops/Automation.cpp`). Jedes Ziel hat eine API-Id; dahinter hängen
Engine-Parameter mit ihrer `AutomationCurve` — im Edit, also je
Baum-Knoten (Branches erben):

- `t<i>.volume` (dB −100..6) und `t<i>.pan` (−1..1): der VolumeAndPan-Fader
  der Spur, am Ende ihrer eigenen Kette (`faderTrackOf`). Ändert sich die
  Kette, wandert die Kurve mit. Eine MIDI-Lane hat keinen eigenen Fader
  (`faderTrackOf` liefert den ihrer Spur) und steht deshalb nicht in den
  Listen (`listAutomatable`, `automation`, `state.automation`); Audio-Lanes
  (Takes) haben ihren eigenen.
- `g<N>.volume` / `g<N>.pan` (seit 2026-10-24, Entscheidung G2 im Todo
  gruppen-submix): der Fader einer Gruppe, hinter ihrer eigenen Kette
  (`groupFaderOf`); er wandert mit ihr wie bei Spuren (`syncFaders`,
  `parkFader`), samt Kurve und Modifier. Nur Ids, keine Namen.
- `p<N>.wet` (0..1): Rack wet, dry = 1 − wet (zwei Kurven).
- `p<N>.<Parametername>` (normalisiert 0..1): verlinkte Instanzen bekommen
  dieselbe Kurve (wie `setParameter`).
- `edge:<von>><nach>` (Pegel 0..1): Gain des AuxSend. Kante an/aus über die
  Zeit = Stufe zwischen 0 und Pegel — der Graph bleibt strukturell gleich,
  Umschalten ist klick- und renderfest.
- `feedback:<von>><nach>.level` / `.delayMs`: AutomatableParameters des
  `dawdFeedbackReturn` (hängen an den alten CachedValues — alte Edits laden
  weiter; setzen nur über `setParameter`, ein direkt gesetzter CachedValue
  erreicht den Parameter nicht).

Punkte `{time, value, curve: linear|step}` in API-Einheiten; bei
`p<N>.<Parametername>` statt `value` auch `real` (angezeigte Zahl in der
Einheit des Parameters, wie `setParameter`), `resolveAutomationReals`
rechnet vor dem Prüfen in `value` um — die Node-Aktion hält `value`. `step` wird in
der Kurve ein Haltepunkt zur Zeit des nächsten Punkts, `readAutomation`
faltet ihn zurück. `setAutomation` ersetzt alle Punkte (Node-Aktion process,
Folgeaufrufe auf dasselbe Ziel per `coalesce` — vergleicht `param`).
Strukturänderungen über die Zeit (ein Modul kommt dazu) bleiben Sache der
Baum-Nodes. `treeCompact` zieht Spur-Ids in `param` nach. Automation läuft
über Section-Grenzen; die Section-Ops schneiden, verschieben und kopieren
sie wie Clips (Abschnitt „Sections als Zeitbereiche“).

**Modifier je Ziel** (`setModifier`/`clearModifier`, gleiche Ids): ein
`te::LFOModifier` (`type: lfo`, Standard) oder `te::StepModifier`
(`type: step`, seit 2026-10-24, Entscheidung L2 im Todo lfo-modifier) der
Engine, zur Kurve dazu (die Engine addiert den Ausschlag normalisiert auf
den Basis- bzw. Kurvenwert). Die Engine hat auch Random, Envelope
Follower, Breakpoint und MIDI Tracker; unidaw nutzt nur LFO und Step
(andere Typen sind ein Fehler). Je Ziel höchstens ein Modifier: ohne
`type` bleibt der vorhandene Typ, ein anderer Typ ersetzt ihn (neuer
Modifier mit den Vorgaben des Typs, `depth` bleibt). Der Modifier
liegt in der ModifierList der Spur, die den Parameter trägt (Fader-Spur,
Modul-Spur, Gruppen-Bus, Quellspur der Kante), die Zuordnung im Plugin-State
(`MODIFIERASSIGNMENTS`) — beides im Edit, also je Knoten, Undo über die
Schnappschüsse. Immer im Transport synchron: Phase bzw. Schritt kommen aus
der Songzeit, der Render ist bei gleicher Zeit gleich (Rate in Hz oder als
Notenwert; Notenwerte sind Bruchteile des Takts wie in der Engine).
Takt-Werte rechnet die Engine aus Tempo und Taktart, Sections brauchen
nichts. `depth` = ±Anteil des Regelbereichs, dry (= 1 − wet) bekommt die
Gegenrichtung, verlinkte Instanzen die gleiche Zuordnung. Step: `steps`
= 2..64 Werte −1..1 (Grenzen der Engine; Schritt k = Basis + steps[k] ×
depth), `rate` = Länge eines Schritts (Notenwert, neu `1/16`) bzw.
Schritte je Sekunde (Hz); die Stufen stehen in `stepData` des Modifiers.
Wandert der Fader (`syncFaders`), zieht der Modifier mit (`moveModifier`,
gleicher Typ und gleiche Einstellungen), und zwar mit dem eingestellten
Wert ohne Ausschlag (`settleModifierBase` — der aktuelle Wert schwingt,
`getVolumeDb` & Co. lesen ihn). Lesen: `modifier` in der Beschreibung
eines Ziels, `state.modifiers`. Rezepte nehmen alle Einstellungen als
`setModifier`-Schritt mit.

## OSC-Fernsteuerung

Live-Spiel per OSC (`docs/feature-longlist.md`: steht dort als Plan —
Live-Performances per OSC). `source/Osc.{h,cpp}`, Op
`oscSettings` (`ops/Osc.cpp`). Decision: Adressschema und Netz
(`docs/decisions/2026-10-26-osc-fernsteuerung.md`).

- **Empfang**: UDP, eigener Thread (`OscListener`) mit `DatagramSocket` und
  einem kleinen OSC-1.0-Zerleger (`parseOsc`: Nachrichten, Bundles,
  Argumente i f s d h T F) — JUCEs `OSCReceiver` nennt den Absender nicht,
  sein Zerleger ist nicht öffentlich; die Nachrichten-Typen kommen aus
  `juce_osc` (Teil der Engine, keine neue Abhängigkeit). Standardmäßig aus;
  an nur über `oscSettings {enabled: true}` (UI: Audio-Popover). Bindet nur
  an `address`, Standard `127.0.0.1` — eine Adresse des Rechners oder
  `0.0.0.0` öffnet den Port ins Netz; das muss man ausdrücklich setzen.
- **Absender**: Pakete nur von 127.0.0.1 (immer) und den IPv4-Adressen in
  `allow` (`oscSettings {allow: [...]}`, Standard leer = nur dieser
  Rechner; IPv6 lauscht nicht). Alle anderen verwirft der Empfangs-Thread
  vor dem Zerlegen, er zählt sie (`rejected`, `lastRejected`), sonst nichts
  — kein Eintrag in der Message-Queue. Ohne
  Port-Wiederverwendung (JUCE setzt `SO_REUSEADDR` für UDP): ein belegter
  Port ist ein Fehler, zwei Instanzen teilen sich keinen Port. Einstellung in
  `osc.json` im `--config`-Ordner (enabled, address, port, allow), der Start
  lauscht wieder; eine Änderung öffnet den Port neu.
- **Thread**: der Empfangs-Thread liest, filtert und zerlegt nur; die
  Nachrichten kommen per `callAsync` auf den Message-Thread (wie die
  HTTP-Befehle). Läuft dort gerade eine Op mit verschachtelter Message-Loop
  (Preset, Plugin-Laden), wartet die Nachricht (`ctx.load` busy, 10 ms
  Timer) — nie mitten in eine Op. Nichts im Audio-Thread.
- **Adressen** (fest, keine generische Op-Durchreiche): `/unidaw/play`,
  `/unidaw/stop` (ohne Argument oder Zahl; 0 = Taster losgelassen, ignoriert),
  `/unidaw/play/from <s>`, `/unidaw/loop <0|1>`, `/unidaw/section/<name>` und
  `/unidaw/marker/<name>` (spielen ab dort; Namen ohne Groß-/Kleinschreibung),
  `/unidaw/track/<name|index>/volume <dB>` | `fader <0..1>` | `pan <-1..1>` | `mute <0|1>` |
  `solo <0|1>` (`trackMix`), `/unidaw/plugin/<modul>/<parameter> <0..1>`
  (`setParameter` normiert). OSC verbietet Leerzeichen in Adressen: `_`
  steht für ein Leerzeichen (erst der Name wie geschrieben, dann mit
  Leerzeichen). Bundles werden der Reihe nach ausgeführt, Zeitstempel
  ignoriert. `oscSettings` liefert die Liste (`addresses`).
- **fader 0..1**: dieselbe Kurve wie der Mixer-Fader der UI — linear in dB
  von -60 bis +6, 0 = -100 dB (aus). In C++ nur `faderDb` in `Osc.cpp`, in
  der UI nur `MX_FLOOR`/`MX_TOP`/`mxDbOf` in `flow.html`; `osc_flow` liest
  die Werte aus `flow.html` und prüft OSC dagegen (0, 0.25, 0.5, 1).
- **Ein Weg**: jede Adresse wird ein Befehl und läuft über `executeOp` —
  Guards, Version, Auto-Branch, Node-Aktion, Undo, Autosave und
  `events.jsonl` wie bei der UI; der Befehl trägt `osc: <Adresse>`. Fehler
  (unbekannte Adresse, falsches Argument, kein Edit, Fehler der Op) zählen in
  `failed` und stehen in `last`; die vor der Op auch in unidaw.err.
- **Fader** (volume, fader, pan, Plugin-Parameter): je Adresse gebündelt, alle
  60 ms der letzte Wert (wie `mxSlider` der UI); Knöpfe sofort und mit
  allem, was davor ankam. Eine Geste (Werte mit Pausen unter 1 s) ist ein
  Undo-Schritt (Abschnitt Undo/Redo).
- **Sicherheit**: OSC hat keine Anmeldung (nur die Absender-Liste), darum nur Transport, Mix und
  Parameter — keine Op, die Dateien liest oder schreibt, Projekte öffnet,
  rendert, Plugins lädt oder den Daemon beendet. Mehr nur über die
  HTTP-API (127.0.0.1).
- Kein Rückkanal (Matthias, 2026-10-26: nicht bauen): Status über
  `oscSettings` (received, ran, failed, rejected, last, lastRejected).
- Auto-Branch wie bei der UI (Matthias, 2026-10-26: so lassen, erst
  beobachten).

## Lizenz und Quellcode (About)

AGPLv3 § 13: unidaw bedient die Flow-UI über HTTP, Nutzer über das Netz
bekommen den Quellcode angeboten. Op `about` (`ops/Session.cpp`, read-only)
liefert `name`, `appVersion` (aus `project()` in `dawd/CMakeLists.txt`;
`version` bleibt wie in jeder Antwort der Stand des Edits), `license`
(`AGPL-3.0`), `licenseName`, `licenseUrl`, `sourceUrl` und `thirdParty`.

- `sourceUrl` kommt aus dem Build: Cache-Variable `UNIDAW_SOURCE_URL`,
  Standard das öffentliche Repo (gleich `homepage` in `mcp/manifest.json`,
  der Test prüft das). Ein Fork setzt `cmake -DDAWD_SOURCE_URL=<sein Repo>`;
  leer sagt die UI, dass der Build keinen Link nennt.
- `thirdParty` ist `THIRD_PARTY_LICENSES.md` der Repo-Wurzel, beim
  Configure als Bytes in den generierten Header `DawdAbout.h` geschrieben —
  kein Pfad in den Quellordner zur Laufzeit. Ändert sich die Datei, läuft
  das Configure neu.
- UI: Knopf ⓘ rechts in der Kopfleiste, Dialog `aboutDlg`.
- Test: `about_flow` (Felder, Version gleich `project()`, Text gleich der
  Datei, read-only, UI-Marker).

## Klang nachbauen

`matchSound` und `tuneSound` (`ops/SoundMatch.cpp`, Todo
`todos/2026-10-18-analyse-musikalitaet-und-sounddesign.md`, P2). Die
Referenz ist nur Messziel, ihr Audio wird nie verwendet (Entscheidung
Matthias 2026-10-18, im Todo): nachgebaut wird mit eigenen Klangerzeugern.

- **Referenz** einmal messen (`measureSoundReference` in `Voice.cpp`, wie
  `describeTimbre` ohne Zeitleiste): Noten (basic-pitch, verschmolzen),
  Klang, CLAP-Vektor. Erkannte Noten ohne Grundton (Grundton mehr als 35 dB
  unter dem stärksten Teilton — Differenztöne in Akkorden) fallen weg;
  Noten, die bis 150 ms in den nächsten Einsatz ragen, enden davor (nur
  ohne Glide in der Referenz). `notes` ersetzt die erkannten Noten (Liste in
  Sekunden ab Referenz-Anfang oder `{track, section | from}`).
- **Wegwerf-Edit** (`Bench`): `Edit::createSingleTrackEdit`, eine Spur mit
  einem MIDI-Clip (die Noten, 120 BPM ab 0) und dem Plugin als Kopie (Zustand
  des Moduls ohne Automation, bzw. neu aufgelöst für Kataloge,
  `newPluginState`). Das Projekt bleibt unberührt; VST3/LV2 laufen über die
  Sandbox wie jede Instanz (eine Instanz je Job, Host-Gruppe „je Plugin“).
  Render wie Clip-FX über `renderWithParams` (32 bit float, 44,1 kHz).
  Velocity aus dem Pegel beim Einsatz, je Einsatz durch die Zahl
  gleichzeitig anfangender Noten geteilt (Akkorde sind lauter, nicht
  stärker angeschlagen).
- **Kandidat messen** (`measureSoundCandidate`): Klang an den Noten der
  Referenz (`describeTimbre` mit deren `Voice`), Glide aus der eigenen
  Notenerkennung, CLAP. Abstand aus sechs Teilen, je 0 (gleich) … 1
  (deutlich anders) … 3: `clap` (1 − Ähnlichkeit) / 0,3; `envelope` mittlerer
  Pegelunterschied je 10 ms (Pegel relativ zum 95. Perzentil, laute Stellen
  zählen mehr, bester Versatz bis ±80 ms) / 8 dB; `spectrum` mittlerer
  Unterschied der Terzbänder des Langzeit-Spektrums / 6 dB (starke Terzen
  zählen mehr wie in `envelope` — sonst überstimmten schwache Terzen aus
  dem Übersprechen im Stem den Hauptbereich, Praxistest remix-1); `motion`
  Bewegung der Helligkeit ohne festen Versatz / 0,3 Okt.; `stereo`
  Korrelation / 0,5 und Seite−Mitte / 12 dB; `glide` Anteil gleitender
  Legato-Übergänge / 0,5. Gewichte 0,2/0,25/0,25/0,1/0,1/0,1; Schwellen und Gewichte gewählt,
  nicht kalibriert (Messwerte im Todo). `worst` nennt die drei Teile mit
  dem größten Beitrag samt Werten beider Seiten.
- **Pegelschutz** (Todo `2026-10-29-klang-werkzeuge-pegelschutz`, Praxistest
  remix-1: Hochpass 73 → 1046 Hz am Bass, −51 statt −19 LUFS, galt als
  näher und wurde gesetzt): beide Profile tragen die Leistung je Terz,
  gemittelt über die lauten 10-ms-Fenster (absolut, unabhängig von Pausen).
  Hauptbereich = Terzen der Referenz bis 10 dB unter ihrer stärksten;
  `levelVsReference` = Pegel darin gegen die Referenz. Verlust unter eine
  Basis mehr als eine Toleranz zählt als Teil `level` ((Verlust −
  Toleranz) / 6 dB, höchstens 3, Gewicht 0,25 obendrauf, nicht im Mittel).
  matchSound: Basis Referenz, Toleranz 18 dB. tuneSound: Basis das Leisere
  aus Ausgangsklang und Referenz, Toleranz 3 dB; ein Ergebnis mehr als 3 dB
  leiser als der Ausgangsklang kommt mit Hinweis, mehr als 6 dB wird nicht
  gesetzt (Befehl steht im Ergebnis, UI „Apply anyway“). Schwellen gewählt.
- **Anderer Rhythmus** (`notes` eines Remix): `reference.rhythmMatch` =
  Anteil der Einsätze mit Partner bis 70 ms (beide Richtungen, das
  Kleinere); darunter vergleicht `compare: "auto"` (Standard, Entscheidung
  Matthias 2026-10-29) nur den Klang, mit Hinweis; `"all"` erzwingt die
  Zeitachse, `"sound"` den Klang; das Ergebnis nennt `compare`. Nur Klang:
  `envelope` aus
  Attack/Release (Verhältnis, Faktor 4 = 1) und Sustain (/ 8 dB) je Note,
  `motion` aus dem Verdunkeln im Ton (`noteSweepOct`); die Referenz an
  ihren erkannten Noten (`refOwn`), der Kandidat an denen, die er spielt.
  Hält die Referenz ihre Noten legato, fehlt ihr Attack — `envelope` fällt
  dann weg. Gemischte Referenz (`layers` ≥ 2, Puls- oder Akkord-Schicht)
  und ein nächster Treffer, der anders als der eigene Klang wie ein Bass
  klingt (CLAP-Klasse), geben je einen Hinweis.
- **matchSound**: Kandidaten = aktueller Klang des Moduls, dazu die per CLAP
  ähnlichsten Katalog-Einträge (`findPresets {like}`), aufgefüllt in der
  Reihenfolge von `listPresets`; ohne Modul die Kataloge (nach Plugin
  gruppiert gerendert). Programm-Presets im Wegwerf-Edit nach Namen, wenn
  die Nummer nicht passt.
- **tuneSound**: 3–6 Parameter — angegeben oder nach Steckbrief: je
  Parameter, welche Teile seine gemessene Wirkung bewegt (attack/tail →
  envelope, brightness → spectrum + motion, low/high → spectrum, width →
  stereo; Stärke je Messgröße aus ihrer ratio: ab 5 → 1, ab 2 → 0,7,
  darunter 0,5; needs gelten, wenn der
  Schalter überhaupt an ist), gierig gewählt (was ein gewählter Parameter
  bewegt, zählt danach halb). Ohne Steckbrief-Wirkung nach dem Namen.
  Ausgenommen Pegel/Stimmung (`notTimbre`) und Schalter/Rhythmus
  (`isSwitch`). Koordinatensuche im normierten Raum: je Parameter ±Schritt
  (0,25, halbiert nach einer Runde ohne Gewinn, Schluss unter 1/64), ein
  gestufter Regler mit gleichem Text kostet keinen Render; Budget an
  Renders und Sekunden. Am Ende **ein** `setParameter` über `executeOp`
  (ein Undo-Schritt, Auto-Branch wie bei Hand), nur wenn der Abstand um
  mehr als 0,01 sinkt und Edit und Modul noch dieselben sind.
- **Jobs in Schritten auf dem Message-Thread** (`Jobs.h`: `JobWork`,
  `onMessageThread`): die Engine ist nicht thread-safe, darum ist jeder
  Render samt Messung eine eigene Nachricht (`callAsync`); Befehle der UI
  und Agenten laufen dazwischen. Der Zustand (mit dem Wegwerf-Edit) gehört
  dem Job und stirbt mit der `JobList` vor der Engine; die Nachrichten
  halten nur einen `weak_ptr`. `jobStatus {wait}` wartet auf dem Thread der
  Verbindung; in `batch` antwortet es für diese Jobs sofort.

## localhost-API absichern

Die API lauscht nur auf 127.0.0.1, aber jede Website im Browser des Nutzers
kann Anfragen dorthin schicken (einfacher POST, DNS-Rebinding). Darum prüft
`HttpServer::refuse` (`source/HttpServer.h`) jede Anfrage vor der Route
(`docs/vertrieb-und-verkauf.md`, Paket 2; Todo
`2026-11-18-localhost-api-absichern`):

- **Host** nur `127.0.0.1:<port>` oder `localhost:<port>` (Port der
  Instanz); fehlt er oder ist er anders: 403 `forbidden host`. Das hält
  DNS-Rebinding ab — auch für `GET /ui` (die Seite trägt den Token) und
  `/audio`.
- **Origin** fehlt (Skripte, MCP, curl) oder ist `http://127.0.0.1:<port>`
  bzw. `http://localhost:<port>` (die eigene UI); jede andere, auch `null`:
  403 `forbidden origin`.
- **Token** bei jedem POST (`/command`, `/upload`): Header
  `Authorization: Bearer <token>`, sonst 401 `missing or wrong token`. GETs
  (`/ui`, `/health`, `/progress`, `/audio`, `/vendor/`) brauchen keinen —
  `<audio src>` kann keinen Header schicken.
- **Token pro Sitzung**: aus `UNIDAW_TOKEN` (Starter, Test-Suite), sonst 32
  Zufallsbytes aus `/dev/urandom` (64 Hex-Zeichen). unidaw schreibt ihn nach
  dem Listen nach `<config>/api-token-<port>` (0600; ein zweiter Start auf
  belegtem Port überschreibt die Datei der laufenden Instanz nicht) und
  löscht die Datei beim Beenden.
- **UI**: `GET /ui` setzt den Token als `<meta name="dawd-token">` in die
  Seite, `flow.html` schickt ihn bei jedem Befehl und Upload (`AUTH`). Eine
  fremde Seite kann die Antwort nicht lesen (Same-Origin, Host-Prüfung).
- **Clients**: `mcp/unidaw_mcp.py` liest `UNIDAW_TOKEN`, sonst
  `~/.config/unidaw/api-token-7400`, je Befehl neu (nach einem Neustart gilt
  der neue Token); 401/403 kommen mit dem Grund als Ergebnis zurück. Suite
  und Werkzeuge, die eigene Instanzen starten, geben `UNIDAW_TOKEN` vor
  (`tests/run_tests.py`, `tools/profile_plugin.py`,
  `tools/remeasure_all.sh`, `tools/ui_replay/`, `bench/sandbox/`). Von
  Hand:

  ```
  curl -s -X POST -H "Authorization: Bearer $(cat ~/.config/unidaw/api-token-7400)" \
    -d '{"op":"ping"}' http://127.0.0.1:7400/command
  ```

Test: `api_guard_flow`.

## Konventionen (API-Vertrag)

- Guard-Meldungen sind Teil der API: `no edit`, `no tree`,
  `no tree / no edit`, `no tree open`, `recording in progress`.
- Jede Antwort trägt `version`; mutierende Ops zählen vor dem Handler hoch,
  auch wenn ein Guard abbricht. Read-only-Ops zählen nicht.
- Der Handler löscht nichts still: Fehlschläge vor Seiteneffekten prüfen.
- Lese-Ops bauen keinen Audio-Graphen: `state`/`listInputs` lesen die
  Eingangs-Zuweisungen ohne Graph aus dem Edit (`INPUTDEVICES`) — der Graph
  entsteht beim ersten Play (Ladestand "audio graph"), Aufnahme, Monitor und
  Arm. Vorher baute jedes `state` nach dem Laden und nach dem
  Leerlauf-Freigeben den ganzen Graphen (Agent-Projekt: 1,1–1,4 s).
- `state` und `treeState` sind die Lese-Sichten; das UI pollt nur
  `position` (~8 Hz beim Abspielen, sonst ~2 Hz; je Poll eine Anfrage
  unterwegs, Pause während eines langen Befehls), `levels` (~10 Hz beim Abspielen — Pegel je Spur und
  Main über LevelMeasurer-Clients; `executeOp` meldet sie vor jeder Mutation
  ab, weil der Measurer rohe Zeiger hält; dazu `lufs` am Main-Ausgang über
  `LiveLoudness` — globaler Ausgangs-Prozessor des DeviceManagers, sieht
  den fertigen Geräteausgang nur im Audio-Callback, nie im Render, misst das
  Paar des Standard-Ausgangs mit dem `LoudnessMeter` der Engine (ohne Lock
  und Allokation), rechnet nur, solange gepollt wird; der Master steht auf
  0 dB (`unityMasterVolume` in `ensureRouting`, Decision
  `docs/decisions/2026-10-26-master-0db.md`), live passt also zu `analyze`) und `inputLevel` (~7 Hz, Audio-Node-Anzeige: aktiver
  System-Eingang über `wpctl`, sonst leer — und Wave-Input-Pegel).

## Neues Feature bauen

**Neuer Op:**
1. Handler in `source/ops/<Familie>.cpp` schreiben.
2. Signatur in `source/ops/Ops.h` ergänzen.
3. Eintrag in der Tabelle in `source/OpRegistry.cpp` (Familie, mutating,
   Guards). Ruft der Op `recordNodeAction` auf, gehört er mit seiner Sorte
   in `actionKindOf` (sonst greift der Auto-Branch nicht).
4. Test in `tests/run_tests.py`; `bash tests/run.sh` vor dem Commit.
5. `listOps` zeigt den Op automatisch; die MCP-Beschreibung in
   `mcp/unidaw_mcp.py` um eine Zeile ergänzen.

**Neuer Aktionstyp (kind):**
Die Kinds sind fest: collect | select | process. Neues Verhalten innerhalb
eines Kinds:
1. `recordNodeAction(ctx, kind, op, params, file)` am passenden Op aufrufen
   und den Op mit dieser Sorte in `actionKindOf` eintragen.
2. Bedienelement in `ui/flow.html` (immer sichtbar, kein Modus); Sichtbarkeiten nur in `applyNodeUi()`.
3. Test: Aktion am Node (`treeState`) prüfen.

## Werkzeuge

- `mcp/unidaw_mcp.py` — MCP-Server (stdio): `unidaw_start`, `unidaw_command`.
- `tests/run.sh` — baut und fährt die Custom-Feature-Suite über die
  Befehls-API (eigene Instanz auf Port 7411 oder dem nächsten freien
  Basis-Port 7441, 7471 …, temporäre Projekte).
  MIDI-Aufnahme läuft headless über den ALSA-Loopback (Client 14:0).
- Build: `cmake --build dawd/build -j$(nproc)`; Start:
  `unidaw --port 7400 --log events.jsonl` (Arbeitsordner `dawd/run`);
  `--interface-sim` ersetzt das echte Audio-Interface durch die Simulation;
  `--window-sim` die Plugin-Fenster (kein Fenster auf dem Bildschirm);
  `--no-audio` öffnet gar kein Audio-/MIDI-Gerät (nur Offline-Render und
  Analyse). Die Engine zählt beim Start trotzdem MIDI (`/dev/snd/seq`) und
  Karten auf; `tools/profile_plugin.py` startet unidaw darum zusätzlich unter
  `bwrap --tmpfs /dev/snd` und belegt das Fireface nicht.
- `dawd/patches/` — eigene Korrekturen an Engine/JUCE (Submodul), bis sie
  upstream sind; `apply.sh` wendet sie idempotent an (CMake-Configure,
  `tests/run.sh`) und bricht ab, wenn einer nach einem Submodul-Update nicht
  mehr passt. Liste und Upstream-Stand stehen in `apply.sh`.
- Entwickler-Instanz auf 7400 als systemd-User-Dienst (unabhängig von
  Terminal/Agent-Session; transient, überlebt keinen Reboot):
  `systemd-run --user --collect --unit=unidaw-7400
  --working-directory=$PWD/dawd/run -E DISPLAY=:0 -E WAYLAND_DISPLAY=wayland-0
  -p StandardError=append:$PWD/dawd/run/unidaw.err
  -p Restart=on-failure -p RestartSec=1
  $PWD/dawd/build/unidaw_artefacts/Release/unidaw --port 7400 --log events.jsonl
  --session session.json`.
- Neustart nach einem Build (neue Ops brauchen die neue Binary, UI-Änderungen
  nur einen Browser-Reload): `state` prüfen (keine Aufnahme), dann
  `systemctl --user restart unidaw-7400` — SIGTERM schreibt den Autosave, der
  Start öffnet das Projekt über die Session-Datei wieder.

## Referenzen

- `decision.2026-10-09-agent-first-daw-eigenbau-frontend-auf-tracktion-engine-mit-befehls-api-und-event-log`
- `decision.2026-10-11-song-baum-fuer-dawd-seed-pfad-favoriten-teilbaum-terminierung`
- `decision.2026-10-13-dawd-op-registry-und-node-aktionen-als-fundament-fur-die-vier-aktions-typen`
- `decision.2026-10-11-dawd-ist-eigenstaendige-daw-flow-tree-als-strukturschicht-linearer-track-view-geplant`
- `docs/agent-ux-vorbilder.md` — MCP-Interfaces anderer DAWs als Vorbilder
  (reaper-mcp, ableton-mcp, LivePilot) und die Lessons daraus
- `docs/engine-bewertung.md` — Bewertung der Tracktion Engine (Entscheidung
  behalten und ausbauen, Messwerte aus `bench/engine/`, Engine-Eigenheiten,
  die unidaw umschifft, Lizenz)
- `docs/decisions/` — alle Decisions seit 2026-10-24 (Liste und ältere
  Decisions in `README.md`)

(die `decision.*` bis 2026-10-23 lagen außerhalb des Repos, Liste in
`docs/decisions/README.md`; die `docs/`-Dateien im Repo)
