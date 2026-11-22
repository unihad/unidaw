#include "Ops.h"
#include "../EngineHelpers.h"

// Projekt komprimieren: nicht favorisiertes Material global rauswerfen.
// Hintergrund-Operation ueber den ganzen Baum — keine Node-Aktion (der Node
// wird davon nicht acted). Destruktiv, darum zweistufig:
// treeCompactPreview (read-only) liefert den Plan und ein Plan-Token,
// treeCompact laeuft nur mit dem Token des aktuellen Plans.
//
// Regeln (die Favoriten sind die Auswahlkriterien):
// - Node bleibt: Seed, Node-Favorit (treeFavorite — markiert den ganzen
//   Pfad), Node mit Material-Favoriten (select-Aktionen) und jeder Vorfahr
//   einer bleibenden Node. Alle anderen Nodes fliegen samt Edit-Dateien.
// - Material fliegt nur in Nodes mit Material-Favoriten, die kein
//   Node-Favorit sind (der schuetzt alles). Dort bleibt ein Clip, wenn er
//   favorisiert ist oder eine favorisierte Section beruehrt; eine Section
//   bleibt, wenn sie favorisiert ist oder ein bleibender Clip sie beruehrt
//   — die anderen fliegen samt ihrem Zeitbereich (removeTime; bleibende
//   Clips beruehren sie nicht, nur Automation wird an ihren Grenzen
//   geschnitten), die folgenden Sections und Clips ruecken nach (keine Gaps). Ein Track bleibt, wenn er
//   einen bleibenden Clip traegt oder eine bleibende Lane in ihn laeuft.
//   Gefrorene Spuren samt Lanes bleiben unberuehrt (das gefrorene Audio
//   passt sonst nicht mehr); fallende Sections schneiden beide gleich.
// - Base-Snapshots bleiben (treeReset holt den Stand vor dem Komprimieren
//   zurueck), Mediendateien auf der Platte bleiben — bis auf Warp- und
//   Clip-FX-Dateien (warpClip, media/<name>-warp<n>.wav; processClip,
//   media/<Clip>-fx<n>.wav), auf die danach nichts mehr zeigt.
namespace dawd
{
    namespace
    {
        struct NodePrune
        {
            int node = 0;
            juce::Array<te::Clip*> clips;
            juce::Array<te::MarkerClip*> sections;
            juce::Array<int> tracks;            // alte Indizes, aufsteigend
            juce::Array<int> trackMap;          // alter Index -> neuer, -1 = faellt weg
        };

        // Bleibende Nodes: Seed, Node-Favoriten, Nodes mit Material-
        // Favoriten und ihre Vorfahren
        juce::Array<int> keptNodes (const TreeState& t)
        {
            juce::Array<int> kept;

            for (auto& n : t.nodes)
                if (n.id == t.root || n.favorite || n.hasFavorites())
                    for (auto* cur = t.find (n.id); cur != nullptr; cur = t.find (cur->parent))
                        kept.addIfNotAlreadyThere (cur->id);

            return kept;
        }

        // Beruehrt der Clip eine der Sections (Material darf ueber
        // Section-Grenzen laufen)?
        bool touches (te::Clip& clip, const juce::Array<te::MarkerClip*>& sections)
        {
            auto s = clip.getPosition().getStart().inSeconds(), e = clip.getPosition().getEnd().inSeconds();

            for (auto* sec : sections)
                if (s < sec->getPosition().getEnd().inSeconds() - 0.001
                    && e > sec->getPosition().getStart().inSeconds() + 0.001)
                    return true;

            return false;
        }

