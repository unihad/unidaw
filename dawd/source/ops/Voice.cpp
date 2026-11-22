#include "Ops.h"
#include "AnalysisSource.h"
#include "SoundMatch.h"
#include "../VoiceAnalysis.h"
#include "../Separation.h"
#include "../GuidedSplit.h"
#include "../Transcription.h"
#include "../Clap.h"
#include "../EngineHelpers.h"
#include "../PluginSettings.h"

#include <array>
#include <numeric>
#include <map>
#include <mutex>
#include <set>

// Musikalitaet und Sounddesign einer Spur (Familie processing, read-only):
// describeVoice (wie gespielt wird), describeTimbre (wie es klingt),
// compareVoice (Referenz-Stem gegen die eigene Spur, Abweichungen mit
// Vorschlag). Die Signalverarbeitung steht in VoiceAnalysis.cpp.
namespace dawd
{
    namespace
    {
        bool known (double v) { return ! std::isnan (v); }
        juce::var num (double v, double step = 0.01) { return known (v) ? juce::var (std::round (v / step) * step) : juce::var(); }

        struct VoiceData
        {
            AnalysisSource src;
            juce::String gridFrom;
            double bpm = 0.0;
            voice::Voice v;
            voice::Timbre tb;
            bool timbre = false;
            juce::StringArray classes;    // CLAP: "synth pad 0.31", bestes zuerst
            std::vector<float> classScores;   // je classLabels()-Eintrag
            juce::String plugin;          // Instrument der Spur (eigene Seite), sonst leer
            int minPitch = 21, maxPitch = 108;
            double highPassHz = 0.0;      // Klang mit minPitch: alles darunter gefiltert

            // Instrumenten-Zeitleiste: CLAP je Takt (bzw. so viele Takte,
            // dass ein Fenster >= 1,5 s lang ist) — ein Instrument, das nur
            // in einem Teil der Takte spielt, verschwindet im Mittel ueber
            // den Bereich (Praxistest 1e: der zweite Synth ab Takt 2)
            struct Window { int firstBar = 0, lastBar = 0; double start = 0.0, end = 0.0; std::vector<float> scores; juce::StringArray top; };
            struct Change { int bar = 0; double time = 0.0, distance = 0.0; juce::StringArray more, less; };
            std::vector<Window> timeline;
            std::vector<Change> changes;
        };

        // Instrument-Klassen fuer CLAP (Englisch, wie die Text-Suche)
        const juce::StringArray& classLabels()
        {
            static const juce::StringArray labels { "synth pad", "synth lead", "synth bass", "pluck synth", "electric piano",
                                                    "acoustic piano", "organ", "string ensemble", "brass section",
                                                    "clean electric guitar", "distorted electric guitar", "acoustic guitar",
                                                    "bass guitar", "bell", "choir" };
            return labels;
        }

        // embedding (optional): der CLAP-Vektor selbst (matchSound/tuneSound)
        juce::StringArray classify (const std::vector<std::vector<float>>& stereo, double rate, std::vector<float>& scores,
                                    std::vector<float>* embedding = nullptr)
        {
            juce::StringArray out;
            scores.clear();

            if (clap::available().isNotEmpty() || stereo.empty() || stereo[0].empty())
                return out;

            // Mono auf 48 kHz, hoechstens 10 s
            std::vector<float> mono (stereo[0].size());

            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = stereo.size() > 1 ? 0.5f * (stereo[0][i] + stereo[1][i]) : stereo[0][i];

            const double ratio = rate / clap::sampleRate;
            std::vector<float> m48 ((size_t) juce::jmin ((double) clap::windowSamples, (double) mono.size() / ratio));
            juce::LagrangeInterpolator interp;
            interp.process (ratio, mono.data(), m48.data(), (int) m48.size(), (int) mono.size(), 0);

            static std::vector<std::vector<float>> textVecs;
            juce::String err;

            // erst ganz fuellen, dann merken — ein Fehler mittendrin liesse
            // sonst einen halben Vektor fuer spaetere Aufrufe stehen
            if (textVecs.empty())
            {
                std::vector<std::vector<float>> vecs;

                for (auto& l : classLabels())
                {
                    auto t = clap::embedText ("the sound of a " + l, err);

                    if (t.empty())
                        return out;

                    vecs.push_back (t);
                }

                textVecs = std::move (vecs);
            }

            // ein Encoder fuer alle Aufrufe: jeder neue oeffnet eine eigene
            // ONNX-Sitzung (~2 s) — die Zeitleiste ruft je Takt. Absichtlich
            // nie freigegeben: die ONNX-Umgebung ist selbst statisch, die
            // Reihenfolge beim Beenden waere sonst Zufall
            static std::mutex encLock;
            static auto& enc = *new clap::AudioEncoder();
            std::vector<float> a;

            {
                const std::lock_guard<std::mutex> g (encLock);
                a = enc.embed (m48.data(), m48.size(), err);
            }

            if (a.size() < (size_t) clap::dim)
                return out;

            a.resize ((size_t) clap::dim);

            if (embedding != nullptr)
                *embedding = a;

            std::vector<std::pair<float, int>> sims;

            for (int i = 0; i < classLabels().size(); ++i)
            {
                sims.push_back ({ clap::similarity (a, textVecs[(size_t) i]), i });
                scores.push_back (sims.back().first);
            }

            std::sort (sims.begin(), sims.end(), [] (auto& x, auto& y) { return x.first > y.first; });

            for (size_t i = 0; i < 3; ++i)
                out.add (classLabels()[sims[i].second] + " " + juce::String (sims[i].first, 2));

            return out;
        }

        // Wechsel: Grenze zwischen zwei Fenstern, an der das Mittel der
        // (bis zu) zwei Fenster danach deutlich vom Mittel der zwei davor
        // abweicht — deutlich heisst: mindestens 1,8-mal das typische
        // Schwanken des Stems von Takt zu Takt (Median benachbarter
        // Abstaende; ein Bass-Stem schwankt in CLAP staerker als ein Pad)
        // und mindestens 0,15; gemeldet wird nur das lokale Maximum. Was
        // mehr und was weniger klingt, nennen die Klassen mit der groessten
        // Differenz.
        constexpr double changeFactor = 1.8, changeMin = 0.15, labelStep = 0.03;

        void buildTimeline (VoiceData& d)
        {
            const double rate = AnalysisSource::stereoRate;
            const auto& st = d.src.stereo;

            if (st.empty() || st[0].empty())
                return;

            const double length = (double) st[0].size() / rate;
            std::vector<VoiceData::Window> ws;

            if (d.v.hasGrid && ! d.v.bars.empty())
            {
                VoiceData::Window cur;
                bool open = false;

                for (auto& b : d.v.bars)
                {
                    if (b.end <= 0.0 || b.start >= length)
                        continue;

                    if (! open)
                        cur = {}, cur.firstBar = b.bar, cur.start = juce::jmax (0.0, b.start), open = true;

                    cur.lastBar = b.bar;
                    cur.end = juce::jmin (length, b.end);

                    if (cur.end - cur.start >= 1.5)
                        ws.push_back (cur), open = false;
                }

                if (open && cur.end - cur.start >= 1.0)
                    ws.push_back (cur);
            }
            else
                for (double t = 0.0; t + 1.5 <= length; t += 2.0)
                {
                    VoiceData::Window w;
                    w.start = t;
                    w.end = juce::jmin (length, t + 2.0);
                    ws.push_back (w);
                }

            // stille Fenster auslassen (40 dB unter dem lautesten): CLAP
            // ordnet auch der Stille Klassen zu, ihr Rand galt sonst als
            // Wechsel (Praxistest 1f)
            {
                std::vector<double> db;

                for (auto& w : ws)
                {
                    double sum = 0.0;
                    size_t n = 0;

                    for (auto& c : st)
                        for (auto i = (size_t) (w.start * rate); i < juce::jmin (c.size(), (size_t) (w.end * rate)); ++i)
                            sum += (double) c[i] * c[i], ++n;

                    db.push_back (10.0 * std::log10 (sum / (double) juce::jmax ((size_t) 1, n) + 1.0e-12));
                }

                const double loudest = db.empty() ? 0.0 : *std::max_element (db.begin(), db.end());
                std::vector<VoiceData::Window> sounding;

                for (size_t i = 0; i < ws.size(); ++i)
                    if (db[i] >= loudest - 40.0)
                        sounding.push_back (ws[i]);

                ws = sounding;
            }

            if (ws.size() < 3)
                return;

            for (auto& w : ws)
            {
                std::vector<std::vector<float>> part;

                for (auto& c : st)
                {
                    const auto a = (size_t) (w.start * rate), e = juce::jmin (c.size(), (size_t) (w.end * rate));
                    part.emplace_back (c.begin() + (long) a, c.begin() + (long) juce::jmax (a, e));
                }

                w.top = classify (part, rate, w.scores);

                if (w.scores.empty())
                    return;   // kein CLAP: keine Zeitleiste
            }

            d.timeline = ws;
            const auto& labels = classLabels();
            const size_t L = ws[0].scores.size(), n = ws.size();

            auto meanOf = [&] (size_t a, size_t b)   // Fenster [a, b)
            {
                std::vector<double> m (L, 0.0);

                for (size_t i = a; i < b; ++i)
                    for (size_t k = 0; k < L; ++k)
                        m[k] += ws[i].scores[k] / (double) (b - a);

                return m;
            };

            auto dist = [&] (const std::vector<double>& x, const std::vector<double>& y)
            {
                double q = 0.0;

                for (size_t k = 0; k < L; ++k)
                    q += (x[k] - y[k]) * (x[k] - y[k]);

                return std::sqrt (q);
            };

            std::vector<double> neighbour;

            for (size_t i = 1; i < n; ++i)
                neighbour.push_back (dist (meanOf (i - 1, i), meanOf (i, i + 1)));

            std::sort (neighbour.begin(), neighbour.end());
            const double noise = neighbour[neighbour.size() / 2];
            const double threshold = juce::jmax (changeMin, changeFactor * noise);
            std::vector<double> D (n, 0.0);

            for (size_t i = 1; i < n; ++i)
                D[i] = dist (meanOf (i >= 2 ? i - 2 : 0, i), meanOf (i, juce::jmin (n, i + 2)));

            for (size_t i = 1; i < n; ++i)
            {
                const bool peak = D[i] >= D[i - 1] && (i + 1 >= n || D[i] >= D[i + 1]);

                if (std::getenv ("UNIDAW_TIMELINE_DEBUG") != nullptr)
                    std::fprintf (stderr, "timeline boundary before window %zu (bars %d-%d) D %.3f noise %.3f threshold %.3f%s\n",
                                  i, ws[i].firstBar, ws[i].lastBar, D[i], noise, threshold, peak ? " peak" : "");

                if (! peak || D[i] < threshold)
                    continue;

                VoiceData::Change c;
                c.bar = ws[i].firstBar;
                c.time = ws[i].start;
                c.distance = D[i];
                const auto before = meanOf (i >= 2 ? i - 2 : 0, i), after = meanOf (i, juce::jmin (n, i + 2));
                std::vector<std::pair<double, int>> diff;
                double shift = 0.0;

                for (size_t k = 0; k < L; ++k)
                    shift += (after[k] - before[k]) / (double) L;

                // relativ zur mittleren Verschiebung: kommt ein Lead dazu,
                // sinken oft alle Werte, die Lead-Klasse aber weniger
                for (size_t k = 0; k < L; ++k)
                    diff.push_back ({ after[k] - before[k] - shift, (int) k });

                std::sort (diff.begin(), diff.end());

                for (auto it = diff.rbegin(); it != diff.rend() && c.more.size() < 3; ++it)
                    if (it->first >= labelStep)
                        c.more.add (labels[it->second]);

                for (auto it = diff.begin(); it != diff.end() && c.less.size() < 3; ++it)
                    if (it->first <= -labelStep)
                        c.less.add (labels[it->second]);

                d.changes.push_back (c);
            }
        }

        // timeline: CLAP je Takt (describeTimbre); matchSound/tuneSound brauchen sie nicht
        juce::String analyse (DawdContext& ctx, const juce::var& spec, VoiceData& d, bool withTimbre, bool timeline = true,
                              std::vector<float>* embedding = nullptr)
        {
            if (auto e = transcription::available(); e.isNotEmpty())
                return e;

            d.src.want22k = d.src.wantMono = d.src.wantStereo = true;
            d.src.ml = true;   // basic-pitch/CLAP: Gruppe A der Lizenz-Sperre

            if (auto e = loadAnalysisSource (ctx, spec, d.src); e.isNotEmpty())
                return e;

            transcription::Options o;
            o.minPitch = (int) spec.getProperty ("minPitch", 21);
            o.maxPitch = (int) spec.getProperty ("maxPitch", 108);

            if (o.minPitch < 0 || o.maxPitch > 127 || o.minPitch > o.maxPitch)
                return "need 0 <= minPitch <= maxPitch <= 127";

            juce::String err;
            transcription::Contour contour;
            auto notes = transcription::transcribe (d.src.audio22k, o, err, &contour);

            if (err.isNotEmpty())
                return err;

            auto features = analysis::computeFeatures (d.src.audio);
            analysis::TempoResult t;

            if (auto e = analysisGrid (ctx, spec, d.src, features, t, d.gridFrom); e.isNotEmpty())
                return e;

            voice::Grid g;
            g.beats = t.beats;
            g.downbeats = t.downbeats;
            g.beatsPerBar = t.beatsPerBar;
            g.valid = t.valid;
            d.bpm = t.bpm;
            d.minPitch = o.minPitch;
            d.maxPitch = o.maxPitch;
            d.v = voice::describeVoice (notes, contour, d.src.stereo, AnalysisSource::stereoRate, g);

            if (withTimbre)
            {
                // Mit minPitch misst der Klang ohne das, was darunter klingt
                // (Praxistest 1h: nur das Spektrum der Noten war gefiltert,
                // Huellkurve und Zeitleiste hoerten Bass und Pad mit): Hochpass
                // zwei Halbtoene unter minPitch, 4. Ordnung. Ueber maxPitch geht
                // das nicht — ein Tiefpass naehme die Obertoene mit.
                std::vector<std::vector<float>> full;

                if (o.minPitch > 21)
                {
                    full = d.src.stereo;
                    d.highPassHz = 440.0 * std::pow (2.0, (o.minPitch - 2 - 69) / 12.0);

                    for (auto& ch : d.src.stereo)
                        for (int pass = 0; pass < 2; ++pass)
                        {
                            juce::IIRFilter hp;
                            hp.setCoefficients (juce::IIRCoefficients::makeHighPass (AnalysisSource::stereoRate, d.highPassHz));
                            hp.processSamples (ch.data(), (int) ch.size());
                        }
                }

                d.tb = voice::describeTimbre (d.v, d.src.stereo, AnalysisSource::stereoRate);
                d.timbre = true;
                d.classes = classify (d.src.stereo, AnalysisSource::stereoRate, d.classScores, embedding);

                if (timeline)
                    buildTimeline (d);

                if (! full.empty())
                    d.src.stereo = std::move (full);
            }

            // Instrument der eigenen Spur (fuer Parameter-Vorschlaege)
            if (d.src.file.isEmpty() && d.src.track >= 0 && ctx.edit != nullptr)
            {
                auto tracks = userTracks (*ctx.edit);
                juce::String inst;

                if (d.src.track < tracks.size())
                    if (auto* group = laneGroupOf (*tracks[d.src.track]) != nullptr ? laneGroupOf (*tracks[d.src.track]) : tracks[d.src.track])
                        if (faderTrackOf (*ctx.edit, *group, nullptr, &inst) != nullptr)
                            d.plugin = inst;
            }

            return {};
        }

        juce::String role (const voice::Voice& v)
        {
            if (v.notes.empty())
                return "silent";

            const bool line = v.voices < 1.3;

            if (line && v.medianPitch < 48)
                return "bass line";

            if (line)
                return "single line (lead/melody)";

            if (known (v.legato) && v.legato >= 0.8)
                return v.voices < 2.4 ? "held dyads (pad-like)" : "held chords (pad)";

            if (known (v.legato) && v.legato < 0.6)
                return "short chord hits (comping/stabs)";

            return v.voices < 2.4 ? "two voices" : "chords";
        }

        juce::String voicesText (double voices)
        {
            return voices < 1.3 ? juce::String ("single line") : voices < 2.4 ? juce::String ("mostly two voices (dyads)")
                                                                               : "chords (~" + juce::String (voices, 1) + " voices)";
        }

