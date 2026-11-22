#include "Ops.h"
#include "AnalysisSource.h"
#include "../DrumAnalysis.h"

#include <set>

// Schlagzeug beschreiben und vergleichen (Familie processing, read-only):
// describeDrums je Instrument Muster je Takt, Akzente, Mikrotiming, Klang;
// compareDrums Referenz (Datei oder Spur) gegen eigene Spur mit konkreten
// Abweichungen je Takt. Die Signalverarbeitung steht in DrumAnalysis.cpp.
namespace dawd
{
    namespace
    {
        bool known (double v) { return ! std::isnan (v); }
        juce::var num (double v) { return known (v) ? juce::var (v) : juce::var(); }

        struct DrumData
        {
            AnalysisSource src;
            juce::String gridFrom;
            double bpm = 0.0, firstDownbeat = 0.0;
            int beatsPerBar = 4;
            drums::Result r;
        };

        juce::String analyse (DawdContext& ctx, const juce::var& spec, DrumData& d)
        {
            d.src.wantMono = d.src.wantStereo = true;

            if (auto e = loadAnalysisSource (ctx, spec, d.src); e.isNotEmpty())
                return e;

            auto features = analysis::computeFeatures (d.src.audio);
            analysis::TempoResult t;

            if (auto e = analysisGrid (ctx, spec, d.src, features, t, d.gridFrom); e.isNotEmpty())
                return e;

            if (! t.valid || t.beats.size() < 2 || t.downbeats.empty())
                return juce::String::fromUTF8 ("no steady pulse found — pass bpm + firstDownbeat");

            d.bpm = t.bpm;
            d.firstDownbeat = t.downbeats.front();
            d.beatsPerBar = t.beatsPerBar;

            // Mono bei 44,1 kHz aus Stereo (Hats liegen ueber 11 kHz)
            std::vector<float> mono;

            if (! d.src.stereo.empty())
            {
                mono.resize (d.src.stereo[0].size());

                for (size_t i = 0; i < mono.size(); ++i)
                    mono[i] = d.src.stereo.size() > 1 ? 0.5f * (d.src.stereo[0][i] + d.src.stereo[1][i]) : d.src.stereo[0][i];
            }

            d.r = drums::describe (mono, AnalysisSource::stereoRate, d.bpm, d.firstDownbeat, t.beatsPerBar);
            return d.r.valid ? juce::String() : (d.r.hints.isEmpty() ? juce::String ("no drums found") : d.r.hints[0]);
        }

        const drums::Instrument* find (const drums::Result& r, const juce::String& name)
        {
            for (auto& in : r.instruments)
                if (in.name == name)
                    return &in;

            return nullptr;
        }

        juce::var soundVar (const drums::Instrument& in)
        {
            auto o = new juce::DynamicObject();

            if (known (in.fundamentalHz)) o->setProperty ("fundamentalHz", in.fundamentalHz);
            if (known (in.bodyHz))        o->setProperty ("bodyHz", in.bodyHz);
            if (known (in.noiseShare))    o->setProperty ("noiseShare", in.noiseShare);
            if (known (in.brightnessHz))  o->setProperty ("brightnessHz", in.brightnessHz);
            if (known (in.decayMs))       o->setProperty ("decayMs", in.decayMs);

            return juce::var (o);
        }

        juce::var instrumentsVar (const drums::Result& r, bool detail)
        {
            auto o = new juce::DynamicObject();

            for (auto& in : r.instruments)
            {
                auto io = new juce::DynamicObject();
                io->setProperty ("hits", (int) in.hits.size());

                if (! in.hits.empty())
                {
                    juce::Array<juce::var> bars, slots;

                    for (auto& [b, p] : in.bars)
                        bars.add (resultObj (true, { { "bar", b }, { "pattern", p } }));

                    for (auto& bv : bars)
                        bv.getDynamicObject()->removeProperty ("ok");

                    for (auto& s : in.slots)
                    {
                        auto so = new juce::DynamicObject();
                        so->setProperty ("slot", s.slot);
                        so->setProperty ("beat", drums::slotName (s.slot));
                        so->setProperty ("count", s.count);
                        so->setProperty ("levelDb", s.levelDb);
                        so->setProperty ("offsetMs", s.offsetMs);
                        slots.add (juce::var (so));
                    }

                    io->setProperty ("bars", bars);
                    io->setProperty ("slots", slots);
                    io->setProperty ("sound", soundVar (in));

                    if (detail)
                    {
                        juce::Array<juce::var> hs;

                        for (auto& h : in.hits)
                        {
                            auto ho = new juce::DynamicObject();
                            ho->setProperty ("time", std::round (h.time * 1000.0) / 1000.0);
                            ho->setProperty ("bar", h.bar);
                            ho->setProperty ("beat", drums::slotName (h.slot));
                            ho->setProperty ("offMs", std::round (h.offMs * 10.0) / 10.0);
                            ho->setProperty ("levelDb", std::round (h.levelDb * 10.0) / 10.0);
                            hs.add (juce::var (ho));
                        }

                        io->setProperty ("hitList", hs);
                    }
                }

                o->setProperty (in.name, juce::var (io));
            }

            return juce::var (o);
        }

