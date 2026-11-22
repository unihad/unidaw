#include "OpRegistry.h"
#include "PluginSandbox.h"

#include "ops/Ops.h"
#include "Undo.h"
#include "PluginWindows.h"
#include "Routing.h"
#include "Sidechain.h"
#include "EngineHelpers.h"

#include <map>

namespace dawd
{
    const std::vector<OpDef>& opTable()
    {
        static const std::vector<OpDef> table = {
            //     name                  kind           mutating  edit   tree   tree+edit  tree|edit  blockRec  handler
            { "ping",                "session",     false, false, false, false, false, false, opPing },
            { "listOps",             "session",     false, false, false, false, false, false, opListOps },
            { "about",               "session",     false, false, false, false, false, false, opAbout },
            { "batch",               "session",     false, false, false, false, false, false, opBatch },
            { "newEdit",             "session",      true, false, false, false, false,  true, opNewEdit },
            { "createTrack",         "session",      true,  true, false, false, false, false, opCreateTrack },
            { "renameTrack",         "session",      true,  true, false, false, false, false, opRenameTrack },
            { "listDir",             "session",     false, false, false, false, false, false, opListDir },
            { "createDir",           "session",      true, false, false, false, false, false, opCreateDir },
            { "quit",                "session",      true, false, false, false, false, false, opQuit },

            { "treeState",           "tree",        false, false,  true, false, false, false, opTreeState },
            { "treeSeed",            "tree",         true, false, false, false, false, false, opTreeSeed },
            { "treeOpen",            "tree",         true, false, false, false, false, false, opTreeOpen },
            { "treeClose",           "tree",         true, false, false, false,  true,  true, opTreeClose },
            { "treeBranch",          "tree",         true, false, false,  true, false,  true, opTreeBranch },
            { "bounceNode",          "tree",         true, false, false,  true, false,  true, opBounceNode },
            { "treeCheckout",        "tree",         true, false, false,  true, false,  true, opTreeCheckout },
            { "treeReset",           "tree",         true, false, false,  true, false,  true, opTreeReset },
            { "treeDelete",          "tree",         true, false, false,  true, false,  true, opTreeDelete },
            { "treeTerminate",       "tree",         true, false,  true, false, false, false, opTreeTerminateFavorite },
            { "treeFavorite",        "tree",         true, false,  true, false, false, false, opTreeTerminateFavorite },
            { "treeLabel",           "tree",         true, false,  true, false, false, false, opTreeLabel },
            { "treeNote",            "tree",         true, false,  true, false, false, false, opTreeNote },
            { "treeSettings",        "tree",         true, false,  true, false, false, false, opTreeSettings },
            { "undo",                "tree",         true, false, false,  true, false,  true, opUndo },
            { "redo",                "tree",         true, false, false,  true, false,  true, opRedo },
            { "treeCompactPreview",  "tree",        false, false, false,  true, false,  true, opTreeCompactPreview },
            { "treeCompact",         "tree",         true, false, false,  true, false,  true, opTreeCompact },

            { "addSection",          "section",      true,  true, false, false, false,  true, opAddSection },
            { "duplicateSection",    "section",      true,  true, false, false, false,  true, opDuplicateSection },
            { "deleteSection",       "section",      true,  true, false, false, false,  true, opDeleteSection },
            { "updateSection",       "section",      true,  true, false, false, false,  true, opUpdateSection },
            { "moveSection",         "section",      true,  true, false, false, false,  true, opMoveSection },
            { "addMarker",           "section",      true,  true, false, false, false,  true, opAddMarker },
            { "updateMarker",        "section",      true,  true, false, false, false,  true, opUpdateMarker },
            { "deleteMarker",        "section",      true,  true, false, false, false,  true, opDeleteMarker },

            { "record",              "media",        true,  true, false, false, false, false, opRecord },
            { "monitor",             "media",        true,  true, false, false, false, false, opMonitor },
            { "inputGain",           "media",        true, false, false, false, false, false, opInputGain },
            { "inputLevel",          "media",       false, false, false, false, false, false, opInputLevel },
            { "listInputs",          "media",       false,  true, false, false, false, false, opListInputs },
            { "armTrack",            "media",        true,  true, false, false, false,  true, opArmTrack },
            { "assignInput",         "media",        true,  true, false, false, false,  true, opAssignInput },
            { "clearInput",          "media",        true,  true, false, false, false,  true, opClearInput },
            { "addClip",             "media",        true,  true, false, false, false, false, opAddClip },
            { "addMidiClip",         "media",        true,  true, false, false, false, false, opAddMidiClip },
            { "deleteClip",          "media",        true,  true, false, false, false, false, opDeleteClip },
            { "undoDelete",          "media",        true,  true, false, false, false, false, opUndoDelete },
            { "setMidiNotes",        "media",        true,  true, false, false, false, false, opSetMidiNotes },
            { "editNotes",           "media",        true,  true, false, false, false, false, opEditNotes },
            { "setControllers",      "media",        true,  true, false, false, false, false, opSetControllers },
            { "exportMidi",          "media",       false,  true, false, false, false, false, opExportMidi },
            { "quantizeClip",        "media",        true,  true, false, false, false, false, opQuantizeClip },
            { "trimClip",            "media",        true,  true, false, false, false, false, opTrimClip },
            { "moveClip",            "media",        true,  true, false, false, false, false, opMoveClip },
            { "splitClip",           "media",        true,  true, false, false, false,  true, opSplitClip },
            { "setClip",             "media",        true,  true, false, false, false,  true, opSetClip },
            { "stretchClip",         "media",        true,  true, false, false, false,  true, opStretchClip },
            { "warpClip",            "media",        true,  true, false, false, false,  true, opWarpClip },
            { "loopClip",            "media",        true,  true, false, false, false,  true, opLoopClip },
            { "duplicateClip",       "media",        true,  true, false, false, false,  true, opDuplicateClip },
            { "lastTake",            "media",       false,  true, false, false, false,  true, opLastTake },
            { "compLane",            "media",        true,  true, false, false, false,  true, opCompLane },

            { "favoriteClip",        "select",       true,  true, false,  true, false, false, opFavoriteClip },
            { "favoriteSection",     "select",       true,  true, false,  true, false, false, opFavoriteSection },

            { "render",              "processing",   true,  true, false, false, false, false, opRender },
            { "renderNode",          "processing",  false, false,  true, false, false, false, opRenderNode },
            { "renderStems",         "processing",   true,  true, false, false, false, false, opRenderStems },
            { "processClip",         "processing",   true,  true, false, false, false,  true, opProcessClip },
            { "renderBatch",         "processing",   true, false, false,  true, false, false, opRenderBatch },
            { "pickInstrument",      "media",        true,  true, false, false, false,  true, opPickInstrument },
            { "analyze",             "processing",  false, false, false, false, false, false, opAnalyze },
            { "setSamples",          "processing",   true,  true, false, false, false,  true, opSetSamples },
            { "listSamples",         "processing",  false,  true, false, false, false, false, opListSamples },
            { "sampleLibraries",     "media",       false, false, false, false, false, false, opSampleLibraries },
            { "findSamples",         "media",       false, false, false, false, false, false, opFindSamples },
            { "compareReference",    "processing",  false,  true, false, false, false, false, opCompareReference },
            { "capture",             "media",       false, false, false, false, false, false, opCapture },
            { "captureStatus",       "media",       false, false, false, false, false, false, opCaptureStatus },
            { "detectSongBoundaries","processing",  false, false, false, false, false, false, opDetectSongBoundaries },
            { "saveCaptureSongs",    "media",       false, false, false, false, false, false, opSaveCaptureSongs },
            { "detectTempo",         "processing",  false, false, false, false, false, false, opDetectTempo },
            { "extractGroove",       "processing",  false, false, false, false, false, false, opExtractGroove },
            { "detectKey",           "processing",  false, false, false, false, false, false, opDetectKey },
            { "describeVoice",       "processing",  false, false, false, false, false, false, opDescribeVoice },
            { "describeTimbre",      "processing",  false, false, false, false, false, false, opDescribeTimbre },
            { "compareVoice",        "processing",  false, false, false, false, false, false, opCompareVoice },
            { "compareStems",        "processing",  false, false, false, false, false, false, opCompareStems },
            { "describeDrums",       "processing",  false, false, false, false, false, false, opDescribeDrums },
            { "compareDrums",        "processing",  false, false, false, false, false, false, opCompareDrums },
            { "analyzeStructure",    "processing",  false, false, false, false, false, false, opAnalyzeStructure },
            { "findRepeats",         "processing",  false, false, false, false, false, false, opFindRepeats },
            { "audioToMidi",         "processing",  false, false, false, false, false, false, opAudioToMidi },
            { "separateStems",       "processing",  false, false, false, false, false,  true, opSeparateStems },
            { "compareClasses",      "processing",  false,  true, false, false, false,  true, opCompareClasses },
            { "describeLayers",      "processing",  false, false, false, false, false,  true, opDescribeLayers },
            { "splitLayer",          "processing",  false, false, false, false, false,  true, opSplitLayer },
            { "compareSeparated",    "processing",  false, false, false, false, false,  true, opCompareSeparated },
            { "jobStatus",           "processing",  false, false, false, false, false, false, opJobStatus },
            { "addMidiFromAudio",    "media",        true,  true, false, false, false,  true, opAddMidiFromAudio },
            { "peak",                "processing",  false,  true, false, false, false, false, opPeak },
            { "clipPeaks",           "processing",  false,  true, false, false, false, false, opClipPeaks },
            { "clipNotes",           "processing",  false,  true, false, false, false, false, opClipPeaks },   // Alias: Noten eines MIDI-Clips lesen (Praxistest 1d suchte "pianoRoll")
            { "dump",                "processing",  false,  true, false, false, false, false, opDump },
            { "listPlugins",         "processing",  false, false, false, false, false, false, opListPlugins },
            { "addPlugin",           "processing",   true,  true, false, false, false, false, opAddPlugin },
            { "removePlugin",        "processing",   true,  true, false, false, false, false, opRemovePlugin },
            { "linkPlugin",          "processing",   true,  true, false, false, false, false, opLinkPlugin },
            { "setChain",            "processing",   true,  true, false, false, false, false, opSetChain },
            { "connect",             "processing",   true,  true, false, false, false, false, opConnect },
            { "disconnect",          "processing",   true,  true, false, false, false, false, opDisconnect },
            { "setEdge",             "processing",   true,  true, false, false, false, false, opSetEdge },
            { "setWet",              "processing",   true,  true, false, false, false, false, opSetWet },
            { "routing",             "processing",  false,  true, false, false, false, false, opRouting },
            { "setParameter",        "processing",   true,  true, false, false, false, false, opSetParameter },
            { "renamePlugin",        "processing",   true,  true, false, false, false, false, opRenamePlugin },
            { "pluginWindow",        "processing",  false,  true, false, false, false, false, opPluginWindow },
            { "pluginWindowSim",     "processing",  false,  true, false, false, false, false, opPluginWindowSim },
            { "reloadPlugin",        "processing",  false,  true, false, false, false, false, opReloadPlugin },
            { "pluginHosting",       "processing",   true,  true, false, false, false, false, opPluginHosting },
            { "listParameters",      "processing",  false,  true, false, false, false, false, opListParameters },
            { "pluginInfo",          "processing",  false,  true, false, false, false, false, opPluginInfo },
            { "listPresets",         "processing",  false,  true, false, false, false, false, opListPresets },
            { "loadPreset",          "processing",   true,  true, false, false, false, false, opLoadPreset },
            { "savePreset",          "processing",  false,  true, false, false, false, false, opSavePreset },
            { "findPresets",         "processing",  false, false, false, false, false, false, opFindPresets },
            { "matchSound",          "processing",  false,  true, false, false, false, false, opMatchSound },
            { "tuneSound",           "processing",  false,  true, false, false, false, false, opTuneSound },
            { "setPluginFile",       "processing",   true,  true, false, false, false, false, opSetPluginFile },
            { "listAutomatable",     "processing",  false,  true, false, false, false, false, opListAutomatable },
            { "automation",          "processing",  false,  true, false, false, false, false, opAutomation },
            { "setAutomation",       "processing",   true,  true, false, false, false, false, opSetAutomation },
            { "clearAutomation",     "processing",   true,  true, false, false, false, false, opClearAutomation },
            { "setModifier",         "processing",   true,  true, false, false, false, false, opSetModifier },
            { "clearModifier",       "processing",   true,  true, false, false, false, false, opClearModifier },
            { "freezeTrack",         "processing",   true,  true, false, false, false,  true, opFreezeTrack },
            { "flattenTrack",        "processing",   true,  true, false, false, false,  true, opFlattenTrack },
            { "createGroup",         "processing",   true,  true, false, false, false, false, opCreateGroup },
            { "setGroup",            "processing",   true,  true, false, false, false, false, opSetGroup },
            { "removeGroup",         "processing",   true,  true, false, false, false, false, opRemoveGroup },

            { "saveRecipe",          "recipes",     false, false, false,  true, false, false, opSaveRecipe },
            { "recipeInfo",          "recipes",     false, false, false, false, false, false, opRecipeInfo },
            { "findRecipes",         "recipes",     false, false, false, false, false, false, opFindRecipes },
            { "applyRecipe",         "recipes",      true, false, false,  true, false,  true, opApplyRecipe },
            { "rateRecipe",          "recipes",     false, false, false, false, false, false, opRateRecipe },

            { "state",               "view",        false,  true, false, false, false, false, opState },
            { "play",                "view",         true,  true, false, false, false, false, opPlay },
            { "loop",                "view",         true,  true, false, false, false, false, opLoop },
            { "position",            "view",        false,  true, false, false, false, false, opPosition },
            { "trackMix",            "view",         true,  true, false, false, false, false, opTrackMix },
            { "groupMix",            "view",         true,  true, false, false, false, false, opGroupMix },
            { "levels",              "view",        false,  true, false, false, false, false, opLevels },
            { "laneActive",          "view",         true,  true, false, false, false, false, opLaneActive },

            { "interfaceState",      "interface",   false, false, false, false, false, false, opInterfaceState },
            { "setInterface",        "interface",    true, false, false, false, false, false, opSetInterface },
            { "interfaceMeters",     "interface",   false, false, false, false, false, false, opInterfaceMeters },
            { "audioSettings",       "session",      true, false, false, false, false,  true, opAudioSettings },
            { "interfaceSim",        "interface",    true, false, false, false, false, false, opInterfaceSim },
            { "oscSettings",         "session",      true, false, false, false, false, false, opOscSettings },
        };

        return table;
    }

