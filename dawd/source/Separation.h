#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <functional>
#include <vector>

// Stem-Trennung mit HT-Demucs v4 (Meta) als ONNX-Modell (Exporte von
// StemSplit) ueber ONNX Runtime: Stereo bei 44,1 kHz in 7,8-s-Segmenten mit
// 25 % Ueberlappung und linearen Rampen (wie dessen infer.py). Zwei Modelle:
// htdemucs (vier Stems) und htdemucs_6s (dazu guitar und piano — trennt
// den other-Bereich weiter auf, z. B. Pad und angeschlagene Akkorde).
// Dazu MVSep Mega 53 (BS-RoFormer, 53 Instrumentenklassen; eigener Export
// mit tools/export_mega53.py): 10-s-Segmente, halbe Ueberlappung, Blenden
// ueber ein Zehntel (wie MSST demix). Die Klassen ueberlappen (drums enthaelt
// kick/snare/hh/toms, vocal lead-/back-vocal; guitar, electric-guitar und
// strings liegen nah beieinander).
// Modelle und Bibliothek laedt tools/fetch_ml.sh.
namespace dawd::separation
{
    constexpr double sampleRate = 44100.0;
    constexpr std::array<const char*, 4> stemNames { "drums", "bass", "other", "vocals" };
    // Reihenfolge der Ausgabe von htdemucs_6s
    constexpr std::array<const char*, 6> stemNames6 { "drums", "bass", "other", "vocals", "guitar", "piano" };
    // Reihenfolge der Ausgabe von mega53 (training.instruments der Config)
    constexpr std::array<const char*, 53> classNames53 {
        "accordion", "acoustic-guitar", "back-vocal", "banjo", "bass", "bassoon", "bells", "bowed_strings", "brass",
        "cello", "clarinet", "congas", "digital-piano", "dobro", "double-bass", "drums", "electric-guitar", "flute",
        "french-horn", "glockenspiel", "guitar", "harmonica", "harp", "harpsichord", "hh", "keys", "kick",
        "lead-vocal", "mandolin", "marimba", "oboe", "organ", "percussion", "piano", "saxophone", "sitar", "snare",
        "strings", "synth", "tambourine", "timpani", "toms", "triangle", "trombone", "trumpet", "tuba", "ukulele",
        "viola", "violin", "vocal", "wind", "wind-chimes", "woodwind" };

    using Stems = std::vector<std::array<std::vector<float>, 2>>;

    // "" wenn das Modell fuer stems (4, 6 oder 53) nutzbar ist, sonst der Grund
    juce::String available (int stems = 4);

    // left/right: Stereo bei 44,1 kHz. out: je Stem {links, rechts} in der
    // Reihenfolge von stemNames, stemNames6 bzw. classNames53, gleiche Laenge
    // wie die Eingabe. keep (optional, je Stem): nur diese zusammensetzen —
    // die anderen bleiben leer (53 Klassen eines langen Songs passen sonst
    // nicht in den Speicher). progress (0..1) liefert false = abbrechen.
    bool separate (const std::vector<float>& left, const std::vector<float>& right, int stems, Stems& out,
                   const std::function<bool (double)>& progress, juce::String& error,
                   const std::vector<bool>& keep = {});
}
