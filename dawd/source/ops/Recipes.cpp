#include "Ops.h"
#include "../Automation.h"
#include "../EngineHelpers.h"
#include "../OpRegistry.h"
#include "../PluginSettings.h"
#include "../Presets.h"
#include "../Routing.h"

#include <map>
#include <set>

// Rezept-Katalog (Familie recipes): was in einem Projekt gut klang oder gut
// lief, als wiederverwendbares Rezept ablegen und anderswo nachspielen.
//
// saveRecipe liest Module, ihre Zustaende und Kanten aus dem geladenen
// Node-Edit — nicht aus den Node-Aktionen, denn setParameter, loadPreset,
// setWet und trackMix stehen dort nicht. Der genaue Klang steckt im
// Plugin-Zustand (state/<n>.dawdpreset), lesbar dazu die Regler, die vom
// Standard abweichen. Die Schritte sind Op-Aufrufe mit Platzhaltern ($track,
// $p1, $recipe, eigene wie $tempo), die applyRecipe ueber executeOp in einem
// neuen Zweig abspielt (Undo je Schritt, zurueck = Eltern-Node auschecken).
//
// Ablage je Rezept ein Ordner <kind>/<id>/ mit recipe.json und state/:
// Entwuerfe in <config>/recipes/, gepruefte im Repo (ctx.recipesDir).
namespace dawd
{
    namespace
    {
        const juce::StringArray recipeKinds { "sound", "chain", "workflow", "mixTarget" };
        const juce::StringArray recipeStatuses { "draft", "checked", "rejected" };

        juce::File draftsDir (DawdContext& ctx)
        {
            return ctx.configDir.getChildFile ("recipes");
        }

        struct Found
        {
            juce::File dir;
            juce::var recipe;
            juce::String where;   // repo | local
        };

        // Alle Rezepte beider Ablagen — wenige Dateien, jedes Mal frisch
        // gelesen (rateRecipe und applyRecipe schreiben sie um)
        std::vector<Found> allRecipes (DawdContext& ctx)
        {
            std::vector<Found> out;

            auto scan = [&] (const juce::File& root, const juce::String& where)
            {
                if (! root.isDirectory())
                    return;

                for (auto& f : root.findChildFiles (juce::File::findFiles, true, "recipe.json"))
                {
                    auto v = juce::JSON::parse (f);

                    if (v.getDynamicObject() != nullptr && v.getProperty ("id", {}).toString().isNotEmpty())
                        out.push_back ({ f.getParentDirectory(), v, where });
                }
            };

            scan (ctx.recipesDir, "repo");
            scan (draftsDir (ctx), "local");
            return out;
        }

        std::optional<Found> findRecipe (DawdContext& ctx, const juce::String& id)
        {
            for (auto& r : allRecipes (ctx))
                if (r.recipe.getProperty ("id", {}).toString() == id)
                    return r;

            return std::nullopt;
        }

        bool writeRecipe (const juce::File& dir, const juce::var& recipe)
        {
            return dir.createDirectory()
                && dir.getChildFile ("recipe.json").replaceWithText (juce::JSON::toString (recipe) + "\n");
        }

        // Ordner verschieben (gepruefte Rezepte wandern ins Repo) — ueber
        // Dateisystem-Grenzen per Kopie
        bool moveDir (const juce::File& from, const juce::File& to)
        {
            to.getParentDirectory().createDirectory();

            if (from.moveFileTo (to))
                return true;

            return from.copyDirectoryTo (to) && from.deleteRecursively();
        }

        juce::String slugOf (const juce::String& title)
        {
            auto s = title.toLowerCase()
                         .replace (juce::String::fromUTF8 ("ä"), "ae")
                         .replace (juce::String::fromUTF8 ("ö"), "oe")
                         .replace (juce::String::fromUTF8 ("ü"), "ue")
                         .replace (juce::String::fromUTF8 ("ß"), "ss");
            juce::String out;

            for (auto c : s)
            {
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                    out << juce::String::charToString (c);
                else if (out.isNotEmpty() && ! out.endsWithChar ('-'))
                    out << "-";
            }

            out = out.trimCharactersAtEnd ("-").substring (0, 40).trimCharactersAtEnd ("-");
            return out.isEmpty() ? juce::String ("recipe") : out;
        }

        juce::String uniqueId (DawdContext& ctx, const juce::String& title)
        {
            auto base = slugOf (title);
            auto id = base;

            for (int n = 2; findRecipe (ctx, id).has_value(); ++n)
                id = base + "-" + juce::String (n);

            return id;
        }

        juce::StringArray stringList (const juce::var& v)
        {
            juce::StringArray out;

            if (auto* arr = v.getArray())
            {
                for (auto& x : *arr)
                    if (x.toString().trim().isNotEmpty())
                        out.add (x.toString().trim());
            }
            else if (v.toString().trim().isNotEmpty())
                out.addTokens (v.toString(), ",", "");

            out.trim();
            out.removeEmptyStrings();
            return out;
        }

        juce::var toVarArray (const juce::StringArray& a)
        {
            juce::Array<juce::var> out;

            for (auto& s : a)
                out.add (s);

            return out;
        }

        juce::var step (const juce::String& op, const juce::var& params, const juce::String& bind = {},
                        const juce::String& text = {}, bool optional = false)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("op", op);
            o->setProperty ("params", params);

            if (bind.isNotEmpty())
                o->setProperty ("bind", bind);

            if (text.isNotEmpty())
                o->setProperty ("text", text);

            if (optional)
                o->setProperty ("optional", true);

            return juce::var (o);
        }

        juce::var obj (std::initializer_list<std::pair<juce::Identifier, juce::var>> props)
        {
            auto o = new juce::DynamicObject();

            for (auto& [k, v] : props)
                o->setProperty (k, v);

            return juce::var (o);
        }

        // ---- Platzhalter ----------------------------------------------------

        // Laengste zuerst: $p12 vor $p1, $track2 vor $track
        using Bindings = std::vector<std::pair<juce::String, juce::var>>;

        void bindValue (Bindings& b, const juce::String& key, const juce::var& value)
        {
            for (auto& [k, v] : b)
                if (k == key)
                {
                    v = value;
                    return;
                }

            b.push_back ({ key, value });
            std::stable_sort (b.begin(), b.end(), [] (auto& x, auto& y) { return x.first.length() > y.first.length(); });
        }

        // Ein Wert, der genau ein Platzhalter ist, bekommt den gebundenen Wert
        // samt Typ ($track -> 2); in laengeren Texten wird ersetzt ("t$track")
        juce::var substitute (const juce::var& v, const Bindings& b)
        {
            if (v.isString())
            {
                auto s = v.toString();

                for (auto& [k, val] : b)
                    if (s == k)
                        return val;

                for (auto& [k, val] : b)
                    s = s.replace (k, val.toString());

                return s;
            }

            if (auto* arr = v.getArray())
            {
                juce::Array<juce::var> out;

                for (auto& x : *arr)
                    out.add (substitute (x, b));

                return out;
            }

            if (auto* o = v.getDynamicObject())
            {
                auto n = new juce::DynamicObject();

                for (auto& p : o->getProperties())
                    n->setProperty (p.name, substitute (p.value, b));

                return juce::var (n);
            }

            return v;
        }

        // Erster uebrig gebliebener Platzhalter ($name), sonst ""
        juce::String unbound (const juce::var& v)
        {
            if (v.isString())
            {
                auto s = v.toString();

                for (int i = s.indexOfChar ('$'); i >= 0; i = s.indexOfChar (i + 1, '$'))
                {
                    if (i + 1 >= s.length() || ! juce::CharacterFunctions::isLetter (s[i + 1]))
                        continue;

                    int e = i + 1;

                    while (e < s.length() && juce::CharacterFunctions::isLetterOrDigit (s[e]))
                        ++e;

                    return s.substring (i, e);
                }

                return {};
            }

            if (auto* arr = v.getArray())
                for (auto& x : *arr)
                    if (auto u = unbound (x); u.isNotEmpty())
                        return u;

            if (auto* o = v.getDynamicObject())
                for (auto& p : o->getProperties())
                    if (auto u = unbound (p.value); u.isNotEmpty())
                        return u;

            return {};
        }

        // ---- Zutaten aus dem Edit -----------------------------------------