    const OpDef* findOp (const juce::String& name)
    {
        for (auto& def : opTable())
            if (name == def.name)
                return &def;

        return nullptr;
    }

    // Solo und Mute sind Abhoer-Hilfen (Decision 2026-10-20): ein trackMix nur
    // mit solo/mute bucht keine Aktion, verzweigt nicht (die Wiedergabe laeuft
    // weiter), ist kein Undo-Schritt und speichert nicht. Vorher legte Solo auf
    // einer zweiten Spur per Auto-Branch einen Node an und lud den Edit neu.
    // Ebenso trackMix ohne Werte (Abfrage; Praxistest 1g: legte einen Node an),
    // und groupMix genauso (Gruppen/Submix: Mute und Solo der Gruppe).
    static bool isListeningOnly (const juce::String& op, const juce::var& cmd)
    {
        return (op == "trackMix" || (op == "groupMix" && ! cmd.hasProperty ("name")))
            && ! cmd.hasProperty ("volumeDb") && ! cmd.hasProperty ("pan");
    }

    // audioSettings/oscSettings ohne Werte lesen nur: keine Version (sonst
    // zeigte das UI nach dem Oeffnen des Audio-Popovers "Refresh")
    static bool isSettingsRead (const juce::String& op, const juce::var& cmd)
    {
        if (op == "audioSettings")
            return ! cmd.hasProperty ("bufferSize") && ! cmd.hasProperty ("sampleRate") && ! cmd.hasProperty ("recordBitDepth");

        if (op == "oscSettings")
            return ! cmd.hasProperty ("enabled") && ! cmd.hasProperty ("port") && ! cmd.hasProperty ("address") && ! cmd.hasProperty ("allow");

        return false;
    }

