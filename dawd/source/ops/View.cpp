#include "Ops.h"
#include "../EngineHelpers.h"
#include "../Automation.h"
#include "../Undo.h"
#include "../Routing.h"
#include "../PluginWindows.h"
#include "../PluginSandbox.h"
#include "../LiveLoudness.h"

#include <map>

namespace dawd
{
    juce::var opState (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        int clipCount = 0;
        juce::Array<juce::var> trackTypes;
        juce::Array<juce::var> trackList;
        int trackIndex = 0;

        // Favoriten-Material: je Ziel entscheidet die letzte select-Aktion
        // am aktuellen Node — die lineare Ansicht zeigt daraus die Sterne.
        const juce::Array<NodeAction>* sel = nullptr;

        if (ctx.tree)
            if (auto* cur = ctx.tree->find (ctx.tree->current))
                sel = &cur->actions;

        for (auto t : userTracks (edit))
        {
            clipCount += t->getClips().size();

            bool midiTrack = isMidiTrack (*t);
            trackTypes.add (midiTrack ? "midi" : "audio");

            // Lane-Track: Output laeuft in einen Zieltrack (Index melden)
            int destIndex = -1;

            if (auto* dest = laneGroupOf (*t))
            {
                int di = 0;

                for (auto* t2 : userTracks (edit))
                {
                    if (t2 == dest) { destIndex = di; break; }
                    ++di;
                }
            }

            // Ziele der Spur in der Routing-Matrix (Lanes: leer, sie laufen
            // in ihre Gruppe) — der Track-Header zeigt "-> Reverb, Main"
            juce::Array<juce::var> routes;

            if (destIndex < 0)
            {
                auto node = findRouteNode (edit, "t" + juce::String (trackIndex));

                for (auto& to : targetsOf (edit, node))
                {
                    juce::String name = "Main";

                    if (auto* m = findModuleTrack (edit, to))
                    {
                        if (auto* p = modulePlugin (*m))
                            name = p->getName();

                        if (moduleLabel (*m).isNotEmpty())
                            name = moduleLabel (*m);   // der Name des Moduls (renamePlugin)
                    }
                    else if (auto* g = findGroupTrack (edit, to))
                        name = moduleLabel (*g);   // Gruppe (Submix)

                    routes.add (resultObj (true, { { "id", to }, { "name", name } }));
                }
            }

            juce::Array<juce::var> clips;

            for (auto* clip : t->getClips())
            {
                auto pos = clip->getPosition();
                auto co = resultObj (true, {
                    { "name", clip->getName() },
                    { "start", pos.getStart().inSeconds() },
                    { "length", pos.getLength().inSeconds() },
                    { "type", dynamic_cast<te::MidiClip*> (clip) != nullptr ? "midi" : "audio" },
                    { "mute", clip->isMuted() },
                    { "favorite", sel != nullptr && TreeNode::lastSelectOn (*sel, "favoriteClip",
                                                                            clip->getName(), trackIndex) } });

                // Kopien aus duplicateClip: Name des Originals (setMidiNotes copies)
                if (auto pattern = clip->state.getProperty ("dawdPattern").toString(); pattern.isNotEmpty())
                    co.getDynamicObject()->setProperty ("pattern", pattern);

                // Teil eines Comps (compLane): aus welcher Lane
                if (auto* from = compSourceOf (edit, *clip))
                    co.getDynamicObject()->setProperty ("compFrom", from->getName());

                // Clip-Gain, Blenden, rueckwaerts, Crossfade (setClip)
                if (auto* ac = dynamic_cast<te::AudioClipBase*> (clip))
                {
                    co.getDynamicObject()->setProperty ("gainDb", std::round (ac->getGainDB() * 100.0) / 100.0);
                    co.getDynamicObject()->setProperty ("fadeIn", ac->getFadeIn().inSeconds());
                    co.getDynamicObject()->setProperty ("fadeOut", ac->getFadeOut().inSeconds());
                    co.getDynamicObject()->setProperty ("fadeCurve", fadeCurveName (ac->getFadeInType()));
                    co.getDynamicObject()->setProperty ("fadeCurveAuto", isFadeCurveAuto (*ac));
                    co.getDynamicObject()->setProperty ("reversed", ac->getIsReversed());
                    co.getDynamicObject()->setProperty ("transpose", (double) ac->getPitchChange());
                    co.getDynamicObject()->setProperty ("speed", ac->getSpeedRatio());
                    co.getDynamicObject()->setProperty ("followTempo", ac->getAutoTempo());
                    co.getDynamicObject()->setProperty ("crossfade", ac->getAutoCrossfade());

                    // Datei fehlt (geloescht, verschoben): Render und Analyse
                    // lehnen ab, das Flow-UI markiert den Clip
                    if (isClipFileMissing (*ac))
                    {
                        co.getDynamicObject()->setProperty ("missing", true);
                        co.getDynamicObject()->setProperty ("missingFile", ac->getOriginalFile().getFullPathName());
                    }

                    // Loop (loopClip) und Warp-Marker (warpClip), nur wenn gesetzt
                    if (auto loop = clipLoopOf (*ac); ! loop.isVoid())
                        co.getDynamicObject()->setProperty ("loop", loop);

                    if (isWarped (*ac))
                    {
                        auto* warp = new juce::DynamicObject();
                        warp->setProperty ("sourceBpm", std::round (ac->getLoopInfo().getBpm (te::AudioFile (edit.engine, ac->getOriginalFile()).getInfo()) * 100.0) / 100.0);
                        // compact (MCP): detect setzt einen Marker je Beat —
                        // lange Listen nur als Zahl, das UI fragt ohne compact
                        auto list = warpMarkersOf (*ac);
                        warp->setProperty ("markerCount", list.size());

                        if (! (bool) cmd.getProperty ("compact", false) || list.size() <= 16)
                            warp->setProperty ("markers", list);
                        co.getDynamicObject()->setProperty ("warp", juce::var (warp));
                    }

                    // Clip-FX (processClip): Schritte und Originaldatei
                    if (auto fx = clipFxOf (*ac); ! fx.isVoid())
                        co.getDynamicObject()->setProperty ("fx", fx);
                }
                else if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
                {
                    co.getDynamicObject()->setProperty ("gainDb", std::round (mc->getVolumeDb() * 100.0) / 100.0);
                    co.getDynamicObject()->setProperty ("transpose", (int) mc->state.getProperty ("dawdTranspose", 0));
                }

                clips.add (co);
            }

            // MIDI-Spuren: der Fader am Instrument (faderTrackOf)
            auto* faderT = faderTrackOf (edit, *t);
            auto* faderVol = faderT != nullptr ? faderT->getVolumePlugin() : nullptr;

            trackList.add (resultObj (true, {
                { "index", trackIndex },
                { "name", t->getName() },
                { "type", midiTrack ? "midi" : "audio" },
                { "clips", clips },
                { "dest", destIndex },
                { "routes", routes },
                { "mute", t->isMuted (false) },
                { "solo", t->isSolo (false) },
                { "frozen", freezeTrackOf (edit, *t) != nullptr },   // freezeTrack: Kette als Audio, Clips stumm
                { "volumeDb", faderVol != nullptr ? std::round (faderSetDb (*faderVol) * 100.0) / 100.0 : 0.0 },
                { "pan", faderVol != nullptr ? std::round (faderSetPan (*faderVol) * 1000.0) / 1000.0 : 0.0 },
                // Ort des Faders (Mixer: Sends davor sind pre-fader); MIDI ohne Instrument: keiner
                { "fader", faderT != nullptr ? juce::var (faderIdOf (edit, *faderT)) : juce::var() } }));

            // Comp-Lane: welche Lane wo spielt (compLane)
            if (isCompLane (*t))
                trackList.getReference (trackList.size() - 1).getDynamicObject()->setProperty ("comp", compRanges (edit, *t));

            // Gruppe (Submix), in die die Spur laeuft — die Timeline zeigt sie als Ordner
            if (destIndex < 0)
                if (auto g = groupOfTrack (edit, trackIndex); g.isNotEmpty())
                    trackList.getReference (trackList.size() - 1).getDynamicObject()->setProperty ("group", g);

            ++trackIndex;
        }

        // Sections nach Start sortiert — Reihenfolge im Song
        juce::Array<juce::var> sections;
        std::vector<std::pair<double, juce::var>> sectionList;

        // Tempo/Takt je Section (eigen oder geerbt) und die Laenge in Takten
        std::map<te::MarkerClip*, SectionTempo> tempoOf;
        {
            auto secs = sortedSections (edit);
            auto tempos = sectionTempos (edit);

            for (size_t i = 0; i < secs.size(); ++i)
                tempoOf[secs[i]] = tempos[i];
        }

            if (auto* mt = edit.getMarkerTrack())
                for (auto* clip : mt->getClips())
                    if (auto* mc = dynamic_cast<te::MarkerClip*> (clip))
                    {
                        auto pos = mc->getPosition();
                        auto tempo = tempoOf[mc];
                        sectionList.push_back ({ pos.getStart().inSeconds(),
                            resultObj (true, {
                                { "name", mc->getName() },
                                { "start", pos.getStart().inSeconds() },
                                { "end", pos.getEnd().inSeconds() },
                                { "length", pos.getLength().inSeconds() },
                                { "color", "#" + mc->getColour().toDisplayString (false) },
                                { "bpm", tempo.bpm },
                                { "timeSig", tempo.timeSig() },
                                { "bpmSet", tempo.bpmSet },
                                { "timeSigSet", tempo.sigSet },
                                { "bars", std::round (sectionBars (edit, *mc, tempo) * 1000.0) / 1000.0 },
                                { "ramp", tempo.ramp },
                                { "bpmEnd", tempo.bpmEnd },
                                { "favorite", sel != nullptr && TreeNode::lastSelectOn (*sel, "favoriteSection",
                                                                                        mc->getName(), -1) } }) });
                    }

        std::sort (sectionList.begin(), sectionList.end(), [] (auto& a, auto& b)
                   { return a.first < b.first; });

        for (auto& s : sectionList)
            sections.add (s.second);

        // Marker (benannte Zeitpunkte), nach Zeit
        juce::Array<juce::var> markers;

        for (auto& m : sortedMarkers (edit))
            markers.add (describeMarker (edit, m));

        auto* waveIn = ctx.engine->getDeviceManager().getDefaultWaveInDevice();
        auto& transport = edit.getTransport();
        auto loopRange = transport.getLoopRange();
        auto loop = new juce::DynamicObject();

        if (! loopRange.isEmpty())
        {
            loop->setProperty ("start", loopRange.getStart().inSeconds());
            loop->setProperty ("end", loopRange.getEnd().inSeconds());
        }

        loop->setProperty ("looping", (bool) transport.looping);

        // Input-Typen der geaermten Lanes — Multi-Track-Record kann gemischt
        // (audio + midi) laufen; recordingType (erster Typ) bleibt kompatibel
        juce::StringArray recTypes;

        for (auto& t : ctx.armedTakes)
            recTypes.addIfNotAlreadyThere (t.midi ? "midi" : "audio");

        juce::Array<juce::var> recTypeList;

        for (auto& t : recTypes)
            recTypeList.add (t);

        // Verfuegbare Eingaenge + Zuweisungen je Spur — die UI zeigt die
        // Zuweisung im Spur-Dropdown (collect)
        auto inputs = enumerateInputs (ctx);
        juce::Array<juce::var> trackInputs;

        for (int i = 0; i < trackList.size(); ++i)
        {
            juce::Array<juce::var> names;

            for (auto& inp : inputs)
                if (auto tracksVar = inp.getProperty ("tracks", {}); tracksVar.isArray())
                    for (auto& t : *tracksVar.getArray())
                        if ((int) (double) t == i)
                            { names.add (inp.getProperty ("name", {}).toString()); break; }

            if (auto* o = trackList.getReference (i).getDynamicObject())
            {
                o->setProperty ("inputs", names);
                o->setProperty ("armed", ! names.isEmpty());
            }
        }

        // Geaermt gilt fuer die Gruppe: waehrend eines Takes liegt die
        // Zuweisung auf der neuen Lane — ihre Gruppe bleibt geaermt
        for (auto& tv : trackList)
            if (auto dest = (int) tv.getProperty ("dest", -1); dest >= 0 && dest < trackList.size()
                && (bool) tv.getProperty ("armed", false))
                if (auto* o = trackList.getReference (dest).getDynamicObject())
                    o->setProperty ("armed", true);

        // Automatisierte Ziele mit Punkten: Lanes in der Timeline, Markierung
        // in der Routing-Matrix
        // Ziele mit LFO (modifier): Markierung an der Lane
        juce::Array<juce::var> automation, modifiers;

        for (auto& t : listAutomationTargets (edit, {}, true))
        {
            if (t.bindings.front().param->getCurve().getNumPoints() > 0)
                automation.add (describeAutomationTarget (t, true));

            if (! describeModifier (t).isVoid())
                modifiers.add (describeAutomationTarget (t, false));
        }

        auto result = resultObj (true, { { "length", edit.getLength().inSeconds() },
                                         { "tracks", (int) userTracks (edit).size() },
                                         { "trackTypes", trackTypes },
                                         { "trackList", trackList },
                                         { "routing", routingState (edit, (bool) cmd.getProperty ("compact", false), (bool) cmd.getProperty ("reasons", false)) },
                                         { "groups", groupList (edit) },
                                         { "automation", automation },
                                         { "modifiers", modifiers },
                                         { "undo", ctx.tree ? undoInfo (ctx) : juce::var() },
                                         { "sections", sections },
                                         { "markers", markers },
                                         { "clips", clipCount },
                                         { "monitor", ctx.monitorOn },
                                         { "recording", transport.isRecording() },
                                         { "recordingType", transport.isRecording()
                                                               ? (recTypes.isEmpty() ? juce::String() : recTypes[0])
                                                               : juce::String() },
                                         { "recordingTypes", recTypeList },
                                         { "position", transport.getPosition().inSeconds() },
                                         { "loop", juce::var (loop) },
                                         { "inputGain", waveIn ? (double) waveIn->getInputGainDb() : 0.0 },
                                         { "inputs", inputs },
                                         { "sampleRate", ctx.engine->getDeviceManager().deviceManager.getAudioDeviceSetup().sampleRate },
                                         { "time", juce::Time::getCurrentTime().toISO8601 (true) } });

        // Fehlende Mediendateien (Clips, Freeze-Audio, Sampler-Samples), nur
        // wenn es welche gibt
        if (auto missing = missingMedia (edit); ! missing.isEmpty())
            result.getDynamicObject()->setProperty ("missingFiles", missingMediaList (missing));

        // only: nur diese Teile ("sections" oder eine Liste) — Praxistest
        // stil-1: fuer die Sections kamen 30 000 Zeichen
        if (cmd.hasProperty ("only"))
        {
            auto onlyArg = cmd.getProperty ("only", {});
            juce::StringArray want, known;

            if (auto* a = onlyArg.getArray())
                for (auto& x : *a)
                    want.add (x.toString());
            else
                want.addTokens (onlyArg.toString(), ", ", {});

            want.removeEmptyStrings();

            for (auto& nv : result.getDynamicObject()->getProperties())
                if (nv.name.toString() != "ok")
                    known.add (nv.name.toString());

            for (auto& w : want)
                if (! known.contains (w) && w != "missingFiles")
                    return resultObj (false, { { "error", "only: no part " + w + " (" + known.joinIntoString (", ") + ")" } });

            auto part = resultObj (true, {});

            for (auto& w : want)
                if (result.hasProperty (w))
                    part.getDynamicObject()->setProperty (w, result.getProperty (w, {}));

            return part;
        }

        return result;
    }

