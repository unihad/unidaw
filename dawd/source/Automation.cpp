#include "Automation.h"
#include "PluginSettings.h"
#include "Routing.h"
#include "Feedback.h"

namespace dawd
{
    namespace
    {
        constexpr double minDb = -100.0, maxDb = 6.0;

        double round4 (double v)  { return std::round (v * 10000.0) / 10000.0; }

        // Nicht-ASCII-Literale ausdruecklich als UTF-8 (sonst Latin-1)
        const juce::String dot = juce::String::fromUTF8 (" · ");
        const juce::String arrow = juce::String::fromUTF8 (" → ");
        const juce::String loop = juce::String::fromUTF8 (" ↺ ");

        float levelToFader (double level)
        {
            return te::decibelsToVolumeFaderPosition ((float) juce::Decibels::gainToDecibels (level, minDb));
        }

        double faderToLevel (float pos)
        {
            auto db = (double) te::volumeFaderPositionToDB (pos);
            return db <= minDb ? 0.0 : juce::Decibels::decibelsToGain (db, minDb);
        }

        AutomationTarget fail (const juce::String& id, const juce::String& error)
        {
            AutomationTarget t;
            t.id = id;
            t.error = error;
            return t;
        }

        AutomationTarget::Binding identity (te::AutomatableParameter* p)
        {
            return { p, [] (double v) { return (float) v; } };
        }

        AutomationTarget::Binding normalised (te::AutomatableParameter* p)
        {
            return { p, [p] (double v) { return p->valueRange.convertFrom0to1 ((float) v); } };
        }

        // Volume/Pan eines Faders (Spur oder Gruppe)
        AutomationTarget faderTarget (const juce::String& id, const juce::String& name, te::VolumeAndPanPlugin& vol,
                                      const juce::String& key, const juce::String& what)
        {
            AutomationTarget t;
            t.id = id;

            if (key == "volume")
            {
                t.label = name + dot + "Volume";
                t.unit = "dB";
                t.min = minDb;
                t.max = maxDb;
                t.bindings.push_back ({ vol.volParam.get(), [] (double db) { return te::decibelsToVolumeFaderPosition ((float) db); } });
                t.fromParam = [] (float pos) { return juce::jmax (minDb, (double) te::volumeFaderPositionToDB (pos)); };
                return t;
            }

            if (key == "pan")
            {
                t.label = name + dot + "Pan";
                t.min = -1.0;
                t.max = 1.0;
                t.bindings.push_back (identity (vol.panParam.get()));
                t.fromParam = [] (float v) { return (double) v; };
                return t;
            }

            return fail (id, "unknown " + what + " parameter: " + key + " (volume, pan)");
        }

        AutomationTarget trackTarget (te::Edit& edit, const juce::String& id, const juce::String& node, const juce::String& key)
        {
            auto num = node.substring (1);
            auto tracks = userTracks (edit);

            if (! num.containsOnly ("0123456789") || num.isEmpty() || num.getIntValue() >= tracks.size())
                return fail (id, "no such track: " + node);

            auto* track = tracks[num.getIntValue()];
            // MIDI-Spuren: Volume/Pan am Ausgang ihres Instruments
            auto* faderTrack = faderTrackOf (edit, *track);

            if (faderTrack == nullptr)
                return fail (id, "MIDI track " + node + " is not routed through an instrument");

            auto* vol = faderTrack->getVolumePlugin();

            if (vol == nullptr)
                return fail (id, "track has no volume");

            return faderTarget (id, track->getName(), *vol, key, "track");
        }

        // Gruppe g<N>: der Fader hinter ihrer eigenen Kette (groupFaderOf),
        // wandert mit ihr wie bei Spuren (syncFaders)
        AutomationTarget groupTarget (te::Edit& edit, const juce::String& id, const juce::String& node, const juce::String& key)
        {
            auto* group = groupIdFor (edit, node) == node ? findGroupTrack (edit, node) : nullptr;

            if (group == nullptr)
                return fail (id, "no such group: " + node);

            auto* fader = groupFaderOf (edit, *group);
            auto* vol = fader != nullptr ? fader->getVolumePlugin() : nullptr;

            if (vol == nullptr)
                return fail (id, "group has no volume");

            return faderTarget (id, moduleLabel (*group), *vol, key, "group");
        }

