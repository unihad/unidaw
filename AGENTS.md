# agent-daw

unidaw — eigenständige DAW (Tracktion Engine, headless Daemon mit HTTP-API),
Flow-UI im Browser. Aufbau: `ARCHITECTURE.md`, Entscheidungen:
`docs/decisions/`.

Produktname ist unidaw (Binary, Starter, MCP, `UNIDAW_*`, OSC `/unidaw/`);
Quellordner `dawd/`, C++-Namespace und Dateiformate heißen weiter dawd
(Decision `docs/decisions/2026-11-19-produktname-unidaw.md`).

## Decisions

**Alle Decisions zu agent-daw stehen im Repo**, unter `docs/decisions/`
(eine Datei je Decision, Format und Liste der älteren Decisions in
`docs/decisions/README.md`; Matthias, 2026-10-24). Entscheidet Matthias
etwas, das Architektur, Verhalten für den Nutzer oder das Produkt
festlegt, schreibt der Agent die Decision-Datei im selben Zug wie Code
und Doku und verweist aus Todo und `ARCHITECTURE.md` darauf. Kleine
API-Entscheidungen zu einzelnen Befunden bleiben im Agent Log des Todos.
Nichts davon liegt außerhalb des Repos.

## Struktur

- `LICENSE` — AGPLv3 (AGPL-3.0-only, weil JUCE AGPLv3 ohne „or later“
  ist). `THIRD_PARTY_LICENSES.md` — Bausteine und Modelle mit Lizenz
  (Englisch, für Nutzer); Prüfung und Pflichten beim Ausliefern:
  `docs/lizenzen-agpl.md`. Neue Abhängigkeit = Eintrag dort.