    // Stoppt die Wiedergabe, wenn der Playhead das Ende erreicht (einmal
    // durchhoeren, kein Loop) — playEnd < 0 schaltet den Timer aus.
    void PlayStopTimer::timerCallback()
    {
        if (ctx == nullptr || ctx->edit == nullptr || ctx->playEnd < 0)
            { stopTimer(); return; }

        auto& transport = ctx->edit->getTransport();

        if (! transport.isPlaying())
            { ctx->playEnd = -1; stopTimer(); return; }

        if (transport.getPosition().inSeconds() >= ctx->playEnd)
        {
            ctx->playEnd = -1;
            stopTimer();

            auto* c = ctx;

            juce::MessageManager::callAsync ([c]
            {
                if (c->edit != nullptr)
                    c->edit->getTransport().stop (false, false);
            });
        }
    }

    juce::var opPlay (DawdContext& ctx, const juce::var& cmd)
    {
        auto& transport = ctx.edit->getTransport();
        auto on = (bool) cmd.getProperty ("on", ! transport.isPlaying());

        if (on)
        {
            auto loopRange = transport.getLoopRange();

            // Mit Loop-Marker: Bereich von Anfang bis Ende (oder als Loop).
            // Ohne: das ganze Stueck von vorne.
            auto startSec = ! loopRange.isEmpty() ? loopRange.getStart().inSeconds() : 0.0;

            // Optional "from": ab dieser Stelle spielen (Klick ins Lineal) —
            // das Ende bleibt Loop-Ende bzw. Song-Ende
            if (cmd.hasProperty ("from"))
                startSec = juce::jmax (0.0, (double) cmd.getProperty ("from", 0.0));

            // Erstes Play nach dem Laden baut den Audio-Graphen (prepareToPlay
            // aller Plugins) — im Ladestand sichtbar
            if (! transport.isPlayContextActive())
                ctx.load.setPhase ("audio graph");

            transport.setPosition (te::TimePosition::fromSeconds (startSec));
            transport.play (false);

            if (ctx.loudness != nullptr)
                ctx.loudness->restart();   // Integrated seit Play-Start

            ctx.playEnd = -1;
            ctx.playStop->stopTimer();

            if (! (bool) transport.looping)
            {
                ctx.playEnd = ! loopRange.isEmpty() ? loopRange.getEnd().inSeconds()
                                                    : ctx.edit->getLength().inSeconds();
                ctx.playStop->startTimerHz (10);
            }
        }
        else
        {
            transport.stop (false, false);
            ctx.playEnd = -1;
            ctx.playStop->stopTimer();
        }

        return resultObj (true, { { "playing", transport.isPlaying() } });
    }

