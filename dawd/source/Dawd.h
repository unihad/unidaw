#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <optional>
#include <map>
#include <algorithm>
#include <vector>

#include "TreeState.h"
#include "SampleLibrary.h"
#include "Jobs.h"
#include "Capture.h"
#include "LoadStatus.h"

namespace te = tracktion;

// Gemeinsamer Unterbau von dawd: Kontext, Ergebnis-/Logger-Helfer, Edit- und
// Aufnahme-Zustand. Die Ops selbst liegen in ops/ und laufen ueber die
// Op-Registry (OpRegistry.h) — dort sind auch Guards und Versionierung.
namespace dawd
{
    using namespace te::literals;

    juce::var resultObj (bool ok, std::initializer_list<std::pair<juce::Identifier, juce::var>> props = {});
    juce::String jsonText (const juce::var& v);

    struct DawdContext;
    class LiveLoudness;

    // Stoppt die Wiedergabe, wenn der Playhead das Ende erreicht (einmal
    // durchhoeren, kein Loop) — playEnd < 0 schaltet den Timer aus.
    struct PlayStopTimer : public juce::Timer
    {
        DawdContext* ctx = nullptr;
        void timerCallback() override;
    };

    // Speichert den Edit des aktuellen Nodes kurz nach der letzten Mutation
    // (gebuendelt) — ein Absturz, z. B. durch ein Plugin, verliert sonst alles
    // seit dem letzten Node-Wechsel. Waehrend einer Aufnahme wird verschoben.
    struct AutoSaveTimer : public juce::Timer
    {
        DawdContext* ctx = nullptr;
        void timerCallback() override;
    };

    // Eine geaermte Lane: Index im Track-Array, ihr Zieltrack (fuer den
    // + Button) und der Input-Typ. Multi-Track-Record traegt einen Eintrag
    // je Lane.
    struct ArmedTake
    {
        int laneIndex = -1;
        int targetIndex = -1;
        bool midi = false;
    };

    // Ein geloeschter Clip fuer undoDelete: die deleteClip-Aktion (ihre Id
    // ist das Token), der Track (EditItemID — Indizes verschieben sich) und
    // der Clip-Zustand als ValueTree-Kopie.
    struct DeletedClip
    {
        int actionId = 0;
        te::EditItemID trackId;
        juce::ValueTree state;
    };

    // Pegel-Client an einem LevelMeasurer (levels-Poll). Der Measurer haelt
    // rohe Zeiger auf seine Clients — darum meldet executeOp vor jeder
    // mutierenden Op alle Clients ab (clearMeters); keine Op kann so einen
    // Track oder Edit mit angemeldetem Client loeschen. Der naechste Poll
    // meldet neu an.
    struct MeterEntry
    {
        te::LevelMeasurer* measurer = nullptr;
        std::unique_ptr<te::LevelMeasurer::Client> client;
    };

    struct UndoHistory;   // Undo.cpp
    struct InterfaceBackend;   // Interface.h
    struct PluginWindows;      // PluginWindows.h

    // Laedt den Interface-Mix des Projekts, sobald das Geraet (wieder)
    // erscheint (ops/Interface.cpp)
    // Audio-Graph im Leerlauf freigeben: nach 20 s ohne Wiedergabe,
    // Aufnahme, Monitor und geaermte Spur. Sonst laufen alle Plugins auch im
    // Stillstand weiter (Praxistest 1e: ~60 % CPU, bei 64 Frames Block
    // Aussetzer in jeder anderen Wiedergabe). Play, Monitor und Arm legen
    // den Graphen wieder an. Aufruf aus dem Interface-Timer (2 s).
    void releaseIdleGraph (DawdContext&);

    struct InterfaceTimer : public juce::Timer
    {
        DawdContext* ctx = nullptr;
        void timerCallback() override;
    };

    struct DawdContext
    {
        // openAudio = false (--no-audio): kein Audio-/MIDI-Geraet oeffnen —
        // Offline-Render und Analyse laufen, Wiedergabe und Aufnahme nicht.
        // prefsDir: Ordner der Engine-Einstellungen (Settings.xml) — das
        // --config-Verzeichnis, damit Test- und Mess-Instanzen die Einstellungen
        // der Entwickler-Instanz nicht teilen
        DawdContext (const juce::String& appName, bool openAudio, const juce::File& prefsDir);

