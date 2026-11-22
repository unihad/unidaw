#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

// Gemessene Klang-Merkmale eines Samples — das, was CLAP nur unscharf kann
// (Punch, Vibrato) und was ein Agent zum Begruenden braucht. ("Metallisch"
// kann CLAP selbst; ein gemessenes Klingen im Ausklang hing im Test nicht
// damit zusammen und fiel wieder raus.) Reine Signalverarbeitung auf JUCE;
// Eingang Mono bei 48 kHz (dieselben Daten wie fuer CLAP), hoechstens die
// ersten 30 s.
namespace dawd::sound
{
    struct Features
    {
        double duration = 0.0;       // ganze Datei, Sekunden
        bool oneShot = true;         // kurz oder ein Einsatz (kein Loop)
        double attackMs = 0.0;       // 10 % -> 90 % der Spitze
        double punchDb = 0.0;        // Leistung der ersten 50 ms gegen 50–300 ms nach dem Einsatz (ab 0,1 s Dauer)
        double low = 0.0;            // Anteil der Energie unter 150 Hz (erste 300 ms)
        double brightness = 0.0;     // spektraler Schwerpunkt, Hz
        double pitchHz = 0.0;        // Grundton, 0 = keiner (Geraeusch, Akkord)
        double vibratoCents = 0.0;   // Auslenkung um die Tonhoehe (regelmaessig, 3,5–9 Hz), 0 = keins
        double vibratoHz = 0.0;
        double peakDb = -100.0;
        double bpm = 0.0;            // nur Loops mit deutlichem Puls

        static constexpr int count = 11;
        std::array<float, count> pack() const;
        static Features unpack (const float* v);
    };

    Features measure (const std::vector<float>& mono48k, double fileDuration);

    // Notenname zu einer Frequenz ("A4"), "" ohne Ton
    juce::String noteName (double hz);
}
