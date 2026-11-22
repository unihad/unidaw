#include "Ops.h"
#include "../Origins.h"
#include "../EngineHelpers.h"

// Freeze/Flatten (Familie processing): eine Spur samt eigener Kette in eine
// Datei rendern und statt der Kette das Audio spielen — spart die CPU
// schwerer Plugins. Nicht ueber den Freeze Point der Engine: der friert
// Plugins auf der Spur selbst ein, bei dawd liegt die Kette auf Modul-Spuren
// hinter AuxSends (die Engine lehnt das als "connected to other tracks" ab),
// und er rendert ueber renderToFile, das headless die Progressbar braucht.
//
// Gefroren: das Audio liegt auf einer versteckten Freeze-Spur, die in die
// Gruppenspur laeuft (wie eine Lane — Mute und Solo gelten mit), die Clips
// der Spur und ihrer Lanes sind stumm, die Racks der Kette aus. Das Audio
// laeuft durch die abgeschaltete Kette (Busse, Fader, Sends bleiben); der
// Abgriff beim Rendern sitzt hinter dem Plugin des letzten Moduls, vor
// Fader und Sends — Fader, Pan und Sends hinter der Kette bleiben live.
namespace dawd
{
    namespace
    {
        const juce::Identifier frozenOffProp ("dawdFrozenOff");    // Plugin/Clip, den der Freeze abschaltet: ItemID der Gruppenspur

        struct Target
        {
            te::AudioTrack* group = nullptr;
            int index = -1;
            juce::Array<te::AudioTrack*> members;   // Gruppe und ihre Lanes
        };

        // Spur aus dem Befehl; eine Lane meint ihre Gruppe
        juce::String findTarget (te::Edit& edit, const juce::var& cmd, Target& t)
        {
            auto tracks = userTracks (edit);
            const int i = (int) (double) cmd.getProperty ("track", -1.0);

            if (! cmd.hasProperty ("track"))
                return "need track";

            if (i < 0 || i >= tracks.size())
                return "no such track: " + juce::String (i);

            t.group = laneGroupOf (*tracks[i]) != nullptr ? laneGroupOf (*tracks[i]) : tracks[i];
            t.index = tracks.indexOf (t.group);
            t.members.add (t.group);

            for (auto* x : tracks)
                if (laneGroupOf (*x) == t.group)
                    t.members.add (x);

            return {};
        }

        juce::String who (const Target& t)
        {
            return "track " + juce::String (t.index) + " (" + t.group->getName() + ")";
        }

        // Geaermt wie state.armed: ein Eingang zielt auf die Spur oder eine Lane
        bool isArmed (DawdContext& ctx, const Target& t)
        {
            auto tracks = userTracks (*ctx.edit);

            for (auto& inp : enumerateInputs (ctx))
                if (auto* idx = inp.getProperty ("tracks", {}).getArray())
                    for (auto& i : *idx)
                        if (t.members.contains (tracks[(int) (double) i]))
                            return true;

            return false;
        }

        // Kette, die sich einfrieren laesst: eigene Module (nur diese Spur
        // speist sie), der Reihe nach, einziger Ausgang der Spur. Hinter dem
        // letzten Modul ist alles frei (Sends, Verzweigung, Pegel) — davor
        // nicht, denn dort laeuft spaeter das gefrorene Audio durch.
        juce::String freezableChain (te::Edit& edit, const Target& t, juce::StringArray& chain)
        {
            juce::StringArray complex, sends;
            chain = ownChainOf (edit, "t" + juce::String (t.index), complex, sends);

            if (chain.isEmpty())
                return who (t) + " has no own plugin chain to freeze (modules shared with other tracks are not frozen)";

            if (! sends.isEmpty())
                return who (t) + " also goes to " + sends.joinIntoString (", ")
                       + juce::String::fromUTF8 (" — freeze needs its chain as the only output");

            const auto exit = chain[chain.size() - 1];

            for (auto& x : complex)
            {
                const bool afterChain = ((x.startsWith ("send after ") || x.startsWith ("branch after ")) && x.endsWith (" " + exit))
                                     || x.startsWith ("level " + exit + " >");

                if (! afterChain)
                    return who (t) + " can not be frozen: " + x
                           + juce::String::fromUTF8 (" (freeze needs a straight chain without feedback; sends and levels after its last module are fine)");
            }

            return {};
        }

