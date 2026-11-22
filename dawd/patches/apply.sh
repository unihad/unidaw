#!/usr/bin/env bash
# Eigene Korrekturen an Engine/JUCE (Submodul), bis sie upstream sind.
# Idempotent: bereits angewendete Patches werden übersprungen. Läuft beim
# CMake-Configure und in tests/run.sh vor dem Bauen.
#
# Fassungen: Jede angewendete Fassung liegt als Kopie in
# <Repo>/.dawd-patches/<Patch> (im Arbeitsbaum des Submoduls bzw. der
# Engine-Kopie, unversioniert: eine .gitignore mit "*" darin; eine Kopie des
# Engine-Ordners nimmt den Vermerk mit). Weicht die Patch-Datei davon ab,
# nimmt apply.sh die alte Fassung zurück und wendet die neue an. Geht das
# nicht (alte Fassung passt nicht mehr rückwärts), bricht es mit Anleitung
# ab. Ohne Vermerk (erster Lauf, frische Kopie) gilt ein Patch, der sich
# rückwärts anwenden lässt, als drin und wird vermerkt.
# Engine-Kopie ohne .git (Worktree): git apply läuft dort ohne Repo
# (GIT_CEILING_DIRECTORIES), sonst fände es das umgebende Repo und übersprünge
# Patches mit "diff --git"-Kopf still ("Skipped patch").
# Test: tests/check_patches.sh.
#
#   juce-lv2-pending-parameters.patch — LV2: Parameter, die vor dem ersten
#     prepareToPlay() (oder zwischen zwei) gesetzt werden, gingen verloren
#     (Zustand wird aus den Ports gesichert, der Cache überschrieben).
#     Upstream: Bug-Report im JUCE-Forum (Audio Plugins, 2026-10-16, Konto
#     unihad, wartet auf Moderation) — "[BR] LV2 hosting: parameter values set
#     before prepareToPlay() are lost". PR braucht die JUCE-CLA.
#
#   juce-lv2-state-pending-parameters.patch — LV2: Der gespeicherte Zustand
#     (getStateInformation: Edit-Datei, Undo-Schnappschuss, Branch, Rezept)
#     liest die Control-Ports; gesetzte Werte kommen aber erst im nächsten
#     processBlock() vom Parameter-Cache in die Ports. Ohne Audio-Graph (kein
#     Gerät, --no-audio, nach releaseIdleGraph) oder vor dem nächsten Block
#     fehlte ein frischer setParameter-Wert: setParameter, undo, redo → alter
#     Wert (x42-eq, todos/2026-10-24-lv2-zustand-ohne-graph.md). Jetzt schreibt
#     getStateInformation die offenen Werte vorher in ihre Ports (dasselbe,
#     was der nächste Block täte; sie bleiben offen). Nur Port-Parameter —
#     Patch-Parameter (JUCE-LV2s wie testgain) erreichen das Plugin weiter erst
#     in run(). Baut auf juce-lv2-pending-parameters.patch auf (danach
#     anwenden). Upstream: noch offen.
#
#   juce-lv2-ui-messages-prepare.patch — LV2: Ein offener Editor legt seine
#     Änderungen in die Warteschlange der Instanz; erst processBlock() leert
#     sie. prepareToPlay() baut die Instanz neu (samt leerer Warteschlange) —
#     bei einer noch nie vorbereiteten Instanz gingen Editor-Änderungen
#     verloren. Jetzt wandern die wartenden Nachrichten in die neue Instanz.
#     Nur aus dem Code belegt (ohne Anzeige kein echtes LV2-Fenster im Test).
#     Baut auf juce-lv2-pending-parameters.patch auf (danach anwenden).
#     Upstream: noch offen.
#
#   juce-lv2-bypass-notify.patch — LV2: Den Port lv2:enabled zeigt JUCE als
#     Parameter "Bypass" (invertiert, BypassParameter). Liest JUCE die
#     Control-Ports nach einem Block zurück (oder kommt ein Wert vom Editor),
#     meldete es den Listenern aber den Port-Wert (enabled = 1) statt des
#     Parameter-Werts (Bypass = 0). Tracktion im Prozess liest getValue()
#     selbst und merkte nichts; der Plugin-Host der Sandbox reicht den
#     gemeldeten Wert an dawd weiter: nach dem ersten Render stand Bypass auf
#     On, das Schließen des Fensters schrieb ihn ins Plugin und in die
#     Edit-Datei — trocken (x42-eq, LSP Compressor;
#     todos/2026-10-31-lv2-bypass-im-host.md). Jetzt meldet es getValue().
#     Upstream: noch offen.
#
#   tracktion-linux-worker-priority.patch — Linux: Die Audio-Worker des Node-
#     Players wollen SCHED_RR 99, scheitern am RLIMIT_RTPRIO (95) und laufen
#     ohne Echtzeit → unter CPU-Last Aussetzer (~5 ms). Mit TRACKTION_RT_PRIORITY
#     bekommen sie SCHED_FIFO mit fester Priorität; dawd setzt 10 (unter dem
#     PipeWire-Daemon, 20 — darüber verdrängen spinnende Worker den Graphen).
#     Gemessen am Fireface UC 2026-10-17 (16 Lastprozesse): vorher 2 Aussetzer
#     in 75 s, mit FIFO 10 keiner in 100 s, mit FIFO 80 ein 37-ms-Stillstand.
#
#   tracktion-fourosc-wavetable-cache.patch — 4OSC: Die bandbegrenzten
#     Wellentabellen (je Samplerate) hingen nur an den Oszillatoren; mit dem
#     letzten 4OSC (jeder Node-Wechsel laedt einen neuen Edit) wurden sie frei
#     und beim naechsten Play/Render auf dem Message-Thread neu gerechnet. Jetzt
#     bleiben sie bis Prozessende. Gemessen 2026-10-19 (Agent-Projekt
#     nachbau-12, 6x 4OSC): erstes Play nach treeCheckout 1,1 s, davon ~0,7 s
#     Tabellen. Upstream: noch offen.
#
#   tracktion-sampler-layers.patch — Sampler: Velocity-Layer (minVelocity/
#     maxVelocity je Sound), Round-Robin- und Choke-Gruppen (offene Hat
#     verstummt bei geschlossener), Attack/Release je Sound. Neue SOUND-
#     Properties nur, wenn gesetzt — alte Edits laden unverändert. Für
#     setSamples/listSamples (todos/2026-10-24-sampler-ausbauen.md).
#     Upstream: noch offen.
#
#   tracktion-midi-pitchbend-reset.patch — MIDI-Clips: Beim Start eines Clips
#     (und bei jedem Sprung) schickt die Engine den letzten Wert jedes
#     Controllers vor der Position, beim Pitch-Bend aber nichts, wenn keiner
#     da ist — das Instrument behielt den Bend eines früheren Clips, Plays
#     oder Renders; eine gelöschte Bend-Kurve ließ den 4OSC verstimmt, bis der
#     Edit neu geladen wurde (todos/2026-10-24-midi-cc-und-export.md). Jetzt
#     kommt ohne Bend-Wert am Start die Mitte (8192). Nur der MidiNode-Weg
#     (Arranger-Clips mit proxyAllowed, Standard); LoopingMidiNode (Launcher)
#     und MPE unverändert. Entscheidung Matthias 2026-10-24 (M1).
#     Upstream: noch offen.
#
#   tracktion-fourosc-units.patch — 4OSC: Einheiten im Anzeigetext, damit
#     real die angezeigte Zahl ist (Decision 2026-10-24-einheiten-real,
#     Entscheidungen Matthias E1, E5): Fine Tune in "ct", Level (Master) in
#     "dB", Pan als "-50%" .. "50%" (L negativ) statt "50L"/"50R" — vorher
#     meldete listParameters für beide Seiten real 50. Nur der Text ändert
#     sich, gespeicherte Werte bleiben. Eigene Anpassung, nicht für upstream.
#
#   tracktion-flush-pending-values.patch — Manche Werte erreichen ein Plugin
#     erst in seinem nächsten Audio-Block: LV2-Patch-Parameter (JUCE-LV2s wie
#     testgain), die Komponente eines VST3, das sie nicht vom Controller nimmt
#     (Dragonfly), Nachrichten eines offenen LV2-Editors. Ohne laufenden
#     Graphen (kein Gerät, --no-audio, nach releaseIdleGraph) kommt der Block
#     nie: Edit-Datei, Undo-Schnappschuss und Branch hatten den alten Wert.
#     Wiederhergestellte LV2-Patch-Parameter kommen ebenso erst aus einem
#     Block in die Parameter (listParameters zeigte nach Undo/treeOpen den
#     alten Wert). Jetzt bekommt beim Sichern (flushPluginStateToValueTree)
#     und nach dem Wiederherstellen einer neuen Instanz nur das betroffene
#     Plugin 1 Sample Stille — nur ohne Graph; ist es nicht vorbereitet, wird
#     es einmal vorbereitet und bleibt es (wie nach releaseIdleGraph).
#     0 Samples reichten Dragonfly VST3 nicht (Entscheidungen Matthias L1x,
#     L-a, L-b; todos/2026-10-24-lv2-zustand-ohne-graph.md). Nachgestellt
#     2026-10-24/25 mit lv2_state_without_graph_flow (testgain LV2, Dragonfly
#     VST3: setParameter, undo, redo, treeOpen, Branch, Checkout ohne Render).
#
#   tracktion-fourosc-phase-seed.patch — 4OSC: Jeder Notenstart zieht die
#     Startphase der Oszillatoren aus einem juce::Random mit Zufalls-Seed
#     (MultiVoiceOscillator::start, utilities/tracktion_Oscillators.cpp) —
#     zwei Renders desselben Projekts waren sample-weise verschieden, Messwerte
#     streuten (Paket 13). Jetzt setzt FourOscPlugin::initialise() einen
#     festen Seed je Stimme und Oszillator: derselbe Render ist sample-gleich,
#     jede Note startet weiter mit eigener Phase. Ebenso fangen die
#     Noise-Welle (std::default_random_engine je Oszillator,
#     tracktion_Oscillators.h) und die Random-Welle der LFOs (juce::Random {1}
#     je Stimme, SimpleLFO in tracktion_FourOscPlugin.h) dort wieder bei ihrem
#     Standard-Seed an — vorher liefen sie über Renders weiter, jeder Render
#     klang anders (2026-10-25). Kommt nach tracktion-fourosc-units.patch.
#     Entscheidung Matthias 2026-10-25 (Q5-b). Upstream: noch offen.
#
#   juce-vst3-process-runloop.patch — VST3 unter Linux: Jedes Editor-Fenster
#     gab dem Plugin seine eigene IRunLoop, die mit dem Fenster starb. u-he
#     Diva merkt sich die Run-Loop des ersten Editors (meldet seinen Timer nie
#     ab) und ruft sie beim zweiten wieder auf: Absturz beim erneuten Öffnen
#     des Fensters (gdb 2026-10-25: Timer an Run-Loop 0x…efa88, kein
#     unregisterTimer, zweites Fenster an derselben Adresse, Absturz mit rdi =
#     alte Run-Loop). Jetzt eine Run-Loop für den ganzen Prozess, nie
#     zerstört. Surge XT und Dragonfly öffneten auch vorher mehrfach.
#     Upstream: noch offen.
#
#   tracktion-explicit-value-on-save.patch — Gesicherter Zustand (Edit-Datei,
#     Undo-Schnappschuss, Branch): Ein Parameter mit angehängtem Wert schreibt
#     seinen aktuellen (modulierten) Wert asynchron in den ValueTree; den
#     eingestellten Wert legt flushState nur dann extra ab ("parameters"), wenn
#     der aktuelle Wert davon abweicht. Direkt nach clearModifier ist der
#     aktuelle Wert schon wieder der eingestellte, der ValueTree hält aber noch
#     den modulierten — der Schnappschuss danach (Redo) lud ihn als neuen
#     Basiswert. Wie weit er daneben lag, hing davon ab, ob das Gerät vorher
#     den Fader an der Stopp-Position moduliert hatte: unter Last +5,2 dB statt
#     0 (lfo_flow rot, todos/2026-10-26-lfo-render-unter-last.md). Jetzt legt
#     flushState den eingestellten Wert auch ab, wenn der ValueTree abweicht.
#     Upstream: noch offen.
#
#   tracktion-midi-rescan-while-recording.patch — MIDI-Geräteliste: Die Engine
#     prüft alle 4 s die ALSA-MIDI-Ports. Kommt ein Port dazu oder fällt einer
#     weg (irgendein Programm auf dem Rechner, auch aseqdump), baut sie die
#     Geräte aller Playback-Kontexte neu (clearAllContextDevices → clearNodes
#     mit discardRecordings): eine laufende Aufnahme war verworfen, der
#     Transport stand, `record off` meldete "not recording"
#     (todos/2026-10-24-lane-active-wackelt.md). Ebenso der erste MIDI-Scan
#     einer Instanz während einer Audio-Aufnahme. Jetzt wartet die neue Liste,
#     bis kein Transport aufnimmt (Prüfung jede Sekunde). Nachgestellt
#     2026-10-26 mit aseqdump (Port kommt / geht) und monitor midi während
#     einer Audio-Aufnahme. Upstream: noch offen.
#
#   tracktion-rubberband-start-delay.patch — Echtzeit-Rubberband (Clips mit
#     autoTempo, gewarpt, Pitch): Nach jedem Neustart des Stretchers (Clip-
#     Anfang, Loop-Neustart, Sprung) verwarf die Hülle den Vorlauf
#     (getStartDelay, 512 Samples) nur blockweise (256) und gab im selben
#     Aufruf schon 256 Samples aus — Vorlauf statt Audio; die echten ersten
#     256 Samples fielen beim nächsten Aufruf als „Vorlauf“ weg. Ein Einsatz
#     genau am Clip- oder Loop-Anfang fehlte bis zu 5,8 ms (danach lag alles
#     richtig). Jetzt erst der ganze Vorlauf weg, dann Ausgabe. Nachgestellt
#     ohne Engine (Rubberband allein, Aufrufmuster der Hülle) und mit
#     stretch_edge_flow (2026-10-26, todos/2026-10-26-einsatz-am-clip-anfang-
#     gedaempft.md). Upstream: noch offen.
#
#   tracktion-channel-remap-shared-buffer.patch — Clips auf einer Spur mit
#     verschieden vielen Kanälen (Mono neben Stereo): die Engine hebt den
#     Mono-Clip mit einem ChannelRemappingNode auf Stereo; im CombiningNode
#     schreiben alle Nodes der Clip-Kette in denselben Puffer, der Remap-Node
#     las seinen Eingang aus seinem eigenen, nicht geleerten Ausgang und
#     addierte ihn auf sich — der Mono-Clip klang doppelt so laut (+6 dB, L
#     und R). Gefunden 2026-10-26 mit processClip (rechnet in Stereo; eine
#     Mono-Haelfte nach splitClip neben der gerechneten). Jetzt kopiert der
#     Node den Eingang zur Seite und beginnt mit Stille, wenn Ein- und Ausgang
#     sich den Speicher teilen. Upstream: noch offen.
#
#   tracktion-fourosc-unison-pan.patch — 4OSC: Mit Unison (mehr als eine
#     Stimme je Oszillator) rechnete MultiVoiceOscillator::process nur mit
#     Spread, Pan blieb ohne Wirkung — Pan 1 hart links und Pan 2 hart rechts
#     änderten weder Pegel L/R noch Korrelation (Praxistest 1m P7: Synth mit
#     4 Stimmen, Spread ±80–100 %). Jetzt wirkt Pan als Balance über der
#     Spread-Stellung wie bei einer Stimme; in der Mitte bleibt jeder Klang
#     sample-gleich (Faktor 1). Nachgestellt 2026-10-26 mit fourosc_pan_flow.
#     Upstream: noch offen.
#
#   juce-vst3-sync-controller.patch — VST3: setStateInformation gleicht den
#     Controller sofort mit dem neuen Zustand ab. Surge XT übernimmt einen
#     Zustand aber erst im nächsten Block (Patch-Laden im Audio-Thread) und
#     meldet die neuen Werte danach weder über den Controller noch über
#     Ausgangs-Parameter (Log 2026-10-29: nur restartComponent
#     kParamValuesChanged beim setState, danach kParamTitlesChanged aus
#     Surges eigenem Message-Thread, nie ein Ausgangs-Parameter). Die
#     Parameter zeigten bis zum nächsten setState die Werte des vorigen
#     Zustands; ein setParameter auf den alten Wert kam nie an. Die neuen
#     Titel kamen asynchron (triggerAsyncUpdate) erst nach der laufenden Op.
#     Jetzt gibt es AudioPluginExtensions::VST3Client::
#     syncControllerWithComponent(): Zustand der Komponente an
#     setComponentState des Controllers, dann Titel und Werte neu lesen —
#     dawd ruft es, nachdem das Plugin einen Block bekam
#     (todos/2026-10-27-surge-werte-nach-loadpreset.md). Upstream: noch offen.
#
#   tracktion-refresh-parameter-values.patch — ExternalPlugin::
#     refreshParameterValues() öffentlich: die Parameter der Engine
#     übernehmen neue Werte des Plugins sonst erst in der nächsten Runde der
#     Message-Loop (AsyncUpdater je Parameter). Gebraucht nach
#     syncControllerWithComponent, damit dieselbe Op die Werte sieht.
#     Upstream: noch offen.
#
#   tracktion-warp-realtime.patch — Warp in Echtzeit mit einem Stretcher:
#     Die WarpMap der Engine lief durch einen eigenen Stretcher vor dem
#     Tempo-Stretcher mit einem Tempo je Block und traf die Beats nur auf
#     ±20 ms (120 BPM) bzw. ~90 ms (60 BPM); vor dem ersten Marker lag die
#     Quellzeit falsch (gespiegelt). Jetzt bildet der TimeRangeReader die
#     Warp-Zeit jedes Blocks auf die Quellzeit ab (Lage bleibt am Raster),
#     der eine TimeStretchReader setzt sein Verhältnis je eingespeistem
#     Stück aus der Map an der Stelle, die Rubberband gerade rechnet (384
#     Samples hinter der Einspeisung) — ±4,5 ms gemessen im Render (44,1 und
#     48 kHz, 0,7x–1,45x). Dazu EngineBehaviour::getWarpPointsForClip: Punkte,
#     die die Anwendung selbst führt (dawd: DAWDWARP), ohne den
#     WarpTimeManager. Ersetzt den Offline-Render von warpClip
#     (todos/2026-10-31-warp-echtzeit.md). Upstream: noch offen.
#
#   tracktion-edit-file-writer-stop.patch — Speichern (EditFileOperations,
#     in dawd nach jeder Mutation): Der Schreib-Thread für die Temp-Datei ist
#     eine geteilte Ressource und endet mit dem letzten EditFileOperations.
#     Sein Destruktor weckte ihn über flushAllFiles() und setzte erst danach
#     das Exit-Flag; stopThread() weckt nur das eigene Event des Threads,
#     nicht dessen waiter. Kam der Thread unter Last dazwischen dran, schlief
#     er erneut waiter.wait (1000): das Speichern hielt den Message-Thread
#     1,0 s (warpClip in warp_realtime_flow neben der Suite, gemessen
#     2026-11-20: saveCurrentEdit 1003 ms, Rest der Op < 10 ms;
#     todos/2026-11-19-warp-realtime-flow-marker-verschieben-unter-last.md).
#     Jetzt erst das Exit-Flag, dann waiter wecken. Upstream: noch offen.
#
#   tracktion-save-replace-file.patch — Speichern (EditFileOperations::save):
#     Die Engine schreibt erst .tmp_<Edit> und legte die Datei dann per
#     File::moveFileTo auf die Edit-Datei. JUCE löscht dabei zuerst das Ziel
#     und benennt danach um: für einen Moment gab es die Node-Datei nicht
#     (listen_flow unter Last: "No such file" direkt nach renderNode, ein
#     Autosave lief; ein Absturz in der Lücke liess nur .tmp_node-N liegen).
#     Jetzt File::replaceFileIn — ein rename() über die alte Datei, wie
#     XmlElement::writeTo (todos/2026-11-20-node-datei-beim-speichern-atomar-
#     ersetzen.md). Upstream: noch offen.
#
#   tracktion-midi-rescan-while-playing.patch — Neubau der Geräte eines
#     Playback-Kontexts (releaseDeviceList: MIDI-Port kommt oder geht, auch
#     der zweite MIDI-Scan einer frischen Instanz; Wechsel des Audio-Geräts)
#     ruft clearNodes, und das stoppt den Playhead. Der Transport-Timer sah
#     dann "Transport spielt, Playhead nicht" und beendete die Wiedergabe:
#     listen_flow unter Last sah nach play + Solo playing false bei Position 0
#     (todos/2026-11-20-solo-kurz-nach-play-stoppt-die-wiedergabe.md). Jetzt
#     läuft der Playhead weiter, wenn er lief und keine Aufnahme läuft (die
#     verwirft clearNodes; während einer Aufnahme wartet die MIDI-Liste ohnehin,
#     tracktion-midi-rescan-while-recording.patch); den Graphen baut der
#     folgende Neustart. Upstream: noch offen.
set -e
# Aus einem Hook in einem verknüpften Worktree erbt das Skript GIT_DIR/
# GIT_INDEX_FILE; dann prüfte `git -C <juce> apply` gegen das falsche Repo.
unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE
root="$(cd "$(dirname "$0")/../.." && pwd)"
git_apply () {   # git_apply <Repo> <Argumente für git apply …>
    # Nicht über den Repo-Ordner hinaus suchen: Ohne eigenes .git fände git
    # sonst das umgebende Repo (Worktree), Pfade aus "diff --git"-Patches
    # gälten relativ zu dessen Wurzel, lägen außerhalb des Ordners und würden
    # still übersprungen — beide Prüfungen gelängen immer.
    GIT_CEILING_DIRECTORIES="$(dirname "$1")" git -C "$1" apply "${@:2}"
}
apply () {   # apply <Repo-Pfad relativ zu root> <Patch>
    local repo="$root/$1" patch="$root/dawd/patches/$2"
    local state="$repo/.dawd-patches/$2"           # angewendete Fassung
    if [ -f "$state" ] && cmp -s "$state" "$patch" &&
       git_apply "$repo" --reverse --check "$patch" 2>/dev/null; then
        return 0                                   # schon drin, Fassung gleich
    fi
    if [ -f "$state" ] && ! cmp -s "$state" "$patch"; then   # neue Fassung
        if git_apply "$repo" --reverse --check "$state" 2>/dev/null; then
            git_apply "$repo" --reverse "$state"
            rm "$state"
            echo "dawd/patches: $2 alte Fassung zurückgenommen"
        elif ! git_apply "$repo" --reverse --check "$patch" 2>/dev/null; then
            echo "dawd/patches: $2 hat eine neue Fassung; die alte ($1/.dawd-patches/$2)" >&2
            echo "  lässt sich nicht zurücknehmen (spätere Patches darüber, Datei von Hand geändert?)." >&2
            echo "  Von Hand: $1 zurücksetzen — Submodul: git -C $1 checkout -- . && rm -r $1/.dawd-patches;" >&2
            echo "  Engine-Kopie ohne .git: neu kopieren —, dann apply.sh neu." >&2
            exit 1
        fi
    fi
    if git_apply "$repo" --reverse --check "$patch" 2>/dev/null; then
        record "$repo" "$patch" "$state"
        echo "dawd/patches: $2 schon drin, Fassung vermerkt"
    elif git_apply "$repo" --check "$patch" 2>/dev/null; then
        git_apply "$repo" "$patch"
        record "$repo" "$patch" "$state"
        echo "dawd/patches: $2 angewendet"
    else
        echo "dawd/patches: $2 passt nicht mehr (Submodul aktualisiert?) — prüfen, ob upstream behoben" >&2
        exit 1
    fi
}
record () {   # record <Repo> <Patch> <Vermerk>: angewendete Fassung vermerken
    mkdir -p "$1/.dawd-patches"
    [ -f "$1/.dawd-patches/.gitignore" ] || echo '*' > "$1/.dawd-patches/.gitignore"
    cp "$2" "$3"
}
# tests/check_patches.sh lädt nur die Funktionen
[ "${BASH_SOURCE[0]}" != "$0" ] && return 0
apply tracktion_engine/modules/juce juce-lv2-pending-parameters.patch
apply tracktion_engine/modules/juce juce-lv2-state-pending-parameters.patch
apply tracktion_engine/modules/juce juce-lv2-ui-messages-prepare.patch
apply tracktion_engine/modules/juce juce-lv2-bypass-notify.patch
apply tracktion_engine/modules/juce juce-vst3-process-runloop.patch
apply tracktion_engine tracktion-linux-worker-priority.patch
apply tracktion_engine tracktion-fourosc-wavetable-cache.patch
apply tracktion_engine tracktion-sampler-layers.patch
apply tracktion_engine tracktion-midi-pitchbend-reset.patch
apply tracktion_engine tracktion-fourosc-units.patch
apply tracktion_engine tracktion-flush-pending-values.patch
apply tracktion_engine tracktion-fourosc-phase-seed.patch
apply tracktion_engine tracktion-explicit-value-on-save.patch
apply tracktion_engine tracktion-midi-rescan-while-recording.patch
apply tracktion_engine tracktion-rubberband-start-delay.patch
apply tracktion_engine tracktion-channel-remap-shared-buffer.patch
apply tracktion_engine/modules/juce juce-vst3-sync-controller.patch
apply tracktion_engine tracktion-fourosc-unison-pan.patch
apply tracktion_engine tracktion-refresh-parameter-values.patch
apply tracktion_engine tracktion-warp-realtime.patch
apply tracktion_engine tracktion-edit-file-writer-stop.patch
apply tracktion_engine tracktion-save-replace-file.patch
apply tracktion_engine tracktion-midi-rescan-while-playing.patch