        juce::StringArray voiceHints (const VoiceData& d)
        {
            const auto& v = d.v;
            juce::StringArray h;

            if (v.notes.empty())
                return { juce::String::fromUTF8 ("no notes found — is the source silent or unpitched (drums)?") };

            // stetige Masse (gehaltener Pegel, Neuanschlaege) — gezaehlte
            // Luecken kippen zwischen zwei Renders
            const double held = known (v.sustainShare) ? v.sustainShare : 1.0, re = known (v.attackDbPerBeat) ? v.attackDbPerBeat : 0.0;

            if (v.chordHits.layer)
                h.add (juce::String::fromUTF8 ("chords are re-struck over held notes: ") + juce::String (v.chordHits.perBar, 1)
                       + " chord hits per bar, " + juce::String (v.chordHits.hiddenPerBar, 1)
                       + juce::String::fromUTF8 (" of them hidden in the notes above (the same tones keep sounding, so they merge into held notes). Often a second instrument plays chords with pauses over a pad (two instrument classes per bar in describeTimbre's timeline point that way) — then build it as its own track: short chords where bars[].hits shows X (register and voicing in hitNotes")
                       + (v.chordHits.highest > 0 ? ", " + voice::noteName (v.chordHits.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (v.chordHits.highest) : juce::String())
                       + juce::String::fromUTF8 ("), the held notes are the pad; otherwise re-strike those chords in the same track. describeLayers {file, start, end} on the reference splits such a stem into its instruments and tells their balance"));

            // Neuanschlaege in klingenden Noten: Tonwiederholungen geteilt,
            // sonst das Urteil (Praxistest 1h: Tremolo oder Instrument?)
            if (const auto& ra = v.reattacks; ra.split > 0)
                h.add (juce::String (ra.split) + juce::String::fromUTF8 (" repeated notes re-struck without a level dip (on the 16th grid) — split into separate notes, the bars show each re-strike"));
            else if (ra.verdict == "played" && ! v.chordHits.layer && ! v.pulse.layer)
                h.add (juce::String::fromUTF8 ("re-attacks inside sounding notes follow the 16th grid (") + juce::String (ra.perBar, 1) + " per bar, "
                       + voice::noteName (ra.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (ra.highest)
                       + juce::String::fromUTF8 (") — played, not a tremolo: an instrument re-strikes these tones (its own track, or re-strike the notes)"));
            else if (ra.verdict == "periodic" && known (ra.intervalMs))
                h.add (juce::String::fromUTF8 ("the level re-attacks evenly every ~") + juce::String (juce::roundToInt (ra.intervalMs)) + " ms (~"
                       + juce::String (1000.0 / ra.intervalMs, 1)
                       + (v.hasGrid ? juce::String::fromUTF8 (" Hz), off the 16th grid — tremolo/LFO or detune beating, not a played part")
                                    : juce::String::fromUTF8 (" Hz) — tremolo/LFO or a played pulse: without a bar grid it cannot tell (give bpm + firstDownbeat)")));

            if (v.pulse.layer)
                h.add (juce::String::fromUTF8 ("a rhythmic part plays under the notes: ") + juce::String (v.pulse.perBar, 1) + " onsets per bar on "
                       + v.pulse.grid + juce::String::fromUTF8 (" (note detection sees ") + juce::String (v.pulse.notesPerBar, 1) + " note starts per bar)"
                       + (v.pulse.heldShare < 0.5 && known (v.pulse.decayMs) ? ", short (the new sound decays in ~" + juce::String (juce::roundToInt (v.pulse.decayMs)) + " ms)" : juce::String())
                       + (v.pulse.chord.isNotEmpty() ? ", tones " + v.pulse.chord : juce::String())
                       + juce::String::fromUTF8 (" — it re-strikes tones that already sound, so it is missing from the notes. Build it as its own track: short notes on bars[].pulse"));
            else if (held >= 0.9 && re < 10.0)
                h.add (juce::String::fromUTF8 ("legato — the level is held through the bar with hardly any re-attacks; hold each note to the next one"));
            else if (held < 0.6 || re >= 15.0)
                h.add (juce::String::fromUTF8 ("detached/re-attacked — level held ") + juce::String (juce::roundToInt (held * 100)) + "% of the bar, re-attacks "
                       + juce::String (re, 1) + " dB per beat");

            if (v.layers.size() >= 2)
            {
                juce::StringArray ls;

                // die Schicht der verdeckten Anschlaege hat keine Lage (nur
                // Tonklassen) — wie in voice.layers ohne Register
                for (auto& L : v.layers)
                    ls.add (L.role + (L.lowest > 0 ? " " + voice::noteName (L.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (L.highest) : juce::String()));

                h.add (juce::String (v.layers.size()) + juce::String::fromUTF8 (" separate layers (likely different instruments): ") + ls.joinIntoString ("; ")
                       + juce::String::fromUTF8 (" — build one track per layer"));
            }

            if (v.fragmentsMerged > 0)
                h.add (juce::String (v.fragmentsMerged) + juce::String::fromUTF8 (" fragments of held notes merged (note detection splits long tones — audioToMidi shows them split)"));

            // Anteil der Legato-Uebergaenge (Todo 2026-10-27-comparevoice-detune)
            if (v.glideShare >= 0.25)
                h.add ("slides in " + juce::String (juce::roundToInt (v.glideShare * v.legatoChanges)) + " of " + juce::String (v.legatoChanges)
                       + " legato note changes, ~" + juce::String (juce::roundToInt (v.glideMs))
                       + juce::String::fromUTF8 (" ms — portamento (mono/legato mode) or pitch bends"));

            if (v.vibratoShare >= 0.25)
                h.add ("vibrato on " + juce::String (juce::roundToInt (v.vibratoShare * 100)) + "% of notes, ~" + juce::String (juce::roundToInt (v.vibratoCents))
                       + " cents at " + juce::String (v.vibratoHz, 1) + " Hz");

            if (known (v.tuneCents) && std::abs (v.tuneCents) > 15.0)
                h.add ("tuned " + juce::String (juce::roundToInt (std::abs (v.tuneCents))) + " cents " + (v.tuneCents > 0 ? "sharp" : "flat"));

            h.add (voicesText (v.voices) + (v.maxVoices > 1 ? ", up to " + juce::String (v.maxVoices) : juce::String()));

            if (known (v.staggerMs) && v.staggerMs >= 40.0 && v.chordOnsets >= 2)
                h.add (juce::String::fromUTF8 ("voices enter staggered by ~") + juce::String (juce::roundToInt (v.staggerMs)) + juce::String::fromUTF8 (" ms (strummed/arpeggiated), not as a block"));

            if (known (v.swellDb) && v.swellDb >= 2.0)
                h.add (juce::String::fromUTF8 ("notes swell after the attack (+") + juce::String (v.swellDb, 1) + juce::String::fromUTF8 (" dB) — slow amp/filter envelope or volume swells"));

            if (known (v.andShare) && known (v.onBeatShare) && v.andShare > v.onBeatShare && v.andShare >= 0.3)
                h.add ("onsets mostly on the 'and' (offbeat eighths)");

            if (d.gridFrom == "detected")
                h.add (juce::String::fromUTF8 ("bar grid detected on this source — pass bpm + firstDownbeat from detectTempo of the full mix for exact bars"));

            return h;
        }

        juce::var voiceSummary (const VoiceData& d)
        {
            const auto& v = d.v;
            auto o = new juce::DynamicObject();
            o->setProperty ("role", role (v));
            o->setProperty ("notes", (int) v.notes.size());

            if (! v.notes.empty())
                o->setProperty ("register", voice::noteName (v.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (v.highest)
                                                + " (median " + voice::noteName (v.medianPitch) + ")");

            o->setProperty ("voices", num (v.voices, 0.1));
            o->setProperty ("maxVoices", v.maxVoices);
            o->setProperty ("legato", num (v.legato));
            o->setProperty ("gapsPerBar", num (v.gapsPerBar, 0.1));
            o->setProperty ("levelHeld", num (v.sustainShare));
            o->setProperty ("dipDb", num (v.dipDb, 0.1));
            o->setProperty ("reattackDbPerBeat", num (v.attackDbPerBeat, 0.1));
            o->setProperty ("staggerMs", num (v.staggerMs, 1.0));
            o->setProperty ("legatoChanges", v.legatoChanges);
            o->setProperty ("glideShare", num (v.glideShare));
            o->setProperty ("glideMs", num (v.glideMs, 1.0));
            o->setProperty ("vibratoShare", num (v.vibratoShare));
            o->setProperty ("tuneCents", num (v.tuneCents, 1.0));
            o->setProperty ("swellDb", num (v.swellDb, 0.1));
            o->setProperty ("medianNoteBeats", num (v.medianNoteBeats));

            // Schichten (mehrere Instrumente im Stem), nur wenn klar getrennt
            if (v.layers.size() >= 2)
            {
                juce::Array<juce::var> ls;

                for (auto& L : v.layers)
                    ls.add (juce::var (new juce::DynamicObject()));

                for (int i = 0; i < ls.size(); ++i)
                {
                    auto& L = v.layers[(size_t) i];
                    auto* lo = ls.getReference (i).getDynamicObject();
                    lo->setProperty ("role", L.role);
                    lo->setProperty ("notes", L.notes);
                    if (L.lowest > 0)
                        lo->setProperty ("register", voice::noteName (L.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (L.highest));

                    lo->setProperty ("medianNoteBeats", num (L.medianBeats));

                    // Noten der Schicht je Takt (+ gleichzeitig, ~ >= 1 Schlag) —
                    // zum Abschreiben in eine eigene Spur
                    juce::Array<juce::var> lb;

                    for (auto& b : d.v.bars)
                    {
                        juce::String txt;
                        double last = -1.0;

                        for (auto m : L.members)
                        {
                            if (m >= d.v.notes.size())
                                continue;

                            const auto& n = d.v.notes[m];

                            if (n.start < b.start - 0.03 || n.start >= b.end - 0.03)
                                continue;

                            if (txt.isNotEmpty())
                                txt << (n.start - last < 0.05 ? "+" : " ");

                            txt << voice::noteName (n.pitch) << (n.end - n.start >= 0.9 * (b.end - b.start) / 4.0 ? "~" : "");
                            last = n.start;
                        }

                        if (txt.isNotEmpty() && lb.size() < 16)
                        {
                            auto bo = new juce::DynamicObject();
                            bo->setProperty ("bar", b.bar);
                            bo->setProperty ("notes", txt);
                            lb.add (juce::var (bo));
                        }
                    }

                    if (! lb.isEmpty())
                        lo->setProperty ("bars", lb);
                }

                o->setProperty ("layers", ls);
            }

            // Akkordanschlaege ueber gehaltenen Noten (zweites Instrument)
            if (const auto& ch = d.v.chordHits; ch.layer)
            {
                auto co = new juce::DynamicObject();
                co->setProperty ("perBar", num (ch.perBar, 0.1));
                co->setProperty ("hiddenPerBar", num (ch.hiddenPerBar, 0.1));

                if (ch.highest > 0)
                    co->setProperty ("register", voice::noteName (ch.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (ch.highest));
                co->setProperty ("rhythmicLayer", true);
                o->setProperty ("chordHits", juce::var (co));
            }

            // Verdeckte Neuanschlaege in klingenden Noten: im Raster gespielt,
            // gleichmaessig (Tremolo) oder unregelmaessig
            if (const auto& ra = d.v.reattacks; ra.verdict.isNotEmpty())
            {
                auto ro = new juce::DynamicObject();
                ro->setProperty ("count", ra.count);
                ro->setProperty ("split", ra.split);
                ro->setProperty ("perBar", num (ra.perBar, 0.1));
                ro->setProperty ("intervalMs", num (ra.intervalMs, 1.0));
                ro->setProperty ("regularity", num (ra.regularity));
                ro->setProperty ("onGrid", num (ra.onGrid));
                ro->setProperty ("register", voice::noteName (ra.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (ra.highest));
                ro->setProperty ("verdict", ra.verdict);
                o->setProperty ("reattacks", juce::var (ro));
            }

            // Einsaetze im Stem gegen Noteneinsaetze (Pulse unter gehaltenen Noten)
            if (const auto& p = d.v.pulse; p.layer)
            {
                auto po = new juce::DynamicObject();
                po->setProperty ("onsetsPerBar", num (p.perBar, 0.5));
                po->setProperty ("noteStartsPerBar", num (p.notesPerBar, 0.5));
                po->setProperty ("grid", p.grid);
                po->setProperty ("decayMs", num (p.decayMs, 1.0));
                po->setProperty ("heldShare", num (p.heldShare));
                po->setProperty ("onGrid", num (p.lockedShare));
                po->setProperty ("chord", p.chord);
                po->setProperty ("rhythmicLayer", p.layer);
                o->setProperty ("pulse", juce::var (po));
            }

            return juce::var (o);
        }

        juce::var timbreVar (const VoiceData& d, bool detail)
        {
            const auto& t = d.tb;
            auto env = new juce::DynamicObject();
            env->setProperty ("attackMs", num (t.attackMs, 1.0));
            env->setProperty ("decayMs", num (t.decayMs, 1.0));
            env->setProperty ("sustainDb", num (t.sustainDb, 0.1));
            env->setProperty ("releaseMs", num (t.releaseMs, 1.0));
            env->setProperty ("fromNotes", t.envelopeNotes);
            env->setProperty ("transientDb", num (t.transientDb, 0.1));

            auto sp = new juce::DynamicObject();
            sp->setProperty ("wave", t.wave);
            sp->setProperty ("slopeDbOct", num (t.slopeDbOct, 0.1));
            sp->setProperty ("oddEvenDb", num (t.oddEvenDb, 0.1));
            sp->setProperty ("brightness", num (t.centroidHarmonic, 0.1));
            sp->setProperty ("centroidHz", num (t.centroidHz, 1.0));
            sp->setProperty ("longTermCentroidHz", num (t.longTermCentroidHz, 1.0));
            sp->setProperty ("noise", num (t.noiseShare));
            sp->setProperty ("presenceDb", num (t.presenceDb, 0.1));
            sp->setProperty ("roughness", num (t.roughness));
            sp->setProperty ("fromNotes", t.harmonicNotes);

            if (t.harmonicsFromChords)
                sp->setProperty ("note", juce::String::fromUTF8 ("no solo notes — measured inside chords, overtones mixed"));

            auto fl = new juce::DynamicObject();
            fl->setProperty ("noteSweepOct", num (t.noteSweepOct));
            fl->setProperty ("barSweepOct", num (t.barSweepOct));

            if (detail)
            {
                juce::Array<juce::var> bc;

                for (auto c : t.barCentroidHz)
                    bc.add (num (c, 1.0));

                fl->setProperty ("barCentroidHz", bc);
            }

            auto st = new juce::DynamicObject();
            st->setProperty ("correlation", num (t.correlation));
            st->setProperty ("sideMinusMidDb", num (t.sideMinusMidDb, 0.1));
            st->setProperty ("lowCorrelation", num (t.lowCorrelation));
            st->setProperty ("midCorrelation", num (t.midCorrelation));
            st->setProperty ("highCorrelation", num (t.highCorrelation));
            st->setProperty ("lrDelayMs", num (t.lrDelayMs, 0.1));
            st->setProperty ("widthWobbleDb", num (t.widthWobbleDb, 0.1));

            auto en = new juce::DynamicObject();
            en->setProperty ("beatingDb", num (t.beatingDb, 0.1));
            en->setProperty ("beatingHz", num (t.beatingHz, 0.1));
            en->setProperty ("detuneCents", num (t.detuneCents, 0.1));

            auto o = new juce::DynamicObject();
            o->setProperty ("envelope", juce::var (env));
            o->setProperty ("spectrum", juce::var (sp));
            o->setProperty ("filter", juce::var (fl));
            o->setProperty ("stereo", juce::var (st));
            o->setProperty ("ensemble", juce::var (en));
            o->setProperty ("tailSec", num (t.tailSec));

            // Tonhoehenbereich: was gefiltert ist und was nicht
            if (d.minPitch > 21 || d.maxPitch < 108)
            {
                auto rg = new juce::DynamicObject();
                rg->setProperty ("minPitch", d.minPitch);
                rg->setProperty ("maxPitch", d.maxPitch);

                if (d.highPassHz > 0.0)
                    rg->setProperty ("highPassHz", num (d.highPassHz, 1.0));

                rg->setProperty ("note", juce::String::fromUTF8 ("the notes (spectrum, envelope per note) are those in the range")
                                     + (d.highPassHz > 0.0 ? juce::String::fromUTF8 ("; everything below minPitch is filtered out of envelope, stereo, spectrum and timeline") : juce::String())
                                     + (d.maxPitch < 108 ? juce::String::fromUTF8 ("; what sounds above maxPitch stays in (a low-pass would cut the overtones too) — on a stem with a louder part above, the envelope, stereo and timeline still hear it") : juce::String()));
                o->setProperty ("range", juce::var (rg));
            }

            auto rv = new juce::DynamicObject();
            rv->setProperty ("rt60Sec", num (t.rt60Sec, 0.05));
            rv->setProperty ("tailDb", num (t.tailDb, 0.1));
            rv->setProperty ("tailCorrelation", num (t.tailCorrelation));
            rv->setProperty ("noteCorrelation", num (t.noteCorrelation));
            rv->setProperty ("notes", t.reverbNotes);
            o->setProperty ("reverb", juce::var (rv));

            if (known (t.echoMs))
            {
                auto ec = new juce::DynamicObject();
                ec->setProperty ("delayMs", t.echoMs);

                if (d.bpm > 0.0)
                    ec->setProperty ("beats", num (t.echoMs / 1000.0 * d.bpm / 60.0, 0.01));

                ec->setProperty ("feedbackDb", num (t.echoFeedbackDb, 0.1));
                ec->setProperty ("share", num (t.echoShare));
                ec->setProperty ("pairs", t.echoPairs);
                o->setProperty ("echo", juce::var (ec));
            }

            if (! d.classes.isEmpty())
                o->setProperty ("soundsLike", juce::var (d.classes));

            if (! d.timeline.empty())
            {
                juce::Array<juce::var> tl;

                for (auto& w : d.timeline)
                {
                    auto wo = new juce::DynamicObject();

                    if (d.v.hasGrid)
                        wo->setProperty ("bars", w.firstBar == w.lastBar ? juce::String (w.firstBar) : juce::String (w.firstBar) + "-" + juce::String (w.lastBar));

                    wo->setProperty ("start", num (w.start));
                    wo->setProperty ("end", num (w.end));
                    wo->setProperty ("soundsLike", juce::var (w.top));
                    tl.add (juce::var (wo));
                }

                o->setProperty ("timeline", tl);
                juce::Array<juce::var> ch;

                for (auto& c : d.changes)
                {
                    auto co = new juce::DynamicObject();

                    if (d.v.hasGrid)
                        co->setProperty ("bar", c.bar);

                    co->setProperty ("at", num (c.time));
                    co->setProperty ("more", juce::var (c.more));
                    co->setProperty ("less", juce::var (c.less));
                    ch.add (juce::var (co));
                }

                o->setProperty ("changes", ch);
            }

            return juce::var (o);
        }

        juce::StringArray timbreHints (const VoiceData& d)
        {
            const auto& t = d.tb;
            juce::StringArray h;

            if (known (t.attackMs))
                h.add (t.attackMs >= 40.0 ? "soft attack ~" + juce::String (juce::roundToInt (t.attackMs)) + " ms"
                                          : t.attackMs <= 10.0 ? juce::String ("hard attack (< 10 ms)") : "attack ~" + juce::String (juce::roundToInt (t.attackMs)) + " ms");

            if (known (t.sustainDb))
                h.add (t.sustainDb > -3.0 ? juce::String::fromUTF8 ("sustains at full level (pad/organ-like)")
                                          : t.sustainDb < -12.0 ? "decays " + juce::String (juce::roundToInt (-t.sustainDb)) + juce::String::fromUTF8 (" dB within the note (pluck/piano-like)")
                                                                : "sustain " + juce::String (t.sustainDb, 1) + " dB below the peak");

            if (t.wave.isNotEmpty())
                h.add ("overtones " + t.wave + " (slope " + juce::String (t.slopeDbOct, 1) + " dB/oct)");

            if (known (t.noteSweepOct) && t.noteSweepOct >= 0.4)
                h.add ("gets darker within each note by " + juce::String (t.noteSweepOct, 1) + juce::String::fromUTF8 (" oct — filter envelope closing"));
            else if (known (t.noteSweepOct) && t.noteSweepOct <= -0.4)
                h.add ("opens within each note by " + juce::String (-t.noteSweepOct, 1) + " oct");

            if (known (t.barSweepOct) && std::abs (t.barSweepOct) >= 0.3)
                h.add (juce::String (t.barSweepOct > 0 ? "opens" : "closes") + " over the part by " + juce::String (std::abs (t.barSweepOct), 1)
                       + juce::String::fromUTF8 (" oct — filter automation/sweep"));

            if (known (t.correlation) && t.correlation < 0.0)
                h.add (juce::String::fromUTF8 ("anti-phase wide (correlation ") + juce::String (t.correlation, 2) + juce::String::fromUTF8 (") — wide unison detune or stereo chorus"));
            else if (known (t.correlation) && t.correlation < 0.5)
                h.add ("wide (correlation " + juce::String (t.correlation, 2) + ")");
            else if (known (t.correlation) && t.correlation > 0.95)
                h.add ("near mono");

            if (known (t.widthWobbleDb) && t.widthWobbleDb >= 2.0)
                h.add (juce::String::fromUTF8 ("width moves (±") + juce::String (t.widthWobbleDb, 1) + juce::String::fromUTF8 (" dB) — chorus/ensemble"));

            // Verstimmung aus der Breite der Teiltoene. Die Pegelschwankung
            // (beatingDb) bekommt keinen Hinweis mehr: sie misst vor allem das
            // Ausklingen am Notenende (4OSC-Saege pur 5,8 dB "bei 4 Hz") —
            // weder Verstimmung noch Tremolo (Todo 2026-10-27-comparevoice-detune)
            if (known (t.detuneCents) && t.detuneCents >= 8.0)
                h.add (juce::String::fromUTF8 ("partials spread ±") + juce::String (juce::roundToInt (t.detuneCents)) + juce::String::fromUTF8 (" cents in held notes — detuned oscillators/unison or chorus (vibrato also widens them)"));

            if (known (t.lrDelayMs) && std::abs (t.lrDelayMs) >= 0.3)
                h.add ("L/R offset " + juce::String (t.lrDelayMs, 1) + juce::String::fromUTF8 (" ms (Haas widening)"));

            if (known (t.tailSec) && t.tailSec >= 0.8)
                h.add ("tail ~" + juce::String (t.tailSec, 1) + juce::String::fromUTF8 (" s after notes end — reverb or long release"));

            if (known (t.tailCorrelation) && known (t.noteCorrelation) && t.noteCorrelation - t.tailCorrelation >= 0.3)
                h.add ("tail is wider than the notes (correlation " + juce::String (t.noteCorrelation, 2) + juce::String::fromUTF8 (" → ")
                       + juce::String (t.tailCorrelation, 2) + ")" + (known (t.rt60Sec) ? ", RT60 ~" + juce::String (t.rt60Sec, 1) + " s" : juce::String())
                       + juce::String::fromUTF8 (" — stereo reverb"));

            if (known (t.echoMs))
                h.add ("echo every " + juce::String ((int) t.echoMs) + " ms"
                       + (d.bpm > 0.0 ? " (" + juce::String (t.echoMs / 1000.0 * d.bpm / 60.0, 2) + " beats)" : juce::String())
                       + ", " + juce::String (t.echoFeedbackDb, 1) + juce::String::fromUTF8 (" dB per repeat — delay"));

            // Wenige Noten: das Mass ist unsicher (Praxistest 1h: Spektrum aus 1-5 Noten)
            if (t.harmonicNotes > 0 && t.harmonicNotes < 3)
                h.add (juce::String::fromUTF8 ("spectrum from only ") + juce::String (t.harmonicNotes) + juce::String::fromUTF8 (" note(s) — uncertain; widen the range (start/end) or the pitch range"));

            if (t.envelopeNotes < 3)
                h.add (juce::String::fromUTF8 ("envelope from only ") + juce::String (t.envelopeNotes) + juce::String::fromUTF8 (" note(s) with a clear onset — uncertain (held or overlapping notes hide the attack)"));

            if (! d.classes.isEmpty())
                h.add ("sounds like (CLAP): " + d.classes.joinIntoString (", "));

            for (auto& c : d.changes)
                h.add ("the sound changes " + (d.v.hasGrid ? "at bar " + juce::String (c.bar) + " (" + juce::String (c.time, 1) + " s)" : "at " + juce::String (c.time, 1) + " s")
                       + (c.more.isEmpty() ? juce::String() : ": more " + c.more.joinIntoString (", "))
                       + (c.less.isEmpty() ? juce::String() : (c.more.isEmpty() ? ": " : "; ") + juce::String ("less ") + c.less.joinIntoString (", "))
                       + juce::String::fromUTF8 (" — an instrument may enter or leave there: describe the bars before and after separately (start/end), notes of an entering part can hide in the held notes of the other"));

            return h;
        }

        juce::var hintVar (const juce::StringArray& h)
        {
            juce::Array<juce::var> a;

            for (auto& s : h)
                a.add (s);

            return a;
        }

        juce::var barsVar (const VoiceData& d, int limit)
        {
            juce::Array<juce::var> bars;

            for (auto& b : d.v.bars)
            {
                if (bars.size() >= limit)
                    break;

                auto o = new juce::DynamicObject();
                o->setProperty ("bar", b.bar);
                o->setProperty ("notes", b.notes);
                o->setProperty ("pattern", b.pattern);
                o->setProperty ("gaps", b.gaps);

                // Akkordanschlaege je Takt, wenn sie einen eigenen Part bilden
                if (d.v.chordHits.layer)
                    for (auto& hb : d.v.chordHits.bars)
                        if (hb.bar == b.bar)
                        {
                            o->setProperty ("hits", hb.pattern);
                            o->setProperty ("hitTones", hb.chord);

                            if (hb.notes.isNotEmpty())
                                o->setProperty ("hitNotes", hb.notes);
                        }

                // Einsatz-Muster im Stem, wenn ein rhythmischer Part darunter liegt
                if (d.v.pulse.layer)
                    for (auto& pb : d.v.pulse.bars)
                        if (pb.bar == b.bar)
                        {
                            o->setProperty ("pulse", pb.pattern);
                            o->setProperty ("pulseTones", pb.chord);
                        }

                bars.add (juce::var (o));
            }

            return bars;
        }

        void describeHead (juce::DynamicObject& o, const VoiceData& d)
        {
            describeAnalysisSource (o, d.src);

            if (d.v.hasGrid)
            {
                o.setProperty ("bpm", num (d.bpm, 0.1));
                o.setProperty ("gridFrom", d.gridFrom);
            }
        }
    }

    juce::var opDescribeVoice (DawdContext& ctx, const juce::var& cmd)
    {
        VoiceData d;

        if (auto e = analyse (ctx, cmd, d, false); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        const bool detail = (bool) cmd.getProperty ("detail", false);
        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeHead (*o, d);
        o->setProperty ("voice", voiceSummary (d));
        o->setProperty ("bars", barsVar (d, detail ? 64 : 16));

        if (detail)
        {
            juce::Array<juce::var> list;

            for (auto& n : d.v.notes)
            {
                auto no = new juce::DynamicObject();
                no->setProperty ("note", voice::noteName (n.pitch));
                no->setProperty ("pitch", n.pitch);
                no->setProperty ("start", num (n.start + d.src.start, 0.001));
                no->setProperty ("length", num (n.end - n.start, 0.001));
                no->setProperty ("velDb", num (n.velDb, 0.1));
                no->setProperty ("activation", num (n.amplitude));

                if (known (n.tuneCents))
                    no->setProperty ("tuneCents", num (n.tuneCents, 1.0));

                if (n.glideMs > 0.0)
                    no->setProperty ("glide", juce::String (juce::roundToInt (n.glideCents)) + " c / " + juce::String (juce::roundToInt (n.glideMs)) + " ms");

                if (n.vibratoHz > 0.0)
                    no->setProperty ("vibrato", juce::String (juce::roundToInt (n.vibratoCents)) + " c @ " + juce::String (n.vibratoHz, 1) + " Hz");

                if (n.fragments > 1)
                    no->setProperty ("fragments", n.fragments);

                list.add (juce::var (no));

                if (list.size() >= 400)
                    break;
            }

            o->setProperty ("notes", list);
        }

        o->setProperty ("hints", hintVar (voiceHints (d)));
        return res;
    }

    namespace
    {
        // Eine Schicht des other-Bereichs (other, guitar oder piano aus htdemucs_6s)
        struct Layer6
        {
            juce::String stem, role, file, bars, summary;
            bool held = false;           // gehaltene Noten (Pad), keine Neuanschlaege
            int lowest = 0, highest = 0;
            double levelDb = 0.0;
            juce::var info;
        };

        struct Layers6
        {
            juce::File dir;
            bool cached = false;
            std::vector<Layer6> layers;
            juce::Array<juce::var> rest;   // drums/bass/vocals: Pegel, spielt
            bool fullMix = false;          // Drums, Bass oder Gesang klingen mit
            juce::var spec;                // Raster relativ zum Bereichsanfang
        };

        // Kernlage: 10.-90. Perzentil der Toene nach klingender Dauer x
        // Pegel — Min-Max nimmt Obertoene und Uebersprechen mit (Praxistest
        // 1j: das Pad hiess "D2–C#7")
        std::pair<int, int> coreRange (const voice::Voice& v)
        {
            std::vector<std::pair<int, double>> w;
            double total = 0.0;

            for (auto& n : v.notes)
            {
                const double g = (n.end - n.start) * std::pow (10.0, (known (n.velDb) ? n.velDb : 0.0) / 20.0);
                w.push_back ({ n.pitch, g });
                total += g;
            }

            if (w.empty() || total <= 0.0)
                return { v.lowest, v.highest };

            std::sort (w.begin(), w.end());
            int lo = w.front().first, hi = w.back().first;
            double acc = 0.0;
            bool loSet = false;

            for (auto& [p, g] : w)
            {
                acc += g;

                if (! loSet && acc >= 0.1 * total)
                    lo = p, loSet = true;

                if (acc >= 0.9 * total)
                {
                    hi = p;
                    break;
                }
            }

            return { lo, hi };
        }

        // Pegel je Takt (dBFS) aus dem Stereo-Audio der Analyse
        std::vector<double> barLevels (const VoiceData& d)
        {
            std::vector<double> out;
            const auto& st = d.src.stereo;

            if (st.empty())
                return out;

            for (auto& b : d.v.bars)
            {
                if (b.bar < 1)
                    continue;

                // Takt-Zeiten zaehlen wie die Noten ab Beginn der Quelle
                const auto i0 = (size_t) juce::jmax (0.0, b.start * AnalysisSource::stereoRate);
                const auto i1 = juce::jmin (st[0].size(), (size_t) juce::jmax (0.0, b.end * AnalysisSource::stereoRate));
                double sq = 0.0;
                size_t n = 0;

                for (auto& ch : st)
                    for (size_t i = i0; i < i1 && i < ch.size(); ++i, ++n)
                        sq += (double) ch[i] * ch[i];

                out.push_back (n > 0 ? 10.0 * std::log10 (sq / (double) n + 1.0e-12) : -120.0);
            }

            return out;
        }

        // Takte mit Noten als "1-3, 5-8"
        juce::String barRanges (const voice::Voice& v)
        {
            juce::Array<int> active;

            for (auto& b : v.bars)
                if (b.bar >= 1 && b.notes.isNotEmpty())
                    active.addIfNotAlreadyThere (b.bar);

            active.sort();
            juce::StringArray out;

            for (int i = 0; i < active.size();)
            {
                int j = i;

                while (j + 1 < active.size() && active[j + 1] == active[j] + 1)
                    ++j;

                out.add (i == j ? juce::String (active[i]) : juce::String (active[i]) + "-" + juce::String (active[j]));
                i = j + 1;
            }

            return out.joinIntoString (", ");
        }

        bool loadStereo (DawdContext& ctx, const juce::File& f, guided::Stereo& out)
        {
            std::unique_ptr<juce::AudioFormatReader> r (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f));

            if (r == nullptr || r->lengthInSamples <= 0)
                return false;

            juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);

            for (int c = 0; c < 2; ++c)
                out[(size_t) c].assign (b.getReadPointer (juce::jmin (c, b.getNumChannels() - 1)),
                                        b.getReadPointer (juce::jmin (c, b.getNumChannels() - 1)) + b.getNumSamples());

            return true;
        }

        bool saveStereo (const juce::File& f, const guided::Stereo& x, double rate)
        {
            f.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));

            if (w == nullptr)
                return false;

            const float* ptrs[] = { x[0].data(), x[1].data() };
            return w->writeFromFloatArrays (ptrs, 2, (int) x[0].size());
        }

        double rmsDb (const guided::Stereo& x)
        {
            double sq = 0.0;

            for (auto& ch : x)
                for (auto v : ch)
                    sq += (double) v * v;

            return 10.0 * std::log10 (sq / juce::jmax ((size_t) 1, x[0].size() + x[1].size()) + 1.0e-12);
        }

        // Eine Schicht-Datei beschreiben: Rolle, Kernlage, Takte, Pegel (auch
        // je Takt) relativ zur lautesten Schicht (loudestDb, dBFS), Klang.
        // spec: Raster (bpm, firstDownbeat relativ zur Datei, beatsPerBar)
        juce::String describeLayerFile (DawdContext& ctx, const juce::var& spec, const juce::String& name, const juce::File& file,
                                        double dbFs, double loudestDb, Layer6& L)
        {
            auto sp = spec.clone();
            sp.getDynamicObject()->setProperty ("file", file.getFullPathName());
            VoiceData d;

            if (auto e = analyse (ctx, sp, d, true); e.isNotEmpty())
                return e;

            const auto& v = d.v;
            L.stem = name;
            L.file = file.getFullPathName();
            L.levelDb = dbFs - loudestDb;
            const auto core = coreRange (v);
            L.lowest = core.first;
            L.highest = core.second;
            L.bars = barRanges (v);
            // Neu angeschlagene Akkorde heissen hier so, auch wenn der
            // Pegel durchgehend steht (legato): sie sind der Part ueber dem Pad
            const bool restruck = v.chordHits.analysed && known (v.chordHits.perBar) && v.chordHits.perBar >= 3.0;
            L.role = restruck ? juce::String ("re-struck chords (") + juce::String (v.chordHits.perBar, 1) + " hits per bar)" : role (v);
            L.held = ! restruck && L.role.startsWith ("held");

            auto o = new juce::DynamicObject();
            o->setProperty ("stem", name);
            o->setProperty ("file", L.file);
            o->setProperty ("levelDb", num (L.levelDb, 0.1));
            o->setProperty ("dbFs", num (dbFs, 0.1));
            o->setProperty ("role", L.role);

            if (! v.notes.empty())
                o->setProperty ("register", voice::noteName (core.first) + juce::String::fromUTF8 ("–") + voice::noteName (core.second)
                                                + " (median " + voice::noteName (v.medianPitch) + ")");

            o->setProperty ("bars", L.bars);
            juce::Array<juce::var> perBar;

            for (auto db : barLevels (d))
                perBar.add (num (db - loudestDb, 0.1));

            o->setProperty ("barLevelsDb", perBar);
            o->setProperty ("voices", num (v.voices, 0.1));
            o->setProperty ("legato", num (v.legato));
            o->setProperty ("medianNoteBeats", num (v.medianNoteBeats));
            o->setProperty ("reattackDbPerBeat", num (v.attackDbPerBeat, 0.1));

            if (v.chordHits.analysed)
                o->setProperty ("chordHitsPerBar", num (v.chordHits.perBar, 0.1));

            o->setProperty ("attackMs", num (d.tb.attackMs, 1.0));
            o->setProperty ("centroidHz", num (d.tb.centroidHz, 1.0));
            o->setProperty ("correlation", num (d.tb.correlation));
            o->setProperty ("soundsLike", d.classes);
            L.info = juce::var (o);
            L.summary = name + " " + L.role + (v.notes.empty() ? juce::String()
                                                               : " " + voice::noteName (core.first) + juce::String::fromUTF8 ("–") + voice::noteName (core.second))
                        + (L.bars.isNotEmpty() ? ", bars " + L.bars : juce::String())
                        + " (" + (L.levelDb >= -0.05 ? juce::String ("loudest") : juce::String (L.levelDb, 1) + " dB") + ")";
            return {};
        }

        // Bereich einer Datei mit sechs Stems trennen (Cache) und jede
        // hoerbare Schicht des other-Bereichs beschreiben. range: file,
        // start, end, bpm?, firstDownbeat? (Quellsekunden), beatsPerBar?
        juce::String describeLayers6 (DawdContext& ctx, const juce::var& range, Layers6& out)
        {
            const double start = range.getProperty ("start", 0.0);

            if (auto e = fileStemsNow (ctx, range, 6, out.dir, out.cached); e.isNotEmpty())
                return e;

            // Pegel aller sechs Teile (RMS ueber den Bereich, dBFS)
            auto levelOf = [&] (const juce::File& f)
            {
                std::unique_ptr<juce::AudioFormatReader> r (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f));

                if (r == nullptr || r->lengthInSamples <= 0)
                    return -120.0;

                juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
                double sq = 0.0;

                for (int c = 0; c < b.getNumChannels(); ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        sq += (double) b.getSample (c, i) * b.getSample (c, i);

                return 10.0 * std::log10 (sq / juce::jmax (1.0, (double) b.getNumChannels() * b.getNumSamples()) + 1.0e-12);
            };

            std::map<juce::String, double> level;

            for (auto* n : separation::stemNames6)
                level[n] = levelOf (out.dir.getChildFile (juce::String (n) + ".wav"));

            // Hoerbar: hoechstens 20 dB unter dem lautesten Teil des other-Bereichs
            const juce::StringArray parts { "other", "guitar", "piano" };
            double loudest = -120.0;

            for (auto& p : parts)
                loudest = juce::jmax (loudest, level[p]);

            // Raster: bpm/firstDownbeat beziehen sich auf die Quelldatei, die
            // Teile beginnen bei start
            juce::var spec (new juce::DynamicObject());

            for (auto key : { "bpm", "beatsPerBar" })
                if (range.hasProperty (key))
                    spec.getDynamicObject()->setProperty (key, range.getProperty (key, {}));

            if (range.hasProperty ("firstDownbeat"))
                spec.getDynamicObject()->setProperty ("firstDownbeat", (double) range.getProperty ("firstDownbeat", 0.0) - start);

            out.spec = spec;

            for (auto& p : parts)
            {
                if (level[p] < loudest - 20.0 || level[p] < -60.0)
                    continue;

                Layer6 L;

                if (auto e = describeLayerFile (ctx, spec, p, out.dir.getChildFile (p + ".wav"), level[p], loudest, L); e.isNotEmpty())
                    return e;

                out.layers.push_back (L);
            }

            for (auto n : { "drums", "bass", "vocals" })
            {
                const bool plays = level[n] >= loudest - 20.0 && level[n] >= -60.0;
                auto o = new juce::DynamicObject();
                o->setProperty ("stem", n);
                o->setProperty ("dbFs", num (level[n], 0.1));
                o->setProperty ("plays", plays);
                out.rest.add (juce::var (o));
                out.fullMix = out.fullMix || plays;
            }

            return {};
        }

        juce::StringArray summaries (const Layers6& r)
        {
            juce::StringArray s;

            for (auto& L : r.layers)
                s.add (L.summary);

            return s;
        }
    }

    juce::String playedChords (DawdContext& ctx, const juce::var& cmd, std::vector<PlayedNote>& out, juce::var& info)
    {
        VoiceData d;

        if (auto e = analyse (ctx, cmd, d, false); e.isNotEmpty())
            return e;

        const auto& v = d.v;

        if (v.notes.empty())
            return "no notes found in the source";

        // Noten da, aber kein Taktraster (kurze Datei ohne Puls; Agent
        // voice-reste: "no notes found" bei <= 4 s ohne bpm)
        if (v.bars.empty())
            return juce::String::fromUTF8 ("no bar grid on this source (too short or no steady pulse) — shape chords places hits on the bar grid: pass bpm + firstDownbeat");

        // 16tel-Raster je Takt; Einsatz = Note setzt ein (pattern x) oder
        // verdeckter Neuanschlag (chordHits X/x); Pause = pattern '.'
        struct Slot { double time = 0.0; char note = '.', hit = '.'; };
        std::vector<Slot> slots;

        for (auto& b : v.bars)
        {
            const int n = juce::jmax (1, b.pattern.length());
            const juce::String* hits = nullptr;

            for (auto& hb : v.chordHits.bars)
                if (hb.bar == b.bar)
                    hits = &hb.pattern;

            for (int k = 0; k < n; ++k)
            {
                Slot sl;
                sl.time = b.start + (b.end - b.start) * k / (double) n;
                sl.note = (char) b.pattern[k];
                sl.hit = hits != nullptr && k < hits->length() ? (char) (*hits)[k] : '.';
                slots.push_back (sl);
            }
        }

        std::sort (slots.begin(), slots.end(), [] (const Slot& a, const Slot& b) { return a.time < b.time; });

        // Gemessenes Vorziehen/Nachziehen: Median der Notenanfaenge gegen
        // ihre Rasterstelle, hoechstens 40 ms
        std::vector<double> offs;

        for (auto& n : v.notes)
        {
            double best = 1.0e9;

            for (auto& sl : slots)
                if (std::abs (n.start - sl.time) < std::abs (best))
                    best = n.start - sl.time;

            if (std::abs (best) < 0.06)
                offs.push_back (best);
        }

        std::sort (offs.begin(), offs.end());
        const double shift = offs.empty() ? 0.0 : juce::jlimit (-0.04, 0.04, offs[offs.size() / 2]);
        // chordHits kennt alle Akkordanschlaege (x sichtbar, X verdeckt) —
        // Wiedereinsaetze zerstueckelter Noten zaehlen dann nicht; ohne
        // chordHits die Noteneinsaetze. chordHits sieht nur Einsaetze von
        // mindestens zwei Tonklassen: ein Einzelton, eine Oktave oder ein
        // Arpeggio vor dem ersten Akkordanschlag (am Dateianfang, nach einer
        // Pause) fiel ganz weg (Todo 2026-10-27-chords-erster-akkord-fehlt).
        // Dort zaehlt darum der Noteneinsatz (open: ein Anschlag klingt)
        const bool byHits = v.chordHits.analysed && ! v.chordHits.bars.empty();
        auto isHitAt = [&] (const Slot& s, bool open) { return byHits ? (s.hit == 'X' || s.hit == 'x' || (! open && s.note == 'x')) : s.note == 'x'; };
        // Spaet einsetzende Obertoene eines klingenden Tons (basic-pitch hoert
        // im Ausklang die Duodezime/Doppeloktave/Terz darueber als eigene
        // Note, Agent voice-reste: ein gehaltener Ton gab mitten im Klang
        // einen zweiten Anschlag): leiser als der Grundton, der schon klingt
        std::set<const voice::Note*> lateOvertones;

        for (auto& n : v.notes)
            for (auto& m : v.notes)
            {
                const int iv = n.pitch - m.pitch;

                if ((iv == 12 || iv == 19 || iv == 24 || iv == 28 || iv == 31) && n.start > m.start + 0.05 && n.start < m.end - 0.05
                    && n.amplitude < m.amplitude)
                    lateOvertones.insert (&n);
            }

        auto startsAt = [&] (double t)
        {
            for (auto& n : v.notes)
                if (lateOvertones.count (&n) == 0 && std::abs (n.start - t) < 0.08)
                    return true;

            return false;
        };

        bool open = false;
        const auto core = coreRange (v);
        const bool strum = known (v.staggerMs) && v.staggerMs >= 30.0;
        const bool poly = ! known (v.voices) || v.voices >= 1.5;
        const bool keepOctaves = (bool) cmd.getProperty ("keepOctaves", false);
        std::vector<int> prev;
        std::vector<double> voicesPerHit;
        int hitCount = 0;

        for (size_t i = 0; i < slots.size(); ++i)
        {
            const auto& sl = slots[i];

            if (sl.note == '.')
                open = false;

            if (! isHitAt (sl, open))
                continue;

            // Ohne chordHits zaehlt ein Noteneinsatz nur, wenn dort eine Note
            // einsetzt, die kein spaeter Oberton ist
            if (! byHits && ! startsAt (sl.time) && open)
                continue;

            open = true;

            // Ende: naechster Anschlag oder Pause
            size_t j = i + 1;

            while (j < slots.size() && ! isHitAt (slots[j], true) && slots[j].note != '.')
                ++j;

            const double t0 = sl.time, t1 = j < slots.size() ? slots[j].time : slots.back().time + (slots.size() > 1 ? slots.back().time - slots[slots.size() - 2].time : 0.1);

            // Tonklassen, die im Fenster klingen (Dauer x Pegel) — je Tonklasse
            // ihr staerkster Ton: ein Oberton in der Oktave darueber hebt sie
            // sonst so, dass kurze Akkordtoene unter die Schwelle fallen
            std::array<double, 12> pc {};
            std::map<int, double> tones;
            double loudest = -120.0;

            for (auto& n : v.notes)
            {
                const double ov = juce::jmin (n.end, t1) - juce::jmax (n.start, t0);

                if (ov <= 0.0 || lateOvertones.count (&n) > 0)
                    continue;

                const double vel = known (n.velDb) ? n.velDb : 0.0;
                tones[n.pitch] += ov * std::pow (10.0, vel / 20.0);

                if (std::abs (n.start - t0) < 0.08)
                    loudest = juce::jmax (loudest, vel);
            }

            // Obertoene zaehlen nicht als eigene Stimme: die Oktave deutlich
            // leiser als ihr Grundton im Fenster; Duodezime, zwei Oktaven und
            // zwei Oktaven + Terz, die mit einem tieferen Ton einsetzen und
            // schwaecher erkannt sind (basic-pitch hoert dort den Oberton).
            // Schwach heisst: Aktivierung unter 0,45 — basic-pitch laesst
            // Obertoene nur zaghaft durch (0,34-0,44), eine gespielte
            // Duodezime liegt darueber (0,47-0,74), auch wenn sie leiser als
            // ihr Grundton erkannt ist (Verhaeltnis 0,6-0,98 wie bei den
            // Obertoenen: C3 C4 E4 G4 wurde C E, Todo
            // 2026-10-30-chords-echte-quinte). Die Aktivierung haengt nicht
            // am Pegel der Datei (-35 dB: gleich oder hoeher)
            std::set<int> overtones;

            for (auto& n : v.notes)
                for (auto& m : v.notes)
                {
                    const int iv = n.pitch - m.pitch;
                    const bool sounding = n.start < t1 && n.end > t0 && m.start < t1 && m.end > t0;

                    if (sounding && (iv == 19 || iv == 24 || iv == 28) && std::abs (n.start - m.start) < 0.05 && n.amplitude < m.amplitude
                        && n.amplitude < 0.45)
                        overtones.insert (n.pitch);
                }

            std::map<int, double> real;   // ohne Obertoene, fuer keepOctaves

            for (auto& [p, w] : tones)
            {
                bool overtone = overtones.count (p) > 0;

                for (auto& [q, wq] : tones)
                    overtone = overtone || (p - q == 12 && w < 0.6 * wq);

                if (! overtone)
                    pc[(size_t) (p % 12)] = juce::jmax (pc[(size_t) (p % 12)], w), real[p] = w;
            }

            const double top = *std::max_element (pc.begin(), pc.end());

            if (top <= 0.0)
                continue;

            std::vector<int> order (12);

            for (int k = 0; k < 12; ++k)
                order[(size_t) k] = k;

            std::sort (order.begin(), order.end(), [&] (int a, int b) { return pc[(size_t) a] > pc[(size_t) b]; });

            // Stimmenzahl je Anschlag aus den klingenden Tonklassen, nicht aus
            // dem Mittel ueber den Part (Praxistest 1l: vierstimmige Strums
            // wurden Zweiklaenge, gerundet aus 2,4); hoechstens sechs
            std::vector<int> chord;

            for (int k = 0; k < 12 && chord.size() < 6; ++k)
                if (pc[(size_t) order[(size_t) k]] >= 0.15 * top
                    || (poly && chord.size() < 2 && pc[(size_t) order[(size_t) k]] >= 0.05 * top))   // mindestens ein Zweiklang
                    chord.push_back (order[(size_t) k]);

            voicesPerHit.push_back ((double) chord.size());

            // Voicing: je Tonklasse die Lage in der Kernlage, die dem
            // vorigen Akkord am naechsten liegt (erster Akkord: um die Mitte)
            const double centre = prev.empty() ? 0.5 * (core.first + core.second)
                                               : std::accumulate (prev.begin(), prev.end(), 0.0) / (double) prev.size();
            std::vector<int> pitches;

            // keepOctaves: jeder Ton in der Lage, in der er klingt (auch
            // Oktavdopplungen ab halbem Gewicht), statt in die Kernlage gelegt
            if (keepOctaves)
                for (auto& [p, w] : real)
                    if (std::find (chord.begin(), chord.end(), p % 12) != chord.end() && w >= 0.5 * pc[(size_t) (p % 12)])
                        pitches.push_back (p);

            for (int c : keepOctaves ? std::vector<int>() : chord)
            {
                int best = -1;
                double bestCost = 1.0e9;

                for (int p = juce::jmax (0, core.first - 12); p <= juce::jmin (127, core.second + 12); ++p)
                {
                    if (p % 12 != c || std::find (pitches.begin(), pitches.end(), p) != pitches.end())
                        continue;

                    double cost = std::abs (p - centre);

                    for (int q : prev)
                        cost = juce::jmin (cost, (double) std::abs (p - q) + 0.5);

                    if (p < core.first || p > core.second)
                        cost += 12.0;   // ausserhalb der Kernlage nur, wenn es nicht anders geht

                    if (cost < bestCost)
                        bestCost = cost, best = p;
                }

                if (best >= 0)
                    pitches.push_back (best);
            }

            std::sort (pitches.begin(), pitches.end());

            // Velocity nach Anschlagstaerke: Notenpegel, sonst X stark / x
            const int vel = loudest > -100.0 ? juce::jlimit (50, 120, (int) std::round (105.0 + 2.0 * loudest))
                                             : sl.hit == 'X' ? 100 : 85;
            const double start = juce::jmax (0.0, t0 + shift);
            const double len = juce::jmax (0.05, (t1 - t0) * 0.97);

            // Strum hoechstens 25 ms und 15 % des Abstands zum naechsten
            // Anschlag: der an getrennten Schichten gemessene Versatz ist
            // verschmiert (Praxistest 1l: 58 ms auf Achtel-Anschlaegen klangen
            // wie Zwischentoene)
            const double strumSec = strum && pitches.size() >= 2 ? juce::jmin (0.001 * v.staggerMs, 0.025, 0.15 * (t1 - t0)) : 0.0;

            for (size_t k = 0; k < pitches.size(); ++k)
            {
                PlayedNote pn;
                pn.pitch = pitches[k];
                pn.start = start + (pitches.size() > 1 ? strumSec * (double) k / (double) (pitches.size() - 1) : 0.0);
                pn.length = juce::jmax (0.05, len - (pn.start - start));
                pn.velocity = vel;
                out.push_back (pn);
            }

            prev = pitches;
            ++hitCount;
        }

        if (out.empty())
            return "no chords found in the source";

        auto o = new juce::DynamicObject();
        o->setProperty ("hits", hitCount);
        o->setProperty ("hitsPerBar", num ((double) hitCount / juce::jmax (1.0, (double) v.bars.size()), 0.1));
        // Stimmen: Median je Anschlag (jeder Anschlag hat seine eigene Zahl)
        std::sort (voicesPerHit.begin(), voicesPerHit.end());
        o->setProperty ("voices", (int) voicesPerHit[voicesPerHit.size() / 2]);
        o->setProperty ("register", voice::noteName (core.first) + juce::String::fromUTF8 ("–") + voice::noteName (core.second));
        o->setProperty ("shiftMs", num (1000.0 * shift, 1.0));
        o->setProperty ("strumMs", strum ? num (juce::jmin (v.staggerMs, 25.0), 1.0) : juce::var (0));
        o->setProperty ("sourceStart", d.src.start);
        o->setProperty ("gridFrom", d.gridFrom);
        o->setProperty ("hitsFrom", byHits ? "chordHits" : "note onsets");
        info = juce::var (o);
        return {};
    }

    // Eine Aufnahme mit Beispielklaengen zerlegen (gefuehrte NMF): examples
    // [{name, file}] je Instrument (z. B. Schicht-Dateien einer freien
    // Stelle), target eine Datei oder mehrere (summiert). Je Beispiel eine
    // Datei <dir>/split-<name>.wav, dazu split-rest.wav.
    juce::var opSplitLayer (DawdContext& ctx, const juce::var& cmd)
    {
        auto* ex = cmd.getProperty ("examples", {}).getArray();

        if (ex == nullptr || ex->size() < 2)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("need examples: [{name, file}, …] — at least two instruments, each alone") } });

        juce::StringArray targets;

        if (auto* tl = cmd.getProperty ("target", {}).getArray())
            for (auto& t : *tl)
                targets.add (t.toString());
        else if (cmd.hasProperty ("target"))
            targets.add (cmd.getProperty ("target", {}).toString());

        if (targets.isEmpty())
            return resultObj (false, { { "error", "need target (a file or a list of files to split)" } });

        juce::StringArray names;
        std::vector<guided::Stereo> sources;

        for (auto& e : *ex)
        {
            const auto name = e.getProperty ("name", {}).toString();
            const auto file = juce::File (e.getProperty ("file", {}).toString());

            if (name.isEmpty() || ! juce::File::createLegalFileName (name).equalsIgnoreCase (name) || name == "rest" || names.contains (name))
                return resultObj (false, { { "error", "each example needs a unique plain name (not 'rest'): " + name } });

            guided::Stereo x;

            if (! file.existsAsFile() || ! loadStereo (ctx, file, x))
                return resultObj (false, { { "error", "no such file: " + file.getFullPathName() } });

            names.add (name);
            sources.push_back (std::move (x));
        }

        guided::Stereo target;
        double rate = 0.0;

        for (auto& t : targets)
        {
            const auto f = juce::File (t);
            std::unique_ptr<juce::AudioFormatReader> r (f.existsAsFile() ? ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f) : nullptr);
            guided::Stereo x;

            if (r == nullptr || ! loadStereo (ctx, f, x))
                return resultObj (false, { { "error", "no such file: " + t } });

            if (rate == 0.0)
                rate = r->sampleRate;
            else if (std::abs (rate - r->sampleRate) > 1.0)
                return resultObj (false, { { "error", "target files need the same sample rate" } });

            if (target[0].empty())
                target = x;
            else
                for (int c = 0; c < 2; ++c)
                    for (size_t i = 0; i < juce::jmin (target[(size_t) c].size(), x[(size_t) c].size()); ++i)
                        target[(size_t) c][i] += x[(size_t) c][i];
        }

