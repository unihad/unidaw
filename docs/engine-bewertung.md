# unidaw — Bewertung der Tracktion Engine

Wie gut trägt die Tracktion Engine unidaw, was fehlt ihr, und lohnt sich ein
Wechsel? Stand 2026-10-14. Die Messwerte stammen aus `bench/engine/`
(Rohdaten als CSV dort), die Umsetzung danach aus den Commits `17c2325` bis
`125795b`.

**Entscheidung: Tracktion Engine behalten und ausbauen, nicht ersetzen.**
Die Engine ist nicht der Engpass. Audioleistung und Funktionsumfang reichen,
die meisten Lücken der Feature-Longlist kann sie schon — es fehlte nur der
unidaw-Befehl. Ein Wechsel kostet Monate und bringt keine Alternative, die
besser passt.

## Ausgangslage

| Aspekt | Stand |
| --- | --- |
| Engine | Tracktion Engine 3.5.0 auf JUCE 8.0, Git-Submodul |
| unidaw-Code | rund 7.300 Zeilen C++ (vor dem 14.10.) |
| Engine-Aufrufe | rund 460, die meisten in `Routing.cpp`, `Media.cpp`, `Processing.cpp` |
| Speicherformat | jeder Baum-Knoten ist eine eigene `.tracktionedit`-Datei |
| Audio-Treiber | ALSA (über PipeWire); JACK über pipewire-jack mit `UNIDAW_AUDIO=jack` (eingebaut, wenn die JACK-Header da sind), s. `docs/live-audio.md` |
| Plugin-Formate | VST3, LV2, Built-ins; kein CLAP (JUCE 8.0 kann es nicht) |
| Tests | Suite gegen die Befehls-API, unabhängig von der Engine |

## Performance

Gemessen auf einem AMD Ryzen 7 4800H (8 Kerne, 16 Threads), Block 512 bei
44,1 kHz, 16 Spuren mit Audio-Clip, Module als Kette je Spur.

| Module | Graph-Neuaufbau | Render 1 Thread | Render 16 Threads | schlimmster Block (16 Threads) |
| --- | --- | --- | --- | --- |
| 0 | 0,3 ms | 28× Echtzeit | 39× Echtzeit | 0,2 ms |
| 16 | 1,2 ms | 15× | 25× | 0,4 ms |
| 32 | 2,6 ms | 9× | 18× | 3,1 ms |
| 64 | 7,0 ms | 4,3× | 9,2× | 0,8 ms |
| 128 | 19,5 ms | 2,0× | 3,8× | 2,4 ms |

- **Echtzeit:** Kein einziger Block über dem Budget von 11,6 ms, auch nicht
  mit 128 Modulen oder 32 Helm-Instanzen (LV2, echtes Fremd-Plugin). Live
  über das Gerät (15 s je Szenario): 0 XRuns, CPU-Last höchstens 17 %.
- **Mehrkern:** Pro Block skaliert die Engine gut (128 Module: 3,5 ms → 0,8 ms
  mit 16 Threads). Der Datei-Render skaliert schwächer (Faktor 1,5–2,1),
  weil dort Schreiben und Warten dazukommen.
- **Graph-Neuaufbau:** Jede strukturelle Änderung (`connect`, `addPlugin`)
  baut den ganzen Graphen neu, im Haupt-Thread, nicht im Audio-Thread. Die
  Kosten wachsen stärker als linear (doppelt so viele Module ≈ 2,8-fach),
  bleiben aber bei rund 20 ms für 128 Module. Kein Grund für einen Wechsel.
- **Grenzfall:** Mit nur einem Thread kam bei 32 Helm-Instanzen ein Block
  auf 11,3 ms. unidaw spielt live mit allen Kernen.
- **Latenz** ist bei unidaw eine Einstellung (ALSA 512 × 4 Puffer ≈ 45 ms),
  keine Engine-Grenze. Für live: JACK bei 64 Frames, s. `docs/live-audio.md`.
- Nicht gemessen: VST3-Plugins von Drittherstellern (keine installiert),
  Last durch andere Programme.

