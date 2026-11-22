#pragma once

#include "Dawd.h"

// Routing-Matrix: Spuren (Quellen) laufen frei durch Plugin-Module bis Main.
// Ein Modul ist genau ein Plugin auf einer eigenen, versteckten Bus-Spur;
// Kanten sind Daten im Edit (jeder Baum-Knoten hat sein eigenes Routing):
//
//   X -> pN    = AuxSend (Bus N, Gain = Pegel der Kante) am Ende der Kette
//                von X; das Modul pN ist [AuxReturn Bus N][Rack(Plugin),
//                Dry/Wet]. Send und Return tragen MIDI mit.
//   X -> main  = AuxSend auf Bus 0; die Main-Spur [AuxReturn Bus 0] geht
//                auf den Master
//   Ausgang    = jede Spur und jedes Modul gibt ihren Track-Output in die
//                Void-Spur (Lautstaerke -inf, nicht gemutet — ein Mute am
//                Ziel schaltet die Sends der Quellen mit stumm)
//
// Typ-Regeln: MIDI-Signal nur in midiFx/instrument, Audio nur in audioFx
// und Main; ein Instrument macht aus MIDI Audio. Keine Schleifen.
namespace dawd
{
    class FeedbackReturnPlugin;

    // Spuren mit dawd-Rolle (Modul-Bus, Void) sind kein Material — alle
    // Spur-Indizes der API zaehlen nur die Nutzer-Spuren.
    bool isRoutingTrack (const te::AudioTrack&);
    juce::Array<te::AudioTrack*> userTracks (te::Edit&);

    // Neue Nutzer-Spur am Ende (Audio, haengt an Main).
    te::AudioTrack* appendUserTrack (te::Edit&);

    // Freeze (ops/Freeze.cpp): das gerenderte Audio einer Spur liegt auf
    // einer versteckten Spur (Rolle "freeze"), die wie eine Lane in ihre
    // Gruppenspur laeuft — Mute und Solo der Spur gelten mit. clipTracks:
    // Nutzer-Spuren und Freeze-Spuren (die Section-Ops schneiden beide).
    te::AudioTrack* freezeTrackOf (te::Edit&, const te::AudioTrack& group);
    te::AudioTrack* addFreezeTrack (te::Edit&, te::AudioTrack& group);
    juce::Array<te::AudioTrack*> clipTracks (te::Edit&);

    // Eigene Kette einer Spur (Simple-Sicht wie routing.chains): Module der
    // Reihe nach; complex sammelt, was davon abweicht (Verzweigung, Send
    // hinter einem Modul vor dem letzten, Feedback, Pegel != 100 %), sends
    // die Ausgaenge der Spur ausser dem ersten Modul. Sends hinter dem
    // letzten Modul sind Post-Fader-Sends (routing.chains postSends; Ziele
    // in postSends, wenn gegeben).
    juce::StringArray ownChainOf (te::Edit&, const juce::String& trackId, juce::StringArray& complex,
                                  juce::StringArray& sends, juce::StringArray* postSends = nullptr);

    // Gruppenspur einer Lane (ihr Output-Ziel, wenn das eine Nutzer-Spur
    // ist), sonst nullptr — eine Gruppe laeuft in die Void-Spur, nicht in
    // eine Lane-Gruppe.
    te::AudioTrack* laneGroupOf (const te::AudioTrack&);

    // Spurtyp liegt am Track (dawdType); alte Edits ohne Feld: "hat Synth".
    bool isMidiTrack (const te::AudioTrack&);

    // Typ setzen; Kanten, die danach nicht mehr passen, fallen weg (eine
    // MIDI-Spur kann nicht auf Main oder in einen Audio-Effekt).
    void setTrackType (te::Edit&, te::AudioTrack&, bool midi);

    // Void-Spur anlegen und alte Edits migrieren (Plugins auf Spuren ->
    // Modulkette, 4OSC auf MIDI-Spuren -> Instrument-Modul, Lane-Synths
    // raus). Idempotent — laeuft bei jedem Laden.
    void ensureRouting (te::Edit&);

    // Modul-Klassen
    bool isValidPluginClass (const juce::String&);
    juce::String classInput (const juce::String& cls);   // midi | audio
    juce::String classOutput (const juce::String& cls);  // midi | audio

    // Knoten im Routing-Graph: "t<i>" (Nutzer-Spur, keine Lane), "p<N>"
    // (Modul), "main". error ist gesetzt, wenn die Id nicht aufloest.
    struct RouteNode
    {
        juce::String id, kind, in, out, cls, error;
        te::AudioTrack* track = nullptr;
    };

    RouteNode findRouteNode (te::Edit&, const juce::String& id);
    juce::String trackNodeId (te::Edit&, const te::AudioTrack&);

    // Ziele eines Knotens (Ids), Reihenfolge: Module nach Bus, dann main.
    juce::StringArray targetsOf (te::Edit&, const RouteNode&);

