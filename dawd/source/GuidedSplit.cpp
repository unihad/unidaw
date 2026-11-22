#include "GuidedSplit.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <complex>

namespace dawd::guided
{
    namespace
    {
        constexpr int fftOrder = 12, fftSize = 1 << fftOrder, hop = 1024;
        constexpr int bins = fftSize / 2 + 1;
        constexpr int templates = 16, freeParts = 2, iterations = 150;   // 2 freie: das Riff im Auftakt landet im Rest, 1 schob es ins Pad
        constexpr double maxHz = 8000.0;
        constexpr float eps = 1.0e-10f;

        using Spectrum = std::vector<std::complex<float>>;   // frames x bins

        struct Stft
        {
            int frames = 0;
            Spectrum ch[2];
        };

        const std::vector<float>& window()
        {
            static const std::vector<float> w = []
            {
                std::vector<float> v (fftSize);

                for (int i = 0; i < fftSize; ++i)
                    v[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) fftSize);

                return v;
            }();

            return w;
        }

        Stft analyse (const Stereo& x)
        {
            juce::dsp::FFT fft (fftOrder);
            Stft s;
            const auto n = x[0].size();
            s.frames = (int) (n / hop) + 1;
            std::vector<float> buf (2 * fftSize);

            for (int c = 0; c < 2; ++c)
            {
                s.ch[c].assign ((size_t) s.frames * bins, {});

                for (int f = 0; f < s.frames; ++f)
                {
                    std::fill (buf.begin(), buf.end(), 0.0f);
                    const long start = (long) f * hop - fftSize / 2;   // zentriert

                    for (int i = 0; i < fftSize; ++i)
                    {
                        const long k = start + i;

                        if (k >= 0 && (size_t) k < n)
                            buf[(size_t) i] = x[(size_t) c][(size_t) k] * window()[(size_t) i];
                    }

                    fft.performRealOnlyForwardTransform (buf.data(), true);

                    for (int b = 0; b < bins; ++b)
                        s.ch[c][(size_t) f * bins + (size_t) b] = { buf[(size_t) (2 * b)], buf[(size_t) (2 * b + 1)] };
                }
            }

            return s;
        }

        std::vector<float> resynth (const Spectrum& spec, int frames, size_t length)
        {
            juce::dsp::FFT fft (fftOrder);
            std::vector<float> out (length, 0.0f), norm (length, 0.0f), buf (2 * fftSize);

            for (int f = 0; f < frames; ++f)
            {
                for (int b = 0; b < bins; ++b)
                {
                    auto v = spec[(size_t) f * bins + (size_t) b];
                    buf[(size_t) (2 * b)] = v.real();
                    buf[(size_t) (2 * b + 1)] = v.imag();
                }

                for (int b = bins; b < fftSize; ++b)   // konjugiert gespiegelt
                {
                    auto v = std::conj (spec[(size_t) f * bins + (size_t) (fftSize - b)]);
                    buf[(size_t) (2 * b)] = v.real();
                    buf[(size_t) (2 * b + 1)] = v.imag();
                }

                fft.performRealOnlyInverseTransform (buf.data());
                const long start = (long) f * hop - fftSize / 2;

                for (int i = 0; i < fftSize; ++i)
                {
                    const long k = start + i;

                    if (k >= 0 && (size_t) k < length)
                    {
                        const float w = window()[(size_t) i];
                        out[(size_t) k] += buf[(size_t) i] * w;
                        norm[(size_t) k] += w * w;
                    }
                }
            }

            for (size_t i = 0; i < length; ++i)
                out[i] = norm[i] > 1.0e-6f ? out[i] / norm[i] : 0.0f;

            return out;
        }

        int nmfBins (double rate)
        {
            return juce::jlimit (16, bins, (int) (maxHz / (rate / fftSize)) + 1);
        }

        // Betragsspektrum (L+R) bis maxHz: F x T, zeilenweise je Frequenz
        std::vector<float> magnitude (const Stft& s, int F)
        {
            std::vector<float> v ((size_t) F * (size_t) s.frames);

            for (int t = 0; t < s.frames; ++t)
                for (int f = 0; f < F; ++f)
                    v[(size_t) f * (size_t) s.frames + (size_t) t] = std::abs (s.ch[0][(size_t) t * bins + (size_t) f])
                                                                   + std::abs (s.ch[1][(size_t) t * bins + (size_t) f]);

            return v;
        }

