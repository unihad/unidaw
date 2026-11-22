#include "Ops.h"
#include "../Origins.h"
#include "../Undo.h"
#include "../EngineHelpers.h"
#include <optional>

// Song-Baum: Seed, Varianten, Checkout, Reset, Status. Guards (no tree /
// no tree / no edit / recording in progress) prueft der Dispatcher.
namespace dawd
{
    // "no such node" bleibt der Anfang (API-Vertrag); hat ein Undo den Node
    // entfernt, steht der Grund dahinter (Praxistest 1j/1n: treeNote auf
    // einen weggeundoten Auto-Node)
    juce::var noSuchNode (DawdContext& ctx, int node, juce::String text = "no such node")
    {
        if (auto why = removedByUndo (ctx, node); why.isNotEmpty())
            text << juce::String::fromUTF8 (" — ") << why;

        return resultObj (false, { { "error", text } });
    }

    // compact: je Node ohne Aktionen, die Notiz gekuerzt (Praxistest 1f:
    // 66 Nodes, ~50.000 Zeichen — kam beim Agenten nicht an); node: nur
    // dieser Node, vollstaendig
    juce::var opTreeState (DawdContext& ctx, const juce::var& cmd)
    {
        auto v = ctx.tree->toVar();
        auto* o = v.getDynamicObject();
        o->setProperty ("ok", true);
        o->setProperty ("projectDir", ctx.tree->projectDir.getFullPathName());

        const bool compact = (bool) cmd.getProperty ("compact", false);
        const bool one = cmd.hasProperty ("node");
        const int want = (int) cmd.getProperty ("node", -1);

        if (! compact && ! one)
            return v;

        juce::Array<juce::var> kept;

        for (auto& n : *o->getProperty ("nodes").getArray())
        {
            if (one && (int) n.getProperty ("id", -1) != want)
                continue;

            if (compact && ! one)
            {
                auto* no = n.getDynamicObject();
                no->setProperty ("actionCount", no->getProperty ("actions").size());

                for (auto key : { "actions", "actionFile", "file", "created", "noteUpdated", "takeType", "takeSection", "takeTrack" })
                    no->removeProperty (key);

                if (auto note = no->getProperty ("note").toString(); note.length() > 120)
                    no->setProperty ("note", note.substring (0, 117) + juce::String::fromUTF8 ("…"));
            }

            kept.add (n);
        }

        if (one && kept.isEmpty())
            return noSuchNode (ctx, want, "no such node: " + juce::String (want));

        o->setProperty ("nodes", kept);
        return v;
    }

    // Letzten Schritt zuruecknehmen / wiederholen (Undo.h); steps = mehrere
    juce::var undoRedo (DawdContext& ctx, const juce::var& cmd, bool undo)
    {
        auto steps = juce::jlimit (1, maxUndoSteps, (int) (double) cmd.getProperty ("steps", 1.0));
        juce::Array<juce::var> ops, removedNotes;
        juce::StringArray noted;
        juce::String error;

        for (int i = 0; i < steps; ++i)
        {
            juce::String op;
            TreeNode removed;
            error = undo ? undoLast (ctx, op, &removed) : redoLast (ctx, op);

            if (error.isNotEmpty())
                break;

            ops.add (op);

            // Der Undo nahm einen Auto-Node samt Notiz mit — sagen, sonst
            // sucht der Agent die Notiz (Praxistest 1j Punkt 12, 1n Befund 10)
            if (removed.id > 0 && removed.note.isNotEmpty())
            {
                auto note = removed.note.length() > 120 ? removed.note.substring (0, 117) + juce::String::fromUTF8 ("…") : removed.note;
                auto o = new juce::DynamicObject();
                o->setProperty ("node", removed.id);
                o->setProperty ("label", removed.label);
                o->setProperty ("note", note);
                removedNotes.add (juce::var (o));
                noted.add (juce::String (removed.id));
            }
        }

        if (ops.isEmpty())
            return resultObj (false, { { "error", error } });

        auto r = resultObj (true, { { undo ? "undone" : "redone", ops }, { "node", ctx.tree->current },
                                    { "undo", undoInfo (ctx) } });

        if (error.isNotEmpty())
            r.getDynamicObject()->setProperty ("stopped", error);

        if (! removedNotes.isEmpty())
        {
            r.getDynamicObject()->setProperty ("removedNotes", removedNotes);
            r.getDynamicObject()->setProperty ("warning", "undo removed node " + noted.joinIntoString (", ")
                + juce::String::fromUTF8 (" (auto-branch) with its note — redo brings node and note back; a new op drops redo and the note with it"));
        }

        return r;
    }

