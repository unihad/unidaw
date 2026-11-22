#include "Routing.h"
#include "Automation.h"
#include "Feedback.h"
#include "Sidechain.h"
#include "PluginWindows.h"
#include "PluginSandbox.h"
#include "EngineHelpers.h"

#include <functional>
#include <map>
#include <set>

namespace dawd
{
    namespace
    {
        const juce::Identifier roleProp ("dawdRole");       // "module" | "void" | "main" | "group"
        const juce::Identifier typeProp ("dawdType");       // "midi" | "audio" (Nutzer-Spur)
        const juce::Identifier moduleProp ("dawdModule");   // Modul-Nummer N = Aux-Bus
        const juce::Identifier classProp ("dawdClass");     // midiFx | instrument | audioFx
        const juce::Identifier nextModuleProp ("dawdNextModule");
        const juce::Identifier nextFeedbackProp ("dawdNextFeedback");
        const juce::Identifier linkProp ("dawdLink");             // Link-Gruppe verlinkter Instanzen
        const juce::Identifier sendProp ("dawdSend");             // AuxSend ist ein Send (Simple-Mode)
        const juce::Identifier routingVersionProp ("dawdRouting");
        constexpr int routingVersion = 2;
        constexpr int mainBus = 0;   // Module haben Bus N >= 1
        const juce::Identifier masterProp ("dawdMaster");   // Modul der Master-Kette
        const juce::Identifier labelProp ("dawdLabel");     // Name des Moduls (renamePlugin)

        bool looksLikeId (const juce::String& s)
        {
            return s == "main" || ((s.startsWithChar ('p') || s.startsWithChar ('t') || s.startsWithChar ('g')) && s.length() > 1 && s.substring (1).containsOnly ("0123456789"));
        }

        juce::String roleOf (const te::AudioTrack& t)
        {
            return t.state.getProperty (roleProp).toString();
        }

        te::AudioTrack* trackWithRole (te::Edit& edit, const juce::String& role)
        {
            for (auto* t : te::getAudioTracks (edit))
                if (roleOf (*t) == role)
                    return t;

            return nullptr;
        }

        te::AudioTrack* voidTrack (te::Edit& edit)  { return trackWithRole (edit, "void"); }

        // Pegel einer Kante: linear 0..1 <-> dB am AuxSend
        float levelToDb (double level)  { return level <= 0.0 ? -100.0f : (float) juce::Decibels::gainToDecibels (level, -100.0); }
        double dbToLevel (float db)     { return db <= -100.0f ? 0.0 : std::round (juce::Decibels::decibelsToGain ((double) db, -100.0) * 10000.0) / 10000.0; }

        te::RackInstance* moduleRack (te::AudioTrack& t)
        {
            for (auto* p : t.pluginList.getPlugins())
                if (auto* r = dynamic_cast<te::RackInstance*> (p))
                    return r;

            return nullptr;
        }

        // Interne Plugins jeder Spur (Fader, Meter) und die Routing-Plugins
        // gehoeren nicht zur Kette
        bool isInfrastructure (te::Plugin& p)
        {
            return dynamic_cast<te::VolumeAndPanPlugin*> (&p) != nullptr
                || dynamic_cast<te::LevelMeterPlugin*> (&p) != nullptr
                || dynamic_cast<te::AuxSendPlugin*> (&p) != nullptr
                || dynamic_cast<te::AuxReturnPlugin*> (&p) != nullptr
                || dynamic_cast<te::RackInstance*> (&p) != nullptr;
        }

        bool hasSynth (te::AudioTrack& t)
        {
            for (auto* p : t.pluginList.getPlugins())
                if (p->isSynth())
                    return true;

            return false;
        }

        // Klasse eines vorhandenen Plugins (Migration)
        juce::String classOfPlugin (te::Plugin& p)
        {
            if (p.isSynth())
                return "instrument";

            if (dynamic_cast<te::MidiModifierPlugin*> (&p) != nullptr
                || dynamic_cast<te::MidiPatchBayPlugin*> (&p) != nullptr)
                return "midiFx";

            return "audioFx";
        }

        juce::Array<te::AudioTrack*> moduleTracks (te::Edit& edit)
        {
            juce::Array<te::AudioTrack*> mods;

            for (auto* t : te::getAudioTracks (edit))
                if (roleOf (*t) == "module")
                    mods.add (t);

            std::sort (mods.begin(), mods.end(), [] (auto* a, auto* b)
                       { return (int) a->state.getProperty (moduleProp) < (int) b->state.getProperty (moduleProp); });
            return mods;
        }

        // Gruppen-Busse (Submix): Rolle "group", Bus-Nummer in dawdModule wie
        // die Module (eine Nummernfolge — die Busse duerfen sich nicht teilen)
        juce::Array<te::AudioTrack*> groupTracks (te::Edit& edit)
        {
            juce::Array<te::AudioTrack*> groups;

            for (auto* t : te::getAudioTracks (edit))
                if (roleOf (*t) == "group")
                    groups.add (t);

            std::sort (groups.begin(), groups.end(), [] (auto* a, auto* b)
                       { return (int) a->state.getProperty (moduleProp) < (int) b->state.getProperty (moduleProp); });
            return groups;
        }

        bool isGroupId (const juce::String& id)
        {
            return id.startsWithChar ('g') && id.length() > 1 && id.substring (1).containsOnly ("0123456789");
        }

        te::AudioTrack* insertRoutingTrack (te::Edit& edit, const juce::String& role, const juce::String& name)
        {
            auto t = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

            if (t == nullptr)
                return nullptr;

            t->state.setProperty (roleProp, role, nullptr);
            t->setName (name);
            return t.get();
        }

        te::AudioTrack* ensureVoid (te::Edit& edit)
        {
            if (auto* v = voidTrack (edit))
                return v;

            auto* v = insertRoutingTrack (edit, "void", "dawd void");

            // Lautstaerke -inf statt Mute: AudioTrack::isMuted (true) erbt das
            // Mute des Ziels — die Sends der Quellen wuerden mit verstummen
            if (v != nullptr)
                if (auto* vol = v->getVolumePlugin())
                    vol->setVolumeDb (-100.0f);

            return v;
        }

        juce::ValueTree auxState (const char* type, int bus)
        {
            juce::ValueTree v (te::IDs::PLUGIN);
            v.setProperty (te::IDs::type, type, nullptr);
            v.setProperty (te::IDs::busNum, bus, nullptr);
            return v;
        }

        // Main ist ein Bus wie die Module: [AuxReturn Bus 0] -> Master. So
        // hat auch die Kante nach Main einen Pegel (AuxSend-Gain).
        te::AudioTrack* ensureMain (te::Edit& edit)
        {
            if (auto* m = trackWithRole (edit, "main"))
                return m;

            auto* m = insertRoutingTrack (edit, "main", "dawd main");

            if (m != nullptr)
            {
                m->setSoloIsolate (true);
                m->pluginList.insertPlugin (auxState (te::AuxReturnPlugin::xmlTypeName, mainBus), 0);
                m->getOutput().setOutputToDefaultDevice (false);
            }

            return m;
        }

        int busOf (const RouteNode& n)
        {
            return n.kind == "main" ? mainBus : (int) n.track->state.getProperty (moduleProp);
        }

        te::AuxSendPlugin* sendTo (te::AudioTrack& from, int bus)
        {
            for (auto* p : from.pluginList.getPlugins())
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p); send != nullptr && send->getBusNumber() == bus)
                    return send;

