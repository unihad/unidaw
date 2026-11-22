#pragma once

#include "Ops.h"
#include "../MusicAnalysis.h"

// Quelle und Raster der Analyse-Ops (detectTempo, detectKey, extractGroove,
// audioToMidi, describeVoice …): eine Datei (Referenz, Stem) oder der eigene
// Mix, beide mit demselben Werkzeug gemessen.
namespace dawd
{
    struct AnalysisSource
    {
        analysis::MonoAudio audio;                // ~22 kHz Mono (Onsets, Chroma)
        std::vector<float> audio22k;              // genau 22050 Hz (basic-pitch)
        std::vector<std::vector<float>> stereo;   // 2 Kanaele bei stereoRate (Klang)
        static constexpr double stereoRate = 44100.0;
        bool want22k = false, wantMono = true, wantStereo = false;
        juce::String file;                        // leer = Mix
        double start = 0.0, end = 0.0;            // Datei- bzw. Songsekunden
        int track = -1;
        // Spitze (Betrag, der Mix ist float und kann ueber 1 liegen) und
        // flache Spitzen ab 0,99: ein 16/24-Bit-Render schneidet ueber
        // 0 dBFS ab, die eigene Spur wird float gemessen
        float peak = 0.0f;
        int clipRuns = 0;
        // Vergleiche (compareVoice/compareStems): spielt im Mix ein externes
        // Instrument, ein zweiter Render und dessen Abstand zum ersten
        // (Todo 2026-10-26-surge-render-schwankt, Matthias 2026-10-27, A):
        // {plugins, deltaLufs, correlation, extraSec}, sonst void
        bool wantSpread = false;
        juce::var spread;
        // Lizenz-Sperre (Decision 2026-11-18-analyse-ops-auf-gesperrten-
        // samples): true = Gruppe A (ML-Modell, Audio -> MIDI) lehnt auch
        // Mix/Spur mit gesperrtem Material ab; false = Messung, dort nur
        // ein Hinweis. Eine gesperrte Datei lehnen beide ab.
        bool ml = false;
    };

    // Datei aus einem Ordner ohne Analyse (sampleLibraries noAnalysis) oder
    // daraus gemacht (originsOf): Fehlertext, sonst ""
    juce::String blockedFileError (DawdContext&, const juce::File&);

    // Mix/Spur im Bereich (Songsekunden, track wie renderMixRange) mit
    // Material aus einem gesperrten Ordner: ml = Fehlertext mit Spur und
    // Datei; sonst ein Hinweis in ctx.licenceNotes und ""
    juce::String checkMixLicence (DawdContext&, double start, double end, int track, bool ml);

    // Hinweis zur Streuung zweier Renders (spread oben) oder "", wenn sie
    // gleich genug sind. who: "yours" | "reference"
    juce::String spreadHint (const AnalysisSource&, const juce::String& who);

    // file (+ start/end in Sekunden der Datei) oder der Mix wie bei analyze
    // (section, start/end, track). Laedt, was want* verlangt. Fehlertext oder "".
    juce::String loadAnalysisSource (DawdContext&, const juce::var& cmd, AnalysisSource&);

    // Quelle in die Antwort: source file|mix, Bereich, Spur
    void describeAnalysisSource (juce::DynamicObject&, const AnalysisSource&);

    // Raster: vorgegeben (bpm, firstDownbeat in Quellsekunden, beatsPerBar)
    // > Projekt-Raster (Quelle ist der eigene Mix) > geschaetzt (aus f).
    // from: given | project | detected. Fehlertext oder "".
    juce::String analysisGrid (DawdContext&, const juce::var& cmd, const AnalysisSource&,
                               const analysis::Features& f, analysis::TempoResult& g, juce::String& from);

    // Zwei Dateien desselben Ausschnitts (Vergleich): gibt nur eine Seite
    // das Raster an (bpm, firstDownbeat, beatsPerBar), gilt es fuer beide —
    // die Takt-Eins relativ zum jeweiligen start (Praxistest 1e: compareDrums
    // {file} ohne bpm erkannte das Raster der eigenen Datei falsch)
    void shareGrid (juce::var& a, juce::var& b);

    // Wie oben, aber die Datei ohne Raster uebernimmt es nur, wenn ihr
    // eigenes Tempo dazu passt (gleich, halb oder doppelt) — ein eigener
    // Render in anderem Tempo (Praxistest 2: Remix 120 BPM gegen die
    // Referenz mit 90 BPM) wurde sonst im fremden Tempo gerastert; dann
    // behaelt jede Seite ihr Raster und hints nennt es. a = eigene Seite.
    void shareGrid (DawdContext&, juce::var& a, juce::var& b, juce::StringArray& hints);

    // Takt/Schlag (1-basiert) einer Zeit ueber die Beat-Liste; vor der
    // ersten Eins <= 0 (Auftakt)
    std::pair<int, int> barBeatAt (const analysis::TempoResult&, double t);
}