    juce::var opUndo (DawdContext& ctx, const juce::var& cmd) { return undoRedo (ctx, cmd, true); }
    juce::var opRedo (DawdContext& ctx, const juce::var& cmd) { return undoRedo (ctx, cmd, false); }

    // Projekt-Einstellungen des Baums: autoBranch (neuer Node bei
    // Sorten-Wechsel). Ohne Parameter nur lesen.
    juce::var opTreeSettings (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;

        if (cmd.hasProperty ("autoBranch"))
        {
            t.autoBranch = (bool) cmd.getProperty ("autoBranch", true);

            if (! t.save())
                return resultObj (false, { { "error", "cannot write tree.json" } });
        }

        return resultObj (true, { { "autoBranch", t.autoBranch } });
    }

    namespace
    {
        // Leerer Edit fuer einen neuen Node (Seed, Bounce) als ctx.edit, an
        // seine Datei gebunden, mit Routing. Ohne Spur — Spuren entstehen
        // durch die Aufnahme (numAudioTracks-Default ist 1). false, wenn er
        // nicht entsteht.
        bool newEmptyEdit (DawdContext& ctx, const juce::File& editFile)
        {
            auto id = te::ProjectItemID::createNewID (te::ProjectID{});
            ctx.recordedTakes.clear();
            ctx.deletedClips.clear();
            ctx.edit = te::Edit::createEdit (te::Edit::Options
                                             {
                                                 *ctx.engine,
                                                 te::loadEditFromFile (*ctx.engine, {}, id),
                                                 id,
                                                 te::Edit::forEditing,
                                                 nullptr,
                                                 te::Edit::getDefaultNumUndoLevels(),
                                                 [editFile] { return editFile; },
                                                 {},
                                                 0
                                             });
            bindEditResolver (ctx, editFile);

            if (ctx.edit == nullptr)
                return false;

            ensureRouting (*ctx.edit);
            return true;
        }
    }

