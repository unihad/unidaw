#include "MusicAnalysis.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <map>

namespace dawd::analysis
{
    namespace
    {
        constexpr double targetRate = 22050.0;

        // Onset-Analyse: 1024er-Fenster, 256er-Hop (~86 Frames/s bei 22 kHz)
        constexpr int onsetOrder = 10, onsetSize = 1 << onsetOrder, onsetHop = 256;
        // Chroma: 4096er-Fenster (5,4 Hz Aufloesung bei 22 kHz), 1024er-Hop
        constexpr int chromaOrder = 12, chromaSize = 1 << chromaOrder, chromaHop = 1024;

        // Gleitender Mittelwert ueber +-half Frames abziehen, negatives auf 0,
        // dann auf Standardabweichung 1 — die Onset-Huellkurve ohne Pegeltrend
        void detrendAndNormalise (std::vector<float>& v, int half)
        {
            const int n = (int) v.size();

            if (n == 0)
                return;

            std::vector<double> prefix ((size_t) n + 1, 0.0);

            for (int i = 0; i < n; ++i)
                prefix[(size_t) i + 1] = prefix[(size_t) i] + v[(size_t) i];

            std::vector<float> out ((size_t) n);

            for (int i = 0; i < n; ++i)
            {
                auto lo = std::max (0, i - half), hi = std::min (n, i + half + 1);
                auto mean = (prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo);
                out[(size_t) i] = (float) std::max (0.0, (double) v[(size_t) i] - mean);
            }

            double sq = 0.0;

            for (auto x : out)
                sq += (double) x * x;

            auto sd = std::sqrt (sq / n);

            if (sd > 1e-9)
                for (auto& x : out)
                    x = (float) (x / sd);

            v = std::move (out);
        }

        // Frame i ist um Sample i * hop zentriert (vorne mit size/2 Nullen
        // aufgefuellt) — Frame-Zeit = i * hop / rate
        template <typename Fn>
        void forEachFrame (const MonoAudio& a, int order, int hop, Fn&& fn)
        {
            const int size = 1 << order;
            const auto n = (juce::int64) a.samples.size();
            juce::dsp::FFT fft (order);
            juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
            std::vector<float> buf (2 * (size_t) size);

            for (juce::int64 centre = 0, i = 0; centre < n; centre += hop, ++i)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);

                for (int k = 0; k < size; ++k)
                {
                    auto s = centre - size / 2 + k;

                    if (s >= 0 && s < n)
                        buf[(size_t) k] = a.samples[(size_t) s];
                }

