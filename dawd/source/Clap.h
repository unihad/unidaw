#pragma once

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

// Klang-Vektoren mit CLAP (LAION larger_clap_general, Apache 2.0) ueber ONNX
// Runtime: Audio und Text landen im selben 512-dim. Raum — eine Anfrage
// "metallic snare" und ein Sample sind so direkt vergleichbar (Kosinus), ein
// Sample mit einem anderen genauso. Audio-Encoder int8-quantisiert (gleiche
// Treffer wie fp32, schneller), Text-Encoder fp32 (int8 verschiebt die
// Treffer). Modelle und Tokenizer laedt tools/fetch_ml.sh nach dawd/models/.
namespace dawd::clap
{
    constexpr int dim = 512;
    constexpr double sampleRate = 48000.0;
    constexpr int windowSamples = 480000;   // 10 s je Fenster

    // "" wenn nutzbar, sonst der Grund (ONNX Runtime oder Modell fehlt)
    juce::String available();

    // Log-Mel wie ClapFeatureExtractor (rand_trunc/repeatpad): kuerzere
    // Fenster werden wiederholt und mit Stille aufgefuellt, dann 1001 Frames
    // x 64 Mel-Baender (Slaney), dB. audio: Mono bei 48 kHz, hoechstens 10 s.
    std::vector<float> logMel (const float* audio, size_t length);

    // Ein Encoder je Thread (eigene ONNX-Session mit einem Thread — mehrere
    // Sessions parallel nutzen die Kerne besser als eine mit vielen Threads)
    class AudioEncoder
    {
    public:
        AudioEncoder();
        ~AudioEncoder();

        // Normierter Vektor je 10-s-Fenster; leer bei Fehler (error gesetzt)
        std::vector<float> embed (const float* audio, size_t length, juce::String& error);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // Normierter Text-Vektor (Englisch). Laedt Modell und Tokenizer beim
    // ersten Aufruf; thread-sicher.
    std::vector<float> embedText (const juce::String& text, juce::String& error);

    // RoBERTa-Byte-Level-BPE wie tokenizer.json (mit <s> ... </s>); fuer Tests
    std::vector<int> tokenize (const juce::String& text, juce::String& error);

    // Vektor als Text fuer JSON-Kataloge (Preset-/Klang-Kataloge in
    // plugin-profiles/): int8 (x127) als base64, 512 Byte -> ~700 Zeichen.
    // decode normiert wieder; leer bei falscher Laenge.
    juce::String encode (const std::vector<float>& v);
    std::vector<float> decode (const juce::String& text);
    // Kosinus normierter Vektoren
    float similarity (const std::vector<float>& a, const std::vector<float>& b);
}