    // Sorte der Node-Aktion, die eine Op aufzeichnet ("" = keine) — vorab
    // bekannt, damit der Auto-Branch vor den Seiteneffekten passiert.
    // Neue Ops, die recordNodeAction aufrufen, gehoeren hier hinein.
    juce::String actionKindOf (DawdContext& ctx, const juce::String& op, const juce::var& cmd)
    {
        if (isListeningOnly (op, cmd))
            return {};

        static const std::map<juce::String, juce::String> kinds = {
            { "addSection", "collect" }, { "updateSection", "collect" }, { "moveSection", "collect" },
            { "addClip", "collect" }, { "addMidiClip", "collect" }, { "pickInstrument", "collect" },
            { "favoriteClip", "select" }, { "favoriteSection", "select" },
            { "deleteClip", "process" }, { "trimClip", "process" }, { "moveClip", "process" },
            { "setMidiNotes", "process" }, { "editNotes", "process" }, { "setControllers", "process" }, { "quantizeClip", "process" }, { "addPlugin", "process" }, { "removePlugin", "process" },
            { "linkPlugin", "process" }, { "setChain", "process" }, { "connect", "process" },
            { "disconnect", "process" }, { "setAutomation", "process" }, { "clearAutomation", "process" },
            { "setModifier", "process" }, { "clearModifier", "process" },
            { "setSamples", "process" }, { "splitClip", "process" }, { "duplicateClip", "process" }, { "setClip", "process" }, { "stretchClip", "process" },
            { "warpClip", "process" }, { "loopClip", "process" }, { "processClip", "process" },
            { "duplicateSection", "collect" }, { "deleteSection", "process" }, { "addMidiFromAudio", "collect" },
            { "addMarker", "collect" }, { "updateMarker", "collect" }, { "deleteMarker", "process" },
            // Feinschliff: zaehlt als Aktion (je Modul/Spur zusammengefasst),
            // damit der Baum zeigt, woran gearbeitet wurde
            { "setParameter", "process" }, { "loadPreset", "process" }, { "setPluginFile", "process" },
            { "setWet", "process" }, { "setEdge", "process" }, { "trackMix", "process" },
            { "freezeTrack", "process" }, { "flattenTrack", "process" }, { "compLane", "process" },
            { "createGroup", "process" }, { "setGroup", "process" }, { "removeGroup", "process" }, { "groupMix", "process" },
        };

        // record zaehlt beim Start (die collect-Aktion kommt beim Stop),
        // createTrack nur mit Instrument
        if (op == "record")
        {
            auto on = cmd.getProperty ("on", {});
            return (on.isVoid() ? ! isRecording (ctx) : (bool) on) ? "collect" : "";
        }

        if (op == "createTrack")
            return cmd.hasProperty ("instrument") ? "collect" : "";

        // Plugin-Fenster: nur das Schliessen nach einer Aenderung im Editor
        if (op == "pluginWindow")
            return closesChangedPluginWindow (ctx, cmd) ? "process" : "";

        auto it = kinds.find (op);
        return it != kinds.end() ? it->second : juce::String();
    }

