# Plugin-Sandboxing — Konzept

Stand 2026-10-25. Todo: `todos/2026-10-25-plugin-sandboxing.md`.
Decision: `docs/decisions/2026-10-25-plugin-sandboxing.md` — gilt, Option C
(Sandbox jetzt). Umsetzung in Phasen:
`todos/2026-10-25-plugin-sandbox-umsetzung.md`. **Stand Phase 4:**
Abschnitt 0 unten (Phase 4, dann 3, 2 und 1); Aufbau im Detail
`ARCHITECTURE.md`, Plugin-Sandbox.

Frage: Wie überlebt unidaw den Absturz eines Fremd-Plugins (VST3, LV2)?
Heute hostet die Tracktion Engine alle Plugins im Prozess von unidaw; ein
Absturz im Plugin beendet den Daemon.

Anlass: Am 2026-10-25 riss u-he Diva (VST3) dawd-7400 dreimal mit
(Segfault in `Diva.so` beim erneuten Öffnen des Editors; Diva hielt die
IRunLoop des ersten Editors). Behoben per JUCE-Patch
`dawd/patches/juce-vst3-process-runloop.patch` (0529b9f). systemd startete
unidaw neu, die Session kam zurück, der Song lief aber nicht weiter.

Belege: Engine/JUCE aus dem Submodul in main (`tracktion_engine` auf
e42d82f, Pfade unten relativ dazu), unidaw-Code im Branch, Messungen in
`bench/sandbox/`. Was ohne Beleg ist, steht als **Schätzung** oder
**offen** da.

## 0 Stand der Umsetzung: Phase 4 (2026-10-26)

Branch `sandbox-phase4`. LV2 läuft wie VST3 im Host-Prozess; Standard an,
`UNIDAW_PLUGIN_SANDBOX=0` schaltet ab.

Was dazukam:

- **LV2 im Host:** Audio, MIDI, Parameter, Zustand, Programme (LV2-Presets),
  Editor-Fenster (LV2-UI im Host), Rückweg, Absturz-Erholung, Sperrliste,
  Gruppierung (`plugin`, `single`, `dawd` = im Prozess) — dieselben Wege
  wie VST3. Die JUCE-Patches `juce-lv2-*` wirken im Host (dieselbe Binary).
- **LV2-Klassen im Host:** der Host lädt einmal je Prozess alle LV2-Klassen
  (`searchPathsForPlugins`, wie das Scan-Kind nach dem Merge-Befund). Ohne
  das sagt die Beschreibung der Instanz Kategorie „Plugin“,
  `isInstrument` 0 (geprüft mit einer Ausgabe im Host, MDA JX10: ohne
  „Plugin“/0, mit „Instrument Plugin“/1); die Engine übernimmt sie
  (`ExternalPlugin::initialise`) und `isSynth` wäre falsch. Kosten: siehe
  `addPlugin` unten.
- **Veraltete Meldungen des Hosts (Speicher Version 4, `outBase`):** LV2
  meldet bei `prepare` und `setState` alle Werte aus den Ports. Kam das, bevor
  der Host einen offenen unidaw-Wert übernahm, und holte unidaw es danach ab,
  sprang der Wert zurück (x42-eq: `setParameter Gain 6` → wieder 0 dB;
  `lv2_state_without_graph_flow` rot). Jede Meldung trägt jetzt den
  unidaw-Wert, den der Host da schon hatte; ist unidaw weiter, fällt sie weg.
- **Werte, die nur ein Block bringt** (Patch-Parameter, Werte ohne Graph,
  Patch `tracktion-flush-pending-values`): der 1-Sample-Block von
  `passPendingValuesToPlugin` kommt vom Message-Thread. Er wartet jetzt wie
  ein schwerer Aufruf (bis 10 s statt eines Echtzeit-Blocks, mindestens
  5 ms) und übernimmt danach sofort, was das Plugin darin meldete — im
  Prozess geht das im Block selbst an die Listener.
- **Editor-Warteschlange:** ein offener LV2-Editor im Host stellt Änderungen
  für den nächsten Block an. Die Engine zählt nur ihre eigenen Editoren
  (`numOpenEditors`); für das Fenster im Host setzt unidaw vor jeder Op und
  alle 100 ms `markValuesPending` (`markLv2EditorPending`) — das Sichern
  ohne Graph gibt dann 1 Sample wie im Prozess. Nur LV2 (VST3 meldet
  Editor-Werte sofort über den Rückweg).
- **Neustart:** nach `setState` und `prepare` bekommt ein LV2 im neuen Host
  1 Sample, bevor der Audio-Thread ihn nimmt — wiederhergestellte
  Patch-Parameter kommen erst aus einem Block (vorher zeigte unidaw nach dem
  Neustart Gain 1,0 statt 0,25, der Klang stimmte).
- **Bypass (2026-11-05):** JUCE zeigt den LV2-Port `lv2:enabled` als
  Parameter „Bypass“ (invertiert), meldete den Listenern aber den Port-Wert.
  Der Host reicht Meldungen weiter: nach dem ersten Render stand Bypass auf
  On, das Schließen des Fensters schrieb ihn zurück — trocken (x42-eq, LSP
  Compressor). Tracktion im Prozess liest `getValue()` selbst und war nicht
  betroffen. Patch `juce-lv2-bypass-notify` (`dawd/patches/apply.sh`);
  `plugin_sandbox_phase4_flow` prüft es (Todo
  `todos/2026-10-31-lv2-bypass-im-host.md`).

Belege (Port 8021, ohne Audio-Gerät: `bwrap --tmpfs /dev/snd`,
`PIPEWIRE_REMOTE` ungültig — dawd-7400 spielte live):

- Neu `plugin_sandbox_phase4_flow`: zwei testgain LV2 in einem Host, Render
  sample-gleich (32-bit float) wie im Prozess, Text wie im Prozess,
  Patch-Parameter ohne Graph (Gain 0,25 halbiert), Undo; MDA JX10 in eigenem
  Host, Klasse instrument, erster Render nach dem Laden sample-gleich (JX10
  klingt in jedem weiteren Render anders, auch im Prozess: Render 2 und 3
  weichen von Render 1 ab, Render k mit und ohne Sandbox gleich); SIGKILL →
  beide neu im selben neuen Host, Gain 0,25 samt Text, Render gleich, Undo;
  `single` = zwei Hosts, `dawd` = im Prozess, je Render gleich; x42-eq
  (`--window-sim`): Regler im Host kommt zurück, Schließen bucht
  `pluginWindow`, Undo/Redo, Edit-Datei; Hosts enden mit unidaw.
- `plugin_window_real_flow` mit LSP Compressor Stereo LV2: echte LV2-UI auf
  Xvfb, mit Sandbox im Host (`_NET_WM_PID` = Host), Lagen wie im Prozess.
- Die LV2-Tests `lv2_state_without_graph_flow`,
  `lv2_params_before_render_flow`, `find_presets_flow`,
  `sound_catalog_flow` prüfen jetzt, wo das Plugin lief (`_check_hosted`):
  mit Standard im Host, mit `UNIDAW_PLUGIN_SANDBOX=0` im Prozess.
- Teil-Suite (26 Tests, Liste im Todo) mit Standard und mit
  `UNIDAW_PLUGIN_SANDBOX=0`: je 26/26.