        auto dir = cmd.hasProperty ("dir") ? juce::File (cmd.getProperty ("dir", {}).toString())
                                           : juce::File (targets[0]).getParentDirectory().getChildFile ("split");

        if (cmd.hasProperty ("dir") && ! juce::File::isAbsolutePath (cmd.getProperty ("dir", {}).toString()))
            return resultObj (false, { { "error", "dir must be an absolute path" } });

        std::vector<guided::Stereo> parts;

        if (auto e = guided::split (sources, target, rate, parts); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        dir.createDirectory();
        names.add ("rest");
        juce::Array<juce::var> out;
        double loudest = -120.0;

        for (size_t i = 0; i < parts.size(); ++i)
            if (names[(int) i] != "rest")
                loudest = juce::jmax (loudest, rmsDb (parts[i]));

        for (size_t i = 0; i < parts.size(); ++i)
        {
            const auto f = dir.getChildFile ("split-" + names[(int) i] + ".wav");

            if (! saveStereo (f, parts[i], rate))
                return resultObj (false, { { "error", "cannot write " + f.getFullPathName() } });

            auto o = new juce::DynamicObject();
            o->setProperty ("name", names[(int) i]);
            o->setProperty ("file", f.getFullPathName());
            o->setProperty ("dbFs", num (rmsDb (parts[i]), 0.1));
            o->setProperty ("levelDb", num (rmsDb (parts[i]) - loudest, 0.1));
            out.add (juce::var (o));
        }

        return resultObj (true, { { "dir", dir.getFullPathName() }, { "parts", out },
                                  { "hints", hintVar ({ juce::String::fromUTF8 ("describe each part on its own (describeVoice/describeTimbre with the file) — the parts are masks of the target, they add up to it; rest holds what no example explains") }) } });
    }