        // Was im Edit eines Nodes mit Material-Favoriten fliegt
        NodePrune planPrune (te::Edit& edit, const TreeNode& node)
        {
            NodePrune p;
            p.node = node.id;

            auto tracks = userTracks (edit);
            auto sections = sortedSections (edit);
            juce::Array<te::MarkerClip*> favSections, keptSections;
            juce::Array<te::Clip*> keptClips;

            for (auto* sec : sections)
                if (TreeNode::lastSelectOn (node.actions, "favoriteSection", sec->getName(), -1))
                    favSections.add (sec);

            keptSections = favSections;

            // Gefrorene Spuren samt Lanes ueberspringen (Entscheidung Matthias
            // 2026-10-24): ihr Material bleibt, sonst passt das gefrorene
            // Audio nicht mehr
            juce::Array<te::AudioTrack*> frozen;

            for (auto* tr : tracks)
                if (freezeTrackOf (edit, laneGroupOf (*tr) != nullptr ? *laneGroupOf (*tr) : *tr) != nullptr)
                    frozen.add (tr);

            for (int i = 0; i < tracks.size(); ++i)
                for (auto* clip : tracks[i]->getClips())
                {
                    if (frozen.contains (tracks[i]))
                        continue;

                    bool keep = TreeNode::lastSelectOn (node.actions, "favoriteClip", clip->getName(), i)
                                || touches (*clip, favSections);

                    if (keep)
                        keptClips.add (clip);
                    else
                        p.clips.add (clip);

                    // Ein bleibender Clip haelt alle Sections, die er
                    // beruehrt — auch einer, den nur eine favorisierte
                    // Section schuetzt (Entscheidung Matthias 2026-10-24:
                    // kein Abschneiden von geschuetztem Material)
                    if (keep)
                        for (auto* sec : sections)
                            if (touches (*clip, { sec }))
                                keptSections.addIfNotAlreadyThere (sec);
                }

            for (auto* sec : sections)
                if (! keptSections.contains (sec))
                    p.sections.add (sec);

            // Tracks: bleibt mit bleibendem Clip oder bleibender Lane, die in
            // ihn laeuft (Fixpunkt — Lanes haengen an Gruppenspuren)
            juce::Array<te::AudioTrack*> keptTracks = frozen;

            for (auto* clip : keptClips)
                if (auto* tr = dynamic_cast<te::AudioTrack*> (clip->getTrack()))
                    keptTracks.addIfNotAlreadyThere (tr);

            for (bool changed = true; changed;)
            {
                changed = false;

                for (auto* tr : tracks)
                    if (! keptTracks.contains (tr))
                        for (auto* k : keptTracks)
                            if (laneGroupOf (*k) == tr)
                            {
                                keptTracks.add (tr);
                                changed = true;
                                break;
                            }
            }

            for (int i = 0, next = 0; i < tracks.size(); ++i)
            {
                bool keep = keptTracks.contains (tracks[i]);
                p.trackMap.add (keep ? next++ : -1);

                if (! keep)
                    p.tracks.add (i);
            }

            return p;
        }