Messung mit echten Plugins (`bench/sandbox/dawd_plugins_phase4.py`,
Rohdaten `bench/sandbox/results-2026-10-26-phase4.jsonl`): Ryzen 7 4800H,
nur Render (Blöcke zu 512, `ops/Processing.cpp`), Song 30 s = 2583 Blöcke,
Median aus 3; zwei Läufe bei loadavg ~2, frühere Läufe unter Last durch
parallele Builds stehen mit in den Rohdaten. „mix“: N × (Surge XT VST3 →
x42-eq LV2, Dexed VST3 → Dragonfly Room LV2, MDA JX10 LV2 → Dragonfly Room
VST3); „chain“: eine Spur, Surge XT → x42-eq → Dragonfly Room LV2 →
Dragonfly Room VST3 → x42-eq.

| Projekt | Modus | Hosts | Laden | Render Wand / CPU | PSS (unidaw) |
| --- | --- | --- | --- | --- | --- |
| chain, 5 Plugins | im Prozess | 0 | 0,17 s | 1,97 s / 2,7 s | 69 MB |
| chain | je Plugin | 4 | 0,42 s | 2,15 s / 3,2 s | 103 MB (28) |
| chain | je Modul | 5 | 0,40 s | 2,16 s / 3,2 s | 110 MB (29) |
| mix, N = 2 (12 Plugins) | im Prozess | 0 | 0,31 s | 2,09 s / 6,4 s | 111 MB |
| mix, N = 2 | je Plugin | 6 | 0,88 s | 1,96 s / 6,9 s | 177 MB (49) |
| mix, N = 2 | je Modul | 12 | 1,01 s | 1,79 s / 6,4 s | 221 MB (50) |
| mix, N = 4 (24 Plugins) | im Prozess | 0 | 0,60 s | 3,67 s / 13,4 s | 180 MB |
| mix, N = 4 | je Plugin | 6 | 1,37 s | 3,01 s / 14,9 s | 276 MB (83) |
| mix, N = 4 | je Modul | 24 | 1,90 s | 2,66 s / 13,9 s | 403 MB (85) |

- **Aufschlag je Plugin und Block (Latenz im Block):** die Kette rechnet der
  Graph nacheinander; mit Sandbox 0,18 s länger = 70 µs je Block für 5
  Rundläufe, also ~14 µs je Plugin und Block (512 Frames, Render). Keine
  zusätzliche Block-Latenz (synchron im Block, Phase 1); die Renders sind
  sample-gleich (Tests oben), die Latenz der Plugins also gleich
  ausgeglichen.
- **CPU (Render):** „je Plugin“: Kette +19 %, mix +8 % (N = 2) und +11 %
  (N = 4); „je Modul“: Kette +18 %, mix +0,5 % und +3 %. Wand: mix mit Sandbox schneller (−6 bis
  −18 %; wie Phase 1, Ursache nicht untersucht).
- **Laden:** `addPlugin` des ersten Moduls eines Plugins (startet den Host):
  LV2 0,053–0,064 s statt 0,017–0,022 s (MDA JX10, x42-eq, Dragonfly LV2:
  +0,036–0,047 s), VST3 0,043–0,225 s statt 0,017–0,157 s (+0,026–0,097 s)
  — das Laden der LV2-Klassen fällt nicht auf. `treeOpen` mit Sandbox
  +0,57 s (mix N = 2, 6 Hosts) und +0,76 s (N = 4).
- **Speicher:** „je Plugin“ +66 MB (N = 2) und +96 MB (N = 4) PSS gegen im
  Prozess (6 Hosts, ~21 MB je Host bei N = 2); „je Modul“ +110 und +224 MB.
  unidaw selbst wird kleiner (Bibliotheken im Host).
- **Live: nicht gemessen** — dawd-7400 spielte über JACK auf dem Gerät.
  Handaufgabe (Todo): `python3 bench/sandbox/dawd_plugins_phase4.py --live
  10` bei freiem Gerät (am Master testgain LV2 mit Gain 0, still).

Befunde zu den offenen Punkten aus Phase 3 (kein Umbau):

- **Hänger im Render kostet 30 s:** `roundTrip` wartet im Render in
  50-ms-Scheiben bis 30 s und prüft dabei nur, ob der Host beendet ist
  (`pidfd`). Ein angehaltener oder verklemmter Host fällt erst nach 30 s auf,
  dann Render-Fehler (Entscheidung Matthias: Frist so lassen). Möglich ohne
  kürzere Frist: in den Scheiben prüfen, ob der Host angehalten ist
  (`/proc/<pid>/stat`, Zustand `T`) oder seit einigen Sekunden keine CPU-Zeit
  mehr verbraucht — das fängt SIGSTOP und Verklemmungen, nicht
  Endlosschleifen; ein Plugin, das legitim auf Platte oder einen
  eigenen Thread wartet, sähe aber auch „ohne CPU“ aus. Wie lang legitime
  Render-Blöcke echter Plugins höchstens dauern: **nicht gemessen**.
- **Leichte Aufrufe teilen den Message-Thread des Hosts:** die Aufrufer der
  Steuer-Aufrufe in unidaw laufen auf dem Message-Thread (Ops über
  `executeOp`, Timer des Stellvertreters, `flushPluginStateToValueTree` mit
  `TRACKTION_ASSERT_MESSAGE_THREAD`), also nacheinander; einen Aufrufer auf
  einem anderen Thread fand ich nicht (nicht vollständig geprüft). Im Host
  konkurrieren sie darum vor allem mit der Oberfläche (offene Editoren) und
  mit einer leichten Anfrage, die schon in ihre 2-s-Frist lief. Gemessen
  (Xvfb mit Mini-WM, `listParameters` des zweiten Moduls, Editor des ersten
  offen/zu): Dexed in einem Host 75 → 86 ms, in zwei Hosts 56 → 72 ms; Surge
  XT 0,60 → 0,61 s bzw. 0,57 → 0,62 s (loadavg ~5,8) — kein messbarer Nachteil durch das
  Teilen. Teurer ist der Rundlauf selbst: je Parameter-Text ~0,4 ms
  (`listParameters` Surge XT, 776 Parameter: 0,22 s im Prozess, 0,50 s im
  Host; Dexed 12 → 79 ms; x42-eq 2 → 15 ms; loadavg ~6). Abhilfe wäre eine
  Op für viele Texte auf einmal — nicht gebaut.

## 0a Stand der Umsetzung: Phase 3 (2026-10-26)

Branch `sandbox-phase3`. **Standard an** (eigener, letzter Commit des
Branches, getrennt mergebar); `UNIDAW_PLUGIN_SANDBOX=0` schaltet ab.

Was dazukam:

- **Gruppierung:** ein Host-Prozess hält mehrere Instanzen; unidaw schickt
  je Instanz einen memfd über einen Socket (SCM_RIGHTS). Standard „je
  Plugin“ (alle Instanzen desselben Plugins in einem Prozess, wie Bitwig
  „By Plug-in“), je Plugin einstellbar: `single` (je Modul ein Prozess)
  und `dawd` (im Prozess). Op `pluginHosting {plugin, mode?}`, Datei
  `<config>/plugin-hosting.json`, gilt beim nächsten Laden des Plugins.
  „Zusammen“ (alle Plugins in einem Prozess) und „je Hersteller“ bietet unidaw
  nicht an (Decision nennt nur Standard + die zwei Ausnahmen).