## Engine-Eigenheiten, die unidaw umschifft

Jede davon hat beim Bauen Zeit gekostet — vor Änderungen in diesen Bereichen
lesen.

| Eigenheit | Folge | Wie unidaw damit umgeht |
| --- | --- | --- |
| `TempoSetting::setBpm` rechnet alle Positionen im Edit auf die Beat-Lage um | Sections und Clips wandern beim Tempowechsel (30 s → 40 s) | `set (…, remap = false)` in `applySectionTempos` |
| Tempo-Rampe (`tempo::Sequence`): an einem Taktwechsel mitten in einer Rampe beginnt der Abschnitt mit dem Tempo am Rampenanfang; die Stufenzahl (4 je Beat) springt mit der Länge | Der Endpunkt einer Rampe trifft die Sekunde des Section-Starts nicht von selbst | `applySectionTempos` führt den Beat des Endpunkts nach (`settle`, auch nach dem Taktwechsel); Test tempo_ramp_flow (Taktwechsel hinter der Rampe, 2026-10-25) |
| `MidiClip::mergeInMidiSequence` liest Zeitstempel als absolute Edit-Zeit | Noten in Sections ab t > 0 landeten vor dem Clip | `mergeNotesRelative` verschiebt um den Clip-Start |
| Der Renderer nimmt Kind-Spuren einer erlaubten Spur immer mit | Alle Spuren laufen in die Void-Spur — `tracksToDo` filtert nicht | Stems und `analyze {track}` filtern über `allowedClips` |
| `endAllowance` greift im Datei-Render nicht | Ausklang fehlte | `tail` verlängert den Bereich selbst |
| Der Graph ist azyklisch, rückgekoppelte Sends fallen still weg | Kein Feedback über AuxSend | eigenes Plugin-Paar mit Ringpuffer (`Feedback.cpp`) |
| Ein direkt gesetzter CachedValue erreicht seinen AutomatableParameter nicht | Wert ändert sich nicht hörbar | Feedback-Pegel/-Verzögerung über `setParameter` |
| `LevelMeasurer` hält rohe Zeiger auf Clients | Absturzgefahr, wenn Track oder Edit mit angemeldetem Client fällt | `executeOp` meldet vor jeder Mutation alle Clients ab (`clearMeters`) |
| Neue Edits haben eine Master-Lautstärke von −3 dB (`Edit::Options::defaultMasterVolumedB`); der Renderer nimmt sie nicht mit (`Renderer::Parameters::useMasterPlugins = false`) | Die Wiedergabe am Gerät ist 3 dB leiser als Render und `analyze` (gemessen 2026-10-26: Main −26 dBFS, Geräteausgang −29 dBFS) | `unityMasterVolume` in `ensureRouting` setzt die Vorgabe −3 dB auf 0 dB (neue und geladene Edits, ein anderer Wert bleibt; [Decision](decisions/2026-10-26-master-0db.md), Test master_unity_flow) |
| Ausgang stumm ab 98 % CPU-Last (`cpuLimitBeforeMuting`) | Plötzliche Stille bei Überlast | bekannt, kein Eingriff |
| `renderToFile` mit Thread braucht die Fortschritts-UI | läuft headless nicht | direktes Task-Muster (`createRenderTask` + `runJob`) |
| Gespeichertes ALSA-Layout mit 64 Geister-Kanälen | lädt bei jedem Start | `DawdEngineBehaviour` beschreibt die Wave-Devices selbst |
| Render-Precheck bei reinen Synth-Graphen | „Didn't find any audio to render“ | `render {force: true}` |
| Offline-Render liest mit 5 s Timeout je Block (`WaveNodeRealTime`: `isOfflineRender ? 5s : 0ms`; `AudioFileCache::CachedFile::read` wartet mit `yield`, dann Stille). Die Länge einer Datei hält der `AudioFileManager` im Cache | Fehlt eine Datei, die die Engine kennt, aber nie geöffnet hat (z. B. Warp-, FX- oder Freeze-Datei gleich nach dem Schreiben gelöscht, Base-Schnappschuss nach `treeCompact`), wartet jeder Block 5 s — `render`/`analyze` kommen praktisch nicht zurück, der Message-Thread steht (auch `ping`). Eine schon gemappte Datei liest weiter (Linux), bei einer schon beim Laden fehlenden gab `render` `ok` ohne Hinweis | `runRender` prüft vorher die Dateien des Render-Bereichs (`missingMedia`) und meldet Clip und Datei; `state.missingFiles`, Clip `missing` (Todo 2026-10-26-fehlende-mediendatei-haengt, Test missing_media_flow). Live-Wiedergabe wartet nicht (0 ms) |
| JUCE-LV2-Host: `prepareToPlay` sichert den Zustand aus den Ports und überschreibt danach den Parameter-Cache | Werte, die vor dem ersten Render/Play (oder zwischen zwei `prepareToPlay`) gesetzt wurden, gingen stillschweigend verloren — Anzeige stimmte, Klang nicht (LSP-EQ, x42) | Patch `dawd/patches/juce-lv2-pending-parameters.patch` (über `patches/apply.sh` bei CMake-Configure und `tests/run.sh`), Upstream-PR an JUCE offen; Test `lv2_params_before_render_flow` |
| JUCE-LV2-Host: `getStateInformation` liest die Control-Ports, gesetzte Werte kommen erst im nächsten `processBlock` dorthin | Ohne laufenden Audio-Graphen (kein Gerät, `--no-audio`, nie gespielt, nach `releaseIdleGraph`) fehlte ein frischer `setParameter`-Wert in Undo-Schnappschuss, Edit-Datei und Branch — `setParameter`, `undo`, `redo` gab den alten Wert (x42-eq) | Patch `dawd/patches/juce-lv2-state-pending-parameters.patch`: offene Port-Werte vor dem Sichern in die Ports; Test `lv2_state_without_graph_flow` (2026-10-24) |
| Dasselbe für Werte, die nur im Audio-Block beim Plugin ankommen: LV2-Patch-Parameter (JUCE-gebaute LV2 wie testgain) und die Komponente eines VST3, das Parameter nicht selbst vom Controller übernimmt (Dragonfly; JUCE-VST3 wie testgain übernehmen sie) | Ohne Audio-Block seit dem Setzen fehlt der Wert im gespeicherten Zustand; bei LV2-Patch-Parametern zeigt `listParameters` nach dem Laden den Standardwert, bis das Plugin einmal rechnet | Patch `dawd/patches/tracktion-flush-pending-values.patch`: ohne Graph bekommt nur das betroffene Plugin beim Sichern und nach dem Wiederherstellen 1 Sample Stille (0 Samples reichten Dragonfly nicht), vorher einmal vorbereitet; Test `lv2_state_without_graph_flow` (2026-10-25) |
| JUCE-LV2-Host: ein offener Editor legt Änderungen in die Warteschlange der Instanz, nur `processBlock` leert sie; `prepareToPlay` baut die Instanz neu | Bei einer nie vorbereiteten Instanz gingen Editor-Änderungen verloren | Patch `dawd/patches/juce-lv2-ui-messages-prepare.patch`: wartende Nachrichten wandern in die neue Instanz; ein offenes Editor-Fenster zählt beim Sichern als „Werte offen“ (Tracktion-Patch oben). Nur aus dem Code belegt — ohne Anzeige kein Test mit echtem Fenster |
| VST3 mit dynamischen Parametern (Surge XT, Surge XT Effects) | Namen/Texte der „FX Parameter n“, „FX A1 Param 3“, „M5: -“ erst, nachdem das Plugin Audio verarbeitet hat; unidaw baut den Graphen erst beim Abspielen — nach `treeOpen`, Checkout, Undo, Auto-Node, `loadPreset` und als neue Instanz blieben die Rohnamen bis zum ersten Render (Praxistest 1n P3, 1m P4). Surge XT meldet die echten Namen ab dem 12. `processBlock` (16 × 1 Sample reichten, 11 × 256 nicht), asynchron über den Message-Thread | `settleParameterNames` (`EngineHelpers.cpp`, nach jeder verändernden Op in `executeOp`): neue Instanzen und das Modul nach `loadPreset`/`setPluginFile` ohne Graph bekommen 32 × 1 Sample Stille über `passPendingValuesToPlugin` (Surge ≤ 3 ms, Diva in der Sandbox ≤ 6 ms); danach liest `syncPluginParameters` Namen und Werte in derselben Op (die Meldung von Surge kommt aus dessen eigenem Message-Thread, `restartComponent` lief erst nach der Op — unter Last auch nach der nächsten: `surge_names_flow` wackelte, 2026-10-29); zu Beginn der nächsten Op gleicht `rememberSettledNames` noch einmal ab (lief ein Graph) und merkt die Namen, damit ein Auto-Node in derselben Op sie trifft. Offen: nach FX-Typ-Wechsel per `setParameter` (Surge XT Effects) helfen stille Blöcke nicht (auch nicht 16 × 256, `prepareToPlay`, `isPlaying`), erst ein Render — Kurznotiz: dann einmal rendern. Test `surge_names_flow` (2026-10-26) |
| VST3-Zustand, den das Plugin erst im Block übernimmt (Surge XT: `setStateInformation` stellt den Patch nur an, der nächste Block lädt ihn) | JUCE gleicht beim `setStateInformation` den Controller sofort ab (`setComponentState` → `kParamValuesChanged`) — vor dem Laden. Danach meldet Surge die neuen Werte nie (keine Ausgangs-Parameter, kein `kParamValuesChanged`; Log 2026-10-29). `listParameters` zeigte nach `loadPreset` die Werte des vorigen Presets, ein `setParameter` auf den alten Wert kam nie an (Retrigger blieb aus, Render 1 zufällig) | Patch `juce-vst3-sync-controller`: `VST3Client::syncControllerWithComponent()` gibt dem Controller den aktuellen Zustand der Komponente und liest Titel und Werte neu; `syncPluginParameters` (`EngineHelpers.cpp`) ruft es nach den stillen Blöcken (`settlePluginNow` in `loadPresetEntry`, `settleParameterNames`), in der Sandbox über die Host-Op `info {syncController}`, danach `refreshParameterValues` (Patch `tracktion-refresh-parameter-values`). Test `surge_values_flow` (2026-10-29) |
| Surge-Makros („M1“ … „M8“): `getText` zeigt immer den aktuellen Wert, nicht den übergebenen | `listParameters` meldete min = max, `setParameter {text|real}` rechnete über den flachen Text und setzte still 0 (Praxistest 1m P12) | `textIgnoresValue` (`PluginSettings.cpp`): min/max leer, `real`/`text` mit Fehler „set it with value 0..1“; `value` wirkt. Test `surge_names_flow` (2026-10-26) |
| 4OSC mit Unison (mehr als eine Stimme je Oszillator): `MultiVoiceOscillator::process` rechnet dann nur mit Spread, Pan bleibt ohne Wirkung | Pan 1 hart links / Pan 2 hart rechts änderten weder Pegel L/R noch Korrelation (Praxistest 1m P7, Synth mit 4 Stimmen, Spread ±80–100 %) | Patch `dawd/patches/tracktion-fourosc-unison-pan.patch`: Pan als Balance über der Spread-Stellung wie bei einer Stimme, in der Mitte sample-gleich; Test `fourosc_pan_flow` (2026-10-26) |
| `juce::MidiMessageSequence::updateMatchedPairs` paart Note-On und Note-Off der Reihe nach | Gleiche Tonhöhe überlappend (Note um 0,05 s verlängert): die spätere Note bekam das Note-Off der früheren (0,05 statt 1,0 s), in jedem Instrument und Voice Mode (Praxistest 1f P11, „4OSC Mono“) | `parseNotes` (`EngineHelpers.cpp`) entflicht vorher: die frühere endet beim Einsatz der späteren (wie `createDefaultPlaybackMidiSequence` beim Abspielen), Note-Offs vor Note-Ons; Test `note_overlap_flow` (2026-10-26) |
| Externe Plugin-Instanzen werden beim Abbau des Edits nicht gelöscht, sondern in eine Warteschlange gestellt (`AsyncPluginDeleter`, eine je 100-ms-Tick) | Node-Wechsel blockieren die Ticks; bei schnellem Wechseln stauten sich Instanzen (nachbau-12 mit 2× Surge XT: 38 MB je Wechsel, 4,2 GB nach 140 Wechseln; Undo lädt genauso) | `DawdEngineBehaviour::doAdditionalInitialisation` setzt `setDeletesPluginInstanceSynchronously (true)` — unidaw hat keine Plugin-Fenster; danach 344 MB nach 140 Wechseln, Checkout 0,30 → 0,27 s, Undo 0,75 → 0,29 s (2026-10-20) |
| Ein abgeschaltetes internes Plugin in einem Rack bleibt im Render-Graphen und meldet seine Latenz weiter, verzögert aber nicht (nur externe Plugins bekommen eine Ersatz-Verzögerung); der Render wirft die gemeldete Latenz am Anfang weg | Spur-Renders ohne Master-Kette (`analyze {track}`, `compare* {track}`, `renderStems`) lagen um die Latenz zu früh: Limiter 78 Samples (1,8 ms, kippte offen/zu-Urteile der Hats), Pitch Shifter 8192 Samples (186 ms) | Das Rack des Moduls wird umgangen, nicht das Plugin (`bypassTarget` in `ops/Processing.cpp`, wie beim Einfrieren); Test `grid_bypass_flow` (2026-10-24) |
| Ein Datei-Render beginnt an `start`: laufende Noten setzen dort neu ein, Instrumente und Effekte behalten den Zustand vom Ende des vorigen Renders (trotz `flushAllPlugins` der Engine) | `analyze {track, start, end}` maß ein kurzes Fenster je nach Vorgeschichte anders: Surge XT „Cyber Pad“, 2,00–2,06 s erste Messung −44 dB, zweite −32 dB, voller Render −30,6 dB (Todo `2026-10-30-lfo-wert-im-fader`); 4OSC mit Attack 1,5 s −16,4 statt −6,9 dB | Fester Vorlauf in `renderMixRange` (`ops/Processing.cpp`): bis 2 s vor `start` (ab 0 s) mitrendern, die Datei enthält nur das Fenster, Limiter messen ab `start` ([Decision](decisions/2026-10-30-analyse-vorlauf.md)); Test `analyse_vorlauf_flow` (2026-10-30). Bleibt: ein Fenster, das vor 2 s beginnt, erbt weiter den Rest des vorigen Renders; Hüllkurven über 2 s setzen am Vorlauf-Beginn neu ein |
| Warp Time der Engine in Echtzeit (`WarpMap` im Beat-Weg von `WaveNodeRealTime`): ein eigener Stretcher vor dem Tempo-Stretcher, die Geschwindigkeit wechselt je Block; nur bei autoTempo/autoPitch wirksam; vor dem ersten Marker falsche Quellzeit; der Weg über eine Proxy-Datei (`WarpTimeRenderJob`) braucht Proxys, die unidaw abschaltet | Klicks auf Markern lagen ±20 ms (120 BPM) bzw. ~90 ms zu spät (60 BPM) neben dem Beat (gemessen 2026-10-26) | Patch `dawd/patches/tracktion-warp-realtime.patch` (2026-10-31): ein Stretcher; der `TimeRangeReader` bildet die Warp-Zeit jedes Blocks auf die Quellzeit ab, der `TimeStretchReader` setzt das Verhältnis je eingespeistem Stück aus der Map an der Stelle, die Rubberband gerade rechnet (384 Samples hinter der Einspeisung; je Ausgabeblock lag es bis 7 ms daneben). Einsätze ±3,5 ms (44,1 kHz) bzw. ±4,5 ms (48 kHz) im Render, `warpClip` 0,01 s statt 1,7 s (200 s Quelle) bzw. 5,6 s (Live-Instanz), keine Datei. unidaw führt die Marker als `DAWDWARP` am Clip und gibt sie über `EngineBehaviour::getWarpPointsForClip` an die Engine (nicht den WarpTimeManager); der Clip spielt die Originaldatei mit followTempo. Bis 2026-10-30 renderte `warpClip` offline (Rubberband mit Keyframe-Map, ±5 ms, eine Datei je Änderung). Tests `warp_clip_flow`, `warp_realtime_flow` |
| Loop im Zeit-Weg (kein followTempo) mit speed ≠ 1: der Knoten liest den Loop-Start als Dateisekunde, das Modell (`setSpeedRatio`, Begrenzung) als Timeline-Sekunde | Loop an falscher Stelle, sobald ein gestreckter Clip loopt | `loopClip` nur mit speed 1 oder followTempo; `stretchClip` lehnt loopende Clips ab |
| Echtzeit-Rubberband-Hülle (`RubberBandStretcher::processData`, `tracktion_TimeStretch.cpp`): nach jedem Neustart (Clip-Anfang, Loop-Neustart, Sprung) verwarf sie den Vorlauf (`getStartDelay`, 512 Samples bei 44,1 kHz) nur 256 Samples je Aufruf und gab im selben Aufruf schon 256 Samples aus | Statt der ersten 256 Samples Audio kam Vorlauf (fast Stille); ein Einsatz genau am Clip- oder Loop-Anfang fehlte bis 5,8 ms (der Rest der Blockhälfte, im Render bei Clip-Start in der ersten Hälfte eines 512er-Blocks; jeder Loop-Neustart), danach lag alles richtig — gemessen an Clips mit followTempo (auch geloopt); laut Code läuft jeder Clip mit Echtzeit-Stretcher durch dieselbe Hülle (gewarpt — seit 2026-10-31 mit wechselndem Verhältnis, Patch tracktion-warp-realtime —, `stretchClip ratio`, Audio-`transpose`). Rubberband allein (Pad + StartDelay) dämpft nicht: nachgestellt mit dem Aufrufmuster der Hülle (2026-10-26) | Patch `dawd/patches/tracktion-rubberband-start-delay.patch`: erst der ganze Vorlauf weg, dann Ausgabe; Test `stretch_edge_flow` (Clip-Anfang percussive/melodic, getrimmt, Loop-Neustarts) |
| Clip-Offset liegt in Sekunden, auch bei Clips mit followTempo (`getOffsetInBeats` = Offset × Beats/s am Clip-Start) | Ändert sich das Section-Tempo, verschiebt sich das Material eines Clips mit Offset um Offset·(1 − alt/neu) Beats (gewarpte Clips haben fast immer einen Offset) | Section-Ops und `treeCompact` merken sich vorher den Offset in Beats und stellen ihn nach `applySectionTempos` wieder her (`followTempoOffsets`/`keepOffsetBeats`); Test `tempo_offset_flow` (bpm, Rampe, Taktwechsel, geerbtes Tempo nach `moveSection`), `warp_clip_flow` (60 BPM auf den Beats). Unter einer Rampe liegen Einsätze im Render bis ~8 ms hinter dem Beat der Tempospur (gemessen 2026-10-26) |
| Ein Clip aus einer komprimierten Datei (ogg, mp3; flac ungeprüft) hält den Render an | `analyze`/`render` kommen nie zurück, der Daemon hängt | `insertAudioClip` dekodiert komprimierte Dateien einmal nach `media/<name>.<ext>.wav` (`playableAudioFile`) — Clips zeigen nur auf WAV/AIFF |
| Die Engine prüft alle 4 s die MIDI-Ports (ALSA); kommt ein Port dazu oder fällt einer weg (irgendein Programm auf dem Rechner), baut sie die Geräte aller Playback-Kontexte neu (`clearAllContextDevices` → `clearNodes` mit `discardRecordings`) | Eine laufende Aufnahme war verworfen, der Transport stand, `record off` meldete „not recording“ (`lane_active` wackelte in der Suite); ebenso der erste MIDI-Scan einer Instanz während einer Audio-Aufnahme | Patch `tracktion-midi-rescan-while-recording`: die neue Liste wartet, bis kein Transport aufnimmt; Test `record_survives_midi_rescan` (2026-10-26) |
| Derselbe Neubau der Geräte (`releaseDeviceList` → `clearNodes`) stoppt auch den Playhead; der Transport-Timer (`TransportControl::timerCallback`) sieht „Transport spielt, Playhead nicht“ und stoppt die Wiedergabe | Ein Port-Wechsel beim Abspielen beendete die Wiedergabe; unter Last kam der zweite MIDI-Scan einer frischen Instanz kurz nach `play`, `listen_flow` sah nach Solo `playing: false` bei Position 0 (etwa 1 von 30 Läufen) | Patch `tracktion-midi-rescan-while-playing`: der Playhead läuft weiter, wenn er lief und keine Aufnahme läuft; den Graphen baut der folgende Neustart. Test `play_survives_midi_rescan` (Position läuft nach dem Scan weiter); `listen_flow` unter Last 0 von 80 (Todo 2026-11-20-solo-kurz-nach-play-stoppt-die-wiedergabe) |
| Schreib-Thread der Temp-Datei (`ThreadedEditFileWriter` in `tracktion_EditFileOperations.cpp`): geteilte Ressource, endet mit dem letzten `EditFileOperations`; der Destruktor weckt ihn über `flushAllFiles()` und setzt erst danach das Exit-Flag, `stopThread()` weckt nur das eigene Event, nicht `waiter` | Kommt der Thread unter Last zwischen beiden dran, schläft er erneut `waiter.wait (1000)`: das Speichern nach einer Mutation hält den Message-Thread 1,0 s (`warpClip` in `warp_realtime_flow` neben der Suite; 2026-11-20 mit 600 `trackMix` neben der Suite: 0–3 Ops mit 1005 ms) | Patch `dawd/patches/tracktion-edit-file-writer-stop.patch`: erst Exit-Flag, dann `waiter` wecken; danach 0 von 1800 Ops über 30 ms (Todo 2026-11-19-warp-realtime-flow-marker-verschieben-unter-last) |
| `EditFileOperations::save` legt `.tmp_<Edit>` per `File::moveFileTo` auf die Edit-Datei; JUCE löscht dabei erst das Ziel und benennt dann um | Für einen Moment fehlt die Node-Datei: `listen_flow` sah unter Last einmal in 60 Läufen „No such file“ direkt nach `renderNode` (Autosave lief); ein Absturz in der Lücke lässt nur `.tmp_node-N` liegen | Patch `dawd/patches/tracktion-save-replace-file.patch`: `File::replaceFileIn` (ein `rename()` über die alte Datei, wie `XmlElement::writeTo`); Test `node_save_atomic_flow` prüft per inotify, dass die Node-Datei nie gelöscht wird (Todo 2026-11-20-node-datei-beim-speichern-atomar-ersetzen) |