        // known: der Parameter zu key, schon aufgeloest (listAutomationTargets) —
        // sonst sucht findParameter ueber alle Namen, je Ziel ein Lauf
        AutomationTarget moduleTarget (te::Edit& edit, const juce::String& id, const juce::String& node, const juce::String& key,
                                       te::AutomatableParameter* known = nullptr)
        {
            auto* track = findModuleTrack (edit, node);

            if (track == nullptr)
                return fail (id, "no such plugin: " + node);

            auto* plugin = modulePlugin (*track);

            if (plugin == nullptr)
                return fail (id, "no such plugin: " + node);

            AutomationTarget t;
            t.id = id;

            if (key == "wet")
            {
                if (findRouteNode (edit, node).cls == "midiFx")
                    return fail (id, "midi plugins have no wet/dry");

                auto* rack = moduleRackOf (*track);

                if (rack == nullptr)
                    return fail (id, "plugin has no rack");

                // Ein Knopf wie setWet: der Rest geht trocken weiter
                t.label = plugin->getName() + dot + "Wet";
                t.bindings.push_back (identity (rack->wetGain.get()));
                t.bindings.push_back ({ rack->dryGain.get(), [] (double v) { return (float) (1.0 - v); } });
                t.fromParam = [] (float v) { return (double) v; };
                return t;
            }

            auto* found = known != nullptr ? known : findParameter (*plugin, key);

            if (found == nullptr)
                return fail (id, "no such parameter: " + key);

            t.label = plugin->getName() + dot + key;
            t.bindings.push_back (normalised (found));
            t.fromParam = [found] (float v) { return (double) found->valueRange.convertTo0to1 (v); };
            t.fromReal = [found] (double real, double& value)
            {
                float n = 0.0f;
                auto err = valueForReal (*found, real, n);
                value = n;
                return err;
            };

            // Verlinkte Instanzen folgen (gleicher Parametername)
            for (auto* m : linkedModules (edit, node))
                if (auto* lp = modulePlugin (*m))
                    if (auto* q = findParameter (*lp, key))
                        t.bindings.push_back (normalised (q));

            return t;
        }

        AutomationTarget edgeTarget (te::Edit& edit, const juce::String& id, const juce::String& from, const juce::String& to)
        {
            auto* send = edgeSend (edit, from, to);

            if (send == nullptr)
                return fail (id, "not connected");

            AutomationTarget t;
            t.id = id;
            t.label = from + arrow + to + dot + "Level";
            t.bindings.push_back ({ send->gain.get(), [] (double v) { return levelToFader (v); } });
            t.fromParam = [] (float pos) { return faderToLevel (pos); };
            return t;
        }

        AutomationTarget feedbackTarget (te::Edit& edit, const juce::String& id, const juce::String& from,
                                         const juce::String& to, const juce::String& key)
        {
            auto* ret = feedbackReturn (edit, from, to);

            if (ret == nullptr)
                return fail (id, "not connected");

            AutomationTarget t;
            t.id = id;
            t.fromParam = [] (float v) { return (double) v; };

            if (key == "level")
            {
                t.label = from + loop + to + dot + "Feedback";
                t.bindings.push_back (identity (ret->levelParam.get()));
                return t;
            }

            if (key == "delayMs")
            {
                t.label = from + loop + to + dot + "Delay";
                t.unit = "ms";
                t.max = maxFeedbackDelayMs;
                t.bindings.push_back (identity (ret->delayParam.get()));
                return t;
            }

            return fail (id, "unknown feedback parameter: " + key + " (level, delayMs)");
        }

        // Rohpunkte der Kurve: Zeit (s), Parameterwert
        struct RawPoint { double time; float value; };

        // ---- Modifier (LFO, Step) --------------------------------------

        const juce::StringArray typeNames { "lfo", "step" };

        const juce::StringArray waveNames { "sine", "triangle", "sawUp", "sawDown", "square", "steps4Up",
                                            "steps4Down", "steps8Up", "steps8Down", "random", "noise" };

        // Notenwerte in der Reihenfolge von te::ModifierCommon::RateType (ab fourBars)
        const juce::StringArray rateNames { "4 bars", "2 bars", "1 bar", "1/2T", "1/2", "1/2D", "1/4T", "1/4", "1/4D",
                                            "1/8T", "1/8", "1/8D", "1/16T", "1/16", "1/16D", "1/32T", "1/32", "1/32D",
                                            "1/64T", "1/64", "1/64D" };

