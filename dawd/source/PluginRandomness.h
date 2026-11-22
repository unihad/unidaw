#pragma once

#include "Dawd.h"

// Plugins, deren Klang bei jedem Render anders ist (Todo
// 2026-10-26-surge-render-schwankt, Matthias 2026-10-27, B). Bisher nur
// Surge XT: Startphasen, Drift, Noise und Zufalls-Modulation kommen aus einem
// Zufall mit Uhr-Seed, ohne Schnittstelle fuer einen festen Seed. Das Wissen
// ueber Surge steht nur hier.
namespace dawd
{
    // Zufallsquellen im aktuellen Klang (englische Stichworte, z. B.
    // "random start phase (A Osc 1, Retrigger off)"); leer = keine bekannten
    // oder ein anderes Plugin. Liest das Patch-XML aus dem Plugin-Zustand.
    juce::StringArray pluginRandomSources (te::Plugin&);

    // Hinweis "renders vary" fuer tuneSound/matchSound oder ""
    juce::String rendersVaryHint (te::Plugin&);
}