Drei Funde lagen in unidaw selbst: Der HTTP-Server las `Content-Length` nur in
dieser Schreibweise (Node-`fetch` schickt Kleinbuchstaben), und Narrow-
Literale mit Nicht-ASCII-Zeichen kamen als Latin-1 an — Labels mit „·“, „→“
laufen über `juce::String::fromUTF8`. Ohne `Cache-Control` legte Firefox jede
POST-Antwort als eigenen Cache-Eintrag ab (UI-Polls ~14/s; 2026-10-20: 400 000
Einträge, 2,2 GB, Cache-Thread dauernd voll, die Seite hing) — jede Antwort
trägt jetzt `Cache-Control: no-store`.

## Was der Engine fehlt

| Fehlt | Vorbild | Relevanz für unidaw |
| --- | --- | --- |
| Plugins in eigenen Prozessen | Bitwig, REAPER | Mittel: ein Absturz beendet den Daemon — Autosave und Neustart begrenzen den Schaden |
| CLAP-Plugins | Bitwig, REAPER | Mittel |
| Schrittweiser Graph-Umbau | REAPER | Niedrig: rund 20 ms bei 128 Modulen |
| Graph mit Schleifen, freie Matrix | REAPER, Bitwig | Niedrig: in unidaw nachgebaut |
| Tempo-Erkennung, Spektral-Analyse | Logic, Live | Mittel: für Analyse-Befehle |
| Video in der Timeline | Cubase, Logic, Pro Tools | erst bei Filmmusik |
| Surround und Atmos | Pro Tools, Logic | erst bei Bedarf |
| Hochwertige eingebaute Effekte | alle großen DAWs | Niedrig: Qualität kommt über VST3/LV2 |

