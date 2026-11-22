#pragma once

#include <tracktion_engine/tracktion_engine.h>

// Song-Baum fuer dawd: Wurzel = Seed (Songidee), jeder Knoten ist ein Stand des
// Stuecks (eigene .tracktionedit-Datei im Projektordner), Kanten = Varianten.
// Zustand liegt in <projektordner>/tree.json (Schema v3).
//
// Aktionen am Knoten sind Datensaetze: kind (collect | select | process),
// op, params, file, created. acted und die Take-Daten (Typ, Section, Track
// fuer den + Button) werden daraus abgeleitet — nicht mehr als lose
// Einzelfelder gespeichert. v1- und v2-Dateien migriert der Loader einmalig.
struct NodeAction
{
    int id = 0;
    juce::String kind;      // collect | select | process
    juce::String op;        // addSection | record | addClip | favoriteClip | deleteClip | addPlugin | ...
    juce::var params;       // Aktionsparameter (section, track, ...)
    juce::String file;      // erzeugtes/verwendetes Medium zum Anhoeren
    juce::String created;   // ISO8601

    juce::var toVar() const
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("id", id);
        o->setProperty ("kind", kind);
        o->setProperty ("op", op);
        o->setProperty ("params", params);
        o->setProperty ("file", file);
        o->setProperty ("created", created);
        return juce::var (o);
    }

    static NodeAction fromVar (const juce::var& v)
    {
        NodeAction a;
        a.id = (int) (double) v.getProperty ("id", 0.0);
        a.kind = v.getProperty ("kind", {}).toString();
        a.op = v.getProperty ("op", {}).toString();
        a.params = v.getProperty ("params", {});
        a.file = v.getProperty ("file", {}).toString();
        a.created = v.getProperty ("created", {}).toString();
        return a;
    }
};

struct TreeNode
{
    int id = 0;
    int parent = 0;                 // 0 = Wurzel
    juce::String label;
    juce::String note;              // Notiz: was am Node gemacht wurde und warum (treeNote)
    juce::String noteUpdated;       // ISO8601 der letzten Aenderung
    juce::String status = "active"; // active | terminated
    bool favorite = false;
    juce::Array<NodeAction> actions;
    juce::String file;              // Dateiname relativ zum Projektordner
    juce::String created;           // ISO8601
    bool autoCreated = false;       // per Auto-Branch angelegt (undoDelete raeumt ihn, wenn er leer wird)
    juce::String focus;             // woran der Node arbeitet: "t<i>" Spur, "song", "" offen

    bool isActed() const { return ! actions.isEmpty(); }

    // Take-Daten fuer den + Button kommen aus der letzten Collect-Aktion;
    // der Take-Typ (audio/midi) steht in deren params. Eine Auto-Section
    // (addSection {auto: true}) ist Folge des Takes, kein eigener
    const NodeAction* lastCollectAction() const
    {
        for (int i = actions.size(); --i >= 0;)
            if (actions.getReference (i).kind == "collect" && ! (bool) actions.getReference (i).params.getProperty ("auto", false))
                return &actions.getReference (i);

        return nullptr;
    }

    // Letzte select-Aktion fuer ein Ziel (Clip: op + track + name, Section:
    // op + name) — der Favoriten-Zustand, false wenn nie markiert.
    static bool lastSelectOn (const juce::Array<NodeAction>& actions, const juce::String& op,
                              const juce::String& name, int track)
    {
        for (int i = actions.size(); --i >= 0;)
        {
            auto& a = actions.getReference (i);

            if (a.kind != "select" || a.op != op
                || a.params.getProperty ("name", {}).toString() != name)
                continue;

            if (track >= 0 && (int) (double) a.params.getProperty ("track", -1.0) != track)
                continue;

            return (bool) a.params.getProperty ("on", true);
        }

        return false;
    }

    // Favorisiertes Material am Node — je Ziel entscheidet die letzte
    // select-Aktion; der Tree-Filter "nur Favoriten" zeigt auch diese Nodes.
    bool hasFavorites() const
    {
        for (auto& a : actions)
            if (a.kind == "select" && (a.op == "favoriteClip" || a.op == "favoriteSection")
                && lastSelectOn (actions, a.op, a.params.getProperty ("name", {}).toString(),
                                 (int) (double) a.params.getProperty ("track", -1.0)))
                return true;

        return false;
    }
};

