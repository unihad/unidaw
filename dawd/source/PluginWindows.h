#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <map>
#include <optional>

namespace te = tracktion;

// Editor-Fenster der Plugin-Module (Op pluginWindow): die eigene Oberflaeche
// eines Fremd-Plugins (AudioProcessorEditor) in einem nativen Fenster. Die
// Engine haelt das Fenster am Plugin (te::PluginWindowState) und schliesst
// es, bevor das Plugin geht (removePlugin, Edit-Wechsel, Beenden); dawd baut
// es in HeadlessUIBehaviour::createPluginWindow. Echt (JUCE ueber X11, unter
// Wayland XWayland — braucht DISPLAY) oder simuliert (--window-sim: ein
// Component ohne Desktop-Fenster, die Test-Suite oeffnet nie ein Fenster).
// Mit Plugin-Sandbox (Standard, aus mit UNIDAW_PLUGIN_SANDBOX=0) gehoert das Fenster dem
// Host-Prozess des Plugins: hinter denselben Funktionen (Modul-Id rein,
// Fehlertext raus) steht dann RemoteWindow, der Host oeffnet ein
// NativeEditorWindow — die Op bleibt.
namespace dawd
{
    struct DawdContext;

    // Zustand eines Fremd-Plugins: Chunk und Werte der Parameter. Verglichen
    // werden nur die Werte — der Chunk folgt einem gesetzten Wert erst nach
    // dem naechsten Audio-Block (LV2, VST3), ein Vergleich daran meldete
    // Aenderungen, die laengst gebucht sind
    struct EditorState
    {
        juce::MemoryBlock chunk;
        juce::Array<float> values;

        bool operator== (const EditorState& o) const { return values == o.values; }
        bool operator!= (const EditorState& o) const { return ! operator== (o); }
    };

    struct PluginWindows
    {
        explicit PluginWindows (bool sim) : simulated (sim) {}

        const bool simulated;

        // Lage je Modul-Id: je Projekt in windows.json (nicht je Node, kein
        // Teil des Edits), ohne Projekt nur im Speicher
        std::map<juce::String, juce::Point<int>> positions;
        juce::File loadedFor;
        bool loaded = false;

        // Zustand des Plugins beim Oeffnen (bzw. nach der letzten Op mit
        // Node-Aktion daran) je Modul-Id; weicht er beim Schliessen ab, bucht
        // executeOp die Aenderung im Editor als process-Aktion pluginWindow
        // (docs/decisions/2026-10-24-plugin-fenster-als-aktion.md)
        std::map<juce::String, EditorState> openedState;
        // Neuer Zustand, solange executeOp diese Aenderung bucht
        std::map<juce::String, EditorState> closingState;
    };

    // Fenster eines Moduls oeffnen (at: Lage auf dem Bildschirm, sonst die
    // gemerkte) — "" = offen, sonst Fehlertext (API-Vertrag)
    juce::String openPluginWindow (DawdContext&, const juce::String& moduleId, std::optional<juce::Point<int>> at);
    // true = mit gebuchter Aenderung im Editor geschlossen
    bool closePluginWindow (DawdContext&, const juce::String& moduleId);
    bool pluginWindowOpen (te::Plugin&);
    std::optional<juce::Point<int>> pluginWindowPosition (DawdContext&, const juce::String& moduleId);

    // Edit-Wechsel im selben Projekt (loadNodeEdit: Branch, Checkout, Undo):
    // vorher die Module mit offenem Fenster merken, danach wieder oeffnen
    juce::StringArray openPluginWindowIds (DawdContext&);
    void reopenPluginWindows (DawdContext&, const juce::StringArray& moduleIds);

