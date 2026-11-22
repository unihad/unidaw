#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <vector>

// Musik-Analyse fuer Agenten, die nicht hoeren: Tempo, Taktschlaege und
// Takt-Eins (spaeter Tonart, Akkorde, Aufbau). Reine Signalverarbeitung auf
// JUCE — keine Engine, keine neue Abhaengigkeit. Die Ops (ops/Analysis.cpp)
// holen die Quelle (Datei oder gerenderter Mix) und uebersetzen in JSON.
namespace dawd::analysis
{
    // Mono-Signal bei ~22 kHz (ganzzahlig heruntergeteilt) — reicht fuer
    // Onsets und Tonklassen und halbiert Speicher und FFT-Last
    struct MonoAudio
    {
        std::vector<float> samples;
        double sampleRate = 0.0;
    };

    MonoAudio loadMono (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length);

    // Merkmale je Frame: Onset-Staerke (Spectral Flux, gesamt und Bass bis
    // 150 Hz; midOnset 200 Hz - 1 kHz ohne Trendabzug, der Snare-Koerper) mit
    // fps Frames je Sekunde, Chroma (12 Tonklassen, C = 0) mit
    // chromaFps. Zeit eines Frames i = i / fps (Mitte des Analysefensters).
    struct Features
    {
        double fps = 0.0;
        std::vector<float> onset, lowOnset, midOnset;
        double chromaFps = 0.0;
        std::vector<std::array<float, 12>> chroma;
        std::vector<float> chromaEnergy;   // Summe vor der Normierung — Stille erkennen
        std::vector<std::array<float, 3>> bandEnergy;   // je Onset-Frame: bis 250 Hz, bis 4 kHz, darueber
        double duration = 0.0;
    };

    Features computeFeatures (const MonoAudio& audio);

    struct TempoCandidate
    {
        double bpm = 0.0, score = 0.0;
    };

    struct TempoResult
    {
        bool valid = false;            // zu kurz oder ohne Pulse: false
        double bpm = 0.0;              // aus der Regression ueber die Beats
        double confidence = 0.0;       // 0..1: wie deutlich die Beats hervortreten
        std::vector<TempoCandidate> candidates;   // bestes zuerst, inkl. halbes/doppeltes Tempo
        std::vector<double> beats;     // Sekunden ab Quellen-Start
        std::vector<double> downbeats; // Takt-Einsen
        int beatsPerBar = 4;
        double meterConfidence = 0.0;  // 0..1
        double maxDriftMs = 0.0;       // groesste Abweichung der Beats vom geraden Raster
        bool phaseShifted = false;     // Raster um eine halbe Periode von den staerksten Onsets weg (Synkope)
        double offbeatVote = 0.0;      // -3..3: > 0.3 verschoben, 0.1..0.3 unsicher
        double backbeatVote = 0.0;     // -1..1: Anteil des Rueckschlags daran (> 0 spricht fuer die Verschiebung)
        bool phaseKept = false;        // Stimme > 0.3, aber Rueckschlag oder Kick auf jedem Schlag halten das Raster
        bool phaseUncertain = false;   // kein Rueckschlag, auf "und" >= 60 % des Onsets: Schlag-Phase offen
        bool straightened = false;     // wandernde Beats durch ein gerades Raster ersetzt (Einsaetze liegen auf seinen 16teln)
        bool snapped = false;          // einzelne wandernde Beats auf die Gerade der uebrigen (>= 85 %) gelegt
        bool fromBars = false;         // Schlag aus der Takt-Wiederholung (der staerkste Rhythmus war kein Puls)
    };

    TempoResult detectTempo (const Features& f);

    // Tonart (Krumhansl-Kessler-Profile, 24 Tonarten) und Akkorde je Beat
    // (Vorlagen, Viterbi-Glaettung). Ohne Beats (kein Puls) je halbe Sekunde.
    struct KeyCandidate
    {
        int tonic = 0;        // Tonklasse, C = 0
        bool minor = false;
        double score = 0.0;   // Korrelation -1..1
    };

    struct Chord
    {
        double start = 0.0, end = 0.0;   // Sekunden ab Quellen-Start
        int root = -1;                    // -1 = kein Akkord (Stille/Geraeusch)
        int type = 0;                     // Index in chordTypes()
        double confidence = 0.0;
    };

    struct KeyResult
    {
        bool valid = false;
        std::vector<KeyCandidate> keys;   // bestes zuerst
        double confidence = 0.0;          // 0..1
        std::vector<Chord> chords;
    };

    struct ChordType
    {
        const char* suffix;               // "", "m", "7", "maj7", "m7", "dim", "sus4", "sus2", "5"
        std::vector<int> intervals;
    };

    const std::vector<ChordType>& chordTypes();

    // Tonklassen-Name, je nach Tonart mit Kreuz oder b
    juce::String pitchName (int pc, bool flats);
    bool keyUsesFlats (int tonic, bool minor);
    bool isRelativeKey (const KeyCandidate& a, const KeyCandidate& b);   // A-Moll <-> C-Dur

    KeyResult detectKey (const Features& f, const std::vector<double>& beats);

    // Schlagzeug-Einsaetze (audioToMidi mode drums): Spectral Flux in drei
    // Baendern (Kick 35–150 Hz, Snare 150–2500 Hz, Hi-Hat ab 6 kHz), Gipfel
    // ueber einer Schwelle relativ zum lauten Rest des Bandes; gleichzeitige
    // Einsaetze werden nach dem staerkeren Band zugeordnet (der Klick einer
    // Kick ist keine Snare, das Rauschen einer Snare keine Hi-Hat).
    struct DrumHit
    {
        double time = 0.0;
        int note = 36;            // GM: 36 Kick, 38 Snare, 42 Hi-Hat
        double strength = 0.0;    // 0..1
    };

    std::vector<DrumHit> detectDrumHits (const MonoAudio& audio, double sensitivity = 1.0);

    // Aufbau: Merkmale je Takt (Lautheit, Klangfarbe, Onset-Dichte, Chroma,
    // Akkord), Abschnitte ueber Neuheit zwischen den Takten (bevorzugt an
    // 4er-Phrasen), gleiche Teile mit gleichem Buchstaben, Energie 1..5 und
    // eine Rollen-Vermutung (intro, verse, chorus, bridge, outro ...)
    struct BarInfo
    {
        double start = 0.0, end = 0.0;
        bool pickup = false;              // Auftakt vor der ersten Takt-Eins
        double rmsDb = -100.0;
        std::array<double, 3> bandsDb {}; // Anteil an der Leistung: tief, mitte, hoch
        double density = 0.0;             // Onsets je Beat
        std::array<double, 12> chroma {};
        int chord = -1;                   // Index in KeyResult::chords
    };

    struct Segment
    {
        int firstBar = 0, numBars = 0;
        double start = 0.0, end = 0.0;
        int label = 0;                    // 0 = A, 1 = B ...
        juce::String role;
        int energy = 1;                   // 1..5
        double rmsDb = -100.0, density = 0.0, highDb = 0.0;
    };

    struct StructureResult
    {
        bool valid = false;
        bool grid = false;                // Takte aus dem Puls (sonst 2-s-Bloecke)
        double barLength = 0.0, firstDownbeat = 0.0;
        std::vector<BarInfo> bars;
        std::vector<Segment> segments;
    };

    StructureResult analyzeStructure (const MonoAudio& audio, const Features& f, const TempoResult& tempo, const KeyResult& key);
}