        juce::var hintVar (const juce::StringArray& h)
        {
            juce::Array<juce::var> a;

            for (auto& s : h)
                a.add (s);

            return a;
        }

        // Schlaege eines Takts (ohne leise "g")
        std::set<int> slotsOf (const drums::Instrument& in, int bar)
        {
            std::set<int> s;

            for (auto& [b, p] : in.bars)
                if (b == bar)
                    for (int i = 0; i < p.length(); ++i)
                        if (p[i] == 'X' || p[i] == 'x')
                            s.insert (i);

            return s;
        }

        juce::String beats (const std::vector<int>& slots)
        {
            juce::StringArray a;

            for (auto s : slots)
                a.add (drums::slotName (s));

            return a.joinIntoString (", ");
        }
    }

    juce::var opDescribeDrums (DawdContext& ctx, const juce::var& cmd)
    {
        DrumData d;

        if (auto e = analyse (ctx, cmd, d); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        auto res = resultObj (true);
        auto* o = res.getDynamicObject();
        describeAnalysisSource (*o, d.src);
        o->setProperty ("bpm", std::round (d.bpm * 10.0) / 10.0);
        o->setProperty ("firstDownbeat", std::round ((d.firstDownbeat + d.src.start) * 1000.0) / 1000.0);
        o->setProperty ("gridFrom", d.gridFrom);
        o->setProperty ("onsets", d.r.onsets);
        o->setProperty ("patternRepeat", d.r.patternRepeat);
        o->setProperty ("instruments", instrumentsVar (d.r, (bool) cmd.getProperty ("detail", false)));
        auto hints = d.r.hints;

        if (d.gridFrom == "detected")
            hints.add (juce::String::fromUTF8 ("bar grid detected on this source — pass bpm + firstDownbeat (detectTempo of the full mix) for exact bars"));

        // Snare fast immer neben dem Backbeat (2e/4e, 2+/4+ …): meist sitzt
        // das Raster daneben, nicht die Snare (Praxistest 1l: Snare auf
        // 2+/4+ als Halftime gelesen, 1g: auf 2e/4e). Vorschlag: die erste
        // Eins um den Versatz plus die gemessene Abweichung verschoben.
        // Snare auf 1/3 bleibt aussen vor (Halftime oder zwei Schlaege daneben).
        if (auto* sn = find (d.r, "snare"); sn != nullptr && d.r.slotsPerBar == 16 && sn->hits.size() >= 4 && d.bpm > 0.0)
        {
            std::array<int, 8> pairs {};

            for (auto& h : sn->hits)
                ++pairs[(size_t) (h.slot % 8)];

            const int s = (int) (std::max_element (pairs.begin(), pairs.end()) - pairs.begin());
            const int k = s - 4;   // 16tel hinter 2/4
            const auto n = (double) sn->hits.size();

            if (k != 0 && k != -4 && pairs[(size_t) s] >= 0.6 * n && pairs[4] <= 0.15 * n)
            {
                std::vector<double> offs;

                for (auto& h : sn->hits)
                    if (h.slot % 8 == s)
                        offs.push_back (h.offMs);

                std::sort (offs.begin(), offs.end());
                const double s16 = 60.0 / d.bpm / 4.0;
                double fd = d.firstDownbeat + k * s16 + offs[offs.size() / 2] / 1000.0;

                if (fd < 0.0)
                    fd += 16.0 * s16;

                const juce::String amount = std::abs (k) == 1 ? "a 16th" : std::abs (k) == 2 ? "an 8th" : "a dotted 8th";
                hints.add ("snare mostly on " + drums::slotName (s) + "/" + drums::slotName (s + 8)
                           + juce::String::fromUTF8 (" instead of 2/4 — the bar grid is probably ") + amount + (k > 0 ? " early" : " late")
                           + ": try firstDownbeat " + juce::String (fd + d.src.start, 3) + " (bpm " + juce::String (d.bpm, 1)
                           + "), unless the song really plays the snare off the beat");
            }
        }

        o->setProperty ("hints", hintVar (hints));
        return res;
    }

    juce::var opCompareDrums (DawdContext& ctx, const juce::var& cmd)
    {
        auto refSpec = cmd.getProperty ("reference", {});

        if (! refSpec.isObject() || ! (refSpec.hasProperty ("file") || refSpec.hasProperty ("track")))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("reference needs {file, start?, end?, bpm?, firstDownbeat?} or a track of the project {track, section? | start?/end?}") } });

        DrumData r, y;
        auto own = cmd.clone();

        for (auto key : { "reference", "limit", "detail" })
            own.getDynamicObject()->removeProperty (key);

        refSpec = refSpec.clone();
        juce::StringArray gridHints;
        shareGrid (ctx, own, refSpec, gridHints);

        if (auto e = analyse (ctx, own, y); e.isNotEmpty())
            return resultObj (false, { { "error", "yours: " + e } });

        auto ref = refSpec.clone();

        if (ref.hasProperty ("file") && ! ref.hasProperty ("end"))
        {
            const double from = (double) ref.getProperty ("start", 0.0);
            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (juce::File (ref.getProperty ("file", {}).toString())));

            if (reader != nullptr)
                ref.getDynamicObject()->setProperty ("end", juce::jmin ((double) reader->lengthInSamples / reader->sampleRate, from + (y.src.end - y.src.start)));
        }

        if (auto e = analyse (ctx, ref, r); e.isNotEmpty())
            return resultObj (false, { { "error", "reference: " + e } });

        struct Dev { double weight; juce::String what, ref, yours, fix; };
        std::vector<Dev> devs;

        // Muster je Takt und Instrument (nach Taktnummer gepaart)
        for (auto* name : { "kick", "snare" })
        {
            auto* ri = find (r.r, name);
            auto* yi = find (y.r, name);

            if (ri == nullptr || yi == nullptr)
                continue;

            juce::StringArray bars;
            double weight = 0.0;

            for (auto& [b, p] : ri->bars)
            {
                if (b < 1 || bars.size() >= 4)
                    continue;

                auto rs = slotsOf (*ri, b), ys = slotsOf (*yi, b);
                std::vector<int> extra, missing;

                for (auto s : ys) if (rs.count (s) == 0) extra.push_back (s);
                for (auto s : rs) if (ys.count (s) == 0) missing.push_back (s);

                if (extra.empty() && missing.empty())
                    continue;

                juce::String t = "bar " + juce::String (b) + ":";

                if (! extra.empty())   t << " too many on " << beats (extra);
                if (! missing.empty()) t << (extra.empty() ? "" : ";") << " missing on " << beats (missing);

                bars.add (t);
                weight += 0.3 * (double) (extra.size() + missing.size());
            }

            if (! bars.isEmpty())
                devs.push_back ({ juce::jmin (3.0, weight), juce::String (name) + " pattern", "", "", bars.joinIntoString ("; ") });
        }

        // Offene Hat: nur Plaetze, die regelmaessig kommen (Einzeltakte
        // streuen — sie klingt in die naechste 16tel hinein)
        {
            auto* ro = find (r.r, "hatOpen");
            auto* yo = find (y.r, "hatOpen");
            std::set<int> rs, ys;

            if (ro) for (auto& s : ro->slots) rs.insert (s.slot);
            if (yo) for (auto& s : yo->slots) ys.insert (s.slot);

            std::vector<int> extra, missing;

            for (auto s : ys) if (rs.count (s) == 0 && rs.count (s - 1) == 0) extra.push_back (s);
            for (auto s : rs) if (ys.count (s) == 0 && ys.count (s - 1) == 0) missing.push_back (s);

            if (! extra.empty() || ! missing.empty())
                devs.push_back ({ 0.8, "open hat", rs.empty() ? juce::String ("none") : beats (std::vector<int> (rs.begin(), rs.end())),
                                  ys.empty() ? juce::String ("none") : beats (std::vector<int> (ys.begin(), ys.end())),
                                  missing.empty() ? "no open hat on " + beats (extra) : "open hat on " + beats (missing) });
        }

        if (r.r.patternRepeat != y.r.patternRepeat)
            devs.push_back ({ 1.5, "pattern length", juce::String (r.r.patternRepeat) + "-bar pattern", juce::String (y.r.patternRepeat) + "-bar pattern",
                              r.r.patternRepeat == 2 ? juce::String::fromUTF8 ("odd and even bars differ in the reference — write a 2-bar pattern") : juce::String ("every bar the same in the reference") });

        // Klang
        auto* rk = find (r.r, "kick");
        auto* yk = find (y.r, "kick");

        if (rk && yk && known (rk->fundamentalHz) && known (yk->fundamentalHz) && rk->fundamentalHz > 0 && yk->fundamentalHz > 0
            && std::abs (std::log2 (yk->fundamentalHz / rk->fundamentalHz)) >= 0.25)
            devs.push_back ({ 1.0, "kick pitch", juce::String (rk->fundamentalHz, 0) + " Hz", juce::String (yk->fundamentalHz, 0) + " Hz",
                              juce::String::fromUTF8 ("tune the kick (sample root / pitch) or pick a ") + (yk->fundamentalHz < rk->fundamentalHz ? "higher" : "lower") + " kick" });

        if (rk && yk && known (rk->decayMs) && known (yk->decayMs) && juce::jmax (rk->decayMs, yk->decayMs) >= 2.0 * juce::jmax (1.0, juce::jmin (rk->decayMs, yk->decayMs)))
            devs.push_back ({ 0.7, "kick length", juce::String (rk->decayMs, 0) + " ms", juce::String (yk->decayMs, 0) + " ms",
                              yk->decayMs > rk->decayMs ? juce::String ("shorter kick (sample length / gate)") : juce::String ("longer kick") });

        auto* rs = find (r.r, "snare");
        auto* ys = find (y.r, "snare");

        if (rs && ys && known (rs->bodyHz) && known (ys->bodyHz) && std::abs (std::log2 (ys->bodyHz / rs->bodyHz)) >= 0.3)
            devs.push_back ({ 0.8, "snare body", juce::String (rs->bodyHz, 0) + " Hz", juce::String (ys->bodyHz, 0) + " Hz",
                              "tune the snare or boost its body around " + juce::String (rs->bodyHz, 0) + " Hz" });

        if (rs && ys && known (rs->noiseShare) && known (ys->noiseShare) && std::abs (rs->noiseShare - ys->noiseShare) >= 0.08)
            devs.push_back ({ 0.8, "snare noise", juce::String (rs->noiseShare, 2), juce::String (ys->noiseShare, 2),
                              ys->noiseShare > rs->noiseShare ? juce::String ("less snare wires/noise: lower highs above 1 kHz, darker sample") : juce::String ("more snare wires/noise above 1 kHz") });

        auto* rh = find (r.r, "hatClosed");
        auto* yh = find (y.r, "hatClosed");

        if (rh && yh && known (rh->brightnessHz) && known (yh->brightnessHz) && std::abs (std::log2 (yh->brightnessHz / rh->brightnessHz)) >= 0.35)
            devs.push_back ({ 0.6, "hat brightness", juce::String (rh->brightnessHz, 0) + " Hz", juce::String (yh->brightnessHz, 0) + " Hz",
                              yh->brightnessHz > rh->brightnessHz ? juce::String ("darker hats") : juce::String ("brighter hats") });

        // Mikrotiming der Hats je Slot (nur wo beide zaehlen)
        if (rh && yh)
        {
            juce::StringArray offs;

            for (auto& s : rh->slots)
                for (auto& t : yh->slots)
                    if (s.slot == t.slot && s.slot % 4 != 0 && std::abs (s.offsetMs - t.offsetMs) >= 10.0 && offs.size() < 3)
                        offs.add (drums::slotName (s.slot) + ": ref " + juce::String (s.offsetMs, 0) + " ms, yours " + juce::String (t.offsetMs, 0) + " ms");

            if (! offs.isEmpty())
                devs.push_back ({ 0.6, "hat timing", "", "", offs.joinIntoString ("; ") + juce::String::fromUTF8 (" — move those notes (+ = later)") });
        }

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
            list.add (juce::var (o));
        }

        auto side = [] (const DrumData& d)
        {
            auto o = new juce::DynamicObject();
            describeAnalysisSource (*o, d.src);
            o->setProperty ("gridFrom", d.gridFrom);

            // Raster der Seite (compareStems uebernimmt das der Drums fuer
            // alle Paare); bpm auf 0,01 — ueber einen ganzen Song laeuft
            // eine Rundung auf 0,1 BPM sonst bis ~50 ms auseinander
            o->setProperty ("bpm", std::round (d.bpm * 100.0) / 100.0);
            o->setProperty ("firstDownbeat", std::round ((d.firstDownbeat + d.src.start) * 1000.0) / 1000.0);
            o->setProperty ("beatsPerBar", d.beatsPerBar);
            o->setProperty ("patternRepeat", d.r.patternRepeat);

            for (auto& in : d.r.instruments)
                if (! in.bars.empty())
                {
                    auto io = new juce::DynamicObject();
                    io->setProperty ("hits", (int) in.hits.size());
                    io->setProperty ("first", in.bars.front().second + (in.bars.size() > 1 ? " " + in.bars[1].second : juce::String()));
                    io->setProperty ("sound", soundVar (in));
                    o->setProperty (in.name, juce::var (io));
                }

            return juce::var (o);
        };

        auto res = resultObj (true, { { "reference", side (r) }, { "yours", side (y) }, { "deviations", list } });

        if (devs.empty())
            gridHints.add (juce::String::fromUTF8 ("no clear deviation in pattern, length or sound"));

        if (! gridHints.isEmpty())
            res.getDynamicObject()->setProperty ("hints", hintVar (gridHints));

        return res;
    }
}