        int rateTypeOf (const juce::String& text)   // RateType oder -1
        {
            auto key = text.removeCharacters (" ").toLowerCase();

            for (int i = 0; i < rateNames.size(); ++i)
                if (rateNames[i].removeCharacters (" ").toLowerCase() == key)
                    return i + (int) te::ModifierCommon::fourBars;

            return -1;
        }

        bool isNumber (const juce::var& v)   { return v.isDouble() || v.isInt() || v.isInt64(); }

        // Schritte des Step-Modifiers: 2..64 Werte -1..1 (Grenzen der Engine)
        bool validSteps (const juce::var& v)
        {
            auto* arr = v.getArray();

            if (arr == nullptr || arr->size() < 2 || arr->size() > te::StepModifier::maxNumSteps)
                return false;

            for (auto& x : *arr)
                if (! isNumber (x) || (double) x < -1.0 || (double) x > 1.0)
                    return false;

            return true;
        }

        // LFO oder Step der Engine (andere Typen fasst dawd nicht an)
        te::Modifier* sourceOf (te::Edit& edit, const juce::ValueTree& assignment)
        {
            auto* m = te::findModifierForID (edit, te::EditItemID::fromProperty (assignment, te::IDs::source)).get();
            return dynamic_cast<te::LFOModifier*> (m) != nullptr || dynamic_cast<te::StepModifier*> (m) != nullptr ? m : nullptr;
        }

        juce::String typeOf (te::Modifier& m)
        {
            return dynamic_cast<te::StepModifier*> (&m) != nullptr ? "step" : "lfo";
        }

        // Zuordnung liegt im Plugin-State — ohne die Quellen-Liste der
        // Engine anzulegen (die kostet je Parameter; Surge hat 778). Eine
        // Zuordnung, deren Modifier weg ist (Modul einer verlinkten Instanz
        // geloescht), zaehlt nicht.
        juce::ValueTree assignmentState (te::AutomatableParameter& p)
        {
            if (auto* plugin = p.getPlugin())
                for (auto c : plugin->state.getChildWithName (te::IDs::MODIFIERASSIGNMENTS))
                    if ((c.hasType (te::IDs::LFO) || c.hasType (te::IDs::STEP)) && c[te::IDs::paramID].toString() == p.paramID
                        && sourceOf (p.getEdit(), c) != nullptr)
                        return c;

            return {};
        }

        te::Modifier* modifierOf (te::AutomatableParameter& p)
        {
            auto a = assignmentState (p);
            return a.isValid() ? sourceOf (p.getEdit(), a) : nullptr;
        }

        // Spur, deren ModifierList den Modifier traegt: die des Plugins, bei
        // einem Plugin im Rack eines Moduls die Modul-Spur
        te::AudioTrack* modifierTrackFor (te::Edit& edit, te::AutomatableParameter& p)
        {
            auto* plugin = p.getPlugin();

            if (plugin == nullptr)
                return nullptr;

            if (auto* t = dynamic_cast<te::AudioTrack*> (plugin->getOwnerTrack()))
                return t;

            if (auto rack = plugin->getOwnerRackType())
                for (auto* t : te::getAudioTracks (edit))
                    if (auto* ri = moduleRackOf (*t); ri != nullptr && ri->type == rack)
                        return t;

            return nullptr;
        }

        // dry (= 1 - wet) laeuft gegen den Regler: Vorzeichen der Bindung
        float signOf (const AutomationTarget::Binding& b, const AutomationTarget& t)
        {
            auto& r = b.param->valueRange;
            return r.convertTo0to1 (b.toParam (t.max)) >= r.convertTo0to1 (b.toParam (t.min)) ? 1.0f : -1.0f;
        }

        void assign (te::AutomatableParameter& p, te::Modifier& m, float value)
        {
            for (auto a : p.getAssignments())
                if (a->isForModifierSource (m))
                {
                    a->value = value;
                    return;
                }

            p.addModifier (m, value);
        }

        void removeModifier (te::Edit& edit, te::Modifier& m)
        {
            // Zuordnungen aller Plugins (auch verlinkter Instanzen), dann der Modifier
            for (auto* plugin : te::getAllPlugins (edit, true))
            {
                auto list = plugin->state.getChildWithName (te::IDs::MODIFIERASSIGNMENTS);

                for (int i = list.getNumChildren(); --i >= 0;)
                    if (te::EditItemID::fromProperty (list.getChild (i), te::IDs::source) == m.itemID)
                        list.removeChild (i, nullptr);
            }

            m.state.getParent().removeChild (m.state, nullptr);
        }

