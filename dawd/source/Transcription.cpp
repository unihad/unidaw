#include "Transcription.h"
#include "Resources.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#if UNIDAW_WITH_ONNX
 #include <onnxruntime_cxx_api.h>
#endif

namespace dawd::transcription
{
    namespace
    {
        // basic-pitch: 2-s-Fenster bei 22050 Hz, FFT-Hop 256, 30 Frames
        // Ueberlappung, 172 Frames und 88 Tonhoehen (MIDI 21..108) je Fenster
        constexpr double sampleRate = 22050.0;
        constexpr int fftHop = 256, windowSamples = 22050 * 2 - fftHop;   // 43844
        constexpr int overlapFrames = 30, overlapLen = overlapFrames * fftHop;
        constexpr int hopSize = windowSamples - overlapLen;
        constexpr int framesPerWindow = 172, pitches = 88, midiOffset = 21;
        constexpr int annotFps = 86;   // int (22050 / 256) wie in basic-pitch
        constexpr int energyTol = 11;

        juce::File modelFile()
        {
            return resourceDir ("models").getChildFile ("basic-pitch.onnx");
        }

        using Matrix = std::vector<std::array<float, pitches>>;

        // Frame -> Sekunden wie basic-pitch (model_frames_to_time): die
        // Fenster-Korrektur gleicht die nicht ganz 172 Frames je Fenster aus
        double frameTime (int frame)
        {
            const double windowOffset = (double) fftHop / sampleRate * (framesPerWindow - (double) windowSamples / fftHop) + 0.0018;
            return frame * fftHop / sampleRate - windowOffset * std::floor ((double) frame / framesPerWindow);
        }

        // Onsets aus dem Anstieg der Noten-Aktivierung ergaenzen (n_diff = 2)
        Matrix inferOnsets (const Matrix& onsets, const Matrix& frames)
        {
            const size_t n = frames.size();
            Matrix diff (n);
            float maxDiff = 0.0f, maxOnset = 0.0f;

            for (size_t t = 0; t < n; ++t)
                for (int p = 0; p < pitches; ++p)
                {
                    float d = 1e9f;

                    for (size_t k = 1; k <= 2; ++k)
                        d = std::min (d, frames[t][(size_t) p] - (t >= k ? frames[t - k][(size_t) p] : 0.0f));

                    diff[t][(size_t) p] = t < 2 ? 0.0f : std::max (0.0f, d);
                    maxDiff = std::max (maxDiff, diff[t][(size_t) p]);
                    maxOnset = std::max (maxOnset, onsets[t][(size_t) p]);
                }

            Matrix out (n);

            for (size_t t = 0; t < n; ++t)
                for (int p = 0; p < pitches; ++p)
                    out[t][(size_t) p] = std::max (onsets[t][(size_t) p],
                                                   maxDiff > 0.0f ? maxOnset * diff[t][(size_t) p] / maxDiff : 0.0f);

            return out;
        }