- **Absturz erkennen und erholen:** der Stellvertreter sieht den toten Host
  über `pidfd` (Timer 50 ms und vor jeder Op), schließt ein offenes Fenster
  des Moduls in unidaw (ohne Aktion) und lädt das Plugin in einem neuen Host:
  gleiche Beschreibung, Bus-Layout, letzter gelesener Zustand, `prepare`.
  Erst dann liest der Audio-Thread den neuen Kanal; Werte, die unidaw setzte,
  während der Host weg war, gehen hinterher. Fenster gehen nicht von selbst
  wieder auf (Diva stürzte beim Öffnen). Was im Editor seit dem letzten
  gelesenen Zustand geändert wurde, ist verloren (Abschnitt 2).
- **Sperrliste:** stürzt ein Plugin 2-mal in 60 s ab (je Plugin gezählt,
  ein Prozess-Tod = ein Absturz), lädt unidaw es nicht mehr von selbst neu:
  `state: blocked`, stumm, ein Render liefert den Fehler wie bei einem
  toten Host. `reloadPlugin {plugin}` hebt die Sperre auf und lädt neu. Neue
  Instanzen (Projekt öffnen, Undo, Branch) laden auch bei Sperre — das ist
  ein Schritt des Nutzers, keine Schleife.
- **Hängender Host:** leichte Steuer-Aufrufe (Parameter-Texte, `info` aus
  dem Timer, `hasEditor`, Fenster verschieben) geben nach 2 s auf und
  liefern einen Ersatz (Text = Zahl); der Host darf nachreichen. Schwere
  (Zustand, `prepare`, Fenster auf/zu, Programm) warten bis 10 s, dann wird
  der Host beendet — ihr Ergebnis muss stimmen. Ein Wächter (Timer)
  beendet den Host, wenn eine Antwort 10 s offen ist oder ein
  Echtzeit-Block 5 s im Host hängt (vorher: Stille bis zum nächsten Edit).
  Render: 30 s je Block wie bisher, dann beendet. Beendet = Absturz → Neustart
  bzw. Sperrliste.
- **Status:** `routing.plugins[].sandbox` {pid, missedBlocks, failed, group,
  state running|restarting|blocked|failed, crashes, restarts, message};
  `message` ist der Hinweis auf Englisch. `position.sandboxSeq` zählt bei
  jeder Änderung — das UI lädt dann neu (Zeichen im Ketten-Slot, Zeile mit
  „Reload“ in der Parameter-Karte, Toast).
- **Plugin-Scan im Kindprozess** (`unidaw --plugin-scan`, alle Formate, auch
  LV2): stirbt das Kind, gilt die Datei in Arbeit als kaputt, ein neues Kind
  macht weiter; kaputte Dateien in `listPlugins` `scanErrors`.

Belege (Port 8021; Skripte für die Handmessungen im Scratchpad, nicht im
Repo):

- Suite `plugin_sandbox_phase3_flow`: zwei TestGain in einem Host; SIGKILL →
  beide neu im selben neuen Host, Gain 0,25 aus dem Zustand, Render
  sample-gleich, Undo; Dexed-Fenster nach SIGKILL zu, wieder auf im neuen
  Host; SIGSEGV in 60 s → `blocked`, Render-Fehler, `pluginWindow`-Fehler,
  `reloadPlugin` → Render sample-gleich; SIGSTOP → `listParameters` 2,0 s
  (statt bis 10 s), Host nach ~10 s beendet und neu; `single` = zwei Hosts,
  `dawd` = im Prozess, je Render sample-gleich; Scan mit einem VST3, das
  beim Laden SIGSEGV auslöst: `scanErrors` „crashed while scanning: signal
  11 (Segmentation fault)“, unidaw lebt.
- Neuladen eines TestGain-Hosts (neuer Prozess, Laden, Zustand, prepare):
  31–35 ms (5 Läufe, Dauer der Op `reloadPlugin`).
- Echtzeit über das Gerät (TestGain, Gain 0,5): Host per SIGSTOP angehalten
  → nach 5,1 s beendet und neu geladen, 437 verpasste Blöcke, Pegel danach
  wieder −14,7 dBFS wie vorher.
- Scan (frische Config, 601 Plugins): im Kind 6,7 s (mit dem abstürzenden
  Bündel und einem zweiten Kind), im Prozess 4,8 s, gleiche Liste; unidaw
  danach 44 MB statt 188 MB RSS. Dasselbe Bündel im Prozess-Scan: unidaw endet
  mit SIGSEGV.

## 0b Stand der Umsetzung: Phase 2 (2026-10-26)

Branch `sandbox-phase2`. Schalter weiter `UNIDAW_PLUGIN_SANDBOX=1`, Standard
aus; ohne Schalter unverändert.

Was dazukam:

- **Rückweg Host → unidaw:** Werte, die das Plugin selbst meldet (Editor,
  VST3-Ausgangs-Parameter im Block), Gesten und neue Namen/Programme
  kommen über den Speicher zurück und gehen in unidaw wie im Prozess an die
  Engine (Automation, Undo, Zustand). Abgeholt vor jeder Op, sonst alle
  50 ms (Timer). Ein von unidaw gesetzter, vom Host noch nicht übernommener
  Wert gewinnt.
- **Parameter-Namen dynamischer Plugins:** nach `parameterInfoChanged`
  holt der Stellvertreter Namen, Einheiten, Werte und Programme neu.
- **Editor-Fenster:** öffnet der Host-Prozess als eigenes Top-Level-Fenster
  mit derselben Lage-Logik wie unidaw (`NativeEditorWindow`); Lage nach
  `windows.json`, Schließen über die Titelleiste als Op `pluginWindow`.
- **`changeProgramName`, Bus-Layout** über den Steuerkanal.
- Ein Fehler aus Phase 1 behoben, eine Falle beim Bau gefunden:
  (1) Steuer- und RT-Thread des Hosts
  konnten die erste Anfrage verlieren (Start-Zähler erst im Thread
  gelesen) — `load` hing dann 60 s, der Host war tot; gesehen beim
  Auto-Branch mit Dexed in 2 von 7 Läufen, nach dem Fix 10 von 10 ohne
  Hänger.
  (2) `hasEditor` legt in JUCE eine VST3-View an; LSP beendet ohne
  Display dabei den Prozess (`[ERR] Can not open display`) — ein erster
  Stand fragte es beim Laden und verlor so jeden LSP-Host. Jetzt fragt der
  Stellvertreter erst, wenn ein Fenster aufgehen soll (wie unidaw im
  Prozess: nach der Prüfung auf ein Display).

Belege (Port 8021, `tests/run_tests.py`; Skripte für die Fenster-Messung
liefen im Scratchpad, nicht im Repo):

- `plugin_sandbox_phase2_flow` (eigene Instanz mit Sandbox und
  `--window-sim`): Regler im Editor des Hosts (Dexed, Cutoff 0,25) ist
  sofort in `listParameters`, bucht beim Schließen `pluginWindow` mit
  Auto-Branch, Undo/Redo, Edit-Datei, Rezept in anderem Projekt gleich.
  Surge XT Effects: Namen und Texte nach Render und nach FX-Typ „Reverb 1“
  gleich wie ohne Sandbox. LSP Sidechain Compressor: Sidechain-Bus in der
  Edit-Datei aus/wieder an — Render-Hüllkurve und `sidechainInput` gleich
  wie ohne Sandbox.
