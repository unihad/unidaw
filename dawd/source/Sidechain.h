#pragma once

#include "Routing.h"

#include <functional>

// Sidechain-Kanten: ein Modul hoert an seinem zweiten Eingang (Sidechain,
// Schluessel) ein anderes Signal — z. B. Ducking: der Kompressor auf dem
// Bass regelt nach der Kick. Die Engine kann Sidechain nur fuer Plugins
// direkt auf einer Spur (Plugin::sidechainSourceID: Send hinter der Kette
// der Quell-Spur, Return in den Eingang des Plugins), nicht im Rack. Das
// Modul-Plugin sitzt im Rack, deshalb traegt ein eigenes Plugin-Paar auf der
// Modul-Spur den Schluessel:
//
//   SidechainIn   vor dem Rack — Quelle (sidechainSourceID) auf die Kanaele
//                 3/4; die RackInstance nimmt vier Kanaele und gibt 3/4 auf
//                 die Sidechain-Pins des Plugins (die das Wrapper-Rack schon
//                 mit dem Plugin verbunden hat)
//   SidechainOut  hinter dem Rack — leert die Kanaele 3/4 (der Dry-Anteil
//                 des Racks traegt sonst den Schluessel weiter)
//
// Ohne Verzoegerung (die Engine ordnet den Graphen); die Kante zaehlt fuer
// den Schleifen-Check. Ein Schluessel je Modul.
namespace dawd
{
    class SidechainInPlugin : public te::Plugin
    {
    public:
        explicit SidechainInPlugin (te::PluginCreationInfo);
        ~SidechainInPlugin() override;

        static const char* xmlTypeName;

        juce::String getName() const override                        { return "Sidechain In"; }
        juce::String getPluginType() override                        { return xmlTypeName; }
        juce::String getSelectableDescription() override             { return getName(); }
        int getNumOutputChannelsGivenInputs (int numInputs) override { return juce::jmax (4, numInputs); }
        BusLayout getBusses() const override                         { return BusLayout::singleStereoInOut(); }
        bool takesMidiInput() override                               { return true; }
        bool canBeAddedToClip() override                             { return false; }
        bool canBeAddedToRack() override                             { return false; }

        void initialise (const te::PluginInitialisationInfo&) override {}
        void deinitialise() override {}
        void applyToBuffer (const te::PluginRenderContext&) override {}
    };

    class SidechainOutPlugin : public te::Plugin
    {
    public:
        explicit SidechainOutPlugin (te::PluginCreationInfo);
        ~SidechainOutPlugin() override;

        static const char* xmlTypeName;

        juce::String getName() const override                        { return "Sidechain Out"; }
        juce::String getPluginType() override                        { return xmlTypeName; }
        juce::String getSelectableDescription() override             { return getName(); }
        int getNumOutputChannelsGivenInputs (int numInputs) override { return juce::jmax (4, numInputs); }
        BusLayout getBusses() const override;
        bool takesMidiInput() override                               { return true; }
        bool canBeAddedToClip() override                             { return false; }
        bool canBeAddedToRack() override                             { return false; }

        void initialise (const te::PluginInitialisationInfo&) override {}
        void deinitialise() override {}
        void applyToBuffer (const te::PluginRenderContext&) override;
    };

    // Beim Engine-Start registrieren (vor dem ersten Laden eines Edits).
    void registerSidechainPlugins (te::Engine&);

    // Grund, warum from -> to keine Sidechain-Kante sein kann ("" = geht).
    // Die Texte sind API-Vertrag.
    // reachesFn (a, b): erreicht a den Knoten b (normale und Sidechain-
    // Kanten)? Leer = selbst suchen; routingState gibt seinen Cache mit.
    juce::String sidechainError (te::Edit&, const RouteNode& from, const RouteNode& to,
                                 const std::function<bool (const juce::String&, const juce::String&)>& reachesFn = {});

    // Kante anlegen/entfernen — Rueckgabe: Fehlertext oder "".
    juce::String connectSidechain (te::Edit&, const juce::String& from, const juce::String& to);
    juce::String disconnectSidechain (te::Edit&, const juce::String& from, const juce::String& to);

    // Quelle des Schluessels eines Moduls ("t0", "p3") oder "".
    juce::String sidechainOf (te::Edit&, te::AudioTrack& moduleTrack);

    // Hat das Modul-Plugin einen Sidechain-Eingang (Pins im Rack)?
    bool hasSidechainInput (te::AudioTrack& moduleTrack);

    // Alle Sidechain-Kanten (from, to), sortiert wie die Feedback-Kanten.
    std::vector<std::pair<juce::String, juce::String>> sidechainEdges (te::Edit&);

    // Ziele, die id als Schluessel speist — fuer den Schleifen-Check.
    juce::StringArray sidechainTargetsOf (te::Edit&, const juce::String& id);

    // Halbe Kanten (Quelle weg) raeumen — beim Laden.
    void pruneSidechains (te::Edit&);
}
