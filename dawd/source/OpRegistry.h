#pragma once

#include "Dawd.h"

namespace dawd
{
    // Eine Op-Beschreibung: Name, Familie, ob sie die stateVersion hochzaehlt,
    // welche Guards vor dem Handler greifen und der Handler selbst.
    struct OpDef
    {
        const char* name;
        const char* kind;              // Familie: session | tree | section | media | processing | recipes | view
        bool mutating;                 // true = zaehlt die stateVersion hoch und wird geloggt
        bool needsEdit;                // Guard: "no edit"
        bool needsTree;                // Guard: "no tree"
        bool needsTreeAndEdit;         // Guard: "no tree / no edit"
        bool needsTreeOrEdit;          // Guard: "no tree open"
        bool blockWhileRecording;      // Guard: "recording in progress"
        juce::var (*fn) (DawdContext&, const juce::var&);
    };

    const std::vector<OpDef>& opTable();
    const OpDef* findOp (const juce::String& name);

    // Gemeinsamer Einstieg fuer alle Befehle: Guards pruefen, zaehlen, loggen.
    juce::var executeOp (DawdContext& ctx, const juce::String& op, const juce::var& cmd);
}