        // Abgriff hinter dem Rack des letzten Moduls auf Main: fuer die Dauer
        // des Renders ein Send dorthin, die Kanten des Moduls und der Fader
        // der Spur aus (sitzt er vor der Kette, wirkt er gefroren danach).
        // Nur die Clips der Spur klingen (allowedClips wie bei den Stems).
        juce::String captureChain (DawdContext& ctx, const Target& t, te::AudioTrack& exit, const juce::File& dest,
                                   double sampleRate, double length)
        {
            auto& edit = *ctx.edit;
            te::Renderer::Parameters r (edit);
            r.destFile = dest;
            r.audioFormat = ctx.engine->getAudioFileFormatManager().getWavFormat();
            r.bitDepth = 32;
            r.sampleRateForAudio = sampleRate;
            r.blockSizeForAudio = 512;
            r.canRenderInMono = false;
            r.checkNodesForAudio = false;
            r.ditheringEnabled = false;
            r.tracksToDo.setRange (0, (int) te::getAllTracks (edit).size(), true);
            r.time = { 0_tp, te::TimePosition::fromSeconds (length) };

            for (auto* m : t.members)
                for (auto* c : m->getClips())
                    r.allowedClips.add (c);

            juce::Array<te::Plugin*> off;
            auto disable = [&off] (te::Plugin* p)
            {
                if (p != nullptr && p->isEnabled())
                {
                    p->setEnabled (false);
                    off.add (p);
                }
            };

            for (auto* p : exit.pluginList.getPlugins())
                if (dynamic_cast<te::AuxSendPlugin*> (p) != nullptr)
                    disable (p);

            if (auto* fader = faderTrackOf (edit, *t.group))
                disable (fader->getVolumePlugin());

            juce::ValueTree send (te::IDs::PLUGIN);
            send.setProperty (te::IDs::type, te::AuxSendPlugin::xmlTypeName, nullptr);
            send.setProperty (te::IDs::busNum, 0, nullptr);   // Main
            auto tap = exit.pluginList.insertPlugin (send, exit.pluginList.indexOf (moduleRackOf (exit)) + 1);

            if (auto* s = dynamic_cast<te::AuxSendPlugin*> (tap.get()))
                s->setGainDb (0.0f);

            const juce::ScopeGuard restore { [&]
            {
                if (tap != nullptr)
                    tap->deleteFromParent();

                for (auto* p : off)
                    p->setEnabled (true);
            } };

            if (tap == nullptr)
                return "cannot tap the chain";

            return renderWithParams (ctx, r);
        }

        // Gegenweg: Freeze-Spur weg, Clips und Racks wie vorher
        void unfreeze (te::Edit& edit, const Target& t)
        {
            const auto owner = t.group->itemID.toString();

            if (auto* ft = freezeTrackOf (edit, *t.group))
                edit.deleteTrack (ft);

            for (auto* p : te::getAllPlugins (edit, false))
                if (p->state.getProperty (frozenOffProp).toString() == owner)
                {
                    p->state.removeProperty (frozenOffProp, nullptr);
                    p->setEnabled (true);
                }

            for (auto* m : t.members)
                for (auto* c : m->getClips())
                    if (c->state.getProperty (frozenOffProp).toString() == owner)
                    {
                        c->state.removeProperty (frozenOffProp, nullptr);
                        c->setMuted (false);
                    }
        }

        juce::Array<juce::var> idList (const juce::StringArray& ids)
        {
            juce::Array<juce::var> a;

            for (auto& id : ids)
                a.add (id);

            return a;
        }
    }