        te::Modifier* createModifier (te::Edit& edit, te::AutomatableParameter& primary, const juce::String& type)
        {
            auto* track = modifierTrackFor (edit, primary);

            if (track == nullptr || track->getModifierList() == nullptr)
                return nullptr;

            auto* m = track->getModifierList()->insertModifier (juce::ValueTree (type == "step" ? te::IDs::STEP : te::IDs::LFO), -1, nullptr).get();

            // Am Transport (Phase/Schritt aus der Songzeit), Tiefe in der Zuordnung
            if (auto* lfo = dynamic_cast<te::LFOModifier*> (m))
            {
                // -1..1
                lfo->syncTypeParam->setParameter ((float) te::ModifierCommon::transport, juce::sendNotification);
                lfo->bipolarParam->setParameter (1.0f, juce::sendNotification);
                lfo->depthParam->setParameter (1.0f, juce::sendNotification);
                lfo->offsetParam->setParameter (0.0f, juce::sendNotification);
                lfo->waveParam->setParameter ((float) te::LFOModifier::waveSine, juce::sendNotification);
                lfo->rateTypeParam->setParameter ((float) te::ModifierCommon::quarter, juce::sendNotification);
                lfo->rateParam->setParameter (1.0f, juce::sendNotification);
                lfo->phaseParam->setParameter (0.0f, juce::sendNotification);
                return lfo;
            }

            if (auto* step = dynamic_cast<te::StepModifier*> (m))
            {
                // Schritte -1..1 unveraendert, ein Schritt je 1/16
                step->syncTypeParam->setParameter ((float) te::ModifierCommon::transport, juce::sendNotification);
                step->depthParam->setParameter (1.0f, juce::sendNotification);
                step->rateTypeParam->setParameter ((float) te::ModifierCommon::sixteenth, juce::sendNotification);
                step->rateParam->setParameter (1.0f, juce::sendNotification);
                return step;
            }

            return nullptr;
        }

        // Rate: Notenwert (am Tempo) oder Hz — LFO: eine Periode, Step: ein Schritt
        void applyRate (te::AutomatableParameter& rateType, te::AutomatableParameter& rateParam, const juce::var& rate)
        {
            if (rate.isString())
            {
                rateType.setParameter ((float) rateTypeOf (rate.toString()), juce::sendNotification);
                rateParam.setParameter (1.0f, juce::sendNotification);
            }
            else
            {
                rateType.setParameter ((float) te::ModifierCommon::hertz, juce::sendNotification);
                rateParam.setParameter ((float) (double) rate, juce::sendNotification);
            }
        }

        juce::var rateOf (te::AutomatableParameter& rateType, te::AutomatableParameter& rateParam)
        {
            auto type = juce::roundToInt (rateType.getCurrentValue());
            return type == te::ModifierCommon::hertz ? juce::var (round4 (rateParam.getCurrentValue()))
                                                     : juce::var (rateNames[type - (int) te::ModifierCommon::fourBars]);
        }

        // Einstellungen auf einen Modifier (geprueft mit validateModifier)
        void applySettings (te::Modifier& m, const juce::var& s)
        {
            if (auto* lfo = dynamic_cast<te::LFOModifier*> (&m))
            {
                if (s.hasProperty ("wave"))
                    lfo->waveParam->setParameter ((float) waveNames.indexOf (s["wave"].toString(), true), juce::sendNotification);

                if (s.hasProperty ("rate"))
                    applyRate (*lfo->rateTypeParam, *lfo->rateParam, s["rate"]);

                if (s.hasProperty ("phase"))
                    lfo->phaseParam->setParameter ((float) (double) s["phase"], juce::sendNotification);
            }

            if (auto* step = dynamic_cast<te::StepModifier*> (&m))
            {
                if (s.hasProperty ("rate"))
                    applyRate (*step->rateTypeParam, *step->rateParam, s["rate"]);

                if (auto* arr = s["steps"].getArray())
                {
                    step->numStepsParam->setParameter ((float) arr->size(), juce::sendNotification);

                    for (int i = 0; i < arr->size(); ++i)
                        step->setStep (i, (float) (double) arr->getReference (i));
                }
            }
        }

        float depthOf (te::AutomatableParameter& p)
        {
            return std::abs ((float) assignmentState (p).getProperty (te::IDs::value, 0.0f));
        }

