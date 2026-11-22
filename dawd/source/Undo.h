#pragma once

#include "Dawd.h"

// Undo/Redo ueber Schnappschuesse: vor und nach jeder rueckgaengig machbaren
// Op (isUndoableOp) merkt sich executeOp den Edit des Nodes (ValueTree-Kopie,
// nach flushState) und seine Aktionen. Undo schreibt den Vorher-Stand zurueck
// und laedt ihn; hat die Op per Auto-Branch einen Node angelegt, verschwindet
// er mit. Redo stellt den Nachher-Stand wieder her (samt angelegtem Node).
// Nur die Aktionen des betroffenen Nodes werden zurueckgedreht — Labels,
// Favoriten, Terminate am Baum bleiben. Die Historie gilt je Projekt; Ops,
// die den Baum umbauen (Reset, Delete, Compact, Projektwechsel), loeschen sie.
namespace dawd
{
    struct UndoSnap
    {
        int node = 0;
        juce::Array<NodeAction> actions;
        juce::ValueTree edit;
    };

    struct UndoStep
    {
        juce::String op;
        UndoSnap before, after;
        TreeNode createdNode;   // id > 0: die Op legte ihn an (Auto-Branch, bounceNode)
        juce::ValueTree createdBase;   // Basis-Stand des angelegten Nodes, wenn nicht der Vorher-Stand (bounceNode)
        // Eine Geste = ein Schritt: gleicher Schluessel wie der letzte Schritt
        // (OSC-Adresse eines Faders) und weniger als gestureGapMs danach ->
        // pushUndo zieht nur dessen Nachher-Stand nach. Leer = eigener Schritt.
        juce::String mergeKey;
        juce::uint32 time = 0;
    };

    constexpr juce::uint32 gestureGapMs = 1000;

    constexpr int maxUndoSteps = 50;

    // Ops, die der Undo erfasst: alle mit Node-Aktion (ausser record — die
    // Aufnahme endet ueber den Timer) und der Feinschliff am Edit
    bool isUndoableOp (const juce::String& op);

    // Ops, nach denen die Historie nicht mehr passt
    bool clearsUndo (const juce::String& op);

    // Schnappschuss moeglich: Baum offen, der Edit ist der des Nodes, keine Aufnahme
    bool canSnapshot (DawdContext&);
    UndoSnap takeSnapshot (DawdContext&);

    void pushUndo (DawdContext&, UndoStep step);
    void clearUndo (DawdContext&);

    // Schritte verwerfen, die einen (inzwischen entfernten) Node betreffen
    void forgetNode (DawdContext&, int node);

    // Fehlertext oder "", op = der zurueckgenommene / wiederholte Op;
    // removed = der Auto-Node, den der Undo entfernte (id 0: keiner)
    juce::String undoLast (DawdContext&, juce::String& op, TreeNode* removed = nullptr);
    juce::String redoLast (DawdContext&, juce::String& op);

    // Hat ein Undo den Node entfernt: Grund fuer "no such node", sonst ""
    juce::String removedByUndo (DawdContext&, int node);

    // Fuer state: {canUndo, canRedo, undoOp, redoOp}
    juce::var undoInfo (DawdContext&);
}
