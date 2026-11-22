#pragma once

#include "Dawd.h"

// Herkunft von Audiodateien, die dawd aus anderen macht (Decision
// docs/decisions/2026-11-18-analyse-ops-auf-gesperrten-samples.md): eine
// Kopie (setSamples), eine dekodierte WAV, Clip-FX, Freeze, Bounce, Stems.
// Die Quellen stehen in ".dawd-origins.json" im Ordner der neuen Datei
// (Dateiname -> absolute Pfade der Quellen, schon aufgeloest: die Quelle
// einer Quelle steht direkt drin). Geprueft wird bei jeder Analyse gegen die
// gesperrten Ordner (sampleLibraries noAnalysis), nicht beim Notieren.
namespace dawd
{
    // Die Datei selbst und ihre notierten Quellen
    juce::StringArray originsOf (const juce::File& f);

    // derived ist aus inputs gemacht: notiert deren Herkunft (originsOf je
    // Eingabe). Ohne Eingaben passiert nichts.
    void recordOrigins (const juce::File& derived, const juce::StringArray& inputs);

    // Material, das im Bereich [start, end) klingt: Audio-Clips und die
    // Samples der Sampler, die MIDI-Clips im Bereich spielen. track >= 0:
    // nur diese Spur samt Lanes und gefrorenem Audio (wie renderMixRange),
    // sonst alle Spuren. Je Datei die Spur, die sie spielt.
    struct SoundingFile
    {
        juce::String track;   // Spurname
        juce::File file;      // wie im Edit (Clip-Quelle, Sample)
    };

    std::vector<SoundingFile> soundingFiles (te::Edit&, double start, double end, int track);

    // Alle Quellen (originsOf) des Materials im Bereich
    juce::StringArray soundingOrigins (te::Edit&, double start, double end, int track);
}