        // KL-NMF, multiplikative Updates. W: F x K (Spalten summieren auf 1),
        // H: K x T. Die ersten `fixed` Spalten von W bleiben fest.
        void nmf (const std::vector<float>& V, int F, int T, std::vector<float>& W, int K, int fixed, std::vector<float>& H, juce::Random& rnd)
        {
            H.assign ((size_t) K * (size_t) T, 0.0f);

            for (auto& h : H)
                h = 0.1f + rnd.nextFloat();

            std::vector<float> WH ((size_t) F * (size_t) T), R ((size_t) F * (size_t) T), num;

            auto product = [&]
            {
                std::fill (WH.begin(), WH.end(), 0.0f);

                for (int f = 0; f < F; ++f)
                {
                    float* row = WH.data() + (size_t) f * (size_t) T;

                    for (int k = 0; k < K; ++k)
                    {
                        const float w = W[(size_t) f * (size_t) K + (size_t) k];
                        const float* h = H.data() + (size_t) k * (size_t) T;

                        for (int t = 0; t < T; ++t)
                            row[t] += w * h[t];
                    }
                }

                for (size_t i = 0; i < R.size(); ++i)
                    R[i] = V[i] / (WH[i] + eps);
            };

            auto normalise = [&]
            {
                for (int k = 0; k < K; ++k)
                {
                    float s = 0.0f;

                    for (int f = 0; f < F; ++f)
                        s += W[(size_t) f * (size_t) K + (size_t) k];

                    s = juce::jmax (s, eps);

                    for (int f = 0; f < F; ++f)
                        W[(size_t) f * (size_t) K + (size_t) k] /= s;

                    for (int t = 0; t < T; ++t)
                        H[(size_t) k * (size_t) T + (size_t) t] *= s;
                }
            };

            normalise();

            for (int it = 0; it < iterations; ++it)
            {
                // H *= W^T R (Spaltensummen von W sind 1)
                product();
                num.assign ((size_t) K * (size_t) T, 0.0f);

                for (int f = 0; f < F; ++f)
                {
                    const float* r = R.data() + (size_t) f * (size_t) T;

                    for (int k = 0; k < K; ++k)
                    {
                        const float w = W[(size_t) f * (size_t) K + (size_t) k];
                        float* n = num.data() + (size_t) k * (size_t) T;

                        for (int t = 0; t < T; ++t)
                            n[t] += w * r[t];
                    }
                }

                for (size_t i = 0; i < H.size(); ++i)
                    H[i] *= num[i];

                if (fixed >= K)
                    continue;

                // freie Spalten: W *= (R H^T) / (Zeilensummen von H)
                product();

                for (int k = fixed; k < K; ++k)
                {
                    const float* h = H.data() + (size_t) k * (size_t) T;
                    float hs = eps;

                    for (int t = 0; t < T; ++t)
                        hs += h[t];

                    for (int f = 0; f < F; ++f)
                    {
                        const float* r = R.data() + (size_t) f * (size_t) T;
                        float s = 0.0f;

                        for (int t = 0; t < T; ++t)
                            s += r[t] * h[t];

                        W[(size_t) f * (size_t) K + (size_t) k] *= s / hs;
                    }
                }

                normalise();
            }
        }
    }

    juce::String split (const std::vector<Stereo>& sources, const Stereo& target, double rate, std::vector<Stereo>& out)
    {
        if (sources.size() < 2)
            return "need at least two sources to split";

        if (target[0].size() < (size_t) fftSize || target[1].size() != target[0].size())
            return "target too short";

        const int F = nmfBins (rate);
        juce::Random rnd (17);

        // 1. Vorlagen je Instrument an der freien Stelle
        std::vector<std::vector<float>> learned;

        for (auto& src : sources)
        {
            if (src[0].size() < (size_t) fftSize || src[1].size() != src[0].size())
                return "source too short";

            auto s = analyse (src);
            auto V = magnitude (s, F);
            std::vector<float> W ((size_t) F * templates), H;

            for (auto& w : W)
                w = 0.1f + rnd.nextFloat();

            nmf (V, F, s.frames, W, templates, 0, H, rnd);
            learned.push_back (std::move (W));
        }

        // 2. Gemisch zerlegen: feste Vorlagen + freie Bausteine
        auto s = analyse (target);
        auto V = magnitude (s, F);
        const int fixed = templates * (int) sources.size();
        const int K = fixed + freeParts;
        std::vector<float> W ((size_t) F * (size_t) K), H;

        for (int f = 0; f < F; ++f)
        {
            for (size_t j = 0; j < learned.size(); ++j)
                for (int k = 0; k < templates; ++k)
                    W[(size_t) f * (size_t) K + j * templates + (size_t) k] = learned[j][(size_t) f * templates + (size_t) k];

            for (int k = fixed; k < K; ++k)
                W[(size_t) f * (size_t) K + (size_t) k] = 0.1f + rnd.nextFloat();
        }

        nmf (V, F, s.frames, W, K, fixed, H, rnd);

        // 3. Wiener-Masken je Gruppe (Quellen, Rest)
        const int T = s.frames;
        std::vector<float> total ((size_t) F * (size_t) T, eps);
        const int groups = (int) sources.size() + 1;
        std::vector<std::vector<float>> part ((size_t) groups, std::vector<float> ((size_t) F * (size_t) T, 0.0f));

        for (int g = 0; g < groups; ++g)
        {
            const int k0 = g < (int) sources.size() ? g * templates : fixed;
            const int k1 = g < (int) sources.size() ? k0 + templates : K;

            for (int f = 0; f < F; ++f)
                for (int k = k0; k < k1; ++k)
                {
                    const float w = W[(size_t) f * (size_t) K + (size_t) k];
                    const float* h = H.data() + (size_t) k * (size_t) T;
                    float* p = part[(size_t) g].data() + (size_t) f * (size_t) T;

                    for (int t = 0; t < T; ++t)
                        p[t] += w * h[t];
                }

            for (size_t i = 0; i < total.size(); ++i)
                total[i] += part[(size_t) g][i];
        }

        out.assign ((size_t) groups, {});

        for (int g = 0; g < groups; ++g)
        {
            for (int c = 0; c < 2; ++c)
            {
                Spectrum masked (s.ch[c].size());

                for (int t = 0; t < T; ++t)
                    for (int b = 0; b < bins; ++b)
                    {
                        const int f = juce::jmin (b, F - 1);   // ueber maxHz: Maske des obersten Bandes
                        const size_t i = (size_t) f * (size_t) T + (size_t) t;
                        masked[(size_t) t * bins + (size_t) b] = s.ch[c][(size_t) t * bins + (size_t) b] * (part[(size_t) g][i] / total[i]);
                    }

                out[(size_t) g][(size_t) c] = resynth (masked, T, target[0].size());
            }
        }

        return {};
    }
}
