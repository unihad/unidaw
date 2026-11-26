#include "VoiceAnalysis.h"

#include "DrumAnalysis.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

namespace dawd::voice
{
    namespace
    {
        bool known (double v) { return ! std::isnan (v); }

        double median (std::vector<double> v)
        {
            v.erase (std::remove_if (v.begin(), v.end(), [] (double x) { return std::isnan (x); }), v.end());

            if (v.empty())
                return unknown;

            std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
            return v[v.size() / 2];
        }

        double percentile (std::vector<double> v, double q)
        {
            if (v.empty())
                return unknown;

            std::sort (v.begin(), v.end());
            return v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) std::round (q * (double) (v.size() - 1)))];
        }

        // Huellkurve: RMS in 10-ms-Fenstern alle 5 ms, dB, Mono aus L+R
        struct Envelope
        {
            std::vector<double> db;
            double fps = 200.0;

            double at (double t) const
            {
                if (db.empty())
                    return -120.0;

                return db[(size_t) juce::jlimit (0, (int) db.size() - 1, (int) std::round (t * fps))];
            }

            double maxIn (double a, double b) const
            {
                double m = -120.0;

                for (int i = juce::jmax (0, (int) std::floor (a * fps)); i <= (int) std::ceil (b * fps) && i < (int) db.size(); ++i)
                    m = juce::jmax (m, db[(size_t) i]);

                return m;
            }

            double minIn (double a, double b) const
            {
                double m = 120.0;

                for (int i = juce::jmax (0, (int) std::floor (a * fps)); i <= (int) std::ceil (b * fps) && i < (int) db.size(); ++i)
                    m = juce::jmin (m, db[(size_t) i]);

                return m > 100.0 ? -120.0 : m;
            }

            double meanIn (double a, double b) const
            {
                double s = 0.0;
                int n = 0;

                for (int i = juce::jmax (0, (int) std::floor (a * fps)); i <= (int) std::ceil (b * fps) && i < (int) db.size(); ++i)
                    s += db[(size_t) i], ++n;

                return n > 0 ? s / n : -120.0;
            }
        };

        std::vector<float> monoOf (const std::vector<std::vector<float>>& st)
        {
            if (st.empty())
                return {};

            std::vector<float> m (st[0].size());

            for (size_t i = 0; i < m.size(); ++i)
                m[i] = st.size() > 1 && i < st[1].size() ? 0.5f * (st[0][i] + st[1][i]) : st[0][i];

            return m;
        }

        Envelope envelopeOf (const std::vector<float>& x, double rate)
        {
            Envelope e;
            const int hop = juce::jmax (1, (int) std::round (rate / 200.0)), win = 2 * hop;
            e.fps = rate / hop;

            for (size_t s = 0; s + (size_t) win <= x.size() + (size_t) hop; s += (size_t) hop)
            {
                double sum = 0.0;
                int n = 0;

                for (size_t i = s; i < s + (size_t) win && i < x.size(); ++i)
                    sum += (double) x[i] * x[i], ++n;

                e.db.push_back (n > 0 ? 10.0 * std::log10 (sum / n + 1.0e-12) : -120.0);
            }

            return e;
        }

        // Scheinnoten in digitaler Stille (Praxistest 1i: "F2" in stillen
        // Takten): bricht ein Ton in die Stille ab, setzt basic-pitch dort
        // einen neuen Einsatz. Liegt der mittlere Pegel der Note (dB) mehr
        // als 60 dB unter dem lautesten Moment, klingt sie nicht
        template <typename N>
        std::vector<N> withoutSilent (const std::vector<N>& notes, const Envelope& env)
        {
            if (env.db.empty())
                return notes;

            const double floor = *std::max_element (env.db.begin(), env.db.end()) - 60.0;
            std::vector<N> kept;

            for (auto& n : notes)
                if (env.meanIn (n.start, n.end) >= floor)
                    kept.push_back (n);

            return kept;
        }

        // Sekunde -> Beat (stueckweise linear, an den Raendern verlaengert)
        double beatAt (const Grid& g, double t)
        {
            const auto& b = g.beats;

            if (b.size() < 2)
                return 0.0;

            if (t <= b.front())
                return (t - b[0]) / (b[1] - b[0]);

            if (t >= b.back())
                return (double) (b.size() - 1) + (t - b.back()) / (b.back() - b[b.size() - 2]);

            auto it = std::upper_bound (b.begin(), b.end(), t);
            auto i = (size_t) (it - b.begin());
            return (double) (i - 1) + (t - b[i - 1]) / (b[i] - b[i - 1]);
        }

        double timeAtBeat (const Grid& g, double beat)
        {
            const auto& b = g.beats;
            auto i = (int) std::floor (beat);

            if (i < 0)
                return b[0] + beat * (b[1] - b[0]);

            if (i >= (int) b.size() - 1)
                return b.back() + (beat - (double) (b.size() - 1)) * (b.back() - b[b.size() - 2]);

            return b[(size_t) i] + (beat - i) * (b[(size_t) i + 1] - b[(size_t) i]);
        }

        // Tonhoehe einer Note je Contour-Frame in Cent relativ zum Halbton
        // (gewichteter Schwerpunkt +-4 Bins); NaN wo die Contour schweigt
        std::vector<std::pair<double, double>> centsSeries (const transcription::Contour& c, const Note& n)
        {
            std::vector<std::pair<double, double>> out;
            // Bin der Note: drei je Halbton ab A0, gemessen um einen Bin
            // verschoben (reine Sinustoene lagen sonst 31 Cent zu hoch)
            const int centre = (n.pitch - transcription::Contour::lowestMidi) * transcription::Contour::binsPerSemitone + 1;

            if (centre < 0 || centre >= transcription::Contour::bins)
                return out;

            auto first = std::lower_bound (c.times.begin(), c.times.end(), n.start - 0.12);

            for (auto i = (size_t) (first - c.times.begin()); i < c.times.size() && c.times[i] <= n.end; ++i)
            {
                double sw = 0.0, sb = 0.0, peak = 0.0;

                for (int b = juce::jmax (0, centre - 4); b <= juce::jmin (transcription::Contour::bins - 1, centre + 4); ++b)
                {
                    auto v = (double) c.frames[i][(size_t) b];
                    peak = juce::jmax (peak, v);
                    auto w = juce::jmax (0.0, v - 0.1);
                    w *= w;
                    sw += w;
                    sb += w * b;
                }

                out.push_back ({ c.times[i], peak >= 0.3 && sw > 0.0 ? (sb / sw - centre) * 100.0 / transcription::Contour::binsPerSemitone
                                                                       : unknown });
            }

            return out;
        }

        void pitchShape (Note& n, const transcription::Contour& c)
        {
            auto series = centsSeries (c, n);
            std::vector<double> voiced, steady;

            for (auto& [t, v] : series)
                if (known (v))
                    voiced.push_back (v);

            const double len = n.end - n.start;

            for (auto& [t, v] : series)
                if (known (v) && t >= n.start + 0.3 * len && t <= n.start + 0.9 * len)
                    steady.push_back (v);

            voiced.clear();

            for (auto& [t, v] : series)
                if (known (v) && t >= n.start)
                    voiced.push_back (v);

            if (voiced.size() < 3)
                return;

            n.tuneCents = median (steady.empty() ? voiced : steady);

            // Glide: der Ton kommt von deutlich daneben und laeuft ueber
            // Zwischenwerte hinein (bis 120 ms vor dem erkannten Einsatz —
            // basic-pitch setzt ihn oft erst am Ende der Gleitbewegung). Ein
            // Tonschritt (Legato ohne Glide) springt ohne Zwischenwerte.
            double glideEnd = n.start;
            {
                // Ankunft: erster Frame ab 60 ms vor dem Einsatz, der nahe am Ziel bleibt
                size_t arrive = series.size();

                for (size_t i = 0; i < series.size(); ++i)
                    if (series[i].first >= n.start - 0.06 && known (series[i].second) && std::abs (series[i].second - n.tuneCents) < 25.0)
                    {
                        arrive = i;
                        break;
                    }

                if (arrive < series.size() && arrive > 0)
                {
                    // rueckwaerts: zusammenhaengende Frames, die sich vom Ziel entfernen
                    size_t from = arrive;
                    double farthest = 0.0;

                    for (size_t i = arrive; i-- > 0;)
                    {
                        auto v = series[i].second;

                        if (! known (v) || std::abs (v - n.tuneCents) < std::abs (farthest) - 15.0 || std::abs (v - n.tuneCents) > 140.0)
                            break;

                        if (std::abs (v - n.tuneCents) > std::abs (farthest))
                            farthest = v - n.tuneCents;

                        from = i;
                    }

                    // Beginn: der letzte Frame, der noch auf der Ausgangshoehe
                    // steht (bis 10 ct). Ein Halbton darunter liegt der vorige
                    // Ton im Fenster der Contour — sein gehaltener Teil gehoert
                    // nicht zum Glide (Todo 2026-10-27-describevoice-glidems-zu-lang:
                    // 4OSC 80 ms Portamento maß 141 ms)
                    const double held = series[from].second;

                    while (from + 1 < arrive && std::abs (series[from + 1].second - held) <= 10.0)
                        ++from;

                    int between = 0;
                    farthest = 0.0;

                    for (size_t i = from; i < arrive; ++i)
                    {
                        const double d = series[i].second - n.tuneCents;

                        if (std::abs (d) >= 25.0 && std::abs (d) <= 85.0)
                            ++between;

                        if (std::abs (d) > std::abs (farthest))
                            farthest = d;
                    }

                    const double ms = (series[arrive].first - series[from].first) * 1000.0;

                    if (std::abs (farthest) >= 40.0 && between >= 2 && ms >= 20.0 && ms <= 400.0)
                    {
                        // Gemessen ist nur ein Teil des Wegs (bis 25 ct vor
                        // dem Ziel, im Fenster +-140 ct): Tempo des Glides,
                        // glideMs auf den ganzen Weg hochgerechnet (Entscheidung
                        // Matthias 2026-10-29: bei linearem Portamento = Einstellung)
                        const double covered = std::abs (series[from].second - series[arrive].second);
                        n.glideCents = farthest;
                        n.glideCentsPerMs = covered / ms;
                        n.glideMs = covered > 0.0 ? std::abs (farthest) / n.glideCentsPerMs : ms;
                        n.glideFrom = series[from].first;
                        glideEnd = series[arrive].first;
                    }
                }
            }

            // Vibrato: periodisches Schwanken 3..9 Hz im gehaltenen Teil
            std::vector<double> sus;
            double fps = 0.0;

            for (size_t i = 0; i < series.size(); ++i)
                if (known (series[i].second) && series[i].first > glideEnd + 0.05)
                    sus.push_back (series[i].second);

            if (series.size() > 1)
                fps = 1.0 / ((series.back().first - series.front().first) / (double) (series.size() - 1));

            if (sus.size() >= 30 && fps > 20.0)
            {
                // Trend weg (gleitender Mittelwert ueber 250 ms)
                const int half = juce::jmax (2, (int) std::round (0.125 * fps));
                std::vector<double> d (sus.size());

                for (size_t i = 0; i < sus.size(); ++i)
                {
                    double s = 0.0;
                    int k = 0;

                    for (int j = (int) i - half; j <= (int) i + half; ++j)
                        if (j >= 0 && j < (int) sus.size())
                            s += sus[(size_t) j], ++k;

                    d[i] = sus[i] - s / k;
                }

                double var = 0.0;

                for (auto x : d)
                    var += x * x;

                var /= (double) d.size();
                // Erster deutlicher Gipfel der Autokorrelation (sonst greift
                // die doppelte Periode)
                std::vector<double> ac;
                const int lo = (int) std::floor (fps / 9.0), hi = juce::jmin ((int) std::ceil (fps / 3.0), (int) d.size() / 2);

                for (int lag = 0; lag <= hi + 1; ++lag)
                {
                    double r = 0.0;

                    for (size_t i = 0; i + (size_t) lag < d.size(); ++i)
                        r += d[i] * d[i + (size_t) lag];

                    ac.push_back (r / ((double) (d.size() - (size_t) lag) * juce::jmax (1.0e-9, var)));
                }

                double bestR = 0.0;
                int bestLag = 0;

                for (int lag = juce::jmax (1, lo); lag <= hi; ++lag)
                    bestR = juce::jmax (bestR, ac[(size_t) lag]);

                for (int lag = juce::jmax (1, lo); lag <= hi; ++lag)
                    if (ac[(size_t) lag] >= 0.85 * bestR && ac[(size_t) lag] >= ac[(size_t) lag - 1] && ac[(size_t) lag] >= ac[(size_t) lag + 1])
                    {
                        bestLag = lag;
                        bestR = ac[(size_t) lag];
                        break;
                    }

                const double depth = std::sqrt (var) * std::sqrt (2.0);

                if (bestR > 0.4 && depth > 8.0 && bestLag > 0)
                {
                    n.vibratoCents = depth;
                    n.vibratoHz = fps / bestLag;
                }
            }
        }

        // Fragmente gehaltener Toene: gleiche Tonhoehe, kurze Luecke und
        // kein neuer Anschlag in der Huellkurve -> eine Note
        std::vector<Note> mergeFragments (std::vector<Note> notes, const Envelope& env, int& merged,
                                          std::vector<std::pair<double, int>>& boundaries)
        {
            std::sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.pitch < b.pitch || (a.pitch == b.pitch && a.start < b.start); });
            std::vector<Note> out;

            for (auto& n : notes)
            {
                if (! out.empty() && out.back().pitch == n.pitch && n.start - out.back().end < 0.09 && n.start > out.back().start)
                {
                    const double before = env.meanIn (n.start - 0.12, n.start - 0.03);
                    const double low = env.minIn (n.start - 0.04, n.start + 0.02);
                    const double after = env.maxIn (n.start, n.start + 0.08);

                    if (before - low < 4.0 && after - low < 6.0)
                    {
                        out.back().end = juce::jmax (out.back().end, n.end);
                        out.back().amplitude = juce::jmax (out.back().amplitude, n.amplitude);
                        ++out.back().fragments;
                        ++merged;
                        boundaries.push_back ({ n.start, n.pitch });
                        continue;
                    }
                }

                out.push_back (n);
            }

            std::sort (out.begin(), out.end(), [] (const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
            return out;
        }
    }

    juce::String noteName (int midi)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    std::vector<transcription::Note> soundingNotes (const std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate)
    {
        return withoutSilent (notes, envelopeOf (mono, rate));
    }

    std::vector<transcription::Note> mergedFragments (const std::vector<transcription::Note>& notes, const std::vector<float>& mono,
                                                      double rate, int& merged)
    {
        std::vector<Note> vn;

        for (auto& r : notes)
        {
            Note n;
            n.start = r.start;
            n.end = r.end;
            n.pitch = r.pitch;
            n.amplitude = r.amplitude;
            vn.push_back (n);
        }

        std::vector<std::pair<double, int>> boundaries;
        std::vector<transcription::Note> out;

        for (auto& n : mergeFragments (vn, envelopeOf (mono, rate), merged, boundaries))
            out.push_back ({ n.start, n.end, n.pitch, n.amplitude });

        return out;
    }

    int joinRingSplits (std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate)
    {
        // Pegel (dB) am Grundton im Fenster [t0, t1): Goertzel mit Hann-Fenster
        auto levelAt = [&] (double freq, double t0, double t1)
        {
            const auto a = (juce::int64) juce::jmax (0.0, t0 * rate), b = juce::jmin ((juce::int64) mono.size(), (juce::int64) (t1 * rate));

            if (b - a < 16)
                return -200.0;

            const double w = 2.0 * juce::MathConstants<double>::pi * freq / rate, c = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0;

            for (auto i = a; i < b; ++i)
            {
                const double hann = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) (i - a) / (double) (b - a - 1));
                const double s0 = mono[(size_t) i] * hann + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            return 10.0 * std::log10 (juce::jmax (1.0e-20, s1 * s1 + s2 * s2 - c * s1 * s2));
        };

        std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.pitch < b.pitch || (a.pitch == b.pitch && a.start < b.start); });
        std::vector<transcription::Note> out;
        int joined = 0;
        const auto env = envelopeOf (mono, rate);

        // Beginn des Tons: eine Kette gleich hoher Noten ohne Luecke zaehlt
        // als ein Ton (der Nachklang ist selbst oft geteilt)
        std::vector<double> toneStart (notes.size());

        for (size_t i = 0; i < notes.size(); ++i)
            toneStart[i] = i > 0 && notes[i - 1].pitch == notes[i].pitch && notes[i].start - notes[i - 1].end < 0.04
                               ? toneStart[i - 1] : notes[i].start;

        for (auto& n : notes)
        {
            if (! out.empty() && out.back().pitch == n.pitch && n.start > out.back().start && n.start - out.back().end < 0.04)
            {
                // Ausloeser: eine andere Tonhoehe setzt zugleich ein (30 ms)
                // oder ein aelterer Ton, der schon vor dem ersten Teil klang,
                // endet dort (60 ms, das Ende schaetzt das Modell ungenauer;
                // abgedaempfte Saite). Das Fenster ist so lang, dass die
                // Nullstelle des Hann-Fensters (2 / Laenge) auf dem Nachbarn
                // liegt, sonst steigt beim Halbton daneben der Pegel nur
                // scheinbar (60 -> 59: +8 dB mit 50 ms, -1,6 dB mit 136 ms)
                const auto hz = [] (int p) { return 440.0 * std::pow (2.0, (p - 69) / 12.0); };
                const double f = hz (n.pitch);
                bool trigger = false;
                double len = 0.05;

                for (size_t j = 0; j < notes.size(); ++j)
                    if (const auto& m = notes[j]; m.pitch != n.pitch && (std::abs (m.start - n.start) <= 0.03
                                               || (toneStart[j] < out.back().start && std::abs (m.end - n.start) <= 0.06)))
                    {
                        trigger = true;
                        len = juce::jmax (len, juce::jmin (0.25, 2.0 / std::abs (f - hz (m.pitch))));
                    }

                // Ohne Ausloeser: mitten im Ausklang setzt das Modell einen
                // neuen Einsatz (langer Zupfton). Verbinden nur, wenn der
                // Gesamtpegel bis 150 ms danach nicht steigt — das Modell
                // setzt echte Anschlaege bis 80 ms zu frueh (Oktav-Bass
                // E1/E2: +5 dB, der Ausklang: -1,3 dB) — und ueber die
                // Grenze klar faellt (Ausklang 5 .. 7 dB in 200 ms; ein
                // gehaltener Ton mit Vibrato bleibt bei 0 dB und zerfaellt
                // weiter, das verbindet erst legato)
                if (! trigger)
                    trigger = env.maxIn (n.start - 0.02, n.start + 0.15) - env.meanIn (n.start - 0.12, n.start - 0.03) < 1.5
                              && env.meanIn (n.start - 0.25, n.start - 0.05) - env.meanIn (n.start + 0.05, n.start + 0.25) > 3.0;

                if (trigger && levelAt (f, n.start + 0.01, n.start + 0.01 + len) - levelAt (f, n.start - 0.01 - len, n.start - 0.01) < 3.0)
                {
                    out.back().end = juce::jmax (out.back().end, n.end);
                    ++joined;
                    continue;
                }
            }

            out.push_back (n);
        }

        std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
        notes = std::move (out);
        return joined;
    }

    int joinLegato (std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate, double overlap)
    {
        const auto env = envelopeOf (mono, rate);
        std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
        int joined = 0;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto& n = notes[i];
            size_t j = i + 1;

            while (j < notes.size() && notes[j].start <= n.start + 0.05)
                ++j;

            if (j == notes.size())
                continue;

            // Einsatz mit derselben Tonhoehe (auch als Akkordton): nur anstossen
            const double next = notes[j].start;
            bool samePitch = false;

            for (size_t k = j; k < notes.size() && notes[k].start <= next + 0.03; ++k)
                samePitch = samePitch || notes[k].pitch == n.pitch;

            const double target = samePitch ? next : next + overlap;

            if (n.end >= target)
                continue;

            if (n.end < next && (next - n.end > 0.5 || env.minIn (n.end, next) < env.maxIn (n.start, n.end) - 9.0))
                continue;

            n.end = target;
            ++joined;
        }

        return joined;
    }

    std::vector<int> onsetVelocities (const std::vector<transcription::Note>& notes, const std::vector<float>& mono, double rate)
    {
        const auto env = envelopeOf (mono, rate);
        std::vector<double> db;

        for (auto& n : notes)
        {
            int k = 0;

            for (auto& m : notes)
                if (std::abs (m.start - n.start) <= 0.03)
                    ++k;

            db.push_back (env.maxIn (n.start - 0.01, n.start + 0.08) - 10.0 * std::log10 ((double) juce::jmax (1, k)));
        }

        const double loudest = db.empty() ? 0.0 : *std::max_element (db.begin(), db.end());
        std::vector<int> out;

        for (auto d : db)
            out.push_back (juce::jlimit (20, 127, juce::roundToInt (100.0 * std::pow (10.0, (d - loudest) / 40.0))));

        return out;
    }

    std::vector<std::pair<double, double>> pitchBends (const transcription::Note& note, const transcription::Contour& contour, double& from)
    {
        Note n;
        n.start = note.start;
        n.end = note.end;
        n.pitch = note.pitch;
        from = n.start;
        pitchShape (n, contour);

        if (! known (n.tuneCents))
            return {};

        auto series = centsSeries (contour, n);

        // Beginn des Glides wie in describeVoice (pitchShape: ohne den
        // gehaltenen Teil des vorigen Tons)
        if (n.glideMs > 0.0)
            from = n.glideFrom;

        std::vector<std::pair<double, double>> curve;
        double lo = 1.0e9, hi = -1.0e9;

        for (auto& [t, c] : series)
            if (t >= from - 1.0e-6 && t <= n.end && known (c))
            {
                lo = juce::jmin (lo, c);
                hi = juce::jmax (hi, c);

                // ausgeduennt: ein Punkt je 5 Cent Aenderung
                if (curve.empty() || std::abs (c - curve.back().second) >= 5.0)
                    curve.push_back ({ t, c });
            }

        if (curve.empty() || (hi - lo < 15.0 && n.glideMs <= 0.0))
            return {};

        return curve;
    }

    namespace
    {
        // Schichten: Noten nach Merkmalen gruppiert (k-means k = 1..3,
        // Silhouette), Merkmale z-normiert: Lage, log Laenge, Einsatzhaerte
        // (Anstieg in 30 ms), Helligkeit (Schwerpunkt / Grundton am Anfang),
        // gehaltener Pegel (Ende gegen Anfang)
        std::vector<Layer> findLayers (const std::vector<Note>& notes, const Envelope& env, const std::vector<float>& mono,
                                       double rate, const Grid& grid, bool hasGrid)
        {
            std::vector<Layer> out;
            const size_t n = notes.size();

            if (n < 8)
                return out;

            std::vector<std::array<double, 5>> f (n);
            juce::dsp::FFT fft (11);
            std::vector<float> buf (4096);

            for (size_t i = 0; i < n; ++i)
            {
                auto& x = notes[i];
                const double dur = juce::jmax (0.02, x.end - x.start);
                const double rise = env.maxIn (x.start, x.start + 0.03) - env.minIn (x.start - 0.03, x.start);
                const double held = env.meanIn (x.start + 0.66 * dur, x.end) - env.maxIn (x.start, x.start + juce::jmin (0.1, dur));

                // Helligkeit: Schwerpunkt der ersten 46 ms relativ zum Grundton
                double bright = 0.0;
                {
                    std::fill (buf.begin(), buf.end(), 0.0f);
                    const auto s0 = (size_t) juce::jmax (0.0, x.start * rate);

                    for (int k = 0; k < 2048 && s0 + (size_t) k < mono.size(); ++k)
                        buf[(size_t) k] = mono[s0 + (size_t) k] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * k / 2047.0));

                    fft.performFrequencyOnlyForwardTransform (buf.data());
                    double sw = 0.0, sc = 0.0;

                    for (int b = 1; b < 1024; ++b)
                    {
                        const double pw = (double) buf[(size_t) b] * buf[(size_t) b];
                        sw += pw;
                        sc += pw * b * rate / 2048.0;
                    }

                    const double f0 = 440.0 * std::pow (2.0, (x.pitch - 69) / 12.0);
                    bright = sw > 0.0 ? std::log2 (juce::jmax (1.0, sc / sw / f0)) : 0.0;
                }

                f[i] = { (double) x.pitch / 12.0, std::log2 (dur), rise / 6.0, bright, held / 6.0 };
            }

            // z-normieren, auf +-2,5 begrenzen (eine ausklingende Note mit
            // -47 dB wuerde sonst zum Startpunkt einer Mini-Gruppe) und
            // gewichten: Einsatz und Haltepegel stammen aus dem Gemisch
            // aller Schichten und streuen, Lage/Laenge/Helligkeit tragen
            const std::array<double, 5> weight { 1.0, 1.0, 0.5, 1.0, 0.5 };

            for (int d = 0; d < 5; ++d)
            {
                double m = 0.0, sq = 0.0;

                for (auto& r : f) m += r[(size_t) d];
                m /= (double) n;
                for (auto& r : f) sq += (r[(size_t) d] - m) * (r[(size_t) d] - m);

                const double sd = std::sqrt (sq / (double) n);

                for (auto& r : f)
                    r[(size_t) d] = weight[(size_t) d] * (sd > 1.0e-9 ? juce::jlimit (-2.5, 2.5, (r[(size_t) d] - m) / sd) : 0.0);
            }

            auto dist = [] (const std::array<double, 5>& a, const std::array<double, 5>& b)
            {
                double s2 = 0.0;

                for (size_t d = 0; d < 5; ++d)
                    s2 += (a[d] - b[d]) * (a[d] - b[d]);

                return std::sqrt (s2);
            };

            double bestSil = 0.0;
            std::vector<int> best;

            for (int k = 2; k <= 3; ++k)
            {
                // k-means von zehn Startnoten aus (je weiter die am weitesten
                // entfernte dazu), das Ergebnis mit der kleinsten Streuung
                // ohne zu kleine Gruppe zaehlt
                std::vector<int> lab;
                double bestSse = 1.0e18;

                for (size_t s0 = 0; s0 < n; s0 += juce::jmax ((size_t) 1, n / 10))
                {
                    std::vector<std::array<double, 5>> c { f[s0] };

                    while ((int) c.size() < k)
                    {
                        size_t far = 0;
                        double fd = -1.0;

                        for (size_t i = 0; i < n; ++i)
                        {
                            double md = 1.0e9;

                            for (auto& cc : c)
                                md = juce::jmin (md, dist (f[i], cc));

                            if (md > fd)
                                fd = md, far = i;
                        }

                        c.push_back (f[far]);
                    }

                    std::vector<int> l (n, 0);

                    for (int it = 0; it < 50; ++it)
                    {
                        for (size_t i = 0; i < n; ++i)
                        {
                            int bi = 0;

                            for (int j = 1; j < k; ++j)
                                if (dist (f[i], c[(size_t) j]) < dist (f[i], c[(size_t) bi]))
                                    bi = j;

                            l[i] = bi;
                        }

                        for (int j = 0; j < k; ++j)
                        {
                            std::array<double, 5> m {};
                            int cnt = 0;

                            for (size_t i = 0; i < n; ++i)
                            {
                                if (l[i] != j)
                                    continue;

                                for (size_t d = 0; d < 5; ++d)
                                    m[d] += f[i][d];

                                ++cnt;
                            }

                            if (cnt > 0)
                                for (auto& v : m)
                                    v /= cnt;

                            c[(size_t) j] = m;
                        }
                    }

                    bool tiny = false;

                    for (int j = 0; j < k; ++j)
                        tiny = tiny || std::count (l.begin(), l.end(), j) < juce::jmax (3, (int) (0.15 * (double) n));

                    double sse = 0.0;

                    for (size_t i = 0; i < n; ++i)
                        sse += std::pow (dist (f[i], c[(size_t) l[i]]), 2.0);

                    if (! tiny && sse < bestSse)
                        bestSse = sse, lab = l;
                }

                // Silhouette (Mittel), jede Gruppe mindestens 15 % der Noten
                if (lab.empty())
                    continue;

                double sil = 0.0;

                for (size_t i = 0; i < n; ++i)
                {
                    std::vector<double> sum ((size_t) k, 0.0);
                    std::vector<int> cnt ((size_t) k, 0);

                    for (size_t q = 0; q < n; ++q)
                        if (q != i)
                            sum[(size_t) lab[q]] += dist (f[i], f[q]), ++cnt[(size_t) lab[q]];

                    const double a = cnt[(size_t) lab[i]] > 0 ? sum[(size_t) lab[i]] / cnt[(size_t) lab[i]] : 0.0;
                    double b = 1.0e9;

                    for (int j = 0; j < k; ++j)
                        if (j != lab[i] && cnt[(size_t) j] > 0)
                            b = juce::jmin (b, sum[(size_t) j] / cnt[(size_t) j]);

                    sil += (b - a) / juce::jmax (a, b);
                }

                sil /= (double) n;

                if (sil > bestSil)
                    bestSil = sil, best = lab;
            }

            // Nur klar getrennte Gruppen melden (ein Pad allein zerfaellt
            // sonst in gehaltene gemeinsame Akkordtoene und den Rest: 0,50)
            if (best.empty() || bestSil < 0.55)
                return out;

            const int k = *std::max_element (best.begin(), best.end()) + 1;

            for (int j = 0; j < k; ++j)
            {
                Layer L;
                std::vector<double> beats, helds;
                L.lowest = 127;

                for (size_t i = 0; i < n; ++i)
                {
                    if (best[i] != j)
                        continue;

                    auto& x = notes[i];
                    L.members.push_back (i);
                    ++L.notes;
                    L.lowest = juce::jmin (L.lowest, x.pitch);
                    L.highest = juce::jmax (L.highest, x.pitch);
                    const double dur = x.end - x.start;
                    beats.push_back (hasGrid ? beatAt (grid, x.end) - beatAt (grid, x.start) : dur * 2.0);
                    helds.push_back (env.meanIn (x.start + 0.66 * dur, x.end) - env.maxIn (x.start, x.start + juce::jmin (0.1, dur)));
                }

                L.medianBeats = median (beats);
                const double held = median (helds);
                L.role = L.medianBeats >= 1.5 ? juce::String ("held notes (pad/chords)")
                       : L.medianBeats < 0.6 && known (held) && held < -9.0 ? juce::String ("short decaying hits (pluck/stab)")
                       : L.lowest < 48 && L.highest < 55 ? juce::String ("low line (bass)")
                       : juce::String ("line/figure");
                out.push_back (L);
            }

            // Gleiche Rolle in zwei Gruppen: eher ein Instrument in zwei
            // Lagen als zwei Instrumente — dann keine Schichten
            for (size_t i = 0; i < out.size(); ++i)
                for (size_t j = i + 1; j < out.size(); ++j)
                    if (out[i].role == out[j].role)
                        return {};

            std::sort (out.begin(), out.end(), [] (const Layer& a, const Layer& b) { return a.notes > b.notes; });
            return out;
        }
    }

    namespace
    {
        // Betragsspektrum von len Abtastwerten ab i0 (Hann ueber len), auf
        // 2^order Punkte mit Nullen aufgefuellt
        std::vector<float> magnitudeOf (const std::vector<float>& x, size_t i0, size_t len, int order)
        {
            const int n = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> buf ((size_t) (2 * n), 0.0f);

            for (size_t k = 0; k < len && k < (size_t) n && i0 + k < x.size(); ++k)
                buf[k] = x[i0 + k] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) k / (double) (len - 1)));

            fft.performFrequencyOnlyForwardTransform (buf.data());
            buf.resize ((size_t) (n / 2 + 1));
            return buf;
        }

        // Verstimmung (Unison-Detune, Chorus, verstimmte Oszillatoren) in einem
        // Fenster [a, b) eines gehaltenen Tons — wie detune_cents in
        // tools/profile_plugin.py (Todo 2026-10-27-detune-glide-messung):
        // Spektrum auf ein 1-Cent-Raster von 600 Hz bis 2 kHz, Gipfel bis
        // 30 dB unter dem staerksten und mindestens 15 dB ueber dem
        // Rauschboden; je Gipfel die halbe Breite (Cent) der mittleren 80 %
        // der Energie in +-50 ct. Verstimmung verbreitert jeden Teilton um
        // gleich viele Cent, Pegelbewegung (Tremolo, Abklingen) nur um wenige
        // Hz. partials: Teiltoene aller Noten, die im Fenster klingen (Hz) —
        // liegen in einem Gipfel Teiltoene verschiedener Noten mehr als 4 ct
        // auseinander (Terzen im Akkord: 14–16 ct), zaehlt er nicht; Oktaven
        // und Quinten (0–2 ct) zaehlen. Leer ohne zwei klare Gipfel.
        std::vector<double> partialWidths (const std::vector<float>& x, double rate, double a, double b, const std::vector<double>& partials)
        {
            const auto i0 = (size_t) juce::jmax (0.0, a * rate);
            const auto i1 = juce::jmin (x.size(), (size_t) juce::jmax (0.0, b * rate));

            if (i1 <= i0 || (double) (i1 - i0) < rate / 2.0)
                return {};

            const auto len = i1 - i0;
            const int order = (int) std::ceil (std::log2 ((double) len * 4.0));
            const auto mag = magnitudeOf (x, i0, len, order);
            const double lo = 600.0, binHz = rate / (double) (1 << order);
            const int half = 50;
            std::vector<double> raw;

            for (size_t k = 1; k < mag.size(); ++k)
            {
                const double f = (double) k * binHz;

                if (f < lo * std::pow (2.0, -half / 1200.0))
                    continue;

                if (f >= 2000.0 * std::pow (2.0, half / 1200.0))
                    break;

                const auto c = (size_t) (std::round (1200.0 * std::log2 (f / lo)) + half);

                if (c >= raw.size())
                    raw.resize (c + 1, 0.0);

                raw[c] += (double) mag[k] * mag[k];
            }

            if (raw.size() < (size_t) (4 * half))
                return {};

            const double base = 2.0 * percentile (raw, 0.25);
            std::vector<double> grid (raw.size());
            double total = 0.0;

            for (size_t i = 0; i < raw.size(); ++i)
                total += grid[i] = juce::jmax (0.0, raw[i] - base);

            if (total <= 0.0)
                return {};

            // gleitende Summe ueber 41 ct
            auto boxed = [] (const std::vector<double>& v)
            {
                std::vector<double> s (v.size(), 0.0);

                for (size_t i = 0; i < v.size(); ++i)
                    for (size_t j = (size_t) juce::jmax (0, (int) i - 20); j <= i + 20 && j < v.size(); ++j)
                        s[i] += v[j];

                return s;
            };

            const auto sm = boxed (grid), rsm = boxed (raw);
            const double top = *std::max_element (sm.begin(), sm.end()), floor = percentile (rsm, 0.25);
            std::vector<double> widths;

            for (size_t i = (size_t) half; i + (size_t) half < sm.size(); ++i)
            {
                if (sm[i] < top * 1.0e-3 || rsm[i] < floor * std::pow (10.0, 1.5)
                    || sm[i] != *std::max_element (sm.begin() + (long) i - half, sm.begin() + (long) i + half + 1))
                    continue;

                // Teiltoene verschiedener Noten im Gipfel
                const double hz = lo * std::pow (2.0, ((double) i - half) / 1200.0);
                double cLo = 1.0e9, cHi = -1.0e9;

                for (auto p : partials)
                    if (const double c = 1200.0 * std::log2 (p / hz); std::abs (c) <= half + 5)
                        cLo = juce::jmin (cLo, c), cHi = juce::jmax (cHi, c);

                if (cHi - cLo > 4.0)
                    continue;

                double sum = 0.0;

                for (size_t j = i - (size_t) half; j <= i + (size_t) half; ++j)
                    sum += grid[j];

                double cum = 0.0;
                int from = -1, to = -1;

                for (size_t j = i - (size_t) half; j <= i + (size_t) half; ++j)
                {
                    cum += grid[j] / sum;

                    if (from < 0 && cum >= 0.1)
                        from = (int) j;

                    if (to < 0 && cum >= 0.9)
                        to = (int) j;
                }

                widths.push_back ((to - from) / 2.0);
            }

            return widths.size() >= 2 ? widths : std::vector<double>();
        }

        // Gleitet der Uebergang von Note a zu Note b (MIDI, tune: Stimmung in
        // Cent) zwischen den Zeiten from und to? Wie legato_glide in
        // tools/profile_plugin.py: 5-ms-Schritte, Tonhoehe je Schritt =
        // Maximum der Summe ueber 8 Harmonische (Fenster ~23 ms) auf einem
        // 10-Cent-Raster von der alten bis zur neuen Note. Gleiten = mindestens
        // 3 hoerbare Schritte am Stueck mindestens 80 ct von beiden Noten
        // entfernt dazwischen, stetig zur neuen Note (je Schritt 0–150 ct,
        // zusammen >= 50 ct) — ein Tonschritt springt, ein Gemisch aus alter
        // und neuer Note steht oder springt, Portamento laeuft durch.
        // Ergebnis: Gleitzeit in ms ueber den ganzen Schritt (hochgerechnet), 0 = gleitet nicht
        double legatoGlideMs (const std::vector<float>& x, double rate, int a, int b, double tune, double from, double to)
        {
            const double hop = 0.005;
            const auto win = (size_t) std::round (0.0232 * rate);
            const int order = (int) std::ceil (std::log2 ((double) win * 4.0));
            const double binsPerHz = (double) (1 << order) / rate;
            const double target = (b - a) * 100.0, sign = target > 0 ? 1.0 : -1.0;
            const double f0 = 440.0 * std::pow (2.0, (a - 69) / 12.0 + tune / 1200.0);
            std::vector<double> cents;

            for (double c = juce::jmin (0.0, target) - 50.0; c <= juce::jmax (0.0, target) + 50.0; c += 10.0)
                cents.push_back (c);

            std::vector<std::pair<double, double>> steps;   // Cent, Staerke

            for (double t = from; t < to; t += hop)
            {
                const long s = (long) std::round (t * rate) - (long) win / 2;

                if (s < 0 || (size_t) s + win > x.size())
                    continue;

                const auto mag = magnitudeOf (x, (size_t) s, win, order);
                double best = -1.0, bestC = 0.0;

                for (auto c : cents)
                {
                    double sal = 0.0;

                    for (int k = 1; k <= 8; ++k)
                    {
                        const double bin = f0 * std::pow (2.0, c / 1200.0) * k * binsPerHz;
                        const auto j = (size_t) bin;

                        if (j + 1 < mag.size())
                            sal += ((double) mag[j] + (bin - (double) j) * ((double) mag[j + 1] - mag[j])) / std::sqrt ((double) k);
                    }

                    if (sal > best)
                        best = sal, bestC = c;
                }

                steps.push_back ({ bestC, best });
            }

            double loud = 0.0;

            for (auto& [c, v] : steps)
                loud = juce::jmax (loud, v);

            std::vector<double> run;
            size_t bestLen = 0;
            double bestSpan = 0.0;

            // Abstand zu beiden Noten: 80 ct, beim Ganzton 60 ct — zwischen
            // 80 und 120 ct passt keine Strecke von 50 ct, Ganztonschritte
            // galten nie als Glide (4OSC legato 80 ms: F4-G4 und D4-E4 in
            // einer Linie ohne Glide, glideShare 0,71 statt 1)
            const double margin = juce::jmin (80.0, 0.3 * std::abs (target));

            for (auto& [c, v] : steps)
            {
                const bool inside = v > 0.1 * loud && std::abs (c) >= margin && std::abs (c - target) >= margin
                                    && c > juce::jmin (0.0, target) && c < juce::jmax (0.0, target);

                if (inside && ! run.empty() && (c - run.back()) * sign >= 0.0 && (c - run.back()) * sign <= 150.0)
                    run.push_back (c);
                else
                    run = inside ? std::vector<double> { c } : std::vector<double>();

                if (! run.empty())
                {
                    const double span = std::abs (run.back() - run.front());

                    if (run.size() > bestLen || (run.size() == bestLen && span > bestSpan))
                        bestLen = run.size(), bestSpan = span;
                }
            }

            // Die Strecke zaehlt nur margin ct von beiden Noten entfernt:
            // hochgerechnet auf den ganzen Schritt (bei linearem Portamento
            // die Einstellung; Entscheidung Matthias 2026-10-29)
            return bestLen >= 3 && bestSpan >= 50.0 ? (double) (bestLen - 1) * hop * 1000.0 * std::abs (target) / bestSpan : 0.0;
        }

        // Leistungsspektrum eines Fensters (Hann) mit Mitte bei centre
        std::vector<float> powerAt (const std::vector<float>& x, long centre, int order)
        {
            const int n = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> buf ((size_t) (2 * n), 0.0f);

            for (int k = 0; k < n; ++k)
            {
                const long j = centre - n / 2 + k;

                if (j >= 0 && j < (long) x.size())
                    buf[(size_t) k] = x[(size_t) j] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * k / (n - 1)));
            }

            fft.performFrequencyOnlyForwardTransform (buf.data());
            std::vector<float> p ((size_t) (n / 2 + 1));

            for (int b = 0; b <= n / 2; ++b)
                p[(size_t) b] = buf[(size_t) b] * buf[(size_t) b];

            return p;
        }

        juce::String pitchClasses (const std::array<double, 12>& c, int maxCount)
        {
            static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const double top = *std::max_element (c.begin(), c.end());
            std::vector<int> idx;

            if (top <= 0.0)
                return {};

            for (int i = 0; i < 12; ++i)
                if (c[(size_t) i] >= 0.35 * top)
                    idx.push_back (i);

            std::sort (idx.begin(), idx.end(), [&c] (int a, int b) { return c[(size_t) a] > c[(size_t) b]; });

            if ((int) idx.size() > maxCount)
                idx.resize ((size_t) maxCount);

            juce::StringArray out;

            for (int i : idx)
                out.add (names[i]);

            return out.joinIntoString (" ");
        }

        // Einsaetze (SuperFlux wie bei den Drums), je Einsatz: die neue
        // Energie (nach minus vor dem Einsatz, 150 Hz - 5 kHz), wie schnell
        // sie abklingt und welche Tonklassen sie traegt; Muster im 16tel-
        // Raster wie bei describeDrums
        Pulse findPulse (const std::vector<float>& x, double rate, const Grid& grid, bool hasGrid)
        {
            Pulse p;
            p.analysed = true;
            const auto ons = drums::onsets (x, rate);
            constexpr int order = 10, n1 = 1 << order, h1 = 128;
            const double fps = rate / h1, binHz = rate / n1;
            const int k0 = (int) std::ceil (150.0 / binHz), k1 = (int) (5000.0 / binHz);
            const int T = (int) (x.size() / h1) + 1;

            if (ons.size() < 4 || T < 16)
                return p;

            std::vector<std::vector<float>> S ((size_t) T);

            for (int t = 0; t < T; ++t)
                S[(size_t) t] = powerAt (x, (long) t * h1, order);

            struct Hit { double time, addedDb, decayMs; bool held; std::array<double, 12> chroma; int lowPitch; double energy; };
            std::vector<Hit> hits;
            const bool debug = std::getenv ("UNIDAW_PULSE_DEBUG") != nullptr;

            for (size_t i = 0; i < ons.size(); ++i)
            {
                const int f = (int) std::round (ons[i].time * fps);

                if (f < 9 || f + 6 >= T)
                    continue;

                std::vector<double> pre ((size_t) (k1 - k0 + 1)), add (pre.size());
                double A = 0.0, P = 0.0;

                for (int k = k0; k <= k1; ++k)
                {
                    double lo = 1.0e30, hi = 0.0;

                    for (int j = f - 8; j <= f - 3; ++j)
                        lo = juce::jmin (lo, (double) S[(size_t) j][(size_t) k]);

                    for (int j = f; j <= f + 4; ++j)
                        hi = juce::jmax (hi, (double) S[(size_t) j][(size_t) k]);

                    pre[(size_t) (k - k0)] = lo;
                    add[(size_t) (k - k0)] = juce::jmax (0.0, hi - lo);
                    A += add[(size_t) (k - k0)];
                    P += hi;
                }

                if (A <= 0.0 || P <= 0.0)
                    continue;

                Hit h {};
                h.time = ons[i].time;
                h.energy = A;
                h.addedDb = 10.0 * std::log10 (A / P);

                // Abklingen der neuen Energie bis zum naechsten Einsatz (max. 0,6 s)
                const double tNext = i + 1 < ons.size() ? ons[i + 1].time : ons[i].time + 0.6;
                const int fEnd = juce::jmin (T - 1, (int) std::round (juce::jmin (tNext, ons[i].time + 0.6) * fps) - 1);
                auto excess = [&] (int j)
                {
                    double e = 0.0;

                    for (int k = k0; k <= k1; ++k)
                        e += add[(size_t) (k - k0)] * juce::jmax (0.0, (double) S[(size_t) j][(size_t) k] - pre[(size_t) (k - k0)]);

                    return e / A;
                };

                double peak = 0.0;
                int peakAt = f;

                for (int j = f; j <= juce::jmin (fEnd, f + 4); ++j)
                    if (const double e = excess (j); e > peak)
                        peak = e, peakAt = j;

                h.held = true;
                h.decayMs = (fEnd - f) * 1000.0 / fps;

                for (int j = peakAt + 1; j <= fEnd; ++j)
                    if (excess (j) < peak / 8.0)
                    {
                        h.held = false;
                        h.decayMs = (j - f) * 1000.0 / fps;
                        break;
                    }

                // Tonklassen: Teiltoene der neuen Energie (4096er-Fenster
                // vor/nach dem Einsatz, Gipfel mit Parabel)
                {
                    const auto a = powerAt (x, (long) ((h.time + 0.05) * rate), 12);
                    const auto b = powerAt (x, (long) ((h.time - 0.06) * rate), 12);
                    const double bin = rate / 4096.0;
                    std::vector<double> d (a.size());

                    for (size_t k = 0; k < a.size(); ++k)
                        d[k] = juce::jmax (0.0, (double) a[k] - (double) b[k]);

                    const int lo = (int) (100.0 / bin), hi = (int) (2500.0 / bin);
                    double top = 0.0;

                    for (int k = lo; k <= hi; ++k)
                        top = juce::jmax (top, d[(size_t) k]);

                    h.lowPitch = 0;

                    for (int k = lo; k <= hi && top > 0.0; ++k)
                    {
                        if (d[(size_t) k] < 0.01 * top || d[(size_t) k] < d[(size_t) k - 1] || d[(size_t) k] < d[(size_t) k + 1])
                            continue;

                        const double y0 = std::log (d[(size_t) k - 1] + 1.0e-20), y1 = std::log (d[(size_t) k] + 1.0e-20), y2 = std::log (d[(size_t) k + 1] + 1.0e-20);
                        const double den = y0 - 2.0 * y1 + y2;
                        const double fr = (k + (den != 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den) : 0.0)) * bin;
                        const int midi = (int) std::lround (69.0 + 12.0 * std::log2 (fr / 440.0));
                        h.chroma[(size_t) (((midi % 12) + 12) % 12)] += std::sqrt (d[(size_t) k]);

                        if (h.lowPitch == 0 && d[(size_t) k] >= 0.3 * top)
                            h.lowPitch = midi;
                    }
                }

                if (debug)
                    std::fprintf (stderr, "pulse t %.3f strength %.2f added %.1f dB decay %.0f ms%s low %d\n", h.time, ons[i].strength, h.addedDb, h.decayMs, h.held ? " held" : "", h.lowPitch);

                // Nur Einsaetze mit deutlichem Zuwachs (Chorus und
                // Schwebung eines gehaltenen Pads bleiben darunter)
                if (h.addedDb >= -10.0)
                    hits.push_back (h);
            }

            // Nur Einsaetze im 16tel-Raster zaehlen: Schwebung und Chorus
            // eines breiten Pads erzeugen ebenso viele Einsaetze, aber mit
            // zufaelliger Lage (Praxistest 1e: 262 Einsaetze, 47 % im Raster
            // = Zufall; echte Pulse: 100 %). Ohne Raster keine Aussage.
            if (! hasGrid)
                return p;

            const int bpb = juce::jmax (1, grid.beatsPerBar);
            const int slots = juce::jmin (32, bpb * 4);
            const double b0 = beatAt (grid, grid.downbeats.front());
            const double slotMs = 1000.0 * (grid.beats.back() - grid.beats.front()) / juce::jmax (1.0, (double) grid.beats.size() - 1.0) * bpb / slots;
            const double tolMs = juce::jmax (20.0, 0.18 * slotMs);
            const size_t found = hits.size();
            hits.erase (std::remove_if (hits.begin(), hits.end(), [&] (const Hit& h)
            {
                const double rel = (beatAt (grid, h.time) - b0) * slots / bpb;
                return std::abs (rel - std::round (rel)) * slotMs > tolMs;
            }), hits.end());

            // Ueberschuss gegen den Zufall: bei zufaelliger Lage traefe der
            // Anteil 2 tol / slot das Raster
            const double chance = juce::jmin (0.9, 2.0 * tolMs / slotMs);
            p.lockedShare = found > 0 ? (double) hits.size() / (double) found : 0.0;
            p.excess = found > 0 ? juce::jmax (0.0, (p.lockedShare - chance) / (1.0 - chance)) : 0.0;
            p.onsets = (int) hits.size();

            if (hits.size() < 4)
                return p;

            std::vector<double> decays, addeds, lows;
            std::array<double, 12> all {};
            int held = 0;

            for (auto& h : hits)
            {
                if (h.held)
                    ++held;
                else
                    decays.push_back (h.decayMs);

                addeds.push_back (h.addedDb);

                if (h.lowPitch > 0)
                    lows.push_back (h.lowPitch);

                for (size_t c = 0; c < 12; ++c)
                    all[c] += h.chroma[c];
            }

            p.decayMs = median (decays);
            p.heldShare = (double) held / (double) hits.size();
            p.addedDb = median (addeds);
            p.chord = pitchClasses (all, 4);

            if (! lows.empty())
            {
                std::sort (lows.begin(), lows.end());
                p.lowest = (int) lows[(size_t) (0.1 * (double) (lows.size() - 1))];
                p.highest = (int) lows[(size_t) (0.9 * (double) (lows.size() - 1))];
            }

            const double maxE = std::max_element (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.energy < b.energy; })->energy;
            std::map<int, PulseBar> bars;
            std::map<int, std::array<double, 12>> barChroma;
            int off = 0;

            for (auto& h : hits)
            {
                const double rel = beatAt (grid, h.time) - b0;
                const int bar = (int) std::floor ((rel + 0.125) / bpb);
                const int slot = juce::jlimit (0, slots - 1, (int) std::llround ((rel - bar * bpb) * slots / bpb));
                auto& pb = bars[bar];

                if (pb.pattern.isEmpty())
                {
                    pb.bar = bar + 1;
                    pb.pattern = juce::String::repeatedString (".", slots);
                }

                const double db = 10.0 * std::log10 (h.energy / maxE);
                const char ch = db >= -6.0 ? 'X' : db >= -18.0 ? 'x' : 'g';
                const auto old = pb.pattern[slot];

                if (old == '.' || (old == 'g' && ch != 'g') || (old == 'x' && ch == 'X'))
                    pb.pattern = pb.pattern.substring (0, slot) + juce::String::charToString ((juce::juce_wchar) ch) + pb.pattern.substring (slot + 1);

                ++pb.onsets;
                off += (slot % 2) == 1 ? 1 : 0;

                for (size_t c = 0; c < 12; ++c)
                    barChroma[bar][c] += h.chroma[c];
            }

            std::vector<double> counts;

            for (auto& [b, pb] : bars)
            {
                pb.chord = pitchClasses (barChroma[b], 3);
                p.bars.push_back (pb);
                counts.push_back ((double) pb.onsets);
            }

            p.perBar = median (counts);
            p.offbeatShare = (double) off / (double) hits.size();
            p.grid = p.offbeatShare >= 0.2 ? "16ths" : p.perBar >= 5.0 ? "8ths" : "quarters";
            return p;
        }
    }

    namespace
    {
        // Hoechster Abstand zweier Einsaetze desselben Anschlags (Strum): die
        // Saiten liegen 30-40 ms auseinander, basic-pitch setzt sie bis zu
        // 80 ms auseinander (Raster der Frames). Hoechstens 0,7 einer 16tel
        // (schnelle 16tel bleiben getrennt), ohne Raster 50 ms
        double strumGap (const Grid& grid, bool hasGrid)
        {
            if (! hasGrid || grid.beats.size() < 2)
                return 0.05;

            const double slot = (grid.beats.back() - grid.beats.front()) / (double) (grid.beats.size() - 1) / 4.0;
            return juce::jlimit (0.05, 0.1, 0.7 * slot);
        }

        // Akkordanschlaege: Einsaetze von mindestens zwei Tonklassen in
        // 50 ms. Roh (vor dem Verschmelzen, Obertoene 12/19/24 ueber einem
        // gleichzeitigen, lauteren Ton weg) gegen die verschmolzenen Noten.
        ChordHits findChordHits (const std::vector<transcription::Note>& rawIn, const std::vector<Note>& merged,
                                 const Grid& grid, bool hasGrid)
        {
            ChordHits ch;
            ch.analysed = true;
            std::vector<std::pair<double, int>> raw;

            for (size_t i = 0; i < rawIn.size(); ++i)
            {
                bool ghost = false;

                for (size_t j = 0; j < rawIn.size() && ! ghost; ++j)
                {
                    const int iv = rawIn[i].pitch - rawIn[j].pitch;
                    ghost = (iv == 12 || iv == 19 || iv == 24) && std::abs (rawIn[i].start - rawIn[j].start) < 0.05
                            && rawIn[i].amplitude < rawIn[j].amplitude;
                }

                if (! ghost)
                    raw.push_back ({ rawIn[i].start, rawIn[i].pitch });
            }

            const double gap = strumGap (grid, hasGrid);

            auto hits = [gap] (std::vector<std::pair<double, int>> ev)
            {
                std::sort (ev.begin(), ev.end());
                std::vector<std::pair<double, std::set<int>>> out;

                for (size_t i = 0; i < ev.size();)
                {
                    size_t j = i;
                    std::set<int> pcs { ev[i].second % 12 };
                    std::set<int> pitches { ev[i].second };

                    // verkettet: ein Strum ist ein Anschlag, nicht zwei — sonst
                    // fehlte die erste Saite allein (eine Tonklasse) und der
                    // Anschlag rutschte eine 16tel spaeter (Praxistest 1l/1m)
                    while (j + 1 < ev.size() && ev[j + 1].first - ev[j].first < gap && ev[j + 1].first - ev[i].first < 0.15)
                        ++j, pcs.insert (ev[j].second % 12), pitches.insert (ev[j].second);

                    if (pcs.size() >= 2)
                        out.push_back ({ ev[i].first, pitches });

                    i = j + 1;
                }

                return out;
            };

            std::vector<std::pair<double, int>> mer;

            for (auto& n : merged)
                mer.push_back ({ n.start, n.pitch });

            const auto hr = hits (raw), hm = hits (mer);
            ch.raw = (int) hr.size();

            if (! hasGrid || hr.empty())
                return ch;

            const int bpb = juce::jmax (1, grid.beatsPerBar);
            const int slots = juce::jmin (32, bpb * 4);
            const double b0 = beatAt (grid, grid.downbeats.front());
            std::map<int, PulseBar> bars;
            std::map<int, std::array<double, 12>> tones;

            for (auto& [t, pitches] : hr)
            {
                bool seen = false;

                for (auto& m : hm)
                    seen = seen || std::abs (m.first - t) < 0.05;

                if (! seen)
                    ++ch.hidden;

                const double rel = beatAt (grid, t) - b0;
                const int bar = (int) std::floor ((rel + 0.125) / bpb);
                const int slot = juce::jlimit (0, slots - 1, (int) std::llround ((rel - bar * bpb) * slots / bpb));
                auto& pb = bars[bar];

                if (pb.pattern.isEmpty())
                {
                    pb.bar = bar + 1;
                    pb.pattern = juce::String::repeatedString (".", slots);
                }

                const char c = seen ? 'x' : 'X';

                if (pb.pattern[slot] != 'X')
                    pb.pattern = pb.pattern.substring (0, slot) + juce::String::charToString ((juce::juce_wchar) c) + pb.pattern.substring (slot + 1);

                ++pb.onsets;

                if (! seen)
                {
                    juce::StringArray chord;

                    for (auto p : pitches)
                    {
                        tones[bar][(size_t) (p % 12)] += 1.0;
                        chord.add (noteName (p));
                        ch.lowest = ch.lowest == 0 ? p : juce::jmin (ch.lowest, p);
                        ch.highest = juce::jmax (ch.highest, p);
                    }

                    // Lage und Voicing der Anschlaege (hoechstens acht je Takt)
                    if (pb.notes.isEmpty() || pb.notes.retainCharacters (" ").length() < 7)
                        pb.notes << (pb.notes.isEmpty() ? "" : " ") << chord.joinIntoString ("+");
                }
            }

            std::vector<double> counts;

            for (auto& [b, pb] : bars)
            {
                pb.chord = pitchClasses (tones[b], 4);
                ch.bars.push_back (pb);
                counts.push_back ((double) pb.onsets);
            }

            const double nBars = juce::jmax (1.0, (double) bars.size());
            ch.perBar = (double) ch.raw / nBars;
            ch.hiddenPerBar = (double) ch.hidden / nBars;
            ch.layer = ch.hidden >= 6 && ch.hiddenPerBar >= 1.5 && ch.raw >= 1.5 * juce::jmax (1, (int) hm.size());
            return ch;
        }
    }

    namespace
    {
        // Verdeckte Neuanschlaege: Stellen mitten in einer klingenden Note —
        // verschmolzene Fragmente gleicher Tonhoehe, ein Oberton (Oktave,
        // Duodezime, zwei Oktaven), der in der Note neu einsetzt (basic-pitch
        // hoert den Anschlag dort statt im Grundton), und Einsaetze im Signal
        // (SuperFlux), an denen keine Note beginnt. In einer einstimmigen
        // Linie mit vielen solchen Stellen im 16tel-Raster wird die Note dort
        // geteilt (Tonwiederholungen, Praxistest 1g/1i/1m: Bass 13 statt 48
        // Noten); sonst nur gezaehlt und beurteilt (Praxistest 1h: Tremolo
        // oder ein zweites Instrument?).
        ReAttacks findReAttacks (std::vector<Note>& notes, const std::vector<std::pair<double, int>>& mergedAt, const Envelope& env,
                                 const std::vector<float>& mono, double rate, const Grid& grid, bool hasGrid)
        {
            ReAttacks ra;
            const size_t n = notes.size();

            if (n == 0)
                return ra;

            auto inside = [] (const Note& m, double t) { return t > m.start + 0.06 && t < m.end - 0.06; };

            // Obertoene, die in einer tieferen Note neu einsetzen und in ihr enden
            std::vector<bool> ghost (n, false);
            struct Cand { double t; size_t note; bool hidden; };   // hidden: keine Note setzt dort ein
            std::vector<Cand> cand;

            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                {
                    const int iv = notes[i].pitch - notes[j].pitch;

                    if ((iv == 12 || iv == 19 || iv == 24) && inside (notes[j], notes[i].start) && notes[i].end <= notes[j].end + 0.08)
                    {
                        ghost[i] = true;
                        cand.push_back ({ notes[i].start, j, true });
                        break;
                    }
                }

            // Einstimmig: hoechstens 20 % der klingenden Zeit mehr als eine
            // Note (die Obertoene oben nicht mitgezaehlt)
            bool line = false;
            {
                double t0 = 1.0e9, t1 = 0.0;
                int sounding = 0, poly = 0;

                for (size_t i = 0; i < n; ++i)
                    if (! ghost[i])
                        t0 = juce::jmin (t0, notes[i].start), t1 = juce::jmax (t1, notes[i].end);

                for (double t = t0; t < t1; t += 0.02)
                {
                    int c = 0;

                    for (size_t i = 0; i < n; ++i)
                        c += ! ghost[i] && notes[i].start <= t && notes[i].end > t ? 1 : 0;

                    sounding += c > 0 ? 1 : 0;
                    poly += c > 1 ? 1 : 0;
                }

                line = sounding > 0 && poly <= 0.2 * sounding;
            }

            auto container = [&] (double t, int pitch) -> int
            {
                for (size_t i = 0; i < n; ++i)
                    if (! ghost[i] && inside (notes[i], t) && (pitch < 0 || notes[i].pitch == pitch))
                        return (int) i;

                return -1;
            };

            auto startsNear = [&] (double t)
            {
                for (size_t i = 0; i < n; ++i)
                    if (! ghost[i] && std::abs (notes[i].start - t) < 0.06)
                        return true;

                return false;
            };

            for (auto& [t, p] : mergedAt)
                if (const int c = container (t, p); c >= 0)
                    cand.push_back ({ t, (size_t) c, true });

            // Einsaetze in klingenden Noten: fuer das Urteil alle (Tremolo
            // zerlegt auch die Noten), gezaehlt und geteilt nur die verdeckten
            for (auto& o : drums::onsets (mono, rate))
                if (const int c = container (o.time, -1); c >= 0)
                    cand.push_back ({ o.time, (size_t) c, ! startsNear (o.time) });

            // je Stelle einmal (60 ms), nur mit hoerbarem Anstieg: Leistung
            // 30 ms danach mindestens 2 dB ueber 40 ms davor — gemittelt, die
            // 10-ms-Huellkurve eines tiefen Tons schwankt mit der Periode, und
            // SuperFlux feuert dort auch ohne Anschlag
            auto power = [&env] (double a, double b)
            {
                double p = 0.0;
                int k = 0;

                for (int i = juce::jmax (0, (int) std::floor (a * env.fps)); i <= (int) std::ceil (b * env.fps) && i < (int) env.db.size(); ++i)
                    p += std::pow (10.0, env.db[(size_t) i] / 10.0), ++k;

                return k > 0 ? 10.0 * std::log10 (p / k + 1.0e-12) : -120.0;
            };

            auto rise = [&power] (double t)
            {
                double after = -120.0;

                for (double d = -0.01; d <= 0.03; d += 0.005)
                    after = juce::jmax (after, power (t + d, t + d + 0.03));

                return after - power (t - 0.05, t - 0.01);
            };

            std::sort (cand.begin(), cand.end(), [] (const Cand& a, const Cand& b) { return a.t < b.t; });
            std::vector<Cand> at;

            for (auto& c : cand)
            {
                if (! at.empty() && c.t - at.back().t < 0.06)
                    at.back().hidden = at.back().hidden || c.hidden;
                else if (rise (c.t) >= 2.0)
                    at.push_back (c);
            }

            ra.count = (int) std::count_if (at.begin(), at.end(), [] (const Cand& c) { return c.hidden; });

            if (at.size() < 4 || ra.count == 0)
                return ra;

            ra.lowest = 127;

            for (auto& c : at)
                if (c.hidden)
                    ra.lowest = juce::jmin (ra.lowest, notes[c.note].pitch), ra.highest = juce::jmax (ra.highest, notes[c.note].pitch);

            // Abstaende (unter 1 s) und ihre Streuung
            {
                std::vector<double> iv;

                for (size_t k = 1; k < at.size(); ++k)
                    if (at[k].t - at[k - 1].t < 1.0)
                        iv.push_back (at[k].t - at[k - 1].t);

                if (iv.size() >= 3)
                {
                    const double m = median (iv);
                    std::vector<double> dev;

                    for (auto x : iv)
                        dev.push_back (std::abs (x - m));

                    ra.intervalMs = 1000.0 * m;
                    ra.regularity = median (dev) / juce::jmax (1.0e-6, m);
                }
            }

            std::vector<bool> onGrid (at.size(), false);

            if (hasGrid)
            {
                const int bpb = juce::jmax (1, grid.beatsPerBar);
                const double b0 = beatAt (grid, grid.downbeats.front());
                const double slotMs = 1000.0 * (grid.beats.back() - grid.beats.front()) / juce::jmax (1.0, (double) grid.beats.size() - 1.0) / 4.0;
                const double tolMs = juce::jmax (20.0, 0.18 * slotMs);
                int hits = 0;

                for (size_t k = 0; k < at.size(); ++k)
                {
                    const double rel = (beatAt (grid, at[k].t) - b0) * 4.0;
                    onGrid[k] = std::abs (rel - std::round (rel)) * slotMs <= tolMs;
                    hits += onGrid[k] ? 1 : 0;
                }

                const double chance = juce::jmin (0.9, 2.0 * tolMs / slotMs);
                ra.onGrid = (double) hits / (double) at.size();
                ra.excess = juce::jmax (0.0, (ra.onGrid - chance) / (1.0 - chance));
                const int firstBar = (int) std::floor ((beatAt (grid, notes.front().start) - b0 + 0.125) / bpb);
                const int lastBar = (int) std::floor ((beatAt (grid, notes.back().start) - b0 + 0.125) / bpb);
                ra.perBar = (double) ra.count / (double) juce::jmax (1, lastBar - firstBar + 1);
            }

            // Urteil erst ab sechs verdeckten Neuanschlaegen, 1,5 je Takt (ein
            // Pad allein hat ein paar zufaellige); in einer Linie ab vier
            if (line ? ra.count < 4 : ra.count < 6 || (known (ra.perBar) && ra.perBar < 1.5))
                return ra;

            ra.verdict = known (ra.excess) && ra.excess >= 0.5 ? "played"
                       : known (ra.regularity) && ra.regularity <= 0.2 ? "periodic"
                       : "irregular";

            // Tonwiederholungen einer Linie: an den Stellen im Raster teilen
            int hiddenOnGrid = 0;

            for (size_t k = 0; k < at.size(); ++k)
                hiddenOnGrid += onGrid[k] && at[k].hidden ? 1 : 0;

            if (line && ra.verdict == "played" && hiddenOnGrid >= 4)
            {
                std::vector<Note> added;
                std::vector<double> cuts;
                std::set<double> used;

                for (size_t k = 0; k < at.size(); ++k)
                    if (onGrid[k] && at[k].hidden)
                        cuts.push_back (at[k].t);

                for (size_t i = 0; i < n; ++i)
                {
                    if (ghost[i])
                        continue;

                    std::vector<double> mine;

                    for (auto t : cuts)
                        if (inside (notes[i], t))
                            mine.push_back (t), used.insert (t);

                    for (auto t : mine)
                    {
                        Note a = notes[i];
                        a.start = t;
                        a.fragments = 1;
                        added.push_back (a);
                    }

                    // Enden: jedes Stueck bis zum naechsten Schnitt
                    for (size_t k = 0; k < mine.size(); ++k)
                        added[added.size() - mine.size() + k].end = k + 1 < mine.size() ? mine[k + 1] : notes[i].end;

                    if (! mine.empty())
                        notes[i].end = mine.front();
                }

                ra.split = (int) used.size();

                // die Obertoene an den Schnitten waren der Anschlag
                std::vector<Note> kept;

                for (size_t i = 0; i < n; ++i)
                    if (! ghost[i] || std::none_of (cuts.begin(), cuts.end(), [&] (double t) { return std::abs (t - notes[i].start) < 0.06; }))
                        kept.push_back (notes[i]);

                kept.insert (kept.end(), added.begin(), added.end());
                std::sort (kept.begin(), kept.end(), [] (const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
                notes = kept;
            }

            return ra;
        }
    }

    Voice describeVoice (const std::vector<transcription::Note>& raw, const transcription::Contour& contour,
                         const std::vector<std::vector<float>>& stereo, double rate, const Grid& grid)
    {
        Voice v;
        auto env = envelopeOf (monoOf (stereo), rate);
        std::vector<Note> notes;

        for (auto& r : raw)
        {
            Note n;
            n.start = r.start;
            n.end = r.end;
            n.pitch = r.pitch;
            n.amplitude = r.amplitude;
            notes.push_back (n);
        }

        std::vector<std::pair<double, int>> mergedAt;
        notes = mergeFragments (notes, env, v.fragmentsMerged, mergedAt);

        // Obertoene als Geister (wie audioToMidi), nach dem Verschmelzen —
        // Vibrato zerlegt den Grundton sonst in Stuecke, die den Oberton
        // einzeln nicht ueberdecken
        {
            std::vector<Note> kept;

            for (size_t i = 0; i < notes.size(); ++i)
            {
                bool ghost = false;

                for (size_t j = 0; j < notes.size(); ++j)
                {
                    auto iv = notes[i].pitch - notes[j].pitch;

                    // Obertoene 2..4 (Oktave, Duodezime, 2 Oktaven) deutlich
                    // leiser; 5..8 (zwei Oktaven + Terz, Quinte, kl. Septime,
                    // drei Oktaven) leiser und mit dem Grundton eingesetzt —
                    // so liegt kaum je eine echte Note
                    const bool low = iv == 12 || iv == 19 || iv == 24;
                    const bool high = iv == 28 || iv == 31 || iv == 34 || iv == 36;
                    const bool overlap = juce::jmin (notes[i].end, notes[j].end) - juce::jmax (notes[i].start, notes[j].start) > 0.7 * (notes[i].end - notes[i].start);

                    if (overlap && ((low && notes[i].amplitude < 0.6 * notes[j].amplitude)
                                    || (high && std::abs (notes[i].start - notes[j].start) < 0.15)))
                        ghost = true;
                }

                if (! ghost)
                    kept.push_back (notes[i]);
            }

            notes = kept;
        }

        // Scheinnoten in digitaler Stille
        notes = withoutSilent (notes, env);

        if (notes.empty())
            return v;

        v.hasContour = ! contour.frames.empty();
        v.hasGrid = grid.valid && grid.beats.size() >= 2 && ! grid.downbeats.empty();

        // Verdeckte Neuanschlaege; Tonwiederholungen einer Linie als Noten
        v.reattacks = findReAttacks (notes, mergedAt, env, monoOf (stereo), rate, grid, v.hasGrid);

        // Anschlag, Schwellen, Tonhoehenverlauf je Note
        double loudest = -120.0;

        for (auto& n : notes)
        {
            n.velDb = env.maxIn (n.start - 0.01, n.start + 0.08);
            loudest = juce::jmax (loudest, n.velDb);

            if (n.end - n.start >= 0.3)
                n.swellDb = env.maxIn (n.start + 0.1, juce::jmin (n.end, n.start + 1.0)) - n.velDb;

            if (v.hasContour)
                pitchShape (n, contour);
        }

        std::vector<double> vel, swell, legato, glideMs, vibC, vibHz, tune;
        int vibratos = 0, pitched = 0;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto& n = notes[i];
            n.velDb -= loudest;
            vel.push_back (n.velDb);
            swell.push_back (n.swellDb);

            // Legato: Laenge relativ zum naechsten Einsatz (irgendeiner Stimme)
            for (size_t j = i + 1; j < notes.size(); ++j)
                if (notes[j].start > n.start + 0.05)
                {
                    legato.push_back (juce::jmin (1.0, (n.end - n.start) / (notes[j].start - n.start)));
                    break;
                }

            if (known (n.tuneCents))
            {
                ++pitched;
                tune.push_back (n.tuneCents);

                if (n.vibratoHz > 0.0)
                    ++vibratos, vibC.push_back (n.vibratoCents), vibHz.push_back (n.vibratoHz);
            }
        }

        v.legato = median (legato);
        v.swellDb = median (swell);
        v.dynamicsDb = percentile (vel, 0.9) - percentile (vel, 0.1);

        if (pitched > 0)
        {
            v.tuneCents = median (tune);
            v.tuneSpread = percentile (tune, 0.9) - percentile (tune, 0.1);
            v.vibratoShare = (double) vibratos / pitched;
            v.vibratoCents = median (vibC);
            v.vibratoHz = median (vibHz);
        }

        // Glide nur in Legato-Uebergaengen (Todo 2026-10-27-comparevoice-detune):
        // Portamento gleitet nur, wenn die alte Note beim Einsatz der neuen
        // noch klingt — ein Anteil an allen Noten blieb unter 0,5, auch wenn
        // jeder Uebergang gleitet. Legato-Uebergang: eine Note folgt allein
        // (kein Akkord, keine weitere klingende Note) einer anderen Tonhoehe
        // (bis eine Oktave), die bis hoechstens 150 ms vorher erkannt klingt,
        // und der Pegel bricht davor nicht ein (bis 20 dB: 4OSC legato 2–3 dB,
        // ueberlappende Noten mit eigenem Ausklang ~12 dB, Pause > 100 dB).
        // Die erkannten Grenzen taugen allein nicht: basic-pitch setzt den
        // Einsatz einer gleitenden Note bis ~120 ms spaet (ans Ende der
        // Bewegung) und laesst Noten zu spaet enden. Gleiten: legatoGlideMs
        // (wie legato_glide in tools/profile_plugin.py) oder pitchShape
        // (Einsatz von unten/oben, auch Pitch-Bends)
        if (stereo.size() > 0 && ! stereo[0].empty())
        {
            const auto mono = monoOf (stereo);
            const double tuning = known (v.tuneCents) ? v.tuneCents : 0.0;
            int changes = 0;

            for (auto& n : notes)
            {
                const Note* prev = nullptr;
                bool alone = true;

                for (auto& o : notes)
                {
                    if (&o == &n)
                        continue;

                    if (std::abs (o.start - n.start) <= 0.05)
                        alone = false;
                    else if (o.start < n.start && o.end > n.start - 0.15 && (prev == nullptr || o.start > prev->start))
                        prev = &o;
                }

                if (! alone || prev == nullptr || prev->pitch == n.pitch || std::abs (prev->pitch - n.pitch) > 12)
                    continue;

                for (auto& o : notes)
                    if (&o != &n && &o != prev && o.start < n.start && o.end - 0.15 > n.start)
                        alone = false;

                if (! alone || env.minIn (n.start - 0.12, n.start + 0.02) < env.maxIn (n.start - 0.25, n.start - 0.12) - 20.0)
                    continue;

                ++changes;
                double next = n.start + 0.3;

                for (auto& o : notes)
                    if (o.start > n.start + 0.05)
                        next = juce::jmin (next, o.start);

                const double from = juce::jmax (0.5 * (prev->start + n.start), n.start - 0.15);
                const double ms = legatoGlideMs (mono, rate, prev->pitch, n.pitch, tuning, from, juce::jmin (next, n.end));

                // Glide ueber den ganzen Schritt (glideMs hochgerechnet):
                // legatoGlideMs sieht ihn in der Mitte zwischen beiden Noten;
                // sonst pitchShape, wenn sein Glide zur vorigen Note zeigt und
                // bei ihr beginnt (ab 80 % des Schritts: die Contour zieht den
                // Nachbarton zur Mitte, Halbton -91..-95 ct) oder an den Rand
                // des Fensters (+-140 ct) reicht — er beginnt dann ausserhalb.
                // Ein Einsatz von unten/oben mitten im Fenster (Scoop, -100 ct
                // nach einer Terz) bleibt sein eigener
                const double step = (prev->pitch - n.pitch) * 100.0;

                if (ms > 0.0)
                    n.glideMs = ms, n.glideCents = step;
                else if (n.glideMs > 0.0 && n.glideCentsPerMs > 0.0 && n.glideCents * step > 0.0
                         && (std::abs (n.glideCents) >= 0.8 * std::abs (step) || std::abs (n.glideCents) >= 110.0))
                    n.glideCents = step, n.glideMs = std::abs (step) / n.glideCentsPerMs;

                if (n.glideMs > 0.0)
                    glideMs.push_back (n.glideMs);
            }

            v.legatoChanges = changes;
            v.glideShare = changes > 0 ? (double) glideMs.size() / changes : 0.0;
            v.glideMs = median (glideMs);
        }

        // Lage
        {
            std::vector<double> p;

            for (auto& n : notes)
                p.push_back (n.pitch);

            v.lowest = (int) *std::min_element (p.begin(), p.end());
            v.highest = (int) *std::max_element (p.begin(), p.end());
            v.medianPitch = (int) median (p);

            double sw = 0.0, sp = 0.0;

            for (auto& n : notes)
            {
                const double w = (n.end - n.start) * std::pow (10.0, n.velDb / 20.0);
                sw += w;
                sp += w * n.pitch;
            }

            v.meanPitch = sw > 0.0 ? sp / sw : v.medianPitch;
        }

        // Stimmen: gleichzeitig klingende Noten alle 20 ms
        {
            double t0 = notes.front().start, t1 = 0.0, sum = 0.0;
            int frames = 0;

            for (auto& n : notes)
                t1 = juce::jmax (t1, n.end);

            for (double t = t0; t < t1; t += 0.02)
            {
                int c = 0;

                for (auto& n : notes)
                    c += n.start <= t && n.end > t ? 1 : 0;

                if (c > 0)
                    sum += c, ++frames, v.maxVoices = juce::jmax (v.maxVoices, c);
            }

            v.voices = frames > 0 ? sum / frames : 0.0;
        }

        // Gleichzeitige Einsaetze (innerhalb 150 ms): Staffelung, Spannweite
        {
            std::vector<double> stagger, span;

            for (size_t i = 0; i < notes.size();)
            {
                size_t j = i + 1;
                int lo = notes[i].pitch, hi = notes[i].pitch;

                while (j < notes.size() && notes[j].start - notes[i].start < 0.15)
                    lo = juce::jmin (lo, notes[j].pitch), hi = juce::jmax (hi, notes[j].pitch), ++j;

                if (j - i >= 2)
                {
                    ++v.chordOnsets;
                    stagger.push_back ((notes[j - 1].start - notes[i].start) * 1000.0);
                    span.push_back (hi - lo);
                }

                i = j;
            }

            v.staggerMs = median (stagger);
            v.chordSpan = median (span);
        }

        // Takte: Noten, Muster im 16tel-Raster, Luecken in der Huellkurve
        if (v.hasGrid)
        {
            const int bpb = juce::jmax (1, grid.beatsPerBar);
            const double b0 = beatAt (grid, grid.downbeats.front());
            const int slotsPerBar = juce::jmin (32, bpb * 4);
            std::vector<double> lens;
            int onBeat = 0, onAnd = 0;

            for (auto& n : notes)
            {
                const double sb = beatAt (grid, n.start), eb = beatAt (grid, n.end);
                lens.push_back (eb - sb);
                const auto slot = (long) std::llround ((sb - b0) * 4.0);
                const auto inBeat = ((slot % 4) + 4) % 4;
                onBeat += inBeat == 0 ? 1 : 0;
                onAnd += inBeat == 2 ? 1 : 0;
            }

            v.onBeatShare = (double) onBeat / (double) notes.size();
            v.andShare = (double) onAnd / (double) notes.size();
            v.medianNoteBeats = median (lens);

            const int firstBar = (int) std::floor ((beatAt (grid, notes.front().start) - b0 + 0.125) / bpb);
            const int lastBar = (int) std::floor ((beatAt (grid, notes.back().start) - b0 + 0.125) / bpb);
            const double peakAll = *std::max_element (env.db.begin(), env.db.end());

            // Einsaetze fuer das Muster: verkettete Notenanfaenge (strumGap,
            // zusammen < 150 ms) sind ein Anschlag — ein Strum fiele sonst
            // mit den spaeteren Saiten in die naechste 16tel (Praxistest 1m:
            // 7 Einsaetze je Takt bei Offbeat-Achteln)
            std::vector<double> onsetTimes;
            {
                const double gap = strumGap (grid, true);
                double prev = -1.0, first = -1.0;

                for (auto& n : notes)
                {
                    if (prev < 0.0 || n.start - prev >= gap || n.start - first >= 0.15)
                        onsetTimes.push_back (n.start), first = n.start;

                    prev = n.start;
                }
            }
            std::vector<double> gaps, sustains, dips, attacks;

            for (int b = firstBar; b <= lastBar && v.bars.size() < 64; ++b)
            {
                Bar bar;
                bar.bar = b + 1;
                bar.start = timeAtBeat (grid, b0 + b * bpb);
                bar.end = timeAtBeat (grid, b0 + (b + 1) * bpb);
                juce::String pat;

                for (int s = 0; s < slotsPerBar; ++s)
                {
                    const double sbeat = b0 + b * bpb + s * (double) bpb / slotsPerBar;
                    const double ts = timeAtBeat (grid, sbeat), te = timeAtBeat (grid, sbeat + (double) bpb / slotsPerBar);
                    const double mid = 0.5 * (ts + te), halfSlot = 0.5 * (te - ts);
                    char ch = '.';

                    for (auto t : onsetTimes)
                        if (std::abs (t - ts) < halfSlot)
                            ch = 'x';

                    for (auto& n : notes)
                        if (ch == '.' && n.start < mid && n.end > mid)
                            ch = '-';

                    pat += juce::String::charToString ((juce::juce_wchar) ch);
                }

                bar.pattern = pat;

                // Noten, die im Takt beginnen (16tel-Toleranz am Rand)
                const double tol = 0.5 * (timeAtBeat (grid, b0 + b * bpb + 0.25) - bar.start);
                juce::StringArray words;
                juce::String group;
                double groupStart = -1.0;

                for (auto& n : notes)
                {
                    if (n.start < bar.start - tol || n.start >= bar.end - tol)
                        continue;

                    bar.pitches.push_back (n.pitch);
                    auto w = (n.glideMs > 0.0 ? juce::String (n.glideCents < 0 ? "/" : "\\") : juce::String())
                             + noteName (n.pitch) + (beatAt (grid, n.end) - beatAt (grid, n.start) >= 0.9 ? "~" : "");

                    if (groupStart >= 0.0 && n.start - groupStart < 0.15)
                        group << "+" << w;
                    else
                    {
                        if (group.isNotEmpty())
                            words.add (group);

                        group = w;
                        groupStart = n.start;
                    }
                }

                if (group.isNotEmpty())
                    words.add (group);

                bar.notes = words.joinIntoString (" ");

                // Tragende Toene: klingende Dauer im Takt x Pegel — robuster
                // als die Liste der Einsaetze (kurze Scheinnoten wiegen wenig)
                {
                    std::map<int, double> w;

                    for (auto& n : notes)
                    {
                        const double ov = juce::jmin (n.end, bar.end) - juce::jmax (n.start, bar.start);

                        if (ov > 0.0)
                            w[n.pitch] += ov * std::pow (10.0, n.velDb / 20.0);
                    }

                    bar.weights.assign (w.begin(), w.end());
                }

                // Luecken: unter Taktmaximum - 9 dB und wieder ueber - 6 dB
                const double bmax = env.maxIn (bar.start, bar.end);

                if (bmax > peakAll - 30.0)
                {
                    bool below = false;
                    int total = 0;
                    double near = 0.0;
                    std::vector<double> smoothed;
                    double dipSum = 0.0;
                    double dipT = 0.0, dipV = 0.0;

                    // Ein Einbruch zaehlt nur an einer Notengrenze — Schwebung
                    // (Detune) oder Tremolo mitten im Ton sind keine Luecke
                    auto atBoundary = [&notes] (double t)
                    {
                        for (auto& n : notes)
                            if (std::abs (n.end - t) < 0.12 || std::abs (n.start - t) < 0.12)
                                return true;

                        return false;
                    };

                    for (int i = juce::jmax (0, (int) (bar.start * env.fps)); i < (int) (bar.end * env.fps) && i < (int) env.db.size(); ++i)
                    {
                        // geglaettet ueber 30 ms
                        double s = 0.0;
                        int k = 0;

                        for (int j = i - 3; j <= i + 3; ++j)
                            if (j >= 0 && j < (int) env.db.size())
                                s += env.db[(size_t) j], ++k;

                        const double e = s / k;
                        ++total;
                        smoothed.push_back (e);
                        dipSum += juce::jlimit (0.0, 24.0, bmax - e - 3.0);

                        if (! below && e < bmax - 9.0)
                            below = true, dipT = i / env.fps, dipV = e;
                        else if (below && e < dipV)
                            dipT = i / env.fps, dipV = e;
                        else if (below && e > bmax - 6.0)
                        {
                            below = false;
                            bar.gaps += atBoundary (dipT) ? 1 : 0;
                        }
                    }

                    // gehalten: stetig von -12 dB (0) bis -6 dB (1) unter dem
                    // geglaetteten Maximum — eine harte Schwelle bei -9 dB
                    // unter dem Gipfel eines Frames sprang mit dem Sustain
                    // (Praxistest 1n: 4OSC 35 % -> 56 %, 38 % -> 90 % gehalten)
                    if (! smoothed.empty())
                    {
                        const double top = *std::max_element (smoothed.begin(), smoothed.end());

                        for (auto e : smoothed)
                            near += juce::jlimit (0.0, 1.0, (e - (top - 12.0)) / 6.0);
                    }

                    bar.sustain = total > 0 ? near / total : 0.0;
                    bar.dipDb = total > 0 ? dipSum / total : 0.0;

                    // Neuanschlaege: Anstieg ueber das Minimum der letzten
                    // 30 ms, oberhalb von 3 dB (Schwebung, Vibrato bleiben
                    // darunter); je Frame /6, damit ein Anstieg einmal zaehlt
                    {
                        const int lag = juce::jmax (1, (int) std::round (0.03 * env.fps));
                        double rise = 0.0;

                        // ab 0: ein Auftakt-Takt beginnt vor der Quelle (negativ) —
                        // vorher las die Schleife ausserhalb des Verlaufs
                        // (Praxistest 1f: reattackDbPerBeat 6,45e+303)
                        for (int i = juce::jmax (0, (int) (bar.start * env.fps)); i < (int) (bar.end * env.fps) && i < (int) env.db.size(); ++i)
                        {
                            double lo = env.db[(size_t) i];

                            for (int j = juce::jmax (0, i - lag); j < i; ++j)
                                lo = juce::jmin (lo, env.db[(size_t) j]);

                            if (env.db[(size_t) i] > bmax - 30.0)
                                rise += juce::jmax (0.0, env.db[(size_t) i] - lo - 3.0) / lag;
                        }

                        bar.attackDb = rise / (double) bpb;
                        attacks.push_back (bar.attackDb);
                    }
                    gaps.push_back (bar.gaps);
                    sustains.push_back (bar.sustain);
                    dips.push_back (bar.dipDb);
                }

                v.bars.push_back (bar);
            }

            v.gapsPerBar = median (gaps);
            v.sustainShare = median (sustains);
            v.dipDb = median (dips);
            v.attackDbPerBeat = median (attacks);
        }

        v.notes = notes;
        v.layers = findLayers (notes, env, monoOf (stereo), rate, grid, v.hasGrid);
        v.pulse = findPulse (monoOf (stereo), rate, grid, v.hasGrid);
        v.chordHits = findChordHits (raw, notes, grid, v.hasGrid);

        if (v.chordHits.layer && v.layers.empty())
        {
            Layer held;
            held.notes = (int) notes.size();
            held.lowest = v.lowest;
            held.highest = v.highest;
            held.medianBeats = v.medianNoteBeats;
            held.role = "held notes (pad/chords)";

            for (size_t i = 0; i < notes.size(); ++i)
                held.members.push_back (i);

            Layer hits;
            hits.notes = v.chordHits.raw;
            hits.lowest = v.chordHits.lowest;
            hits.highest = v.chordHits.highest;
            hits.role = "re-struck chords (stabs/lead chords over the held notes, or the same instrument re-striking)";
            v.layers = { held, hits };
        }

        // Noteneinsaetze je Takt (Akkord = einer) gegen die Pulse: deutlich
        // mehr Einsaetze als Noten = ein rhythmischer Part, den die
        // Notenerkennung nicht sieht (er schlaegt klingende Toene neu an)
        if (! v.pulse.bars.empty())
        {
            std::map<int, int> starts;
            double last = -1.0;

            for (auto& b : v.bars)
                starts[b.bar] = 0;

            for (auto& n : notes)
            {
                if (n.start - last >= 0.05)
                    for (auto& b : v.bars)
                        if (n.start >= b.start - 0.03 && n.start < b.end - 0.03)
                            ++starts[b.bar];

                last = n.start;
            }

            std::vector<double> c;

            for (auto& pb : v.pulse.bars)
                c.push_back ((double) (starts.count (pb.bar) > 0 ? starts[pb.bar] : 0));

            v.pulse.notesPerBar = median (c);
            v.pulse.layer = v.pulse.excess >= 0.6 && v.pulse.perBar >= 6.0 && v.pulse.perBar >= 2.0 * juce::jmax (1.0, v.pulse.notesPerBar);

            if (v.pulse.layer && v.layers.empty())
            {
                Layer held;
                held.notes = (int) notes.size();
                held.lowest = v.lowest;
                held.highest = v.highest;
                held.medianBeats = v.medianNoteBeats;
                held.role = v.medianNoteBeats >= 1.0 || v.legato >= 0.8 ? "held notes (pad/chords)" : "notes";

                for (size_t i = 0; i < notes.size(); ++i)
                    held.members.push_back (i);

                Layer pulse;
                pulse.notes = v.pulse.onsets;
                pulse.lowest = v.pulse.lowest;
                pulse.highest = v.pulse.highest;
                pulse.medianBeats = unknown;
                pulse.role = (v.pulse.heldShare < 0.5 ? juce::String ("short ") : juce::String()) + "pulses on " + v.pulse.grid
                             + (v.pulse.chord.isNotEmpty() ? " (" + v.pulse.chord + ")" : juce::String());
                v.layers = { held, pulse };
            }
        }
        return v;
    }

    namespace
    {
        // Betragsspektrum eines Ausschnitts (Hann, Nullen bis 2er-Potenz)
        std::vector<float> spectrum (const std::vector<float>& x, size_t from, size_t len, int& order)
        {
            order = juce::jlimit (10, 15, (int) std::ceil (std::log2 ((double) juce::jmax ((size_t) 1, len))));
            const size_t size = (size_t) 1 << order;
            len = juce::jmin (len, size, x.size() > from ? x.size() - from : (size_t) 0);
            std::vector<float> buf (2 * size, 0.0f);

            for (size_t i = 0; i < len; ++i)
                buf[i] = x[from + i] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) juce::jmax ((size_t) 1, len - 1)));

            juce::dsp::FFT fft (order);
            fft.performFrequencyOnlyForwardTransform (buf.data());
            buf.resize (size / 2 + 1);
            return buf;
        }

        double centroidOf (const std::vector<float>& mag, double binHz, double lowHz = 60.0)
        {
            double sw = 0.0, s = 0.0;

            for (size_t b = 1; b < mag.size(); ++b)
            {
                auto hz = (double) b * binHz;

                if (hz < lowHz)
                    continue;

                auto p = (double) mag[b] * mag[b];
                sw += p;
                s += p * hz;
            }

            return sw > 1.0e-12 ? s / sw : unknown;
        }

        double correlationOf (const std::vector<float>& l, const std::vector<float>& r, const std::vector<bool>& use, size_t hop)
        {
            double lr = 0.0, ll = 0.0, rr = 0.0;

            for (size_t i = 0; i < l.size() && i < r.size(); ++i)
                if (use.empty() || use[juce::jmin (use.size() - 1, i / hop)])
                    lr += (double) l[i] * r[i], ll += (double) l[i] * l[i], rr += (double) r[i] * r[i];

            return ll > 1.0e-9 && rr > 1.0e-9 ? lr / std::sqrt (ll * rr) : unknown;
        }
    }

    Timbre describeTimbre (const Voice& v, const std::vector<std::vector<float>>& stereo, double rate)
    {
        Timbre tb;

        if (stereo.empty() || stereo[0].empty())
            return tb;

        const auto mono = monoOf (stereo);
        const auto env = envelopeOf (mono, rate);
        const auto& notes = v.notes;

        // Fuer die Huellkurve geglaettet (+-10 ms, Leistung): Schwebung soll
        // den Gipfel nicht vorziehen
        Envelope smooth = env;

        for (size_t i = 0; i < env.db.size(); ++i)
        {
            double p = 0.0;
            int k = 0;

            for (int j = (int) i - 2; j <= (int) i + 2; ++j)
                if (j >= 0 && j < (int) env.db.size())
                    p += std::pow (10.0, env.db[(size_t) j] / 10.0), ++k;

            smooth.db[i] = 10.0 * std::log10 (p / k + 1.0e-12);
        }

        auto othersAt = [&notes] (const Note& self, double a, double b)
        {
            for (auto& n : notes)
                if (&n != &self && n.start < b && n.end > a)
                    return true;

            return false;
        };

        // Huellkurve je Note mit klarem Einsatz
        std::vector<double> att, dec, sus, rel;

        for (auto& n : notes)
        {
            const double dur = n.end - n.start;

            if (dur < 0.15)
                continue;

            const double peakEnd = n.start + juce::jmin (0.5, dur);
            double peak = -120.0, peakT = n.start;

            for (double t = n.start - 0.02; t <= peakEnd; t += 1.0 / smooth.fps)
                if (smooth.at (t) > peak)
                    peak = smooth.at (t), peakT = t;

            // Attack = Anstieg von -20 auf -3 dB unter dem gehaltenen Pegel
            // (75. Perzentil der ersten 0,8 s — robust gegen Schwebung, die
            // das Maximum vorzieht)
            std::vector<double> lv;

            for (double t = n.start; t <= juce::jmin (n.end, n.start + 0.8); t += 1.0 / smooth.fps)
                lv.push_back (smooth.at (t));

            const double level = percentile (lv, 0.75);

            if (smooth.minIn (n.start - 0.06, n.start) > level - 6.0)
                continue;   // kein klarer Einsatz (Legato im Klang davor)

            // ab dem Einbruch vor der Note (bei Legato klingt davor die
            // vorige Note — die Suche darf nicht in sie hineinlaufen)
            double tMin = n.start - 0.08;

            for (double t = n.start - 0.08; t <= n.start + 0.02; t += 1.0 / smooth.fps)
                if (smooth.at (t) < smooth.at (tMin))
                    tMin = t;

            double t3 = tMin;

            while (t3 < n.end && smooth.at (t3) < level - 3.0)
                t3 += 1.0 / smooth.fps;

            double t0 = t3;

            while (t0 > tMin && smooth.at (t0) > level - 20.0)
                t0 -= 1.0 / smooth.fps;

            if (smooth.at (tMin) <= level - 20.0)
                while (t0 > tMin - 0.3 && smooth.at (t0 - 1.0 / smooth.fps) <= level - 20.0 && smooth.at (t0) > level - 20.0)
                    t0 -= 1.0 / smooth.fps;

            att.push_back ((t3 - t0) * 1000.0);

            if (n.end - (peakT + 0.15) >= 0.1)
            {
                const double s = smooth.meanIn (peakT + 0.15, n.end - 0.03) - peak;
                sus.push_back (s);
                double t = peakT;

                while (t < n.end && smooth.at (t) > peak + s + 1.0)
                    t += 1.0 / smooth.fps;

                dec.push_back (s > -1.0 ? 0.0 : (t - peakT) * 1000.0);
            }

            // Release: nur wenn danach nichts anderes klingt
            if (! othersAt (n, n.end, n.end + 0.5))
            {
                const double e0 = smooth.at (n.end);
                double t = n.end;

                while (t < n.end + 3.0 && t * smooth.fps < (double) smooth.db.size() && smooth.at (t) > e0 - 20.0)
                    t += 1.0 / smooth.fps;

                if (smooth.at (t) <= e0 - 20.0)
                    rel.push_back ((t - n.end) * 1000.0);
            }
        }

        tb.envelopeNotes = (int) att.size();
        tb.attackMs = median (att);
        tb.decayMs = median (dec);
        tb.sustainDb = median (sus);
        tb.releaseMs = median (rel);

        // Obertoene je Note: bevorzugt allein klingende Toene
        std::vector<double> slope, oddEven, centH, centHz, noise, sweep;
        std::vector<const Note*> solo, all;

        for (auto& n : notes)
            if (n.end - n.start >= 0.12)
            {
                all.push_back (&n);

                if (! othersAt (n, n.start + 0.04, juce::jmin (n.end, n.start + 0.44)))
                    solo.push_back (&n);
            }

        tb.harmonicsFromChords = solo.empty();

        for (auto* n : solo.empty() ? all : solo)
        {
            const double from = n->start + 0.04, len = juce::jmin (n->end - from, 0.4);

            if (len < 0.08)
                continue;

            int order = 0;
            auto mag = spectrum (mono, (size_t) (from * rate), (size_t) (len * rate), order);
            const double binHz = rate / (double) (1 << order);
            const double f0 = 440.0 * std::pow (2.0, ((double) n->pitch + (known (n->tuneCents) ? n->tuneCents / 100.0 : 0.0) - 69.0) / 12.0);
            std::vector<double> db;
            double harmPower = 0.0;

            for (int k = 1; k <= 12 && k * f0 < 0.45 * rate; ++k)
            {
                double m = 0.0;

                for (auto b = (size_t) std::floor (k * f0 * 0.97 / binHz); b <= (size_t) std::ceil (k * f0 * 1.03 / binHz) && b < mag.size(); ++b)
                    m = juce::jmax (m, (double) mag[b]);

                for (auto b = (size_t) std::floor (k * f0 * 0.97 / binHz); b <= (size_t) std::ceil (k * f0 * 1.03 / binHz) && b < mag.size(); ++b)
                    harmPower += (double) mag[b] * mag[b];

                db.push_back (20.0 * std::log10 (m + 1.0e-9));
            }

            if (db.size() < 4)
                continue;

            double total = 0.0;

            for (auto b = (size_t) std::floor (0.5 * f0 / binHz); b <= (size_t) std::ceil ((db.size() + 0.5) * f0 / binHz) && b < mag.size(); ++b)
                total += (double) mag[b] * mag[b];

            const double top = *std::max_element (db.begin(), db.end());

            for (auto& d : db)
                d -= top;

            // Neigung: dB ueber log2(k), nur hoerbare Teiltoene
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int cnt = 0;

            for (size_t k = 0; k < db.size(); ++k)
                if (db[k] > -60.0)
                {
                    auto x = std::log2 ((double) k + 1.0);
                    sx += x, sy += db[k], sxx += x * x, sxy += x * db[k], ++cnt;
                }

            if (cnt >= 3)
                slope.push_back ((cnt * sxy - sx * sy) / (cnt * sxx - sx * sx));

            double odd = 0, even = 0;
            int no = 0, ne = 0;

            for (size_t k = 1; k < db.size(); ++k)
                ((k + 1) % 2 == 1 ? odd : even) += db[k], ++((k + 1) % 2 == 1 ? no : ne);

            if (no > 0 && ne > 0)
                oddEven.push_back (odd / no - even / ne);

            double pw = 0.0, pk = 0.0;

            for (size_t k = 0; k < db.size(); ++k)
            {
                auto p = std::pow (10.0, db[k] / 10.0);
                pw += p;
                pk += p * (double) (k + 1);
            }

            centH.push_back (pk / juce::jmax (1.0e-12, pw));
            centHz.push_back (centroidOf (mag, binHz));
            noise.push_back (total > 1.0e-12 ? juce::jlimit (0.0, 1.0, 1.0 - harmPower / total) : unknown);

            // Filter je Note: Schwerpunkt am Anfang gegen das letzte Drittel
            if (n->end - n->start >= 0.3)
            {
                int o1 = 0, o2 = 0;
                auto early = spectrum (mono, (size_t) ((n->start + 0.01) * rate), (size_t) (0.06 * rate), o1);
                const double lateFrom = n->start + 0.66 * (n->end - n->start);
                auto late = spectrum (mono, (size_t) (lateFrom * rate), (size_t) (juce::jmin (0.3, n->end - lateFrom) * rate), o2);
                auto ce = centroidOf (early, rate / (double) (1 << o1)), cl = centroidOf (late, rate / (double) (1 << o2));

                if (known (ce) && known (cl) && ce > 0.0 && cl > 0.0)
                    sweep.push_back (std::log2 (ce / cl));
            }

            ++tb.harmonicNotes;
        }

        tb.slopeDbOct = median (slope);
        tb.oddEvenDb = median (oddEven);
        tb.centroidHarmonic = median (centH);
        tb.centroidHz = median (centHz);
        tb.noiseShare = median (noise);
        tb.noteSweepOct = median (sweep);

        if (known (tb.slopeDbOct))
        {
            if (tb.slopeDbOct < -22.0)
                tb.wave = "sine-like";
            else if (known (tb.noiseShare) && tb.noiseShare > 0.5)
                tb.wave = "noisy";
            else if (known (tb.oddEvenDb) && tb.oddEvenDb > 8.0)
                tb.wave = tb.slopeDbOct < -10.0 ? "triangle-like" : "square-like";
            else if (tb.slopeDbOct < -12.0)
                tb.wave = "soft/filtered";
            else
                tb.wave = "saw-like";
        }

        // Schwerpunkt je Takt (Filterfahrt ueber den Teil)
        if (! v.bars.empty())
        {
            std::vector<double> xs, ys;

            for (auto& bar : v.bars)
            {
                if (bar.end - bar.start < 0.2)
                    continue;

                int o = 0;
                std::vector<double> cs;

                for (double t = bar.start; t + 0.05 < bar.end; t += 0.1)
                    if (env.at (t) > env.maxIn (bar.start, bar.end) - 20.0)
                    {
                        auto mag = spectrum (mono, (size_t) (t * rate), (size_t) (0.05 * rate), o);
                        cs.push_back (centroidOf (mag, rate / (double) (1 << o)));
                    }

                auto c = median (cs);
                tb.barCentroidHz.push_back (known (c) ? std::round (c) : unknown);

                if (known (c) && c > 0.0)
                    xs.push_back ((double) xs.size()), ys.push_back (std::log2 (c));
            }

            if (xs.size() >= 2)
            {
                const double n = (double) xs.size();
                double sx = 0, sy = 0, sxx = 0, sxy = 0;

                for (size_t i = 0; i < xs.size(); ++i)
                    sx += xs[i], sy += ys[i], sxx += xs[i] * xs[i], sxy += xs[i] * ys[i];

                tb.barSweepOct = (n * sxy - sx * sy) / (n * sxx - sx * sx) * (n - 1.0);
            }
        }

        // Stereo: nur wo es klingt (Huellkurve > Maximum - 40 dB)
        if (stereo.size() >= 2)
        {
            const auto& l = stereo[0];
            const auto& r = stereo[1];
            const auto hop = (size_t) juce::jmax (1.0, std::round (rate / env.fps));
            const double peakAll = env.db.empty() ? -120.0 : *std::max_element (env.db.begin(), env.db.end());
            std::vector<bool> use (env.db.size());

            for (size_t i = 0; i < env.db.size(); ++i)
                use[i] = env.db[i] > peakAll - 40.0;

            tb.correlation = correlationOf (l, r, use, hop);

            double sp = 0.0, mp = 0.0;

            for (size_t i = 0; i < l.size() && i < r.size(); ++i)
                if (use[juce::jmin (use.size() - 1, i / hop)])
                {
                    auto m = 0.5 * (l[i] + r[i]), s = 0.5 * (l[i] - r[i]);
                    mp += m * m, sp += s * s;
                }

            tb.sideMinusMidDb = mp > 1.0e-12 ? juce::jlimit (-60.0, 40.0, 10.0 * std::log10 (sp / mp + 1.0e-12)) : unknown;

            // Je Band (bis 250 Hz, bis 4 kHz, darueber)
            auto band = [&] (const std::vector<float>& x, double lo, double hi)
            {
                std::vector<float> y (x);

                for (int pass = 0; pass < 2; ++pass)
                {
                    if (lo > 0.0)
                    {
                        juce::IIRFilter f;
                        f.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, lo));
                        f.processSamples (y.data(), (int) y.size());
                    }

                    if (hi > 0.0)
                    {
                        juce::IIRFilter f;
                        f.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, hi));
                        f.processSamples (y.data(), (int) y.size());
                    }
                }

                return y;
            };

            tb.lowCorrelation = correlationOf (band (l, 0.0, 250.0), band (r, 0.0, 250.0), use, hop);
            tb.midCorrelation = correlationOf (band (l, 250.0, 4000.0), band (r, 250.0, 4000.0), use, hop);
            tb.highCorrelation = correlationOf (band (l, 4000.0, 0.0), band (r, 4000.0, 0.0), use, hop);

            // L/R-Versatz (Haas): Kreuzkorrelation +-5 ms ueber die lauteste Sekunde
            {
                size_t loudest = 0;

                for (size_t i = 0; i < env.db.size(); ++i)
                    if (env.db[i] > env.db[loudest])
                        loudest = i;

                const auto n = (size_t) rate;
                const auto from = (size_t) juce::jmax (0.0, (double) loudest * hop - 0.5 * rate);
                const int maxLag = (int) std::round (0.005 * rate);
                double best = -1.0e30;
                int bestLag = 0;

                for (int lag = -maxLag; lag <= maxLag; ++lag)
                {
                    double s = 0.0;

                    for (size_t i = from; i < from + n && i < l.size(); ++i)
                    {
                        auto j = (long) i + lag;

                        if (j >= 0 && j < (long) r.size())
                            s += (double) l[i] * r[(size_t) j];
                    }

                    if (s > best)
                        best = s, bestLag = lag;
                }

                // Am Rand des Suchfensters oder bei schwacher Aehnlichkeit
                // (gegenphasiger Inhalt) ist der Versatz keiner
                double ll = 0.0, rr = 0.0;

                for (size_t i = from; i < from + n && i < l.size() && i < r.size(); ++i)
                    ll += (double) l[i] * l[i], rr += (double) r[i] * r[i];

                if (std::abs (bestLag) < maxLag && ll > 0.0 && rr > 0.0 && best / std::sqrt (ll * rr) > 0.5)
                    tb.lrDelayMs = std::round (bestLag / rate * 10000.0) / 10.0;
            }

            // Breiten-Wackeln (Chorus): Seite/Mitte in 20-ms-Schritten, Streuung
            {
                std::vector<double> w;
                const auto step = (size_t) (0.02 * rate);

                for (size_t s0 = 0; s0 + step <= l.size() && s0 + step <= r.size(); s0 += step)
                {
                    if (! use[juce::jmin (use.size() - 1, s0 / hop)])
                        continue;

                    double ms = 0.0, ss = 0.0;

                    for (size_t i = s0; i < s0 + step; ++i)
                    {
                        auto m = 0.5 * (l[i] + r[i]), s = 0.5 * (l[i] - r[i]);
                        ms += m * m, ss += s * s;
                    }

                    if (ms > 1.0e-10 && ss > 1.0e-10)
                        w.push_back (10.0 * std::log10 (ss / ms));
                }

                if (w.size() >= 10)
                    tb.widthWobbleDb = (percentile (w, 0.9) - percentile (w, 0.1)) / 2.0;
            }
        }

        // Schwebung (Detune/Unison): Pegelschwankung 2..20 Hz im gehaltenen
        // Teil langer Toene
        {
            std::vector<double> depth, rate2;

            for (auto& n : notes)
            {
                if (n.end - n.start < 0.5)
                    continue;

                std::vector<double> e;

                for (double t = n.start + 0.15; t < n.end - 0.05; t += 1.0 / env.fps)
                    e.push_back (env.at (t));

                if (e.size() < 40)
                    continue;

                // linearen Trend weg (Abklingen)
                const double nE = (double) e.size();
                double sx = 0, sy = 0, sxx = 0, sxy = 0;

                for (size_t i = 0; i < e.size(); ++i)
                    sx += (double) i, sy += e[i], sxx += (double) i * i, sxy += (double) i * e[i];

                const double b = (nE * sxy - sx * sy) / (nE * sxx - sx * sx), a = (sy - b * sx) / nE;
                double var = 0.0;

                for (size_t i = 0; i < e.size(); ++i)
                    e[i] -= a + b * (double) i, var += e[i] * e[i];

                var /= nE;
                double bestR = 0.0;
                int bestLag = 0;

                for (int lag = (int) std::ceil (env.fps / 20.0); lag <= (int) std::ceil (env.fps / 2.0) && lag < (int) e.size() / 2; ++lag)
                {
                    double r = 0.0;

                    for (size_t i = 0; i + (size_t) lag < e.size(); ++i)
                        r += e[i] * e[i + (size_t) lag];

                    r /= (double) (e.size() - (size_t) lag) * juce::jmax (1.0e-9, var);

                    if (r > bestR)
                        bestR = r, bestLag = lag;
                }

                depth.push_back (std::sqrt (var));

                if (bestR > 0.3 && bestLag > 0)
                    rate2.push_back (env.fps / bestLag);
            }

            tb.beatingDb = median (depth);
            tb.beatingHz = median (rate2);
        }

        // Verstimmung: Breite der Teiltoene in gehaltenen Toenen (partialWidths,
        // Todo 2026-10-27-comparevoice-detune). beatingDb trennt verstimmte
        // Klaenge nicht — es misst vor allem das Ausklingen am Notenende.
        // Fenster 0,25 s nach dem Einsatz (Anschlag, langsames Portamento)
        // bis 0,25 s vor dem erkannten Ende (basic-pitch laesst Noten
        // 0,1–0,2 s zu spaet enden, das Ausklingen gehoert nicht hinein),
        // hoechstens 1 s, mindestens 0,5 s; ohne Einsatz einer anderen Note
        // darin; Fenster ueberlappen sich nicht (Akkord: einmal), bis zu 12
        // gleichmaessig verteilt. Alle Gipfel zusammen, Median.
        {
            std::vector<std::pair<double, double>> windows;
            double lastEnd = -1.0;

            for (auto& n : notes)
            {
                const double a = n.start + 0.25;
                double b = juce::jmin (n.end - 0.25, a + 1.0);

                for (auto& o : notes)
                    if (&o != &n && o.start > a - 0.1 && o.start < b + 0.05)
                        b = juce::jmin (b, o.start - 0.05);

                if (b - a >= 0.5 && a >= lastEnd)
                    windows.push_back ({ a, b }), lastEnd = b;
            }

            const double tuning = known (v.tuneCents) ? v.tuneCents : 0.0;
            std::vector<double> widths;
            const size_t count = juce::jmin ((size_t) 12, windows.size());

            for (size_t w = 0; w < count; ++w)
            {
                const auto [a, b] = windows[w * windows.size() / count];
                std::vector<double> partials;

                for (auto& o : notes)
                    if (o.start < b && o.end - 0.15 > a)
                        for (double f = 440.0 * std::pow (2.0, (o.pitch - 69) / 12.0 + tuning / 1200.0), k = 1.0; k * f < 2200.0; k += 1.0)
                            partials.push_back (k * f);

                for (auto x : partialWidths (mono, rate, a, b, partials))
                    widths.push_back (x);
            }

            tb.detuneCents = median (widths);
        }

        // Hallfahne: nach Noten, auf die 0,4 s nichts folgt, bis -30 dB
        {
            std::vector<double> tails;

            for (auto& n : notes)
            {
                if (othersAt (n, n.end, n.end + 0.4))
                    continue;

                const double e0 = env.maxIn (n.end - 0.05, n.end);
                double t = n.end;

                while (t < n.end + 4.0 && t * env.fps < (double) env.db.size() && env.at (t) > e0 - 30.0)
                    t += 1.0 / env.fps;

                if (env.at (t) <= e0 - 30.0)
                    tails.push_back (t - n.end);
            }

            tb.tailSec = median (tails);
        }

        // Hall: nach Noten, auf die 0,6 s nichts folgt — Steigung zwischen -5
        // und -25 dB unter dem Notenende als RT60 (x 3), der Pegel 150 ms
        // danach und die Stereo-Korrelation der Fahne gegen die des Notenendes.
        // Stereo-Hall macht die Fahne breiter als die Note; ein langes Release
        // behaelt die Breite der Note.
        {
            auto correlationIn = [&stereo, rate] (double a, double b)
            {
                if (stereo.size() < 2)
                    return unknown;

                const auto i0 = (size_t) juce::jmax (0.0, a * rate);
                const auto i1 = juce::jmin (stereo[0].size(), stereo[1].size(), (size_t) juce::jmax (0.0, b * rate));
                double ll = 0.0, rr = 0.0, lr = 0.0;

                for (size_t i = i0; i < i1; ++i)
                {
                    const double l = stereo[0][i], r = stereo[1][i];
                    ll += l * l, rr += r * r, lr += l * r;
                }

                return ll > 1.0e-9 && rr > 1.0e-9 ? lr / std::sqrt (ll * rr) : unknown;
            };

            std::vector<double> rt, early, tailCorr, noteCorr;

            for (auto& n : notes)
            {
                if (n.end - n.start < 0.1 || othersAt (n, n.end, n.end + 0.6))
                    continue;

                const double e0 = env.maxIn (n.end - 0.05, n.end);

                // Nur Enden, nach denen der Klang wirklich verstummt —
                // ausklingender Gesang endet bei basic-pitch zu frueh
                if (env.at (n.end + 0.6) > e0 - 15.0)
                    continue;

                double t5 = unknown, t25 = unknown;

                for (double t = n.end; t < n.end + 3.0 && t * env.fps < (double) env.db.size(); t += 1.0 / env.fps)
                {
                    if (! known (t5) && env.at (t) <= e0 - 5.0)
                        t5 = t;

                    if (env.at (t) <= e0 - 25.0)
                    {
                        t25 = t;
                        break;
                    }
                }

                if (known (t5) && known (t25) && t25 - t5 >= 2.0 / env.fps)
                    rt.push_back (3.0 * (t25 - t5));

                early.push_back (juce::jmax (-60.0, env.at (n.end + 0.15) - e0));

                // Fahne nur, solange sie noch klingt (bis 30 dB unter dem Ende)
                if (env.at (n.end + 0.1) > e0 - 30.0)
                {
                    tailCorr.push_back (correlationIn (n.end + 0.05, n.end + 0.5));
                    noteCorr.push_back (correlationIn (juce::jmax (n.start + 0.02, n.end - 0.3), n.end));
                }
            }

            tb.reverbNotes = (int) rt.size();

            if (rt.size() >= 2)
                tb.rt60Sec = median (rt);

            if (early.size() >= 2)
                tb.tailDb = median (early);

            if (tailCorr.size() >= 2)
            {
                tb.tailCorrelation = median (tailCorr);
                tb.noteCorrelation = median (noteCorr);
            }
        }

        // Delay: auf eine Note folgt dieselbe Tonhoehe in festem Abstand und
        // leiser. Gespielte Wiederholungen sind nur zum Teil leiser — Echos
        // fast immer. Je Note nur die naechste gleiche Tonhoehe (Ketten:
        // A -> A' -> A'' zaehlen als zwei Paare im selben Abstand).
        {
            constexpr double binSec = 0.01;
            std::map<int, int> quieter, all;
            std::vector<std::tuple<int, double, double>> pairs;   // Bin, Abstand, Pegelschritt

            for (size_t i = 0; i < notes.size(); ++i)
                for (size_t j = i + 1; j < notes.size(); ++j)
                {
                    const double lag = notes[j].start - notes[i].start;

                    if (lag > 1.5)
                        break;

                    if (notes[j].pitch != notes[i].pitch || lag < 0.06)
                        continue;

                    const int bin = (int) std::round (lag / binSec);
                    ++all[bin];
                    const double step = notes[j].velDb - notes[i].velDb;

                    if (step <= -1.5)
                    {
                        ++quieter[bin];
                        pairs.emplace_back (bin, lag, step);
                    }

                    break;
                }

            auto around = [] (const std::map<int, int>& m, int b)
            {
                int s = 0;

                for (int k = b - 1; k <= b + 1; ++k)
                    if (auto it = m.find (k); it != m.end())
                        s += it->second;

                return s;
            };

            int best = 0, bestCount = 0;

            for (auto& [bin, c] : quieter)
                if (auto s = around (quieter, bin); s > bestCount)
                    best = bin, bestCount = s;

            if (bestCount >= 3 && (double) bestCount >= 0.2 * (double) notes.size()
                && (double) bestCount >= 0.8 * (double) around (all, best))
            {
                std::vector<double> lags, steps;

                for (auto& [bin, lag, step] : pairs)
                    if (std::abs (bin - best) <= 1)
                        lags.push_back (lag), steps.push_back (step);

                tb.echoMs = std::round (median (lags) * 1000.0);
                tb.echoFeedbackDb = median (steps);
                tb.echoPairs = (int) lags.size();
                tb.echoShare = (double) lags.size() / (double) notes.size();
            }
        }

        // Anschlag: je Einsatz (Noten mit >= 40 ms Abstand) der Gipfel in den
        // ersten 40 ms gegen den Pegel 80–250 ms danach (bis vor den naechsten
        // Einsatz). Gross = perkussiv (Pick, Strum, Pluck), klein = weich.
        {
            std::vector<double> onsets;

            for (auto& n : notes)
                if (onsets.empty() || n.start - onsets.back() >= 0.04)
                    onsets.push_back (n.start);

            std::vector<double> tr;

            for (size_t i = 0; i < onsets.size(); ++i)
            {
                const double t = onsets[i];
                const double next = i + 1 < onsets.size() ? onsets[i + 1] : t + 1.0;
                const double b0 = t + 0.08, b1 = juce::jmin (t + 0.25, next - 0.02);

                if (b1 - b0 < 0.04)
                    continue;

                std::vector<double> body;

                for (double u = b0; u <= b1; u += 1.0 / smooth.fps)
                    body.push_back (smooth.at (u));

                tr.push_back (env.maxIn (t - 0.01, t + 0.04) - percentile (body, 0.5));
            }

            if (tr.size() >= 3)
                tb.transientDb = median (tr);
        }

        // Praesenz und Rauheit aus dem mittleren Leistungsspektrum
        {
            constexpr int order = 12, size = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> buf (2 * size);
            std::vector<double> power (size / 2 + 1, 0.0);
            std::vector<double> flat;
            const double binHz = rate / size;
            const int f2 = (int) (2000.0 / binHz), f8 = juce::jmin (size / 2, (int) (8000.0 / binHz));
            double loudest = 0.0;
            std::vector<std::pair<double, double>> frames;   // (Energie, Flachheit 2–8 kHz)

            for (size_t start = 0; start + (size_t) size <= mono.size(); start += size / 2)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);

                for (int i = 0; i < size; ++i)
                    buf[(size_t) i] = mono[start + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (size - 1)));

                fft.performFrequencyOnlyForwardTransform (buf.data());
                double e = 0.0, bandMax = 0.0;

                for (int b = 1; b <= size / 2; ++b)
                {
                    const double pw = (double) buf[(size_t) b] * buf[(size_t) b];
                    power[(size_t) b] += pw;
                    e += pw;

                    if (b >= f2 && b < f8)
                        bandMax = juce::jmax (bandMax, pw);
                }

                // Rauheit: spektrale Flachheit 2–8 kHz (geometrisches gegen
                // arithmetisches Mittel der Leistung). Verzerrte Akkorde
                // fuellen die Luecken zwischen den Teiltoenen mit
                // Intermodulation und Rauschen; verstimmte saubere Saegezaehne
                // bleiben Linien (Praxistest 1k: Referenz 0,12, Saw-Ensemble 0,05)
                double logSum = 0.0, linSum = 0.0;

                for (int b = f2; b < f8; ++b)
                {
                    const double pw = (double) buf[(size_t) b] * buf[(size_t) b];
                    logSum += std::log (pw + 1.0e-20);
                    linSum += pw;
                }

                const int nb = juce::jmax (1, f8 - f2);
                frames.push_back ({ e, linSum > 0.0 ? std::exp (logSum / nb) / (linSum / nb) : 0.0 });
                loudest = juce::jmax (loudest, e);
            }

            double total = 0.0, presence = 0.0;

            for (int b = 1; b <= size / 2; ++b)
            {
                total += power[(size_t) b];

                if (b * binHz >= 1000.0 && b * binHz < 5000.0)
                    presence += power[(size_t) b];
            }

            if (total > 0.0 && presence > 0.0)
                tb.presenceDb = 10.0 * std::log10 (presence / total);

            // Helligkeit aus dem Langzeit-Spektrum (Matthias 2026-10-24, V1;
            // Praxistest 1m: je Note gemessen "zu dunkel", die Baender von
            // compareReference passten — der Median je Note folgt der
            // Notenverteilung, nicht dem Klang)
            {
                double sw = 0.0, sf = 0.0;

                for (int b = 1; b <= size / 2; ++b)
                    if (b * binHz >= 60.0)
                        sw += power[(size_t) b], sf += power[(size_t) b] * b * binHz;

                if (sw > 1.0e-12)
                    tb.longTermCentroidHz = sf / sw;
            }

            for (auto& [e, f] : frames)
                if (e >= loudest * 1.0e-3)   // ab -30 dB unter dem lautesten Fenster
                    flat.push_back (f);

            if (! flat.empty())
                tb.roughness = median (flat);
        }

        return tb;
    }
}
