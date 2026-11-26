#include "Ops.h"
#include "AnalysisSource.h"
#include "../DrumAnalysis.h"
#include "../MusicAnalysis.h"
#include "../Transcription.h"
#include "../VoiceAnalysis.h"
#include "../EngineHelpers.h"

// Musik-Analyse fuer Agenten (Familie processing, read-only): was ein Mensch
// heraushoert — Tempo, Takt, spaeter Tonart, Akkorde, Aufbau. Quelle ist eine
// Datei (Referenz, Original) oder der eigene Mix, damit Original und Nachbau
// mit demselben Werkzeug gemessen werden. Die Signalverarbeitung steht in
// MusicAnalysis.cpp.
namespace dawd
{
    namespace
    {
        double round3 (double v) { return std::round (v * 1000.0) / 1000.0; }
        double round2 (double v) { return std::round (v * 100.0) / 100.0; }
        double round1 (double v) { return std::round (v * 10.0) / 10.0; }

        // Schlaege aus dem neuen Drum-Detektor (DrumAnalysis): Mono 44,1 kHz
        // aus dem Stereo der Quelle, Raster aus bpm/Takt-Eins
        std::vector<drums::NoteHit> drumHitsOf (const AnalysisSource& src, double bpm, double firstDownbeat, int beatsPerBar, double minLevelDb = -100.0)
        {
            std::vector<float> mono;

            if (! src.stereo.empty())
            {
                mono.resize (src.stereo[0].size());

                for (size_t i = 0; i < mono.size(); ++i)
                    mono[i] = src.stereo.size() > 1 ? 0.5f * (src.stereo[0][i] + src.stereo[1][i]) : src.stereo[0][i];
            }

            return drums::noteHits (drums::describe (mono, AnalysisSource::stereoRate, bpm > 0 ? bpm : 120.0, firstDownbeat, beatsPerBar), minLevelDb);
        }

        juce::var timesFrom (const std::vector<double>& ts, double offset)
        {
            juce::Array<juce::var> out;

            for (auto t : ts)
                out.add (round3 (t + offset));

            return out;
        }
    }