    // freezeTrack {track, on?}: on (Default) friert ein, on: false taut auf.
    // Eine Aktion am Node (process), Undo wie jede Op.
    juce::var opFreezeTrack (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        Target t;

        if (auto err = findTarget (edit, cmd, t); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        const bool on = (bool) cmd.getProperty ("on", true);
        const bool frozen = freezeTrackOf (edit, *t.group) != nullptr;
        auto params = new juce::DynamicObject();
        params->setProperty ("track", t.index);
        params->setProperty ("on", on);

        if (! on)
        {
            if (! frozen)
                return resultObj (false, { { "error", who (t) + " is not frozen" } });

            unfreeze (edit, t);
            recordNodeAction (ctx, "process", "freezeTrack", juce::var (params));
            return resultObj (true, { { "track", t.index }, { "name", t.group->getName() }, { "on", false } });
        }

        if (frozen)
            return resultObj (false, { { "error", who (t) + " is already frozen" } });

        // Geaermte Spur: abgelehnt, nichts geaendert (Entscheidung Matthias 2026-10-24)
        if (isArmed (ctx, t))
            return resultObj (false, { { "error", who (t) + " is armed for recording; disarm it first (armTrack {track, on: false})" } });

        juce::StringArray chain;

        if (auto err = freezableChain (edit, t, chain); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto* exit = findModuleTrack (edit, chain[chain.size() - 1]);

        if (exit == nullptr || moduleRackOf (*exit) == nullptr)
            return resultObj (false, { { "error", "module " + chain[chain.size() - 1] + " has no rack" } });

        const double length = edit.getLength().inSeconds();

        if (length <= 0.0)
            return resultObj (false, { { "error", who (t) + " has nothing to freeze (the song is empty)" } });

        auto editFile = te::EditFileOperations (edit).getEditFile();
        auto dir = (editFile != juce::File() ? editFile.getParentDirectory() : juce::File::getCurrentWorkingDirectory()).getChildFile ("freeze");
        auto dest = dir.getChildFile (juce::File::createLegalFileName (t.group->getName()) + "-"
                                      + juce::String (juce::Time::currentTimeMillis()) + ".wav");
        const double rate = freezeSampleRate (ctx);
        const auto started = juce::Time::getMillisecondCounterHiRes();

        if (auto err = captureChain (ctx, t, *exit, dest, rate, length); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // Lizenz-Sperre: das Audio stammt aus dem Material der Spur
        recordOrigins (dest, soundingOrigins (edit, 0.0, length, userTracks (edit).indexOf (t.group)));

        const double renderSec = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        auto* ft = addFreezeTrack (edit, *t.group);

        if (ft == nullptr)
            return resultObj (false, { { "error", "cannot create the freeze track" } });

        te::AudioFile audioFile (*ctx.engine, dest);
        ft->insertWaveClip ("freeze", dest, { { 0_tp, te::TimePosition::fromSeconds (length) }, {} }, false);
        ctx.engine->getAudioFileManager().validateFile (audioFile, true);   // wie insertAudioClip: sonst ist die erste Wiedergabe still

        const auto owner = t.group->itemID.toString();

        for (auto* m : t.members)
            for (auto* c : m->getClips())
                if (! c->isMuted())
                {
                    c->setMuted (true);
                    c->state.setProperty (frozenOffProp, owner, nullptr);
                }

        for (auto& id : chain)
            if (auto* mt = findModuleTrack (edit, id))
                if (auto* rack = moduleRackOf (*mt); rack != nullptr && rack->isEnabled())
                {
                    rack->setEnabled (false);
                    rack->state.setProperty (frozenOffProp, owner, nullptr);
                }

        recordNodeAction (ctx, "process", "freezeTrack", juce::var (params));
        return resultObj (true, { { "track", t.index }, { "name", t.group->getName() }, { "on", true },
                                  { "chain", idList (chain) }, { "file", dest.getFullPathName() },
                                  { "length", length }, { "sampleRate", rate }, { "renderSec", std::round (renderSec * 1000.0) / 1000.0 } });
    }

    // flattenTrack {track}: der Freeze wird endgueltig — das Audio liegt als
    // normaler Clip auf der Spur (jetzt Audio), die Module der Kette und das
    // Material (Clips der Spur und ihrer Lanes) sind weg, die Spur geht
    // dorthin, wohin das letzte Modul ging (gleiche Pegel und Sends). Der
    // Fader zieht mit. Rueckweg: Undo oder der Eltern-Node.
    juce::var opFlattenTrack (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        Target t;

        if (auto err = findTarget (edit, cmd, t); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto* ft = freezeTrackOf (edit, *t.group);

        if (ft == nullptr)
            return resultObj (false, { { "error", who (t) + " is not frozen (freezeTrack first)" } });

        juce::StringArray chain, complex, sends;
        chain = ownChainOf (edit, "t" + juce::String (t.index), complex, sends);

        if (chain.isEmpty())
            return resultObj (false, { { "error", who (t) + " has no chain" } });

        const auto exitNode = findRouteNode (edit, chain[chain.size() - 1]);
        struct Out { juce::String to; double level; bool send; };
        std::vector<Out> outs;

        for (auto& to : targetsOf (edit, exitNode))
            outs.push_back ({ to, edgeLevel (edit, exitNode.id, to), isSendEdge (edit, exitNode.id, to) });

        const auto gid = "t" + juce::String (t.index);

        for (auto* m : t.members)
        {
            juce::Array<te::Clip*> clips;

            for (auto* c : m->getClips())
                clips.add (c);

            for (auto* c : clips)
                c->removeFromParent();
        }

        juce::Array<te::Clip*> frozenClips;

        for (auto* c : ft->getClips())
            frozenClips.add (c);

        int moved = 0;

        for (auto* c : frozenClips)
            if (c->moveTo (*t.group))
                ++moved;

        edit.deleteTrack (ft);

        for (auto& id : chain)
            removePluginModule (edit, id);

        for (auto* m : t.members)
            setTrackType (edit, *m, false);

        juce::Array<juce::var> routes;

        for (auto& o : outs)
            if (connectNodes (edit, gid, o.to, o.level, o.send).isEmpty())
                routes.add (o.to);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", t.index);
        recordNodeAction (ctx, "process", "flattenTrack", juce::var (params));

        return resultObj (true, { { "track", t.index }, { "name", t.group->getName() }, { "removed", idList (chain) },
                                  { "clips", moved }, { "routes", routes } });
    }

    double freezeSampleRate (DawdContext& ctx)
    {
        const auto rate = ctx.engine->getDeviceManager().getSampleRate();

        for (auto ok : { 44100.0, 48000.0, 88200.0, 96000.0 })
            if (std::abs (rate - ok) < 1.0)
                return ok;

        return 44100.0;
    }

    juce::String frozenTrackError (DawdContext& ctx, int trackIndex)
    {
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size() || freezeTrackOf (*ctx.edit, *tracks[trackIndex]) == nullptr)
            return {};

        return "track " + juce::String (trackIndex) + " (" + tracks[trackIndex]->getName()
               + ") is frozen; unfreeze it first (freezeTrack {track, on: false})";
    }
}