    // Aenderung im Editor als Node-Aktion (executeOp, Op pluginWindow):
    // schliesst cmd ein Fenster, dessen Plugin-Zustand sich seit dem Oeffnen
    // geaendert hat (Baum offen)?
    bool closesChangedPluginWindow (DawdContext&, const juce::var& cmd);
    // Vor Schnappschuss und Auto-Branch: Fenster zu, neuen Zustand merken,
    // den beim Oeffnen zurueck ins Plugin — Vorher-Stand (Undo) und
    // Eltern-Node bekommen ihn, nicht die Aenderung
    void beginPluginWindowChange (DawdContext&, const juce::String& moduleId);
    // Gemerkten neuen Zustand ins Plugin des (evtl. neuen) Edits; false = keiner
    bool applyPluginWindowChange (DawdContext&, const juce::String& moduleId);
    // Nach einer Op mit Node-Aktion an dem Modul (setParameter, loadPreset …):
    // ihre Aenderung ist schon gebucht, der Vergleich beginnt neu
    void pluginWindowSettled (DawdContext&, const juce::String& moduleId);
    // Nur mit --window-sim (Test-Suite): einen Regler im simulierten Editor
    // drehen, wie es die Oberflaeche des Plugins tut ("" = ok, sonst Fehlertext)
    juce::String turnSimulatedKnob (DawdContext&, const juce::String& moduleId, const juce::String& param, double value);

    // Plugin-Sandbox: der Host-Prozess dieser Instanz ist weg — ein offenes
    // Fenster des Moduls gilt nicht mehr als offen (zu, ohne Aktion: Aenderungen
    // im Editor seit dem letzten gelesenen Zustand sind mit dem Host verloren)
    void closeWindowOfLostHost (DawdContext&, juce::AudioPluginInstance&);

    // Fabrik fuer HeadlessUIBehaviour::createPluginWindow
    std::unique_ptr<juce::Component> createPluginWindow (DawdContext&, te::PluginWindowState&);

    // Natives Fenster mit Titelleiste des Desktops fuer einen Editor — in dawd
    // (EditorWindow) und im Host-Prozess der Plugin-Sandbox. Lage = linke
    // obere Ecke des ganzen Fensters samt Titelleiste.
    //
    // Beim Oeffnen kennt JUCE den Rahmen noch nicht (_NET_FRAME_EXTENTS setzt
    // der Fenstermanager erst danach) und schickt die Lage unveraendert: der
    // Fenstermanager legt den Rahmen dorthin, der Inhalt landet eine
    // Titelleiste tiefer. getPosition() meldet den Inhalt — als Lage gemerkt,
    // wanderte das Fenster bei jedem Oeffnen um die Titelleiste nach unten
    // (2026-10-25, GNOME/XWayland: angefragt 400,300, Inhalt bei 400,337,
    // _NET_FRAME_EXTENTS 0,0,37,0).
    struct NativeEditorWindow : public juce::DocumentWindow, private juce::Timer
    {
        NativeEditorWindow (const juce::String& title, juce::Component* contentToOwn, bool resizable, juce::Point<int> at);

        // Rahmen des Desktops, sobald der Fenstermanager ihn gemeldet hat
        std::optional<juce::Point<int>> frameOffset();
        // Lage samt Titelleiste ({} = Rahmen noch unbekannt)
        std::optional<juce::Point<int>> outerPosition();
        // Ganzes Fenster nach at: mit bekanntem Rahmen zieht JUCE ihn ab,
        // ohne liegt der Rahmen ohnehin bei at
        void place (juce::Point<int> at);

    private:
        // Liegengebliebene X-Ereignisse abarbeiten (dispatchQueuedXEvents)
        void timerCallback() override;
    };

    // X-Ereignisse, die Xlib schon gelesen hat, an JUCE geben (XErrors.cpp).
    // JUCE arbeitet sie nur ab, wenn die X-Verbindung lesbar wird. Liest
    // ein Aufruf mit Antwort ausserhalb davon (isWindowShowing fragt
    // _NET_WM_STATE ab — jede Op pluginWindow) ein ConfigureNotify mit in
    // die Warteschlange, bleibt es dort, bis wieder etwas vom Server kommt:
    // JUCE behielt die alte Lage des Fensters (Test unter Last, 2026-10-30).
    void dispatchQueuedXEvents();

    // X-Fehler loggen statt den Prozess zu beenden (XErrors.cpp): JUCE setzt
    // seinen Handler nur fuer eine JUCEApplication. main ruft es gleich nach
    // dem Initialiser — gilt fuer dawd, Plugin-Host und Scan-Kind. Ohne
    // DISPLAY tut es nichts.
    void installXErrorHandler();
}
