#include "Ops.h"
#include "../Presets.h"
#include "../Routing.h"
#include "../EngineHelpers.h"
#include "../PluginProfiles.h"
#include "../Clap.h"
#include "../PluginSettings.h"

// Presets (Familie processing): listPresets sucht, loadPreset laedt (wie
// setParameter: keine Node-Aktion, aber Undo), savePreset legt den Zustand
// eines Moduls als dawd-Preset ab (<config>/presets/, fuer alle Projekte).
// findPresets sucht zusaetzlich nach Klang: Klasse und Merkmalsworte aus dem
// Preset-Katalog (plugin-profiles/<format>-<uid>-presets.json,
// tools/profile_presets.py), auch ueber alle katalogisierten Plugins.
namespace dawd
{
    namespace
    {
        te::Plugin* presetPlugin (DawdContext& ctx, const juce::var& cmd, juce::String& id)
        {
            id = cmd.getProperty ("plugin", {}).toString();

            if (auto* m = findModuleTrack (*ctx.edit, id))
                return modulePlugin (*m);

            return nullptr;
        }

        juce::var entryVar (const PresetEntry& e)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("name", e.name);
            o->setProperty ("category", e.category);
            o->setProperty ("source", e.source);

            if (e.source == "program")
                o->setProperty ("program", e.program);
            else
                o->setProperty ("path", e.file.getFullPathName());

            // Datei mit mehreren Klaengen (DX7-Cartridge): welcher
            if (e.source == "file" && e.program >= 0)
                o->setProperty ("program", e.program);

            return juce::var (o);
        }

        juce::var loadSound (DawdContext& ctx, te::Plugin& plugin, const juce::String& id, const juce::String& soundId);

