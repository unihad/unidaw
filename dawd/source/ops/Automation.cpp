#include "Ops.h"
#include "../Automation.h"
#include "../EngineHelpers.h"
#include "../Routing.h"

// Automation (Familie processing): Parameter von Spuren, Plugins und
// Routing-Matrix ueber die Songzeit. Ziele und Einheiten: Automation.h.
namespace dawd
{
    juce::var opListAutomatable (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto target = cmd.getProperty ("target", {}).toString().trim();

        // target: Knoten-Id oder Name von Spur, Modul oder Gruppe — ein
        // unbekanntes Ziel ist ein Fehler (Praxistest remix-1: der Spurname
        // gab still eine leere Liste)
        if (target.isNotEmpty())
        {
            const auto num = target.substring (1);
            const bool isId = target.length() > 1 && num.containsOnly ("0123456789");
            juce::String node;

            if (isId && target.startsWithChar ('t'))
                node = num.getIntValue() < userTracks (edit).size() ? target : juce::String();
            else if (isId && target.startsWithChar ('p'))
                node = findModuleTrack (edit, target) != nullptr ? target : juce::String();
            else if (isId && target.startsWithChar ('g'))
                node = groupIdFor (edit, target);
            else if (auto ti = trackIndexFor (edit, target); ti >= 0 && ti < userTracks (edit).size())
                node = "t" + juce::String (ti);
            else if (auto m = moduleIdFor (edit, target); m.isNotEmpty())
                node = m;
            else
                node = groupIdFor (edit, target);

            if (node.isEmpty())
                return resultObj (false, { { "error", "no such target: " + target + " (t<i>, p<N>, g<N> or a track, module or group name)" } });

            target = node;
        }

        auto targets = listAutomationTargets (edit, target);

        // compact ohne target (MCP): je Knoten nur Name und Anzahl — die
        // volle Liste hatte 119 000 Zeichen (Praxistest remix-1)
        if (target.isEmpty() && (bool) cmd.getProperty ("compact", false))
        {
            const auto dot = juce::String::fromUTF8 (" · ");
            juce::StringArray order;
            std::map<juce::String, std::pair<juce::String, int>> nodes;   // Knoten -> Name, Anzahl

            for (auto& t : targets)
            {
                const bool edge = t.id.startsWith ("edge:") || t.id.startsWith ("feedback:");
                auto node = edge ? t.id.fromFirstOccurrenceOf (":", false, false).upToFirstOccurrenceOf (">", false, false)
                                 : t.id.upToFirstOccurrenceOf (".", false, false);
                order.addIfNotAlreadyThere (node);
                auto& n = nodes[node];

                if (n.first.isEmpty() && ! edge)
                {
                    auto* mt = node.startsWithChar ('p') ? findModuleTrack (edit, node) : nullptr;
                    n.first = mt != nullptr ? moduleLabel (*mt) : t.label.upToFirstOccurrenceOf (dot, false, false);
                }

                ++n.second;
            }

            juce::Array<juce::var> list;

            for (auto& n : order)
                list.add (resultObj (true, { { "target", n }, { "name", nodes[n].first }, { "count", nodes[n].second } }));

            for (auto& v : list)
                v.getDynamicObject()->removeProperty ("ok");

            return resultObj (true, { { "targets", list },
                                      { "note", juce::String::fromUTF8 ("parameters per node — listAutomatable {target} lists one node's (id or name), detail: true all") } });
        }

        juce::Array<juce::var> params;

        for (auto& t : targets)
            params.add (describeAutomationTarget (t, false));

        return resultObj (true, { { "params", params } });
    }

