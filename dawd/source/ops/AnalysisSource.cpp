#include "AnalysisSource.h"
#include "../Transcription.h"
#include "../EngineHelpers.h"
#include "../Origins.h"
#include "../SampleLibrary.h"

#include <tuple>

namespace dawd
{
    namespace
    {
        double round3 (double v) { return std::round (v * 1000.0) / 1000.0; }

        void fill (AnalysisSource& src, juce::AudioFormatReader& reader, juce::int64 s0, juce::int64 n)
        {
            if (src.want22k)
                src.audio22k = transcription::loadMono22k (reader, s0, n);

            if (src.wantMono)
                src.audio = analysis::loadMono (reader, s0, n);

            if (src.wantStereo)
                src.stereo = transcription::loadResampled (reader, s0, n, AnalysisSource::stereoRate, 2);

            // Spitze und Clipping: vier gleiche Samples in Folge ab 0,99 sind
            // eine flache Spitze (2026-10-25: ein 24-Bit-Render der eigenen
            // Spur schnitt bei 0 dBFS ab, compareVoice meldete Register,
            // Stimmen und Rhythmus aus den Verzerrungen)
            const int chans = (int) juce::jmin (2u, reader.numChannels);
            juce::AudioBuffer<float> buf (chans, 65536);
            std::vector<int> same ((size_t) chans, 0);
            std::vector<float> last ((size_t) chans, 0.0f);
            src.peak = 0.0f;
            src.clipRuns = 0;

            for (juce::int64 pos = 0; pos < n; pos += buf.getNumSamples())
            {
                const int len = (int) juce::jmin ((juce::int64) buf.getNumSamples(), n - pos);
                reader.read (&buf, 0, len, s0 + pos, true, chans > 1);

                for (int c = 0; c < chans; ++c)
                {
                    auto* x = buf.getReadPointer (c);

                    for (int i = 0; i < len; ++i)
                    {
                        const float a = std::abs (x[i]);
                        src.peak = juce::jmax (src.peak, a);

                        if (a >= 0.99f && x[i] == last[(size_t) c])
                        {
                            if (++same[(size_t) c] == 3)
                                ++src.clipRuns;
                        }
                        else
                            same[(size_t) c] = 0;

                        last[(size_t) c] = x[i];
                    }
                }
            }
        }

        // Externe Instrumente, die eine Spur (track >= 0) bzw. der ganze Mix
        // spielt: Name je Modul
        juce::StringArray externalInstruments (te::Edit& edit, int track)
        {
            juce::StringArray names;
            auto users = userTracks (edit);

            for (int i = 0; i < users.size(); ++i)
            {
                if ((track >= 0 && i != track) || ! isMidiTrack (*users[i]) || laneGroupOf (*users[i]) != nullptr)
                    continue;

                juce::String id;
                faderTrackOf (edit, *users[i], nullptr, &id);

                if (auto* mt = id.isNotEmpty() ? findModuleTrack (edit, id) : nullptr)
                    if (auto* ep = dynamic_cast<te::ExternalPlugin*> (modulePlugin (*mt)))
                        names.addIfNotAlreadyThere (ep->getName());
            }

            return names;
        }

        // Integrierte Lautheit (EBU R128) zweier Kanaele bei rate
        double lufsOf (const std::vector<std::vector<float>>& x, double rate)
        {
            te::LoudnessMeter meter;
            const int block = 4096;
            meter.prepare (rate, 2, block);
            const size_t n = x.empty() ? 0 : x[0].size();

            for (size_t pos = 0; pos < n; pos += (size_t) block)
            {
                const float* ch[2] = { x[0].data() + pos, x[x.size() > 1 ? 1 : 0].data() + pos };
                meter.process (ch, 2, (int) juce::jmin ((size_t) block, n - pos));
            }

            meter.flush();
            return meter.getReadings().integratedLufs;
        }