    // Loop-Marker setzen (Sektion) oder Loop-Modus schalten.
    juce::var opLoop (DawdContext& ctx, const juce::var& cmd)
    {
        auto& transport = ctx.edit->getTransport();
        auto sectionName = cmd.getProperty ("section", {}).toString();

        if (cmd.hasProperty ("section"))
        {
            te::TimeRange range;

            if (auto* mt = ctx.edit->getMarkerTrack())
                for (auto* clip : mt->getClips())
                    if (auto* mc = dynamic_cast<te::MarkerClip*> (clip);
                        mc != nullptr && mc->getName() == sectionName)
                        range = mc->getPosition().time;

            if (range.isEmpty())
                return resultObj (false, { { "error", "no such section: " + sectionName } });

            transport.setLoopRange (range);
        }
        else if (! cmd.hasProperty ("on"))
        {
            // Ohne Sektion und ohne on: Loop-Marker loeschen
            transport.setLoopRange ({});
            transport.looping = false;
        }

        if (cmd.hasProperty ("on"))
            transport.looping = (bool) cmd.getProperty ("on", false);

        auto loopRange = transport.getLoopRange();
        auto loop = new juce::DynamicObject();

        if (! loopRange.isEmpty())
        {
            loop->setProperty ("start", loopRange.getStart().inSeconds());
            loop->setProperty ("end", loopRange.getEnd().inSeconds());
        }

        loop->setProperty ("looping", (bool) transport.looping);

        return resultObj (true, { { "loop", juce::var (loop) } });
    }

