#pragma once

#include "Transcription.h"

#include <juce_core/juce_core.h>

#include <limits>
#include <vector>

// Musikalitaet und Sounddesign einer Spur (Stem oder eigene Spur) fuer
// Agenten, die nicht hoeren. Praxistest 1c: Die Referenz-Synth spielte
// legato mit Glides, gestaffelten Stimmen und sehr breit; gebaut wurde
// Achtel-Staccato mit Blockakkorden — die bisherigen Werkzeuge messen nur
// Mittelwerte (Baender, Tonklassen) und meldeten "nah".
//
// describeVoice: Noten (basic-pitch, Fragmente gehaltener Toene
// verschmolzen), Tonhoehenverlauf aus der Contour (Glide, Vibrato,
// Feinstimmung), Artikulation (legato, Luecken je Takt), Stimmen und
// Staffelung, Rhythmus je Takt im Raster, Dynamik (Anschlag, Schwellen).
// describeTimbre: Huellkurve (Attack, Decay, Sustain, Release), Obertoene
// (Neigung, ungerade/gerade, Wellenform-Klasse, Rauschen), Filterbewegung
// je Note und ueber die Takte, Stereo (Korrelation, Seite/Mitte je Band,
// L/R-Versatz, Breiten-Wackeln = Chorus), Verstimmung (Breite der Teiltoene),
// Pegelschwankung, Hallfahne.
// Reine Signalverarbeitung auf JUCE, keine neue Abhaengigkeit.
namespace dawd::voice
{
    // Raster in Sekunden ab Quellen-Start (aus analysisGrid)
    struct Grid
    {
        std::vector<double> beats, downbeats;
        int beatsPerBar = 4;
        bool valid = false;
    };

    constexpr double unknown = std::numeric_limits<double>::quiet_NaN();

    struct Note
    {
        double start = 0.0, end = 0.0;
        int pitch = 60;
        double velDb = 0.0;            // Pegel beim Einsatz relativ zum lautesten Einsatz
        double tuneCents = unknown;    // Abweichung vom Halbton im gehaltenen Teil
        double glideCents = 0.0;       // Einsatz von unten (< 0) bzw. oben (> 0), 0 = gerade
        double glideMs = 0.0;
        double glideFrom = 0.0;        // Beginn des Glides (s), nur aus der Contour (pitchShape)
        double glideCentsPerMs = 0.0;  // Tempo des Glides, nur aus der Contour (pitchShape)
        double vibratoCents = 0.0, vibratoHz = 0.0;
        double swellDb = unknown;      // Anstieg nach dem Einsatz (Schwellen)
        int fragments = 1;             // aus so vielen basic-pitch-Noten verschmolzen
        double amplitude = 0.0;        // Aktivierung aus basic-pitch (Geister-Filter)
    };

    struct Bar
    {
        int bar = 1;                   // 1 = erste Takt-Eins, <= 0 Auftakt
        double start = 0.0, end = 0.0;
        juce::String notes;            // "/F4~+A4~ C5": + gleichzeitig, ~ gehalten (>= 1 Schlag), / \ Glide von unten/oben
        juce::String pattern;          // je 16tel: x Einsatz, - klingt, . Pause
        int gaps = 0;                  // Einbrueche > 9 dB unter dem Taktmaximum (Zaehlung, streut)
        double dipDb = 0.0;            // mittlere Tiefe unter dem Taktmaximum (ab 3 dB), stabil
        double attackDb = 0.0;         // Summe der Anstiege > 3 dB in 30 ms je Schlag (Neuanschlaege)
        double sustain = 0.0;          // Anteil des Takts nahe am Maximum (0..1)
        std::vector<int> pitches;      // Einsaetze im Takt (MIDI)
        std::vector<std::pair<int, double>> weights;   // Tonhoehe -> klingende Dauer im Takt x Pegel
    };