        // Regler, die vom Standard abweichen (Extern: normalisierter Default
        // des Plugins; eingebaut: Default des Werts) — lesbar, der Klang
        // selbst steckt im Zustand. real/unit wie listParameters (Praxistest
        // 1j P8, 1m P11: hier stand der interne Wert, Detune 0,195 fuer 20 %)
        juce::var changedParameters (te::Plugin& plugin, int limit, int& changed)
        {
            juce::Array<juce::var> out;
            const bool external = dynamic_cast<te::ExternalPlugin*> (&plugin) != nullptr;
            const auto params = plugin.getAutomatableParameters();
            changed = 0;

            for (auto* p : params)
            {
                auto def = p->getDefaultValue();

                if (! def.has_value())
                    continue;

                auto cur = external ? p->getCurrentNormalisedValue() : p->getCurrentValue();

                if (std::abs (cur - *def) < 1.0e-3f)
                    continue;

                if (++changed > limit)
                    continue;

                auto shown = parameterVar (plugin, *p, params.indexOf (p));
                out.add (obj ({ { "name", shown.getProperty ("name", {}) },
                                { "text", shown.getProperty ("text", {}) },
                                { "real", shown.getProperty ("real", {}) },
                                { "unit", shown.getProperty ("unit", {}) } }));
            }

            for (auto& s : pluginSettings (plugin))
                out.add (obj ({ { "name", s.name }, { "text", s.get().toString() } }));

            return out;
        }

        juce::StringArray moduleIdsIn (const juce::File& editFile)
        {
            juce::StringArray ids;
            auto xml = juce::XmlDocument::parse (editFile);

            std::function<void (const juce::XmlElement&)> walk = [&] (const juce::XmlElement& e)
            {
                if (e.hasAttribute ("dawdModule"))
                    ids.addIfNotAlreadyThere ("p" + e.getStringAttribute ("dawdModule"));

                for (auto* c : e.getChildIterator())
                    walk (*c);
            };

            if (xml != nullptr)
                walk (*xml);

            return ids;
        }

        // Module, die eine Node-Aktion nennt (addPlugin, setChain, connect …)
        void moduleIdsInActions (const juce::var& v, juce::StringArray& ids)
        {
            if (v.isString())
            {
                auto s = v.toString();

                // "p3", "p3.Cutoff", "edge:p3>p4", "feedback:p3>p4.level"
                for (auto& tok : juce::StringArray::fromTokens (s, ":>.", ""))
                    if (tok.length() > 1 && tok[0] == 'p' && tok.substring (1).containsOnly ("0123456789"))
                        ids.addIfNotAlreadyThere (tok);
            }
            else if (auto* arr = v.getArray())
            {
                for (auto& x : *arr)
                    moduleIdsInActions (x, ids);
            }
            else if (auto* o = v.getDynamicObject())
            {
                for (auto& p : o->getProperties())
                    moduleIdsInActions (p.value, ids);
            }
        }

        int moduleNumber (const juce::String& id)
        {
            return id.substring (1).getIntValue();
        }

        juce::var findById (const juce::var& list, const juce::String& id)
        {
            if (auto* arr = list.getArray())
                for (auto& x : *arr)
                    if (x.getProperty ("id", {}).toString() == id)
                        return x;

            return {};
        }

        // Master-Kette in Reihenfolge: ab mainEntry den Kanten nach bis main
        juce::StringArray masterChain (te::Edit& edit)
        {
            juce::StringArray out;
            auto cur = mainEntry (edit);

            while (cur != "main" && cur.isNotEmpty() && ! out.contains (cur))
            {
                out.add (cur);
                auto targets = targetsOf (edit, findRouteNode (edit, cur));
                cur = targets.isEmpty() ? juce::String() : targets[0];
            }

            return out;
        }

        // Module, die ein Knoten ueber normale Kanten (auch Sends) erreicht —
        // ohne Main und ohne die Master-Kette
        juce::StringArray reachableModules (te::Edit& edit, const juce::var& routing, const juce::String& from)
        {
            juce::StringArray seen, todo { from };
            auto* edges = routing.getProperty ("edges", {}).getArray();

            while (edges != nullptr && ! todo.isEmpty())
            {
                auto id = todo[0];
                todo.remove (0);

                for (auto& e : *edges)
                {
                    auto to = e.getProperty ("to", {}).toString();

                    if (e.getProperty ("from", {}).toString() == id && to.startsWithChar ('p')
                        && ! isMasterModule (edit, to) && ! seen.contains (to))
                    {
                        seen.add (to);
                        todo.add (to);
                    }
                }
            }

            return seen;
        }

        // Section, in der t liegt: die letzte, die bei t oder davor beginnt
        // (vor der ersten: die erste); nullptr ohne Sections
        te::MarkerClip* sectionUnder (te::Edit& edit, double t)
        {
            auto sections = sortedSections (edit);
            te::MarkerClip* out = sections.empty() ? nullptr : sections.front();

            for (auto* s : sections)
                if (s->getPosition().getStart().inSeconds() <= t + 1.0e-3)
                    out = s;

            return out;
        }

