#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

// Gefuehrte Trennung: Ein Trennmodell legt im vollen Mix Instrumente
// zusammen, die an einer freien Stelle des Songs getrennt liegen (Praxistest
// 1i/1j: Pad und angeschlagene Akkorde im Refrain in einer Schicht, im Intro
// getrennt). An der freien Stelle lernt KL-NMF je Instrument Klangvorlagen
// (Spektren); im Abschnitt zerlegen die festen Vorlagen plus ein paar freie
// Bausteine (Rest: was keine Vorlage erklaert) das Gemisch, Wiener-Masken je
// Kanal liefern die Instrumente. STFT 4096/1024, NMF bis 8 kHz (darueber
// setzt sich die Maske des obersten Bandes fort), 150 Durchlaeufe.
namespace dawd::guided
{
    using Stereo = std::array<std::vector<float>, 2>;

    // sources: je Instrument das Audio der freien Stelle; target: das
    // Gemisch. out: je Quelle ein Teil in gleicher Reihenfolge, dazu als
    // letzter der Rest — alle so lang wie target. Fehlertext oder "".
    juce::String split (const std::vector<Stereo>& sources, const Stereo& target, double rate, std::vector<Stereo>& out);
}