struct TreeState
{
    int version = 3;
    int root = 0;
    int current = 0;
    int nextId = 1;
    int nextActionId = 1;
    // Der Baum baut sich aus den Aktionen: eine Aktion einer anderen Sorte
    // als die des aktuellen (acted) Nodes legt vorher einen Kind-Node an
    // (OpRegistry.cpp, executeOp). Abschaltbar je Projekt (treeSettings).
    bool autoBranch = true;
    // Audio des Songs (audioSettings): Rate, auf die dawd das Geraet beim
    // Oeffnen stellt (0 = was das Geraet hat), Bit-Tiefe neuer Aufnahmen
    int sampleRate = 0;
    int recordBitDepth = 24;
    juce::Array<TreeNode> nodes;
    juce::File projectDir;

    TreeNode* find (int id)
    {
        for (auto& n : nodes)
            if (n.id == id)
                return &n;

        return nullptr;
    }

    const TreeNode* find (int id) const
    {
        for (auto& n : nodes)
            if (n.id == id)
                return &n;

        return nullptr;
    }

    // Neue Aktion an einen Knoten haengen — der einzige Schreibweg fuer
    // Aktionen. Speichern macht der Aufrufer (oder recordNodeAction in Dawd).
    NodeAction appendAction (int nodeId, const juce::String& kind, const juce::String& op,
                             const juce::var& params, const juce::String& file)
    {
        NodeAction a;
        a.id = nextActionId++;
        a.kind = kind;
        a.op = op;
        a.params = params;
        a.file = file;
        a.created = juce::Time::getCurrentTime().toISO8601 (true);

        if (auto* n = find (nodeId))
            n->actions.add (a);

        return a;
    }

    juce::var toVar() const
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("version", version);
        o->setProperty ("root", root);
        o->setProperty ("current", current);
        o->setProperty ("nextId", nextId);
        o->setProperty ("nextActionId", nextActionId);
        o->setProperty ("autoBranch", autoBranch);
        o->setProperty ("sampleRate", sampleRate);
        o->setProperty ("recordBitDepth", recordBitDepth);

        juce::Array<juce::var> list;

        for (auto& n : nodes)
        {
            auto no = new juce::DynamicObject();
            no->setProperty ("id", n.id);
            no->setProperty ("parent", n.parent);
            no->setProperty ("label", n.label);
            no->setProperty ("note", n.note);
            no->setProperty ("noteUpdated", n.noteUpdated);
            no->setProperty ("status", n.status);
            no->setProperty ("favorite", n.favorite);
            no->setProperty ("file", n.file);
            no->setProperty ("created", n.created);
            no->setProperty ("auto", n.autoCreated);
            no->setProperty ("focus", n.focus);

            juce::Array<juce::var> actions;

            for (auto& a : n.actions)
                actions.add (a.toVar());

            no->setProperty ("actions", actions);

            // Abgeleitete Felder — die UI und bestehende Clients lesen sie
            // weiter: acted + Herkunft aus der ersten Aktion, Take-Daten aus
            // der letzten Collect-Aktion, Favoriten-Material (select).
            auto acted = n.isActed();
            auto* first = acted ? &n.actions.getReference (0) : nullptr;
            auto* media = n.lastCollectAction();

            no->setProperty ("acted", acted);
            no->setProperty ("actedBy", first != nullptr ? first->op : juce::String());
            no->setProperty ("actionFile", first != nullptr ? first->file : juce::String());
            no->setProperty ("takeType", media != nullptr
                                                ? media->params.getProperty ("type", {}).toString()
                                                : juce::String());
            no->setProperty ("takeSection", media != nullptr
                                                ? media->params.getProperty ("section", {}).toString()
                                                : juce::String());
            no->setProperty ("takeTrack", media != nullptr
                                              ? (int) (double) media->params.getProperty ("track", -1.0)
                                              : -1);
            no->setProperty ("hasFavorites", n.hasFavorites());
            list.add (juce::var (no));
        }

