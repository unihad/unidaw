#include "Clap.h"
#include "Resources.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#if UNIDAW_WITH_ONNX
 #include <onnxruntime_cxx_api.h>
#endif

namespace dawd::clap
{
    namespace
    {
        constexpr int nFft = 1024, hop = 480, bins = nFft / 2 + 1, mels = 64, frames = windowSamples / hop + 1;   // 1001

        juce::File modelsDir()
        {
            return resourceDir ("models");
        }

        juce::File audioModel()  { return modelsDir().getChildFile ("clap-audio-q8.onnx"); }
        juce::File textModel()   { return modelsDir().getChildFile ("clap-text.onnx"); }
        juce::File tokenizerFile() { return modelsDir().getChildFile ("clap-tokenizer.json"); }

        // Slaney-Mel wie librosa / transformers.mel_filter_bank (norm slaney)
        double hzToMel (double f)
        {
            const double logStep = 27.0 / std::log (6.4);
            return f < 1000.0 ? 3.0 * f / 200.0 : 15.0 + std::log (f / 1000.0) * logStep;
        }

        double melToHz (double m)
        {
            const double logStep = std::log (6.4) / 27.0;
            return m < 15.0 ? 200.0 * m / 3.0 : 1000.0 * std::exp (logStep * (m - 15.0));
        }

        // filters[bin * mels + m]
        const std::vector<float>& melFilters()
        {
            static const std::vector<float> f = []
            {
                std::vector<double> edges (mels + 2);
                const double lo = hzToMel (50.0), hi = hzToMel (14000.0);

                for (int i = 0; i < mels + 2; ++i)
                    edges[(size_t) i] = melToHz (lo + (hi - lo) * i / (mels + 1));

                std::vector<float> out ((size_t) (bins * mels), 0.0f);

                for (int b = 0; b < bins; ++b)
                {
                    const double hz = sampleRate / 2.0 * b / (bins - 1);

                    for (int m = 0; m < mels; ++m)
                    {
                        auto e0 = edges[(size_t) m], e1 = edges[(size_t) m + 1], e2 = edges[(size_t) m + 2];
                        auto down = (hz - e0) / (e1 - e0), up = (e2 - hz) / (e2 - e1);
                        auto w = std::max (0.0, std::min (down, up)) * 2.0 / (e2 - e0);
                        out[(size_t) (b * mels + m)] = (float) w;
                    }
                }

                return out;
            }();

            return f;
        }

        const std::vector<float>& hannWindow()
        {
            static const std::vector<float> w = []
            {
                std::vector<float> v ((size_t) nFft);

                for (int i = 0; i < nFft; ++i)
                    v[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / nFft));   // periodisch

                return v;
            }();

            return w;
        }

       #if UNIDAW_WITH_ONNX
        Ort::Env& env()
        {
            static Ort::Env e (ORT_LOGGING_LEVEL_WARNING, "dawd-clap");
            return e;
        }

        std::vector<float> normalized (const float* v)
        {
            double n = 0.0;

            for (int i = 0; i < dim; ++i)
                n += (double) v[i] * v[i];

            n = std::sqrt (std::max (n, 1e-20));
            std::vector<float> out ((size_t) dim);

            for (int i = 0; i < dim; ++i)
                out[(size_t) i] = (float) (v[i] / n);

            return out;
        }
       #endif

        // ---- Tokenizer (GPT-2/RoBERTa Byte-Level-BPE) ----

        struct Tokenizer
        {
            std::unordered_map<std::string, int> vocab;
            std::unordered_map<std::string, int> ranks;   // "a b" -> Rang
            std::array<std::string, 256> byteChar;        // Byte -> UTF-8 des Ersatzzeichens
            int bos = 0, eos = 2;
            std::map<std::string, std::vector<int>> cache;

            juce::String load (const juce::File& file)
            {
                auto json = juce::JSON::parse (file);
                auto model = json.getProperty ("model", {});

                if (auto* v = model.getProperty ("vocab", {}).getDynamicObject())
                    for (auto& p : v->getProperties())
                        vocab[p.name.toString().toStdString()] = (int) p.value;

                if (auto* m = model.getProperty ("merges", {}).getArray())
                    for (int i = 0; i < m->size(); ++i)
                        ranks[(*m)[i].toString().toStdString()] = i;

                if (vocab.empty() || ranks.empty())
                    return "cannot read tokenizer: " + file.getFullPathName();

                // bytes_to_unicode: druckbare Bytes bleiben, der Rest ab U+0100
                int next = 0;

                for (int b = 0; b < 256; ++b)
                {
                    bool printable = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
                    auto cp = (juce::juce_wchar) (printable ? b : 256 + next++);
                    byteChar[(size_t) b] = juce::String::charToString (cp).toStdString();
                }

                bos = vocab.count ("<s>") ? vocab["<s>"] : 0;
                eos = vocab.count ("</s>") ? vocab["</s>"] : 2;
                return {};
            }