    // Eine Schicht im Stem: Noten mit aehnlichem Klang und aehnlicher
    // Spielweise (ein Instrument) — der other-Stem mischt oft mehrere
    struct Layer
    {
        int notes = 0;
        int lowest = 0, highest = 0;
        double medianBeats = unknown;
        juce::String role;              // "held notes (pad/chords)", "short decaying hits (pluck/stab)" …
        std::vector<size_t> members;    // Indizes in Voice::notes
    };

    // Pulse: Einsaetze im Stem, die keine neuen Noten sind — ein zweites
    // Instrument schlaegt dieselben Akkordtoene im 16tel-Takt an, waehrend
    // ein Pad sie haelt (Praxistest 1e: 263 Einsaetze in 8 Takten, die
    // Notenerkennung sah 8 Noten je Takt und meldete "gehaltenes Pad")
    struct PulseBar
    {
        int bar = 1;
        juce::String pattern;          // je 16tel: X stark, x, g leise, . keiner
        juce::String chord;            // Tonklassen der neuen Energie ("A C E")
        juce::String notes;            // nur chordHits: die verdeckten Anschlaege mit Lage ("F3+C4 A3+D4")
        int onsets = 0;
    };

    struct Pulse
    {
        bool analysed = false;
        int onsets = 0;                // Einsaetze mit deutlichem Zuwachs im Raster
        double perBar = unknown;       // Median je Takt
        double notesPerBar = unknown;  // Noteneinsaetze je Takt (Akkord = einer) zum Vergleich
        double offbeatShare = unknown; // Anteil auf e/a (16tel zwischen den Achteln)
        double decayMs = unknown;      // Median: neue Energie -9 dB (bis zum naechsten Einsatz)
        double heldShare = unknown;    // Anteil, deren neue Energie bis zum naechsten Einsatz haelt
        double addedDb = unknown;      // Median: neue Energie gegen den ganzen Klang beim Einsatz
        double lockedShare = unknown;  // Anteil der Einsaetze im 16tel-Raster (+-18 % eines 16tels)
        double excess = unknown;       // derselbe Anteil ueber dem Zufall (0 = zufaellig, 1 = alle im Raster)
        int lowest = 0, highest = 0;   // tiefster starker Teilton der neuen Energie (10./90. Perzentil)
        juce::String chord, grid;      // "A C E", "16ths" / "8ths" / "quarters"
        std::vector<PulseBar> bars;
        bool layer = false;            // deutlich mehr Einsaetze als Noten: eigener rhythmischer Part
    };

    // Verdeckte Akkordanschlaege: Akkorde werden neu angeschlagen, waehrend
    // dieselben Toene weiterklingen — ein zweites Instrument mit Akkorden und
    // Pausen ueber einem Pad (Intro des Referenzsongs aus Praxistest 1c) oder dasselbe
    // Instrument, das Akkorde mit gemeinsamen Toenen wiederholt. Die
    // Notenerkennung sieht die Anschlaege (Bruchstuecke), das Verschmelzen
    // gehaltener Toene klebt sie zusammen, weil der Pegel nie einbricht
    // (Praxistest 1e/1f: Intro 5,2 Akkordanschlaege je Takt roh, 2,7 nach
    // dem Verschmelzen). Ob es zwei Instrumente sind, zeigt eher CLAP je Takt.
    struct ChordHits
    {
        bool analysed = false;
        int raw = 0, hidden = 0;            // Akkordanschlaege roh / davon im Verschmelzen verschwunden
        double perBar = unknown, hiddenPerBar = unknown;
        std::vector<PulseBar> bars;         // pattern: X verschwunden, x auch als Note sichtbar; chord: Toene der verschwundenen
        int lowest = 0, highest = 0;        // Lage der verschwundenen Anschlaege (Praxistest 1h: hitTones ohne Oktave)
        bool layer = false;
    };