    // Tempo, Taktschlaege, Takt und Takt-Eins. Zeiten in Sekunden der Quelle
    // (Datei bzw. Song). Kompakt per Default; detail: true liefert alle
    // Beats und Takt-Einsen (bei langen Songs Hunderte Zahlen).
    // Groove einer Aufnahme (z. B. des Drum-Stems der Referenz): Schlaege
    // (detectDrumHits) im Takt-Raster aus detectTempo, je Rasterplatz im Takt
    // die mittlere Abweichung vom geraden Raster (Anteil des Rasters, +
    // spaeter) und die Staerke. quantizeClip {groove} legt das auf Noten.
    juce::var opExtractGroove (DawdContext& ctx, const juce::var& cmd)
    {
        double grid = 0.25;

        if (auto err = parseGrid (cmd.getProperty ("grid", "1/16"), grid); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // kick | snare | hat (zu und offen) | hatClosed | hatOpen (die Namen
        // aus describeDrums) | "" = alle
        auto only = cmd.getProperty ("instrument", {}).toString();
        const std::map<juce::String, int> notes { { "kick", 36 }, { "snare", 38 }, { "hat", 42 }, { "hatClosed", 42 }, { "hatOpen", 46 } };

        if (only.isNotEmpty() && notes.count (only) == 0)
            return resultObj (false, { { "error", "instrument must be kick, snare, hat, hatClosed or hatOpen" } });

        AnalysisSource src;

        src.wantStereo = true;   // Drum-Detektor braucht 44,1 kHz

        if (auto err = loadAnalysisSource (ctx, cmd, src); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto features = analysis::computeFeatures (src.audio);
        analysis::TempoResult t;
        juce::String gridFrom;

        if (auto err = analysisGrid (ctx, cmd, src, features, t, gridFrom); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (! t.valid || t.beats.size() < 4 || t.downbeats.empty())
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no steady pulse found — need a few bars with clear beats (or pass bpm + firstDownbeat)") } });

        auto hits = drumHitsOf (src, t.bpm, t.downbeats.front(), t.beatsPerBar);

        // Sekunde -> Beat ueber die erkannten Schlaege (stueckweise linear,
        // folgt kleinen Tempo-Schwankungen); Beat 0 = erste Takt-Eins
        const auto& b = t.beats;
        auto beatAt = [&b] (double sec)
        {
            size_t i = 1;

            while (i + 1 < b.size() && b[i] < sec)
                ++i;

            return (double) (i - 1) + (sec - b[i - 1]) / (b[i] - b[i - 1]);
        };

        const double bar0 = beatAt (t.downbeats.front());
        const int slotsPerBar = juce::jmax (1, (int) std::round (t.beatsPerBar / grid));
        std::vector<std::vector<double>> offsets ((size_t) slotsPerBar), levels ((size_t) slotsPerBar);
        int used = 0;

        for (auto& h : hits)
        {
            if (only.isNotEmpty() && h.note != notes.at (only) && ! (only == "hat" && h.note == 46))
                continue;

            auto pos = (beatAt (h.time) - bar0) / grid;
            auto slot = (long) std::llround (pos);
            auto inBar = (int) (((slot % slotsPerBar) + slotsPerBar) % slotsPerBar);
            offsets[(size_t) inBar].push_back (pos - (double) slot);
            levels[(size_t) inBar].push_back (h.strength);
            ++used;
        }

        if (used < 4)
            return resultObj (false, { { "error", "too few hits for a groove (" + juce::String (used) + ")" } });

        // Bezug sind die Viertel: deren mittlere Lage ist Null. Das nimmt den
        // festen Versatz zwischen Beat-Raster und Onset-Erkennung heraus (sonst
        // laege der ganze Groove um ~15 ms zu spaet)
        {
            std::vector<double> onBeat;
            const int perBeat = juce::jmax (1, (int) std::round (1.0 / grid));

            for (int k = 0; k < slotsPerBar; k += perBeat)
                onBeat.insert (onBeat.end(), offsets[(size_t) k].begin(), offsets[(size_t) k].end());

            if (! onBeat.empty() && std::abs (grid * perBeat - 1.0) < 1.0e-6)
            {
                std::nth_element (onBeat.begin(), onBeat.begin() + (long) onBeat.size() / 2, onBeat.end());
                auto ref = onBeat[onBeat.size() / 2];

                for (auto& o : offsets)
                    for (auto& x : o)
                        x -= ref;
            }
        }

        const double gridMs = grid * 60000.0 / t.bpm;
        juce::Array<juce::var> slots;

        for (int k = 0; k < slotsPerBar; ++k)
        {
            auto& o = offsets[(size_t) k];
            auto obj = new juce::DynamicObject();
            obj->setProperty ("slot", k);
            obj->setProperty ("count", (int) o.size());

            if (! o.empty())
            {
                auto med = o;
                std::nth_element (med.begin(), med.begin() + (long) med.size() / 2, med.end());
                auto m = med[med.size() / 2];
                double lvl = 0.0;

                for (auto l : levels[(size_t) k])
                    lvl += l / (double) levels[(size_t) k].size();

                obj->setProperty ("offset", round3 (m));
                obj->setProperty ("offsetMs", round1 (m * gridMs));
                obj->setProperty ("level", round2 (lvl));
            }

            slots.add (juce::var (obj));
        }

        // Swing: wie weit die ungeraden Plaetze spaeter liegen als die geraden
        // (Mediane — einzelne Doppeltreffer ziehen ein Mittel weg)
        std::vector<double> oddOff, evenOff;

        for (int k = 0; k < slotsPerBar; ++k)
            for (auto o : offsets[(size_t) k])
                (k % 2 ? oddOff : evenOff).push_back (o);

        auto med = [] (std::vector<double> v) { std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end()); return v[v.size() / 2]; };
        const int nOdd = (int) oddOff.size(), nEven = (int) evenOff.size();
        const double odd = nOdd > 0 ? med (oddOff) * nOdd : 0.0, even = nEven > 0 ? med (evenOff) * nEven : 0.0;

        auto res = resultObj (true);
        auto* r = res.getDynamicObject();
        describeAnalysisSource (*r, src);
        r->setProperty ("bpm", round1 (t.bpm));
        r->setProperty ("beatsPerBar", t.beatsPerBar);
        r->setProperty ("firstDownbeat", round3 (t.downbeats.front() + src.start));
        r->setProperty ("gridFrom", gridFrom);
        r->setProperty ("grid", grid);
        r->setProperty ("hits", used);
        r->setProperty ("swing", nOdd > 0 && nEven > 0 ? juce::var (round2 (odd / nOdd - even / nEven)) : juce::var());
        r->setProperty ("slots", slots);
        return res;
    }

    juce::var opDetectTempo (DawdContext& ctx, const juce::var& cmd)
    {
        AnalysisSource src;

        if (auto err = loadAnalysisSource (ctx, cmd, src); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto features = analysis::computeFeatures (src.audio);
        auto t = analysis::detectTempo (features);
        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeAnalysisSource (*o, src);

        if (! t.valid)
        {
            o->setProperty ("bpm", juce::var());
            o->setProperty ("hints", juce::Array<juce::var> { features.duration < 4.0
                                                                  ? "too short for tempo detection (need at least 4 s)"
                                                                  : "no steady pulse found (no clear onsets)" });
            return res;
        }

        juce::Array<juce::var> candidates;

        for (auto& c : t.candidates)
            candidates.add (resultObj (true, { { "bpm", round1 (c.bpm) }, { "score", round2 (c.score) } }));

        for (auto& c : candidates)
            c.getDynamicObject()->removeProperty ("ok");

        o->setProperty ("bpm", round1 (t.bpm));
        o->setProperty ("confidence", round2 (t.confidence));
        o->setProperty ("candidates", candidates);
        o->setProperty ("timeSig", juce::String (t.beatsPerBar) + "/4");
        o->setProperty ("beatsPerBar", t.beatsPerBar);
        o->setProperty ("meterConfidence", round2 (t.meterConfidence));
        o->setProperty ("firstBeat", round3 (t.beats.front() + src.start));
        o->setProperty ("firstDownbeat", t.downbeats.empty() ? juce::var() : juce::var (round3 (t.downbeats.front() + src.start)));
        o->setProperty ("beatCount", (int) t.beats.size());
        o->setProperty ("bars", (int) t.downbeats.size());
        o->setProperty ("maxDriftMs", round1 (t.maxDriftMs));

        if ((bool) cmd.getProperty ("detail", false))
        {
            o->setProperty ("beats", timesFrom (t.beats, src.start));
            o->setProperty ("downbeats", timesFrom (t.downbeats, src.start));
        }

        juce::StringArray hints;

        if (t.confidence < 0.4)
            hints.add (juce::String::fromUTF8 ("weak pulse — treat the tempo as a guess, check the candidates"));

        // Halbes/doppeltes Tempo fast so gut: der klassische Oktavfehler
        for (size_t i = 1; i < t.candidates.size(); ++i)
        {
            auto ratio = t.candidates[i].bpm / t.bpm;

            if (t.candidates[i].score > 0.85 && (std::abs (ratio - 2.0) < 0.06 || std::abs (ratio - 0.5) < 0.015))
                hints.add ((ratio > 1 ? "double time " : "half time ") + juce::String (round1 (t.candidates[i].bpm))
                           + juce::String::fromUTF8 (" BPM fits almost as well — pick by feel (where the snare/backbeat sits)"));
        }

        if (t.fromBars)
            hints.add (juce::String::fromUTF8 ("the strongest rhythm is not the beat (syncopated parts, no drums) — the tempo comes from the bar repetition, 4 beats per bar"));

        if (t.straightened)
            hints.add (juce::String::fromUTF8 ("the beat tracking followed syncopated hits — the onsets sit on a straight 16th grid, so the grid is straight (played in time)"));

        if (t.snapped)
            hints.add (juce::String::fromUTF8 ("the beat tracking followed syncopated hits in places — almost all beats sit on one straight grid, so the grid is straight (played in time)"));

        if (t.maxDriftMs > 40.0)
            hints.add ("beats drift up to " + juce::String (round1 (t.maxDriftMs))
                       + juce::String::fromUTF8 (" ms from a straight grid — tempo changes or played freely"));

        if (t.meterConfidence < 0.3)
            hints.add (juce::String::fromUTF8 ("bar start (downbeat) uncertain — ") + juce::String (t.beatsPerBar) + "/4 is a guess");

        // Kein Rueckschlag als Gegenprobe (Entscheidung Matthias 2026-10-29,
        // todos/2026-10-27-detecttempo-intro-ohne-drums.md)
        if (t.phaseUncertain)
            hints.add (juce::String::fromUTF8 ("beat phase uncertain — no snare backbeat found and the offbeat eighths are almost as strong as the beats, so the grid may sit half a beat off (typical for an intro without drums); run detectTempo over a longer range that includes the drums and use its bpm/firstDownbeat for this part"));
        else if (! t.phaseShifted && ! t.phaseKept && t.offbeatVote > 0.1)
            hints.add (juce::String::fromUTF8 ("beat phase uncertain — the grid may sit half a beat off (syncopation); detectTempo on the drums stem (separateStems) is more reliable"));

        // Praxistest 2 (remix-1, House): die Hats auf "und" sprachen fuer das
        // Verschieben, Rueckschlag oder Kick auf jedem Schlag hielten dagegen
        if (t.phaseKept)
            hints.add (juce::String::fromUTF8 ("hats or bass sit between the beats (offbeat, as in house) — the grid stays on the kick and the snare/clap backbeat"));

        // Ohne Rueckschlag kam die Verschiebung nur aus Hats/Akkorden — bei
        // einer Kick auf jedem Schlag (House) waere sie falsch
        if (t.phaseShifted && t.backbeatVote >= 0.2)
            hints.add (juce::String::fromUTF8 ("the strongest hits sit between the beats (syncopated kick/bass) — beats follow the snare backbeat, hats and chord changes instead; check firstDownbeat by ear-proxy (detectKey chords on bar starts)"));
        else if (t.phaseShifted)
            hints.add (juce::String::fromUTF8 ("the grid sits half a beat away from the strongest hits — hats and chord changes put the beat there, no snare backbeat confirms it; if the kick plays on every beat (four-on-the-floor), the kick is the beat: use firstBeat/firstDownbeat half a beat earlier or later"));

        juce::Array<juce::var> hintList;

        for (auto& h : hints)
            hintList.add (h);

        o->setProperty ("hints", hintList);
        return res;
    }

    // Tonart und Akkorde: Tonart mit Alternativen und Tonleiter, Akkorde je
    // Beat geglaettet (mit Takt/Schlag, wenn ein Puls da ist), Zusammenfassung
    // nach Anteil. Zeiten in Sekunden der Quelle.
    juce::var opDetectKey (DawdContext& ctx, const juce::var& cmd)
    {
        AnalysisSource src;

        if (auto err = loadAnalysisSource (ctx, cmd, src); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto features = analysis::computeFeatures (src.audio);
        analysis::TempoResult tempo;
        juce::String gridFrom;

        if (auto err = analysisGrid (ctx, cmd, src, features, tempo, gridFrom); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto k = analysis::detectKey (features, tempo.valid ? tempo.beats : std::vector<double> {});
        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeAnalysisSource (*o, src);

        if (! k.valid)
        {
            o->setProperty ("key", juce::var());
            o->setProperty ("hints", juce::Array<juce::var> { "no tonal content found (silence or noise only)" });
            return res;
        }

        auto& best = k.keys.front();
        const bool flats = analysis::keyUsesFlats (best.tonic, best.minor);
        auto keyName = [] (const analysis::KeyCandidate& c)
        {
            return analysis::pitchName (c.tonic, analysis::keyUsesFlats (c.tonic, c.minor)) + (c.minor ? " minor" : " major");
        };

        o->setProperty ("key", keyName (best));
        o->setProperty ("tonic", analysis::pitchName (best.tonic, flats));
        o->setProperty ("mode", best.minor ? "minor" : "major");
        o->setProperty ("confidence", round2 (k.confidence));

        juce::Array<juce::var> alts;

        for (size_t i = 1; i < k.keys.size() && alts.size() < 3; ++i)
        {
            auto a = new juce::DynamicObject();
            a->setProperty ("key", keyName (k.keys[i]));
            a->setProperty ("score", round2 (k.keys[i].score));
            alts.add (juce::var (a));
        }

        o->setProperty ("alternatives", alts);

        juce::Array<juce::var> scale;

        for (auto iv : best.minor ? std::vector<int> { 0, 2, 3, 5, 7, 8, 10 } : std::vector<int> { 0, 2, 4, 5, 7, 9, 11 })
            scale.add (analysis::pitchName (best.tonic + iv, flats));

        o->setProperty ("scale", scale);

        // Akkorde, mit Takt/Schlag (1-basiert) ab der Takt-Eins, wenn ein Puls da ist
        const auto& types = analysis::chordTypes();
        const bool grid = tempo.valid && ! tempo.downbeats.empty();
        std::map<juce::String, double> share;
        juce::Array<juce::var> chords;
        double heard = 0.0;

        for (auto& c : k.chords)
        {
            auto name = c.root < 0 ? juce::String ("N") : analysis::pitchName (c.root, flats) + types[(size_t) c.type].suffix;
            auto co = new juce::DynamicObject();
            co->setProperty ("start", round3 (c.start + src.start));
            co->setProperty ("end", round3 (c.end + src.start));
            co->setProperty ("chord", name);

            if (grid)
            {
                auto [bar, beat] = barBeatAt (tempo, c.start);
                co->setProperty ("bar", bar);
                co->setProperty ("beat", beat);

                if (bar < 1)
                    co->setProperty ("pickup", true);   // vor der ersten Eins
            }

            co->setProperty ("confidence", round2 (c.confidence));
            chords.add (juce::var (co));

            if (c.root >= 0)
            {
                share[name] += c.end - c.start;
                heard += c.end - c.start;
            }
        }

        o->setProperty ("chords", chords);

        std::vector<std::pair<juce::String, double>> sorted (share.begin(), share.end());
        std::sort (sorted.begin(), sorted.end(), [] (auto& a, auto& b) { return a.second > b.second; });
        juce::Array<juce::var> summary;

        for (size_t i = 0; i < sorted.size() && i < 8; ++i)
        {
            auto so = new juce::DynamicObject();
            so->setProperty ("chord", sorted[i].first);
            so->setProperty ("share", round2 (heard > 0 ? sorted[i].second / heard : 0.0));
            summary.add (juce::var (so));
        }

        o->setProperty ("summary", summary);

        if (grid)
        {
            o->setProperty ("bpm", round1 (tempo.bpm));
            o->setProperty ("firstDownbeat", round3 (tempo.downbeats.front() + src.start));
            o->setProperty ("gridFrom", gridFrom);
        }

        juce::StringArray hints;

        if (k.confidence < 0.4)
            hints.add (juce::String::fromUTF8 ("key is ambiguous — check the alternatives and the chords"));

        if (k.keys.size() > 1 && analysis::isRelativeKey (best, k.keys[1]) && best.score - k.keys[1].score < 0.1)
            hints.add (keyName (k.keys[1]) + juce::String::fromUTF8 (" shares the same notes (relative key) — the chords tell which is home"));

        // Naechste Tonart ausser der Paralleltonart eine Quinte entfernt und
        // knapp dahinter: die Tonleitern unterscheiden sich in einem Ton
        for (size_t i = 1; i < k.keys.size(); ++i)
        {
            if (analysis::isRelativeKey (best, k.keys[i]))
                continue;

            auto steps = (k.keys[i].tonic - best.tonic + 12) % 12;

            if (k.keys[i].minor == best.minor && (steps == 5 || steps == 7) && best.score - k.keys[i].score < 0.1)
                hints.add (keyName (k.keys[i]) + juce::String::fromUTF8 (" is a fifth away and close — its scale differs by one note; check that note and where the melody comes to rest"));

            break;
        }

        if (! grid)
            hints.add (juce::String::fromUTF8 ("no steady pulse — chords are in 0.5 s steps, without bar/beat (pass bpm + firstDownbeat for a grid)"));
        else if (gridFrom == "detected")
            hints.add (juce::String::fromUTF8 ("bars come from a tempo guessed on this source — on a stem pass bpm + firstDownbeat from detectTempo of the full mix"));

        juce::Array<juce::var> hintList;

        for (auto& h : hints)
            hintList.add (h);

        o->setProperty ("hints", hintList);
        return res;
    }

    // Aufbau: Abschnitte mit Buchstabe (gleiche Teile gleich), Rollen-
    // Vermutung, Taktzahl, Energie 1..5, Akkordfolge; dazu Tempo und Tonart
    // und fertige addSection-Befehle fuer batch (Songgeruest wie die Quelle).
    // detail: true liefert die Merkmale je Takt.
    juce::var opAnalyzeStructure (DawdContext& ctx, const juce::var& cmd)
    {
        AnalysisSource src;

        if (auto err = loadAnalysisSource (ctx, cmd, src); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto features = analysis::computeFeatures (src.audio);
        analysis::TempoResult tempo;
        juce::String gridFrom = "detected";

        // Raster vorgeben (Praxistest 2: auf einem Vocal-Stem 139 BPM, 3/4,
        // 149 Takte) — bpm + firstDownbeat wie bei den anderen Analyse-Ops;
        // ohne sie wie bisher geschaetzt, auch auf dem eigenen Mix
        if (cmd.hasProperty ("bpm") || cmd.hasProperty ("firstDownbeat"))
        {
            if (auto err = analysisGrid (ctx, cmd, src, features, tempo, gridFrom); err.isNotEmpty())
                return resultObj (false, { { "error", err } });
        }
        else
            tempo = analysis::detectTempo (features);

        auto key = analysis::detectKey (features, tempo.valid ? tempo.beats : std::vector<double> {});
        auto st = analysis::analyzeStructure (src.audio, features, tempo, key);
        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeAnalysisSource (*o, src);

        if (! st.valid)
        {
            o->setProperty ("sections", juce::Array<juce::var>());
            o->setProperty ("hints", juce::Array<juce::var> { "too short or silent for a structure (need a few bars)" });
            return res;
        }

        const bool flats = key.valid && analysis::keyUsesFlats (key.keys.front().tonic, key.keys.front().minor);
        const auto& types = analysis::chordTypes();
        auto chordName = [&] (int idx)
        {
            if (idx < 0 || idx >= (int) key.chords.size() || key.chords[(size_t) idx].root < 0)
                return juce::String ("N");

            auto& c = key.chords[(size_t) idx];
            return analysis::pitchName (c.root, flats) + types[(size_t) c.type].suffix;
        };

        const juce::String timeSig = juce::String (tempo.beatsPerBar) + "/4";

        if (st.grid)
        {
            o->setProperty ("bpm", round1 (tempo.bpm));
            o->setProperty ("timeSig", timeSig);
            // Beginn von Takt 1 (nach einem Auftakt) — passt zu startBar
            auto bar1 = st.bars.size() > 1 && st.bars.front().pickup ? st.bars[1].start : st.bars.front().start;
            o->setProperty ("firstDownbeat", round3 (bar1 + src.start));
            o->setProperty ("gridFrom", gridFrom);
        }

        if (key.valid)
            o->setProperty ("key", analysis::pitchName (key.keys.front().tonic, flats) + (key.keys.front().minor ? " minor" : " major"));

        const bool pickup = st.bars.front().pickup;
        o->setProperty ("bars", (int) st.bars.size() - (pickup ? 1 : 0));

        juce::Array<juce::var> sections, commands;
        std::map<juce::String, int> roleCount;

        for (auto& sg : st.segments)
        {
            auto so = new juce::DynamicObject();
            auto label = juce::String::charToString ((juce::juce_wchar) ('A' + juce::jmin (25, sg.label)));
            so->setProperty ("label", label);
            so->setProperty ("role", sg.role);
            so->setProperty ("start", round3 (sg.start + src.start));
            so->setProperty ("end", round3 (sg.end + src.start));

            if (st.grid)
            {
                // Takt 1 = erste Takt-Eins; ein Auftakt ist Takt 0
                so->setProperty ("startBar", sg.firstBar + (pickup ? 0 : 1));
                so->setProperty ("bars", st.bars[(size_t) sg.firstBar].pickup ? 0 : sg.numBars);
            }

            so->setProperty ("energy", sg.energy);
            so->setProperty ("rmsDb", round1 (sg.rmsDb));

            // Akkordfolge: ein Akkord je Takt, Wiederholungen zusammengefasst
            juce::StringArray prog;

            for (int b = sg.firstBar; b < sg.firstBar + sg.numBars; ++b)
            {
                auto name = chordName (st.bars[(size_t) b].chord);

                if (prog.isEmpty() || prog[prog.size() - 1] != name)
                    prog.add (name);
            }

            so->setProperty ("chords", prog.joinIntoString (" "));
            sections.add (juce::var (so));

            auto n = ++roleCount[sg.role];
            auto cmdObj = new juce::DynamicObject();
            cmdObj->setProperty ("op", "addSection");
            cmdObj->setProperty ("name", sg.role + " " + juce::String (n));

            if (st.grid && ! st.bars[(size_t) sg.firstBar].pickup)
            {
                cmdObj->setProperty ("bars", sg.numBars);
                cmdObj->setProperty ("bpm", round1 (tempo.bpm));
                cmdObj->setProperty ("timeSig", timeSig);
            }
            else
                cmdObj->setProperty ("length", round3 (sg.end - sg.start));

            commands.add (juce::var (cmdObj));
        }

        o->setProperty ("sections", sections);
        o->setProperty ("sectionCommands", commands);

        if ((bool) cmd.getProperty ("detail", false))
        {
            juce::Array<juce::var> bars;

            for (size_t b = 0; b < st.bars.size(); ++b)
            {
                auto& bi = st.bars[b];
                auto bo = new juce::DynamicObject();
                bo->setProperty ("bar", (int) b + (pickup ? 0 : 1));
                bo->setProperty ("start", round3 (bi.start + src.start));
                bo->setProperty ("rmsDb", round1 (bi.rmsDb));
                bo->setProperty ("low", round1 (bi.bandsDb[0]));
                bo->setProperty ("mid", round1 (bi.bandsDb[1]));
                bo->setProperty ("high", round1 (bi.bandsDb[2]));
                bo->setProperty ("density", round2 (bi.density));
                bo->setProperty ("chord", chordName (bi.chord));
                bars.add (juce::var (bo));
            }

            o->setProperty ("barDetail", bars);
        }

        juce::StringArray hints;

        if (! st.grid)
            hints.add (juce::String::fromUTF8 ("no steady pulse — sections are in 2 s blocks, without bars"));

        hints.add (juce::String::fromUTF8 ("roles are a guess from repetition and energy — listen with analyze/compareReference per section"));

        // Zweifel am Raster wie bei detectTempo nennen (Praxistest 1e: der
        // Taktbeginn lag eine 16tel neben dem des Drum-Stems, ohne Hinweis)
        if (st.grid)
        {
            juce::StringArray doubt;

            if (tempo.phaseUncertain)
                doubt.add (juce::String::fromUTF8 ("no snare backbeat — the beats may sit half a beat off; take the grid from a part with drums"));
            else if (tempo.phaseShifted || (tempo.offbeatVote > 0.1 && ! tempo.phaseKept))
                doubt.add ("the beats may sit half a beat off");

            if (tempo.meterConfidence < 0.3)
                doubt.add ("the downbeat is a guess");

            if (tempo.maxDriftMs > 40.0)
                doubt.add ("the beats drift " + juce::String (round1 (tempo.maxDriftMs)) + " ms");

            if (! doubt.isEmpty())
                hints.add ("bar start uncertain (" + doubt.joinIntoString (", ")
                           + juce::String::fromUTF8 (") — check firstDownbeat with detectTempo/describeDrums on the drums stem (a snare beside 2/4 names the corrected one) before building bars on it"));

            // Tempo selbst unsicher (Praxistest 2: Vocal-Stem 139 BPM, 3/4,
            // 149 Takte — nur "bar start uncertain"): auf Stems und Teilen
            // ohne Drums das Raster des ganzen Mixes vorgeben
            if (gridFrom == "detected" && (tempo.confidence < 0.4 || tempo.fromBars || tempo.maxDriftMs > 40.0))
                hints.add (juce::String::fromUTF8 ("tempo, meter and bar count are guessed on this source (")
                           + (tempo.confidence < 0.4 ? juce::String ("weak pulse") : tempo.fromBars ? juce::String ("no clear beat, tempo from the bar repetition") : juce::String ("drifting beats"))
                           + juce::String::fromUTF8 (") — on a stem or a part without drums pass bpm + firstDownbeat from detectTempo of the full mix"));

            // Takte vor der ersten erkannten Takt-Eins sind zurueckgerechnet
            // (Praxistest 2: Takt 1 bei 0,18 s, detectTempo nannte 26,7 s)
            const double bar1 = st.bars.size() > 1 && pickup ? st.bars[1].start : st.bars.front().start;

            if (gridFrom == "detected" && ! tempo.downbeats.empty() && tempo.downbeats.front() - bar1 > 0.5 * st.barLength)
                hints.add ("bars before " + juce::String (round3 (tempo.downbeats.front() + src.start))
                           + juce::String::fromUTF8 (" s (the first downbeat found, detectTempo firstDownbeat) are counted back on the same grid — no beats were tracked there"));
        }

        if (pickup)
            hints.add ("pickup before bar 1 (" + juce::String (round3 ((st.bars.size() > 1 ? st.bars[1].start : st.firstDownbeat) + src.start))
                       + juce::String::fromUTF8 (" s) — its command uses length in seconds"));

        // Kein Teil kehrt wieder (Praxistest 3: vorher hiess dann alles
        // "chorus"): die Rollen sagen nur die Lage
        {
            std::map<int, int> labels;

            for (auto& sg : st.segments)
                if (! st.bars[(size_t) sg.firstBar].pickup)
                    ++labels[sg.label];

            if (labels.size() >= 3 && std::none_of (labels.begin(), labels.end(), [] (auto& l) { return l.second > 1; }))
                hints.add (juce::String::fromUTF8 ("no part comes back with the same chords — roles only say where a part sits; findRepeats {start, end} of one part looks for its repeats"));
        }

        juce::Array<juce::var> hintList;

        for (auto& h : hints)
            hintList.add (h);

        o->setProperty ("hints", hintList);
        return res;
    }

    namespace
    {
        // Was audioToMidi/addMidiFromAudio ausser den Noten wissen
        struct NoteExtras
        {
            bool legato = false, bends = false;
            int merged = 0, joined = 0;          // legato: verschmolzene Fragmente, verlaengerte Noten
            transcription::Contour contour;      // bends
            std::vector<int> velocities;         // je Note (Pegel beim Einsatz, drums: Staerke)
        };

        // Noten erkennen fuer audioToMidi/addMidiFromAudio: Quelle laden,
        // basic-pitch, Modus (polyphonic, melody = oberste Stimme, bass =
        // unterste), Bereich. Mit legato: Fragmente gehaltener Toene
        // verschmolzen und Noten bis an den naechsten Einsatz gebunden;
        // bends: die Contour dazu. Noten behalten ihre Oktave immer
        // (keepOctaves gilt fuer shape chords). Fehlertext in err.
        std::vector<transcription::Note> notesFromSource (DawdContext& ctx, const juce::var& cmd, AnalysisSource& src, juce::String& err,
                                                          NoteExtras& extras)
        {
            transcription::Options o;
            auto mode = cmd.getProperty ("mode", "polyphonic").toString();

            if (mode != "polyphonic" && mode != "melody" && mode != "bass" && mode != "drums")
            {
                err = "mode must be polyphonic, melody, bass or drums";
                return {};
            }

            extras.legato = (bool) cmd.getProperty ("legato", false);
            extras.bends = (bool) cmd.getProperty ("bends", false);
            src.ml = true;   // Audio -> MIDI: Gruppe A der Lizenz-Sperre, auch mode drums

            if (mode == "drums" && (extras.legato || extras.bends))
            {
                err = "legato and bends need pitched notes (not mode drums)";
                return {};
            }

            // Schlagzeug: Einsaetze je Band (eigene Signalverarbeitung, kein
            // Modell) als GM-Noten 36 Kick, 38 Snare, 42 Hi-Hat — passt auf
            // das Drum Kit; sensitivity > 1 findet leisere Schlaege
            if (mode == "drums")
            {
                src.wantStereo = true;

                if ((err = loadAnalysisSource (ctx, cmd, src)).isNotEmpty())
                    return {};

                auto sens = (double) cmd.getProperty ("sensitivity", 1.0);

                if (sens <= 0.0 || sens > 4.0)
                {
                    err = "sensitivity must be 0..4 (default 1)";
                    return {};
                }

                std::vector<transcription::Note> drums;

                // Neuer Detektor (Praxistest 1d: der alte fand 0 Snares, Kicks
                // zur Haelfte Bass-Uebersprechen); Raster nur fuer die
                // Zusammenfassung je Rasterplatz — aus dem Tempo der Quelle
                juce::ignoreUnused (sens);
                auto t = analysis::detectTempo (analysis::computeFeatures (src.audio));

                for (auto& h : drumHitsOf (src, t.valid ? t.bpm : 120.0, t.valid && ! t.downbeats.empty() ? t.downbeats.front() : 0.0, t.valid ? t.beatsPerBar : 4, -12.0))
                    drums.push_back ({ h.time, h.time + 0.1, h.note, 0.3 + 0.7 * h.strength });

                for (auto& n : drums)
                    extras.velocities.push_back (juce::jlimit (1, 127, (int) std::round (127.0 * n.amplitude)));

                return drums;
            }

            if ((err = transcription::available()).isNotEmpty())
                return {};

            o.minPitch = (int) cmd.getProperty ("minPitch", mode == "melody" ? 48 : 21);
            o.maxPitch = (int) cmd.getProperty ("maxPitch", mode == "bass" ? 60 : 108);
            o.onsetThreshold = (double) cmd.getProperty ("onsetThreshold", 0.5);
            o.frameThreshold = (double) cmd.getProperty ("frameThreshold", 0.3);
            o.minNoteMs = (double) cmd.getProperty ("minNoteMs", 127.7);

            if (o.minPitch < 0 || o.maxPitch > 127 || o.minPitch > o.maxPitch)
            {
                err = "need 0 <= minPitch <= maxPitch <= 127";
                return {};
            }

            src.want22k = true;
            src.wantMono = false;

            if ((err = loadAnalysisSource (ctx, cmd, src)).isNotEmpty())
                return {};

            // ohne Scheinnoten in digitaler Stille (Praxistest 1i, wie describeVoice)
            auto notes = voice::soundingNotes (transcription::transcribe (src.audio22k, o, err, extras.bends ? &extras.contour : nullptr),
                                               src.audio22k, 22050.0);

            // Nachklang beim naechsten Einsatz neu angeschlagen (Zupfton:
            // geteilte Toene, in melody gewann der hoehere Nachklang)
            voice::joinRingSplits (notes, src.audio22k, 22050.0);

            // Gehaltene Toene zerfallen in Fragmente (Praxistest 1c: Vibrato
            // und Schwellungen lasen sich als Achtel-Stabs) — vor dem
            // Oberton-Filter verschmelzen, wie describeVoice
            if (extras.legato)
                notes = voice::mergedFragments (notes, src.audio22k, 22050.0, extras.merged);

            // Obertoene als Geisternoten: auf dem 2. bis 17. Teilton (Oktave,
            // Duodezime, zwei Oktaven, grosse Terz, Quinte, kleine Septime,
            // drei Oktaven darueber, dann auf Halbtoene gerundet; tief
            // gezupft kommt der 9. und 17. als Note) einer lauteren Note, die sie
            // zeitlich zu mehr als der Haelfte ueberdeckt, und deutlich
            // leiser — weg (abschaltbar). Der Zupfton hat einen starken
            // 7. Teilton (A4 -> G7). Setzt sie zugleich ein (30 ms), ist
            // leiser und endet nach weniger als 60 % der Laenge, reicht
            // das: hohe Teiltoene klingen schneller ab (Zupfton E5 -> E6)
            if (! (bool) cmd.getProperty ("keepHarmonics", false))
            {
                std::vector<transcription::Note> kept;

                for (auto& n : notes)
                {
                    bool ghost = false;

                    for (auto& m : notes)
                    {
                        auto iv = n.pitch - m.pitch;

                        const bool partial = iv == 12 || iv == 19 || iv == 24 || iv == 28 || iv == 31 || iv == 34 || iv == 36
                                             || iv == 38 || iv == 40 || iv == 42 || iv == 43 || iv == 44 || iv == 46 || iv == 47 || iv == 48 || iv == 49;
                        const bool dying = std::abs (n.start - m.start) <= 0.03 && n.amplitude < m.amplitude
                                           && n.end - n.start < 0.6 * (m.end - m.start);

                        if (partial && (n.amplitude < 0.75 * m.amplitude || dying)
                            && juce::jmin (n.end, m.end) - juce::jmax (n.start, m.start) > 0.5 * (n.end - n.start))
                            ghost = true;
                    }

                    if (! ghost)
                        kept.push_back (n);
                }

                notes = kept;
            }

            // Eine Stimme: nach Einsatz sortiert; Einsaetze innerhalb von 30 ms
            // gelten als gleichzeitig — davon bleibt die hoechste (melody) bzw.
            // tiefste (bass); jede Note endet, wo die naechste beginnt. So
            // bleiben Oktavspruenge erhalten und ein ausklingender Ton loescht
            // keinen neuen (Praxistest 1: "Aussetzer" im Bass)
            if (mode != "polyphonic" && ! notes.empty())
            {
                const bool top = mode == "melody";
                std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });
                std::vector<transcription::Note> line;

                for (size_t i = 0; i < notes.size();)
                {
                    auto best = notes[i];
                    size_t j = i + 1;

                    for (; j < notes.size() && notes[j].start - notes[i].start < 0.03; ++j)
                        if (top ? notes[j].pitch > best.pitch : notes[j].pitch < best.pitch)
                            best = notes[j];

                    if (! line.empty() && line.back().end > best.start)
                        line.back().end = best.start;

                    // Ein vom Nachfolger auf unter 60 ms gekuerzter Ton war ein
                    // Schnipsel (Anschlag des naechsten) — er weicht
                    if (line.empty() || line.back().end - line.back().start > 0.06)
                        line.push_back (best);
                    else
                        line.back() = best;

                    i = j;
                }

                notes = line;
            }

            // Gebunden: bis an den naechsten Einsatz, 20 ms darueber
            if (extras.legato)
                extras.joined = voice::joinLegato (notes, src.audio22k, 22050.0, 0.02);

            // Velocity aus dem Pegel beim Einsatz statt der Aktivierung des Modells
            extras.velocities = voice::onsetVelocities (notes, src.audio22k, 22050.0);
            return notes;
        }

        // bends: je Note der Tonhoehenverlauf (Cent gegen ihren Halbton); eine
        // Note mit Glide beginnt am Anfang des Glides
        std::vector<std::vector<std::pair<double, double>>> bendsOf (std::vector<transcription::Note>& notes, const NoteExtras& extras)
        {
            std::vector<std::vector<std::pair<double, double>>> out (notes.size());

            if (! extras.bends)
                return out;

            std::vector<double> onset;

            for (auto& n : notes)
                onset.push_back (n.start);

            for (size_t i = 0; i < notes.size(); ++i)
            {
                double from = notes[i].start;
                out[i] = voice::pitchBends (notes[i], extras.contour, from);

                if (! out[i].empty())
                    notes[i].start = juce::jmax (0.0, juce::jmin (notes[i].start, from));
            }

            // Die Contour einer Note sieht Nachbarn bis zwei Halbtoene weit:
            // ihr Verlauf endet, wo ein solcher Nachbar danach einsetzt (sonst
            // lagen Ende und naechster Glide doppelt im Pitch-Bend)
            for (size_t i = 0; i < notes.size(); ++i)
            {
                if (out[i].empty())
                    continue;

                double cut = notes[i].end;

                for (size_t j = 0; j < notes.size(); ++j)
                    if (j != i && notes[j].start > notes[i].start + 0.05 && std::abs (notes[j].pitch - notes[i].pitch) <= 2)
                        cut = juce::jmin (cut, notes[j].start);

                auto& c = out[i];
                c.erase (std::remove_if (c.begin(), c.end(), [cut] (auto& p) { return p.first >= cut; }), c.end());
                double lo = 1.0e9, hi = -1.0e9;

                for (auto& [t, v] : c)
                    lo = juce::jmin (lo, v), hi = juce::jmax (hi, v);

                if (c.empty() || hi - lo < 15.0)
                    c.clear(), notes[i].start = onset[i];
            }

            return out;
        }

        // Ein Pitch-Bend gilt fuer den ganzen Kanal: nur eine Linie (keine
        // zwei Noten ueberlappen mehr als 150 ms — Glide und Legato duerfen)
        bool singleLine (const std::vector<transcription::Note>& notes)
        {
            for (size_t i = 0; i < notes.size(); ++i)
                for (size_t j = i + 1; j < notes.size(); ++j)
                    if (juce::jmin (notes[i].end, notes[j].end) - juce::jmax (notes[i].start, notes[j].start) > 0.15)
                        return false;

            return true;
        }

        void addLegatoInfo (juce::DynamicObject& o, const NoteExtras& extras)
        {
            if (extras.legato)
            {
                o.setProperty ("fragmentsMerged", extras.merged);
                o.setProperty ("legatoJoined", extras.joined);
            }
        }
    }

    // Audio -> MIDI (read-only): Noten aus einer Datei oder dem Mix, Zeiten in
    // Sekunden der Quelle; mit Puls (detectTempo) auch in Beats ab der
    // ersten Takt-Eins. Bis zu 2000 Noten, dazu eine Zusammenfassung.
    juce::var opAudioToMidi (DawdContext& ctx, const juce::var& cmd)
    {
        const auto shape = cmd.getProperty ("shape", "notes").toString();

        if (shape != "notes" && shape != "chords")
            return resultObj (false, { { "error", "shape must be notes or chords" } });

        if (shape == "chords" && ((bool) cmd.getProperty ("legato", false) || (bool) cmd.getProperty ("bends", false)))
            return resultObj (false, { { "error", "legato and bends apply to shape notes (chords already hold to the next hit)" } });

        if (shape == "chords")
        {
            std::vector<PlayedNote> played;
            juce::var info;

            if (auto e = playedChords (ctx, cmd, played, info); e.isNotEmpty())
                return resultObj (false, { { "error", e } });

            const double s0 = (double) info.getProperty ("sourceStart", 0.0);
            juce::Array<juce::var> list;

            for (auto& n : played)
            {
                auto no = new juce::DynamicObject();
                no->setProperty ("pitch", n.pitch);
                no->setProperty ("start", round3 (n.start + s0));
                no->setProperty ("length", round3 (n.length));
                no->setProperty ("velocity", n.velocity);
                list.add (juce::var (no));
            }

            return resultObj (true, { { "shape", "chords" }, { "count", (int) played.size() }, { "notes", list }, { "chords", info } });
        }

        AnalysisSource src;
        juce::String err;
        NoteExtras extras;
        auto notes = notesFromSource (ctx, cmd, src, err, extras);

        if (err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        const auto bends = bendsOf (notes, extras);
        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeAnalysisSource (*o, src);
        juce::Array<juce::var> list;
        int lo = 127, hi = 0, bent = 0;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            lo = juce::jmin (lo, n.pitch);
            hi = juce::jmax (hi, n.pitch);
            bent += bends[i].empty() ? 0 : 1;

            if (list.size() >= 2000)
                continue;

            auto no = new juce::DynamicObject();
            no->setProperty ("pitch", n.pitch);
            no->setProperty ("start", round3 (n.start + src.start));
            no->setProperty ("length", round3 (n.end - n.start));
            no->setProperty ("velocity", extras.velocities[i]);

            // [Sekunde, Cent gegen den Halbton] — Zeit wie start
            if (! bends[i].empty())
            {
                juce::Array<juce::var> pts;

                for (auto& [t, c] : bends[i])
                    pts.add (juce::Array<juce::var> { round3 (t + src.start), juce::roundToInt (c) });

                no->setProperty ("bend", pts);
            }

            list.add (juce::var (no));
        }

        o->setProperty ("count", (int) notes.size());
        addLegatoInfo (*o, extras);

        if (extras.bends)
            o->setProperty ("bentNotes", bent);

        if (! notes.empty())
        {
            o->setProperty ("lowest", lo);
            o->setProperty ("highest", hi);
        }

        o->setProperty ("notes", list);

        if (notes.size() > 2000)
            o->setProperty ("hints", juce::Array<juce::var> { juce::String::fromUTF8 ("more than 2000 notes — only the first 2000 are listed; use start/end or addMidiFromAudio") });

        return res;
    }

    // Audio -> MIDI direkt in den Song: erkennt die Noten (Quelle wie
    // audioToMidi) und legt sie als MIDI-Clip in eine Section (insertSection,
    // insertTrack wie track bei addMidiClip, Clip-Start insertStart/insertStartBeat in
    // der Section) — Noten relativ zum Beginn der Quelle. collect-Aktion.
    juce::var opAddMidiFromAudio (DawdContext& ctx, const juce::var& cmd)
    {
        auto sectionName = cmd.getProperty ("insertSection", {}).toString();
        auto* section = findSection (*ctx.edit, sectionName);

        if (section == nullptr)
            return resultObj (false, { { "error", sectionName.isEmpty() ? juce::String ("need insertSection (the section to put the clip in)")
                                                                         : "no such section: " + sectionName } });

        const auto shape = cmd.getProperty ("shape", "notes").toString();

        if (shape != "notes" && shape != "chords")
            return resultObj (false, { { "error", "shape must be notes or chords" } });

        // Ueber addMidiClip, damit Spur-/Lane-Regeln, Trimmen und die Aktion
        // genau gleich laufen
        juce::Array<juce::var> list;
        juce::var chordInfo;
        int count = 0;
        NoteExtras extras;
        std::vector<std::vector<std::pair<double, double>>> bends;
        std::vector<double> starts;
        bool line = false;
        const double bendRange = (double) cmd.getProperty ("bendRange", 2.0);

        if (bendRange < 1.0 || bendRange > 48.0)
            return resultObj (false, { { "error", "bendRange must be 1..48 semitones (the synth's pitch-bend range, default 2)" } });

        if (shape == "chords")
        {
            std::vector<PlayedNote> played;

            if (auto e = playedChords (ctx, cmd, played, chordInfo); e.isNotEmpty())
                return resultObj (false, { { "error", e } });

            for (auto& n : played)
            {
                auto no = new juce::DynamicObject();
                no->setProperty ("pitch", n.pitch);
                no->setProperty ("start", n.start);
                no->setProperty ("length", n.length);
                no->setProperty ("velocity", n.velocity);
                list.add (juce::var (no));
            }

            count = (int) played.size();
        }
        else
        {
            AnalysisSource src;
            juce::String err;
            auto notes = notesFromSource (ctx, cmd, src, err, extras);

            if (err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            if (notes.empty())
                return resultObj (false, { { "error", "no notes found in the source" } });

            bends = bendsOf (notes, extras);

            for (size_t i = 0; i < notes.size(); ++i)
            {
                const auto& n = notes[i];
                auto no = new juce::DynamicObject();
                no->setProperty ("pitch", n.pitch);
                no->setProperty ("start", juce::jmax (0.0, n.start));
                no->setProperty ("length", juce::jmax (0.01, n.end - n.start));
                no->setProperty ("velocity", extras.velocities[i]);
                list.add (juce::var (no));
            }

            count = (int) notes.size();
            line = singleLine (notes);
            starts.clear();

            for (auto& n : notes)
                starts.push_back (juce::jmax (0.0, n.start));
        }

        auto add = new juce::DynamicObject();
        add->setProperty ("track", cmd.getProperty ("insertTrack", -1));
        add->setProperty ("section", sectionName);

        if (cmd.hasProperty ("insertStart"))
            add->setProperty ("start", cmd.getProperty ("insertStart", 0.0));

        // Clip-Lage in Beats, Noten in Sekunden: addMidiClip lehnt das Mischen
        // ab (Praxistest 1d) — hier ist es gewollt, die Lage wird umgerechnet
        if (cmd.hasProperty ("insertStartBeat"))
        {
            auto* sec = findSection (*ctx.edit, sectionName);

            if (sec == nullptr)
                return resultObj (false, { { "error", "no such section: " + sectionName } });

            const double s0 = sec->getPosition().getStart().inSeconds();
            add->setProperty ("start", beatsToSeconds (*ctx.edit, s0, (double) cmd.getProperty ("insertStartBeat", 0.0)));
        }

        add->setProperty ("notes", list);

        if (cmd.hasProperty ("name"))
            add->setProperty ("name", cmd.getProperty ("name", {}));

        if (cmd.hasProperty ("newLane"))
            add->setProperty ("newLane", cmd.getProperty ("newLane", {}));

        auto r = opAddMidiClip (ctx, juce::var (add));

        if ((bool) r.getProperty ("ok", false))
        {
            auto* ro = r.getDynamicObject();
            ro->setProperty ("transcribed", count);
            addLegatoInfo (*ro, extras);

            if (shape == "chords")
                ro->setProperty ("chords", chordInfo);

            // Pitch-Bend in den Clip (eine Linie, ein Kanal): Bend-Punkte der
            // Noten, eine Note ohne Bend setzt auf die Mitte zurueck
            if (extras.bends)
            {
                const double clipStart = (double) r.getProperty ("start", 0.0);
                juce::Array<juce::var> pts;
                int last = 0, bent = 0;
                const double clipLength = (double) r.getProperty ("length", 0.0);
                auto add = [&] (double t, int v)
                {
                    if (t >= clipLength - 0.001)
                        return;

                    auto p = new juce::DynamicObject();
                    p->setProperty ("beat", secondsToBeats (*ctx.edit, clipStart, juce::jmax (0.0, t)));
                    p->setProperty ("value", v);
                    pts.add (juce::var (p));
                    last = v;
                };

                for (size_t i = 0; line && i < bends.size(); ++i)
                {
                    if (bends[i].empty())
                    {
                        if (last != 0)
                            add (starts[i], 0);

                        continue;
                    }

                    ++bent;

                    for (auto& [t, c] : bends[i])
                        add (t, juce::jlimit (-8192, 8191, juce::roundToInt (c / (100.0 * bendRange) * 8192.0)));
                }

                ro->setProperty ("bentNotes", bent);

                if (! line)
                    ro->setProperty ("hints", juce::Array<juce::var> { juce::String::fromUTF8 ("pitch bends not written: notes overlap (a bend moves every note on the channel) — use mode melody or bass for one line") });
                else if (! pts.isEmpty())
                {
                    auto sc = new juce::DynamicObject();
                    sc->setProperty ("track", r.getProperty ("track", -1));
                    sc->setProperty ("name", r.getProperty ("name", {}));
                    sc->setProperty ("controller", "pitchBend");
                    sc->setProperty ("points", pts);
                    auto br = opSetControllers (ctx, juce::var (sc));

                    if (! (bool) br.getProperty ("ok", false))
                        return resultObj (false, { { "error", "clip added, pitch bends failed: " + br.getProperty ("error", {}).toString() } });

                    ro->setProperty ("pitchBendPoints", pts.size());
                    ro->setProperty ("bendRange", bendRange);
                }
            }
        }

        return r;
    }
}


namespace dawd
{
    namespace
    {
        // stems von findRepeats: der Ordner als Text (<name>.wav darin) oder
        // die Dateien einzeln — {drums: file, …} oder die stems von jobStatus
        // [{stem, file}]. Praxistest 1l: ein Agent gab das Objekt, die
        // Antwort nannte "Object 0x…" als Ordner.
        juce::String stemFiles (const juce::var& v, std::map<juce::String, juce::File>& out)
        {
            const char* names[] = { "drums", "bass", "other", "vocals" };

            if (v.isString())
            {
                for (auto* name : names)
                    if (auto file = juce::File (v.toString()).getChildFile (juce::String (name) + ".wav"); file.existsAsFile())
                        out[name] = file;

                return {};
            }

            std::map<juce::String, juce::String> given;

            if (auto* o = v.getDynamicObject())
                for (auto& p : o->getProperties())
                    given[p.name.toString()] = p.value.toString();
            else if (auto* a = v.getArray())
                for (auto& e : *a)
                    given[e.getProperty ("stem", {}).toString()] = e.getProperty ("file", {}).toString();

            for (auto* name : names)
                if (given.count (name))
                {
                    if (! juce::File::isAbsolutePath (given[name]) || ! juce::File (given[name]).existsAsFile())
                        return "no such stem file: " + given[name] + " (" + name + ")";

                    out[name] = juce::File (given[name]);
                }

            if (out.empty())
                return juce::String::fromUTF8 ("stems: a folder with drums/bass/other/vocals.wav, {drums: file, bass: file, …} or the stems of jobStatus [{stem, file}]");

            return {};
        }
    }

    // findRepeats {file, start, end, bpm?, firstDownbeat?, beatsPerBar?,
    // limit?, stems?}: wo im ganzen Song der Teil start..end wiederkommt
    // (Akkorde je Schlag, Kosinus ueber die Laenge des Teils), und mit stems
    // (Ordner der Stems des ganzen Songs) je Fundstelle, welche Stems
    // spielen, dazu Stellen, an denen ein Stem allein spielt — dort liegen
    // seine Instrumente frei. Pop wiederholt Teile und Muster: der Refrain
    // kommt mehrmals, ein Instrument steht im Intro allein (Praxistest 1f:
    // im Intro des Referenzsongs spielt der other-Stem 14 s allein,
    // Pad und Lead-Synth ohne Drums und Bass).
    juce::var opFindRepeats (DawdContext& ctx, const juce::var& cmd)
    {
        if (! cmd.hasProperty ("file") || ! cmd.hasProperty ("start") || ! cmd.hasProperty ("end"))
            return resultObj (false, { { "error", "need file, start and end (the part to look for)" } });

        const double ts = (double) cmd.getProperty ("start", 0.0), te = (double) cmd.getProperty ("end", 0.0);

        if (te < ts + 2.0)
            return resultObj (false, { { "error", "the part must be at least 2 s long" } });

        std::map<juce::String, juce::File> stemFile;

        if (cmd.hasProperty ("stems"))
            if (auto err = stemFiles (cmd.getProperty ("stems", {}), stemFile); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        auto whole = cmd.clone();
        whole.getDynamicObject()->removeProperty ("start");
        whole.getDynamicObject()->removeProperty ("end");
        AnalysisSource src;

        if (auto err = loadAnalysisSource (ctx, whole, src); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (te > src.end + 0.01)
            return resultObj (false, { { "error", "end is beyond the file (" + juce::String (src.end, 3) + " s)" } });

        // Teil = (fast) ganzer Song: daneben passt keine Wiederholung
        // (Praxistest 2: 0-219 s gab nur "no other place … unique")
        if (te - ts > 0.5 * (src.end - src.start))
            return resultObj (false, { { "error", "the part (" + juce::String (ts, 1) + "-" + juce::String (te, 1) + " s) is more than half of the "
                                                    + juce::String (src.end - src.start, 1) + juce::String::fromUTF8 (" s file — no repeat fits beside it; pass start/end of one part (a section from analyzeStructure)") } });

        const auto f = analysis::computeFeatures (src.audio);
        analysis::TempoResult g;
        juce::String gridFrom;

        if (auto err = analysisGrid (ctx, whole, src, f, g, gridFrom); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (! g.valid || g.beats.size() < 16)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no beat grid found — give bpm and firstDownbeat") } });

        // Akkorde aus other + bass, wenn Stems da sind: der Gesang aendert
        // sich zwischen den Wiederholungen und verwaescht den Vergleich
        std::vector<analysis::Features> harmonic;

        for (auto* name : { "other", "bass" })
            if (stemFile.count (name))
            {
                auto spec = juce::var (new juce::DynamicObject());
                spec.getDynamicObject()->setProperty ("file", stemFile[name].getFullPathName());
                AnalysisSource s2;

                if (loadAnalysisSource (ctx, spec, s2).isEmpty())
                    harmonic.push_back (analysis::computeFeatures (s2.audio));
            }

        if (harmonic.empty())
            harmonic.push_back (f);

        // Akkorde und Pegel je Schlag
        const size_t nb = g.beats.size() - 1;
        std::vector<std::array<double, 12>> chroma (nb);
        std::vector<double> level (nb, -120.0);

        for (size_t i = 0; i < nb; ++i)
        {
            const auto a = (size_t) (g.beats[i] * f.chromaFps), b = juce::jmin (f.chroma.size(), (size_t) (g.beats[i + 1] * f.chromaFps));
            double e = 0.0, norm = 0.0;

            for (auto& h : harmonic)
                for (size_t k = (size_t) (g.beats[i] * h.chromaFps); k < juce::jmin (h.chroma.size(), (size_t) (g.beats[i + 1] * h.chromaFps)); ++k)
                    for (size_t c = 0; c < 12; ++c)
                        chroma[i][c] += h.chroma[k][c] * h.chromaEnergy[k];

            for (size_t k = a; k < b; ++k)
                e += f.chromaEnergy[k];

            for (auto v : chroma[i])
                norm += v * v;

            norm = std::sqrt (norm);

            for (auto& v : chroma[i])
                v = norm > 1.0e-12 ? v / norm : 0.0;

            level[i] = b > a ? 10.0 * std::log10 (e / (double) (b - a) + 1.0e-12) : -120.0;
        }

        // Tonart-Profil abziehen: ein Song in einer Tonart klingt in den
        // Tonklassen ueberall aehnlich (typisch 0,93) — es zaehlt der
        // Akkordverlauf, nicht die Tonart
        {
            std::array<double, 12> mean {};
            int n = 0;

            for (size_t i = 0; i < nb; ++i)
                if (level[i] > -60.0)
                {
                    for (size_t c = 0; c < 12; ++c)
                        mean[c] += chroma[i][c];

                    ++n;
                }

            for (auto& v : mean)
                v /= juce::jmax (1, n);

            for (auto& ch : chroma)
            {
                double norm = 0.0;

                for (size_t c = 0; c < 12; ++c)
                    norm += (ch[c] - mean[c]) * (ch[c] - mean[c]);

                norm = std::sqrt (norm);

                for (size_t c = 0; c < 12; ++c)
                    ch[c] = norm > 1.0e-9 ? (ch[c] - mean[c]) / norm : 0.0;
            }
        }

        size_t i0 = 0, i1 = 0;

        while (i0 < nb && g.beats[i0] < ts - 0.05)
            ++i0;

        i1 = i0;

        while (i1 + 1 < nb && g.beats[i1 + 2] <= te + 0.05)
            ++i1;

        const size_t L = i1 >= i0 ? i1 - i0 + 1 : 0;

        if (L < 4 || i0 + L > nb)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("the beat grid does not cover the part (fewer than 4 beats — an intro without drums has no detectable beats): give bpm and firstDownbeat") } });

        double targetLevel = 0.0;

        for (size_t i = 0; i < L; ++i)
            targetLevel += level[i0 + i] / (double) L;

        std::vector<std::pair<double, size_t>> sims;

        for (size_t k = 0; k + L <= nb; ++k)
        {
            double s = 0.0;

            for (size_t i = 0; i < L; ++i)
            {
                double dot = 0.0;

                for (size_t c = 0; c < 12; ++c)
                    dot += chroma[k + i][c] * chroma[i0 + i][c];

                s += dot;
            }

            sims.push_back ({ s / (double) L, k });
        }

        std::sort (sims.begin(), sims.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        const int limit = juce::jlimit (1, 20, (int) cmd.getProperty ("limit", 5));

        // Typische Aehnlichkeit (Median ueber alle Lagen) und Streuung: eine
        // Wiederholung liegt 2,5 Streuungen darueber (mindestens 0,3)
        const double typical = sims[sims.size() / 2].first;
        double spread = 0.0;

        for (auto& [sv, k] : sims)
            spread += (sv - typical) * (sv - typical);

        spread = std::sqrt (spread / (double) sims.size());
        const double needed = cmd.hasProperty ("minSimilarity") ? (double) cmd.getProperty ("minSimilarity", 0.5)
                                                             : juce::jmax (0.3, typical + 2.5 * spread);
        std::vector<std::pair<double, size_t>> picks;

        for (auto& [s, k] : sims)
        {
            if ((int) picks.size() >= limit || s < needed)
                break;

            if ((size_t) std::abs ((long) k - (long) i0) < L / 2)
                continue;

            bool overlaps = false;

            for (auto& p : picks)
                overlaps = overlaps || (size_t) std::abs ((long) k - (long) p.second) < L / 2;

            if (! overlaps)
                picks.push_back ({ s, k });
        }

        std::sort (picks.begin(), picks.end(), [] (auto& a, auto& b) { return a.second < b.second; });

        // Stems: Pegel je Stem und Bereich
        std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> stems;

        if (cmd.hasProperty ("stems"))
        {
            for (auto& [name, file] : stemFile)
                if (auto* r = ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file))
                    stems[name].reset (r);

            const auto where = stemFile.empty() ? juce::File (cmd.getProperty ("stems", {}).toString()) : stemFile.begin()->second.getParentDirectory();

            if (stems.empty())
                return resultObj (false, { { "error", "no stems in " + where.getFullPathName() + juce::String::fromUTF8 (" (drums/bass/other/vocals.wav of the whole song — separateStems {file} without start/end)") } });
        }

        auto rmsDb = [] (juce::AudioFormatReader& r, double a, double b)
        {
            const auto s0 = (juce::int64) (a * r.sampleRate), n = (juce::int64) ((b - a) * r.sampleRate);

            if (n <= 0 || s0 >= r.lengthInSamples)
                return -120.0;

            juce::AudioBuffer<float> buf ((int) r.numChannels, (int) std::min<juce::int64> (n, r.lengthInSamples - s0));
            r.read (&buf, 0, buf.getNumSamples(), s0, true, true);
            double q = 0.0;

            for (int c = 0; c < buf.getNumChannels(); ++c)
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    q += (double) buf.getSample (c, i) * buf.getSample (c, i);

            return 10.0 * std::log10 (q / juce::jmax (1.0, (double) buf.getNumSamples() * buf.getNumChannels()) + 1.0e-12);
        };

        auto playing = [&] (double a, double b)
        {
            juce::StringArray out;
            double top = -120.0;
            std::map<juce::String, double> lv;

            for (auto& [name, r] : stems)
                top = juce::jmax (top, lv[name] = rmsDb (*r, a, b));

            for (auto& [name, v] : lv)
                if (v > -45.0 && v > top - 25.0)
                    out.add (name);

            return out;
        };

        auto range = [&] (size_t k, double sim)
        {
            const double a = g.beats[k] + src.start, b = g.beats[k + L] + src.start;
            auto o = new juce::DynamicObject();
            o->setProperty ("start", round3 (a));
            o->setProperty ("end", round3 (b));

            if (sim >= 0.0)
                o->setProperty ("similarity", round2 (sim));

            double lv = 0.0;

            for (size_t i = 0; i < L; ++i)
                lv += level[k + i] / (double) L;

            o->setProperty ("levelDb", round1 (lv - targetLevel));

            if (! stems.empty())
                o->setProperty ("playing", juce::var (playing (a, b)));

            return juce::var (o);
        };

        juce::Array<juce::var> repeats;

        for (auto& [s, k] : picks)
            repeats.add (range (k, s));

        auto res = resultObj (true, { { "file", src.file }, { "bpm", round1 (g.bpm) }, { "gridFrom", gridFrom },
                                      { "part", range (i0, -1.0) }, { "repeats", repeats },
                                      { "typicalSimilarity", round2 (typical) } });
        auto* o = res.getDynamicObject();
        juce::StringArray hints;

        if (repeats.isEmpty())
            hints.add (juce::String::fromUTF8 ("no other place with clearly the same chords (similarity well above the song's typical ") + juce::String (typical, 2) + juce::String::fromUTF8 (") — the part is unique in the song"));
        else
            hints.add (juce::String (repeats.size()) + juce::String::fromUTF8 (" place(s) with the same chords — analyse them too: fewer instruments or another take show what the part contains (patterns repeat, instruments enter one by one)"));

        // Stems allein: Bereiche (>= 4 s) ohne die anderen Stems
        if (! stems.empty())
        {
            juce::Array<juce::var> exposed;
            const double win = 1.0;

            for (auto& [name, r] : stems)
            {
                double runStart = -1.0;

                for (double t = 0.0; t + win <= src.end + 1.0e-6; t += win)
                {
                    const auto p = playing (t, t + win);
                    const bool alone = p.size() == 1 && p[0] == name;

                    if (alone && runStart < 0.0)
                        runStart = t;

                    if ((! alone || t + 2.0 * win > src.end) && runStart >= 0.0)
                    {
                        const double runEnd = alone ? t + win : t;

                        if (runEnd - runStart >= 4.0)
                        {
                            auto e = new juce::DynamicObject();
                            e->setProperty ("stem", name);
                            e->setProperty ("start", round3 (runStart));
                            e->setProperty ("end", round3 (runEnd));
                            exposed.add (juce::var (e));
                            hints.add (juce::String::fromUTF8 ("the ") + name + " stem plays alone at " + juce::String (runStart, 1) + juce::String::fromUTF8 ("–")
                                       + juce::String (runEnd, 1) + juce::String::fromUTF8 (" s — its instruments are exposed there: describe them in that range (describeVoice/describeTimbre with start/end on the stem), and pass that range as exposed to describeLayers for your part — in the full mix the model may merge instruments that are separate there; take register and voicing of the part from there (the full mix hides the low voices)"));
                        }

                        runStart = -1.0;
                    }
                }
            }

            o->setProperty ("exposed", exposed);
        }

        o->setProperty ("hints", juce::var (hints));
        return res;
    }
}
