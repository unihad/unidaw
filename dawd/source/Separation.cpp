#include "Separation.h"
#include "Resources.h"

#include <algorithm>
#include <memory>
#include <mutex>

#if JUCE_LINUX
 #include <malloc.h>
#endif

#if UNIDAW_WITH_ONNX
 #include <onnxruntime_cxx_api.h>
#endif

namespace dawd::separation
{
    namespace
    {
        // Je Modell: Segment, Schritt und Blende. HT-Demucs: 7,8 s, 25 %
        // Ueberlappung, Rampe ueber die ganze Ueberlappung; mega53: 10 s,
        // halbe Ueberlappung, Blende ueber ein Zehntel (MSST demix)
        struct Layout { int segment, stride, fade; size_t count; };

        Layout layoutOf (int stems)
        {
            if (stems == 53)
                return { 441000, 220500, 44100, classNames53.size() };

            return { 343980, 343980 - 343980 / 4, 343980 / 4, stems == 6 ? (size_t) 6 : (size_t) 4 };
        }

        juce::File modelFile (int stems)
        {
            const char* name = stems == 53 ? "mega53.onnx" : stems == 6 ? "htdemucs_6s.onnx" : "htdemucs.onnx";
            return resourceDir ("models").getChildFile (name);
        }

       #if UNIDAW_WITH_ONNX
        // Eine Session nur fuer die Dauer einer Trennung, ohne Speicher-Arena:
        // mit dauerhaften Sessions und Arena behielt dawd nach der ersten
        // Trennung 8,7 GB (4 Stems) bzw. 13 GB (dazu 6 Stems) fuer immer —
        // Suite- und Test-Instanzen fuellten den Rechner (2026-10-19). Das
        // Laden kostet je Trennung ~1-2 s. Ein Job nach dem anderen, damit nie
        // zwei Modelle gleichzeitig im Speicher liegen.
        Ort::Env& env()
        {
            static Ort::Env e (ORT_LOGGING_LEVEL_WARNING, "dawd-demucs");
            return e;
        }

        std::mutex& runLock()
        {
            static std::mutex m;
            return m;
        }

        std::unique_ptr<Ort::Session> openSession (int stems)
        {
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads (juce::jmax (1, juce::SystemStats::getNumCpus() - 1));
            // mega53: Optimieren kostete 2 min Laden je Job (6 s ohne) und
            // machte den Lauf nicht schneller (Messung 2026-10-21)
            so.SetGraphOptimizationLevel (stems == 53 ? GraphOptimizationLevel::ORT_DISABLE_ALL : GraphOptimizationLevel::ORT_ENABLE_ALL);
            so.DisableCpuMemArena();
            so.DisableMemPattern();
            return std::make_unique<Ort::Session> (env(), modelFile (stems).getFullPathName().toRawUTF8(), so);
        }
       #endif
    }

    juce::String available (int stems)
    {
       #if UNIDAW_WITH_ONNX
        if (stems == 53 && ! modelFile (stems).existsAsFile())
            return juce::String::fromUTF8 ("53-class model missing (") + modelFile (stems).getFullPathName()
                   + juce::String::fromUTF8 (") — run MEGA53=1 tools/fetch_ml.sh (exports it, needs Python with torch)");

        if (! modelFile (stems).existsAsFile())
            return juce::String::fromUTF8 (stems == 6 ? "HT-Demucs 6-stem model missing (" : "HT-Demucs model missing (")
                   + modelFile (stems).getFullPathName() + juce::String::fromUTF8 (") — run unidaw-fetch-models (AppImage: fetch-models; source tree: tools/fetch_ml.sh)");

        return {};
       #else
        juce::ignoreUnused (stems);
        return juce::String::fromUTF8 ("unidaw was built without ONNX Runtime — run tools/fetch_ml.sh, then rebuild (bash tests/run.sh)");
       #endif
    }