    // Leichtgewichtig fuer den Playhead-Poll (~10 Hz) — nichts weiter drin.
    // sandboxSeq: zaehlt bei Absturz/Neustart/Sperre eines Plugin-Hosts (das
    // UI laedt dann den Stand neu). windows: Module mit offenem Plugin-Fenster. Ein Fenster, das ueber seine
    // Titelleiste zuging, aendert keine Version — die Flow-UI gleicht ihre
    // Fenster-Knoepfe hierueber ab (sonst brauchte das Oeffnen zwei Klicks)
    juce::var opPosition (DawdContext& ctx, const juce::var&)
    {
        auto& transport = ctx.edit->getTransport();
        juce::Array<juce::var> windows;

        if (te::PluginWindowState::getNumOpenWindows (*ctx.edit) > 0)
            for (auto& id : moduleIds (*ctx.edit))
                if (auto* m = findModuleTrack (*ctx.edit, id))
                    if (auto* p = modulePlugin (*m); p != nullptr && pluginWindowOpen (*p))
                        windows.add (id);

        return resultObj (true, { { "position", transport.getPosition().inSeconds() },
                                  { "playing", transport.isPlaying() },
                                  { "recording", isRecording (ctx) },
                                  { "windows", windows },
                                  { "sandboxSeq", (juce::int64) sandboxStatusSeq() } });
    }