    // Wie viele Instrumente spielen ausser Drums, Bass und Gesang? Der
    // Bereich einer Datei wird mit htdemucs_6s getrennt (other, guitar, piano
    // teilen den other-Bereich auf); je hoerbarem Teil Pegel, Rolle, Lage,
    // Takte, Anschlaege und Klang. Praxistest 1h: Pad und angeschlagene
    // Akkorde steckten zusammen im other-Stem, die Akkorde 5 dB lauter als das
    // Pad — kein Werkzeug zeigte das Verhaeltnis. Praxistest 1i: im vollen
    // Refrain legte das Modell Pad und Akkorde zusammen, im freien Intro nicht
    // — exposed {start, end} vergleicht mit so einer Stelle.
    juce::var opDescribeLayers (DawdContext& ctx, const juce::var& cmd)
    {
        if (! cmd.hasProperty ("file") || ! cmd.hasProperty ("start") || ! cmd.hasProperty ("end"))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("need file, start and end (the part, at most 120 s — the reference or a stem of it)") } });

        const double start = cmd.getProperty ("start", 0.0), end = cmd.getProperty ("end", 0.0);

        if (end - start > 120.0)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("range too long (at most 120 s) — describe the part, not the song") } });

        const auto ex = cmd.getProperty ("exposed", {});

        if (! ex.isVoid() && (! ex.isObject() || ! ex.hasProperty ("start") || ! ex.hasProperty ("end")
                              || (double) ex.getProperty ("end", 0.0) - (double) ex.getProperty ("start", 0.0) > 120.0))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("exposed needs {start, end} (at most 120 s) — a place where the stem plays alone (findRepeats exposed)") } });

        if (auto e = transcription::available(); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        auto range = cmd.clone();
        range.getDynamicObject()->removeProperty ("exposed");
        Layers6 here;

        if (auto e = describeLayers6 (ctx, range, here); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        juce::Array<juce::var> layers;
        juce::StringArray roles;

        for (auto& L : here.layers)
        {
            layers.add (L.info);
            roles.add (L.role);
        }

        juce::StringArray h;
        const auto seen = summaries (here);

        if (here.layers.size() >= 2)
            h.add (juce::String ((int) here.layers.size()) + juce::String::fromUTF8 (" instruments besides drums/bass/vocals: ") + seen.joinIntoString ("; ")
                   + juce::String::fromUTF8 (" — build one track per layer, analyse each layer file on its own (describeVoice/describeTimbre/compareVoice against it), and set their balance by levelDb"));
        else if (here.layers.size() == 1)
            h.add (juce::String::fromUTF8 ("one instrument besides drums/bass/vocals: ") + seen[0]
                   + juce::String::fromUTF8 (" — if describeVoice still reports chordHits or layers, the parts share a sound class (the model cannot split them)"));
        else
            h.add (juce::String::fromUTF8 ("nothing besides drums/bass/vocals sounds here"));

        for (int i = 0; i < roles.size(); ++i)
            for (int j = i + 1; j < roles.size(); ++j)
                if (roles[i] == roles[j])
                    h.add (juce::String::fromUTF8 ("two layers with the same role (") + roles[i]
                           + juce::String::fromUTF8 (") — possibly one instrument split across stems: listen before building two tracks"));

        // Eine Schicht nur in wenigen Takten (z. B. ein Riff im Auftakt) ist
        // kein Ersatz fuer ein Instrument, das durchspielt
        for (auto& L : here.layers)
            if (L.bars.isNotEmpty() && ! L.bars.containsChar (',') && ! L.bars.containsChar ('-'))
                h.add (L.stem + juce::String::fromUTF8 (" sounds in bar ") + L.bars + juce::String::fromUTF8 (" only — a short part, not a layer that plays through"));

        auto res = resultObj (true);
        auto* o = res.getDynamicObject();

        if (ex.isObject())
        {
            // Dieselbe Zerlegung an der freien Stelle: dort trennt das Modell
            // sauberer; fehlt hier eine Schicht, steckt sie in einer anderen
            auto exRange = range.clone();
            exRange.getDynamicObject()->setProperty ("start", ex.getProperty ("start", {}));
            exRange.getDynamicObject()->setProperty ("end", ex.getProperty ("end", {}));
            exRange.getDynamicObject()->removeProperty ("firstDownbeat");

            if (ex.hasProperty ("firstDownbeat"))
                exRange.getDynamicObject()->setProperty ("firstDownbeat", ex.getProperty ("firstDownbeat", {}));

            Layers6 there;

            if (auto e = describeLayers6 (ctx, exRange, there); e.isNotEmpty())
                return resultObj (false, { { "error", "exposed: " + e } });

            juce::Array<juce::var> exLayers;

            for (auto& L : there.layers)
                exLayers.add (L.info);

            auto eo = new juce::DynamicObject();
            eo->setProperty ("start", ex.getProperty ("start", {}));
            eo->setProperty ("end", ex.getProperty ("end", {}));
            eo->setProperty ("count", (int) there.layers.size());
            eo->setProperty ("layers", exLayers);
            o->setProperty ("exposed", juce::var (eo));

            const auto exSeen = summaries (there);
            bool missing = false;
            juce::String missingNote;

            for (auto& T : there.layers)
            {
                if (! T.held)
                    continue;

                const bool heldHere = std::any_of (here.layers.begin(), here.layers.end(), [] (const Layer6& L) { return L.held; });

                if (heldHere)
                    continue;

                // Wo steckt es hier? Die Schicht mit den neu angeschlagenen
                // Akkorden bzw. die lauteste, deren Lage es ueberdeckt
                juce::String host;

                for (auto& L : here.layers)
                    if (L.role.startsWith ("re-struck") || host.isEmpty())
                        host = L.stem;

                missing = true;
                missingNote = juce::String::fromUTF8 ("at the exposed place (") + juce::String ((double) ex.getProperty ("start", 0.0), 2) + juce::String::fromUTF8 ("–")
                              + juce::String ((double) ex.getProperty ("end", 0.0), 2) + " s) the part splits into: " + exSeen.joinIntoString ("; ")
                              + juce::String::fromUTF8 (". Here the held layer (") + T.stem + " " + T.role + " " + voice::noteName (T.lowest) + juce::String::fromUTF8 ("–")
                              + voice::noteName (T.highest) + juce::String::fromUTF8 (") has no layer of its own — in the full mix the model merged it")
                              + (host.isNotEmpty() ? juce::String::fromUTF8 (" into '") + host + "'" : juce::String());
                break;
            }

            // Zusammengelegt: mit den Klaengen der freien Stelle trennen
            // (gefuehrte NMF) — je Schicht der freien Stelle eine Datei
            if (missing && there.layers.size() >= 2)
            {
                const auto tag = juce::String ((double) ex.getProperty ("start", 0.0), 3) + "-" + juce::String ((double) ex.getProperty ("end", 0.0), 3);
                const auto splitDir = here.dir.getChildFile ("split " + tag);
                bool splitCached = true;

                for (auto& T : there.layers)
                    splitCached = splitCached && splitDir.getChildFile ("split-" + T.stem + ".wav").existsAsFile();

                splitCached = splitCached && splitDir.getChildFile ("split-rest.wav").existsAsFile();

                if (! splitCached)
                {
                    std::vector<guided::Stereo> sources;
                    guided::Stereo target;

                    for (auto& T : there.layers)
                    {
                        guided::Stereo x;

                        if (! loadStereo (ctx, juce::File (T.file), x))
                            return resultObj (false, { { "error", "cannot read " + T.file } });

                        sources.push_back (std::move (x));
                    }

                    for (auto& L : here.layers)
                    {
                        guided::Stereo x;

                        if (! loadStereo (ctx, juce::File (L.file), x))
                            return resultObj (false, { { "error", "cannot read " + L.file } });

                        if (target[0].empty())
                            target = x;
                        else
                            for (int c = 0; c < 2; ++c)
                                for (size_t i = 0; i < juce::jmin (target[(size_t) c].size(), x[(size_t) c].size()); ++i)
                                    target[(size_t) c][i] += x[(size_t) c][i];
                    }

                    std::vector<guided::Stereo> parts;

                    if (auto e = guided::split (sources, target, separation::sampleRate, parts); e.isNotEmpty())
                        return resultObj (false, { { "error", "split: " + e } });

                    splitDir.createDirectory();

                    for (size_t i = 0; i < parts.size(); ++i)
                    {
                        const auto f = splitDir.getChildFile ("split-" + (i < there.layers.size() ? there.layers[i].stem : juce::String ("rest")) + ".wav");

                        if (! saveStereo (f, parts[i], separation::sampleRate))
                            return resultObj (false, { { "error", "cannot write " + f.getFullPathName() } });
                    }
                }

                // Beschreiben wie Schichten; Pegel relativ zur lautesten
                juce::StringArray names;

                for (auto& T : there.layers)
                    names.add (T.stem);

                names.add ("rest");
                std::map<juce::String, double> db;
                double loudestSplit = -120.0;

                for (auto& n : names)
                {
                    guided::Stereo x;
                    db[n] = loadStereo (ctx, splitDir.getChildFile ("split-" + n + ".wav"), x) ? rmsDb (x) : -120.0;

                    if (n != "rest")
                        loudestSplit = juce::jmax (loudestSplit, db[n]);
                }

                juce::Array<juce::var> splitLayers;
                juce::StringArray splitSeen;

                for (int i = 0; i < names.size(); ++i)
                {
                    Layer6 S;

                    if (auto e = describeLayerFile (ctx, here.spec, names[i], splitDir.getChildFile ("split-" + names[i] + ".wav"), db[names[i]], loudestSplit, S); e.isNotEmpty())
                        return resultObj (false, { { "error", e } });

                    if (i < (int) there.layers.size())
                        S.info.getDynamicObject()->setProperty ("like", there.layers[(size_t) i].stem + " at the exposed place (" + there.layers[(size_t) i].role + ")");
                    else
                        S.info.getDynamicObject()->setProperty ("like", juce::String::fromUTF8 ("rest — what neither sound explains (other parts, bleed)"));

                    splitLayers.add (S.info);

                    if (i < (int) there.layers.size())
                        splitSeen.add (S.summary + " -> " + S.file);
                }

                auto so = new juce::DynamicObject();
                so->setProperty ("dir", splitDir.getFullPathName());
                so->setProperty ("cached", splitCached);
                so->setProperty ("layers", splitLayers);
                o->setProperty ("split", juce::var (so));
                h.add (missingNote + juce::String::fromUTF8 (". Split with the sounds of the exposed place (split): ") + splitSeen.joinIntoString ("; ")
                       + juce::String::fromUTF8 (" — build one track per split layer, analyse and compare each against its split file (register, voicing, hits come from there; addMidiFromAudio {file: split file, shape: 'chords', bpm, firstDownbeat} writes a re-struck chord layer as played chords), and mix them at their levelDb; split-rest holds what neither sound explains"));
            }
            else if (missing)
                h.add (missingNote + juce::String::fromUTF8 (". Build it as its own track with register and sound from the exposed layer file, and the re-struck chords as another track; take their balance from the exposed place"));

            if (! missing && there.layers.size() > here.layers.size())
                h.add (juce::String::fromUTF8 ("more layers at the exposed place (") + juce::String ((int) there.layers.size()) + " vs " + juce::String ((int) here.layers.size())
                       + juce::String::fromUTF8 (") — the model merged instruments in the full mix: ") + exSeen.joinIntoString ("; "));
        }
        else if (here.fullMix)
            h.add (juce::String::fromUTF8 ("drums/bass/vocals play here — in a full mix the model may merge instruments (e.g. a pad into re-struck chords): pass exposed {start, end} of a place where this part plays alone (findRepeats exposed) to compare"));

        if (std::any_of (here.layers.begin(), here.layers.end(), [] (const Layer6& L) { return L.stem != "other"; }))
            h.add (juce::String::fromUTF8 ("stem names are the model's classes — a 'guitar' layer may be a synth: go by role, soundsLike and register"));

        o->setProperty ("file", cmd.getProperty ("file", {}));
        o->setProperty ("start", start);
        o->setProperty ("end", end);
        o->setProperty ("dir", here.dir.getFullPathName());
        o->setProperty ("cached", here.cached);
        o->setProperty ("count", (int) here.layers.size());
        o->setProperty ("layers", layers);
        o->setProperty ("others", here.rest);
        o->setProperty ("hints", hintVar (h));
        return res;
    }

    // timeline: false laesst die CLAP-Zeitleiste je Takt weg; voice: true
    // legt die Spielweise dazu (Glide, Vibrato) — fuer die Steckbriefe
    // (tools/profile_plugin.py misst je Render beides in einem Aufruf)
    juce::var opDescribeTimbre (DawdContext& ctx, const juce::var& cmd)
    {
        VoiceData d;

        if (auto e = analyse (ctx, cmd, d, true, (bool) cmd.getProperty ("timeline", true)); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeHead (*o, d);
        o->setProperty ("timbre", timbreVar (d, (bool) cmd.getProperty ("detail", false)));
        o->setProperty ("hints", hintVar (timbreHints (d)));

        if ((bool) cmd.getProperty ("voice", false))
        {
            auto v = voiceSummary (d);
            v.getDynamicObject()->setProperty ("vibratoCents", num (d.v.vibratoCents, 1.0));
            v.getDynamicObject()->setProperty ("vibratoHz", num (d.v.vibratoHz, 0.1));
            o->setProperty ("voice", v);
        }

        return res;
    }

    namespace
    {
        struct Deviation
        {
            double weight = 0.0;
            juce::String what, ref, yours, fix;
            juce::StringArray keys;   // Parameter-Stichworte fuer Vorschlaege
        };

        // Parameter des eigenen Instruments, deren Name ein Stichwort traegt
        juce::StringArray paramsFor (DawdContext& ctx, const juce::String& plugin, const juce::StringArray& keys)
        {
            juce::StringArray out;

            if (plugin.isEmpty() || keys.isEmpty())
                return out;

            auto* mt = findModuleTrack (*ctx.edit, plugin);
            auto* p = mt != nullptr ? modulePlugin (*mt) : nullptr;

            if (p == nullptr)
                return out;

            for (auto* prm : p->getAutomatableParameters())
            {
                auto name = parameterLabel (*p, *prm);
                auto lower = name.toLowerCase();

                // Surge: Szene B ist meist stumm — Szene A zuerst
                if (lower.startsWith ("b "))
                    continue;

                for (auto& k : keys)
                    if (lower.contains (k))
                    {
                        out.addIfNotAlreadyThere (name);
                        break;
                    }

                if (out.size() >= 4)
                    break;
            }

            for (auto& st : pluginSettings (*p))
                for (auto& k : keys)
                    if (st.name.toLowerCase().contains (k) && out.size() < 5)
                        out.addIfNotAlreadyThere (st.name);

            return out;
        }

        // Regler fuer die Gleitzeit: Portamento/Glide, bei 4OSC "Legato"
        // (0–500 ms, die Tonhoehe gleitet in dieser Zeit). Ein Modus-Schalter
        // ("Voice Mode", "Play Mode") gleitet allein nicht (Praxistest stil-1:
        // compareVoice nannte "Legato"/"Voice Mode", der Agent suchte "Glide",
        // fand nichts und hielt 4OSC fuer ohne Portamento)
        juce::StringArray glideTimeParams (DawdContext& ctx, const juce::String& plugin)
        {
            juce::StringArray out;
            auto* mt = plugin.isNotEmpty() ? findModuleTrack (*ctx.edit, plugin) : nullptr;
            auto* p = mt != nullptr ? modulePlugin (*mt) : nullptr;

            if (p == nullptr)
                return out;

            for (auto* prm : p->getAutomatableParameters())
            {
                auto name = parameterLabel (*p, *prm);
                auto lower = name.toLowerCase();

                if (lower.startsWith ("b ") || lower.contains ("mode") || lower.contains (" on") || lower.contains ("enable"))
                    continue;

                if (lower.contains ("portamento") || lower.contains ("glide") || (lower.contains ("legato") && (prm->getLabel().trim() == "ms" || prm->getCurrentValueAsString().trim().endsWith ("ms"))))
                    out.addIfNotAlreadyThere (name);
            }

            return out;
        }

        juce::String pitchList (const std::vector<int>& ps)
        {
            juce::StringArray s;

            for (auto p : ps)
                s.addIfNotAlreadyThere (voice::noteName (p));

            return s.joinIntoString (" ");
        }

        // Pegel je 16tel und Takt (Maximum von 10-ms-RMS im Platz, dB) —
        // fuer den Vergleich unabhaengig von den erkannten Noten
        std::map<int, std::vector<double>> slotLevels (const VoiceData& d)
        {
            std::map<int, std::vector<double>> out;
            const auto& st = d.src.stereo;

            if (! d.v.hasGrid || st.empty() || st[0].empty())
                return out;

            const double rate = AnalysisSource::stereoRate;
            const size_t n = st[0].size(), hop = (size_t) (0.01 * rate);
            std::vector<double> env;

            for (size_t i = 0; i + hop <= n; i += hop)
            {
                double q = 0.0;

                for (size_t k = i; k < i + hop; ++k)
                    for (auto& c : st)
                        q += (double) c[k] * c[k];

                env.push_back (10.0 * std::log10 (q / (double) (hop * st.size()) + 1.0e-12));
            }

            for (auto& b : d.v.bars)
            {
                std::vector<double> lv (16, -120.0);

                for (int k = 0; k < 16; ++k)
                {
                    const double a = b.start + (b.end - b.start) * k / 16.0, e = b.start + (b.end - b.start) * (k + 1) / 16.0;

                    for (auto f = (long) (a / 0.01); f < (long) (e / 0.01); ++f)
                        if (f >= 0 && f < (long) env.size())
                            lv[(size_t) k] = juce::jmax (lv[(size_t) k], env[(size_t) f]);
                }

                out[b.bar] = lv;
            }

            return out;
        }

        std::vector<Deviation> compare (const VoiceData& r, const VoiceData& y)
        {
            std::vector<Deviation> out;
            const auto& rv = r.v;
            const auto& yv = y.v;
            auto f1 = [] (double v) { return known (v) ? juce::String (v, 2) : juce::String ("?"); };
            auto ms = [] (double v) { return known (v) ? juce::String (juce::roundToInt (v)) + " ms" : juce::String ("?"); };

            if (rv.notes.empty() || yv.notes.empty())
                return out;

            // Takte paaren ueber die Zeit ab Bereichsanfang, nicht ueber die
            // Nummer: jede Seite zaehlt ab ihrer eigenen ersten Eins
            // (Praxistest 1l: Referenz mit firstDownbeat ab dem Refrain,
            // eigene Spur ab dem Takt davor — "bar 5" verglich verschobene Takte)
            auto yourBarAt = [&yv] (double t) -> const voice::Bar*
            {
                for (auto& b : yv.bars)
                    if (std::abs (b.start - t) < 0.25 * (b.end - b.start))
                        return &b;

                return nullptr;
            };

            // Artikulation: gehaltener Pegel und mittlere Einbruchtiefe je
            // Takt (stetig — gezaehlte Luecken und Notenzahlen kippen schon
            // zwischen zwei Renders desselben Projekts, Praxistest 1d)
            {
                auto art = [] (const voice::Voice& v)
                {
                    return "level held " + juce::String (juce::roundToInt ((known (v.sustainShare) ? v.sustainShare : 0.0) * 100)) + "% of the bar, dips "
                           + juce::String (known (v.dipDb) ? v.dipDb : 0.0, 1) + " dB, re-attacks " + juce::String (known (v.attackDbPerBeat) ? v.attackDbPerBeat : 0.0, 1) + " dB/beat";
                };
                const double dh = known (rv.sustainShare) && known (yv.sustainShare) ? rv.sustainShare - yv.sustainShare : 0.0;
                const double dd = known (rv.dipDb) && known (yv.dipDb) ? yv.dipDb - rv.dipDb : 0.0;
                const double ra = known (rv.attackDbPerBeat) ? rv.attackDbPerBeat : 0.0, ya = known (yv.attackDbPerBeat) ? yv.attackDbPerBeat : 0.0;
                const double da = ya - ra;
                const bool attacksDiffer = std::abs (da) >= 4.0 && juce::jmax (ra, ya) >= 2.0 * juce::jmax (0.5, juce::jmin (ra, ya));

                if (std::abs (dh) >= 0.2 || std::abs (dd) >= 3.0 || attacksDiffer)
                    out.push_back ({ std::abs (dh) * 4.0 + std::abs (dd) * 0.3 + (attacksDiffer ? std::abs (da) * 0.15 : 0.0), "articulation", art (rv), art (yv),
                                     dh > 0 || dd > 0 || da > 0 ? juce::String::fromUTF8 ("hold each note until the next onset (setMidiNotes lengths), fewer re-attacks; legato mode or a longer release")
                                                      : juce::String::fromUTF8 ("shorten the notes (staccato), leave gaps"),
                                     { "release", "play mode", "voice mode" } });
            }

            // Raster gegeneinander verschoben: der Pegelverlauf je 16tel passt
            // um einige Plaetze versetzt deutlich besser (Praxistest 1f:
            // Projekt-Raster ab 0, Musik eine 16tel spaeter, firstDownbeat
            // 0,135 nur fuer die Referenz — jeder Takt wurde verschoben verglichen)
            {
                const auto rl = slotLevels (r), yl = slotLevels (y);
                std::vector<double> ra, ya;

                for (auto& [bar, lv] : rl)
                    if (auto it = yl.find (bar); it != yl.end())
                        for (int k = 0; k < 16; ++k)
                            ra.push_back (lv[(size_t) k]), ya.push_back (it->second[(size_t) k]);

                auto corr = [&] (int shift)
                {
                    double mx = 0, my = 0, sxy = 0, sxx = 0, syy = 0;
                    int n = 0;

                    for (int i = 0; i < (int) ra.size(); ++i)
                        if (const int j = i + shift; j >= 0 && j < (int) ya.size())
                            mx += ra[(size_t) i], my += ya[(size_t) j], ++n;

                    if (n < 16)
                        return 0.0;

                    mx /= n, my /= n;

                    for (int i = 0; i < (int) ra.size(); ++i)
                        if (const int j = i + shift; j >= 0 && j < (int) ya.size())
                            sxy += (ra[(size_t) i] - mx) * (ya[(size_t) j] - my), sxx += (ra[(size_t) i] - mx) * (ra[(size_t) i] - mx),
                            syy += (ya[(size_t) j] - my) * (ya[(size_t) j] - my);

                    return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
                };

                if (ra.size() >= 32)
                {
                    const double c0 = corr (0);
                    int best = 0;
                    double cb = c0;

                    for (int sft = -4; sft <= 4; ++sft)
                        if (const double c = corr (sft); c > cb)
                            cb = c, best = sft;

                    if (best != 0 && cb >= 0.5 && cb - c0 >= 0.15)
                        out.push_back ({ 4.0, "grids do not line up", "bar 1 on the reference grid",
                                         juce::String (std::abs (best)) + juce::String::fromUTF8 (" 16th(s) ") + (best > 0 ? "later" : "earlier")
                                             + " (level per 16th matches " + juce::String (cb, 2) + " shifted vs " + juce::String (c0, 2) + ")",
                                         juce::String::fromUTF8 ("the bars of both sides are compared shifted — give firstDownbeat for both sides (or move your clips) so bar 1 lines up; every per-bar result below is off until then"), {} });
                }
            }

            // Luecken: die Referenz klingt, die eigene Spur schweigt fast —
            // je 16tel nach Takt gepaart, Pegel gegen den mittleren Versatz
            // beider Seiten (Praxistest 1e: zwei fehlende Bassnoten, eine
            // halbe Sekunde Stille; die Notenerkennung hatte die Noten auch
            // in der Referenz nicht gefunden, "keine Abweichung")
            {
                const auto rl = slotLevels (r), yl = slotLevels (y);
                std::vector<double> diffs;
                double refPeak = -120.0;

                for (auto& [bar, lv] : rl)
                    for (auto v : lv)
                        refPeak = juce::jmax (refPeak, v);

                for (auto& [bar, lv] : rl)
                    if (auto it = yl.find (bar); it != yl.end())
                        for (int k = 0; k < 16; ++k)
                            if (lv[(size_t) k] > refPeak - 30.0 && it->second[(size_t) k] > -80.0)
                                diffs.push_back (it->second[(size_t) k] - lv[(size_t) k]);

                if (diffs.size() >= 16)
                {
                    std::sort (diffs.begin(), diffs.end());
                    const double offset = diffs[diffs.size() / 2];
                    juce::StringArray gaps;
                    int gapSlots = 0;
                    const double slotSec = r.bpm > 0.0 ? 15.0 / r.bpm : 0.12;

                    for (auto& [bar, lv] : rl)
                    {
                        auto it = yl.find (bar);

                        if (it == yl.end())
                            continue;

                        for (int k = 0; k < 16;)
                        {
                            int e = k;

                            while (e < 16 && lv[(size_t) e] > refPeak - 30.0 && it->second[(size_t) e] - offset < lv[(size_t) e] - 15.0)
                                ++e;

                            if (e - k >= 2)
                            {
                                gaps.add ("bar " + juce::String (bar) + " beat " + juce::String (1.0 + k / 4.0, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                                          + " (" + juce::String (juce::roundToInt ((e - k) * slotSec * 1000.0)) + " ms)");
                                gapSlots += e - k;
                            }

                            k = juce::jmax (e, k + 1);
                        }
                    }

                    if (! gaps.isEmpty())
                        out.push_back ({ 2.5 + 0.1 * gapSlots, "silent where the reference sounds", "sounds there",
                                         gaps.size() > 4 ? gaps.joinIntoString (", ", 0, 4) + " (+" + juce::String (gaps.size() - 4) + " more)" : gaps.joinIntoString (", "),
                                         juce::String::fromUTF8 ("notes are missing or end too early there — the note detection can miss held reference notes, so check those beats in the reference (describeVoice with start/end, detail) and fill them (setMidiNotes)"), {} });
                }
            }

            // Besetzung ueber die Zeit: die Referenz wechselt an einer Stelle
            // den Klang (ein Instrument kommt dazu oder geht), die eigene
            // Spur nicht — oder umgekehrt (gepaart nach Zeit, +-1 Takt)
            {
                const double barSec = ! yv.bars.empty() ? yv.bars.front().end - yv.bars.front().start : 2.0;

                auto near = [barSec] (const std::vector<VoiceData::Change>& cs, double t)
                {
                    for (auto& c : cs)
                        if (std::abs (c.time - t) <= 1.25 * barSec)
                            return true;

                    return false;
                };

                // Taktnummer der eigenen Spur zur selben Zeit
                auto yourBar = [&yourBarAt] (const VoiceData::Change& c)
                {
                    auto* b = yourBarAt (c.time);
                    return b != nullptr ? b->bar : c.bar;
                };

                auto txt = [] (const VoiceData::Change& c, int bar)
                {
                    return "bar " + juce::String (bar) + (c.more.isEmpty() ? juce::String() : " (more " + c.more.joinIntoString (", ") + ")");
                };

                if (! r.timeline.empty() && ! y.timeline.empty())
                {
                    for (auto& c : r.changes)
                        if (! near (y.changes, c.time))
                        {
                            out.push_back ({ 1.5, "sound changes over time", "changes at " + txt (c, yourBar (c)), "no change there",
                                             juce::String::fromUTF8 ("in the reference an instrument enters or leaves at bar ") + juce::String (yourBar (c))
                                                 + juce::String::fromUTF8 (" — describe the reference's bars before and after separately (describeVoice/describeTimbre with start/end) and build the second part on its own track"), {} });
                            break;
                        }

                    for (auto& c : y.changes)
                        if (! near (r.changes, c.time))
                        {
                            out.push_back ({ 1.0, "sound changes over time", "no change there", "changes at " + txt (c, c.bar),
                                             juce::String::fromUTF8 ("yours changes its sound at bar ") + juce::String (c.bar) + juce::String::fromUTF8 (", the reference does not — check that bar"), {} });
                            break;
                        }
                }
            }

            // Akkordanschlaege ueber gehaltenen Noten: die Referenz hat einen
            // zweiten Part, der Akkorde mit Pausen anschlaegt, die eigene Spur
            // deutlich weniger Akkordanschlaege
            {
                const auto& rc = r.v.chordHits;
                const auto& yc = y.v.chordHits;
                const double rp = known (rc.perBar) ? rc.perBar : 0.0;
                // beide Seiten aus dem Audio gezaehlt (Matthias 2026-10-25, Q5-a:
                // aus den MIDI-Noten gezaehlt meldete die eigene Spur gegen
                // ihren eigenen Render fehlende Anschlaege — Surge)
                const double yp = known (yc.perBar) ? yc.perBar : 0.0;

                if (rc.layer && yp < 0.6 * rp)
                    out.push_back ({ 2.5, "chord hits over the held notes",
                                     juce::String (rp, 1) + juce::String::fromUTF8 (" chord hits/bar (") + juce::String (rc.hiddenPerBar, 1) + " hidden in held notes)",
                                     juce::String (yp, 1) + " chord hits/bar",
                                     juce::String::fromUTF8 ("the reference re-strikes chords with pauses over its held notes — a second instrument. Build it on its own track: describeVoice on the reference shows bars[].hits (X = hit) and hitTones; keep the pad held. Compare the hits track and the pad together (render both) — each alone misses the other"), {} });
            }

            // Rhythmischer Part unter den Noten (Pulse): die Referenz schlaegt
            // klingende Toene im 16tel-Takt neu an, die eigene Spur nicht
            // (oder umgekehrt) — Notenvergleich und Artikulation sehen das nicht
            {
                const auto& rp = rv.pulse;
                const auto& yp = yv.pulse;
                auto txt = [] (const voice::Pulse& p)
                {
                    if (! p.layer)
                        return juce::String ("none (the onsets follow the notes)");

                    return juce::String (p.perBar, 1) + " onsets/bar" + (p.layer ? " on " + p.grid : juce::String())
                           + (p.layer && p.heldShare < 0.5 && known (p.decayMs) ? ", decay ~" + juce::String (juce::roundToInt (p.decayMs)) + " ms" : juce::String())
                           + (p.layer && p.chord.isNotEmpty() ? ", tones " + p.chord : juce::String());
                };
                const double rc = rp.layer ? rp.perBar : 0.0, yc = yp.layer ? yp.perBar : 0.0;

                if (rp.layer && yc < 0.6 * rc)
                    out.push_back ({ 3.0, "rhythmic part under the notes", txt (rp), txt (yp),
                                     juce::String::fromUTF8 ("the reference re-strikes its tones in a pulse that the notes do not show — add a second track with short notes on the pulse pattern (describeVoice on the reference: bars[].pulse, pulseTones), short amp envelope (decay ~")
                                         + (known (rp.decayMs) ? juce::String (juce::roundToInt (rp.decayMs)) : juce::String ("150")) + " ms, sustain low)", { "decay", "sustain" } });
                else if (yp.layer && rc < 0.6 * yc)
                    out.push_back ({ 2.0, "rhythmic part under the notes", txt (rp), txt (yp),
                                     juce::String::fromUTF8 ("yours re-strikes in a pulse the reference does not have — hold the notes instead (longer notes, no repeated hits, no gate/tremolo)"), {} });
                else if (rp.layer && yp.layer && known (rp.decayMs) && known (yp.decayMs)
                         && juce::jmax (rp.decayMs, yp.decayMs) >= 2.0 * juce::jmin (rp.decayMs, yp.decayMs) && std::abs (rp.decayMs - yp.decayMs) >= 60.0)
                    out.push_back ({ 1.0, "pulse decay", txt (rp), txt (yp),
                                     yp.decayMs > rp.decayMs ? juce::String ("shorter pulses: lower decay/sustain of the amp envelope") : juce::String ("longer pulses: more decay/sustain"),
                                     { "decay", "sustain" } });
            }

            // Lage
            if (const int dp = (int) std::round (yv.meanPitch - rv.meanPitch); std::abs (dp) >= 5)
                out.push_back ({ std::abs (dp) / 5.0, "register", voice::noteName (rv.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (rv.highest),
                                 voice::noteName (yv.lowest) + juce::String::fromUTF8 ("–") + voice::noteName (yv.highest),
                                 "transpose by " + juce::String (-dp) + " semitones" + (std::abs (std::abs (dp) - 12) <= 2 ? " (an octave)" : ""), {} });

            // Tragende Toene je Takt (klingende Dauer x Pegel, die Toene, die
            // 80 % davon ausmachen) — nach Taktnummer gepaart
            {
                auto main = [] (const voice::Bar& b)
                {
                    auto w = b.weights;
                    std::sort (w.begin(), w.end(), [] (auto& x, auto& y) { return x.second > y.second; });
                    double total = 0.0, acc = 0.0;

                    for (auto& [p, x] : w)
                        total += x;

                    std::vector<int> out;

                    for (auto& [p, x] : w)
                    {
                        if (acc >= 0.8 * total || out.size() >= 4)
                            break;

                        out.push_back (p);
                        acc += x;
                    }

                    return out;
                };

                juce::StringArray bars;
                double weight = 0.0;

                for (auto& rb : rv.bars)
                {
                    auto yit = yourBarAt (rb.start);

                    if (yit == nullptr || rb.bar < 1 || yit->bar < 1 || bars.size() >= 4)
                        continue;

                    // Meldung in der Zaehlung der eigenen Spur (dort wird geaendert)
                    const auto barTxt = "bar " + juce::String (yit->bar) + (yit->bar != rb.bar ? " (ref bar " + juce::String (rb.bar) + ")" : juce::String());

                    auto rm = main (rb), ym = main (*yit);

                    if (rm.empty() || ym.empty())
                        continue;

                    // Gewichtetes Tonklassen-Profil (Kosinus): zwei fast gleich
                    // schwere Nebentoene kippen sonst die Menge
                    std::array<double, 12> rp {}, yp {};

                    for (auto& [p, x] : rb.weights) rp[(size_t) (p % 12)] += x;
                    for (auto& [p, x] : yit->weights) yp[(size_t) (p % 12)] += x;

                    double dot = 0.0, nr = 0.0, ny = 0.0;

                    for (size_t k = 0; k < 12; ++k)
                        dot += rp[k] * yp[k], nr += rp[k] * rp[k], ny += yp[k] * yp[k];

                    const double cosine = nr > 0.0 && ny > 0.0 ? dot / std::sqrt (nr * ny) : 1.0;
                    const double oct = (double) ym[0] - (double) rm[0];   // tragendster Ton

                    if (cosine < 0.6)
                        bars.add (barTxt + ": ref " + pitchList (rm) + " / yours " + pitchList (ym)), weight += 0.6;
                    else if (std::abs (oct) >= 9.0 && rm[0] % 12 == ym[0] % 12)
                        bars.add (barTxt + ": " + voice::noteName (ym[0]) + " instead of " + voice::noteName (rm[0])), weight += 0.4;
                }

                if (! bars.isEmpty())
                    out.push_back ({ juce::jmin (2.5, weight), "main notes per bar", "", "", bars.joinIntoString ("; "), {} });
            }

            // Stimmen und Staffelung
            if (const double dv = yv.voices - rv.voices; std::abs (dv) >= 0.6)
                out.push_back ({ std::abs (dv), "voicing", voicesText (rv.voices), voicesText (yv.voices),
                                 dv > 0 ? juce::String::fromUTF8 ("drop chord tones — fewer simultaneous notes") : juce::String ("add chord tones"), {} });

            if (known (rv.staggerMs) && rv.staggerMs >= 40.0 && (! known (yv.staggerMs) || yv.staggerMs < 20.0))
                out.push_back ({ 1.0, "chord onsets", "staggered ~" + ms (rv.staggerMs), known (yv.staggerMs) ? "block (" + ms (yv.staggerMs) + ")" : juce::String ("no chords"),
                                 "offset the voices by ~" + ms (rv.staggerMs) + juce::String::fromUTF8 (" (lower voice first or strum) instead of block chords"), {} });

            // Tonhoehenverlauf
            if (rv.glideShare - yv.glideShare >= 0.2)
                out.push_back ({ 1.0 + 3.0 * (rv.glideShare - yv.glideShare), "glide",
                                 juce::String (juce::roundToInt (rv.glideShare * 100)) + "% of legato note changes, ~" + ms (rv.glideMs),
                                 juce::String (juce::roundToInt (yv.glideShare * 100)) + "%",
                                 juce::String::fromUTF8 ("portamento ~") + ms (rv.glideMs) + juce::String::fromUTF8 (" (mono/legato fingered mode slides only between overlapping notes) or pitch bends into the notes"),
                                 { "portamento", "glide", "play mode", "voice mode", "legato" } });
            else if (yv.glideShare - rv.glideShare >= 0.2)
                out.push_back ({ 1.0 + 2.0 * (yv.glideShare - rv.glideShare), "glide", juce::String (juce::roundToInt (rv.glideShare * 100)) + "%",
                                 juce::String (juce::roundToInt (yv.glideShare * 100)) + "% of legato note changes slide", "turn portamento off", { "portamento", "glide" } });

            if (rv.vibratoShare - yv.vibratoShare >= 0.25)
                out.push_back ({ 1.0, "vibrato", juce::String (juce::roundToInt (rv.vibratoCents)) + " c @ " + juce::String (rv.vibratoHz, 1) + " Hz", "none",
                                 "add vibrato (LFO to pitch ~" + juce::String (rv.vibratoHz, 1) + " Hz)", { "vibrato", "lfo" } });

            if (known (rv.tuneCents) && known (yv.tuneCents) && std::abs (rv.tuneCents - yv.tuneCents) > 15.0)
                out.push_back ({ 0.7, "tuning", juce::String (juce::roundToInt (rv.tuneCents)) + " c", juce::String (juce::roundToInt (yv.tuneCents)) + " c",
                                 "fine-tune by " + juce::String (juce::roundToInt (rv.tuneCents - yv.tuneCents)) + " cents", { "fine", "tune" } });

            if (known (rv.swellDb) && known (yv.swellDb) && rv.swellDb - yv.swellDb >= 2.5)
                out.push_back ({ 0.8, "dynamics", "swells +" + juce::String (rv.swellDb, 1) + " dB after the attack", juce::String (yv.swellDb, 1) + " dB",
                                 "slower amp/filter attack or a volume swell per note", { "attack" } });

            // Rhythmus: Einsaetze je Takt
            {
                auto onsets = [] (const voice::Voice& v)
                {
                    std::vector<double> c;

                    for (auto& b : v.bars)
                        c.push_back ((double) b.pattern.retainCharacters ("x").length());

                    std::sort (c.begin(), c.end());
                    return c.empty() ? 0.0 : c[c.size() / 2];
                };

                const double ro = onsets (rv), yo = onsets (yv);

                if (ro > 0 && yo > 0 && std::abs (yo - ro) >= 2.0 && (yo / ro >= 1.6 || ro / yo >= 1.6))
                    out.push_back ({ juce::jmin (2.0, std::abs (std::log2 (yo / ro))), "rhythm", juce::String (ro, 0) + " onsets/bar",
                                     juce::String (yo, 0) + " onsets/bar", yo > ro ? juce::String ("play fewer, longer notes") : juce::String ("play more onsets"), {} });
            }

            if (! r.timbre || ! y.timbre)
                return out;

            const auto& rt = r.tb;
            const auto& yt = y.tb;

            // Huellkurve nur aus mindestens drei Noten mit klarem Einsatz je Seite
            const bool envOk = rt.envelopeNotes >= 3 && yt.envelopeNotes >= 3;

            if (envOk && known (rt.attackMs) && known (yt.attackMs) && std::abs (rt.attackMs - yt.attackMs) >= 15.0
                && juce::jmax (rt.attackMs, yt.attackMs) >= 2.0 * juce::jmax (1.0, juce::jmin (rt.attackMs, yt.attackMs)))
                out.push_back ({ 1.2, "attack", ms (rt.attackMs), ms (yt.attackMs), "amp envelope attack ~" + ms (rt.attackMs), { "attack" } });

            if (envOk && known (rt.sustainDb) && known (yt.sustainDb) && std::abs (rt.sustainDb - yt.sustainDb) >= 6.0)
                out.push_back ({ 0.8, "sustain", juce::String (rt.sustainDb, 1) + " dB", juce::String (yt.sustainDb, 1) + " dB",
                                 rt.sustainDb > yt.sustainDb ? juce::String ("raise sustain / longer decay") : juce::String ("lower sustain, shorter decay"), { "sustain", "decay" } });

            if (envOk && known (rt.releaseMs) && known (yt.releaseMs) && std::abs (rt.releaseMs - yt.releaseMs) >= 150.0)
                out.push_back ({ 0.6, "release", ms (rt.releaseMs), ms (yt.releaseMs), "release ~" + ms (rt.releaseMs), { "release" } });

            // Helligkeit aus dem Langzeit-Spektrum beider Seiten (V1)
            if (known (rt.longTermCentroidHz) && known (yt.longTermCentroidHz) && rt.longTermCentroidHz > 0 && yt.longTermCentroidHz > 0)
                if (const double oct = std::log2 (yt.longTermCentroidHz / rt.longTermCentroidHz); std::abs (oct) >= 0.5)
                    out.push_back ({ std::abs (oct) * 1.5, "brightness", juce::String (juce::roundToInt (rt.longTermCentroidHz)) + " Hz centroid (long-term spectrum)",
                                     juce::String (juce::roundToInt (yt.longTermCentroidHz)) + " Hz", oct > 0 ? juce::String ("darker: filter cutoff down") : juce::String ("brighter: filter cutoff up"),
                                     { "cutoff", "filter", "bright" } });

            // Durchsetzung: Anschlag, Praesenz, Rauheit (Praxistest 1k: die
            // Akkorde setzten sich nicht durch — weich, sauber, ohne Mitten-Hoehen)
            // fuer die CLAP-Klasse unten: weichen Anschlag oder Rauheit ab,
            // fuehrt das Etikett allein zum falschen Klang
            juce::StringArray traits, tries;
            const bool attackOff = known (rt.transientDb) && known (yt.transientDb) && std::abs (rt.transientDb - yt.transientDb) >= 3.0;

            if (attackOff && rt.transientDb > yt.transientDb)
                traits.add ("struck harder"), tries.add ("a sharper attack (plucked/picked, MDA Transient)");
            else if (attackOff)
                traits.add ("softer"), tries.add ("a softer attack");

            if (attackOff)
                out.push_back ({ 1.1, "attack strength", juce::String (rt.transientDb, 1) + " dB transient", juce::String (yt.transientDb, 1) + " dB",
                                 rt.transientDb > yt.transientDb
                                     ? juce::String::fromUTF8 ("harder hits: shorter amp attack and decay to a lower sustain, a transient shaper (MDA Transient: attack up), a compressor with slower attack (lets the hit through), or a plucked/picked sound")
                                     : juce::String::fromUTF8 ("softer hits: longer amp attack, a compressor with fast attack, or less transient (MDA Transient: attack down)"),
                                 { "attack", "decay", "sustain" } });

            // Praesenz und Rauheit nur, wenn die Hoehen auf einer Seite
            // hoerbar sind (Paket 13: 4OSC-Renders desselben Stands streuten
            // bei -85 dB um 3 dB; die Flachheit eines stummen Bands ist Rauschen)
            const bool highsHeard = ! (known (rt.presenceDb) && known (yt.presenceDb)) || juce::jmax (rt.presenceDb, yt.presenceDb) >= -60.0;

            if (known (rt.presenceDb) && known (yt.presenceDb) && highsHeard && std::abs (rt.presenceDb - yt.presenceDb) >= 3.0)
                out.push_back ({ 0.6, "presence", juce::String (rt.presenceDb, 1) + " dB at 1-5 kHz", juce::String (yt.presenceDb, 1) + " dB",
                                 rt.presenceDb > yt.presenceDb
                                     ? "more presence: EQ +" + juce::String (juce::roundToInt (rt.presenceDb - yt.presenceDb)) + juce::String::fromUTF8 (" dB around 2–4 kHz (or less below 500 Hz); drive adds upper harmonics")
                                     : "less presence: EQ -" + juce::String (juce::roundToInt (yt.presenceDb - rt.presenceDb)) + juce::String::fromUTF8 (" dB around 2–4 kHz"),
                                 { "high", "mid", "gain" } });

            // relativ: doppelt so rau zaehlt, auch wenn die Flachheit klein
            // bleibt (Praxistest 1k: Referenz 0,12 gegen 0,05 — verzerrt gegen sauber)
            const bool roughOff = highsHeard && known (rt.roughness) && known (yt.roughness) && std::abs (rt.roughness - yt.roughness) >= 0.04
                                  && juce::jmax (rt.roughness, yt.roughness) >= 1.8 * juce::jmax (0.01, juce::jmin (rt.roughness, yt.roughness));

            if (roughOff && rt.roughness > yt.roughness)
                traits.add ("rougher"), tries.add ("drive (Overdrive after the preset, or a driven synth / distorted guitar preset)");
            else if (roughOff)
                traits.add ("cleaner"), tries.add ("less drive");

            if (roughOff)
                out.push_back ({ 1.3, "roughness", juce::String (rt.roughness, 2) + " (2-8 kHz flatness)", juce::String (yt.roughness, 2),
                                 rt.roughness > yt.roughness
                                     ? juce::String::fromUTF8 ("rougher/driven: drive or distortion (MDA Overdrive, MDA Bandisto, Surge XT Effects distortion/waveshaper, 4OSC Distortion), or a noise/detuned layer")
                                     : juce::String::fromUTF8 ("cleaner: less drive/distortion, fewer detuned voices"),
                                 { "drive", "dist", "noise" } });

            if (known (rt.slopeDbOct) && known (yt.slopeDbOct) && std::abs (rt.slopeDbOct - yt.slopeDbOct) >= 4.0)
                out.push_back ({ 0.7, "overtones", rt.wave + " " + juce::String (rt.slopeDbOct, 1) + " dB/oct", yt.wave + " " + juce::String (yt.slopeDbOct, 1) + " dB/oct",
                                 "oscillator shape / filter slope toward " + rt.wave, { "osc", "shape", "wave" } });

            if (known (rt.noteSweepOct) && known (yt.noteSweepOct) && std::abs (rt.noteSweepOct - yt.noteSweepOct) >= 0.4)
                out.push_back ({ 0.8, "filter envelope", juce::String (rt.noteSweepOct, 1) + " oct per note", juce::String (yt.noteSweepOct, 1) + " oct",
                                 rt.noteSweepOct > yt.noteSweepOct ? juce::String ("more filter envelope amount (closes within the note)") : juce::String ("less filter envelope"),
                                 { "env amount", "filter eg", "envelope" } });

            if (known (rt.barSweepOct) && known (yt.barSweepOct) && std::abs (rt.barSweepOct - yt.barSweepOct) >= 0.3)
                out.push_back ({ 0.8, "filter over the part", juce::String (rt.barSweepOct, 1) + " oct", juce::String (yt.barSweepOct, 1) + " oct",
                                 "automate the cutoff across the bars (automation)", { "cutoff" } });

            if (known (rt.correlation) && known (yt.correlation) && std::abs (rt.correlation - yt.correlation) >= 0.3)
                out.push_back ({ std::abs (rt.correlation - yt.correlation) * 2.5, "stereo width", "correlation " + f1 (rt.correlation), "correlation " + f1 (yt.correlation),
                                 rt.correlation < yt.correlation ? juce::String::fromUTF8 ("wider: unison detune with stereo spread, stereo chorus — or two takes panned") : juce::String ("narrower: less unison/chorus, toward mono"),
                                 { "unison", "detune", "width", "spread", "chorus" } });

            if (known (rt.widthWobbleDb) && known (yt.widthWobbleDb) && rt.widthWobbleDb - yt.widthWobbleDb >= 1.5)
                out.push_back ({ 0.7, "chorus", juce::String::fromUTF8 ("width moves ±") + juce::String (rt.widthWobbleDb, 1) + " dB", juce::String (yt.widthWobbleDb, 1) + " dB",
                                 "add chorus/ensemble", { "chorus" } });

            // Verstimmung: Breite der Teiltoene (Cent) statt Pegelschwebung
            // (Todo 2026-10-27-comparevoice-detune)
            if (known (rt.detuneCents) && known (yt.detuneCents) && std::abs (rt.detuneCents - yt.detuneCents) >= 5.0
                && juce::jmax (rt.detuneCents, yt.detuneCents) >= 8.0)
                out.push_back ({ 0.6, "detune", juce::String::fromUTF8 ("partials ±") + juce::String (juce::roundToInt (rt.detuneCents)) + " cents",
                                 juce::String::fromUTF8 ("±") + juce::String (juce::roundToInt (yt.detuneCents)) + " cents",
                                 rt.detuneCents > yt.detuneCents ? juce::String ("detune oscillators / unison voices (or add chorus)")
                                                                 : juce::String ("less detune / fewer unison voices or chorus"),
                                 { "detune", "unison", "chorus" } });

            if (known (rt.tailSec) != known (yt.tailSec) || (known (rt.tailSec) && known (yt.tailSec) && std::abs (rt.tailSec - yt.tailSec) >= 0.5))
                if (known (rt.tailSec) && (! known (yt.tailSec) || rt.tailSec > yt.tailSec))
                    out.push_back ({ 0.6, "tail", juce::String (rt.tailSec, 1) + " s", known (yt.tailSec) ? juce::String (yt.tailSec, 1) + " s" : juce::String ("none"),
                                     "more reverb or a longer release", { "reverb", "release" } });

            // Hall: Abklingen und Pegel der Fahne; Breite der Fahne (Stereo-Hall)
            if (known (rt.rt60Sec) && known (yt.rt60Sec) && std::abs (rt.rt60Sec - yt.rt60Sec) >= juce::jmax (0.3, 0.3 * rt.rt60Sec))
                out.push_back ({ 0.6, "reverb time", "RT60 " + juce::String (rt.rt60Sec, 1) + " s", "RT60 " + juce::String (yt.rt60Sec, 1) + " s",
                                 rt.rt60Sec > yt.rt60Sec ? juce::String ("longer reverb (room size/decay) or release") : juce::String ("shorter reverb or release"),
                                 { "decay", "size", "room", "release" } });

            if (known (rt.tailDb) && known (yt.tailDb) && std::abs (rt.tailDb - yt.tailDb) >= 6.0)
                out.push_back ({ 0.6, "reverb level", "tail " + juce::String (rt.tailDb, 1) + " dB", juce::String (yt.tailDb, 1) + " dB",
                                 rt.tailDb > yt.tailDb ? juce::String ("more reverb (wet) or a longer release") : juce::String ("less reverb (wet)"),
                                 { "wet", "mix", "reverb", "release" } });

            // Ohne messbare Fahne auf der eigenen Seite (trocken) zaehlt sie als
            // nicht breiter als die Note
            if (known (rt.tailCorrelation) && known (rt.noteCorrelation)
                && (rt.noteCorrelation - rt.tailCorrelation)
                       - (known (yt.tailCorrelation) && known (yt.noteCorrelation) ? yt.noteCorrelation - yt.tailCorrelation : 0.0) >= 0.3)
                out.push_back ({ 0.5, "reverb width", "tail correlation " + f1 (rt.tailCorrelation) + " (notes " + f1 (rt.noteCorrelation) + ")",
                                 known (yt.tailCorrelation) ? f1 (yt.tailCorrelation) + " (notes " + f1 (yt.noteCorrelation) + ")" : juce::String ("no tail"),
                                 "a stereo reverb (the tail spreads wider than the dry notes)", { "reverb", "width" } });

            // Delay: Abstand und Rueckkopplung
            if (known (rt.echoMs) && ! known (yt.echoMs))
                out.push_back ({ 0.8, "delay", juce::String ((int) rt.echoMs) + " ms, " + juce::String (rt.echoFeedbackDb, 1) + " dB per repeat", "none",
                                 "add a delay (time " + juce::String ((int) rt.echoMs) + " ms)", { "delay", "time", "feedback" } });
            else if (! known (rt.echoMs) && known (yt.echoMs))
                out.push_back ({ 0.6, "delay", "none", juce::String ((int) yt.echoMs) + " ms", "remove the delay (or less feedback/wet)", { "delay", "wet" } });
            else if (known (rt.echoMs) && known (yt.echoMs))
            {
                if (std::abs (rt.echoMs - yt.echoMs) >= juce::jmax (15.0, 0.1 * rt.echoMs))
                    out.push_back ({ 0.7, "delay time", juce::String ((int) rt.echoMs) + " ms", juce::String ((int) yt.echoMs) + " ms",
                                     "set the delay time to " + juce::String ((int) rt.echoMs) + " ms", { "time", "delay" } });

                if (std::abs (rt.echoFeedbackDb - yt.echoFeedbackDb) >= 3.0)
                    out.push_back ({ 0.5, "delay feedback", juce::String (rt.echoFeedbackDb, 1) + " dB per repeat", juce::String (yt.echoFeedbackDb, 1) + " dB",
                                     rt.echoFeedbackDb > yt.echoFeedbackDb ? juce::String ("more feedback (repeats fade slower)") : juce::String ("less feedback"),
                                     { "feedback" } });
            }

            // Klasse nur bei klarem Abstand: beide Seiten sehen ihre Spitze
            // deutlich vor der Spitze der anderen (CLAP-Werte liegen dicht,
            // ein knapper Wechsel kippt schon zwischen zwei Renders)
            if (r.classScores.size() == (size_t) classLabels().size() && y.classScores.size() == r.classScores.size())
            {
                auto top = [] (const std::vector<float>& v) { return (size_t) (std::max_element (v.begin(), v.end()) - v.begin()); };
                const auto rt0 = top (r.classScores), yt0 = top (y.classScores);

                // Rauheit und Anschlag wiegen mehr als das Etikett (Praxistest
                // 1l: "string ensemble" fuehrte zu sauberen Streichern, die
                // Referenz war rau wie ein Synth mit Drive)
                const auto cls = classLabels()[(int) rt0];
                const auto fix = traits.isEmpty()
                    ? juce::String ("findPresets {like: reference} for the class")
                    : "sounds like " + cls + ", but is " + traits.joinIntoString (" and ") + " than yours"
                          + juce::String::fromUTF8 (" — findPresets {like} follows the label, not that: try a variant with ")
                          + tries.joinIntoString (" and ") + ", then compare again";

                if (rt0 != yt0 && y.classScores[yt0] - y.classScores[rt0] >= 0.02f && r.classScores[rt0] - r.classScores[yt0] >= 0.02f)
                    out.push_back ({ 0.5, "instrument (CLAP)", cls + " " + juce::String (r.classScores[rt0], 2),
                                     classLabels()[(int) yt0] + " " + juce::String (y.classScores[yt0], 2), fix, {} });
            }

            return out;
        }
    }

    juce::var opCompareVoice (DawdContext& ctx, const juce::var& cmd)
    {
        auto refSpec = cmd.getProperty ("reference", {});

        if (! refSpec.isObject() || ! (refSpec.hasProperty ("file") || refSpec.hasProperty ("track")))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("reference needs {file, start?, end?, bpm?, firstDownbeat?} or a track of the project {track, section? | start?/end?}") } });

        const bool withTimbre = (bool) cmd.getProperty ("timbre", true);
        VoiceData r, y;

        // Eigene Seite: der Befehl selbst (file oder Mix/Spur/Section),
        // ohne die Felder der Referenz
        auto own = cmd.clone();

        for (auto key : { "reference", "timbre", "limit", "detail" })
            own.getDynamicObject()->removeProperty (key);

        refSpec = refSpec.clone();
        juce::StringArray gridHints;
        shareGrid (ctx, own, refSpec, gridHints);

        // Tonhoehenbereich oben gilt fuer beide Seiten (Matthias 2026-10-24,
        // V2; Praxistest 1h: Vergleich je Register); einer in reference
        // gilt nur dort und geht vor
        auto ranged = [] (const juce::var& v) { return v.hasProperty ("minPitch") || v.hasProperty ("maxPitch"); };
        const bool referenceOnly = ranged (refSpec) && ! ranged (own);

        for (auto key : { "minPitch", "maxPitch" })
            if (own.hasProperty (key) && ! refSpec.hasProperty (key))
                refSpec.getDynamicObject()->setProperty (key, own.getProperty (key, {}));

        // Externes Instrument im Mix: zweiter Render, Streuung als Hinweis
        // (Matthias 2026-10-27, A; Todo 2026-10-26-surge-render-schwankt)
        y.src.wantSpread = r.src.wantSpread = true;

        if (auto e = analyse (ctx, own, y, withTimbre); e.isNotEmpty())
            return resultObj (false, { { "error", "yours: " + e } });

        // Referenz-Datei ohne end: so lang wie die eigene Seite (wie compareReference)
        auto ref = refSpec.clone();

        if (ref.hasProperty ("file") && ! ref.hasProperty ("end"))
        {
            const double from = (double) ref.getProperty ("start", 0.0);
            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (juce::File (ref.getProperty ("file", {}).toString())));

            if (reader != nullptr)
                ref.getDynamicObject()->setProperty ("end", juce::jmin ((double) reader->lengthInSamples / reader->sampleRate, from + (y.src.end - y.src.start)));
        }

        if (auto e = analyse (ctx, ref, r, withTimbre); e.isNotEmpty())
            return resultObj (false, { { "error", "reference: " + e } });

        if (y.v.notes.empty())
            return resultObj (false, { { "error", juce::String::fromUTF8 ("your side is silent in this range — no notes to compare (analyze {track} shows the level)") } });

        auto devs = compare (r, y);
        std::sort (devs.begin(), devs.end(), [] (auto& a, auto& b) { return a.weight > b.weight; });
        const int limit = juce::jlimit (1, 20, (int) cmd.getProperty ("limit", 8));
        juce::Array<juce::var> list;

        for (auto& dv : devs)
        {
            if (list.size() >= limit)
                break;

            auto o = new juce::DynamicObject();
            o->setProperty ("what", dv.what);

            if (dv.ref.isNotEmpty())
            {
                o->setProperty ("ref", dv.ref);
                o->setProperty ("yours", dv.yours);
            }

            o->setProperty ("fix", dv.fix);

            auto ps = paramsFor (ctx, y.plugin, dv.keys);

            // Glide nur mit einem Regler fuer die Gleitzeit; dessen Namen
            // und Einheit in fix (4OSC: "Legato" ist die Gleitzeit)
            if (dv.what == "glide" && y.plugin.isNotEmpty() && findModuleTrack (*ctx.edit, y.plugin) != nullptr)
            {
                const auto times = glideTimeParams (ctx, y.plugin);
                const bool add = dv.keys.contains ("legato");

                if (times.isEmpty())
                {
                    ps.clear();

                    if (add)
                        o->setProperty ("fix", dv.fix + juce::String::fromUTF8 (" — this instrument has no glide-time knob: pitch bends into the notes (setControllers) or an instrument with portamento"));
                }
                else
                {
                    for (int i = times.size(); --i >= 0;)
                    {
                        ps.removeString (times[i]);
                        ps.insert (0, times[i]);
                    }

                    o->setProperty ("fix", dv.fix + juce::String::fromUTF8 (" — glide time here: ") + times.joinIntoString (", ")
                                    + (add ? juce::String::fromUTF8 (" (ms); the mode must let notes glide (mono/legato)") : juce::String::fromUTF8 (" to 0")));
                }
            }

            if (! ps.isEmpty())
                o->setProperty ("params", juce::var (ps));

            list.add (juce::var (o));
        }

        auto side = [&] (const VoiceData& d)
        {
            auto s = voiceSummary (d);
            describeAnalysisSource (*s.getDynamicObject(), d.src);

            if (d.v.hasGrid)
                s.getDynamicObject()->setProperty ("gridFrom", d.gridFrom);

            if (! d.classes.isEmpty())
                s.getDynamicObject()->setProperty ("soundsLike", d.classes[0]);

            if (d.plugin.isNotEmpty())
                s.getDynamicObject()->setProperty ("instrument", d.plugin);

            return s;
        };

        auto res = resultObj (true, { { "reference", side (r) }, { "yours", side (y) }, { "deviations", list } });

        // Bereich nur in reference: vergleicht ein Register mit allem
        if (referenceOnly)
            res.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("minPitch/maxPitch inside reference narrow only the reference — put them at the top level to narrow both sides to one register"));

        // Clipping (2026-10-25): ein 16/24-Bit-Render schneidet ueber 0 dBFS
        // ab, die Verzerrung bringt Geistertoene unter und ueber den Noten,
        // Einsaetze und Rauschen — gegen den eigenen Render meldete
        // compareVoice Register, Stimmen und Rhythmus. Nur Hinweis: die
        // Abweichungen sind im Audio.
        juce::StringArray hints (gridHints);

        for (auto* d : { &r, &y })
        {
            const juce::String who = d == &r ? "reference" : "yours";

            if (d->src.clipRuns >= 3)
                hints.add (who + " clips: " + juce::String (d->src.clipRuns)
                           + juce::String::fromUTF8 (" flat peaks at 0 dBFS — clipping adds tones below and above the notes, onsets and noise; register, voicing, rhythm, overtones and presence can come from it. Compare unclipped audio (lower the level before rendering, or render with bitDepth 32)"));
            else if (d->src.file.isEmpty() && d->src.peak > 1.0f)
                hints.add (who + " peaks at +" + juce::String (juce::Decibels::gainToDecibels (d->src.peak), 1)
                           + juce::String::fromUTF8 (" dBFS — measured unclipped (float), but a 16/24-bit render clips there: lower the level before you render it as a reference"));
        }

        for (auto* d : { &r, &y })
            if (auto h = spreadHint (d->src, d == &r ? "reference" : "yours"); h.isNotEmpty())
                hints.add (h);

        if (devs.empty())
            hints.add (juce::String::fromUTF8 ("no clear deviation in articulation, pitch, voicing, rhythm or sound"));

        if (! hints.isEmpty())
            res.getDynamicObject()->setProperty ("hints", hintVar (hints));

        if (! devs.empty() && (bool) cmd.getProperty ("detail", false))
        {
            res.getDynamicObject()->setProperty ("referenceBars", barsVar (r, 16));
            res.getDynamicObject()->setProperty ("yourBars", barsVar (y, 16));
        }

        return res;
    }
}

