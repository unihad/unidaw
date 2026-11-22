#pragma once

#include "../Dawd.h"

// Alle Op-Handler je Familie. Die Registry (OpRegistry.h) kennt Name,
// Familie, Guards und Handler-Zuordnung; hier stehen nur die Signaturen.
namespace dawd
{
    // Session: Projekt auf/zu, Verzeichnisse, Daemon-Lebenszyklus
    juce::var opPing (DawdContext& ctx, const juce::var& cmd);
    juce::var opListOps (DawdContext& ctx, const juce::var& cmd);
    juce::var opAbout (DawdContext& ctx, const juce::var& cmd);
    juce::var opNewEdit (DawdContext& ctx, const juce::var& cmd);
    juce::var opCreateTrack (DawdContext& ctx, const juce::var& cmd);
    juce::var opRenameTrack (DawdContext& ctx, const juce::var& cmd);
    juce::var opListDir (DawdContext& ctx, const juce::var& cmd);
    juce::var opCreateDir (DawdContext& ctx, const juce::var& cmd);
    juce::var opQuit (DawdContext& ctx, const juce::var& cmd);
    juce::var opBatch (DawdContext& ctx, const juce::var& cmd);

    // Tree: Song-Baum (Seed, Varianten, Favoriten, Status)
    juce::var opTreeState (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeSeed (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeOpen (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeClose (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeBranch (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeCheckout (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeReset (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeDelete (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeTerminateFavorite (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeLabel (DawdContext& ctx, const juce::var& cmd);
    juce::var opBounceNode (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeNote (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeSettings (DawdContext& ctx, const juce::var& cmd);
    juce::var opUndo (DawdContext& ctx, const juce::var& cmd);
    juce::var opRedo (DawdContext& ctx, const juce::var& cmd);

    // Compact: nicht favorisiertes Material global rauswerfen (Trockenlauf + Lauf)
    juce::var opTreeCompactPreview (DawdContext& ctx, const juce::var& cmd);
    juce::var opTreeCompact (DawdContext& ctx, const juce::var& cmd);

    // Section: Songstruktur
    juce::var opAddSection (DawdContext& ctx, const juce::var& cmd);
    juce::var opUpdateSection (DawdContext& ctx, const juce::var& cmd);
    juce::var opMoveSection (DawdContext& ctx, const juce::var& cmd);
    juce::var opAddMarker (DawdContext& ctx, const juce::var& cmd);      // Marker: benannte Zeitpunkte
    juce::var opUpdateMarker (DawdContext& ctx, const juce::var& cmd);
    juce::var opDeleteMarker (DawdContext& ctx, const juce::var& cmd);

    // Media: Audio und MIDI (record, Insert, Monitor, Take, Clip-Edit)
    juce::var opRecord (DawdContext& ctx, const juce::var& cmd);
    juce::var opArmTrack (DawdContext& ctx, const juce::var& cmd);
    juce::var opMonitor (DawdContext& ctx, const juce::var& cmd);
    juce::var opInputGain (DawdContext& ctx, const juce::var& cmd);
    juce::var opInputLevel (DawdContext& ctx, const juce::var& cmd);
    juce::var opListInputs (DawdContext& ctx, const juce::var& cmd);
    juce::var opAssignInput (DawdContext& ctx, const juce::var& cmd);
    juce::var opClearInput (DawdContext& ctx, const juce::var& cmd);
    juce::var opAddClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opAddMidiClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opDeleteClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opUndoDelete (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetMidiNotes (DawdContext& ctx, const juce::var& cmd);
    juce::var opEditNotes (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetControllers (DawdContext& ctx, const juce::var& cmd);
    juce::var opExportMidi (DawdContext& ctx, const juce::var& cmd);
    juce::var opQuantizeClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opTrimClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opMoveClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opLastTake (DawdContext& ctx, const juce::var& cmd);

    // Select: Favoriten auf Material (Clips und Sections)
    juce::var opFavoriteClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opFavoriteSection (DawdContext& ctx, const juce::var& cmd);

    // Processing: Plugin-Module, Routing-Matrix, Parameter, Render/Analyse
    juce::var opRender (DawdContext& ctx, const juce::var& cmd);
    juce::var opRenderNode (DawdContext& ctx, const juce::var& cmd);
    juce::var opPeak (DawdContext& ctx, const juce::var& cmd);
    juce::var opClipPeaks (DawdContext& ctx, const juce::var& cmd);
    juce::var opDump (DawdContext& ctx, const juce::var& cmd);
    juce::var opListPlugins (DawdContext& ctx, const juce::var& cmd);
    juce::var opAddPlugin (DawdContext& ctx, const juce::var& cmd);
    juce::var opRemovePlugin (DawdContext& ctx, const juce::var& cmd);
    juce::var opLinkPlugin (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetChain (DawdContext& ctx, const juce::var& cmd);
    juce::var opConnect (DawdContext& ctx, const juce::var& cmd);
    juce::var opDisconnect (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetEdge (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetWet (DawdContext& ctx, const juce::var& cmd);
    juce::var opRouting (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetParameter (DawdContext& ctx, const juce::var& cmd);
    juce::var opRenderStems (DawdContext& ctx, const juce::var& cmd);
    juce::var opRenderBatch (DawdContext& ctx, const juce::var& cmd);
    juce::var opPickInstrument (DawdContext& ctx, const juce::var& cmd);

    // Instrument-Modul fuer eine MIDI-Spur (createTrack instrument,
    // pickInstrument): Plugin aufloesen, Spur -> Instrument -> Main. Keine
    // Node-Aktion (die Aufrufer buchen collect). Fehler: {ok false, error}.
    juce::var addInstrumentToTrack (DawdContext& ctx, int trackIndex, const juce::var& cmd);
    juce::var opAnalyze (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetSamples (DawdContext& ctx, const juce::var& cmd);
    juce::var opSplitClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opStretchClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opWarpClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opLoopClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opDuplicateClip (DawdContext& ctx, const juce::var& cmd);
    juce::var opDuplicateSection (DawdContext& ctx, const juce::var& cmd);
    juce::var opDeleteSection (DawdContext& ctx, const juce::var& cmd);
    juce::var opSampleLibraries (DawdContext& ctx, const juce::var& cmd);
    juce::var opFindSamples (DawdContext& ctx, const juce::var& cmd);
    juce::var opListSamples (DawdContext& ctx, const juce::var& cmd);
    juce::var opCompareReference (DawdContext& ctx, const juce::var& cmd);

    // Referenzen aus dem Systemaudio (ops/Capture.cpp): Mitschnitt,
    // Songgrenzen, Songs als WAV ablegen
    juce::var opCapture (DawdContext& ctx, const juce::var& cmd);
    juce::var opCaptureStatus (DawdContext& ctx, const juce::var& cmd);
    juce::var opDetectSongBoundaries (DawdContext& ctx, const juce::var& cmd);
    juce::var opSaveCaptureSongs (DawdContext& ctx, const juce::var& cmd);

    // Mix-Bereich (Song, section, start/end; track = nur diese Spur samt
    // Lanes wie ein Stem) als 32-Bit-Float-WAV nach dest rendern — fuer die
    // Analyse-Ops. Bis zu 2 s vor start laufen als Vorlauf mit (Hall,
    // Einschwingen), die Datei enthaelt nur den Bereich. Bereich und
    // gepruefter Spur-Index (-1 = alle) zurueck. Fehlertext oder "".
    juce::String renderMixRange (DawdContext& ctx, const juce::var& cmd, const juce::File& dest,
                                 double& start, double& end, int& trackIndex);

    // Rendern mit fertigen Renderer-Parametern (wie render: Plugins
    // einschwingen, Abhoer-Hilfen aus). Fehlertext oder "".
    juce::String renderWithParams (DawdContext& ctx, te::Renderer::Parameters& r);

    // Edit eines anderen Nodes nur zum Rendern laden (Routing, Tempos,
    // Dateipfade relativ zur Edit-Datei) — das offene Projekt bleibt, wie es
    // ist (renderNode, bounceNode, renderBatch). nullptr, wenn es nicht laedt.
    std::unique_ptr<te::Edit> loadEditForRender (DawdContext& ctx, const juce::File& editFile);

    // Mix eines Edits mit den Render-Optionen von render (Format, Rate,
    // Bittiefe, section/start/end, tail, normalize, force …) nach dest, immer
    // Stereo. Ergebnis wie render (file, bytes, …, clippedSamples) oder
    // {ok: false, error}.
    juce::var renderEditMix (DawdContext& ctx, te::Edit& edit, const juce::var& opts, const juce::File& dest);

    // Stems eines Edits wie renderStems (cmd: Render-Optionen, master,
    // groups) in dir — Ergebnis wie renderStems
    juce::var renderStemsOf (DawdContext& ctx, te::Edit& edit, const juce::var& cmd, const juce::File& dir);

    // Rate fuer interne Renders (Freeze, Bounce): die des Geraets, wenn sie
    // eine Render-Rate ist, sonst 44,1 kHz
    double freezeSampleRate (DawdContext& ctx);

    // Freeze/Flatten (Familie processing, ops/Freeze.cpp). frozenTrackError:
    // Meldung, wenn die Spur gefroren ist (executeOp sperrt damit Ops an
    // ihrem Material und ihrer Kette), sonst "".
    juce::var opFreezeTrack (DawdContext& ctx, const juce::var& cmd);
    juce::var opFlattenTrack (DawdContext& ctx, const juce::var& cmd);
    juce::String frozenTrackError (DawdContext& ctx, int trackIndex);

    // Clip-FX (Familie processing, ops/ClipFx.cpp): processClip rechnet einen
    // Audio-Clip offline durch Module; Kind DAWDFX am Clip haelt das
    // Original. clipFxOf: Feld fuer state (void ohne Bearbeitung)
    juce::var opProcessClip (DawdContext& ctx, const juce::var& cmd);
    extern const juce::Identifier fxTreeId;
    bool isClipProcessed (const te::AudioClipBase&);
    juce::var clipFxOf (const te::AudioClipBase&);

    // Gruppen/Submix (Familie processing bzw. view, ops/Groups.cpp): ein
    // Gruppen-Bus in der Routing-Matrix. groupInfo/groupList: Felder fuer
    // state (Timeline, Mixer), groupOfTrack: innerste Gruppe einer Spur ("")
    juce::var opCreateGroup (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetGroup (DawdContext& ctx, const juce::var& cmd);
    juce::var opRemoveGroup (DawdContext& ctx, const juce::var& cmd);
    juce::var opGroupMix (DawdContext& ctx, const juce::var& cmd);
    juce::var groupInfo (te::Edit& edit, const juce::String& id);
    juce::Array<juce::var> groupList (te::Edit& edit);
    juce::String groupOfTrack (te::Edit& edit, int track);

    // Musik-Analyse (Familie processing, ops/Analysis.cpp)
    juce::var opDetectTempo (DawdContext& ctx, const juce::var& cmd);
    juce::var opExtractGroove (DawdContext& ctx, const juce::var& cmd);
    juce::var opDetectKey (DawdContext& ctx, const juce::var& cmd);
    juce::var opRenamePlugin (DawdContext& ctx, const juce::var& cmd);
    juce::var opPluginWindow (DawdContext& ctx, const juce::var& cmd);
    juce::var opPluginWindowSim (DawdContext& ctx, const juce::var& cmd);
    juce::var opReloadPlugin (DawdContext& ctx, const juce::var& cmd);
    juce::var opPluginHosting (DawdContext& ctx, const juce::var& cmd);
    juce::var opDescribeVoice (DawdContext& ctx, const juce::var& cmd);
    juce::var opDescribeTimbre (DawdContext& ctx, const juce::var& cmd);
    juce::var opCompareVoice (DawdContext& ctx, const juce::var& cmd);
    juce::var opCompareStems (DawdContext& ctx, const juce::var& cmd);
    juce::var opDescribeDrums (DawdContext& ctx, const juce::var& cmd);
    juce::var opCompareDrums (DawdContext& ctx, const juce::var& cmd);
    juce::var opAnalyzeStructure (DawdContext& ctx, const juce::var& cmd);
    juce::var opFindRepeats (DawdContext& ctx, const juce::var& cmd);
    juce::var opAudioToMidi (DawdContext& ctx, const juce::var& cmd);
    juce::var opSeparateStems (DawdContext& ctx, const juce::var& cmd);
    juce::var opCompareClasses (DawdContext& ctx, const juce::var& cmd);
    juce::var opDescribeLayers (DawdContext& ctx, const juce::var& cmd);

    // Gespielte Akkorde statt Notenerkennung (audioToMidi/addMidiFromAudio
    // {shape: "chords"}, ops/Voice.cpp): Anschlaege auf dem 16tel-Raster
    // (sichtbare Einsaetze und verdeckte Neuanschlaege), je Anschlag die
    // klingenden Tonklassen in der Kernlage mit kleiner Stimmbewegung, Laenge
    // bis zum naechsten Anschlag bzw. zur Pause, Velocity nach Anschlagstaerke,
    // Strum wie gemessen. Zeiten in Sekunden ab Quellbeginn; info: Ueberblick.
    struct PlayedNote { int pitch = 60; double start = 0.0, length = 0.0; int velocity = 100; };
    juce::String playedChords (DawdContext& ctx, const juce::var& cmd, std::vector<PlayedNote>& out, juce::var& info);
    juce::var opSplitLayer (DawdContext& ctx, const juce::var& cmd);
    juce::var opCompareSeparated (DawdContext& ctx, const juce::var& cmd);

    // Stems einer Datei als Analysematerial (ops/Stems.cpp): file, start?,
    // end? wie separateStems, Ordner unter <config>/stems/. Fehlen Stems des
    // Modells (4 oder 6), wird jetzt getrennt — blockiert, etwa Echtzeit.
    // Fehlertext oder "".
    juce::String fileStemsNow (DawdContext& ctx, const juce::var& cmd, int model, juce::File& dir, bool& cached);
    juce::var opJobStatus (DawdContext& ctx, const juce::var& cmd);
    // Stand eines Jobs als Antwort (job, kind, status, progress, seconds, Ergebnis)
    juce::var jobVar (Job& j);
    juce::var opAddMidiFromAudio (DawdContext& ctx, const juce::var& cmd);

    // Automation (Familie processing, ops/Automation.cpp)
    juce::var opListAutomatable (DawdContext& ctx, const juce::var& cmd);
    juce::var opAutomation (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetAutomation (DawdContext& ctx, const juce::var& cmd);
    juce::var opClearAutomation (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetModifier (DawdContext& ctx, const juce::var& cmd);
    juce::var opClearModifier (DawdContext& ctx, const juce::var& cmd);
    juce::var opListParameters (DawdContext& ctx, const juce::var& cmd);
    juce::var opPluginInfo (DawdContext& ctx, const juce::var& cmd);

    // Klang-Vektoren (CLAP) fuer Suche und Kataloge (ops/Library.cpp):
    // soundQuery baut den Anfrage-Vektor aus text (deutsche Klangworte werden
    // uebersetzt) oder like (Referenz wie bei findSamples); embedSoundFile
    // setzt clap (clap::encode) und sound (Merkmale) fuer analyze {file, embed}
    juce::String soundQuery (DawdContext& ctx, const juce::String& text, const juce::var& like, std::vector<float>& qvec,
                             juce::var& reference, juce::String* english = nullptr);
    juce::String embedSoundFile (DawdContext& ctx, const juce::File& file, double start, double end, juce::DynamicObject& out);

    // Presets (Familie processing, ops/Presets.cpp)
    juce::var opListPresets (DawdContext& ctx, const juce::var& cmd);
    juce::var opLoadPreset (DawdContext& ctx, const juce::var& cmd);
    juce::var opSavePreset (DawdContext& ctx, const juce::var& cmd);
    juce::var opFindPresets (DawdContext& ctx, const juce::var& cmd);

    // Klang nachbauen (Familie processing, ops/SoundMatch.cpp): matchSound
    // sucht Presets zu einem Referenzklang, tuneSound stellt Parameter eines
    // Moduls Richtung Referenz — beide als Job (jobStatus)
    juce::var opMatchSound (DawdContext& ctx, const juce::var& cmd);
    juce::var opTuneSound (DawdContext& ctx, const juce::var& cmd);

    // Zustand eines neuen Plugins wie addPlugin es aufloest (eingebaut oder
    // gescannt, name + format?/uid?) — ungueltig mit error
    juce::ValueTree newPluginState (DawdContext& ctx, const juce::String& name, const juce::String& format,
                                    const juce::String& uid, juce::String& error);

    // Messung eines Parameters aus dem Steckbrief (measured.parameters), sameAs
    // aufgeloest und needs auf die eigene Nummer umgeschrieben (wie pluginInfo)
    juce::var profileMeasurement (const juce::var& measured, const juce::String& name);
    juce::var opSetPluginFile (DawdContext& ctx, const juce::var& cmd);

    // Rezept-Katalog (Familie recipes, ops/Recipes.cpp)
    juce::var opSaveRecipe (DawdContext& ctx, const juce::var& cmd);
    juce::var opRecipeInfo (DawdContext& ctx, const juce::var& cmd);
    juce::var opFindRecipes (DawdContext& ctx, const juce::var& cmd);
    juce::var opApplyRecipe (DawdContext& ctx, const juce::var& cmd);
    juce::var opRateRecipe (DawdContext& ctx, const juce::var& cmd);

    // Rezepte, deren Module dieses Plugin (format + uid wie in pluginInfo)
    // enthalten, ohne verworfene: [{id, title, kind, status, stars}]
    juce::var recipesWithPlugin (DawdContext& ctx, const juce::String& format, const juce::String& uid, int limit);

    // View: Zustand, Transport, Mix
    juce::var opState (DawdContext& ctx, const juce::var& cmd);
    juce::var opPlay (DawdContext& ctx, const juce::var& cmd);
    juce::var opLoop (DawdContext& ctx, const juce::var& cmd);
    juce::var opPosition (DawdContext& ctx, const juce::var& cmd);
    juce::var opTrackMix (DawdContext& ctx, const juce::var& cmd);
    juce::var opLevels (DawdContext& ctx, const juce::var& cmd);
    juce::var opLaneActive (DawdContext& ctx, const juce::var& cmd);

    // Comping (ops/Comp.cpp): je Zeitbereich spielt eine Lane
    juce::var opCompLane (DawdContext& ctx, const juce::var& cmd);

    // Interface: Hardware-Monitoring des Audio-Interfaces (Fireface UC)
    juce::var opInterfaceState (DawdContext& ctx, const juce::var& cmd);
    juce::var opSetInterface (DawdContext& ctx, const juce::var& cmd);
    juce::var opInterfaceMeters (DawdContext& ctx, const juce::var& cmd);

    // Audio: Rate/Bit-Tiefe des Songs, Puffergroesse des Geraets
    juce::var opAudioSettings (DawdContext& ctx, const juce::var& cmd);
    juce::var opInterfaceSim (DawdContext& ctx, const juce::var& cmd);

    // OSC-Fernsteuerung: Empfang an/aus, Port, Adresse (Osc.h)
    juce::var opOscSettings (DawdContext& ctx, const juce::var& cmd);
}