        juce::var pruneToVar (const NodePrune& p, te::Edit& edit, const TreeNode& node)
        {
            auto tracks = userTracks (edit);
            juce::Array<juce::var> clips, sections, trackList;

            for (auto* clip : p.clips)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("track", tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip->getTrack())));
                o->setProperty ("name", clip->getName());
                clips.add (juce::var (o));
            }

            for (auto* sec : p.sections)
                sections.add (sec->getName());

            for (auto i : p.tracks)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("index", i);
                o->setProperty ("name", tracks[i]->getName());
                trackList.add (juce::var (o));
            }

            auto o = new juce::DynamicObject();
            o->setProperty ("node", node.id);
            o->setProperty ("label", node.label);
            o->setProperty ("clips", clips);
            o->setProperty ("sections", sections);
            o->setProperty ("tracks", trackList);
            return juce::var (o);
        }

        // Nicht favorisiertes Material aus dem Edit werfen
        void applyPrune (te::Edit& edit, const NodePrune& p)
        {
            const auto offsets = followTempoOffsets (edit);

            for (auto* clip : p.clips)
                clip->removeFromParent();

            // Sections von hinten: die folgenden ruecken um die Laenge nach
            // vorn — die Positionen der davor liegenden bleiben gueltig
            auto secs = p.sections;
            std::sort (secs.begin(), secs.end(), [] (auto* a, auto* b)
                       { return a->getPosition().getStart() > b->getPosition().getStart(); });

            for (auto* sec : secs)
            {
                auto start = sec->getPosition().getStart().inSeconds();
                auto end = sec->getPosition().getEnd().inSeconds();
                sec->removeFromParent();
                removeTime (edit, start, end);
            }

            auto tracks = userTracks (edit);

            for (int i = p.tracks.size(); --i >= 0;)
                edit.deleteTrack (tracks[p.tracks[i]]);

            // Gefallene Sections nehmen ihr Tempo mit (der Offset der
            // Clips, die dem Tempo folgen, bleibt in Beats)
            applySectionTempos (edit);
            keepOffsetBeats (edit, offsets);
        }

        // Track-Indizes in den Aktionen nachziehen; select-Aktionen auf
        // gefallenes Material fliegen mit (sie haben kein Ziel mehr)
        void remapActions (TreeNode& node, const NodePrune& p, const juce::StringArray& droppedSections)
        {
            for (int i = node.actions.size(); --i >= 0;)
            {
                auto& a = node.actions.getReference (i);
                auto* po = a.params.getDynamicObject();

                if (po == nullptr)
                    continue;

                if (a.kind == "select" && a.op == "favoriteSection"
                    && droppedSections.contains (po->getProperty ("name").toString()))
                {
                    node.actions.remove (i);
                    continue;
                }

                // Routing-Kanten nennen Spuren als "t<i>" (connect/disconnect)
                for (auto key : { "from", "to" })
                {
                    auto id = po->getProperty (key).toString();

                    if (id.startsWithChar ('t') && id.substring (1).containsOnly ("0123456789") && id.length() > 1)
                    {
                        auto old = id.substring (1).getIntValue();

                        if (old < p.trackMap.size())
                            po->setProperty (key, "t" + juce::String (p.trackMap[old]));
                    }
                }

                // Automations-Ziele nennen Spuren als "t<i>.volume" bzw.
                // "edge:t<i>>…" (setAutomation/clearAutomation)
                if (po->hasProperty ("param"))
                {
                    auto param = po->getProperty ("param").toString();
                    auto prefix = param.startsWith ("edge:") ? juce::String ("edge:") : juce::String();
                    auto rest = param.substring (prefix.length());
                    auto num = rest.substring (1).initialSectionContainingOnly ("0123456789");
                    auto tail = rest.substring (1 + num.length());

                    if (rest.startsWithChar ('t') && num.isNotEmpty() && (tail.startsWithChar ('.') || tail.startsWithChar ('>'))
                        && num.getIntValue() < p.trackMap.size())
                        po->setProperty ("param", prefix + "t" + juce::String (p.trackMap[num.getIntValue()]) + tail);
                }

                // createGroup nennt seine Spuren als Liste
                if (auto* list = po->getProperty ("tracks").getArray())
                    for (auto& v : *list)
                        if (auto old = (int) (double) v; old >= 0 && old < p.trackMap.size())
                            v = p.trackMap[old];

                if (! po->hasProperty ("track"))
                    continue;

                auto old = (int) (double) po->getProperty ("track");

                if (old < 0 || old >= p.trackMap.size())
                    continue;

                auto now = p.trackMap[old];

                if (a.kind == "select" && now < 0)
                    node.actions.remove (i);
                else
                    po->setProperty ("track", now);
            }
        }

        juce::File editFileOf (const TreeState& t, const TreeNode& n)
        {
            return t.projectDir.getChildFile (n.file);
        }

        // ---- Warp- und Clip-FX-Dateien (Todos 2026-10-26-warp-dateien-aufraeumen,
        // 2026-10-26-fx-dateien-aufraeumen) ----
        // warpClip renderte bis 2026-10-30 je Aenderung media/<name>-warp<n>.wav
        // (seit dem Echtzeit-Warp keine Datei; alte Projekte haben sie noch,
        // playWarpSources nimmt sie beim Laden vom Clip), processClip
        // je Schritt media/<Clip>-fx<n>.wav (Undo braucht die alten). Eine
        // Datei, auf die ein Clip-FX-Original zeigt (DAWDFX: source, ORIGINAL),
        // ist ein Verweis wie jeder andere. Eine fliegt, wenn ihr Dateiname nach dem Komprimieren in
        // keiner Zeichenkette mehr steht: bleibende Edit-Dateien (Nodes,
        // Base-Schnappschuesse, jede andere *tracktionedit* im Projektordner),
        // gestutzte Edits ohne ihr fallendes Material, tree.json. Die
        // Undo-Historie zaehlt nicht: treeCompact loescht sie (clearsUndo),
        // ebenso die undoDelete-Token (loadNodeEdit). Teilstring-Treffer und
        // unlesbare Dateien (roh durchsucht) halten eine Datei im Zweifel fest.
        bool isRenderedFile (const juce::File& f)
        {
            const auto stem = f.getFileNameWithoutExtension().trimCharactersAtEnd ("0123456789");
            return f.hasFileExtension ("wav") && (stem.endsWith ("-warp") || stem.endsWith ("-fx"));
        }

        struct FileRefs
        {
            juce::StringArray unused;   // Dateinamen ohne Verweis (bisher)

            void see (const juce::String& s)
            {
                for (int i = unused.size(); --i >= 0;)
                    if (s.contains (unused[i]))
                        unused.remove (i);
            }

            void see (const juce::ValueTree& v, const juce::Array<juce::ValueTree>& skip)
            {
                if (unused.isEmpty() || skip.contains (v))
                    return;

                for (int i = 0; i < v.getNumProperties(); ++i)
                    see (v.getProperty (v.getPropertyName (i)).toString());

                for (int i = 0; i < v.getNumChildren(); ++i)
                    see (v.getChild (i), skip);
            }

            void see (const juce::XmlElement& x)
            {
                if (x.isTextElement())
                    see (x.getText());

                for (int i = 0; i < x.getNumAttributes(); ++i)
                    see (x.getAttributeValue (i));

                for (auto* c = x.getFirstChildElement(); c != nullptr && ! unused.isEmpty(); c = c->getNextElement())
                    see (*c);
            }

            void see (const juce::var& v)
            {
                if (auto* a = v.getArray())
                    for (auto& e : *a)
                        see (e);
                else if (auto* o = v.getDynamicObject())
                    for (auto& p : o->getProperties())
                        see (p.value);
                else
                    see (v.toString());
            }

            void seeFile (const juce::File& f, bool json)
            {
                if (unused.isEmpty() || ! f.existsAsFile())
                    return;

                if (json)
                {
                    auto v = juce::JSON::parse (f);

                    if (! v.isVoid())
                        return see (v);
                }
                else if (auto xml = juce::parseXML (f))
                {
                    return see (*xml);
                }

                see (f.loadFileAsString());
            }
        };

        std::unique_ptr<te::Edit> loadOther (DawdContext& ctx, const juce::File& f)
        {
            auto edit = te::loadEditFromFile (*ctx.engine, f);

            if (edit != nullptr)
            {
                edit->editFileRetriever = [f] { return f; };
                ensureRouting (*edit);
                applySectionTempos (*edit);
            }

            return edit;
        }

        // Plan rechnen; work bekommt je zu stutzendem Node den Edit und den
        // Prune-Plan (der Edit des aktuellen Nodes ist ctx.edit)
        struct Plan
        {
            juce::Array<int> removeNodes;
            juce::var json;
            juce::String token;
        };

        template <typename Fn>
        Plan buildPlan (DawdContext& ctx, Fn&& work)
        {
            auto& t = *ctx.tree;
            Plan plan;
            auto kept = keptNodes (t);
            juce::Array<juce::var> removed, pruned;

            for (auto& n : t.nodes)
                if (! kept.contains (n.id))
                {
                    plan.removeNodes.add (n.id);
                    auto o = new juce::DynamicObject();
                    o->setProperty ("node", n.id);
                    o->setProperty ("label", n.label);
                    removed.add (juce::var (o));
                }

            FileRefs refs;

            for (auto& f : t.projectDir.getChildFile ("media").findChildFiles (juce::File::findFiles, false, "*.wav"))
                if (isRenderedFile (f))
                    refs.unused.add (f.getFileName());

            juce::StringArray skipFiles;   // fallende Nodes, gestutzte Edits (aus dem Zustand)

            for (auto id : plan.removeNodes)
            {
                skipFiles.add ("node-" + juce::String (id) + ".tracktionedit");
                skipFiles.add ("node-" + juce::String (id) + ".base.tracktionedit");
            }

            for (auto& n : t.nodes)
            {
                if (! kept.contains (n.id) || n.favorite || ! n.hasFavorites())
                    continue;

                std::unique_ptr<te::Edit> other;
                te::Edit* edit = ctx.edit.get();

                if (n.id != t.current)
                {
                    other = loadOther (ctx, editFileOf (t, n));
                    edit = other.get();
                }

                if (edit == nullptr)
                    continue;

                auto p = planPrune (*edit, n);

                if (p.clips.isEmpty() && p.sections.isEmpty() && p.tracks.isEmpty())
                    continue;

                // Verweise des gestutzten Stands, vor dem Schneiden
                juce::Array<juce::ValueTree> dropped;

                for (auto* clip : p.clips)
                    dropped.add (clip->state);

                edit->flushState();
                refs.see (edit->state, dropped);
                skipFiles.add (editFileOf (t, n).getFileName());

                pruned.add (pruneToVar (p, *edit, n));
                work (n, *edit, p);
            }

            for (auto& f : t.projectDir.findChildFiles (juce::File::findFiles, false, "*tracktionedit*"))
                if (! skipFiles.contains (f.getFileName()))
                    refs.seeFile (f, false);

            refs.seeFile (t.projectDir.getChildFile ("tree.json"), true);
            refs.unused.sort (false);
            juce::Array<juce::var> files;

            for (auto& name : refs.unused)
                files.add ("media/" + name);

            auto o = new juce::DynamicObject();
            o->setProperty ("removeNodes", removed);
            o->setProperty ("pruneNodes", pruned);
            o->setProperty ("removeFiles", files);
            plan.json = juce::var (o);
            plan.token = juce::String::toHexString (juce::JSON::toString (plan.json, true).hashCode64());
            return plan;
        }

        bool anyFavorites (const TreeState& t)
        {
            for (auto& n : t.nodes)
                if (n.favorite || n.hasFavorites())
                    return true;

            return false;
        }
    }

    juce::var opTreeCompactPreview (DawdContext& ctx, const juce::var&)
    {
        if (! anyFavorites (*ctx.tree))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no favorites — nothing to keep") } });

        saveCurrentEdit (ctx);
        auto plan = buildPlan (ctx, [] (const TreeNode&, te::Edit&, const NodePrune&) {});

        return resultObj (true, { { "plan", plan.token },
                                  { "removeNodes", plan.json["removeNodes"] },
                                  { "pruneNodes", plan.json["pruneNodes"] },
                                  { "removeFiles", plan.json["removeFiles"] } });
    }

    juce::var opTreeCompact (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto token = cmd.getProperty ("plan", {}).toString();

        if (token.isEmpty())
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no plan — run treeCompactPreview first") } });

        if (! anyFavorites (t))
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no favorites — nothing to keep") } });

        saveCurrentEdit (ctx);

        // Erst pruefen, dann schneiden: der Plan muss dem Trockenlauf gleichen
        if (buildPlan (ctx, [] (const TreeNode&, te::Edit&, const NodePrune&) {}).token != token)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("plan changed — run treeCompactPreview again") } });

        ctx.edit->getTransport().stop (false, false);

        auto plan = buildPlan (ctx, [&] (const TreeNode& n, te::Edit& edit, const NodePrune& p)
        {
            juce::StringArray droppedSections;

            for (auto* sec : p.sections)
                droppedSections.add (sec->getName());

            applyPrune (edit, p);
            te::EditFileOperations (edit).save (false, true, false);
            remapActions (*t.find (n.id), p, droppedSections);
        });

        for (auto id : plan.removeNodes)
        {
            t.projectDir.getChildFile ("node-" + juce::String (id) + ".tracktionedit").deleteFile();
            t.projectDir.getChildFile ("node-" + juce::String (id) + ".base.tracktionedit").deleteFile();
        }

        // Faellt die aktuelle Node weg, geht es beim naechsten bleibenden
        // Vorfahren weiter (der Seed bleibt immer)
        if (plan.removeNodes.contains (t.current))
        {
            auto* cur = t.find (t.current);

            while (cur != nullptr && plan.removeNodes.contains (cur->id))
                cur = t.find (cur->parent);

            t.current = cur != nullptr ? cur->id : t.root;
        }

        for (int i = t.nodes.size(); --i >= 0;)
            if (plan.removeNodes.contains (t.nodes.getUnchecked (i).id))
                t.nodes.remove (i);

        loadNodeEdit (ctx, editFileOf (t, *t.find (t.current)));

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        // Warp- und FX-Dateien ohne Verweis zuletzt: der Baum steht, nichts zeigt mehr darauf
        if (auto* files = plan.json["removeFiles"].getArray())
            for (auto& f : *files)
                t.projectDir.getChildFile (f.toString()).deleteFile();

        return resultObj (true, { { "node", t.current },
                                  { "removeNodes", plan.json["removeNodes"] },
                                  { "pruneNodes", plan.json["pruneNodes"] },
                                  { "removeFiles", plan.json["removeFiles"] } });
    }
}