    namespace
    {
        // Clip-Bearbeitung beim Sammeln ist Arrangieren, keine neue Variante:
        // in einem collect-Node starten diese process-Ops keinen Kind-Node
        // (Praxistest 1: jedes duplicateClip zwischen zwei Inserts machte den
        // Baum tiefer). Klang (Plugins, Routing, Automation) branched weiter.
        bool isArrangementEdit (const juce::String& op)
        {
            static const juce::StringArray ops { "deleteClip", "undoDelete", "trimClip", "moveClip", "splitClip",
                                                 "duplicateClip", "setMidiNotes", "editNotes", "setControllers", "quantizeClip", "setClip", "stretchClip", "warpClip", "loopClip", "deleteSection", "compLane", "deleteMarker" };
            return ops.contains (op);
        }

        // ---- Fokus: woran eine Op arbeitet ------------------------------
        // "t<i>" = Gruppenspur i (Noten, Clips, Pegel, die Module ihrer
        // Kette), "song" = Sections, Master-Kette, Module mehrerer Spuren,
        // "new" = eine Spur, die erst entsteht, "" = ohne Fokus. Praxistest
        // 1d: ein Node hielt 24 Aktionen ueber alle Spuren — ein Wechsel des
        // Fokus legt jetzt einen Kind-Node an.
        juce::String trackFocus (te::Edit& edit, int ti)
        {
            auto tracks = userTracks (edit);

            if (ti < 0 || ti >= tracks.size())
                return "new";

            auto* g = laneGroupOf (*tracks[ti]) != nullptr ? laneGroupOf (*tracks[ti]) : tracks[ti];
            return "t" + juce::String (tracks.indexOf (g));
        }

        juce::String nodeFocus (te::Edit& edit, const juce::String& id)
        {
            if (id == "main" || id.startsWithChar ('g'))   // eine Gruppe gehoert mehreren Spuren
                return "song";

            if (id.startsWithChar ('t'))
                return trackFocus (edit, id.substring (1).getIntValue());

            if (! id.startsWithChar ('p') || isMasterModule (edit, id))
                return id.startsWithChar ('p') ? "song" : "";

            // Welche Spuren erreichen das Modul? Eine = ihr Fokus, mehrere = Song
            juce::StringArray feeders;
            auto tracks = userTracks (edit);

            for (int i = 0; i < tracks.size(); ++i)
            {
                if (laneGroupOf (*tracks[i]) != nullptr)
                    continue;

                juce::StringArray seen, todo { "t" + juce::String (i) };

                while (! todo.isEmpty())
                {
                    auto n = todo[0];
                    todo.remove (0);

                    if (n == "main" || seen.contains (n))
                        continue;

                    seen.add (n);

                    if (n == id)
                    {
                        feeders.add ("t" + juce::String (i));
                        break;
                    }

                    todo.addArray (targetsOf (edit, findRouteNode (edit, n)));
                }
            }

            return feeders.size() == 1 ? feeders[0] : feeders.size() > 1 ? juce::String ("song") : juce::String();
        }

        juce::String focusOf (DawdContext& ctx, const juce::String& op, const juce::var& cmd)
        {
            auto& edit = *ctx.edit;
            static const juce::StringArray songOps { "addSection", "updateSection", "moveSection", "duplicateSection", "deleteSection", "favoriteSection",
                                                     "addMarker", "updateMarker", "deleteMarker" };

            if (songOps.contains (op))
                return "song";

            if (op == "createTrack")
                return "new";

            if (op == "createGroup" || op == "removeGroup" || op == "groupMix" || (op == "setChain" && cmd.hasProperty ("group")))
                return "song";

            if (op == "addPlugin")
            {
                if (cmd.getProperty ("at", {}).toString() == "master")
                    return "song";

                if (cmd.hasProperty ("after"))
                    return nodeFocus (edit, cmd.getProperty ("after", {}).toString());

                return cmd.hasProperty ("track") ? trackFocus (edit, (int) cmd.getProperty ("track", -1)) : juce::String();
            }

            if (op == "connect" || op == "disconnect" || op == "setEdge")
                return nodeFocus (edit, cmd.getProperty ("from", {}).toString());

            if (op == "setAutomation" || op == "clearAutomation" || op == "setModifier" || op == "clearModifier")
                return nodeFocus (edit, cmd.getProperty ("param", {}).toString().upToFirstOccurrenceOf (".", false, false));

            if (op == "addMidiFromAudio")
                return trackFocus (edit, (int) cmd.getProperty ("insertTrack", -1));

            if (cmd.hasProperty ("plugin"))
                return nodeFocus (edit, cmd.getProperty ("plugin", {}).toString());

            if (cmd.hasProperty ("track"))
                return trackFocus (edit, (int) cmd.getProperty ("track", -1));

            return {};
        }

