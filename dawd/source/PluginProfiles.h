#pragma once

#include "Dawd.h"

// Wissen ueber Plugins fuer Agenten (dawd/plugin-profiles/): das Kernset
// (coreset.json — was listPlugins standardmaessig zeigt, mit Kategorie und
// "wofuer"), gemessene Steckbriefe (<format>-<uid>.json, tools/
// profile_plugin.py) und kuratierte Kurznotizen (notes/<format>-<uid>.md).
namespace dawd
{
    juce::File profilesDirectory();

    struct CoreEntry
    {
        juce::String name, format, category, purpose;
    };

    const std::vector<CoreEntry>& coreSet();
    const CoreEntry* coreEntryFor (const juce::String& name);

    // "<format>-<uid>" (builtin: "builtin-<type>")
    juce::String profileSlug (const juce::String& format, const juce::String& uid);

    // Gemessener Steckbrief (geparstes JSON) oder void; Kurznotiz oder ""
    juce::var loadProfile (const juce::String& slug);
    juce::String loadNotes (const juce::String& slug);
}
