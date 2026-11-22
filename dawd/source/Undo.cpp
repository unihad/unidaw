#include "Undo.h"
#include "EngineHelpers.h"

#include <set>

namespace dawd
{
    struct UndoHistory
    {
        std::vector<UndoStep> undo, redo;
        std::set<int> removed;   // von Undo entfernte Auto-Nodes (fuer "no such node")
    };

    namespace
    {
        UndoHistory& history (DawdContext& ctx)
        {
            if (! ctx.undoHistory)
                ctx.undoHistory = std::make_shared<UndoHistory>();

            return *ctx.undoHistory;
        }

        // Tiefe Kopie: params sind DynamicObjects, die spaeter veraendert werden koennten
        juce::Array<NodeAction> copyActions (const juce::Array<NodeAction>& actions)
        {
            juce::Array<NodeAction> out;

            for (auto a : actions)
            {
                a.params = juce::JSON::parse (juce::JSON::toString (a.params));
                out.add (a);
            }

            return out;
        }

        juce::File editFileOf (TreeState& t, const TreeNode& n)
        {
            return t.projectDir.getChildFile (n.file);
        }

        bool writeEdit (const juce::ValueTree& state, const juce::File& f)
        {
            auto xml = state.createXml();
            return xml != nullptr && xml->writeTo (f);
        }

        // Stand eines Nodes zurueckschreiben, Node aktuell machen, Edit laden
        juce::String applySnapshot (DawdContext& ctx, const UndoSnap& s)
        {
            auto& t = *ctx.tree;
            auto* n = t.find (s.node);

            if (n == nullptr)
                return "node " + juce::String (s.node) + " no longer exists";

            auto file = editFileOf (t, *n);

            if (! writeEdit (s.edit, file))
                return "cannot write " + file.getFileName();

            n->actions = copyActions (s.actions);
            t.current = s.node;

            if (! t.save())
                return "cannot write tree.json";

            loadNodeEdit (ctx, file);
            return {};
        }
    }

    bool isUndoableOp (const juce::String& op)
    {
        static const std::set<juce::String> ops = {
            "addSection", "updateSection", "moveSection", "addClip", "addMidiClip", "createTrack",
            "pickInstrument", "favoriteClip", "favoriteSection", "deleteClip", "undoDelete", "trimClip",
            "moveClip", "setMidiNotes", "editNotes", "setControllers", "quantizeClip", "addPlugin", "removePlugin", "renamePlugin", "renameTrack", "linkPlugin", "setChain", "connect",
            "disconnect", "setAutomation", "clearAutomation", "setModifier", "clearModifier", "setParameter", "loadPreset", "setPluginFile", "setEdge", "setWet",
            "trackMix", "laneActive", "setSamples", "splitClip", "duplicateClip", "duplicateSection", "setClip", "deleteSection", "addMidiFromAudio", "stretchClip", "warpClip", "loopClip", "processClip",
            "freezeTrack", "flattenTrack", "compLane", "createGroup", "setGroup", "removeGroup", "groupMix",
            "addMarker", "updateMarker", "deleteMarker",
            "pluginWindow",   // nur das Schliessen nach einer Aenderung im Editor (executeOp)
            "bounceNode",     // legt einen Node an: Undo nimmt ihn weg wie einen Auto-Node
        };

        return ops.count (op) > 0;
    }

    bool clearsUndo (const juce::String& op)
    {
        return op == "treeClose" || op == "treeOpen" || op == "treeSeed" || op == "treeReset"
            || op == "treeDelete" || op == "treeCompact" || op == "newEdit";
    }

    bool canSnapshot (DawdContext& ctx)
    {
        if (! ctx.tree || ctx.edit == nullptr || isRecording (ctx))
            return false;

        auto* cur = ctx.tree->find (ctx.tree->current);
        return cur != nullptr && te::EditFileOperations (*ctx.edit).getEditFile() == editFileOf (*ctx.tree, *cur);
    }

    UndoSnap takeSnapshot (DawdContext& ctx)
    {
        UndoSnap s;
        s.node = ctx.tree->current;
        s.actions = copyActions (ctx.tree->find (s.node)->actions);
        ctx.edit->flushState();
        s.edit = ctx.edit->state.createCopy();
        applySavedListening (s.edit, listeningMutes (ctx, *ctx.edit));   // Abhoer-Hilfen sind kein Teil des Stands
        return s;
    }

    void pushUndo (DawdContext& ctx, UndoStep step)
    {
        auto& h = history (ctx);
        step.time = juce::Time::getMillisecondCounter();

        // Fader-Geste ueber OSC: Werte alle 60 ms, aber ein Schritt
        if (step.mergeKey.isNotEmpty() && ! h.undo.empty())
            if (auto& prev = h.undo.back(); prev.mergeKey == step.mergeKey && prev.after.node == step.before.node
                                            && step.time - prev.time < gestureGapMs)
            {
                prev.after = std::move (step.after);
                prev.time = step.time;
                h.redo.clear();
                return;
            }

        h.undo.push_back (std::move (step));

        if ((int) h.undo.size() > maxUndoSteps)
            h.undo.erase (h.undo.begin());

        h.redo.clear();
    }