namespace dawd
{
    // Alle Spuren gegen ihre Stems in einem Aufruf (Praxistest 1d: je Runde
    // drei Aufrufe mit Pfad, Offsets, BPM und Downbeat): pairs [{track, file,
    // drums?}], gemeinsame Referenz-Felder in reference {start?, end?, bpm?,
    // firstDownbeat?}, eigene Seite wie compareVoice (section, start/end).
    // Je Spur die wichtigsten Abweichungen (limit, Default 3) und der Pegel
    // beider Seiten. Drum-Stems ("drums" im Dateinamen oder drums: true)
    // laufen ueber compareDrums (Praxistest 1f-1k: als Basslinie gelesen).
    juce::var opCompareStems (DawdContext& ctx, const juce::var& cmd)
    {
        auto* pairs = cmd.getProperty ("pairs", {}).getArray();

        if (pairs == nullptr || pairs->isEmpty())
            return resultObj (false, { { "error", "need pairs: [{track, file}]" } });

        auto shared = cmd.getProperty ("reference", {});
        const bool sharedGrid = shared.isObject() && (shared.hasProperty ("bpm") || shared.hasProperty ("firstDownbeat"));
        std::vector<juce::var> rows ((size_t) pairs->size());
        juce::StringArray detected;

        auto isDrums = [] (const juce::var& pr)
        {
            return pr.hasProperty ("drums") ? (bool) pr.getProperty ("drums", false)
                                            : juce::File (pr.getProperty ("file", {}).toString()).getFileNameWithoutExtension().containsIgnoreCase ("drum");
        };

        // Ohne Referenz-Raster gilt das des Drum-Paars fuer alle Paare
        // (Matthias 2026-10-24, D1; Praxistest 1i/1l: jeder Stem schaetzte
        // sein eigenes, Muster um Takte versetzt gepaart) — das erste
        // Drum-Paar laeuft deshalb zuerst
        int drumPair = -1;

        if (! sharedGrid)
            for (int i = 0; i < pairs->size() && drumPair < 0; ++i)
                if (isDrums (pairs->getReference (i)))
                    drumPair = i;

        std::vector<int> order;

        if (drumPair >= 0)
            order.push_back (drumPair);

        for (int i = 0; i < pairs->size(); ++i)
            if (i != drumPair)
                order.push_back (i);

        juce::var grid;   // {file, start, bpm, firstDownbeat, beatsPerBar, gridFrom} des Drum-Paars
        int adopted = 0;

        for (auto i : order)
        {
            auto& pr = pairs->getReference (i);

            if (! pr.hasProperty ("track") || ! pr.hasProperty ("file"))
                return resultObj (false, { { "error", "each pair needs track and file" } });

            auto ref = shared.isObject() ? shared.clone() : juce::var (new juce::DynamicObject());
            auto* ro = ref.getDynamicObject();
            ro->setProperty ("file", pr.getProperty ("file", {}));

            // Eigenes Fenster je Paar (Praxistest 1l: Ganzsong-Stems und
            // Schicht-Dateien ab 0 in einem Aufruf). Ein gemeinsames
            // firstDownbeat behaelt seinen Abstand zum start.
            if (pr.hasProperty ("start") && ! pr.hasProperty ("firstDownbeat") && ro->hasProperty ("firstDownbeat"))
                ro->setProperty ("firstDownbeat", (double) ro->getProperty ("firstDownbeat") - (double) ro->getProperty ("start")
                                                     + (double) pr.getProperty ("start", 0.0));

            for (auto key : { "start", "end", "bpm", "firstDownbeat", "beatsPerBar" })
                if (pr.hasProperty (key))
                    ro->setProperty (key, pr.getProperty (key, {}));

            // Raster der Drums, die Takt-Eins mit gleichem Abstand zum start
            if (grid.isObject() && ! ro->hasProperty ("bpm") && ! ro->hasProperty ("firstDownbeat"))
            {
                ro->setProperty ("bpm", grid.getProperty ("bpm", {}));
                ro->setProperty ("beatsPerBar", grid.getProperty ("beatsPerBar", {}));
                ro->setProperty ("firstDownbeat", (double) ref.getProperty ("start", 0.0)
                                                     + (double) grid.getProperty ("firstDownbeat", 0.0) - (double) grid.getProperty ("start", 0.0));
                ++adopted;
            }

            // Spurname selbst aufloesen: der direkte Aufruf geht nicht ueber
            // executeOp (Praxistest 1e: "Bass" wurde als 0 gelesen, alle
            // Paare massen dieselbe Spur)
            auto track = pr.getProperty ("track", {});

            if (track.isString() && ! track.toString().containsOnly ("-0123456789"))
            {
                const int idx = ctx.edit != nullptr ? trackIndexFor (*ctx.edit, track.toString()) : -1;

                if (idx < 0)
                    return resultObj (false, { { "error", "no such track: " + track.toString() } });

                track = idx;
            }

            auto one = cmd.clone();
            auto* o = one.getDynamicObject();
            o->removeProperty ("pairs");
            o->setProperty ("reference", ref);
            o->setProperty ("track", track);

            if (! cmd.hasProperty ("limit"))
                o->setProperty ("limit", 3);

            const bool drums = isDrums (pr);

            if (drums)
            {
                o->removeProperty ("timbre");
                o->removeProperty ("detail");
            }

            auto r = drums ? opCompareDrums (ctx, one) : opCompareVoice (ctx, one);
            auto row = new juce::DynamicObject();
            row->setProperty ("track", pr.getProperty ("track", {}));
            row->setProperty ("file", pr.getProperty ("file", {}));

            if (! (bool) r.getProperty ("ok", false))
                row->setProperty ("error", r.getProperty ("error", {}));
            else
            {
                auto ys = r.getProperty ("yours", {}), rs = r.getProperty ("reference", {});
                row->setProperty ("compared", drums ? "drums" : "voice");

                if (! drums)
                {
                    row->setProperty ("yours", ys.getProperty ("role", {}));
                    row->setProperty ("reference", rs.getProperty ("role", {}));
                }

                if (ys.hasProperty ("rmsDb") && rs.hasProperty ("rmsDb"))
                {
                    const double yd = ys.getProperty ("rmsDb", 0.0), rd = rs.getProperty ("rmsDb", 0.0);
                    auto lv = new juce::DynamicObject();
                    lv->setProperty ("reference", rd);
                    lv->setProperty ("yours", yd);
                    lv->setProperty ("diff", std::round ((yd - rd) * 10.0) / 10.0);
                    row->setProperty ("levelDb", juce::var (lv));
                }

                row->setProperty ("deviations", r.getProperty ("deviations", {}));

                if (ys.hasProperty ("renderSpread"))
                    row->setProperty ("renderSpread", ys.getProperty ("renderSpread", {}));

                // Hinweise des Vergleichs je Paar (2026-10-25): "reference
                // clips …" / "yours peaks at …" gelten fuer die Seiten dieses
                // Paars und erklaeren seine Abweichungen
                if (r.hasProperty ("hints"))
                    row->setProperty ("hints", r.getProperty ("hints", {}));

                if (i == drumPair && rs.hasProperty ("bpm"))
                    grid = juce::var (resultObj (true, { { "file", pr.getProperty ("file", {}) }, { "start", rs.getProperty ("start", 0.0) },
                                                         { "bpm", rs.getProperty ("bpm", {}) }, { "firstDownbeat", rs.getProperty ("firstDownbeat", {}) },
                                                         { "beatsPerBar", rs.getProperty ("beatsPerBar", 4) }, { "gridFrom", rs.getProperty ("gridFrom", {}) } }));

                if (rs.getProperty ("gridFrom", {}).toString() == "detected")
                    detected.add (juce::File (pr.getProperty ("file", {}).toString()).getFileName());
            }

            rows[(size_t) i] = juce::var (row);
        }

        juce::Array<juce::var> out;

        for (auto& row : rows)
            out.add (row);

        auto res = resultObj (true, { { "tracks", out } });
        juce::StringArray hints;

        if (adopted > 0)
        {
            const auto drumsFile = juce::File (grid.getProperty ("file", {}).toString()).getFileName();
            auto g = grid.clone();
            g.getDynamicObject()->removeProperty ("ok");
            g.getDynamicObject()->removeProperty ("start");
            g.getDynamicObject()->setProperty ("from", "drums");
            res.getDynamicObject()->setProperty ("grid", g);
            detected.removeString (drumsFile);
            hints.add ("bar grid of the drums pair (" + drumsFile + ", " + grid.getProperty ("gridFrom", {}).toString() + ": "
                       + grid.getProperty ("bpm", {}).toString() + " BPM, firstDownbeat " + grid.getProperty ("firstDownbeat", {}).toString()
                       + juce::String::fromUTF8 (") used for all pairs — if bar 1 is off, pass reference {bpm, firstDownbeat} from detectTempo of the full mix"));
        }

        // Ohne Raster schaetzt jede Referenz ihr eigenes (Praxistest 1i: um
        // zwei Takte versetzt verglichen, ohne Warnung)
        if (! detected.isEmpty())
            hints.add ("bar grid of the reference detected per stem (" + detected.joinIntoString (", ")
                       + juce::String::fromUTF8 (") — patterns can pair up shifted by beats or bars: pass reference {bpm, firstDownbeat} from detectTempo of the drums stem or the full mix"));

        if (! hints.isEmpty())
            res.getDynamicObject()->setProperty ("hints", hintVar (hints));

        return res;
    }

