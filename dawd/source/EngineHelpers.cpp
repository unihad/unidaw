#include "EngineHelpers.h"
#include "Origins.h"
#include "PluginSettings.h"
#include "PluginSandbox.h"

#include <algorithm>
#include <cmath>

namespace dawd
{
    te::AudioTrack* getOrInsertAudioTrackAt (te::Edit& edit, int index)
    {
        while (userTracks (edit).size() <= index)
        {
            edit.getTransport().stopIfRecording();

            if (appendUserTrack (edit) == nullptr)
                return nullptr;
        }

        return userTracks (edit)[index];
    }

    te::AudioTrack* makeLaneTrack (te::Edit& edit, te::AudioTrack& target)
    {
        auto* laneTrack = getOrInsertAudioTrackAt (edit, (int) userTracks (edit).size());
        // Die Lane laeuft in ihre Gruppe — kein eigener Send nach Main
        for (auto* p : laneTrack->pluginList.getPlugins())
            if (dynamic_cast<te::AuxSendPlugin*> (p) != nullptr)
                p->deleteFromParent();

        laneTrack->getOutput().setOutputToTrack (&target);
        laneTrack->getOutput().updateOutput();
        setTrackType (edit, *laneTrack, isMidiTrack (target));

        int laneNo = 1;

        for (auto* t : userTracks (edit))
            if (t != laneTrack && laneGroupOf (*t) == &target && ! isCompLane (*t))   // der Comp zaehlt nicht
                ++laneNo;

        laneTrack->setName (target.getName() + "." + juce::String (laneNo));
        return laneTrack;
    }

    void activateLane (te::Edit& edit, te::AudioTrack& lane)
    {
        auto* groupHead = &lane;

        if (auto* dest = laneGroupOf (lane))
            groupHead = dest;

        for (auto* t : userTracks (edit))
            if (t != &lane && laneGroupOf (*t) == groupHead)
                t->setMute (true);

        groupHead->setMute (false);
        lane.setMute (false);
    }

    juce::String parseGrid (const juce::var& grid, double& beats)
    {
        const auto fail = juce::String ("grid must be like 1/16, 1/8T, 1/8. or a number of beats");

        if (grid.isInt() || grid.isDouble() || grid.isInt64())
        {
            beats = (double) grid;

            // grid: 16 hiess "16 Beats je Rasterplatz" (Praxistest 1d: ein
            // Slot mit -680 ms) — gemeint war fast immer 1/16
            if (beats > 4.0 && beats <= 64.0 && std::abs (beats - std::round (beats)) < 1.0e-9)
                return "grid " + juce::String ((int) beats) + " would be " + juce::String ((int) beats)
                       + juce::String::fromUTF8 (" beats per slot — did you mean \"1/") + juce::String ((int) beats) + "\"?";

            return beats > 0.0 && beats <= 4.0 ? juce::String() : fail;
        }

        auto s = grid.toString().trim();

        if (! s.startsWith ("1/"))
            return fail;

        bool triplet = s.endsWithIgnoreCase ("T"), dotted = s.endsWith (".");
        auto den = s.substring (2, s.length() - (triplet || dotted ? 1 : 0)).getIntValue();

        if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32 && den != 64)
            return fail;