        // Gefrorene Spur (ops/Freeze.cpp): Material und Kette stecken im
        // gerenderten Audio — eine Op daran waere nicht zu hoeren. Frei
        // bleiben Freeze selbst, Abhoeren, Fader/Pan samt Automation und der
        // Pegel der Kanten hinter dem letzten Modul der Kette.
        juce::String frozenGuard (DawdContext& ctx, const juce::String& op, const juce::var& cmd)
        {
            static const juce::StringArray free { "freezeTrack", "flattenTrack", "trackMix", "favoriteClip", "setGroup" };   // setGroup: Kanten hinter der Kette
            static const juce::StringArray noAction { "laneActive", "armTrack", "assignInput" };

            if (ctx.edit == nullptr || free.contains (op) || (actionKindOf (ctx, op, cmd).isEmpty() && ! noAction.contains (op))
                || clipTracks (*ctx.edit).size() == userTracks (*ctx.edit).size())   // keine Freeze-Spur
                return {};

            const auto param = cmd.getProperty ("param", {}).toString();

            if ((op == "setAutomation" || op == "clearAutomation" || op == "setModifier" || op == "clearModifier")
                && param.startsWithChar ('t')
                && (param.endsWith (".volume") || param.endsWith (".pan")))
                return {};

            const auto focus = focusOf (ctx, op, cmd);

            if (! focus.startsWithChar ('t'))
                return {};

            auto err = frozenTrackError (ctx, focus.substring (1).getIntValue());

            if (err.isNotEmpty() && op == "setEdge")
            {
                juce::StringArray complex, sends;
                auto chain = ownChainOf (*ctx.edit, focus, complex, sends);

                if (! chain.isEmpty() && cmd.getProperty ("from", {}).toString() == chain[chain.size() - 1])
                    return {};
            }

            return err;
        }

        // Label eines Fokus-Nodes: "Bass: Sound", "Song: Arrangement" (englisch,
        // Labels gespeicherter Projekte bleiben, wie sie sind)
        juce::String focusLabel (DawdContext& ctx, const juce::String& op, const juce::String& focus)
        {
            static const juce::StringArray notes { "addClip", "addMidiClip", "setMidiNotes", "editNotes", "setControllers", "quantizeClip", "deleteClip", "trimClip", "moveClip",
                                                   "splitClip", "duplicateClip", "setClip", "stretchClip", "warpClip", "loopClip", "addMidiFromAudio", "createTrack", "pickInstrument", "compLane" };
            static const juce::StringArray mix { "trackMix", "setWet", "setEdge", "connect", "disconnect", "createGroup", "setGroup", "removeGroup", "groupMix" };
            static const juce::StringArray song { "addSection", "updateSection", "moveSection", "duplicateSection", "deleteSection",
                                                  "addMarker", "updateMarker", "deleteMarker" };
            auto what = song.contains (op) ? juce::String ("Arrangement") : notes.contains (op) ? juce::String ("Notes")
                      : mix.contains (op) ? juce::String ("Mix") : juce::String ("Sound");
            juce::String who = "Song";

            if (focus.startsWithChar ('t'))
            {
                auto tracks = userTracks (*ctx.edit);
                auto i = focus.substring (1).getIntValue();
                who = i >= 0 && i < tracks.size() ? tracks[i]->getName() : focus;
            }
            else if (focus == "new")
                who = juce::String ("New track");

            return who + ": " + what;
        }

        // Mix-Ebene (Praxistest 2/3): Fader/Pan und Clip-Gain sind Balance
        // zwischen den Spuren — ein Node "Song: Mix" statt je Spur einer
        // (fuenf trackMix in einem batch ergaben fuenf Nodes; ein trackMix
        // landete still im Noten-Node seiner Spur). Nur im Klang-Node
        // derselben Spur bleibt er dort (Feinschliff nach dem Klangbau).
        // Nur fuer den Baum: der Freeze-Guard prueft weiter die Spur (focusOf).
        bool isMixEdit (const juce::String& op, const juce::var& cmd)
        {
            if (op == "trackMix")
                return cmd.hasProperty ("volumeDb") || cmd.hasProperty ("pan");

            if (op != "setClip" || ! cmd.hasProperty ("gainDb"))
                return false;

            for (auto key : { "fadeIn", "fadeOut", "fadeCurve", "transpose", "reverse", "normalize", "crossfade" })
                if (cmd.hasProperty (key))
                    return false;

            return true;
        }

        // Auto-Branch: der aktuelle Node ist acted und hat eine andere Sorte
        // (die seiner ersten Aktion) oder einen anderen Fokus — die Aktion
        // bekommt einen Kind-Node. Rueckgabe: neue Node-Id, 0 = kein Branch
        // noetig, -1 = Fehler (err)
        int autoBranchFor (DawdContext& ctx, const juce::String& op, const juce::String& kind, const juce::String& focus,
                           bool mixEdit, juce::var& err)
        {
            const auto labelOp = mixEdit ? juce::String ("trackMix") : op;

            if (kind.isEmpty() || ! ctx.tree || ! ctx.tree->autoBranch || ctx.edit == nullptr)
                return 0;

            auto* cur = ctx.tree->find (ctx.tree->current);

            if (cur == nullptr || ! cur->isActed())
                return 0;

            const auto firstKind = cur->actions.getReference (0).kind;
            bool kindChange = firstKind != kind;

            if (firstKind == "collect" && kind == "process" && isArrangementEdit (op))
                kindChange = false;

            // Clip ersetzen = deleteClip + addMidiClip: ein Node aus reinen
            // Clip-Edits nimmt das Einfuegen auf derselben Spur mit auf
            // (Praxistest 3: deleteClip legte einen eigenen Node an, das
            // Einfuegen danach noch einen)
            if (firstKind == "process" && kind == "collect" && op != "addSection")
            {
                bool onlyClipEdits = true;

                for (auto& a : cur->actions)
                    onlyClipEdits = onlyClipEdits && isArrangementEdit (a.op);

                if (onlyClipEdits)
                    kindChange = false;
            }

            // Song-Ebene: "Song: Mix" (Pegel) und die uebrige Song-Arbeit
            // (Master-Kette, geteilte Module) sind getrennte Schritte
            if (focus == "song" && cur->focus == "song")
            {
                const auto& a0 = cur->actions.getReference (0);

                if ((a0.op == "trackMix" || isMixEdit (a0.op, a0.params)) != mixEdit)
                    kindChange = true;
            }

            const bool focusChange = cur->focus.isNotEmpty() && focus.isNotEmpty() && focus != cur->focus;

            if (! kindChange && ! focusChange)
                return 0;

            // Nur, wenn der geladene Edit der des Nodes ist — ein loser Edit
            // (newEdit) gehoert zu keinem Node, ein Branch wuerde ihn verwerfen
            if (te::EditFileOperations (*ctx.edit).getEditFile()
                    != ctx.tree->projectDir.getChildFile (cur->file))
                return 0;

            auto label = focus.isNotEmpty() ? focusLabel (ctx, labelOp, focus) : kind + " " + juce::String (ctx.tree->nextId);
            auto o = new juce::DynamicObject();
            o->setProperty ("label", label);
            auto r = opTreeBranch (ctx, juce::var (o));

            if (! (bool) r.getProperty ("ok", false))
            {
                err = r;
                return -1;
            }

            auto id = (int) r.getProperty ("node", 0);

            if (auto* n = ctx.tree->find (id))
            {
                n->autoCreated = true;
                ctx.tree->save();
            }

            return id;
        }
    }