        o->setProperty ("nodes", list);
        return juce::var (o);
    }

    // v2-Kinds (section | audio | midi | processing) auf die neuen mappen:
    // Clip-Edit und Plugins sind Process, alles andere Collect. Der Take-Typ
    // (audio/midi) stand im alten Kind — er wandert in die params, die
    // Ableitung liest ihn von dort.
    static NodeAction migrateAction (NodeAction a)
    {
        auto oldKind = a.kind;

        if (oldKind != "collect" && oldKind != "select" && oldKind != "process")
        {
            a.kind = (a.op == "deleteClip" || a.op == "trimClip" || a.op == "moveClip"
                      || a.op == "addPlugin") ? "process" : "collect";

            if (auto* po = a.params.getDynamicObject();
                po != nullptr && a.kind == "collect"
                && (a.op == "record" || a.op == "addClip" || a.op == "addMidiClip")
                && po->getProperty ("type").toString().isEmpty()
                && (oldKind == "audio" || oldKind == "midi"))
                po->setProperty ("type", oldKind);
        }

        return a;
    }

    static TreeState fromVar (const juce::var& v, const juce::File& dir)
    {
        TreeState t;
        t.projectDir = dir;
        t.version = (int) (double) v.getProperty ("version", 1.0);
        t.root = (int) (double) v.getProperty ("root", 0.0);
        t.current = (int) (double) v.getProperty ("current", 0.0);
        t.nextId = (int) (double) v.getProperty ("nextId", 1.0);
        t.nextActionId = (int) (double) v.getProperty ("nextActionId", 1.0);
        t.autoBranch = (bool) v.getProperty ("autoBranch", true);
        t.sampleRate = (int) v.getProperty ("sampleRate", 0);
        t.recordBitDepth = (int) v.getProperty ("recordBitDepth", 24);

        for (auto& nv : * v.getProperty ("nodes", {}).getArray())
        {
            TreeNode n;
            n.id = (int) (double) nv.getProperty ("id", 0.0);
            n.parent = (int) (double) nv.getProperty ("parent", 0.0);
            n.label = nv.getProperty ("label", {}).toString();
            n.note = nv.getProperty ("note", {}).toString();
            n.noteUpdated = nv.getProperty ("noteUpdated", {}).toString();
            n.status = nv.getProperty ("status", "active").toString();
            n.favorite = (bool) nv.getProperty ("favorite", false);
            n.file = nv.getProperty ("file", {}).toString();
            n.created = nv.getProperty ("created", {}).toString();
            n.autoCreated = (bool) nv.getProperty ("auto", false);
            n.focus = nv.getProperty ("focus", {}).toString();

            if (auto actionsVar = nv.getProperty ("actions", {}); actionsVar.isArray())
            {
                for (auto& av : *actionsVar.getArray())
                    n.actions.add (migrateAction (NodeAction::fromVar (av)));
            }
            else
            {
                // v1-Migration: die Altfelder werden zu einer Aktion.
                // actedBy sagt, wodurch der Node acted wurde; die Take-Daten
                // (Typ, Section, Track) wandern in deren params.
                auto acted = (bool) nv.getProperty ("acted", false);
                auto actedBy = nv.getProperty ("actedBy", {}).toString();

                if (acted && actedBy.isNotEmpty())
                {
                    NodeAction a;
                    a.id = t.nextActionId++;
                    a.op = actedBy;
                    a.file = nv.getProperty ("actionFile", {}).toString();
                    a.created = n.created;
                    a.kind = "collect";     // v1 kannte nur Struktur- und Media-Arbeit

                    auto params = new juce::DynamicObject();
                    params->setProperty ("type", nv.getProperty ("takeType", {}).toString());
                    params->setProperty ("section", nv.getProperty ("takeSection", {}).toString());
                    params->setProperty ("track", (int) (double) nv.getProperty ("takeTrack", -1.0));
                    a.params = juce::var (params);

                    n.actions.add (a);
                }
            }

            t.nodes.add (n);
        }

        t.version = 3;
        return t;
    }

    static juce::File treeFile (const juce::File& dir)
    {
        return dir.getChildFile ("tree.json");
    }

    bool save() const
    {
        return TreeState::treeFile (projectDir).replaceWithText (juce::JSON::toString (toVar()));
    }

    static bool load (TreeState& out, const juce::File& dir)
    {
        auto f = treeFile (dir);

        if (! f.existsAsFile())
            return false;

        auto v = juce::JSON::parse (f.loadFileAsString());

        if (! v.isObject())
            return false;

        out = fromVar (v, dir);
        return true;
    }
};
