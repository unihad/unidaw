#include "Ops.h"
#include "../Automation.h"
#include "../EngineHelpers.h"

// Gruppen (Submix): mehrere Spuren teilen Fader, Mute, Solo und eine eigene
// Kette. Eine Gruppe ist ein Knoten der Routing-Matrix ("g<N>", ein Bus wie
// Main, Routing.h); die Mitglieder laufen ueber normale Kanten hinein, die
// Gruppe ueber Kanten weiter (eigene Kette, Main). Mitgliedschaft und Kette
// liest dawd aus dem Graphen — keine zweite Datenhaltung. Kein Folder/Submix
// der Engine: der summiert die Track-Ausgaenge, bei dawd laufen alle
// Signale ueber AuxSends (die Ausgaenge parken in der Void-Spur).
namespace dawd
{
    namespace
    {
        juce::String trackLabel (te::Edit& edit, int i)
        {
            auto tracks = userTracks (edit);
            return "track " + juce::String (i) + (i >= 0 && i < tracks.size() ? " (" + tracks[i]->getName() + ")" : juce::String());
        }

        juce::String groupLabel (te::AudioTrack& g, const juce::String& id)
        {
            return id + " (" + moduleLabel (g) + ")";
        }

        // Spur aus dem Befehl (Index oder Name); eine Lane meint ihre Spur
        int trackFrom (te::Edit& edit, const juce::var& v)
        {
            auto tracks = userTracks (edit);
            auto i = v.isString() ? trackIndexFor (edit, v.toString()) : v.isVoid() ? -1 : (int) (double) v;

            if (i < 0 || i >= tracks.size())
                return -1;

            if (auto* g = laneGroupOf (*tracks[i]))
                i = tracks.indexOf (g);

            return i;
        }

        using Moves = std::vector<std::pair<juce::String, juce::String>>;

        // Welche End-Kanten einer Spur umziehen: beim Eintreten alle (Main,
        // Master-Kette, andere Gruppe) ausser denen, die schon in target
        // laufen; beim Austreten (leave) die in eine Gruppe. target "" = eine
        // neue Gruppe. Module auf dem Weg, die auch andere Spuren speisen
        // (geteiltes Instrument), ziehen mit — die anderen muessen dann in
        // allowed stehen. Erst pruefen, dann aendern: Fehler vor Seiteneffekten.
        juce::String planMoves (te::Edit& edit, int track, const juce::String& target, bool leave,
                                const juce::Array<int>& allowed, Moves& moves)
        {
            const auto id = "t" + juce::String (track);
            auto path = groupPathOf (edit, id);

            if (path.exits.empty())
                return trackLabel (edit, track) + juce::String::fromUTF8 (" does not reach main — route it first (pickInstrument or connect), then group it");

            auto tracks = userTracks (edit);

            for (int j = 0; j < tracks.size(); ++j)
            {
                if (j == track || allowed.contains (j) || laneGroupOf (*tracks[j]) != nullptr)
                    continue;

                auto other = groupPathOf (edit, "t" + juce::String (j));

                for (auto& n : path.nodes)
                    if (n != id && other.nodes.contains (n))
                        return n + " is shared by " + trackLabel (edit, track) + " and " + trackLabel (edit, j)
                               + juce::String::fromUTF8 (" — group them together (createGroup tracks) or give each its own plugin (linkPlugin)");
            }

            for (auto& [from, to] : path.exits)
                if (to != target && (! leave || to.startsWithChar ('g')))
                    moves.push_back ({ from, to });

            if (moves.empty())
                return leave ? trackLabel (edit, track) + " is in no group"
                             : trackLabel (edit, track) + " is already in " + target;

            // Schleife: die Gruppe laeuft (ueber ihre Kette) schon durch den
            // Weg der Spur
            if (! leave && target.isNotEmpty())
                for (auto& [from, to] : moves)
                    if (auto err = edgeError (edit, findRouteNode (edit, from), findRouteNode (edit, target)); err.isNotEmpty())
                        return from + " > " + target + ": " + err;

            return {};
        }

        juce::String applyMoves (te::Edit& edit, const Moves& moves, const juce::String& target)
        {
            for (auto& [from, to] : moves)
            {
                const auto level = edgeLevel (edit, from, to);
                disconnectNodes (edit, from, to);

                if (! targetsOf (edit, findRouteNode (edit, from)).contains (target))
                    if (auto err = connectNodes (edit, from, target, level); err.isNotEmpty())
                        return from + " > " + target + ": " + err;
            }

            return {};
        }

