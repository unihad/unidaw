#pragma once

#include "Dawd.h"

// OSC-Fernsteuerung (Live-Spiel): ein UDP-Empfang (eigener Thread mit
// Absender-Adresse, Nachrichten-Typen aus juce_osc), standardmaessig aus,
// nur auf 127.0.0.1 — eine andere Adresse nur ausdruecklich (oscSettings
// {address}); Pakete nur von 127.0.0.1 und den Absendern in allow, alle
// anderen verworfen und gezaehlt (rejected). Feste Adressen unter /dawd/ werden zu
// Befehlen der Op-Registry (play, loop, trackMix, setParameter) und laufen
// ueber executeOp: Guards, Version, Auto-Branch, Undo und events.jsonl wie
// bei der UI; das Event traegt "osc": <Adresse>. Kein Datei- oder
// Projektzugriff ueber OSC. Ausfuehrung auf dem Message-Thread (wie HTTP),
// Fader-Werte gebuendelt (je Adresse der letzte Wert, alle 60 ms), eine
// Fader-Geste ist ein Undo-Schritt (UndoStep::mergeKey). Adressen:
// oscAddresses() und ARCHITECTURE.md, Abschnitt OSC-Fernsteuerung.
namespace dawd
{
    // Beim Start: osc.json im --config-Ordner lesen, bei enabled lauschen
    void oscStartup (DawdContext&);
    // Vor dem Ende des Dispatch-Loops: Empfang schliessen
    void oscShutdown();

    // oscSettings {enabled?, port?, address?}: erst pruefen und binden, dann
    // speichern; ein Fehler aendert nichts. Ohne Parameter nur lesen.
    juce::var oscSettings (DawdContext&, const juce::var& cmd);
}