    void clearUndo (DawdContext& ctx)
    {
        auto& h = history (ctx);
        h.undo.clear();
        h.redo.clear();
        h.removed.clear();
    }

    void forgetNode (DawdContext& ctx, int node)
    {
        auto& h = history (ctx);

        for (auto* list : { &h.undo, &h.redo })
            list->erase (std::remove_if (list->begin(), list->end(), [node] (const UndoStep& st)
                         { return st.before.node == node || st.after.node == node || st.createdNode.id == node; }),
                         list->end());
    }

    juce::String undoLast (DawdContext& ctx, juce::String& op, TreeNode* removed)
    {
        auto& h = history (ctx);

        if (h.undo.empty())
            return "nothing to undo";

        auto step = h.undo.back();
        auto& t = *ctx.tree;
        op = step.op;

        // Per Auto-Branch angelegter Node: verschwindet — aber nicht, wenn
        // inzwischen von Hand weiterverzweigt wurde
        if (step.createdNode.id > 0)
            for (auto& n : t.nodes)
                if (n.parent == step.createdNode.id)
                    return "node " + juce::String (step.createdNode.id) + juce::String::fromUTF8 (" has branches now — undo stops here");

        flushAutosave (ctx);

        if (auto err = applySnapshot (ctx, step.before); err.isNotEmpty())
        {
            h.undo.pop_back();   // nicht mehr anwendbar — verwerfen
            return err;
        }

        if (step.createdNode.id > 0)
        {
            // Redo holt den Node so zurueck, wie er jetzt ist — mit Notiz
            // und Label, die nach der Op dazukamen
            if (auto* n = t.find (step.createdNode.id))
                step.createdNode = *n;

            if (removed != nullptr)
                *removed = step.createdNode;

            h.removed.insert (step.createdNode.id);

            for (int i = t.nodes.size(); --i >= 0;)
                if (t.nodes.getReference (i).id == step.createdNode.id)
                    t.nodes.remove (i);

            t.projectDir.getChildFile (step.createdNode.file).deleteFile();
            t.projectDir.getChildFile ("node-" + juce::String (step.createdNode.id) + ".base.tracktionedit").deleteFile();
            t.save();
        }

        h.undo.pop_back();
        h.redo.push_back (step);
        return {};
    }

    juce::String redoLast (DawdContext& ctx, juce::String& op)
    {
        auto& h = history (ctx);

        if (h.redo.empty())
            return "nothing to redo";

        auto step = h.redo.back();
        auto& t = *ctx.tree;
        op = step.op;
        flushAutosave (ctx);

        // Angelegten Node wiederherstellen: Datensatz, Basis = Stand des Elternknotens
        if (step.createdNode.id > 0 && t.find (step.createdNode.id) == nullptr)
        {
            if (t.find (step.createdNode.parent) == nullptr)
            {
                h.redo.pop_back();
                return "node " + juce::String (step.createdNode.parent) + " no longer exists";
            }

            writeEdit (step.createdBase.isValid() ? step.createdBase : step.before.edit, t.projectDir.getChildFile (
                "node-" + juce::String (step.createdNode.id) + ".base.tracktionedit"));
            t.nodes.add (step.createdNode);
            t.nextId = juce::jmax (t.nextId, step.createdNode.id + 1);
            h.removed.erase (step.createdNode.id);
        }

        if (auto err = applySnapshot (ctx, step.after); err.isNotEmpty())
        {
            h.redo.pop_back();
            return err;
        }

        h.redo.pop_back();
        h.undo.push_back (step);
        return {};
    }

    juce::String removedByUndo (DawdContext& ctx, int node)
    {
        auto& h = history (ctx);

        if (h.removed.count (node) == 0)
            return {};

        for (auto& st : h.redo)
            if (st.createdNode.id == node)
                return "node " + juce::String (node) + " was removed by undo; redo brings it back"
                     + (st.createdNode.note.isNotEmpty() ? " with its note" : "");

        return "node " + juce::String (node) + " was removed by undo; redo is no longer possible (a newer op dropped it)";
    }

    juce::var undoInfo (DawdContext& ctx)
    {
        auto& h = history (ctx);
        return resultObj (true, { { "canUndo", ! h.undo.empty() },
                                  { "canRedo", ! h.redo.empty() },
                                  { "undoOp", h.undo.empty() ? juce::String() : h.undo.back().op },
                                  { "redoOp", h.redo.empty() ? juce::String() : h.redo.back().op } });
    }
}