Schon in der Engine, in unidaw aber per Compiler-Schalter aus: Timestretch
(Rubberband, SoundTouch), ARA (Melodyne), JACK. MP3-Export braucht den
LAME-Encoder.

## Optionen

| Option | Aufwand | Risiko |
| --- | --- | --- |
| **Ausbauen** (Befehle für vorhandene Engine-Funktionen) | je S–M | gering |
| Plugins in eigenen Prozessen | L–XL | mittel (Latenz, Audio-Übergabe) |
| Ersetzen durch libardour | Monate | hoch: nicht als Bibliothek gedacht, alle Projekte migrieren |
| Eigenbau | Jahre | sehr hoch |
| REAPER/Live/Bitwig fernsteuern | Wochen | hoch: geschlossen, kein Song-Baum, kein headless |

Beim Ersetzen müssten rund 460 Engine-Aufrufe neu geschrieben und alle
`.tracktionedit`-Projekte migriert werden; die API-Suite wäre das
Sicherheitsnetz. Engine-Fehler am besten als Pull Request an Tracktion
schicken statt einen eigenen Fork zu pflegen.

## Lizenz

Tracktion Engine: GPLv3 oder kommerziell. JUCE 8: **AGPLv3** oder
kommerziell — die AGPL greift auch bei Nutzung über das Netz, und unidaw ist
ein HTTP-Daemon. Plan (Stand 2026-10-14): erst Eigennutzung, später
Vertrieb. Als Open Source unter GPL/AGPL geht das ohne Lizenzkosten;
Closed Source oder als Dienst braucht es kommerzielle Lizenzen beider
Hersteller. Die Engine-Wahl ändert das nicht — jede einbettbare Alternative
steht unter GPL.