    namespace
    {
        // Einsaetze (>= 40 ms Abstand) in Sekunden ab der Takt-Eins
        std::vector<double> onsetTimes (const VoiceData& d, double firstDownbeat)
        {
            std::vector<double> out;
            double last = -1.0e9;

            for (auto& n : d.v.notes)
                if (n.start - last >= 0.04)
                    out.push_back (n.start - firstDownbeat), last = n.start;

            return out;
        }

        // Anteil gemeinsamer Einsaetze (je Referenz-Einsatz einer der eigenen
        // innerhalb von 40 ms), eigene Seite um shift Sekunden frueher gelegt
        // — auf 16tel gerundet kippte ein Einsatz nahe der Grenze schon bei
        // 5 ms Versatz ins Nachbarfeld
        double onsetMatch (const std::vector<double>& r, const std::vector<double>& y, double shift)
        {
            if (r.empty() || y.empty())
                return 0.0;

            int hits = 0;

            for (double t : r)
            {
                auto it = std::lower_bound (y.begin(), y.end(), t + shift - 0.04);
                hits += it != y.end() && *it <= t + shift + 0.04 ? 1 : 0;
            }

            return (double) hits / (double) juce::jmax (r.size(), y.size());
        }
    }

    // Gegenprobe: der eigene Export und der Referenz-Abschnitt laufen durch
    // dieselbe Trennung (6 Stems, mit exposed die gefuehrte Trennung mit den
    // Klaengen der freien Stelle des Originals) und werden Schicht fuer Schicht
    // verglichen — Noten/Anschlaege, Rhythmus (gemeinsame Einsaetze, beste
    // Verschiebung), Klang und Pegelanteil am Ganzen. Trennartefakte treffen
    // beide Seiten gleich; klingt eine Schicht nach der Trennung anders, war
    // der Ansatz fuer diesen Part falsch (Matthias, 2026-10-19).
    juce::var opCompareSeparated (DawdContext& ctx, const juce::var& cmd)
    {
        const auto ref = cmd.getProperty ("reference", {});

        if (! cmd.hasProperty ("file") || ! ref.isObject() || ! ref.hasProperty ("file") || ! ref.hasProperty ("start") || ! ref.hasProperty ("end"))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("need file (your export) and reference {file, start, end, bpm?, firstDownbeat?, exposed?}") } });

        if (! ref.hasProperty ("bpm"))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("need reference.bpm (and firstDownbeat) — the rhythm is compared on the 16th grid") } });

        const double refStart = ref.getProperty ("start", 0.0);
        const double bpm = ref.getProperty ("bpm", 120.0);
        const double refFd = (double) ref.getProperty ("firstDownbeat", refStart) - refStart;            // relativ zum Abschnitt
        const double yourFd = cmd.hasProperty ("firstDownbeat") ? (double) cmd.getProperty ("firstDownbeat", 0.0)
                                                                 : refFd;                                   // Export beginnt am Abschnitt
        // 1. Referenz: Schichten (mit exposed gefuehrt getrennt)
        auto layersRes = opDescribeLayers (ctx, ref);

        if (! (bool) layersRes.getProperty ("ok", false))
            return resultObj (false, { { "error", "reference: " + layersRes.getProperty ("error", {}).toString() } });

        const auto refDir = juce::File (layersRes.getProperty ("dir", {}).toString());
        const auto split = layersRes.getProperty ("split", {});
        juce::StringArray names;
        std::map<juce::String, juce::String> refFiles;

        if (split.isObject())
            for (auto& l : *split.getProperty ("layers", {}).getArray())
                names.add (l["stem"].toString()), refFiles[l["stem"].toString()] = l["file"].toString();
        else
            for (auto& l : *layersRes.getProperty ("layers", {}).getArray())
                names.add (l["stem"].toString()), refFiles[l["stem"].toString()] = l["file"].toString();

        for (auto n : { "bass", "drums" })
            names.add (n), refFiles[n] = refDir.getChildFile (juce::String (n) + ".wav").getFullPathName();

        // 2. Export: dieselbe Trennung
        auto expCmd = cmd.clone();
        expCmd.getDynamicObject()->removeProperty ("reference");
        juce::File expDir;
        bool expCached = false;

        if (auto e = fileStemsNow (ctx, expCmd, 6, expDir, expCached); e.isNotEmpty())
            return resultObj (false, { { "error", "your file: " + e } });

        std::map<juce::String, juce::String> yourFiles;

        for (auto n : { "bass", "drums" })
            yourFiles[n] = expDir.getChildFile (juce::String (n) + ".wav").getFullPathName();

        if (split.isObject())
        {
            // mit den Klaengen der freien Stelle des ORIGINALS trennen
            const auto exposed = layersRes.getProperty ("exposed", {});
            std::vector<guided::Stereo> sources;
            juce::StringArray exStems;

            for (auto& l : *exposed.getProperty ("layers", {}).getArray())
            {
                guided::Stereo x;

                if (! loadStereo (ctx, juce::File (l["file"].toString()), x))
                    return resultObj (false, { { "error", "cannot read " + l["file"].toString() } });

                sources.push_back (std::move (x));
                exStems.add (l["stem"].toString());
            }

            guided::Stereo target;

            for (auto& l : *layersRes.getProperty ("layers", {}).getArray())
            {
                guided::Stereo x;

                if (! loadStereo (ctx, expDir.getChildFile (l["stem"].toString() + ".wav"), x))
                    continue;

                if (target[0].empty())
                    target = x;
                else
                    for (int c = 0; c < 2; ++c)
                        for (size_t i = 0; i < juce::jmin (target[(size_t) c].size(), x[(size_t) c].size()); ++i)
                            target[(size_t) c][i] += x[(size_t) c][i];
            }

            std::vector<guided::Stereo> parts;

            if (auto e = guided::split (sources, target, separation::sampleRate, parts); e.isNotEmpty())
                return resultObj (false, { { "error", "split of your file: " + e } });

            const auto splitDir = expDir.getChildFile ("split like reference");
            splitDir.createDirectory();

            for (size_t i = 0; i < parts.size(); ++i)
            {
                const auto n = i < (size_t) exStems.size() ? exStems[(int) i] : juce::String ("rest");
                const auto f = splitDir.getChildFile ("split-" + n + ".wav");

                if (! saveStereo (f, parts[i], separation::sampleRate))
                    return resultObj (false, { { "error", "cannot write " + f.getFullPathName() } });

                yourFiles[n] = f.getFullPathName();
            }
        }
        else
            for (auto& n : names)
                if (! yourFiles.count (n))
                    yourFiles[n] = expDir.getChildFile (n + ".wav").getFullPathName();

        // Pegelanteil am Ganzen (alle sechs Stems der Seite)
        auto totalDb = [&] (const juce::File& dir)
        {
            double sq = 0.0;

            for (auto* n : separation::stemNames6)
            {
                guided::Stereo x;

                if (loadStereo (ctx, dir.getChildFile (juce::String (n) + ".wav"), x))
                    sq += std::pow (10.0, rmsDb (x) / 10.0);
            }

            return 10.0 * std::log10 (sq + 1.0e-12);
        };

        const double refTotal = totalDb (refDir), yourTotal = totalDb (expDir);
        juce::Array<juce::var> pairs;
        juce::StringArray worst;

        for (auto& n : names)
        {
            guided::Stereo rx, yx;
            const bool haveR = loadStereo (ctx, juce::File (refFiles[n]), rx), haveY = loadStereo (ctx, juce::File (yourFiles[n]), yx);

            if (! haveR || ! haveY)
                continue;

            const double rLevel = rmsDb (rx) - refTotal, yLevel = rmsDb (yx) - yourTotal;
            auto o = new juce::DynamicObject();
            o->setProperty ("name", n);
            o->setProperty ("referenceFile", refFiles[n]);
            o->setProperty ("yourFile", yourFiles[n]);
            o->setProperty ("levelDb", juce::var (resultObj (true, { { "reference", num (rLevel, 0.1) }, { "yours", num (yLevel, 0.1) } })));
            juce::StringArray issues;

            if (std::abs (yLevel - rLevel) >= 3.0)
                issues.add (juce::String (yLevel > rLevel ? "too loud" : "too quiet") + " by " + juce::String (std::abs (yLevel - rLevel), 1) + " dB in the mix");

            if (rLevel < -40.0 && yLevel < -40.0)
            {
                o->setProperty ("verdict", "silent on both sides");
                pairs.add (juce::var (o));
                continue;
            }

            juce::Array<juce::var> devList;

            if (n == "drums")
            {
                auto dc = new juce::DynamicObject();
                dc->setProperty ("reference", juce::var (resultObj (true, { { "file", refFiles[n] }, { "bpm", bpm }, { "firstDownbeat", refFd } })));
                dc->setProperty ("file", yourFiles[n]);
                dc->setProperty ("bpm", bpm);
                dc->setProperty ("firstDownbeat", yourFd);
                dc->setProperty ("limit", 4);
                auto dr = opCompareDrums (ctx, juce::var (dc));

                // Muster schwanken in compareDrums schon bei 5 ms Versatz
                // (Kalibrierung 2026-10-19): ins Urteil nur Klang und Pegel
                if (auto* dl = dr.getProperty ("deviations", {}).getArray())
                    for (auto& d : *dl)
                    {
                        devList.add (d);
                        const auto w = d["what"].toString();

                        if (! w.contains ("pattern") && ! w.contains ("timing") && w != "open hat")
                            issues.add (w);
                    }
            }
            else
            {
                auto spec = [&] (const juce::String& f, double fd)
                {
                    return juce::var (resultObj (true, { { "file", f }, { "bpm", bpm }, { "firstDownbeat", fd } }));
                };

                VoiceData r, y;

                if (auto e = analyse (ctx, spec (refFiles[n], refFd), r, true); e.isNotEmpty())
                    return resultObj (false, { { "error", e } });

                if (auto e = analyse (ctx, spec (yourFiles[n], yourFd), y, true); e.isNotEmpty())
                    return resultObj (false, { { "error", e } });

                // Rhythmus: gemeinsame Einsaetze, beste Verschiebung
                const auto ro = onsetTimes (r, refFd), yo = onsetTimes (y, yourFd);
                const double six = 15.0 / juce::jmax (1.0, bpm);
                const double m0 = onsetMatch (ro, yo, 0.0);
                int bestShift = 0;
                double best = m0;

                for (int sh = -2; sh <= 2; ++sh)
                    if (onsetMatch (ro, yo, sh * six) > best + 0.1)
                        best = onsetMatch (ro, yo, sh * six), bestShift = sh;

                auto oo = new juce::DynamicObject();
                oo->setProperty ("reference", (int) ro.size());
                oo->setProperty ("yours", (int) yo.size());
                oo->setProperty ("match", num (m0));
                oo->setProperty ("bestShift16", bestShift);
                oo->setProperty ("bestMatch", num (best));
                o->setProperty ("onsets", juce::var (oo));

                if (bestShift != 0)
                    issues.add ("onsets " + juce::String (std::abs (bestShift)) + " 16th(s) " + (bestShift > 0 ? "late" : "early") + " (match " + juce::String (m0, 2) + " -> " + juce::String (best, 2) + " shifted)");
                else if (m0 < 0.5 && ! ro.empty())
                    issues.add ("rhythm: only " + juce::String (juce::roundToInt (m0 * 100)) + "% of the onsets line up");

                auto devs = compare (r, y);
                std::sort (devs.begin(), devs.end(), [] (auto& a, auto& b) { return a.weight > b.weight; });

                for (size_t i = 0; i < devs.size() && i < 5; ++i)
                {
                    devList.add (juce::var (resultObj (true, { { "what", devs[i].what }, { "ref", devs[i].ref }, { "yours", devs[i].yours }, { "fix", devs[i].fix } })));

                    // Stimmen die Einsaetze im Rahmen der Messstreuung
                    // (Kalibrierung: gleiches Audio 5 ms verschoben 0,6-0,8),
                    // zaehlen Rhythmus-Meldungen von compareVoice nicht
                    const bool rhythmic = devs[i].what.contains ("onset") || devs[i].what.contains ("hits") || devs[i].what == "rhythm";

                    if (devs[i].weight >= 1.0 && ! (rhythmic && m0 >= 0.6))
                        issues.add (devs[i].what);
                }
            }

            o->setProperty ("deviations", devList);
            o->setProperty ("verdict", issues.isEmpty() ? juce::String ("matches") : "differs: " + issues.joinIntoString ("; "));

            if (! issues.isEmpty())
                worst.add (n + ": " + issues.joinIntoString ("; "));

            pairs.add (juce::var (o));
        }

        juce::StringArray h;
        h.add (worst.isEmpty() ? juce::String::fromUTF8 ("all layers match after the same separation")
                               : juce::String::fromUTF8 ("after the same separation these layers differ — work on the first one, then run compareSeparated again: ") + worst.joinIntoString (" | "));
        h.add (juce::String::fromUTF8 ("both sides went through the same separation, so bleed and smearing hit both alike: a layer that differs here sounds different in the mix; your firstDownbeat defaults to the reference's (your export starts where the part starts)"));
        return resultObj (true, { { "file", cmd.getProperty ("file", {}) }, { "split", split.isObject() }, { "pairs", pairs }, { "hints", hintVar (h) } });
    }
}

