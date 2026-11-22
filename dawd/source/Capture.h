#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <vector>

// Mitschnitt des Systemaudios (capture, captureStatus): pw-record als
// Unterprozess am Monitor des Standard-Ausgangs — was Player, Browser und
// Streaming gerade abspielen, unabhaengig vom Geraet, mit dem dawd selbst
// spielt. Kein Edit, nicht im Song-Baum; die WAVs liegen unter
// <config>/captures/. UNIDAW_CAPTURE_CMD ersetzt das Programm (Tests): die
// Woerter des Befehls, dahinter der Zielpfad; Stop per SIGINT.
//
// Songgrenzen (detectSongBoundaries): Pegel je 50-ms-Fenster, gestreamt —
// ein stundenlanger Mitschnitt muss nicht in den Speicher.
namespace dawd
{
    class SystemCapture : private juce::Timer
    {
    public:
        ~SystemCapture() override;

        // Startet den Mitschnitt nach file. target = Name eines Sinks ("" =
        // Standard-Ausgang). Fehlertext oder "".
        juce::String start (const juce::File& file, const juce::String& target, int rate);

        // Beendet ihn (SIGINT, pw-record schreibt den WAV-Header fertig;
        // nach 3 s SIGKILL, der Header wird dann aus der Dateigroesse
        // repariert) und haengt die Segmente zusammen. Fehlertext oder "".
        juce::String stop();

        bool running();                       // Mitschnitt laeuft (auch zwischen zwei Segmenten)
        bool reconnecting();                  // Programm gerade weg, der Neustart steht aus
        int reconnects() const { return restarts; }
        juce::File file() const { return current; }
        double seconds() const;               // seit dem Start (Uhr)
        juce::String failure();               // erstes Programm endete, ohne etwas zu schreiben (Fehlstart): letzte Zeile seines Logs
        double levelDb();                     // Pegel der letzten ~0,1 s (Spitze, dBFS, nur s24 stereo), -100 = still

    private:
        // Endet pw-record mitten im Mitschnitt (PipeWire-Neustart), startet
        // der Timer ein neues in <name>.partN.wav; stop haengt alles in
        // <name>.wav zusammen, die Luecke als Stille
        struct Segment
        {
            juce::File file;
            juce::Time started;
        };

        std::vector<Segment> segments;
        std::vector<std::string> args;        // Befehl ohne Zielpfad
        juce::String prog, lastError;
        int pid = -1, restarts = 0;
        bool exited = false, active = false;
        juce::File current, log;
        juce::Time started;

        juce::String spawn (const juce::File& dest);
        juce::String joinSegments();
        void reap (bool block);
        void timerCallback() override;
    };

    // RIFF- und data-Groesse aus der Dateigroesse nachziehen (abgebrochener
    // Schreiber). true, wenn die Datei danach lesbar ist.
    bool repairWavHeader (const juce::File& file);

    struct SongSpan
    {
        double start = 0.0, end = 0.0;
        double levelDb = -100.0;   // lautestes Fenster (RMS, dBFS)
    };

    struct BoundaryParams
    {
        double silenceDb = -50.0;   // darunter zaehlt ein Fenster als still
        double minGapSec = 0.8;     // so lange Stille trennt zwei Songs
        double minSongSec = 30.0;   // kuerzere Stuecke haengen am Nachbarn ...
        double joinGapSec = 8.0;    // ... wenn die Pause dazwischen nicht laenger ist
    };

    struct BoundaryResult
    {
        double duration = 0.0;
        std::vector<SongSpan> songs;
        std::vector<std::pair<double, double>> gaps;   // Stille zwischen den Songs
    };

    // Fenster-Pegel (RMS ueber alle Kanaele, dBFS) mit hop Sekunden
    std::vector<float> windowLevels (juce::AudioFormatReader& reader, double hop);

    BoundaryResult findSongs (const std::vector<float>& levelsDb, double hop, double duration, const BoundaryParams& p);
}