    // Solo/Mute pro Track: Solo -> nur Solo-Tracks hoerbar, kein Solo ->
    // alle ungemuteten Tracks. Die Audibility aktualisiert der Edit
    // selbst (Value-Listener auf mute/solo).
    juce::var opTrackMix (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) cmd.getProperty ("track", -1);
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track: " + juce::String (trackIndex) } });

        auto* track = tracks[trackIndex];

        // Mute ist Abhoer-Hilfe wie Solo (Decision 2026-10-20): der gespeicherte
        // Wert bleibt in Node-Datei, Undo und Render
        if (cmd.hasProperty ("mute"))
            listenMute (ctx, *track, (bool) cmd.getProperty ("mute", false));

        if (cmd.hasProperty ("solo"))
            track->setSolo ((bool) cmd.getProperty ("solo", false));

        // Lautstaerke (dB) und Pan (-1..1) am Fader der Spur — vor den Sends,
        // wirkt also auf alle Wege ins Routing. MIDI-Spuren tragen kein Audio:
        // dort sitzt der Fader am Ausgang ihres Instruments. Ist der
        // Parameter automatisiert, gilt beim Abspielen die Kurve.
        juce::StringArray shared;
        juce::String instrument;
        auto* faderTrack = faderTrackOf (*ctx.edit, *track, &shared, &instrument);
        auto* vol = faderTrack != nullptr ? faderTrack->getVolumePlugin() : nullptr;