        // Einstellungen eines Modifiers (fuer moveModifier und describeModifier)
        juce::var settingsOf (te::Modifier& m, float depth)
        {
            juce::var o;

            if (auto* step = dynamic_cast<te::StepModifier*> (&m))
            {
                juce::Array<juce::var> steps;

                for (int i = 0; i < juce::roundToInt (step->numStepsParam->getCurrentValue()); ++i)
                    steps.add (round4 (step->getStep (i)));

                o = resultObj (true, { { "type", "step" },
                                       { "steps", steps },
                                       { "rate", rateOf (*step->rateTypeParam, *step->rateParam) },
                                       { "depth", round4 (depth) } });
            }
            else if (auto* lfo = dynamic_cast<te::LFOModifier*> (&m))
            {
                o = resultObj (true, { { "type", "lfo" },
                                       { "wave", waveNames[juce::roundToInt (lfo->waveParam->getCurrentValue())] },
                                       { "rate", rateOf (*lfo->rateTypeParam, *lfo->rateParam) },
                                       { "depth", round4 (depth) },
                                       { "phase", round4 (lfo->phaseParam->getCurrentValue()) } });
            }

            if (auto* obj = o.getDynamicObject())
                obj->removeProperty ("ok");

            return o;
        }
    }

    void settleModifierBase (te::AutomatableParameter& p)
    {
        // Mit Kurve gilt sie (setParameter wuerde ihren einzigen Punkt verschieben)
        if (p.getCurve().getNumPoints() > 0)
            return;

        auto* mod = modifierOf (p);

        if (mod == nullptr)
            return;

        const auto base = p.getCurrentExplicitValue();
        const auto now = p.getEdit().getTransport().getPosition();
        mod->enabledParam->setParameter (0.0f, juce::sendNotification);
        p.updateFromAutomationSources (now);    // Modifier-Anteil 0
        p.setParameter (base, juce::sendNotification);
        mod->enabledParam->setParameter (1.0f, juce::sendNotification);
    }

    namespace
    {
        float setValueOf (te::AutomatableParameter& p)
        {
            return p.getCurve().getNumPoints() > 0 ? p.getCurrentBaseValue() : p.getCurrentExplicitValue();
        }
    }

    double faderSetDb (te::VolumeAndPanPlugin& vol)
    {
        return vol.volParam != nullptr ? (double) te::volumeFaderPositionToDB (setValueOf (*vol.volParam)) : (double) vol.getVolumeDb();
    }

    double faderSetPan (te::VolumeAndPanPlugin& vol)
    {
        return vol.panParam != nullptr ? (double) setValueOf (*vol.panParam) : (double) vol.getPan();
    }

    juce::String validateModifier (const AutomationTarget& t, const juce::var& s)
    {
        // Ohne type gilt der Typ des vorhandenen Modifiers, neu ein LFO
        auto* mod = t.bindings.empty() ? nullptr : modifierOf (*t.bindings.front().param);
        const auto current = mod != nullptr ? typeOf (*mod) : juce::String();
        const auto type = s.getProperty ("type", current.isEmpty() ? juce::String ("lfo") : current).toString();

        if (! typeNames.contains (type))
            return "unknown modifier type: " + type + " (" + typeNames.joinIntoString (", ") + ")";

        if (type == "step")
        {
            for (auto key : { "wave", "phase" })
                if (s.hasProperty (key))
                    return juce::String (key) + " is only for type lfo";

            if (s.hasProperty ("steps") ? ! validSteps (s["steps"]) : current != "step")
                return "steps must be a list of 2..64 values -1..1";
        }
        else if (s.hasProperty ("steps"))
            return "steps is only for type step";

        if (s.hasProperty ("wave") && ! waveNames.contains (s["wave"].toString(), true))
            return "wave must be one of " + waveNames.joinIntoString (", ");

        if (s.hasProperty ("rate"))
        {
            auto rate = s["rate"];
            auto ok = rate.isString() ? rateTypeOf (rate.toString()) >= 0
                                      : isNumber (rate) && (double) rate >= 0.01 && (double) rate <= 50.0;

            if (! ok)
                return "rate must be 0.01..50 (Hz) or a note value: " + rateNames.joinIntoString (", ");
        }

        for (auto key : { "depth", "phase" })
            if (s.hasProperty (key))
            {
                auto v = s[key];

                if (! isNumber (v) || (double) v < 0.0 || (double) v > 1.0)
                    return juce::String (key) + " must be 0..1";
            }

        return {};
    }

