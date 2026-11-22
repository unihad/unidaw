#pragma once

#include "Dawd.h"

// Presets fuer Agenten (und Menschen): was ein Plugin an Klaengen mitbringt,
// finden und laden — ohne Plugin-GUI. Quellen:
// - dawd: mit savePreset gespeicherte Zustaende (<config>/presets/<Plugin>/),
//   fuer jedes Plugin, auch die eingebauten (4OSC, Sampler)
// - program: Programme des Plugins (LV2-Presets, VST3-Programmlisten)
// - file: Preset-Dateien bekannter Plugins — Surge XT (.fxp in den
//   Patch-Ordnern), VST3 (.vstpreset unter ~/.vst3/presets/<Hersteller>/<Plugin>),
//   Dexed (DX7-Cartridges .syx: je Stimme ein Eintrag, program = Stimme)
namespace dawd
{
    struct PresetEntry
    {
        juce::String name, category, source;   // source: dawd | program | file
        juce::File file;                        // dawd, file
        int program = -1;                       // program; file: Stimme einer DX7-Cartridge
        juce::String catalogPath;               // file aus einem Katalog, relativ: Datei erst beim Laden suchen
    };

    std::vector<PresetEntry> presetsFor (DawdContext&, te::Plugin&);

    // Pfad aus einem Preset-Katalog (plugin-profiles/*-presets.json,
    // *-sounds.json): relativ zum Preset-Ordner des Plugins (Surge XT:
    // patches_factory/..., Dexed: Cartridges/..., sfizz: <Ordner>/x.sfz),
    // aeltere Kataloge absolut. Passt f dazu? Relativ = f endet auf "/" + path.
    bool matchesCatalogPath (const juce::File& f, const juce::String& path);

    // Laedt den Eintrag; Fehlertext oder "". changed = Zahl der Parameter,
    // deren Wert sich dabei geaendert hat (zeigt, dass der Klang wechselte)
    // stateChanged (optional): der Plugin-Zustand hat sich geaendert, auch
    // wenn noch kein Parameter es meldet (Surge XT nach dem ersten Render)
    juce::String loadPresetEntry (DawdContext&, te::Plugin&, const PresetEntry&, int& changed, bool* stateChanged = nullptr);

    // Zustand als dawd-Preset speichern; Fehlertext oder "", file = Datei
    juce::String savePresetFile (DawdContext&, te::Plugin&, const juce::String& name,
                                 const juce::String& category, juce::File& file);

    // Zustand als dawd-Preset in genau diese Datei (Rezepte: state/<ref>.dawdpreset)
    juce::String writePresetFile (te::Plugin&, const juce::String& name, const juce::File& file);

    // Datei, die ein Plugin laedt (sfizz: .sfz) — im Plugin-Zustand ersetzen.
    // Fehlertext oder ""; kind = was die Datei ist ("sfz")
    juce::String setPluginFile (te::Plugin&, const juce::File& file, juce::String& kind);

    // Ordner der dawd-Presets eines Plugins
    juce::File dawdPresetDir (DawdContext&, te::Plugin&);
}
