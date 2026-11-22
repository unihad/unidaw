#include "Ops.h"
#include "SoundMatch.h"
#include "../OpRegistry.h"
#include "../Presets.h"
#include "../Routing.h"
#include "../EngineHelpers.h"
#include "../PluginProfiles.h"
#include "../PluginSettings.h"
#include "../PluginRandomness.h"
#include "../Clap.h"

#include <array>
#include <cmath>
#include <set>

// Klang nachbauen (Familie processing, Todo 2026-10-18 P2): matchSound sucht
// Presets und erkundete Klaenge zu einem Referenzklang, tuneSound stellt
// Parameter eines Moduls Richtung Referenz. Entscheidung Matthias
// 2026-10-18: die Referenz ist nur Messziel, ihr Audio wird nie verwendet —
// nachgebaut wird mit eigenen Klangerzeugern (Presets, Parameter).
//
// Beide spielen die Noten der Referenz (describeVoice) durch das Plugin —
// in einem Wegwerf-Edit (eine Spur, das Plugin als Kopie, 120 BPM ab 0), das
// Projekt bleibt unberuehrt — und messen jeden Render gegen die Referenz:
// Huellkurve und Spektrum Zeitfenster fuer Zeitfenster (beide spielen
// dieselben Noten an denselben Stellen), Klang an den Noten der Referenz
// (describeTimbre), Stereo, Glide (eigene Notenerkennung) und CLAP. Jobs in
// Schritten auf dem Message-Thread (Jobs.h): ein Schritt = ein Render.
namespace dawd
{
    namespace
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        bool known (double v) { return ! std::isnan (v); }
        juce::var num (double v, double step = 0.01) { return known (v) ? juce::var (std::round (v / step) * step) : juce::var(); }
        juce::var db1 (double v) { return known (v) ? juce::var (std::round (v * 10.0) / 10.0 + 0.0) : juce::var(); }   // dB, eine Stelle

        // ---- Messung gegeneinander ------------------------------------------

        constexpr int hop = 441;                       // 10 ms bei 44,1 kHz
        constexpr int fftOrder = 11, fftSize = 1 << fftOrder;
        constexpr int numBands = 26;                   // Terzen 50 Hz … 16 kHz
        constexpr double floorDb = -45.0;

        double bandCentre (int b) { return 50.0 * std::pow (2.0, b / 3.0); }

        struct Profile
        {
            std::vector<double> env;                   // Pegel je 10 ms, dB unter dem 95. Perzentil (floorDb .. +6)
            std::vector<double> centroid;              // Schwerpunkt je 10 ms (log2 Hz), NaN wenn leise
            std::array<double, numBands> bands {};     // Langzeit-Spektrum, dB relativ zur Summe
            // Leistung je Terz, gemittelt ueber die lauten 10-ms-Fenster
            // (bis 30 dB unter dem eigenen 95. Perzentil): absolut, also
            // "wie laut, waehrend es spielt" — unabhaengig von Pausen und
            // Rhythmus. Fuer den Pegelschutz (Praxistest remix-1: Hochpass
            // 73 -> 1046 Hz am Bass, -51 statt -19 LUFS, galt als naeher)
            std::array<double, numBands> power {};
            double topDb = -200.0;                     // 95. Perzentil der 10-ms-Pegel (dBFS)
            bool valid = false;
        };

        Profile profileOf (const std::vector<std::vector<float>>& st)
        {
            Profile p;

            if (st.empty() || st[0].size() < (size_t) (4 * hop))
                return p;

            const double rate = AnalysisSource::stereoRate;
            const size_t n = st[0].size(), frames = n / (size_t) hop;
            std::vector<float> mid (n);

            for (size_t i = 0; i < n; ++i)
                mid[i] = st.size() > 1 ? 0.5f * (st[0][i] + st[1][i]) : st[0][i];

            std::vector<double> raw (frames);

            for (size_t f = 0; f < frames; ++f)
            {
                double q = 0.0;

                for (size_t i = f * (size_t) hop; i < (f + 1) * (size_t) hop; ++i)
                    for (auto& c : st)
                        q += (double) c[i] * c[i];

                raw[f] = 10.0 * std::log10 (q / (double) ((size_t) hop * st.size()) + 1.0e-14);
            }

            auto sorted = raw;
            std::sort (sorted.begin(), sorted.end());
            const double top = sorted[(size_t) (0.95 * (double) (frames - 1))];

            if (top < -100.0)
                return p;   // stumm

            p.topDb = top;
            p.env.resize (frames);

            for (size_t f = 0; f < frames; ++f)
                p.env[f] = juce::jlimit (floorDb, 6.0, raw[f] - top);

            juce::dsp::FFT fft (fftOrder);
            juce::dsp::WindowingFunction<float> win ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);
            std::vector<float> buf (2 * (size_t) fftSize);
            std::array<double, numBands> power {};
            const double binHz = rate / fftSize;
            p.centroid.assign (frames, nan);
            int loud = 0;

            for (size_t f = 0; f < frames; ++f)
            {
                if (p.env[f] < -30.0)
                    continue;

                ++loud;
                const long start = (long) (f * (size_t) hop) + hop / 2 - fftSize / 2;
                std::fill (buf.begin(), buf.end(), 0.0f);

                for (int i = 0; i < fftSize; ++i)
                    if (const long k = start + i; k >= 0 && k < (long) n)
                        buf[(size_t) i] = mid[(size_t) k];

                win.multiplyWithWindowingTable (buf.data(), (size_t) fftSize);
                fft.performFrequencyOnlyForwardTransform (buf.data());
                double sw = 0.0, s = 0.0;

                for (int k = 1; k < fftSize / 2; ++k)
                {
                    const double hz = k * binHz;

                    if (hz < 40.0 || hz > 18000.0)
                        continue;

                    const double pw = (double) buf[(size_t) k] * buf[(size_t) k];
                    sw += pw * hz;
                    s += pw;

                    if (const int b = (int) std::floor (3.0 * std::log2 (hz / 50.0) + 0.5); b >= 0 && b < numBands)
                        power[(size_t) b] += pw;
                }

                if (s > 0.0)
                    p.centroid[f] = std::log2 (sw / s);
            }

            double total = 0.0;

            for (auto x : power)
                total += x;