        bool allSoloed (te::Edit& edit, const juce::Array<int>& members)
        {
            auto tracks = userTracks (edit);
            bool all = ! members.isEmpty();

            for (auto i : members)
                all = all && tracks[i]->isSolo (false);

            return all;
        }

        juce::Array<juce::var> intList (const juce::Array<int>& xs)
        {
            juce::Array<juce::var> out;

            for (auto x : xs)
                out.add (x);

            return out;
        }
    }

    // Felder einer Gruppe fuer Timeline und Mixer: Name, Fader, Pan, Mute
    // (Abhoer-Hilfe), Solo (= alle Mitglieder solo), Mitglieder, Kette, Ziele
    juce::var groupInfo (te::Edit& edit, const juce::String& id)
    {
        auto& g = *findGroupTrack (edit, id);
        auto* fader = groupFaderOf (edit, g);
        auto* vol = fader != nullptr ? fader->getVolumePlugin() : nullptr;
        auto members = groupMembers (edit, id, false);
        juce::StringArray complex, sends;
        juce::Array<juce::var> chain, targets;

        for (auto& p : ownChainOf (edit, id, complex, sends))
            chain.add (p);

        for (auto& to : targetsOf (edit, findRouteNode (edit, id)))
            targets.add (to);

        auto o = resultObj (true, { { "id", id },
                                    { "name", moduleLabel (g) },
                                    { "members", intList (members) },
                                    { "volumeDb", vol != nullptr ? std::round (faderSetDb (*vol) * 100.0) / 100.0 : 0.0 },
                                    { "pan", vol != nullptr ? std::round (faderSetPan (*vol) * 1000.0) / 1000.0 : 0.0 },
                                    { "mute", g.isMuted (false) },
                                    { "solo", allSoloed (edit, groupMembers (edit, id, true)) },
                                    { "chain", chain },
                                    { "targets", targets } });

        o.getDynamicObject()->removeProperty ("ok");

        if (fader != nullptr && fader != &g)
            o.getDynamicObject()->setProperty ("fader", faderIdOf (edit, *fader));   // hinter der Kette der Gruppe

        return o;
    }

    juce::Array<juce::var> groupList (te::Edit& edit)
    {
        juce::Array<juce::var> list;

        for (auto& id : groupIds (edit))
            list.add (groupInfo (edit, id));

        return list;
    }

    juce::String groupOfTrack (te::Edit& edit, int track)
    {
        auto groups = groupPathOf (edit, "t" + juce::String (track)).groups;
        return groups.isEmpty() ? juce::String() : groups[0];
    }