        // Port von basic-pitch output_to_notes_polyphonic: Noten ab den
        // Onset-Gipfeln vorwaerts verfolgen, solange die Aktivierung traegt;
        // danach (Melodia-Trick) uebrige Energie ohne Onset als Noten
        std::vector<Note> notesFrom (const Matrix& frames, Matrix onsets, const Options& o)
        {
            const int n = (int) frames.size();
            const int minLen = (int) std::round (o.minNoteMs / 1000.0 * (sampleRate / fftHop));
            std::vector<Note> notes;

            if (n < 3)
                return notes;

            onsets = inferOnsets (onsets, frames);
            Matrix energy = frames;

            struct Onset { int t, p; };
            std::vector<Onset> starts;

            for (int t = 1; t + 1 < n; ++t)
                for (int p = 0; p < pitches; ++p)
                {
                    auto v = onsets[(size_t) t][(size_t) p];

                    if (v >= o.onsetThreshold && v > onsets[(size_t) t - 1][(size_t) p] && v > onsets[(size_t) t + 1][(size_t) p])
                        starts.push_back ({ t, p });
                }

            // rueckwaerts in der Zeit (wie np.where(...)[::-1])
            std::reverse (starts.begin(), starts.end());

            auto add = [&] (int s, int e, int p)
            {
                double sum = 0.0;

                for (int t = s; t < e; ++t)
                    sum += frames[(size_t) t][(size_t) p];

                notes.push_back ({ frameTime (s), frameTime (e), p + midiOffset, e > s ? sum / (e - s) : 0.0 });
            };

            auto clear = [&] (int t, int p)
            {
                energy[(size_t) t][(size_t) p] = 0.0f;

                if (p + 1 < pitches) energy[(size_t) t][(size_t) p + 1] = 0.0f;
                if (p > 0) energy[(size_t) t][(size_t) p - 1] = 0.0f;
            };

            for (auto& st : starts)
            {
                if (st.t >= n - 1)
                    continue;

                int i = st.t + 1, k = 0;

                while (i < n - 1 && k < energyTol)
                {
                    k = energy[(size_t) i][(size_t) st.p] < o.frameThreshold ? k + 1 : 0;
                    ++i;
                }

                i -= k;

                if (i - st.t <= minLen)
                    continue;

                for (int t = st.t; t < i; ++t)
                    clear (t, st.p);

                add (st.t, i, st.p);
            }

            if (o.melodiaTrick)
            {
                for (;;)
                {
                    int bt = 0, bp = 0;
                    float best = 0.0f;

                    for (int t = 0; t < n; ++t)
                        for (int p = 0; p < pitches; ++p)
                            if (energy[(size_t) t][(size_t) p] > best)
                            {
                                best = energy[(size_t) t][(size_t) p];
                                bt = t;
                                bp = p;
                            }

                    if (best <= o.frameThreshold)
                        break;

                    energy[(size_t) bt][(size_t) bp] = 0.0f;

                    int i = bt + 1, k = 0;

                    while (i < n - 1 && k < energyTol)
                    {
                        k = energy[(size_t) i][(size_t) bp] < o.frameThreshold ? k + 1 : 0;
                        clear (i, bp);
                        ++i;
                    }

                    const int end = i - 1 - k;
                    i = bt - 1;
                    k = 0;

                    while (i > 0 && k < energyTol)
                    {
                        k = energy[(size_t) i][(size_t) bp] < o.frameThreshold ? k + 1 : 0;
                        clear (i, bp);
                        --i;
                    }

                    const int start = i + 1 + k;

                    if (end - start <= minLen)
                        continue;

                    add (start, end, bp);
                }
            }

            std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
            return notes;
        }

       #if UNIDAW_WITH_ONNX
        Ort::Env& env()
        {
            static Ort::Env e (ORT_LOGGING_LEVEL_WARNING, "dawd");
            return e;
        }

        Ort::Session& session()
        {
            static std::unique_ptr<Ort::Session> s;

            if (s == nullptr)
            {
                Ort::SessionOptions so;
                so.SetIntraOpNumThreads (2);
                s = std::make_unique<Ort::Session> (env(), modelFile().getFullPathName().toRawUTF8(), so);
            }

            return *s;
        }
       #endif
    }

    juce::String available()
    {
       #if UNIDAW_WITH_ONNX
        if (! modelFile().existsAsFile())
            return "basic-pitch model missing (" + modelFile().getFullPathName() + juce::String::fromUTF8 (") — run unidaw-fetch-models (AppImage: fetch-models; source tree: tools/fetch_ml.sh)");

        return {};
       #else
        return juce::String::fromUTF8 ("unidaw was built without ONNX Runtime — run tools/fetch_ml.sh, then rebuild (bash tests/run.sh)");
       #endif
    }

    std::vector<std::vector<float>> loadResampled (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length,
                                                   double targetRate, int channels)
    {
        std::vector<std::vector<float>> out ((size_t) channels);
        length = juce::jmin (length, reader.lengthInSamples - start);

        if (length <= 0 || reader.sampleRate <= 0.0)
            return out;

        const int chans = (int) juce::jmax (1u, reader.numChannels);
        constexpr int block = 1 << 16;
        juce::AudioBuffer<float> buf (juce::jmax (2, chans), block);

        for (auto& c : out)
            c.reserve ((size_t) length);

        for (juce::int64 pos = 0; pos < length; pos += block)
        {
            auto n = (int) juce::jmin ((juce::int64) block, length - pos);
            buf.clear();
            reader.read (&buf, 0, n, start + pos, true, true);   // Mono fuellt beide Kanaele

            for (int i = 0; i < n; ++i)
            {
                if (channels == 1)
                {
                    float s = 0.0f;

                    for (int c = 0; c < chans; ++c)
                        s += buf.getSample (c, i);

                    out[0].push_back (s / chans);
                }
                else
                    for (int c = 0; c < channels; ++c)
                        out[(size_t) c].push_back (buf.getSample (c, i));
            }
        }

        if (std::abs (reader.sampleRate - targetRate) < 0.5)
            return out;

        const double ratio = reader.sampleRate / targetRate;

        for (auto& ch : out)
        {
            // Tiefpass vor dem Herunterrechnen (4. Ordnung, 0,45 * Zielrate)
            if (reader.sampleRate > targetRate)
                for (int pass = 0; pass < 2; ++pass)
                {
                    juce::IIRFilter lp;
                    lp.setCoefficients (juce::IIRCoefficients::makeLowPass (reader.sampleRate, 0.45 * targetRate));
                    lp.processSamples (ch.data(), (int) ch.size());
                }

            std::vector<float> res ((size_t) ((double) ch.size() / ratio));
            juce::LagrangeInterpolator interp;
            interp.process (ratio, ch.data(), res.data(), (int) res.size(), (int) ch.size(), 0);
            ch = std::move (res);
        }

        return out;
    }