- Echtes Fenster (Desktop :0, GNOME/XWayland, Dexed, 2026-10-26):
  Rahmen und Inhalt laut `xwininfo` an derselben Stelle wie ohne Sandbox —
  angefragt 60,80: Rahmen 42,55, Inhalt 67,117; verschoben auf 240,160:
  Rahmen 215,135, Inhalt 240,197; dreimal zu/auf ohne Wandern. Schließen
  über die Titelleiste (WM_DELETE_WINDOW) → `window: false`, Fenster weg,
  Lage in `windows.json`. Offen über Auto-Branch und Undo (jeweils neuer
  Host-Prozess, gleiche Lage). Diva und Surge XT: je dreimal auf/zu im
  selben Host, zweimal über die Titelleiste, kein Ausfall.
- **Dexed 1 LSB (aus Phase 1) kommt nicht von der Sandbox:** nach
  `treeOpen` weichen die ersten zwei Renders auch im Prozess ab (1 LSB bei
  16 bit, ab Sample 0 oder ab der zweiten Note), ab dem dritten Render
  sind alle sample-gleich — im Prozess und in der Sandbox, auch
  gegeneinander (32-bit float; je 2 Sitzungen × 6 Renders je Modus).
  Die Vermutung aus Phase 1 (Message-Thread des Hosts) ist damit
  widerlegt; die Ursache in Dexed ist nicht untersucht. Der Test wärmt
  jetzt zwei Renders auf und vergleicht exakt.

Nicht über die API prüfbar (Handprüfung, Todo): Regler im echten Editor
drehen und Schließen, Gesten (die Engine nutzt sie für die Aufnahme von
Automation aus Reglern, die unidaw nicht hat), `changeProgramName` (kein
Aufrufer in unidaw).

## 0c Stand der Umsetzung: Phase 1 (2026-10-25)

Branch `sandbox-phase1`. Schalter `UNIDAW_PLUGIN_SANDBOX=1`, Standard aus.

Was geht:

- VST3-Plugins laufen je Instanz in einem Host-Prozess (`unidaw
  --plugin-host`), eingehängt über `PluginManager::createPluginInstance`
  ohne Engine-Patch. Audio, MIDI, Parameter, Playhead synchron im selben
  Block; Latenz des Plugins wie bisher an die Engine.
- Echtzeit (Audio-Graph über das Gerät) und Offline-Render.
- Zustand (`getStateInformation`/`setStateInformation`), Programme
  (Liste, Wechsel), Parameter-Texte über den Steuerkanal — damit tragen
  Edit-Datei, `treeOpen`, Undo, Presets wie im Prozess.
- Ohne Schalter: Engine-Fabrik unverändert, `routing.plugins[]` ohne Feld
  `sandbox`.

Abweichung vom Konzept (Abschnitt 2): Der Steuerkanal läuft nicht über die
JUCE-Pipe (`ChildProcessCoordinator`), sondern im selben Shared Memory mit
Futex. Grund: ein Mechanismus statt zwei, synchrone Aufrufe aus jedem
unidaw-Thread ohne Umweg über den Message-Thread von unidaw. Einen toten Host
meldet `pidfd` (poll) sofort — das leistete im Konzept der Pipe-Bruch.

Belege (Suite `plugin_sandbox_flow`, Messung
`bench/sandbox/dawd_plugins.py`, Rohdaten
`bench/sandbox/results-2026-10-25-dawd-plugins.jsonl`):

- TestGain-VST3: Render ohne/mit Sandbox sample-gleich (32-bit float),
  auch nach `treeOpen` (Zustand), Parameter-Änderung und Undo.
- Dexed (Instrument, 2 Noten, 2 s): 5 Renders mit Sandbox, 2 wichen um
  1 LSB bei 16 bit ab (3,05e-5); im Prozess 11 von 11 gleich. Ursache nicht
  untersucht — Vermutung: Im Host läuft der Message-Thread des Plugins
  während des Renders weiter, in unidaw steht er (Render auf dem
  Message-Thread). Der Test erlaubt 1 LSB. **Überholt durch Phase 2:**
  auch im Prozess weichen die ersten zwei Renders nach dem Laden ab
  (Abschnitt 0b).
- Echte Plugins, Ryzen 7 4800H, ALSA 512 Frames/44,1 kHz, Song 30 s, je
  N × Dragonfly Room Reverb (Audio-Spur) + N × Surge XT (MIDI-Spur), ein
  zweiter Agent baute parallel; Render Median aus 3, live 10 s:

| N | Modus | Laden (`treeOpen`) | Render Wand / CPU | live CPU (100 % = 1 Kern) | PSS |
| --- | --- | --- | --- | --- | --- |
| 1 | im Prozess | 0,10 s | 1,18 s / 2,1 s | 26 % | 184 MB |
| 1 | Sandbox, 2 Hosts | 0,25 s | 1,21 s / 2,3 s | 29 % (unidaw 19 %) | 210 MB |
| 4 | im Prozess | 0,37–0,38 s | 2,50–2,62 s / 7,2–7,3 s | 36–43 % | 254–256 MB |
| 4 | Sandbox, 8 Hosts | 0,99–1,00 s | 1,73–1,82 s / 7,2–7,6 s | 55–66 % (unidaw 22 %) | 357–359 MB |
| 8 | im Prozess | 0,73 s | 2,99 s / 15,3 s | 82 % | 363 MB |
| 8 | Sandbox, 16 Hosts | 2,14 s | 2,73 s / 16,7 s | 98 % (unidaw 28 %) | 562 MB |

  (N = 4 zweimal gemessen, daher Spannen.)
  - Laden: + 0,07–0,09 s je Host-Prozess.
  - RAM: + 12–13 MB PSS je Host-Prozess (Schätzung im Konzept: 25–35 MB).
  - Render: nicht langsamer — Wandzeit gleich (N = 1) oder kürzer
    (N = 4: −30 %, N = 8: −9 %), CPU-Zeit −1 bis +9 %. Die Schätzung „bis
    ~30–40 % langsamer“ (Abschnitt 3) trifft mit diesen Plugins nicht zu.
    Warum es mit Sandbox schneller geht: nicht untersucht.
  - Live: keine verpasste Frist in allen Läufen; CPU +3 bis +30
    Prozentpunkte eines Kerns, stark schwankend (N = 4: 55 und 66 %).
- Toter Host (SIGKILL): unidaw lebt, Render meldet nach 0,56 s einen Fehler
  statt einer Datei, live Stille; ein neuer Edit (z. B. `treeBranch`)
  startet neue Hosts.

Offen nach Phase 4: `ARCHITECTURE.md`, Plugin-Sandbox, „Fehlt nach
Phase 4“, und das Todo.

## Kurzfassung

- Die Engine und JUCE bieten **kein Out-of-Process-Hosting** für den
  Audio-Betrieb. Es gibt nur den Plugin-**Scan** im Kindprozess (Tracktion)
  und die allgemeinen IPC-Klassen von JUCE (Pipe, Ping). unidaw nutzt nicht
  einmal den Scan im Kindprozess.
- Es gibt aber einen sauberen **Einhängepunkt ohne Engine-Patch**:
  `PluginManager::createPluginInstance` ist eine austauschbare Funktion.
  unidaw kann dort eine Stellvertreter-Instanz liefern, die über
  Shared Memory mit einem Host-Prozess spricht.
- Die Messung zeigt: Ein Block kann **im selben Block** in einen anderen
  Prozess und zurück — ohne zusätzliche Latenz. Aufschlag je Plugin und
  Block: Median 40–60 µs, Maximum ~0,1 ms mit Echtzeit-Priorität.