## Umgesetzt am 2026-10-14

| Schritt | Commit | Todo |
| --- | --- | --- |
| Absturzschutz: Autosave, sauberes Beenden, `--session`, `Restart=on-failure` | `17c2325` | `todos/2026-10-14-absturzschutz-autosave-session-restore.md` |
| Automation über die Songzeit, Volume/Pan in `trackMix` | `fb5d58e` | `todos/2026-10-14-automation-parameter-und-routing.md` |
| Messlücken: Blockzeiten, Helm, Live-Lauf | `41e433a` | — (Ergebnisse oben, `bench/engine/`) |
| Tempo und Takt pro Section | `a8fb8cf` | `todos/2026-10-14-tempo-und-takt-pro-section.md` |
| Render-Optionen und Stems | `c5738b5` | `todos/2026-10-14-render-optionen-und-stems.md` |
| Metering und Lautheitsanalyse (`levels`, `analyze`) | `125795b` | `todos/2026-10-14-metering-und-lautheitsanalyse.md` |

Details zu jedem Feature: `ARCHITECTURE.md` (Absturzschutz, Tempo, Automation,
Routing), `docs/feature-longlist.md` (Stand je Punkt), die Todos (Agent Log).

## Offen

- Echte Plugin-Isolation — verschoben, bis fremde Plugins regelmäßig laufen
  oder der Vertrieb ansteht.
- ~~Batch-Befehl für Agenten~~ — erledigt 2026-10-14 (`batch`; der Graph
  baut erst nach dem ganzen Batch neu).
- Aus der Longlist: Freeze/Flatten, Vergleich mit einem Referenztrack,
  Bezier-Automation, ~~Tempo-Rampen~~ (erledigt 2026-10-25, `updateSection`
  ramp), Live-LUFS-Anzeige.