    bool separate (const std::vector<float>& left, const std::vector<float>& right, int stems, Stems& out,
                   const std::function<bool (double)>& progress, juce::String& error, const std::vector<bool>& keep)
    {
        if ((error = available (stems)).isNotEmpty())
            return false;

       #if UNIDAW_WITH_ONNX
        try
        {
            const auto [segment, stride, fade, count] = layoutOf (stems);
            const size_t total = left.size();
            // mega53 (36 s je Segment): nur so viele Segmente, bis das Ende
            // bedeckt ist; HT-Demucs wie bisher (Ergebnisse bleiben gleich)
            const size_t chunks = stems == 53
                ? 1 + (total > (size_t) segment ? (total - (size_t) segment + (size_t) stride - 1) / (size_t) stride : 0)
                : std::max<size_t> (1, (total + (size_t) stride - 1) / (size_t) stride);
            auto kept = [&keep] (size_t s) { return keep.empty() || (s < keep.size() && keep[s]); };

            out.assign (count, {});

            for (size_t s = 0; s < count; ++s)
                if (kept (s))
                    for (auto& ch : out[s])
                        ch.assign (total, 0.0f);

            std::vector<float> weight (total, 0.0f), window ((size_t) segment, 1.0f);

            for (int i = 0; i < fade; ++i)
            {
                auto f = (float) i / (float) (fade - 1);
                window[(size_t) i] = f;
                window[(size_t) (segment - 1 - i)] = f;
            }

            // Nach der Trennung den Speicher ans System zurueckgeben (glibc
            // behielt sonst ~4 GB); der Guard laeuft nach dem Ende der Session
            const juce::ScopeGuard trim { []
            {
               #if JUCE_LINUX
                malloc_trim (0);
               #endif
            } };
            const std::lock_guard<std::mutex> one (runLock());
            auto sess = openSession (stems);
            auto mem = Ort::MemoryInfo::CreateCpu (OrtDeviceAllocator, OrtMemTypeDefault);
            const char* inNames[] = { "mix" };
            const char* outNames[] = { "stems" };
            const std::array<int64_t, 3> shape { 1, 2, segment };
            std::vector<float> input (2 * (size_t) segment);

            for (size_t c = 0; c < chunks; ++c)
            {
                if (! progress ((double) c / (double) chunks))
                {
                    error = "cancelled";
                    return false;
                }

                const size_t start = c * (size_t) stride;
                const size_t len = std::min ((size_t) segment, total - std::min (total, start));
                std::fill (input.begin(), input.end(), 0.0f);
                std::copy_n (left.begin() + (long) start, len, input.begin());
                std::copy_n (right.begin() + (long) start, len, input.begin() + segment);

                auto in = Ort::Value::CreateTensor<float> (mem, input.data(), input.size(), shape.data(), shape.size());
                auto res = sess->Run (Ort::RunOptions { nullptr }, inNames, &in, 1, outNames, 1);
                const float* data = res[0].GetTensorData<float>();   // (1, 4/6/53, 2, segment)

                for (size_t s = 0; s < count; ++s)
                    for (size_t ch = 0; ch < 2 && kept (s); ++ch)
                    {
                        const float* src = data + (s * 2 + ch) * (size_t) segment;
                        auto& dst = out[s][ch];

                        for (size_t i = 0; i < len; ++i)
                            dst[start + i] += src[i] * window[i];
                    }

                for (size_t i = 0; i < len; ++i)
                    weight[start + i] += window[i];
            }

            for (auto& stem : out)
                for (auto& ch : stem)
                    for (size_t i = 0; i < ch.size(); ++i)   // leer = nicht behalten
                        ch[i] /= std::max (weight[i], 1e-8f);

            progress (1.0);
            return true;
        }
        catch (const Ort::Exception& e)
        {
            error = juce::String ("separation failed: ") + e.what();
            return false;
        }
       #else
        juce::ignoreUnused (left, right, stems, out, progress, keep);
        return false;
       #endif
    }
}