    juce::var opTreeSeed (DawdContext& ctx, const juce::var& cmd)
    {
        if (ctx.tree)
            return resultObj (false, { { "error", "tree already open: " + ctx.tree->projectDir.getFullPathName() + " (treeClose first)" } });

        auto dir = juce::File (cmd.getProperty ("dir", {}).toString());

        if (dir.getFullPathName().isEmpty())
            return resultObj (false, { { "error", "no dir" } });

        if (TreeState::treeFile (dir).existsAsFile())
            return resultObj (false, { { "error", "dir already has a tree: " + dir.getFullPathName() } });

        if (! dir.createDirectory().wasOk())
            return resultObj (false, { { "error", "cannot create dir: " + dir.getFullPathName() } });

        auto editFile = dir.getChildFile ("node-1.tracktionedit");

        if (! newEmptyEdit (ctx, editFile))
            return resultObj (false, { { "error", "cannot create edit" } });

        TreeNode node;
        node.id = 1;
        node.parent = 0;
        node.label = cmd.getProperty ("label", "seed").toString();
        node.file = editFile.getFileName();
        node.created = juce::Time::getCurrentTime().toISO8601 (true);

        TreeState t;
        t.projectDir = dir;
        t.root = 1;
        t.current = 1;
        t.nextId = 2;
        t.nodes.add (node);

        juce::String seedError;

        if (auto seedFile = cmd.getProperty ("file", {}).toString(); seedFile.isNotEmpty())
        {
            saveCurrentEdit (ctx);   // Clip-Pfad relativ zur vorhandenen Datei (wie bounceNode)
            ctx.edit->ensureNumberOfAudioTracks (1);
            double seedLen = 0.0;
            seedError = insertAudioClip (ctx, 0, juce::File (seedFile), 0.0, &seedLen);

            // Seed-Audio braucht eine Section (Record/Insert sind
            // sectionspflichtig): erste Section ueber die Dateilaenge
            if (seedError.isEmpty() && seedLen > 0.0)
                insertSection (*ctx.edit, "Seed", 0.0, seedLen, nextSectionColour (*ctx.edit));
        }

        // Ohne Seed-Datei: automatisch die erste Section mit Default-Laenge
        // — der Song startet damit, Record/Insert sind sectionspflichtig
        if (sortedSections (*ctx.edit).empty())
            insertSection (*ctx.edit, nextSectionName (*ctx.edit), 0.0, defaultSectionLength,
                           nextSectionColour (*ctx.edit));

        saveCurrentEdit (ctx);
        t.projectDir.getChildFile ("node-1.tracktionedit").copyFileTo (
            t.projectDir.getChildFile ("node-1.base.tracktionedit"));
        ctx.tree = std::move (t);
        interfaceProjectChanged (ctx);   // Interface-Mix des Projekts aufs Geraet
        audioProjectChanged (ctx);       // Rate und Bit-Tiefe des Songs

        if (! ctx.tree->save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", 1 }, { "dir", dir.getFullPathName() },
                                  { "seedError", seedError } });
    }

    juce::var opTreeOpen (DawdContext& ctx, const juce::var& cmd)
    {
        if (ctx.tree)
            return resultObj (false, { { "error", "tree already open: " + ctx.tree->projectDir.getFullPathName() + " (treeClose first)" } });

        auto dir = juce::File (cmd.getProperty ("dir", {}).toString());
        TreeState t;

        if (dir.getFullPathName().isEmpty() || ! TreeState::load (t, dir))
            return resultObj (false, { { "error", "no tree in dir: " + dir.getFullPathName() } });

        auto node = t.find (t.current);

        if (node == nullptr)
            return resultObj (false, { { "error", "current node missing in tree" } });

        auto editFile = t.projectDir.getChildFile (node->file);

        if (! editFile.existsAsFile())
            return resultObj (false, { { "error", "edit file missing: " + editFile.getFullPathName() } });

        loadNodeEdit (ctx, editFile);
        ctx.tree = std::move (t);
        interfaceProjectChanged (ctx);   // Interface-Mix des Projekts aufs Geraet
        audioProjectChanged (ctx);       // Rate und Bit-Tiefe des Songs
        auto result = resultObj (true, { { "node", ctx.tree->current },
                                         { "dir", ctx.tree->projectDir.getFullPathName() } });

        // Fehlende Mediendateien: das Projekt laedt trotzdem, die Clips sind
        // in state markiert, Render und Analyse lehnen mit Fehler ab
        if (ctx.edit != nullptr)
            if (auto missing = missingMedia (*ctx.edit); ! missing.isEmpty())
                result.getDynamicObject()->setProperty ("missingFiles", missingMediaList (missing));

        return result;
    }

    juce::var opTreeClose (DawdContext& ctx, const juce::var&)
    {
        // Projektwechsel: aktueller Node bleibt gespeichert, Tree-Status
        // (tree.json) bleibt auf der Platte — treeOpen holt ihn zurueck.
        saveCurrentEdit (ctx);
        disarmRecording (ctx);
        ctx.edit = nullptr;
        ctx.recordedTakes.clear();
        ctx.deletedClips.clear();
        ctx.tree.reset();
        interfaceProjectChanged (ctx);
        audioProjectChanged (ctx);
        ctx.monitorOn = false;
        return resultObj (true, { { "closed", true } });
    }

    juce::var opTreeBranch (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto parentId = (int) (double) cmd.getProperty ("node", (double) t.current);
        auto parent = t.find (parentId);

        if (parent == nullptr)
            return noSuchNode (ctx, parentId);

        if (parent->status == "terminated")
            return resultObj (false, { { "error", "node is terminated" } });

        if (! parent->isActed())
            return resultObj (false, { { "error", juce::String::fromUTF8 ("no action at this node yet — act first, then branch") } });

        saveCurrentEdit (ctx);

        auto id = t.nextId++;
        auto editFile = t.projectDir.getChildFile ("node-" + juce::String (id) + ".tracktionedit");

        if (! t.projectDir.getChildFile (parent->file).copyFileTo (editFile))
            return resultObj (false, { { "error", "cannot copy edit file" } });

        editFile.copyFileTo (t.projectDir.getChildFile (
            "node-" + juce::String (id) + ".base.tracktionedit"));

        loadNodeEdit (ctx, editFile);

        // parent zeigt in t.nodes — nach add() kann das Array umgezogen
        // sein, die Id deshalb vorher sichern
        auto parentNodeId = parent->id;
        auto label = cmd.getProperty ("label", {}).toString();
        TreeNode child;
        child.id = id;
        child.parent = parentNodeId;
        child.label = label.isEmpty() ? "Variant " + juce::String (id) : label;
        child.file = editFile.getFileName();
        child.created = juce::Time::getCurrentTime().toISO8601 (true);
        t.nodes.add (child);
        t.current = id;

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", id }, { "parent", parentNodeId } });
    }

    // Node-Bounce als Zweig-Seed: den Mix eines Nodes (ganz oder eine
    // Section, mit Ausklang) als 32-Bit-Float-WAV nach bounces/ rendern und
    // daraus einen neuen Kind-Node des Nodes machen — wie ein Seed: eine
    // Audio-Spur "Bounce" mit der Datei ab 0, dazu die Sections des Nodes
    // mit Namen, Farben, Tempo und Takt (eine Section: nach vorn geschoben,
    // mit ihrem wirksamen Tempo; gleitet sie per Rampe, kommt die
    // Folge-Section als Ziel mit) und die Marker darin. Spuren, Module und Master-Kette bleiben
    // zurueck — sie stecken im Audio. Der neue Node traegt die
    // collect-Aktion bounceNode {node, section?, tail?} mit der Datei (acted:
    // weiter verzweigen geht sofort). Kein Undo-Schritt: wie treeBranch ein
    // Baum-Schritt, zurueck ueber treeDelete. Nicht in actionKindOf: die Op
    // legt ihren Node selbst an, ein Auto-Branch davor waere ein Node zu viel.
    juce::var opBounceNode (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        const int srcId = (int) (double) cmd.getProperty ("node", (double) t.current);
        auto* src = t.find (srcId);

        if (src == nullptr)
            return noSuchNode (ctx, srcId);

        if (src->status == "terminated")
            return resultObj (false, { { "error", "node is terminated" } });

        const auto section = cmd.getProperty ("section", {}).toString();
        const auto tail = (double) cmd.getProperty ("tail", 0.0);

        if (tail < 0.0 || tail > 30.0)
            return resultObj (false, { { "error", "tail must be 0..30 s" } });

        // Stand auf der Platte = der gehoerte (auch fuer den Wechsel unten)
        saveCurrentEdit (ctx);

        std::unique_ptr<te::Edit> other;
        te::Edit* edit = ctx.edit.get();

        if (srcId != t.current)
        {
            other = loadEditForRender (ctx, t.projectDir.getChildFile (src->file));

            if (other == nullptr)
                return resultObj (false, { { "error", "cannot load node " + juce::String (srcId) } });

            edit = other.get();
        }

        // Sections des Nodes (vor dem Render: der Bereich steht danach fest)
        struct Sec { juce::String name; double start, end; juce::Colour colour; SectionTempo tempo; };
        std::vector<Sec> secs;
        std::optional<Sec> rampTarget;   // Teil-Bounce einer Rampe: die Section, zu deren Tempo sie gleitet
        {
            auto sorted = sortedSections (*edit);
            auto tempos = sectionTempos (*edit);

            for (size_t i = 0; i < sorted.size(); ++i)
                if (section.isEmpty() || sorted[i]->getName() == section)
                {
                    secs.push_back ({ sorted[i]->getName(), sorted[i]->getPosition().getStart().inSeconds(),
                                      sorted[i]->getPosition().getEnd().inSeconds(), sorted[i]->getColour(), tempos[i] });

                    if (section.isNotEmpty() && tempos[i].ramps() && i + 1 < sorted.size())
                        rampTarget = Sec { sorted[i + 1]->getName(), sorted[i + 1]->getPosition().getStart().inSeconds(),
                                           sorted[i + 1]->getPosition().getEnd().inSeconds(), sorted[i + 1]->getColour(),
                                           tempos[i + 1] };
                }
        }

        if (section.isNotEmpty() && secs.empty())
            return resultObj (false, { { "error", "node " + juce::String (srcId) + " has no section " + section } });

        // Marker des Bereichs (Matthias 2026-10-25): ganzer Bounce alle,
        // Teil-Bounce nur die in der Section; im neuen Node um den Versatz
        // verschoben
        std::vector<Marker> marks;

        for (auto& m : sortedMarkers (*edit))
            if (m.time >= secs.front().start - 0.001 && (section.isEmpty() || m.time < secs.back().end - 0.001))
                marks.push_back (m);

        const int id = t.nextId;
        auto bounce = t.projectDir.getChildFile ("bounces").getChildFile ("node-" + juce::String (id) + ".wav");
        auto opts = new juce::DynamicObject();
        opts->setProperty ("format", "wav");
        opts->setProperty ("bitDepth", 32);
        opts->setProperty ("sampleRate", freezeSampleRate (ctx));
        opts->setProperty ("force", true);   // ein stiller Node ist auch ein Stand

        if (section.isNotEmpty())
            opts->setProperty ("section", section);

        if (tail > 0.0)
            opts->setProperty ("tail", tail);

        ctx.load.setPhase ("bounce");
        auto rendered = renderEditMix (ctx, *edit, juce::var (opts), bounce);

        if (! (bool) rendered.getProperty ("ok", false))
            return rendered;

        // Lizenz-Sperre: der Bounce stammt aus allem Material im Bereich
        recordOrigins (bounce, soundingOrigins (*edit, secs.front().start, secs.back().end + tail, -1));

        other.reset();

        // Neuer Node: leerer Edit mit der Datei und den Sections
        t.nextId = id + 1;
        auto editFile = t.projectDir.getChildFile ("node-" + juce::String (id) + ".tracktionedit");

        if (! newEmptyEdit (ctx, editFile))
            return resultObj (false, { { "error", "cannot create edit" } });

        // Datei zuerst schreiben: den Clip-Pfad baut die Engine relativ zur
        // Edit-Datei — fehlt sie, zaehlt sie als Ordner ("../bounces/…"), und
        // beim Laden zeigt der Pfad eine Ebene zu hoch
        saveCurrentEdit (ctx);
        ctx.edit->ensureNumberOfAudioTracks (1);

        if (auto err = insertAudioClip (ctx, 0, bounce, 0.0); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        userTracks (*ctx.edit)[0]->setName ("Bounce");
        const double offset = secs.front().start;

        for (size_t i = 0; i < secs.size(); ++i)
            if (auto* m = insertSection (*ctx.edit, secs[i].name, secs[i].start - offset, secs[i].end - offset, secs[i].colour))
            {
                // Eigene Angaben bleiben eigen, geerbte geerbt; die erste
                // Section einer Teil-Bounce hat ihre Vorgaengerin nicht mehr
                const auto& tp = secs[i].tempo;
                const bool first = i == 0 && offset > 0.0;
                setSectionTempo (*m, tp.bpmSet || first ? juce::var (tp.bpm) : juce::var (0.0),
                                 tp.sigSet || first ? juce::var (tp.timeSig()) : juce::var (juce::String()));

                // Tempo-Rampe: das Audio gleitet, das Raster soll mitgleiten
                if (tp.ramp)
                    setSectionRamp (*m, true);
            }

        // Teil-Bounce einer Rampen-Section: ohne Folge-Section haette die
        // Rampe kein Ziel (das Audio gleitet, das Raster nicht). Die
        // Folge-Section kommt als Ziel mit — Name, Farbe, Tempo/Takt — und
        // deckt den Ausklang (der ist ihr Anfang) in ganzen Takten, mindestens
        // einen Takt.
        juce::var rampTargetRes;

        if (rampTarget)
        {
            const auto& tp = rampTarget->tempo;
            const double start = secs.back().end - offset;
            const int bars = juce::jmax (1, (int) std::ceil (tail / tp.barSeconds() - 1.0e-6));

            if (auto* m = insertSection (*ctx.edit, rampTarget->name, start, start + bars * tp.barSeconds(), rampTarget->colour))
            {
                setSectionTempo (*m, juce::var (tp.bpm), juce::var (tp.timeSig()));
                secs.push_back (*rampTarget);
                rampTargetRes = juce::var (new juce::DynamicObject());
                rampTargetRes.getDynamicObject()->setProperty ("name", rampTarget->name);
                rampTargetRes.getDynamicObject()->setProperty ("bars", bars);
            }
        }

        for (auto& m : marks)
            addMarker (*ctx.edit, m.name, juce::jmax (0.0, m.time - offset));

        applySectionTempos (*ctx.edit);
        const auto autoSec = autoSection (*ctx.edit);   // Ausklang hinter der letzten Section
        saveCurrentEdit (ctx);
        editFile.copyFileTo (t.projectDir.getChildFile ("node-" + juce::String (id) + ".base.tracktionedit"));

        auto label = cmd.getProperty ("label", {}).toString();
        TreeNode child;
        child.id = id;
        child.parent = srcId;
        child.label = label.isNotEmpty() ? label : src->label + " (bounce)";
        child.file = editFile.getFileName();
        child.created = juce::Time::getCurrentTime().toISO8601 (true);
        t.nodes.add (child);
        t.current = id;

        auto params = new juce::DynamicObject();
        params->setProperty ("node", srcId);

        if (section.isNotEmpty())
            params->setProperty ("section", section);

        if (tail > 0.0)
            params->setProperty ("tail", tail);

        recordNodeAction (ctx, "collect", "bounceNode", juce::var (params), bounce.getFullPathName());
        recordAutoSection (ctx, autoSec);
        saveCurrentEdit (ctx);

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        auto res = resultObj (true, { { "node", id }, { "parent", srcId }, { "label", child.label },
                                      { "file", bounce.getFullPathName() },
                                      { "sampleRate", rendered.getProperty ("sampleRate", {}) },
                                      { "length", (double) rendered.getProperty ("end", 0.0) - (double) rendered.getProperty ("start", 0.0) },
                                      { "sections", (int) secs.size() }, { "markers", (int) marks.size() } });

        if (! autoSec.isVoid())
            res.getDynamicObject()->setProperty ("autoSection", autoSec);

        if (! rampTargetRes.isVoid())
            res.getDynamicObject()->setProperty ("rampTarget", rampTargetRes);

        return res;
    }

    juce::var opTreeCheckout (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto nodeId = (int) (double) cmd.getProperty ("node", 0.0);
        auto node = t.find (nodeId);

        if (node == nullptr)
            return noSuchNode (ctx, nodeId);

        if (node->status == "terminated")
            return resultObj (false, { { "error", "node is terminated" } });

        auto editFile = t.projectDir.getChildFile (node->file);

        if (! editFile.existsAsFile())
            return resultObj (false, { { "error", "edit file missing: " + editFile.getFullPathName() } });

        saveCurrentEdit (ctx);
        loadNodeEdit (ctx, editFile);
        t.current = nodeId;

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", nodeId }, { "label", node->label } });
    }

    juce::var opTreeReset (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto nodeId = (int) (double) cmd.getProperty ("node", t.current);
        auto node = t.find (nodeId);

        if (node == nullptr)
            return noSuchNode (ctx, nodeId);

        auto baseFile = t.projectDir.getChildFile (
            "node-" + juce::String (nodeId) + ".base.tracktionedit");

        // Untergeordnete Nodes erben den Stand dieser Node — haengt an ihnen
        // Arbeit, waere der Reset dieser Node ein Widerspruch.
        for (auto& n : t.nodes)
            if (n.isActed() && n.id != nodeId && isAncestorOf (t, nodeId, n.id))
                return resultObj (false, { { "error", "nodes below have actions and depend on this node" } });

        if (! baseFile.existsAsFile())
            return resultObj (false, { { "error", "no base snapshot for node " + juce::String (nodeId) } });

        auto editFile = t.projectDir.getChildFile (node->file);

        if (! baseFile.copyFileTo (editFile))
            return resultObj (false, { { "error", "cannot restore base snapshot" } });

        if (nodeId == t.current)
            loadNodeEdit (ctx, editFile);

        // Reset raeumt die Aktionen — acted und Take-Daten fallen mit weg
        node->actions.clear();

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", nodeId }, { "note", "node reset to base snapshot" } });
    }

    juce::var opTreeDelete (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto nodeId = (int) (double) cmd.getProperty ("node", 0.0);
        auto node = t.find (nodeId);

        if (node == nullptr)
            return noSuchNode (ctx, nodeId);

        if (nodeId == t.root)
            return resultObj (false, { { "error", "cannot delete the seed" } });

        // Teilbaum sammeln (node + alle Nachfahren)
        juce::Array<int> subtree;
        subtree.add (nodeId);

        bool changed = true;

        while (changed)
        {
            changed = false;

            for (auto& n : t.nodes)
                if (! subtree.contains (n.id) && subtree.contains (n.parent))
                {
                    subtree.add (n.id);
                    changed = true;
                }
        }

        for (auto id : subtree)
        {
            auto* n = t.find (id);

            if (n->isActed())
                return resultObj (false, { { "error", juce::String::fromUTF8 ("node has actions — terminate it instead") } });

            if (n->favorite)
                return resultObj (false, { { "error", "node is a favorite" } });
        }

        // Parent-ID vor dem Entfernen sichern — der node-Zeiger haengt
        // danach im freigegebenen Array-Speicher
        auto fallbackParent = node->parent;

        for (auto id : subtree)
        {
            t.projectDir.getChildFile ("node-" + juce::String (id) + ".tracktionedit").deleteFile();
            t.projectDir.getChildFile ("node-" + juce::String (id) + ".base.tracktionedit").deleteFile();
        }

        for (int i = t.nodes.size(); --i >= 0;)
            if (subtree.contains (t.nodes.getUnchecked (i).id))
                t.nodes.remove (i);

        // Fiel die aktuelle Node in den geloeschten Teilbaum, geht es beim Parent weiter
        if (subtree.contains (t.current))
        {
            t.current = fallbackParent;
            auto parentFile = t.projectDir.getChildFile (t.find (t.current)->file);
            loadNodeEdit (ctx, parentFile);
        }

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", nodeId }, { "removed", subtree.size() } });
    }

    juce::var opTreeTerminateFavorite (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto op = cmd.getProperty ("op", {}).toString();
        auto nodeId = (int) (double) cmd.getProperty ("node", t.current);
        auto node = t.find (nodeId);

        if (node == nullptr)
            return noSuchNode (ctx, nodeId);

        auto on = cmd.getProperty ("on", {});

        if (op == "treeTerminate")
        {
            auto terminating = on.isVoid() ? node->status != "terminated" : (bool) on;

            if (terminating)
            {
                if (node->favorite)
                    return resultObj (false, { { "error", "node is on the path to a favorite" } });

                for (auto& n : t.nodes)
                    if (n.favorite && isAncestorOf (t, nodeId, n.id))
                        return resultObj (false, { { "error", "node is on the path to a favorite" } });

                node->status = "terminated";
                node->favorite = false;

                for (auto& n : t.nodes)
                    if (isAncestorOf (t, nodeId, n.id))
                    {
                        n.status = "terminated";
                        n.favorite = false;
                    }
            }
            else
            {
                node->status = "active";

                for (auto& n : t.nodes)
                    if (isAncestorOf (t, nodeId, n.id))
                        n.status = "active";
            }
        }
        else
        {
            if (node->status == "terminated")
                return resultObj (false, { { "error", "node is terminated" } });

            auto favoriting = on.isVoid() ? ! node->favorite : (bool) on;

            if (favoriting)
            {
                for (auto cur = t.find (nodeId); cur != nullptr; cur = t.find (cur->parent))
                {
                    if (cur->status == "terminated")
                        return resultObj (false, { { "error", "ancestor is terminated" } });

                    cur->favorite = true;
                }
            }
            else
            {
                for (auto& n : t.nodes)
                    if (n.favorite && isAncestorOf (t, nodeId, n.id))
                        return resultObj (false, { { "error", "node is not the end of a favorite path" } });

                node->favorite = false;
            }
        }

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", nodeId },
                                  { op == "treeTerminate" ? "status" : "favorite",
                                    op == "treeTerminate" ? juce::var (node->status) : juce::var (node->favorite) } });
    }

    juce::var opTreeLabel (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto nodeId = (int) (double) cmd.getProperty ("node", t.current);
        auto node = t.find (nodeId);
        auto label = cmd.getProperty ("label", {}).toString();

        if (node == nullptr)
            return noSuchNode (ctx, nodeId);

        if (label.isEmpty())
            return resultObj (false, { { "error", "no label" } });

        node->label = label;

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        return resultObj (true, { { "node", nodeId }, { "label", label } });
    }

    // Notiz am Node: was dort gemacht wurde und warum — fuer Menschen, die
    // den Baum spaeter lesen (Agenten fuellen sie im Durchlauf). text setzt
    // die Notiz ("" loescht), append haengt eine Zeile an. Baum-Daten wie
    // das Label: kein Undo, keine Node-Aktion.
    juce::var opTreeNote (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto nodeId = (int) (double) cmd.getProperty ("node", t.current);
        auto* node = t.find (nodeId);

        if (node == nullptr)
            return noSuchNode (ctx, nodeId);

        // note als Alias fuer text (Praxistest remix-1: treeNote {note} -> "no text")
        const auto key = cmd.hasProperty ("text") ? "text" : "note";

        if (! cmd.hasProperty (key))
            return resultObj (false, { { "error", "no text: treeNote {text, node?, append?}" } });

        auto text = cmd.getProperty (key, {}).toString().trim();
        const bool append = (bool) cmd.getProperty ("append", false);

        if (append && text.isEmpty())
            return resultObj (false, { { "error", "nothing to append" } });

        auto note = append && node->note.isNotEmpty() ? node->note + "\n" + text : text;

        // Eine andere Notiz ueberschreiben sagen (Praxistest 3: ein trackMix
        // landete im Node davor, die neue Notiz ersetzte dessen Notiz)
        const bool replaced = ! append && node->note.isNotEmpty() && node->note != text && text.isNotEmpty();
        const auto previous = node->note;

        // 4000 reichte fuer einen Abschlussbericht knapp nicht (Praxistest 1c)
        if (note.length() > 16000)
            return resultObj (false, { { "error", "note too long (max 16000 characters)" } });

        node->note = note;
        node->noteUpdated = note.isEmpty() ? juce::String() : juce::Time::getCurrentTime().toISO8601 (true);

        if (! t.save())
            return resultObj (false, { { "error", "cannot write tree.json" } });

        auto r = resultObj (true, { { "node", nodeId }, { "note", note } });

        if (replaced)
        {
            r.getDynamicObject()->setProperty ("replaced", previous);
            r.getDynamicObject()->setProperty ("hint", "replaced the earlier note of node " + juce::String (nodeId) + " '" + node->label
                                                          + "' (shown in 'replaced') - append: true adds to a note instead");
        }

        return r;
    }
}

