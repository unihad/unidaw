#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <optional>

namespace te = tracktion;

// Plugin-Sandbox, Phase 1–4 (docs/plugin-sandboxing.md, Decision
// docs/decisions/2026-10-25-plugin-sandboxing.md): VST3- und LV2-Plugins
// (LV2 seit Phase 4) laufen in einem eigenen Host-Prozess (dawd --plugin-host). In dawd steht ein
// Stellvertreter (RemotePluginInstance) — die Engine sieht ein normales
// ExternalPlugin. Audio, MIDI, Parameter und Playhead gehen je Block
// synchron ueber Shared Memory (memfd + Futex) in den Host und im selben
// Block zurueck; Laden, prepareToPlay, Zustand, Programme, Parameter-Texte,
// Bus-Layout und das Editor-Fenster ueber einen Steuerkanal im selben
// Speicher. Was das Plugin selbst aendert (Editor, Modulation, neue Namen),
// kommt ueber einen Rueckweg im Speicher nach dawd (Phase 2).
// Phase 3: Gruppierung (Standard "je Plugin": alle Instanzen eines Plugins in
// einem Host-Prozess; je Plugin "single" = je Instanz einer, "dawd" = im
// Prozess), Neustart mit dem letzten Zustand nach Absturz oder Haenger,
// Sperrliste gegen Absturz-Schleifen, Plugin-Scan im Kindprozess.
//
// Schalter: Standard an (seit Phase 3). Umgebungsvariable
// UNIDAW_PLUGIN_SANDBOX=0 beim Start schaltet ab — dann bleibt dawd wie vor der
// Sandbox (die Engine-Fabrik wird nicht ersetzt, Scan im Prozess).
namespace dawd
{
    // Sandbox an? (Standard; aus mit UNIDAW_PLUGIN_SANDBOX=0)
    bool pluginSandboxRequested();

    // Ersetzt PluginManager::createPluginInstance: VST3 und LV2 ueber den
    // Host-Prozess, alles andere (eingebaute) wie bisher im Prozess
    void installPluginSandbox (te::Engine&);

    // Einstellung der Gruppierung je Plugin (<config>/plugin-hosting.json):
    // "plugin" (Standard) | "single" | "dawd". Gilt fuer neu geladene Instanzen.
    void setPluginHostingFile (const juce::File&);
    // "" = nicht anwendbar (Schalter aus, eingebautes Plugin, anderes Format)
    juce::String pluginHostingMode (te::Plugin&);
    // "" = gesetzt, sonst Fehlertext
    juce::String setPluginHostingMode (te::Plugin&, const juce::String& mode);

    // reloadPlugin: Sperre fuer das Plugin aufheben und, wenn sein Host nicht
    // laeuft, neu laden ("" = laeuft, sonst Fehlertext)
    juce::String reloadSandboxedPlugin (te::Plugin&);
    // "" = in Ordnung (oder nicht in der Sandbox), sonst warum der Host nicht laeuft
    juce::String sandboxProblem (juce::AudioPluginInstance&);
    // Zaehlt bei jedem Ausfall, Neustart, Sperren — das UI fragt ihn mit
    // position ab und laedt dann den Stand neu
    uint32_t sandboxStatusSeq();
    // Wird gerufen (Message-Thread), wenn der Host einer Instanz weg ist —
    // dawd schliesst dann ein offenes Fenster des Moduls
    void setPluginHostLostHandler (std::function<void (juce::AudioPluginInstance&)>);

    // Stellvertreter?
    bool isSandboxed (const juce::AudioPluginInstance*);
    bool isSandboxed (te::Plugin&);

    // Meldungen des Hosts jetzt uebernehmen (Werte aus Editor/Plugin, Gesten,
    // neue Parameter-Namen) und den Host pruefen (weg: neu laden); sonst macht
    // das ein Timer alle 50 ms. Nur auf dem Message-Thread — executeOp ruft es
    // vor jeder Op
    void syncSandboxedPlugin (te::Plugin&);
    void syncSandboxedPlugins (te::Edit&);

    // VST3 im Host: Controller mit dem Zustand der Komponente abgleichen, dann
    // Namen und Werte holen (syncPluginParameters in EngineHelpers). Nur auf dem
    // Message-Thread; false = nicht in der Sandbox oder Host weg
    bool syncSandboxedController (te::Plugin&);

    // LV2 im Host mit offenem Editor: Werte als offen markieren, damit das
    // naechste Sichern ohne Graph dem Plugin einen Block gibt (die Meldungen
    // des Editors warten im Host darauf). Message-Thread.
    void markLv2EditorPending (te::Plugin&);

    // Editor-Fenster im Host-Prozess (PluginWindows, Phase 2): ein eigenes
    // Top-Level-Fenster des Hosts. "" = offen, sonst Fehlertext
    juce::String openSandboxWindow (juce::AudioPluginInstance&, const juce::String& title, juce::Point<int> at);
    void moveSandboxWindow (juce::AudioPluginInstance&, juce::Point<int> at);
    void closeSandboxWindow (juce::AudioPluginInstance&);

    // outer: Lage samt Titelleiste, sobald der Host den Rahmen kennt;
    // closeRequested: Klick auf Schliessen seit der letzten Abfrage
    struct SandboxWindowEvents
    {
        std::optional<juce::Point<int>> outer;
        bool closeRequested = false;
    };

    SandboxWindowEvents pollSandboxWindow (juce::AudioPluginInstance&);

    // --window-sim: einen Regler im Host drehen, wie es die Oberflaeche des
    // Plugins tut — die Aenderung kommt ueber den Rueckweg ("" = ok)
    juce::String turnSandboxedKnob (juce::AudioPluginInstance&, int index, float value);

    // {pid, missedBlocks, failed, group, state, crashes, restarts, message?}
    // des Host-Prozesses oder void (nicht in der Sandbox)
    juce::var sandboxInfo (te::Plugin&);

    // Summe der Fehlschlaege (Frist verpasst, Host tot) aller Stellvertreter
    // eines Edits — ein Render vergleicht vorher/nachher und bricht dann mit
    // Fehler ab, statt still eine falsche Datei zu liefern
    juce::int64 sandboxFailureCount (te::Edit&);
    juce::String sandboxFailureText (te::Edit&);

    // Plugin-Scan im Kindprozess (dawd --plugin-scan): je Datei die gefundenen
    // Plugins oder warum nicht (Absturz, Frist). Stuerzt ein Plugin ab, macht
    // ein neues Kind mit der naechsten Datei weiter.
    struct PluginScanResult
    {
        std::vector<juce::PluginDescription> descs;
        juce::String error;
    };

    std::vector<PluginScanResult> scanPluginFilesInChild (const juce::String& format, const juce::StringArray& files);
    int runPluginScan (int outFd, const juce::String& format, const juce::StringArray& files);

    // Host-Modus: dawd --plugin-host <socket-fd> --parent <pid>; kehrt erst
    // beim Beenden zurueck (Rueckgabe = Exit-Code)
    int runPluginHost (int sock, int parentPid);
}