- Die Wege (a) Prozess je Instanz, (b) je Hersteller/Format, (c) ein
  gemeinsamer Host teilen sich dieselbe Infrastruktur (XL). Die Gruppierung
  ist danach nur eine Einstellung (S) — Bitwig und REAPER bieten mehrere.
- Weg (d) — Absturz-Erholung ohne Sandbox — ist M und behebt den Schmerz
  vom 2026-10-25 (Song stand, Fenster weg, Gefahr einer Absturzschleife).
- **Empfehlung:** jetzt (d) bauen, dazu den Plugin-Scan in einen
  Kindprozess. Die Sandbox-Infrastruktur erst, wenn Abstürze trotz (d)
  stören oder der Vertrieb ansteht; dann mit Gruppierung „je Plugin“ als
  Standard.

## 1 Was Tracktion Engine und JUCE heute bieten

| Baustein | Was er kann | Beleg |
| --- | --- | --- |
| Tracktion `PluginScanHelpers` | Plugin-Scan im Kindprozess: der Host startet die eigene Binary mit `--PluginScan:<pipe>`, schickt je Datei eine SCAN-Nachricht (XML), wartet auf FOUND; stirbt der Kindprozess, ein zweiter Versuch mit frischem Prozess | `modules/tracktion_engine/plugins/tracktion_PluginScanHelpers.h:23` (`PluginScanMasterProcess : juce::ChildProcessCoordinator`), `:175` (`PluginScanChildProcess`), `:297–330` (Ablauf, zweite Chance) |
| … nur für VST/LADSPA | LV2 wird immer im Prozess gescannt | `tracktion_PluginScanHelpers.h:344–354` (`shouldUseSeparateProcessToScan`: Name enthält „VST“ oder „LADSPA“) |
| … nur, wenn die App es freischaltet | `EngineBehaviour::canScanPluginsOutOfProcess()` ist `false`; `main` muss `PluginManager::startChildProcessPluginScan` aufrufen | `modules/tracktion_engine/utilities/tracktion_EngineBehaviour.h:70–85`, `plugins/tracktion_PluginManager.cpp:412–417, 483–495` |
| unidaw heute | scannt selbst im Message-Thread (`format->findAllTypesForFile`), kein Kindprozess; `DawdEngineBehaviour` überschreibt `canScanPluginsOutOfProcess` nicht | `dawd/source/ops/Processing.cpp:1764–1830`, `dawd/source/Dawd.cpp:89–129` |
| JUCE `ChildProcessCoordinator`/`ChildProcessWorker` | Kindprozess starten, Nachrichten (MemoryBlock) über eine benannte Pipe, Ping jede Sekunde, Verbindung gilt nach Timeout (Standard **8 s**) als verloren; ein Lesefehler der Pipe meldet den Verlust sofort | `modules/juce/modules/juce_events/interprocess/juce_ConnectedChildProcess.h:59, 152`, `.cpp:43` (`defaultTimeoutMs = 8000`), `.cpp:56–97` (Ping-Thread), `juce_InterprocessConnection.cpp:389–395, 405–424` (`connectionLostInt`) |
| JUCE `XEmbedComponent` | fremdes X11-Fenster in ein JUCE-Fenster einbetten (Linux) | `modules/juce/modules/juce_gui_extra/embedding/juce_XEmbedComponent.h:142` |
| Tracktion `PluginManager::createPluginInstance` | austauschbare Fabrik für `AudioPluginInstance` („can be set to provide custom behaviour“); VST3 und LV2 laufen auf Linux über sie (synchron) | `plugins/tracktion_PluginManager.h:96–103`, `.cpp:38–42`; Aufruf `plugins/external/tracktion_ExternalPlugin.cpp:1941`; VST3/LV2 verlangen keine asynchrone Erzeugung: `juce_audio_processors_headless/format_types/juce_VST3PluginFormatHeadless.cpp:111–114`, `juce_LV2PluginFormatHeadless.cpp:89–92`, Weiche `tracktion_ExternalPlugin.cpp:2237–2247` |
| Tracktion `CrashStackTracer` | merkt sich je Thread, in welchem Plugin der Code gerade steckt (`CRASH_TRACER_PLUGIN`, z. B. um `processBlock`, `setStateInformation`) | `utilities/tracktion_CrashTracer.h:16–43`, `.cpp:101–125`; `tracktion_ExternalPlugin.cpp:724, 768, 1124, 1172` |
| Shared Memory für Audio | **nicht vorhanden** — keine Klasse in Engine oder JUCE für Audio zwischen Prozessen (`grep shm_open\|memfd` ohne Treffer außer `MemoryMappedFile`) | grep über `modules/` |

Fazit: Out-of-Process-Hosting müsste unidaw selbst bauen. Der Einhängepunkt
dafür (`createPluginInstance`) ist da, ohne Engine-Patch.

## 2 Architektur einer Sandbox (gemeinsam für a, b, c)

```
unidaw (Engine, Graph)                     Plugin-Host-Prozess (unidaw --plugin-host)
 ExternalPlugin                           echte AudioPluginInstance (VST3/LV2,
  └─ RemotePluginInstance  ──RT-Kanal──>   gleiche JUCE-Patches wie unidaw)
     (AudioPluginInstance)  Shared Memory   Editor-Fenster (eigenes Top-Level-
                            + Futex          Fenster, X11/XWayland)
                         ──Steuerkanal──>  Zustand, Parameter-Texte, Programme
                            JUCE-Pipe        Fenster auf/zu/Lage
```

- **Stellvertreter:** `RemotePluginInstance` (eine `juce::AudioPluginInstance`)
  kommt aus `createPluginInstance`. Für die Engine bleibt alles ein
  `te::ExternalPlugin` mit derselben `PluginDescription` (Format „VST3“
  bleibt „VST3“). unidaw-Code, der `getAudioPluginInstance()` nutzt
  (`PluginWindows.cpp`, `Presets.cpp`, `Sidechain.cpp`), sieht den
  Stellvertreter.
- **Host-Prozess = unidaw-Binary im Worker-Modus** (wie der Tracktion-Scan:
  eigene Binary, `tracktion_PluginScanHelpers.h:35`). So gelten alle
  JUCE-Patches aus `dawd/patches/` auch dort, und die Zustands-Blobs sind
  byteweise gleich. Das ist wichtig: Edit-Dateien, Undo-Schnappschüsse,
  Rezepte (`.dawdpreset`) und die Dexed-Cartridge-Logik lesen den
  JUCE-VST3-Blob (`Presets.cpp:251–262`) — sie bleiben kompatibel.
- **RT-Kanal je Instanz:** Slot im Shared Memory (`memfd`/`shm_open`) mit
  Audio-Puffern (alle Busse, auch der Sidechain-Bus), MIDI-Ereignissen
  hin und zurück (MIDI-Effekte erzeugen MIDI), Parameter-Änderungen des
  Blocks (Automation), Playhead (Tempo, Position, Play-Status) und
  `isNonRealtime`. Synchronisation je Block über zwei Futex-Wörter: Host
  setzt „rechne Block n“, wartet mit Frist auf „fertig n“. Gemessen in
  `bench/sandbox/` (Abschnitt 4).
- **Steuerkanal:** JUCE `ChildProcessCoordinator` (Pipe) für alles außerhalb
  des Audio-Threads: Laden, `prepareToPlay`, Zustand holen/setzen,
  Parameter-Liste und -Texte (`getText`), Programme (`Presets.cpp:398–402,
  488–491`), Latenz, Bus-Layout (`Sidechain.cpp:67–70`), Fenster.
  Pipe-Bruch meldet einen toten Host sofort (s. o.), der 8-s-Ping ist nur
  Rückfall.