        if (cmd.hasProperty ("volumeDb") || cmd.hasProperty ("pan"))
        {
            if (faderTrack == nullptr)
                return resultObj (false, { { "error", "MIDI track " + juce::String (trackIndex)
                                                      + juce::String::fromUTF8 (" is not routed through an instrument — nothing to set the level of (pickInstrument or connect it)") } });

            if (vol == nullptr)
                return resultObj (false, { { "error", "track has no volume" } });

            auto db = (double) cmd.getProperty ("volumeDb", 0.0);
            auto pan = (double) cmd.getProperty ("pan", 0.0);

            if (cmd.hasProperty ("volumeDb") && (db < -100.0 || db > 6.0))
                return resultObj (false, { { "error", juce::String::fromUTF8 ("volumeDb must be -100..6 (engine fader) — for more, raise the instrument's own volume/output parameter or put a Limiter behind it (Gain up to +24 dB)") } });

            if (cmd.hasProperty ("pan") && (pan < -1.0 || pan > 1.0))
                return resultObj (false, { { "error", "pan must be -1..1" } });

            if (cmd.hasProperty ("volumeDb"))
                vol->setVolumeDb ((float) db);

            if (cmd.hasProperty ("pan"))
                vol->setPan ((float) pan);
        }

        auto res = resultObj (true, { { "track", trackIndex },
                                      { "mute", track->isMuted (false) },
                                      { "solo", track->isSolo (false) },
                                      { "volumeDb", vol != nullptr ? std::round (faderSetDb (*vol) * 100.0) / 100.0 : 0.0 },
                                      { "pan", vol != nullptr ? std::round (faderSetPan (*vol) * 1000.0) / 1000.0 : 0.0 } });