        std::unique_ptr<te::Engine> engine;
        // Editor-Fenster der Module (pluginWindow): echt oder --window-sim.
        // Vor edit: Fenster schliessen mit dem Edit und melden ihre Lage hierher
        std::shared_ptr<PluginWindows> windows;
        std::unique_ptr<te::Edit> edit;
        std::optional<TreeState> tree;
        std::unique_ptr<SampleLibrary> library;   // findSamples; <config>/libraries.json
        // Hinweise einer Analyse-Op auf Material aus Ordnern ohne Analyse
        // (Lizenz); executeOp setzt sie als licenceNote in die Antwort
        juce::StringArray licenceNotes;
        juce::File configDir;                      // --config (Bibliotheken, Presets, Rezept-Entwuerfe)
        juce::File recipesDir;                     // --recipes: gepruefte Rezepte (Repo recipes/)
        std::unique_ptr<JobList> jobs = std::make_unique<JobList>();   // separateStems, jobStatus
        std::unique_ptr<SystemCapture> capture = std::make_unique<SystemCapture>();   // capture (pw-record)
        juce::File logFile;
        int nextSeq = 1;
        int stateVersion = 0;
        std::vector<ArmedTake> armedTakes;  // geaermte Lanes der laufenden Aufnahme
        const void* loggedTakeEnd = nullptr; // Diagnose: Take-Ende ohne Stopp schon geloggt
        juce::String pendingTakeSection;    // Take-Section, gilt fuer alle Lanes
        bool monitorOn = false;
        juce::uint32 lastAudioUse = 0;      // releaseIdleGraph: zuletzt gespielt/aufgenommen/ueberwacht/geaermt
        std::vector<DeletedClip> deletedClips;  // undoDelete — gilt im geladenen Edit
        std::vector<std::vector<te::EditItemID>> recordedTakes;
        double playEnd = -1;
        std::unique_ptr<PlayStopTimer> playStop;
        std::unique_ptr<AutoSaveTimer> autoSave;
        std::map<juce::String, MeterEntry> meters;   // Spur-ItemID -> Client (levels)
        std::vector<std::unique_ptr<te::LevelMeasurer::Client>> retiredMeterClients;
        // Live-LUFS am Main-Ausgang (LiveLoudness.h): gehoert dem
        // DeviceManager (globaler Ausgangs-Prozessor), nullptr ohne Geraet
        LiveLoudness* loudness = nullptr;
        std::shared_ptr<UndoHistory> undoHistory;   // Undo.h — shared_ptr: Typ hier unvollstaendig

        // Abhoer-Mute (Decision 2026-10-20): gespeicherter Mute-Wert je Spur
        // (ItemID), solange trackMix ihn zum Abhoeren ueberdeckt. Gilt nur fuer
        // den Edit mit diesem State (die Referenz haelt ihn eindeutig) — ein
        // neu geladener Edit hat wieder den gespeicherten Stand.
        juce::ValueTree listenState;
        std::map<juce::String, bool> savedMute;

        // Ladestand fuer GET /progress (LoadStatus.h): laufende Op, Phase,
        // Plugin i von n, Hintergrund — vom HTTP-Thread lesbar, waehrend der
        // Message-Thread noch arbeitet
        LoadStatus load;
        std::unique_ptr<juce::Timer> loadTimer;   // traegt den Hintergrund nach (Dawd.cpp)

        // Session-Datei (--session): haelt den offenen Projektordner, damit
        // ein Neustart nach einem Absturz das Projekt wieder oeffnet. Ohne
        // Flag leer — Test-Instanzen merken sich nichts.
        juce::File sessionFile;
        juce::String sessionDir;    // zuletzt in die Session-Datei geschrieben

        // Eingangs-Anzeige (inputLevel, Anzeige-Poll): Pegel-Client am
        // LevelMeasurer des Wave-Inputs (bleibt nach einem Device-Rebuild am
        // alten Messer stehen und wird am neuen wieder angemeldet) und der
        // Name des aktiven System-Eingangs, gecacht (wpctl ist ein Prozess).
        te::LevelMeasurer::Client inputMeter;
        te::InputDevice* inputMeterDevice = nullptr;
        juce::String systemSource;
        juce::int64 systemSourceTime = 0;

        // Audio-Interface (interfaceState/setInterface): ALSA oder simuliert
        // (--interface-sim); ifaceAppliedToken = Geraet, auf das der Mix des
        // offenen Projekts geladen ist ("" = steht aus)
        std::shared_ptr<InterfaceBackend> iface;
        juce::String ifaceAppliedToken;
        std::unique_ptr<InterfaceTimer> ifaceTimer;

        // Audio-Einstellungen (ops/Audio.cpp): Puffergroesse global (audio.json
        // im --config-Ordner, 0 = Standard), Rate und Aufnahme-Bit-Tiefe je Song
        // (tree.json). Unter JACK stellt dawd PipeWire um (clock.force-*) und
        // merkt sich, was es gesetzt hat — nur das setzt es wieder zurueck.
        bool audioOpen = false;
        int bufferSetting = 0;
        int forcedRate = 0, forcedQuantum = 0;
        double deviceRate = 0; int deviceBuffer = 0;   // ALSA: Startwerte, gelten bei 0
    };

    // Hintergrund (Jobs, Klang-Analyse) in den Ladestand uebernehmen —
    // Message-Thread (executeOp, Timer)
    void updateBackgroundStatus (DawdContext& ctx);

    void appendEvent (DawdContext& ctx, const juce::String& op, const juce::var& params, const juce::var& result);

    bool isRecording (DawdContext& ctx);
    void saveCurrentEdit (DawdContext& ctx);

