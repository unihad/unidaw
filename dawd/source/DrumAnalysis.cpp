#include "DrumAnalysis.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace dawd::drums
{
    namespace
    {
        constexpr int fftOrder = 10, N = 1 << fftOrder, hop = 128;
        constexpr double edges[] = { 30, 70, 110, 160, 250, 355, 500, 710, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 11300, 18000 };
        constexpr int NB = (int) std::size (edges) - 1;
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();

        double centre (int b) { return std::sqrt (edges[b] * edges[b + 1]); }

        double quantile (std::vector<double> v, double q)
        {
            if (v.empty())
                return nan;

            std::sort (v.begin(), v.end());
            return v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) (q * (double) (v.size() - 1)))];
        }

        double medianOf (std::vector<double> v) { return v.empty() ? nan : quantile (v, 0.5); }

        // Leistungsspektrum je Frame (Hann, Mitte bei t * hop)
        std::vector<std::vector<float>> powerFrames (const std::vector<float>& x)
        {
            juce::dsp::FFT fft (fftOrder);
            std::vector<float> win (N), buf (2 * N);

            for (int i = 0; i < N; ++i)
                win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (N - 1)));

            std::vector<std::vector<float>> out;

            for (size_t c = 0; c < x.size(); c += hop)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);

                for (int k = 0; k < N; ++k)
                {
                    auto j = (long) c - N / 2 + k;

                    if (j >= 0 && j < (long) x.size())
                        buf[(size_t) k] = x[(size_t) j] * win[(size_t) k];
                }

                fft.performFrequencyOnlyForwardTransform (buf.data());
                std::vector<float> p ((size_t) N / 2 + 1);

                for (int b = 0; b <= N / 2; ++b)
                    p[(size_t) b] = buf[(size_t) b] * buf[(size_t) b];

                out.push_back (std::move (p));
            }

            return out;
        }

        // SuperFlux ab 150 Hz: log-Betrag, Max-Filter ueber +-1 Bin, Anstieg
        // gegen das Maximum 3-4 Frames davor
        std::vector<double> superflux (const std::vector<std::vector<float>>& spec, double rate, double lo = 150.0, double hi = 16000.0)
        {
            const double binHz = rate / N;
            const int b0 = juce::jmax (1, (int) (lo / binHz)), b1 = juce::jmin (N / 2 - 1, (int) (hi / binHz));
            const int W = b1 - b0 + 1, lag = 3, T = (int) spec.size();
            std::vector<std::vector<double>> L ((size_t) T, std::vector<double> ((size_t) W + 2)), M ((size_t) T, std::vector<double> ((size_t) W));

            for (int t = 0; t < T; ++t)
            {
                for (int i = 0; i < W + 2; ++i)
                    L[(size_t) t][(size_t) i] = std::log10 (1.0 + 1.0e4 * std::sqrt ((double) spec[(size_t) t][(size_t) (b0 - 1 + i)]));

                for (int i = 0; i < W; ++i)
                    M[(size_t) t][(size_t) i] = juce::jmax (L[(size_t) t][(size_t) i], L[(size_t) t][(size_t) i + 1], L[(size_t) t][(size_t) i + 2]);
            }

            std::vector<double> odf ((size_t) T, 0.0);

            for (int t = lag + 1; t < T; ++t)
            {
                double s = 0.0;

                for (int i = 0; i < W; ++i)
                    s += juce::jmax (0.0, L[(size_t) t][(size_t) i + 1] - juce::jmax (M[(size_t) (t - lag)][(size_t) i], M[(size_t) (t - lag - 1)][(size_t) i]));

                odf[(size_t) t] = s / W;
            }

            return odf;
        }

        // Gipfel mit Schwelle = gleitendes Mittel (+-60 ms) + 6 % des 90.
        // Perzentils, Hysterese, +-12 ms lokales Maximum, 35 ms Abstand (ab
        // 15 ms zaehlt ein Anstieg im Koerper-Band body doch), Parabel fuer
        // die Feinlage. (Frame, Staerke relativ zum P90)
        std::vector<std::pair<double, double>> peaks (const std::vector<double>& odf, double fps, const std::vector<double>* body = nullptr)
        {
            const int T = (int) odf.size();
            std::vector<double> pk;

            for (int t = 1; t + 1 < T; ++t)
                if (odf[(size_t) t] >= odf[(size_t) t - 1] && odf[(size_t) t] > odf[(size_t) t + 1])
                    pk.push_back (odf[(size_t) t]);

            const double p90 = pk.empty() ? 1.0 : juce::jmax (1.0e-12, quantile (pk, 0.9));
            double bodyP90 = 0.0;

            if (body != nullptr)
            {
                std::vector<double> bp;

                for (int t = 1; t + 1 < T; ++t)
                    if ((*body)[(size_t) t] >= (*body)[(size_t) t - 1] && (*body)[(size_t) t] > (*body)[(size_t) t + 1])
                        bp.push_back ((*body)[(size_t) t]);

                bodyP90 = bp.empty() ? 0.0 : quantile (bp, 0.9);
            }

            auto bodyAt = [&] (int f)
            {
                double v = 0.0;

                for (int d = -2; d <= 2; ++d)
                    if (f + d >= 0 && f + d < T)
                        v = juce::jmax (v, (*body)[(size_t) (f + d)]);

                return v;
            };

            const int near = (int) (0.012 * fps), gap = (int) (0.035 * fps), sep = (int) (0.015 * fps), w = (int) (0.06 * fps);
            std::vector<std::pair<double, double>> out;
            int last = -1000000;
            bool armed = true;

            for (int t = 1; t + 1 < T; ++t)
            {
                double m = 0.0;
                const int a = juce::jmax (0, t - w), b = juce::jmin (T, t + w + 1);

                for (int i = a; i < b; ++i)
                    m += odf[(size_t) i];

                m /= (double) (b - a);

                if (odf[(size_t) t] < m + 0.03 * p90)
                    armed = true;

                // Kick oder Snare kurz nach einer Hat ist ein eigener Einsatz,
                // auch ohne Abstand und Hysterese: im Koerper-Band (body,
                // 100 Hz - 1 kHz) steigt es deutlich staerker als beim
                // vorigen Einsatz (Praxistest 1k/1n: eine Hat 30 ms vor der
                // Kick, 25 ms vor der Snare verschluckte sie — "Kick fehlt
                // auf 1+"; im Fluss ab 150 Hz ist die Hat davor lauter)
                const bool follow = body != nullptr && t - last >= sep && t - last < gap
                                    && bodyAt (t) >= bodyP90 * 0.2 && bodyAt (t) >= 2.0 * bodyAt (last);

                if ((! armed && ! follow) || odf[(size_t) t] < m + 0.06 * p90)
                    continue;

                bool isMax = true;

                for (int d = -near; d <= near && isMax; ++d)
                    if (d != 0 && t + d >= 0 && t + d < T && odf[(size_t) (t + d)] > odf[(size_t) t])
                        isMax = false;

                if (! isMax || (t - last < gap && ! follow))
                    continue;

                const double y0 = odf[(size_t) t - 1], y1 = odf[(size_t) t], y2 = odf[(size_t) t + 1], den = y0 - 2.0 * y1 + y2;
                const double frac = den != 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den) : 0.0;
                out.push_back ({ t + frac, y1 / p90 });
                last = t;
                armed = false;
            }

            return out;
        }

        // Vorlage als Summe von Glocken ueber log2(Frequenz)
        std::array<double, NB> bell (std::initializer_list<std::array<double, 3>> peaksIn)
        {
            std::array<double, NB> w {};

            for (int b = 0; b < NB; ++b)
            {
                double s = 0.02;

                for (auto& p : peaksIn)
                    s += p[2] * std::exp (-0.5 * std::pow (std::log2 (centre (b) / p[0]) / p[1], 2.0));

                w[(size_t) b] = s;
            }

            return w;
        }

        struct Event
        {
            double time = 0.0, strength = 0.0;
            int frame = 0;
            std::array<double, NB> vec {};
            std::array<double, 4> lvl {}, share {};
            int bar = 0, slot = 0;
            double offMs = 0.0;
        };

        // Euklidische NMF V ~ A W mit Traeger-Masken; W wird ab adaptFrom
        // angepasst und je Komponente normiert
        void nmf (const std::vector<std::array<double, NB>>& V, std::array<std::array<double, NB>, 4>& W,
                  const std::array<std::array<double, NB>, 4>& masks, std::vector<std::array<double, 4>>& A, int iters, int adaptFrom)
        {
            const size_t n = V.size();
            constexpr int K = 4;
            const double eps = 1.0e-12;
            A.assign (n, { 0.1, 0.1, 0.1, 0.1 });

            for (int it = 0; it < iters; ++it)
            {
                double WWt[K][K];

                for (int a = 0; a < K; ++a)
                    for (int c = 0; c < K; ++c)
                    {
                        double s = 0.0;

                        for (int b = 0; b < NB; ++b)
                            s += W[(size_t) a][(size_t) b] * W[(size_t) c][(size_t) b];

                        WWt[a][c] = s;
                    }

                for (size_t r = 0; r < n; ++r)
                {
                    double VW[K], AWW[K];

                    for (int k = 0; k < K; ++k)
                    {
                        double s = 0.0, s2 = 0.0;

                        for (int b = 0; b < NB; ++b)
                            s += V[r][(size_t) b] * W[(size_t) k][(size_t) b];

                        for (int c = 0; c < K; ++c)
                            s2 += A[r][(size_t) c] * WWt[c][k];

                        VW[k] = s;
                        AWW[k] = s2;
                    }

                    for (int k = 0; k < K; ++k)
                        A[r][(size_t) k] *= VW[k] / (AWW[k] + eps);
                }

                if (it < adaptFrom)
                    continue;

                double AtA[K][K];

                for (int a = 0; a < K; ++a)
                    for (int c = 0; c < K; ++c)
                    {
                        double s = 0.0;

                        for (size_t r = 0; r < n; ++r)
                            s += A[r][(size_t) a] * A[r][(size_t) c];

                        AtA[a][c] = s;
                    }

                for (int k = 0; k < K; ++k)
                {
                    std::array<double, NB> AtV {};

                    for (size_t r = 0; r < n; ++r)
                        for (int b = 0; b < NB; ++b)
                            AtV[(size_t) b] += A[r][(size_t) k] * V[r][(size_t) b];

                    double norm = 0.0;

                    for (int b = 0; b < NB; ++b)
                    {
                        double den = 0.0;

                        for (int c = 0; c < K; ++c)
                            den += AtA[k][c] * W[(size_t) c][(size_t) b];

                        W[(size_t) k][(size_t) b] *= AtV[(size_t) b] / (den + eps) * masks[(size_t) k][(size_t) b];
                        norm += W[(size_t) k][(size_t) b] * W[(size_t) k][(size_t) b];
                    }

                    norm = std::sqrt (norm);

                    if (norm <= 0.0)
                        norm = 1.0;

                    for (int b = 0; b < NB; ++b)
                        W[(size_t) k][(size_t) b] /= norm;

                    for (size_t r = 0; r < n; ++r)
                        A[r][(size_t) k] *= norm;
                }
            }
        }

        // Mittleres Leistungsspektrum (4096) an den Zeiten + off, Laenge len
        std::vector<double> avgSpectrum (const std::vector<float>& x, double rate, const std::vector<double>& times, double off, double len)
        {
            constexpr int order = 12, nfft = 1 << order;
            const int n = juce::jmin (nfft, (int) (len * rate));
            juce::dsp::FFT fft (order);
            std::vector<float> buf (2 * nfft);
            std::vector<double> acc ((size_t) nfft / 2 + 1, 0.0);
            int cnt = 0;

            for (auto t : times)
            {
                const auto s0 = (long) ((t + off) * rate);

                if (s0 < 0 || s0 + n > (long) x.size())
                    continue;

                std::fill (buf.begin(), buf.end(), 0.0f);

                for (int i = 0; i < n; ++i)
                    buf[(size_t) i] = x[(size_t) (s0 + i)] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1)));

                fft.performFrequencyOnlyForwardTransform (buf.data());

                for (int b = 0; b <= nfft / 2; ++b)
                    acc[(size_t) b] += (double) buf[(size_t) b] * buf[(size_t) b];

                ++cnt;
            }

            for (auto& a : acc)
                a /= juce::jmax (1, cnt);

            return acc;
        }

        double peakHz (const std::vector<double>& sp, double binHz, double lo, double hi)
        {
            const int b0 = juce::jmax (1, (int) (lo / binHz)), b1 = juce::jmin ((int) sp.size() - 2, (int) (hi / binHz));
            int b = b0;

            for (int i = b0; i <= b1; ++i)
                if (sp[(size_t) i] > sp[(size_t) b])
                    b = i;

            if (sp[(size_t) b] <= 0.0)
                return nan;

            const double y0 = 10 * std::log10 (sp[(size_t) b - 1] + 1e-20), y1 = 10 * std::log10 (sp[(size_t) b] + 1e-20), y2 = 10 * std::log10 (sp[(size_t) b + 1] + 1e-20);
            const double den = y0 - 2 * y1 + y2;
            return (b + (den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0)) * binHz;
        }

        // T20 aus einer Geraden durch den dB-Verlauf nach dem Gipfel
        // Steigung des Pegels ueber fitS nach dem Gipfel, auf 20 dB
        // hochgerechnet — nur als relatives Mass (Hats offen/zu): die ersten
        // 50 ms einer Hat liegen ueber dem Grundpegel
        double slopeMs (const std::vector<double>& env, double fps, double tOn, double fitS, double stop)
        {
            const int f0 = (int) (tOn * fps);

            if (f0 < 0 || f0 + 5 >= (int) env.size())
                return nan;

            int pk = f0;

            for (int i = f0; i < juce::jmin ((int) env.size(), f0 + (int) (0.04 * fps)); ++i)
                if (env[(size_t) i] > env[(size_t) pk])
                    pk = i;

            int end = pk + (int) (fitS * fps);

            if (stop > 0.0)
                end = juce::jmin (end, (int) (stop * fps) - 2);

            end = juce::jmin (end, (int) env.size() - 1);

            if (end - pk < 4)
                return nan;

            double mx = 0, my = 0;
            const int n = end - pk + 1;

            for (int i = pk; i <= end; ++i)
                mx += i, my += env[(size_t) i];

            mx /= n, my /= n;
            double num = 0, den = 0;

            for (int i = pk; i <= end; ++i)
                num += (i - mx) * (env[(size_t) i] - my), den += (i - mx) * (i - mx);

            const double sl = num / den;
            return sl >= -1.0e-6 ? nan : juce::jmin (3000.0, 20.0 / (-sl * fps) * 1000.0);
        }

        // Ausklang (T20): Zeit vom Gipfel (bis 40 ms nach dem Einsatz), bis
        // der Pegel D dB darunter faellt, auf 20 dB hochgerechnet. D = 20,
        // hoechstens bis 3 dB ueber dem Grundpegel vor dem Schlag (10 dB
        // mindestens, sonst verdeckt: nan). Gemessen bis zum naechsten
        // Einsatz (stop) bzw. maxS. Vorher eine Gerade ueber 80-120 ms und
        // hochgerechnet (Praxistest 1e): ueber dem Grundpegel der Hats wurde
        // sie flacher, je kuerzer das Sample — der Ausklang wurde "laenger"
        // gemessen, Kicks kamen auf 1,3-2,2 s.
        double decayMs (const std::vector<double>& env, double fps, double tOn, double maxS, double stop)
        {
            const int f0 = (int) (tOn * fps);

            if (f0 < 2 || f0 + 5 >= (int) env.size())
                return nan;

            int pk = f0;

            for (int i = f0; i < juce::jmin ((int) env.size(), f0 + (int) (0.04 * fps)); ++i)
                if (env[(size_t) i] > env[(size_t) pk])
                    pk = i;

            double floor = 1.0e9;

            for (int i = juce::jmax (0, f0 - (int) (0.05 * fps)); i <= juce::jmax (0, f0 - (int) (0.005 * fps)); ++i)
                floor = juce::jmin (floor, env[(size_t) i]);

            const double D = juce::jmin (20.0, env[(size_t) pk] - floor - 3.0);

            if (D < 10.0)
                return nan;

            int end = pk + (int) (maxS * fps);

            if (stop > 0.0)
                end = juce::jmin (end, (int) (stop * fps) - 1);

            end = juce::jmin (end, (int) env.size() - 1);

            for (int i = pk + 1; i <= end; ++i)
                if (env[(size_t) i] <= env[(size_t) pk] - D)
                {
                    // zwischen zwei Rahmen linear
                    const double a = env[(size_t) i - 1], b = env[(size_t) i], target = env[(size_t) pk] - D;
                    const double frac = a > b ? (a - target) / (a - b) : 1.0;
                    const double t = (i - 1 - pk + frac) / fps;
                    return std::round (juce::jmin (3000.0, t * 20.0 / D * 1000.0));
                }

            return nan;
        }
    }

    std::vector<NoteHit> noteHits (const Result& r, double minLevelDb)
    {
        std::vector<NoteHit> out;

        for (auto& in : r.instruments)
        {
            const int note = in.name == "kick" ? 36 : in.name == "snare" ? 38 : in.name == "hatOpen" ? 46 : 42;

            for (auto& h : in.hits)
                if (h.levelDb >= minLevelDb)
                    out.push_back ({ h.time, note, juce::jlimit (0.05, 1.0, std::pow (10.0, h.levelDb / 20.0)) });
        }

        std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.time < b.time; });
        return out;
    }

    juce::String slotName (int slot)
    {
        static const char* sub[] = { "", "e", "+", "a" };
        return juce::String (slot / 4 + 1) + sub[slot % 4];
    }

    std::vector<Onset> onsets (const std::vector<float>& xIn, double rate)
    {
        std::vector<Onset> out;

        if (xIn.size() < (size_t) N * 4 || rate <= 0.0)
            return out;

        const double pad = 0.05;
        std::vector<float> x ((size_t) (pad * rate), 0.0f);
        x.insert (x.end(), xIn.begin(), xIn.end());
        const double fps = rate / hop;

        for (auto& [tf, strength] : peaks (superflux (powerFrames (x), rate), fps))
            out.push_back ({ tf / fps + 0.004 - pad, strength });

        return out;
    }

    Result describe (const std::vector<float>& xIn, double rate, double bpm, double firstDownbeat, int beatsPerBar)
    {
        Result res;

        if (xIn.size() < (size_t) N * 4 || rate <= 0.0 || bpm <= 0.0)
            return res;

        // 50 ms Stille davor: die ersten Frames haben kein "davor" — ein
        // Schlag bei 0 s fiele sonst heraus. Alle Zeiten danach - pad.
        const double pad = 0.05;
        std::vector<float> x ((size_t) (pad * rate), 0.0f);
        x.insert (x.end(), xIn.begin(), xIn.end());
        firstDownbeat += pad;

        const double fps = rate / hop;
        const auto spec = powerFrames (x);
        const int T = (int) spec.size();
        const double binHz = rate / N;

        // Bandleistung je Frame
        std::vector<std::array<double, NB>> P ((size_t) T);

        for (int t = 0; t < T; ++t)
            for (int b = 0; b < NB; ++b)
            {
                double s = 1.0e-15;

                for (int k = juce::jmax (1, (int) std::ceil (edges[b] / binHz)); k * binHz < edges[b + 1] && k <= N / 2; ++k)
                    s += spec[(size_t) t][(size_t) k];

                P[(size_t) t][(size_t) b] = s;
            }

        // Einsaetze und ihre hinzugekommene Leistung je Band
        std::vector<Event> ev;

        const auto body = superflux (spec, rate, 100.0, 1000.0);
        const auto pks = peaks (superflux (spec, rate), fps, &body);

        for (size_t pi = 0; pi < pks.size(); ++pi)
        {
            const auto [tf, strength] = pks[pi];
            const int t = (int) std::round (tf);
            // bis vor den naechsten Einsatz: eine Hat kurz vor der Kick nimmt
            // deren Bass nicht mit
            const int next = pi + 1 < pks.size() ? (int) std::round (pks[pi + 1].first) - 1 : T - 1;

            if (t < 12 || t > T - 8)
                continue;

            Event e;
            e.frame = t;
            e.strength = strength;
            e.time = tf / fps + 0.004;   // Kalibrierung Onset-Kurve -> Anschlag

            for (int b = 0; b < NB; ++b)
            {
                const int span = (int) (juce::jmin (0.040, juce::jmax (0.017, 2.0 / centre (b))) * fps);
                double pre = 1.0e30, post = 0.0;

                for (int i = t - 10; i <= t - 3; ++i)
                    pre = juce::jmin (pre, P[(size_t) i][(size_t) b]);

                for (int i = t; i <= juce::jmin (T - 1, t + span, juce::jmax (t, next)); ++i)
                    post = juce::jmax (post, P[(size_t) i][(size_t) b]);

                e.vec[(size_t) b] = std::sqrt (juce::jmax (0.0, post - pre));
            }

            ev.push_back (e);
        }

        res.onsets = (int) ev.size();

        if (ev.size() < 4)
        {
            res.hints.add ("too few onsets for a drum pattern");
            return res;
        }

        // NMF: kick, snare, hat, low (Bass-Uebersprechen)
        std::array<std::array<double, NB>, 4> W { bell ({ { 60, 0.6, 1.0 }, { 180, 0.6, 0.6 }, { 3000, 1.2, 0.25 } }),
                                                  bell ({ { 230, 0.5, 0.7 }, { 3000, 1.5, 0.7 } }),
                                                  bell ({ { 10000, 0.8, 1.0 } }),
                                                  bell ({ { 70, 0.5, 1.0 } }) };
        const std::array<std::function<bool (double)>, 4> carrier { [] (double c) { return c < 6000; }, [] (double c) { return c > 140; },
                                                                    [] (double c) { return c > 3000; }, [] (double c) { return c < 200; } };
        std::array<std::array<double, NB>, 4> masks {};

        for (int k = 0; k < 4; ++k)
            for (int b = 0; b < NB; ++b)
            {
                masks[(size_t) k][(size_t) b] = carrier[(size_t) k] (centre (b)) ? 1.0 : 0.0;
                W[(size_t) k][(size_t) b] *= masks[(size_t) k][(size_t) b];
            }

        std::array<double, NB> scale {};

        for (int b = 0; b < NB; ++b)
        {
            std::vector<double> v;

            for (auto& e : ev)
                v.push_back (e.vec[(size_t) b]);

            scale[(size_t) b] = juce::jmax (1.0e-9, quantile (v, 0.9));
        }

        std::vector<std::array<double, NB>> V;

        for (auto& e : ev)
        {
            std::array<double, NB> r {};

            for (int b = 0; b < NB; ++b)
                r[(size_t) b] = e.vec[(size_t) b] / scale[(size_t) b];

            V.push_back (r);
        }

        std::vector<std::array<double, 4>> A;
        nmf (V, W, masks, A, 500, 30);

        for (int k = 0; k < 4; ++k)
        {
            std::vector<double> acts;

            for (auto& a : A)
                acts.push_back (a[(size_t) k]);

            const double ref = juce::jmax (1.0e-12, quantile (acts, 0.95));
            double wmax = 0.0;

            for (int b = 0; b < NB; ++b)
                wmax = juce::jmax (wmax, W[(size_t) k][(size_t) b]);

            for (size_t r = 0; r < ev.size(); ++r)
            {
                double mine = 0.0, tot = 0.0;

                for (int b = 0; b < NB; ++b)
                {
                    if (W[(size_t) k][(size_t) b] < 0.25 * wmax)
                        continue;   // Kernbaender der Vorlage

                    double all = 0.0;

                    for (int c = 0; c < 4; ++c)
                        all += A[r][(size_t) c] * W[(size_t) c][(size_t) b];

                    mine += std::pow (A[r][(size_t) k] * W[(size_t) k][(size_t) b], 2.0);
                    tot += all * all;
                }

                ev[r].lvl[(size_t) k] = 20.0 * std::log10 (juce::jmax (A[r][(size_t) k], 1.0e-12) / ref);
                ev[r].share[(size_t) k] = tot > 0.0 ? mine / tot : 0.0;
            }
        }

        // Raster
        const int spb = juce::jlimit (4, 32, beatsPerBar * 4);
        res.slotsPerBar = spb;
        const double s16 = 60.0 / bpm / 4.0;

        for (auto& e : ev)
        {
            const double pos = (e.time - firstDownbeat) / s16;
            const auto k = (long) std::llround (pos);
            e.bar = (int) std::floor ((double) k / spb) + 1;
            e.slot = (int) (((k % spb) + spb) % spb);
            e.offMs = (pos - (double) k) * s16 * 1000.0;
        }

        // Treffer je Instrument (relativ zum Instrument selbst)
        std::array<std::vector<const Event*>, 3> hits;   // kick, snare, hat

        for (auto& e : ev)
            for (int k = 0; k < 3; ++k)
            {
                const double l = e.lvl[(size_t) k], s = e.share[(size_t) k];

                if ((l >= -6.0 && s >= 0.02) || (l >= -20.0 && s >= 0.7))
                    hits[(size_t) k].push_back (&e);
                // Kick, deren Grundton in "low" steckt: mit Anschlag (Bass hat keinen)
                else if (k == 0 && e.strength >= 2.0 && l >= -9.0 && e.lvl[3] >= -6.0 && e.share[3] >= 0.3)
                    hits[0].push_back (&e);
            }

        // Huellkurven je Bandbereich (dB)
        auto bandEnv = [&P, T] (double lo, double hi)
        {
            std::vector<double> env ((size_t) T);

            for (int t = 0; t < T; ++t)
            {
                double s = 0.0;

                for (int b = 0; b < NB; ++b)
                    if (centre (b) >= lo && centre (b) < hi)
                        s += P[(size_t) t][(size_t) b];

                env[(size_t) t] = 10.0 * std::log10 (s + 1.0e-20);
            }

            return env;
        };

        std::vector<double> allOn;

        for (auto& e : ev)
            allOn.push_back (e.time);

        auto nextAfter = [] (double t, const std::vector<double>& times)
        {
            for (auto u : times)
                if (u > t + 0.03)
                    return u;

            return -1.0;
        };

        // Ausklang von Kick/Snare bis zum naechsten Kick oder Snare (die
        // Hats dazwischen beenden ihn nicht — die Messung nimmt den ersten
        // Durchgang unter die Schwelle)
        std::vector<double> kickSnare;

        for (int k = 0; k < 2; ++k)
            for (auto* e : hits[(size_t) k])
                kickSnare.push_back (e->time);

        std::sort (kickSnare.begin(), kickSnare.end());

        // Hats offen/zu ueber die Laenge (T20 6-16 kHz), nicht wo zugleich Snare
        const auto hatEnv = bandEnv (6000, 16000);
        std::map<const Event*, double> hatLen;
        std::set<const Event*> snareSet (hits[1].begin(), hits[1].end()), openSet;

        // Die Steigung endet vor dem naechsten Einsatz (Praxistest 1i/1k:
        // eine vorgezogene Hat kurz vor der Snare las deren Rauschen als
        // offene Hat)
        for (auto* e : hits[2])
            if (snareSet.count (e) == 0)
            {
                double stop = -1.0;

                for (auto u : allOn)
                    if (u > e->time + 0.005)
                    {
                        stop = u;
                        break;
                    }

                if (auto d = slopeMs (hatEnv, fps, e->time, 0.05, stop); ! std::isnan (d))
                    hatLen[e] = d;
            }

        if (hatLen.size() >= 4)
        {
            std::vector<double> lg;

            for (auto& [e, d] : hatLen)
                lg.push_back (std::log (d));

            double a = *std::min_element (lg.begin(), lg.end()), b = *std::max_element (lg.begin(), lg.end());

            for (int it = 0; it < 30; ++it)
            {
                double sa = 0, sb = 0;
                int na = 0, nb = 0;

                for (auto v : lg)
                    (std::abs (v - a) <= std::abs (v - b) ? (sa += v, ++na) : (sb += v, ++nb));

                if (na == 0 || nb == 0)
                    break;

                a = sa / na, b = sb / nb;
            }

            if (b - a >= std::log (2.5))
                for (auto& [e, d] : hatLen)
                    if (std::abs (std::log (d) - b) < std::abs (std::log (d) - a))
                        openSet.insert (e);
        }

        struct Group { const char* name; int key; std::vector<const Event*> hs; };
        std::vector<Group> groups { { "kick", 0, hits[0] }, { "snare", 1, hits[1] }, { "hatClosed", 2, {} }, { "hatOpen", 2, {} } };

        for (auto* e : hits[2])
            (openSet.count (e) ? groups[3] : groups[2]).hs.push_back (e);

        std::set<int> barSet;

        for (auto& e : ev)
            if (e.bar >= 1)
                barSet.insert (e.bar);

        const int nbars = (int) barSet.size();

        for (auto& g : groups)
        {
            Instrument inst;
            inst.name = g.name;
            std::map<std::pair<int, int>, const Event*> by;

            for (auto* e : g.hs)
            {
                auto key = std::make_pair (e->bar, e->slot);

                if (by.count (key) == 0 || e->lvl[(size_t) g.key] > by[key]->lvl[(size_t) g.key])
                    by[key] = e;
            }

            if (by.empty())
            {
                res.instruments.push_back (inst);
                continue;
            }

            double top = -1.0e9;

            for (auto& [k, e] : by)
                top = juce::jmax (top, e->lvl[(size_t) g.key]);

            for (auto& [k, e] : by)
                inst.hits.push_back ({ e->time - pad, e->bar, e->slot, e->offMs, e->lvl[(size_t) g.key] - top });

            for (auto b : barSet)
            {
                juce::String pat;

                for (int i = 0; i < spb; ++i)
                {
                    auto it = by.find ({ b, i });

                    if (it == by.end())
                        pat << ".";
                    else
                    {
                        const double l = it->second->lvl[(size_t) g.key] - top;
                        pat << (l >= -4.0 ? "X" : l < -12.0 ? "g" : "x");
                    }
                }

                inst.bars.push_back ({ b, pat });
            }

            // je Slot: Anzahl, Pegel, Mikrotiming relativ zu den Vierteln
            std::vector<double> quarter, allOff;

            for (auto& [k, e] : by)
            {
                allOff.push_back (e->offMs);

                if (k.second % 4 == 0)
                    quarter.push_back (e->offMs);
            }

            const double ref = ! quarter.empty() ? medianOf (quarter) : medianOf (allOff);

            for (int i = 0; i < spb; ++i)
            {
                std::vector<double> offs, lvs;

                for (auto& [k, e] : by)
                    if (k.second == i && k.first >= 1)
                        offs.push_back (e->offMs - ref), lvs.push_back (e->lvl[(size_t) g.key] - top);

                if ((int) offs.size() < juce::jmax (2, nbars / 3))
                    continue;

                inst.slots.push_back ({ i, (int) offs.size(), std::round (medianOf (lvs) * 10.0) / 10.0, std::round (medianOf (offs) * 10.0) / 10.0 });
            }

            // Klang aus klaren Schlaegen (bis -6 dB unter dem lautesten)
            std::vector<double> times, strong;

            for (auto& [k, e] : by)
            {
                times.push_back (e->time);

                if (e->lvl[(size_t) g.key] - top >= -6.0)
                    strong.push_back (e->time);
            }

            std::sort (times.begin(), times.end());

            if (strong.empty())
                strong = times;

            if (juce::String (g.name) == "kick")
            {
                auto sp = avgSpectrum (x, rate, strong, 0.010, 0.093);
                inst.fundamentalHz = std::round (peakHz (sp, rate / 4096.0, 35, 200));
                const auto env = bandEnv (30, 200);
                std::vector<double> ds;

                for (auto t : times)
                    if (auto d = decayMs (env, fps, t, 0.6, nextAfter (t, times)); ! std::isnan (d))
                        ds.push_back (d);

                inst.decayMs = std::round (medianOf (ds));
            }
            else if (juce::String (g.name) == "snare")
            {
                auto sp = avgSpectrum (x, rate, strong, 0.0, 0.060);
                const double bh = rate / 4096.0;
                double tot = 0.0, hi = 0.0;

                for (size_t b = (size_t) (100 / bh); b < sp.size(); ++b)
                    tot += sp[b], hi += b * bh >= 1000 ? sp[b] : 0.0;

                inst.bodyHz = std::round (peakHz (sp, bh, 150, 500));
                inst.noiseShare = tot > 0 ? std::round (hi / tot * 100.0) / 100.0 : nan;
                const auto env = bandEnv (1000, 5000);
                std::vector<double> ds;

                for (auto t : strong)
                    if (auto d = decayMs (env, fps, t, 0.6, nextAfter (t, kickSnare)); ! std::isnan (d))
                        ds.push_back (d);

                inst.decayMs = std::round (medianOf (ds));
            }
            else
            {
                auto sp = avgSpectrum (x, rate, times, 0.0, 0.030);
                const double bh = rate / 4096.0;
                double sw = 0.0, s = 0.0;

                for (size_t b = (size_t) (2000 / bh); b < sp.size(); ++b)
                    sw += sp[b], s += sp[b] * b * bh;

                inst.brightnessHz = sw > 0 ? std::round (s / sw) : nan;
                std::vector<double> ls;

                for (auto& [k, e] : by)
                    if (hatLen.count (e))
                        ls.push_back (hatLen[e]);

                inst.decayMs = std::round (medianOf (ls));
            }

            res.instruments.push_back (inst);
        }

        // Wiederholung: 1 oder 2 Takte (Kick + Snare)
        {
            auto pat = [&res] (const juce::String& name, int bar)
            {
                for (auto& in : res.instruments)
                    if (in.name == name)
                        for (auto& [b, p] : in.bars)
                            if (b == bar)
                                return p.replaceCharacters ("Xxg", "###");

                return juce::String();
            };

            // Takte ohne Kick und Snare (Ausklang, Pause) zaehlen nicht
            auto playing = [&] (int bar) { return pat ("kick", bar).containsChar ('#') || pat ("snare", bar).containsChar ('#'); };

            auto sim = [&] (int lag)
            {
                double s = 0.0;
                int n = 0;

                for (auto b : barSet)
                    if (barSet.count (b + lag) && playing (b) && playing (b + lag))
                        for (auto* name : { "kick", "snare" })
                        {
                            auto p = pat (name, b), q = pat (name, b + lag);

                            if (p.isEmpty() || q.isEmpty())
                                continue;

                            int same = 0;

                            for (int i = 0; i < p.length(); ++i)
                                same += p[i] == q[i] ? 1 : 0;

                            s += (double) same / p.length();
                            ++n;
                        }

                return n > 0 ? s / n : 0.0;
            };

            const double s1 = sim (1), s2 = sim (2);

            if (nbars >= 4 && s2 > s1 + 0.04 && s2 >= 0.9)
            {
                res.patternRepeat = 2;
                res.hints.add (juce::String::fromUTF8 ("the pattern repeats every 2 bars — odd and even bars differ (build it as a 2-bar pattern)"));
            }
        }

        res.valid = true;
        return res;
    }
}