            return nullptr;
        }

        // Ausgang einer Spur bzw. eines Moduls: immer in die Void-Spur — der
        // Weg zu Main und zu den Modulen laeuft ueber Sends
        void parkOutput (te::Edit& edit, te::AudioTrack& t)
        {
            t.getOutput().setOutputToTrack (ensureVoid (edit));
        }

        void addSend (te::AudioTrack& from, int bus, double level, bool asSend = false)
        {
            auto st = auxState (te::AuxSendPlugin::xmlTypeName, bus);

            if (asSend)
                st.setProperty (sendProp, true, nullptr);

            if (auto p = from.pluginList.insertPlugin (st, -1))
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p.get()))
                    send->setGainDb (levelToDb (level));
        }

        // Rack eines Moduls, dessen Plugin eine andere Id hat als seine
        // Verbindungen: ein Preset hatte die Id des Plugins ueberschrieben
        // (Sampler aus Rezepten bis 2026-10-19) — die Verbindungen auf das
        // Plugin umbiegen, sonst bleibt das Modul stumm
        void repairRackPluginIds (te::Edit& edit)
        {
            bool repaired = false;

            for (auto* rack : edit.getRackList().getTypes())
            {
                auto plugins = rack->getPlugins();

                if (plugins.size() != 1)
                    continue;

                const auto id = plugins[0]->itemID;
                juce::Array<te::EditItemID> stale;

                for (auto* c : rack->getConnections())
                    for (auto end : { c->sourceID.get(), c->destID.get() })
                        if (end.isValid() && end != id)
                            stale.addIfNotAlreadyThere (end);

                // nur der eindeutige Fall: genau eine fremde Id, die es nicht gibt
                if (stale.size() != 1 || te::findPluginForID (edit, stale[0]) != nullptr)
                    continue;

                for (auto* c : rack->getConnections())
                {
                    auto& rc = const_cast<te::RackConnection&> (*c);

                    if (rc.sourceID.get() == stale[0])
                        rc.sourceID = id;

                    if (rc.destID.get() == stale[0])
                        rc.destID = id;
                }

                repaired = true;
            }

            if (repaired)
                edit.restartPlayback();
        }

        // Plugin des Moduls in ein Rack wickeln: die RackInstance bringt
        // Dry/Wet fuer jedes Plugin mit (auch VST3 ohne Mix-Regler)
        void wrapInRack (te::Edit& edit, te::AudioTrack& t, te::Plugin& plugin)
        {
            te::Plugin::Array plugins;
            plugins.add (&plugin);
            auto index = t.pluginList.getPlugins().indexOf (&plugin);

            if (auto rack = te::RackType::createTypeToWrapPlugins (plugins, edit))
                t.pluginList.insertPlugin (te::RackInstance::create (*rack), index);
        }

        // Feedback-Kante im Edit: Send (Quelle) und Return (Ziel) mit
        // gleichem Schluessel
        struct FeedbackEdge
        {
            juce::String from, to;
            FeedbackSendPlugin* send = nullptr;
            FeedbackReturnPlugin* ret = nullptr;
        };

        juce::String nodeIdOf (te::Edit& edit, te::AudioTrack& t)
        {
            if (roleOf (t) == "module")
                return "p" + t.state.getProperty (moduleProp).toString();

            if (roleOf (t) == "group")
                return "g" + t.state.getProperty (moduleProp).toString();

            return trackNodeId (edit, t);
        }

        std::vector<FeedbackEdge> feedbackEdges (te::Edit& edit)
        {
            std::map<int, FeedbackEdge> byKey;

            for (auto* t : te::getAudioTracks (edit))
                for (auto* p : t->pluginList.getPlugins())
                {
                    if (auto* s = dynamic_cast<FeedbackSendPlugin*> (p))
                    {
                        auto& e = byKey[s->key.get()];
                        e.send = s;
                        e.from = nodeIdOf (edit, *t);
                    }
                    else if (auto* r = dynamic_cast<FeedbackReturnPlugin*> (p))
                    {
                        auto& e = byKey[r->key.get()];
                        e.ret = r;
                        e.to = nodeIdOf (edit, *t);
                    }
                }

            std::vector<FeedbackEdge> edges;

            for (auto& [k, e] : byKey)
                edges.push_back (e);

            // Module der Reihe nach, wie die Matrix
            std::sort (edges.begin(), edges.end(), [] (auto& a, auto& b)
                       { return a.from != b.from ? a.from < b.from : a.to < b.to; });
            return edges;
        }

        // Halbe Kanten (Quelle oder Ziel weg, z. B. nach treeCompact) raeumen
        void pruneFeedback (te::Edit& edit)
        {
            for (auto& e : feedbackEdges (edit))
                if (e.send == nullptr || e.ret == nullptr || e.from.isEmpty() || e.to.isEmpty())
                {
                    if (e.send != nullptr) e.send->deleteFromParent();
                    if (e.ret != nullptr)  e.ret->deleteFromParent();
                }
        }

        const FeedbackEdge* findFeedback (const std::vector<FeedbackEdge>& edges, const juce::String& from, const juce::String& to)
        {
            for (auto& e : edges)
                if (e.from == from && e.to == to && e.send != nullptr && e.ret != nullptr)
                    return &e;

            return nullptr;
        }

        double roundLevel (double v)  { return std::round (v * 10000.0) / 10000.0; }

        // Alle normalen Kanten des Graphen (Quellen und Module als Ausgaenge)
        struct Edge { juce::String from, to; };

        std::vector<Edge> allEdges (te::Edit& edit)
        {
            std::vector<Edge> edges;
            juce::StringArray outs;
            auto tracks = userTracks (edit);

            for (int i = 0; i < tracks.size(); ++i)
                if (laneGroupOf (*tracks[i]) == nullptr)
                    outs.add ("t" + juce::String (i));

            for (auto* m : moduleTracks (edit))
                outs.add ("p" + m->state.getProperty (moduleProp).toString());

            for (auto* g : groupTracks (edit))
                outs.add ("g" + g->state.getProperty (moduleProp).toString());

            for (auto& from : outs)
                for (auto& to : targetsOf (edit, findRouteNode (edit, from)))
                    edges.push_back ({ from, to });

            return edges;
        }

        juce::StringArray inputsOf (const std::vector<Edge>& edges, const juce::String& id)
        {
            juce::StringArray ins;

            for (auto& e : edges)
                if (e.to == id)
                    ins.addIfNotAlreadyThere (e.from);

            return ins;
        }

        juce::StringArray outputsOf (const std::vector<Edge>& edges, const juce::String& id)
        {
            juce::StringArray outs;

            for (auto& e : edges)
                if (e.from == id)
                    outs.add (e.to);

            return outs;
        }

        // Kette einer Spur aus dem Graphen lesen: Module, die nur von ihrem
        // Vorgaenger gespeist werden, der Reihe nach. complex sammelt, was
        // der Simple-Mode nicht darstellt.
        struct Chain
        {
            juce::StringArray plugins, sends, complex;
            juce::StringArray postSends;   // hinter dem letzten Modul, hinter dem Fader (Post-Fader-Send)
            bool toMain = false;
            juce::String toMaster;   // endet im Eingang der Master-Kette (Modul-Id)
            juce::String toGroup;    // endet in einer Gruppe (Submix) — wie Main ein Ende, keine Verzweigung
        };

        Chain deriveChain (te::Edit& edit, const std::vector<Edge>& edges,
                           const std::vector<FeedbackEdge>& fb, const juce::String& trackId)
        {
            Chain c;
            auto cur = trackId;
            juce::StringArray seen { trackId };

            for (;;)
            {
                auto outs = outputsOf (edges, cur);
                juce::StringArray exclusive, sends;

                // Als Send markierte Kanten gehoeren nie zur Kette
                for (auto& o : outs)
                    if (isSendEdge (edit, cur, o))
                        sends.add (o);
                    else if (o.startsWithChar ('p') && inputsOf (edges, o) == juce::StringArray { cur } && ! seen.contains (o)
                             && ! isMasterModule (edit, o))   // die Summe gehoert nie zur Kette einer Spur
                        exclusive.add (o);

                // Erste Gruppe ohne Modul davor: das Ende der Kette (wie Main)
                juce::String groupEnd;

                for (auto& o : outs)
                    if (isGroupId (o) && exclusive.isEmpty() && ! sends.contains (o) && ! outs.contains ("main"))
                        { groupEnd = o; break; }

                if (cur == trackId)
                {
                    for (auto& o : outs)
                    {
                        // Ohne eigene Kette direkt in die Master-Kette: ein
                        // Ende wie Main, kein Send (Matthias 2026-10-24, P1d)
                        if (exclusive.isEmpty() && isMasterModule (edit, o) && ! sends.contains (o) && c.toMaster.isEmpty())
                            c.toMaster = o;
                        else if (o != exclusive[0] && o != groupEnd && ! (exclusive.isEmpty() && o == "main" && ! sends.contains (o)))
                            c.sends.add (o);
                    }
                }
                else
                {
                    // Hinter dem letzten Modul ist ein Send in ein Modul keine
                    // Verzweigung, sondern ein Post-Fader-Send (Decision
                    // 2026-10-24): markiert immer, eine normale Kante nur neben
                    // einem Ende der Kette — Main, Master-Kette und Gruppen
                    // sind Enden, nie Sends
                    const bool last = exclusive.isEmpty();
                    auto postTarget = [&] (const juce::String& o) { return last && o.startsWithChar ('p') && ! isMasterModule (edit, o); };
                    juce::StringArray loose;

                    for (auto& o : outs)
                        if (sends.contains (o))
                        {
                            if (postTarget (o))
                                c.postSends.add (o);
                            else
                                c.complex.addIfNotAlreadyThere ("send after " + cur);
                        }
                        else if (isMasterModule (edit, o) && exclusive.isEmpty() && c.toMaster.isEmpty())
                            c.toMaster = o;
                        else if (o != "main" && o != groupEnd && ! exclusive.contains (o))
                        {
                            if (postTarget (o))
                                loose.add (o);
                            else
                                c.complex.addIfNotAlreadyThere ("branch after " + cur);
                        }

                    if (! loose.isEmpty())
                    {
                        const bool hasEnd = (outs.contains ("main") && ! sends.contains ("main")) || c.toMaster.isNotEmpty()
                                         || groupEnd.isNotEmpty();

                        if (hasEnd)
                            c.postSends.addArray (loose);
                        else
                            c.complex.addIfNotAlreadyThere ("branch after " + cur);   // die Kante ist das Ende
                    }

                    if (! exclusive.isEmpty() && outs.contains ("main"))
                        c.complex.addIfNotAlreadyThere ("branch after " + cur);
                }

                if (exclusive.size() > 1)
                    c.complex.addIfNotAlreadyThere ("parallel plugins after " + cur);

                if (exclusive.isEmpty())
                {
                    c.toMain = outs.contains ("main") && ! sends.contains ("main");
                    c.toGroup = groupEnd;
                    break;
                }

                auto next = exclusive[0];

                if (std::abs (edgeLevel (edit, cur, next) - 1.0) > 0.001)
                    c.complex.addIfNotAlreadyThere ("level " + cur + " > " + next);

                c.plugins.add (next);
                seen.add (next);
                cur = next;
            }

            if (c.toMain && std::abs (edgeLevel (edit, cur, "main") - 1.0) > 0.001)
                c.complex.addIfNotAlreadyThere ("level " + cur + " > main");

            if (c.toGroup.isNotEmpty() && std::abs (edgeLevel (edit, cur, c.toGroup) - 1.0) > 0.001)
                c.complex.addIfNotAlreadyThere ("level " + cur + " > " + c.toGroup);

            for (auto& e : fb)
                if (seen.contains (e.from) || seen.contains (e.to))
                    c.complex.addIfNotAlreadyThere ("feedback " + e.from + " > " + e.to);

            for (auto& [from, to] : sidechainEdges (edit))
                if (seen.contains (from) || seen.contains (to))
                    c.complex.addIfNotAlreadyThere ("sidechain " + from + " > " + to);

            return c;
        }

        // DFS ueber die Kanten: erreicht from den Knoten target?
        bool reaches (te::Edit& edit, const juce::String& from, const juce::String& target)
        {
            juce::StringArray todo { from }, seen;

            while (! todo.isEmpty())
            {
                auto id = todo[todo.size() - 1];
                todo.remove (todo.size() - 1);

                if (id == target)
                    return true;

                if (seen.contains (id))
                    continue;

                seen.add (id);
                auto n = findRouteNode (edit, id);

                if (n.error.isEmpty())
                {
                    todo.addArray (targetsOf (edit, n));
                    todo.addArray (sidechainTargetsOf (edit, id));   // Schluessel zaehlen mit
                }
            }

            return false;
        }
    }

    bool isRoutingTrack (const te::AudioTrack& t)
    {
        return roleOf (t).isNotEmpty();
    }

    juce::Array<te::AudioTrack*> userTracks (te::Edit& edit)
    {
        juce::Array<te::AudioTrack*> tracks;

        for (auto* t : te::getAudioTracks (edit))
            if (! isRoutingTrack (*t))
                tracks.add (t);

        return tracks;
    }

    te::AudioTrack* freezeTrackOf (te::Edit& edit, const te::AudioTrack& group)
    {
        for (auto* t : te::getAudioTracks (edit))
            if (roleOf (*t) == "freeze" && t->getOutput().getDestinationTrack() == &group)
                return t;

        return nullptr;
    }

    te::AudioTrack* addFreezeTrack (te::Edit& edit, te::AudioTrack& group)
    {
        auto* t = insertRoutingTrack (edit, "freeze", "dawd freeze " + group.getName());

        if (t != nullptr)
            t->getOutput().setOutputToTrack (&group);

        return t;
    }

    juce::Array<te::AudioTrack*> clipTracks (te::Edit& edit)
    {
        juce::Array<te::AudioTrack*> tracks;

        for (auto* t : te::getAudioTracks (edit))
            if (! isRoutingTrack (*t) || roleOf (*t) == "freeze")
                tracks.add (t);

        return tracks;
    }

    juce::StringArray ownChainOf (te::Edit& edit, const juce::String& trackId, juce::StringArray& complex,
                                  juce::StringArray& sends, juce::StringArray* postSends)
    {
        auto c = deriveChain (edit, allEdges (edit), feedbackEdges (edit), trackId);
        complex = c.complex;
        sends = c.sends;

        if (postSends != nullptr)
            *postSends = c.postSends;

        return c.plugins;
    }

    te::AudioTrack* appendUserTrack (te::Edit& edit)
    {
        // Vor die erste Routing-Spur: die Nutzer-Spuren bleiben vorn, ihre
        // automatischen Namen ("Track N") zaehlen dann richtig
        te::AudioTrack* firstRouting = nullptr;

        for (auto* t : te::getAudioTracks (edit))
            if (isRoutingTrack (*t))
                { firstRouting = t; break; }

        auto t = edit.insertNewAudioTrack (firstRouting != nullptr ? te::TrackInsertPoint (*firstRouting, true)
                                                                   : te::TrackInsertPoint::getEndOfTracks (edit),
                                           nullptr);

        if (t == nullptr)
            return nullptr;

        t->state.setProperty (typeProp, "audio", nullptr);
        parkOutput (edit, *t);

        // Gibt es eine Master-Kette, laeuft die neue Spur in deren Eingang
        auto entry = mainEntry (edit);
        auto* entryTrack = entry == "main" ? nullptr : findModuleTrack (edit, entry);
        addSend (*t, entryTrack != nullptr ? (int) entryTrack->state.getProperty (moduleProp) : mainBus, 1.0);
        return t.get();
    }

    te::AudioTrack* laneGroupOf (const te::AudioTrack& t)
    {
        auto* dest = t.getOutput().getDestinationTrack();
        return dest != nullptr && ! isRoutingTrack (*dest) ? dest : nullptr;
    }

    bool isMidiTrack (const te::AudioTrack& t)
    {
        auto type = t.state.getProperty (typeProp).toString();

        if (type.isNotEmpty())
            return type == "midi";

        return hasSynth (const_cast<te::AudioTrack&> (t));
    }

    void setTrackType (te::Edit& edit, te::AudioTrack& t, bool midi)
    {
        t.state.setProperty (typeProp, midi ? "midi" : "audio", nullptr);

        auto node = findRouteNode (edit, trackNodeId (edit, t));

        if (node.error.isNotEmpty())
            return;

        for (auto& to : targetsOf (edit, node))
            if (findRouteNode (edit, to).in != node.out)
                disconnectNodes (edit, node.id, to);
    }

    bool isValidPluginClass (const juce::String& cls)
    {
        return cls == "midiFx" || cls == "instrument" || cls == "audioFx";
    }

    juce::String classInput (const juce::String& cls)
    {
        return cls == "audioFx" ? "audio" : "midi";
    }

    juce::String classOutput (const juce::String& cls)
    {
        return cls == "midiFx" ? "midi" : "audio";
    }

    juce::String trackNodeId (te::Edit& edit, const te::AudioTrack& t)
    {
        auto idx = userTracks (edit).indexOf (const_cast<te::AudioTrack*> (&t));
        return idx < 0 ? juce::String() : "t" + juce::String (idx);
    }

    namespace
    {
        // Fader-Traeger: welche Spur ein Modul (oder die Spur selbst) als
        // Fader traegt; die Spur merkt sich den letzten Wert, falls ihr
        // Traeger geloescht wird
        const juce::Identifier faderOfProp ("dawdFaderOf"), faderDbProp ("dawdFaderDb"), faderPanProp ("dawdFaderPan");

        // Ort vor der Kette (bis 2026-10-21 der einzige): die Audio-Spur
        // selbst, bei MIDI-Spuren das erste Instrument im Routing
        te::AudioTrack* preChainFader (te::Edit& edit, te::AudioTrack& track, juce::StringArray* sharedWith, juce::String* instrumentId)
        {
            auto* group = laneGroupOf (track) != nullptr ? laneGroupOf (track) : &track;

            if (! isMidiTrack (*group))
                return &track;

            auto self = trackNodeId (edit, *group);
            juce::StringArray queue = targetsOf (edit, findRouteNode (edit, self)), seen;

            while (! queue.isEmpty())
            {
                auto id = queue[0];
                queue.remove (0);

                if (seen.contains (id))
                    continue;

                seen.add (id);
                auto* m = findModuleTrack (edit, id);

                if (m == nullptr)
                    continue;

                auto cls = m->state.getProperty (classProp).toString();

                if (cls == "instrument")
                {
                    if (instrumentId != nullptr)
                        *instrumentId = id;

                    if (sharedWith != nullptr)
                        for (auto& [from, to] : routingEdges (edit))
                            if (to == id && from != self && ! seen.contains (from))
                                sharedWith->addIfNotAlreadyThere (from);

                    return m;
                }

                if (cls == "midiFx")
                    queue.addArray (targetsOf (edit, findRouteNode (edit, id)));
            }

            return nullptr;
        }

        void moveCurve (te::AutomatableParameter& from, te::AutomatableParameter& to)
        {
            auto& a = from.getCurve();

            if (a.getNumPoints() == 0)
                return;

            auto& b = to.getCurve();
            b.clear (nullptr);

            for (int i = 0; i < a.getNumPoints(); ++i)
                b.addPoint (a.getPointTime (i), a.getPointValue (i), a.getPointCurve (i), nullptr);

            a.clear (nullptr);
        }

        // Pegel, Pan und ihre Automation (Kurve und LFO) von einem Fader auf einen anderen;
        // der alte steht danach neutral
        void moveFader (te::VolumeAndPanPlugin& from, te::VolumeAndPanPlugin& to)
        {
            settleModifierBase (*from.volParam);
            settleModifierBase (*from.panParam);
            to.setVolumeDb (from.getVolumeDb());
            to.setPan (from.getPan());
            moveCurve (*from.volParam, *to.volParam);
            moveCurve (*from.panParam, *to.panParam);
            moveModifier (from.edit, *from.volParam, *to.volParam);
            moveModifier (from.edit, *from.panParam, *to.panParam);
            from.setVolumeDb (0.0f);
            from.setPan (0.0f);
        }

        // Ein Modul, das den Fader einer Spur traegt, wird geloescht: der
        // Fader parkt samt Automation an der Spur selbst, syncFaders zieht
        // ihn danach an den neuen Ort
        void parkFader (te::Edit& edit, te::AudioTrack& module)
        {
            const auto owner = module.state.getProperty (faderOfProp).toString();

            if (owner.isEmpty())
                return;

            auto owners = userTracks (edit);
            owners.addArray (groupTracks (edit));   // der Fader einer Gruppe hinter ihrer Kette

            for (auto* t : owners)
                if (nodeIdOf (edit, *t) == owner)
                    if (auto* from = module.getVolumePlugin(); from != nullptr && t->getVolumePlugin() != nullptr)
                    {
                        moveFader (*from, *t->getVolumePlugin());
                        module.state.removeProperty (faderOfProp, nullptr);
                        t->state.setProperty (faderOfProp, owner, nullptr);
                    }
        }
    }

    // Der Fader sitzt nach der Kette, wie der Kanal-Fader nach den Inserts
    // (Entscheidung Matthias 2026-10-21; Praxistests 1h, 1k, 1m: ein Limiter
    // in der Kette glich den Fader davor aus): am letzten Modul der eigenen
    // Kette. Ohne eigene Kette, bei Verzweigungen oder geteiltem Instrument
    // der Ort davor. Ein Send hinter dem letzten Modul ist keine Verzweigung
    // (deriveChain: postSends) — er geht hinter dem Fader ab (Decision
    // 2026-10-24). Audio-Lanes behalten ihren eigenen Fader.
    te::AudioTrack* faderTrackOf (te::Edit& edit, te::AudioTrack& track, juce::StringArray* sharedWith, juce::String* instrumentId)
    {
        auto* pre = preChainFader (edit, track, sharedWith, instrumentId);
        auto* group = laneGroupOf (track) != nullptr ? laneGroupOf (track) : &track;

        if (group != &track && ! isMidiTrack (*group))
            return pre;

        auto c = deriveChain (edit, allEdges (edit), feedbackEdges (edit), trackNodeId (edit, *group));

        for (auto& x : c.complex)
            if (x.startsWith ("branch") || x.startsWith ("parallel") || x.startsWith ("send after"))
                return pre;

        if (c.plugins.isEmpty())
            return pre;

        auto* last = findModuleTrack (edit, c.plugins[c.plugins.size() - 1]);

        if (last == nullptr || last->state.getProperty (classProp).toString() == "midiFx")
            return pre;

        // MIDI-Spur: nur wenn ihr Instrument in der eigenen Kette liegt
        if (isMidiTrack (*group))
        {
            bool ownInstrument = false;

            for (auto& id : c.plugins)
                ownInstrument = ownInstrument || findRouteNode (edit, id).cls == "instrument";

            if (! ownInstrument)
                return pre;
        }

        return last;
    }

    juce::String faderIdOf (te::Edit& edit, te::AudioTrack& fader)
    {
        return nodeIdOf (edit, fader);
    }

    te::AudioTrack* groupFaderOf (te::Edit& edit, te::AudioTrack& group)
    {
        // Wie bei den Spuren: am letzten Modul der eigenen Kette der Gruppe,
        // ohne Kette oder bei Verzweigungen am Gruppen-Bus selbst
        auto c = deriveChain (edit, allEdges (edit), feedbackEdges (edit), nodeIdOf (edit, group));

        for (auto& x : c.complex)
            if (x.startsWith ("branch") || x.startsWith ("parallel") || x.startsWith ("send after"))
                return &group;

        auto* last = c.plugins.isEmpty() ? nullptr : findModuleTrack (edit, c.plugins[c.plugins.size() - 1]);
        return last != nullptr && last->state.getProperty (classProp).toString() == "audioFx" ? last : &group;
    }

    void syncFaders (te::Edit& edit)
    {
        auto users = userTracks (edit);
        auto mods = moduleTracks (edit);

        // Spuren (t<i>) und Gruppen (g<N>): Ort vor der Kette ist die Spur
        // bzw. der Gruppen-Bus
        std::vector<std::pair<te::AudioTrack*, te::AudioTrack*>> owners;   // Traeger, Fader-Ort

        for (auto* t : users)
            if (laneGroupOf (*t) == nullptr)
                owners.push_back ({ t, faderTrackOf (edit, *t) });

        for (auto* g : groupTracks (edit))
            owners.push_back ({ g, groupFaderOf (edit, *g) });

        for (auto [t, at] : owners)
        {
            auto* atVol = at != nullptr ? at->getVolumePlugin() : nullptr;

            if (atVol == nullptr)
                continue;

            const auto self = nodeIdOf (edit, *t);
            te::AudioTrack* holder = t->state.getProperty (faderOfProp).toString() == self ? t : nullptr;

            for (auto* m : mods)
                if (m->state.getProperty (faderOfProp).toString() == self)
                    holder = m;

            // Alte Projekte und neue Spuren: der Wert liegt am Ort vor der Kette
            if (holder == nullptr && ! t->state.hasProperty (faderDbProp))
                holder = roleOf (*t) == "group" ? t : preChainFader (edit, *t, nullptr, nullptr);

            if (holder != at)
            {
                if (auto* hv = holder != nullptr ? holder->getVolumePlugin() : nullptr)
                    moveFader (*hv, *atVol);
                else if (t->state.hasProperty (faderDbProp))
                {
                    // Traeger geloescht (removePlugin, pickInstrument): letzter Wert
                    atVol->setVolumeDb ((float) (double) t->state.getProperty (faderDbProp));
                    atVol->setPan ((float) (double) t->state.getProperty (faderPanProp, 0.0));
                }

                if (holder != nullptr)
                    holder->state.removeProperty (faderOfProp, nullptr);
            }

            // Auch wenn der Fader schon am richtigen Ort sitzt (neue Spur):
            // ohne Traeger-Marke hielt der naechste Lauf den Ort fuer geloescht
            // und setzte den ersten trackMix auf den alten Wert zurueck
            // Rueckfallwert: der eingestellte Wert, nicht der LFO-Stand
            // (Todo 2026-10-30-lfo-wert-im-fader: im Tal gestoppt stand hier -100 dB)
            at->state.setProperty (faderOfProp, self, nullptr);
            t->state.setProperty (faderDbProp, faderSetDb (*atVol), nullptr);
            t->state.setProperty (faderPanProp, faderSetPan (*atVol), nullptr);
        }
    }

    juce::String moduleLabel (te::AudioTrack& t)
    {
        return t.state.getProperty (labelProp).toString();
    }

    juce::String checkModuleLabel (te::Edit& edit, const juce::String& id, const juce::String& label)
    {
        auto l = label.trim();

        if (l.isEmpty())
            return {};

        if (l.length() > 40)
            return "name too long (max 40 characters)";

        if (looksLikeId (l))
            return "name must not look like an id (p3, t0, main)";

        // Module und Gruppen teilen die Namen — ein Name ist eine Adresse
        auto named = moduleTracks (edit);
        named.addArray (groupTracks (edit));

        for (auto* m : named)
            if (m->state.getProperty (labelProp).toString().equalsIgnoreCase (l) && nodeIdOf (edit, *m) != id)
                return "name already used by " + nodeIdOf (edit, *m);

        return {};
    }

    void setModuleLabel (te::AudioTrack& t, const juce::String& label)
    {
        if (label.trim().isEmpty())
            t.state.removeProperty (labelProp, nullptr);
        else
            t.state.setProperty (labelProp, label.trim(), nullptr);
    }

    juce::String moduleIdFor (te::Edit& edit, const juce::String& ref)
    {
        if (auto* m = findModuleTrack (edit, ref))
            return "p" + m->state.getProperty (moduleProp).toString();

        return {};
    }

    RouteNode findRouteNode (te::Edit& edit, const juce::String& idIn)
    {
        // Ein Modul-Name steht fuer seine Id
        auto id = idIn;

        if (! looksLikeId (id))
        {
            if (auto mid = moduleIdFor (edit, id); mid.isNotEmpty())
                id = mid;
            else if (auto gid = groupIdFor (edit, id); gid.isNotEmpty())
                id = gid;
        }

        RouteNode n;
        n.id = id;

        if (id == "main")
        {
            n.kind = "main";
            n.in = "audio";
            return n;
        }

        auto num = id.substring (1);

        if (id.length() < 2 || ! num.containsOnly ("0123456789"))
        {
            n.error = "no such node: " + id;
            return n;
        }

        if (id.startsWithChar ('t'))
        {
            auto tracks = userTracks (edit);
            auto idx = num.getIntValue();

            if (idx >= tracks.size())
            {
                n.error = "no such node: " + id;
                return n;
            }

            n.kind = "track";
            n.track = tracks[idx];
            n.out = isMidiTrack (*n.track) ? "midi" : "audio";

            if (auto* g = laneGroupOf (*n.track))
                n.error = id + juce::String::fromUTF8 (" is a lane — route its track t") + juce::String (tracks.indexOf (g));

            return n;
        }

        if (id.startsWithChar ('g'))
        {
            if (auto* t = findGroupTrack (edit, id))
            {
                n.kind = "group";
                n.track = t;
                n.in = "audio";
                n.out = "audio";
                return n;
            }
        }

        if (id.startsWithChar ('p'))
        {
            if (auto* t = findModuleTrack (edit, id))
            {
                n.kind = "plugin";
                n.track = t;
                n.cls = t->state.getProperty (classProp).toString();
                n.in = classInput (n.cls);
                n.out = classOutput (n.cls);
                return n;
            }
        }

        n.error = "no such node: " + id;
        return n;
    }

    juce::StringArray targetsOf (te::Edit& edit, const RouteNode& node)
    {
        juce::StringArray targets;

        if (node.track == nullptr)
            return targets;

        juce::Array<int> buses;

        for (auto* p : node.track->pluginList.getPlugins())
            if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                buses.addIfNotAlreadyThere (send->getBusNumber());

        for (auto* m : moduleTracks (edit))
            if (buses.contains ((int) m->state.getProperty (moduleProp)))
                targets.add ("p" + m->state.getProperty (moduleProp).toString());

        for (auto* g : groupTracks (edit))
            if (buses.contains ((int) g->state.getProperty (moduleProp)))
                targets.add ("g" + g->state.getProperty (moduleProp).toString());

        if (buses.contains (mainBus))
            targets.add ("main");

        return targets;
    }

    namespace
    {
        // edgeError mit austauschbarer Schleifenpruefung: routingState fragt
        // ~1000 Zellen und rechnet die Erreichbarkeit dafuer einmal vor
        juce::String edgeErrorWith (const RouteNode& from, const RouteNode& to,
                                    const std::function<bool (const juce::String&, const juce::String&)>& reachesFn)
        {
            if (from.error.isNotEmpty())
                return from.error;

            if (to.error.isNotEmpty())
                return to.error;

            if (from.kind == "main")
                return "main has no output";

            if (to.kind == "track")
                return "tracks have no input";

            if (from.id == to.id)
                return "would create a loop";

            if (from.out == "midi" && to.in == "audio")
                return to.kind == "main" ? "midi cannot reach main" : "midi needs an instrument or midi fx";

            if (from.out == "audio" && to.in == "midi")
                return "audio cannot enter a midi plugin";

            if (reachesFn (to.id, from.id))
                return "would create a loop";

            return {};
        }
    }

    juce::String edgeError (te::Edit& edit, const RouteNode& from, const RouteNode& to)
    {
        return edgeErrorWith (from, to, [&edit] (const juce::String& a, const juce::String& b) { return reaches (edit, a, b); });
    }

    bool isSendEdge (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (from.track == nullptr || to.error.isNotEmpty() || to.kind == "track")
            return false;

        auto* send = sendTo (*from.track, busOf (to));
        return send != nullptr && (bool) send->state.getProperty (sendProp, false);
    }

    juce::String connectNodes (te::Edit& edit, const juce::String& fromId, const juce::String& toId, double level,
                               bool asSend)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (auto err = edgeError (edit, from, to); err.isNotEmpty())
            return err;

        if (targetsOf (edit, from).contains (toId))
            return "already connected";

        if (level < 0.0 || level > 1.0)
            return "level must be 0..1";

        addSend (*from.track, busOf (to), level, asSend);
        return {};
    }

    juce::String setEdgeLevel (te::Edit& edit, const juce::String& fromId, const juce::String& toId, double level)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (from.error.isNotEmpty())
            return from.error;

        if (to.error.isNotEmpty())
            return to.error;

        if (level < 0.0 || level > 1.0)
            return "level must be 0..1";

        auto* send = from.track != nullptr && to.kind != "track" ? sendTo (*from.track, busOf (to)) : nullptr;

        if (send == nullptr)
            return "not connected";

        send->setGainDb (levelToDb (level));
        return {};
    }

    double edgeLevel (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (from.track == nullptr || to.error.isNotEmpty() || to.kind == "track")
            return 0.0;

        auto* send = sendTo (*from.track, busOf (to));
        return send != nullptr ? dbToLevel (send->getGainDb()) : 0.0;
    }

    juce::String disconnectNodes (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        // Eine Lane hat keine Kanten — ihr Fehler gilt auch hier
        if (from.error.isNotEmpty())
            return from.error;

        if (to.error.isNotEmpty())
            return to.error;

        if (! targetsOf (edit, from).contains (toId))
            return "not connected";

        auto bus = busOf (to);

        for (auto* p : from.track->pluginList.getPlugins())
            if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p); send != nullptr && send->getBusNumber() == bus)
                p->deleteFromParent();

        return {};
    }

    namespace
    {
        int allocateBus (te::Edit& edit)
        {
            auto n = juce::jmax (1, (int) edit.state.getProperty (nextModuleProp, 1));

            // Hoechste vorhandene Nummer + 1 — Ids werden nie wiederverwendet,
            // auch wenn das Feld fehlt (die Aktionen am Node nennen sie); die
            // Gruppen-Busse zaehlen mit (gemeinsame Bus-Nummern)
            auto buses = moduleTracks (edit);
            buses.addArray (groupTracks (edit));

            for (auto* m : buses)
                n = juce::jmax (n, (int) m->state.getProperty (moduleProp) + 1);

            edit.state.setProperty (nextModuleProp, n + 1, nullptr);
            return n;
        }
    }

    juce::String addPluginModule (te::Edit& edit, const juce::ValueTree& pluginState, const juce::String& cls)
    {
        auto n = allocateBus (edit);
        auto* t = insertRoutingTrack (edit, "module", "p" + juce::String (n));

        if (t == nullptr)
            return {};

        t->state.setProperty (moduleProp, n, nullptr);
        t->state.setProperty (classProp, cls, nullptr);

        // Solo einer Quelle darf die Module nicht stummschalten
        t->setSoloIsolate (true);

        t->pluginList.insertPlugin (auxState (te::AuxReturnPlugin::xmlTypeName, n), 0);
        auto plugin = t->pluginList.insertPlugin (pluginState, 1);

        if (plugin == nullptr)
        {
            edit.deleteTrack (t);
            return {};
        }

        t->setName (plugin->getName());
        wrapInRack (edit, *t, *plugin);
        parkOutput (edit, *t);
        return "p" + juce::String (n);
    }

    void markMasterModule (te::Edit& edit, const juce::String& id)
    {
        if (auto* m = findModuleTrack (edit, id))
            m->state.setProperty (masterProp, true, nullptr);
    }

    bool isMasterModule (te::Edit& edit, const juce::String& id)
    {
        auto* m = findModuleTrack (edit, id);
        return m != nullptr && (bool) m->state.getProperty (masterProp, false);
    }

    juce::StringArray masterModules (te::Edit& edit)
    {
        juce::StringArray masters;

        for (auto* m : moduleTracks (edit))
            if ((bool) m->state.getProperty (masterProp, false))
                masters.add ("p" + m->state.getProperty (moduleProp).toString());

        return masters;
    }

    juce::String mainEntry (te::Edit& edit)
    {
        auto masters = masterModules (edit);

        if (masters.isEmpty())
            return "main";

        auto edges = allEdges (edit);
        juce::StringArray heads;

        // Kopf = Master-Modul ohne Master-Eingang, das ueber Master-Module
        // (oder direkt) Main erreicht
        for (auto& m : masters)
        {
            bool fedByMaster = false;

            for (auto& in : inputsOf (edges, m))
                fedByMaster = fedByMaster || masters.contains (in);

            if (fedByMaster)
                continue;

            auto cur = m;

            for (int guard = 0; guard <= masters.size() && cur != "main"; ++guard)
            {
                juce::String next;

                for (auto& e : edges)
                    if (e.from == cur && (e.to == "main" || masters.contains (e.to)))
                        { next = e.to; break; }

                if (next.isEmpty())
                    break;

                cur = next;
            }

            if (cur == "main")
                heads.add (m);
        }

        return heads.size() == 1 ? heads[0] : juce::String ("main");
    }

    te::AudioTrack* findModuleTrack (te::Edit& edit, const juce::String& id)
    {
        auto num = id.substring (1);

        if (! id.startsWithChar ('p') || num.isEmpty() || ! num.containsOnly ("0123456789"))
        {
            // Name statt Id
            if (id.trim().isEmpty() || looksLikeId (id))
                return nullptr;

            for (auto* m : moduleTracks (edit))
                if (m->state.getProperty (labelProp).toString().equalsIgnoreCase (id.trim()))
                    return m;

            return nullptr;
        }

        for (auto* m : moduleTracks (edit))
            if ((int) m->state.getProperty (moduleProp) == num.getIntValue())
                return m;

        return nullptr;
    }

    te::Plugin* modulePlugin (te::AudioTrack& t)
    {
        if (auto* rack = moduleRack (t); rack != nullptr && rack->type != nullptr)
            for (auto* p : rack->type->getPlugins())
                return p;

        for (auto* p : t.pluginList.getPlugins())
            if (! isInfrastructure (*p))
                return p;

        return nullptr;
    }

    double moduleWet (te::AudioTrack& t)
    {
        auto* rack = moduleRack (t);
        return rack != nullptr ? std::round (rack->wetGain->getCurrentValue() * 10000.0) / 10000.0 : 1.0;
    }

    juce::String setModuleWet (te::Edit& edit, const juce::String& id, double wet)
    {
        auto* t = findModuleTrack (edit, id);

        if (t == nullptr)
            return "no such plugin: " + id;

        if (t->state.getProperty (classProp).toString() == "midiFx")
            return "midi plugins have no wet/dry";

        if (wet < 0.0 || wet > 1.0)
            return "wet must be 0..1";

        auto* rack = moduleRack (*t);

        if (rack == nullptr)
            return "plugin has no rack";

        // Ein Knopf wie beim Synth: wet = Anteil durchs Plugin, der Rest
        // geht trocken weiter
        rack->wetGain->setParameter ((float) wet, juce::sendNotification);
        rack->dryGain->setParameter ((float) (1.0 - wet), juce::sendNotification);
        return {};
    }

    bool removePluginModule (te::Edit& edit, const juce::String& id)
    {
        auto* t = findModuleTrack (edit, id);

        if (t == nullptr)
            return false;

        parkFader (edit, *t);
        auto bus = (int) t->state.getProperty (moduleProp);

        for (auto* other : te::getAudioTracks (edit))
            for (auto* p : other->pluginList.getPlugins())
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p); send != nullptr && send->getBusNumber() == bus)
                    p->deleteFromParent();

        for (auto& e : feedbackEdges (edit))
            if (e.from == id || e.to == id)
            {
                if (e.send != nullptr) e.send->deleteFromParent();
                if (e.ret != nullptr)  e.ret->deleteFromParent();
            }

        te::RackType::Ptr rackType;

        if (auto* rack = moduleRack (*t))
            rackType = rack->type;

        edit.deleteTrack (t);

        if (rackType != nullptr)
            edit.getRackList().removeRackType (rackType);

        return true;
    }

    // ---- Gruppen (Submix) -------------------------------------------------

    juce::String addGroupBus (te::Edit& edit, const juce::String& name)
    {
        auto n = allocateBus (edit);
        auto* t = insertRoutingTrack (edit, "group", "g" + juce::String (n));

        if (t == nullptr)
            return {};

        t->state.setProperty (moduleProp, n, nullptr);

        // Solo der Mitglieder darf die Gruppe nicht stummschalten (wie Module)
        t->setSoloIsolate (true);
        t->pluginList.insertPlugin (auxState (te::AuxReturnPlugin::xmlTypeName, n), 0);
        setModuleLabel (*t, name);
        parkOutput (edit, *t);
        return "g" + juce::String (n);
    }

    te::AudioTrack* findGroupTrack (te::Edit& edit, const juce::String& ref)
    {
        if (isGroupId (ref))
        {
            for (auto* g : groupTracks (edit))
                if ((int) g->state.getProperty (moduleProp) == ref.substring (1).getIntValue())
                    return g;

            return nullptr;
        }

        if (ref.trim().isEmpty() || looksLikeId (ref))
            return nullptr;

        for (auto* g : groupTracks (edit))
            if (g->state.getProperty (labelProp).toString().equalsIgnoreCase (ref.trim()))
                return g;

        return nullptr;
    }

    juce::String groupIdFor (te::Edit& edit, const juce::String& ref)
    {
        auto* g = findGroupTrack (edit, ref);
        return g != nullptr ? nodeIdOf (edit, *g) : juce::String();
    }

    juce::StringArray groupIds (te::Edit& edit)
    {
        juce::StringArray ids;

        for (auto* g : groupTracks (edit))
            ids.add (nodeIdOf (edit, *g));

        return ids;
    }

    bool removeGroupBus (te::Edit& edit, const juce::String& id)
    {
        auto* t = findGroupTrack (edit, id);

        if (t == nullptr || ! isGroupId (id))
            return false;

        auto bus = (int) t->state.getProperty (moduleProp);

        for (auto* other : te::getAudioTracks (edit))
            for (auto* p : other->pluginList.getPlugins())
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p); send != nullptr && send->getBusNumber() == bus)
                    p->deleteFromParent();

        for (auto& e : feedbackEdges (edit))
            if (e.from == id || e.to == id)
            {
                if (e.send != nullptr) e.send->deleteFromParent();
                if (e.ret != nullptr)  e.ret->deleteFromParent();
            }

        edit.deleteTrack (t);
        return true;
    }

    GroupPath groupPathOf (te::Edit& edit, const juce::String& nodeId)
    {
        // Normale Kanten ohne Sends; Main, die Master-Kette und Gruppen sind
        // Enden (hinter einer Gruppe geht es nicht weiter)
        GroupPath gp;
        juce::StringArray todo { nodeId };

        while (! todo.isEmpty())
        {
            auto id = todo[0];
            todo.remove (0);

            if (gp.nodes.contains (id))
                continue;

            gp.nodes.add (id);
            auto n = findRouteNode (edit, id);

            if (n.error.isNotEmpty())
                continue;

            for (auto& to : targetsOf (edit, n))
            {
                if (isSendEdge (edit, id, to))
                    continue;

                if (to == "main" || isMasterModule (edit, to) || isGroupId (to))
                {
                    gp.exits.push_back ({ id, to });

                    if (isGroupId (to))
                        gp.groups.addIfNotAlreadyThere (to);
                }
                else
                    todo.add (to);
            }
        }

        return gp;
    }

    juce::Array<int> groupMembers (te::Edit& edit, const juce::String& id, bool nested)
    {
        // Gruppen, die (ueber verschachtelte Gruppen) in id laufen
        juce::StringArray inner { id };

        for (bool grew = nested; grew;)
        {
            grew = false;

            for (auto& g : groupIds (edit))
                if (! inner.contains (g))
                    for (auto& x : groupPathOf (edit, g).groups)
                        if (inner.contains (x))
                            { inner.add (g); grew = true; break; }
        }

        juce::Array<int> members;
        auto tracks = userTracks (edit);

        for (int i = 0; i < tracks.size(); ++i)
            if (laneGroupOf (*tracks[i]) == nullptr)
                for (auto& g : groupPathOf (edit, "t" + juce::String (i)).groups)
                    if (inner.contains (g))
                        { members.add (i); break; }

        return members;
    }

    namespace
    {
        juce::String feedbackErrorWith (const std::vector<FeedbackEdge>& fbEdges, const RouteNode& from, const RouteNode& to)
        {
            if (from.error.isNotEmpty())
                return from.error;

            if (to.error.isNotEmpty())
                return to.error;

            if (from.kind == "main")
                return "main has no output";

            if (to.kind != "plugin")
                return "feedback goes into a plugin";

            if (from.out != "audio" || to.in != "audio")
                return "feedback is audio only";

            if (findFeedback (fbEdges, from.id, to.id) != nullptr)
                return "already connected";

            return {};
        }
    }

    juce::String feedbackError (te::Edit& edit, const RouteNode& from, const RouteNode& to)
    {
        return feedbackErrorWith (feedbackEdges (edit), from, to);
    }

    juce::String connectFeedback (te::Edit& edit, const juce::String& fromId, const juce::String& toId,
                                  double level, double delayMs)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (auto err = feedbackError (edit, from, to); err.isNotEmpty())
            return err;

        if (level < 0.0 || level > 1.0)
            return "level must be 0..1";

        if (delayMs < 0.0 || delayMs > maxFeedbackDelayMs)
            return "delayMs must be 0.." + juce::String ((int) maxFeedbackDelayMs);

        auto key = juce::jmax (1, (int) edit.state.getProperty (nextFeedbackProp, 1));

        for (auto& e : feedbackEdges (edit))
            key = juce::jmax (key, (e.send != nullptr ? e.send->key.get() : e.ret->key.get()) + 1);

        edit.state.setProperty (nextFeedbackProp, key + 1, nullptr);

        juce::ValueTree sendState (te::IDs::PLUGIN);
        sendState.setProperty (te::IDs::type, FeedbackSendPlugin::xmlTypeName, nullptr);
        sendState.setProperty ("fbKey", key, nullptr);

        juce::ValueTree retState (te::IDs::PLUGIN);
        retState.setProperty (te::IDs::type, FeedbackReturnPlugin::xmlTypeName, nullptr);
        retState.setProperty ("fbKey", key, nullptr);
        retState.setProperty ("fbLevel", level, nullptr);
        retState.setProperty ("fbDelayMs", delayMs, nullptr);

        // Return direkt hinter den AuxReturn, vor das Plugin (Rack); Send
        // ans Ende der Kette der Quelle (nach dem Fader, wie die Sends)
        to.track->pluginList.insertPlugin (retState, 1);
        from.track->pluginList.insertPlugin (sendState, -1);
        return {};
    }

    juce::String disconnectFeedback (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto edges = feedbackEdges (edit);
        auto* e = findFeedback (edges, fromId, toId);

        if (e == nullptr)
            return "not connected";

        e->send->deleteFromParent();
        e->ret->deleteFromParent();
        return {};
    }

    juce::String setFeedback (te::Edit& edit, const juce::String& fromId, const juce::String& toId,
                              const juce::var& level, const juce::var& delayMs)
    {
        auto edges = feedbackEdges (edit);
        auto* e = findFeedback (edges, fromId, toId);

        if (e == nullptr)
            return "not connected";

        if (! level.isVoid() && ((double) level < 0.0 || (double) level > 1.0))
            return "level must be 0..1";

        if (! delayMs.isVoid() && ((double) delayMs < 0.0 || (double) delayMs > maxFeedbackDelayMs))
            return "delayMs must be 0.." + juce::String ((int) maxFeedbackDelayMs);

        if (! level.isVoid())
            e->ret->levelParam->setParameter ((float) (double) level, juce::sendNotification);

        if (! delayMs.isVoid())
            e->ret->delayParam->setParameter ((float) (double) delayMs, juce::sendNotification);

        return {};
    }

    te::AudioTrack* mainBusTrack (te::Edit& edit)
    {
        return trackWithRole (edit, "main");
    }

    juce::StringArray moduleIds (te::Edit& edit)
    {
        juce::StringArray ids;

        for (auto* m : moduleTracks (edit))
            ids.add ("p" + m->state.getProperty (moduleProp).toString());

        return ids;
    }

    std::vector<std::pair<juce::String, juce::String>> routingEdges (te::Edit& edit)
    {
        std::vector<std::pair<juce::String, juce::String>> out;

        for (auto& e : allEdges (edit))
            out.emplace_back (e.from, e.to);

        return out;
    }

    std::vector<std::pair<juce::String, juce::String>> routingFeedbackEdges (te::Edit& edit)
    {
        std::vector<std::pair<juce::String, juce::String>> out;

        for (auto& e : feedbackEdges (edit))
            if (e.send != nullptr && e.ret != nullptr)
                out.emplace_back (e.from, e.to);

        return out;
    }

    te::RackInstance* moduleRackOf (te::AudioTrack& moduleTrack)
    {
        return moduleRack (moduleTrack);
    }

    te::AuxSendPlugin* edgeSend (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (from.track == nullptr || to.error.isNotEmpty() || to.kind == "track")
            return nullptr;

        return sendTo (*from.track, busOf (to));
    }

    FeedbackReturnPlugin* feedbackReturn (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto edges = feedbackEdges (edit);
        auto* e = findFeedback (edges, fromId, toId);
        return e != nullptr ? e->ret : nullptr;
    }

    juce::String moduleLink (te::AudioTrack& t)
    {
        return t.state.getProperty (linkProp).toString();
    }

    juce::Array<te::AudioTrack*> linkedModules (te::Edit& edit, const juce::String& id)
    {
        juce::Array<te::AudioTrack*> linked;
        auto* t = findModuleTrack (edit, id);

        if (t == nullptr || moduleLink (*t).isEmpty())
            return linked;

        for (auto* m : moduleTracks (edit))
            if (m != t && moduleLink (*m) == moduleLink (*t))
                linked.add (m);

        return linked;
    }

    juce::String linkPluginModule (te::Edit& edit, const juce::String& sourceId)
    {
        auto* src = findModuleTrack (edit, sourceId);
        auto* p = src != nullptr ? modulePlugin (*src) : nullptr;

        if (p == nullptr)
            return {};

        // Kopie mit frischen Ids — gleiche Einstellungen, eigenes Objekt
        p->flushPluginStateToValueTree();
        auto copy = p->state.createCopy();
        te::EditItemID::remapIDs (copy, nullptr, edit);

        if (moduleLink (*src).isEmpty())
            src->state.setProperty (linkProp, sourceId, nullptr);

        auto id = addPluginModule (edit, copy, src->state.getProperty (classProp).toString());

        if (auto* t = findModuleTrack (edit, id))
            t->state.setProperty (linkProp, moduleLink (*src), nullptr);

        return id;
    }

    juce::String setTrackChain (te::Edit& edit, int track, const juce::StringArray& plugins)
    {
        return setNodeChain (edit, "t" + juce::String (track), plugins);
    }

    // Kette einer Spur oder einer Gruppe (eigene Kette des Submix)
    juce::String setNodeChain (te::Edit& edit, const juce::String& trackId, const juce::StringArray& plugins)
    {
        auto node = findRouteNode (edit, trackId);

        if (node.error.isNotEmpty())
            return node.error;

        if (node.kind != "track" && node.kind != "group")
            return "no such track";

        juce::StringArray unique;

        for (auto& p : plugins)
        {
            if (findModuleTrack (edit, p) == nullptr)
                return "no such plugin: " + p;

            if (unique.contains (p))
                return p + juce::String::fromUTF8 (" twice in the chain — link it instead");

            unique.add (p);
        }

        auto edges = allEdges (edit);
        auto old = deriveChain (edit, edges, feedbackEdges (edit), trackId);

        // Module der neuen Kette gehoeren nur dieser Spur
        juce::StringArray own { trackId };
        own.addArray (old.plugins);
        own.addArray (plugins);

        for (auto& p : plugins)
            for (auto& in : inputsOf (edges, p))
                if (! own.contains (in))
                    return p + juce::String::fromUTF8 (" is shared — link it for this track instead");

        // Alte Kettenkanten und die Ausgaenge der neuen Kettenmodule raus
        // (mit Pegel, fuer das Zurueckrollen)
        struct Saved { juce::String from, to; double level; bool send; };
        std::vector<Saved> removed;

        // Post-Fader-Sends des alten letzten Moduls ziehen ans neue Ende
        // mit (Art und Pegel bleiben), sonst gingen sie verloren bzw.
        // hingen vor dem Fader
        std::vector<Saved> post;

        for (auto& to : old.postSends)
        {
            const auto from = old.plugins[old.plugins.size() - 1];
            post.push_back ({ from, to, edgeLevel (edit, from, to), isSendEdge (edit, from, to) });
        }

        auto drop = [&] (const juce::String& from, const juce::String& to)
        {
            for (auto& r : removed)
                if (r.from == from && r.to == to)
                    return;

            removed.push_back ({ from, to, edgeLevel (edit, from, to), isSendEdge (edit, from, to) });
            disconnectNodes (edit, from, to);
        };

        // Ziel der neuen Kette: wo die alte endete. Lief sie in ein
        // gemeinsames Modul (Master-Bus-Kompressor), endet die neue dort —
        // nicht auf Main (Praxistest 1i: die Drum-Kette aus applyRecipe
        // umging Master-Comp und Limiter, +4,4 dBTP). Ohne alte Kette der
        // Kopf der Master-Kette wie bei neuen Spuren.
        // Eine Gruppe (Submix) bleibt das Ende, wie Main.
        juce::String tail = old.toMain ? juce::String ("main") : old.toGroup.isNotEmpty() ? old.toGroup
                          : old.toMaster.isNotEmpty() ? old.toMaster : mainEntry (edit);

        if (! old.toMain && old.toGroup.isEmpty() && old.toMaster.isEmpty() && ! old.plugins.isEmpty())
        {
            const auto last = old.plugins[old.plugins.size() - 1];
            juce::StringArray ends;

            for (auto& o : outputsOf (edges, last))
                if (! isSendEdge (edit, last, o) && ! old.plugins.contains (o) && ! plugins.contains (o))
                    ends.add (o);

            if (ends.size() == 1)
                tail = ends[0];
        }

        if (plugins.contains (tail))
            tail = "main";

        juce::String prev = trackId;

        for (auto& p : old.plugins)
        {
            drop (prev, p);
            prev = p;
        }

        if (old.toMain)
            drop (prev, "main");
        else if (tail != "main" && outputsOf (edges, prev).contains (tail) && ! isSendEdge (edit, prev, tail))
            drop (prev, tail);   // auch die Spur selbst direkt im Master-Eingang: sonst laeuft sie doppelt

        for (auto& p : plugins)
            for (auto& o : outputsOf (edges, p))
                drop (p, o);

        for (auto& ps : post)
            drop (ps.from, ps.to);

        // Neue Kette verdrahten
        std::vector<std::pair<juce::String, juce::String>> made;
        juce::StringArray path { trackId };
        path.addArray (plugins);
        path.add (tail);
        juce::String err;

        for (int i = 0; i + 1 < path.size() && err.isEmpty(); ++i)
        {
            if (targetsOf (edit, findRouteNode (edit, path[i])).contains (path[i + 1]))
                continue;

            err = connectNodes (edit, path[i], path[i + 1]);

            if (err.isEmpty())
                made.push_back ({ path[i], path[i + 1] });
            else
                err = path[i] + " > " + path[i + 1] + ": " + err;
        }

        const auto end = plugins.isEmpty() ? trackId : plugins[plugins.size() - 1];

        for (auto& ps : post)
        {
            if (err.isNotEmpty())
                break;

            if (plugins.contains (ps.to) || targetsOf (edit, findRouteNode (edit, end)).contains (ps.to))
                continue;   // jetzt Teil der Kette bzw. schon verbunden

            err = connectNodes (edit, end, ps.to, ps.level, ps.send);

            if (err.isEmpty())
                made.push_back ({ end, ps.to });
            else
                err = end + " > " + ps.to + ": " + err;
        }

        if (err.isNotEmpty())
        {
            for (auto& [f, t] : made)
                disconnectNodes (edit, f, t);

            for (auto& r : removed)
                connectNodes (edit, r.from, r.to, r.level, r.send);
        }

        return err;
    }

    void ensureRouting (te::Edit& edit)
    {
        unityMasterVolume (edit);   // jeder neue und geladene Edit
        ensureVoid (edit);
        ensureMain (edit);
        pruneFeedback (edit);
        pruneSidechains (edit);
        repairRackPluginIds (edit);
        syncFaders (edit);

        auto version = (int) edit.state.getProperty (routingVersionProp, 0);

        if (version >= routingVersion)
            return;

        auto tracks = userTracks (edit);

        // v0 + v1: Ausgang auf Master war die Kante nach Main — wird ein
        // Send auf den Main-Bus, der Ausgang parkt in der Void-Spur
        juce::Array<te::AudioTrack*> outputs;

        for (auto* t : tracks)
            if (laneGroupOf (*t) == nullptr)
                outputs.add (t);

        outputs.addArray (moduleTracks (edit));

        for (auto* t : outputs)
        {
            auto& out = t->getOutput();

            if (out.getDestinationTrack() == nullptr && ! out.outputsToNone())
            {
                parkOutput (edit, *t);

                if (sendTo (*t, mainBus) == nullptr)
                    addSend (*t, mainBus, 1.0);
            }
        }

        // v0: Typ je Spur aus dem alten "hat Synth" (Lane erbt von der
        // Gruppe), Plugins der Gruppenspuren -> Modulkette bis Main
        if (version < 1)
        {
            for (auto* t : tracks)
            {
                auto* group = laneGroupOf (*t);
                t->state.setProperty (typeProp, hasSynth (group != nullptr ? *group : *t) ? "midi" : "audio", nullptr);
            }

            for (auto* t : tracks)
            {
                juce::Array<te::Plugin::Ptr> chain;

                for (auto* p : t->pluginList.getPlugins())
                    if (! isInfrastructure (*p))
                        chain.add (p);

                // Lanes laufen in ihre Gruppe — deren Instrument klingt fuer sie
                if (laneGroupOf (*t) != nullptr)
                {
                    for (auto& p : chain)
                        if (p->isSynth())
                            p->deleteFromParent();

                    continue;
                }

                auto id = trackNodeId (edit, *t);
                auto prev = id;

                for (auto& p : chain)
                {
                    // Das Plugin zieht um (Zustand samt Parametern bleibt) —
                    // chain haelt das Objekt am Leben, der Cache findet es ueber
                    // seinen State wieder
                    auto cls = classOfPlugin (*p);
                    p->removeFromParent();

                    auto mod = addPluginModule (edit, p->state, cls);

                    if (mod.isEmpty())
                        continue;

                    if (connectNodes (edit, prev, mod).isEmpty())
                        prev = mod;
                }

                if (prev != id)
                {
                    connectNodes (edit, prev, "main");
                    disconnectNodes (edit, id, "main");
                }
                else if (isMidiTrack (*t))
                {
                    // MIDI ohne Instrument klang schon vorher nicht
                    disconnectNodes (edit, id, "main");
                }
            }
        }

        // v1: Module ohne Rack bekommen ihren Rack-Wrap (Dry/Wet)
        for (auto* m : moduleTracks (edit))
            if (moduleRack (*m) == nullptr)
                if (auto* p = modulePlugin (*m))
                    wrapInRack (edit, *m, *p);

        edit.state.setProperty (routingVersionProp, routingVersion, nullptr);
    }

    juce::var routingState (te::Edit& edit, bool compact, bool reasons)
    {
        juce::Array<juce::var> sources, plugins, edges, matrix, reasonRows;
        juce::StringArray rows, cols;
        auto tracks = userTracks (edit);

        for (int i = 0; i < tracks.size(); ++i)
        {
            if (laneGroupOf (*tracks[i]) != nullptr)
                continue;

            auto id = "t" + juce::String (i);
            rows.add (id);
            sources.add (resultObj (true, { { "id", id }, { "track", i },
                                            { "name", tracks[i]->getName() },
                                            { "signal", isMidiTrack (*tracks[i]) ? "midi" : "audio" } }));
        }

        for (auto* m : moduleTracks (edit))
        {
            auto id = "p" + m->state.getProperty (moduleProp).toString();
            auto cls = m->state.getProperty (classProp).toString();
            auto* p = modulePlugin (*m);
            juce::String format = "builtin";

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (p))
                format = ep->desc.pluginFormatName;

            rows.add (id);
            cols.add (id);
            auto entry = resultObj (true, { { "id", id },
                                            { "name", p != nullptr ? p->getName() : m->getName() },
                                            { "label", moduleLabel (*m).isEmpty() ? juce::var() : juce::var (moduleLabel (*m)) },
                                            { "class", cls },
                                            { "in", classInput (cls) },
                                            { "out", classOutput (cls) },
                                            { "format", format },
                                            { "wet", cls == "midiFx" ? juce::var() : juce::var (moduleWet (*m)) },
                                            { "link", moduleLink (*m).isEmpty() ? juce::var() : juce::var (moduleLink (*m)) },
                                            { "sidechainInput", hasSidechainInput (*m) },
                                            { "master", (bool) m->state.getProperty (masterProp, false) },
                                            { "sidechain", sidechainOf (edit, *m).isEmpty() ? juce::var() : juce::var (sidechainOf (edit, *m)) },
                                            { "window", p != nullptr && pluginWindowOpen (*p) } });

            // Plugin-Sandbox: nur bei Modulen im Host-Prozess (ohne Schalter fehlt das Feld)
            if (auto sb = p != nullptr ? sandboxInfo (*p) : juce::var(); ! sb.isVoid())
                entry.getDynamicObject()->setProperty ("sandbox", sb);

            // Gruppierung laut Einstellung (pluginHosting), nur wo sie gilt (VST3, LV2, Schalter an)
            if (auto mode = p != nullptr ? pluginHostingMode (*p) : juce::String(); mode.isNotEmpty())
                entry.getDynamicObject()->setProperty ("hosting", mode);

            plugins.add (entry);
        }

        // Gruppen (Submix): Busse wie Main, mit Namen; Zeilen und Spalten
        juce::Array<juce::var> groups;

        for (auto* g : groupTracks (edit))
        {
            auto id = nodeIdOf (edit, *g);
            juce::Array<juce::var> members;

            for (auto i : groupMembers (edit, id, false))
                members.add (i);

            rows.add (id);
            cols.add (id);
            groups.add (resultObj (true, { { "id", id }, { "name", moduleLabel (*g) }, { "members", members } }));
        }

        cols.add ("main");
        auto fbEdges = feedbackEdges (edit);
        auto scEdges = sidechainEdges (edit);

        // Knoten, Ziele und Erreichbarkeit einmal lesen statt je Zelle: die
        // Matrix hat Zeilen x Spalten Zellen (Agent-Projekt mit 28 Modulen:
        // ~1000), und je Zelle lief findRouteNode, eine Tiefensuche ueber den
        // ganzen Graphen und feedbackEdges ueber alle Spuren (~100 ms je state)
        std::map<juce::String, RouteNode> nodeCache;
        std::map<juce::String, juce::StringArray> targetCache;
        std::map<juce::String, std::set<juce::String>> reachCache;

        auto nodeFor = [&] (const juce::String& id) -> const RouteNode&
        {
            auto it = nodeCache.find (id);

            if (it == nodeCache.end())
                it = nodeCache.emplace (id, findRouteNode (edit, id)).first;

            return it->second;
        };

        auto targetsFor = [&] (const juce::String& id) -> const juce::StringArray&
        {
            auto it = targetCache.find (id);

            if (it == targetCache.end())
                it = targetCache.emplace (id, targetsOf (edit, nodeFor (id))).first;

            return it->second;
        };

        // wie reaches: Tiefensuche, Knoten mit Fehler werden nicht verfolgt
        auto reachesCached = [&] (const juce::String& start, const juce::String& target)
        {
            auto it = reachCache.find (start);

            if (it == reachCache.end())
            {
                std::set<juce::String> seen;
                juce::StringArray todo { start };

                while (! todo.isEmpty())
                {
                    auto id = todo[todo.size() - 1];
                    todo.remove (todo.size() - 1);

                    if (! seen.insert (id).second)
                        continue;

                    if (nodeFor (id).error.isEmpty())
                        todo.addArray (targetsFor (id));

                    for (auto& [from, to] : scEdges)
                        if (from == id)
                            todo.add (to);
                }

                it = reachCache.emplace (start, std::move (seen)).first;
            }

            return it->second.count (target) > 0;
        };

        for (auto& from : rows)
        {
            auto fromNode = nodeFor (from);
            auto targets = targetsFor (from);
            juce::Array<juce::var> cells;

            for (auto& to : targets)
                edges.add (resultObj (true, { { "from", from }, { "to", to },
                                              { "level", edgeLevel (edit, from, to) },
                                              { "send", isSendEdge (edit, from, to) } }));

            // reasons (compact): dieselben Gruende je Ziel gruppiert —
            // erlaubt [ids], gesperrt {Grund: [ids]}; bestehende Kanten
            // stehen in edges/feedback/sidechains (Praxistest 1l: die Matrix
            // eines Agent-Projekts hatte 126 000 Zeichen)
            struct Group { juce::Array<juce::var> allowed; juce::NamedValueSet blocked; };
            Group edgeG, fbG, scG;

            auto put = [] (Group& g, bool exists, const juce::String& reason, const juce::String& to)
            {
                if (exists)
                    return;

                if (reason.isEmpty())
                    g.allowed.add (to);
                else if (auto* arr = g.blocked.getVarPointer (juce::Identifier (reason)))
                    arr->append (to);
                else
                    g.blocked.set (juce::Identifier (reason), juce::Array<juce::var> { to });
            };

            for (auto& to : cols)
            {
                if (compact && ! reasons)
                    break;

                const auto& toNode = nodeFor (to);
                auto connected = targets.contains (to);
                auto reason = connected ? juce::String() : edgeErrorWith (fromNode, toNode, reachesCached);
                auto* fb = findFeedback (fbEdges, from, to);
                auto keyed = std::find (scEdges.begin(), scEdges.end(), std::make_pair (from, to)) != scEdges.end();

                if (compact)
                {
                    put (edgeG, connected, reason, to);
                    put (fbG, fb != nullptr, fb != nullptr ? juce::String() : feedbackErrorWith (fbEdges, fromNode, toNode), to);
                    put (scG, keyed, keyed ? juce::String() : sidechainError (edit, fromNode, toNode, reachesCached), to);
                    continue;
                }

                juce::var fbVar;

                if (fb != nullptr)
                    fbVar = resultObj (true, { { "level", roundLevel (fb->ret->levelParam->getCurrentValue()) },
                                               { "delayMs", roundLevel (fb->ret->delayParam->getCurrentValue()) } });

                cells.add (resultObj (true, { { "to", to }, { "connected", connected }, { "reason", reason },
                                              { "level", connected ? edgeLevel (edit, from, to) : 0.0 },
                                              { "feedback", fbVar },
                                              { "feedbackReason", fb != nullptr ? juce::String()
                                                                                : feedbackErrorWith (fbEdges, fromNode, toNode) } }));

                // Sidechain-Kante (Schluessel in den zweiten Eingang)
                auto* cell = cells.getReference (cells.size() - 1).getDynamicObject();
                cell->setProperty ("sidechain", keyed);
                cell->setProperty ("sidechainReason", keyed ? juce::String() : sidechainError (edit, fromNode, toNode, reachesCached));
            }

            if (! compact)
                matrix.add (resultObj (true, { { "from", from }, { "cells", cells } }));
            else if (reasons)
            {
                // leere Listen weglassen
                auto groupVar = [] (const Group& g)
                {
                    auto* o = new juce::DynamicObject();

                    if (! g.allowed.isEmpty())
                        o->setProperty ("allowed", g.allowed);

                    if (! g.blocked.isEmpty())
                    {
                        auto* b = new juce::DynamicObject();

                        for (auto& nv : g.blocked)
                            b->setProperty (nv.name, nv.value);

                        o->setProperty ("blocked", juce::var (b));
                    }

                    return juce::var (o);
                };

                auto row = groupVar (edgeG);
                row.getDynamicObject()->setProperty ("from", from);
                row.getDynamicObject()->setProperty ("feedback", groupVar (fbG));
                row.getDynamicObject()->setProperty ("sidechain", groupVar (scG));
                reasonRows.add (row);
            }
        }

        juce::Array<juce::var> feedback;

        for (auto& e : feedbackEdges (edit))
            if (e.send != nullptr && e.ret != nullptr)
                feedback.add (resultObj (true, { { "from", e.from }, { "to", e.to },
                                                 { "level", roundLevel (e.ret->levelParam->getCurrentValue()) },
                                                 { "delayMs", roundLevel (e.ret->delayParam->getCurrentValue()) },
                                                 { "minDelayMs", roundLevel (e.ret->minDelayMs()) } }));

        juce::Array<juce::var> sidechains;

        for (auto& [from, to] : scEdges)
            sidechains.add (resultObj (true, { { "from", from }, { "to", to } }));

        // Simple-Sicht: je Spur Kette, Sends und was nur Complex darstellt
        juce::Array<juce::var> chains;
        auto graph = allEdges (edit);
        auto chainOwners = sources;

        for (auto& g : groups)   // die eigene Kette einer Gruppe
            chainOwners.add (resultObj (true, { { "id", g.getProperty ("id", {}) }, { "group", g.getProperty ("id", {}) } }));

        for (auto& src : chainOwners)
        {
            auto id = src.getProperty ("id", {}).toString();
            auto c = deriveChain (edit, graph, fbEdges, id);
            juce::Array<juce::var> chainPlugins, sends, complex;

            for (auto& p : c.plugins)
                chainPlugins.add (p);

            for (auto& to : c.sends)
                sends.add (resultObj (true, { { "to", to }, { "level", edgeLevel (edit, id, to) } }));

            for (auto& r : c.complex)
                complex.add (r);

            juce::Array<juce::var> postSends;

            for (auto& to : c.postSends)
            {
                const auto from = c.plugins[c.plugins.size() - 1];
                postSends.add (resultObj (true, { { "from", from }, { "to", to }, { "level", edgeLevel (edit, from, to) } }));
            }

            auto chain = resultObj (true, { { "id", id }, { "track", src.getProperty ("track", {}) },
                                            { "plugins", chainPlugins }, { "toMain", c.toMain },
                                            { "sends", sends }, { "postSends", postSends }, { "complex", complex } });

            if (c.toMaster.isNotEmpty())
                chain.getDynamicObject()->setProperty ("toMaster", c.toMaster);

            if (c.toGroup.isNotEmpty())
                chain.getDynamicObject()->setProperty ("toGroup", c.toGroup);

            if (src.hasProperty ("group"))
            {
                chain.getDynamicObject()->removeProperty ("track");
                chain.getDynamicObject()->setProperty ("group", id);
            }

            chains.add (chain);
        }

        auto r = resultObj (true, { { "sources", sources }, { "plugins", plugins }, { "groups", groups },
                                    { "edges", edges }, { "feedback", feedback }, { "sidechains", sidechains },
                                    { "matrix", matrix },
                                    { "chains", chains } });

        if (compact)
            r.getDynamicObject()->removeProperty ("matrix");

        if (compact && reasons)
            r.getDynamicObject()->setProperty ("reasons", reasonRows);

        return r;
    }
}
