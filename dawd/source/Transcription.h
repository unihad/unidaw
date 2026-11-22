#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <vector>

// Noten aus Audio (Audio -> MIDI) mit basic-pitch (Spotify, Apache 2.0) ueber
// ONNX Runtime (MIT). Beides liegt nicht im Repo: tools/fetch_ml.sh laedt sie,
// CMake schaltet UNIDAW_WITH_ONNX ein, wenn ONNX Runtime da ist. Ohne meldet
// available() den Grund.
namespace dawd::transcription
{
    struct Note
    {
        double start = 0.0, end = 0.0;   // Sekunden ab Quellen-Start
        int pitch = 60;
        double amplitude = 0.0;          // 0..1 (Velocity = 127 * amplitude)
    };

    struct Options
    {
        double onsetThreshold = 0.5;     // basic-pitch-Defaults
        double frameThreshold = 0.3;
        double minNoteMs = 127.7;
        int minPitch = 21, maxPitch = 108;
        bool melodiaTrick = true;
    };

    // "" wenn nutzbar, sonst der Grund (Bibliothek oder Modell fehlt)
    juce::String available();

    // Audio auf eine feste Rate bringen (Tiefpass + Lagrange): channels 1 =
    // Mono-Mix, 2 = Stereo (Mono doppelt). Je Kanal ein Vektor.
    std::vector<std::vector<float>> loadResampled (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length,
                                                   double targetRate, int channels);

    // Mono-Audio bei genau 22050 Hz (basic-pitch-Rate)
    std::vector<float> loadMono22k (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length);

    // Tonhoehen-Verlauf (Contour) von basic-pitch: je Frame 264 Bins, drei
    // je Halbton ab MIDI 21 (Bin b = MIDI 21 + b / 3), Werte 0..1. times[i]
    // = Sekunden ab Quellen-Start. Daraus lesen sich Glides, Bends, Vibrato
    // und Feinstimmung, die die Noten (ganze Halbtoene) nicht zeigen.
    struct Contour
    {
        static constexpr int bins = 264, binsPerSemitone = 3, lowestMidi = 21;
        std::vector<std::array<float, bins>> frames;
        std::vector<double> times;
    };

    std::vector<Note> transcribe (const std::vector<float>& audio22k, const Options& options, juce::String& error,
                                  Contour* contour = nullptr);
}