- **Eingebaute Plugins** (4OSC, Sampler, Limiter, dawds Feedback- und
  Sidechain-Paare) bleiben im Prozess — eigener Code, keine Fremd-Gefahr.

## 3 Die Wege im Vergleich

| | (a) Prozess je Instanz | (b) je Hersteller/Format | (c) ein Host für alle | (d) nur Erholung, keine Sandbox |
| --- | --- | --- | --- | --- |
| Absturz trifft | nur diese Instanz | alle Instanzen der Gruppe | alle Fremd-Plugins | ganz unidaw (Neustart) |
| Session läuft weiter | ja | ja | ja, alle Fremd-Plugins stumm bis Neustart des Hosts | nein, Neustart ~1 s + Laden |
| Zusätzliche Latenz | keine (synchron im Block) | keine | keine | keine |
| Aufschlag je Plugin und Block | gemessen 40–60 µs Median, ~0,1 ms Max (512 Frames, FIFO) | gleich | gleich, ein Thread je Instanz im Host nötig (sonst reihen sich parallele Graph-Pfade) | 0 |
| CPU (Schätzung) | 512 Fr.: 10 Plugins ≈ 4 % eines Kerns; 64 Fr./48 kHz ≈ 15 % | gleich | gleich | 0 |
| RAM (Schätzung) | + ~25–35 MB je Instanz (Prozess-Basis + Plugin-Bibliothek je Prozess) | + je Gruppe | + ~25 MB einmal | 0 |
| Offline-Render | synchron ohne Frist; Aufschlag je Block bremst bei vielen Plugins (Schätzung bis ~30–40 %) | gleich | gleich | unverändert |
| Editor-Fenster | gehört dem Host-Prozess | gleich | gleich | wie heute |
| Absturz im Editor (Diva-Fall) | Instanz weg, Audio der anderen läuft | Gruppe weg | alle Fremd-Plugins weg | unidaw weg |
| Hängendes Plugin | Frist verpasst → Stille in diesem Block, unidaw bleibt bedienbar | gleich | gleich | unidaw hängt |
| Aufwand | XL (Infrastruktur) | XL + S | XL − etwas (ein Prozess, einfachere Verwaltung) | M |
| Hauptrisiko | Prozesszahl, RAM bei großen Projekten | Gruppierung trifft nicht immer (geteilte Bibliotheken anderer Hersteller) | ein Absturz lädt alle Plugins neu (Sekunden) | Absturz bleibt Absturz; Zuordnung zum Plugin nicht immer eindeutig |

Die Gruppierung (a/b/c) ist nach der Infrastruktur nur ein Schlüssel:
Instanz → Host-Prozess. Darum kann unidaw sie wie Bitwig und REAPER als
Einstellung anbieten, Standard plus Ausnahmen je Plugin.

### Audio- und MIDI-Transport

- **Synchron im selben Block** (empfohlen): Der Graph-Thread schreibt den
  Block, weckt den Host, wartet mit Frist. Keine zusätzliche Latenz, kein
  Eingriff in die Latenzkompensation. Messung: Abschnitt 4.
- **Versetzt um einen Block** (Alternative): Der Host rechnet Block n,
  während unidaw Block n−1 ausgibt; der Stellvertreter meldet einen Block
  Latenz über `getLatencySamples`, die Engine gleicht sie aus
  (`tracktion_ExternalPlugin.cpp:84–90`). Kostet 11,6 ms (512) bzw. 1,3 ms
  (64) beim Live-Spielen. Nach der Messung nicht nötig.
- **Frist:** Live = Restbudget des Blocks (Schätzung: Hälfte des Budgets,
  damit der Rest des Graphen fertig wird). Verpasst → Ausgang still, im
  Status zählen. Render = keine harte Frist (z. B. 2 s); stirbt der Host
  im Render, bricht der Render mit Fehler ab statt still eine falsche
  Datei zu schreiben.
- Tracktion rechnet den Graphen auf mehreren Threads
  (`ThreadPoolStrategy::lightweightSemHybrid`,
  `modules/tracktion_engine/playback/tracktion_EditPlaybackContext.cpp:19`);
  der wartende Graph-Thread schläft im Futex, die Arbeit macht der Host.

### Parameter und Automation

- Automation bleibt in unidaw (`AutomatableParameter`); die Werte reisen mit
  dem Block in den Host.
- Änderungen im Editor (im Host) kommen über den Steuerkanal zurück
  (`setValueNotifyingHost` am Stellvertreter) — so funktioniert der
  Vergleich beim Schließen (`PluginWindows.cpp`, `openedState`, Decision
  2026-10-24-plugin-fenster-als-aktion) weiter.
- Risiko: Plugins mit dynamischen Parametern (Surge XT Effects benennt
  „FX Parameter n“ erst nach dem ersten Audio-Block um,
  `docs/engine-bewertung.md`) — der Stellvertreter muss Namensänderungen
  nachziehen.

### Zustand (Undo, Edit-Datei, Rezepte)

- `getStateInformation` ist ein Aufruf über den Steuerkanal. Undo macht vor
  und nach jeder Op einen Schnappschuss mit `flushState`
  (`dawd/source/Undo.cpp:109`) — das ist dann je Fremd-Plugin ein Rundlauf
  plus das Serialisieren im Host (das gibt es heute auch). Schätzung: unter
  1 ms je Plugin außer bei großen Zuständen.
- Der Stellvertreter merkt sich den letzten gelesenen Zustand. Stirbt der
  Host, gilt dieser als Stand für die Wiederherstellung; Änderungen im
  Editor seit dem letzten Lesen gehen verloren.
- dawds LV2-/Tracktion-Patches zum „offenen Wert ohne Audio-Block“
  (`docs/engine-bewertung.md`) wirken im Host bzw. am `ExternalPlugin` —
  sie bleiben nötig und müssen im Sandbox-Betrieb mitgetestet werden.

### Editor-Fenster über die Prozessgrenze

- unidaw zeigt Editoren in eigenen Top-Level-Fenstern (`EditorWindow`,
  `dawd/source/PluginWindows.cpp:137–188`), nicht eingebettet in eine
  unidaw-Oberfläche. Darum braucht es **kein XEmbed**: Der Host-Prozess
  öffnet das Fenster selbst (Titel, Lage aus `windows.json` per
  Steuerkanal), meldet Verschieben und Schließen zurück.
- Die Engine erwartet ein `juce::Component` als Fenster
  (`HeadlessUIBehaviour::createPluginWindow`). Dass ein Component ohne
  Desktop-Fenster reicht, zeigt `--window-sim` schon heute (`SimWindow`,
  `PluginWindows.cpp:190–215`). Ein `RemoteWindow` nach diesem Muster
  steht in unidaw für das Fenster im Host. Die Op `pluginWindow` und die
  Funktionen in `PluginWindows.h` bleiben (so vorgesehen in
  `PluginWindows.h:17–19` und `ARCHITECTURE.md`, Plugin-Fenster).
- Das Schließen über die Titelleiste ruft heute `executeOp` mit
  `pluginWindow {open: false}` (`PluginWindows.cpp:165–181`); im Host wird
  daraus eine Nachricht an unidaw, die denselben Weg nimmt.