    void writeModifier (te::Edit& edit, const AutomationTarget& t, const juce::var& s)
    {
        if (t.bindings.empty())
            return;

        auto& primary = *t.bindings.front().param;
        auto* mod = modifierOf (primary);
        auto depth = mod != nullptr ? depthOf (primary) : 0.25f;
        const auto type = s.getProperty ("type", mod != nullptr ? typeOf (*mod) : juce::String ("lfo")).toString();

        // Anderer Typ: neuer Modifier mit dessen Vorgaben, die Tiefe bleibt
        if (mod != nullptr && typeOf (*mod) != type)
        {
            removeModifier (edit, *mod);
            mod = nullptr;
        }

        if (mod == nullptr)
            mod = createModifier (edit, primary, type);

        if (mod == nullptr)
            return;

        applySettings (*mod, s);

        if (s.hasProperty ("depth"))
            depth = (float) (double) s["depth"];

        for (auto& b : t.bindings)
            assign (*b.param, *mod, signOf (b, t) * depth);
    }

    void clearModifier (te::Edit& edit, const AutomationTarget& t)
    {
        if (! t.bindings.empty())
            if (auto* mod = modifierOf (*t.bindings.front().param))
                removeModifier (edit, *mod);
    }

    juce::var describeModifier (const AutomationTarget& t)
    {
        if (t.bindings.empty())
            return {};

        auto& b = t.bindings.front();
        auto* mod = modifierOf (*b.param);

        if (mod == nullptr)
            return {};

        auto depth = depthOf (*b.param);
        auto o = settingsOf (*mod, depth);
        auto& r = b.param->valueRange;
        // Basis: der Kurvenwert am Playhead, ohne Kurve der eingestellte Wert
        auto base = r.convertTo0to1 (b.param->getCurve().getNumPoints() > 0 ? b.param->getCurrentBaseValue()
                                                                           : b.param->getCurrentExplicitValue());
        auto at = [&] (float n) { return round4 (juce::jlimit (t.min, t.max, t.fromParam (r.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, n))))); };
        o.getDynamicObject()->setProperty ("low", at (base - depth));
        o.getDynamicObject()->setProperty ("high", at (base + depth));
        return o;
    }

    void moveModifier (te::Edit& edit, te::AutomatableParameter& from, te::AutomatableParameter& to)
    {
        auto* mod = modifierOf (from);

        if (mod == nullptr || &from == &to)
            return;

        auto s = settingsOf (*mod, depthOf (from));
        removeModifier (edit, *mod);

        if (auto* moved = createModifier (edit, to, s["type"].toString()))
        {
            applySettings (*moved, s);
            assign (to, *moved, (float) (double) s["depth"]);
        }
    }

    AutomationTarget findAutomationTarget (te::Edit& edit, const juce::String& id)
    {
        if (id.startsWith ("edge:"))
        {
            auto rest = id.fromFirstOccurrenceOf ("edge:", false, false);

            if (! rest.contains (">"))
                return fail (id, "edge id must be edge:<from>><to>");

            return edgeTarget (edit, id, rest.upToFirstOccurrenceOf (">", false, false),
                               rest.fromFirstOccurrenceOf (">", false, false));
        }

        if (id.startsWith ("feedback:"))
        {
            auto rest = id.fromFirstOccurrenceOf ("feedback:", false, false);
            auto edge = rest.upToLastOccurrenceOf (".", false, false);

            if (! rest.contains (".") || ! edge.contains (">"))
                return fail (id, "feedback id must be feedback:<from>><to>.level|delayMs");

            return feedbackTarget (edit, id, edge.upToFirstOccurrenceOf (">", false, false),
                                   edge.fromFirstOccurrenceOf (">", false, false),
                                   rest.fromLastOccurrenceOf (".", false, false));
        }

        auto node = id.upToFirstOccurrenceOf (".", false, false);
        auto key = id.fromFirstOccurrenceOf (".", false, false);

        if (! id.contains (".") || key.isEmpty())
            return fail (id, "unknown automation target: " + id);

        if (node.startsWithChar ('t'))
            return trackTarget (edit, id, node, key);

        if (node.startsWithChar ('p'))
            return moduleTarget (edit, id, node, key);

        if (node.startsWithChar ('g'))
            return groupTarget (edit, id, node, key);

        return fail (id, "unknown automation target: " + id);
    }

    std::vector<AutomationTarget> listAutomationTargets (te::Edit& edit, const juce::String& target, bool onlyAutomated)
    {
        juce::StringArray ids;
        std::map<juce::String, te::AutomatableParameter*> known;   // Modul-Ziel -> Parameter
        auto wants = [&target] (const juce::String& node) { return target.isEmpty() || node == target; };
        auto tracks = userTracks (edit);

        // Eine MIDI-Lane hat keinen eigenen Fader (faderTrackOf liefert den
        // ihrer Spur) — sonst erschien t0.volume auch als t2.volume
        // (Praxistest 1h); Audio-Lanes (Takes) haben ihren eigenen
        for (int i = 0; i < tracks.size(); ++i)
            if (wants ("t" + juce::String (i)))
                if (auto* g = laneGroupOf (*tracks[i]); g == nullptr || faderTrackOf (edit, *tracks[i]) != faderTrackOf (edit, *g))
                    ids.add ("t" + juce::String (i) + ".volume"), ids.add ("t" + juce::String (i) + ".pan");

        for (auto& g : groupIds (edit))
            if (wants (g))
                ids.add (g + ".volume"), ids.add (g + ".pan");

        for (auto& m : moduleIds (edit))
        {
            if (! wants (m))
                continue;

            auto* track = findModuleTrack (edit, m);
            auto* plugin = track != nullptr ? modulePlugin (*track) : nullptr;

            if (plugin == nullptr)
                continue;

            if (findRouteNode (edit, m).cls != "midiFx")
                ids.add (m + ".wet");

            // onlyAutomated: nur Parameter mit Punkten aufloesen — Name und
            // Ziel kosten je Parameter einen Lauf ueber alle Parameter; bei
            // zwei Surge XT (je 778) und Diva dauerte state 25 s und
            // blockierte Play/Monitor (Praxistest 1d, Flow-UI)
            auto params = plugin->getAutomatableParameters();
            auto labels = parameterLabels (*plugin);

            for (int i = 0; i < params.size(); ++i)
                if (! onlyAutomated || params[i]->getCurve().getNumPoints() > 0 || assignmentState (*params[i]).isValid())
                {
                    ids.add (m + "." + labels[i]);
                    known[ids[ids.size() - 1]] = params[i];
                }
        }

        for (auto& [from, to] : routingEdges (edit))
            if (wants (from) || wants (to))
                ids.add ("edge:" + from + ">" + to);

        for (auto& [from, to] : routingFeedbackEdges (edit))
            if (wants (from) || wants (to))
                ids.add ("feedback:" + from + ">" + to + ".level"), ids.add ("feedback:" + from + ">" + to + ".delayMs");

        std::vector<AutomationTarget> out;

        // Modul-Parameter direkt mit dem Parameter — ueber findAutomationTarget
        // lief findParameter je Ziel ueber alle Namen: quadratisch, Surge XT
        // (778) 0,9-1,7 s allein, unter Last ueber 3 s (Todo
        // 2026-11-18-automatable-many-params-zeitgrenze-unter-last)
        for (auto& id : ids)
        {
            auto k = known.find (id);
            auto t = k != known.end() ? moduleTarget (edit, id, id.upToFirstOccurrenceOf (".", false, false),
                                                      id.fromFirstOccurrenceOf (".", false, false), k->second)
                                      : findAutomationTarget (edit, id);

            if (t.error.isEmpty())
                out.push_back (std::move (t));
        }

        return out;
    }

    juce::String resolveAutomationReals (const AutomationTarget& t, const juce::var& points, juce::var& out)
    {
        out = points;

        if (! points.isArray())
            return {};   // meldet validateAutomationPoints

        juce::Array<juce::var> resolved;

        for (auto& p : *points.getArray())
        {
            if (! p.hasProperty ("real"))
            {
                resolved.add (p);
                continue;
            }

            if (p.hasProperty ("value"))
                return "each point needs value or real, not both";

            if (! t.fromReal)
                return juce::String::fromUTF8 ("real only for plugin parameters (p<N>.<name>) — use value");

            if (! isNumber (p.getProperty ("real", {})))
                return "real must be a number";

            double value = 0.0;

            if (auto err = t.fromReal ((double) p.getProperty ("real", {}), value); err.isNotEmpty())
                return err;

            auto* o = new juce::DynamicObject();

            for (auto& nv : p.getDynamicObject()->getProperties())
                if (nv.name.toString() != "real")
                    o->setProperty (nv.name, nv.value);

            o->setProperty ("value", value);
            resolved.add (juce::var (o));
        }

        out = resolved;
        return {};
    }

    juce::String validateAutomationPoints (const AutomationTarget& t, const juce::var& points)
    {
        if (! points.isArray())
            return "points must be an array";

        double last = 0.0;

        for (auto& p : *points.getArray())
        {
            if (! p.hasProperty ("time") || ! p.hasProperty ("value"))
                return "each point needs time and value or real";

            auto time = (double) p.getProperty ("time", 0.0);
            auto value = (double) p.getProperty ("value", 0.0);
            auto curve = p.getProperty ("curve", "linear").toString();

            if (time < 0.0)
                return "time must be >= 0";

            if (time < last)
                return "points must be sorted by time";

            if (value < t.min || value > t.max)
                return "value must be " + juce::String (t.min) + ".." + juce::String (t.max) + (t.unit.isNotEmpty() ? " " + t.unit : juce::String());

            if (curve != "linear" && curve != "step")
                return "curve must be linear or step";

            last = time;
        }

        return {};
    }

    void writeAutomation (te::Edit& edit, const AutomationTarget& t, const juce::var& points)
    {
        auto* arr = points.getArray();
        auto now = edit.getTransport().getPosition();

        for (auto& b : t.bindings)
        {
            auto& curve = b.param->getCurve();
            curve.clear (nullptr);

            if (arr != nullptr)
                for (int i = 0; i < arr->size(); ++i)
                {
                    auto& p = arr->getReference (i);
                    auto value = b.toParam ((double) p.getProperty ("value", 0.0));
                    curve.addPoint (te::TimePosition::fromSeconds ((double) p.getProperty ("time", 0.0)), value, 0.0f, nullptr);

                    // Stufe: Wert bis zum naechsten Punkt halten
                    if (p.getProperty ("curve", "linear").toString() == "step" && i + 1 < arr->size())
                        curve.addPoint (te::TimePosition::fromSeconds ((double) arr->getReference (i + 1).getProperty ("time", 0.0)),
                                        value, 0.0f, nullptr);
                }

            // Ohne Punkte gilt wieder der Basiswert, mit Punkten der Kurvenwert
            // am Playhead
            b.param->updateToFollowCurve (now);
        }
    }

    juce::var readAutomation (const AutomationTarget& t)
    {
        juce::Array<juce::var> out;

        if (t.bindings.empty())
            return out;

        auto& curve = t.bindings.front().param->getCurve();
        std::vector<RawPoint> raw;

        for (int i = 0; i < curve.getNumPoints(); ++i)
            raw.push_back ({ curve.getPointTime (i).inSeconds(), curve.getPointValue (i) });

        // Ein Haltepunkt (gleiche Zeit wie der Folgepunkt, Wert des Vorgaengers)
        // wird zur Stufe am Vorgaenger zurueckgefaltet
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (! out.isEmpty() && i + 1 < raw.size() && raw[i + 1].time == raw[i].time
                && raw[i].value == raw[i - 1].value)
            {
                if (auto* o = out.getReference (out.size() - 1).getDynamicObject())
                    o->setProperty ("curve", "step");

                continue;
            }

            out.add (resultObj (true, { { "time", round4 (raw[i].time) },
                                        { "value", round4 (t.fromParam (raw[i].value)) },
                                        { "curve", "linear" } }));
        }

        for (auto& v : out)
            if (auto* o = v.getDynamicObject())
                o->removeProperty ("ok");

        return out;
    }

    juce::var describeAutomationTarget (const AutomationTarget& t, bool withPoints)
    {
        auto* primary = t.bindings.empty() ? nullptr : t.bindings.front().param;
        auto points = primary != nullptr ? primary->getCurve().getNumPoints() : 0;
        auto o = resultObj (true, { { "param", t.id }, { "label", t.label }, { "unit", t.unit },
                                    { "min", t.min }, { "max", t.max },
                                    { "value", primary != nullptr ? round4 (t.fromParam (primary->getCurrentValue())) : 0.0 },
                                    { "automated", points > 0 } });

        if (withPoints)
            o.getDynamicObject()->setProperty ("points", readAutomation (t));

        if (auto m = describeModifier (t); ! m.isVoid())
            o.getDynamicObject()->setProperty ("modifier", m);

        o.getDynamicObject()->removeProperty ("ok");
        return o;
    }
}
