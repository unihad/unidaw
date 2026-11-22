#include "SoundFeatures.h"
#include "MusicAnalysis.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace dawd::sound
{
    namespace
    {
        constexpr double sr = 48000.0;

        double db (double power) { return 10.0 * std::log10 (std::max (power, 1e-12)); }

        // Tonhoehe je 10 ms (YIN auf 12 kHz), 0 = stimmlos, -1 = leise
        std::vector<double> pitchTrack (const std::vector<float>& x, double peakPower)
        {
            constexpr int dec = 4, win = 400, hop = 120, tauMin = 12, tauMax = 240;   // 12 kHz, 33 ms, 50..1000 Hz
            const double rate = sr / dec;
            std::vector<float> y (x.size() / dec);

            for (size_t i = 0; i < y.size(); ++i)
                y[i] = 0.25f * (x[i * dec] + x[i * dec + 1] + x[i * dec + 2] + x[i * dec + 3]);

            std::vector<double> f0;
            std::vector<double> d ((size_t) tauMax + 1);

            for (size_t start = 0; start + win + tauMax < y.size(); start += hop)
            {
                double energy = 0.0;

                for (int i = 0; i < win; ++i)
                    energy += (double) y[start + (size_t) i] * y[start + (size_t) i];

                if (db (energy / win) < db (peakPower) - 35.0)
                {
                    f0.push_back (-1.0);   // leise: zaehlt nicht
                    continue;
                }

                // Differenzfunktion, kumulativ normiert
                double run = 0.0;
                d[0] = 1.0;

                for (int tau = 1; tau <= tauMax; ++tau)
                {
                    double s = 0.0;

                    for (int i = 0; i < win; ++i)
                    {
                        double v = (double) y[start + (size_t) i] - y[start + (size_t) (i + tau)];
                        s += v * v;
                    }

                    run += s;
                    d[(size_t) tau] = run > 0.0 ? s * tau / run : 1.0;
                }

                int best = -1;

                for (int tau = tauMin; tau < tauMax; ++tau)
                    if (d[(size_t) tau] < 0.15)
                    {
                        while (tau + 1 < tauMax && d[(size_t) tau + 1] < d[(size_t) tau])
                            ++tau;

                        best = tau;
                        break;
                    }

                if (best < 0)
                {
                    f0.push_back (0.0);
                    continue;
                }

                // Parabel durch die Nachbarn
                double a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
                double denom = a - 2.0 * b + c;
                double t = best + (std::abs (denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0);
                f0.push_back (rate / t);
            }

            return f0;
        }

        double median (std::vector<double> v)
        {
            if (v.empty())
                return 0.0;

            std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
            return v[v.size() / 2];
        }
    }

    std::array<float, Features::count> Features::pack() const
    {
        return { (float) duration, oneShot ? 1.0f : 0.0f, (float) attackMs, (float) punchDb, (float) low,
                 (float) brightness, (float) pitchHz, (float) vibratoCents, (float) vibratoHz, (float) peakDb, (float) bpm };
    }

    Features Features::unpack (const float* v)
    {
        Features f;
        f.duration = v[0]; f.oneShot = v[1] > 0.5f; f.attackMs = v[2]; f.punchDb = v[3]; f.low = v[4];
        f.brightness = v[5]; f.pitchHz = v[6]; f.vibratoCents = v[7]; f.vibratoHz = v[8];
        f.peakDb = v[9]; f.bpm = v[10];
        return f;
    }

    juce::String noteName (double hz)
    {
        if (hz <= 0.0)
            return {};

        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        int midi = (int) std::lround (69.0 + 12.0 * std::log2 (hz / 440.0));
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    Features measure (const std::vector<float>& x, double fileDuration)
    {
        Features f;
        f.duration = fileDuration;
        const size_t n = x.size();

        if (n < 64)
            return f;

        // Spitze und Einsatz (1-ms-Huellkurve)
        constexpr int envHop = 48;
        std::vector<double> env (n / envHop);

        for (size_t i = 0; i < env.size(); ++i)
        {
            double s = 0.0;

            for (int k = 0; k < envHop; ++k)
                s += (double) x[i * envHop + (size_t) k] * x[i * envHop + (size_t) k];

            env[i] = std::sqrt (s / envHop);
        }

        double peakSample = 0.0;

        for (auto v : x)
            peakSample = std::max (peakSample, (double) std::abs (v));

        f.peakDb = 20.0 * std::log10 (std::max (peakSample, 1e-6));

        if (env.empty() || peakSample < 1e-5)
            return f;

        auto peakAt = (size_t) (std::max_element (env.begin(), env.end()) - env.begin());
        const double peakEnv = env[peakAt];
        size_t t10 = peakAt;

        while (t10 > 0 && env[t10 - 1] >= 0.1 * peakEnv)
            --t10;

        size_t t90 = t10;

        while (t90 < peakAt && env[t90] < 0.9 * peakEnv)
            ++t90;

        f.attackMs = (double) (t90 - t10) * envHop / sr * 1000.0;

        // Punch: mittlere Leistung der ersten 50 ms gegen 50–300 ms
        const size_t on = t10 * envHop;
        auto meanPower = [&] (size_t a, size_t b)
        {
            a = std::min (a, n);
            b = std::min (b, n);
            double s = 0.0;

            for (size_t i = a; i < b; ++i)
                s += (double) x[i] * x[i];

            return b > a ? s / (double) (b - a) : 0.0;
        };
        const size_t ms50 = (size_t) (0.05 * sr), ms300 = (size_t) (0.3 * sr);
        if (fileDuration >= 0.1)   // Einzelzyklen und Klicks haben keinen Ausklang
            f.punchDb = juce::jlimit (-20.0, 40.0, db (meanPower (on, on + ms50)) - db (meanPower (on + ms50, on + ms300)));

        // Spektrum: 2048er Frames, Hop 1024 (erste 10 s)
        constexpr int order = 11, size = 1 << order, hop = 1024, half = size / 2;
        juce::dsp::FFT fft (order);
        std::vector<float> buf ((size_t) size * 2), hann ((size_t) size);

        for (int i = 0; i < size; ++i)
            hann[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / size));

        const double binHz = sr / size;
        const int bin150 = (int) (150.0 / binHz);
        const size_t limit = std::min (n, (size_t) (10.0 * sr));
        double lowE = 0.0, allE = 0.0, centroidSum = 0.0, centroidW = 0.0;
        const double peakFrameDb = db (peakEnv * peakEnv);

        for (size_t start = 0; start + size <= std::max (limit, (size_t) size) && start + size <= n; start += hop)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            double frameE = 0.0;

            for (int i = 0; i < size; ++i)
            {
                buf[(size_t) i] = x[start + (size_t) i] * hann[(size_t) i];
                frameE += (double) x[start + (size_t) i] * x[start + (size_t) i];
            }

            fft.performFrequencyOnlyForwardTransform (buf.data(), true);
            const double frameDb = db (frameE / size);
            const double centre = (double) (start + half);
            double total = 0.0, weighted = 0.0, low = 0.0;

            for (int b = 1; b < half; ++b)
            {
                double p = (double) buf[(size_t) b] * buf[(size_t) b];
                total += p;
                weighted += p * b * binHz;

                if (b <= bin150)
                    low += p;
            }

            if (frameDb > peakFrameDb - 40.0 && total > 0.0)
            {
                centroidSum += weighted;
                centroidW += total;
            }

            if (centre >= (double) on && centre < (double) (on + ms300))
            {
                lowE += low;
                allE += total;
            }
        }

        f.low = allE > 0.0 ? lowE / allE : 0.0;
        f.brightness = centroidW > 0.0 ? centroidSum / centroidW : 0.0;

        // Einsaetze in der 10-ms-Huellkurve: Anstieg um 9 dB, 100 ms Pause
        int onsets = 0;
        {
            std::vector<double> e10;

            for (size_t i = 0; i + 10 <= env.size(); i += 10)
            {
                double s = 0.0;

                for (int k = 0; k < 10; ++k)
                    s += env[i + (size_t) k] * env[i + (size_t) k];

                e10.push_back (db (s / 10.0));
            }

            int lastOnset = -100;

            for (int i = 1; i < (int) e10.size(); ++i)
            {
                double before = e10[(size_t) i - 1];

                for (int k = 2; k <= 5 && i - k >= 0; ++k)
                    before = std::min (before, e10[(size_t) (i - k)]);

                if (e10[(size_t) i] - before > 9.0 && e10[(size_t) i] > peakFrameDb - 30.0 && i - lastOnset >= 10)
                {
                    ++onsets;
                    lastOnset = i;
                }
            }
        }

        f.oneShot = fileDuration <= 2.0 || onsets <= 1;

        // Tonhoehe und Vibrato (erste 10 s)
        if (n > (size_t) (0.3 * sr))
        {
            std::vector<float> head (x.begin(), x.begin() + (long) std::min (n, (size_t) (10.0 * sr)));
            auto f0 = pitchTrack (head, peakEnv * peakEnv);
            std::vector<double> voiced;
            int active = 0;

            for (auto v : f0)
            {
                active += v >= 0.0 ? 1 : 0;

                if (v > 0.0)
                    voiced.push_back (v);
            }

            if (! voiced.empty() && voiced.size() * 2 > (size_t) active)
                f.pitchHz = median (voiced);

            // Vibrato je zusammenhaengendem Ton (>= 0,8 s, keine Spruenge):
            // Abweichung vom gleitenden Mittel ueber 0,5 s, Rate aus der
            // Autokorrelation, Auslenkung = sqrt(2) * RMS
            double depthW = 0.0, rateW = 0.0, weight = 0.0;
            size_t i = 0;

            while (i < f0.size())
            {
                if (f0[i] <= 0.0)
                {
                    ++i;
                    continue;
                }

                size_t j = i + 1;

                while (j < f0.size() && f0[j] > 0.0 && std::abs (1200.0 * std::log2 (f0[j] / f0[j - 1])) < 100.0)
                    ++j;

                if (j - i >= 80)
                {
                    std::vector<double> c;

                    for (size_t k = i; k < j; ++k)
                        c.push_back (1200.0 * std::log2 (f0[k] / 440.0));

                    constexpr int halfWin = 25;
                    std::vector<double> r;

                    for (int k = halfWin; k + halfWin < (int) c.size(); ++k)
                    {
                        double m = 0.0;

                        for (int q = -halfWin; q < halfWin; ++q)
                            m += c[(size_t) (k + q)];

                        r.push_back (c[(size_t) k] - m / (2 * halfWin));
                    }

                    // Regelmaessig? Autokorrelation bei 3,5–9 Hz (Lag 11..28
                    // Frames) — springt die Messung zwischen den Stimmen
                    // eines Akkords, ist die Abweichung gross, aber nicht
                    // periodisch
                    double sq = 0.0, bestAc = 0.0;
                    int bestLag = 0;

                    for (auto v : r)
                        sq += v * v;

                    for (int lag = 11; lag <= 28 && lag * 2 < (int) r.size(); ++lag)
                    {
                        double s = 0.0, e1 = 0.0, e2 = 0.0;

                        for (size_t k = 0; k + (size_t) lag < r.size(); ++k)
                        {
                            s += r[k] * r[k + (size_t) lag];
                            e1 += r[k] * r[k];
                            e2 += r[k + (size_t) lag] * r[k + (size_t) lag];
                        }

                        double ac = e1 > 0.0 && e2 > 0.0 ? s / std::sqrt (e1 * e2) : 0.0;

                        if (ac > bestAc)
                        {
                            bestAc = ac;
                            bestLag = lag;
                        }
                    }

                    if (! r.empty() && bestAc >= 0.5)
                    {
                        double w = (double) r.size();
                        depthW += std::sqrt (2.0 * sq / r.size()) * w;
                        rateW += 100.0 / bestLag * w;
                        weight += w;
                    }
                }

                i = j;
            }

            if (weight > 0.0)
            {
                double rate = rateW / weight, depth = depthW / weight;

                if (rate >= 3.5 && rate <= 9.0 && depth >= 10.0)
                {
                    f.vibratoCents = depth;
                    f.vibratoHz = rate;
                }
            }
        }

        // Tempo nur fuer Loops mit deutlichem Puls
        if (! f.oneShot && fileDuration >= 4.0)
        {
            analysis::MonoAudio mono;
            mono.sampleRate = sr / 2.0;
            mono.samples.resize (n / 2);

            for (size_t k = 0; k < mono.samples.size(); ++k)
                mono.samples[k] = 0.5f * (x[2 * k] + x[2 * k + 1]);

            auto tempo = analysis::detectTempo (analysis::computeFeatures (mono));

            if (tempo.valid && tempo.confidence >= 0.3)
                f.bpm = tempo.bpm;
        }

        return f;
    }
}