- Wenn ein eingebettetes Fenster doch gewünscht ist (z. B. App-Hülle,
  Paket 4): `juce::XEmbedComponent` ist da (Abschnitt 1), offen, ob es mit
  den Plugins unter XWayland trägt.

### Absturz erkennen und wiederherstellen

1. Erkennen: Pipe-Bruch im Steuerkanal (sofort) oder verpasste Frist im
   RT-Kanal + `waitpid`/`pidfd`. Messung: toter Worker nach einer Frist
   erkannt (11,7 ms), die anderen liefen weiter (`crash-512`).
2. Modul stumm schalten (Stellvertreter gibt Stille), Status
   `crashed` in `routing.plugins[]`, Eintrag im Event-Log, Meldung in der
   Flow-UI.
3. Neu laden: Host neu starten, Instanz mit dem letzten Zustand anlegen.
   Fenster **nicht** automatisch öffnen (Diva stürzte gerade beim Öffnen).
   Nach zwei Abstürzen in kurzer Zeit bleibt das Modul aus, bis eine
   Op `reloadPlugin` (und ein Knopf in der UI) es wieder holt — wie Bitwigs
   „Reload Plug-in“ (Abschnitt 5).

### Auswirkungen auf Ops und Tests

- Ops bleiben gleich; neu: Status `crashed`/`sandbox` je Modul in
  `routingState`, Op `reloadPlugin`, Einstellung für die Gruppierung
  (je Config). Alles mit UI (AGENTS.md: API-only gilt als unfertig).
- Suite: Test-Plugin, das auf Befehl abstürzt (TestGain-VST3 in
  `plugins/` um einen Absturz-Parameter erweitern); Tests: Absturz → unidaw
  lebt, Modul still im Render, `reloadPlugin` holt den Zustand zurück,
  Undo/Rezept/Edit-Datei gleich wie ohne Sandbox. Die Suite öffnet keine
  Fenster (`--window-sim`) — der Host-Prozess braucht denselben Schalter.
- Die vielen Analyse-Ops rendern (analyze, compare*, renderStems, Freeze).
  Der Aufschlag je Block trifft unidaw darum stärker als eine DAW, die vor
  allem live spielt — beim Bau messen (`bench/engine` mit Fremd-Plugins).
  Rechnung zur Schätzung „bis ~30–40 %“: 16 Module rendern mit einem
  Thread 15× Echtzeit (`docs/engine-bewertung.md`) = 0,77 ms je Block;
  10 Fremd-Plugins × 31 µs (Messung `render-512`) = 0,31 ms dazu. Mit
  echten, rechenstarken Plugins ist der Anteil kleiner.

### Weg (d): Absturz-Erholung ohne Sandbox

Heute: Autosave ~0,4 s nach jeder Mutation, Neustart über systemd
(`Restart=on-failure`, `RestartSec=1`), `--session` öffnet das Projekt
wieder (`ARCHITECTURE.md`, Absturzschutz; `dawd/source/main.cpp:109–127`).
Gemessen (2026-10-25, Binary aus main, `--no-audio`): Start bis `ping`
0,05 s, `treeOpen` mit 3× Diva + 3× Surge XT 0,52 s. Der Neustart selbst
ist also schnell. Es fehlt:

1. **Absturz dem Plugin zuordnen:** Signal-Handler (SIGSEGV, SIGBUS,
   SIGILL, SIGFPE, SIGABRT) schreibt mit `write()` eine kleine
   `crash.json` ins Run-Verzeichnis und löst das Signal erneut aus. Inhalt:
   Fehleradresse und, wenn sie in einer Plugin-Bibliothek liegt, deren
   Name (Adressbereiche beim Laden der Plugins vorab gesammelt — so bleibt
   der Handler signal-sicher), dazu `CrashStackTracer::getCrashedPlugin`
   für den Thread. Der Diva-Absturz lag im Editor-Timer im Message-Thread,
   wo kein `CRASH_TRACER_PLUGIN` greift — die Adresse ist der verlässlichere
   Weg. Offen: Abstürze in libc, die ein Plugin auslöst (Adresse zeigt dann
   auf libc).
2. **Sperrliste:** Beim Start liest unidaw `crash.json`. Lag der Absturz im
   Editor eines Plugins, öffnet unidaw dessen Fenster nicht wieder; lag er
   im Audio/Laden und kam zweimal, lädt unidaw das Modul **gesperrt** (Modul
   bleibt im Edit samt Zustand, Plugin nicht instanziiert, stumm). Meldung
   in `state`/UI, Op zum Entsperren. Verhindert die Absturzschleife.
3. **Arbeitsplatz zurück:** Session-Datei merkt sich offene Fenster,
   Play-Position und ob gespielt wurde; nach dem Neustart steht die
   Position wieder da, Fenster (außer gesperrte) gehen auf. Ob unidaw
   von selbst weiterspielt: Frage an Matthias (Decision-Vorschlag).
4. **Scan in einen Kindprozess:** Der Scan lädt jedes Plugin
   (`Processing.cpp:1805`); ein abstürzendes Plugin beim Scan beendet
   unidaw. Tracktion bringt den Kindprozess-Scan mit (Abschnitt 1, LV2 nicht
   abgedeckt). Nebeneffekt, gemessen: Nach dem Scan im Prozess wuchs unidaw
   von 25 MB auf 161 MB RSS — die gescannten Bibliotheken bleiben geladen.

## 4 Messung (bench/sandbox)

`bench/sandbox/shmbench.cpp`: Host und Worker-Prozesse, ein Slot je Worker
im Shared Memory, Futex-Wecken, Frist = Budget des Blocks. Gemessen wird
der Aufschlag (Rundlauf minus Rechenzeit im Worker). Ryzen 7 4800H,
Kernel 7.0, SCHED_FIFO (erlaubt bis 95), zwei andere Agents bauten
parallel. Rohdaten: `bench/sandbox/results-2026-10-25-ryzen7-4800h.csv`.

| Szenario | Budget | Aufschlag je Plugin Median / p99,9 / Max | Summe je Block Max | Anteil am Budget (Max) |
| --- | --- | --- | --- | --- |
| im Prozess (Vergleich) | 11,6 ms | 2,9 µs / 17 µs / 19 µs | 19 µs | 0,2 % |
| 1 Plugin, 512 Fr. | 11,6 ms | 55 µs / 104 µs / 107 µs | 107 µs | 0,9 % |
| 1 Plugin, ohne FIFO | 11,6 ms | 72 µs / 484 µs / 982 µs | 982 µs | 8 % |
| 1 Plugin, Worker rechnet 2 ms | 11,6 ms | 64 µs / 490 µs / 518 µs | 518 µs | 4 % |
| Kette aus 8, 512 Fr. | 11,6 ms | 45 µs / 437 µs / 567 µs | 1034 µs | 9 % |
| 8 parallel (Thread je Plugin) | 11,6 ms | 63 µs / 479 µs / 603 µs | 603 µs | 5 % |
| 1 Plugin, 64 Fr. / 48 kHz | 1,33 ms | 39 µs / 61 µs / 105 µs | 105 µs | 8 % |
| Kette aus 8, 64 Fr. / 48 kHz | 1,33 ms | 15 µs / 44 µs / 49 µs | 297 µs | 22 % |
| Render ohne Takt, 512 Fr. | — | 31 µs / 82 µs / 363 µs | — | — |

- Keine verpasste Frist in allen Läufen.
- 50 µs aktives Warten vor dem Futex half nicht verlässlich (Kette 64:
  Max 748 µs statt 297 µs) — nicht nötig.