            std::vector<int> bpe (const std::string& word)
            {
                if (auto it = cache.find (word); it != cache.end())
                    return it->second;

                // Symbole = einzelne UTF-8-Zeichen des Ersatz-Strings
                std::vector<std::string> sym;

                for (size_t i = 0; i < word.size();)
                {
                    auto c = (unsigned char) word[i];
                    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
                    sym.push_back (word.substr (i, len));
                    i += len;
                }

                while (sym.size() > 1)
                {
                    int best = -1;
                    size_t at = 0;

                    for (size_t i = 0; i + 1 < sym.size(); ++i)
                        if (auto it = ranks.find (sym[i] + " " + sym[i + 1]); it != ranks.end() && (best < 0 || it->second < best))
                        {
                            best = it->second;
                            at = i;
                        }

                    if (best < 0)
                        break;

                    auto a = sym[at], b = sym[at + 1];
                    std::vector<std::string> merged;

                    for (size_t i = 0; i < sym.size(); ++i)
                    {
                        if (i + 1 < sym.size() && sym[i] == a && sym[i + 1] == b)
                        {
                            merged.push_back (a + b);
                            ++i;
                        }
                        else
                            merged.push_back (sym[i]);
                    }

                    sym = std::move (merged);
                }

                std::vector<int> ids;

                for (auto& s : sym)
                    if (auto it = vocab.find (s); it != vocab.end())
                        ids.push_back (it->second);

                if (cache.size() < 10000)
                    cache[word] = ids;

                return ids;
            }

            // Vor-Zerlegung wie die GPT-2-Regex:
            // 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
            static std::vector<juce::String> pieces (const juce::String& text)
            {
                std::vector<juce::juce_wchar> c;

                for (auto p = text.getCharPointer(); ! p.isEmpty();)
                    c.push_back (p.getAndAdvance());

                // \p{L} und \p{N} ohne Locale (iswalpha kennt in der C-Locale
                // keine Umlaute): ASCII, Latin-1/Extended, Griechisch,
                // Kyrillisch; alles ab U+3000 (CJK) zaehlt als Buchstabe
                auto isL = [] (juce::juce_wchar ch)
                {
                    if (ch < 0x80)
                        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');

                    return (ch >= 0xC0 && ch <= 0x24F && ch != 0xD7 && ch != 0xF7) || ch == 0xAA || ch == 0xB5 || ch == 0xBA
                        || (ch >= 0x370 && ch <= 0x52F && ch != 0x37E && ch != 0x387) || ch >= 0x3040;
                };
                auto isN = [] (juce::juce_wchar ch)
                {
                    return (ch >= '0' && ch <= '9') || ch == 0xB2 || ch == 0xB3 || ch == 0xB9 || (ch >= 0xBC && ch <= 0xBE);
                };
                auto isS = [] (juce::juce_wchar ch) { return juce::CharacterFunctions::isWhitespace (ch); };
                auto isO = [&] (juce::juce_wchar ch) { return ! isS (ch) && ! isL (ch) && ! isN (ch); };
                const size_t n = c.size();
                std::vector<juce::String> out;
                auto take = [&] (size_t from, size_t to)
                {
                    juce::String s;

                    for (size_t k = from; k < to; ++k)
                        s += juce::String::charToString (c[k]);

                    out.push_back (s);
                };

                for (size_t i = 0; i < n;)
                {
                    if (c[i] == '\'')
                    {
                        bool hit = false;

                        for (const char* suffix : { "re", "ve", "ll", "s", "t", "m", "d" })
                        {
                            size_t len = std::strlen (suffix), k = 0;

                            while (k < len && i + 1 + k < n && c[i + 1 + k] == (juce::juce_wchar) suffix[k])
                                ++k;

                            if (k == len)
                            {
                                take (i, i + 1 + len);
                                i += 1 + len;
                                hit = true;
                                break;
                            }
                        }

                        if (hit)
                            continue;
                    }

                    // " ?" vor Buchstaben, Ziffern, Sonstigem
                    size_t start = i, j = i;

                    if (c[j] == ' ' && j + 1 < n && ! isS (c[j + 1]))
                        ++j;

                    if (j < n && ! isS (c[j]))
                    {
                        auto cls = isL (c[j]) ? 0 : isN (c[j]) ? 1 : 2;
                        auto same = [&] (juce::juce_wchar ch) { return cls == 0 ? isL (ch) : cls == 1 ? isN (ch) : isO (ch); };

                        while (j < n && same (c[j]))
                            ++j;

                        take (start, j);
                        i = j;
                        continue;
                    }

                    // Leerraum: \s+(?!\S) laesst das letzte Zeichen vor einem
                    // Wort stehen (das holt sich " ?"), sonst \s+
                    while (j < n && isS (c[j]))
                        ++j;

                    if (j < n && j - i > 1)
                        --j;

                    take (i, j);
                    i = j;
                }

                return out;
            }