    // Verdeckte Neuanschlaege einzelner Toene: Einsaetze mitten in einer
    // klingenden Note — der Pegel bricht kaum ein, basic-pitch sieht sie
    // nicht oder nur als Oberton (Praxistest 1g: Bass 13 statt 48 Noten).
    // Im Raster gespielt = ein Instrument schlaegt an; gleichmaessiger
    // Abstand neben dem Raster = Tremolo/LFO oder Schwebung (Praxistest 1h)
    struct ReAttacks
    {
        int count = 0;                 // Einsaetze in klingenden Noten
        int split = 0;                 // davon als eigene Noten geteilt (einstimmige Linie, im Raster)
        double perBar = unknown;
        double intervalMs = unknown;   // Median der Abstaende (unter 1 s)
        double regularity = unknown;   // Streuung der Abstaende / Median (0 = gleichmaessig)
        double onGrid = unknown;       // Anteil im 16tel-Raster
        double excess = unknown;       // derselbe Anteil ueber dem Zufall (0 zufaellig, 1 alle)
        int lowest = 0, highest = 0;   // Toene, in denen neu angeschlagen wird
        juce::String verdict;          // played | periodic | irregular, leer = zu wenige
    };

    struct Voice
    {
        std::vector<Note> notes;
        std::vector<Bar> bars;
        int fragmentsMerged = 0;
        double legato = unknown;       // Median Notenlaenge / Abstand zum naechsten Einsatz (0..1)
        double gapsPerBar = unknown, sustainShare = unknown, dipDb = unknown, attackDbPerBeat = unknown;
        int droppedShort = 0;          // kurze, schwache Scheinnoten verworfen
        double voices = 0.0;           // mittlere Zahl gleichzeitig klingender Noten
        int maxVoices = 0, chordOnsets = 0;
        double staggerMs = unknown;    // Spreizung gleichzeitiger Einsaetze (Median)
        double chordSpan = unknown;    // Abstand tiefster-hoechster Ton gleichzeitiger Einsaetze (Halbtoene)
        int lowest = 0, highest = 0, medianPitch = 0;
        double meanPitch = 0.0;        // nach Dauer x Pegel gewichtet (stabil, auch bei zwei gleich starken Lagen)
        int legatoChanges = 0;         // Legato-Uebergaenge (eine Note folgt allein, ohne Einbruch, einer anderen)
        double glideShare = 0.0, glideMs = unknown;   // Anteil der Legato-Uebergaenge, die gleiten
        double vibratoShare = 0.0, vibratoCents = unknown, vibratoHz = unknown;
        double tuneCents = unknown, tuneSpread = unknown;
        double onBeatShare = unknown, andShare = unknown, medianNoteBeats = unknown;
        double swellDb = unknown, dynamicsDb = unknown;
        bool hasContour = false, hasGrid = false;
        std::vector<Layer> layers;   // >= 2 nur bei klar getrennten Gruppen
        Pulse pulse;
        ChordHits chordHits;
        ReAttacks reattacks;
    };

    // raw: Noten aus basic-pitch (ohne Geister-Filter), contour dazu,
    // stereo: 2 Kanaele bei rate (Huellkurve), grid optional
    Voice describeVoice (const std::vector<transcription::Note>& raw, const transcription::Contour& contour,
                         const std::vector<std::vector<float>>& stereo, double rate, const Grid& grid);