    // Read-only: ein Ziel mit Punkten, ohne param alle automatisierten Ziele
    // (fuer die Lanes in der Timeline)
    juce::var opAutomation (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("param", {}).toString();

        if (id.isNotEmpty())
        {
            auto t = findAutomationTarget (*ctx.edit, id);

            if (t.error.isNotEmpty())
                return resultObj (false, { { "error", t.error } });

            auto o = describeAutomationTarget (t, true);
            o.getDynamicObject()->setProperty ("ok", true);
            return o;
        }

        juce::Array<juce::var> params, modifiers;

        for (auto& t : listAutomationTargets (*ctx.edit, {}, true))
        {
            if (t.bindings.front().param->getCurve().getNumPoints() > 0)
                params.add (describeAutomationTarget (t, true));

            if (! describeModifier (t).isVoid())
                modifiers.add (describeAutomationTarget (t, false));
        }

        return resultObj (true, { { "params", params }, { "modifiers", modifiers } });
    }

    juce::var opSetAutomation (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("param", {}).toString();
        auto t = findAutomationTarget (*ctx.edit, id);

        if (t.error.isNotEmpty())
            return resultObj (false, { { "error", t.error } });

        // real je Punkt (Plugin-Parameter) -> value; die Aktion haelt value
        juce::var points;

        if (auto err = resolveAutomationReals (t, cmd.getProperty ("points", {}), points); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (auto err = validateAutomationPoints (t, points); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        writeAutomation (*ctx.edit, t, points);

        // Folgeaufrufe auf denselben Parameter (Punkte ziehen) = eine Aktion
        auto params = new juce::DynamicObject();
        params->setProperty ("param", id);
        params->setProperty ("points", points);
        recordNodeAction (ctx, "process", "setAutomation", juce::var (params), {}, true);

        auto o = describeAutomationTarget (t, true);
        o.getDynamicObject()->setProperty ("ok", true);
        return o;
    }

    juce::var opClearAutomation (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("param", {}).toString();
        auto t = findAutomationTarget (*ctx.edit, id);

        if (t.error.isNotEmpty())
            return resultObj (false, { { "error", t.error } });

        writeAutomation (*ctx.edit, t, juce::var (juce::Array<juce::var>()));

        auto params = new juce::DynamicObject();
        params->setProperty ("param", id);
        recordNodeAction (ctx, "process", "clearAutomation", juce::var (params));

        auto o = describeAutomationTarget (t, false);
        o.getDynamicObject()->setProperty ("ok", true);
        return o;
    }

    // Modifier (LFO, Step) an einem Ziel: anlegen oder aendern — Folgeaufrufe auf
    // dasselbe Ziel sind eine Aktion; die Aktion haelt die ganzen
    // Einstellungen (zusammengefasst ersetzt sie die vorige)
    juce::var opSetModifier (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("param", {}).toString();
        auto t = findAutomationTarget (*ctx.edit, id);

        if (t.error.isNotEmpty())
            return resultObj (false, { { "error", t.error } });

        if (auto err = validateModifier (t, cmd); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        writeModifier (*ctx.edit, t, cmd);
        auto m = describeModifier (t);

        if (m.isVoid())
            return resultObj (false, { { "error", "no modifier possible on " + id } });

        auto params = new juce::DynamicObject();
        params->setProperty ("param", id);

        for (auto& nv : m.getDynamicObject()->getProperties())
            if (nv.name.toString() != "low" && nv.name.toString() != "high")
                params->setProperty (nv.name, nv.value);

        recordNodeAction (ctx, "process", "setModifier", juce::var (params), {}, true);

        auto o = describeAutomationTarget (t, false);
        o.getDynamicObject()->setProperty ("ok", true);
        return o;
    }

    juce::var opClearModifier (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("param", {}).toString();
        auto t = findAutomationTarget (*ctx.edit, id);

        if (t.error.isNotEmpty())
            return resultObj (false, { { "error", t.error } });

        clearModifier (*ctx.edit, t);

        auto params = new juce::DynamicObject();
        params->setProperty ("param", id);
        recordNodeAction (ctx, "process", "clearModifier", juce::var (params));

        auto o = describeAutomationTarget (t, false);
        o.getDynamicObject()->setProperty ("ok", true);
        return o;
    }
}