            if (total <= 0.0)
                return p;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                p.bands[b] = 10.0 * std::log10 (power[b] / total + 1.0e-12);
                p.power[b] = power[b] / (double) juce::jmax (1, loud);
            }

            p.valid = true;
            return p;
        }

        // Teile des Abstands: 0 = gleich, 1 = deutlich anders (Schwellen
        // gewaehlt, nicht kalibriert — Messwerte im Todo), hoechstens 3
        enum Part { pClap, pEnvelope, pSpectrum, pMotion, pStereo, pGlide, numParts };
        const char* const partNames[numParts] = { "clap", "envelope", "spectrum", "motion", "stereo", "glide" };
        constexpr double partWeights[numParts] = { 0.2, 0.25, 0.25, 0.1, 0.1, 0.1 };
        constexpr double maxPart = 3.0;

        // Pegelschutz (Todo 2026-10-29-klang-werkzeuge-pegelschutz): Energie
        // im Hauptbereich der Referenz (Terzen bis 10 dB unter ihrer
        // staerksten), absolut, waehrend es spielt. Liegt ein Kandidat mehr
        // als toleranceDb unter baselineDb, kommt der Teil "level" hinzu —
        // (Verlust − Toleranz) / 6 dB, hoechstens 3, mit Gewicht 0,25 auf
        // den Abstand obendrauf (nicht im Mittel: ohne Verlust bleibt der
        // Abstand wie bisher). Schwellen gewaehlt, nicht kalibriert
        constexpr double levelStepDb = 6.0, levelWeight = 0.25;
        // tuneSound: Toleranz der Suche (ab 3 dB Verlust zaehlt "level",
        // das Ergebnis kommt mit Hinweis) und Grenze fuer das automatische
        // Setzen (mehr als 6 dB leiser als der Ausgangsklang: nicht setzen)
        constexpr double tuneWarnDb = 3.0, tuneMaxLossDb = 6.0;

        struct CompareOptions
        {
            std::array<bool, numBands> main {};   // Hauptbereich der Referenz
            int mainLo = -1, mainHi = -1;
            double refLevelDb = nan;              // Referenz im Hauptbereich
            double baselineDb = nan;              // darunter zaehlt Verlust (nan = kein Schutz)
            double toleranceDb = 18.0;
            // compare "sound": Huellkurve und Bewegung ohne Zeitachse — die
            // Referenz an ihren eigenen (erkannten) Noten, der Kandidat an
            // denen, die er spielt (anderer Rhythmus, Remix). mode: auto
            // (Standard: "sound", wenn notes einen anderen Rhythmus spielen —
            // Entscheidung Matthias 2026-10-29), all, sound
            juce::String mode = "auto";
            bool soundOnly = false;
            voice::Timbre refOwn;
        };

        void setMainRange (CompareOptions& o, const Profile& rp)
        {
            double top = -200.0;

            for (auto b : rp.bands)
                top = juce::jmax (top, b);

            o.mainLo = o.mainHi = -1;

            for (int b = 0; b < numBands; ++b)
                if ((o.main[(size_t) b] = rp.bands[(size_t) b] >= top - 10.0))
                {
                    if (o.mainLo < 0)
                        o.mainLo = b;

                    o.mainHi = b;
                }
        }

        double levelIn (const Profile& p, const CompareOptions& o)
        {
            double sum = 0.0;

            for (size_t b = 0; b < (size_t) numBands; ++b)
                if (o.main[b])
                    sum += p.power[b];

            return 10.0 * std::log10 (sum + 1.0e-20);
        }

        juce::String hzText (double v)
        {
            return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
        }

        // "40–125 Hz": Hauptbereich der Referenz (Terzgrenzen)
        juce::String mainRangeText (const CompareOptions& o)
        {
            if (o.mainLo < 0)
                return "?";

            return hzText (bandCentre (o.mainLo) / std::pow (2.0, 1.0 / 6.0)) + juce::String::fromUTF8 ("–")
                 + hzText (bandCentre (o.mainHi) * std::pow (2.0, 1.0 / 6.0));
        }

        struct Eval
        {
            bool ok = false, silent = false;
            juce::String error;
            double distance = 0.0;
            std::array<double, numParts> parts { nan, nan, nan, nan, nan, nan };
            double clapSim = nan, envDb = nan, specDb = nan, motionOct = nan;
            double levelDb = nan, levelPart = nan;   // Hauptbereich der Referenz (absolut), Teil "level"
            int lagMs = 0, worstBand = -1;
            double worstBandDb = 0.0;
            juce::StringArray worst;
            juce::String soundsLike;
        };

        constexpr double silentDistance = 9.0;

        // Kandidat c gegen Referenz r (rp: Profil der Referenz)
        Eval compareSounds (const MeasuredSound& r, const Profile& rp, const MeasuredSound& c, const CompareOptions& opt)
        {
            Eval e;
            const auto cp = profileOf (c.src.stereo);
            const auto& rtb = opt.soundOnly ? opt.refOwn : r.tb;

            if (! cp.valid)
            {
                e.ok = true;
                e.silent = true;
                e.distance = silentDistance;
                e.worst.add ("silent with these notes");
                return e;
            }

            e.soundsLike = c.classes.isEmpty() ? juce::String() : c.classes[0];
            const auto frames = (int) juce::jmin (rp.env.size(), cp.env.size());

            // Huellkurve: Pegel je 10 ms, bester Versatz bis 80 ms (die Noten
            // der Referenz kommen aus der Notenerkennung und koennen etwas
            // spaet liegen). Laute Stellen zaehlen mehr (Gewicht 0,1 an der
            // Grenze bis 1 am Gipfel): erkannte Notenenden liegen ungenau,
            // in leisen Ausklaengen macht das viele dB
            auto envCost = [&] (int lag, int& used)
            {
                double sum = 0.0, wsum = 0.0;
                used = 0;

                for (int f = 0; f < frames; ++f)
                    if (const int g = f + lag; g >= 0 && g < (int) cp.env.size())
                        if (const double top = juce::jmax (rp.env[(size_t) f], cp.env[(size_t) g]); top > floorDb + 5.0)
                        {
                            const double w = 0.1 + 0.9 * juce::jlimit (0.0, 1.0, (top - floorDb) / -floorDb);
                            sum += w * std::abs (rp.env[(size_t) f] - cp.env[(size_t) g]);
                            wsum += w;
                            ++used;
                        }

                return used > 0 ? sum / wsum : nan;
            };

            int lag = 0, used = 0;

            if (opt.soundOnly)
            {
                // Huellkurve je Note statt ueber die Zeit: Attack und Release
                // als Verhaeltnis (Faktor 4 = 1), Sustain / 8 dB
                double sum = 0.0;
                int k = 0;

                auto ratio = [&] (double a, double b, double floorMs)
                {
                    if (known (a) && known (b))
                        sum += std::abs (std::log2 (juce::jmax (a, floorMs) / juce::jmax (b, floorMs))) / 2.0, ++k;
                };

                ratio (rtb.attackMs, c.tb.attackMs, 5.0);
                ratio (rtb.releaseMs, c.tb.releaseMs, 20.0);

                if (known (rtb.sustainDb) && known (c.tb.sustainDb))
                    sum += std::abs (rtb.sustainDb - c.tb.sustainDb) / 8.0, ++k;

                if (k > 0)
                    e.parts[pEnvelope] = sum / k;
            }
            else
            {
                double best = envCost (0, used);

                for (int L = -8; L <= 8; ++L)
                {
                    int u = 0;

                    if (const double v = envCost (L, u); known (v) && u >= frames / 4 && (! known (best) || v < best - 0.05))
                        best = v, lag = L;
                }

                e.lagMs = lag * 10;
                e.envDb = best;

                if (known (best))
                    e.parts[pEnvelope] = best / 8.0;
            }

            // Spektrum: Terzen mit Energie auf einer der Seiten; starke
            // Terzen zaehlen mehr (Gewicht 0,1 an der Grenze bis 1 am
            // Gipfel, wie die Huellkurve) — sonst ueberstimmten viele
            // schwache Terzen (Uebersprechen im Stem) den Hauptbereich:
            // ein Hochpass, der den Bass wegnimmt, galt als naeher
            // (Praxistest remix-1, Todo 2026-10-29-klang-werkzeuge-pegelschutz)
            {
                double top = -200.0;

                for (size_t b = 0; b < (size_t) numBands; ++b)
                    top = juce::jmax (top, juce::jmax (rp.bands[b], cp.bands[b]));

                double sum = 0.0, wsum = 0.0, worst = 0.0, worstW = 0.0;

                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    const double louder = juce::jmax (rp.bands[b], cp.bands[b]);

                    if (louder < top - 40.0)
                        continue;

                    const double d = juce::jmax (rp.bands[b], top - 50.0) - juce::jmax (cp.bands[b], top - 50.0);
                    const double w = 0.1 + 0.9 * juce::jlimit (0.0, 1.0, (louder - (top - 40.0)) / 40.0);
                    sum += w * std::abs (d);
                    wsum += w;

                    if (w * std::abs (d) > worstW)
                        worstW = w * std::abs (d), worst = d, e.worstBand = (int) b;
                }

                if (wsum > 0.0)
                {
                    e.specDb = sum / wsum;
                    e.worstBandDb = worst;
                    e.parts[pSpectrum] = e.specDb / 6.0;
                }
            }

            // Bewegung der Helligkeit (Filterfahrt), ohne den festen Versatz;
            // compare "sound": Verdunkeln im Ton je Note (noteSweepOct)
            if (opt.soundOnly)
            {
                if (known (rtb.noteSweepOct) && known (c.tb.noteSweepOct))
                {
                    e.motionOct = std::abs (rtb.noteSweepOct - c.tb.noteSweepOct);
                    e.parts[pMotion] = e.motionOct / 0.3;
                }
            }
            else
            {
                std::vector<std::pair<double, double>> both;

                for (int f = 0; f < frames; ++f)
                    if (const int g = f + lag; g >= 0 && g < (int) cp.centroid.size())
                        if (known (rp.centroid[(size_t) f]) && known (cp.centroid[(size_t) g]))
                            both.push_back ({ rp.centroid[(size_t) f], cp.centroid[(size_t) g] });

                if (both.size() >= 10)
                {
                    double mr = 0.0, mc = 0.0;

                    for (auto& [a, b] : both)
                        mr += a, mc += b;

                    mr /= (double) both.size();
                    mc /= (double) both.size();
                    double sum = 0.0;

                    for (auto& [a, b] : both)
                        sum += std::abs ((a - mr) - (b - mc));

                    e.motionOct = sum / (double) both.size();
                    e.parts[pMotion] = e.motionOct / 0.3;
                }
            }

            // Stereo aus describeTimbre (beide an den Noten der Referenz)
            {
                double sum = 0.0;
                int k = 0;

                if (known (rtb.correlation) && known (c.tb.correlation))
                    sum += std::abs (rtb.correlation - c.tb.correlation) / 0.5, ++k;

                if (known (rtb.sideMinusMidDb) && known (c.tb.sideMinusMidDb))
                    sum += std::abs (juce::jmax (-30.0, rtb.sideMinusMidDb) - juce::jmax (-30.0, c.tb.sideMinusMidDb)) / 12.0, ++k;

                if (k > 0)
                    e.parts[pStereo] = sum / k;
            }

            // Glide: Anteil der Noten mit Einsatz von unten/oben
            if (r.v.notes.size() >= 2 && ! c.v.notes.empty())
            {
                double d = std::abs (r.v.glideShare - c.v.glideShare) / 0.5;

                if (r.v.glideShare >= 0.25 && c.v.glideShare >= 0.25 && known (r.v.glideMs) && known (c.v.glideMs)
                    && r.v.glideMs > 0.0 && c.v.glideMs > 0.0)
                    d += 0.5 * std::abs (std::log2 (r.v.glideMs / c.v.glideMs));

                e.parts[pGlide] = d;
            }

            if (r.clap.size() == (size_t) clap::dim && c.clap.size() == (size_t) clap::dim)
            {
                e.clapSim = clap::similarity (r.clap, c.clap);
                e.parts[pClap] = (1.0 - e.clapSim) / 0.3;
            }

            double sum = 0.0, wsum = 0.0;

            for (int i = 0; i < numParts; ++i)
                if (known (e.parts[(size_t) i]))
                {
                    e.parts[(size_t) i] = juce::jlimit (0.0, maxPart, e.parts[(size_t) i]);
                    sum += partWeights[i] * e.parts[(size_t) i];
                    wsum += partWeights[i];
                }

            e.ok = wsum > 0.0;
            e.distance = e.ok ? sum / wsum : silentDistance;

            // Pegelschutz: Verlust im Hauptbereich der Referenz
            e.levelDb = levelIn (cp, opt);

            if (e.ok && known (opt.baselineDb))
            {
                e.levelPart = juce::jlimit (0.0, maxPart, (opt.baselineDb - e.levelDb - opt.toleranceDb) / levelStepDb);
                e.distance += levelWeight * e.levelPart;
            }

            // Was am weitesten weg ist, in Worten (Messwerte beider Seiten)
            auto ms = [] (double v) { return known (v) ? juce::String (juce::roundToInt (v)) + " ms" : juce::String ("?"); };
            auto hz = [] (double v) { return known (v) ? (v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz") : juce::String ("?"); };
            auto f2 = [] (double v) { return known (v) ? juce::String (v, 2) : juce::String ("?"); };
            std::vector<std::pair<double, juce::String>> texts;

            for (int i = 0; i < numParts; ++i)
            {
                const double v = e.parts[(size_t) i];

                if (! known (v) || v < 0.35)
                    continue;

                juce::String t;

                if (i == pEnvelope)
                {
                    t = opt.soundOnly ? juce::String ("note shape (each side at its own notes)")
                                      : "level over time off by " + juce::String (e.envDb, 1) + " dB on average";

                    if (known (rtb.attackMs) && known (c.tb.attackMs))
                        t << ", attack ref " << ms (rtb.attackMs) << " / this " << ms (c.tb.attackMs);

                    if (known (rtb.releaseMs) && known (c.tb.releaseMs))
                        t << ", release ref " << ms (rtb.releaseMs) << " / this " << ms (c.tb.releaseMs);

                    if (known (rtb.sustainDb) && known (c.tb.sustainDb))
                        t << ", sustain ref " << juce::String (rtb.sustainDb, 1) << " / this " << juce::String (c.tb.sustainDb, 1) << " dB";
                }
                else if (i == pSpectrum)
                {
                    t = "tone: centroid ref " + hz (rtb.longTermCentroidHz) + " / this " + hz (c.tb.longTermCentroidHz);

                    if (e.worstBand >= 0)
                        t << ", around " << hz (bandCentre (e.worstBand)) << " the reference is " << juce::String (juce::roundToInt (std::abs (e.worstBandDb)))
                          << " dB " << (e.worstBandDb > 0.0 ? "stronger" : "weaker");
                }
                else if (i == pMotion)
                {
                    t = opt.soundOnly ? juce::String ("brightness within a note differs (filter envelope)")
                                      : "brightness moves differently (" + juce::String (e.motionOct, 2) + " oct apart over time - filter envelope or sweep)";

                    if (known (rtb.noteSweepOct) && known (c.tb.noteSweepOct))
                        t << ", darker within a note ref " << f2 (rtb.noteSweepOct) << " / this " << f2 (c.tb.noteSweepOct) << " oct";
                }
                else if (i == pStereo)
                {
                    t = "width: correlation ref " + f2 (rtb.correlation) + " / this " + f2 (c.tb.correlation);

                    if (known (rtb.sideMinusMidDb) && known (c.tb.sideMinusMidDb))
                        t << ", side ref " << juce::String (juce::jmax (-30.0, rtb.sideMinusMidDb), 1) << " / this "
                          << juce::String (juce::jmax (-30.0, c.tb.sideMinusMidDb), 1) << " dB";
                }
                else if (i == pGlide)
                {
                    t = "glides: ref " + juce::String (juce::roundToInt (r.v.glideShare * 100)) + "% of legato note changes" + (r.v.glideShare > 0.0 ? " (" + ms (r.v.glideMs) + ")" : juce::String())
                        + " / this " + juce::String (juce::roundToInt (c.v.glideShare * 100)) + "%" + (c.v.glideShare > 0.0 ? " (" + ms (c.v.glideMs) + ")" : juce::String());
                }
                else if (i == pClap)
                {
                    t = "CLAP similarity " + f2 (e.clapSim);

                    if (! r.classes.isEmpty() && ! c.classes.isEmpty())
                        t << " (ref sounds like " << r.classes[0].upToLastOccurrenceOf (" ", false, false) << ", this like "
                          << c.classes[0].upToLastOccurrenceOf (" ", false, false) << ")";
                }

                texts.push_back ({ partWeights[i] * v, partNames[i] + juce::String (": ") + t });
            }

            if (known (e.levelPart) && e.levelPart >= 0.35)
                texts.push_back ({ levelWeight * e.levelPart, "level: " + juce::String (juce::roundToInt (opt.refLevelDb - e.levelDb))
                                                              + juce::String::fromUTF8 (" dB quieter than the reference in its main range (") + mainRangeText (opt)
                                                              + juce::String::fromUTF8 (") — nearly silent with these notes or the filter takes away what the reference has") });

            std::sort (texts.begin(), texts.end(), [] (auto& a, auto& b) { return a.first > b.first; });

            for (size_t i = 0; i < juce::jmin ((size_t) 3, texts.size()); ++i)
                e.worst.add (texts[i].second);

            return e;
        }

        juce::var partsVar (const Eval& e)
        {
            auto o = new juce::DynamicObject();

            for (int i = 0; i < numParts; ++i)
                o->setProperty (partNames[i], num (e.parts[(size_t) i]));

            if (known (e.levelPart))
                o->setProperty ("level", num (e.levelPart));

            return juce::var (o);
        }

        // ---- Wegwerf-Edit ---------------------------------------------------

        struct Bench
        {
            std::unique_ptr<te::Edit> edit;
            te::Plugin::Ptr plugin;
            juce::String key;          // was geladen ist
            double length = 0.0;

            ~Bench()
            {
                plugin = nullptr;
                edit.reset();
            }
        };

        // Eine Spur, die die Noten der Referenz ab 0 spielt; velocities: aus
        // notes (sonst aus dem Pegel beim Einsatz)
        juce::String makeBench (DawdContext& ctx, const MeasuredSound& ref, const std::vector<int>& velocities, Bench& b)
        {
            b.edit = te::Edit::createSingleTrackEdit (*ctx.engine);

            if (b.edit == nullptr)
                return "cannot create the render edit";

            b.length = ref.src.end - ref.src.start;
            auto tracks = te::getAudioTracks (*b.edit);

            if (tracks.isEmpty())
                return "cannot create the render edit";

            auto* clip = dynamic_cast<te::MidiClip*> (tracks.getFirst()->insertNewClip (te::TrackItem::Type::midi, "notes",
                                                         { te::TimePosition(), te::TimePosition::fromSeconds (b.length) }, nullptr));

            if (clip == nullptr)
                return "cannot create the render edit";

            auto& ts = b.edit->tempoSequence;
            const auto& notes = ref.v.notes;

            // Velocity aus dem Pegel beim Einsatz (velDb, relativ zum
            // lautesten Einsatz). Ein Akkord ist lauter, weil mehrere Toene
            // klingen, nicht weil sie staerker angeschlagen sind: je Einsatz
            // die Leistung durch die Zahl der Noten, die mit ihm (30 ms)
            // anfangen. Annahme: Pegel ~ Velocity^2, der staerkste = 100
            std::vector<double> db;

            for (auto& n : notes)
            {
                int k = 0;

                for (auto& m : notes)
                    if (std::abs (m.start - n.start) <= 0.03)
                        ++k;

                db.push_back ((known (n.velDb) ? n.velDb : 0.0) - 10.0 * std::log10 ((double) juce::jmax (1, k)));
            }

            const double loudest = db.empty() ? 0.0 : *std::max_element (db.begin(), db.end());

            for (size_t i = 0; i < notes.size(); ++i)
            {
                const auto& n = notes[i];
                const double s = juce::jmax (0.0, n.start), e = juce::jmin (b.length, n.end);

                if (e - s < 0.01)
                    continue;

                const int vel = i < velocities.size() ? velocities[i]
                                                      : juce::jlimit (20, 127, juce::roundToInt (100.0 * std::pow (10.0, (db[i] - loudest) / 40.0)));
                const auto b0 = ts.toBeats (te::TimePosition::fromSeconds (s)), b1 = ts.toBeats (te::TimePosition::fromSeconds (e));
                clip->getSequence().addNote (n.pitch, b0, b1 - b0, vel, 0, nullptr);
            }

            return {};
        }

        // Plugin in den Wegwerf-Edit (Kopie eines Zustands); key = was es ist
        juce::String benchPlugin (Bench& b, const juce::ValueTree& source, const juce::String& key)
        {
            auto* track = te::getAudioTracks (*b.edit).getFirst();

            if (b.plugin != nullptr)
            {
                b.plugin->deleteFromParent();
                b.plugin = nullptr;
                b.key = {};
            }

            auto state = source.createCopy();

            // Automation des Moduls bleibt draussen: gemessen wird die Einstellung
            for (int i = state.getNumChildren(); --i >= 0;)
                if (state.getChild (i).hasType (te::IDs::AUTOMATIONCURVE))
                    state.removeChild (i, nullptr);

            b.edit->createNewItemID().writeID (state, nullptr);
            b.plugin = track->pluginList.insertPlugin (state, 0);

            if (b.plugin == nullptr)
                return "cannot load " + key;

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (b.plugin.get()))
            {
                ep->initialiseFully();

                if (ep->getAudioPluginInstance() == nullptr)
                    return "cannot load " + key;
            }

            b.key = key;
            return {};
        }

        juce::String renderBench (DawdContext& ctx, Bench& b, const juce::File& dest)
        {
            te::Renderer::Parameters r (*b.edit);
            r.edit = b.edit.get();
            r.destFile = dest;
            r.audioFormat = ctx.engine->getAudioFileFormatManager().getWavFormat();
            r.bitDepth = 32;
            r.sampleRateForAudio = AnalysisSource::stereoRate;
            r.blockSizeForAudio = 512;
            r.canRenderInMono = false;
            r.checkNodesForAudio = false;
            r.ditheringEnabled = false;
            r.tracksToDo.setRange (0, (int) te::getAllTracks (*b.edit).size(), true);
            r.time = { te::TimePosition(), te::TimePosition::fromSeconds (b.length) };
            return renderWithParams (ctx, r);
        }

        // Render der Bank, gemessen gegen die Referenz
        Eval evaluate (DawdContext& ctx, Bench& b, const MeasuredSound& ref, const Profile& rp, const CompareOptions& opt)
        {
            Eval e;
            auto tmp = uniqueTempFile ("dawd-match", ".wav");
            const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };

            if (auto err = renderBench (ctx, b, tmp); err.isNotEmpty())
            {
                e.error = err;
                return e;
            }

            MeasuredSound c;

            if (auto err = measureSoundCandidate (ctx, ref, tmp, c); err.isNotEmpty())
            {
                e.error = err;
                return e;
            }

            return compareSounds (ref, rp, c, opt);
        }

        // Katalog-Name eines Plugins wie die Steckbriefe (<format>-<uid>)
        juce::String slugOf (te::Plugin& p)
        {
            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
                return profileSlug (ep->desc.pluginFormatName, juce::String::toHexString (ep->desc.uniqueId));

            return profileSlug ("builtin", p.getPluginType());
        }

        // Referenz: {file, start?, end?, bpm?, firstDownbeat?, …} | {section?,
        // start?, end?, track?} | Pfad
        juce::String referenceSpec (const juce::var& cmd, juce::var& spec, juce::var& like)
        {
            spec = cmd.getProperty ("reference", {});

            if (spec.isString())
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("file", spec);
                spec = juce::var (o);
            }

            if (! spec.isObject() || ! (spec.hasProperty ("file") || spec.hasProperty ("track") || spec.hasProperty ("section")
                                        || spec.hasProperty ("start")))
                return juce::String::fromUTF8 ("need reference: {file, start?, end?} or a part of the mix {track?, section? | start, end} — a stem of the reference works best, 2–8 bars");

            auto l = new juce::DynamicObject();

            for (auto key : { "file", "start", "end", "section", "track" })
                if (spec.hasProperty (key))
                    l->setProperty (key, spec.getProperty (key, {}));

            like = juce::var (l);
            return {};
        }

        constexpr double maxReferenceSec = 30.0;

        // Noten ohne Grundton in der Referenz: die Notenerkennung hoert in
        // Akkorden Differenztoene (A3+C4+E4 Rechteck -> A2 mit halber
        // Aktivierung, 2026-10-26). Gespielt klaengen sie echt — sie fallen
        // weg, wenn der Grundton waehrend der Note mehr als 35 dB unter dem
        // staerksten Teilton liegt (Schwelle gewaehlt)
        int dropNotesWithoutFundamental (MeasuredSound& ref)
        {
            const auto& st = ref.src.stereo;

            if (st.empty() || st[0].empty())
                return 0;

            const double rate = AnalysisSource::stereoRate;
            constexpr int order = 12, size = 1 << order;
            juce::dsp::FFT fft (order);
            juce::dsp::WindowingFunction<float> win ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
            std::vector<float> buf (2 * (size_t) size);
            std::vector<voice::Note> keep;
            int dropped = 0;

            for (auto& n : ref.v.notes)
            {
                const double f0 = 440.0 * std::pow (2.0, (n.pitch - 69) / 12.0);
                const auto from = (long) ((n.start + 0.03) * rate), to = (long) (n.end * rate) - size;
                double atF0 = 0.0, peak = 0.0;
                int frames = 0;

                for (long at = from; at <= juce::jmax (from, to) && frames < 8; at += size / 2, ++frames)
                {
                    std::fill (buf.begin(), buf.end(), 0.0f);

                    for (int i = 0; i < size; ++i)
                        if (const long k = at + i; k >= 0 && k < (long) st[0].size())
                            buf[(size_t) i] = st.size() > 1 ? 0.5f * (st[0][(size_t) k] + st[1][(size_t) k]) : st[0][(size_t) k];

                    win.multiplyWithWindowingTable (buf.data(), (size_t) size);
                    fft.performFrequencyOnlyForwardTransform (buf.data());

                    for (int k = 1; k < size / 2; ++k)
                    {
                        const double hz = k * rate / size, pw = (double) buf[(size_t) k] * buf[(size_t) k];

                        if (hz >= 30.0 && hz <= 8000.0)
                            peak = juce::jmax (peak, pw);

                        if (std::abs (std::log2 (hz / f0)) <= 1.0 / 24.0)
                            atF0 = juce::jmax (atF0, pw);
                    }
                }

                if (peak > 0.0 && 10.0 * std::log10 (atF0 / peak + 1.0e-20) < -35.0)
                    ++dropped;
                else
                    keep.push_back (n);
            }

            if (dropped > 0 && ! keep.empty())
            {
                ref.v.notes = std::move (keep);
                ref.tb = voice::describeTimbre (ref.v, ref.src.stereo, rate);
                return dropped;
            }

            return 0;
        }

        int trimOverlaps (MeasuredSound&);
        juce::String applyNotes (DawdContext&, const juce::var&, MeasuredSound&, std::vector<int>&);

        // Spielen notes denselben Rhythmus wie die Referenz? Anteil der
        // Einsaetze (gleichzeitige bis 30 ms = einer), die auf der anderen
        // Seite einen Einsatz bis 70 ms daneben haben — das Kleinere beider
        // Richtungen; NaN ohne Noten
        double onsetMatch (const std::vector<voice::Note>& a, const std::vector<voice::Note>& b)
        {
            auto onsets = [] (const std::vector<voice::Note>& ns)
            {
                std::vector<double> t;

                for (auto& n : ns)
                    t.push_back (n.start);

                std::sort (t.begin(), t.end());
                std::vector<double> out;

                for (auto x : t)
                    if (out.empty() || x - out.back() > 0.03)
                        out.push_back (x);

                return out;
            };

            const auto oa = onsets (a), ob = onsets (b);

            if (oa.empty() || ob.empty())
                return nan;

            auto share = [] (const std::vector<double>& from, const std::vector<double>& to)
            {
                int hit = 0;

                for (auto x : from)
                    for (auto y : to)
                        if (std::abs (x - y) <= 0.07)
                        {
                            ++hit;
                            break;
                        }

                return (double) hit / (double) from.size();
            };

            return juce::jmin (share (oa, ob), share (ob, oa));
        }

        constexpr double rhythmDiffers = 0.5;   // darunter: anderer Rhythmus (gewaehlt)

        // Referenz messen; Noten: notes (cmd) oder die erkannten, bereinigt.
        // opt: Hauptbereich und Pegel der Referenz, ihr Klang an den eigenen
        // Noten; rhythm: onsetMatch der notes gegen die erkannten Noten
        juce::String measureReferenceFor (DawdContext& ctx, const juce::var& cmd, const juce::var& spec, MeasuredSound& ref, Profile& rp,
                                          int& dropped, std::vector<int>& velocities, CompareOptions& opt, double& rhythm)
        {
            if (auto e = measureSoundReference (ctx, spec, ref); e.isNotEmpty())
                return e;

            opt.refOwn = ref.tb;   // Klang an den erkannten Noten

            if (cmd.hasProperty ("notes"))
            {
                const auto heard = ref.v.notes;

                if (auto e = applyNotes (ctx, cmd.getProperty ("notes", {}), ref, velocities); e.isNotEmpty())
                    return e;

                rhythm = onsetMatch (heard, ref.v.notes);
            }
            else
            {
                dropped = dropNotesWithoutFundamental (ref);
                trimOverlaps (ref);
                opt.refOwn = ref.tb;
            }

            if (ref.src.end - ref.src.start > maxReferenceSec + 0.01)
                return juce::String::fromUTF8 ("reference is longer than 30 s — give start/end (2–8 bars of the part)");

            if (ref.v.notes.empty())
                return juce::String::fromUTF8 ("no notes found in the reference — the candidates play its notes (describeVoice shows what it hears)");

            rp = profileOf (ref.src.stereo);

            if (! rp.valid)
                return "reference is silent";

            setMainRange (opt, rp);
            opt.refLevelDb = levelIn (rp, opt);
            return {};
        }

        // compare: "auto" (Standard) | "all" | "sound" — sound = nur der
        // Klang, ohne Zeitachse (Huellkurve und Helligkeit je Note); auto =
        // sound, wenn notes einen anderen Rhythmus spielen (useCompare)
        juce::String compareMode (const juce::var& cmd, CompareOptions& opt)
        {
            const auto mode = cmd.getProperty ("compare", "auto").toString();

            if (mode != "auto" && mode != "all" && mode != "sound")
                return "compare: auto (default), all or sound";

            opt.mode = mode;
            opt.soundOnly = mode == "sound";
            return {};
        }

        // auto: anderer Rhythmus (rhythmMatch unter 0,5) -> nur der Klang
        void useCompare (CompareOptions& opt, double rhythm)
        {
            if (opt.mode == "auto")
                opt.soundOnly = known (rhythm) && rhythm < rhythmDiffers;
        }

        // Hinweise zum Rhythmus der notes und zu einer gemischten Referenz
        void referenceHints (juce::StringArray& hints, const MeasuredSound& ref, const CompareOptions& opt, double rhythm)
        {
            const bool differs = known (rhythm) && rhythm < rhythmDiffers;
            const auto share = juce::String::fromUTF8 ("your notes play another rhythm than the reference (") + juce::String (juce::roundToInt (rhythm * 100))
                             + juce::String::fromUTF8 ("% of the onsets match)");

            if (differs && opt.mode == "auto")
                hints.add (share + juce::String::fromUTF8 (" — compared the sound only: envelope and motion per note (the reference at its own notes, the candidates at the notes they play), no level over time; compare: \"all\" keeps the time axis"));
            else if (differs && ! opt.soundOnly)
                hints.add (share + juce::String::fromUTF8 (" — compare: \"all\" keeps envelope and motion over time; they compare notes against gaps and say little (compare: \"sound\" or leave compare out)"));
            else if (opt.soundOnly)
                hints.add (juce::String::fromUTF8 ("compare: sound — envelope and motion per note (the reference at its own notes, the candidates at the notes they play), no level over time"));

            juce::StringArray roles;

            if (ref.v.layers.size() >= 2)
                for (auto& L : ref.v.layers)
                    roles.addIfNotAlreadyThere (L.role);

            if (ref.v.layers.size() >= 2 || ref.v.pulse.layer || ref.v.chordHits.layer)
                hints.add (juce::String::fromUTF8 ("the reference sounds like several instruments")
                           + (roles.isEmpty() ? juce::String() : " (" + roles.joinIntoString ("; ") + ")")
                           + juce::String::fromUTF8 (" — the candidates are measured against all of them; a stem with one instrument works best (describeVoice shows the layers, compareClasses splits finer)"));
        }

        // Erkannte Notenenden liegen im Ausklang: eine Note, die kurz (bis
        // 150 ms) in den naechsten Einsatz ragt, endet davor — sonst gleitet
        // ein Mono-/Legato-Preset, wo die Referenz nicht gleitet. Nur ohne
        // Glide in der Referenz (glideShare < 0,2)
        int trimOverlaps (MeasuredSound& ref)
        {
            if (ref.v.glideShare >= 0.2)
                return 0;

            int trimmed = 0;

            for (auto& n : ref.v.notes)
            {
                double cut = n.end;

                for (auto& m : ref.v.notes)
                    if (m.start > n.start + 0.05 && m.start < n.end && n.end - m.start <= 0.15)
                        cut = juce::jmin (cut, m.start - 0.01);

                if (cut < n.end && cut > n.start + 0.03)
                {
                    n.end = cut;
                    ++trimmed;
                }
            }

            if (trimmed > 0)
                ref.tb = voice::describeTimbre (ref.v, ref.src.stereo, AnalysisSource::stereoRate);

            return trimmed;
        }

        // notes: die Noten, die jeder Kandidat spielt, statt der erkannten —
        // [{pitch, start, length, velocity?}] in Sekunden ab dem Anfang der
        // Referenz, oder {track, section? | from?}: die MIDI-Noten einer Spur
        // (samt Lanes) ab from (Songsekunde; section = deren Anfang, sonst 0)
        juce::String applyNotes (DawdContext& ctx, const juce::var& arg, MeasuredSound& ref, std::vector<int>& velocities)
        {
            struct Given { int pitch; double start, end; int vel; };
            std::vector<Given> given;
            const double length = ref.src.end - ref.src.start;

            if (auto* list = arg.getArray())
            {
                for (auto& x : *list)
                {
                    if (! x.hasProperty ("pitch") || ! x.hasProperty ("start") || ! x.hasProperty ("length"))
                        return "notes: each needs pitch, start, length (seconds from the reference start)";

                    const double st = x.getProperty ("start", 0.0), len = x.getProperty ("length", 0.0);
                    given.push_back ({ juce::jlimit (0, 127, (int) x.getProperty ("pitch", 60)), st, st + len,
                                       juce::jlimit (1, 127, (int) x.getProperty ("velocity", 100)) });
                }
            }
            else if (arg.isObject() && arg.hasProperty ("track"))
            {
                const int index = trackIndexFor (*ctx.edit, arg.getProperty ("track", {}).toString());
                auto tracks = userTracks (*ctx.edit);

                if (index < 0 || index >= tracks.size())
                    return "notes: no such track: " + arg.getProperty ("track", {}).toString();

                double from = (double) arg.getProperty ("from", 0.0);

                if (arg.hasProperty ("section"))
                {
                    auto* sec = findSection (*ctx.edit, arg.getProperty ("section", {}).toString());

                    if (sec == nullptr)
                        return "notes: no such section: " + arg.getProperty ("section", {}).toString();

                    from = sec->getPosition().getStart().inSeconds();
                }

                auto* group = tracks[index];

                for (auto* t : tracks)
                    if (t == group || laneGroupOf (*t) == group)
                        for (auto* clip : t->getClips())
                            if (auto* mc = dynamic_cast<te::MidiClip*> (clip); mc != nullptr && ! mc->isMuted())
                            {
                                const auto pos = mc->getPosition();

                                for (auto* n : mc->getSequence().getNotes())
                                {
                                    auto r = n->getEditTimeRange (*mc);
                                    const double s0 = juce::jmax (r.getStart().inSeconds(), pos.getStart().inSeconds());
                                    const double s1 = juce::jmin (r.getEnd().inSeconds(), pos.getEnd().inSeconds());

                                    if (s1 > s0 && s1 > from && s0 < from + length)
                                        given.push_back ({ n->getNoteNumber(), s0 - from, s1 - from, n->getVelocity() });
                                }
                            }
            }
            else
                return "notes: a list [{pitch, start, length, velocity?}] (seconds from the reference start) or {track, section? | from?}";

            std::sort (given.begin(), given.end(), [] (const Given& a, const Given& b) { return a.start < b.start; });
            given.erase (std::remove_if (given.begin(), given.end(), [length] (const Given& g)
                                         { return g.end <= g.start || g.end <= 0.0 || g.start >= length; }), given.end());

            if (given.empty())
                return "notes: none inside the reference (" + juce::String (length, 2) + " s)";

            ref.v.notes.clear();
            velocities.clear();

            for (auto& g : given)
            {
                voice::Note n;
                n.pitch = g.pitch;
                n.start = juce::jmax (0.0, g.start);
                n.end = juce::jmin (length, g.end);
                ref.v.notes.push_back (n);
                velocities.push_back (g.vel);
            }

            ref.tb = voice::describeTimbre (ref.v, ref.src.stereo, AnalysisSource::stereoRate);
            return {};
        }

        juce::var referenceVar (const MeasuredSound& ref, int dropped, double rhythm = nan)
        {
            auto o = new juce::DynamicObject();
            describeAnalysisSource (*o, ref.src);
            o->removeProperty ("rmsDb");
            o->setProperty ("notes", (int) ref.v.notes.size());

            if (known (rhythm))
                o->setProperty ("rhythmMatch", num (rhythm));   // notes gegen die gehoerten Einsaetze

            if (dropped > 0)
                o->setProperty ("droppedNotes", dropped);   // ohne Grundton (Differenztoene)

            if (! ref.classes.isEmpty())
                o->setProperty ("soundsLike", ref.classes[0]);

            return juce::var (o);
        }

        // Job in Schritten auf dem Message-Thread: step() = ein Render,
        // finish() schreibt das Ergebnis
        struct SteppedWork : JobWork
        {
            virtual bool step (DawdContext&, Job&) = 0;     // false = fertig
            virtual void finish (DawdContext&, Job&) = 0;
        };

        void scheduleStep (DawdContext& ctx, std::weak_ptr<Job> weak)
        {
            juce::MessageManager::callAsync ([&ctx, weak]
            {
                auto job = weak.lock();

                if (job == nullptr || job->done || job->work == nullptr)
                    return;

                auto& w = static_cast<SteppedWork&> (*job->work);

                if (! job->cancel && w.step (ctx, *job))
                {
                    scheduleStep (ctx, weak);
                    return;
                }

                w.finish (ctx, *job);
                job->work = nullptr;   // Wegwerf-Edit und Plugin-Kopie freigeben
                job->progress = 1.0;
                job->done = true;
            });
        }

        juce::var startJob (DawdContext& ctx, const juce::String& kind, std::shared_ptr<SteppedWork> work)
        {
            auto job = ctx.jobs->create (kind);
            job->onMessageThread = true;
            job->work = work;
            scheduleStep (ctx, job);
            return jobVar (*job);
        }

        void setResult (Job& job, juce::var result, const juce::String& error)
        {
            const std::lock_guard<std::mutex> sl (job.lock);
            job.result = result;
            job.error = error;
        }

        // ---- matchSound -----------------------------------------------------

        struct Candidate
        {
            juce::String kind;                  // current | preset | sound
            juce::String pluginName, format;    // Plugin des Katalogs; leer = das Modul
            PresetEntry preset;                 // preset (sound: Basis aus dem Klang-Katalog)
            juce::String soundId, label;
            double catalogScore = nan;
            juce::var load;                     // loadPreset-Befehl
        };

        PresetEntry entryOf (const juce::var& v)
        {
            PresetEntry e;
            e.name = v.getProperty ("name", {}).toString();
            e.category = v.getProperty ("category", {}).toString();
            e.source = v.getProperty ("source", {}).toString();
            e.program = (int) v.getProperty ("program", -1);

            // Katalog: Pfad relativ zum Preset-Ordner, loadPresetEntry sucht die Datei
            if (auto path = v.getProperty ("path", {}).toString(); juce::File::isAbsolutePath (path))
                e.file = juce::File (path);
            else
                e.catalogPath = path;

            return e;
        }

        juce::var loadCommand (const juce::String& moduleId, const Candidate& c)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("op", "loadPreset");

            if (moduleId.isNotEmpty())
                o->setProperty ("plugin", moduleId);

            if (c.kind == "sound")
                o->setProperty ("sound", c.soundId);
            else if (c.preset.source == "program")
                o->setProperty ("program", c.preset.program);
            else
            {
                o->setProperty ("path", c.preset.file != juce::File() ? c.preset.file.getFullPathName() : c.preset.catalogPath);

                if (c.preset.program >= 0)
                    o->setProperty ("program", c.preset.program);
            }

            return juce::var (o);
        }

        Candidate candidateOf (const juce::var& hit, const juce::String& moduleId)
        {
            Candidate c;
            c.kind = hit.getProperty ("kind", {}).toString() == "sound" ? "sound" : "preset";
            c.pluginName = moduleId.isEmpty() ? hit.getProperty ("pluginName", {}).toString() : juce::String();
            c.format = moduleId.isEmpty() ? hit.getProperty ("format", {}).toString() : juce::String();
            c.catalogScore = hit.hasProperty ("score") ? (double) hit.getProperty ("score", 0.0) : nan;

            if (c.kind == "sound")
            {
                c.soundId = hit.getProperty ("id", {}).toString();
                c.label = hit.getProperty ("name", {}).toString();
            }
            else
            {
                c.preset = entryOf (hit);
                c.label = c.preset.category.isNotEmpty() ? c.preset.category + "/" + c.preset.name : c.preset.name;
            }

            c.load = loadCommand (moduleId, c);
            return c;
        }

        struct MatchWork : SteppedWork
        {
            juce::String varyHint;   // "renders vary" (PluginRandomness), oder ""
            MeasuredSound ref;
            Profile refProfile;
            CompareOptions opts;
            double rhythm = nan;           // onsetMatch der notes
            int dropped = 0;
            std::vector<int> velocities;   // aus notes
            bool givenNotes = false;
            Bench bench;
            juce::String moduleId;               // Modul im Projekt (oder leer: Kataloge)
            juce::ValueTree moduleState;
            std::map<juce::String, juce::ValueTree> states;   // Kataloge: Zustand je Plugin
            std::map<juce::String, juce::var> soundCatalogs;  // <slug> -> Klang-Katalog
            std::vector<Candidate> cands;
            std::vector<Eval> evals;
            size_t next = 0;
            int limit = 5;

            // Erkundeter Klang aus <slug>-sounds.json: Basis-Preset + Parameter
            juce::String loadSound (DawdContext& ctx, te::Plugin& p, const Candidate& c)
            {
                const auto slug = slugOf (p);

                if (soundCatalogs.count (slug) == 0)
                    soundCatalogs[slug] = juce::JSON::parse (profilesDirectory().getChildFile (slug + "-sounds.json"));

                juce::var snd;

                if (auto* a = soundCatalogs[slug].getProperty ("sounds", {}).getArray())
                    for (auto& x : *a)
                        if (x.getProperty ("id", {}).toString() == c.soundId)
                            snd = x;

                if (snd.isVoid())
                    return "no such sound: " + c.soundId;

                const auto base = snd.getProperty ("base", {});

                if (base.getProperty ("name", {}).toString().isNotEmpty())
                {
                    auto e = entryOf (base);
                    int changed = 0;

                    if (auto err = loadPresetEntry (ctx, p, e, changed); err.isNotEmpty())
                        return err;
                }

                if (auto* params = snd.getProperty ("params", {}).getDynamicObject())
                    for (auto& prop : params->getProperties())
                        if (auto* prm = findParameter (p, prop.name.toString()))
                            prm->setNormalisedParameter (juce::jlimit (0.0f, 1.0f, (float) (double) prop.value), juce::sendNotification);

                return {};
            }

            juce::String prepare (DawdContext& ctx, const Candidate& c)
            {
                if (moduleId.isNotEmpty())
                {
                    // ein Klang ohne Basis setzt nur Parameter: frisch vom Modul
                    const bool fresh = c.kind == "current" || (c.kind == "sound" && bench.key != "module");

                    if (bench.plugin == nullptr || bench.key != "module" || fresh)
                        if (auto err = benchPlugin (bench, moduleState, "module"); err.isNotEmpty())
                            return err;
                }
                else
                {
                    const auto key = c.pluginName + "|" + c.format;

                    if (bench.key != key)
                    {
                        if (states.count (key) == 0)
                        {
                            juce::String err;
                            auto st = newPluginState (ctx, c.pluginName, c.format, {}, err);

                            if (err.isNotEmpty())
                                return err;

                            states[key] = st;
                        }

                        if (auto err = benchPlugin (bench, states[key], key); err.isNotEmpty())
                            return err;
                    }
                }

                if (c.kind == "current")
                    return {};

                if (c.kind == "sound")
                    return loadSound (ctx, *bench.plugin, c);

                // Programm nach Nummer — steht dort ein anderer Name (Liste
                // anders sortiert als im Katalog), nach dem Namen
                auto entry = c.preset;

                if (auto* ep = dynamic_cast<te::ExternalPlugin*> (bench.plugin.get()); ep != nullptr && entry.source == "program"
                    && entry.name.isNotEmpty() && ep->getProgramName (entry.program).trim() != entry.name)
                    for (int i = 0; i < ep->getNumPrograms(); ++i)
                        if (ep->getProgramName (i).trim() == entry.name)
                        {
                            entry.program = i;
                            break;
                        }

                int changed = 0;
                return loadPresetEntry (ctx, *bench.plugin, entry, changed);
            }

            bool step (DawdContext& ctx, Job& job) override
            {
                if (next >= cands.size())
                    return false;

                auto& c = cands[next];
                Eval e;

                if (auto err = prepare (ctx, c); err.isNotEmpty())
                    e.error = err;
                else
                    e = evaluate (ctx, bench, ref, refProfile, opts);

                evals.push_back (e);
                ++next;
                job.progress = 0.98 * (double) next / (double) cands.size();
                return next < cands.size();
            }

            void finish (DawdContext&, Job& job) override
            {
                std::vector<size_t> order;
                juce::Array<juce::var> failed;

                for (size_t i = 0; i < evals.size(); ++i)
                {
                    if (evals[i].ok)
                        order.push_back (i);
                    else
                    {
                        auto f = new juce::DynamicObject();
                        f->setProperty ("name", cands[i].label);
                        f->setProperty ("error", evals[i].error);
                        failed.add (juce::var (f));
                    }
                }

                std::stable_sort (order.begin(), order.end(), [this] (size_t a, size_t b) { return evals[a].distance < evals[b].distance; });
                juce::Array<juce::var> list;
                const Eval* current = nullptr;

                for (size_t i = 0; i < evals.size(); ++i)
                    if (cands[i].kind == "current" && evals[i].ok)
                        current = &evals[i];

                for (auto i : order)
                {
                    if (list.size() >= limit)
                        break;

                    const auto& c = cands[i];
                    const auto& e = evals[i];
                    auto o = new juce::DynamicObject();
                    o->setProperty ("name", c.kind == "current" ? juce::String ("(current sound)") : c.label);
                    o->setProperty ("kind", c.kind);

                    if (c.pluginName.isNotEmpty())
                    {
                        o->setProperty ("pluginName", c.pluginName);
                        o->setProperty ("format", c.format);
                    }

                    o->setProperty ("distance", num (e.distance, 0.001));
                    o->setProperty ("parts", partsVar (e));

                    if (known (e.clapSim))
                        o->setProperty ("clapSimilarity", num (e.clapSim, 0.001));

                    // Pegel im Hauptbereich der Referenz gegen die Referenz (dB)
                    if (known (e.levelDb) && known (opts.refLevelDb))
                        o->setProperty ("levelVsReference", db1 (e.levelDb - opts.refLevelDb));

                    if (known (c.catalogScore))
                        o->setProperty ("catalogScore", num (c.catalogScore, 0.001));

                    if (e.silent)
                        o->setProperty ("silent", true);

                    if (e.soundsLike.isNotEmpty())
                        o->setProperty ("soundsLike", e.soundsLike);

                    o->setProperty ("worst", juce::var (e.worst));

                    if (c.kind != "current")
                        o->setProperty ("load", c.load);

                    list.add (juce::var (o));
                }

                auto res = new juce::DynamicObject();
                res->setProperty ("reference", referenceVar (ref, dropped, rhythm));
                res->setProperty ("compare", opts.soundOnly ? "sound" : "all");

                if (moduleId.isNotEmpty())
                    res->setProperty ("plugin", moduleId);

                res->setProperty ("rendered", (int) evals.size());
                res->setProperty ("candidates", (int) cands.size());
                res->setProperty ("matches", list);

                if (! failed.isEmpty())
                    res->setProperty ("failed", failed);

                juce::StringArray hints;

                if (! order.empty())
                {
                    const auto& b = cands[order.front()];

                    if (b.kind == "current")
                        hints.add (juce::String::fromUTF8 ("the current sound is closest — tuneSound {plugin, reference} moves its parameters closer"));
                    else if (moduleId.isNotEmpty())
                        hints.add (juce::String::fromUTF8 ("closest: ") + b.label + juce::String::fromUTF8 (" — run its load command, then tuneSound {plugin: \"")
                                   + moduleId + juce::String::fromUTF8 ("\", reference} to move its parameters closer"));
                    else
                        hints.add (juce::String::fromUTF8 ("closest: ") + b.label + " (" + b.pluginName + juce::String::fromUTF8 (") — add the plugin (createTrack {type: \"midi\", instrument: \"")
                                   + b.pluginName + juce::String::fromUTF8 ("\"} or addPlugin), run the load command with plugin = its module id, then tuneSound"));
                }

                if (current != nullptr)
                    hints.add ("current sound: distance " + juce::String (current->distance, 3));

                // Rolle: der naechste klingt wie ein Bass, der eigene nicht
                // (oder umgekehrt) — gemischter Stem (Praxistest remix-1: Pad
                // gegen den 4-Stem other -> Bass-Gitarre)
                if (current != nullptr && ! order.empty() && cands[order.front()].kind != "current")
                {
                    const auto& b = evals[order.front()];
                    auto cls = [] (const juce::String& s) { return s.upToLastOccurrenceOf (" ", false, false); };
                    auto bass = [] (const juce::String& s) { return s.contains ("bass"); };

                    if (b.soundsLike.isNotEmpty() && current->soundsLike.isNotEmpty() && bass (b.soundsLike) != bass (current->soundsLike))
                        hints.add (juce::String::fromUTF8 ("closest \"") + cands[order.front()].label + juce::String::fromUTF8 ("\" sounds like ") + cls (b.soundsLike)
                                   + ", your current sound like " + cls (current->soundsLike)
                                   + juce::String::fromUTF8 (" — another role; is the reference the part you build? (a mixed stem can be led by another instrument)"));
                }

                referenceHints (hints, ref, opts, rhythm);

                hints.add (juce::String::fromUTF8 ("distance 0 = same; each part 0 (same) … 1 (clearly different) … 3; every candidate played the same notes"));

                if (varyHint.isNotEmpty())
                    hints.add (varyHint);

                if (! givenNotes)
                    hints.add (juce::String::fromUTF8 ("the candidates played the notes heard in the reference (") + juce::String ((int) ref.v.notes.size())
                               + juce::String::fromUTF8 (") — when describeVoice shows wrong notes (overtones, sub-octaves), give notes: {track, section} (your MIDI part) or a list; the ranking depends on them"));
                res->setProperty ("hints", juce::var (hints));
                setResult (job, juce::var (res), job.cancel ? juce::String ("cancelled") : juce::String());
            }
        };

        // ---- tuneSound ------------------------------------------------------

        // Regler, die nicht den Klang, sondern Stimmung oder Pegel stellen
        // (wie tools/explore_sounds.py NOT_TIMBRE)
        bool notTimbre (const juce::String& name)
        {
            static const juce::StringArray words { "tune", "tuning", "octave", "transpose", "pitch", "fine", "coarse", "semi",
                                                   "semitone", "semitones", "master", "volume", "output", "level", "gain" };
            auto tokens = juce::StringArray::fromTokens (name.toLowerCase().replaceCharacters ("_-", "  "), " ", {});

            for (auto& t : tokens)
                if (words.contains (t))
                    return true;

            return false;
        }

        // Schalter und Rhythmus-Regler: die Koordinatensuche stellt stetig,
        // diese springen (Handpruefung Helm 2026-10-26: delay_on, arp_on,
        // reverb_on belegten 3 von 4 Plaetzen)
        bool isSwitch (const juce::String& name)
        {
            auto tokens = juce::StringArray::fromTokens (name.toLowerCase().replaceCharacters ("_-", "  "), " ", {});
            tokens.removeEmptyStrings();

            if (tokens.isEmpty())
                return false;

            static const juce::StringArray last { "on", "off", "enable", "enabled", "bypass", "active", "mode", "type", "waveform", "wave" };
            static const juce::StringArray any { "arp", "sync", "tempo", "retrigger", "bpm", "seq", "stutter", "voices", "octave", "octaves" };

            if (last.contains (tokens[tokens.size() - 1]))
                return true;

            for (auto& t : tokens)
                if (any.contains (t))
                    return true;

            return name.containsIgnoreCase ("beats per minute") || name.containsIgnoreCase ("beats_per_minute");
        }

        // needs aus dem Steckbrief erfuellt? Sie nennen die Schalter, mit
        // denen gemessen wurde ("Filter Type = lowpass", "Reverb Mix > 0").
        // Wirkung hat der Regler, wenn der Schalter ueberhaupt an ist: "X = v"
        // gilt, solange X nicht aus steht (none/off/false/0), "X > 0" wie
        // geschrieben. "settings on (…)" (gemessen mit allen Effekten an)
        // gilt als nicht erfuellt — der Regler haengt an einem Effekt
        bool needsMet (te::Plugin& p, const juce::String& need)
        {
            if (need.startsWithIgnoreCase ("settings on"))
                return false;

            auto isOff = [] (const juce::String& t)
            {
                auto x = t.trim().toLowerCase();
                return x.isEmpty() || x == "none" || x == "off" || x == "false" || x == "0" || x == "bypass" || x == "bypassed";
            };

            for (auto op : { " = ", " > " })
            {
                const int at = need.indexOf (op);

                if (at <= 0)
                    continue;

                const auto name = need.substring (0, at).trim();
                const bool eq = juce::String (op) == " = ";

                for (auto& st : pluginSettings (p))
                    if (st.name.equalsIgnoreCase (name))
                    {
                        auto cur = st.get();

                        if (cur.isBool())
                            return (bool) cur;

                        return eq ? ! isOff (cur.toString()) : (double) cur > 0.0;
                    }

                if (auto* prm = findParameter (p, name))
                    return eq ? ! isOff (prm->getCurrentValueAsString()) : prm->getCurrentNormalisedValue() > 0.001f;

                return true;
            }

            // nur ein Name (Steckbrief Version 2: "filter_on"): der Schalter ist an
            if (auto* prm = findParameter (p, need.trim()))
                return prm->getCurrentNormalisedValue() > 0.5f && ! isOff (prm->getCurrentValueAsString());

            return true;
        }

        struct TuneParam
        {
            juce::String name, reason, fromText;
            float from = 0.0f;
        };

        struct TuneWork : SteppedWork
        {
            juce::String varyHint;   // "renders vary" (PluginRandomness), oder ""
            MeasuredSound ref;
            Profile refProfile;
            CompareOptions opts;
            double rhythm = nan;           // onsetMatch der notes
            int dropped = 0;
            std::vector<int> velocities;   // aus notes
            Bench bench;
            juce::String moduleId;
            te::EditItemID moduleItem;
            const te::Edit* edit = nullptr;
            juce::ValueTree moduleState;
            juce::StringArray wanted;
            int maxParams = 4, budget = 30, renders = 0;
            juce::uint32 deadline = 0;
            bool apply = true, started = false, improvedRound = false;
            std::vector<TuneParam> params;
            std::vector<float> bestX;
            Eval start, best;
            float stepSize = 0.25f;
            size_t p = 0;
            int dir = 0;
            juce::String stopped;
            juce::Array<juce::var> history;

            te::AutomatableParameter* param (size_t i) { return findParameter (*bench.plugin, params[i].name); }

            void set (size_t i, float v)
            {
                if (auto* prm = param (i))
                    prm->setNormalisedParameter (v, juce::sendNotification);
            }

            // Parameter waehlen: angegeben, sonst nach Steckbrief die, die die
            // groessten Teile des Abstands bewegen (3–6), ohne Stimmung/Pegel
            juce::String choose()
            {
                auto& plugin = *bench.plugin;

                if (! wanted.isEmpty())
                {
                    for (auto& n : wanted)
                    {
                        auto* prm = findParameter (plugin, n);

                        if (prm == nullptr)
                            return "no such parameter: " + n + " (listParameters)";

                        params.push_back ({ parameterLabel (plugin, *prm), "given", {}, 0.0f });
                    }

                    return {};
                }

                std::array<double, numParts> c {};

                for (int i = 0; i < numParts; ++i)
                    c[(size_t) i] = known (start.parts[(size_t) i]) ? partWeights[i] * start.parts[(size_t) i] : 0.0;

                const auto measured = loadProfile (slugOf (plugin)).getProperty ("measured", {}).getProperty ("parameters", {});
                // je Parameter, welche Teile des Abstands er bewegt (Staerke
                // je Messgroesse laut Steckbrief: strong 1, medium 0,7, weak 0,5)
                struct Scored
                {
                    TuneParam param;
                    std::array<double, numParts> touch {};
                    bool taken = false;
                };

                std::vector<Scored> scored;

                for (auto* prm : plugin.getAutomatableParameters())
                {
                    const auto label = parameterLabel (plugin, *prm);

                    if (notTimbre (label) || isSwitch (label))
                        continue;

                    Scored x;
                    juce::StringArray why;
                    const auto lower = label.toLowerCase();

                    if (lower.contains ("portamento") || lower.contains ("glide"))
                        x.touch[pGlide] = 1.0, why.add ("glide (by name)");

                    // ohne Steckbrief oder dort ohne Wirkung ("no audible effect
                    // with the test signal" — das Testsignal regt manches
                    // nicht an, Todo 2026-10-18): nach dem Namen, halb gewichtet
                    auto byName = [&] (double weight)
                    {
                        const std::pair<const char*, Part> keys[] = {
                            { "attack", pEnvelope }, { "decay", pEnvelope }, { "sustain", pEnvelope }, { "release", pEnvelope },
                            { "cutoff", pSpectrum }, { "freq", pSpectrum }, { "resonance", pSpectrum }, { "bright", pSpectrum },
                            { "tone", pSpectrum }, { "drive", pSpectrum }, { "width", pStereo }, { "spread", pStereo },
                            { "detune", pStereo }, { "unison", pStereo }, { "chorus", pStereo },
                        };

                        for (auto& [k, part] : keys)
                            if (lower.contains (k))
                            {
                                x.touch[(size_t) part] = weight;
                                why.add (juce::String (partNames[part]) + " (by name)");
                                break;
                            }
                    };

                    if (measured.isObject())
                    {
                        auto m = profileMeasurement (measured, label);
                        bool met = m.isObject();

                        if (auto* needs = m.getProperty ("needs", {}).getArray())
                            for (auto& n : *needs)
                                met = met && needsMet (plugin, n.toString());

                        const auto strength = m.getProperty ("strength", {}).toString();
                        const double sw = strength == "strong" ? 1.0 : strength == "medium" ? 0.7 : strength == "weak" ? 0.5 : 0.0;

                        if (auto* does = m.getProperty ("does", {}).getArray(); does != nullptr && met && sw > 0.0)
                            for (auto& d : *does)
                            {
                                const auto metric = d.getProperty ("metric", {}).toString();
                                // Staerke je Messgroesse (ratio = Aenderung / Schwelle, Stufen
                                // wie profile_plugin.strength): Version 2 misst mehr Groessen,
                                // Nebenwirkungen (Amp Attack: brightness 1,3) zaehlten mit der
                                // Staerke des ganzen Reglers sonst so viel wie seine Hauptwirkung
                                // und alle Regler banden gleich (Todo 2026-10-15, Testphrase v2)
                                const double ratio = d.getProperty ("ratio", 0.0);
                                const auto level = ratio >= 5.0 ? juce::String ("strong") : ratio >= 2.0 ? juce::String ("medium")
                                                 : ratio > 0.0 ? juce::String ("weak") : strength;
                                const double mw = level == "strong" ? 1.0 : level == "medium" ? 0.7 : level == "weak" ? 0.5 : sw;

                                // Messgroessen des Steckbriefs (Version 2 mit sustain,
                                // sweep, glide, beating, detune, correlation) -> Teile des
                                // Abstands; detune wie Detune/Unison nach dem Namen: Stereo
                                auto touch = [&] (Part part) { x.touch[(size_t) part] = juce::jmax (x.touch[(size_t) part], mw); };

                                if (metric == "attack" || metric == "tail" || metric == "sustain" || metric == "beating")
                                    touch (pEnvelope);
                                else if (metric == "brightness")
                                    touch (pSpectrum), touch (pMotion);
                                else if (metric == "low" || metric == "high")
                                    touch (pSpectrum);
                                else if (metric == "sweep")
                                    touch (pMotion);
                                else if (metric == "width" || metric == "correlation" || metric == "detune")
                                    touch (pStereo);
                                else if (metric == "glide")
                                    touch (pGlide);
                                else if (metric != "vibrato")
                                    continue;   // lufs, pitch, velocity: Pegel und Stimmung

                                x.touch[pClap] = juce::jmax (x.touch[pClap], 0.25 * mw);
                                why.addIfNotAlreadyThere (metric + " (" + level + ")");
                            }

                        if (met && why.isEmpty() && (m.getProperty ("strength", {}).toString() == "none"))
                            byName (0.5);
                    }
                    else
                        byName (1.0);

                    x.param = { label, why.joinIntoString (", "), {}, 0.0f };

                    if (! why.isEmpty())
                        scored.push_back (x);
                }

                // der Reihe nach den besten; was er bewegt, zaehlt danach
                // halb — die naechsten Plaetze gehen an andere Teile
                // (Huellkurve, Stereo …), nicht an fuenf Helligkeitsregler
                while ((int) params.size() < maxParams)
                {
                    int bestI = -1;
                    double bestScore = 0.0;

                    for (size_t i = 0; i < scored.size(); ++i)
                    {
                        double sc = 0.0;

                        for (int k = 0; k < numParts; ++k)
                            sc += scored[i].touch[(size_t) k] * c[(size_t) k];

                        if (! scored[i].taken && sc > bestScore + 1.0e-9)
                            bestScore = sc, bestI = (int) i;
                    }

                    if (bestI < 0)
                        break;

                    scored[(size_t) bestI].taken = true;
                    params.push_back (scored[(size_t) bestI].param);

                    for (int k = 0; k < numParts; ++k)
                        if (scored[(size_t) bestI].touch[(size_t) k] > 0.0)
                            c[(size_t) k] *= 0.5;
                }

                if (params.empty())
                    return juce::String::fromUTF8 ("no parameter found that moves what differs — give params: [names] (listParameters)");

                return {};
            }

            bool step (DawdContext& ctx, Job& job) override
            {
                if (! started)
                {
                    started = true;

                    if (auto err = benchPlugin (bench, moduleState, "module"); err.isNotEmpty())
                    {
                        start.error = err;
                        return false;
                    }

                    start = evaluate (ctx, bench, ref, refProfile, opts);
                    ++renders;

                    if (! start.ok)
                        return false;

                    // Pegelschutz ab hier: Verlust unter den Ausgangsklang
                    // bzw. die Referenz (das Leisere) im Hauptbereich der
                    // Referenz zaehlt ab 3 dB
                    opts.baselineDb = juce::jmin (start.levelDb, opts.refLevelDb);
                    opts.toleranceDb = tuneWarnDb;
                    start.levelPart = 0.0;

                    if (auto err = choose(); err.isNotEmpty())
                    {
                        start.ok = false;
                        start.error = err;
                        return false;
                    }

                    for (size_t i = 0; i < params.size(); ++i)
                        if (auto* prm = param (i))
                        {
                            params[i].from = prm->getCurrentNormalisedValue();
                            params[i].fromText = prm->getCurrentValueAsString();
                            bestX.push_back (params[i].from);
                        }

                    best = start;
                    history.add (num (start.distance, 0.001));
                    return true;
                }

                if (renders >= budget)
                {
                    stopped = "budget";
                    return false;
                }

                if (juce::Time::getMillisecondCounter() >= deadline)
                {
                    stopped = "time";
                    return false;
                }

                // Koordinatensuche: je Parameter +step, -step; ein Durchgang
                // ohne Gewinn halbiert die Schrittweite
                for (;;)
                {
                    if (p >= params.size())
                    {
                        p = 0;
                        dir = 0;

                        if (! improvedRound)
                        {
                            stepSize *= 0.5f;

                            if (stepSize < 1.0f / 64.0f)
                            {
                                stopped = "converged";
                                return false;
                            }
                        }

                        improvedRound = false;
                    }

                    const size_t i = p;
                    const int d = dir;
                    const float v = juce::jlimit (0.0f, 1.0f, bestX[i] + (d == 0 ? stepSize : -stepSize));

                    if (d == 0)
                        dir = 1;
                    else
                        dir = 0, ++p;

                    if (std::abs (v - bestX[i]) < 1.0e-4f)
                        continue;

                    auto* prm = param (i);

                    if (prm == nullptr)
                        continue;

                    const auto before = prm->getCurrentValueAsString();
                    set (i, v);

                    // gestufte Regler: gleicher Text, gleicher Klang — kein Render
                    if (prm->getCurrentValueAsString() == before)
                    {
                        set (i, bestX[i]);
                        continue;
                    }

                    auto e = evaluate (ctx, bench, ref, refProfile, opts);
                    ++renders;
                    job.progress = 0.98 * juce::jmin (1.0, (double) renders / (double) budget);

                    if (e.ok && e.distance < best.distance - 0.005)
                    {
                        best = e;
                        bestX[i] = v;
                        improvedRound = true;
                        history.add (num (e.distance, 0.001));

                        if (d == 0)
                            dir = 0, ++p;   // nach oben besser: die andere Richtung nicht mehr
                    }
                    else
                        set (i, bestX[i]);

                    return true;
                }
            }

            void finish (DawdContext& ctx, Job& job) override
            {
                if (job.cancel)
                    stopped = "cancelled";

                if (! start.ok)
                {
                    setResult (job, {}, start.error.isNotEmpty() ? start.error : juce::String ("cannot measure the current sound"));
                    return;
                }

                auto res = new juce::DynamicObject();
                res->setProperty ("plugin", moduleId);
                res->setProperty ("reference", referenceVar (ref, dropped, rhythm));
                res->setProperty ("compare", opts.soundOnly ? "sound" : "all");
                res->setProperty ("renders", renders);
                res->setProperty ("stopped", stopped);

                auto dist = new juce::DynamicObject();
                dist->setProperty ("before", num (start.distance, 0.001));
                dist->setProperty ("after", num (best.distance, 0.001));
                res->setProperty ("distance", juce::var (dist));
                auto parts = new juce::DynamicObject();
                parts->setProperty ("before", partsVar (start));
                parts->setProperty ("after", partsVar (best));
                res->setProperty ("parts", juce::var (parts));
                res->setProperty ("history", history);

                // Pegel im Hauptbereich der Referenz gegen die Referenz (dB)
                const double lossDb = start.levelDb - best.levelDb;
                {
                    auto lv = new juce::DynamicObject();
                    lv->setProperty ("before", db1 (start.levelDb - opts.refLevelDb));
                    lv->setProperty ("after", db1 (best.levelDb - opts.refLevelDb));
                    lv->setProperty ("range", mainRangeText (opts));
                    res->setProperty ("levelVsReference", juce::var (lv));
                }

                juce::Array<juce::var> list;
                auto values = new juce::DynamicObject();

                for (size_t i = 0; i < params.size() && i < bestX.size(); ++i)
                {
                    set (i, bestX[i]);
                    auto o = new juce::DynamicObject();
                    o->setProperty ("name", params[i].name);
                    o->setProperty ("from", params[i].fromText);

                    if (auto* prm = param (i))
                        o->setProperty ("to", prm->getCurrentValueAsString());

                    o->setProperty ("value", num (bestX[i], 0.0001));
                    o->setProperty ("changed", std::abs (bestX[i] - params[i].from) > 1.0e-4f);
                    o->setProperty ("why", params[i].reason);
                    list.add (juce::var (o));

                    if (std::abs (bestX[i] - params[i].from) > 1.0e-4f)
                        values->setProperty (juce::Identifier (params[i].name), bestX[i]);
                }

                res->setProperty ("params", list);
                res->setProperty ("worst", juce::var (best.worst));

                auto cmd = new juce::DynamicObject();
                const juce::var cmdVar (cmd);   // haelt den Befehl: executeOp gibt seine Kopie frei
                cmd->setProperty ("op", "setParameter");
                cmd->setProperty ("plugin", moduleId);
                cmd->setProperty ("values", juce::var (values));
                const bool better = best.distance < start.distance - 0.01 && ! values->getProperties().isEmpty();
                // Pegelschutz: deutlich leiser als vorher wird nicht gesetzt
                // (Praxistest remix-1: Bass -19 -> -51 LUFS galt als naeher)
                const bool quieter = better && known (lossDb) && lossDb > tuneMaxLossDb;
                bool applied = false;
                juce::String note;
                juce::StringArray hints;

                if (better && known (lossDb) && lossDb > tuneWarnDb)
                    hints.add (juce::String::fromUTF8 ("the closer setting is ") + juce::String (juce::roundToInt (lossDb))
                               + juce::String::fromUTF8 (" dB quieter than before in the reference's main range (") + mainRangeText (opts)
                               + juce::String::fromUTF8 (") — a filter or envelope that takes the sound away can measure closer; listen")
                               + (quieter ? juce::String::fromUTF8 (" (more than 6 dB: not applied)") : juce::String::fromUTF8 (" (applied)")));

                if (quieter)
                {
                    note = juce::String::fromUTF8 ("not applied: the result is ") + juce::String (juce::roundToInt (lossDb))
                         + juce::String::fromUTF8 (" dB quieter than before — run setParameter yourself if it still fits, or give params without that knob");
                }
                else if (better && apply && ! job.cancel)
                {
                    // Projekt gewechselt (anderer Node, Edit neu geladen)? Dann
                    // nichts setzen — die Werte stehen im Befehl
                    auto* mt = ctx.edit != nullptr && ctx.edit.get() == edit ? findModuleTrack (*ctx.edit, moduleId) : nullptr;

                    if (mt == nullptr || mt->itemID != moduleItem)
                        note = juce::String::fromUTF8 ("the project changed while tuning (other node or module) — nothing applied; run setParameter yourself if it still fits");
                    else
                    {
                        auto r = executeOp (ctx, "setParameter", juce::JSON::parse (juce::JSON::toString (cmdVar)));
                        applied = (bool) r.getProperty ("ok", false);

                        if (! applied)
                            note = "setParameter failed: " + r.getProperty ("error", {}).toString();
                    }
                }
                else if (! better)
                    note = juce::String::fromUTF8 ("no closer setting found — nothing changed (try other params, a bigger budget or another preset via matchSound)");

                res->setProperty ("applied", applied);

                if (better)
                    res->setProperty ("setParameter", cmdVar);

                if (note.isNotEmpty())
                    res->setProperty ("note", note);
                else if (applied)
                    res->setProperty ("note", juce::String::fromUTF8 ("applied as one setParameter (one undo step)"));

                if (varyHint.isNotEmpty())
                    hints.add (varyHint);

                referenceHints (hints, ref, opts, rhythm);

                if (! hints.isEmpty())
                    res->setProperty ("hints", juce::var (hints));

                setResult (job, juce::var (res), job.cancel ? juce::String ("cancelled") : juce::String());
            }
        };

        // Modul im Projekt und sein Zustand (Plugin-Zustand geschrieben)
        te::Plugin* moduleFor (DawdContext& ctx, const juce::String& id, te::AudioTrack** track = nullptr)
        {
            auto* mt = findModuleTrack (*ctx.edit, id);
            auto* p = mt != nullptr ? modulePlugin (*mt) : nullptr;

            if (track != nullptr)
                *track = mt;

            if (p != nullptr)
                p->flushPluginStateToValueTree();

            return p;
        }

        bool isModuleId (const juce::String& s)
        {
            return s.startsWithChar ('p') && s.length() > 1 && s.substring (1).containsOnly ("0123456789");
        }
    }

    // matchSound {reference, plugin? | catalogs?, candidates?, limit?} (Job):
    // plugin = Modul im Projekt (sein aktueller Klang und seine Presets und
    // Klaenge), sonst die Kataloge (catalogs: Plugin-Namen, ohne = alle).
    // Vorauswahl nach CLAP wie findPresets {like} (candidates, Standard 8),
    // dann spielt jeder Kandidat die Noten der Referenz und wird gemessen.
    juce::var opMatchSound (DawdContext& ctx, const juce::var& cmd)
    {
        juce::var spec, like;

        if (auto err = referenceSpec (cmd, spec, like); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        const int count = juce::jlimit (1, 40, (int) cmd.getProperty ("candidates", 8));
        auto work = std::make_shared<MatchWork>();
        work->givenNotes = cmd.hasProperty ("notes");
        work->limit = juce::jlimit (1, 40, (int) cmd.getProperty ("limit", 5));

        // plugin: Modul-Id oder -Name; ein Plugin-Name meint den Katalog
        auto pluginArg = cmd.getProperty ("plugin", {}).toString();
        juce::StringArray catalogs;

        if (auto* a = cmd.getProperty ("catalogs", {}).getArray())
            for (auto& x : *a)
                catalogs.add (x.toString());
        else if (cmd.hasProperty ("catalogs"))
            catalogs.add (cmd.getProperty ("catalogs", {}).toString());

        te::Plugin* module = nullptr;

        if (pluginArg.isNotEmpty())
        {
            module = moduleFor (ctx, pluginArg);

            if (module == nullptr && isModuleId (pluginArg))
                return resultObj (false, { { "error", "no such plugin: " + pluginArg } });

            if (module == nullptr)
                catalogs.add (pluginArg);
        }

        if (auto err = compareMode (cmd, work->opts); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (auto err = measureReferenceFor (ctx, cmd, spec, work->ref, work->refProfile, work->dropped, work->velocities, work->opts, work->rhythm);
            err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        useCompare (work->opts, work->rhythm);

        // Pegelschutz: mehr als 18 dB unter der Referenz zaehlt (Presets
        // sind verschieden laut, ein Pegel gegen einen Stem streut)
        work->opts.baselineDb = work->opts.refLevelDb;
        work->opts.toleranceDb = 18.0;
        juce::StringArray notes;

        if (module != nullptr)
        {
            work->moduleId = moduleIdFor (*ctx.edit, pluginArg);
            work->moduleState = module->state.createCopy();
            work->varyHint = rendersVaryHint (*module);   // Matthias 2026-10-27, B
            Candidate cur;
            cur.kind = "current";
            cur.label = "(current sound)";
            work->cands.push_back (cur);

            // CLAP-Vorauswahl aus dem Katalog des Plugins, der Rest in der
            // Reihenfolge von listPresets (eigene zuerst)
            auto q = new juce::DynamicObject();
            q->setProperty ("plugin", work->moduleId);
            q->setProperty ("like", like);
            q->setProperty ("limit", count);
            auto found = opFindPresets (ctx, juce::var (q));
            std::set<juce::String> seen;

            if (auto* a = found.getProperty ("presets", {}).getArray())
                for (auto& h : *a)
                {
                    auto c = candidateOf (h, work->moduleId);
                    seen.insert (c.kind + c.label + c.preset.file.getFullPathName() + juce::String (c.preset.program));
                    work->cands.push_back (c);
                }
            else if (found.hasProperty ("error"))
                notes.add ("no CLAP preselection (" + found.getProperty ("error", {}).toString() + juce::String::fromUTF8 (") — presets in list order"));

            for (auto& e : presetsFor (ctx, *module))
            {
                if ((int) work->cands.size() > count)
                    break;

                Candidate c;
                c.kind = "preset";
                c.preset = e;
                c.label = e.category.isNotEmpty() ? e.category + "/" + e.name : e.name;

                if (seen.count (c.kind + c.label + e.file.getFullPathName() + juce::String (e.program)) > 0)
                    continue;

                c.load = loadCommand (work->moduleId, c);
                work->cands.push_back (c);
            }
        }
        else
        {
            if (auto e = clap::available(); e.isNotEmpty())
                return resultObj (false, { { "error", "the catalogs are preselected with CLAP: " + e + juce::String::fromUTF8 (" — or give plugin: a module id (its presets in list order)") } });

            std::vector<Candidate> all;

            for (int i = 0; i < juce::jmax (1, catalogs.size()); ++i)
            {
                auto q = new juce::DynamicObject();

                if (! catalogs.isEmpty())
                    q->setProperty ("name", catalogs[i]);

                q->setProperty ("like", like);
                q->setProperty ("limit", count);
                auto found = opFindPresets (ctx, juce::var (q));

                if (! (bool) found.getProperty ("ok", false))
                    return resultObj (false, { { "error", found.getProperty ("error", {}) } });

                if (auto* a = found.getProperty ("presets", {}).getArray())
                    for (auto& h : *a)
                        all.push_back (candidateOf (h, {}));
            }

            std::stable_sort (all.begin(), all.end(), [] (const Candidate& a, const Candidate& b) { return a.catalogScore > b.catalogScore; });

            if ((int) all.size() > count)
                all.resize ((size_t) count);

            // nach Plugin gruppiert rendern: jedes Plugin wird einmal geladen
            std::stable_sort (all.begin(), all.end(), [] (const Candidate& a, const Candidate& b)
                              { return a.pluginName + a.format < b.pluginName + b.format; });
            work->cands = std::move (all);
        }

        if (work->cands.empty())
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no candidates — no presets or catalogue entries with a CLAP vector (tools/profile_presets.py)") } });

        if (auto err = makeBench (ctx, work->ref, work->velocities, work->bench); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        const int n = (int) work->cands.size();
        auto res = startJob (ctx, "matchSound", work);
        res.getDynamicObject()->setProperty ("candidates", n);
        res.getDynamicObject()->setProperty ("reference", referenceVar (work->ref, work->dropped, work->rhythm));
        res.getDynamicObject()->setProperty ("compare", work->opts.soundOnly ? "sound" : "all");

        if (! notes.isEmpty())
            res.getDynamicObject()->setProperty ("note", notes.joinIntoString ("; "));

        return res;
    }

    // tuneSound {plugin, reference, params? | maxParams?, budget?, seconds?,
    // apply?} (Job, ein Undo-Schritt): Koordinatensuche ueber wenige
    // Parameter gegen den Abstand zur Referenz (wie matchSound gemessen),
    // auf einer Kopie des Moduls; am Ende ein setParameter am Modul.
    juce::var opTuneSound (DawdContext& ctx, const juce::var& cmd)
    {
        juce::var spec, like;

        if (auto err = referenceSpec (cmd, spec, like); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        const auto id = cmd.getProperty ("plugin", {}).toString();
        te::AudioTrack* mt = nullptr;
        auto* module = moduleFor (ctx, id, &mt);

        if (module == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        auto work = std::make_shared<TuneWork>();

        if (auto* a = cmd.getProperty ("params", {}).getArray())
            for (auto& x : *a)
                work->wanted.add (x.toString());
        else if (cmd.getProperty ("params", {}).isString())
            work->wanted.add (cmd.getProperty ("params", {}).toString());

        for (auto& n : work->wanted)
            if (findParameter (*module, n) == nullptr)
                return resultObj (false, { { "error", "no such parameter: " + n + " (listParameters)" } });

        work->maxParams = juce::jlimit (1, 6, (int) cmd.getProperty ("maxParams", 4));
        work->budget = juce::jlimit (4, 200, (int) cmd.getProperty ("budget", 30));
        const double seconds = juce::jlimit (10.0, 900.0, (double) cmd.getProperty ("seconds", 180.0));
        work->apply = (bool) cmd.getProperty ("apply", true);

        if (auto err = compareMode (cmd, work->opts); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (auto err = measureReferenceFor (ctx, cmd, spec, work->ref, work->refProfile, work->dropped, work->velocities, work->opts, work->rhythm);
            err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        useCompare (work->opts, work->rhythm);
        work->moduleId = moduleIdFor (*ctx.edit, id);
        work->moduleItem = mt->itemID;
        work->varyHint = rendersVaryHint (*module);   // Matthias 2026-10-27, B
        work->edit = ctx.edit.get();
        work->moduleState = module->state.createCopy();
        work->deadline = juce::Time::getMillisecondCounter() + (juce::uint32) (seconds * 1000.0);

        if (auto err = makeBench (ctx, work->ref, work->velocities, work->bench); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto res = startJob (ctx, "tuneSound", work);
        res.getDynamicObject()->setProperty ("plugin", work->moduleId);
        res.getDynamicObject()->setProperty ("budget", work->budget);
        res.getDynamicObject()->setProperty ("reference", referenceVar (work->ref, work->dropped, work->rhythm));
        res.getDynamicObject()->setProperty ("compare", work->opts.soundOnly ? "sound" : "all");

        if (work->varyHint.isNotEmpty())
            res.getDynamicObject()->setProperty ("hints", juce::var (juce::StringArray (work->varyHint)));

        return res;
    }
}