    juce::var executeOp (DawdContext& ctx, const juce::String& op, const juce::var& cmdIn)
    {
        auto* def = findOp (op);
        juce::var result;

        // Ladestand (GET /progress): jede Op ausser den Anzeige-Polls ist
        // "busy", solange sie den Message-Thread haelt
        const bool displayPoll = op == "position" || op == "inputLevel" || op == "levels" || op == "interfaceMeters";

        struct BusyScope
        {
            LoadStatus* s;
            BusyScope (LoadStatus* st, const juce::String& o) : s (st) { if (s != nullptr) s->begin (o); }
            ~BusyScope() { if (s != nullptr) s->end(); }
        } busy (displayPoll ? nullptr : &ctx.load, op);

        // Modul-Namen (renamePlugin) als Adresse: plugin/from/to/after mit
        // einem Namen werden zur Id — jede Op versteht Namen, Node-Aktionen
        // und Log speichern weiter Ids
        auto cmd = cmdIn;

        if (ctx.edit != nullptr && cmdIn.isObject())
            for (auto key : { "track", "insertTrack" })
                if (auto v = cmdIn.getProperty (key, {}); v.isString() && ! v.toString().containsOnly ("-0123456789"))
                    if (auto idx = trackIndexFor (*ctx.edit, v.toString()); idx >= 0)
                    {
                        if (cmd.getDynamicObject() == cmdIn.getDynamicObject())
                            cmd = cmdIn.clone();

                        cmd.getDynamicObject()->setProperty (key, idx);
                    }

        // Clip ueber den Spurnamen: liegt der Clip nicht auf der Spur, aber
        // auf einer ihrer Lanes ("Bass.1"), gilt die Lane (Praxistest 1e:
        // setMidiNotes/moveClip/clipNotes {track: "Bass"} fanden den Clip
        // auf "Bass.1" nicht)
        if (ctx.edit != nullptr && cmd.isObject() && op != "renameTrack" && op != "createTrack")
            if (auto t = cmd.getProperty ("track", {}), n = cmd.getProperty ("name", {}); (t.isInt() || t.isDouble()) && n.isString())
            {
                auto tracks = userTracks (*ctx.edit);
                const int idx = (int) t;

                if (idx >= 0 && idx < tracks.size() && findClipByName (*tracks[idx], n.toString()) == nullptr)
                    for (int j = 0; j < tracks.size(); ++j)
                        if (laneGroupOf (*tracks[j]) == tracks[idx] && findClipByName (*tracks[j], n.toString()) != nullptr)
                        {
                            if (cmd.getDynamicObject() == cmdIn.getDynamicObject())
                                cmd = cmdIn.clone();

                            cmd.getDynamicObject()->setProperty ("track", j);
                            break;
                        }
            }

        if (ctx.edit != nullptr && cmdIn.isObject())
            for (auto key : { "plugin", "from", "to", "after", "before" })
                if (auto v = cmdIn.getProperty (key, {}); v.isString())
                    if (auto id = moduleIdFor (*ctx.edit, v.toString()); id.isNotEmpty() && id != v.toString())
                    {
                        if (cmd.getDynamicObject() == cmdIn.getDynamicObject())
                            cmd = cmdIn.clone();

                        cmd.getDynamicObject()->setProperty (key, id);
                    }

        // Gruppen-Namen ebenso (Gruppen/Submix)
        if (ctx.edit != nullptr && cmdIn.isObject())
            for (auto key : { "group", "from", "to", "after" })
                if (auto v = cmd.getProperty (key, {}); v.isString() && moduleIdFor (*ctx.edit, v.toString()).isEmpty())
                    if (auto id = groupIdFor (*ctx.edit, v.toString()); id.isNotEmpty() && id != v.toString())
                    {
                        if (cmd.getDynamicObject() == cmdIn.getDynamicObject())
                            cmd = cmdIn.clone();

                        cmd.getDynamicObject()->setProperty (key, id);
                    }

        // Plugin-Sandbox: was die Plugins im Host-Prozess selbst gemeldet
        // haben (Editor, Modulation, neue Namen), vor der Op uebernehmen —
        // sonst saehe die Op einen Stand bis zu 50 ms alt (Timer)
        if (ctx.edit != nullptr)
            syncSandboxedPlugins (*ctx.edit);

        rememberSettledNames();

        // Plugin-Fenster schliessen nach einer Aenderung im Editor: dann
        // zaehlt die read-only Op wie eine Mutation (Version, Aktion, Undo,
        // Auto-Branch); umschalten heisst hier schliessen
        const bool windowChange = def != nullptr && op == "pluginWindow" && ctx.edit != nullptr
                               && actionKindOf (ctx, op, cmd).isNotEmpty();

        if (windowChange && ! cmd.hasProperty ("open"))
        {
            if (cmd.getDynamicObject() == cmdIn.getDynamicObject())
                cmd = cmdIn.clone();

            cmd.getDynamicObject()->setProperty ("open", false);
        }

        const bool mutating = def != nullptr && (def->mutating || windowChange) && ! isSettingsRead (op, cmd);

        if (def == nullptr)
        {
            // Unbekannte Ops zaehlen wie bisher als Mutation (nicht in der
            // Read-only-Liste) und landen im Log.
            ++ctx.stateVersion;
            result = resultObj (false, { { "error", "unknown op: " + op } });
        }
        else
        {
            // Wie bisher: die Version zaehlt vor dem Handler hoch — auch wenn
            // ein Guard abbricht. Nur Read-only-Ops zaehlen nicht.
            if (mutating)
                ++ctx.stateVersion;

            // Pegel-Clients vor jeder Mutation abmelden — die Op darf Tracks
            // oder den Edit loeschen, der Measurer haelt rohe Zeiger
            if (mutating)
                clearMeters (ctx);

            // Guards in fester Reihenfolge: Existenz (edit/tree), dann Aufnahme.
            // Die Meldungen sind Teil der API — Tests und UI haengen daran.
            if (def->needsEdit && ctx.edit == nullptr)
                result = resultObj (false, { { "error", "no edit" } });
            else if (def->needsTree && ! ctx.tree)
                result = resultObj (false, { { "error", "no tree" } });
            else if (def->needsTreeAndEdit && (! ctx.tree || ctx.edit == nullptr))
                result = resultObj (false, { { "error", "no tree / no edit" } });
            else if (def->needsTreeOrEdit && ! ctx.tree && ctx.edit == nullptr)
                result = resultObj (false, { { "error", "no tree open" } });
            else if (def->blockWhileRecording && isRecording (ctx))
                result = resultObj (false, { { "error", "recording in progress" } });
            else if (auto frozen = def->mutating ? frozenGuard (ctx, op, cmd) : juce::String(); frozen.isNotEmpty())
                result = resultObj (false, { { "error", frozen } });
            else
            {
                // Der Baum baut sich aus den Aktionen: andere Sorte als der
                // acted Node = vorher ein Kind-Node (treeSettings autoBranch)
                // Undo: Stand vor der Op (vor einem Auto-Branch) merken
                const bool undoable = mutating && isUndoableOp (op) && ! isListeningOnly (op, cmd) && canSnapshot (ctx);
                UndoSnap before;
                auto kind = actionKindOf (ctx, op, cmd);   // vor dem Handler (record aendert sie)
                auto focus = kind.isNotEmpty() && ctx.edit != nullptr ? focusOf (ctx, op, cmd) : juce::String();
                const bool mixEdit = isMixEdit (op, cmd);

                if (mixEdit && focus.startsWithChar ('t') && ctx.tree)
                {
                    auto* cur = ctx.tree->find (ctx.tree->current);
                    const bool soundNodeOfTrack = cur != nullptr && cur->isActed() && cur->focus == focus
                                               && cur->actions.getReference (0).kind == "process"
                                               && ! isArrangementEdit (cur->actions.getReference (0).op);

                    if (! soundNodeOfTrack)
                        focus = "song";
                }
                const auto windowId = cmd.getProperty ("plugin", {}).toString();

                // Die Aenderung im Editor ist schon im Plugin: fuer Vorher-Stand
                // und Eltern-Node den Zustand beim Oeffnen zurueck, der neue
                // kommt im Handler (im evtl. neuen Edit) wieder hinein
                if (windowChange)
                    beginPluginWindowChange (ctx, windowId);

                if (undoable)
                    before = takeSnapshot (ctx);

                juce::var branchError;
                auto parent = ctx.tree ? ctx.tree->current : 0;
                auto autoNode = autoBranchFor (ctx, op, kind, focus, mixEdit && focus == "song", branchError);

                if (autoNode < 0)
                    result = branchError;
                else
                {
                    // Hinweise auf gesperrtes Material je Op (batch ruft
                    // executeOp verschachtelt: die aeusseren bleiben)
                    auto outerNotes = std::move (ctx.licenceNotes);
                    ctx.licenceNotes.clear();
                    result = def->fn (ctx, cmd);

                    if (! ctx.licenceNotes.isEmpty())
                        if (auto* o = result.getDynamicObject())
                            o->setProperty ("licenceNote", ctx.licenceNotes.joinIntoString ("; "));

                    ctx.licenceNotes = std::move (outerNotes);
                }

                if (windowChange)
                    applyPluginWindowChange (ctx, windowId);   // Handler lief nicht: Aenderung nicht verlieren

                // Fader nach der Kette: wandert mit, wenn die Op die Kette aenderte
                if (mutating && ctx.edit != nullptr && (bool) result.getProperty ("ok", false))
                {
                    syncFaders (*ctx.edit);
                    pruneSidechains (*ctx.edit);   // Quelle geloescht: Schluessel weg
                    refreshAutoCrossfades (*ctx.edit);   // Blenden der Ueberlappungen nach Laden/Verschieben
                }

                // Zieht sich das Material zurueck (Chops aus einem ganzen
                // Stem), faellt die Auto-Section dieses Nodes wieder weg
                // oder schrumpft (Praxistest 2) — samt ihrer Aktion
                static const juce::StringArray shrinking { "deleteClip", "trimClip", "moveClip", "stretchClip", "warpClip", "loopClip" };

                if (mutating && ctx.edit != nullptr && ctx.tree && shrinking.contains (op) && (bool) result.getProperty ("ok", false))
                    if (auto* n = ctx.tree->find (ctx.tree->current))
                    {
                        juce::StringArray names;
                        juce::Array<int> ids;

                        for (auto& a : n->actions)
                            if (a.op == "addSection" && (bool) a.params.getProperty ("auto", false))
                            {
                                names.add (a.params.getProperty ("name", {}).toString());
                                ids.add (a.id);
                            }

                        if (auto back = retractAutoSection (*ctx.edit, names); back.isObject())
                        {
                            if ((bool) back.getProperty ("removed", false))
                                removeNodeAction (ctx, ids[names.indexOf (back.getProperty ("name", {}).toString())]);

                            saveCurrentEdit (ctx);

                            if (auto* o = result.getDynamicObject())
                                o->setProperty ((bool) back.getProperty ("removed", false) ? "autoSectionRemoved" : "autoSectionShrunk", back);
                        }
                    }

                if (autoNode > 0)
                {
                    if ((bool) result.getProperty ("ok", false))
                    {
                        auto info = new juce::DynamicObject();
                        info->setProperty ("node", autoNode);
                        info->setProperty ("parent", parent);
                        info->setProperty ("kind", kind);
                        info->setProperty ("label", ctx.tree->find (autoNode)->label);

                        if (auto* o = result.getDynamicObject())
                            o->setProperty ("autoNode", juce::var (info));
                    }
                    else
                    {
                        // Fehlgeschlagen (vor Seiteneffekten): den leeren
                        // Kind-Node wieder weg, zurueck zum Vorgaenger
                        auto del = new juce::DynamicObject();
                        del->setProperty ("node", autoNode);
                        opTreeDelete (ctx, juce::var (del));
                    }
                }

                // Neue Plugin-Instanzen (Laden, Checkout, Undo, Auto-Node,
                // addPlugin) und ein Modul mit neuem Zustand ohne Graph
                // rechnen kurz, damit sie ihre echten Parameter-Namen melden
                // (Surge XT, Praxistest 1n P3)
                if (mutating && ctx.edit != nullptr)
                {
                    te::Plugin* touched = nullptr;

                    if (op == "loadPreset" || op == "setPluginFile")
                        if (auto* mt = findModuleTrack (*ctx.edit, cmd.getProperty ("plugin", {}).toString()))
                            touched = modulePlugin (*mt);

                    settleParameterNames (*ctx.edit, touched);
                }

                if ((bool) result.getProperty ("ok", false) && ctx.tree && kind.isNotEmpty())
                {
                    // Feinschliff als Aktion, zusammengefasst je Modul/Spur
                    static const juce::StringArray refine { "setParameter", "loadPreset", "setPluginFile", "setWet", "setEdge", "trackMix", "groupMix", "pluginWindow" };

                    if (refine.contains (op))
                    {
                        auto params = new juce::DynamicObject();

                        for (auto key : { "plugin", "track", "from", "to", "group" })
                            if (cmd.hasProperty (key))
                                params->setProperty (key, cmd.getProperty (key, {}));

                        recordNodeAction (ctx, "process", op, juce::var (params), {}, true);
                    }

                    // Op an einem Modul mit offenem Fenster: ihre Aenderung
                    // ist gebucht, zaehlt beim Schliessen nicht als Editor-Aenderung
                    if (op != "pluginWindow" && ctx.edit != nullptr)
                        for (auto id : { cmd.getProperty ("plugin", {}).toString(),
                                         cmd.getProperty ("param", {}).toString().upToFirstOccurrenceOf (".", false, false) })
                            if (id.isNotEmpty())
                                pluginWindowSettled (ctx, id);

                    // Fokus des Nodes setzt seine erste Aktion; eine neue
                    // Spur ist erst nach der Op bekannt (Index im Ergebnis)
                    if (auto* n = ctx.tree->find (ctx.tree->current); n != nullptr && n->focus.isEmpty() && focus.isNotEmpty())
                    {
                        auto f = focus;

                        if (f == "new")
                        {
                            auto idx = result.getProperty ("laneOf", result.getProperty ("index", result.getProperty ("track", -1)));
                            f = (int) idx >= 0 ? trackFocus (*ctx.edit, (int) idx) : juce::String();
                        }

                        n->focus = f;

                        // Auto-Node einer neuen Spur: Label mit ihrem Namen
                        if (focus == "new" && n->autoCreated && f.isNotEmpty())
                        {
                            n->label = focusLabel (ctx, op, f);

                            if (auto an = result.getProperty ("autoNode", {}); an.isObject())
                                an.getDynamicObject()->setProperty ("label", n->label);
                        }

                        ctx.tree->save();
                    }
                }

                if ((bool) result.getProperty ("ok", false))
                {
                    // undoDelete, das einen leeren Auto-Node wegraeumte, ist
                    // selbst ein Undo — kein eigener Schritt
                    if (undoable && canSnapshot (ctx) && ! result.hasProperty ("removedNode"))
                    {
                        UndoStep step;
                        step.op = op;
                        step.before = before;
                        step.after = takeSnapshot (ctx);
                        step.mergeKey = cmd.getProperty ("osc", {}).toString();   // OSC-Fader: eine Geste, ein Schritt

                        if (autoNode > 0)
                            if (auto* n = ctx.tree->find (autoNode))
                                step.createdNode = *n;

                        // bounceNode legt seinen Node selbst an (Basis = der Bounce)
                        if (op == "bounceNode")
                            if (auto* n = ctx.tree->find ((int) result.getProperty ("node", 0)))
                            {
                                step.createdNode = *n;
                                step.createdBase = step.after.edit;
                            }

                        pushUndo (ctx, std::move (step));
                    }

                    if (clearsUndo (op))
                        clearUndo (ctx);
                }
            }

            // Absturzschutz: erfolgreiche Mutationen landen gebuendelt auf der
            // Platte, die Session-Datei folgt dem offenen Projekt.
            // play aendert den Edit nicht: kein Speichern (je Play sonst
            // 50-100 ms Message-Thread kurz nach dem Start, Plugin-Zustaende)
            if (mutating && op != "play" && ! isListeningOnly (op, cmd) && (bool) result.getProperty ("ok", false))
                scheduleAutosave (ctx);

            updateSessionFile (ctx);
        }

        // Jede Antwort traegt die Version — das UI vergleicht sie mit dem Stand,
        // den es selbst gerendert hat, und zeigt bei Abweichung den Refresh-Button.
        if (auto* o = result.getDynamicObject())
            o->setProperty ("version", ctx.stateVersion);

        // Anzeige-Polls (Playhead, Eingangspegel) nicht mitloggen — sonst
        // waechst events.jsonl um ~10 Eintraege/s
        // batch nicht: seine Befehle stehen einzeln im Log
        // jobStatus {wait} fragt alle 100 ms ab (main.cpp, waitPoll): nur die
        // letzte Antwort zaehlt, mit wait wie gesendet
        if (op == "jobStatus" && cmd.hasProperty ("waitPoll"))
        {
            if ((bool) result.getProperty ("ok", false) && result.getProperty ("status", {}).toString() == "running"
                && ! (bool) cmd.getProperty ("waitLast", false))
                return result;

            auto logged = cmd.clone();
            logged.getDynamicObject()->setProperty ("wait", cmd.getProperty ("waitPoll", {}));
            logged.getDynamicObject()->removeProperty ("waitPoll");
            logged.getDynamicObject()->removeProperty ("waitLast");
            appendEvent (ctx, op, logged, result);
            return result;
        }

        if (op != "position" && op != "inputLevel" && op != "levels" && op != "batch")
            appendEvent (ctx, op, cmd, result);

        return result;
    }
}