        // Zweiter Render desselben Bereichs, wenn ein externes Instrument
        // mitspielt; Abstand zum ersten (first). Surge XT zieht Startphasen,
        // Drift und Noise aus einem Zufall mit Uhr-Seed und behaelt
        // Effekt-Zustand zwischen Renders (Todo 2026-10-26-surge-render-schwankt)
        juce::var renderSpread (DawdContext& ctx, const juce::var& cmd, const juce::File& first, int track)
        {
            auto plugins = externalInstruments (*ctx.edit, track);

            if (plugins.isEmpty())
                return {};

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            auto tmp = uniqueTempFile ("dawd-spread", ".wav");
            const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };
            double s = 0.0, e = 0.0;
            int t = -1;

            if (renderMixRange (ctx, cmd, tmp, s, e, t).isNotEmpty())
                return {};

            auto load = [&] (const juce::File& f)
            {
                std::unique_ptr<juce::AudioFormatReader> r (
                    ctx.engine->getAudioFileFormatManager().getWavFormat()->createReaderFor (new juce::FileInputStream (f), true));
                return r != nullptr ? transcription::loadResampled (*r, 0, r->lengthInSamples, AnalysisSource::stereoRate, 2)
                                    : std::vector<std::vector<float>> {};
            };

            auto a = load (first), b = load (tmp);

            if (a.size() < 2 || b.size() < 2)
                return {};

            // Korrelation der Samples ueber beide Kanaele (1 = gleich)
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;

            for (size_t c = 0; c < 2; ++c)
                for (size_t i = 0; i < juce::jmin (a[c].size(), b[c].size()); ++i)
                {
                    const double x = a[c][i], y = b[c][i];
                    sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y; n += 1;
                }

            const double va = saa - sa * sa / juce::jmax (1.0, n), vb = sbb - sb * sb / juce::jmax (1.0, n);
            const double corr = va > 0 && vb > 0 ? (sab - sa * sb / n) / std::sqrt (va * vb) : 1.0;
            const double la = lufsOf (a, AnalysisSource::stereoRate), lb = lufsOf (b, AnalysisSource::stereoRate);
            const double dl = la > -69.0 && lb > -69.0 ? std::abs (la - lb) : 0.0;

            juce::Array<juce::var> names;

            for (auto& p : plugins)
                names.add (p);

