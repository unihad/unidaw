#include "Sidechain.h"

#include <algorithm>

namespace dawd
{
    namespace
    {
        const juce::Identifier roleProp ("dawdRole");
        const juce::Identifier moduleProp ("dawdModule");

        juce::String nodeIdOf (te::Edit& edit, te::AudioTrack& t)
        {
            if (t.state.getProperty (roleProp).toString() == "module")
                return "p" + t.state.getProperty (moduleProp).toString();

            if (t.state.getProperty (roleProp).toString() == "group")   // Gruppen-Bus als Schluessel-Quelle
                return "g" + t.state.getProperty (moduleProp).toString();

            return trackNodeId (edit, t);
        }

        SidechainInPlugin* sidechainIn (te::AudioTrack& t)
        {
            for (auto* p : t.pluginList.getPlugins())
                if (auto* s = dynamic_cast<SidechainInPlugin*> (p))
                    return s;

            return nullptr;
        }

        SidechainOutPlugin* sidechainOut (te::AudioTrack& t)
        {
            for (auto* p : t.pluginList.getPlugins())
                if (auto* s = dynamic_cast<SidechainOutPlugin*> (p))
                    return s;

            return nullptr;
        }

        te::AudioTrack* sourceTrack (te::Edit& edit, SidechainInPlugin& in)
        {
            return dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, in.getSidechainSourceID()));
        }

        // Rack-Pins (1-basiert), die im Wrapper-Rack auf die Sidechain-Pins
        // des Plugins gehen — hoechstens zwei (Stereo-Schluessel)
        juce::Array<int> keyPins (te::AudioTrack& t)
        {
            juce::Array<int> pins;
            auto* rack = moduleRackOf (t);
            auto* plugin = modulePlugin (t);

            if (rack == nullptr || rack->type == nullptr || plugin == nullptr)
                return pins;

            // Hauptbus: bei VST3/LV2 der erste Eingangsbus, sonst so viele
            // Kanaele, wie das Plugin ausgibt (Compressor: L, R + Trigger)
            juce::StringArray ins, outs;
            plugin->getChannelNames (&ins, &outs);
            int mainIns = outs.size();

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (plugin))
            {
                auto* pi = ep->getAudioPluginInstance();

                if (pi == nullptr || pi->getBusCount (true) < 2)
                    return pins;

                mainIns = pi->getChannelCountOfBus (true, 0);
            }

            for (auto* c : rack->type->getConnections())
                if (! c->sourceID.get().isValid() && c->destID.get() == plugin->itemID
                    && c->destPin.get() > mainIns && c->sourcePin.get() > 0 && pins.size() < 2)
                    pins.addIfNotAlreadyThere (c->sourcePin.get());

            pins.sort();
            return pins;
        }

        bool reachesWithKeys (te::Edit& edit, const juce::String& from, const juce::String& target)
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
                    todo.addArray (sidechainTargetsOf (edit, id));
                }
            }

            return false;
        }

        // Rack auf zwei Kanaele zurueck, Plugin-Paar raus
        void removeKey (te::AudioTrack& t)
        {
            if (auto* rack = moduleRackOf (t))
            {
                rack->setInputMapping (2, -1);
                rack->setInputMapping (3, -1);
                rack->setNumInputChannels (2);
            }

            if (auto* in = sidechainIn (t))
                in->deleteFromParent();

            if (auto* out = sidechainOut (t))
                out->deleteFromParent();

            // Der eingebaute Compressor hoert ohne Schluessel wieder auf sich
            if (auto* comp = dynamic_cast<te::CompressorPlugin*> (modulePlugin (t)))
                comp->useSidechainTrigger = false;
        }
    }

    //==============================================================================
    const char* SidechainInPlugin::xmlTypeName = "dawdSidechainIn";
    const char* SidechainOutPlugin::xmlTypeName = "dawdSidechainOut";

    SidechainInPlugin::SidechainInPlugin (te::PluginCreationInfo info) : te::Plugin (info) {}
    SidechainInPlugin::~SidechainInPlugin() { notifyListenersOfDeletion(); }

    SidechainOutPlugin::SidechainOutPlugin (te::PluginCreationInfo info) : te::Plugin (info) {}
    SidechainOutPlugin::~SidechainOutPlugin() { notifyListenersOfDeletion(); }

    // Vier Eingaenge: sonst haengt die Engine die Kanaele 3/4 unbearbeitet
    // an (Passthrough) und das Leeren kaeme nicht an
    te::Plugin::BusLayout SidechainOutPlugin::getBusses() const
    {
        return BusLayout::singleInOut (te::ChannelConfiguration::discreteChannels (4),
                                       te::ChannelConfiguration::discreteChannels (4));
    }

    void SidechainOutPlugin::applyToBuffer (const te::PluginRenderContext& fc)
    {
        if (fc.destBuffer == nullptr)
            return;

        for (int c = 2; c < fc.destBuffer->getNumChannels(); ++c)
            fc.destBuffer->clear (c, fc.bufferStartSample, fc.bufferNumSamples);
    }

    void registerSidechainPlugins (te::Engine& engine)
    {
        engine.getPluginManager().createBuiltInType<SidechainInPlugin>();
        engine.getPluginManager().createBuiltInType<SidechainOutPlugin>();
    }

    //==============================================================================
    juce::String sidechainOf (te::Edit& edit, te::AudioTrack& t)
    {
        if (auto* in = sidechainIn (t))
            if (auto* src = sourceTrack (edit, *in))
                return nodeIdOf (edit, *src);

        return {};
    }

    bool hasSidechainInput (te::AudioTrack& t)
    {
        return ! keyPins (t).isEmpty();
    }

    std::vector<std::pair<juce::String, juce::String>> sidechainEdges (te::Edit& edit)
    {
        std::vector<std::pair<juce::String, juce::String>> edges;

        for (auto& id : moduleIds (edit))
            if (auto* t = findModuleTrack (edit, id))
                if (auto from = sidechainOf (edit, *t); from.isNotEmpty())
                    edges.emplace_back (from, id);

        std::sort (edges.begin(), edges.end());
        return edges;
    }

    juce::StringArray sidechainTargetsOf (te::Edit& edit, const juce::String& id)
    {
        juce::StringArray targets;

        for (auto& e : sidechainEdges (edit))
            if (e.first == id)
                targets.add (e.second);

        return targets;
    }

    juce::String sidechainError (te::Edit& edit, const RouteNode& from, const RouteNode& to,
                                 const std::function<bool (const juce::String&, const juce::String&)>& reachesFn)
    {
        if (from.error.isNotEmpty())
            return from.error;

        if (to.error.isNotEmpty())
            return to.error;

        if (from.kind == "main")
            return "main has no output";

        if (to.kind != "plugin")
            return "sidechain goes into a plugin";

        if (from.out != "audio" || to.in != "audio")
            return "sidechain is audio only";

        if (! hasSidechainInput (*to.track))
            return "plugin has no sidechain input";

        if (auto cur = sidechainOf (edit, *to.track); cur.isNotEmpty())
            return cur == from.id ? "already connected" : "sidechain already taken by " + cur;

        if (from.id == to.id || (reachesFn ? reachesFn (to.id, from.id) : reachesWithKeys (edit, to.id, from.id)))
            return "would create a loop";

        return {};
    }

    juce::String connectSidechain (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (auto err = sidechainError (edit, from, to); err.isNotEmpty())
            return err;

        auto& t = *to.track;
        auto* rack = moduleRackOf (t);
        auto pins = keyPins (t);

        // Reste einer Kante, deren Quelle weg ist
        removeKey (t);

        juce::ValueTree inState (te::IDs::PLUGIN);
        inState.setProperty (te::IDs::type, SidechainInPlugin::xmlTypeName, nullptr);

        juce::ValueTree outState (te::IDs::PLUGIN);
        outState.setProperty (te::IDs::type, SidechainOutPlugin::xmlTypeName, nullptr);

        // In direkt vor das Rack (hinter AuxReturn und Feedback-Return),
        // Out direkt dahinter
        auto rackIndex = t.pluginList.indexOf (rack);
        auto inPlugin = t.pluginList.insertPlugin (inState, rackIndex);
        t.pluginList.insertPlugin (outState, rackIndex + 2);

        if (inPlugin == nullptr)
            return "sidechain failed";

        // Kanaele 1/2 bleiben das Signal des Moduls, 3/4 sind der Schluessel
        inPlugin->setSidechainSourceID (from.track->itemID);

        for (int c = 0; c < 4; ++c)
            inPlugin->makeConnection (c, c, nullptr);

        rack->setNumInputChannels (4);
        rack->setInputMapping (2, pins[0]);
        rack->setInputMapping (3, pins.size() > 1 ? pins[1] : -1);

        if (auto* comp = dynamic_cast<te::CompressorPlugin*> (modulePlugin (t)))
            comp->useSidechainTrigger = true;

        return {};
    }

    juce::String disconnectSidechain (te::Edit& edit, const juce::String& fromId, const juce::String& toId)
    {
        auto from = findRouteNode (edit, fromId);
        auto to = findRouteNode (edit, toId);

        if (from.error.isNotEmpty())
            return from.error;

        if (to.error.isNotEmpty())
            return to.error;

        if (to.kind != "plugin" || sidechainOf (edit, *to.track) != from.id)
            return "not connected";

        removeKey (*to.track);
        return {};
    }

    void pruneSidechains (te::Edit& edit)
    {
        for (auto& id : moduleIds (edit))
            if (auto* t = findModuleTrack (edit, id))
                if (sidechainIn (*t) != nullptr && sidechainOf (edit, *t).isEmpty())
                    removeKey (*t);
    }
}