        if (faderTrack != nullptr && faderTrack != track)
        {
            const auto at = faderIdOf (*ctx.edit, *faderTrack);
            res.getDynamicObject()->setProperty ("fader", at);   // Modul am Ende der Kette bzw. Instrument
            // Klartext: fader "p4" las ein Agent als "der Fader ist der Reverb" (Praxistest stil-1)
            res.getDynamicObject()->setProperty ("faderAt", "after " + at + " (" + moduleLabel (*faderTrack) + ")"
                                                 + (findRouteNode (*ctx.edit, at).cls == "instrument" ? ", the track's instrument"
                                                                                                      : ", the last module of the track's chain"));
        }

        // Abhoer-Hilfe sagt es selbst (Praxistest 1m: der Agent hielt das
        // stumme Mute im Vergleich fuer einen Fehler) — nur beim Einschalten
        // (Entscheidung Matthias 2026-10-24)
        if ((bool) cmd.getProperty ("mute", false) || (bool) cmd.getProperty ("solo", false))
            res.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("mute and solo are listening aids for playback only — render, analyze, compareReference and the other measurements ignore them; "
                                                                                 "to measure a single track use analyze {track} or renderStems, to change the mix use volumeDb or the routing"));

        if (instrument.isNotEmpty() && ! shared.isEmpty())
        {
            juce::Array<juce::var> sw;

            for (auto& s : shared)
                sw.add (s);

            res.getDynamicObject()->setProperty ("sharedWith", sw);
        }

        return res;
    }

    // Lane-Auswahl: nur eine Lane pro Gruppe spielt. Die aktive Lane
    // ungemutet, die Geschwister gemutet. Die Hauptlane (der Zieltrack)
    // bleibt als Bus immer an — ihr Track-Mute wuerde die Lanes mit toeten;
    // ihre Clips bleiben unangetastet (Seed-Audio als Song-Grundlage
    // spielt immer weiter).
    juce::var opLaneActive (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) cmd.getProperty ("track", -1);
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track: " + juce::String (trackIndex) } });

        auto* lane = tracks[trackIndex];
        auto* dest = laneGroupOf (*lane);
        auto* groupHead = dest == nullptr ? lane : dest;

        for (auto* t : tracks)
            if (t != lane && laneGroupOf (*t) == groupHead)
                t->setMute (true);

        groupHead->setMute (false);
        lane->setMute (false);
        forgetLaneListening (ctx, *lane);

        return resultObj (true, { { "track", trackIndex } });
    }

    // Pegel-Poll (~10 Hz, nicht geloggt): Spitzenpegel seit dem letzten Poll
    // je Gruppenspur (nach Fader, vor den Sends — Lanes laufen in ihre Gruppe)
    // und fuer Main (die Summe). Ohne laufende Wiedergabe -100 dB. Gemessen
    // am Ort des Faders (faderTrackOf): nach der eigenen Kette, bei MIDI am
    // Instrument — die Spur selbst traegt dort nur MIDI. Dazu lufs
    // {momentary, shortTerm, integrated} am Main-Ausgang (LiveLoudness.h;
    // null ohne Messwert: ohne Geraet, noch keine 400 ms bzw. 3 s, Stille).
    juce::var opLevels (DawdContext& ctx, const juce::var&)
    {
        auto& edit = *ctx.edit;

        auto read = [&ctx] (te::AudioTrack& t) -> double
        {
            auto* lm = t.getLevelMeterPlugin();

            if (lm == nullptr)
                return -100.0;

            auto& m = ctx.meters[t.itemID.toString()];

            if (m.measurer != &lm->measurer)
            {
                // Ein neuer Measurer ohne vorherige Op kommt nicht vor (clearMeters).
                // Falls doch: der alte Client bleibt bis Prozessende liegen — der
                // alte Measurer koennte noch auf ihn zeigen
                if (m.client != nullptr)
                    ctx.retiredMeterClients.push_back (std::move (m.client));

                m.measurer = &lm->measurer;
                m.client = std::make_unique<te::LevelMeasurer::Client>();
                m.measurer->addClient (*m.client);
            }

            float db = -100.0f;

            for (int chan = 0; chan < 2; ++chan)
                db = juce::jmax (db, m.client->getAndClearAudioLevel (chan).dB);

            return std::round (juce::jmax (-100.0f, db) * 10.0f) / 10.0;
        };

        juce::Array<juce::var> tracks;
        auto users = userTracks (edit);
        std::map<te::AudioTrack*, double> peakOf;

        for (int i = 0; i < users.size(); ++i)
            if (laneGroupOf (*users[i]) == nullptr)
            {
                // Ein geteiltes Instrument einmal lesen (Lesen leert den Client)
                auto* at = faderTrackOf (edit, *users[i]);
                auto* m = at != nullptr ? at : users[i];

                if (! peakOf.count (m))
                    peakOf[m] = read (*m);

                tracks.add (resultObj (true, { { "track", i }, { "peakDb", peakOf[m] } }));
            }

        for (auto& t : tracks)
            t.getDynamicObject()->removeProperty ("ok");

        // Gruppen (Submix): am Fader der Gruppe
        juce::Array<juce::var> groups;

        for (auto& id : groupIds (edit))
            if (auto* g = findGroupTrack (edit, id))
            {
                auto o = resultObj (true, { { "group", id }, { "peakDb", read (*groupFaderOf (edit, *g)) } });
                o.getDynamicObject()->removeProperty ("ok");
                groups.add (o);
            }

        // Live-LUFS: der Poll haelt die Messung wach und nennt das Main-Paar
        // (Kanaele des Standard-Ausgangs im Geraet)
        auto lufs = new juce::DynamicObject();
        te::LoudnessMeter::Readings lr;

        if (ctx.loudness != nullptr)
        {
            int l = -1, r = -1;

            if (auto* out = ctx.engine->getDeviceManager().getDefaultWaveOutDevice())
            {
                auto& chans = out->getChannels();

                if (chans.getNumChannels() > 0)
                    l = chans[0].indexInDevice;

                if (chans.getNumChannels() > 1)
                    r = chans[1].indexInDevice;
            }

            ctx.loudness->touch (l, r);
            lr = ctx.loudness->readings();
        }

        // Gleiche Skala wie analyze: der Master steht auf 0 dB
        // (unityMasterVolume), der Geraeteausgang ist also Main wie im Render
        auto lu = [] (bool valid, float v)
        {
            return valid && v > -99.9f ? juce::var (std::round (v * 10.0) / 10.0) : juce::var();
        };
        lufs->setProperty ("momentary", lu (lr.momentaryValid, lr.momentaryLufs));
        lufs->setProperty ("shortTerm", lu (lr.shortTermValid, lr.shortTermLufs));
        lufs->setProperty ("integrated", lu (lr.integratedValid, lr.integratedLufs));

        auto* main = mainBusTrack (edit);
        return resultObj (true, { { "tracks", tracks }, { "groups", groups },
                                  { "mainDb", main != nullptr ? read (*main) : -100.0 },
                                  { "lufs", juce::var (lufs) },
                                  { "playing", edit.getTransport().isPlaying() } });
    }
}