        // Alle Worte der Anfrage in Kategorie oder Name
        bool matches (const PresetEntry& e, const juce::StringArray& words)
        {
            auto hay = (e.category + " " + e.name).toLowerCase();

            for (auto& w : words)
                if (! hay.contains (w))
                    return false;

            return true;
        }
    }

    juce::var opListPresets (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id;
        auto* plugin = presetPlugin (ctx, cmd, id);

        if (plugin == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        auto words = juce::StringArray::fromTokens (cmd.getProperty ("query", {}).toString().toLowerCase(), true);
        words.removeEmptyStrings();
        auto category = cmd.getProperty ("category", {}).toString();
        auto limit = juce::jlimit (1, 500, (int) cmd.getProperty ("limit", 50));
        auto offset = juce::jmax (0, (int) cmd.getProperty ("offset", 0));

        auto all = presetsFor (ctx, *plugin);
        std::map<juce::String, int> categories;
        juce::Array<juce::var> list;
        int total = 0;

        for (auto& e : all)
        {
            categories[e.category]++;

            if ((category.isNotEmpty() && ! e.category.equalsIgnoreCase (category)) || ! matches (e, words))
                continue;

            if (total >= offset && list.size() < limit)
                list.add (entryVar (e));

            ++total;
        }

        juce::Array<juce::var> cats;

        for (auto& [name, count] : categories)
            cats.add (resultObj (true, { { "name", name }, { "count", count } }));

        for (auto& c : cats)
            c.getDynamicObject()->removeProperty ("ok");

        return resultObj (true, { { "plugin", id }, { "pluginName", plugin->getName() }, { "total", total },
                                  { "presets", list }, { "categories", cats } });
    }

    juce::var opLoadPreset (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id;
        auto* plugin = presetPlugin (ctx, cmd, id);

        if (plugin == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        if (cmd.hasProperty ("sound"))
            return loadSound (ctx, *plugin, id, cmd.getProperty ("sound", {}).toString());

        auto all = presetsFor (ctx, *plugin);
        std::vector<PresetEntry> hits;

        if (cmd.hasProperty ("path"))
        {
            auto path = cmd.getProperty ("path", {}).toString();
            juce::File f;

            // relativ (Pfad aus findPresets ueber die Kataloge): im Preset-Ordner des Plugins
            if (juce::File::isAbsolutePath (path))
                f = juce::File (path);
            else
                for (auto& x : all)
                    if (matchesCatalogPath (x.file, path))
                    {
                        f = x.file;
                        break;
                    }

            if (! f.existsAsFile())
                return resultObj (false, { { "error", "no such file: " + (f == juce::File() ? path : f.getFullPathName()) } });

            PresetEntry e;
            e.name = f.getFileNameWithoutExtension();
            e.source = f.hasFileExtension ("dawdpreset") ? "dawd" : "file";
            e.file = f;
            e.program = (int) cmd.getProperty ("program", -1);   // Stimme einer Cartridge

            // Cartridge-Stimme: Name und Kategorie wie in der Liste
            for (auto& x : all)
                if (e.program >= 0 && x.file == f && x.program == e.program)
                    e = x;

            hits.push_back (e);
        }
        else if (cmd.hasProperty ("program"))
        {
            for (auto& e : all)
                if (e.source == "program" && e.program == (int) cmd.getProperty ("program", -1))
                    hits.push_back (e);
        }
        else
        {
            // name: "Name" oder "Kategorie/Name"; erst exakt, dann eindeutig per Wortsuche
            auto name = cmd.getProperty ("name", {}).toString().trim();

            if (name.isEmpty())
                return resultObj (false, { { "error", "need name, path or program (see listPresets)" } });

            auto cat = name.containsChar ('/') ? name.upToLastOccurrenceOf ("/", false, false) : juce::String();
            auto bare = name.containsChar ('/') ? name.fromLastOccurrenceOf ("/", false, false) : name;

            for (auto& e : all)
                if (e.name.equalsIgnoreCase (bare) && (cat.isEmpty() || e.category.equalsIgnoreCase (cat)))
                    hits.push_back (e);

            if (hits.empty())
            {
                auto words = juce::StringArray::fromTokens (name.toLowerCase().replaceCharacter ('/', ' '), true);
                words.removeEmptyStrings();

                for (auto& e : all)
                    if (matches (e, words))
                        hits.push_back (e);
            }
        }

        if (hits.empty())
            return resultObj (false, { { "error", "no such preset (see listPresets)" } });

        if (hits.size() > 1)
        {
            juce::StringArray names;

            for (size_t i = 0; i < juce::jmin ((size_t) 10, hits.size()); ++i)
                names.add (hits[i].category.isNotEmpty() ? hits[i].category + "/" + hits[i].name : hits[i].name);

            return resultObj (false, { { "error", juce::String ((int) hits.size()) + juce::String::fromUTF8 (" presets match — be exact (Category/Name or path): ")
                                                       + names.joinIntoString (", ") } });
        }

        int changed = 0;
        bool stateChanged = false;

        if (auto err = loadPresetEntry (ctx, *plugin, hits.front(), changed, &stateChanged); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        saveCurrentEdit (ctx);
        auto res = resultObj (true, { { "plugin", id }, { "preset", entryVar (hits.front()) }, { "changedParameters", changed } });

        if (auto mono = monoModeOf (*plugin); mono.isNotEmpty())
            res.getDynamicObject()->setProperty ("plays", juce::String::fromUTF8 ("one note at a time (") + mono + juce::String::fromUTF8 (") — chords need Poly"));

        // Praxistest 1c: Surge XT meldete 0, obwohl der Klang wechselte. Seit
        // dem Abgleich des VST3-Controllers (syncPluginParameters, Todo
        // 2026-10-27-surge-werte-nach-loadpreset) zeigt es die Werte sofort;
        // bleibt es bei 0, liegt der Unterschied ausserhalb der Parameter
        // oder das Plugin meldet spaet (andere Formate)
        if (changed == 0 && dynamic_cast<te::ExternalPlugin*> (plugin) != nullptr)
            res.getDynamicObject()->setProperty ("note", stateChanged
                ? juce::String::fromUTF8 ("preset loaded (plugin state changed), but no parameter shows a new value — the preset differs outside the parameters, or the plugin reports its values late")
                : juce::String::fromUTF8 ("nothing changed — the preset equals the current sound"));

        return res;
    }

    juce::var opSavePreset (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id;
        auto* plugin = presetPlugin (ctx, cmd, id);

        if (plugin == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        juce::File file;

        if (auto err = savePresetFile (ctx, *plugin, cmd.getProperty ("name", {}).toString(),
                                       cmd.getProperty ("category", {}).toString(), file); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        return resultObj (true, { { "plugin", id }, { "path", file.getFullPathName() } });
    }
}

namespace dawd
{
    // Datei setzen, die ein Plugin laedt (sfizz: .sfz-Instrument) — Undo wie loadPreset
    juce::var opSetPluginFile (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id;
        auto* plugin = presetPlugin (ctx, cmd, id);

        if (plugin == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        juce::File f (cmd.getProperty ("file", {}).toString());

        if (! f.existsAsFile())
            return resultObj (false, { { "error", "no such file: " + f.getFullPathName() } });

        juce::String kind;

        if (auto err = setPluginFile (*plugin, f, kind); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        saveCurrentEdit (ctx);
        return resultObj (true, { { "plugin", id }, { "file", f.getFullPathName() }, { "kind", kind } });
    }

    namespace
    {
        // Preset-Kataloge (gecacht, neu gelesen wenn sich die Datei aendert)
        struct Catalog
        {
            juce::Time modified;
            juce::var data;
        };

        const juce::var& catalogFile (const juce::File& f)
        {
            static std::map<juce::String, Catalog> cache;
            auto& c = cache[f.getFullPathName()];

            if (c.modified != f.getLastModificationTime())
                c = { f.getLastModificationTime(), juce::JSON::parse (f) };

            return c.data;
        }

        juce::Array<juce::File> catalogFiles()
        {
            return profilesDirectory().findChildFiles (juce::File::findFiles, false, "*-presets.json");
        }

        juce::String catalogKey (const juce::String& category, const juce::String& name)
        {
            return category.toLowerCase() + "/" + name.toLowerCase();
        }

        // Suchworte -> Klasse oder Merkmalswort der Kataloge, eine Tabelle
        // fuer query und text. Die Klassenworte folgen CLASS_HINTS in
        // tools/profile_presets.py ("electric piano" findet so alle keys,
        // nicht nur Presets mit beiden Worten im Namen).
        const std::map<juce::String, juce::String>& presetSynonyms()
        {
            static const std::map<juce::String, juce::String> table = []
            {
                const std::vector<std::pair<const char*, std::vector<const char*>>> groups {
                    { "keys", { "keys", "keyboard", "keyboards", "piano", "pianos", "pianet", "electric piano", "e piano",
                                "e-piano", "e.piano", "epiano", "ep", "rhodes", "wurli", "wurlitzer", "clav", "clavinet",
                                "harpsichord" } },
                    { "organ", { "organ", "organs", "hammond" } },
                    { "strings", { "string", "strings", "violin", "violins", "cello", "orchestra", "orchestral" } },
                    { "pad", { "pad", "pads" } },
                    { "lead", { "lead", "leads" } },
                    { "bass", { "bass", "basses", "bassline", "sub", "sub bass" } },
                    { "pluck", { "pluck", "plucks", "plucked", "pizzicato", "pizz", "mallet", "mallets", "marimba", "harp" } },
                    { "bell", { "bell", "bells", "chime", "chimes", "glockenspiel", "vibes" } },
                    { "brass", { "brass", "horn", "horns", "trumpet", "trumpets", "sax", "saxophone" } },
                    { "drum", { "drum", "drums", "percussion", "perc" } },
                    { "seq", { "seq", "sequence", "sequencer", "arp", "arpeggio", "arpeggiator" } },
                    { "poly", { "poly", "polysynth", "chord", "chords", "stab", "stabs" } },
                    { "fx", { "fx", "sfx", "effect", "effects", "sound effect" } },
                    { "detuned", { "detuned", "detune", "unison", "supersaw" } },
                    { "legato/glide", { "glide", "portamento", "porta", "legato", "slide" } },
                    { "filter sweep", { "sweep", "filter sweep" } },
                    { "wide", { "wide", "stereo" } },
                    { "hard attack", { "fast attack", "punchy" } },
                    { "soft attack", { "slow attack" } },
                    { "long tail", { "long release" } },
                    { "sustained", { "sustain" } },
                };
                std::map<juce::String, juce::String> m;

                for (auto& [target, terms] : groups)
                    for (auto* t : terms)
                        m[t] = target;

                return m;
            }();
            return table;
        }

        // Ein Suchwort (oder zwei, die zusammen in der Tabelle stehen:
        // "electric piano") und seine Klasse bzw. sein Merkmalswort
        struct QueryTerm
        {
            juce::String text, target;
        };

        std::vector<QueryTerm> queryTerms (const juce::StringArray& words)
        {
            const auto& syn = presetSynonyms();
            std::vector<QueryTerm> out;

            for (int i = 0; i < words.size(); ++i)
            {
                auto text = words[i];

                if (i + 1 < words.size() && syn.count (text + " " + words[i + 1]) > 0)
                    text += " " + words[++i];

                auto it = syn.find (text);
                out.push_back ({ text, it != syn.end() ? it->second : juce::String() });
            }

            return out;
        }

        // Kurze Tabellenworte nur am Wortanfang ("ep" nicht in "sweep",
        // "arp" nicht in "harp"), sonst als Teilstring wie bisher
        bool containsTerm (const juce::String& hay, const QueryTerm& t)
        {
            if (t.target.isEmpty() || t.text.length() > 3)
                return hay.contains (t.text);

            for (int i = hay.indexOf (t.text); i >= 0; i = hay.indexOf (i + 1, t.text))
                if (i == 0 || ! juce::CharacterFunctions::isLetterOrDigit (hay[i - 1]))
                    return true;

            return false;
        }

        // Alle Worte der Anfrage muessen passen: im Namen/der Kategorie (2),
        // als Klasse oder Merkmalswort laut Tabelle (+1), sonst irgendwo in
        // Klasse/Worten (1). 0 = passt nicht.
        int queryScore (const std::vector<QueryTerm>& terms, const juce::String& name, const juce::String& hay,
                        const juce::String& cls, const juce::StringArray& w)
        {
            int score = 0;

            for (auto& t : terms)
            {
                int s = (containsTerm (name, t) ? 2 : 0)
                      + (t.target.isNotEmpty() && (cls == t.target || w.contains (t.target)) ? 1 : 0);

                if (s == 0 && containsTerm (hay, t))
                    s = 1;

                if (s == 0)
                    return 0;

                score += s;
            }

            return score;
        }

        // Katalog-Name eines Moduls: <format>-<uid> wie die Steckbriefe
        juce::String catalogSlug (te::Plugin& plugin)
        {
            juce::String fmt = "builtin", uid = plugin.getPluginType();

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&plugin))
                fmt = ep->desc.pluginFormatName, uid = juce::String::toHexString (ep->desc.uniqueId);

            return profileSlug (fmt, uid);
        }

        // Erkundeter Klang: Basis-Preset laden, dann die geaenderten
        // Parameter (normiert) setzen — ein Undo-Schritt wie loadPreset
        juce::var loadSound (DawdContext& ctx, te::Plugin& plugin, const juce::String& id, const juce::String& soundId)
        {
            auto f = profilesDirectory().getChildFile (catalogSlug (plugin) + "-sounds.json");
            juce::var snd;

            if (f.existsAsFile())
                if (auto* a = catalogFile (f).getProperty ("sounds", {}).getArray())
                    for (auto& x : *a)
                        if (x.getProperty ("id", {}).toString() == soundId)
                            snd = x;

            if (snd.isVoid())
                return resultObj (false, { { "error", "no such sound: " + soundId + " (findPresets lists sounds with kind \"sound\")" } });

            auto base = snd.getProperty ("base", {});
            auto path = base.getProperty ("path", {}).toString();
            auto program = (int) base.getProperty ("program", -1);
            int changed = 0;

            // Ohne Basis (Init-Patch) nur die Parameter
            if (base.getProperty ("name", {}).toString().isNotEmpty())
            {
                const PresetEntry* hit = nullptr;
                auto all = presetsFor (ctx, plugin);

                for (auto& e : all)
                    if (path.isNotEmpty() ? (matchesCatalogPath (e.file, path) && e.program == program)
                                          : (e.source == "program" && e.program == program))
                        hit = &e;

                if (hit == nullptr)
                    return resultObj (false, { { "error", "base preset of " + soundId + " not found: " + base.getProperty ("name", {}).toString() } });

                if (auto err = loadPresetEntry (ctx, plugin, *hit, changed); err.isNotEmpty())
                    return resultObj (false, { { "error", err } });
            }

            juce::StringArray missing;

            if (auto* params = snd.getProperty ("params", {}).getDynamicObject())
                for (auto& prop : params->getProperties())
                {
                    if (auto* p = findParameter (plugin, prop.name.toString()))
                    {
                        p->setNormalisedParameter (juce::jlimit (0.0f, 1.0f, (float) (double) prop.value), juce::sendNotification);
                        ++changed;
                    }
                    else
                        missing.add (prop.name.toString());
                }

            saveCurrentEdit (ctx);
            auto res = resultObj (true, { { "plugin", id }, { "sound", soundId }, { "name", snd.getProperty ("name", {}) },
                                          { "base", base }, { "params", snd.getProperty ("params", {}) }, { "changedParameters", changed } });

            if (! missing.isEmpty())
                res.getDynamicObject()->setProperty ("missingParameters", missing.joinIntoString (", "));

            return res;
        }
    }

    // findPresets {plugin? | name?, query?, class?, text? | like?, limit?}:
    // plugin = Modul im Projekt (seine Presets wie listPresets, mit
    // Klasse/Worten aus dem Katalog, falls es einen gibt); ohne plugin alle
    // katalogisierten Plugins (name schraenkt auf ein Plugin ein). query: alle
    // Worte in Kategorie, Name, Klasse oder Merkmalsworten ("bright pad",
    // "bass decaying"); gaengige Worte zaehlen ueber presetSynonyms() als
    // Klasse/Merkmalswort ("electric piano", "rhodes" -> keys, "unison" ->
    // detuned), Treffer im Namen und in der Klasse zuerst. text/like ordnen
    // nach Klang (CLAP, wie findSamples):
    // nur Eintraege mit Vektor im Katalog, die aehnlichsten zuerst (score =
    // Aehnlichkeit + bis 0.2, wenn der Text Klasse, Namen oder Worte nennt).
    // Neben den Presets stehen die erkundeten Klaenge (kind "sound",
    // <format>-<uid>-sounds.json, tools/explore_sounds.py) — laden mit
    // loadPreset {plugin, sound: id}.
    juce::var opFindPresets (DawdContext& ctx, const juce::var& cmd)
    {
        auto words = juce::StringArray::fromTokens (cmd.getProperty ("query", {}).toString().toLowerCase(), true);
        words.removeEmptyStrings();
        auto terms = queryTerms (words);
        auto cls = cmd.getProperty ("class", {}).toString().toLowerCase();
        auto limit = juce::jlimit (1, 500, (int) cmd.getProperty ("limit", 20));
        auto offset = juce::jmax (0, (int) cmd.getProperty ("offset", 0));
        auto only = cmd.getProperty ("name", {}).toString();
        auto text = cmd.getProperty ("text", {}).toString().trim();

        // plugin ist eine Modul-Id ("p3"); ein Plugin-Name dort meint den
        // Katalog (Praxistest 1c: {plugin: "Surge XT"} -> "no such plugin")
        auto pluginArg = cmd.getProperty ("plugin", {}).toString();
        const bool moduleId = pluginArg.startsWithChar ('p') && pluginArg.length() > 1
                              && pluginArg.substring (1).containsOnly ("0123456789");

        if (cmd.hasProperty ("plugin") && ! moduleId && only.isEmpty())
            only = pluginArg;

        std::vector<float> qvec;
        juce::var reference;
        juce::String english;

        if (text.isNotEmpty() || cmd.hasProperty ("like"))
            if (auto err = soundQuery (ctx, text, cmd.getProperty ("like", {}), qvec, reference, &english); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        // CLAP trennt Synth-Klaenge nur grob (Aehnlichkeiten um 0.1-0.3):
        // nennt der Text eine Klasse, ein Wort aus dem Namen oder ein
        // Merkmalswort, zaehlt das etwas dazu
        auto textWords = juce::StringArray::fromTokens (english.toLowerCase().replaceCharacters ("-,.", "   "), true);
        textWords.removeEmptyStrings();
        juce::StringArray textClasses;
        std::map<juce::String, juce::String> textTargets;   // Textwort -> Merkmalswort laut Tabelle

        for (auto& t : queryTerms (textWords))
            if (t.target.isNotEmpty())
            {
                textClasses.addIfNotAlreadyThere (t.target);

                for (auto& part : juce::StringArray::fromTokens (t.text, true))
                    textTargets[part] = t.target;
            }

        auto boost = [&] (juce::DynamicObject& o, const juce::String& c, const juce::StringArray& w)
        {
            float b = textClasses.contains (c) ? 0.1f : 0.0f;
            auto name = (o.getProperty ("category").toString() + " " + o.getProperty ("name").toString()).toLowerCase();

            for (auto& t : textWords)
            {
                if (t.length() >= 4 && name.contains (t))
                    b += 0.05f;

                auto it = textTargets.find (t);

                for (auto& x : w)
                    if (x.containsWholeWord (t) || (it != textTargets.end() && x == it->second))
                        b += 0.03f;
            }

            return juce::jmin (b, 0.2f);
        };

        struct Hit { float score; juce::var entry; int match; };
        std::vector<Hit> hits;
        std::map<juce::String, int> classes;
        int unembedded = 0;

        auto classList = [&classes]
        {
            juce::Array<juce::var> cl;

            for (auto& [name, count] : classes)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("name", name);
                o->setProperty ("count", count);
                cl.add (juce::var (o));
            }

            return cl;
        };

        // entry: was in der Antwort steht; vec: CLAP-Vektor aus dem Katalog
        auto consider = [&] (const juce::var& entry, const juce::String& vec)
        {
            auto* o = entry.getDynamicObject();
            auto c = o->getProperty ("class").toString();
            juce::StringArray w;

            if (auto* a = o->getProperty ("words").getArray())
                for (auto& x : *a)
                    w.add (x.toString());

            auto name = (o->getProperty ("category").toString() + " " + o->getProperty ("name").toString()).toLowerCase();
            auto hay = name + " " + (c + " " + w.joinIntoString (" ")).toLowerCase();
            auto match = queryScore (terms, name, hay, c, w);

            if (! terms.empty() && match == 0)
                return;

            float score = 0.0f;

            if (! qvec.empty())
            {
                auto v = clap::decode (vec);

                if (v.empty())
                {
                    ++unembedded;
                    return;
                }

                score = clap::similarity (qvec, v) + boost (*o, c, w);
                o->setProperty ("score", std::round ((double) score * 1000.0) / 1000.0);
            }

            if (c.isNotEmpty())
                classes[c]++;

            if (cls.isNotEmpty() && c != cls)
                return;

            hits.push_back ({ score, entry, match });
        };

        // Erkundete Klaenge eines Katalogs (Basis-Preset + geaenderte Parameter)
        auto considerSounds = [&] (const juce::File& f, const juce::String& pname, const juce::String& pfmt)
        {
            if (! f.existsAsFile())
                return;

            if (auto* a = catalogFile (f).getProperty ("sounds", {}).getArray())
                for (auto& snd : *a)
                {
                    auto o = new juce::DynamicObject();
                    o->setProperty ("kind", "sound");

                    if (pname.isNotEmpty())
                    {
                        o->setProperty ("pluginName", pname);
                        o->setProperty ("format", pfmt);
                    }

                    for (auto key : { "id", "name", "class", "words" })
                        if (snd.hasProperty (key))
                            o->setProperty (key, snd.getProperty (key, {}));

                    o->setProperty ("base", snd.getProperty ("base", {}).getProperty ("name", {}));
                    o->setProperty ("category", "");
                    consider (juce::var (o), snd.getProperty ("clap", {}).toString());
                }
        };

        auto finish = [&] (juce::DynamicObject& res)
        {
            if (! qvec.empty())
                std::stable_sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.score > b.score; });
            else if (! terms.empty())   // Treffer im Namen und in der Klasse zuerst
                std::stable_sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.match > b.match; });

            juce::Array<juce::var> list;

            for (size_t i = (size_t) offset; i < hits.size() && list.size() < limit; ++i)
                list.add (hits[i].entry);

            res.setProperty ("total", (int) hits.size());

            // Seiten statt einer stillen Kappung (Praxistest remix-1: limit
            // 5000 lieferte 500)
            if (offset > 0)
                res.setProperty ("offset", offset);

            if ((int) hits.size() > offset + list.size())
                res.setProperty ("more", juce::String ((int) hits.size() - offset - list.size())
                                         + juce::String::fromUTF8 (" more — offset: ") + juce::String (offset + list.size())
                                         + ((int) cmd.getProperty ("limit", 20) > 500 ? " (limit is at most 500)" : ""));

            // query filtert nur Namen/Kategorien (Praxistest 1d: still 0 Treffer)
            if (hits.empty() && ! words.isEmpty())
                res.setProperty ("note", juce::String::fromUTF8 ("query only matches preset names, categories and sound words — to search by sound use text (a description) or like (an audio file)"));
            res.setProperty ("presets", list);
            res.setProperty ("classes", classList());

            if (! qvec.empty())
                res.setProperty ("unembedded", unembedded);

            if (! reference.isVoid())
                res.setProperty ("reference", reference);
        };

        if (cmd.hasProperty ("plugin") && moduleId)
        {
            if (ctx.edit == nullptr)
                return resultObj (false, { { "error", "no edit" } });

            juce::String id;
            auto* plugin = presetPlugin (ctx, cmd, id);

            if (plugin == nullptr)
                return resultObj (false, { { "error", "no such plugin: " + id } });

            std::map<juce::String, juce::var> known;
            auto f = profilesDirectory().getChildFile (catalogSlug (*plugin) + "-presets.json");

            if (f.existsAsFile())
                if (auto* a = catalogFile (f).getProperty ("presets", {}).getArray())
                    for (auto& p : *a)
                        known[catalogKey (p.getProperty ("category", {}), p.getProperty ("name", {}))] = p;

            for (auto& e : presetsFor (ctx, *plugin))
            {
                auto v = entryVar (e);
                juce::String vec;

                if (auto it = known.find (catalogKey (e.category, e.name)); it != known.end())
                {
                    v.getDynamicObject()->setProperty ("class", it->second.getProperty ("class", {}));
                    v.getDynamicObject()->setProperty ("words", it->second.getProperty ("words", {}));
                    vec = it->second.getProperty ("clap", {}).toString();
                }

                consider (v, vec);
            }

            considerSounds (profilesDirectory().getChildFile (catalogSlug (*plugin) + "-sounds.json"), {}, {});

            auto res = resultObj (true, { { "plugin", id }, { "pluginName", plugin->getName() }, { "catalog", f.existsAsFile() } });
            finish (*res.getDynamicObject());
            return res;
        }

        juce::Array<juce::var> plugins;

        for (auto& f : catalogFiles())
        {
            auto& data = catalogFile (f);
            auto pname = data.getProperty ("plugin", {}).getProperty ("name", {}).toString();
            auto pfmt = data.getProperty ("plugin", {}).getProperty ("format", {}).toString();

            if (only.isNotEmpty() && ! pname.equalsIgnoreCase (only))
                continue;

            plugins.add (pname);

            if (auto* a = data.getProperty ("presets", {}).getArray())
                for (auto& p : *a)
                {
                    auto o = new juce::DynamicObject();
                    o->setProperty ("pluginName", pname);
                    o->setProperty ("format", pfmt);

                    for (auto key : { "name", "category", "source", "path", "program", "class", "words" })
                        if (p.hasProperty (key))
                            o->setProperty (key, p.getProperty (key, {}));

                    consider (juce::var (o), p.getProperty ("clap", {}).toString());
                }

            considerSounds (f.getSiblingFile (f.getFileName().replace ("-presets.json", "-sounds.json")), pname, pfmt);
        }

        if (only.isNotEmpty() && plugins.isEmpty())
            return resultObj (false, { { "error", "no preset catalog for " + only + " (tools/profile_presets.py)" } });

        auto res = resultObj (true, { { "catalogs", plugins } });
        finish (*res.getDynamicObject());
        return res;
    }
}
