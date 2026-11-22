#pragma once

#include "AnalysisSource.h"
#include "../VoiceAnalysis.h"

// Messung fuer matchSound/tuneSound (ops/SoundMatch.cpp) — dieselbe Analyse
// wie describeVoice/describeTimbre (ops/Voice.cpp). Die Referenz wird einmal
// gemessen; jeder Kandidat spielt ihre Noten und wird mit ihren Noten
// gemessen (Huellkurve und Klang an denselben Stellen), Glide aus der eigenen
// Notenerkennung des Kandidaten.
namespace dawd
{
    struct MeasuredSound
    {
        AnalysisSource src;              // stereo: was gemessen wurde (mit minPitch hochpassgefiltert)
        voice::Voice v;                  // Referenz: Noten und Raster; Kandidat: eigene Notenerkennung (Glide)
        voice::Timbre tb;
        std::vector<float> clap;         // CLAP-Vektor (Mono 48 kHz, hoechstens 10 s), leer ohne Modell
        juce::StringArray classes;       // "synth pad 0.31", bestes zuerst
        double bpm = 0.0;
        juce::String gridFrom;
    };

    // Referenz: file (+ start/end) oder der Mix (section, start/end, track)
    // wie describeTimbre, ohne Zeitleiste. Fehlertext oder "".
    juce::String measureSoundReference (DawdContext&, const juce::var& spec, MeasuredSound&);

    // Kandidat: eine Datei, die die Noten der Referenz ab Sekunde 0 spielt
    juce::String measureSoundCandidate (DawdContext&, const MeasuredSound& ref, const juce::File& file, MeasuredSound&);
}
