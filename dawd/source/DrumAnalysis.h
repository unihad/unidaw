#pragma once

#include <juce_core/juce_core.h>

#include <limits>
#include <vector>

// Schlagzeug fuer Agenten, die nicht hoeren (Praxistest 1d: der alte
// Detektor fand auf dem Drum-Stem 0 Snares, Kicks waren zur Haelfte
// Bass-Uebersprechen — der Agent hoerte das Muster mit zwoelf analyze-
// Fenstern ab und verpasste, dass die Referenz ein 2-Takt-Muster spielt).
//
// Einsaetze: Spektralfluss (SuperFlux) ab 150 Hz — die Tiefen schwanken
// durch Schwebung Kick/Bass ohne Anschlag. Je Einsatz die hinzugekommene
// Leistung in 16 Baendern; NMF mit Vorlagen kick/snare/hat/low (Vorwissen
// nur als spektraler Traeger und Startform) zerlegt auch Kick + Hat auf
// demselben Einsatz. Treffer je Instrument relativ zu ihm selbst (kein
// absoluter Pegel), Hats offen/zu ueber die Laenge, 16tel-Raster ab der
// Takt-Eins, Klang je Instrument. Reine Signalverarbeitung auf JUCE.
namespace dawd::drums
{
    struct Hit
    {
        double time = 0.0;      // Sekunden ab Quellen-Start
        int bar = 1, slot = 0;  // Takt (1 = erste Takt-Eins), 16tel im Takt
        double offMs = 0.0;     // Abstand zum Rasterplatz
        double levelDb = 0.0;   // relativ zum lautesten Schlag des Instruments
    };

    struct SlotStat
    {
        int slot = 0, count = 0;
        double levelDb = 0.0, offsetMs = 0.0;   // Median; Timing relativ zu den Vierteln
    };

    struct Instrument
    {
        juce::String name;      // kick, snare, hatClosed, hatOpen
        std::vector<Hit> hits;  // je Rasterplatz der staerkste
        std::vector<std::pair<int, juce::String>> bars;   // Takt -> Muster ("X" Akzent, "x", "g" leise, ".")
        std::vector<SlotStat> slots;
        double fundamentalHz = std::numeric_limits<double>::quiet_NaN();   // Kick
        double bodyHz = std::numeric_limits<double>::quiet_NaN();          // Snare
        double noiseShare = std::numeric_limits<double>::quiet_NaN();      // Snare: Energie > 1 kHz
        double brightnessHz = std::numeric_limits<double>::quiet_NaN();    // Hats: Schwerpunkt > 2 kHz
        double decayMs = std::numeric_limits<double>::quiet_NaN();         // T20
    };

    struct Result
    {
        bool valid = false;
        int onsets = 0, slotsPerBar = 16, patternRepeat = 1;
        std::vector<Instrument> instruments;
        juce::StringArray hints;
    };

    // mono bei rate (am besten 44,1 kHz — Hats liegen ueber 11 kHz), Raster
    // aus bpm und erster Takt-Eins (Sekunden ab Quellen-Start)
    Result describe (const std::vector<float>& mono, double rate, double bpm, double firstDownbeat, int beatsPerBar);

    // Alle Treffer als (Zeit, GM-Note, Staerke 0..1): 36 Kick, 38 Snare,
    // 42 Hat zu, 46 Hat offen — fuer extractGroove und audioToMidi drums
    struct NoteHit { double time; int note; double strength; };
    // minLevelDb: leisere Schlaege weglassen (-12: ohne Ghost-Notes)
    std::vector<NoteHit> noteHits (const Result&, double minLevelDb = -100.0);

    // Nur die Einsaetze (SuperFlux ab 150 Hz, wie describe): Zeit in
    // Sekunden ab Quellen-Start, Staerke relativ zum 90. Perzentil der
    // Gipfel — fuer tonale Stems (describeVoice: Pulse unter gehaltenen
    // Noten)
    struct Onset { double time; double strength; };
    std::vector<Onset> onsets (const std::vector<float>& mono, double rate);

    // Rasterplatz als Zaehlzeit: 0 -> "1", 1 -> "1e", 2 -> "1+", 3 -> "1a"
    juce::String slotName (int slot);
}