    // Der Fader, der eine Spur lauter/leiser macht: am letzten Modul ihrer
    // eigenen Kette (nach den Effekten, vor der Summe). Ohne eigene Kette,
    // bei Verzweigungen oder geteiltem Instrument: bei Audio-Spuren ihr
    // eigener, bei MIDI-Spuren der am Ausgang ihres Instruments — ueber
    // MIDI-Effekte hinweg das erste Instrument im Routing. nullptr:
    // MIDI-Spur ohne Instrument. sharedWith: andere Knoten, die dasselbe
    // Instrument speisen; instrumentId: das Instrument (auch wenn der Fader
    // weiter hinten sitzt).
    te::AudioTrack* faderTrackOf (te::Edit&, te::AudioTrack& track, juce::StringArray* sharedWith = nullptr,
                                  juce::String* instrumentId = nullptr);
    juce::String faderIdOf (te::Edit&, te::AudioTrack& fader);   // "p3" bzw. "t0"

    // Wandert der Fader-Ort (Kette geaendert, alte Projekte mit dem Fader
    // vor der Kette), ziehen Pegel, Pan und ihre Automation mit. Nach jeder
    // veraendernden Op und beim Laden.
    void syncFaders (te::Edit&);

    // Grund, warum from -> to nicht geht ("" = erlaubt). Die Texte sind
    // API-Vertrag (UI-Tooltip, Tests).
    juce::String edgeError (te::Edit&, const RouteNode& from, const RouteNode& to);

    // Kante anlegen/entfernen, Pegel (linear 0..1) setzen — Rueckgabe:
    // Fehlertext oder "".
    // asSend markiert die Kante als Send (Simple-Mode: Spur -> Bus, nicht
    // Teil der Kette)
    juce::String connectNodes (te::Edit&, const juce::String& from, const juce::String& to, double level = 1.0,
                               bool asSend = false);
    bool isSendEdge (te::Edit&, const juce::String& from, const juce::String& to);
    juce::String disconnectNodes (te::Edit&, const juce::String& from, const juce::String& to);
    juce::String setEdgeLevel (te::Edit&, const juce::String& from, const juce::String& to, double level);
    double edgeLevel (te::Edit&, const juce::String& from, const juce::String& to);

    // Feedback-Kanten (Feedback.h): Audio aus from geht verzoegert (>= ein
    // Block) zurueck in das Modul to — auch in sich selbst und gegen die
    // Richtung des Graphen; Pegel 0..1 mit Soft-Clip. Sie zaehlen nicht fuer
    // den Schleifen-Check der normalen Kanten.
    juce::String connectFeedback (te::Edit&, const juce::String& from, const juce::String& to,
                                  double level, double delayMs);
    juce::String disconnectFeedback (te::Edit&, const juce::String& from, const juce::String& to);
    juce::String setFeedback (te::Edit&, const juce::String& from, const juce::String& to,
                              const juce::var& level, const juce::var& delayMs);

    // Grund, warum from -> to keine Feedback-Kante sein kann ("" = geht).
    juce::String feedbackError (te::Edit&, const RouteNode& from, const RouteNode& to);

    // Modul aus einem Plugin-State bauen (unverdrahtet) — Id "p<N>".
    juce::String addPluginModule (te::Edit&, const juce::ValueTree& pluginState, const juce::String& cls);
    te::AudioTrack* findModuleTrack (te::Edit&, const juce::String& id);   // id "p3" oder Name des Moduls

    // Name eines Moduls (Praxistest 1d: drei "x42-dpl" waren nur ueber p9,
    // p10, p11 zu unterscheiden). Leer = kein Name, das Modul heisst wie sein
    // Plugin. Ein Name gilt ueberall als Adresse statt der Id.
    juce::String moduleLabel (te::AudioTrack& moduleTrack);
    juce::String checkModuleLabel (te::Edit&, const juce::String& id, const juce::String& label);   // Fehlertext oder ""
    void setModuleLabel (te::AudioTrack& moduleTrack, const juce::String& label);
    juce::String moduleIdFor (te::Edit&, const juce::String& idOrLabel);   // "p3" oder "" 

    // Master-Kette (addPlugin at: master): Module markieren; mainEntry ist
    // der Eingang der Kette (erstes Master-Modul vor Main) oder "main" —
    // neue Spuren und Instrumente laufen dorthin statt an der Kette vorbei
    void markMasterModule (te::Edit&, const juce::String& id);
    bool isMasterModule (te::Edit&, const juce::String& id);
    juce::StringArray masterModules (te::Edit&);
    juce::String mainEntry (te::Edit&);
    te::Plugin* modulePlugin (te::AudioTrack& moduleTrack);   // das Plugin im Rack