            auto o = new juce::DynamicObject();
            o->setProperty ("plugins", names);
            o->setProperty ("deltaLufs", std::round (dl * 100.0) / 100.0);
            o->setProperty ("correlation", std::round (corr * 1000.0) / 1000.0);
            o->setProperty ("extraSec", std::round ((juce::Time::getMillisecondCounterHiRes() - t0) / 100.0) / 10.0);
            return juce::var (o);
        }
    }

    
    juce::String blockedFileError (DawdContext& ctx, const juce::File& f)
    {
        if (ctx.library == nullptr || ctx.library->blockedForAnalysis().empty())
            return {};

        for (auto& o : originsOf (f))
            if (auto by = ctx.library->analysisBlockedBy (juce::File (o)); by != juce::File())
                return "no analysis of files in " + by.getFullPathName() + " (blocked for its licence; allowAnalysis lifts it)"
                     + (o == f.getFullPathName() ? juce::String() : ": " + f.getFileName() + " is made from " + o);

        return {};
    }

    juce::String checkMixLicence (DawdContext& ctx, double start, double end, int track, bool ml)
    {
        if (ctx.library == nullptr || ctx.edit == nullptr || ctx.library->blockedForAnalysis().empty())
            return {};

        juce::StringArray found;

        for (auto& s : soundingFiles (*ctx.edit, start, end, track))
            for (auto& o : originsOf (s.file))
                if (auto by = ctx.library->analysisBlockedBy (juce::File (o)); by != juce::File())
                {
                    found.addIfNotAlreadyThere ("track \"" + s.track + "\" plays " + o
                                                + (o == s.file.getFullPathName() ? juce::String() : " (via " + s.file.getFileName() + ")"));
                    break;
                }

        if (found.isEmpty())
            return {};

        if (ml)
            return "no analysis of material from a folder blocked for its licence: " + found.joinIntoString ("; ")
                 + " - measure another track (track) or range (start/end, section), or allowAnalysis lifts it";

        ctx.licenceNotes.addIfNotAlreadyThere (found.joinIntoString ("; ")
                                               + " - from a folder blocked for its licence; measured anyway, it is your own song");
        return {};
    }

    // file (+ start/end in Sekunden der Datei) oder der Mix wie bei
    // analyze (section, start/end, track). Fehlertext oder "".
    juce::String loadAnalysisSource (DawdContext& ctx, const juce::var& cmd, AnalysisSource& src)
    {
        if (cmd.hasProperty ("file"))
        {
            auto path = cmd.getProperty ("file", {}).toString();
            auto file = juce::File (path);

            if (path.isEmpty() || ! file.existsAsFile())
                return "no such file: " + path;

            if (auto e = blockedFileError (ctx, file); e.isNotEmpty())
                return e;

            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));

            if (reader == nullptr)
                return "cannot read audio file: " + file.getFileName();

            auto length = (double) reader->lengthInSamples / reader->sampleRate;
            src.start = (double) cmd.getProperty ("start", 0.0);
            src.end = cmd.hasProperty ("end") ? (double) cmd.getProperty ("end", 0.0) : length;

            if (src.start < 0.0 || src.end <= src.start || src.end > length + 1e-3)
                return "need 0 <= start < end <= file length (" + juce::String (round3 (length)) + " s)";

            src.file = file.getFullPathName();
            auto s0 = (juce::int64) (src.start * reader->sampleRate), n = (juce::int64) ((src.end - src.start) * reader->sampleRate);

            fill (src, *reader, s0, n);
            return {};
        }

        // Mix: braucht einen offenen Edit (die Registry prueft das nicht,
        // weil eine Datei-Analyse ohne Projekt geht)
        if (ctx.edit == nullptr)
            return "no edit";

        auto tmp = uniqueTempFile ("dawd-detect", ".wav");
        const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };

        if (auto err = renderMixRange (ctx, cmd, tmp, src.start, src.end, src.track); err.isNotEmpty())
            return err;

        if (auto e = checkMixLicence (ctx, src.start, src.end, src.track, src.ml); e.isNotEmpty())
            return e;

        std::unique_ptr<juce::AudioFormatReader> reader (
            ctx.engine->getAudioFileFormatManager().getWavFormat()->createReaderFor (new juce::FileInputStream (tmp), true));

        if (reader == nullptr)
            return "cannot read render";

        fill (src, *reader, 0, reader->lengthInSamples);

        if (src.wantSpread)
            src.spread = renderSpread (ctx, cmd, tmp, src.track);

        return {};
    }

    juce::String spreadHint (const AnalysisSource& src, const juce::String& who)
    {
        if (! src.spread.isObject())
            return {};

        const double dl = src.spread.getProperty ("deltaLufs", 0.0), corr = src.spread.getProperty ("correlation", 1.0);

        // Schwelle gewaehlt: 4OSC mit Seed und Surge ohne Zufall liegen bei
        // 1,0 / 0,0 LU (Messung im Todo), Surge mit Zufall bei Korrelation < 0,9
        if (corr >= 0.999 && dl < 0.05)
            return {};

        juce::StringArray names;

        if (auto* a = src.spread.getProperty ("plugins", {}).getArray())
            for (auto& n : *a)
                names.add (n.toString());

        return who + juce::String::fromUTF8 (" renders differently each time (")
               + names.joinIntoString (", ")
               + "): two renders differ by " + juce::String (dl, 2) + " LU, sample correlation " + juce::String (corr, 2)
               + juce::String::fromUTF8 (" — the plugin uses random start phases, drift or noise, or keeps state between renders. Deviations of that size come from the plugin, not from the settings (checked with a second render, +")
               + juce::String ((double) src.spread.getProperty ("extraSec", 0.0), 1) + " s)";
    }

    // Quelle in die Antwort: source file|mix, Bereich, Spur
    void describeAnalysisSource (juce::DynamicObject& o, const AnalysisSource& src)
    {
        o.setProperty ("source", src.file.isEmpty() ? "mix" : "file");

        if (src.file.isNotEmpty())
            o.setProperty ("file", src.file);

        o.setProperty ("start", round3 (src.start));
        o.setProperty ("end", round3 (src.end));

        if (src.track >= 0)
            o.setProperty ("track", src.track);

        if (src.spread.isObject())
            o.setProperty ("renderSpread", src.spread);

        // Pegel der Quelle (Mono-Summe) — zum Einpegeln gegen einen Stem
        // (Praxistest 1k: sechs compareReference je Runde)
        if (! src.audio.samples.empty())
        {
            double sq = 0.0;

            for (auto x : src.audio.samples)
                sq += (double) x * x;

            o.setProperty ("rmsDb", std::round (10.0 * std::log10 (sq / (double) src.audio.samples.size() + 1.0e-12) * 10.0) / 10.0);
        }
    }

    // Raster fuer Akkorde/Groove: vorgegeben (bpm, firstDownbeat in
    // Quellsekunden, beatsPerBar) > Projekt-Raster (Quelle ist der eigene
    // Mix, Tempo ist bekannt) > geschaetzt. Praxistest 1c: detectKey auf
    // dem Other-Stem schaetzte 171,5 BPM und zaehlte die Takte falsch,
    // extractGroove lag eine Achtel daneben. Fehlertext oder "".
    juce::String analysisGrid (DawdContext& ctx, const juce::var& cmd, const AnalysisSource& src,
                               const analysis::Features& f, analysis::TempoResult& g, juce::String& from)
    {
        g = {};

        if (cmd.hasProperty ("bpm") || cmd.hasProperty ("firstDownbeat"))
        {
            // Eigener Mix: ohne bpm gilt das Tempo des Projekts (Praxistest
            // 1m: compareStems {firstDownbeat} hatte kein bpm-Feld dafuer)
            const bool projectTempo = ! cmd.hasProperty ("bpm") && src.file.isEmpty() && ctx.edit != nullptr;

            if (! cmd.hasProperty ("bpm") && ! projectTempo)
                return "firstDownbeat needs bpm";

            const auto at = te::TimePosition::fromSeconds (src.start);
            const double bpm = projectTempo ? ctx.edit->tempoSequence.getBpmAt (at) : (double) cmd.getProperty ("bpm", 0.0);
            const int bpb = (int) cmd.getProperty ("beatsPerBar", projectTempo ? ctx.edit->tempoSequence.getTimeSigAt (at).numerator.get() : 4);
            const double d0 = (double) cmd.getProperty ("firstDownbeat", src.start) - src.start;

            if (bpm < 30.0 || bpm > 300.0 || bpb < 1 || bpb > 16)
                return "need bpm 30..300 and beatsPerBar 1..16";

            const double p = 60.0 / bpm;
            const double first = d0 - std::floor (d0 / p) * p;   // erster Schlag >= 0

            for (double t = first; t < f.duration; t += p)
            {
                g.beats.push_back (t);

                if (std::abs (std::remainder (t - d0, p * bpb)) < p * 0.25)
                    g.downbeats.push_back (t);
            }

            g.bpm = bpm;
            g.beatsPerBar = bpb;
            from = "given";
        }
        else if (src.file.isEmpty() && ctx.edit != nullptr)
        {
            auto& ts = ctx.edit->tempoSequence;

            // Mit section zaehlen Schlaege und Takte ab ihrem Beginn (Praxistest
            // 1e: eine Section mit einer Viertel davor verschob jede Takt-Eins
            // um eine Viertel — die Takte des Edits zaehlen ab Songbeginn;
            // Praxistest 1m: ein Auftakt von 0,11 s legte auch jeden Schlag
            // eine 16tel daneben, weil die Schlaege ab 0 s zaehlten)
            auto* section = cmd.hasProperty ("section") ? findSection (*ctx.edit, cmd.getProperty ("section", {}).toString()) : nullptr;
            const double phase = section != nullptr ? ts.toBeats (section->getPosition().getStart()).inBeats() : 0.0;
            const double b0 = phase + std::ceil (ts.toBeats (te::TimePosition::fromSeconds (src.start)).inBeats() - phase - 1.0e-6);
            const int bpbAtStart = ts.getTimeSigAt (te::TimePosition::fromSeconds (src.start)).numerator.get();

            for (double b = b0;; b += 1.0)
            {
                auto t = ts.toTime (te::BeatPosition::fromBeats (b)).inSeconds();

                if (t >= src.end - 1.0e-6)
                    break;

                g.beats.push_back (t - src.start);
                auto bb = ts.toBarsAndBeats (te::TimePosition::fromSeconds (t));

                if (section != nullptr ? std::fmod (b - phase + 1.0e-6, (double) juce::jmax (1, bpbAtStart)) < 0.25
                                       : (bb.beats.inBeats() < 0.25 || bb.beats.inBeats() > bb.numerator - 0.25))
                    g.downbeats.push_back (t - src.start);
            }

            auto& sig = ts.getTimeSigAt (te::TimePosition::fromSeconds (src.start));
            g.bpm = ts.getBpmAt (te::TimePosition::fromSeconds (src.start));
            g.beatsPerBar = sig.numerator.get();
            from = "project";
        }
        else
        {
            g = analysis::detectTempo (f);
            from = "detected";
            return {};
        }

        g.valid = g.beats.size() >= 2;
        g.confidence = g.meterConfidence = 1.0;
        return {};
    }

    void shareGrid (juce::var& a, juce::var& b)
    {
        auto copy = [] (const juce::var& from, juce::var& to)
        {
            if (! from.isObject() || ! to.isObject() || ! from.hasProperty ("file") || ! to.hasProperty ("file")
                || ! from.hasProperty ("bpm") || to.hasProperty ("bpm") || to.hasProperty ("firstDownbeat"))
                return;

            auto* o = to.getDynamicObject();
            o->setProperty ("bpm", from.getProperty ("bpm", {}));

            if (from.hasProperty ("beatsPerBar"))
                o->setProperty ("beatsPerBar", from.getProperty ("beatsPerBar", {}));

            if (from.hasProperty ("firstDownbeat"))
                o->setProperty ("firstDownbeat", (double) to.getProperty ("start", 0.0)
                                                    + (double) from.getProperty ("firstDownbeat", 0.0) - (double) from.getProperty ("start", 0.0));
        };

        copy (a, b);
        copy (b, a);
    }

    void shareGrid (DawdContext& ctx, juce::var& a, juce::var& b, juce::StringArray& hints)
    {
        auto gridOf = [] (const juce::var& v) { return v.isObject() && v.hasProperty ("file") && v.hasProperty ("bpm"); };
        auto open = [] (const juce::var& v) { return v.isObject() && v.hasProperty ("file") && ! v.hasProperty ("bpm") && ! v.hasProperty ("firstDownbeat"); };

        for (auto [from, to, who] : { std::tuple<juce::var*, juce::var*, const char*> { &a, &b, "the reference" }, { &b, &a, "yours" } })
        {
            if (! gridOf (*from) || ! open (*to))
                continue;

            AnalysisSource src;

            if (loadAnalysisSource (ctx, *to, src).isNotEmpty())
                break;   // Fehler meldet die Analyse selbst

            const auto t = analysis::detectTempo (analysis::computeFeatures (src.audio));
            const double given = (double) from->getProperty ("bpm", 0.0);

            if (t.valid && given > 0.0)
            {
                const double q = t.bpm / given;

                if (std::abs (q - 1.0) > 0.04 && std::abs (q - 2.0) > 0.08 && std::abs (q - 0.5) > 0.02)
                {
                    hints.add (juce::String (who) + " plays at about " + juce::String (t.bpm, 1) + juce::String::fromUTF8 (" BPM, the other side's grid has ")
                               + juce::String (given, 1) + juce::String::fromUTF8 (" BPM — not the same song section, so each side keeps its own grid; pass bpm + firstDownbeat for ")
                               + who + " if its detected grid is wrong");
                    break;
                }
            }

            shareGrid (a, b);
            break;
        }
    }

    // Takt/Schlag (1-basiert) einer Zeit ueber die Beat-Liste (folgt dem
    // Tempo); vor der ersten Eins negativ bzw. 0 (Auftakt)
    std::pair<int, int> barBeatAt (const analysis::TempoResult& g, double t)
    {
        auto nearest = [&g] (double x)
        {
            size_t best = 0;

            for (size_t i = 1; i < g.beats.size(); ++i)
                if (std::abs (g.beats[i] - x) < std::abs (g.beats[best] - x))
                    best = i;

            return (int) best;
        };

        const int beatsIn = nearest (t) - nearest (g.downbeats.front());
        const int bar = (int) std::floor ((double) beatsIn / g.beatsPerBar);
        return { bar + 1, beatsIn - bar * g.beatsPerBar + 1 };
    }
}