            std::vector<int> encode (const juce::String& text)
            {
                std::vector<int> ids { bos };

                for (auto& piece : pieces (text))
                {
                    std::string mapped;
                    auto utf8 = piece.toStdString();

                    for (unsigned char b : utf8)
                        mapped += byteChar[b];

                    for (auto id : bpe (mapped))
                        ids.push_back (id);
                }

                ids.push_back (eos);
                return ids;
            }
        };

        std::mutex textLock;
        std::unique_ptr<Tokenizer> tokenizer;
       #if UNIDAW_WITH_ONNX
        std::unique_ptr<Ort::Session> textSession;
       #endif

        juce::String loadTokenizer()
        {
            if (tokenizer != nullptr)
                return {};

            auto t = std::make_unique<Tokenizer>();

            if (auto err = t->load (tokenizerFile()); err.isNotEmpty())
                return err;

            tokenizer = std::move (t);
            return {};
        }
    }

    juce::String available()
    {
       #if UNIDAW_WITH_ONNX
        for (auto& f : { audioModel(), textModel(), tokenizerFile() })
            if (! f.existsAsFile())
                return "CLAP model missing (" + f.getFullPathName() + juce::String::fromUTF8 (") — run unidaw-fetch-models (AppImage: fetch-models; source tree: tools/fetch_ml.sh)");

        return {};
       #else
        return juce::String::fromUTF8 ("unidaw was built without ONNX Runtime — run tools/fetch_ml.sh, then rebuild (bash tests/run.sh)");
       #endif
    }

    std::vector<float> logMel (const float* audio, size_t length)
    {
        // repeatpad: kuerzere Fenster so oft wiederholen, wie ganz hineinpasst,
        // dann mit Stille auffuellen
        length = std::min (length, (size_t) windowSamples);
        std::vector<float> wave ((size_t) windowSamples, 0.0f);

        if (length > 0)
        {
            size_t repeats = (size_t) windowSamples / length;

            for (size_t r = 0; r < repeats; ++r)
                std::copy (audio, audio + length, wave.begin() + (long) (r * length));
        }

        // center = True: reflektierend um nFft / 2 erweitern
        const int pad = nFft / 2;
        std::vector<float> padded ((size_t) (windowSamples + 2 * pad));

        for (int i = 0; i < windowSamples; ++i)
            padded[(size_t) (i + pad)] = wave[(size_t) i];

        for (int i = 0; i < pad; ++i)
        {
            padded[(size_t) (pad - 1 - i)] = wave[(size_t) (i + 1)];
            padded[(size_t) (pad + windowSamples + i)] = wave[(size_t) (windowSamples - 2 - i)];
        }

        juce::dsp::FFT fft (10);
        std::vector<float> buf ((size_t) (2 * nFft)), power ((size_t) bins);
        std::vector<float> out ((size_t) (frames * mels));
        const auto& win = hannWindow();
        const auto& filters = melFilters();

        for (int f = 0; f < frames; ++f)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);

            for (int i = 0; i < nFft; ++i)
                buf[(size_t) i] = padded[(size_t) (f * hop + i)] * win[(size_t) i];

            fft.performRealOnlyForwardTransform (buf.data(), true);

            for (int b = 0; b < bins; ++b)
            {
                auto re = buf[(size_t) (2 * b)], im = buf[(size_t) (2 * b + 1)];
                power[(size_t) b] = re * re + im * im;
            }

            for (int m = 0; m < mels; ++m)
            {
                double s = 0.0;

                for (int b = 0; b < bins; ++b)
                    s += (double) filters[(size_t) (b * mels + m)] * power[(size_t) b];

                out[(size_t) (f * mels + m)] = (float) (10.0 * std::log10 (std::max (s, 1e-10)));
            }
        }

        return out;
    }

    struct AudioEncoder::Impl
    {
       #if UNIDAW_WITH_ONNX
        std::unique_ptr<Ort::Session> session;
       #endif
    };

    AudioEncoder::AudioEncoder() : impl (std::make_unique<Impl>()) {}
    AudioEncoder::~AudioEncoder() = default;

    std::vector<float> AudioEncoder::embed (const float* audio, size_t length, juce::String& error)
    {
        if ((error = available()).isNotEmpty())
            return {};

       #if UNIDAW_WITH_ONNX
        try
        {
            if (impl->session == nullptr)
            {
                Ort::SessionOptions so;
                so.SetIntraOpNumThreads (1);
                so.SetInterOpNumThreads (1);
                impl->session = std::make_unique<Ort::Session> (env(), audioModel().getFullPathName().toRawUTF8(), so);
            }

            auto mel = logMel (audio, length);
            std::array<int64_t, 4> shape { 1, 1, frames, mels };
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            auto in = Ort::Value::CreateTensor<float> (mem, mel.data(), mel.size(), shape.data(), shape.size());
            const char* inNames[] = { "input_features" };
            const char* outNames[] = { "audio_embeds" };
            auto res = impl->session->Run (Ort::RunOptions { nullptr }, inNames, &in, 1, outNames, 1);
            return normalized (res[0].GetTensorData<float>());
        }
        catch (const Ort::Exception& e)
        {
            error = juce::String ("CLAP audio: ") + e.what();
            return {};
        }
       #else
        juce::ignoreUnused (audio, length);
        return {};
       #endif
    }

    std::vector<int> tokenize (const juce::String& text, juce::String& error)
    {
        const std::lock_guard<std::mutex> sl (textLock);

        if ((error = loadTokenizer()).isNotEmpty())
            return {};

        return tokenizer->encode (text);
    }

    std::vector<float> embedText (const juce::String& text, juce::String& error)
    {
        if ((error = available()).isNotEmpty())
            return {};

       #if UNIDAW_WITH_ONNX
        const std::lock_guard<std::mutex> sl (textLock);

        try
        {
            if ((error = loadTokenizer()).isNotEmpty())
                return {};

            if (textSession == nullptr)
            {
                Ort::SessionOptions so;
                so.SetIntraOpNumThreads (4);
                textSession = std::make_unique<Ort::Session> (env(), textModel().getFullPathName().toRawUTF8(), so);
            }

            auto ids32 = tokenizer->encode (text);
            std::vector<int64_t> ids (ids32.begin(), ids32.end());
            std::array<int64_t, 2> shape { 1, (int64_t) ids.size() };
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            auto in = Ort::Value::CreateTensor<int64_t> (mem, ids.data(), ids.size(), shape.data(), shape.size());
            const char* inNames[] = { "input_ids" };
            const char* outNames[] = { "text_embeds" };
            auto res = textSession->Run (Ort::RunOptions { nullptr }, inNames, &in, 1, outNames, 1);
            return normalized (res[0].GetTensorData<float>());
        }
        catch (const Ort::Exception& e)
        {
            error = juce::String ("CLAP text: ") + e.what();
            return {};
        }
       #else
        juce::ignoreUnused (text);
        return {};
       #endif
    }

    juce::String encode (const std::vector<float>& v)
    {
        if (v.size() != (size_t) dim)
            return {};

        juce::MemoryBlock mb ((size_t) dim);

        for (int i = 0; i < dim; ++i)
            mb[(size_t) i] = (char) (juce::int8) juce::jlimit (-127, 127, juce::roundToInt (v[(size_t) i] * 127.0f));

        return juce::Base64::toBase64 (mb.getData(), mb.getSize());   // Standard-base64 (Python: base64)
    }

    std::vector<float> decode (const juce::String& text)
    {
        juce::MemoryOutputStream out;

        if (! juce::Base64::convertFromBase64 (out, text) || out.getDataSize() != (size_t) dim)
            return {};

        auto mb = out.getMemoryBlock();

        std::vector<float> v ((size_t) dim);
        double norm = 0.0;

        for (int i = 0; i < dim; ++i)
        {
            v[(size_t) i] = (float) (juce::int8) mb[(size_t) i];
            norm += (double) v[(size_t) i] * v[(size_t) i];
        }

        norm = std::sqrt (std::max (norm, 1e-20));

        for (auto& x : v)
            x = (float) (x / norm);

        return v;
    }

    float similarity (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return 0.0f;

        double s = 0.0;

        for (size_t i = 0; i < a.size(); ++i)
            s += (double) a[i] * b[i];

        return (float) s;
    }
}