        double beatAt (te::Edit& edit, double seconds)
        {
            return edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (seconds)).inBeats();
        }

        // Automation relativ zur Section (Rezept v2, Entscheidung Matthias
        // 2026-10-24): Punkte in Schlaegen ab dem Beginn der Quell-Section,
        // applyRecipe legt sie ab dem Beginn der Ziel-Section an. crop
        // schneidet auf [start, end] zu, mit dem Wert der Kurve an den Raendern.
        juce::var pointsInBeats (te::Edit& edit, const AutomationTarget& at, const juce::var& points,
                                 double start, double end, bool crop)
        {
            constexpr double eps = 1.0e-4;
            const double b0 = beatAt (edit, start);
            auto r4 = [] (double x) { return std::round (x * 10000.0) / 10000.0; };
            auto valueAt = [&] (double t)
            {
                return r4 (at.fromParam (at.bindings.front().param->getCurve().getValueAt (te::TimePosition::fromSeconds (t), 0.0f)));
            };

            juce::Array<juce::var> inside, out;
            juce::String beforeCurve;   // Kurve des letzten Punkts vor der Section
            bool after = false;

            if (auto* arr = points.getArray())
            {
                for (auto& p : *arr)
                {
                    auto t = (double) p.getProperty ("time", 0.0);

                    if (crop && t < start - eps)
                        beforeCurve = p.getProperty ("curve", "linear").toString();
                    else if (crop && t > end + eps)
                        after = true;
                    else
                        inside.add (p);
                }
            }

            auto timeOf = [] (const juce::var& p) { return (double) p.getProperty ("time", 0.0); };

            if (beforeCurve.isNotEmpty() && (inside.isEmpty() || timeOf (inside.getFirst()) > start + eps))
                out.add (obj ({ { "beat", 0.0 }, { "value", valueAt (start) }, { "curve", beforeCurve } }));

            for (auto& p : inside)
                out.add (obj ({ { "beat", r4 (beatAt (edit, timeOf (p)) - b0) }, { "value", p.getProperty ("value", {}) },
                                { "curve", p.getProperty ("curve", "linear") } }));

            if (after && (inside.isEmpty() || timeOf (inside.getLast()) < end - eps))
                out.add (obj ({ { "beat", r4 (beatAt (edit, end) - b0) }, { "value", valueAt (end) }, { "curve", "linear" } }));

            return out;
        }

        // Rezept-v2-Punkte {beat} ab dem Beginn der Ziel-Section in Songzeit
        juce::var pointsInSeconds (te::Edit& edit, const juce::var& points, double start)
        {
            juce::Array<juce::var> out;
            const double b0 = beatAt (edit, start);

            if (auto* arr = points.getArray())
            {
                for (auto& p : *arr)
                {
                    auto t = edit.tempoSequence.toTime (te::BeatPosition::fromBeats (b0 + (double) p.getProperty ("beat", 0.0))).inSeconds();
                    auto o = obj ({ { "time", std::round (t * 10000.0) / 10000.0 }, { "value", p.getProperty ("value", {}) } });

                    if (p.hasProperty ("curve"))
                        o.getDynamicObject()->setProperty ("curve", p.getProperty ("curve", {}));

                    out.add (o);
                }
            }

            return out;
        }

        bool hasBeatPoints (const juce::var& step)
        {
            auto* pts = step.getProperty ("params", {}).getProperty ("points", {}).getArray();
            return step.getProperty ("op", {}).toString() == "setAutomation" && pts != nullptr
                && ! pts->isEmpty() && pts->getFirst().hasProperty ("beat");
        }
    }

    namespace
    {
        // Hoerprobe auf -1 dBFS Spitze, 16 Bit: ohne Master-Kette liegt eine
        // Spur oft ueber 0 dBFS und verzerrte im Browser-Player
        bool normalisePreview (DawdContext& ctx, const juce::File& f)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f));

            if (reader == nullptr || reader->lengthInSamples <= 0)
                return false;

            juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
            const double rate = reader->sampleRate;
            reader.reset();
            const float peak = buf.getMagnitude (0, buf.getNumSamples());

            if (peak > 0.0f)
                buf.applyGain (juce::Decibels::decibelsToGain (-1.0f) / peak);

            f.deleteFile();
            std::unique_ptr<juce::OutputStream> out (f.createOutputStream().release());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (out.get(), rate, (unsigned int) buf.getNumChannels(), 16, {}, 0));

            if (w == nullptr)
                return false;

            out.release();
            return w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
        }
    }

    // ---- saveRecipe ---------------------------------------------------------

    juce::var opSaveRecipe (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto& edit = *ctx.edit;
        auto title = cmd.getProperty ("title", {}).toString().trim();
        auto goal = cmd.getProperty ("goal", {}).toString().trim();
        auto kind = cmd.getProperty ("kind", {}).toString();

        if (title.isEmpty())
            return resultObj (false, { { "error", "need a title" } });

        if (goal.isEmpty())
            return resultObj (false, { { "error", "need a goal (one sentence: what the recipe achieves)" } });

        if (kind.isNotEmpty() && ! recipeKinds.contains (kind))
            return resultObj (false, { { "error", "kind must be one of: " + recipeKinds.joinIntoString (", ") } });

        auto nodeId = (int) (double) cmd.getProperty ("node", (double) t.current);

        if (nodeId != t.current)
            return resultObj (false, { { "error", "node " + juce::String (nodeId) + " is not checked out (treeCheckout first)" } });

        auto* node = t.find (t.current);

        if (node == nullptr)
            return resultObj (false, { { "error", "no such node" } });

        // section: Hoerprobe und Automation nur aus dieser Section
        te::MarkerClip* srcSection = nullptr;

        if (cmd.hasProperty ("section"))
        {
            auto name = cmd.getProperty ("section", {}).toString();
            srcSection = findSection (edit, name);

            if (srcSection == nullptr)
                return resultObj (false, { { "error", "no such section: " + name } });
        }

        // Eigene Schritte (Arbeitsablauf, Mix-Ziel oder Ergaenzung):
        // [{op?, params?, bind?, text?, optional?}] — mindestens op oder text
        juce::Array<juce::var> extraSteps;

        if (cmd.hasProperty ("steps"))
        {
            auto* arr = cmd.getProperty ("steps", {}).getArray();

            if (arr == nullptr)
                return resultObj (false, { { "error", "steps must be a list of {op?, params?, bind?, text?}" } });

            for (int i = 0; i < arr->size(); ++i)
            {
                // Ein String ist ein Text-Schritt (Praxistest remix-1: steps
                // als Liste von Strings scheiterte mit "step 1 needs op or text")
                auto s = arr->getReference (i);

                if (s.isString())
                {
                    auto o = new juce::DynamicObject();
                    o->setProperty ("text", s.toString());
                    s = juce::var (o);
                }

                auto op = s.getProperty ("op", {}).toString();
                auto text = s.getProperty ("text", {}).toString();

                if (op.isEmpty() && text.isEmpty())
                    return resultObj (false, { { "error", "step " + juce::String (i + 1) + " needs op or text ({text} or {op, params})" } });

                if (op.isNotEmpty() && findOp (op) == nullptr)
                    return resultObj (false, { { "error", "step " + juce::String (i + 1) + ": unknown op " + op } });

                if (op == "applyRecipe" || op == "batch" || op == "quit")
                    return resultObj (false, { { "error", "step " + juce::String (i + 1) + ": " + op + " is not allowed in a recipe" } });

                extraSteps.add (step (op, s.getProperty ("params", juce::var (new juce::DynamicObject())),
                                      s.getProperty ("bind", {}).toString(), text, (bool) s.getProperty ("optional", false)));
            }
        }

        // ---- welche Module ----
        saveCurrentEdit (ctx);
        auto routing = routingState (edit, true);
        auto users = userTracks (edit);
        juce::StringArray modules, warnings;
        juce::String scope;

        // Ein Modul gehoert der Spur, die es als einzige erreicht (wie der
        // Fokus des Auto-Branch)
        std::map<juce::String, juce::StringArray> reach;

        if (auto* srcs = routing.getProperty ("sources", {}).getArray())
            for (auto& s : *srcs)
                reach[s.getProperty ("id", {}).toString()] = reachableModules (edit, routing, s.getProperty ("id", {}).toString());

        auto ownedBy = [&] (const juce::String& tid, const juce::String& m)
        {
            bool mine = false, other = false;

            for (auto& [sid, r] : reach)
                if (r.contains (m))
                    (sid == tid ? mine : other) = true;

            return mine && ! other;
        };

        if (cmd.hasProperty ("modules"))
        {
            scope = "modules";

            for (auto& id : stringList (cmd.getProperty ("modules", {})))
            {
                if (findModuleTrack (edit, id) == nullptr)
                    return resultObj (false, { { "error", "no such plugin: " + id } });

                modules.addIfNotAlreadyThere (id);
            }
        }
        else if (cmd.hasProperty ("track"))
        {
            scope = "track";
            auto track = (int) (double) cmd.getProperty ("track", -1.0);

            if (track < 0 || track >= users.size())
                return resultObj (false, { { "error", "no such track: " + juce::String (track) } });

            auto tid = "t" + juce::String (track);
            auto chain = findById (routing.getProperty ("chains", {}), tid);

            // Die Kette und was nur diese Spur speist: Module hinter einer
            // Verzweigung oder einem Send (Praxistest 1h: der Parallelweg
            // fehlte). Geteilte Module und die Master-Kette bleiben draussen.
            if (auto it = reach.find (tid); it != reach.end())
                for (auto& m : it->second)
                    if (ownedBy (tid, m))
                        modules.add (m);

            if (modules.isEmpty())
                return resultObj (false, { { "error", "track " + juce::String (track) + " has no plugins in its chain" } });

            // Verzweigungen, Sends und Pegel zwischen Modulen spielt das Rezept
            // nach; nur der Pegel am Ende der Kette fehlt
            for (auto& c : stringList (chain.getProperty ("complex", {})))
                if (c.startsWith ("level ") && c.endsWith ("> main"))
                    warnings.add ("not in the recipe: " + c);
        }
        else if (! extraSteps.isEmpty() && kind == "workflow")
        {
            scope = "steps";
        }
        else
        {
            // Default: was an diesem Node dazukam (neu gegenueber dem Base-
            // Snapshot) oder in seinen Aktionen vorkommt
            scope = "node";
            auto base = t.projectDir.getChildFile ("node-" + juce::String (t.current) + ".base.tracktionedit");
            auto before = base.existsAsFile() ? moduleIdsIn (base) : juce::StringArray();
            juce::StringArray named;

            for (auto& a : node->actions)
                moduleIdsInActions (a.params, named);

            for (auto& id : moduleIds (edit))
                if ((base.existsAsFile() && ! before.contains (id)) || named.contains (id))
                    modules.add (id);

            if (modules.isEmpty() && extraSteps.isEmpty())
                return resultObj (false, { { "error", juce::String::fromUTF8 ("no plugins added or used at this node — give track, modules or steps") } });
        }

        std::sort (modules.begin(), modules.end(), [] (auto& a, auto& b) { return moduleNumber (a) < moduleNumber (b); });

        // Master-Module in Ketten-Reihenfolge ans Ende (addPlugin at master
        // haengt hinten an)
        juce::StringArray masters;

        for (auto& id : masterChain (edit))
            if (modules.contains (id))
                masters.add (id);

        for (auto& id : modules)
            if (isMasterModule (edit, id))
                masters.addIfNotAlreadyThere (id);

        for (auto& m : masters)
            modules.removeString (m);

        modules.addArray (masters);

        // ---- Refs ----
        std::map<juce::String, juce::String> refs;   // p3 -> $p1, t0 -> $track

        for (int i = 0; i < modules.size(); ++i)
            refs[modules[i]] = "$p" + juce::String (i + 1);

        juce::StringArray trackIds;   // t<i> in Reihenfolge des ersten Auftretens
        auto useTrack = [&] (const juce::String& tid)
        {
            if (! trackIds.contains (tid))
            {
                trackIds.add (tid);
                refs[tid] = trackIds.size() == 1 ? juce::String ("$track") : "$track" + juce::String (trackIds.size());
            }

            return refs[tid];
        };

        auto nodeRef = [&] (const juce::String& id) -> juce::String
        {
            if (id == "main")
                return id;

            if (id.startsWithChar ('t'))
                return "t" + useTrack (id);

            auto it = refs.find (id);
            return it != refs.end() ? it->second : juce::String();
        };

        // ---- Zutaten + Schritte ----
        auto stamp = juce::Time::getCurrentTime();

        // Gleicher Titel aktualisiert das Rezept (Entscheidung Matthias
        // 2026-10-24, vorher "-2"): Id, Bewertung, Zaehler und Rueckmeldungen
        // bleiben, der Status geht auf draft (der neue Stand ist ungeprueft)
        std::optional<Found> previous;

        for (auto& r : allRecipes (ctx))
            if (r.recipe.getProperty ("title", {}).toString().trim() == title)
            {
                previous = r;
                break;
            }

        auto id = previous ? previous->recipe.getProperty ("id", {}).toString() : uniqueId (ctx, title);
        auto keep = [&] (const juce::Identifier& key, const juce::var& v)
        {
            return previous && ! cmd.hasProperty (key) ? previous->recipe.getProperty (key, v) : v;
        };
        auto carry = [&] (const juce::Identifier& key, const juce::var& v)
        {
            return previous ? previous->recipe.getProperty (key, v) : v;
        };

        if (kind.isEmpty())
        {
            bool instrument = false;

            for (auto& m : modules)
                instrument = instrument || findRouteNode (edit, m).cls == "instrument";

            kind = modules.isEmpty() ? juce::String ("workflow") : instrument ? juce::String ("sound") : juce::String ("chain");
        }

        auto finalDir = draftsDir (ctx).getChildFile (kind).getChildFile (id);

        // Beim Aktualisieren erst neben dem Katalog bauen — das alte Rezept
        // bleibt, bis das neue ganz geschrieben ist
        auto dir = previous ? ctx.configDir.getChildFile ("recipes-update").getChildFile (id) : finalDir;

        if (previous)
            dir.deleteRecursively();

        auto stateDir = dir.getChildFile ("state");
        juce::Array<juce::var> moduleList, steps, edgesOut;

        for (int i = 0; i < modules.size(); ++i)
        {
            auto& mid = modules[i];
            auto* mt = findModuleTrack (edit, mid);
            auto* plugin = mt != nullptr ? modulePlugin (*mt) : nullptr;

            if (plugin == nullptr)
                return resultObj (false, { { "error", "plugin " + mid + " is not loaded" } });

            auto info = findById (routing.getProperty ("plugins", {}), mid);
            auto cls = info.getProperty ("class", {}).toString();
            // eingebaut: Typ-Kennung als uid ("4bandEq"), der Anzeigename der
            // Engine ("4-Band Equaliser") ist kein Name fuer addPlugin
            juce::String format = "builtin", uid = plugin->getPluginType(), name = plugin->getName();

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (plugin))
            {
                format = ep->desc.pluginFormatName;
                uid = juce::String::toHexString (ep->desc.uniqueId);
                name = ep->desc.name;
            }

            auto ref = refs[mid];
            auto stateFile = stateDir.getChildFile (ref.substring (1) + ".dawdpreset");

            if (! stateDir.createDirectory())
                return resultObj (false, { { "error", "cannot create " + stateDir.getFullPathName() } });

            if (auto err = writePresetFile (*plugin, title, stateFile); err.isNotEmpty())
                return resultObj (false, { { "error", mid + ": " + err } });

            int changed = 0;
            auto params = changedParameters (*plugin, 40, changed);
            auto master = masters.contains (mid);
            auto wet = info.getProperty ("wet", {});

            auto m = obj ({ { "ref", ref }, { "source", mid }, { "plugin", name }, { "format", format },
                            { "class", cls }, { "state", "state/" + stateFile.getFileName() },
                            { "parameters", params }, { "changedParameters", changed } });

            if (uid.isNotEmpty())
                m.getDynamicObject()->setProperty ("uid", uid);

            if (! wet.isVoid())
                m.getDynamicObject()->setProperty ("wet", wet);

            if (master)
                m.getDynamicObject()->setProperty ("master", true);

            if (! info.getProperty ("link", {}).isVoid())
                warnings.addIfNotAlreadyThere (mid + juce::String::fromUTF8 (" is linked — the recipe adds an independent copy"));

            moduleList.add (m);

            auto add = obj ({ { "name", name }, { "class", cls } });

            if (format != "builtin")
                add.getDynamicObject()->setProperty ("format", format);

            if (uid.isNotEmpty())
                add.getDynamicObject()->setProperty ("uid", uid);

            if (master)
                add.getDynamicObject()->setProperty ("at", "master");

            steps.add (step ("addPlugin", add, ref));
            steps.add (step ("loadPreset", obj ({ { "plugin", ref }, { "path", "$recipe/" + m.getProperty ("state", {}).toString() } })));

            if (wet.isDouble() || wet.isInt())
                if ((double) wet < 0.999)
                    steps.add (step ("setWet", obj ({ { "plugin", ref }, { "wet", wet } })));
        }

        // Ketten als setChain (ersetzt die Kette der Ziel-Spur sauber), der
        // Rest der Kanten einzeln
        std::set<std::pair<juce::String, juce::String>> covered;
        juce::StringArray chainTracks;   // Spuren, deren Kette das Rezept setzt

        if (auto* chains = routing.getProperty ("chains", {}).getArray())
        {
            for (auto& c : *chains)
            {
                auto tid = c.getProperty ("id", {}).toString();
                auto plugins = stringList (c.getProperty ("plugins", {}));

                if (plugins.isEmpty())
                    continue;

                bool own = true;

                for (auto& p : plugins)
                    own = own && modules.contains (p) && ! masters.contains (p);

                if (! own)
                    continue;

                // Endet die Kette vor einem Modul, in das ein Parallelweg
                // zurueckfliesst, fuehrt setChain sie bis Main weiter —
                // sonst haengte setChain ihr letztes Modul zusaetzlich an Main
                if (! (bool) c.getProperty ("toMain", false) && c.getProperty ("toMaster", {}).toString().isEmpty())
                {
                    auto path = plugins;

                    for (;;)
                    {
                        auto last = path[path.size() - 1];
                        juce::String next;
                        bool ends = false;

                        for (auto& to : targetsOf (edit, findRouteNode (edit, last)))
                        {
                            if (isSendEdge (edit, last, to))
                                continue;

                            if (to == "main" || isMasterModule (edit, to))
                                ends = true;
                            else if (next.isEmpty() && modules.contains (to) && ! path.contains (to) && ownedBy (tid, to))
                                next = to;
                        }

                        if (ends)
                            plugins = path;

                        if (ends || next.isEmpty())
                            break;

                        path.add (next);
                    }
                }

                juce::Array<juce::var> chainRefs;
                auto prev = tid;

                for (auto& p : plugins)
                {
                    chainRefs.add (refs[p]);
                    covered.insert ({ prev, p });
                    prev = p;
                }

                for (auto& to : targetsOf (edit, findRouteNode (edit, prev)))
                    if (to == "main" || masters.contains (to) || isMasterModule (edit, to))
                        covered.insert ({ prev, to });

                steps.add (step ("setChain", obj ({ { "track", useTrack (tid) }, { "plugins", chainRefs } })));
                chainTracks.add (tid);

                // setChain verbindet mit vollem Pegel
                prev = tid;

                for (auto& p : plugins)
                {
                    auto level = edgeLevel (edit, prev, p);

                    if (std::abs (level - 1.0) > 0.001)
                        steps.add (step ("setEdge", obj ({ { "from", nodeRef (prev) }, { "to", refs[p] }, { "level", level } })));

                    prev = p;
                }
            }
        }

        std::set<std::pair<juce::String, juce::String>> connected;

        if (auto* edges = routing.getProperty ("edges", {}).getArray())
        {
            for (auto& e : *edges)
            {
                auto from = e.getProperty ("from", {}).toString();
                auto to = e.getProperty ("to", {}).toString();

                if (! modules.contains (from) && ! modules.contains (to))
                    continue;

                if (covered.count ({ from, to }) > 0 || masters.contains (from))
                    continue;

                // Ins Master-Modul fuehrt addPlugin at master selbst; ein
                // Modul hinter einer Verzweigung geht wie setChain in den
                // Eingang der Master-Kette (connect auf main)
                if (masters.contains (to) || isMasterModule (edit, to))
                {
                    if (! modules.contains (from))
                        continue;

                    to = "main";
                }

                auto f = nodeRef (from), tt = nodeRef (to);

                if (f.isEmpty() || tt.isEmpty())
                {
                    warnings.add ("edge " + from + " > " + to + " left out (" + (f.isEmpty() ? from : to) + " is not in the recipe)");
                    continue;
                }

                if (! connected.insert ({ f, tt }).second)
                    continue;

                auto c = obj ({ { "from", f }, { "to", tt }, { "level", e.getProperty ("level", 1.0) } });

                if ((bool) e.getProperty ("send", false))
                    c.getDynamicObject()->setProperty ("send", true);

                steps.add (step ("connect", c));
                edgesOut.add (obj ({ { "from", f }, { "to", tt }, { "level", e.getProperty ("level", 1.0) } }));
            }
        }

        if (auto* fb = routing.getProperty ("feedback", {}).getArray())
        {
            for (auto& e : *fb)
            {
                auto from = e.getProperty ("from", {}).toString();
                auto to = e.getProperty ("to", {}).toString();

                if (! modules.contains (from) || ! modules.contains (to))
                {
                    if (modules.contains (from) || modules.contains (to))
                        warnings.add ("feedback " + from + " > " + to + " left out (one end is not in the recipe)");

                    continue;
                }

                steps.add (step ("connect", obj ({ { "from", refs[from] }, { "to", refs[to] }, { "feedback", true },
                                                   { "level", e.getProperty ("level", 0.5) },
                                                   { "delayMs", e.getProperty ("delayMs", {}) } })));
            }
        }

        // Sidechain-Schluessel in ein Modul des Rezepts (Entscheidung Matthias
        // 2026-10-24: wie Feedback mitnehmen); die Quelle muss im Rezept sein
        if (auto* sc = routing.getProperty ("sidechains", {}).getArray())
        {
            for (auto& e : *sc)
            {
                auto from = e.getProperty ("from", {}).toString();
                auto to = e.getProperty ("to", {}).toString();

                if (! modules.contains (to))
                    continue;

                auto f = nodeRef (from);

                if (f.isEmpty())
                {
                    warnings.add ("sidechain " + from + " > " + to + " left out (" + from + " is not in the recipe)");
                    continue;
                }

                steps.add (step ("connect", obj ({ { "from", f }, { "to", refs[to] }, { "sidechain", true } })));
            }
        }

        // Fader der Spuren, deren Kette das Rezept setzt (Praxistest 1j: das
        // Pad kam ohne seinen Pegel mit +10 dBTP an)
        for (auto& tid : chainTracks)
        {
            auto* fader = faderTrackOf (edit, *users[tid.substring (1).getIntValue()]);
            auto* vol = fader != nullptr ? fader->getVolumePlugin() : nullptr;

            // Der gesetzte Wert, nicht der Stand einer Automation am Playhead
            if (vol != nullptr)
                steps.add (step ("trackMix", obj ({ { "track", refs[tid] },
                                                    { "volumeDb", std::round (te::volumeFaderPositionToDB (vol->volParam->getCurrentExplicitValue()) * 100.0) / 100.0 },
                                                    { "pan", std::round (vol->panParam->getCurrentExplicitValue() * 1000.0) / 1000.0 } })));
        }

        // Automation der Module, ihrer Kanten und dieser Fader mit allen
        // Punkten — vorher kam nur der Momentanwert im Plugin-Zustand mit
        // (Praxistest 1g/1h). Zeiten in Schlaegen ab dem Beginn der Section
        // (section, sonst die, in der die Automation anfaengt)
        auto mapNode = [&] (const juce::String& n) -> juce::String
        {
            if (n.startsWithChar ('t'))
                return trackIds.contains (n) ? "t" + refs[n] : juce::String();

            auto it = refs.find (n);
            return it != refs.end() ? it->second : juce::String();
        };

        // "p3.Cutoff" -> "$p1.Cutoff", "edge:t0>p3" -> "edge:t$track>$p1",
        // "feedback:p3>p4.level" -> "feedback:$p1>$p2.level"; "" = ein Ende fehlt
        auto mapParam = [&] (const juce::String& pid) -> juce::String
        {
            if (pid.startsWith ("edge:") || pid.startsWith ("feedback:"))
            {
                auto head = pid.upToFirstOccurrenceOf (":", true, false);
                auto rest = pid.fromFirstOccurrenceOf (":", false, false);
                auto a = mapNode (rest.upToFirstOccurrenceOf (">", false, false));
                auto b = rest.fromFirstOccurrenceOf (">", false, false);
                juce::String tail;

                if (head == "feedback:")
                {
                    tail = b.fromFirstOccurrenceOf (".", true, false);
                    b = b.upToFirstOccurrenceOf (".", false, false);
                }

                b = mapNode (b);
                return a.isEmpty() || b.isEmpty() ? juce::String() : head + a + ">" + b + tail;
            }

            auto n = mapNode (pid.upToFirstOccurrenceOf (".", false, false));
            return n.isEmpty() ? juce::String() : n + pid.fromFirstOccurrenceOf (".", true, false);
        };

        juce::StringArray automationNodes (modules), automated;
        automationNodes.addArray (chainTracks);
        std::vector<std::pair<juce::String, AutomationTarget>> autos;
        double firstPoint = std::numeric_limits<double>::max();

        for (auto& n : automationNodes)
        {
            for (auto& at : listAutomationTargets (edit, n, true))
            {
                if (at.bindings.empty() || at.bindings.front().param->getCurve().getNumPoints() == 0 || automated.contains (at.id))
                    continue;

                automated.add (at.id);
                auto param = mapParam (at.id);

                if (param.isEmpty())
                {
                    warnings.add ("automation " + at.id + " left out (one end is not in the recipe)");
                    continue;
                }

                firstPoint = juce::jmin (firstPoint, at.bindings.front().param->getCurve().getPointTime (0).inSeconds());
                autos.push_back ({ param, at });
            }
        }

        auto* autoSection = autos.empty() ? nullptr : srcSection != nullptr ? srcSection : sectionUnder (edit, firstPoint);
        const double secStart = autoSection != nullptr ? autoSection->getPosition().getStart().inSeconds() : 0.0;
        const double secEnd = autoSection != nullptr ? autoSection->getPosition().getEnd().inSeconds() : 0.0;

        for (auto& [param, at] : autos)
            steps.add (step ("setAutomation", obj ({ { "param", param },
                                                     { "points", pointsInBeats (edit, at, readAutomation (at), secStart, secEnd, srcSection != nullptr) } })));

        // LFO/Step je Ziel: Einstellungen ohne Zeitbezug (laeuft mit der Songzeit)
        juce::StringArray modulated;

        for (auto& n : automationNodes)
            for (auto& at : listAutomationTargets (edit, n, true))
            {
                auto m = describeModifier (at);

                if (m.isVoid() || modulated.contains (at.id))
                    continue;

                modulated.add (at.id);
                auto param = mapParam (at.id);

                if (param.isEmpty())
                {
                    warnings.add ((m["type"].toString() == "step" ? "Step modifier on " : "LFO on ") + at.id + " left out (one end is not in the recipe)");
                    continue;
                }

                // Alle Einstellungen des Typs (lfo: wave/phase, step: steps)
                auto params = obj ({ { "param", param } });

                for (auto& nv : m.getDynamicObject()->getProperties())
                    if (nv.name.toString() != "low" && nv.name.toString() != "high")
                        params.getDynamicObject()->setProperty (nv.name, nv.value);

                steps.add (step ("setModifier", params));
            }

        for (auto& s : extraSteps)
            steps.add (s);

        // Spuren, die das Rezept braucht (applyRecipe nimmt track/tracks
        // oder legt sie an)
        juce::Array<juce::var> trackList;

        for (auto& tid : trackIds)
        {
            auto src = findById (routing.getProperty ("sources", {}), tid);
            trackList.add (obj ({ { "ref", refs[tid] }, { "type", src.getProperty ("signal", "audio") },
                                  { "source", tid.substring (1).getIntValue() } }));
        }

        juce::StringArray pluginNames;

        for (auto& m : moduleList)
            pluginNames.addIfNotAlreadyThere (m.getProperty ("plugin", {}).toString());

        auto origin = obj ({ { "project", t.projectDir.getFullPathName() }, { "node", t.current },
                             { "label", node->label }, { "note", node->note.substring (0, 600) },
                             { "scope", scope }, { "created", stamp.toISO8601 (true) },
                             { "author", cmd.getProperty ("author", "agent") } });

        // Quell-Section der Automation (Zeiten der setAutomation-Schritte
        // in Schlaegen ab ihrem Beginn)
        if (autoSection != nullptr)
            origin.getDynamicObject()->setProperty ("section", autoSection->getName());

        if (previous)
        {
            auto old = previous->recipe.getProperty ("origin", {});
            origin.getDynamicObject()->setProperty ("created", old.getProperty ("created", stamp.toISO8601 (true)));
            origin.getDynamicObject()->setProperty ("updated", stamp.toISO8601 (true));
        }

        // version 2: setAutomation-Punkte {beat} relativ zur Section (1: {time} in Songzeit)
        auto recipe = obj ({ { "version", 2 }, { "id", id }, { "title", title }, { "kind", kind }, { "goal", goal },
                             { "tags", keep ("tags", toVarArray (stringList (cmd.getProperty ("tags", {})))) },
                             { "status", "draft" },
                             { "plugins", toVarArray (pluginNames) },
                             { "tracks", trackList }, { "modules", moduleList }, { "edges", edgesOut },
                             { "steps", steps },
                             { "notes", keep ("notes", cmd.getProperty ("notes", {}).toString()) },
                             { "variants", keep ("variants", toVarArray (stringList (cmd.getProperty ("variants", {})))) },
                             { "warnings", toVarArray (warnings) },
                             { "origin", origin },
                             { "rating", carry ("rating", obj ({ { "stars", 0 }, { "comment", "" } })) },
                             { "stats", carry ("stats", obj ({ { "applied", 0 }, { "used", 0 }, { "ok", 0 } })) },
                             { "feedback", carry ("feedback", juce::Array<juce::var>()) } });

        // Vorhoeren (Rezept-Strip, Freigabe durch Matthias): die Spur ohne
        // Master-Kette, in der Section (section) oder die ersten 20 s
        juce::String preview;

        if (scope == "track")
        {
            auto pc = obj ({ { "track", cmd.getProperty ("track", -1) } });

            if (cmd.hasProperty ("section"))
                pc.getDynamicObject()->setProperty ("section", cmd.getProperty ("section", {}));
            else
            {
                pc.getDynamicObject()->setProperty ("start", 0.0);
                pc.getDynamicObject()->setProperty ("end", juce::jmin (20.0, edit.getLength().inSeconds()));
            }

            double s0 = 0.0, s1 = 0.0;
            int tr = -1;
            dir.createDirectory();

            if (renderMixRange (ctx, pc, dir.getChildFile ("preview.wav"), s0, s1, tr).isEmpty()
                && normalisePreview (ctx, dir.getChildFile ("preview.wav")))
            {
                preview = "preview.wav";
                recipe.getDynamicObject()->setProperty ("preview", preview);
            }
        }

        if (! writeRecipe (dir, recipe))
            return resultObj (false, { { "error", "cannot write " + dir.getFullPathName() } });

        // Das alte Rezept (auch ein geprueftes im Repo) weicht dem neuen Entwurf
        if (previous)
        {
            previous->dir.deleteRecursively();

            if (! moveDir (dir, finalDir))
                return resultObj (false, { { "error", "cannot move the recipe to " + finalDir.getFullPathName() } });
        }

        auto res = resultObj (true, { { "id", id }, { "kind", kind }, { "status", "draft" }, { "dir", finalDir.getFullPathName() },
                                      { "modules", moduleList.size() }, { "steps", steps.size() },
                                      { "tracks", trackList.size() }, { "updated", previous.has_value() },
                                      { "warnings", toVarArray (warnings) } });

        if (autoSection != nullptr)
            res.getDynamicObject()->setProperty ("section", autoSection->getName());

        if (preview.isNotEmpty())
            res.getDynamicObject()->setProperty ("preview", finalDir.getChildFile (preview).getFullPathName());

        return res;
    }

    // ---- recipeInfo / findRecipes ---------------------------------------------

    juce::var opRecipeInfo (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("id", {}).toString();
        auto found = findRecipe (ctx, id);

        if (! found)
            return resultObj (false, { { "error", "no such recipe: " + id + " (see findRecipes)" } });

        return resultObj (true, { { "recipe", found->recipe }, { "dir", found->dir.getFullPathName() }, { "where", found->where } });
    }

    juce::var opFindRecipes (DawdContext& ctx, const juce::var& cmd)
    {
        auto kind = cmd.getProperty ("kind", {}).toString();
        auto status = cmd.getProperty ("status", {}).toString();
        auto plugin = cmd.getProperty ("plugin", {}).toString().toLowerCase();
        auto tags = stringList (cmd.getProperty ("tags", {}));
        const int limit = juce::jlimit (1, 200, (int) cmd.getProperty ("limit", 20));

        if (kind.isNotEmpty() && ! recipeKinds.contains (kind))
            return resultObj (false, { { "error", "kind must be one of: " + recipeKinds.joinIntoString (", ") } });

        if (status.isNotEmpty() && status != "all" && ! recipeStatuses.contains (status))
            return resultObj (false, { { "error", "status must be one of: " + recipeStatuses.joinIntoString (", ") + ", all" } });

        juce::StringArray words;
        words.addTokens (cmd.getProperty ("query", {}).toString().toLowerCase(), " ,_-/", "");
        words.removeEmptyStrings();

        struct Hit { Found f; int matched; double score; };
        std::vector<Hit> hits;
        int total = 0;

        for (auto& f : allRecipes (ctx))
        {
            auto& r = f.recipe;
            ++total;
            auto rs = r.getProperty ("status", "draft").toString();

            if (status.isEmpty() ? rs == "rejected" : (status != "all" && rs != status))
                continue;

            if (kind.isNotEmpty() && r.getProperty ("kind", {}).toString() != kind)
                continue;

            auto rTags = stringList (r.getProperty ("tags", {}));
            bool tagsOk = true;

            for (auto& tg : tags)
                tagsOk = tagsOk && rTags.contains (tg, true);

            if (! tagsOk)
                continue;

            auto plugins = stringList (r.getProperty ("plugins", {})).joinIntoString (" ").toLowerCase();

            if (plugin.isNotEmpty() && ! plugins.contains (plugin))
                continue;

            auto titleL = r.getProperty ("title", {}).toString().toLowerCase();
            auto goalL = r.getProperty ("goal", {}).toString().toLowerCase();
            auto tagsL = rTags.joinIntoString (" ").toLowerCase();
            juce::String rest = r.getProperty ("notes", {}).toString().toLowerCase();

            if (auto* st = r.getProperty ("steps", {}).getArray())
                for (auto& s : *st)
                    rest << " " << s.getProperty ("text", {}).toString().toLowerCase() << " " << s.getProperty ("op", {}).toString().toLowerCase();

            int matched = 0;
            double score = 0.0;

            for (auto& w : words)
            {
                double s = (titleL.contains (w) ? 3.0 : 0.0) + (tagsL.contains (w) ? 2.0 : 0.0)
                         + (goalL.contains (w) ? 2.0 : 0.0) + (plugins.contains (w) ? 1.5 : 0.0)
                         + (rest.contains (w) ? 1.0 : 0.0);

                if (s > 0.0)
                    ++matched;

                score += s;
            }

            if (! words.isEmpty() && matched == 0)
                continue;

            // Bewaehrtes zuerst: geprueft, Sterne, Erfolge
            auto stats = r.getProperty ("stats", {});
            auto used = (double) stats.getProperty ("used", 0);
            score += (rs == "checked" ? 1.0 : 0.0) + 0.2 * (double) r.getProperty ("rating", {}).getProperty ("stars", 0)
                   + (used > 0 ? (double) stats.getProperty ("ok", 0) / used : 0.0);

            hits.push_back ({ f, matched, score });
        }

        std::sort (hits.begin(), hits.end(), [] (auto& a, auto& b)
        {
            return a.matched != b.matched ? a.matched > b.matched : a.score > b.score;
        });

        juce::Array<juce::var> list;

        for (size_t i = 0; i < hits.size() && (int) i < limit; ++i)
        {
            auto& r = hits[i].f.recipe;
            int stepCount = 0;

            if (auto* st = r.getProperty ("steps", {}).getArray())
                stepCount = st->size();

            list.add (obj ({ { "id", r.getProperty ("id", {}) }, { "title", r.getProperty ("title", {}) },
                             { "kind", r.getProperty ("kind", {}) }, { "goal", r.getProperty ("goal", {}) },
                             { "tags", r.getProperty ("tags", {}) }, { "plugins", r.getProperty ("plugins", {}) },
                             { "status", r.getProperty ("status", {}) },
                             { "stars", r.getProperty ("rating", {}).getProperty ("stars", 0) },
                             { "stats", r.getProperty ("stats", {}) }, { "steps", stepCount },
                             { "where", hits[i].f.where } }));

            if (auto pv = hits[i].f.dir.getChildFile ("preview.wav"); pv.existsAsFile())
                list.getReference (list.size() - 1).getDynamicObject()->setProperty ("preview", pv.getFullPathName());
        }

        auto res = resultObj (true, { { "recipes", list }, { "matched", (int) hits.size() }, { "total", total } });

        // Leerer Katalog: einmal deutlich sagen statt weiter suchen lassen
        // (Praxistest 1c: drei Suchen ins Leere)
        if (total == 0)
            res.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("the recipe catalog is empty — no recipes yet; build from presets (findPresets) and plugin profiles (pluginInfo) instead"));

        return res;
    }

    // Rezepte mit diesem Plugin fuer pluginInfo (der Steckbrief zeigt, wo es
    // vorkommt): ohne verworfene, Gepruefte und Sterne zuerst
    juce::var recipesWithPlugin (DawdContext& ctx, const juce::String& format, const juce::String& uid, int limit)
    {
        std::vector<Found> hits;

        for (auto& f : allRecipes (ctx))
        {
            if (f.recipe.getProperty ("status", "draft").toString() == "rejected")
                continue;

            bool uses = false;

            if (auto* mods = f.recipe.getProperty ("modules", {}).getArray())
                for (auto& m : *mods)
                    uses = uses || (m.getProperty ("format", {}).toString() == format
                                    && m.getProperty ("uid", {}).toString().equalsIgnoreCase (uid));

            if (uses)
                hits.push_back (f);
        }

        auto rank = [] (const juce::var& r)
        {
            return (r.getProperty ("status", {}).toString() == "checked" ? 10.0 : 0.0)
                 + (double) r.getProperty ("rating", {}).getProperty ("stars", 0);
        };

        std::stable_sort (hits.begin(), hits.end(), [&] (auto& a, auto& b) { return rank (a.recipe) > rank (b.recipe); });
        juce::Array<juce::var> list;

        for (size_t i = 0; i < hits.size() && (int) i < limit; ++i)
        {
            auto& r = hits[i].recipe;
            list.add (obj ({ { "id", r.getProperty ("id", {}) }, { "title", r.getProperty ("title", {}) },
                             { "kind", r.getProperty ("kind", {}) }, { "status", r.getProperty ("status", {}) },
                             { "stars", r.getProperty ("rating", {}).getProperty ("stars", 0) } }));
        }

        return list;
    }

    // ---- applyRecipe ----------------------------------------------------------

    juce::var opApplyRecipe (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("id", {}).toString();
        auto found = findRecipe (ctx, id);

        if (! found)
            return resultObj (false, { { "error", "no such recipe: " + id + " (see findRecipes)" } });

        auto recipe = found->recipe;
        auto* steps = recipe.getProperty ("steps", {}).getArray();
        juce::Array<juce::var> manual;
        int executable = 0;

        if (steps != nullptr)
            for (auto& s : *steps)
                if (s.getProperty ("op", {}).toString().isNotEmpty())
                    ++executable;
                else
                    manual.add (s.getProperty ("text", {}));

        if (executable == 0)
            return resultObj (false, { { "error", "recipe has no executable steps - follow its steps by hand (recipeInfo)" },
                                       { "manual", manual } });

        Bindings b;
        bindValue (b, "$recipe", found->dir.getFullPathName());

        if (auto* o = cmd.getProperty ("params", {}).getDynamicObject())
            for (auto& p : o->getProperties())
                bindValue (b, "$" + p.name.toString(), p.value);

        // Spuren: track (erste), tracks (alle) oder neu anlegen
        juce::Array<juce::var> given;

        if (auto* arr = cmd.getProperty ("tracks", {}).getArray())
            given = *arr;
        else if (cmd.hasProperty ("track"))
            given.add (cmd.getProperty ("track", {}));

        auto* trackList = recipe.getProperty ("tracks", {}).getArray();
        auto users = userTracks (*ctx.edit);

        if (trackList != nullptr)
        {
            for (int i = 0; i < juce::jmin (given.size(), trackList->size()); ++i)
            {
                auto idx = (int) (double) given[i];
                auto type = trackList->getReference (i).getProperty ("type", "audio").toString();

                if (idx < 0 || idx >= users.size())
                    return resultObj (false, { { "error", "no such track: " + given[i].toString() } });

                if ((type == "midi") != isMidiTrack (*users[idx]))
                    return resultObj (false, { { "error", "track " + juce::String (idx) + " is not a " + type + " track (the recipe needs one)" } });
            }
        }

        // Automation relativ zur Section (Rezept v2) beginnt am Anfang der
        // Ziel-Section: section, sonst die unter dem Playhead
        bool relative = false;

        for (auto& s : *steps)
            relative = relative || hasBeatPoints (s);

        juce::String targetSection;

        if (cmd.hasProperty ("section"))
        {
            targetSection = cmd.getProperty ("section", {}).toString();

            if (findSection (*ctx.edit, targetSection) == nullptr)
                return resultObj (false, { { "error", "no such section: " + targetSection } });
        }
        else if (relative)
        {
            if (auto* sec = sectionUnder (*ctx.edit, ctx.edit->getTransport().getPosition().inSeconds()))
                targetSection = sec->getName();
        }

        // Neuer Zweig, damit der Vergleich bleibt — ausser der Node ist noch
        // leer (frischer Seed oder frischer Zweig) oder der Auto-Branch ist
        // aus (treeSettings, Praxistest 1i); branch entscheidet ausdruecklich
        auto& t = *ctx.tree;
        auto parentNode = t.current;
        int created = 0;
        auto* cur = t.find (t.current);

        if ((bool) cmd.getProperty ("branch", t.autoBranch) && cur != nullptr && cur->isActed())
        {
            auto r = executeOp (ctx, "treeBranch", obj ({ { "label", juce::String ("Recipe: ") + recipe.getProperty ("title", {}).toString() } }));

            if (! (bool) r.getProperty ("ok", false))
                return r;

            created = (int) r.getProperty ("node", 0);
        }

        // Die Schritte gehoeren zum einen Node — kein Auto-Branch dazwischen
        struct AutoBranchOff
        {
            TreeState& t;
            bool was;
            ~AutoBranchOff() { t.autoBranch = was; }
        } autoBranchOff { t, t.autoBranch };

        t.autoBranch = false;

        auto fail = [&] (const juce::var& err)
        {
            // Zweig wieder weg: zurueck zum Eltern-Node, Zweig loeschen
            if (created > 0)
            {
                executeOp (ctx, "treeCheckout", obj ({ { "node", parentNode } }));
                executeOp (ctx, "treeDelete", obj ({ { "node", created } }));
                err.getDynamicObject()->setProperty ("rolledBack", true);
            }

            return err;
        };

        juce::StringArray givenRefs;   // Refs auf bestehende Spuren

        if (trackList != nullptr)
        {
            for (int i = 0; i < trackList->size(); ++i)
            {
                auto& tr = trackList->getReference (i);
                auto ref = tr.getProperty ("ref", {}).toString();

                if (i < given.size())
                {
                    bindValue (b, ref, (int) (double) given[i]);
                    givenRefs.add (ref);
                    continue;
                }

                auto r = executeOp (ctx, "createTrack", obj ({ { "type", tr.getProperty ("type", "audio") } }));

                if (! (bool) r.getProperty ("ok", false))
                    return fail (resultObj (false, { { "error", "cannot create track for " + ref + ": " + r.getProperty ("error", {}).toString() } }));

                bindValue (b, ref, r.getProperty ("index", 0));
            }
        }

        juce::Array<juce::var> done, keptFaders;
        int n = 0;

        for (auto& s : *steps)
        {
            ++n;
            auto op = s.getProperty ("op", {}).toString();

            if (op.isEmpty())
                continue;

            // Eine bestehende Spur behaelt ihren Fader (Entscheidung Matthias
            // 2026-10-24); den Fader des Rezepts bekommen nur neue Spuren
            if (op == "trackMix" && givenRefs.contains (s.getProperty ("params", {}).getProperty ("track", {}).toString()))
            {
                keptFaders.addIfNotAlreadyThere (substitute (s.getProperty ("params", {}).getProperty ("track", {}), b));
                continue;
            }

            // ... samt Lautstaerke- und Pan-Automation und LFO ("t$track.volume",
            // Entscheidung Matthias 2026-10-24)
            if (auto param = s.getProperty ("params", {}).getProperty ("param", {}).toString();
                (op == "setAutomation" || op == "setModifier") && param.startsWithChar ('t')
                && givenRefs.contains (param.substring (1).upToFirstOccurrenceOf (".", false, false)))
            {
                keptFaders.addIfNotAlreadyThere (substitute (param.substring (1).upToFirstOccurrenceOf (".", false, false), b));
                continue;
            }

            auto params = substitute (s.getProperty ("params", {}), b);

            if (auto u = unbound (params); u.isNotEmpty())
                return fail (resultObj (false, { { "error", "step " + juce::String (n) + " (" + op + "): unbound placeholder " + u
                                                            + " - pass params {" + u.substring (1) + ": ...}" },
                                                 { "step", n }, { "op", op } }));

            if (params.getDynamicObject() == nullptr)
                params = juce::var (new juce::DynamicObject());

            if (hasBeatPoints (s))
            {
                auto* sec = findSection (*ctx.edit, targetSection);
                params.getDynamicObject()->setProperty ("points", pointsInSeconds (*ctx.edit, params.getProperty ("points", {}),
                                                                                  sec != nullptr ? sec->getPosition().getStart().inSeconds() : 0.0));
            }

            auto r = executeOp (ctx, op, params);

            if (! (bool) r.getProperty ("ok", false))
            {
                if ((bool) s.getProperty ("optional", false))
                    continue;

                auto err = resultObj (false, { { "error", "step " + juce::String (n) + " (" + op + ") failed: " + r.getProperty ("error", {}).toString() },
                                               { "step", n }, { "op", op }, { "params", params },
                                               { "variants", recipe.getProperty ("variants", {}) } });

                if (op == "addPlugin")
                    err.getDynamicObject()->setProperty ("hint", "plugin missing here? see listPlugins, or try a variant recipe");

                if (created == 0)
                    err.getDynamicObject()->setProperty ("applied", done);

                return fail (err);
            }

            if (auto bind = s.getProperty ("bind", {}).toString(); bind.isNotEmpty())
                bindValue (b, bind, r.hasProperty ("id") ? r.getProperty ("id", {}) : r.getProperty ("index", {}));

            done.add (op);
        }

        // Am Node festhalten, welches Rezept hier lief; Zaehler hochsetzen
        auto action = obj ({ { "id", id }, { "title", recipe.getProperty ("title", {}) } });

        if (relative)
            action.getDynamicObject()->setProperty ("section", targetSection);

        recordNodeAction (ctx, "process", "applyRecipe", action);

        if (auto* stats = recipe.getProperty ("stats", {}).getDynamicObject())
            stats->setProperty ("applied", (int) stats->getProperty ("applied") + 1);

        writeRecipe (found->dir, recipe);

        auto bound = new juce::DynamicObject();

        for (auto& [k, v] : b)
            if (k != "$recipe")
                bound->setProperty (k, v);

        auto r = resultObj (true, { { "recipe", id }, { "node", t.current }, { "branched", created > 0 },
                                    { "parent", parentNode }, { "bound", juce::var (bound) },
                                    { "steps", done.size() }, { "manual", manual } });

        if (relative)
            r.getDynamicObject()->setProperty ("section", targetSection);

        if (! keptFaders.isEmpty())
            r.getDynamicObject()->setProperty ("keptFader", keptFaders);

        return r;
    }

    // ---- rateRecipe -----------------------------------------------------------

    juce::var opRateRecipe (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("id", {}).toString();
        auto found = findRecipe (ctx, id);

        if (! found)
            return resultObj (false, { { "error", "no such recipe: " + id + " (see findRecipes)" } });

        auto recipe = found->recipe;
        auto* r = recipe.getDynamicObject();
        bool any = false;

        // Rueckmeldung aus der Nutzung (Agent): used, ok, note
        if (cmd.hasProperty ("used") || cmd.hasProperty ("ok") || cmd.hasProperty ("note"))
        {
            any = true;
            auto stats = recipe.getProperty ("stats", {});

            if (stats.getDynamicObject() == nullptr)
            {
                stats = obj ({ { "applied", 0 }, { "used", 0 }, { "ok", 0 } });
                r->setProperty ("stats", stats);
            }

            auto* s = stats.getDynamicObject();

            // Eine Notiz allein ist keine Nutzung; ok zaehlt als Nutzung
            if ((bool) cmd.getProperty ("used", cmd.hasProperty ("ok")))
            {
                s->setProperty ("used", (int) s->getProperty ("used") + 1);

                if ((bool) cmd.getProperty ("ok", false))
                    s->setProperty ("ok", (int) s->getProperty ("ok") + 1);
            }

            auto fb = recipe.getProperty ("feedback", {});

            if (fb.getArray() == nullptr)
            {
                fb = juce::Array<juce::var>();
                r->setProperty ("feedback", fb);
            }

            fb.getArray()->add (obj ({ { "date", juce::Time::getCurrentTime().toISO8601 (true) },
                                       { "ok", cmd.getProperty ("ok", {}) },
                                       { "note", cmd.getProperty ("note", {}).toString().substring (0, 1000) } }));

            while (fb.getArray()->size() > 50)
                fb.getArray()->remove (0);
        }

        // Bewertung (Mensch): stars 1..5, comment, status
        if (cmd.hasProperty ("stars") || cmd.hasProperty ("comment"))
        {
            any = true;
            auto rating = recipe.getProperty ("rating", {});

            if (rating.getDynamicObject() == nullptr)
            {
                rating = obj ({ { "stars", 0 }, { "comment", "" } });
                r->setProperty ("rating", rating);
            }

            if (cmd.hasProperty ("stars"))
            {
                auto stars = (int) (double) cmd.getProperty ("stars", 0.0);

                if (stars < 1 || stars > 5)
                    return resultObj (false, { { "error", "stars must be 1..5" } });

                rating.getDynamicObject()->setProperty ("stars", stars);
            }

            if (cmd.hasProperty ("comment"))
                rating.getDynamicObject()->setProperty ("comment", cmd.getProperty ("comment", {}).toString().substring (0, 2000));
        }

        auto dir = found->dir;

        if (cmd.hasProperty ("status"))
        {
            any = true;
            auto status = cmd.getProperty ("status", {}).toString();

            if (! recipeStatuses.contains (status))
                return resultObj (false, { { "error", "status must be one of: " + recipeStatuses.joinIntoString (", ") } });

            r->setProperty ("status", status);

            // Gepruefte Rezepte liegen im Repo, alle anderen lokal
            auto root = status == "checked" ? ctx.recipesDir : draftsDir (ctx);
            auto target = root.getChildFile (recipe.getProperty ("kind", "chain").toString()).getChildFile (id);

            if (target != dir)
            {
                if (target.exists())
                    return resultObj (false, { { "error", "already exists: " + target.getFullPathName() } });

                if (! moveDir (dir, target))
                    return resultObj (false, { { "error", "cannot move the recipe to " + target.getFullPathName() } });

                dir = target;
            }
        }

        if (! any)
            return resultObj (false, { { "error", "nothing to rate - give used/ok/note (use) or stars/comment/status (review)" } });

        if (! writeRecipe (dir, recipe))
            return resultObj (false, { { "error", "cannot write " + dir.getFullPathName() } });

        return resultObj (true, { { "id", id }, { "status", recipe.getProperty ("status", {}) },
                                  { "rating", recipe.getProperty ("rating", {}) }, { "stats", recipe.getProperty ("stats", {}) },
                                  { "dir", dir.getFullPathName() } });
    }
}