        beats = 4.0 / den * (triplet ? 2.0 / 3.0 : 1.0) * (dotted ? 1.5 : 1.0);
        return {};
    }

    te::AudioTrack* activeLaneOf (te::Edit& edit, te::AudioTrack& group)
    {
        for (auto* t : userTracks (edit))
            if (laneGroupOf (*t) == &group && ! t->isMuted (false))
                return t;

        return nullptr;
    }

    bool rangeIsFree (te::AudioTrack& track, double start, double end)
    {
        for (auto* c : track.getClips())
        {
            auto pos = c->getPosition();

            if (pos.getStart().inSeconds() < end - 1.0e-6 && pos.getEnd().inSeconds() > start + 1.0e-6)
                return false;
        }

        return true;
    }

    juce::StringArray clipsInRange (te::AudioTrack& track, double start, double end)
    {
        juce::StringArray out;

        for (auto* c : track.getClips())
        {
            auto pos = c->getPosition();

            if (pos.getStart().inSeconds() < end - 1.0e-6 && pos.getEnd().inSeconds() > start + 1.0e-6)
                out.add ("'" + c->getName() + "' (" + juce::String (pos.getStart().inSeconds(), 1) + "-"
                         + juce::String (pos.getEnd().inSeconds(), 1) + " s)");
        }

        return out;
    }

    juce::StringArray playingLanesWithClips (te::Edit& edit, te::AudioTrack& group)
    {
        juce::StringArray out;

        for (auto* t : userTracks (edit))
            if (laneGroupOf (*t) == &group && ! t->isMuted (false) && ! t->getClips().isEmpty())
                out.add (t->getName());

        return out;
    }

    te::InputDeviceInstance* armInputInstance (DawdContext& ctx, bool midi)
    {
        auto& dm = ctx.engine->getDeviceManager();

        if (midi && dm.getNumMidiInDevices() == 0)
        {
            // Das ALSA-seq-Device taucht in der Enumeration erst nach
            // Sekunden auf (PipeWire) — Rescan mit Wiederholung, statt den
            // ersten Record ins Leere laufen zu lassen.
            for (int i = 0; i < 20 && dm.getNumMidiInDevices() == 0; ++i)
            {
                dm.rescanMidiDeviceList();
                juce::Thread::sleep (100);
                juce::Timer::callPendingTimersSynchronously();
            }
        }

        if (midi)
            for (auto mi : dm.getMidiInDevices())
                mi->setEnabled (true);
        else
            for (auto* wi : dm.getWaveInputDevices())
                wi->setEnabled (true);

        dm.dispatchPendingUpdates();

        ctx.edit->getTransport().ensureContextAllocated();
        auto context = ctx.edit->getTransport().getCurrentPlaybackContext();
        te::InputDeviceInstance* instance = nullptr;

        if (midi)
        {
            if (auto* dev = dm.getDefaultMidiInDevice())
                instance = context->getInputFor (dev);

            if (instance == nullptr)
                for (auto in : context->getAllInputs())
                    if (in->getInputDevice().isMidi())
                        { instance = in; break; }
        }
        else
        {
            if (auto* dev = dm.getDefaultWaveInDevice())
                instance = context->getInputFor (dev);

            if (instance == nullptr)
                for (auto in : context->getAllInputs())
                    if (! in->getInputDevice().isMidi())
                        { instance = in; break; }
        }

        return instance;
    }

    te::InputDeviceInstance* inputInstanceFor (DawdContext& ctx, te::InputDevice* device)
    {
        if (device == nullptr)
            return nullptr;

        ctx.edit->getTransport().ensureContextAllocated();
        return ctx.edit->getTransport().getCurrentPlaybackContext()->getInputFor (device);
    }

    namespace
    {
        std::vector<te::InputDevice*> allInputs (DawdContext& ctx)
        {
            auto& dm = ctx.engine->getDeviceManager();
            std::vector<te::InputDevice*> all;

            for (auto* wi : dm.getWaveInputDevices())
                all.push_back (wi);

            for (auto mi : dm.getMidiInDevices())
                all.push_back (mi.get());

            return all;
        }
    }

    juce::String inputDisplayName (DawdContext& ctx, te::InputDevice& device)
    {
        int same = 0;

        for (auto* d : allInputs (ctx))
        {
            if (d->getName() != device.getName())
                continue;

            ++same;

            if (d == &device)
                return same == 1 ? device.getName() : device.getName() + " #" + juce::String (same);
        }

        return device.getName();
    }

    te::InputDevice* findInputByName (DawdContext& ctx, const juce::String& name)
    {
        for (auto* d : allInputs (ctx))
            if (inputDisplayName (ctx, *d) == name)
                return d;

        return nullptr;
    }

    juce::Array<juce::var> enumerateInputs (DawdContext& ctx)
    {
        auto& dm = ctx.engine->getDeviceManager();
        auto tracks = userTracks (*ctx.edit);
        // Nur lesen: den Audio-Graphen dafuer NICHT anlegen. ensureContextAllocated
        // baute bei jedem state nach dem Laden bzw. nach dem Leerlauf-Freigeben
        // (releaseIdleGraph) den ganzen Graphen samt prepareToPlay aller Plugins
        // (Agent-Projekt mit Surge/Samplern: 1,1-1,4 s auf dem Message-Thread).
        // Ohne Graph stehen die Zuweisungen im Edit (INPUTDEVICES), dieselbe
        // Quelle, aus der die InputDeviceInstance ihre Ziele liest.
        auto context = ctx.edit->getTransport().getCurrentPlaybackContext();
        auto inputDevices = ctx.edit->state.getChildWithName (te::IDs::INPUTDEVICES);

        auto toIndices = [&] (const juce::Array<te::EditItemID>& targets)
        {
            juce::Array<juce::var> idx;

            for (auto id : targets)
                for (int i = 0; i < tracks.size(); ++i)
                    if (tracks[i]->itemID == id)
                        { idx.add (i); break; }

            return idx;
        };

        auto trackIndicesFor = [&] (te::InputDevice& device)
        {
            if (context != nullptr)
            {
                auto* in = context->getInputFor (&device);
                return in != nullptr ? toIndices (in->getTargets()) : juce::Array<juce::var>();
            }

            // wie InputDeviceInstance::getTargets: abgeschaltete Geraete haben
            // keine Ziele; Zustand per deviceID, sonst ueber den Namen (alte Edits)
            juce::Array<te::EditItemID> targets;

            if (! device.isEnabled())
                return juce::Array<juce::var>();

            juce::ValueTree st;

            for (const auto& v : inputDevices)
                if (v.hasType (te::IDs::INPUTDEVICE) && v[te::IDs::deviceID] == device.getDeviceID())
                    { st = v; break; }

            if (! st.isValid())
                for (const auto& v : inputDevices)
                    if (v.hasType (te::IDs::INPUTDEVICE) && v[te::IDs::name] == device.getName())
                        { st = v; break; }

            for (const auto& d : st)
                if (d.hasType (te::IDs::INPUTDEVICEDESTINATION))
                    if (auto tid = te::EditItemID::fromProperty (d, te::IDs::targetID); tid.isValid())
                        targets.add (tid);

            return toIndices (targets);
        };

        juce::Array<juce::var> inputs;

        for (auto* wi : dm.getWaveInputDevices())
            inputs.add (resultObj (true, {
                { "name", inputDisplayName (ctx, *wi) }, { "type", "audio" },
                { "enabled", wi->isEnabled() },
                { "tracks", trackIndicesFor (*wi) } }));

        for (auto mi : dm.getMidiInDevices())
            inputs.add (resultObj (true, {
                { "name", inputDisplayName (ctx, *mi) }, { "type", "midi" },
                { "enabled", mi->isEnabled() },
                { "tracks", trackIndicesFor (*mi) } }));

        return inputs;
    }

    // Komprimierte Formate (ogg, flac, mp3) einmal nach WAV dekodieren —
    // ein Render mit einem solchen Clip im Edit kommt nicht zurueck. Die
    // WAV liegt in media/ neben dem Edit ("<name>.<ext>.wav", nach einer
    // Aenderung der Quelle neu); unkomprimierte Dateien bleiben, wie sie sind.
    juce::File playableAudioFile (DawdContext& ctx, const juce::File& file, juce::String& err)
    {
        juce::AudioFormat* format = nullptr;
        std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFindingFormat (*ctx.engine, file, format));

        if (reader == nullptr || format == nullptr || ! format->isCompressed())
            return file;

        auto editFile = ctx.edit->editFileRetriever ? ctx.edit->editFileRetriever() : juce::File();
        auto dir = editFile != juce::File() ? editFile.getSiblingFile ("media")
                                            : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dawd-media");
        auto wav = dir.getChildFile (file.getFileName() + ".wav");

        if (wav.existsAsFile() && wav.getLastModificationTime() >= file.getLastModificationTime())
        {
            recordOrigins (wav, { file.getFullPathName() });   // auch fuer aeltere Projekte
            return wav;
        }

        if (! dir.createDirectory().wasOk())
        {
            err = "cannot create " + dir.getFullPathName();
            return {};
        }

        juce::TemporaryFile tmp (wav);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            juce::WavAudioFormat().createWriterFor (new juce::FileOutputStream (tmp.getFile()), reader->sampleRate,
                                                    reader->numChannels, 24, {}, 0));

        if (writer == nullptr || ! writer->writeFromAudioReader (*reader, 0, -1))
        {
            err = "cannot decode " + file.getFileName();
            return {};
        }

        writer.reset();

        if (! tmp.overwriteTargetFileWithTemporary())
        {
            err = "cannot write " + wav.getFullPathName();
            return {};
        }

        recordOrigins (wav, { file.getFullPathName() });
        return wav;
    }

    juce::String insertAudioClip (DawdContext& ctx, int trackIndex, const juce::File& file, double start,
                                  double* lengthOut)
    {
        juce::String decodeError;
        auto source = playableAudioFile (ctx, file, decodeError);

        if (decodeError.isNotEmpty())
            return decodeError;

        te::AudioFile audioFile (*ctx.engine, source);

        if (! audioFile.isValid())
            return "invalid audio file";

        auto tracks = userTracks (*ctx.edit);

        // Naechste Spur (index == count) wird erzeugt — das Edit startet ohne Spuren
        if (trackIndex < 0 || trackIndex > tracks.size())
            return "no such track";

        auto* track = trackIndex == tracks.size()
                          ? getOrInsertAudioTrackAt (*ctx.edit, trackIndex)
                          : tracks[trackIndex];

        double length = audioFile.getLength();

        if (lengthOut != nullptr)
            *lengthOut = length;

        track->insertWaveClip (file.getFileNameWithoutExtension(), source,
                               { { te::TimePosition::fromSeconds (start),
                                   te::TimePosition::fromSeconds (start + length) }, {} }, false);

        // Der AudioFileCache scannt asynchron — bis dahin meldet AudioFile::isValid
        // Rate 0 und der Live-Graph baut den Clip-Node nicht ein (erste Wiedergabe
        // still). Info synchron befuellen, damit der naechste Graph den Clip enthaelt.
        ctx.engine->getAudioFileManager().validateFile (audioFile, true);

        return {};
    }

    bool waitForClipRenders (te::Edit& edit, int timeoutMs)
    {
        auto busy = [&edit]
        {
            for (auto* t : te::getAudioTracks (edit))
                for (auto* c : t->getClips())
                    if (auto* ac = dynamic_cast<te::AudioClipBase*> (c))
                        // nur rueckwaerts: gestreckte Clips (Rubberband) melden
                        // headless dauerhaft "Proxy noetig", werden aber im
                        // Render in Echtzeit gestreckt und klingen richtig
                        if (ac->isGeneratingPlaybackFile() && ac->getIsReversed())
                            return true;

            return false;
        };

        auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (busy())
        {
            if (juce::Time::getMillisecondCounter() > end)
                return false;

            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }

        return true;
    }

    bool isClipFileMissing (const te::AudioClipBase& ac)
    {
        return ! ac.getOriginalFile().existsAsFile();
    }

    juce::Array<MissingFile> missingMedia (te::Edit& edit, const te::Renderer::Parameters* scope)
    {
        juce::Array<MissingFile> out;
        const auto users = userTracks (edit);
        const auto all = te::getAllTracks (edit);

        for (int ti = 0; ti < all.size(); ++ti)
        {
            auto* at = dynamic_cast<te::AudioTrack*> (all[ti]);

            if (at == nullptr || (scope != nullptr && ! scope->tracksToDo.isZero() && ! scope->tracksToDo[ti]))
                continue;

            auto* group = dynamic_cast<te::AudioTrack*> (at->getOutput().getDestinationTrack());
            const bool freeze = ! users.contains (at) && group != nullptr && freezeTrackOf (edit, *group) == at;

            if (! users.contains (at) && ! freeze)
                continue;

            for (auto* c : at->getClips())
            {
                auto* ac = dynamic_cast<te::AudioClipBase*> (c);

                if (ac == nullptr || ac->disabled || ! isClipFileMissing (*ac))
                    continue;

                if (scope != nullptr && (! scope->allowedClips.isEmpty() && ! scope->allowedClips.contains (c)))
                    continue;

                if (scope != nullptr && ! scope->time.overlaps (c->getEditTimeRange()))
                    continue;

                MissingFile m;
                m.kind = freeze ? "freeze" : "clip";
                m.name = c->getName();
                m.file = ac->getOriginalFile();
                auto* owner = freeze ? group : at;
                m.track = users.indexOf (owner);
                m.trackName = owner->getName();
                out.add (m);
            }
        }

        if (scope == nullptr)
            for (auto* p : te::getAllPlugins (edit, false))
                if (auto* sp = dynamic_cast<te::SamplerPlugin*> (p))
                    for (int i = 0; i < sp->getNumSounds(); ++i)
                        if (auto f = te::SourceFileReference::findFileFromString (edit, sp->getSoundMedia (i)); ! f.existsAsFile())
                        {
                            MissingFile m;
                            m.kind = "sample";
                            m.name = sp->getSoundName (i);
                            m.file = f;

                            if (auto* owner = sp->getOwnerTrack())
                                if (auto id = owner->state.getProperty ("dawdModule").toString(); id.isNotEmpty())
                                    m.module = "p" + id;

                            out.add (m);
                        }

        return out;
    }

    juce::var missingMediaList (const juce::Array<MissingFile>& list)
    {
        juce::Array<juce::var> out;

        for (auto& m : list)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("kind", m.kind);

            if (m.kind == "sample")
                o->setProperty ("module", m.module);
            else
            {
                o->setProperty ("track", m.track);
                o->setProperty ("trackName", m.trackName);
            }

            o->setProperty ("name", m.name);
            o->setProperty ("file", m.file.getFullPathName());
            out.add (juce::var (o));
        }

        return out;
    }

    juce::String missingMediaError (const juce::Array<MissingFile>& list)
    {
        if (list.isEmpty())
            return {};

        auto& m = list.getReference (0);
        const auto where = "track " + juce::String (m.track) + " (" + m.trackName + ")";
        juce::String text;

        if (m.kind == "freeze")
            text = "freeze audio missing for " + where + ": " + m.file.getFullPathName()
                 + " - unfreeze the track (freezeTrack on: false) and freeze it again";
        else if (m.kind == "sample")
            text = "sample file missing in " + m.module + " (" + m.name + "): " + m.file.getFullPathName()
                 + " - put the file back or replace the sample (setSamples)";
        else
            text = "audio file missing for clip \"" + m.name + "\" on " + where + ": " + m.file.getFullPathName()
                 + " - put the file back or delete the clip (deleteClip)";

        if (list.size() > 1)
            text << " (" << (list.size() - 1) << " more missing - see state.missingFiles)";

        return text;
    }

    double beatsToSeconds (te::Edit& edit, double fromSeconds, double beats)
    {
        // Der Hin- und Rueckweg ueber die Tempospur rundet: Beat 0 ab dem
        // Start einer spaeteren Section ergab -7e-15 s und damit "start must
        // be >= 0" (Praxistest 3, addMidiClip {section: "Refrain",
        // startBeat: 0}) — nicht-negative Beats bleiben nicht-negativ
        if (beats == 0.0)
            return 0.0;

        auto& ts = edit.tempoSequence;
        auto b0 = ts.toBeats (te::TimePosition::fromSeconds (fromSeconds));
        auto s = ts.toTime (b0 + te::BeatDuration::fromBeats (beats)).inSeconds() - fromSeconds;
        return beats > 0.0 ? juce::jmax (0.0, s) : s;
    }

    double secondsToBeats (te::Edit& edit, double fromSeconds, double seconds)
    {
        auto& ts = edit.tempoSequence;
        return (ts.toBeats (te::TimePosition::fromSeconds (fromSeconds + seconds))
                - ts.toBeats (te::TimePosition::fromSeconds (fromSeconds))).inBeats();
    }

    juce::String parseNotes (te::Edit& edit, const juce::var& notes, double clipStart,
                             juce::MidiMessageSequence& seq, double& notesEnd, bool* inBeatsOut)
    {
        notesEnd = 0.0;
        int inSeconds = 0, inBeats = 0;
        const juce::StringArray known { "pitch", "velocity", "start", "length", "beat", "beats", "startBeat", "lengthBeats" };
        const auto fields = juce::String::fromUTF8 ("notes take pitch, velocity? and either start/length (seconds) or beat/beats (beats, also startBeat/lengthBeats)");

        int noteNo = 0;
        struct Note { int pitch, velocity; double start, end; };
        std::vector<Note> parsed;

        for (auto& nv : *notes.getArray())
        {
            ++noteNo;

            // Unbekannte Felder nicht still uebergehen — sonst wird aus einem
            // Tippfehler eine Note an falscher Stelle oder "invalid note"
            if (auto* o = nv.getDynamicObject())
                for (auto& prop : o->getProperties())
                    if (! known.contains (prop.name.toString()))
                        return "unknown note field '" + prop.name.toString() + juce::String::fromUTF8 ("' — ") + fields;

            const bool beats = nv.hasProperty ("beat") || nv.hasProperty ("beats")
                            || nv.hasProperty ("startBeat") || nv.hasProperty ("lengthBeats");
            ++(beats ? inBeats : inSeconds);

            if (inBeats > 0 && inSeconds > 0)
                return juce::String::fromUTF8 ("notes mix seconds (start/length) and beats (beat/beats) — use one per call");

            auto pitch = (int) (double) nv.getProperty ("pitch", -1.0);
            auto velocity = (int) (double) nv.getProperty ("velocity", 100.0);
            double noteStart, noteLength;

            if (beats)
            {
                auto b = (double) nv.getProperty ("beat", nv.getProperty ("startBeat", -1.0));
                auto len = (double) nv.getProperty ("beats", nv.getProperty ("lengthBeats", 0.0));

                if (b < 0.0 || len <= 0.0)
                    return "invalid note " + juce::String (noteNo) + ": beat must be >= 0 and beats > 0";

                // Beat 0 geht hin und zurueck durch die Tempo-Spur; liegt der
                // Clip auf einem krummen Beat, kommt ein winzig negativer Wert
                // heraus (b >= 0 ist oben geprueft)
                noteStart = juce::jmax (0.0, beatsToSeconds (edit, clipStart, b));
                noteLength = beatsToSeconds (edit, clipStart, b + len) - noteStart;

                if (pitch < 0 || pitch > 127 || noteLength <= 0.0 || velocity < 1 || velocity > 127)
                    return "invalid note " + juce::String (noteNo) + ": pitch 0..127, velocity 1..127, beat >= 0, beats > 0";
            }
            else
            {
                noteStart = (double) nv.getProperty ("start", -1.0);
                noteLength = (double) nv.getProperty ("length", 0.0);
            }

            if (pitch < 0 || pitch > 127 || noteStart < 0.0 || noteLength <= 0.0 || velocity < 1 || velocity > 127)
                return "invalid note " + juce::String (noteNo) + ": pitch 0..127, velocity 1..127, start >= 0, length > 0";

            parsed.push_back ({ pitch, velocity, noteStart, noteStart + noteLength });
            notesEnd = juce::jmax (notesEnd, noteStart + noteLength);
        }

        // Gleiche Tonhoehe ueberlappend (Praxistest 1f P11: Noten um 0,05
        // Beat verlaengert, damit sie aneinander anschliessen): die fruehere
        // endet beim Einsatz der spaeteren, wie die Engine es beim Abspielen
        // tut. Sonst paarte updateMatchedPairs der Reihe nach — die spaetere
        // Note bekam das Note-Off der frueheren und war nach 0,05 s aus, in
        // jedem Instrument (4OSC, Surge, Dexed). Gleicher Einsatz: die laengere bleibt
        std::sort (parsed.begin(), parsed.end(), [] (const Note& a, const Note& b)
        {
            return a.pitch != b.pitch ? a.pitch < b.pitch : a.start != b.start ? a.start < b.start : a.end > b.end;
        });

        std::vector<Note> kept;

        for (auto& n : parsed)
        {
            if (! kept.empty() && kept.back().pitch == n.pitch)
            {
                if (n.start <= kept.back().start)
                    continue;

                kept.back().end = juce::jmin (kept.back().end, n.start);
            }

            kept.push_back (n);
        }

        // Note-Offs zuerst: bei gleicher Zeit liegt dann das Off der frueheren
        // Note vor dem On der naechsten (addEvent setzt hinter Gleichzeitige)
        for (auto& n : kept)
            seq.addEvent (juce::MidiMessage::noteOff (1, n.pitch), n.end);

        for (auto& n : kept)
            seq.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) n.velocity), n.start);

        seq.updateMatchedPairs();

        if (inBeatsOut != nullptr)
            *inBeatsOut = inBeats > 0;

        return {};
    }

    juce::Array<juce::var> clipControllers (te::Edit& edit, te::MidiClip& clip)
    {
        juce::Array<juce::var> out;
        const auto pos = clip.getPosition();
        const auto clipStart = pos.getStart().inSeconds();
        std::vector<std::pair<double, te::MidiControllerEvent*>> events;

        // Wie bei den Noten nur, was im Clip liegt (nach trimClip/splitClip
        // verdeckte Events nicht melden)
        for (auto* e : clip.getSequence().getControllerEvents())
            if (auto s = e->getEditTime (clip).inSeconds() - clipStart; s >= -0.0005 && s < pos.getLength().inSeconds() - 0.0005)
                events.push_back ({ s, e });

        std::stable_sort (events.begin(), events.end(), [] (auto& a, auto& b) { return a.first < b.first; });

        for (auto& [s, e] : events)
        {
            const auto type = e->getType();
            juce::var controller = type;
            auto value = e->getControllerValue() >> 7;   // die Engine haelt 14 Bit

            if (type == te::MidiControllerEvent::pitchWheelType)
            {
                controller = "pitchBend";
                value = e->getControllerValue() - 8192;
            }
            else if (type == te::MidiControllerEvent::programChangeType)
                controller = "program";
            else if (type == te::MidiControllerEvent::channelPressureType)
                controller = "channelPressure";
            else if (type == te::MidiControllerEvent::aftertouchType)
                controller = "aftertouch";
            else if (type < 0 || type > 127)
                continue;

            auto o = new juce::DynamicObject();
            o->setProperty ("controller", controller);
            o->setProperty ("beat", std::round (secondsToBeats (edit, clipStart, juce::jmax (0.0, s)) * 1000.0) / 1000.0);
            o->setProperty ("start", std::round (juce::jmax (0.0, s) * 1000.0) / 1000.0);
            o->setProperty ("value", value);

            if (type == te::MidiControllerEvent::aftertouchType)
                o->setProperty ("pitch", e->getMetadata());

            out.add (juce::var (o));
        }

        return out;
    }

    void mergeNotesRelative (te::MidiClip& clip, juce::MidiMessageSequence& seq)
    {
        seq.addTimeToMessages (clip.getPosition().getStartOfSource().inSeconds());
        clip.mergeInMidiSequence (seq, te::MidiList::NoteAutomationType::none);
    }

    juce::String insertMidiSequence (DawdContext& ctx, int trackIndex, const juce::String& name,
                                     juce::MidiMessageSequence& seq, double start, double length,
                                     double* lengthOut, int* notesOut)
    {
        if (length <= 0.0)
            length = seq.getEndTime();

        auto* track = getOrInsertAudioTrackAt (*ctx.edit, trackIndex);

        if (! isMidiTrack (*track))
            setTrackType (*ctx.edit, *track, true);

        auto* clip = track->insertNewClip (te::TrackItem::Type::midi, name,
                                           { te::TimePosition::fromSeconds (start),
                                             te::TimePosition::fromSeconds (start + length) }, nullptr);

        if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
        {
            mergeNotesRelative (*mc, seq);

            if (notesOut != nullptr)
                *notesOut = mc->getSequence().getNumNotes();
        }

        if (lengthOut != nullptr)
            *lengthOut = length;

        return {};
    }

    juce::String insertMidiClip (DawdContext& ctx, int trackIndex, const juce::File& file, double start,
                                 double* lengthOut)
    {
        juce::MidiFile mf;
        juce::FileInputStream in (file);

        if (! in.openedOk() || ! mf.readFrom (in))
            return "invalid midi file";

        mf.convertTimestampTicksToSeconds();

        juce::MidiMessageSequence seq;

        for (int i = 0; i < mf.getNumTracks(); ++i)
            if (auto* t = mf.getTrack (i))
                for (int j = 0; j < t->getNumEvents(); ++j)
                    seq.addEvent (t->getEventPointer (j)->message);

        seq.updateMatchedPairs();
        double length = seq.getEndTime();

        if (! (length > 0.0))
            return "no notes in midi file";

        return insertMidiSequence (ctx, trackIndex, file.getFileNameWithoutExtension(),
                                   seq, start, length, lengthOut);
    }

    te::Clip* findClipByName (te::AudioTrack& track, const juce::String& name)
    {
        for (auto* clip : track.getClips())
            if (clip->getName() == name)
                return clip;

        return nullptr;
    }

    juce::String uniqueClipName (te::AudioTrack& track, const juce::String& base)
    {
        auto stem = base;
        auto last = base.fromLastOccurrenceOf (" ", false, false);

        if (base.containsChar (' ') && last.containsOnly ("0123456789") && last.isNotEmpty())
            stem = base.upToLastOccurrenceOf (" ", false, false);

        if (findClipByName (track, stem) == nullptr)
            return stem;

        for (int n = 2;; ++n)
            if (findClipByName (track, stem + " " + juce::String (n)) == nullptr)
                return stem + " " + juce::String (n);
    }

    te::MarkerClip* findSection (te::Edit& edit, const juce::String& name)
    {
        if (auto* mt = edit.getMarkerTrack())
            for (auto* clip : mt->getClips())
                if (auto* mc = dynamic_cast<te::MarkerClip*> (clip))
                    if (mc->getName() == name)
                        return mc;

        return nullptr;
    }

    std::vector<te::MarkerClip*> sortedSections (te::Edit& edit)
    {
        std::vector<te::MarkerClip*> sections;

        if (auto* mt = edit.getMarkerTrack())
            for (auto* clip : mt->getClips())
                if (auto* mc = dynamic_cast<te::MarkerClip*> (clip))
                    sections.push_back (mc);

        std::sort (sections.begin(), sections.end(), [] (auto* a, auto* b)
                   { return a->getPosition().getStart() < b->getPosition().getStart(); });

        return sections;
    }

    namespace
    {
        const juce::Identifier bpmProp ("dawdBpm");
        const juce::Identifier timeSigProp ("dawdTimeSig");
        const juce::Identifier rampProp ("dawdRamp");

        // Sekunden einer linearen Rampe ueber beats Schlaege von bpm1 nach
        // bpm2 — so, wie die Engine sie abspielt (tempo::Sequence: 4 Stufen
        // je Beat, 1..100, jede mit dem Tempo an ihrem Anfang)
        double rampSeconds (double beats, double bpm1, double bpm2)
        {
            const int n = (int) juce::jlimit (1.0, 100.0, 4.0 * beats);
            double t = 0.0;

            for (int k = 0; k < n; ++k)
                t += beats / n * 60.0 / (bpm1 + (bpm2 - bpm1) * k / n);

            return t;
        }

        bool parseTimeSig (const juce::String& s, int& num, int& den)
        {
            if (! s.containsChar ('/'))
                return false;

            auto n = s.upToFirstOccurrenceOf ("/", false, false).trim();
            auto d = s.fromFirstOccurrenceOf ("/", false, false).trim();

            if (n.isEmpty() || d.isEmpty() || ! n.containsOnly ("0123456789") || ! d.containsOnly ("0123456789"))
                return false;

            num = n.getIntValue();
            den = d.getIntValue();
            return num >= 1 && num <= 32 && (den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32);
        }
    }

    std::vector<SectionTempo> sectionTempos (te::Edit& edit)
    {
        std::vector<SectionTempo> out;
        SectionTempo cur;

        for (auto* sec : sortedSections (edit))
        {
            SectionTempo t = cur;
            t.bpmSet = t.sigSet = false;
            auto bpm = (double) sec->state.getProperty (bpmProp, 0.0);

            if (bpm > 0.0)
            {
                t.bpm = bpm;
                t.bpmSet = true;
            }

            int num = 4, den = 4;

            if (parseTimeSig (sec->state.getProperty (timeSigProp).toString(), num, den))
            {
                t.num = num;
                t.den = den;
                t.sigSet = true;
            }

            t.ramp = (bool) sec->state.getProperty (rampProp, false);
            out.push_back (t);
            cur = t;
        }

        for (size_t i = 0; i < out.size(); ++i)
            out[i].bpmEnd = out[i].ramp && i + 1 < out.size() ? out[i + 1].bpm : out[i].bpm;

        return out;
    }

    SectionTempo tempoBefore (te::Edit& edit, double t)
    {
        auto secs = sortedSections (edit);
        auto tempos = sectionTempos (edit);
        SectionTempo result;

        for (size_t i = 0; i < secs.size(); ++i)
            if (secs[i]->getPosition().getEnd().inSeconds() <= t + 1e-6)
                result = tempos[i];

        result.bpmSet = result.sigSet = result.ramp = false;
        result.bpmEnd = result.bpm;
        return result;
    }

    juce::String setSectionRamp (te::MarkerClip& section, const juce::var& ramp)
    {
        if (ramp.isVoid())
            return {};

        if (! ramp.isBool())
            return "ramp must be true or false";

        if ((bool) ramp)
            section.state.setProperty (rampProp, true, nullptr);
        else
            section.state.removeProperty (rampProp, nullptr);

        return {};
    }

    double sectionBars (te::Edit& edit, te::MarkerClip& section, const SectionTempo& tempo)
    {
        auto pos = section.getPosition();

        if (! tempo.ramps())
            return pos.getLength().inSeconds() / tempo.barSeconds();

        auto& ts = edit.tempoSequence;
        return (ts.toBeats (pos.getEnd()) - ts.toBeats (pos.getStart())).inBeats() / tempo.beatsPerBar();
    }

    double secondsForBars (const SectionTempo& tempo, double bars)
    {
        return tempo.ramps() ? rampSeconds (bars * tempo.beatsPerBar(), tempo.bpm, tempo.bpmEnd)
                             : bars * tempo.barSeconds();
    }

    juce::String validateSectionTempo (const juce::var& bpm, const juce::var& timeSig)
    {
        if (! bpm.isVoid() && (double) bpm != 0.0 && ((double) bpm < 20.0 || (double) bpm > 300.0))
            return "bpm must be 20..300 (0 = inherit)";

        int num = 4, den = 4;

        if (! timeSig.isVoid() && timeSig.toString().isNotEmpty() && ! parseTimeSig (timeSig.toString(), num, den))
            return "timeSig must be n/d (n 1..32, d 1, 2, 4, 8, 16 or 32; empty = inherit)";

        return {};
    }

    juce::String setSectionTempo (te::MarkerClip& section, const juce::var& bpm, const juce::var& timeSig)
    {
        if (auto err = validateSectionTempo (bpm, timeSig); err.isNotEmpty())
            return err;

        if (! bpm.isVoid())
        {
            if ((double) bpm == 0.0)
                section.state.removeProperty (bpmProp, nullptr);
            else
                section.state.setProperty (bpmProp, (double) bpm, nullptr);
        }

        if (! timeSig.isVoid())
        {
            if (timeSig.toString().isEmpty())
                section.state.removeProperty (timeSigProp, nullptr);
            else
                section.state.setProperty (timeSigProp, timeSig.toString().removeCharacters (" "), nullptr);
        }

        return {};
    }

    void applySectionTempos (te::Edit& edit)
    {
        auto& ts = edit.tempoSequence;

        // Ab dem zweiten Eintrag alles weg — ohne Remap: Clips liegen in
        // Sekunden und bleiben, wo sie sind
        for (int i = ts.getNumTempos(); --i > 0;)
            ts.removeTempo (i, false);

        for (int i = ts.getNumTimeSigs(); --i > 0;)
            ts.removeTimeSig (i);

        auto secs = sortedSections (edit);
        auto tempos = sectionTempos (edit);
        SectionTempo first = tempos.empty() ? SectionTempo() : tempos.front();

        // Kurve des Punkts am Section-Start: 0 = linear zum naechsten Punkt
        // (Rampe), 1 = Sprung
        auto curveOf = [&] (size_t i) { return i < tempos.size() && tempos[i].ramps() ? 0.0f : 1.0f; };

        // set mit remap = false: setBpm rechnet alle Positionen im Edit auf die
        // Beat-Lage um (Sections, Clips) — dawd haelt sie in Sekunden
        if (auto* t0 = ts.getTempo (0))
            t0->set (t0->getStartBeat(), first.bpm, curveOf (0), false);

        if (auto* s0 = ts.getTimeSig (0))
        {
            s0->numerator = first.num;
            s0->denominator = first.den;
        }

        // Endpunkt einer Rampe so legen, dass er auf die Sekunde des
        // Section-Starts faellt: der Beat haengt von der Rampe ab, die vom
        // Beat abhaengt — nachfuehren, bis die Engine die Zeit trifft (die
        // Stufenzahl der Engine springt mit dem Beat: bester Wert zaehlt)
        auto settle = [&] (te::TempoSetting& t, double target, double bpm, float curve)
        {
            auto best = t.getStartBeat();
            double bestErr = HUGE_VAL;

            for (int k = 0; k < 40; ++k)
            {
                const double err = target - ts.toTime (t.getStartBeat()).inSeconds();

                if (std::abs (err) < std::abs (bestErr))
                    best = t.getStartBeat(), bestErr = err;

                if (std::abs (err) < 1.0e-7)
                    break;

                t.set (t.getStartBeat() + te::BeatDuration::fromBeats (err * bpm / 60.0), bpm, curve, false);
            }

            t.set (best, bpm, curve, false);
        };

        // Der Reihe nach: die Abbildung Zeit -> Beat bis zum Section-Start
        // steht dann schon (spaetere Wechsel aendern sie nicht)
        for (size_t i = 1; i < secs.size(); ++i)
        {
            auto& prev = tempos[i - 1];
            auto& cur = tempos[i];
            auto start = secs[i]->getPosition().getStart();
            te::TempoSetting::Ptr point;

            if (cur.bpm != prev.bpm || curveOf (i) == 0.0f)
            {
                auto beat = ts.toBeats (start);

                // Nach einer Rampe: Startwert aus der Formel der linearen
                // Rampe (Sekunden -> Beats), settle macht ihn genau
                if (prev.ramps())
                {
                    const double secs0 = secs[i - 1]->getPosition().getStart().inSeconds();
                    const double len = start.inSeconds() - secs0;
                    beat = ts.toBeats (secs[i - 1]->getPosition().getStart())
                         + te::BeatDuration::fromBeats (len * (cur.bpm - prev.bpm) / (60.0 * std::log (cur.bpm / prev.bpm)));
                }

                point = ts.insertTempo (beat, cur.bpm, curveOf (i));

                if (point != nullptr && prev.ramps())
                    settle (*point, start.inSeconds(), cur.bpm, curveOf (i));
            }

            if (cur.num != prev.num || cur.den != prev.den)
                if (auto sig = ts.insertTimeSig (start))
                {
                    sig->numerator = cur.num;
                    sig->denominator = cur.den;

                    // Ein Taktwechsel knapp vor dem Endpunkt verschiebt die
                    // Rampe der Engine ein wenig: nachfuehren
                    if (point != nullptr && prev.ramps())
                        settle (*point, start.inSeconds(), cur.bpm, curveOf (i));
                }
        }
    }

    std::vector<OffsetBeats> followTempoOffsets (te::Edit& edit)
    {
        std::vector<OffsetBeats> out;

        for (auto* track : userTracks (edit))
            for (auto* clip : track->getClips())
                if (auto* ac = dynamic_cast<te::AudioClipBase*> (clip); ac != nullptr && ac->getAutoTempo())
                    out.push_back ({ ac->itemID, ac->getPosition().getOffset().inSeconds(), ac->getOffsetInBeats().inBeats() });

        return out;
    }

    void keepOffsetBeats (te::Edit& edit, const std::vector<OffsetBeats>& before)
    {
        for (auto& o : before)
        {
            auto* ac = dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, o.id));

            if (ac == nullptr || ! ac->getAutoTempo())
                continue;

            auto pos = ac->getPosition();

            if (std::abs (pos.getOffset().inSeconds() - o.seconds) > 1.0e-9)
                continue;

            const double secs = o.beats / edit.tempoSequence.getBeatsPerSecondAt (pos.getStart());

            if (std::abs (secs - o.seconds) > 1.0e-9)
                ac->setPosition ({ pos.time, te::TimeDuration::fromSeconds (secs) });
        }
    }

    te::MarkerClip* insertSection (te::Edit& edit, const juce::String& name, double start, double end,
                                   juce::Colour colour)
    {
        edit.ensureMarkerTrack();
        auto* item = edit.getMarkerTrack()->insertNewClip (te::TrackItem::Type::marker, name,
                                                           { te::TimePosition::fromSeconds (start),
                                                             te::TimePosition::fromSeconds (end) }, nullptr);

        if (auto* mc = dynamic_cast<te::MarkerClip*> (item))
        {
            mc->setColour (colour);
            return mc;
        }

        return nullptr;
    }

    juce::String nextSectionName (te::Edit& edit)
    {
        int maxN = 0;

        for (auto* sec : sortedSections (edit))
            if (sec->getName().trim().startsWith ("Section "))
                maxN = juce::jmax (maxN,
                                   sec->getName().fromFirstOccurrenceOf ("Section ", false, false).getIntValue());

        return "Section " + juce::String (maxN + 1);
    }

    juce::Colour nextSectionColour (te::Edit& edit)
    {
        // Klar unterscheidbare Grundfarben (auf dunklem Grund lesbar)
        static const juce::Colour palette[] = {
            juce::Colour::fromString ("ff3fb950"),
            juce::Colour::fromString ("ff58a6ff"),
            juce::Colour::fromString ("ffbc8cff"),
            juce::Colour::fromString ("ffe3b341"),
            juce::Colour::fromString ("fff0883e"),
            juce::Colour::fromString ("fff85149"),
            juce::Colour::fromString ("ff39c5cf"),
            juce::Colour::fromString ("ffdb61a2"),
        };
        constexpr int num = (int) std::size (palette);
        auto sections = sortedSections (edit);

        for (auto& c : palette)
        {
            bool used = false;

            for (auto* sec : sections)
                if (sec->getColour() == c)
                    { used = true; break; }

            if (! used)
                return c;
        }

        return palette[sections.size() % num];
    }

    juce::var autoSection (te::Edit& edit)
    {
        auto secs = sortedSections (edit);

        if (secs.empty())
            return {};

        const double lastEnd = secs.back()->getPosition().getEnd().inSeconds();
        double materialEnd = 0.0;

        for (auto* track : userTracks (edit))
            for (auto* clip : track->getClips())
                materialEnd = juce::jmax (materialEnd, clip->getPosition().getEnd().inSeconds());

        if (materialEnd <= lastEnd + 0.001)
            return {};

        const auto tempo = tempoBefore (edit, lastEnd);
        const int bars = juce::jmax (1, (int) std::ceil ((materialEnd - lastEnd) / tempo.barSeconds() - 1.0e-6));
        const double end = lastEnd + bars * tempo.barSeconds();
        const auto name = nextSectionName (edit);

        auto* sec = insertSection (edit, name, lastEnd, end, nextSectionColour (edit));

        if (sec == nullptr)
            return {};

        sec->state.setProperty (autoSectionProp, true, nullptr);
        applySectionTempos (edit);

        auto o = new juce::DynamicObject();
        o->setProperty ("name", name);
        o->setProperty ("start", lastEnd);
        o->setProperty ("end", end);
        o->setProperty ("bars", bars);
        return juce::var (o);
    }

    const juce::Identifier autoSectionProp ("dawdAutoSection");

    juce::var retractAutoSection (te::Edit& edit, const juce::StringArray& names)
    {
        auto secs = sortedSections (edit);

        if (secs.size() < 2)
            return {};

        auto* last = secs.back();

        if (! (bool) last->state.getProperty (autoSectionProp, false) || ! names.contains (last->getName()))
            return {};

        const double start = last->getPosition().getStart().inSeconds();
        const double end = last->getPosition().getEnd().inSeconds();
        double materialEnd = 0.0;

        for (auto* track : userTracks (edit))
            for (auto* clip : track->getClips())
                materialEnd = juce::jmax (materialEnd, clip->getPosition().getEnd().inSeconds());

        auto o = new juce::DynamicObject();
        o->setProperty ("name", last->getName());

        if (materialEnd <= start + 0.001)
        {
            last->removeFromParent();
            applySectionTempos (edit);
            o->setProperty ("removed", true);
            return juce::var (o);
        }

        const auto tempo = tempoBefore (edit, start);
        const int bars = juce::jmax (1, (int) std::ceil ((materialEnd - start) / tempo.barSeconds() - 1.0e-6));
        const double newEnd = start + bars * tempo.barSeconds();

        if (newEnd >= end - 0.001)
            return {};

        last->setEnd (te::TimePosition::fromSeconds (newEnd), true);
        applySectionTempos (edit);
        o->setProperty ("end", newEnd);
        o->setProperty ("bars", bars);
        return juce::var (o);
    }

    namespace
    {
        const juce::Identifier markersType ("DAWDMARKERS"), markerType ("MARKER"), timeProp ("time");

        // Marker folgen den Section-Ops wie Clips (Empfehlung, Matthias
        // 2026-10-25); false = sie bleiben an ihrer Songzeit
        constexpr bool markersFollowTime = true;
        constexpr double edgeTolMarkers = 0.001;

        juce::ValueTree markerList (te::Edit& edit, bool create)
        {
            return create ? edit.state.getOrCreateChildWithName (markersType, nullptr)
                          : edit.state.getChildWithName (markersType);
        }
    }

    std::vector<Marker> sortedMarkers (te::Edit& edit)
    {
        std::vector<Marker> out;

        for (auto m : markerList (edit, false))
            if (m.hasType (markerType))
                out.push_back ({ m.getProperty (te::IDs::name).toString(), (double) m.getProperty (timeProp, 0.0) });

        std::stable_sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.time < b.time; });
        return out;
    }

    juce::ValueTree findMarker (te::Edit& edit, const juce::String& name)
    {
        for (auto m : markerList (edit, false))
            if (m.hasType (markerType) && m.getProperty (te::IDs::name).toString() == name)
                return m;

        return {};
    }

    void setMarker (juce::ValueTree& marker, const juce::String& name, double time)
    {
        marker.setProperty (te::IDs::name, name, nullptr);
        marker.setProperty (timeProp, std::round (juce::jmax (0.0, time) * 1.0e6) / 1.0e6, nullptr);
    }

    void addMarker (te::Edit& edit, const juce::String& name, double time)
    {
        juce::ValueTree m (markerType);
        setMarker (m, name, time);
        markerList (edit, true).appendChild (m, nullptr);
    }

    void removeMarker (te::Edit& edit, const juce::String& name)
    {
        if (auto m = findMarker (edit, name); m.isValid())
            m.getParent().removeChild (m, nullptr);
    }

    juce::var describeMarker (te::Edit& edit, const Marker& m)
    {
        juce::String section;

        for (auto* sec : sortedSections (edit))
            if (sec->getPosition().getStart().inSeconds() <= m.time + edgeTolMarkers)
                section = sec->getName();

        auto bb = edit.tempoSequence.toBarsAndBeats (te::TimePosition::fromSeconds (m.time));
        auto o = new juce::DynamicObject();
        o->setProperty ("name", m.name);
        o->setProperty ("time", m.time);
        o->setProperty ("section", section);
        o->setProperty ("bar", bb.bars + 1);
        o->setProperty ("beat", std::round ((bb.beats.inBeats() + 1.0) * 1000.0) / 1000.0);
        return juce::var (o);
    }

    juce::String nextMarkerName (te::Edit& edit)
    {
        int maxN = 0;

        for (auto& m : sortedMarkers (edit))
            if (m.name.startsWith ("Marker "))
                maxN = juce::jmax (maxN, m.name.fromFirstOccurrenceOf ("Marker ", false, false).getIntValue());

        return "Marker " + juce::String (maxN + 1);
    }

    namespace
    {
        constexpr double edgeTol = 0.001;   // Clip-/Section-Grenzen (wie sectionAt)

        // Marker ab t um delta verschieben (Zeit-Helfer, markersFollowTime)
        void shiftMarkersFrom (te::Edit& edit, double t, double delta)
        {
            if (! markersFollowTime)
                return;

            for (auto m : markerList (edit, false))
                if (auto time = (double) m.getProperty (timeProp, 0.0); time >= t - edgeTol)
                    setMarker (m, m.getProperty (te::IDs::name).toString(), time + delta);
        }

        // Clips und Sections, die ab t beginnen, um delta verschieben —
        // preserveSync=false: der Clip spielt weiter die gleiche Stelle der
        // Quelle, keepLength=true
        void shiftItemsFrom (te::Edit& edit, double t, double delta)
        {
            auto shift = [t, delta] (te::Clip* c)
            {
                if (c->getPosition().getStart().inSeconds() >= t - edgeTol)
                    c->setStart (c->getPosition().getStart() + te::TimeDuration::fromSeconds (delta), false, true);
            };

            if (auto* mt = edit.getMarkerTrack())
                for (auto* clip : mt->getClips())
                    shift (clip);

            for (auto* track : clipTracks (edit))
                for (auto* clip : track->getClips())
                    shift (clip);

            shiftMarkersFrom (edit, t, delta);
        }

        // Noten, die der Clip nur verdeckt (ausserhalb seiner Grenzen),
        // wegraeumen — jedes Teilstueck hat nur, was in ihm klingt
        void dropHiddenNotes (te::Clip& clip)
        {
            if (auto* mc = dynamic_cast<te::MidiClip*> (&clip))
            {
                auto p = mc->getPosition();
                juce::Array<te::MidiNote*> hidden;

                for (auto* n : mc->getSequence().getNotes())
                {
                    auto s = n->getEditTimeRange (*mc).getStart().inSeconds();

                    if (s < p.getStart().inSeconds() - 0.0005 || s >= p.getEnd().inSeconds() - 0.0005)
                        hidden.add (n);
                }

                for (auto* n : hidden)
                    mc->getSequence().removeNote (*n, nullptr);
            }
        }

        // Clip auf [from, to) zuschneiden, das Material bleibt an seiner
        // Songzeit (preserveSync: der Offset waechst, nie unter 0)
        void cutClipTo (te::Clip& clip, double from, double to)
        {
            if (clip.getPosition().getStart().inSeconds() < from - edgeTol)
                clip.setStart (te::TimePosition::fromSeconds (from), true, false);

            if (clip.getPosition().getEnd().inSeconds() > to + edgeTol)
                clip.setEnd (te::TimePosition::fromSeconds (to), true);

            dropHiddenNotes (clip);
        }

        // Automation stueckweise umlegen: jedes Stueck [from, to) der alten
        // Zeit landet um delta verschoben (Stuecke duerfen sich in der alten
        // Zeit ueberlappen — eine Kopie). An den Stueckgrenzen haelt je ein
        // Punkt den Wert der alten Kurve, so bleibt die Form in jedem Stueck;
        // Grenzpunkte, die nichts aendern, fallen wieder weg (eine
        // unberuehrte Kurve behaelt genau ihre Punkte).
        struct Piece { double from, to, delta; };

        void remapAutomation (te::Edit& edit, const std::vector<Piece>& pieces)
        {
            struct Pt { double time; float value, curve; bool added; int order; };

            for (auto* prm : edit.getAllAutomatableParams (true))
            {
                auto& curve = prm->getCurve();
                const int n = curve.getNumPoints();

                if (n == 0)
                    continue;

                std::vector<Pt> src;

                for (int i = 0; i < n; ++i)
                    src.push_back ({ curve.getPointTime (i).inSeconds(), curve.getPointValue (i), curve.getPointCurve (i), false, 1 });

                const double eps = 1.0e-9;

                auto lerp = [&] (size_t a, double t)
                {
                    auto& p = src[a];
                    auto& q = src[a + 1];
                    return q.time - p.time < eps ? q.value
                                                 : (float) (p.value + (q.value - p.value) * (t - p.time) / (q.time - p.time));
                };

                // Wert unmittelbar vor t (an einem Sprung: der linke)
                auto leftValue = [&] (double t)
                {
                    size_t i = 0;

                    while (i < src.size() && src[i].time < t - eps)
                        ++i;

                    if (i == 0) return src.front().value;
                    if (i == src.size()) return src.back().value;
                    if (src[i].time <= t + eps) return src[i].value;
                    return lerp (i - 1, t);
                };

                // Wert ab t (an einem Sprung: der rechte)
                auto rightValue = [&] (double t)
                {
                    int j = (int) src.size() - 1;

                    while (j >= 0 && src[(size_t) j].time > t + eps)
                        --j;

                    if (j < 0) return src.front().value;
                    if (j == (int) src.size() - 1) return src.back().value;
                    if (src[(size_t) j].time >= t - eps) return src[(size_t) j].value;
                    return lerp ((size_t) j, t);
                };

                std::vector<Pt> out;

                for (auto& pc : pieces)
                {
                    if (pc.to <= pc.from + eps)
                        continue;

                    out.push_back ({ pc.from + pc.delta, rightValue (pc.from), 0.0f, true, 1 });

                    for (auto& p : src)
                        if (p.time >= pc.from - 1.0e-6 && p.time < pc.to - 1.0e-6)
                            out.push_back ({ p.time + pc.delta, p.value, p.curve, false, 1 });

                    if (std::isfinite (pc.to))
                        out.push_back ({ pc.to + pc.delta, leftValue (pc.to), 0.0f, true, 0 });
                }

                // Ein Stueck endet, bevor das naechste an derselben Stelle beginnt
                std::stable_sort (out.begin(), out.end(), [eps] (const Pt& a, const Pt& b)
                                  { return a.time < b.time - eps || (std::abs (a.time - b.time) <= eps && a.order < b.order); });

                auto same = [eps] (const Pt& a, const Pt& b)
                {
                    return std::abs (a.time - b.time) <= eps && std::abs (a.value - b.value) <= 1.0e-6f;
                };

                for (bool changed = true; changed;)
                {
                    changed = false;

                    for (size_t i = 0; i < out.size(); ++i)
                    {
                        if (! out[i].added)
                            continue;

                        const bool hasPrev = i > 0, hasNext = i + 1 < out.size();
                        auto& p = out[i];
                        bool drop = (hasPrev && same (out[i - 1], p)) || (hasNext && same (out[i + 1], p))
                                 || (! hasPrev && hasNext && std::abs (out[i + 1].value - p.value) <= 1.0e-6f)
                                 || (hasPrev && ! hasNext && std::abs (out[i - 1].value - p.value) <= 1.0e-6f);

                        if (! drop && hasPrev && hasNext)
                        {
                            auto& a = out[i - 1];
                            auto& b = out[i + 1];

                            if (p.time - a.time > eps && b.time - p.time > eps && a.curve == 0.0f
                                && std::abs (a.value + (b.value - a.value) * (p.time - a.time) / (b.time - a.time) - p.value) <= 1.0e-5f)
                                drop = true;
                        }

                        if (drop)
                        {
                            out.erase (out.begin() + (long) i);
                            changed = true;
                            break;
                        }
                    }
                }

                bool unchanged = out.size() == src.size();

                for (size_t i = 0; unchanged && i < out.size(); ++i)
                    unchanged = std::abs (out[i].time - src[i].time) <= eps && out[i].value == src[i].value;

                if (unchanged)
                    continue;

                curve.clear (nullptr);

                for (auto& p : out)
                    curve.addPoint (te::TimePosition::fromSeconds (juce::jmax (0.0, p.time)), p.value, p.curve, nullptr);
            }
        }
    }

    juce::String fadeCurveName (te::AudioFadeCurve::Type t)
    {
        switch (t)
        {
            case te::AudioFadeCurve::convex:  return "convex";
            case te::AudioFadeCurve::concave: return "concave";
            case te::AudioFadeCurve::sCurve:  return "sCurve";
            default:                          return "linear";
        }
    }

    namespace
    {
        const juce::Identifier autoCurveProp ("dawdAutoCurve");
    }

    bool isFadeCurveAuto (const te::AudioClipBase& ac)
    {
        return (bool) ac.state.getProperty (autoCurveProp, false);
    }

    // Ausdruecklich als false gespeichert: setClip setzt eine Kurve gleich
    // dem Engine-Default (linear) ohne Eigenschaft im Edit
    void setFadeCurveAuto (te::AudioClipBase& ac, bool on)
    {
        ac.state.setProperty (autoCurveProp, on, nullptr);
    }

    const juce::Identifier warpTreeId ("DAWDWARP");

    namespace
    {
        // Warp-Punkte (Quelle, Warp) eines gewarpten Clips: Nutzer-Marker
        // aus DAWDWARP, dazu die Anker an Dateianfang und -ende
        std::vector<std::pair<double, double>> warpPoints (const te::AudioClipBase& ac)
        {
            std::vector<std::pair<double, double>> out;
            auto warp = ac.state.getChildWithName (warpTreeId);
            auto len = (double) warp.getProperty ("length", 0.0);

            for (auto m : warp)
                out.push_back ({ (double) m.getProperty ("source", 0.0), (double) m.getProperty ("warp", 0.0) });

            if (out.empty() || out.front().first > 1.0e-6)
                out.insert (out.begin(), { 0.0, 0.0 });

            if (out.back().first < len - 1.0e-6)
                out.push_back ({ len, len });

            return out;
        }

        // Linear zwischen den Punkten (a -> b), aussen Steigung 1
        double mapPoints (const std::vector<std::pair<double, double>>& pts, double x, bool sourceToWarp)
        {
            auto from = [&] (size_t i) { return sourceToWarp ? pts[i].first : pts[i].second; };
            auto to = [&] (size_t i) { return sourceToWarp ? pts[i].second : pts[i].first; };

            if (pts.empty())
                return x;

            if (x <= from (0))
                return to (0) + (x - from (0));

            for (size_t i = 1; i < pts.size(); ++i)
                if (x <= from (i))
                {
                    auto span = from (i) - from (i - 1);
                    return span <= 0.0 ? to (i) : to (i - 1) + (x - from (i - 1)) / span * (to (i) - to (i - 1));
                }

            return to (pts.size() - 1) + (x - from (pts.size() - 1));
        }

        double fileBpmOf (te::AudioClipBase& ac)
        {
            return ac.getLoopInfo().getBpm (te::AudioFile (ac.edit.engine, ac.getOriginalFile()).getInfo());
        }
    }

    bool isWarped (const te::AudioClipBase& ac)
    {
        return ac.state.getChildWithName (warpTreeId).isValid();
    }

    juce::File unwarpedFile (const te::AudioClipBase& ac)
    {
        // Echtzeit-Warp: der Clip spielt die Originaldatei selbst (ihr Verweis
        // folgt einem verschobenen Projektordner, source nicht)
        auto warp = ac.state.getChildWithName (warpTreeId);
        return warp.isValid() && ! (bool) warp.getProperty ("realtime", false) ? juce::File (warp.getProperty ("source", {}).toString())
                                                                               : ac.getOriginalFile();
    }

    std::vector<std::pair<te::TimePosition, te::TimePosition>> realtimeWarpPoints (const te::AudioClipBase& ac)
    {
        std::vector<std::pair<te::TimePosition, te::TimePosition>> out;
        auto warp = ac.state.getChildWithName (warpTreeId);

        if (! warp.isValid() || ! (bool) warp.getProperty ("realtime", false))
            return out;

        for (auto m : warp)
            out.push_back ({ te::TimePosition::fromSeconds ((double) m.getProperty ("source", 0.0)),
                             te::TimePosition::fromSeconds ((double) m.getProperty ("warp", 0.0)) });

        return out;
    }

    void playWarpSources (te::Edit& edit)
    {
        for (auto* track : clipTracks (edit))
            for (auto* c : track->getClips())
                if (auto* ac = dynamic_cast<te::AudioClipBase*> (c))
                    if (auto warp = ac->state.getChildWithName (warpTreeId); warp.isValid() && ! (bool) warp.getProperty ("realtime", false))
                    {
                        // Gleiche Laenge wie die gerenderte Datei: LoopInfo,
                        // Offset und Loop gelten weiter
                        juce::File source (warp.getProperty ("source", {}).toString());
                        ac->getSourceFileReference().setToFile (source, te::SourceFileReference::PathStyle::chooseBest, true);
                        warp.setProperty ("realtime", true, nullptr);
                    }
    }

    double warpToSource (const te::AudioClipBase& ac, double warpSeconds)
    {
        return isWarped (ac) ? mapPoints (warpPoints (ac), warpSeconds, false) : warpSeconds;
    }

    double sourceToWarp (const te::AudioClipBase& ac, double sourceSeconds)
    {
        return isWarped (ac) ? mapPoints (warpPoints (ac), sourceSeconds, true) : sourceSeconds;
    }

    double clipSourceTime (te::AudioClipBase& ac, double t)
    {
        auto& edit = ac.edit;
        auto pos = ac.getPosition();

        if (! ac.getAutoTempo())
        {
            // Engine (Zeit-Weg): Dateiposition (Offset + t) * speed, im Loop
            // ab dessen Start modulo seiner Laenge (die Laenge mal speed)
            auto x = (pos.getOffset().inSeconds() + t) * ac.getSpeedRatio();

            if (ac.isLooping() && ac.getLoopLength().inSeconds() > 0.0)
            {
                auto len = ac.getLoopLength().inSeconds() * ac.getSpeedRatio();
                x = ac.getLoopStart().inSeconds() + std::fmod (juce::jmax (0.0, x), len);
            }

            return x;
        }

        // Beat-Weg: Beats des Materials (Song-Beats ab dem Clip-Start plus
        // Offset), im Loop modulo, dann Sekunden zum Tempo der Datei
        auto beat = ac.getOffsetInBeats().inBeats() + secondsToBeats (edit, pos.getStart().inSeconds(), t);

        if (ac.isLooping() && ac.getLoopLengthBeats().inBeats() > 0.0)
            beat = ac.getLoopStartBeats().inBeats() + std::fmod (juce::jmax (0.0, beat), ac.getLoopLengthBeats().inBeats());

        auto bpm = fileBpmOf (ac);
        return warpToSource (ac, bpm > 0.0 ? beat * 60.0 / bpm : 0.0);
    }

    juce::Array<juce::var> warpMarkersOf (te::AudioClipBase& ac)
    {
        juce::Array<juce::var> out;

        if (! isWarped (ac))
            return out;

        auto bpm = fileBpmOf (ac);
        auto start = ac.getPosition().getStart().inSeconds();
        auto offsetBeats = ac.getOffsetInBeats().inBeats();
        auto r3 = [] (double v) { return std::round (v * 1000.0) / 1000.0; };

        for (auto m : ac.state.getChildWithName (warpTreeId))
        {
            auto beat = (double) m.getProperty ("warp", 0.0) * bpm / 60.0 - offsetBeats;
            out.add (juce::var (new juce::DynamicObject()));
            auto* o = out.getReference (out.size() - 1).getDynamicObject();
            o->setProperty ("source", r3 ((double) m.getProperty ("source", 0.0)));
            o->setProperty ("beat", r3 (beat));
            // vor dem Clip-Start (Material vor der Kante): Tempo am Start
            auto bps = ac.edit.tempoSequence.getBeatsPerSecondAt (te::TimePosition::fromSeconds (start));
            o->setProperty ("time", r3 (beat >= 0.0 ? beatsToSeconds (ac.edit, start, beat) : beat / bps));
        }

        return out;
    }

    juce::var clipLoopOf (te::AudioClipBase& ac)
    {
        if (! ac.isLooping())
            return {};

        auto r3 = [] (double v) { return std::round (v * 1000.0) / 1000.0; };
        auto pos = ac.getPosition();
        auto start = pos.getStart().inSeconds();
        auto length = pos.getLength().inSeconds();
        auto* o = new juce::DynamicObject();
        juce::Array<juce::var> restarts;

        if (ac.getAutoTempo())
        {
            auto bpm = fileBpmOf (ac);
            auto ls = ac.getLoopStartBeats().inBeats();
            auto ll = ac.getLoopLengthBeats().inBeats();
            auto off = ac.getOffsetInBeats().inBeats();
            auto toSource = [&] (double beat) { return warpToSource (ac, bpm > 0.0 ? beat * 60.0 / bpm : 0.0); };
            o->setProperty ("start", r3 (toSource (ls)));
            o->setProperty ("length", r3 (toSource (ls + ll) - toSource (ls)));
            o->setProperty ("offset", r3 (toSource (ls + off) - toSource (ls)));
            o->setProperty ("startBeat", r3 (ls));
            o->setProperty ("lengthBeats", r3 (ll));
            o->setProperty ("offsetBeats", r3 (off));

            for (auto b = ll - std::fmod (off, ll); ll > 0.0 && restarts.size() < 64; b += ll)
            {
                auto s = beatsToSeconds (ac.edit, start, b);

                if (s >= length - 1.0e-6)
                    break;

                restarts.add (r3 (s));
            }
        }
        else
        {
            // Zeit-Weg (nur speed 1, siehe loopClip): Sekunden der Datei
            auto ll = ac.getLoopLength().inSeconds();
            auto off = pos.getOffset().inSeconds();
            o->setProperty ("start", r3 (ac.getLoopStart().inSeconds()));
            o->setProperty ("length", r3 (ll));
            o->setProperty ("offset", r3 (off));

            for (auto s = ll - std::fmod (off, ll); ll > 0.0 && s < length - 1.0e-6 && restarts.size() < 64; s += ll)
                restarts.add (r3 (s));
        }

        o->setProperty ("restarts", restarts);
        return juce::var (o);
    }

    void unityMasterVolume (te::Edit& edit)
    {
        static const juce::Identifier unityProp ("dawdMasterUnity");
        auto mv = edit.getMasterVolumePlugin();

        if (mv == nullptr || mv->state.hasProperty (unityProp))
            return;

        if (std::abs (mv->getVolumeDb() - (-3.0f)) < 0.01f)
            mv->setVolumeDb (0.0f);

        mv->state.setProperty (unityProp, true, nullptr);
    }

    void refreshAutoCrossfades (te::Edit& edit)
    {
        std::vector<te::AudioClipBase*> audio;

        for (auto* track : clipTracks (edit))
            for (auto* c : track->getClips())
                if (auto* ac = dynamic_cast<te::AudioClipBase*> (c))
                {
                    // Nie gesetzt = an (Decision 2026-10-25-crossfade-standard)
                    if (! ac->state.hasProperty (te::IDs::autoCrossfade))
                        ac->setAutoCrossfade (true);

                    // Kurve nie gesetzt: dawd fuehrt sie (aeltere Projekte mit
                    // einer per fadeCurve gesetzten Kurve behalten sie)
                    if (! ac->state.hasProperty (autoCurveProp))
                        setFadeCurveAuto (*ac, ! ac->state.hasProperty (te::IDs::fadeInType)
                                                && ! ac->state.hasProperty (te::IDs::fadeOutType));

                    if (ac->getAutoCrossfade())
                        ac->updateAutoCrossfadesAsync (false);

                    audio.push_back (ac);
                }

        // Gefuehrte Kurve: convex (gleiche Leistung) auf der Seite, die
        // ueberblendet, sonst linear — erst nachdem alle Flags stehen
        using Dir = te::AudioClipBase::ClipDirection;

        for (auto* ac : audio)
            if (isFadeCurveAuto (*ac))
            {
                auto type = [ac] (Dir d)
                {
                    return ac->getAutoCrossfade() && ac->getOverlappingClip (d) != nullptr
                         ? te::AudioFadeCurve::convex : te::AudioFadeCurve::linear;
                };

                if (auto t = type (Dir::previous); ac->getFadeInType() != t)
                    ac->setFadeInType (t);

                if (auto t = type (Dir::next); ac->getFadeOutType() != t)
                    ac->setFadeOutType (t);
            }
    }

    te::Clip* splitClipAt (te::Clip& clip, double at)
    {
        auto pos = clip.getPosition();

        if (at <= pos.getStart().inSeconds() + edgeTol || at >= pos.getEnd().inSeconds() - edgeTol)
            return nullptr;

        auto* ct = clip.getClipTrack();
        auto* right = ct != nullptr ? ct->splitClip (clip, te::TimePosition::fromSeconds (at)) : nullptr;

        if (right == nullptr)
            return nullptr;

        if (auto* track = dynamic_cast<te::AudioTrack*> (ct))
            right->setName (uniqueClipName (*track, clip.getName()));

        right->state.removeProperty ("dawdPattern", nullptr);   // die rechte Haelfte ist keine Kopie
        dropHiddenNotes (clip);
        dropHiddenNotes (*right);
        return right;
    }

    // Die Zeit-Helfer schneiden auch das gefrorene Audio (clipTracks: Nutzer-
    // und Freeze-Spuren) — es bleibt deckungsgleich mit seinem Material
    void splitClipsAt (te::Edit& edit, double t)
    {
        for (auto* track : clipTracks (edit))
        {
            juce::Array<te::Clip*> over;

            for (auto* c : track->getClips())
                if (c->getPosition().getStart().inSeconds() < t - edgeTol && c->getPosition().getEnd().inSeconds() > t + edgeTol)
                    over.add (c);

            for (auto* c : over)
                splitClipAt (*c, t);
        }
    }

    void insertTime (te::Edit& edit, double t, double length)
    {
        splitClipsAt (edit, t);
        shiftItemsFrom (edit, t, length);
        remapAutomation (edit, { { 0.0, t, 0.0 }, { t, HUGE_VAL, length } });
    }

    int removeTime (te::Edit& edit, double start, double end, int* cutOut)
    {
        int removed = 0, cut = 0;

        for (auto* track : clipTracks (edit))
        {
            juce::Array<te::Clip*> clips;

            for (auto* c : track->getClips())
                clips.add (c);

            for (auto* c : clips)
            {
                auto cs = c->getPosition().getStart().inSeconds(), ce = c->getPosition().getEnd().inSeconds();

                if (ce <= start + edgeTol || cs >= end - edgeTol)
                    continue;

                if (cs >= start - edgeTol && ce <= end + edgeTol)
                {
                    c->removeFromParent();
                    ++removed;
                    continue;
                }

                // Ueber den ganzen Bereich: zwei Teile, sonst eine Seite kuerzen
                if (cs < start - edgeTol && ce > end + edgeTol)
                    splitClipAt (*c, end);

                if (cs < start - edgeTol)
                    cutClipTo (*c, cs, start);
                else
                    cutClipTo (*c, end, ce);

                ++cut;
            }
        }

        // Marker im entfernten Bereich fallen weg
        if (markersFollowTime)
            for (auto& m : sortedMarkers (edit))
                if (m.time >= start - edgeTol && m.time < end - edgeTol)
                    removeMarker (edit, m.name);

        shiftItemsFrom (edit, end, start - end);
        remapAutomation (edit, { { 0.0, start, 0.0 }, { end, HUGE_VAL, start - end } });

        if (cutOut != nullptr)
            *cutOut = cut;

        return removed;
    }

    int duplicateTime (te::Edit& edit, double start, double end, double at, juce::StringArray* markersOut)
    {
        const double len = end - start;
        splitClipsAt (edit, at);
        shiftItemsFrom (edit, at, len);

        // Die Quelle rueckt mit, wenn sie hinter der Einfuegestelle liegt
        const double s = at <= start + edgeTol ? start + len : start;
        const double e = s + len;
        int copied = 0;

        for (auto* track : clipTracks (edit))
        {
            juce::Array<te::Clip*> clips;

            for (auto* c : track->getClips())
                if (c->getPosition().getStart().inSeconds() < e - edgeTol && c->getPosition().getEnd().inSeconds() > s + edgeTol)
                    clips.add (c);

            for (auto* c : clips)
                if (auto copy = te::duplicateClip (*c))
                {
                    copy->setName (uniqueClipName (*track, c->getName()));

                    if (c->getPosition().getStart().inSeconds() < s - edgeTol || c->getPosition().getEnd().inSeconds() > e + edgeTol)
                    {
                        cutClipTo (*copy, s, e);
                        copy->state.removeProperty ("dawdPattern", nullptr);   // ein Teilstueck ist keine Kopie
                    }

                    copy->setStart (te::TimePosition::fromSeconds (at + copy->getPosition().getStart().inSeconds() - s), false, true);
                    ++copied;
                }
        }

        // Marker des Bereichs: Kopie an derselben Stelle der Kopie, Name mit
        // Suffix wie bei duplicateSection ("Drop" -> "Drop 2")
        for (auto& m : sortedMarkers (edit))
            if (m.time >= s - edgeTol && m.time < e - edgeTol)
            {
                juce::String name;

                for (int n = 2; name.isEmpty() || findMarker (edit, name).isValid(); ++n)
                    name = m.name + " " + juce::String (n);

                addMarker (edit, name, at + m.time - s);

                if (markersOut != nullptr)
                    markersOut->add (name);
            }

        remapAutomation (edit, { { 0.0, at, 0.0 }, { start, end, at - start }, { at, HUGE_VAL, len } });
        return copied;
    }

    void reorderTime (te::Edit& edit, const std::vector<TimeShift>& shifts)
    {
        auto sorted = shifts;
        std::sort (sorted.begin(), sorted.end(), [] (auto& a, auto& b) { return a.from < b.from; });

        // Clips ueber einer Grenze, an der sich die Verschiebung aendert, teilen
        for (size_t i = 0; i + 1 < sorted.size(); ++i)
            if (std::abs (sorted[i].delta - sorted[i + 1].delta) > 1.0e-9)
                splitClipsAt (edit, sorted[i].to);

        // Erst messen, dann schieben — sonst rutscht ein Clip in den Bereich
        // des naechsten Stuecks und wandert doppelt
        std::vector<std::pair<te::Clip*, double>> moves;

        for (auto* track : clipTracks (edit))
            for (auto* c : track->getClips())
            {
                auto cs = c->getPosition().getStart().inSeconds();

                for (auto& sh : sorted)
                    if (cs >= sh.from - edgeTol && cs < sh.to - edgeTol)
                    {
                        if (std::abs (sh.delta) > 1.0e-9)
                            moves.push_back ({ c, sh.delta });

                        break;
                    }
            }

        for (auto& [c, delta] : moves)
            c->setStart (c->getPosition().getStart() + te::TimeDuration::fromSeconds (delta), false, true);

        // Marker wandern mit ihrem Stueck
        if (markersFollowTime)
            for (auto m : markerList (edit, false))
            {
                auto time = (double) m.getProperty (timeProp, 0.0);

                for (auto& sh : sorted)
                    if (time >= sh.from - edgeTol && time < sh.to - edgeTol)
                    {
                        setMarker (m, m.getProperty (te::IDs::name).toString(), time + sh.delta);
                        break;
                    }
            }

        std::vector<Piece> pieces;

        for (auto& sh : sorted)
            pieces.push_back ({ sh.from, sh.to, sh.delta });

        remapAutomation (edit, pieces);
    }

    namespace
    {
        const juce::Identifier compProp ("dawdComp"), compFromProp ("dawdCompFrom");

        // Blende an einer Schnittkante (Audio), passend in den Clip
        void setCutFade (te::Clip& c, bool in, double fade)
        {
            if (auto* ac = dynamic_cast<te::AudioClipBase*> (&c))
            {
                auto len = c.getPosition().getLength().inSeconds();
                auto other = (in ? ac->getFadeOut() : ac->getFadeIn()).inSeconds();
                auto f = te::TimeDuration::fromSeconds (juce::jlimit (0.0, juce::jmax (0.0, len - other), fade));

                if (in)
                    ac->setFadeIn (f);
                else
                    ac->setFadeOut (f);
            }
        }

        // [from, to) der Comp-Lane leeren; Clips ueber den Grenzen werden
        // dort abgeschnitten und blenden an der Schnittkante
        void clearCompRange (te::AudioTrack& comp, double from, double to, double fade)
        {
            juce::Array<te::Clip*> clips;

            for (auto* c : comp.getClips())
                clips.add (c);

            for (auto* c : clips)
            {
                auto cs = c->getPosition().getStart().inSeconds(), ce = c->getPosition().getEnd().inSeconds();

                if (ce <= from + edgeTol || cs >= to - edgeTol)
                    continue;

                if (cs >= from - edgeTol && ce <= to + edgeTol)
                {
                    c->removeFromParent();
                    continue;
                }

                if (cs < from - edgeTol && ce > to + edgeTol)
                    if (auto* right = splitClipAt (*c, to))
                        setCutFade (*right, true, fade);

                if (cs < from - edgeTol)
                {
                    cutClipTo (*c, cs, from);
                    setCutFade (*c, false, fade);
                }
                else
                {
                    cutClipTo (*c, to, ce);
                    setCutFade (*c, true, fade);
                }
            }
        }

        // Clip-Teile der Lane in [from, to) als Comp-Teile auf die Comp-Lane
        // kopieren (Material an seiner Songzeit, Schnittkanten blenden)
        void copyToComp (te::AudioTrack& take, te::AudioTrack& comp, double from, double to, double fade)
        {
            juce::Array<te::Clip*> clips;

            for (auto* c : take.getClips())
                if (c->getPosition().getStart().inSeconds() < to - edgeTol && c->getPosition().getEnd().inSeconds() > from + edgeTol)
                    clips.add (c);

            for (auto* c : clips)
            {
                const auto name = uniqueClipName (comp, c->getName());
                auto copy = te::duplicateClip (*c);

                if (copy == nullptr)
                    continue;

                if (! copy->moveTo (comp))
                {
                    copy->removeFromParent();
                    continue;
                }

                const auto cs = c->getPosition().getStart().inSeconds(), ce = c->getPosition().getEnd().inSeconds();
                cutClipTo (*copy, from, to);
                copy->setName (name);
                copy->state.removeProperty ("dawdPattern", nullptr);
                copy->state.setProperty (compFromProp, take.itemID.toString(), nullptr);

                if (cs < from - edgeTol)
                    setCutFade (*copy, true, fade);

                if (ce > to + edgeTol)
                    setCutFade (*copy, false, fade);
            }
        }
    }

    bool isCompLane (const te::AudioTrack& t)
    {
        return (bool) t.state.getProperty (compProp, false);
    }

    te::AudioTrack* compLaneOf (te::Edit& edit, const te::AudioTrack& group)
    {
        for (auto* t : userTracks (edit))
            if (laneGroupOf (*t) == &group && isCompLane (*t))
                return t;

        return nullptr;
    }

    te::AudioTrack* compSourceOf (te::Edit& edit, const te::Clip& clip)
    {
        auto id = clip.state.getProperty (compFromProp).toString();

        if (id.isNotEmpty())
            for (auto* t : userTracks (edit))
                if (t->itemID.toString() == id)
                    return t;

        return nullptr;
    }

    te::AudioTrack* compRange (te::Edit& edit, te::AudioTrack& take, double start, double end, double fade)
    {
        auto* group = laneGroupOf (take);

        if (group == nullptr || isCompLane (take))
            return nullptr;

        auto* comp = compLaneOf (edit, *group);

        // Der Comp beginnt mit dem, was jetzt spielt
        if (comp == nullptr)
        {
            auto* playing = activeLaneOf (edit, *group);
            comp = makeLaneTrack (edit, *group);
            comp->state.setProperty (compProp, true, nullptr);
            comp->setName (group->getName() + ".comp");

            if (playing != nullptr)
                copyToComp (*playing, *comp, 0.0, HUGE_VAL, 0.0);
        }

        // Die Blende liegt mittig auf der Grenze: der alte Teil laeuft eine
        // halbe Blende hinein, der neue beginnt eine halbe davor
        const double h = fade / 2.0;
        clearCompRange (*comp, start + h, end - h, fade);
        copyToComp (take, *comp, juce::jmax (0.0, start - h), end + h, fade);
        return comp;
    }

    juce::Array<juce::var> compRanges (te::Edit& edit, te::AudioTrack& comp)
    {
        struct Part { double start, end; te::AudioTrack* from; };
        std::vector<Part> parts;

        for (auto* c : comp.getClips())
            parts.push_back ({ c->getPosition().getStart().inSeconds(), c->getPosition().getEnd().inSeconds(), compSourceOf (edit, *c) });

        std::sort (parts.begin(), parts.end(), [] (auto& a, auto& b) { return a.start < b.start; });

        // Ueberlappung = Blende: die Grenze liegt in ihrer Mitte
        for (size_t i = 1; i < parts.size(); ++i)
            if (parts[i].start < parts[i - 1].end - edgeTol)
            {
                auto mid = (parts[i].start + parts[i - 1].end) / 2.0;
                parts[i - 1].end = mid;
                parts[i].start = mid;
            }

        auto tracks = userTracks (edit);
        auto r3 = [] (double v) { return std::round (v * 1000.0) / 1000.0; };
        juce::Array<juce::var> out;
        Part cur {};
        bool open = false;

        auto flush = [&]
        {
            if (! open)
                return;

            auto* o = new juce::DynamicObject();
            o->setProperty ("lane", tracks.indexOf (cur.from));
            o->setProperty ("name", cur.from != nullptr ? cur.from->getName() : juce::String());
            o->setProperty ("start", r3 (cur.start));
            o->setProperty ("end", r3 (cur.end));
            out.add (juce::var (o));
        };

        for (auto& p : parts)
        {
            if (open && p.from == cur.from && p.start <= cur.end + edgeTol)
            {
                cur.end = juce::jmax (cur.end, p.end);
                continue;
            }

            flush();
            cur = p;
            open = true;
        }

        flush();
        return out;
    }

    juce::String monoModeOf (te::Plugin& p)
    {
        for (auto* prm : p.getAutomatableParameters())
        {
            auto name = prm->getParameterName();
            auto lower = name.toLowerCase();

            if (! (lower.contains ("play mode") || lower.contains ("voice mode") || lower == "mono" || lower == "polyphony"))
                continue;

            auto text = prm->getCurrentValueAsString().trim();
            auto t = text.toLowerCase();

            // "Polyphony" als Stimmenzahl: 1 = einstimmig
            if (lower == "polyphony" ? t == "1" : ((t.contains ("mono") || t.contains ("legato")) && ! t.contains ("poly")))
                return name + ": " + text;

            if (lower == "mono" && (t == "on" || t == "1"))
                return name + ": " + text;
        }

        // Einstellungen ohne Automations-Parameter (4OSC: Voice Mode)
        for (auto& st : pluginSettings (p))
            if (st.name.equalsIgnoreCase ("Voice Mode") && st.get)
                if (auto v = st.get().toString(); v == "mono" || v == "legato")
                    return st.name + ": " + v;

        return {};
    }

    bool hasChords (const juce::MidiMessageSequence& seq)
    {
        std::vector<std::pair<double, double>> notes;

        for (int i = 0; i < seq.getNumEvents(); ++i)
            if (auto* e = seq.getEventPointer (i); e->message.isNoteOn() && e->noteOffObject != nullptr)
                notes.push_back ({ e->message.getTimeStamp(), e->noteOffObject->message.getTimeStamp() });

        std::sort (notes.begin(), notes.end());

        for (size_t i = 1; i < notes.size(); ++i)
            for (size_t j = i; j-- > 0;)
                if (notes[j].second - notes[i].first > 0.03)
                    return true;

        return false;
    }

    void settlePlugins (te::Edit& edit)
    {
        for (auto* p : te::getAllPlugins (edit, true))
            if (auto* sp = dynamic_cast<te::SamplerPlugin*> (p))
                sp->flushPendingUpdates();
    }

    namespace
    {
        // Instanzen, die schon Bloecke bekamen (die des letzten Aufrufs), und
        // die davon, deren Namen noch ausstehen. Schwache Referenz aufs
        // Plugin: ein neues Plugin an der Adresse eines geloeschten (treeClose
        // + treeOpen, Auto-Node) gilt nicht als schon beruhigt
        struct SettledPlugin
        {
            juce::WeakReference<te::Selectable> plugin;
            juce::AudioPluginInstance* instance = nullptr;
        };

        std::vector<SettledPlugin> settledPlugins;
        std::vector<juce::WeakReference<te::Selectable>> namesPending;
    }

    bool syncPluginParameters (te::Plugin& p)
    {
        auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);
        auto* pi = ep != nullptr ? ep->getAudioPluginInstance() : nullptr;

        if (pi == nullptr)
            return false;

        bool synced = false;

        if (isSandboxed (pi))
            synced = syncSandboxedController (p);
        else if (auto* vst3 = pi->getVST3Client())
            synced = vst3->syncControllerWithComponent();

        if (synced)
            ep->refreshParameterValues();

        return synced;
    }

    namespace
    {
        void passSilentBlocks (te::ExternalPlugin& ep)
        {
            // Je Block 1 Sample Stille ueber den Weg der Engine (nur ohne
            // Graph, nur eingeschaltet, vorher einmal vorbereitet; in der
            // Sandbox ein Rundlauf zum Host). Surge XT meldete ab dem 12.
            // Block die neuen Namen (16 x 1 Sample reichten, 11 x 256 nicht)
            for (int i = 0; i < 32; ++i)
            {
                ep.markValuesPending();
                ep.passPendingValuesToPlugin();
            }
        }
    }

    void settlePluginNow (te::Plugin& p)
    {
        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p); ep != nullptr && ep->getAudioPluginInstance() != nullptr)
        {
            passSilentBlocks (*ep);
            syncPluginParameters (p);
        }
    }

    void settleParameterNames (te::Edit& edit, te::Plugin* touched)
    {
        std::vector<SettledPlugin> now;

        for (auto* p : te::getAllPlugins (edit, true))
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (p);
            auto* pi = ep != nullptr ? ep->getAudioPluginInstance() : nullptr;

            if (pi == nullptr)
                continue;

            now.push_back ({ p, pi });
            const bool known = std::any_of (settledPlugins.begin(), settledPlugins.end(),
                                            [&] (const SettledPlugin& s) { return s.plugin.get() == p && s.instance == pi; });

            if (known && p != touched)
                continue;

            passSilentBlocks (*ep);

            if (syncPluginParameters (*p))
                rememberParameterNames (*p);

            namesPending.push_back (p);
        }

        settledPlugins = std::move (now);
    }

    void rememberSettledNames()
    {
        for (auto& w : namesPending)
            if (auto* p = dynamic_cast<te::Plugin*> (w.get()))
            {
                syncPluginParameters (*p);
                rememberParameterNames (*p);
            }

        namesPending.clear();
    }

    bool anyTrackSolo (te::Edit& edit)
    {
        for (auto* t : te::getAllTracks (edit))
            if (t->isSolo (false))
                return true;

        return false;
    }

    void applySavedListening (juce::ValueTree& v, const std::map<juce::String, bool>& savedMute)
    {
        if (te::TrackList::isTrack (v))
        {
            v.removeProperty (te::IDs::solo, nullptr);

            if (auto it = savedMute.find (v[te::IDs::id].toString()); it != savedMute.end())
                v.setProperty (te::IDs::mute, it->second, nullptr);
        }

        for (auto child : v)
            applySavedListening (child, savedMute);
    }

    int trackIndexFor (te::Edit& edit, const juce::String& ref)
    {
        auto r = ref.trim();
        auto tracks = userTracks (edit);

        if (r.isEmpty())
            return -1;

        if (r.containsOnly ("-0123456789"))
            return r.getIntValue();

        for (int i = 0; i < tracks.size(); ++i)
            if (tracks[i]->getName().equalsIgnoreCase (r))
                return i;

        return -1;
    }

    juce::String checkTrackName (te::Edit& edit, int index, const juce::String& name)
    {
        auto n = name.trim();
        auto tracks = userTracks (edit);

        if (n.isEmpty())
            return "name must not be empty";

        if (n.length() > 40)
            return "name too long (max 40 characters)";

        if (n.containsOnly ("-0123456789") || n.equalsIgnoreCase ("main") || n.containsChar ('.'))
            return "name must not be a number, main or contain a dot (lanes are <name>.<n>)";

        for (int i = 0; i < tracks.size(); ++i)
            if (i != index && laneGroupOf (*tracks[i]) == nullptr && tracks[i]->getName().equalsIgnoreCase (n))
                return "name already used by track " + juce::String (i);

        return {};
    }

    void renameTrack (te::Edit& edit, int index, const juce::String& name)
    {
        auto tracks = userTracks (edit);

        if (index < 0 || index >= tracks.size())
            return;

        auto* group = laneGroupOf (*tracks[index]) != nullptr ? laneGroupOf (*tracks[index]) : tracks[index];
        group->setName (name.trim());

        // Lanes: "<Name>.<n>" mit ihrer bisherigen Nummer
        for (auto* t : tracks)
            if (laneGroupOf (*t) == group)
                t->setName (name.trim() + "." + t->getName().fromLastOccurrenceOf (".", false, false));
    }
}
