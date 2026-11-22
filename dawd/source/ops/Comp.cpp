#include "Ops.h"
#include "../EngineHelpers.h"

#include <cmath>

namespace dawd
{
    // Comping (2026-10-24): im Bereich spielt die Lane track — im Comp der
    // Gruppe, einer eigenen Lane aus Clip-Teilen der Takes
    // (EngineHelpers compRange). Bereich als start/end in Songsekunden oder
    // section mit fromBeat/toBeat (Beats ab dem Section-Anfang, ohne = die
    // ganze Section; der Bereich darf ueber das Section-Ende laufen).
    // fadeMs: Blende an den Uebergaengen (Audio, Default 10 ms, mittig auf
    // der Grenze). Der Comp wird die spielende Lane; laneActive waehlt
    // weiter eine ganze Lane (auch den Comp zurueck).
    juce::var opCompLane (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto tracks = userTracks (edit);
        const int ti = (int) (double) cmd.getProperty ("track", -1.0);

        if (! cmd.hasProperty ("track"))
            return resultObj (false, { { "error", "need track (the take lane that plays in the range)" } });

        if (ti < 0 || ti >= tracks.size())
            return resultObj (false, { { "error", "no such track: " + juce::String (ti) } });

        auto* take = tracks[ti];
        auto* group = laneGroupOf (*take);

        if (group == nullptr)
            return resultObj (false, { { "error", "track " + juce::String (ti) + juce::String::fromUTF8 (" is not a take lane — comp picks ranges from the lanes of a group "
                                                                                                    "(laneActive picks a whole lane)") } });

        if (isCompLane (*take))
            return resultObj (false, { { "error", "track " + juce::String (ti) + juce::String::fromUTF8 (" is the comp itself — pick the range from a take lane") } });

        const bool secs = cmd.hasProperty ("start") || cmd.hasProperty ("end");
        const bool beats = cmd.hasProperty ("fromBeat") || cmd.hasProperty ("toBeat");
        const bool sec = cmd.hasProperty ("section");

        if (secs && (beats || sec))
            return resultObj (false, { { "error", "use start/end (song seconds) or section with fromBeat/toBeat, not both" } });

        if (beats && ! sec)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("fromBeat/toBeat count from the start of a section — give section") } });

        if (! secs && ! sec)
            return resultObj (false, { { "error", "need a range: start and end (song seconds) or section (fromBeat/toBeat optional)" } });

        if (secs && ! (cmd.hasProperty ("start") && cmd.hasProperty ("end")))
            return resultObj (false, { { "error", "need start and end (song seconds)" } });

        double start, end;

        if (secs)
        {
            start = (double) cmd.getProperty ("start", 0.0);
            end = (double) cmd.getProperty ("end", 0.0);
        }
        else
        {
            auto name = cmd.getProperty ("section", {}).toString();
            auto* section = findSection (edit, name);

            if (section == nullptr)
                return resultObj (false, { { "error", "no such section: " + name } });

            const auto ss = section->getPosition().getStart().inSeconds();
            const auto fb = (double) cmd.getProperty ("fromBeat", 0.0);

            if (fb < 0.0)
                return resultObj (false, { { "error", "fromBeat must be >= 0" } });

            start = ss + beatsToSeconds (edit, ss, fb);
            end = cmd.hasProperty ("toBeat") ? ss + beatsToSeconds (edit, ss, (double) cmd.getProperty ("toBeat", 0.0))
                                             : section->getPosition().getEnd().inSeconds();
        }

        if (start < 0.0)
            return resultObj (false, { { "error", "start must be >= 0" } });

        if (end <= start + 0.001)
            return resultObj (false, { { "error", "end must lie after start" } });

        const auto fadeMs = (double) cmd.getProperty ("fadeMs", 10.0);

        if (fadeMs < 0.0 || fadeMs > 500.0)
            return resultObj (false, { { "error", "fadeMs must be 0..500" } });

        // MIDI schneidet hart (die Noten gehoeren dem Teil, in dem sie beginnen)
        const bool midi = isMidiTrack (*group);
        const double fade = midi ? 0.0 : juce::jmin (fadeMs / 1000.0, end - start);
        auto* comp = compRange (edit, *take, start, end, fade);

        if (comp == nullptr)
            return resultObj (false, { { "error", "comp failed" } });

        activateLane (edit, *comp);
        forgetLaneListening (ctx, *comp);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", ti);
        params->setProperty ("start", start);
        params->setProperty ("end", end);

        if (! midi)
            params->setProperty ("fadeMs", fadeMs);

        recordNodeAction (ctx, "process", "compLane", juce::var (params));

        auto res = resultObj (true, { { "track", userTracks (edit).indexOf (comp) }, { "lane", ti },
                                      { "start", start }, { "end", end }, { "comp", compRanges (edit, *comp) } });

        if (! midi)
            res.getDynamicObject()->setProperty ("fadeMs", fadeMs);

        return res;
    }
}