    // Solo und Mute sind Abhoer-Hilfen (Decision 2026-10-20): nicht in
    // Node-Datei, Undo-Stand und Render. listeningMutes: ItemID -> gespeicherter
    // Mute-Wert im offenen Edit (leer fuer jeden anderen Edit).
    const std::map<juce::String, bool>& listeningMutes (DawdContext& ctx, te::Edit& edit);
    void listenMute (DawdContext& ctx, te::Track& track, bool on);   // trackMix mute
    void forgetLaneListening (DawdContext& ctx, te::AudioTrack& lane);   // Lane-Wahl setzt Mute als Struktur

    // Fuer die Dauer eines Renders/einer Messung: kein Solo, Mute wie
    // gespeichert. Nach ScopedRenderStatus anlegen (Geraet frei — nicht hoerbar).
    struct ScopedListeningOff
    {
        ScopedListeningOff (DawdContext& ctx, te::Edit& edit);
        ~ScopedListeningOff();

        juce::Array<te::Track::Ptr> soloed;
        juce::Array<std::pair<te::Track::Ptr, bool>> muted;   // Spur, Abhoer-Wert
    };

    // Autosave: nach einer erfolgreichen mutierenden Op planen (executeOp),
    // vor dem Beenden ausstehende Speicherung sofort schreiben (quit, Signal).
    void scheduleAutosave (DawdContext& ctx);
    void flushAutosave (DawdContext& ctx);

    // Alle Pegel-Clients abmelden (vor Mutationen, beim Beenden)
    void clearMeters (DawdContext& ctx);

    // Session-Datei auf den offenen Projektordner nachziehen (nach jeder Op;
    // schreibt nur bei Aenderung, leerer dir = kein Projekt offen).
    void updateSessionFile (DawdContext& ctx);
    juce::String readSessionDir (const juce::File& sessionFile);

    // Record-Enabled zuruecksetzen — der Transport-Stop raeumt nicht auf; mit
    // recordEnabled bleibt MonitorMode::automatic dauerhaft live
    // (isRecordingActive fragt recordEnabled) und das Mikro speist weiter.
    void disarmRecording (DawdContext& ctx);

    void bindEditResolver (DawdContext& ctx, const juce::File& editFile);

    // Node-Edit laden: Takes gehoeren zum Edit, in dem sie aufgenommen wurden —
    // recordedTakes leeren, Datei laden, Resolver binden.
    void loadNodeEdit (DawdContext& ctx, const juce::File& editFile);

    juce::var stopRecordingCollect (DawdContext& ctx);

    // Neue Aktion am aktuellen Node ablegen und speichern — der einzige
    // Schreibweg fuer Node-Aktionen (acted und Take-Daten leiten sich ab).
    // coalesce: ist die letzte Aktion am Node dieselbe (kind, op, params
    // track + name + param), wird sie ersetzt statt eine neue anzuhaengen (z. B.
    // setMidiNotes beim Zeichnen). Rueckgabe: Aktions-Id (0 ohne Baum).
    int recordNodeAction (DawdContext& ctx, const juce::String& kind, const juce::String& op,
                          const juce::var& params = {}, const juce::String& file = {},
                          bool coalesce = false);

    // Auto-Section (EngineHelpers autoSection) als eigene collect-Aktion
    // addSection {name, auto: true} am Node, hinter der Aktion der
    // ausloesenden Op (Entscheidung Matthias 2026-10-24) — die Sorte des
    // Nodes (erste Aktion) und die Take-Daten bleiben; Undo nimmt sie mit
    // der Op zurueck. newSection void = keine Auto-Section.
    void recordAutoSection (DawdContext& ctx, const juce::var& newSection);

    // Aktion am aktuellen Node entfernen (undoDelete) — der Gegenweg zu
    // recordNodeAction; false, wenn der Node die Aktion nicht (mehr) hat.
    bool removeNodeAction (DawdContext& ctx, int actionId);

    bool isAncestorOf (const TreeState& t, int ancestor, int id);

    // Projektwechsel: Interface-Mix des neuen Projekts laden (treeOpen,
    // treeSeed, treeClose); syncInterfaceProject laedt ihn nach dem Einstecken
    void interfaceProjectChanged (DawdContext& ctx);
    void syncInterfaceProject (DawdContext& ctx);

    // Audio-Einstellungen anwenden: beim Start (Puffer), nach Projektwechsel
    // (Rate und Bit-Tiefe des Songs); beim Beenden PipeWire-Vorgaben loesen
    void audioStartup (DawdContext& ctx);
    void audioProjectChanged (DawdContext& ctx);
    void audioShutdown (DawdContext& ctx);

    // Temp-Datei mit eindeutigem Namen: mehrere dawd teilen /tmp (unidaw-7400,
    // Suite-Gruppen A/B, Agents) — getNonexistentChildFile gab zwei Prozessen
    // denselben freien Namen, einer las dann den Render des anderen
    // (compare_reference_articulation_flow, 2026-10-29)
    inline juce::File uniqueTempFile (const juce::String& prefix, const juce::String& suffix)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile (prefix + "-" + juce::Uuid().toString() + suffix);
    }
}