    struct Timbre
    {
        double attackMs = unknown, decayMs = unknown, sustainDb = unknown, releaseMs = unknown;
        int envelopeNotes = 0;
        double slopeDbOct = unknown, oddEvenDb = unknown, centroidHarmonic = unknown, centroidHz = unknown, noiseShare = unknown;
        juce::String wave;             // sine-like, saw-like, square-like, triangle-like, soft/filtered, noisy
        int harmonicNotes = 0;
        bool harmonicsFromChords = false;
        double noteSweepOct = unknown; // + = wird im Ton dunkler (Filter-Huellkurve schliesst)
        double barSweepOct = unknown;  // Schwerpunkt erster -> letzter Takt (+ = heller)
        std::vector<double> barCentroidHz;
        double correlation = unknown, sideMinusMidDb = unknown, lowCorrelation = unknown, midCorrelation = unknown, highCorrelation = unknown;
        double lrDelayMs = unknown, widthWobbleDb = unknown;
        double beatingDb = unknown, beatingHz = unknown;   // Pegelschwankung im Ton (Tremolo; misst auch das Ausklingen)
        double detuneCents = unknown;  // Breite der Teiltoene in gehaltenen Toenen (Unison-Detune, Chorus; ein Oszillator ~1)
        double tailSec = unknown;
        // Hall und Delay (grob, auf einem Stem): Abklingen nach freistehenden
        // Notenenden; dieselbe Tonhoehe in festem Abstand wieder, jedes Mal leiser
        double rt60Sec = unknown;          // Abklingen -5 -> -25 dB unter dem Notenende, auf 60 dB hochgerechnet
        double tailDb = unknown;           // Pegel 150 ms nach dem Notenende gegen das Notenende
        double tailCorrelation = unknown;  // L/R-Korrelation der Fahne ...
        double noteCorrelation = unknown;  // ... und der letzten 300 ms der Note
        int reverbNotes = 0;
        double echoMs = unknown;           // Abstand der Wiederholungen
        double echoFeedbackDb = unknown;   // Pegel je Wiederholung (negativ)
        double echoShare = unknown;        // Anteil der Noten mit Echo
        int echoPairs = 0;
        // Durchsetzung (Praxistest 1k: Akkorde ohne Anschlag, Rauheit, Praesenz)
        double transientDb = unknown;  // Anschlag: Gipfel in den ersten 40 ms gegen den Pegel 80–250 ms danach (Median je Einsatz)
        double presenceDb = unknown;   // Energie 1–5 kHz gegen die ganze Energie
        double longTermCentroidHz = unknown;   // Schwerpunkt des mittleren Leistungsspektrums (ab 60 Hz) — Helligkeit wie compareReference
        double roughness = unknown;    // spektrale Flachheit 2–8 kHz (0 tonal/sauber .. 1 rauschig/verzerrt)
    };

    Timbre describeTimbre (const Voice&, const std::vector<std::vector<float>>& stereo, double rate);

    // "A4" (MIDI 69), Kreuze
    juce::String noteName (int midi);

    // Noten ohne Scheinnoten in digitaler Stille (mittlerer Pegel mehr als
    // 60 dB unter dem lautesten Moment) — dieselbe Regel wie describeVoice,
    // fuer audioToMidi/addMidiFromAudio; mono bei rate ab Quellen-Start
    std::vector<transcription::Note> soundingNotes (const std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate);

    // audioToMidi {legato}: Fragmente gehaltener Toene verschmolzen (wie
    // describeVoice: gleiche Tonhoehe, kurze Luecke, kein neuer Anschlag in
    // der Huellkurve); merged zaehlt die verschwundenen Fragmente
    std::vector<transcription::Note> mergedFragments (const std::vector<transcription::Note>& notes, const std::vector<float>& mono,
                                                      double rate, int& merged);

    // audioToMidi {legato}: eine Note klingt bis zum naechsten Einsatz,
    // wenn der Pegel dazwischen nicht einbricht (weniger als 9 dB unter
    // ihrem Gipfel, Luecke bis 0,5 s), und ueberlappt ihn um overlap
    // Sekunden — ein Mono-Synth spielt dann gebunden bzw. gleitet. Gleiche
    // Tonhoehe stoesst nur an. Rueckgabe: so verlaengerte Noten.
    int joinLegato (std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate, double overlap);

    // Velocity aus dem Pegel beim Einsatz (RMS, wie velDb in describeVoice)
    // statt der Modell-Aktivierung: Akkordtoene teilen sich die Leistung,
    // Pegel ~ Velocity^2, der staerkste Einsatz = 100 (wie matchSound)
    std::vector<int> onsetVelocities (const std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate);

    // audioToMidi {bends}: Tonhoehenverlauf einer Note aus der Contour
    // (Zeit, Cent gegen ihren Halbton), ab dem Beginn eines Glides (from,
    // sonst der Einsatz) bis zum Ende; leer ohne Bewegung (unter 15 Cent
    // Spannweite, kein Glide). Reicht +-133 Cent (Fenster der Contour).
    std::vector<std::pair<double, double>> pitchBends (const transcription::Note& note, const transcription::Contour& contour, double& from);
}