- `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `.github/` (Spenden-Link,
  Issue-Vorlage) — öffentliche Projektdateien für GitHub, auf Englisch:
  nur Linux, kein Support, Issues ja, PRs von außen nein. Ändert sich
  Install, Start oder der MCP-Weg, README mitziehen.
- `APP.md` — interner Steckbrief der App (Name, Zielgruppe,
  Launchstatus; Format: Decision `2026-11-09-steckbrief-format-fuer-apps`
  im internen Repo unihad-cockpit). Wer den Launchstatus ändert (Einreichen,
  Freischalten, auf Eis legen), ändert `launch` und `updated` mit.
- Öffentlicher Stand: `tools/export_public.sh` legt den Baum von main ohne
  die internen Pfade (`todos/`, `APP.md`, Vertriebsplanung, Diva-Katalog;
  Liste `EXCLUDE` im Skript) als Commit auf den lokalen Branch `public` —
  ohne Historie, je Release ein Commit, kein Push (Decision
  `docs/decisions/2026-11-18-oeffentlicher-stand-ohne-historie.md`). Neue
  interne Datei = Eintrag in `EXCLUDE`. Preset-Kataloge in
  `dawd/plugin-profiles/` tragen Pfade relativ zum Preset-Ordner des
  Plugins (`profile_plugin.catalog_path`), nie `/home/<user>/…`.
- `dawd/` — Daemon (C++, CMake): HTTP-Einstieg (`source/main.cpp`),
  Op-Registry (`OpRegistry.cpp` — Name, Familie, Mutating, Guards, Handler),
  Op-Handler je Familie (`source/ops/`, darunter `Processing.cpp` für
  Plugin-Module, Routing und kommende KI-Verarbeitung, `Freeze.cpp` für
  Freeze/Flatten, `ClipFx.cpp` für Clip-FX (`processClip`: Audio-Clip
  offline durch Module, Rückweg über `DAWDFX`), `Groups.cpp` für Gruppen/Submix — Bus `g<N>` in der
  Matrix), Kontext/Logger
  (`Dawd.h`), Engine-Helfer (`EngineHelpers.*`), Routing-Matrix
  (`Routing.*` — Module, Kanten, Typ-Regeln, Ketten, Links, Migration;
  `Feedback.*` — Feedback-Plugin-Paar mit Ringpuffer; `Sidechain.*` —
  Schlüssel in den zweiten Eingang eines Moduls), Aufnahme (`RecordSession.cpp`), Song-Baum
  (`TreeState.h`, v3: Node-Aktionen kind/op/params/file — kinds
  collect|select|process, acted abgeleitet), Flow-UI (`ui/flow.html` — zur
  Laufzeit gelesen, Änderungen brauchen nur einen Browser-Reload),
  Upload (`/upload`), Schutz der localhost-API (`HttpServer.h` — Host,
  Origin, Token pro Sitzung in `<config>/api-token-<port>`, jeder POST mit
  `Authorization: Bearer`; `ARCHITECTURE.md`, localhost-API absichern), Ordnerlisten (`listDir`),
  Lizenz und Quellcode-Link (Op `about` in `ops/Session.cpp`, Drittlizenzen
  beim Configure in den generierten Header `DawdAbout.h`, Link per
  `-DDAWD_SOURCE_URL`; `ARCHITECTURE.md`, Lizenz und Quellcode), mitgelieferte Ressourcen
  relativ zur Binary (`Resources.*`; Starter `launcher/unidaw-start`), Musikalitaet und Klang
  einer Spur (`VoiceAnalysis.*` — Contour aus basic-pitch, Huellkurve,
  Obertoene, Stereo; Ops `describeVoice`/`describeTimbre`/`compareVoice` in
  `ops/Voice.cpp`, Quelle und Raster der Analyse-Ops in
  `ops/AnalysisSource.*`; Klang nachbauen mit `matchSound`/`tuneSound` in
  `ops/SoundMatch.*` — Jobs in Schritten auf dem Message-Thread,
  `ARCHITECTURE.md`, Klang nachbauen; Schlagzeug in `DrumAnalysis.*` mit
  `describeDrums`/`compareDrums` in `ops/Drums.cpp`), Audio-Interface
  (`Interface.*` — Fireface-UC-Regler über alsa-lib, Simulation mit
  `--interface-sim`; Ops in `ops/Interface.cpp`), Live-LUFS am Main-Ausgang
  (`LiveLoudness.h` — globaler Ausgangs-Prozessor, `levels.lufs`), Plugin-Fenster
  (`PluginWindows.*` — Editor eines Moduls im nativen Fenster, Op
  `pluginWindow`, Simulation mit `--window-sim`; `XErrors.cpp` — X-Fehler
  loggen statt beenden), OSC-Fernsteuerung (`Osc.*` — UDP-Empfang, feste
  Adressen unter `/unidaw/` auf play/loop/trackMix/setParameter über
  `executeOp`, standardmäßig aus, nur 127.0.0.1, Absender-Liste `allow`; Op `oscSettings` in
  `ops/Osc.cpp`; `ARCHITECTURE.md`, Abschnitt OSC-Fernsteuerung), Plugin-Sandbox
  (`PluginSandbox.*` — Stellvertreter in unidaw, `PluginHost.cpp` — Host-
  Prozess `unidaw --plugin-host` (eine oder mehrere Instanzen, Gruppierung
  je Plugin) und Scan-Kind `unidaw --plugin-scan`, `PluginSandboxShared.h` —
  Speicher-Layout; Phase 1–4: VST3 und LV2 im Host, Neustart nach
  Absturz/Hänger, Sperrliste, Ops `reloadPlugin`/`pluginHosting`; Standard
  an, `UNIDAW_PLUGIN_SANDBOX=0` schaltet ab, `ARCHITECTURE.md`).
- `mcp/` — MCP-Server (`unidaw_mcp.py`), kapselt die Befehls-API als Tools.
  Zugleich Plugin-Ordner für Claude Code (`mcp/.claude-plugin/plugin.json`,
  Marketplace `.claude-plugin/marketplace.json` in der Wurzel) und Quelle
  des MCP-Bundles (`mcp/manifest.json`, `tools/build_mcpb.py` baut
  `.mcpb` und `server.json` für die MCP-Registry). Version überall gleich
  `dawd/CMakeLists.txt`; Aufbau und Veröffentlichen:
  `docs/mcp-veroeffentlichen.md`.
- `plugins/` — TestGain-VST3 als Verifikation.
- `tests/` — Custom-Feature-Tests.
- AppImage: `tools/build_appimage.sh` baut es, `tests/check_appimage.sh`
  prüft es ohne Quellordner (bubblewrap), Einstieg und Desktop-Datei in
  `dawd/appimage/`, Modelle per `dawd/launcher/unidaw-fetch-models`;
  Aufbau und Grenzen (glibc des Build-Rechners): `docs/appimage.md`.
- `tools/short/` — Aufnahme-Kette der Shorts (`record_short.js`, Drehbuch
  je Short als `<short>.json`); Ablauf und Umgehungen: `docs/shorts.md`,
  Abschnitt Aufnahme-Kette. Test: `short_chain_flow`.
- `recipes/` — geprüfte Rezepte (Klang, Kette, Arbeitsablauf, Mix-Ziel);
  Index `recipes/README.md`, erzeugt von `tools/recipes.py index`.

## Architektur (verbindlich)

Schichten, Datenablage, Befehlsablauf und Erweiterungswege: `ARCHITECTURE.md`.
Die Regeln, die jede Änderung einhalten muss:

- **Alles läuft über die Op-Registry** (`dawd/source/OpRegistry.cpp`). Neuer
  Befehl = Handler in `dawd/source/ops/<Familie>.cpp`, Signatur in `ops/Ops.h`,
  Tabelleneintrag (Familie, mutating, Guards). Die Guard-Meldungen sind
  API-Vertrag (`no edit`, `no tree`, `no tree / no edit`, `no tree open`,
  `recording in progress`) — nicht umformulieren; Handler prüfen nur, was die
  Tabelle nicht abdecken kann.
- **Read-only zählt nicht**: `mutating: false` für Lese-Ops, `position` wird
  nie geloggt, jede Antwort trägt `version`.
- **Node-Aktionen sind Datensätze**: Am Knoten nur über
  `recordNodeAction (ctx, kind, op, params, file, coalesce)` schreiben
  (`dawd/source/Dawd.h`; `coalesce` ersetzt die letzte gleiche Aktion,
  `removeNodeAction` ist der Gegenweg für Undo); `acted`, die Take-Daten (`takeType`,
  `takeSection`, `takeTrack` aus der letzten Collect-Aktion), `actedBy`,
  `actionFile` und `hasFavorites` sind Ableitungen aus `actions[]` und werden
  nie separat gespeichert. `kind` ist collect | select | process —
  collect = Material reinholen (Sections, Record, Inserts), select =
  Favoriten auf Clips/Sections, process = Bearbeitung (Clip-Edit, Plugins,
  Routing).
- **Der Baum baut sich aus den Aktionen**: eine Op, die eine Node-Aktion
  bucht, gehört mit ihrer Sorte in `actionKindOf` (`OpRegistry.cpp`) —
  `executeOp` legt bei Wechsel der Sorte oder des Fokus (andere Spur, Song)
  vorher einen Kind-Node an; Feinschliff (`setParameter`, `trackMix` …) ist
  eine zusammengefasste process-Aktion (Details:
  `ARCHITECTURE.md`, Node-Aktionen). Tests, die bewusst Sorten in einem Node
  mischen, schalten das nach dem Seed ab (`treeSettings autoBranch=False`).
- **Undo**: eine neue Op, die den Edit oder die Node-Aktionen ändert, gehört
  in `isUndoableOp` (`source/Undo.cpp`); eine, nach der die Historie nicht
  mehr passt (baut den Baum um, wechselt den Edit), in `clearsUndo`.
- **Plugins hängen nicht an Spuren**: jedes Plugin ist ein Modul im
  Projekt, Spuren laufen über die Routing-Matrix (`connect`/`disconnect`)
  bis Main; MIDI nur durch MIDI-Effekte/Instrumente, Audio nur durch
  Audio-Effekte und Main. Spur-Indizes zählen nur Nutzer-Spuren
  (`userTracks`, nie `te::getAudioTracks`), Lane-Gruppe über `laneGroupOf`,
  Spurtyp über `isMidiTrack` (Details: `ARCHITECTURE.md`, Routing-Matrix).
  Indizes sind nicht stabil: Lanes (Takes, Inserts) sind eigene Spuren
  hinten in `userTracks` und belegen die nächsten Indizes; ein Index gilt
  nur bis zur nächsten Änderung der Spurliste. Die stabile Adresse ist der
  Spurname (`trackIndexFor`, `OpRegistry.cpp`) — Doku und Hinweise an
  Agenten empfehlen ihn (Decision
  `docs/decisions/2026-10-24-spurnamen-als-adresse.md`).
- **Flow-UI**: Sichtbarkeiten nur in `applyNodeUi()` setzen (eine Stelle);
  die Seite besteht aus getrennten CSS/HTML/JS-Konstanten, ohne Framework und
  ohne Build-Schritt. Section-Regeln (Sections sind Zeitbereiche ohne
  Gaps, das Material ist frei: Clips, Aufnahmen und Automation laufen über
  Section-Grenzen, kein Kürzen am Section-Ende; Material hinter der letzten
  Section legt eine Auto-Section an; Section-Ops schneiden Zeit über die
  Helfer in `EngineHelpers` — `deleteSection` entfernt den Zeitbereich,
  Clips an den Grenzen abgeschnitten, spätere rücken nach, die letzte
  bleibt, Undo holt sie zurück; Zeit einfügen teilt einen Clip an der
  Stelle; Details: `ARCHITECTURE.md`, Sections als Zeitbereiche; nicht
  favorisiertes Material projektweit wirft `treeCompact` raus, mit
  Trockenlauf `treeCompactPreview` davor) erzwingt nur das Backend in den Op-Handlern — das
  Frontend ruft die Ops auf (Vorschau ist nur optisch, bei `!ok` springt der
  Zustand zurück) und dupliziert die Regeln nicht; die MCP-Doku beschreibt
  sie, die Suite prüft sie an der API.
- **Alle Features auch über die UI**: jeder neue Befehl braucht eine
  Repräsentation im Flow-UI (Control in der Transport-Leiste, Reiter im
  Dock unten, Referenz/Analyse-Panel in der rechten Spalte oder
  Element/Drag in der Timeline; Layout: Decision
  `docs/decisions/2026-10-29-ui-seitenleiste-referenz.md`) — API-only gilt als
  unfertig. Bevorzugt Direktmanipulation (Drag & Drop, Klick ins Lineal)
  statt Formularfelder.
- **Jedes neue Feature kommt mit Tests** in `tests/run_tests.py`;
  `bash tests/run.sh` vor dem Commit.

## Vorbilder (Agent-UX)

Vergleich mit den MCP-Interfaces anderer DAWs und die Lessons daraus
(Kontext-Budget, Analyse-Ops, Batch-Op): `docs/agent-ux-vorbilder.md`.
Lesereferenz — die Vorbilder (reaper-mcp, ableton-mcp, LivePilot) werden
nicht installiert, ihre Host-DAWs laufen hier nicht.

## Engine

Eigene Engine-/JUCE-Korrekturen liegen als Patch in `dawd/patches/` (kein
Fork, Upstream-PR je Patch); `apply.sh` wendet sie beim Bauen an. Die
angewendete Fassung je Patch vermerkt es in `<Repo>/.dawd-patches/`
(unversioniert); ändert sich eine Patch-Datei, nimmt es die alte Fassung
zurück und wendet die neue an — geht das nicht, bricht es mit Anleitung ab.
Auch in Worktrees mit Engine-Kopie ohne `.git` prüft es richtig (Kopie
samt `.dawd-patches` übernehmen). Test: `tests/check_patches.sh`.

**Frischer Worktree** (`git worktree add …`): einmal
`bash tools/worktree_setup.sh` darin laufen lassen, dann baut
`bash tests/run.sh` ohne Handarbeit. Das Skript kopiert die Engine aus dem
Haupt-Checkout ohne alle `.git`-Dateien (samt `.dawd-patches`) und
`dawd/third_party`, verlinkt `dawd/models` und fährt das CMake-Configure
(gut 1 min). Was schon da ist, bleibt.

Bewertung der Tracktion Engine, Messwerte und die Eigenheiten, die unidaw
umschifft (Tempo ohne Remap, Noten relativ zum Clip, Stems über
allowedClips, Pegel-Clients vor Mutationen abmelden …):
`docs/engine-bewertung.md`. Vor Änderungen an Tempo, MIDI-Einfügen, Render,
Feedback oder Metering lesen. Benchmark: `bench/engine/`.

## UI-Checkliste

Vor jedem UI-Commit: `docs/ui-checklist.md` durchgehen — alle Buttons
sichtbar (auch auf farbigem Grund), alle Aktionen ansteuerbar, jeder
Node-Zustand hat einen nächsten Schritt, Guards führen zurück statt
stillschweigend zu versagen. Die Suite testet die API, nicht die
Sichtbarkeit — die Checkliste ist der Sichtbarkeits-Anteil. Bedienung im
Browser (headless, eigene Instanz): `tools/ui_replay/fehler_check.sh <port>`
(Umbenennen, Kürzel in Feldern, gewählte Spur, Trim, verdeckte Clips);
Bedienaufwand ganzer Agent-Läufe: `tools/ui_replay/` (README).

**Die Checkliste lebt mit**: bringt eine Feature-Änderung neue UI-Muster
(neue Buttons, Drags, Zustände, Wege zurück), ergänzt der Agent die
sinnvollen neuen Checks in `docs/ui-checklist.md` — im selben Commit wie
das Feature.

## Sample-Bibliothek

Die Samples liegen auf der externen Platte unter
`/run/media/matthias/708E12148E11D406/Samples` (NI, Ableton, Arturia,
Landr; ~50 GB, nur von unidaw lesbare Formate). Inhalt, Herkunft, was
bewusst fehlt (Kontakt, Ableton-"able"-Packs), das Anmelden und die
Klang-Analyse für die Suche nach Text/Referenz: `docs/sample-bibliothek.md`.

## ML-Bausteine

`tools/fetch_ml.sh` lädt ONNX Runtime, die Modelle (basic-pitch,
HT-Demucs, CLAP für die Sample-Suche nach Klang; mit `MEGA53=1` dazu
das 53-Klassen-Modell, exportiert aus PyTorch) und Rubberband (nicht im Repo, in `.gitignore`); danach neu
bauen. Ohne sie bauen unidaw und die Suite weiter, die Ops melden den
fehlenden Baustein, ihre Tests werden übersprungen.

## Tests

**Vor jedem Commit: `bash tests/run.sh`** — prüft C++-Stringliterale
(`tests/check_literals.py`: Nicht-ASCII nur über `juce::String::fromUTF8`,
sonst kommt Zeichensalat in den Antworten an), prüft `apply.sh` an
Wegwerf-Repos (`tests/check_patches.sh`), baut und lässt die Suite laufen.

Der lokale pre-commit-Hook (`.git/hooks/pre-commit`) führt `tests/run.sh`
aus; ein roter Test blockt den Commit.

- Commits, die nur `todos/` und `docs/` ändern, überspringen die Suite
  ([Decision](docs/decisions/2026-10-25-doku-commits-ohne-suite.md)).
- Ändert der Commit keinen ML-Code (`dawd/source/Separation.*`,
  `dawd/source/ops/Stems.cpp`, `dawd/source/ops/Voice.cpp`,
  `tools/fetch_ml.sh`, `dawd/CMakeLists.txt`), setzt er `UNIDAW_SKIP_ML=1`:
  die Suite läuft ohne die ML-Trenn-Tests (`ML_TESTS` in
  `tests/run_tests.py`).
- Der pre-push-Hook (`.git/hooks/pre-push`) fährt vor jedem Push
  `UNIDAW_TESTS=health,treeSeed_fields,ml bash tests/run.sh`
  ([Decision](docs/decisions/2026-10-25-ml-tests-vor-dem-push.md));
  ändern alle gepushten Commits nur `todos/` und `docs/`, lässt er den
  Push ohne Suite durch (z. B. reine Todo-Commits).
- Die Suite gibt die Dauer je Test aus (`ok (2.6 s)`).
- `tests/run.sh` fährt die Suite in zwei Gruppen auf zwei unidaw-Instanzen
  parallel ([Decision](docs/decisions/2026-10-25-suite-parallel.md)):
  Gruppe A auf `UNIDAW_TEST_PORT` (Standard 7411; + 1 Session-Instanz,
  + 20 audio_api_flow), Gruppe B auf `UNIDAW_TEST_PORT` + 2. Gruppe B steht
  als Liste `GROUP_B` in `tests/run_tests.py`: nur Tests, die allein laufen
  (eigener Seed, kein Zustand aus früheren Tests) — vorwärts und rückwärts
  allein geprüft. Alles andere ist Gruppe A: die Kette ab `treeSeed_fields`,
  die ML-Trenn-Tests (nie in beiden Instanzen zugleich), die Tests am
  MIDI-Through-Loopback und jeder neue Test. Ein neuer Test kommt erst nach
  der Prüfung allein (`UNIDAW_TESTS=treeSeed_fields,<name>`) in `GROUP_B`.
  Ausgabe live mit Präfix `[A]`/`[B]`, am Ende beide Zusammenfassungen und
  alle Failures; bei Rot bleiben die Logs liegen (Pfad in der Ausgabe).
  Eine Instanz wie bisher: `UNIDAW_TESTS=…` (Teilmenge) oder `UNIDAW_SERIAL=1`
  (Notausgang); eine Gruppe allein: `UNIDAW_GROUP=A|B python3
  tests/run_tests.py`.

- Die Suite testet die Custom-Features über die Befehls-API — das, was das
  Flow-UI benutzt: Song-Baum (Seed/Branch/Checkout/Reset/Terminate/Favorite/
  Label, Base-Snapshots, Node-Aktionen mit abgeleitetem acted und Take-Daten),
  Favoriten auf Material (favorites_flow), Projekt komprimieren
  (compact_flow; missing_media_flow: fehlende Mediendatei — normal,
  gewarpt, Clip-FX, Freeze, nach treeReset/treeOpen/Branch/Undo — gibt in
  Sekunden einen Fehler mit Clip und Datei statt zu hängen, state markiert,
  Sampler rendert weiter, eigene Instanz; compact_span_flow: favorisierte Section schützt Clips, die
  sie berühren, bleibender Clip hält seine Sections, gefallene Sections
  rücken nach; compact_frozen_flow: gefrorene Spur bleibt samt Material),
  Sections als Zeitbereiche (section_span_flow:
  Insert/Move/Trim/Kopie/setMidiNotes über Section-Grenzen ohne Kürzen,
  Auto-Section in ganzen Takten mit geerbtem Tempo, als Node-Aktion
  addSection, Undo;
  section_time_flow: deleteSection schneidet an den Grenzen, Section
  kürzen/verlängern, davor einfügen teilt, Noten/Audio/Automation an ihrer
  Stelle, Undo; section_move_copy_flow: moveSection teilt und verschiebt
  Teile, duplicateSection kopiert Clip-Teile und Automation; Aufnahme über
  das Section-Ende mit Auto-Section beim Stopp: section_record_flow;
  marker_flow: Marker per Zeit und Takt/Schlag, state.markers, verschieben
  und umbenennen als eine Aktion, Einfügen/Löschen/Umordnen von Sections
  nimmt sie mit, Kopieren kopiert sie mit eindeutigem Suffix („Big Drop 2“,
  „Big Drop 3“), Fehlertexte, Undo/Redo, treeOpen),
  Inputs + Multi-Track-Record
  (inputs_flow), Arm je Spur + Audio und MIDI parallel (arm_track_flow),
  Clip-Undo (undo_delete_flow), MIDI-Noten ersetzen (set_midi_notes_flow),
  MIDI-Controller (midi_cc_flow: setControllers CC und Pitch-Bend, clipNotes,
  Bereich, Fehlertexte ohne Änderung, eine Aktion, Undo, Bend hörbar am
  4OSC, gelöschte Kurve wieder in der Mitte ohne Neuladen, splitClip,
  copies auf alle Kopien des Patterns) und MIDI-Export (midi_export_flow: Song/Spur/Clip/Section
  als .mid mit Tempo und Takt der Sections, CC davor am Anfang, Bend-Mitte
  am Clip-Anfang ohne Bend, gemutete
  Lane fehlt, Rundweg über addClip, Fehlertexte),
  Clip-Inhalt für die Anzeige (clipPeaks in insert_flow/midi_add_notes_flow),
  Routing-Matrix (routing_rules_flow, routing_audio_flow mit Render-Peaks,
  routing_tree_flow, routing_migration, routing_wet_level_flow,
  routing_v1_upgrade, routing_feedback_flow mit Echo-Hüllkurve,
  routing_simple_chain_flow, sidechain_flow mit Ducking im Render;
  post_fader_send_flow: Send hinter dem letzten Modul ist Post-Fader-Send,
  Limiter am Kettenende gleicht den Fader nicht aus, Gruppe bleibt Ende,
  setChain nimmt die Sends mit, altes Projekt: Fader wandert beim Laden;
  routing_ends_flow: addPlugin at end folgt keinen Sends, Post-Sends ziehen
  ans neue Ende, Spur ohne Kette in die Master-Kette = toMaster, Master-Marke
  in routing.plugins, removeGroup warnt bei Kurve/Modifier der Gruppe), Gruppen/Submix
  (groups_flow: Gruppen-Fader senkt die Mitglieder im Render, Spur ausserhalb
  bleibt, Kette der Gruppe mit Fader dahinter, Mute/Solo als Abhoer-Hilfe beim
  Abspielen, setGroup rein/raus, geteiltes Instrument nur zusammen, Stems je
  Gruppe, Freeze eines Mitglieds, removeGroup haelt die Pegel, Undo,
  Fehlertexte), Freeze/Flatten (freeze_flow: Kette als
  Audio mit Wellenform gleich an Drum Kit und Audio-Spur, 4OSC per
  Peak/LUFS, geteilter Hall bleibt live, Ablehnung ohne eigene Kette und
  geärmt,
  Sperre an Material und Kette, Fader frei, Stems, Section-Op rückt das
  Audio mit, Unfreeze, Flatten mit Fader, Undo/Redo, treeOpen; CPU-Zeit
  der Instanz je Render live/gefroren, je das bessere von zwei — robust
  gegen Last der parallelen Gruppe), Comping (comp_flow: drei Takes mit
  220/440/880 Hz, Comp aus Bereichen in Sekunden und Beats — Frequenz und
  Pegel je Bereich im Render, kein Loch und keine Doppelung an den Grenzen
  (Crossfade convex), laneActive ganzer
  Take und zurück, Undo/Redo, deleteSection rückt mit, Freeze gleich und
  gesperrt, neuer Take, MIDI-Noten je Bereich, Fehlertexte),
  Play ab Position (play_position), Tempo-Erkennung (detect_tempo_flow:
  synthetischer Groove 128 BPM 4/4 und Walzer 90 BPM 3/4 mit bekannter
  Takt-Eins, Bereich, detail, ohne Puls, Fehlertexte, Mix aus Audio-Clip und
  MIDI durch 4OSC), Tonart/Akkorde (detect_key_flow: C-Dur I-IV-V-I mit
  Takt/Schlag, B-Dur mit b, Am F C G mit Paralleltonart, gezupfte
  Melodie (Karplus-Strong) in a-, c- und e-Moll mit Quint-Hinweis, wenn
  knapp, ohne Puls, Mix aus MIDI-Akkorden durch 4OSC), Aufbau (analyze_structure_flow: Song mit
  Auftakt, Intro/Strophe/Refrain/Outro an den richtigen Takten, Buchstaben,
  Energie, Akkorde, sectionCommands per batch, ohne Puls, Mix),
  Noten in Beats (notes_in_beats_flow: startBeat/lengthBeats und
  beat/beats bei 90 BPM, setMidiNotes, clipPeaks mit Beats, Mischen und
  Fehlertexte; beats_eighth_time_flow: in 3/8 und 6/8 ist ein Beat eine
  Achtel, bpm zählt Viertel — Clip-Start und Taktlänge in Sekunden), Clips teilen/kopieren (clip_split_duplicate_flow: split
  MIDI/Audio mit freien Namen und aufgeräumten Noten, duplicate count am
  Stück und in eine andere Section, duplicateSection mit
  Clips/Tempo und Verschieben, Fehlertexte, Undo; clip_ops_flow: trimClip nach links
  laesst MIDI-Noten an ihrer Songzeit, addMidiClip kuerzt nicht am
  Section-Ende (Auto-Section); clip_fallen_flow: Praxistest 2/3 — startBeat 0
  ab späterer Section, Fehlertexte mit der Rechnung, Warnung bei startBeat
  hinter der Section, Pattern-Hinweis bei setMidiNotes/editNotes, Überlappung
  = Fehler mit dem Clip, neue Lane nur mit newLane und lauter Warnung, addClip
  in die spielende Lane (Decision 2026-10-29-ueberlappung-fehler),
  duplicateClip {section, startBeat}, addSection/moveSection
  mit after allein und position last, Auto-Section schrumpft/fällt beim
  Choppen samt Aktion, MIDI-Clip-Gain-Hinweis mit 4OSC-Kurve hörbar im
  Render; clip_fallen_tree_flow: Clip ersetzen ein Node, Pegel mehrerer
  Spuren ein Node „Song: Mix“ (auch im batch), Fader im Klang-Node der Spur
  bleibt, treeNote meldet replaced;
  clip_trim_audio_flow: trimClip nach links ueber den Dateianfang laesst Audio
  an seiner Songzeit, davor Stille, auch rueckwaerts/gestreckt, Undo), Section löschen
  (section_delete_flow: Clips weg, spätere rücken, letzte bleibt, Undo;
  Schnitt an den Grenzen: section_time_flow),
  Audio→MIDI (audio_to_midi_flow: Tonleiter 44,1/48 kHz, Akkorde
  polyphon/melody/bass ohne Oberton-Geister, gezupfte Melodie (Karplus-Strong)
  polyphon und melody ohne geteilten Nachklang (auch mitten im Ausklang) und Teilton-Geister, auch transponiert (−12, −5, +3, +7) mit Ton einen Halbton neben dem Nachklang, Bereich, Mix, addMidiFromAudio
  in eine Section; ohne ONNX skip; audio_to_midi_legato_flow: legato
  verschmilzt Vibrato-Fragmente und bindet eine 4OSC-Glide-Linie, Staccato
  bleibt kurz, Velocity aus dem Pegel, bends mit Glide von unten/oben und
  Pitch-Bend im Clip, keepOctaves in shape chords, describeVoice glideMs
  ab dem Beginn der Bewegung wie bends, hochgerechnet = Portamento 80/150 ms; played_chords_flow: shape chords als
  gespielter Part, Oktave/Arpeggio vor dem ersten Akkordanschlag), Stem-Trennung (stems_flow: Job läuft
  im Hintergrund, jobStatus wait, vier Stereo-Stems in media/stems, Bass
  im Bass-Stem, kein Gesang, addClipCommands per batch, Auswahl, Abbruch,
  Fehlertexte; ohne Modell skip), Instrumentenklassen (classes_flow:
  compareClasses mit 53er-Modell, Drum Kit + 4OSC-Akkorde gegen den
  Drum-Stem — Drum-Spur in drums, Hinweis zum Synth mit Spurnamen, ignore,
  Referenz aus dem Cache, Fehlertexte; ohne Modell skip), Timestretch (stretch_flow: ratio 0,8 →
  102,4 BPM, followTempo mit gemessenem Quelltempo → 100 BPM, kein Kürzen am
  Section-Ende (Auto-Section), Undo, Audio-Transpose +12, Fehlertexte; ohne Rubberband
  skip), Warp-Marker (warp_clip_flow: Klicks an krummen Stellen auf den
  Beats im Render ±5 ms, keine Datei je Änderung, state/clipPeaks zeigen sie, Marker verschieben als
  eine Aktion, Undo, Abstand bei 60 BPM, Sperren für reverse/stretchClip/
  trimClip, treeOpen, clear, ein Marker mit sourceBpm, detect legt einen
  128er-Groove auf 120 BPM, compact-state nur mit markerCount, Fehlertexte;
  ohne Rubberband skip; warp_realtime_flow: Echtzeit-Warp — 16-s-Clip aus
  90 s Quelle, warpClip < 0,3 s ohne Datei, Klicks ±5 ms nach dem
  Verschieben, beim Abspielen und nach Undo; compact_warp_flow: keine
  Warp-Datei über Branch/Undo, altes Projekt mit -warp-Datei spielt nach
  treeOpen die Quelle, treeCompact räumt die alte Datei), Loop im Audio-Clip (loop_clip_flow: Bereich,
  Durchgänge und Offset hörbar im Render, state.loop mit restarts,
  clipPeaks, trimClip nach links lässt die Klicks stehen, treeOpen,
  Abschalten, Loop in Beats bei 60 BPM samt offsetBeats, gewarpt + Loop,
  Sperren, Fehlertexte), Clip-Eigenschaften
  (clip_props_flow: Gain und Blenden hörbar im Render, rückwärts als
  gespiegelte Rampe, MIDI transponieren/leiser, state, Fehlertexte, eine
  Aktion je Clip, Undo; clip_normalize_flow: Peak/RMS auf targetDb über
  gainDb hörbar im Render, nur der Bereich nach trimClip, Vorgaben,
  Fehlertexte, Undo; crossfade_flow: Crossfade standardmäßig an mit Kurve convex
  (+3 dB in der Mitte bei gleichem Material), linear ohne Pegelsprung, nur
  an einem Clip blendet nur der, beide aus addieren, ausgeschaltet bleibt
  nach treeOpen aus, Clips eines älteren Projekts ohne Flag werden beim
  Laden an, fadeCurve auto, Blenden folgen moveClip, Fehlertexte, Undo), Plugin-Parameter (plugin_params_flow: MIDI-Fader
  am Instrument samt Automation, eindeutige Namen, Realwerte, mehrere in
  einem Aufruf, 4OSC-Einstellungen hörbar, Fehlertexte; eingaben_flow: Schalter und
  Zahlen als Text, Hochpass am LPF/HPF hörbar, Fehlertexte; einheiten_flow:
  real überall die angezeigte Zahl in der Einheit — alle eingebauten Plugins,
  4OSC-Hüllkurven in ms —, Fehler mit Einheit, Rezept mit real/unit wie
  listParameters, altes Rezept lädt, setAutomation mit real), Plugin-Fenster
  (plugin_window_flow mit --window-sim: auf/zu/umschalten, Lage je Modul
  in windows.json und nach treeOpen, dreimal zu/auf ohne Wandern, offen über Auto-Branch und Undo, weg
  mit removePlugin, kein Undo, keine Node-Aktion, keine Version, eingebaute
  Plugins ohne Editor; plugin_window_no_display: eigene Instanz ohne
  Simulation meldet "no display"; plugin_window_action_flow: Regler im
  simulierten Editor per pluginWindowSim, Schliessen bucht eine
  zusammengefasste process-Aktion pluginWindow mit Version, Undo/Redo und
  Auto-Branch, Eltern-Node behaelt den Stand, ohne Aenderung keine Aktion,
  setParameter bei offenem Fenster zaehlt nicht doppelt;
  plugin_window_real_flow: echte Fenster auf eigenem Xvfb (freie Nummer ab
  :90) mit Mini-Fenstermanager im Test (Rahmen 5/30 px, wie GNOME),
  Dexed und LSP Compressor (VST3 und LV2), im Prozess und mit UNIDAW_PLUGIN_SANDBOX=1 —
  auf an x/y, dreimal zu/auf ohne Wandern, verschieben, Schliessen über
  WM_DELETE_WINDOW → window: false, Lage gleich in windows.json und laut X,
  offen über Auto-Branch und Undo, Fenster gehört unidaw bzw. dem Host
  (_NET_WM_PID), Lagen mit und ohne Sandbox gleich; ohne Xvfb oder Plugin
  skip; plugin_window_x_error_flow: Mini-WM ohne _NET_WM_STATE, JUCE
  bekommt BadAtom — unidaw bzw. Host loggen es und laufen weiter, Fenster
  auf/zu/WM_DELETE_WINDOW, quit mit Exit 0, je Modus ein frischer Xvfb),
  Plugin-Sandbox
  Phase 1 (plugin_sandbox_flow: eigene Instanz mit UNIDAW_PLUGIN_SANDBOX=1,
  TestGain-VST3 rendert sample-gleich wie ohne Sandbox, Zustand per
  treeOpen im Host-Prozess, Parameter und Texte, Undo, Editor laut Host,
  Dexed sample-gleich ab dem dritten Render (wenn installiert), live über das
  Gerät (headless: skip), toter Host lädt mit letztem Zustand neu, Hosts
  enden mit unidaw; mit UNIDAW_PLUGIN_SANDBOX=0 kein Feld sandbox, sonst
  rechnet die Referenz im Prozess über pluginHosting unidaw), Plugin-Sandbox Phase 2
  (plugin_sandbox_phase2_flow: Regler im Editor des Host-Prozesses kommt
  zurueck — listParameters, pluginWindow-Aktion beim Schliessen mit
  Auto-Branch, Undo/Redo, Edit-Datei, Rezept (Dexed); neue Parameter-Namen
  wie ohne Sandbox (Surge XT Effects); Sidechain-Bus in der Edit-Datei
  aus/wieder an wie ohne Sandbox (LSP); skip je Plugin), Plugin-Sandbox
  Phase 3 (plugin_sandbox_phase3_flow: Scan-Kind — ein VST3, das beim Laden
  abstürzt (mit cc gebaut), steht in scanErrors, unidaw lebt; zwei TestGain in
  einem Host; SIGKILL → beide neu mit letztem Zustand, Render gleich, Undo;
  Dexed-Fenster nach Absturz zu; zweiter Absturz in 60 s → gesperrt, stumm,
  Render- und Fenster-Fehler, reloadPlugin; SIGSTOP → leichter Aufruf gibt
  nach 2 s auf, Wächter beendet nach 10 s, Neustart; pluginHosting
  single/dawd/plugin nach treeOpen mit gleichem Render; Fehlertexte; Hosts
  enden mit unidaw), Plugin-Sandbox Phase 4 (plugin_sandbox_phase4_flow: LV2
  im Host — zwei testgain LV2 in einem Host, Render sample-gleich wie im
  Prozess, Patch-Parameter ohne Graph, Undo; MDA JX10 in eigenem Host, erster
  Render gleich; SIGKILL → neu mit letztem Zustand samt Patch-Parameter;
  pluginHosting single/dawd; x42-eq-Fenster simuliert: Regler im Host kommt
  zurück, Schließen bucht pluginWindow, Undo/Redo, Edit-Datei, Bypass
  bleibt nach einem Render Off und der Regler wirkt; die
  LV2-Tests lv2_state_without_graph_flow, lv2_params_before_render_flow,
  find_presets_flow, sound_catalog_flow prüfen mit _check_hosted, wo das
  Plugin lief), Summe (mix_bus_flow: at master mit Pegeln, Limiter hält True
  Peak bei +12 dB, Hall hinter dem Instrument einer MIDI-Spur,
  Fehlertexte; master_routing_flow: at master mit before vor ein
  Master-Modul, Name als Adresse, Fehlertexte, routing reasons verlustfrei
  gegen die Matrix, Automation einer MIDI-Spur nicht unter ihrer Lane;
  limiter_gr_flow: Gain-Reduction und Eingangs-Crest je Limiter in
  analyze/compareReference, Hinweise bei viel Reduktion und hohem Crest,
  umgangene Master-Kette fehlt), Befunde aus Praxistest 1c (befunde_1c_flow: Note auf Beat 0
  eines krummen Clips, setMidiNotes fromBeat/toBeat, at end vor der
  Master-Kette, addPlugin after, analyze {track} ohne Master-Kette,
  listParameters paramQuery/limit, render level ohne normalize,
  findPresets mit Plugin-Namen, Mono-Warnung bei Akkorden, Synkope in
  detectTempo/extractGroove, Raster bpm/firstDownbeat bzw. Projekt in
  detectKey, compareReference downbeat mit Tonklassen, Clip ab dem
  Dateianfang; dazu Module benennen per renamePlugin/label, Name als Adresse),
  Befunde aus Praxistest 1d (befunde_1d_flow: Sampler klingt im selben batch
  nach treeCheckout, Chorus/Delay als Einstellungen und Chorus macht breit,
  dbFs je Band, keine Harmonie gegen Drum-Referenz, clipNotes, Hinweise bei
  findPresets query/treeSeed/neuer Lane, compareStems, grid 16 abgelehnt,
  Spuren benennen mit Lanes und Name als Adresse), Schlagzeug
  (drums_describe_flow: Kick/Snare-Muster trotz Bass-Uebersprechen, offene
  Hat, vorgezogenes "e", Kick-Grundton/Snare-Koerper, 2-Takt-Muster, leiser
  = gleiche Muster, compareDrums mit Zaehlzeiten; drums_tempo_flow: Hat
  kurz vor Kick/Snare verschluckt sie nicht, Hat-Timing, Kick+Snare
  zugleich, compareDrums {track} ohne Master-Kette, compareStems mit
  Fenster je Paar und Raster-Hinweis, Tempo synkopierter Akkorde ohne
  Drums, Refrain mit Kick/Bass auf den Offbeat-Achteln — der Rueckschlag
  der Snare setzt den Schlag, ohne Snare passt die Takt-Eins zum Raster,
  Intro ohne Drums meldet die offene Schlag-Phase, Drums mit Snare nicht,
  Zweifel am Taktbeginn in analyzeStructure),
  Analyse-Messfehler aus den Praxistests 2/3 (tempo_wander_flow: eigener
  Song mit spaeter klingender Bridge ohne Kick — gerades Raster statt
  Drift, analyzeStructure ohne Zweifel, steigendes Tempo behaelt den
  Drift-Hinweis; tempo_house_flow: Kick auf allen Vierteln, Clap 2/4,
  Offbeat-Hats/-Bass — der Schlag bleibt auf der Kick, auch im Drum-Stem,
  der Refrain mit Snare verschiebt weiter; compare_grid_tempo_flow:
  compareDrums {file} in anderem Tempo behaelt sein eigenes Raster mit
  Hinweis, gleiches Tempo uebernimmt das der Referenz;
  structure_parts_flow: Bridge als eigener Teil neben drei gleichen
  Refrains, Raster per bpm/firstDownbeat, findRepeats ueber den ganzen
  Song ist ein Fehler, Hinweis ohne Wiederholung, geschaetztes Tempo ohne
  Drums, Takte vor der ersten Takt-Eins; tempo_dotted_flow: punktierte
  Akzente mit Zwei-Takt-Muster ergeben 130 statt 87 BPM; glide_steps_flow:
  4OSC legato 80 ms gleitet auch bei Ganztonschritten, 0 ms nie;
  chords_overtone_flow: shape chords ohne Raster mit klarer Meldung, ein
  spaeter Oberton ist kein Anschlag, Oktave mit keepOctaves, gespielte
  Duodezime G4 ueber C3 bleibt Akkordton),
  Befunde aus Praxistest 1e (befunde_1e_flow: removePlugin schliesst die
  Kette, pickInstrument ersetzt, Spurname findet Lane-Clips, editNotes,
  setSamples note + root, renderStems ohne Master-Kette, separateStems
  ohne Projekt, Graph im Leerlauf frei, Raster ab Section und zwischen zwei
  Dateien, Automation rueckt mit, Luecken gegen die Referenz),
  Wiederholungen im Song (find_repeats_flow: Teil A kommt wieder, Stems:
  wer spielt wo, other liegt vor den Drums frei, Stems als Ordner, Dateien
  oder jobStatus-Liste, Fehlertexte), Parameter-Suche mit mehreren Woertern
  (param_query_words_flow: alle Woerter in einem Namen, sonst eines davon
  mit note, listParameters und pluginInfo),
  Musikalitaet und Klang (voice_flow: Synth-Linien mit bekannter
  Spielweise — legato/staccato, Glide, Vibrato 6 Hz, breiter Unison mit
  langsamem Attack; Schichten, Pulse und verdeckte Akkordanschlaege ueber
  einem Pad, Instrumenten-Zeitleiste mit Lead ab Takt 5; Stimmung, Muster je Takt, Huellkurve, Korrelation,
  Schwebung; compareVoice Datei/Datei und Referenz/eigene Spur mit
  Projekt-Raster, zwei Renders mit freilaufenden Oszillatoren ohne
  Abweichung, Neuanschlaege, Fehlertexte; ohne ONNX skip), Klang nachbauen (match_tune_flow: eigene Referenz aus
  einem 4OSC-Preset, matchSound findet es unter sechs Kandidaten mit
  weniger als halbem Abstand, load-Befehl laedt; tuneSound mit den eigenen
  MIDI-Noten (notes) naehert ein verstelltes Modul messbar an — ein
  setParameter, Undo; Fortschritt, ping waehrend des Jobs, Abbruch,
  Fehlertexte; ohne ONNX skip), Pegelschutz beim Klang nachbauen
  (sound_level_guard_flow: 4OSC-Dreieck-Bass mit Hochpass auf Offbeats
  gegen einen Saegen-Bass auf den Vierteln mit Hat-Uebersprechen —
  tuneSound endet nicht mehr als 6 dB leiser, ab 3 dB mit Hinweis,
  rhythmMatch und dann automatisch nur der Klang (compare sound, Hinweis);
  gegen dieselbe Referenz 30 dB leiser mit compare all wird der Hochpass,
  der den Bass wegnimmt, nicht gesetzt (note, setParameter im Ergebnis, Modul unveraendert); matchSound
  bestraft ein fast stummes Preset mit parts.level und levelVsReference;
  compare "sound" mit Hinweis, Fehlertext; Todo
  2026-10-29-klang-werkzeuge-pegelschutz; ohne ONNX skip), Verstimmung und Glide in describeTimbre/compareVoice (comparevoice_detune_glide_flow: 4OSC an der Testphrase, detuneCents trennt Unison 4/Detune 25 % von pur, glideShare nur in Legato-Uebergaengen erkennt 80 ms Legato-Glide, Abweichungen "detune"/"glide" in beide Richtungen, kein Tremolo-Hinweis am puren Klang, an einer eigenen 4OSC-Spur nennt "glide" den Regler fuer die Gleitzeit (Legato, ms) in params und fix; Todo 2026-10-27-comparevoice-detune), Stille und Spruenge (voice_silence_flow: keine Scheinnote nach einem Ton, der in Stille abbricht, auch nicht in audioToMidi/addMidiFromAudio, levelHeld stetig mit dem 4OSC-Sustain, Schicht ohne Register, stille Takte kein Wechsel), Tonwiederholungen und Neuanschlaege (voice_repeats_flow: Bass-Achtel ohne Pegeleinbruch als eigene Noten, Tremolo gegen Anschlaege im Raster, Stimmen je Strum und Strum als ein Anschlag in shape chords, hitNotes mit Lage, chordHits beider Seiten aus dem Audio, Spur gegen ihren eigenen Render (float) ohne Abweichung, Hinweis auf Clipping im 24-Bit-Render, auch je Paar in compareStems, Gegenproben register/voicing/rhythm, Tonhoehenbereich in compareVoice/describeTimbre), 4OSC-Startphase und Wellen (fourosc_seed_flow: zwei Renders sample-gleich, auch nach Play, je Note eine eigene Phase, Wave-Namen spielen die benannte Welle, Noise und Zufalls-LFO je Render gleich), Klang-Katalog fortsetzen (explore_archive_flow: tools/explore_sounds.py behält beim Fortsetzen alle gespeicherten Klänge, Startpunkt-Presets verdrängen keine, alle `*-sounds.json` mit phrase = VERSION; ohne unidaw), Surge-Werte nach loadPreset (surge_values_flow: listParameters zeigt sofort die Werte des neuen Presets statt des vorigen — Osc-Typ, Retrigger —, Retrigger an per setParameter wirkt, zwei Renders gleich; mit und ohne Sandbox; VST3-Controller-Abgleich `syncPluginParameters`, Patch juce-vst3-sync-controller; ohne Surge skip), Surge-Zufall (surge_spread_flow: compareVoice/compareStems rendern eine Spur mit externem Instrument zweimal und melden renderSpread samt Hinweis, tuneSound nennt die Zufallsquellen aus dem Surge-Patch (`PluginRandomness.*`), Init Sine gleich, 4OSC ohne zweiten Render; ohne Surge/ONNX skip), Drums + Auto-Branch (drums_and_branch_flow: Drum-Kit-
  Pattern per mode drums erkannt, Arrangieren im collect-Node ohne neuen
  Node), Agenten-Komfort
  (agent_comfort_flow: Uhr in ping/state, batch stopOnError, Clip-Name,
  setSamples-Feldprüfung), Sampler (sampler_flow: Drum Kit in GM-Belegung klingt, setSamples
  One-Shot/Bereich/Ausschnitt, Kopie ins Projekt, ogg dekodiert,
  Fehlertexte ohne Änderung, eine process-Aktion, Undo; sampler_layers_flow:
  Velocity-Layer, Round Robin im Wechsel und je Render von vorn, Choke —
  offene Hat verstummt, auch im Drum Kit —, Attack/Release hörbar im
  Render, Felder nur wenn gesetzt, Fehlertexte, Undo, treeOpen), Sample-Bibliothek
  (sample_library_flow: Kits eingebaut, Ordner anmelden, Suche über Ordner-
  und Dateinamen, blättern, Vorhörer/versteckte Ordner/able-AIFF/kaputte
  Dateien fallen weg, Symlink-Schleife; kleinkram_flow: fehlende Dateien
  mit missing und Hinweis, Filter channels in beiden Suchen, unidaw_start nennt
  den offenen Baum), Suche nach Klang (sound_search_flow:
  Analyse im Hintergrund, Vibrato/Tonhöhe/Punch/Loop-bpm an erzeugten
  Signalen, Sortieren und Filtern, Text auf den Kits mit Kategorie aus dem
  Text und deutschen Worten, like findet die gleiche Datei, Fehlertexte,
  inkrementell, Abmelden vergisst; ohne CLAP-Modell skip), Ordner ohne
  Klang-Analyse (sound_license_block_flow: noAnalysis loescht die
  Klang-Daten darunter, Analyse und like lassen den Ordner aus, Namenssuche
  bleibt, Sperre in libraries.json auch nach Neustart und von Hand
  eingetragen, allowAnalysis, Fehlertexte; eigene Instanz; Decision
  2026-11-18-klang-analyse-je-hersteller; ohne CLAP-Modell skip),
  Analyse-Ops auf gesperrtem Material (analysis_licence_block_flow: Datei
  aus dem Ordner abgelehnt, analyze ohne embed frei, Kopie per setSamples,
  Clip-FX und Freeze kennen ihre Quelle (`.dawd-origins.json`,
  `source/Origins.*`), audioToMidi auf der Spur abgelehnt mit Spur und
  Datei, detectTempo misst mit licenceNote, auch je Befehl im batch,
  freie Spur frei, allowAnalysis wirkt sofort; eigene Instanz; Decision
  2026-11-18-analyse-ops-auf-gesperrten-samples), Rezept-Katalog
  (recipe_flow: speichern aus Node/Spur/Modulen samt Reglern und Wet, Suche
  nach Text/Art/Tag/Plugin, anwenden in anderem Projekt in neuem Zweig mit
  gleichen Werten, gleicher Kette und gleichem Render, Instrument auf neuer
  MIDI-Spur, Platzhalter, Rückbau bei Fehlschlag und fehlendem Plugin,
  Bewertung und Repo-Ablage, Fehlertexte; recipe_befunde_flow: Fader,
  Automation und Parallelwege kommen mit, gleicher Render, Auto-Branch aus
  bleibt im Node, pluginInfo nennt die Rezepte, gleicher Titel aktualisiert,
  Automation in Schlägen relativ zur Section — zugeschnitten, auf eine
  andere Section mit anderem Tempo, Section unter dem Playhead, version 1
  in Songzeit —, bestehende Spur behält ihren Fader samt Lautstärke- und
  Pan-Automation), Dexed-Cartridges
  (dexed_cartridge_flow: Stimmen als Presets, Algorithmus der geladenen
  Stimme; ohne Dexed skip), Node-Notizen (tree_note_flow:
  setzen, anhängen, löschen, anderer Node, bleibt über Undo und treeOpen,
  Fehlertexte), Referenz-Vergleich (compare_reference_flow:
  Lautheit, Klangfarbe, mp3, Stereo-Seitensignal, 32-kHz-Referenz,
  mp3/ogg/flac als Clip nach WAV dekodiert, Fehlertexte;
  compare_reference_articulation_flow: 4OSC legato mit Glide gegen kurze
  Noten eine Oktave tiefer, Gegenprobe gegen sich selbst, articulation
  false, über 60 s ohne), Referenzen aus dem Systemaudio (capture_songs_flow: Mitschnitt über tests/fake_capture.py statt pw-record, Fehlstart, Kopf repariert, Wiederverbinden nach Absturz (zwei Segmente), drei Songs mit Pause im Song, kurzen Stücken (joinGapSec) und Fade-out, Parameter, Ablegen nach config/references, Fehlertexte), Undo/Redo (undo_flow: Section, Mix,
  Auto-Node weg und zurück, Schritte, Redo verworfen, Baum bleibt, Stopp bei
  Hand-Branch, Historie nach treeOpen leer; undo_delete_auto_node;
  undo_note_trackmix_flow: Auto-Node mit Notiz weg und zurück, Grund hinter
  "no such node", Mute/Solo-Hinweis, Fader im selben batch), Batch
  (batch_flow: Version/Auto-Branch
  je Befehl, stopOnError, Verbote, Log), Auto-Branch (auto_branch_flow: Sorten-
  Wechsel und Fokus-Wechsel = Kind-Node mit Label "<Spur>: Sound",
  Feinschliff zusammengefasst im Node, Rückbau bei Fehlschlag,
  Schalter), Instrument im Collect (instrument_collect_flow), Metering
  (metering_flow: analyze Song/
  Spur/Section, levels im Stand und beim Abspielen, nicht geloggt, Edit-Wechsel
  mit angemeldeten Clients; live_lufs_flow: levels.lufs beim Abspielen eines
  Tons wie analyze, nach dem Stopp bleibt integrated, play setzt zurück,
  ohne Gerät null — der Live-Teil braucht die Ausgabe, headless skip;
  master_unity_flow: Master 0 dB im neuen Edit, ein älteres Projekt mit −3 dB
  wird beim Laden 0 dB, ein anderer Wert bleibt, Render unverändert), Dynamik (dynamics_flow: Crest, PLR, Rauschteppich,
  Pegelschwankung je Band, flache Spitzen auf Hoehe des Maximums, Hinweise im
  Vergleich), Hall und Delay auf einem Stem (reverb_echo_flow: Echo 375 ms
  mit Rueckkopplung, gleich laute Wiederholungen kein Echo, Hallfahne mit
  RT60 und breiter als die Note, compareVoice meldet Delay und Hall; ohne
  ONNX skip), Render-Optionen + Stems (render_options_flow:
  Formate am Header, Samplerate/Bittiefe, Section + Ausklang, Peak/LUFS,
  Fehlertexte, Stems getrennt), Clip-Render (clip_render_flow: nur der
  Clip durch seine Spur, Fader wirkt, ohne Master-Kette und mit master:
  true, Clip auf stummer Lane klingt, Spurname, tail/flac/48 kHz, MIDI
  durch 4OSC, keine Aktion/kein Undo, Fehlertexte, gefroren abgelehnt),
  Node-Bounce (bounce_node_flow: Mix als Kind-Node mit Spur "Bounce" und
  Sections samt Tempo/Takt und Markern (Teil-Bounce: nur die der Section,
  verschoben), collect-Aktion mit Datei, Fader im Audio,
  Sammeln bleibt im Node, Verzweigen, Quelle unverändert, anderer Node und
  eine Section mit Ausklang samt Auto-Section, Teil-Bounce einer Rampe mit
  der Folge-Section als Ziel, Undo/Redo mit Basis = Bounce,
  Fehlertexte, treeOpen, Seed-Datei außerhalb des Projekts nach treeOpen),
  Batch-Render (render_batch_flow: 2 Nodes x Song/jede Section x wav/flac
  mit Dateinamen und Pegel je Node, offener Node bleibt, gemeinsame
  Optionen samt ogg ohne Bittiefe, Stems je Variante, Teilfehler in
  failed, Fehlertexte vorab ohne Datei), Tempo/Takt je Section (section_tempo_flow:
  erben, Takte, MIDI folgt dem Tempo, Noten in Sections ab t > 0, Verschieben;
  tempo_ramp_flow: Rampe zum Tempo der nächsten Section, Sekunden bleiben,
  Noten auf der Rampe wie die Engine sie rechnet, nächste Section auf ihrer
  Sekunde auch mit Taktwechsel, bars mit Rampe, exportMidi-Stufen, Kopie,
  Fehlertexte, Undo, treeOpen; tempo_ramp_eighths_flow: bars und Länge
  mit Rampe in 3/8 und 6/8, Beats der Engine in Achteln),
  Automation (automation_flow: Stufe
  hörbar im Render, Kante an/aus, Wet/Parameter/Link, Feedback, Fehlertexte,
  coalesce, treeOpen/Branch, trackMix volumeDb/pan), LFO je Parameter
  (lfo_flow: Tremolo am Fader hörbar im Takt und in Hz, zieht mit dem
  Fader hinter ein neues Modul und zurück, Low-Pass-Frequenz schwankt im
  Spektrum, eine Aktion, Undo/Redo, treeOpen, Branch, Fehlertexte; step_modifier_flow:
  Fader-Pegel folgt den Stufen im Takt und in Hz, Typ bleibt/wechselt, zieht
  mit dem Fader, Undo/Redo, treeOpen, Rezept, Fehlertexte; lfo_fader_move_flow:
  Fader-Umzug mit tiefem LFO, im Tal gestoppt und zehnmal im Lauf, behält
  den eingestellten Pegel, Rückfallwert ohne LFO-Stand, analyze nach
  treeCheckout gleich), Render-Vorlauf (analyse_vorlauf_flow: 4OSC mit
  Attack 1,5 s, kurzes Fenster misst beim ersten und zweiten Mal und nach
  treeCheckout wie der volle Render, Rezept-Hörprobe ohne Vorlauf, Limiter
  misst nur das Fenster), Gruppen-Automation
  (group_automation_flow: Kurve auf g<N>.volume senkt alle Mitglieder im
  Render, Spur ausserhalb bleibt, Kurve und LFO ziehen mit der Kette der
  Gruppe hin und zurueck, treeOpen, Undo, Fehlertexte), Anzeigen und
  API-Kleinkram (api_anzeigen_flow: LFO low/high wie im Render, trackMix und
  state melden den eingestellten Fader statt des LFO-Stands, faderAt,
  listAutomatable mit Spur-/Modulnamen, Fehler bei unbekanntem Ziel und
  Übersicht mit compact, state {only}, findPresets offset/more, MDA Image
  startet auf Mode 0,75 und lässt Mono in der Mitte), Absturzschutz (autosave_flow,
  session_restore_flow mit eigener Instanz auf Port 7412: SIGKILL,
  Neustart mit --session, SIGTERM), Audio-Backend (audio_api_flow, Port 7431:
  UNIDAW_AUDIO=jack ohne JACK-Server startet trotzdem, Geraete-Setup nie in der
  Settings.xml des --config-Ordners, Altlast wird geraeumt), MCP-Server als
  Plugin und Bundle (mcp_package_flow: Versionen gleich CMake, Marketplace
  zeigt aufs Plugin, .mcpb reproduzierbar mit SHA-256 in server.json,
  Registry-Grenzen, Server aus dem entpackten Bundle antwortet auf
  initialize/tools/list/ping, unidaw_start ohne unidaw nennt UNIDAW_BIN, AppImage
  startet mit "unidaw"), Ressourcen ohne Quellordner (resources_relocated_flow:
  kein Quellpfad in der Binary, Kopie unter usr/bin mit share/unidaw — UI,
  Vendor, Kits, Plugin-Profile, Rezepte, Modelle im Nutzerordner —, Starter
  startet bzw. laesst ein laufendes unidaw stehen, mit --foreground endet unidaw
  mit Ctrl+C, Link auf die Binary, Starter
  ohne unidaw), Audio-Interface (interface_flow:
  simuliertes Fireface UC, Paare, Mono in beide Seiten, Fehlertexte ohne
  Änderung, Mix pro Song über treeOpen und Einstecken, kein Undo, keine
  Node-Aktion), Laden und Ladestand (load_progress_flow: GET /progress im
  Leerlauf, während eines langen Befehls und danach mit last, Zuweisungen
  ohne Audio-Graph, Antworten einzeilig, Cache-Control no-store, play ohne
  Autosave, abgebrochener Client beendet unidaw nicht, listPlugins aus dem
  Scan-Stand), Solo und Mute als Abhör-Hilfe (listen_flow: auf anderer Spur
  ohne Node und Aktion, Wiedergabe läuft weiter, kein Undo-Schritt, nicht in
  der Node-Datei, Render ignoriert beides), Wiedergabe über einen
  MIDI-Scan (play_survives_midi_rescan: erster MIDI-Scan einer eigenen
  Instanz während play — spielt weiter, Position läuft), Speichern ohne Lücke
  (node_save_atomic_flow: inotify sieht die Node-Datei beim Speichern nie
  gelöscht, nur per rename ersetzt — ohne und mit Solo), OSC-Fernsteuerung (osc_flow,
  OSC-Port UNIDAW_TEST_PORT + 1000: standardmäßig aus, Fehler ohne Änderung,
  belegter Port, osc.json, Transport/Section/Marker, Taster-Loslassen
  ignoriert, Fader gebündelt als ein Undo-Schritt, Name mit "_", Index,
  Bundle, Plugin-Parameter, kaputtes Paket, fader 0..1 gegen die Kurve aus
  flow.html, fremder Absender (127.0.0.2) verworfen und gezählt, allow,
  unbekannte Adressen, Event-Log mit "osc", aus = ohne Wirkung), About/Lizenzen (about_flow: Lizenz AGPLv3,
  appVersion gleich project(), sourceUrl, thirdParty gleich
  THIRD_PARTY_LICENSES.md, read-only, Knopf und Dialog im UI), Migrationen (v1, v2), Upload +
  Insert-Flow, Listen, listDir samt Projektordnern (listDir_projects), die
  Routen /ui, /health, /audio und Render/Peak/Dump, Schutz der localhost-API
  (api_guard_flow: ohne/falscher Token 401 und quit laeuft nicht, Upload
  ohne Token schreibt nichts, fremde Origin und `null` 403, fremder oder
  fehlender Host 403 auch fuer /ui und /audio, UI traegt den Token, Datei
  0600; eigene Instanz ohne UNIDAW_TOKEN: Zufalls-Token, zweiter Start auf
  belegtem Port laesst die Datei stehen, nach quit weg).
- Jedes neue Custom-Feature kommt mit Tests in die Suite. Wenn ein Feature
  geändert wird, wird der Test mit geändert — die Suite ist die Verifikation,
  kein manuelles Durchklicken.
- Audio-Aufnahme (record) braucht einen Audio-Eingang: ohne ihn (headless)
  wird nur der Guard-Weg geprüft; mit Eingang nimmt arm_track_flow Audio und
  MIDI in einem Record parallel auf (sonst "skip" in der Ausgabe). MIDI-Aufnahme läuft headless über den
  ALSA-Midi-Through-Loopback (Client 14:0) und wird voll getestet
  (midi_record_flow, Multi-Track in inputs_flow, über das Section-Ende mit
  Auto-Section in section_record_flow). Der monitor op (Input armen
  ohne Aufnahme) wird mit monitor_flow über denselben Loopback getestet.
  MIDI-Takes werden beim Stop
  als WAV gerendert (direktes Task-Muster — renderToFile mit useThread braucht
  die Progressbar-UI und läuft headless nicht).
- Tests laufen gegen eine eigene unidaw-Instanz (Port 7411, temporaere
  Projektordner) — eine laufende Entwickler-Instanz auf 7400 bleibt unberührt.
  `UNIDAW_TEST_PORT` setzt einen anderen Port (die Session-Instanz nimmt
  Port + 1). Ohne ihn sucht `tests/run.sh` den ersten freien Basis-Port aus
  7411, 7441, 7471 … (`tests/ports_free.py`) und sperrt ihn per `flock` für
  den Lauf — parallele Suiten aus Worktrees und dem pre-push-Hook teilen sich
  nie eine Instanz. `run_tests.py` startet nicht, wenn auf dem Port schon ein
  unidaw antwortet, bricht ab, sobald die eigene Instanz stirbt (mit dem Ende
  von `unidaw.err`), schickt `quit` nur an die eigene Instanz, und die Instanz
  endet mit dem Lauf (PR_SET_PDEATHSIG). Alle
  Test-Instanzen laufen mit `--interface-sim` — kein Test fasst das echte
  Audio-Interface an. Kein Test öffnet ein Fenster auf dem Bildschirm: die
  Suite leert `DISPLAY` (und `WAYLAND_DISPLAY`) für alle Instanzen, die
  Suite-Instanz simuliert Plugin-Fenster (`--window-sim`). Echte Fenster
  prüft nur `plugin_window_real_flow` auf einem eigenen Xvfb (Paket
  `xvfb`). Xvfb hat keinen Fenstermanager; den spielt der Test selbst
  (`_mini_wm`, ctypes auf libX11: EWMH-Atome, Rahmen, ConfigureNotify).
  `plugin_window_x_error_flow` lässt `_NET_WM_STATE` weg: JUCE bekommt
  BadAtom, unidaw und Host loggen es (`[x11 <pid>] X error: …`,
  `source/XErrors.cpp`) und laufen weiter.
- Kein `pkill -f "port NNNN"` beim Entwickeln (bleibt hängen, wird abgebrochen):
  die Suite verwaltet ihre Instanz selbst; läufernde Repro-Instanzen über die
  API beenden (`/command` mit `quit` und Token aus
  `<config>/api-token-<port>`, `ARCHITECTURE.md`, localhost-API absichern).
- `UNIDAW_TESTS=name1,name2` laesst beim Entwickeln nur diese Tests laufen
  (Tests, die auf einen vorherigen Seed bauen, wie insert_flow, scheitern
  dann); vor dem Commit immer die ganze Suite.
- Reihenfolge ist build → test: die Tests brauchen das gebaute Binary, `run.sh`
  macht beides in einem Schritt. Vor dem Commit ausführen, nicht danach.