- Absturz (`crash-512`): Worker 0 per SIGKILL, erkannt nach 11,7 ms (eine
  Frist), die drei anderen liefen 301 Blöcke ohne Ausfall weiter.
- Grenzen: Gain statt echter Plugins, kein MIDI/Playhead/Parameter im Slot,
  Fenster nicht geprüft. Das Ergebnis beantwortet nur die Kernfrage: Der
  synchrone Weg passt ins Budget; bei 64 Frames und langen Ketten wird er
  spürbar (Schätzung: ab ~20 Fremd-Plugins in Reihe wird 64 Frames knapp).

Speicher (gemessen, Binary aus main, `--no-audio`, frische Config):
unidaw ohne Projekt 23 MB RSS (10 MB PSS); nach dem Scan 161 MB; je
Diva-Instanz +63 MB (erste +72 MB), je Surge-XT-Instanz +24 MB. Die
Plugin-Kosten zahlt man in jedem Weg; (a) zahlt die Prozess-Basis und die
geteilten Teile der Plugin-Bibliothek je Instanz einmal mehr — daher die
Schätzung + ~25–35 MB je Instanz.

## 5 Was andere Hosts machen (nur Belegtes)

- **Bitwig Studio:** Hosting-Modi „Within Bitwig“, „Together“ (alle
  Plugins zusammen, getrennt von der Audio-Engine), „By Manufacturer“, „By
  Plug-in“ (Instanzen desselben Plugins zusammen) und „Individually“ (je
  Instanz ein Prozess; als Ausnahme je Plugin wählbar). Nach einem Absturz
  ersetzt eine Meldung die Oberfläche im Device Panel, mit „Reload
  Plug-in“ und „Reload All Plug-ins“. Links mehr Sicherheit, rechts
  weniger RAM. Quelle: Bitwig User Guide, „Plug-in Handling and Options“,
  <https://bitwig.com/userguide/latest/vst_plug-in_handling_and_options>
  (abgerufen 2026-10-25); <https://bitwig.com/support/technical_support/what-is-plug-in-crash-protection-26>.
  Latenz und CPU-Kosten nennt Bitwig dort nicht — **nicht belegt**.
- **REAPER:** je Plugin „Run as“: native (Absturz beendet REAPER),
  „separate process“ (alle so markierten Plugins in **einem** Prozess —
  einer stürzt, alle stürzen, die Session bleibt) und „dedicated process“
  (je Plugin ein Prozess, eigener Speicher). Laut Admin des REAPER-Blogs
  keine zusätzliche Latenz für 64-Bit-Plugins im eigenen Prozess; Bridge-
  Fenster erscheinen als eigene Fenster in der Taskleiste. Quelle:
  <https://reaper.blog/2012/02/run-plugin-as-dedicated-process/> (Blog,
  nicht das Handbuch; Stand 2012).
- **Carla** (installiert: Paket `carla` 2.5.10): Option „Run plugins in
  bridge mode when possible“ (`ENGINE_OPTION_PREFER_PLUGIN_BRIDGES`,
  Standard aus, „EXPERIMENTAL“) und UI-Bridges (Standard an) —
  `/usr/share/carla/carla_backend.py:931–938`, `carla_shared.py:292–293`.
  Die Bridge-Binaries (`/usr/lib/carla/carla-bridge-native`, `-lv2-*`)
  nutzen laut Symbolen POSIX Shared Memory für einen Audio-Pool und je
  einen RT- und Nicht-RT-Kanal (`/crlbrdg_shm_ap_`, `_rtC_`, `_nonrtC_`,
  `_nonrtS_`, `BridgeRtClientControl::waitForClient`), dazu Ping und
  Meldungen „bridge crashed“ / „Timeout while waiting for a response from
  plugin-bridge“ (`strings` auf `libcarla_standalone2.so`). Dass der
  RT-Kanal im selben Block wartet, ist aus den Namen abgeleitet, nicht
  aus dem Quelltext — **nicht belegt**.

## 6 Aufwand und Risiken

Maßstab wie `docs/feature-longlist.md`: S < 1 Tag, M 1–3 Tage, L ≈ 1 Woche,
XL > 2 Wochen; Schätzwerte ±50 %.

| Teil | Aufwand | Begründung |
| --- | --- | --- |
| (d) Zuordnung + Sperrliste + Arbeitsplatz zurück + Test | M | Signal-Handler, `crash.json`, Lade-Sperre je Modul, Session-Felder, UI-Meldung, Test mit abstürzendem TestGain |
| Scan im Kindprozess | S–M | Tracktion bringt ihn mit; dawds eigener Scan-Cache und LV2 (nicht abgedeckt) brauchen Anpassung |
| Sandbox-Infrastruktur (a/b/c) | XL | Worker-Modus (S–M), RT-Kanal mit MIDI/Playhead/Parametern/Bussen (M), Stellvertreter-Instanz mit Parametern, Texten, Programmen, Latenz, Zustand (L), Fenster im Host (M), Absturz/Neuladen/Status/UI (M), Tests + Messung mit echten Plugins (M) |
| Gruppierung (a/b/c) als Einstellung | S | nur der Schlüssel Instanz → Prozess, wenn die Infrastruktur steht |

Abweichung: Die Longlist schätzte M/L, `docs/engine-bewertung.md` L–XL.
Diese Aufstellung kommt auf XL, weil Fenster, Zustand/Undo und das Neuladen
mit eingerechnet sind.

Risiken der Sandbox:

- Plugins, die mehr vom Host wollen als `AudioPluginInstance` abbildet
  (VST3-Kontextmenüs, Host-Erweiterungen) — offen, je Plugin prüfen.
- Echtzeit-Priorität des Host-Prozesses: ohne SCHED_FIFO 10× schlechteres
  Maximum (Messung). unidaw muss sie für die Host-Prozesse setzen.
- Mehr Prozesse = mehr RAM; die OOM-Lage auf diesem Rechner ist bekannt
  (parallele Suiten).
- Doppelte Testfläche (mit und ohne Sandbox) für alle Fremd-Plugin-Tests.

Risiken von (d): Ein Absturz bleibt ein Absturz (kurze Stille, Neustart);
die Zuordnung zum Plugin ist nicht immer eindeutig; im Vertrieb reicht das
laut Paket 5 (`docs/vertrieb-und-verkauf.md`) nicht.

## 7 Empfehlung

1. **Jetzt (d) + Scan im Kindprozess** (M + S–M). Das trifft den Anlass:
   Diva stürzte im Editor; mit (d) öffnet unidaw nach dem Neustart den
   Editor nicht wieder, stellt Position und Fenster her und gerät nicht in
   eine Schleife. Kein Eingriff in Audio-Pfad, Undo, Render.
2. **Sandbox-Infrastruktur (XL) später**, wenn eines eintritt: Abstürze
   trotz (d) kosten Arbeit, mehrere unsichere Fremd-Plugins kommen dazu,
   oder der Vertrieb startet (Paket 5). Dann synchron im Block (keine
   Latenz), Host = unidaw-Binary, Gruppierung als Einstellung mit Standard
   „je Plugin“ (Absturz trifft nur Instanzen desselben Plugins, RAM
   moderat), Ausnahmen „einzeln“ und „in unidaw“ je Plugin.
3. Vor dem Bau der Sandbox: `bench/sandbox` um MIDI/Playhead erweitern und
   mit Diva/Surge im Host messen (Render-Tempo und 64 Frames).