                window.multiplyWithWindowingTable (buf.data(), (size_t) size);
                fft.performFrequencyOnlyForwardTransform (buf.data());
                fn ((int) i, buf.data());
            }
        }

        double median (std::vector<double> v)
        {
            if (v.empty())
                return 0.0;

            std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
            return v[v.size() / 2];
        }
    }

    MonoAudio loadMono (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length)
    {
        MonoAudio a;
        length = juce::jmin (length, reader.lengthInSamples - start);

        if (length <= 0 || reader.sampleRate <= 0.0)
            return a;

        const int factor = juce::jmax (1, (int) std::round (reader.sampleRate / targetRate));
        const int chans = (int) juce::jmax (1u, reader.numChannels);
        a.sampleRate = reader.sampleRate / factor;
        a.samples.reserve ((size_t) (length / factor + 1));

        constexpr int block = 1 << 16;
        juce::AudioBuffer<float> buf (chans, block);
        double acc = 0.0;
        int accN = 0;

        for (juce::int64 pos = 0; pos < length; pos += block)
        {
            auto n = (int) juce::jmin ((juce::int64) block, length - pos);
            buf.clear();
            reader.read (&buf, 0, n, start + pos, true, true);

            for (int i = 0; i < n; ++i)
            {
                float s = 0.0f;

                for (int c = 0; c < chans; ++c)
                    s += buf.getSample (c, i);

                // Mittel ueber factor Samples: grober Tiefpass vor dem Teilen
                acc += s / chans;

                if (++accN == factor)
                {
                    a.samples.push_back ((float) (acc / factor));
                    acc = 0.0;
                    accN = 0;
                }
            }
        }

        return a;
    }

    Features computeFeatures (const MonoAudio& a)
    {
        Features f;

        if (a.samples.empty() || a.sampleRate <= 0.0)
            return f;

        f.duration = (double) a.samples.size() / a.sampleRate;
        f.fps = a.sampleRate / onsetHop;
        f.chromaFps = a.sampleRate / chromaHop;

        // Onsets: Spectral Flux ueber eine Filterbank in Terzbaendern (40 Hz
        // bis 8 kHz) mit log1p-Kompression, nur Anstiege; gesamt und Bass bis
        // 150 Hz (Kick, Bass — fuer die Takt-Eins). Die Baender geben tiefen
        // Toenen so viel Gewicht wie hohen; lineare Bins oder dB-Werte
        // liessen leise, breitbandige Hi-Hats die Kick ueberstimmen
        {
            const int bins = onsetSize / 2 + 1;
            const double binHz = a.sampleRate / onsetSize;
            const double norm = onsetSize / 4.0;   // Hann: Sinus-Amplitude 1 -> size/4
            struct Band { int lo, hi; bool low, mid; };
            std::vector<Band> bands;

            for (double fc = 40.0; fc <= juce::jmin (8000.0, a.sampleRate * 0.45); fc *= std::pow (2.0, 1.0 / 3.0))
            {
                auto lo = juce::jmax (bands.empty() ? 1 : bands.back().hi, (int) std::floor (fc / std::pow (2.0, 1.0 / 6.0) / binHz));
                auto hi = juce::jmax (lo + 1, (int) std::ceil (fc * std::pow (2.0, 1.0 / 6.0) / binHz));

                if (hi > bins)
                    break;

                if (! bands.empty() && lo < bands.back().hi)
                    continue;

                bands.push_back ({ lo, hi, fc <= 150.0, fc >= 190.0 && fc <= 1050.0 });
            }

            const int numLow = (int) std::count_if (bands.begin(), bands.end(), [] (auto& b) { return b.low; });
            const int numMid = (int) std::count_if (bands.begin(), bands.end(), [] (auto& b) { return b.mid; });
            std::vector<float> prev (bands.size(), 0.0f), cur (bands.size());

            forEachFrame (a, onsetOrder, onsetHop, [&] (int i, const float* mag)
            {
                double flux = 0.0, low = 0.0, mid = 0.0;

                for (size_t k = 0; k < bands.size(); ++k)
                {
                    double sum = 0.0;

                    for (int b = bands[k].lo; b < bands[k].hi; ++b)
                        sum += mag[b] / norm;

                    cur[k] = (float) std::log1p (1000.0 * sum);
                    auto d = juce::jmax (0.0f, cur[k] - prev[k]);
                    flux += d;

                    if (bands[k].low)
                        low += d;

                    if (bands[k].mid)
                        mid += d;
                }

                // Frame 0 vergleicht mit Stille: ein Einsatz am Dateianfang
                // zaehlt (bei einem Schnitt mitten im Klang ein Schein-Onset,
                // den das Raster am Rand wieder verwirft)
                juce::ignoreUnused (i);

                // Leistung in drei breiten Baendern (bis 250 Hz, bis 4 kHz,
                // darueber) — Klangfarbe je Takt fuer den Aufbau
                std::array<float, 3> be {};

                for (int b = 1; b < bins; ++b)
                {
                    auto hz = b * binHz;
                    auto pw = (mag[b] / norm) * (mag[b] / norm);
                    be[hz < 250.0 ? 0 : hz < 4000.0 ? 1 : 2] += (float) pw;
                }

                f.bandEnergy.push_back (be);
                f.onset.push_back ((float) (flux / (double) bands.size()));
                f.lowOnset.push_back ((float) (low / juce::jmax (1, numLow)));
                f.midOnset.push_back ((float) (mid / juce::jmax (1, numMid)));
                std::swap (prev, cur);
            });

            const int half = (int) std::round (f.fps * 0.5);
            detrendAndNormalise (f.onset, half);
            detrendAndNormalise (f.lowOnset, half);
        }

        // Chroma: Magnituden von 80 Hz bis 5 kHz auf die naechste Tonklasse,
        // gewichtet nach Abstand zur Halbton-Mitte; je Frame auf Max 1
        {
            const double binHz = a.sampleRate / chromaSize;
            struct BinMap { int bin, pc; float weight; };
            std::vector<BinMap> map;

            // Tonklasse und Gewicht (Abstand zur Halbton-Mitte) einer Frequenz
            auto pitchClassOf = [] (double hz)
            {
                auto midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
                auto nearest = std::round (midi);
                auto dev = midi - nearest;
                return std::pair<int, float> { ((int) nearest % 12 + 12) % 12, (float) std::exp (-0.5 * (dev / 0.25) * (dev / 0.25)) };
            };

            for (int b = (int) std::ceil (80.0 / binHz); b * binHz <= juce::jmin (5000.0, a.sampleRate * 0.45); ++b)
            {
                auto [pc, weight] = pitchClassOf (b * binHz);
                map.push_back ({ b, pc, weight });
            }

            struct Peak { double hz; float mag; };
            std::vector<Peak> peaks, fundamentals;

            forEachFrame (a, chromaOrder, chromaHop, [&] (int, const float* mag)
            {
                std::array<float, 12> c {};
                float top = 0.0f;

                for (auto& m : map)
                {
                    c[(size_t) m.pc] += mag[m.bin] * m.weight;
                    top = juce::jmax (top, mag[m.bin]);
                }

                // Tonart-Chroma: Spitzen ueber 2 % der lautesten; eine Spitze
                // auf dem h-fachen (h 2..10, +-30 Cent) einer tieferen
                // Grundton-Spitze zaehlt fuer deren Tonklasse. Sonst zieht
                // ein obertonreicher Ton (gezupft, Saege) die Tonart eine
                // Quinte nach oben (3. Teilton)
                std::array<float, 12> k {};
                peaks.clear();
                fundamentals.clear();

                for (auto& m : map)
                {
                    auto y0 = mag[m.bin - 1], y1 = mag[m.bin], y2 = mag[m.bin + 1];

                    if (y1 > y0 && y1 >= y2 && y1 > 0.02f * top && top > 1e-9f)
                    {
                        auto den = (double) y0 - 2.0 * y1 + y2;
                        peaks.push_back ({ (m.bin + (den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0)) * binHz, y1 });
                    }
                }

                for (auto& p : peaks)
                {
                    auto home = p.hz;

                    for (auto& f0 : fundamentals)
                    {
                        auto ratio = p.hz / f0.hz;
                        auto h = std::round (ratio);

                        if (h >= 2.0 && h <= 10.0 && std::abs (1200.0 * std::log2 (ratio / h)) < 30.0)
                        {
                            home = f0.hz;
                            break;
                        }
                    }

                    if (home == p.hz)
                        fundamentals.push_back (p);

                    auto [pc, weight] = pitchClassOf (home);
                    k[(size_t) pc] += p.mag * weight;
                }

                f.keyChroma.push_back (k);

                auto mx = *std::max_element (c.begin(), c.end());
                f.chromaEnergy.push_back (std::accumulate (c.begin(), c.end(), 0.0f));

                if (mx > 1e-6f)
                    for (auto& x : c)
                        x /= mx;

                f.chroma.push_back (c);
            });
        }

        return f;
    }

    TempoResult detectTempo (const Features& f)
    {
        TempoResult r;
        const auto& onset = f.onset;
        const int n = (int) onset.size();

        if (f.fps <= 0.0 || f.duration < 4.0 || n < 8)
            return r;

        // Ohne Puls kein Tempo: es braucht deutliche Onsets (Gipfel ueber
        // einer Standardabweichung), mindestens 4 und etwa einen je 5 s —
        // sonst fände das Tracking in einem stehenden Ton Schein-Beats
        int clear = 0;

        for (int i = 1; i + 1 < n; ++i)
            if (onset[(size_t) i] > 1.0f && onset[(size_t) i] >= onset[(size_t) i - 1] && onset[(size_t) i] > onset[(size_t) i + 1])
                ++clear;

        if (clear < juce::jmax (4, (int) (f.duration * 0.2)))
            return r;

        // 1. Tempo: Autokorrelation der Onset-Huellkurve, 40..240 BPM,
        //    gewichtet um 120 BPM (log-normal, eine Oktave Streuung) — so
        //    gewinnt bei gleich guten Kandidaten das uebliche Tempo
        const int minLag = juce::jmax (1, (int) std::floor (f.fps * 60.0 / 240.0));
        const int maxLag = juce::jmin (n / 2, (int) std::ceil (f.fps * 60.0 / 40.0));

        if (maxLag <= minLag + 2)
            return r;

        std::vector<double> weighted ((size_t) maxLag + 2, 0.0);

        for (int lag = minLag; lag <= maxLag + 1 && lag < n; ++lag)
        {
            double s = 0.0;

            for (int i = 0; i + lag < n; ++i)
                s += (double) onset[(size_t) i] * onset[(size_t) (i + lag)];

            s /= (n - lag);
            auto bpm = 60.0 * f.fps / lag;
            auto oct = std::log2 (bpm / 120.0);
            weighted[(size_t) lag] = s * std::exp (-0.5 * oct * oct);
        }

        struct Peak { double lag, score; };
        std::vector<Peak> peaks;

        for (int lag = minLag + 1; lag < maxLag; ++lag)
        {
            auto a = weighted[(size_t) lag - 1], b = weighted[(size_t) lag], c = weighted[(size_t) lag + 1];

            if (b > 0.0 && b >= a && b >= c)
            {
                // Parabel durch drei Punkte: Lag mit Nachkommastellen
                auto denom = a - 2.0 * b + c;
                auto off = std::abs (denom) > 1e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
                peaks.push_back ({ lag + off, b });
            }
        }

        if (peaks.empty())
            return r;

        std::sort (peaks.begin(), peaks.end(), [] (auto& x, auto& y) { return x.score > y.score; });

        for (auto& p : peaks)
        {
            auto bpm = 60.0 * f.fps / p.lag;
            bool distinct = true;

            for (auto& c : r.candidates)
                distinct = distinct && std::abs (c.bpm - bpm) / bpm > 0.04;

            if (distinct)
                r.candidates.push_back ({ bpm, p.score / peaks.front().score });

            if (r.candidates.size() == 3)
                break;
        }

        // 1b. Takt-Probe: das Muster wiederholt sich je Takt (1,2-4,6 s). Ist
        //     dieser Abstand kein ganzes Vielfaches des Schlags, war der
        //     Kandidat ein Rhythmus (punktierte Achtel synkopierter Akkorde
        //     ohne Drums: 86,5 statt 130 BPM), nicht der Puls — dann gilt ein
        //     Takt = 4 (oder 3) Schlaege (Praxistest 1h: Intro ohne Drums)
        double period = peaks.front().lag;
        {
            auto raw = [&] (int lag)
            {
                double s = 0.0;

                for (int i = 0; i + lag < n; ++i)
                    s += (double) onset[(size_t) i] * onset[(size_t) (i + lag)];

                return s / (n - lag);
            };

            const int lo = (int) std::ceil (1.2 * f.fps), hi = juce::jmin (n / 2, (int) std::floor (4.6 * f.fps));
            int bar = -1;
            double barAc = 0.0;

            for (int lag = lo; lag <= hi; ++lag)
                if (auto s = raw (lag); s > barAc)
                    barAc = s, bar = lag;

            if (bar > lo && bar < hi)
            {
                const double a = raw (bar - 1), c = raw (bar + 1), den = a - 2.0 * barAc + c;
                const double barLag = bar + (std::abs (den) > 1e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den) : 0.0);
                const double ratio = barLag / period;

                if (std::abs (ratio - std::round (ratio)) > 0.1 && barAc >= raw ((int) std::round (period)))
                {
                    double best = 0.0;

                    // Auch zwei Takte (8 bzw. 6 Schlaege): in 15-s-Fenstern eines
                    // Referenzsongs lag die Wiederholung bei 3,68 s = 2 Takten,
                    // 4 und 3 Schlaege gaben 60/45 BPM, das Fenster blieb bei
                    // 87 statt 130 BPM. Nur ein rhythmisches Verhaeltnis zum
                    // alten Kandidaten (punktiert: 3:2, 4:3 und umgekehrt) zaehlt —
                    // ein Fenster mit 130,4 BPM sprang sonst auf 148 BPM
                    const double oldLag = period;

                    for (int k : { 4, 3, 8, 6 })
                    {
                        const double lag = barLag / k;
                        const int li = (int) std::round (lag);

                        const double bpm = 60.0 * f.fps / lag;   // ueblicher Bereich (ein frei gespielter Teil traf sonst 58 BPM)
                        const double q = oldLag / lag;
                        bool rhythmic = false;

                        for (double rq : { 1.5, 4.0 / 3.0, 2.0 / 3.0, 0.75 })
                            rhythmic = rhythmic || std::abs (q / rq - 1.0) < 0.04;

                        if (rhythmic && bpm >= 80.0 && bpm <= 180.0 && li > minLag && li < maxLag && weighted[(size_t) li] > 1.25 * best)
                            best = weighted[(size_t) li], period = lag;
                    }

                    if (best > 0.0)
                    {
                        r.fromBars = true;
                        r.candidates.insert (r.candidates.begin(), { 60.0 * f.fps / period, best / peaks.front().score });
                    }
                    else
                        period = peaks.front().lag;
                }
            }
        }

        // 2. Beats: dynamische Programmierung (Ellis 2007) — jeder Beat
        //    liegt auf einem Onset und etwa eine Periode hinter dem vorigen
        std::vector<double> local;
        std::vector<int> frames;

        auto track = [&] (double period)
        {
            local.assign ((size_t) n, 0.0);
            const int w = (int) std::round (period);

            for (int i = 0; i < n; ++i)
            {
                double s = 0.0;

                for (int k = -w; k <= w; ++k)
                    if (i + k >= 0 && i + k < n)
                    {
                        auto z = k * 32.0 / period;
                        s += onset[(size_t) (i + k)] * std::exp (-0.5 * z * z);
                    }

                local[(size_t) i] = s;
            }

            constexpr double tightness = 100.0;
            std::vector<double> cum ((size_t) n, 0.0);
            std::vector<int> back ((size_t) n, -1);
            const int lo = (int) std::round (2.0 * period), hi = juce::jmax (1, (int) std::round (period / 2.0));

            for (int t = 0; t < n; ++t)
            {
                double best = 0.0;
                int arg = -1;

                for (int p = juce::jmax (0, t - lo); p <= t - hi; ++p)
                {
                    auto g = std::log ((t - p) / period);
                    auto s = cum[(size_t) p] - tightness * g * g;

                    if (arg < 0 || s > best)
                    {
                        best = s;
                        arg = p;
                    }
                }

                if (arg >= 0 && best > 0.0)
                {
                    cum[(size_t) t] = local[(size_t) t] + best;
                    back[(size_t) t] = arg;
                }
                else
                    cum[(size_t) t] = local[(size_t) t];
            }

            // Letzter Beat: der spaeteste lokale Gipfel von cum ueber der
            // Haelfte des Medians aller Gipfel
            std::vector<double> peakVals;

            for (int t = 1; t + 1 < n; ++t)
                if (cum[(size_t) t] > cum[(size_t) t - 1] && cum[(size_t) t] >= cum[(size_t) t + 1])
                    peakVals.push_back (cum[(size_t) t]);

            const double thresh = 0.5 * median (peakVals);
            int last = -1;

            for (int t = n - 2; t >= 1 && last < 0; --t)
                if (cum[(size_t) t] > cum[(size_t) t - 1] && cum[(size_t) t] >= cum[(size_t) t + 1] && cum[(size_t) t] >= thresh)
                    last = t;

            frames.clear();

            for (int t = last; t >= 0; t = back[(size_t) t])
                frames.push_back (t);

            std::reverse (frames.begin(), frames.end());

            // Schwache Beats am Rand (Stille, Ausklang) weg
            double rms = 0.0;

            for (auto v : local)
                rms += v * v;

            rms = std::sqrt (rms / n);

            while (! frames.empty() && local[(size_t) frames.front()] < 0.5 * rms)
                frames.erase (frames.begin());

            while (! frames.empty() && local[(size_t) frames.back()] < 0.5 * rms)
                frames.pop_back();
        };

        track (period);

        if (frames.size() < 4)
            return r;

        // Erst die Rand-Abstaende: weicht der erste/letzte Abstand (je
        // Periode) um mehr als 12 % ab, faellt der Rand-Beat — er wuerde die
        // Regression bei kurzen Bereichen sonst mitziehen
        auto gapOff = [&] (int a, int b)
        {
            auto gap = (double) (b - a);
            auto k = juce::jmax (1.0, std::round (gap / period));
            return std::abs (gap / k - period) / period;
        };

        while (frames.size() >= 4)
        {
            if (gapOff (frames[frames.size() - 2], frames.back()) > 0.12)
                frames.pop_back();
            else if (gapOff (frames[0], frames[1]) > 0.12)
                frames.erase (frames.begin());
            else
                break;
        }

        std::vector<int> idx;
        double slope = 0.0, icept = 0.0;

        auto fit = [&]
        {
            idx.assign (1, 0);

            for (size_t i = 1; i < frames.size(); ++i)
                idx.push_back (idx.back() + juce::jmax (1, (int) std::round ((frames[i] - frames[i - 1]) / period)));

            const double nIdx = (double) idx.size();
            double sx = 0, sy = 0, sxx = 0, sxy = 0;

            for (size_t i = 0; i < frames.size(); ++i)
            {
                double x = idx[i], y = frames[i] / f.fps;
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }

            slope = (nIdx * sxy - sx * sy) / (nIdx * sxx - sx * sx);
            icept = (sy - slope * sx) / nIdx;
        };

        auto residual = [&] (size_t i) { return std::abs (frames[i] / f.fps - (icept + slope * idx[i])); };

        for (fit(); frames.size() >= 4 && slope > 0.0;)
        {
            auto rFirst = residual (0), rLast = residual (frames.size() - 1);

            if (juce::jmax (rFirst, rLast) <= 0.2 * slope)
                break;

            if (rLast >= rFirst)
                frames.pop_back();
            else
                frames.erase (frames.begin());

            fit();
        }

        if (frames.size() < 4 || ! (slope > 0.0))
            return r;

        // 3a. Wandern die Beats (> 40 ms), obwohl fast alle Einsaetze auf
        //     einem geraden 16tel-Raster liegen, folgte das Tracking
        //     synkopierten Anschlaegen (Praxistest 1h: Intro ohne Drums,
        //     Drift 119 ms, der Agent hielt es fuer frei gespielt). Dann
        //     gilt das gerade Raster: Periode und Phase passend zu den
        //     Einsaetzen, der Schlag auf der 16tel, die den getrackten Beats
        //     am naechsten liegt.
        {
            double drift = 0.0;

            for (size_t i = 0; i < frames.size(); ++i)
                drift = juce::jmax (drift, residual (i));

            std::vector<std::pair<double, double>> pk;   // Frame, Staerke

            for (int i = 1; i + 1 < n; ++i)
                if (onset[(size_t) i] > 1.0f && onset[(size_t) i] >= onset[(size_t) i - 1] && onset[(size_t) i] > onset[(size_t) i + 1])
                    pk.push_back ({ (double) i, (double) onset[(size_t) i] });

            if (drift > 0.04 && pk.size() >= 8)
            {
                const double sigma = 0.015 * f.fps, near = 0.025 * f.fps;
                double total = 0.0, bestScore = -1.0, bestP = period, bestPhase = 0.0;

                for (auto& [fr, s] : pk)
                    total += s;

                auto distTo = [] (double x, double phase, double step)
                {
                    const double u = std::fmod (x - phase, step);
                    const double v = u < 0.0 ? u + step : u;
                    return juce::jmin (v, step - v);
                };

                for (double P = 0.97 * period; P <= 1.03 * period; P += 0.001 * period)
                    for (double ph = 0.0; ph < P / 4.0; ph += 0.25)
                    {
                        double sc = 0.0;

                        for (auto& [fr, s] : pk)
                        {
                            const double d = distTo (fr, ph, P / 4.0) / sigma;
                            sc += s * std::exp (-0.5 * d * d);
                        }

                        if (sc > bestScore)
                            bestScore = sc, bestP = P, bestPhase = ph;
                    }

                double onGrid = 0.0;

                for (auto& [fr, s] : pk)
                    onGrid += distTo (fr, bestPhase, bestP / 4.0) <= near ? s : 0.0;

                if (onGrid >= 0.7 * total)
                {
                    // Schlag-Phase: die 16tel, auf der die meisten getrackten Beats liegen
                    double beatPhase = bestPhase;
                    int most = -1;

                    for (int j = 0; j < 4; ++j)
                    {
                        int cnt = 0;

                        for (auto fr : frames)
                            cnt += distTo (fr, bestPhase + j * bestP / 4.0, bestP) <= bestP / 8.0 ? 1 : 0;

                        if (cnt > most)
                            most = cnt, beatPhase = bestPhase + j * bestP / 4.0;
                    }

                    const double from = frames.front() - bestP / 2.0, to = frames.back() + bestP / 2.0;
                    std::vector<int> straight;

                    for (double x = beatPhase; x < n; x += bestP)
                        if (x >= from && x <= to && x >= 0.0)
                            straight.push_back ((int) std::round (x));

                    // nur wenn die meisten getrackten Beats auf dieser 16tel liegen —
                    // sonst ist die Schlag-Phase offen und das Tracking bleibt
                    if (straight.size() >= 4 && most >= 0.4 * (double) frames.size())
                    {
                        frames = straight;
                        period = bestP;
                        fit();
                        r.straightened = true;
                    }
                }
            }
        }

        // 3a'. Wandern nur einzelne Beats, waehrend fast alle (>= 85 %) auf
        //     einer Geraden liegen (+-20 ms), folgte das Tracking in einem Teil
        //     synkopierten Anschlaegen — die Quelle selbst ist gerade (eigener
        //     Render aus quantisiertem MIDI, Praxistest 3: 108 BPM, Drift
        //     59-99 ms im 4-Takt-Teil ohne Drums; 3a griff nicht, weil
        //     gehaltene Klaenge viele Onset-Gipfel neben dem 16tel-Raster
        //     geben). Dann gilt die Gerade durch diese Beats. Frei gespieltes
        //     Material und falsche Tempi lagen in der Gegenprobe bei hoechstens
        //     72 % (152 Messungen).
        if (! r.straightened && frames.size() >= 8)
        {
            double drift = 0.0;

            for (size_t i = 0; i < frames.size(); ++i)
                drift = juce::jmax (drift, residual (i));

            if (drift > 0.04)
            {
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                int m = 0;

                for (size_t i = 0; i < frames.size(); ++i)
                    if (residual (i) <= 0.02)
                    {
                        const double x = idx[i], y = frames[i] / f.fps;
                        sx += x; sy += y; sxx += x * x; sxy += x * y;
                        ++m;
                    }

                if (m >= 0.85 * (double) frames.size() && m * sxx - sx * sx > 0.0)
                {
                    const double sl = (m * sxy - sx * sy) / (m * sxx - sx * sx), ic = (sy - sl * sx) / m;
                    int onLine = 0;

                    for (size_t i = 0; i < frames.size(); ++i)
                        onLine += std::abs (frames[i] / f.fps - (ic + sl * idx[i])) <= 0.02 ? 1 : 0;

                    if (sl > 0.0 && onLine >= 0.85 * (double) frames.size())
                    {
                        std::vector<int> straight;

                        for (int k = idx.front(); k <= idx.back(); ++k)
                            if (auto fr = (int) std::round ((ic + sl * k) * f.fps); fr >= 0 && fr < n)
                                straight.push_back (fr);

                        if (straight.size() >= 4)
                        {
                            frames = straight;
                            period = sl * f.fps;
                            fit();
                            r.snapped = true;
                        }
                    }
                }
            }
        }

        // 3b. Eine halbe Zaehlzeit daneben? Kick und Bass auf "und"
        //     (synkopiert) ziehen das Tracking auf die Offbeats (Praxistest
        //     1c: Takt-Eins und Groove-Raster lagen eine Achtel daneben).
        //     Hats/Tamburin betonen die Viertel, Harmonien wechseln auf dem
        //     Schlag — beides an den Beats und um eine halbe Periode
        //     versetzt messen; ist die versetzte Lage deutlich besser, rueckt
        //     das Raster dorthin
        {
            const int half = (int) std::round (slope * f.fps / 2.0);
            const int nb0 = (int) frames.size();
            std::vector<double> hi ((size_t) n, 0.0);
            double hiMean = 0.0;

            for (int i = 1; i < n && i < (int) f.bandEnergy.size(); ++i)
            {
                hi[(size_t) i] = juce::jmax (0.0, (double) f.bandEnergy[(size_t) i][2] - (double) f.bandEnergy[(size_t) i - 1][2]);
                hiMean += hi[(size_t) i] / n;
            }

            auto peakNear = [&] (const std::vector<double>& v, int fr)
            {
                double m = 0.0;

                for (int k = -2; k <= 2; ++k)
                    if (fr + k >= 0 && fr + k < (int) v.size())
                        m = juce::jmax (m, v[(size_t) (fr + k)]);

                return m;
            };

            // Chroma-Wechsel ueber Fenster von je einer Periode ab den Rasterpunkten
            auto changeAt = [&] (int shift)
            {
                double sum = 0.0;
                int cnt = 0;

                for (int i = 1; i < nb0 && ! f.chroma.empty(); ++i)
                {
                    auto window = [&] (int fr)
                    {
                        std::array<double, 12> c {};
                        auto c0 = juce::jlimit (0, (int) f.chroma.size() - 1, (int) std::round ((fr + shift) / f.fps * f.chromaFps));
                        auto c1 = juce::jlimit (c0 + 1, (int) f.chroma.size(), (int) std::round ((fr + shift + slope * f.fps) / f.fps * f.chromaFps));

                        for (int k = c0; k < c1; ++k)
                            for (size_t q = 0; q < 12; ++q)
                                c[q] += f.chroma[(size_t) k][q];

                        return c;
                    };

                    auto a = window (frames[(size_t) i - 1]), b = window (frames[(size_t) i]);
                    double dot = 0, na = 0, nb2 = 0;

                    for (size_t q = 0; q < 12; ++q)
                        dot += a[q] * b[q], na += a[q] * a[q], nb2 += b[q] * b[q];

                    if (na > 0 && nb2 > 0)
                        sum += 1.0 - dot / std::sqrt (na * nb2), ++cnt;
                }

                return cnt > 0 ? sum / cnt : 0.0;
            };

            double hiOn = 0.0, hiOff = 0.0;

            for (auto fr : frames)
            {
                hiOn += peakNear (hi, fr);
                hiOff += peakNear (hi, fr + half);
            }

            auto rel = [] (double on, double off) { return on + off > 1e-12 ? (off - on) / (off + on) : 0.0; };
            const double hiVote = hiMean > 1e-12 ? rel (hiOn, hiOff) : 0.0;
            const double chVote = rel (changeAt (0), changeAt (half));

            // Rueckschlag: die Snare auf 2 und 4 macht im Band 200 Hz - 1 kHz
            // (Snare-Koerper; Kick und Bass liegen darunter) jeden zweiten
            // Schlag staerker als den dazwischen. Liegt dieses Gefaelle auf
            // den Zwischen-Achteln, sitzt das Raster eine halbe Zaehlzeit
            // daneben (Refrain-Mix Praxistest 1l: Kick und Bass auf fast allen
            // Achteln, Gesang — Hats und Akkordwechsel entschieden nicht)
            double backVote = 0.0;
            {
                const std::vector<double> mid (f.midOnset.begin(), f.midOnset.end());
                double sum[2][2] = {};   // [auf dem Schlag / dazwischen][gerader / ungerader Schlag]
                int cnt[2][2] = {};

                for (size_t i = 0; i < frames.size(); ++i)
                {
                    const int p = idx[i] % 2;
                    sum[0][p] += peakNear (mid, frames[i]);
                    sum[1][p] += peakNear (mid, frames[i] + half);
                    ++cnt[0][p];
                    ++cnt[1][p];
                }

                if (cnt[0][0] > 0 && cnt[0][1] > 0)
                {
                    double m[2][2], mean = 0.0;

                    for (int a = 0; a < 2; ++a)
                        for (int p = 0; p < 2; ++p)
                            mean += (m[a][p] = sum[a][p] / cnt[a][p]) / 4.0;

                    if (mean > 1e-9)
                        backVote = juce::jlimit (-1.0, 1.0, (std::abs (m[1][0] - m[1][1]) - std::abs (m[0][0] - m[0][1])) / mean);
                }
            }

            r.offbeatVote = hiVote + chVote + backVote;
            r.backbeatVote = backVote;

            // Veto gegen das Verschieben (Praxistest 2 remix-1, House: Kick
            // auf allen Vierteln, Clap auf 2/4, offene Hats und Bass auf
            // "und" — die Hats zogen das Raster auf die Offbeats): ein
            // deutlicher Rueckschlag auf den jetzigen Schlaegen (<= -0,2)
            // oder eine Kick auf jedem Schlag ohne Tiefen dazwischen (Onset
            // bis 150 Hz eine halbe Periode daneben < 35 % — Drum-Stem des
            // Remix 3-17 %) haelt das Raster, solange kein Rueckschlag fuer
            // die Verschiebung spricht. In der Gegenprobe (152 Messungen) kam
            // keine Verschiebung aus so einer Lage
            {
                const std::vector<double> lowv (f.lowOnset.begin(), f.lowOnset.end());
                double lowOn = 0.0, lowOff = 0.0;

                for (auto fr : frames)
                {
                    lowOn += peakNear (lowv, fr);
                    lowOff += peakNear (lowv, fr + half);
                }

                r.phaseKept = r.offbeatVote > 0.3
                              && (backVote <= -0.2 || (lowOn > 1e-9 && lowOff < 0.35 * lowOn && backVote < 0.2));
            }

            if (r.offbeatVote > 0.3 && ! r.phaseKept)
            {
                for (auto& fr : frames)
                    fr += half;

                while (! frames.empty() && frames.back() >= n)
                    frames.pop_back();

                if (frames.size() < 4)
                    return r;

                fit();
                r.phaseShifted = true;
            }

            // Ohne Rueckschlag (keine Snare: Intro ohne Drums) und mit fast so
            // starken Einsaetzen auf "und" wie auf dem Schlag ist die
            // Schlag-Phase im Signal kaum bestimmt — die Merkmale
            // unterscheiden sich um 10-25 % oder widersprechen sich
            // (Referenzsong aus Lauf 1l: Intro 0-18,5 s eine halbe Zaehlzeit
            // daneben, ohne Hinweis). Entscheidung Matthias 2026-10-29: dann
            // melden — kein Rueckschlag (|backVote| < 0,15), auf der halben
            // Zaehlzeit mindestens 60 % des Onsets der Schlaege und kein
            // Schlagzeug: im Band 250 Hz - 4 kHz kommt weniger als 6 % der
            // Leistung aus Anstiegen (gehaltene Klaenge; Intro 0,044-0,048,
            // Mixe und Stems mit Drums ab 0,069 — ohne diese Bedingung meldeten
            // auch 7 von 34 Fenstern des Drum-Stems und 6 von 23 ganzen Songs)
            {
                double on = 0.0, off = 0.0, level = 0.0, rise = 0.0;

                for (auto fr : frames)
                {
                    on += peakNear (local, fr);
                    off += peakNear (local, fr + half);
                }

                for (size_t i = 1; i < f.bandEnergy.size(); ++i)
                {
                    level += f.bandEnergy[i][1];
                    rise += juce::jmax (0.0f, f.bandEnergy[i][1] - f.bandEnergy[i - 1][1]);
                }

                r.phaseUncertain = std::abs (backVote) < 0.15 && on > 1e-12 && off >= 0.6 * on
                                   && level > 1e-12 && rise < 0.06 * level;
            }
        }

        r.bpm = 60.0 / slope;

        for (size_t i = 0; i < frames.size(); ++i)
        {
            r.beats.push_back (frames[i] / f.fps);
            r.maxDriftMs = juce::jmax (r.maxDriftMs, residual (i) * 1000.0);
        }

        // Konfidenz: wie stark die Beat-Frames ueber dem Mittel liegen
        double atBeats = 0.0, overall = std::accumulate (local.begin(), local.end(), 0.0) / n;

        for (auto fr : frames)
            atBeats += local[(size_t) fr];

        atBeats /= (double) frames.size();
        r.confidence = overall > 1e-9 ? juce::jlimit (0.0, 1.0, 1.0 - overall / atBeats) : 0.0;

        // 4. Takt: je Beat Bass-Onset und Harmoniewechsel (Chroma gegen den
        //    vorigen Beat), als z-Werte; die Taktphase mit dem staerksten
        //    Mittel ist die Eins — 3er- oder 4er-Takt nach dem deutlicheren
        //    Muster, 4/4 im Zweifel
        const size_t nb = frames.size();
        std::vector<double> low (nb, 0.0), change (nb, 0.0);
        std::vector<std::array<float, 12>> chromaAt (nb);

        for (size_t i = 0; i < nb; ++i)
        {
            for (int k = -2; k <= 2; ++k)
                if (frames[i] + k >= 0 && frames[i] + k < n)
                    low[i] = juce::jmax (low[i], (double) f.lowOnset[(size_t) (frames[i] + k)]);

            if (! f.chroma.empty())
            {
                auto t0 = r.beats[i], t1 = i + 1 < nb ? r.beats[i + 1] : t0 + slope;
                auto c0 = juce::jlimit (0, (int) f.chroma.size() - 1, (int) std::round (t0 * f.chromaFps));
                auto c1 = juce::jlimit (c0 + 1, (int) f.chroma.size(), (int) std::round (t1 * f.chromaFps));
                std::array<float, 12> c {};

                for (int k = c0; k < c1; ++k)
                    for (size_t p = 0; p < 12; ++p)
                        c[p] += f.chroma[(size_t) k][p];

                chromaAt[i] = c;
            }

            if (i > 0)
            {
                double dot = 0, na = 0, nb2 = 0;

                for (size_t p = 0; p < 12; ++p)
                {
                    dot += chromaAt[i][p] * chromaAt[i - 1][p];
                    na += chromaAt[i][p] * chromaAt[i][p];
                    nb2 += chromaAt[i - 1][p] * chromaAt[i - 1][p];
                }

                change[i] = na > 0 && nb2 > 0 ? 1.0 - dot / std::sqrt (na * nb2) : 0.0;
            }
        }

        // Streuung mindestens 40 % des Mittels: ein Merkmal, das von Schlag
        // zu Schlag fast gleich bleibt (Kick auf allen Achteln ohne Snare),
        // traegt keine Takt-Information — sonst blaeht der z-Wert Reste auf.
        // Bei 130 BPM ist ein Takt genau 159 Onset-Frames lang; wo der
        // Anschlag im Frame liegt, wiederholt sich dann je Takt, und die
        // Kick-Staerke schwankte darum taktweise um 18 % — als z-Wert
        // stimmte das die Akkordwechsel nieder (Takt-Eins zwei Schlaege
        // daneben)
        auto zscore = [] (std::vector<double>& v)
        {
            double m = std::accumulate (v.begin(), v.end(), 0.0) / (double) v.size(), sq = 0.0;

            for (auto x : v)
                sq += (x - m) * (x - m);

            auto sd = juce::jmax (std::sqrt (sq / (double) v.size()), 0.4 * std::abs (m));

            for (auto& x : v)
                x = sd > 1e-9 ? (x - m) / sd : 0.0;
        };

        zscore (low);
        zscore (change);

        struct Meter { int beats; int phase; double score; };
        auto bestPhase = [&] (int m)
        {
            Meter best { m, 0, -1e9 };

            for (int ph = 0; ph < m; ++ph)
            {
                double on = 0, off = 0;
                int nOn = 0, nOff = 0;

                for (size_t i = 0; i < nb; ++i)
                {
                    auto s = low[i] + change[i];

                    if (((idx[i] - ph) % m + m) % m == 0) { on += s; ++nOn; }
                    else { off += s; ++nOff; }
                }

                if (nOn > 0 && nOff > 0 && on / nOn - off / nOff > best.score)
                    best = { m, ph, on / nOn - off / nOff };
            }

            return best;
        };

        auto m4 = bestPhase (4), m3 = bestPhase (3);
        auto meter = m3.score > 1.25 * m4.score && m3.score > 0.3 ? m3 : m4;
        r.beatsPerBar = meter.beats;
        r.meterConfidence = juce::jlimit (0.0, 1.0, meter.score / 1.5);

        for (size_t i = 0; i < nb; ++i)
            if (((idx[i] - meter.phase) % meter.beats + meter.beats) % meter.beats == 0)
                r.downbeats.push_back (r.beats[i]);

        r.valid = true;
        return r;
    }

    const std::vector<ChordType>& chordTypes()
    {
        static const std::vector<ChordType> types = {
            { "", { 0, 4, 7 } }, { "m", { 0, 3, 7 } }, { "7", { 0, 4, 7, 10 } }, { "maj7", { 0, 4, 7, 11 } },
            { "m7", { 0, 3, 7, 10 } }, { "dim", { 0, 3, 6 } }, { "sus4", { 0, 5, 7 } }, { "sus2", { 0, 2, 7 } }, { "5", { 0, 7 } },
        };
        return types;
    }

    juce::String pitchName (int pc, bool flats)
    {
        static const char* sharp[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static const char* flat[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
        pc = ((pc % 12) + 12) % 12;
        return flats ? flat[pc] : sharp[pc];
    }

    bool keyUsesFlats (int tonic, bool minor)
    {
        // Dur mit b: F Bb Eb Ab Db (Fis-Dur mit Kreuzen, nicht Ges-Dur — so
        // ueblicher); Moll ueber die parallele Dur-Tonart
        auto major = minor ? (tonic + 3) % 12 : tonic;
        return major == 5 || major == 10 || major == 3 || major == 8 || major == 1;
    }

    bool isRelativeKey (const KeyCandidate& a, const KeyCandidate& b)
    {
        if (a.minor == b.minor)
            return false;

        return a.minor ? (a.tonic + 3) % 12 == b.tonic : (b.tonic + 3) % 12 == a.tonic;
    }

    KeyResult detectKey (const Features& f, const std::vector<double>& beats)
    {
        KeyResult r;
        const int n = (int) f.chroma.size();

        if (n == 0 || f.chromaFps <= 0.0)
            return r;

        // Stille-Frames (unter 2 % der lautesten) zaehlen nicht
        const float maxEnergy = *std::max_element (f.chromaEnergy.begin(), f.chromaEnergy.end());
        auto audible = [&] (int i) { return f.chromaEnergy[(size_t) i] > 0.02f * maxEnergy && maxEnergy > 1e-6f; };

        // 1. Tonart: Tonart-Chroma je Frame auf Max 1, mal Wurzel seiner
        //    Summe, ueber alle hoerbaren Frames gegen die
        //    Krumhansl-Kessler-Profile in allen 24 Lagen (Pearson)
        std::array<double, 12> total {};

        for (int i = 0; i < n && i < (int) f.keyChroma.size(); ++i)
        {
            auto& k = f.keyChroma[(size_t) i];
            auto mx = *std::max_element (k.begin(), k.end());

            if (audible (i) && mx > 1e-6f)
                for (size_t p = 0; p < 12; ++p)
                    total[p] += k[p] / mx * std::sqrt ((double) std::accumulate (k.begin(), k.end(), 0.0f));
        }

        if (std::accumulate (total.begin(), total.end(), 0.0) <= 0.0)
            return r;

        static const double majorProfile[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double minorProfile[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        auto pearson = [] (const std::array<double, 12>& a, const double* prof, int shift)
        {
            double ma = 0, mb = 0;

            for (int i = 0; i < 12; ++i) { ma += a[(size_t) i]; mb += prof[i]; }

            ma /= 12; mb /= 12;
            double num = 0, da = 0, db = 0;

            for (int i = 0; i < 12; ++i)
            {
                auto x = a[(size_t) ((i + shift) % 12)] - ma, y = prof[i] - mb;
                num += x * y; da += x * x; db += y * y;
            }

            return da > 0 && db > 0 ? num / std::sqrt (da * db) : 0.0;
        };

        for (int t = 0; t < 12; ++t)
        {
            r.keys.push_back ({ t, false, pearson (total, majorProfile, t) });
            r.keys.push_back ({ t, true, pearson (total, minorProfile, t) });
        }

        std::sort (r.keys.begin(), r.keys.end(), [] (auto& a, auto& b) { return a.score > b.score; });
        // Abstand zur naechsten Tonart, die nicht die Paralleltonart ist
        // (A-Moll vs C-Dur teilen alle Toene — das ist keine Unsicherheit
        // ueber die Tonleiter, nur ueber das Zentrum)
        size_t other = 1;

        while (other < r.keys.size() && isRelativeKey (r.keys[0], r.keys[other]))
            ++other;

        r.confidence = juce::jlimit (0.0, 1.0, (r.keys[0].score - r.keys[other].score) * 4.0 + juce::jmax (0.0, r.keys[0].score - 0.5));

        // 2. Akkorde: Segmente je Beat (ohne Beats je 0,5 s), Chroma-Mittel
        //    gegen Vorlagen (Kosinus), Viterbi mit Wechsel-Strafe
        std::vector<double> bounds;

        if (beats.size() >= 2)
        {
            bounds = beats;

            if (bounds.front() > 0.25)
                bounds.insert (bounds.begin(), 0.0);

            bounds.push_back (juce::jmin (f.duration, bounds.back() + (beats.back() - beats[beats.size() - 2])));
        }
        else
            for (double t = 0.0; t < f.duration; t += 0.5)
                bounds.push_back (t);

        if (bounds.back() < f.duration - 0.05)
            bounds.push_back (f.duration);

        const auto& types = chordTypes();
        const int numStates = 12 * (int) types.size() + 1;   // letzter = kein Akkord
        const int noChord = numStates - 1;
        std::vector<std::array<float, 12>> templates;

        for (int root = 0; root < 12; ++root)
            for (auto& ct : types)
            {
                std::array<float, 12> tpl {};

                for (auto iv : ct.intervals)
                    tpl[(size_t) ((root + iv) % 12)] = 1.0f;

                templates.push_back (tpl);
            }

        const size_t segs = bounds.size() - 1;
        std::vector<std::vector<double>> emit (segs, std::vector<double> ((size_t) numStates, 0.0));

        for (size_t s = 0; s < segs; ++s)
        {
            auto c0 = juce::jlimit (0, n - 1, (int) std::floor (bounds[s] * f.chromaFps));
            auto c1 = juce::jlimit (c0 + 1, n, (int) std::ceil (bounds[s + 1] * f.chromaFps));
            std::array<double, 12> c {};
            int heard = 0;

            for (int k = c0; k < c1; ++k)
                if (audible (k))
                {
                    ++heard;

                    for (size_t p = 0; p < 12; ++p)
                        c[p] += f.chroma[(size_t) k][p];
                }

            double norm = 0.0;

            for (auto x : c)
                norm += x * x;

            norm = std::sqrt (norm);
            // Kein Akkord: Stille, oder flaches Chroma (Geraeusch, Drums)
            double flatness = 0.0;

            if (norm > 0)
            {
                double mx = *std::max_element (c.begin(), c.end()), mean = std::accumulate (c.begin(), c.end(), 0.0) / 12.0;
                flatness = mean / mx;
            }

            emit[s][(size_t) noChord] = heard == 0 || norm <= 0 ? 1.0 : juce::jlimit (0.0, 1.0, (flatness - 0.45) * 2.0);

            for (int st = 0; st < noChord; ++st)
            {
                if (heard == 0 || norm <= 0)
                    continue;

                auto& tpl = templates[(size_t) st];
                double dot = 0.0, tn = 0.0, mx = 0.0, weakest = 1e9;

                for (size_t p = 0; p < 12; ++p)
                {
                    dot += c[p] * tpl[p];
                    tn += tpl[p];
                    mx = juce::jmax (mx, c[p]);

                    if (tpl[p] > 0)
                        weakest = juce::jmin (weakest, c[p]);
                }

                // Kosinus, gedaempft, wenn ein Akkordton fehlt (ein Einzelton
                // ist kein Dur-Akkord); Vierklaenge, sus und 5 nur, wenn sie
                // deutlich besser passen
                auto sim = dot / (norm * std::sqrt (tn)) * (0.5 + 0.5 * juce::jmin (1.0, 2.0 * weakest / mx));
                auto type = st % (int) types.size();
                emit[s][(size_t) st] = sim - (type >= 2 ? 0.06 : 0.0);
            }
        }

        constexpr double switchPenalty = 0.08;
        std::vector<std::vector<int>> backPtr (segs, std::vector<int> ((size_t) numStates, 0));
        std::vector<double> score ((size_t) numStates), next ((size_t) numStates);

        for (int st = 0; st < numStates; ++st)
            score[(size_t) st] = emit[0][(size_t) st];

        for (size_t s = 1; s < segs; ++s)
        {
            auto best = (int) (std::max_element (score.begin(), score.end()) - score.begin());

            for (int st = 0; st < numStates; ++st)
            {
                auto stay = score[(size_t) st], change = score[(size_t) best] - switchPenalty;
                backPtr[s][(size_t) st] = stay >= change ? st : best;
                next[(size_t) st] = juce::jmax (stay, change) + emit[s][(size_t) st];
            }

            std::swap (score, next);
        }

        std::vector<int> path (segs);
        path[segs - 1] = (int) (std::max_element (score.begin(), score.end()) - score.begin());

        for (size_t s = segs - 1; s > 0; --s)
            path[s - 1] = backPtr[s][(size_t) path[s]];

        for (size_t s = 0; s < segs; ++s)
        {
            auto st = path[s];

            if (! r.chords.empty() && (r.chords.back().root < 0 ? noChord : r.chords.back().root * (int) types.size() + r.chords.back().type) == st)
            {
                auto& c = r.chords.back();
                auto len0 = c.end - c.start, len1 = bounds[s + 1] - bounds[s];
                c.confidence = (c.confidence * len0 + emit[s][(size_t) st] * len1) / (len0 + len1);
                c.end = bounds[s + 1];
                continue;
            }

            Chord c;
            c.start = bounds[s];
            c.end = bounds[s + 1];
            c.root = st == noChord ? -1 : st / (int) types.size();
            c.type = st == noChord ? 0 : st % (int) types.size();
            c.confidence = emit[s][(size_t) st];
            r.chords.push_back (c);
        }

        r.valid = true;
        return r;
    }

    StructureResult analyzeStructure (const MonoAudio& a, const Features& f, const TempoResult& tempo, const KeyResult& key)
    {
        StructureResult r;

        if (a.samples.empty() || f.fps <= 0.0)
            return r;

        // 1. Takte: ab der Takt-Eins im Takt-Raster; Auftakt davor als
        //    eigener "Takt 0"; ohne Puls Bloecke zu 2 s
        std::vector<double> starts;
        r.grid = tempo.valid && ! tempo.downbeats.empty();

        if (r.grid)
        {
            const double barLen = 60.0 / tempo.bpm * tempo.beatsPerBar;
            r.barLength = barLen;
            r.firstDownbeat = tempo.downbeats.front();
            // Die Takt-Einsen aus dem Tracking, Luecken (ausgelassene Beats)
            // mit dem Raster aufgefuellt
            double t = r.firstDownbeat;

            while (t - barLen > 0.05)
                t -= barLen;

            if (t > 0.05)
                starts.push_back (0.0);   // Auftakt

            for (; t < f.duration - 0.6 * barLen; t += barLen)
            {
                double best = t;

                for (auto d : tempo.downbeats)
                    if (std::abs (d - t) < 0.15 * barLen)
                        best = d;

                starts.push_back (best);
                t = best;
            }
        }
        else
            for (double t = 0.0; t < f.duration - 0.5; t += 2.0)
                starts.push_back (t);

        if (starts.size() < 2)
            return r;

        starts.push_back (f.duration);

        // 2. Merkmale je Takt
        auto frameOf = [&] (double t, double fps, size_t size) { return (size_t) juce::jlimit (0, (int) size, (int) std::round (t * fps)); };
        const double beatLen = r.grid ? 60.0 / tempo.bpm : 0.5;

        for (size_t b = 0; b + 1 < starts.size(); ++b)
        {
            BarInfo bi;
            bi.start = starts[b];
            bi.end = starts[b + 1];
            bi.pickup = r.grid && b == 0 && bi.start < r.firstDownbeat - 0.05;

            auto s0 = (size_t) juce::jlimit (0.0, (double) a.samples.size(), bi.start * a.sampleRate);
            auto s1 = (size_t) juce::jlimit ((double) s0, (double) a.samples.size(), bi.end * a.sampleRate);
            double sq = 0.0;

            for (auto i = s0; i < s1; ++i)
                sq += (double) a.samples[i] * a.samples[i];

            bi.rmsDb = s1 > s0 ? juce::Decibels::gainToDecibels (std::sqrt (sq / (double) (s1 - s0)), -100.0) : -100.0;

            auto o0 = frameOf (bi.start, f.fps, f.onset.size()), o1 = frameOf (bi.end, f.fps, f.onset.size());
            std::array<double, 3> be {};
            int peaks = 0;

            for (auto i = o0; i < o1; ++i)
            {
                for (size_t k = 0; k < 3; ++k)
                    be[k] += f.bandEnergy[i][k];

                if (i > 0 && i + 1 < f.onset.size() && f.onset[i] > 1.0f && f.onset[i] >= f.onset[i - 1] && f.onset[i] > f.onset[i + 1])
                    ++peaks;
            }

            auto total = be[0] + be[1] + be[2];

            for (size_t k = 0; k < 3; ++k)
                bi.bandsDb[k] = total > 0.0 && be[k] > 0.0 ? 10.0 * std::log10 (be[k] / total) : -60.0;

            bi.density = peaks / juce::jmax (1.0, (bi.end - bi.start) / beatLen);

            auto c0 = frameOf (bi.start, f.chromaFps, f.chroma.size()), c1 = frameOf (bi.end, f.chromaFps, f.chroma.size());

            for (auto i = c0; i < c1; ++i)
                for (size_t p = 0; p < 12; ++p)
                    bi.chroma[p] += f.chroma[i][p] * f.chromaEnergy[i];

            double cn = 0.0;

            for (auto x : bi.chroma)
                cn += x * x;

            if (cn > 0.0)
                for (auto& x : bi.chroma)
                    x /= std::sqrt (cn);

            // Akkord am Taktanfang (der Akkord, der den Takt am laengsten fuellt)
            double bestOverlap = 0.0;

            for (size_t c = 0; c < key.chords.size(); ++c)
            {
                auto ov = juce::jmin (bi.end, key.chords[c].end) - juce::jmax (bi.start, key.chords[c].start);

                if (ov > bestOverlap)
                {
                    bestOverlap = ov;
                    bi.chord = (int) c;
                }
            }

            r.bars.push_back (bi);
        }

        // Stille am Rand (Vorlauf, Ausklang unter -60 dB) gehoert zu keinem Teil
        while (r.bars.size() > 2 && r.bars.back().rmsDb < -60.0)
            r.bars.pop_back();

        while (r.bars.size() > 2 && r.bars.front().rmsDb < -60.0)
            r.bars.erase (r.bars.begin());

        // 3. Grenzen: Neuheit zwischen den Takten (Foote: Distanz der K Takte
        //    davor zu den K danach minus die Distanz innerhalb), bevorzugt
        //    an 4er-/2er-Phrasen; Mindestlaenge 4 Takte (2 ohne Puls)
        const size_t nb = r.bars.size();
        double rmsMin = 1e9, rmsMax = -1e9;

        for (auto& bi : r.bars)
            if (bi.rmsDb > -90.0)
            {
                rmsMin = juce::jmin (rmsMin, bi.rmsDb);
                rmsMax = juce::jmax (rmsMax, bi.rmsDb);
            }

        auto dist = [&] (const BarInfo& x, const BarInfo& y)
        {
            double cos = 0.0;

            for (size_t p = 0; p < 12; ++p)
                cos += x.chroma[p] * y.chroma[p];

            double d = 1.0 - cos;   // 0..1
            d += std::abs (x.rmsDb - y.rmsDb) / 12.0;
            d += std::abs (x.density - y.density) / 2.0;

            for (size_t k = 0; k < 3; ++k)
                d += std::abs (x.bandsDb[k] - y.bandsDb[k]) / 12.0;

            return d;
        };

        const int K = r.grid ? 4 : 3;
        std::vector<double> novelty (nb, 0.0);

        for (size_t b = 1; b < nb; ++b)
        {
            int lo = juce::jmax (0, (int) b - K), hi = juce::jmin ((int) nb, (int) b + K);

            if ((int) b - lo < 1 || hi - (int) b < 1)
                continue;

            double cross = 0.0, within = 0.0;
            int nc = 0, nw = 0;

            for (int i = lo; i < hi; ++i)
                for (int j = i + 1; j < hi; ++j)
                {
                    auto d = dist (r.bars[(size_t) i], r.bars[(size_t) j]);

                    if ((i < (int) b) != (j < (int) b)) { cross += d; ++nc; }
                    else { within += d; ++nw; }
                }

            novelty[b] = juce::jmax (0.0, (nc > 0 ? cross / nc : 0.0) - (nw > 0 ? within / nw : 0.0));

            if (r.grid)
            {
                // Phrasen-Bonus relativ zur Takt-Eins (Auftakt nicht mitgezaehlt)
                auto barNo = (int) b - (r.bars.front().pickup ? 1 : 0);
                novelty[b] *= barNo % 4 == 0 ? 1.0 : barNo % 2 == 0 ? 0.85 : 0.7;
            }
        }

        // Grenzen = lokale Maxima der Neuheit (+-2 Takte) ueber einer
        // Schwelle relativ zum staerksten Wechsel — eine globale Schwelle
        // liesse grosse Wechsel (Intro -> Drums) die kleineren (Strophe ->
        // Refrain) verdecken
        const double maxNovelty = *std::max_element (novelty.begin(), novelty.end());
        const double thresh = juce::jmax (0.15, 0.1 * maxNovelty);
        const int minLen = r.grid ? 4 : 2;
        std::vector<size_t> order;

        for (size_t b = 1; b < nb; ++b)
        {
            bool peak = novelty[b] >= thresh;

            for (int k = -2; k <= 2 && peak; ++k)
                if (k != 0 && (int) b + k >= 0 && (int) b + k < (int) nb)
                    peak = novelty[b] >= novelty[(size_t) ((int) b + k)];

            if (peak)
                order.push_back (b);
        }

        // Staerkste Grenzen zuerst, Abstand >= minLen zu allen gewaehlten
        // und zum Ende
        std::sort (order.begin(), order.end(), [&] (auto x, auto y) { return novelty[x] > novelty[y]; });
        const bool hasPickup = r.bars.front().pickup;
        std::vector<size_t> cuts { 0 };

        // Ein Auftakt ist immer eine eigene Grenze
        if (hasPickup && nb > 1)
            cuts.push_back (1);

        for (auto b : order)
        {
            bool ok = (int) (nb - b) >= minLen;

            for (auto c : cuts)
            {
                const bool afterPickup = hasPickup && c == 1;
                ok = ok && (std::abs ((int) b - (int) c) >= minLen || (afterPickup && (int) b - 1 >= minLen) || (c == 0 && hasPickup));
            }

            if (ok)
                cuts.push_back (b);
        }

        std::sort (cuts.begin(), cuts.end());

        // Lange Teile weiter teilen: in Songs mit gleich lauten Teilen gehen
        // deren Grenzen neben den grossen Wechseln (Intro, Ende) unter der
        // Schwelle unter (Praxistest 1: ein 58-Takt-Teil). Teile ueber 12
        // Takten bekommen ihre staerkste innere Grenze (mind. minLen Takte
        // Abstand), solange die ueber einer festen Untergrenze liegt.
        for (bool split = true; split;)
        {
            split = false;
            auto ends = cuts;
            ends.push_back (nb);

            for (size_t c = 0; c + 1 < ends.size(); ++c)
            {
                auto from = ends[c], to = ends[c + 1];

                if ((int) (to - from) <= (r.grid ? 12 : 8))
                    continue;

                size_t best = 0;
                double bestNov = 0.15;

                for (auto b = from + (size_t) minLen; b + (size_t) minLen <= to; ++b)
                    if (novelty[b] > bestNov)
                    {
                        bestNov = novelty[b];
                        best = b;
                    }

                if (best > 0)
                {
                    cuts.push_back (best);
                    std::sort (cuts.begin(), cuts.end());
                    split = true;
                    break;
                }
            }
        }

        // Ein wiederholter 4-Takt-Akkordverlauf, auf den ein anderer folgt,
        // ist eine Grenze, auch wenn Klang und Lautheit gleich bleiben
        // (Praxistest 3: Refrain und Bridge des eigenen Songs als ein Teil
        // mit 16 Takten). Nur an Phrasen (4 Takte ab Teilbeginn): die zwei
        // Bloecke davor stimmen im Grundton je Takt zu >= 75 % ueberein, der
        // danach mit dem davor zu <= 25 %. Rauschende Akkorde (echte Mixe)
        // erreichen die 75 % selten — dann bleibt es bei der Neuheit oben.
        if (r.grid && ! key.chords.empty())
        {
            auto rootAt = [&] (size_t b) { auto c = r.bars[b].chord; return c >= 0 && c < (int) key.chords.size() ? key.chords[(size_t) c].root : -1; };
            auto agree = [&] (size_t a, size_t b)
            {
                int same = 0;

                for (size_t i = 0; i < 4; ++i)
                    same += rootAt (a + i) == rootAt (b + i) ? 1 : 0;

                return same / 4.0;
            };

            for (bool split = true; split;)
            {
                split = false;
                auto ends = cuts;
                ends.push_back (nb);

                for (size_t c = 0; c + 1 < ends.size() && ! split; ++c)
                {
                    const auto from = ends[c], to = ends[c + 1];

                    for (auto b = from + 8; b + 4 <= to && ! split; b += 4)
                        if (agree (b - 8, b - 4) >= 0.75 && agree (b - 4, b) <= 0.25)
                        {
                            cuts.push_back (b);
                            std::sort (cuts.begin(), cuts.end());
                            split = true;
                        }
                }
            }
        }

        cuts.push_back (nb);

        // 4. Segmente, Buchstaben (gleicher Akkordverlauf Takt fuer Takt und
        //    aehnliche Lautheit = gleicher Teil), Energie 1..5
        for (size_t c = 0; c + 1 < cuts.size(); ++c)
        {
            Segment seg;
            seg.firstBar = (int) cuts[c];
            seg.numBars = (int) (cuts[c + 1] - cuts[c]);
            seg.start = r.bars[cuts[c]].start;
            seg.end = r.bars[cuts[c + 1] - 1].end;
            double rms = 0.0, dens = 0.0, high = 0.0;

            for (auto b = cuts[c]; b < cuts[c + 1]; ++b)
            {
                rms += r.bars[b].rmsDb;
                dens += r.bars[b].density;
                high += r.bars[b].bandsDb[2];
            }

            seg.rmsDb = rms / seg.numBars;
            seg.density = dens / seg.numBars;
            seg.highDb = high / seg.numBars;
            r.segments.push_back (seg);
        }

        double dMax = 1e-9, hMin = 1e9, hMax = -1e9;
        rmsMin = 1e9;
        rmsMax = -1e9;

        for (auto& sg : r.segments)
        {
            rmsMin = juce::jmin (rmsMin, sg.rmsDb);
            rmsMax = juce::jmax (rmsMax, sg.rmsDb);
            dMax = juce::jmax (dMax, sg.density);
            hMin = juce::jmin (hMin, sg.highDb);
            hMax = juce::jmax (hMax, sg.highDb);
        }

        for (auto& sg : r.segments)
        {
            auto loud = rmsMax > rmsMin ? (sg.rmsDb - rmsMin) / (rmsMax - rmsMin) : 0.5;
            auto hi = hMax > hMin ? (sg.highDb - hMin) / (hMax - hMin) : 0.5;
            auto score = juce::jlimit (0.0, 1.0, 0.6 * loud + 0.2 * (sg.density / dMax) + 0.2 * hi);
            sg.energy = 1 + (int) std::round (4.0 * score);
        }

        auto segSim = [&] (const Segment& x, const Segment& y)
        {
            auto n = juce::jmin (x.numBars, y.numBars);
            double s = 0.0;

            for (int i = 0; i < n; ++i)
            {
                auto& bx = r.bars[(size_t) (x.firstBar + i)];
                auto& by = r.bars[(size_t) (y.firstBar + i)];

                for (size_t p = 0; p < 12; ++p)
                    s += bx.chroma[p] * by.chroma[p];
            }

            return n > 0 ? s / n : 0.0;
        };

        // Grundton des Akkords je Takt gleich (Takt fuer Takt ab dem
        // Teilbeginn; ohne Akkord zaehlt "kein Akkord" als gleich)
        auto rootOf = [&] (const BarInfo& bi) { return bi.chord >= 0 && bi.chord < (int) key.chords.size() ? key.chords[(size_t) bi.chord].root : -1; };
        auto chordAgree = [&] (const Segment& x, const Segment& y)
        {
            auto n = juce::jmin (x.numBars, y.numBars);
            int same = 0;

            for (int i = 0; i < n; ++i)
                same += rootOf (r.bars[(size_t) (x.firstBar + i)]) == rootOf (r.bars[(size_t) (y.firstBar + i)]) ? 1 : 0;

            return n > 0 ? same / (double) n : 0.0;
        };

        int nextLabel = 0;

        for (size_t i = 0; i < r.segments.size(); ++i)
        {
            auto& sg = r.segments[i];
            sg.label = -1;

            for (size_t j = 0; j < i && sg.label < 0; ++j)
            {
                auto& other = r.segments[j];

                // 0,95: in diatonischen Songs liegen auch verschiedene Teile
                // ueber 0,9 (gleiche Tonklassen, andere Reihenfolge). Dazu
                // derselbe Akkordverlauf: mindestens die Haelfte der Takte
                // mit gleichem Grundton — sonst lagen in Praxistest 3 auch
                // Strophe, Refrain und Bridge des eigenen Songs ueber 0,95
                // (Grundton gleich in 25-42 % der Takte, Refrain und Refrain 2
                // in 88 %) und eine Referenz war ganz "chorus"
                if (std::abs (sg.rmsDb - other.rmsDb) < 4.0 && std::abs (sg.energy - other.energy) <= 1 && segSim (sg, other) > 0.95
                    && chordAgree (sg, other) >= 0.5)
                    sg.label = other.label;
            }

            if (sg.label < 0)
                sg.label = nextLabel++;
        }

        // 5. Rollen (Vermutung): der lauteste wiederkehrende Teil = Refrain,
        //    der haeufigste andere wiederkehrende = Strophe, einmalige am
        //    Anfang/Ende mit wenig Energie = Intro/Outro, sonst Bridge
        // Teile ganz am Anfang (nach einem Auftakt) und am Ende zaehlen fuer
        // Refrain/Strophe nicht mit — ein Intro, das als Outro wiederkommt,
        // ist kein Refrain
        std::map<int, int> count, inner;
        std::map<int, double> energy;
        const size_t firstBody = hasPickup ? 2 : 1;

        for (size_t i = 0; i < r.segments.size(); ++i)
        {
            auto& sg = r.segments[i];
            ++count[sg.label];

            if (i >= firstBody && i + 1 < r.segments.size())
            {
                ++inner[sg.label];
                energy[sg.label] += sg.energy;
            }
        }

        int chorus = -1, verse = -1;

        for (auto& [label, n] : inner)
            if (count[label] > 1 && (chorus < 0 || energy[label] / n > energy[chorus] / inner[chorus]))
                chorus = label;

        for (auto& [label, n] : inner)
            if (count[label] > 1 && label != chorus && (verse < 0 || n > inner[verse]))
                verse = label;

        double avgEnergy = 0.0;

        for (auto& sg : r.segments)
            avgEnergy += sg.energy;

        avgEnergy /= (double) r.segments.size();

        for (size_t i = 0; i < r.segments.size(); ++i)
        {
            auto& sg = r.segments[i];

            if (sg.label == chorus)
                sg.role = "chorus";
            else if (sg.label == verse)
                sg.role = "verse";
            else if (count[sg.label] > 1 && inner[sg.label] > 0)
                sg.role = "part";
            else if (r.segments.size() == 1)
                sg.role = "main";
            else if (i == 0 && (sg.energy <= avgEnergy || r.bars[(size_t) sg.firstBar].pickup))
                sg.role = r.bars[(size_t) sg.firstBar].pickup ? "pickup" : "intro";
            else if (i + 1 == r.segments.size() && sg.energy <= avgEnergy)
                sg.role = "outro";
            else if (i == 1 && r.segments[0].role == "pickup" && sg.energy <= avgEnergy)
                sg.role = "intro";
            else
                sg.role = "bridge";
        }

        r.valid = true;
        return r;
    }

    std::vector<DrumHit> detectDrumHits (const MonoAudio& a, double sensitivity)
    {
        std::vector<DrumHit> hits;

        if (a.samples.size() < 2048 || a.sampleRate <= 0.0)
            return hits;

        // Flux je Frequenz-Bin (log1p, gegen das Maximum der zwei Frames
        // davor), summiert je Band: Kick 35–150 Hz, Snare 1–4,5 kHz (ihr
        // Rauschen — um 200 Hz laeuft der Sweep einer Kick), Hi-Hat ab 6 kHz.
        // Pro Bin statt Bandenergie: ein Schlag in ausklingendes Material
        // (Hat nach Snare, Kick in die Kick davor) faellt so auf.
        constexpr int order = 9, size = 1 << order, hop = 64;
        const double binHz = a.sampleRate / size, fps = a.sampleRate / hop;
        const double bands[3][2] = { { 35.0, 150.0 }, { 1000.0, 4500.0 }, { 6000.0, 0.47 * a.sampleRate } };
        const double onsetDelay = 0.4 * size / a.sampleRate;   // Anstieg erscheint vor dem Anschlag
        const double norm = size / 4.0;
        const int bins = size / 2 + 1;
        std::array<std::vector<float>, 3> flux;
        std::vector<float> prev ((size_t) bins, 0.0f), prev2 ((size_t) bins, 0.0f), cur ((size_t) bins);
        int frame = 0;

        forEachFrame (a, order, hop, [&] (int, const float* mag)
        {
            std::array<double, 3> f {};

            for (int b = 1; b < bins; ++b)
            {
                cur[(size_t) b] = (float) std::log1p (1000.0 * mag[b] / norm);
                auto hz = b * binHz;
                auto d = frame < 2 ? 0.0f : juce::jmax (0.0f, cur[(size_t) b] - juce::jmax (prev[(size_t) b], prev2[(size_t) b]));

                for (int k = 0; k < 3; ++k)
                    if (hz >= bands[k][0] && hz <= bands[k][1])
                        f[(size_t) k] += d;
            }

            for (int k = 0; k < 3; ++k)
                flux[(size_t) k].push_back ((float) f[(size_t) k]);

            std::swap (prev2, prev);
            std::swap (prev, cur);
            ++frame;
        });

        const int n = (int) flux[0].size();

        // Normierung je Band: 95. Perzentil der deutlichen Gipfel (>= 10 % des groessten)
        std::array<std::vector<float>, 3> r;

        for (int k = 0; k < 3; ++k)
        {
            auto& f = flux[(size_t) k];
            std::vector<float> peaks;

            for (int t = 1; t + 1 < n; ++t)
                if (f[(size_t) t] > 0.0f && f[(size_t) t] >= f[(size_t) t - 1] && f[(size_t) t] > f[(size_t) t + 1])
                    peaks.push_back (f[(size_t) t]);

            float ref = 1.0f;

            if (! peaks.empty())
            {
                auto mx = *std::max_element (peaks.begin(), peaks.end());
                peaks.erase (std::remove_if (peaks.begin(), peaks.end(), [mx] (float v) { return v < 0.1f * mx; }), peaks.end());
                std::sort (peaks.begin(), peaks.end());
                ref = juce::jmax (1e-6f, peaks[(size_t) (0.95 * (double) (peaks.size() - 1))]);
            }

            r[(size_t) k].resize ((size_t) n);

            for (int t = 0; t < n; ++t)
                r[(size_t) k][(size_t) t] = f[(size_t) t] / ref;
        }

        // Ereignisse: lokale Maxima der Summe (+-25 ms); je Ereignis die
        // Baender vergleichen — Snare: Mitten klar ueber Hoehen und Tiefen;
        // Kick: Tiefen mit Anschlag-Klick in den Mitten (ein Basston hat keinen);
        // Hi-Hat: Hoehen mindestens 40 % der Mitten (auch mit Snare zusammen)
        const int near = (int) std::round (0.025 * fps), minGap = (int) std::round (0.05 * fps);
        const float scale = (float) (1.0 / juce::jlimit (0.25, 4.0, sensitivity));
        std::vector<float> total ((size_t) n);

        for (int t = 0; t < n; ++t)
            total[(size_t) t] = r[0][(size_t) t] + r[1][(size_t) t] + r[2][(size_t) t];

        auto at = [&] (int k, int t)
        {
            float m = 0.0f;

            for (int d = -3; d <= 3; ++d)
                if (t + d >= 0 && t + d < n)
                    m = juce::jmax (m, r[(size_t) k][(size_t) (t + d)]);

            return m;
        };

        std::array<int, 3> last { -minGap, -minGap, -minGap };
        const int noteOf[3] = { 36, 38, 42 };

        for (int t = 1; t + 1 < n; ++t)
        {
            if (total[(size_t) t] < 0.25f * scale)
                continue;

            bool peak = true;

            for (int d = -near; d <= near && peak; ++d)
                if (d != 0 && t + d >= 0 && t + d < n && total[(size_t) (t + d)] > total[(size_t) t])
                    peak = false;

            if (! peak)
                continue;

            const float lo = at (0, t), mi = at (1, t), hi = at (2, t);
            const bool is[3] = { lo >= 0.35f * scale && lo >= 0.5f * mi && mi >= 0.25f * lo,
                                 mi >= 0.5f * scale && mi >= 1.4f * hi && mi >= 1.8f * lo,
                                 hi >= 0.35f * scale && hi >= 0.4f * mi };
            const float strength[3] = { lo, mi, hi };

            for (int k = 0; k < 3; ++k)
                if (is[k] && t - last[(size_t) k] >= minGap)
                {
                    hits.push_back ({ juce::jmax (0.0, t / fps + onsetDelay), noteOf[k], juce::jlimit (0.0, 1.0, (double) strength[k] / 1.5) });
                    last[(size_t) k] = t;
                }
        }

        std::sort (hits.begin(), hits.end(), [] (auto& x, auto& y) { return x.time < y.time || (x.time == y.time && x.note < y.note); });
        return hits;
    }
}
