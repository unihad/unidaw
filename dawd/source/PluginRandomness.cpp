#include "PluginRandomness.h"

namespace dawd
{
    namespace
    {
        // Patch-XML im Zustand von Surge XT (saveRaw: Kopf "sub3", XML,
        // Wavetables); nullptr ohne
        std::unique_ptr<juce::XmlElement> surgePatch (te::ExternalPlugin& ep)
        {
            auto* pi = ep.getAudioPluginInstance();

            if (pi == nullptr)
                return {};

            juce::MemoryBlock mb;
            pi->getStateInformation (mb);

            // VST3 ueber JUCE: Binaer-XML <VST3PluginState><IComponent>Base64
            // des Plugin-Zustands</IComponent>
            if (auto outer = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()); outer != nullptr && outer->hasTagName ("VST3PluginState"))
                if (auto* comp = outer->getChildByName ("IComponent"))
                {
                    juce::MemoryBlock inner;

                    if (inner.fromBase64Encoding (comp->getAllSubText().trim()))
                        mb = inner;
                }

            // Kopf und Wavetables sind binaer (Nullbytes): Bytes durchsuchen
            const std::string bytes (static_cast<const char*> (mb.getData()), mb.getSize());
            const auto a = bytes.find ("<?xml"), b = a == std::string::npos ? a : bytes.find ("</patch>", a);

            if (b == std::string::npos)
                return {};

            return juce::XmlDocument::parse (juce::String::fromUTF8 (bytes.data() + a, (int) (b + 8 - a)));
        }
    }

    // Quellen in Surge XT 1.3.4 (Tag release_xt_1.3.4): Startphase je Note
    // ohne Retrigger (ClassicOscillator.cpp:238 u. a.), String/S&H immer
    // (StringOscillator.cpp:144), Drift ueber ::rand()
    // (OscillatorCommonFunctions.h), Noise (SurgeVoice.cpp:1219), LFO-Formen
    // Noise/S&H und Trigger Random (LFOModulationSource.cpp:93, 268), Random-
    // Modquellen (ModulationSource.h:638). Zaehlt nur hoerbare Oszillatoren
    // (Solo vor Mute, Ringmodulation nimmt beide mit) und nur LFOs, die
    // etwas modulieren.
    juce::StringArray pluginRandomSources (te::Plugin& p)
    {
        juce::StringArray out;
        auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

        if (ep == nullptr || ! ep->getName().startsWith ("Surge XT") || ep->getName().contains ("Effects"))
            return out;

        auto patch = surgePatch (*ep);
        auto* params = patch != nullptr ? patch->getChildByName ("parameters") : nullptr;

        if (params == nullptr)
            return out;

        auto el = [&] (const juce::String& n) { return params->getChildByName (n); };
        auto val = [&] (const juce::String& n) { auto* e = el (n); return e != nullptr ? e->getStringAttribute ("value") : juce::String(); };
        auto on = [&] (const juce::String& n) { return val (n).getIntValue() != 0; };

        // Modquellen, die etwas modulieren (modrouting source="N")
        std::set<int> sources;

        for (auto* e : params->getChildIterator())
            for (auto* m : e->getChildWithTagNameIterator ("modrouting"))
                sources.insert (m->getIntAttribute ("source", -1));

        // ot_*: 0 Classic, 1 Sine, 2 Wavetable, 3 S&H Noise, 5 FM3, 6 FM2,
        // 7 Window, 8 Modern, 9 String, 11 Alias
        static const std::set<int> phaseRandom { 0, 1, 2, 5, 6, 7, 8, 11 }, alwaysRandom { 3, 9 };
        static const char* typeNames[] = { "Classic", "Sine", "Wavetable", "S&H Noise", "Audio In", "FM3", "FM2", "Window", "Modern", "String", "Twist", "Alias" };

        juce::StringArray scenes { "a" };

        if (val ("scenemode").getIntValue() != 0 || val ("scene_active").getIntValue() == 1)
            scenes = { "a", "b" };

        for (auto& s : scenes)
        {
            const auto S = s.toUpperCase();
            const bool anySolo = on (s + "_solo_o1") || on (s + "_solo_o2") || on (s + "_solo_o3")
                                 || on (s + "_solo_ring12") || on (s + "_solo_ring23") || on (s + "_solo_noise");
            auto audible = [&] (const juce::String& k)
            {
                return anySolo ? on (s + "_solo_" + k) : (! on (s + "_mute_" + k) && val (s + "_level_" + k).getFloatValue() > 0.0f);
            };

            std::set<int> oscs;

            for (int i = 1; i <= 3; ++i)
                if (audible ("o" + juce::String (i)))
                    oscs.insert (i);

            if (audible ("ring12")) { oscs.insert (1); oscs.insert (2); }
            if (audible ("ring23")) { oscs.insert (2); oscs.insert (3); }

            for (auto i : oscs)
            {
                const auto o = s + "_osc" + juce::String (i);
                const int type = val (o + "_type").getIntValue();
                const auto name = S + " Osc " + juce::String (i) + " " + (type >= 0 && type < 12 ? typeNames[type] : "?");

                if (alwaysRandom.count (type) > 0)
                    out.add ("random excitation (" + name + ", also with Retrigger on)");
                else if (phaseRandom.count (type) > 0 && ! on (o + "_retrigger"))
                    out.add ("random start phase (" + name + ", Retrigger off)");
            }

            if (auto* drift = el (s + "_drift"); drift != nullptr && (drift->getDoubleAttribute ("value") != 0.0 || drift->getChildByName ("modrouting") != nullptr))
                out.add (S + " Osc Drift");

            if (audible ("noise"))
                out.add (S + " Noise");

            // lfo0..5 Voice-LFOs (Quelle 17..22), lfo6..11 Scene-LFOs (23..28);
            // Form 4 Noise, 5 S&H; Trigger 2 Random
            for (int k = 0; k < 12; ++k)
            {
                const auto l = s + "_lfo" + juce::String (k);
                const int shape = val (l + "_shape").getIntValue();

                if ((shape == 4 || shape == 5 || val (l + "_trigmode").getIntValue() == 2) && sources.count (17 + k) > 0)
                    out.add (S + (k < 6 ? " LFO " + juce::String (k + 1) : " Scene LFO " + juce::String (k - 5))
                             + (shape == 4 ? " Noise" : shape == 5 ? " S&H" : " random trigger"));
            }
        }

        // ms_random_bipolar / ms_random_unipolar
        if (sources.count (31) > 0 || sources.count (32) > 0)
            out.add ("Random modulation source");

        return out;
    }

    juce::String rendersVaryHint (te::Plugin& p)
    {
        auto src = pluginRandomSources (p);

        if (src.isEmpty())
            return {};

        return "renders vary: " + p.getName() + " plays random parts (" + src.joinIntoString (", ")
               + juce::String::fromUTF8 (") — each render sounds a bit different, so small distance changes can come from chance, not from a parameter. Retrigger on and Drift 0 make the oscillators repeatable (that changes the sound)");
    }
}