// Messung fuer matchSound/tuneSound (ops/SoundMatch.cpp, Header SoundMatch.h)
namespace dawd
{
    namespace
    {
        // Hochpass wie in analyse (minPitch): zwei Halbtoene darunter, 4. Ordnung
        void highPass (std::vector<std::vector<float>>& stereo, double hz)
        {
            for (auto& ch : stereo)
                for (int pass = 0; pass < 2; ++pass)
                {
                    juce::IIRFilter hp;
                    hp.setCoefficients (juce::IIRCoefficients::makeHighPass (AnalysisSource::stereoRate, hz));
                    hp.processSamples (ch.data(), (int) ch.size());
                }
        }
    }

    juce::String measureSoundReference (DawdContext& ctx, const juce::var& spec, MeasuredSound& out)
    {
        VoiceData d;

        if (auto e = analyse (ctx, spec, d, true, false, &out.clap); e.isNotEmpty())
            return e;

        if (d.highPassHz > 0.0)
            highPass (d.src.stereo, d.highPassHz);

        out.src = std::move (d.src);
        out.v = std::move (d.v);
        out.tb = d.tb;
        out.classes = d.classes;
        out.bpm = d.bpm;
        out.gridFrom = d.gridFrom;
        return {};
    }

    juce::String measureSoundCandidate (DawdContext& ctx, const MeasuredSound& ref, const juce::File& file, MeasuredSound& out)
    {
        auto spec = new juce::DynamicObject();
        spec->setProperty ("file", file.getFullPathName());
        out.src.want22k = transcription::available().isEmpty();
        out.src.wantMono = false;
        out.src.wantStereo = true;

        if (auto e = loadAnalysisSource (ctx, juce::var (spec), out.src); e.isNotEmpty())
            return e;

        // eigene Noten nur fuer Glide und Stimmung (ohne Raster)
        if (out.src.want22k)
        {
            transcription::Options o;
            juce::String err;
            transcription::Contour contour;
            auto notes = transcription::transcribe (out.src.audio22k, o, err, &contour);

            if (err.isNotEmpty())
                return err;

            out.v = voice::describeVoice (notes, contour, out.src.stereo, AnalysisSource::stereoRate, {});
        }

        // Klang an den Noten der Referenz — dieselben Stellen auf beiden Seiten
        out.tb = voice::describeTimbre (ref.v, out.src.stereo, AnalysisSource::stereoRate);
        std::vector<float> scores;
        out.classes = classify (out.src.stereo, AnalysisSource::stereoRate, scores, &out.clap);
        out.bpm = ref.bpm;
        return {};
    }
}