    // createGroup {name?, tracks?}: neuer Gruppen-Bus in den Eingang von Main
    // (bzw. der Master-Kette); die Spuren laufen statt nach Main in die Gruppe
    // (Pegel der Kanten bleiben, Sends bleiben Sends — sie gehen an der Gruppe
    // vorbei wie in jedem Mischpult)
    juce::var opCreateGroup (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto name = cmd.getProperty ("name", {}).toString().trim();

        if (name.isEmpty())
            for (int n = 1; name.isEmpty() || findGroupTrack (edit, name) != nullptr; ++n)
                name = "Group " + juce::String (n);

        if (auto err = checkModuleLabel (edit, {}, name); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        juce::Array<int> members;

        if (cmd.hasProperty ("tracks"))
        {
            auto* arr = cmd.getProperty ("tracks", {}).getArray();

            if (arr == nullptr)
                return resultObj (false, { { "error", "tracks must be a list" } });

            for (auto& v : *arr)
            {
                auto i = trackFrom (edit, v);

                if (i < 0)
                    return resultObj (false, { { "error", "no such track: " + v.toString() } });

                members.addIfNotAlreadyThere (i);
            }
        }

        std::vector<Moves> plans (members.size());

        for (int k = 0; k < members.size(); ++k)
            if (auto err = planMoves (edit, members[k], {}, false, members, plans[k]); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        auto id = addGroupBus (edit, name);

        if (id.isEmpty())
            return resultObj (false, { { "error", "insert failed" } });

        connectNodes (edit, id, mainEntry (edit));

        for (auto& plan : plans)
            if (auto err = applyMoves (edit, plan, id); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        auto params = new juce::DynamicObject();
        params->setProperty ("group", id);
        params->setProperty ("name", name);
        params->setProperty ("tracks", intList (members));
        recordNodeAction (ctx, "process", "createGroup", juce::var (params));

        return resultObj (true, { { "group", id }, { "name", name }, { "members", intList (members) } });
    }

    // setGroup {track, group}: Spur in eine Gruppe (auch aus einer anderen
    // heraus); group "" oder null nimmt sie heraus — sie laeuft dann wieder
    // in den Eingang von Main
    juce::var opSetGroup (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;

        if (! cmd.hasProperty ("track"))
            return resultObj (false, { { "error", "need track" } });

        if (! cmd.hasProperty ("group"))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("need group (\"\" or null takes the track out of its group)") } });

        auto track = trackFrom (edit, cmd.getProperty ("track", {}));

        if (track < 0)
            return resultObj (false, { { "error", "no such track: " + cmd.getProperty ("track", {}).toString() } });

        const auto ref = cmd.getProperty ("group", {}).toString().trim();
        const bool leave = ref.isEmpty();
        auto target = leave ? mainEntry (edit) : groupIdFor (edit, ref);

        if (target.isEmpty())
            return resultObj (false, { { "error", "no such group: " + ref } });

        Moves moves;

        if (auto err = planMoves (edit, track, target, leave, {}, moves); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (auto err = applyMoves (edit, moves, target); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto params = new juce::DynamicObject();
        params->setProperty ("track", track);
        params->setProperty ("group", leave ? juce::String() : target);
        recordNodeAction (ctx, "process", "setGroup", juce::var (params));

        auto now = groupOfTrack (edit, track);
        return resultObj (true, { { "track", track }, { "group", now.isEmpty() ? juce::var() : juce::var (now) } });
    }

    // removeGroup {group}: die Gruppe loest sich auf — die Mitglieder laufen
    // dorthin, wohin die Gruppe lief (Pegel: Kante rein x Kante raus x
    // Fader der Gruppe, hoechstens 100 %); ihre Kette bleibt als geteiltes
    // Modul
    juce::var opRemoveGroup (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        const auto ref = cmd.getProperty ("group", {}).toString();
        auto* g = findGroupTrack (edit, ref);

        if (g == nullptr)
            return resultObj (false, { { "error", "no such group: " + ref } });

        const auto id = groupIdFor (edit, ref);
        const auto name = moduleLabel (*g);
        auto* fader = groupFaderOf (edit, *g);
        auto* vol = fader != nullptr ? fader->getVolumePlugin() : nullptr;
        const double gain = vol != nullptr ? juce::Decibels::decibelsToGain ((double) vol->getVolumeDb(), -100.0) : 1.0;
        const double pan = vol != nullptr ? (double) vol->getPan() : 0.0;

        // Kurve und Modifier am Gruppen-Fader gehen mit dem Bus verloren
        // (Matthias 2026-10-24, A1): die Antwort sagt es
        juce::StringArray lostAutomation;

        for (auto key : { "volume", "pan" })
        {
            auto t = findAutomationTarget (edit, id + "." + key);

            if (t.error.isNotEmpty() || t.bindings.empty())
                continue;

            juce::StringArray what;

            if (t.bindings.front().param->getCurve().getNumPoints() > 0)
                what.add ("curve");

            if (! describeModifier (t).isVoid())
                what.add ("modifier");

            if (! what.isEmpty())
                lostAutomation.add (id + "." + key + " (" + what.joinIntoString (", ") + ")");
        }

        std::vector<std::pair<juce::String, double>> ins, outs;

        for (auto& [from, to] : routingEdges (edit))
        {
            if (to == id)
                ins.push_back ({ from, edgeLevel (edit, from, to) });

            if (from == id)
                outs.push_back ({ to, edgeLevel (edit, from, to) });
        }

        removeGroupBus (edit, id);
        juce::Array<juce::var> bridged;
        juce::StringArray warnings;
        bool clipped = false;

        for (auto& [from, inLevel] : ins)
            for (auto& [to, outLevel] : outs)
            {
                const auto want = inLevel * outLevel * gain;
                clipped = clipped || want > 1.0001;
                const auto err = connectNodes (edit, from, to, juce::jlimit (0.0, 1.0, want));

                if (err.isEmpty())
                    bridged.add (from + juce::String::fromUTF8 (" → ") + to);
                else if (err != "already connected")
                    warnings.add (from + " can not go to " + to + " (" + err + ")");
            }

        if (outs.empty() && ! ins.empty())
            warnings.add (juce::String::fromUTF8 ("the group had no output — its members are silent now (connect them)"));

        if (clipped)
            warnings.add (juce::String::fromUTF8 ("the group fader was above 0 dB — the members are quieter now (edge levels end at 100 %)"));

        if (std::abs (pan) > 0.001)
            warnings.add ("the group pan (" + juce::String (pan, 2) + ") is gone");

        if (! lostAutomation.isEmpty())
            warnings.add ("the group automation is gone: " + lostAutomation.joinIntoString (", "));

        auto params = new juce::DynamicObject();
        params->setProperty ("group", id);
        params->setProperty ("name", name);
        recordNodeAction (ctx, "process", "removeGroup", juce::var (params));

        auto res = resultObj (true, { { "group", id }, { "name", name }, { "reconnected", bridged } });

        if (! warnings.isEmpty())
            res.getDynamicObject()->setProperty ("warning", warnings.joinIntoString ("; "));

        return res;
    }

    // groupMix {group, volumeDb?, pan?, mute?, solo?, name?}: der Kanalzug der
    // Gruppe. Fader hinter der Kette der Gruppe (wie bei Spuren); Mute und
    // Solo sind Abhoer-Hilfen wie bei trackMix (Decision 2026-10-20) — Solo
    // schaltet alle Mitglieder solo (auch die verschachtelter Gruppen)
    juce::var opGroupMix (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        const auto ref = cmd.getProperty ("group", {}).toString();
        auto* g = findGroupTrack (edit, ref);

        if (g == nullptr)
            return resultObj (false, { { "error", "no such group: " + ref } });

        const auto id = groupIdFor (edit, ref);
        const auto name = cmd.getProperty ("name", {}).toString().trim();

        if (cmd.hasProperty ("name"))
        {
            if (name.isEmpty())
                return resultObj (false, { { "error", "name must not be empty" } });

            if (auto err = checkModuleLabel (edit, id, name); err.isNotEmpty())
                return resultObj (false, { { "error", err } });
        }

        auto db = (double) cmd.getProperty ("volumeDb", 0.0);
        auto pan = (double) cmd.getProperty ("pan", 0.0);

        if (cmd.hasProperty ("volumeDb") && (db < -100.0 || db > 6.0))
            return resultObj (false, { { "error", "volumeDb must be -100..6 (engine fader)" } });

        if (cmd.hasProperty ("pan") && (pan < -1.0 || pan > 1.0))
            return resultObj (false, { { "error", "pan must be -1..1" } });

        auto* fader = groupFaderOf (edit, *g);
        auto* vol = fader != nullptr ? fader->getVolumePlugin() : nullptr;

        if (vol == nullptr && (cmd.hasProperty ("volumeDb") || cmd.hasProperty ("pan")))
            return resultObj (false, { { "error", "group has no volume" } });

        if (cmd.hasProperty ("mute"))
            listenMute (ctx, *g, (bool) cmd.getProperty ("mute", false));

        if (cmd.hasProperty ("solo"))
        {
            auto tracks = userTracks (edit);

            for (auto i : groupMembers (edit, id, true))
                tracks[i]->setSolo ((bool) cmd.getProperty ("solo", false));
        }

        if (cmd.hasProperty ("volumeDb"))
            vol->setVolumeDb ((float) db);

        if (cmd.hasProperty ("pan"))
            vol->setPan ((float) pan);

        if (cmd.hasProperty ("name"))
            setModuleLabel (*g, name);

        auto res = groupInfo (edit, id);
        res.getDynamicObject()->setProperty ("ok", true);
        res.getDynamicObject()->setProperty ("group", id);

        if ((bool) cmd.getProperty ("mute", false) || (bool) cmd.getProperty ("solo", false))
            res.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("mute and solo are listening aids for playback only — render, analyze and the other measurements ignore them; "
                                                                                 "to change the mix use volumeDb or the routing"));

        return res;
    }
}