    std::vector<float> loadMono22k (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length)
    {
        auto ch = loadResampled (reader, start, length, sampleRate, 1);
        return ch.empty() ? std::vector<float>() : std::move (ch[0]);
    }

    std::vector<Note> transcribe (const std::vector<float>& audio, const Options& options, juce::String& error,
                                  Contour* contour)
    {
        if ((error = available()).isNotEmpty())
            return {};

       #if UNIDAW_WITH_ONNX
        try
        {
            // Vorne eine halbe Ueberlappung Stille, dann Fenster im Abstand hopSize
            std::vector<float> padded ((size_t) (overlapLen / 2), 0.0f);
            padded.insert (padded.end(), audio.begin(), audio.end());

            Matrix noteAct, onsetAct;
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            const char* inNames[] = { "serving_default_input_2:0" };
            const char* outNames[] = { "StatefulPartitionedCall:1", "StatefulPartitionedCall:2", "StatefulPartitionedCall:0" };   // note, onset, contour
            const size_t numOuts = contour != nullptr ? 3 : 2;
            const std::array<int64_t, 3> shape { 1, windowSamples, 1 };
            std::vector<float> window ((size_t) windowSamples);
            const int keep0 = overlapFrames / 2, keep1 = framesPerWindow - overlapFrames / 2;

            for (size_t i = 0; i < padded.size(); i += (size_t) hopSize)
            {
                std::fill (window.begin(), window.end(), 0.0f);
                std::copy_n (padded.begin() + (long) i, std::min ((size_t) windowSamples, padded.size() - i), window.begin());

                auto input = Ort::Value::CreateTensor<float> (mem, window.data(), window.size(), shape.data(), shape.size());
                auto outs = session().Run (Ort::RunOptions { nullptr }, inNames, &input, 1, outNames, numOuts);
                const float* note = outs[0].GetTensorData<float>();
                const float* onset = outs[1].GetTensorData<float>();
                const float* cont = numOuts > 2 ? outs[2].GetTensorData<float>() : nullptr;

                if (cont != nullptr && outs[2].GetTensorTypeAndShapeInfo().GetShape().back() != Contour::bins)
                    throw Ort::Exception ("unexpected contour shape", ORT_FAIL);

                for (int f = keep0; f < keep1; ++f)
                {
                    std::array<float, pitches> nr {}, orow {};

                    for (int p = 0; p < pitches; ++p)
                    {
                        nr[(size_t) p] = note[f * pitches + p];
                        orow[(size_t) p] = onset[f * pitches + p];
                    }

                    noteAct.push_back (nr);
                    onsetAct.push_back (orow);

                    if (cont != nullptr)
                    {
                        std::array<float, Contour::bins> crow {};
                        std::copy_n (cont + f * Contour::bins, Contour::bins, crow.begin());
                        contour->frames.push_back (crow);
                    }
                }
            }

            const auto frames = (size_t) std::floor ((double) audio.size() * annotFps / sampleRate);
            noteAct.resize (std::min (noteAct.size(), frames));
            onsetAct.resize (noteAct.size());

            if (contour != nullptr)
            {
                contour->frames.resize (noteAct.size());
                contour->times.clear();

                for (size_t t = 0; t < contour->frames.size(); ++t)
                    contour->times.push_back (frameTime ((int) t));
            }

            // Tonhoehen ausserhalb des Bereichs stummschalten
            for (size_t t = 0; t < noteAct.size(); ++t)
                for (int p = 0; p < pitches; ++p)
                    if (p + midiOffset < options.minPitch || p + midiOffset > options.maxPitch)
                        noteAct[t][(size_t) p] = onsetAct[t][(size_t) p] = 0.0f;

            return notesFrom (noteAct, onsetAct, options);
        }
        catch (const Ort::Exception& e)
        {
            error = juce::String ("transcription failed: ") + e.what();
            return {};
        }
       #else
        juce::ignoreUnused (audio, options);
        return {};
       #endif
    }
}