    // Dry/Wet des Moduls (ein Knopf: wet = Anteil durchs Plugin); MIDI-
    // Effekte haben keins
    double moduleWet (te::AudioTrack& moduleTrack);
    juce::String setModuleWet (te::Edit&, const juce::String& id, double wet);
    bool removePluginModule (te::Edit&, const juce::String& id);

    // Verlinkte Instanzen (Simple-Mode): gleiche Einstellungen, eigenes
    // Signal. linkPluginModule kopiert das Plugin samt Zustand in ein neues
    // Modul derselben Link-Gruppe (Id der Gruppe = Id des ersten Moduls).
    juce::String moduleLink (te::AudioTrack& moduleTrack);
    juce::String linkPluginModule (te::Edit&, const juce::String& sourceId);
    juce::Array<te::AudioTrack*> linkedModules (te::Edit&, const juce::String& id);  // ohne id selbst

    // Kette einer Spur (Simple-Mode): Spur -> plugins[0] -> ... -> Main.
    // Ersetzt die bisherige Kette; Module der neuen Kette duerfen keine
    // Eingaenge von ausserhalb haben (geteilt -> verlinken). Fehler rollt
    // zurueck. Rueckgabe: Fehlertext oder "".
    juce::String setTrackChain (te::Edit&, int track, const juce::StringArray& plugins);
    juce::String setNodeChain (te::Edit&, const juce::String& trackOrGroupId, const juce::StringArray& plugins);

    // Gruppen (Submix, ops/Groups.cpp): ein Gruppen-Bus "g<N>" ist ein Knoten
    // der Matrix wie Main — versteckte Spur [AuxReturn Bus N] mit Fader,
    // Rolle "group", solo-isolated, Name als Adresse wie bei Modulen — und
    // laeuft ueber Kanten weiter (eigene Kette, Main). Mitglied ist, wessen
    // Weg nach Main durch die Gruppe laeuft: keine zweite Datenhaltung.
    juce::String addGroupBus (te::Edit&, const juce::String& name);   // "g<N>" oder ""
    te::AudioTrack* findGroupTrack (te::Edit&, const juce::String& idOrName);
    juce::String groupIdFor (te::Edit&, const juce::String& idOrName);   // "g3" oder ""
    juce::StringArray groupIds (te::Edit&);
    bool removeGroupBus (te::Edit&, const juce::String& id);   // nur der Bus samt Kanten in ihn

    // Der Fader einer Gruppe: am letzten Modul ihrer eigenen Kette, sonst
    // der Gruppen-Bus (wie faderTrackOf bei Spuren)
    te::AudioTrack* groupFaderOf (te::Edit&, te::AudioTrack& groupTrack);

    // Wege eines Knotens bis zum Ende (Main, Master-Kette, Gruppe) ueber
    // normale Kanten ohne Sends: besuchte Knoten, die End-Kanten und die
    // Gruppen darunter (die innersten — hinter einer Gruppe geht es nicht
    // weiter)
    struct GroupPath
    {
        juce::StringArray nodes, groups;
        std::vector<std::pair<juce::String, juce::String>> exits;
    };

    GroupPath groupPathOf (te::Edit&, const juce::String& nodeId);

    // Spur-Indizes (ohne Lanes), deren Weg in die Gruppe laeuft; nested:
    // auch ueber Gruppen, die in sie laufen
    juce::Array<int> groupMembers (te::Edit&, const juce::String& id, bool nested);

    // Fuer die Automation (Automation.cpp): die Engine-Plugins hinter
    // Modulen und Kanten. nullptr, wenn es sie nicht gibt.
    juce::StringArray moduleIds (te::Edit&);                                  // "p<N>", nach N
    te::AudioTrack* mainBusTrack (te::Edit&);                                 // Main (Summe), nullptr ohne Routing
    std::vector<std::pair<juce::String, juce::String>> routingEdges (te::Edit&);  // normale Kanten
    std::vector<std::pair<juce::String, juce::String>> routingFeedbackEdges (te::Edit&);
    te::RackInstance* moduleRackOf (te::AudioTrack& moduleTrack);
    te::AuxSendPlugin* edgeSend (te::Edit&, const juce::String& from, const juce::String& to);
    FeedbackReturnPlugin* feedbackReturn (te::Edit&, const juce::String& from, const juce::String& to);

    // Lese-Sicht: Knoten, Kanten und die Matrix (je Ausgang alle Eingaenge
    // mit connected und Grund) — das UI rendert sie, ohne Regeln zu kennen.
    // compact: ohne die Zell-Matrix (Gruende je Zelle fuer das UI) — fuer Agenten;
    // reasons (nur mit compact): dieselben Gruende je Quelle gruppiert
    // [{from, allowed, blocked: {Grund: [ids]}, feedback: {..}, sidechain: {..}}]
    juce::var routingState (te::Edit&, bool compact = false, bool reasons = false);
}
