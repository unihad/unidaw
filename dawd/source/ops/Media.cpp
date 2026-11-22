#include "Ops.h"
#include "../Origins.h"
#include "../EngineHelpers.h"
#include "../Undo.h"
#include "../LiveLoudness.h"
#include "../MusicAnalysis.h"


// Audio und MIDI: Aufnahme in Sections, Inserts als Lanes, Monitoring,
// Clip-Bearbeitung (kind process).
namespace dawd
{
    namespace
    {
        // copies ist ein Schalter (alle Kopien des Musters), keine Anzahl —
        // Praxistest remix-1: setMidiNotes {copies: 16} sollte 16-mal wiederholen
        juce::String copiesError (const juce::var& cmd)
        {
            if (cmd.hasProperty ("copies") && ! cmd.getProperty ("copies", {}).isBool())
                return juce::String::fromUTF8 ("copies must be true or false — it writes into the clip's duplicateClip copies, it is not a repeat count (duplicateClip {count} repeats a clip)");

            return {};
        }

        // Transposition eines MIDI-Clips (setClip), absolut: die Noten sind um
        // diesen Wert verschoben; setMidiNotes schreibt absolute Noten -> 0
        const juce::Identifier transposeProp ("dawdTranspose");

        // Normalisieren (setClip normalize): Zielpegel ohne targetDb.
        // Voreinstellung, Frage an Matthias im Todo clips-arrangement
        constexpr double normalizePeakDefaultDb = -1.0, normalizeRmsDefaultDb = -20.0;

        // Pegel des hoerbaren Bereichs eines Audio-Clips in dBFS, gemessen in
        // der Quelldatei — ohne Clip-Gain und Blenden (Peak: hoechster
        // |Wert| aller Kanaele; RMS: ueber alle Kanaele). Gestreckt und
        // followTempo ueber die Abbildung der Engine (clipTimeToSourceFileTime),
        // rueckwaerts gespiegelt. Fehlertext oder "".
        juce::String measureClipLevel (DawdContext& ctx, te::AudioClipBase& clip, bool rms, bool reversed, double& db)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (*ctx.engine, clip.getOriginalFile()));

            if (reader == nullptr || reader->sampleRate <= 0.0)
                return "cannot read the clip's audio";

            auto pos = clip.getPosition();
            auto toSource = [&] (double t)
            {
                if (clip.getAutoTempo())
                    return (clip.getBeatOfRelativeTime (te::TimeDuration::fromSeconds (t)) - clip.getStartBeat() + clip.getOffsetInBeats()).inBeats()
                           / clip.getLoopInfo().getBeatsPerSecond (te::AudioFile (*ctx.engine, clip.getOriginalFile()).getInfo());

                return (t + pos.getOffset().inSeconds()) * clip.getSpeedRatio();
            };

            const double fileLen = (double) reader->lengthInSamples / reader->sampleRate;
            double s0 = juce::jlimit (0.0, fileLen, toSource (0.0));
            double s1 = juce::jlimit (0.0, fileLen, toSource (pos.getLength().inSeconds()));

            if (reversed)
                std::tie (s0, s1) = std::make_pair (fileLen - s1, fileLen - s0);

            auto from = (juce::int64) (s0 * reader->sampleRate), to = (juce::int64) (s1 * reader->sampleRate);
            const int ch = (int) juce::jmax (1u, reader->numChannels);
            juce::AudioBuffer<float> buf (ch, 65536);
            double peak = 0.0, sum = 0.0;
            juce::int64 count = 0;

            for (auto at = from; at < to; at += buf.getNumSamples())
            {
                const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), to - at);
                reader->read (&buf, 0, n, at, true, true);

                for (int c = 0; c < ch; ++c)
                {
                    auto* d = buf.getReadPointer (c);

                    for (int i = 0; i < n; ++i)
                    {
                        peak = juce::jmax (peak, (double) std::abs (d[i]));
                        sum += (double) d[i] * d[i];
                    }
                }

                count += (juce::int64) n * ch;
            }

            const double level = rms ? (count > 0 ? std::sqrt (sum / (double) count) : 0.0) : peak;

            if (level < 1.0e-6)
                return juce::String::fromUTF8 ("the clip is silent — nothing to normalize");

            db = juce::Decibels::gainToDecibels (level);
            return {};
        }

        // Akkorde in ein einstimmiges Instrument: Warnung fuer die Antwort ("" = passt)
        juce::String monoWarning (te::Edit& edit, te::AudioTrack& track, const juce::MidiMessageSequence& seq)
        {
            juce::String inst;
            auto* group = laneGroupOf (track) != nullptr ? laneGroupOf (track) : &track;

            if (! hasChords (seq) || faderTrackOf (edit, *group, nullptr, &inst) == nullptr || inst.isEmpty())
                return {};

            auto* mt = findModuleTrack (edit, inst);
            auto* plugin = mt != nullptr ? modulePlugin (*mt) : nullptr;
            auto mode = plugin != nullptr ? monoModeOf (*plugin) : juce::String();

            return mode.isEmpty() ? juce::String()
                                  : juce::String::fromUTF8 ("notes overlap (chords) but the instrument plays one note at a time (")
                                        + mode + juce::String::fromUTF8 (") — only one note of each chord sounds; set it to Poly");
        }

        // trimClip nach links ueber den Dateianfang: die Engine haelt den
        // Offset bei 0 und wuerde das Audio mit der Kante verschieben. Eine
        // Kopie der Datei mit Stille davor (rueckwaerts: dahinter, das
        // Abspielen spiegelt die Datei) laesst den Clip an newStart mit
        // Offset 0 beginnen und das Audio an seiner Songzeit ("" = ok)
        juce::String padAudioClipStart (DawdContext& ctx, te::AudioClipBase& clip, double newStart)
        {
            auto pos = clip.getPosition();
            auto file = clip.getOriginalFile();
            std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (*ctx.engine, file));

            if (reader == nullptr)
                return "cannot read the clip's audio";

            // So viel Datei fehlt vor dem Dateianfang (Sekunden der Datei):
            // gestreckt mal speedRatio, followTempo in Beats der Aufnahme
            auto missing = pos.getStart().inSeconds() - pos.getOffset().inSeconds() - newStart;
            auto before = missing * clip.getSpeedRatio();

            if (clip.getAutoTempo())
                before = (clip.getContentStartBeat() - clip.edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (newStart))).inBeats()
                         / clip.getLoopInfo().getBeatsPerSecond (te::AudioFile (*ctx.engine, file).getInfo());

            auto padSamples = (int) std::round (juce::jmax (0.0, before) * reader->sampleRate);

            auto editFile = ctx.edit->editFileRetriever ? ctx.edit->editFileRetriever() : juce::File();
            auto dir = editFile != juce::File() ? editFile.getSiblingFile ("media")
                                                : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dawd-media");

            if (! dir.createDirectory().wasOk())
                return "cannot create " + dir.getFullPathName();

            auto padded = dir.getNonexistentChildFile (file.getFileNameWithoutExtension() + "-pad", ".wav", false);
            auto bits = reader->bitsPerSample == 16 || reader->bitsPerSample == 32 ? (int) reader->bitsPerSample : 24;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                juce::WavAudioFormat().createWriterFor (new juce::FileOutputStream (padded), reader->sampleRate,
                                                        reader->numChannels, bits, {}, 0));

            juce::AudioBuffer<float> silence ((int) reader->numChannels, juce::jmax (1, padSamples));
            silence.clear();
            const bool reversed = clip.getIsReversed();

            if (writer == nullptr
                || (! reversed && ! writer->writeFromAudioSampleBuffer (silence, 0, padSamples))
                || ! writer->writeFromAudioReader (*reader, 0, -1)
                || (reversed && ! writer->writeFromAudioSampleBuffer (silence, 0, padSamples)))
            {
                writer.reset();
                padded.deleteFile();
                return "cannot write " + padded.getFullPathName();
            }

            writer.reset();
            recordOrigins (padded, { file.getFullPathName() });   // Lizenz-Sperre
            clip.getSourceFileReference().setToFile (padded, te::SourceFileReference::PathStyle::chooseBest, true);
            ctx.engine->getAudioFileManager().validateFile (te::AudioFile (*ctx.engine, padded), true);
            clip.setPosition ({ { te::TimePosition::fromSeconds (newStart), pos.getEnd() }, {} });
            return {};
        }
    }

    namespace
    {
        // Section, deren Bereich die Zeit enthaelt (Toleranz wie beim
        // Loeschen-Range) — fuer die Aktions-Params, aus denen die
        // Take-Daten am Node abgeleitet werden.
        juce::String sectionAt (te::Edit& edit, double t)
        {
            if (auto* mt = edit.getMarkerTrack())
                for (auto* clip : mt->getClips())
                    if (auto* mc = dynamic_cast<te::MarkerClip*> (clip))
                    {
                        auto pos = mc->getPosition();

                        if (t >= pos.getStart().inSeconds() - 0.001
                            && t < pos.getEnd().inSeconds() - 0.001)
                            return mc->getName();
                    }

            return {};
        }

        // Warnung, wenn eine Lane die bisher spielenden stummschaltet
        // (Praxistest 1d, 2 und 3: die Basslinie war still, nur eine
        // Warnung am Rand) — sagt, warum, und wie es zurueckgeht. reason
        // leer = in eine bestehende, bisher stumme Lane eingefuegt
        juce::String lanesMutedWarning (const juce::String& lane, const juce::String& reason, const juce::StringArray& muted)
        {
            return (reason.isEmpty() ? juce::String ("lane ") : juce::String ("new lane ")) + lane
                   + juce::String::fromUTF8 (" now plays and MUTES ") + muted.joinIntoString (", ")
                   + (reason.isEmpty() ? juce::String() : " (" + reason + ")")
                   + juce::String::fromUTF8 (" — their clips are silent now; laneActive {track: \"")
                   + muted[0] + juce::String::fromUTF8 ("\"} plays them again (one lane plays at a time)");
        }

        // Ueberlappung in der Ziel-Lane ist ein Fehler, keine stille neue
        // Lane (Decision 2026-10-29-ueberlappung-fehler)
        juce::String overlapError (te::AudioTrack& lane, double start, double end)
        {
            return "overlaps " + clipsInRange (lane, start, end).joinIntoString (", ") + " on " + lane.getName()
                   + juce::String::fromUTF8 (" — delete/trim it first, or newLane: true for a new lane (a take; it mutes the other lanes)");
        }

        // Die anderen spielenden Lanes mit Clips, die das Aktivieren von
        // `lane` stummschaltet
        juce::StringArray lanesMutedBy (te::Edit& edit, te::AudioTrack& group, te::AudioTrack* lane)
        {
            auto muted = playingLanesWithClips (edit, group);

            if (lane != nullptr)
                muted.removeString (lane->getName());

            return muted;
        }
    }

    namespace
    {
        juce::String midiGainHint (te::Edit& edit, te::AudioTrack& track, double gainDb)
        {
            juce::String inst, how;
            auto* group = laneGroupOf (track) != nullptr ? laneGroupOf (track) : &track;

            if (faderTrackOf (edit, *group, nullptr, &inst) != nullptr && inst.isNotEmpty())
                if (auto* mt = findModuleTrack (edit, inst))
                    if (auto* plugin = modulePlugin (*mt))
                    {
                        // Gerechnet fuer Anschlag 100 mit den Kurven der Engine
                        const double v100 = 100.0 / 127.0, scale = juce::Decibels::decibelsToGain (gainDb);
                        const double vNew = juce::jmax (1.0, std::round (100.0 * scale)) / 127.0;

                        if (auto* osc = dynamic_cast<te::FourOscPlugin*> (plugin))
                        {
                            // FourOscVoice::velocityToGain: v * 25^v * 0.04
                            const double sens = juce::jlimit (0.0, 100.0, (double) osc->ampVelocityValue.get()) / 100.0;
                            auto curve = [sens] (double vel) { const double v = vel * sens + 1.0 - sens; return v * std::pow (25.0, v) * 0.04; };
                            const double db = juce::Decibels::gainToDecibels (curve (vNew) / curve (v100), -100.0);
                            how = " (4OSC '" + inst + "' with Amp Velocity " + juce::String (juce::roundToInt (sens * 100.0)) + " %: about "
                                  + juce::String (db, 1) + " dB at velocity 100)";
                        }
                        else if (dynamic_cast<te::SamplerPlugin*> (plugin) != nullptr)
                        {
                            // SamplerPlugin: gainDb - 20 * (1 - velocity)
                            how = " (Sampler '" + inst + "': about " + juce::String (-20.0 * (v100 - vNew), 1)
                                  + " dB at velocity 100, never more than -20 dB)";
                        }
                        else
                            how = " (" + plugin->getName() + ": depends on its velocity setting, often only a few dB)";
                    }

            return juce::String::fromUTF8 ("MIDI clip gain only scales the note velocities — how much quieter it gets depends on the instrument's velocity response")
                   + how + juce::String::fromUTF8 ("; for a real level change automate the track volume over the clip (setAutomation t")
                   + juce::String (userTracks (edit).indexOf (group)) + ".volume) or lower the fader";
        }
    }

    juce::var opAddClip (DawdContext& ctx, const juce::var& cmd)
    {
        // Insert laeuft immer in einer Section — Section ist Pflicht,
        // start ist der Offset ab ihrem Anfang. Der Clip darf ueber das
        // Section-Ende laufen; hinter der letzten Section waechst eine
        // Auto-Section mit (autoSection).
        auto trackIndex = (int) (double) cmd.getProperty ("track", 0.0);
        auto file = juce::File (cmd.getProperty ("file", {}).toString());
        auto sectionName = cmd.getProperty ("section", {}).toString();

        if (sectionName.isEmpty())
            return resultObj (false, { { "error", "no section" } });

        auto* section = findSection (*ctx.edit, sectionName);

        if (section == nullptr)
            return resultObj (false, { { "error", "no such section: " + sectionName } });

        auto offset = (double) cmd.getProperty ("start", 0.0);

        if (offset < 0.0)
            return resultObj (false, { { "error", "start must be >= 0" } });

        auto start = section->getPosition().getStart().inSeconds() + offset;
        double length = 0.0;

        // MIDI-Dateien (.mid/.midi) landen als MidiClip — sonst WaveClip
        bool isMidi = file.hasFileExtension (".mid;.midi");

        // Vor den Spur-Artefakten pruefen — ein Fehler ohne Seiteneffekte
        if (! file.existsAsFile())
            return resultObj (false, { { "error", isMidi ? "invalid midi file" : "invalid audio file" } });

        // Der Zieltrack: track-Param (Index), -1 = eine neue Spur je Typ
        // erzeugen (MIDI bleibt unverdrahtet, bis sie geroutet wird)
        auto tracks = userTracks (*ctx.edit);
        bool newTrack = trackIndex < 0;

        if (newTrack)
            trackIndex = (int) tracks.size();

        if (trackIndex < 0 || trackIndex > tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* track = getOrInsertAudioTrackAt (*ctx.edit, trackIndex);
        te::AudioTrack* wantedLane = nullptr;

        // Ziel ist eine Lane? Der Clip geht dorthin, die Gruppe ist der Zieltrack
        if (auto* dest = laneGroupOf (*track))
        {
            wantedLane = track;
            track = dest;
        }

        // MIDI-Datei macht die Spur zur MIDI-Spur (Kanten, die nicht mehr
        // passen, fallen weg — Main braucht dann ein Instrument)
        if (isMidi && ! isMidiTrack (*track))
            setTrackType (*ctx.edit, *track, true);

        // Wie addMidiClip (Decision 2026-10-29-ueberlappung-fehler): in die
        // gewaehlte bzw. spielende Lane, wenn dort Platz ist; eine neue Lane
        // fuer den ersten Clip, einen anderen Typ oder mit newLane: true
        // (ein Take — die UI uebergibt es beim Insert). Ueberlappung =
        // Fehler, ohne Clip. Die Laenge kennt erst das Einfuegen: der Clip
        // geht rein, bei Ueberlappung wieder raus.
        auto* laneTrack = wantedLane != nullptr ? wantedLane : activeLaneOf (*ctx.edit, *track);
        juce::String newLaneReason;

        if (laneTrack == nullptr)
            newLaneReason = "first clip";
        else if ((bool) cmd.getProperty ("newLane", false))
            newLaneReason = "newLane: true";
        else if (isMidiTrack (*laneTrack) != isMidi)
            newLaneReason = isMidi ? "MIDI file, the lane holds audio" : "audio file, the lane holds MIDI";

        const auto muted = lanesMutedBy (*ctx.edit, *track, newLaneReason.isEmpty() ? laneTrack : nullptr);

        if (newLaneReason.isNotEmpty())
            laneTrack = makeLaneTrack (*ctx.edit, *track);

        juce::Array<te::Clip*> before;

        for (auto* c : laneTrack->getClips())
            before.add (c);

        int laneIndex = 0;

        for (auto* t : userTracks (*ctx.edit))
        {
            if (t == laneTrack)
                break;

            ++laneIndex;
        }

        auto error = isMidi
                     ? insertMidiClip (ctx, laneIndex, file, start, &length)
                     : insertAudioClip (ctx, laneIndex, file, start, &length);

        if (error.isNotEmpty())
            return resultObj (false, { { "error", error } });

        te::Clip* added = nullptr;

        for (auto* c : laneTrack->getClips())
            if (! before.contains (c))
                added = c;

        if (newLaneReason.isEmpty())
        {
            bool clash = false;

            for (auto* c : before)
                if (c->getPosition().getStart().inSeconds() < start + length - 1.0e-6
                    && c->getPosition().getEnd().inSeconds() > start + 1.0e-6)
                    clash = true;

            if (clash)
            {
                if (added != nullptr)
                    added->removeFromParent();

                return resultObj (false, { { "error", overlapError (*laneTrack, start, start + length) } });
            }

            // Zweiter Clip derselben Datei auf der Lane: eindeutiger Name ("hit 2")
            if (added != nullptr)
                for (auto* c : before)
                    if (c->getName() == added->getName())
                    {
                        added->setName (juce::String());
                        added->setName (uniqueClipName (*laneTrack, c->getName()));
                        break;
                    }
        }

        // Nur eine Lane spielt — die Ziel-Lane aktiviert die Gruppe, die
        // Geschwister werden gemutet (die Antwort nennt sie)
        activateLane (*ctx.edit, *laneTrack);
        forgetLaneListening (ctx, *laneTrack);

        auto newSection = autoSection (*ctx.edit);

        // Aktion am Node (kind collect): Take-Typ, Section, Zieltrack (fuer
        // den + Button) und die eingefuegte Datei zum Anhoeren.
        auto params = new juce::DynamicObject();
        params->setProperty ("type", isMidi ? "midi" : "audio");
        params->setProperty ("section", sectionName);
        params->setProperty ("track", (int) userTracks (*ctx.edit).indexOf (track));
        recordNodeAction (ctx, "collect", "addClip", juce::var (params),
                          file.getFullPathName());
        recordAutoSection (ctx, newSection);

        auto res = resultObj (true, { { "track", laneIndex }, { "laneOf", (int) userTracks (*ctx.edit).indexOf (track) },
                                      { "section", sectionName }, { "name", added != nullptr ? added->getName() : juce::String() },
                                      { "lane", laneTrack->getName() }, { "newLane", newLaneReason.isNotEmpty() },
                                      { "newLaneReason", newLaneReason }, { "mutedLanes", juce::var (muted) },
                                      { "start", start }, { "length", length } });

        if (! muted.isEmpty())
            res.getDynamicObject()->setProperty ("warning", lanesMutedWarning (laneTrack->getName(), newLaneReason, muted));

        if (! newSection.isVoid())
            res.getDynamicObject()->setProperty ("autoSection", newSection);

        return res;
    }

    juce::var opAddMidiClip (DawdContext& ctx, const juce::var& cmd)
    {
        // MIDI-Clip aus Noten — der Weg ohne Keyboard und ohne Datei.
        // notes: [{pitch, start, length, velocity?}] in Sekunden oder [{pitch,
        // beat, beats, velocity?}] in Beats, jeweils relativ zum Clip-Start;
        // die Clip-Lage per start/length (Sekunden) oder startBeat/lengthBeats.
        // Der Clip startet wie ein Insert in einer Section (und darf ueber ihr
        // Ende laufen) und wird eine Lane des Zieltracks.
        auto trackIndex = (int) (double) cmd.getProperty ("track", 0.0);
        auto sectionName = cmd.getProperty ("section", {}).toString();

        if (sectionName.isEmpty())
            return resultObj (false, { { "error", "no section" } });

        auto* section = findSection (*ctx.edit, sectionName);

        if (section == nullptr)
            return resultObj (false, { { "error", "no such section: " + sectionName } });

        if (cmd.hasProperty ("start") && cmd.hasProperty ("startBeat"))
            return resultObj (false, { { "error", "use start (seconds) or startBeat, not both" } });

        if (cmd.hasProperty ("length") && cmd.hasProperty ("lengthBeats"))
            return resultObj (false, { { "error", "use length (seconds) or lengthBeats, not both" } });

        const auto sectionStart = section->getPosition().getStart().inSeconds();
        auto offset = cmd.hasProperty ("startBeat")
                          ? ((double) cmd.getProperty ("startBeat", 0.0) < 0.0 ? -1.0
                                 : beatsToSeconds (*ctx.edit, sectionStart, (double) cmd.getProperty ("startBeat", 0.0)))
                          : (double) cmd.getProperty ("start", 0.0);

        if (offset < 0.0)
            return resultObj (false, { { "error", cmd.hasProperty ("startBeat")
                                                      ? "start must be >= 0 (startBeat " + cmd.getProperty ("startBeat", {}).toString()
                                                            + " counts from the start of section '" + sectionName + "')"
                                                      : "start must be >= 0 (start " + cmd.getProperty ("start", {}).toString()
                                                            + " counts in seconds from the start of section '" + sectionName + "')" } });

        auto start = sectionStart + offset;

        // startBeat/start zaehlen ab dem Start der Section — landet der Clip
        // hinter ihrem Ende, war fast immer ein Songbeat gemeint (Praxistest
        // 3: {section: "Break", startBeat: 64} landete 64 Beats hinter Break)
        juce::String placeWarning;

        if (start >= section->getPosition().getEnd().inSeconds() - 1.0e-6)
        {
            const auto secBeats = secondsToBeats (*ctx.edit, sectionStart, section->getPosition().getLength().inSeconds());
            placeWarning = (cmd.hasProperty ("startBeat") ? "startBeat " + cmd.getProperty ("startBeat", {}).toString()
                                                          : "start " + cmd.getProperty ("start", {}).toString() + " s")
                         + " lies past the end of section '" + sectionName + "' (" + juce::String (std::round (secBeats * 1000.0) / 1000.0)
                         + juce::String::fromUTF8 (" beats long) — it counts from the section start, so the clip starts at ")
                         + juce::String (start, 2) + " s"
                         + (sectionAt (*ctx.edit, start).isNotEmpty() ? " in section '" + sectionAt (*ctx.edit, start) + "'" : juce::String());
        }

        // Vor den Spur-Artefakten pruefen — ein Fehler ohne Seiteneffekte
        auto notesVar = cmd.getProperty ("notes", {});

        if (! notesVar.isArray() || notesVar.getArray()->isEmpty())
            return resultObj (false, { { "error", "no notes" } });

        juce::MidiMessageSequence seq;
        double notesEnd = 0.0;
        bool notesInBeats = false;

        if (auto err = parseNotes (*ctx.edit, notesVar, start, seq, notesEnd, &notesInBeats); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // Clip in Beats, Noten in Sekunden: fast immer ein Versehen (Praxistest
        // 1b — die Noten lagen im 1-s-Raster statt auf den Beats)
        if ((cmd.hasProperty ("startBeat") || cmd.hasProperty ("lengthBeats")) && ! notesInBeats)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("clip is placed in beats (startBeat/lengthBeats) but notes use start/length (seconds) — give notes as {pitch, beat, beats}, or place the clip with start/length") } });

        // Laenge: der Param schlaegt das Notenende (Clip darf laenger sein)
        auto length = cmd.hasProperty ("lengthBeats")
                          ? ((double) cmd.getProperty ("lengthBeats", 0.0) <= 0.0 ? 0.0
                                 : beatsToSeconds (*ctx.edit, start, (double) cmd.getProperty ("lengthBeats", 0.0)))
                          : (double) cmd.getProperty ("length", notesEnd);

        if (length <= 0.0)
            return resultObj (false, { { "error", "length must be > 0" } });

        // Wie addClip: Zieltrack aufloesen, der Clip wird eine Lane des
        // Zieltracks, die Spur wird MIDI-Spur
        auto tracks = userTracks (*ctx.edit);
        bool newTrack = trackIndex < 0;

        if (newTrack)
            trackIndex = (int) tracks.size();

        if (trackIndex < 0 || trackIndex > tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* track = getOrInsertAudioTrackAt (*ctx.edit, trackIndex);
        te::AudioTrack* wantedLane = nullptr;

        if (auto* dest = laneGroupOf (*track))
        {
            wantedLane = track;
            track = dest;
        }

        if (! isMidiTrack (*track))
            setTrackType (*ctx.edit, *track, true);

        // In die gewaehlte bzw. die spielende Lane, wenn dort Platz ist —
        // Patterns entstehen Clip fuer Clip auf einer Lane. Eine neue Lane
        // (die die bisherige stummschaltet) nur fuer den ersten Clip oder mit
        // newLane: true; Ueberlappung ist ein Fehler (Decision
        // 2026-10-29-ueberlappung-fehler, Praxistest 3: die Basslinie war still)
        auto* laneTrack = wantedLane != nullptr ? wantedLane : activeLaneOf (*ctx.edit, *track);
        const auto clipEnd = start + length;
        const bool forceNew = (bool) cmd.getProperty ("newLane", false);
        juce::String newLaneReason;

        if (laneTrack == nullptr)
            newLaneReason = "first clip";
        else if (forceNew)
            newLaneReason = "newLane: true";
        else if (! rangeIsFree (*laneTrack, start, clipEnd))
            return resultObj (false, { { "error", overlapError (*laneTrack, start, clipEnd) } });

        const auto muted = lanesMutedBy (*ctx.edit, *track, newLaneReason.isEmpty() ? laneTrack : nullptr);

        if (newLaneReason.isNotEmpty())
            laneTrack = makeLaneTrack (*ctx.edit, *track);

        activateLane (*ctx.edit, *laneTrack);

        forgetLaneListening (ctx, *laneTrack);

        int laneIndex = 0;

        for (auto* t : userTracks (*ctx.edit))
        {
            if (t == laneTrack)
                break;

            ++laneIndex;
        }

        int notes = 0;

        // Name frei waehlbar ("Bass A"), sonst "Notes" — eindeutig auf der Lane
        auto clipName = cmd.getProperty ("name", {}).toString().trim();
        if (clipName.isEmpty())
            clipName = "Notes";

        if (findClipByName (*laneTrack, clipName) != nullptr)
            clipName = uniqueClipName (*laneTrack, clipName);

        auto error = insertMidiSequence (ctx, laneIndex, clipName, seq, start, length,
                                         &length, &notes);

        if (error.isNotEmpty())
            return resultObj (false, { { "error", error } });

        auto newSection = autoSection (*ctx.edit);

        auto params = new juce::DynamicObject();
        params->setProperty ("type", "midi");
        params->setProperty ("section", sectionName);
        params->setProperty ("track", (int) userTracks (*ctx.edit).indexOf (track));
        recordNodeAction (ctx, "collect", "addMidiClip", juce::var (params));
        recordAutoSection (ctx, newSection);

        auto round3 = [] (double v) { return std::round (v * 1000.0) / 1000.0; };
        auto mono = monoWarning (*ctx.edit, *track, seq);

        // Eine neue Lane schaltet die bisher spielende stumm — deutlich sagen
        // (Praxistest 1d: die Drum-Lane war still, ohne dass es auffiel)
        if (! muted.isEmpty())
            mono = (mono.isNotEmpty() ? mono + "; " : juce::String())
                   + lanesMutedWarning (laneTrack->getName(), newLaneReason, muted);

        if (placeWarning.isNotEmpty())
            mono = (mono.isNotEmpty() ? mono + "; " : juce::String()) + placeWarning;
        auto res = resultObj (true, { { "track", laneIndex }, { "laneOf", trackIndex },
                                  { "section", sectionName }, { "name", clipName },
                                  { "lane", laneTrack->getName() }, { "newLane", newLaneReason.isNotEmpty() },
                                  { "newLaneReason", newLaneReason }, { "mutedLanes", juce::var (muted) },
                                  { "notesIn", notesInBeats ? "beats" : "seconds" },
                                  { "start", start }, { "length", length },
                                  { "startBeat", round3 (secondsToBeats (*ctx.edit, sectionStart, start - sectionStart)) },
                                  { "lengthBeats", round3 (secondsToBeats (*ctx.edit, start, length)) },
                                  { "notes", notes } });

        if (mono.isNotEmpty())
            res.getDynamicObject()->setProperty ("warning", mono);

        if (! newSection.isVoid())
            res.getDynamicObject()->setProperty ("autoSection", newSection);

        return res;
    }

    namespace
    {
        // Multi-Track-Record: je Zieltrack eine Lane, alle zugewiesenen
        // Geraete laufen in ihre Lanes — der erste Ziellane ersetzt die
        // Zuweisung (move), weitere kommen dazu (ein Geraet kann mehrere
        // Spuren speisen). Jede Lane aktiviert ihre Gruppe; Start-Position
        // und Start gelten fuer alle gleichzeitig. Ein ArmedTake-Eintrag
        // je Lane — stopRecordingCollect sammelt daraus alle Takes ein.
        juce::var startMultiRecord (DawdContext& ctx, te::MarkerClip* section, const juce::String& sectionName,
                                    std::vector<std::pair<te::InputDevice*, std::vector<te::AudioTrack*>>>& assigned)
        {
            auto& transport = ctx.edit->getTransport();
            ctx.armedTakes.clear();

            for (auto& [dev, groups] : assigned)
            {
                auto midi = dev->isMidi();
                dev->setEnabled (true);

                if (midi)
                    if (auto* mi = dynamic_cast<te::MidiInputDevice*> (dev))
                        mi->mergeRecordings = false;

                for (size_t j = 0; j < groups.size(); ++j)
                {
                    auto* group = groups[j];
                    auto* laneTrack = makeLaneTrack (*ctx.edit, *group);

                    // updateOutput startet den Playback-Rebuild async — den
                    // synchron abschliessen und die Instanz im frischen
                    // Kontext holen (alter Kontext wuerde doppelt erfassen)
                    juce::Timer::callPendingTimersSynchronously();
                    auto* instance = inputInstanceFor (ctx, dev);

                    if (instance == nullptr)
                        return resultObj (false, { { "error", "no input instance" } });

                    // Erster Ziellane ersetzt die Zuweisung, weitere kommen
                    // dazu — ein Geraet kann mehrere Spuren speisen
                    if (auto res = instance->setTarget (laneTrack->itemID, j == 0, nullptr, 0); ! res)
                        return resultObj (false, { { "error", res.error() } });

                    instance->setRecordingEnabled (laneTrack->itemID, true);
                    activateLane (*ctx.edit, *laneTrack);
                    forgetLaneListening (ctx, *laneTrack);

                    ctx.armedTakes.push_back ({ (int) userTracks (*ctx.edit).size() - 1,
                                                (int) userTracks (*ctx.edit).indexOf (group),
                                                midi });
                }
            }

            ctx.engine->getDeviceManager().dispatchPendingUpdates();

            // Kein Punch (seit 2026-10-24): die Aufnahme startet am
            // Section-Anfang und laeuft bis zum Stopp, auch ueber das
            // Section-Ende. Der Transport ist gestoppt, darum greift die
            // Position sauber.
            ctx.edit->recordingPunchInOut = false;

            if (section != nullptr)
                transport.setPosition (section->getPosition().getStart());

            transport.record (false, false);

            if (! transport.isRecording())
                return resultObj (false, { { "error", "cannot start recording" } });

            if (ctx.loudness != nullptr)
                ctx.loudness->restart();   // Integrated seit Aufnahme-Start

            ctx.monitorOn = true;
            ctx.pendingTakeSection = sectionName;

            juce::Array<juce::var> takeList;

            for (auto& t : ctx.armedTakes)
                takeList.add (resultObj (true, {
                    { "track", t.laneIndex }, { "laneOf", t.targetIndex },
                    { "input", inputDisplayName (ctx, *assigned[0].first) },
                    { "type", t.midi ? "midi" : "audio" } }));

            if (section != nullptr)
                return resultObj (true, { { "track", ctx.armedTakes[0].laneIndex },
                                          { "laneOf", ctx.armedTakes[0].targetIndex },
                                          { "input", inputDisplayName (ctx, *assigned[0].first) },
                                          { "type", ctx.armedTakes[0].midi ? "midi" : "audio" },
                                          { "section", sectionName },
                                          { "takes", takeList } });

            return resultObj (true, { { "track", ctx.armedTakes[0].laneIndex },
                                      { "laneOf", ctx.armedTakes[0].targetIndex },
                                      { "input", inputDisplayName (ctx, *assigned[0].first) },
                                      { "type", ctx.armedTakes[0].midi ? "midi" : "audio" },
                                      { "takes", takeList } });
        }
    }

    juce::var opRecord (DawdContext& ctx, const juce::var& cmd)
    {
        auto& transport = ctx.edit->getTransport();
        auto on = cmd.getProperty ("on", {});
        auto start = on.isVoid() ? ! transport.isRecording() : (bool) on;

        // Spuren sind Audio- oder MIDI-Spuren. Der Typ gilt nur fuer den
        // Fallback, wenn keine Spur einen Input zugewiesen hat
        // (assignInput) — eine neue Spur wird dann MIDI- oder Audio-Spur.
        auto type = cmd.getProperty ("type", "audio").toString();
        auto midi = type == "midi";

        if (type != "audio" && type != "midi")
            return resultObj (false, { { "error", "type must be audio or midi" } });

        // Sektion ist Pflicht: die Aufnahme startet an ihrem Anfang und
        // laeuft bis zum Stopp (auch ueber ihr Ende; Auto-Section beim Stopp).
        te::MarkerClip* section = nullptr;
        auto sectionName = cmd.getProperty ("section", {}).toString();

        if (start && sectionName.isEmpty())
            return resultObj (false, { { "error", "no section" } });

        if (sectionName.isNotEmpty())
        {
            section = findSection (*ctx.edit, sectionName);

            if (section == nullptr)
                return resultObj (false, { { "error", "no such section: " + sectionName } });
        }

        if (start)
        {
            if (transport.isRecording())
                return resultObj (false, { { "error", "already recording" } });

            // Rollender Transport? Stoppen — die Aufnahme startet sauber
            // vom Section-Anfang. Mitten im Lauf blieben Takes leer bzw.
            // starteten am Playhead-Stand (der Playback-Kontext enthaelt
            // die neue Lane nicht frisch).
            if (transport.isPlaying())
                transport.stop (false, false);

            // Zugewiesene Inputs (assignInput) einsammeln — sie bestimmen
            // die geaermten Spuren (Multi-Track-Record). Ohne Zuweisung
            // gilt der Fallback: type/track-Param und die Default-Geraete.
            ctx.edit->getTransport().ensureContextAllocated();
            auto context = ctx.edit->getTransport().getCurrentPlaybackContext();
            auto tracks = userTracks (*ctx.edit);

            std::vector<std::pair<te::InputDevice*, std::vector<te::AudioTrack*>>> assigned;

            for (auto in : context->getAllInputs())
            {
                std::vector<te::AudioTrack*> groups;

                for (auto id : in->getTargets())
                    for (auto* t : tracks)
                        if (t->itemID == id)
                        {
                            auto* group = t;

                            if (auto* dest = laneGroupOf (*t))
                                group = dest;

                            if (std::find (groups.begin(), groups.end(), group) == groups.end())
                                groups.push_back (group);

                            break;
                        }

                if (! groups.empty())
                    assigned.push_back ({ &in->getInputDevice(), std::move (groups) });
            }

            if (! assigned.empty())
                return startMultiRecord (ctx, section, sectionName, assigned);

            auto* instance = armInputInstance (ctx, midi);

            if (instance == nullptr)
                return resultObj (false, { { "error", midi ? "no midi input device"
                                                           : "no audio input device" } });

            // Takes bleiben einzelne Clips (nie in den bestehenden Clip mergen) —
            // jede Aufnahme in einen belegten Bereich wird eine eigene Lane.
            if (midi)
                if (auto* mi = dynamic_cast<te::MidiInputDevice*> (&instance->getInputDevice()))
                    mi->mergeRecordings = false;

            // track uebergeben (Track-Klick im UI): die Aufnahme geht in diesen
            // Track. Sonst das bestehende Ziel der Instanz nehmen (Monitor hat
            // die Spur geaermt), sonst die erste passende Spur: Audio -> erste
            // Spur, MIDI -> erste MIDI-Spur, sonst eine neue Spur.
            int trackIndex = -1;

            if (cmd.hasProperty ("track"))
                trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
            else
            {
                for (int i = 0; i < tracks.size(); ++i)
                    if (instance->getTargets().contains (tracks[i]->itemID))
                        { trackIndex = i; break; }

                if (trackIndex < 0 && ! tracks.isEmpty())
                {
                    if (! midi)
                        trackIndex = 0;
                    else
                        for (int i = 0; i < tracks.size(); ++i)
                            if (isMidiTrack (*tracks[i]))
                                { trackIndex = i; break; }
                }
            }

            bool newTrack = trackIndex < 0;

            if (newTrack)
                trackIndex = (int) tracks.size();

            if (trackIndex < 0 || trackIndex > tracks.size())
                return resultObj (false, { { "error", "no such track" } });

            auto* track = getOrInsertAudioTrackAt (*ctx.edit, trackIndex);

            // Neue Spur durch die Aufnahme: Typ kommt aus der Aktionswahl
            // (eine neue MIDI-Spur ist unverdrahtet, bis sie geroutet wird)
            if (midi && newTrack)
                setTrackType (*ctx.edit, *track, true);

            // Ziel ist eine Lane? Die Aufnahme geht in die Gruppe (den
            // Zieltrack der Lane), nicht in eine Verschachtelung.
            if (auto* dest = laneGroupOf (*track))
                track = dest;

            int targetIndex = trackIndex;

            // Jede Aufnahme wird eine eigene Lane: eigener Track, dessen
            // Output in den Zieltrack laeuft — sie klingt durch dessen
            // Processing (Plugins, Volume), das spaeter immer gilt. Die
            // Track-Gruppenspur bleibt leerer Bus (ihr Inhalt — z. B.
            // Seed-Audio — spielt immer weiter), die Takes leben in den Lanes.
            {
                auto* laneTrack = makeLaneTrack (*ctx.edit, *track);

                // updateOutput startet den Playback-Rebuild async — den
                // synchron abschliessen und die Instanz des neuen Kontexts
                // holen, sonst erfasst der alte Kontext die Aufnahme doppelt
                juce::Timer::callPendingTimersSynchronously();
                instance = armInputInstance (ctx, midi);

                if (instance == nullptr)
                    return resultObj (false, { { "error", "no input device" } });

                track = laneTrack;
                trackIndex = (int) userTracks (*ctx.edit).size() - 1;
            }

            // Nur eine Lane spielt — die neuen Lane aktiviert die Gruppe,
            // die Geschwister werden gemutet
            activateLane (*ctx.edit, *track);
            forgetLaneListening (ctx, *track);

            if (auto res = instance->setTarget (track->itemID, true, nullptr, 0); ! res)
                return resultObj (false, { { "error", res.error() } });

            // Kein Punch (seit 2026-10-24): Start am Section-Anfang, die
            // Aufnahme laeuft bis zum Stopp. Der Transport ist gestoppt
            // (oben), darum greift die Position sauber.
            ctx.edit->recordingPunchInOut = false;

            if (section != nullptr)
                transport.setPosition (section->getPosition().getStart());

            instance->setRecordingEnabled (track->itemID, true);
            transport.record (false, false);

            if (! transport.isRecording())
                return resultObj (false, { { "error", "cannot start recording" } });

            if (ctx.loudness != nullptr)
                ctx.loudness->restart();   // Integrated seit Aufnahme-Start

            ctx.armedTakes = { { trackIndex, targetIndex, midi } };
            ctx.pendingTakeSection = sectionName;
            ctx.monitorOn = true;

            if (section != nullptr)
                return resultObj (true, { { "track", trackIndex },
                                          { "laneOf", targetIndex },
                                          { "input", inputDisplayName (ctx, instance->getInputDevice()) },
                                          { "type", midi ? "midi" : "audio" },
                                          { "section", sectionName } });

            return resultObj (true, { { "track", trackIndex },
                                      { "laneOf", targetIndex },
                                      { "input", inputDisplayName (ctx, instance->getInputDevice()) },
                                      { "type", midi ? "midi" : "audio" } });
        }

        if (! transport.isRecording())
        {
            // Diagnose (todos/2026-10-24-lane-active-wackelt.md): endete eine
            // Aufnahme ohne record off, steht hier, wo der Transport steht
            if (! ctx.armedTakes.empty())
                std::cerr << "[record] stop: not recording, " << ctx.armedTakes.size()
                          << " armed take(s), position " << transport.getPosition().inSeconds()
                          << " s, playing " << (int) transport.isPlaying() << "\n";

            return resultObj (false, { { "error", "not recording" } });
        }

        // Der Play-Start landet als playPending erst im naechsten Audio-Block
        // am Playhead. Stoppt man vorher, startet der nachgelagerte Block den
        // Transport trotzdem — dann laeuft er ohne Aufnahme weiter.
        if (auto* context = transport.getCurrentPlaybackContext())
            for (int i = 0; i < 50 && context->isPlayPending(); ++i)
                juce::Thread::sleep (10);

        return stopRecordingCollect (ctx);
    }

    juce::var opArmTrack (DawdContext& ctx, const juce::var& cmd)
    {
        // Arm-Schalter je Spur: geaermt = ein Eingang ist der Spur
        // zugewiesen — record nimmt alle geaermten Spuren gleichzeitig auf
        // (Audio und MIDI parallel). on: das gegebene Geraet, sonst die
        // bestehende Zuweisung, sonst das Default-Geraet des Spurtyps; ein
        // neues Geraet ersetzt das bisherige. off: Zuweisung aufheben.
        // Eine Lane armt ihre Gruppe. Keine Node-Aktion (wie assignInput).
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto on = (bool) cmd.getProperty ("on", true);
        auto inputName = cmd.getProperty ("input", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* track = tracks[trackIndex];

        if (auto* dest = laneGroupOf (*track))
        {
            track = dest;
            trackIndex = tracks.indexOf (track);
        }

        bool midiTrack = isMidiTrack (*track);

        auto& dm = ctx.engine->getDeviceManager();
        ctx.edit->getTransport().ensureContextAllocated();
        auto context = ctx.edit->getTransport().getCurrentPlaybackContext();

        // Bestehende Zuweisungen der Spur
        juce::StringArray current;

        for (auto in : context->getAllInputs())
            if (in->getTargets().contains (track->itemID))
                current.add (inputDisplayName (ctx, in->getInputDevice()));

        auto clear = [&]
        {
            for (auto in : context->getAllInputs())
                if (in->getTargets().contains (track->itemID))
                    [[maybe_unused]] auto res = in->removeTarget (track->itemID, nullptr);
        };

        if (! on)
        {
            clear();
            saveCurrentEdit (ctx);
            return resultObj (true, { { "track", trackIndex }, { "armed", false } });
        }

        if (inputName.isEmpty() && ! current.isEmpty())
            return resultObj (true, { { "track", trackIndex }, { "armed", true },
                                      { "input", current[0] },
                                      { "type", midiTrack ? "midi" : "audio" } });

        // Geraet aufloesen: Name oder Default des Spurtyps
        te::InputDevice* dev = nullptr;

        if (inputName.isNotEmpty())
        {
            dev = findInputByName (ctx, inputName);

            if (dev == nullptr)
                return resultObj (false, { { "error", "no such input: " + inputName } });
        }
        else if (auto* inst = armInputInstance (ctx, midiTrack))
            dev = &inst->getInputDevice();

        if (dev == nullptr)
            return resultObj (false, { { "error", midiTrack ? "no midi input device"
                                                            : "no audio input device" } });

        dev->setEnabled (true);
        dm.dispatchPendingUpdates();
        clear();

        auto* instance = inputInstanceFor (ctx, dev);

        if (instance == nullptr)
            return resultObj (false, { { "error", "no input instance" } });

        if (auto res = instance->setTarget (track->itemID, false, nullptr, 0); ! res)
            return resultObj (false, { { "error", res.error() } });

        // Laeuft das Monitoring, hoert die frisch geaermte Spur gleich mit
        if (ctx.monitorOn)
            dev->setMonitorMode (te::InputDevice::MonitorMode::on);

        saveCurrentEdit (ctx);
        return resultObj (true, { { "track", trackIndex }, { "armed", true },
                                  { "input", inputDisplayName (ctx, *dev) },
                                  { "type", dev->isMidi() ? "midi" : "audio" } });
    }

    juce::var opMonitor (DawdContext& ctx, const juce::var& cmd)
    {
        auto on = (bool) cmd.getProperty ("on", false);

        if (! on)
        {
            // Monitoring aus: Aufnahme stoppen falls aktiv, Record-Enabled
            // raeumen, MonitorMode auf automatic. Ziele bleiben stehen —
            // monitor on findet sie wieder (sonst neue Spur pro Toggle).
            auto& transport = ctx.edit->getTransport();

            if (transport.isRecording())
            {
                transport.stop (false, true);
                ctx.armedTakes.clear();
                ctx.pendingTakeSection = {};
            }

            auto& dm = ctx.engine->getDeviceManager();

            for (auto* wi : dm.getWaveInputDevices())
                if (wi->getMonitorMode() == te::InputDevice::MonitorMode::on)
                    wi->setMonitorMode (te::InputDevice::MonitorMode::automatic);

            for (auto mi : dm.getMidiInDevices())
                if (mi->getMonitorMode() == te::InputDevice::MonitorMode::on)
                    mi->setMonitorMode (te::InputDevice::MonitorMode::automatic);

            disarmRecording (ctx);
            ctx.monitorOn = false;
            return resultObj (true, { { "monitor", false } });
        }

        // armed: alle geaermten Spuren hoeren (beide Typen) — der Weg des
        // einen Monitor-Buttons; die Zuweisungen bleiben, wie sie sind
        if ((bool) cmd.getProperty ("armed", false))
        {
            ctx.edit->getTransport().ensureContextAllocated();
            auto context = ctx.edit->getTransport().getCurrentPlaybackContext();
            juce::StringArray devices;

            for (auto in : context->getAllInputs())
                if (! in->getTargets().isEmpty())
                {
                    in->getInputDevice().setMonitorMode (te::InputDevice::MonitorMode::on);
                    devices.addIfNotAlreadyThere (inputDisplayName (ctx, in->getInputDevice()));
                }

            if (devices.isEmpty())
                return resultObj (false, { { "error", "no armed track" } });

            juce::Array<juce::var> list;

            for (auto& d : devices)
                list.add (d);

            ctx.monitorOn = true;
            return resultObj (true, { { "monitor", true }, { "inputs", list } });
        }

        auto type = cmd.getProperty ("type", "midi").toString();
        auto midi = type == "midi";

        if (type != "audio" && type != "midi")
            return resultObj (false, { { "error", "type must be audio or midi" } });

        auto* instance = armInputInstance (ctx, midi);

        if (instance == nullptr)
            return resultObj (false, { { "error", midi ? "no midi input device"
                                                       : "no audio input device" } });

        auto tracks = userTracks (*ctx.edit);

        // Bestehendes Ziel der Instanz nehmen (Monitor/Record setzten es),
        // sonst eine neue Spur erzeugen (MIDI unverdrahtet, bis sie geroutet wird).
        int trackIndex = -1;

        for (int i = 0; i < tracks.size(); ++i)
            if (instance->getTargets().contains (tracks[i]->itemID))
                { trackIndex = i; break; }

        if (trackIndex < 0)
            trackIndex = (int) tracks.size();

        if (trackIndex < 0 || trackIndex > tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* track = getOrInsertAudioTrackAt (*ctx.edit, trackIndex);

        if (midi && trackIndex == (int) tracks.size())
            setTrackType (*ctx.edit, *track, true);

        if (auto res = instance->setTarget (track->itemID, true, nullptr, 0); ! res)
            return resultObj (false, { { "error", res.error() } });

        instance->getInputDevice().setMonitorMode (te::InputDevice::MonitorMode::on);

        ctx.monitorOn = true;
        saveCurrentEdit (ctx);
        return resultObj (true, { { "monitor", true }, { "track", trackIndex },
                                  { "input", inputDisplayName (ctx, instance->getInputDevice()) } });
    }

    juce::var opInputGain (DawdContext& ctx, const juce::var& cmd)
    {
        // Input-Gain des Wave-Inputs in dB — Pegel vor Monitor und Aufnahme.
        auto db = (double) cmd.getProperty ("db", 0.0);

        if (auto* dev = ctx.engine->getDeviceManager().getDefaultWaveInDevice())
            dev->setInputGainDb ((float) db);

        return resultObj (true, { { "inputGain", db } });
    }

    juce::var opListInputs (DawdContext& ctx, const juce::var&)
    {
        // Verfuegbare Eingaenge (wave + midi) und ihre Spur-Zuweisungen —
        // die Collect-Auswahl dafuer (assignInput, Multi-Track-Record).
        return resultObj (true, { { "inputs", enumerateInputs (ctx) } });
    }

    juce::var opAssignInput (DawdContext& ctx, const juce::var& cmd)
    {
        // Eingangsgeraet (Name aus listInputs) einer Spur zuweisen — die
        // Aufnahme nimmt dann alle Spuren mit Zuweisung gleichzeitig
        // (Multi-Track-Record). Ein Geraet kann mehrere Spuren speisen.
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto inputName = cmd.getProperty ("input", {}).toString();

        if (inputName.isEmpty())
            return resultObj (false, { { "error", "no input name" } });

        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto& dm = ctx.engine->getDeviceManager();
        te::InputDevice* dev = nullptr;

        dev = findInputByName (ctx, inputName);

        if (dev == nullptr)
            return resultObj (false, { { "error", "no such input: " + inputName } });

        dev->setEnabled (true);
        dm.dispatchPendingUpdates();

        auto* instance = inputInstanceFor (ctx, dev);

        if (instance == nullptr)
            return resultObj (false, { { "error", "no input instance" } });

        // move=false: ergaenzen, ein Geraet behaelt seine anderen Ziele
        if (auto res = instance->setTarget (tracks[trackIndex]->itemID, false, nullptr, 0); ! res)
            return resultObj (false, { { "error", res.error() } });

        saveCurrentEdit (ctx);
        return resultObj (true, { { "track", trackIndex }, { "input", inputName },
                                  { "type", dev->isMidi() ? "midi" : "audio" } });
    }

    juce::var opClearInput (DawdContext& ctx, const juce::var& cmd)
    {
        // Alle Zuweisungen einer Spur aufheben — sie wird von der Aufnahme
        // nicht mehr geaermt.
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        ctx.edit->getTransport().ensureContextAllocated();
        auto context = ctx.edit->getTransport().getCurrentPlaybackContext();

        juce::Array<juce::var> cleared;

        for (auto in : context->getAllInputs())
            if (in->getTargets().contains (tracks[trackIndex]->itemID))
            {
                if (auto res = in->removeTarget (tracks[trackIndex]->itemID, nullptr); res.failed())
                    return resultObj (false, { { "error", res.getErrorMessage() } });

                cleared.add (inputDisplayName (ctx, in->getInputDevice()));
            }

        saveCurrentEdit (ctx);
        return resultObj (true, { { "track", trackIndex }, { "cleared", cleared } });
    }

    namespace
    {
        // Aktiver System-Eingang: dawd oeffnet ALSA "default" — welches Mikrofon
        // dahinter haengt (internes Mic, USB-Interface), weiss nur PipeWire.
        // wpctl-Beschreibung statt eigener PipeWire-Anbindung; fehlt wpctl,
        // bleibt der Name leer (die UI zeigt dann nur dawds Geraetenamen).
        juce::String querySystemSource()
        {
            juce::ChildProcess proc;

            if (! proc.start ({ "wpctl", "inspect", "@DEFAULT_AUDIO_SOURCE@" }))
                return {};

            auto output = proc.readAllProcessOutput();
            proc.waitForProcessToFinish (2000);

            // wpctl-Zeilen tragen Flucht-Punkte/Leerzeichen vor dem Schluessel —
            // contains statt startsWith; Prioritaet: Beschreibung, Nick, Name
            for (auto key : { "node.description", "node.nick", "node.name" })
                for (auto& line : juce::StringArray::fromLines (output))
                    if (line.contains (key))
                        return line.fromFirstOccurrenceOf ("=", false, false).trim().unquoted();

            return {};
        }
    }

    juce::var opInputLevel (DawdContext& ctx, const juce::var&)
    {
        // Live-Pegel des Eingangs fuer die Audio-Node-Anzeige (Anzeige-Poll,
        // nicht im Event-Log). Der LevelMeasurer wird vom Audio-Callback des
        // Wave-Inputs gespeist — auch ohne Monitoring.
        auto* device = ctx.engine->getDeviceManager().getDefaultWaveInDevice();

        if (device != nullptr && device != ctx.inputMeterDevice)
        {
            device->levelMeasurer.addClient (ctx.inputMeter);
            ctx.inputMeterDevice = device;
        }

        float level = -100.0f;

        if (device != nullptr)
            for (int chan = 0; chan < 2; ++chan)
                level = juce::jmax (level, ctx.inputMeter.getAndClearAudioLevel (chan).dB);

        auto now = juce::Time::currentTimeMillis();

        if (now - ctx.systemSourceTime > 3000)
        {
            ctx.systemSource = querySystemSource();
            ctx.systemSourceTime = now;
        }

        return resultObj (true, { { "device", device != nullptr ? device->getName() : juce::String() },
                                  { "system", ctx.systemSource },
                                  { "level", (double) level },
                                  { "monitoring", ctx.monitorOn },
                                  { "recording", isRecording (ctx) } });
    }

    juce::var opLastTake (DawdContext& ctx, const juce::var&)
    {
        for (auto it = ctx.recordedTakes.rbegin(); it != ctx.recordedTakes.rend(); ++it)
        {
            juce::Array<juce::var> files;
            int notes = 0;

            for (auto id : *it)
                for (auto* track : userTracks (*ctx.edit))
                    if (auto* clip = track->findClipForID (id))
                    {
                        if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
                        {
                            notes += mc->getSequence().getNumNotes();
                        }
                        else if (auto f = clip->getCurrentSourceFile(); f.existsAsFile())
                        {
                            files.add (f.getFullPathName());
                        }

                        break;
                    }

            if (! files.isEmpty() || notes > 0)
                return resultObj (true, { { "files", files }, { "notes", notes } });
        }

        return resultObj (false, { { "error", "no recorded take" } });
    }

    namespace
    {
        // Antwort um die Auto-Section ergaenzen (void = keine entstanden)
        juce::var withAutoSection (juce::var res, const juce::var& newSection)
        {
            if (! newSection.isVoid())
                res.getDynamicObject()->setProperty ("autoSection", newSection);

            return res;
        }
    }

    juce::var opDeleteClip (DawdContext& ctx, const juce::var& cmd)
    {
        // Clip loeschen am aktuellen Knoten — der Weg ohne treeReset (der
        // wirft alle Aktionen am Node weg). Adressierung: track + name aus
        // state.trackList.
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();

        if (name.isEmpty())
            return resultObj (false, { { "error", "no clip name" } });

        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        auto clipStart = clip->getPosition().getStart().inSeconds();
        auto clipState = clip->state.createCopy();
        auto trackId = tracks[trackIndex]->itemID;
        clip->removeFromParent();
        saveCurrentEdit (ctx);

        // Aktion am Node — die Take-Daten (Section, Track) leiten sich aus
        // den Params ab, das Plus bleibt auf der Section vorgewaehlt.
        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("section", sectionAt (*ctx.edit, clipStart));
        auto actionId = recordNodeAction (ctx, "process", "deleteClip", juce::var (params));

        // Fuer undoDelete: Clip-Zustand merken, die Aktions-Id ist das Token
        // (ein paar Schritte reichen — das UI bietet Undo kurz an)
        ctx.deletedClips.push_back ({ actionId, trackId, clipState });

        if (ctx.deletedClips.size() > 20)
            ctx.deletedClips.erase (ctx.deletedClips.begin());

        return resultObj (true, { { "track", trackIndex }, { "name", name }, { "undo", actionId } });
    }

    juce::var opUndoDelete (DawdContext& ctx, const juce::var& cmd)
    {
        // Geloeschten Clip zurueckholen: Token = Aktions-Id von deleteClip.
        // Gilt nur im geladenen Edit am aktuellen Node und solange die
        // Aktion noch dort steht — sonst gibt es nichts zurueckzunehmen.
        auto token = (int) (double) cmd.getProperty ("undo", 0.0);
        auto it = std::find_if (ctx.deletedClips.begin(), ctx.deletedClips.end(),
                                [token] (const DeletedClip& d) { return d.actionId == token; });

        if (token <= 0 || it == ctx.deletedClips.end())
            return resultObj (false, { { "error", "nothing to undo" } });

        te::AudioTrack* track = nullptr;
        int trackIndex = 0;

        for (auto* t : userTracks (*ctx.edit))
        {
            if (t->itemID == it->trackId) { track = t; break; }
            ++trackIndex;
        }

        if (track == nullptr)
            return resultObj (false, { { "error", "track is gone" } });

        if (! removeNodeAction (ctx, token))
            return resultObj (false, { { "error", "nothing to undo" } });

        auto name = it->state.getProperty (te::IDs::name).toString();
        track->state.addChild (it->state, -1, nullptr);
        ctx.deletedClips.erase (it);
        saveCurrentEdit (ctx);

        // Legte das Loeschen per Auto-Branch einen Node an und ist er jetzt
        // wieder leer, verschwindet er — zurueck zum Elternknoten
        auto r = resultObj (true, { { "track", trackIndex }, { "name", name } });

        if (ctx.tree)
            if (auto* n = ctx.tree->find (ctx.tree->current);
                n != nullptr && n->autoCreated && ! n->isActed() && ! n->favorite)
            {
                bool hasChildren = false;

                for (auto& other : ctx.tree->nodes)
                    hasChildren = hasChildren || other.parent == n->id;

                if (! hasChildren)
                {
                    auto removed = n->id;
                    auto del = new juce::DynamicObject();
                    del->setProperty ("node", removed);

                    if ((bool) opTreeDelete (ctx, juce::var (del)).getProperty ("ok", false))
                    {
                        forgetNode (ctx, removed);
                        r.getDynamicObject()->setProperty ("removedNode", removed);
                    }
                }
            }

        return r;
    }

    namespace
    {
        // Kopien aus duplicateClip teilen ein Pattern, aber nicht die Noten:
        // ein Edit am einen Clip laesst die anderen stehen (Praxistest 3) —
        // Hinweis mit den Namen der Geschwister, leer ohne Pattern
        juce::String patternHint (te::AudioTrack& track, te::MidiClip& clip, const juce::String& how)
        {
            auto pattern = clip.state.getProperty ("dawdPattern").toString();

            if (pattern.isEmpty())
                return {};

            juce::StringArray others;

            for (auto* c : track.getClips())
                if (c != &clip && c->state.getProperty ("dawdPattern").toString() == pattern)
                    others.add (c->getName());

            if (others.isEmpty())
                return {};

            return juce::String (others.size()) + " other clip(s) of pattern '" + pattern + "' keep their old notes ("
                   + others.joinIntoString (", ") + juce::String::fromUTF8 (") — copies are independent; ") + how;
        }
    }

    juce::var opSetMidiNotes (DawdContext& ctx, const juce::var& cmd)
    {
        // Noten eines MIDI-Clips ersetzen (Piano-Roll): notes wie bei
        // addMidiClip — Sekunden oder Beats relativ zum Clip-Start. Der Clip
        // waechst mit, wenn Noten ueber sein Ende reichen (nie ueber den
        // naechsten Clip der Lane hinaus, ueber Section-Grenzen schon —
        // hinter der letzten waechst eine Auto-Section), schrumpft aber
        // nicht. copies: true = alle Clips desselben
        // Patterns (Original und Kopien aus duplicateClip) auf der Spur.
        // Aufeinanderfolgende Aufrufe auf denselben Clip sind eine Aktion.
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* mc = dynamic_cast<te::MidiClip*> (findClipByName (*tracks[trackIndex], name));

        if (mc == nullptr)
            return resultObj (false, { { "error", "no such MIDI clip: " + name } });

        auto notesVar = cmd.getProperty ("notes", {});

        if (! notesVar.isArray())
            return resultObj (false, { { "error", "no notes" } });

        juce::Array<te::MidiClip*> targets { mc };
        if (auto e = copiesError (cmd); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        const bool copies = (bool) cmd.getProperty ("copies", false);

        if (copies)
        {
            auto pattern = mc->state.getProperty ("dawdPattern").toString();

            if (pattern.isEmpty())
                return resultObj (false, { { "error", "clip has no copies (copies: true needs a clip from duplicateClip or its original)" } });

            for (auto* c : tracks[trackIndex]->getClips())
                if (auto* other = dynamic_cast<te::MidiClip*> (c); other != nullptr && other != mc
                    && other->state.getProperty ("dawdPattern").toString() == pattern)
                    targets.add (other);
        }

        // Nur einen Bereich ersetzen (fromBeat/toBeat, Beats ab Clip-Anfang):
        // Noten, die dort beginnen, fallen weg, die neuen muessen dort
        // beginnen, der Rest bleibt (Praxistest 1c: 136 Noten fuer eine
        // Aenderung in einem Takt)
        const bool ranged = cmd.hasProperty ("fromBeat") || cmd.hasProperty ("toBeat");
        const double fromBeat = (double) cmd.getProperty ("fromBeat", 0.0);
        const double toBeat = (double) cmd.getProperty ("toBeat", 1.0e9);

        if (ranged && (fromBeat < 0.0 || toBeat <= fromBeat))
            return resultObj (false, { { "error", "need 0 <= fromBeat < toBeat" } });

        auto checkRange = [&] (const juce::MidiMessageSequence& seq, double clipStart) -> juce::String
        {
            if (! ranged)
                return {};

            const double rs = beatsToSeconds (*ctx.edit, clipStart, fromBeat) - 1.0e-6;
            const double re = toBeat >= 1.0e8 ? 1.0e12 : beatsToSeconds (*ctx.edit, clipStart, toBeat) - 1.0e-6;
            int n = 0;

            for (int i = 0; i < seq.getNumEvents(); ++i)
                if (auto& m = seq.getEventPointer (i)->message; m.isNoteOn())
                {
                    ++n;

                    if (m.getTimeStamp() < rs || m.getTimeStamp() >= re)
                        return "note " + juce::String (n) + " starts outside fromBeat..toBeat";
                }

            return {};
        };

        // Beats je Clip umrechnen (das Tempo kann sich zwischen den Kopien aendern)
        struct Applied { int beyondEnd = 0, replaced = 0; juce::String limitedBy; bool grew = false; };

        auto apply = [&] (te::MidiClip& clip, Applied& out) -> juce::String
        {
            auto pos = clip.getPosition();
            auto start = pos.getStart().inSeconds();
            juce::MidiMessageSequence seq;
            double notesEnd = 0.0;

            if (auto err = parseNotes (*ctx.edit, notesVar, start, seq, notesEnd); err.isNotEmpty())
                return err;

            if (auto err = checkRange (seq, start); err.isNotEmpty())
                return err;

            // Wachsen nur bis zum naechsten Clip der Lane (nie darueber,
            // Praxistest 1b: der Clip ueberlappte seinen Nachfolger)
            const auto oldLength = pos.getLength().inSeconds();

            if (start + notesEnd > pos.getEnd().inSeconds() + 0.001)
            {
                auto end = start + notesEnd;

                if (auto* ct = dynamic_cast<te::ClipTrack*> (clip.getTrack()))
                    for (auto* c : ct->getClips())
                        if (c != &clip && c->getPosition().getStart().inSeconds() > start + 1.0e-6
                            && c->getPosition().getStart().inSeconds() < end)
                        {
                            end = c->getPosition().getStart().inSeconds();
                            out.limitedBy = "next clip " + c->getName();
                        }

                if (end > pos.getEnd().inSeconds())
                    clip.setEnd (te::TimePosition::fromSeconds (end), true);   // Offset bleibt
            }

            out.grew = clip.getPosition().getLength().inSeconds() > oldLength + 1.0e-6;

            for (int i = 0; i < seq.getNumEvents(); ++i)
                if (auto* e = seq.getEventPointer (i); e->message.isNoteOn()
                    && e->message.getTimeStamp() >= clip.getPosition().getLength().inSeconds() - 1.0e-6)
                    ++out.beyondEnd;

            if (ranged)
            {
                juce::Array<te::MidiNote*> drop;

                for (auto* n : clip.getSequence().getNotes())
                {
                    auto b = secondsToBeats (*ctx.edit, start, n->getEditStartTime (clip).inSeconds() - start);

                    if (b >= fromBeat - 1.0e-6 && b < toBeat - 1.0e-6)
                        drop.add (n);
                }

                for (auto* n : drop)
                    clip.getSequence().removeNote (*n, nullptr);

                out.replaced = drop.size();
            }
            else
                clip.getSequence().clear (nullptr);

            mergeNotesRelative (clip, seq);
            clip.state.removeProperty (transposeProp, nullptr);   // neue Noten sind absolut
            return {};
        };

        // Erst alle pruefen (gleiche Noten, nur Fehler in der Liste) — der
        // erste Fehler bricht vor jeder Aenderung ab
        juce::MidiMessageSequence probe;
        {
            double probeEnd = 0.0;

            if (auto err = parseNotes (*ctx.edit, notesVar, mc->getPosition().getStart().inSeconds(), probe, probeEnd); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            if (auto err = checkRange (probe, mc->getPosition().getStart().inSeconds()); err.isNotEmpty())
                return resultObj (false, { { "error", err } });
        }

        Applied first;
        int beyondTotal = 0;
        juce::StringArray updated;

        for (auto* clip : targets)
        {
            Applied a;

            if (auto err = apply (*clip, a); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            if (clip == mc)
                first = a;

            beyondTotal += a.beyondEnd;
            updated.add (clip->getName());
        }

        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("notes", mc->getSequence().getNumNotes());

        if (copies)
            params->setProperty ("copies", updated.size() - 1);

        recordNodeAction (ctx, "process", "setMidiNotes", juce::var (params), {}, true);
        recordAutoSection (ctx, newSection);

        auto r = resultObj (true, { { "track", trackIndex }, { "name", name },
                                    { "notes", mc->getSequence().getNumNotes() },
                                    { "length", mc->getPosition().getLength().inSeconds() }, { "grew", first.grew } });

        if (copies)
            r.getDynamicObject()->setProperty ("updated", juce::var (updated));

        if (ranged)
            r.getDynamicObject()->setProperty ("replaced", first.replaced);

        if (! newSection.isVoid())
            r.getDynamicObject()->setProperty ("autoSection", newSection);

        juce::StringArray warnings;

        if (beyondTotal > 0)
            warnings.add (juce::String (beyondTotal) + " note(s) start after the clip end and stay silent"
                          + (first.limitedBy.isNotEmpty() ? " (clip limited by " + first.limitedBy + ")" : juce::String()));

        if (auto mono = monoWarning (*ctx.edit, *tracks[trackIndex], probe); mono.isNotEmpty())
            warnings.add (mono);

        if (! warnings.isEmpty())
            r.getDynamicObject()->setProperty ("warning", warnings.joinIntoString ("; "));

        if (! copies)
            if (auto hint = patternHint (*tracks[trackIndex], *mc, "setMidiNotes {..., copies: true} writes the notes into all of them"); hint.isNotEmpty())
                r.getDynamicObject()->setProperty ("hint", hint);

        return r;
    }

    // Einzelne Noten aendern, ohne die Liste neu zu senden (Praxistest 1e:
    // 127 Drum-Noten fuenfmal komplett geschickt, um Ghost-Snares lauter zu
    // machen). edits: [{pitch, beat (Beats ab Clip-Anfang, +-1/32) | beat
    // + toBeat (alle dieser Tonhoehe im Bereich), velocity?, beats?,
    // toPitch?, moveTo?, delete?}], add: Noten wie bei setMidiNotes. Erst
    // alles pruefen: ein Eintrag ohne Treffer aendert nichts. toBeat ist der
    // Suchbereich, moveTo das Ziel (Beat ab Clip-Anfang) einer einzelnen
    // Note (Praxistest 1f-1k: Agenten hielten toBeat fuer das Ziel).
    juce::var opEditNotes (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* mc = dynamic_cast<te::MidiClip*> (findClipByName (*tracks[trackIndex], name));

        if (mc == nullptr)
            return resultObj (false, { { "error", "no such MIDI clip: " + name } });

        auto* edits = cmd.getProperty ("edits", {}).getArray();
        auto addVar = cmd.getProperty ("add", {});

        if ((edits == nullptr || edits->isEmpty()) && ! addVar.isArray())
            return resultObj (false, { { "error", "need edits: [{pitch, beat, velocity? | beats? | toPitch? | moveTo? | delete?}] or add: [notes]" } });

        const double start = mc->getPosition().getStart().inSeconds();
        auto beatOf = [&] (te::MidiNote& n) { return secondsToBeats (*ctx.edit, start, n.getEditStartTime (*mc).inSeconds() - start); };
        const double clipBeats = secondsToBeats (*ctx.edit, start, mc->getPosition().getLength().inSeconds());
        std::vector<std::pair<te::MidiNote*, juce::var>> todo;

        if (edits != nullptr)
            for (int i = 0; i < edits->size(); ++i)
            {
                const auto& e = edits->getReference (i);
                const auto where = "edit " + juce::String (i + 1) + ": ";

                if (! e.hasProperty ("pitch") || ! e.hasProperty ("beat"))
                    return resultObj (false, { { "error", where + "needs pitch and beat" } });

                const int pitch = (int) e.getProperty ("pitch", -1);
                const double b0 = (double) e.getProperty ("beat", 0.0);
                const double b1 = e.hasProperty ("toBeat") ? (double) e.getProperty ("toBeat", 0.0) : b0;

                if (b1 < b0)
                    return resultObj (false, { { "error", where + juce::String::fromUTF8 ("toBeat must be >= beat (toBeat ends the search range — to move a note use moveTo)") } });

                if (e.hasProperty ("velocity") && ((int) e.getProperty ("velocity", 0) < 1 || (int) e.getProperty ("velocity", 0) > 127))
                    return resultObj (false, { { "error", where + "velocity must be 1..127" } });

                if (e.hasProperty ("beats") && (double) e.getProperty ("beats", 0.0) <= 0.0)
                    return resultObj (false, { { "error", where + "beats must be > 0" } });

                if (e.hasProperty ("toPitch") && ((int) e.getProperty ("toPitch", -1) < 0 || (int) e.getProperty ("toPitch", -1) > 127))
                    return resultObj (false, { { "error", where + "toPitch must be 0..127" } });

                if (e.hasProperty ("moveTo") && ((double) e.getProperty ("moveTo", 0.0) < 0.0 || (double) e.getProperty ("moveTo", 0.0) >= clipBeats))
                    return resultObj (false, { { "error", where + "moveTo must be 0.." + juce::String (clipBeats, 2) + " (beats from the clip start, inside the clip)" } });

                int found = 0;

                for (auto* n : mc->getSequence().getNotes())
                {
                    const double b = beatOf (*n);

                    if (n->getNoteNumber() == pitch && b >= b0 - 1.0 / 32.0 && b <= b1 + 1.0 / 32.0)
                    {
                        todo.push_back ({ n, e });
                        ++found;
                    }
                }

                if (found == 0)
                    return resultObj (false, { { "error", where + "no note " + juce::String (pitch) + " at beat " + juce::String (b0)
                                                              + (e.hasProperty ("toBeat") ? ".." + juce::String (b1) : juce::String()) } });

                if (found > 1 && e.hasProperty ("moveTo"))
                    return resultObj (false, { { "error", where + "moveTo needs exactly one note, found " + juce::String (found) + juce::String::fromUTF8 (" — give one beat, no toBeat range") } });
            }

        juce::MidiMessageSequence addSeq;
        double addEnd = 0.0;

        if (addVar.isArray())
            if (auto err = parseNotes (*ctx.edit, addVar, start, addSeq, addEnd); err.isNotEmpty())
                return resultObj (false, { { "error", "add: " + err } });

        int changed = 0, deleted = 0;
        juce::Array<te::MidiNote*> drop;

        for (auto& [n, e] : todo)
        {
            if ((bool) e.getProperty ("delete", false))
            {
                drop.addIfNotAlreadyThere (n);
                continue;
            }

            if (e.hasProperty ("velocity"))
                n->setVelocity ((int) e.getProperty ("velocity", 100), nullptr);

            if (e.hasProperty ("beats"))
                n->setStartAndLength (n->getStartBeat(), te::BeatDuration::fromBeats ((double) e.getProperty ("beats", 1.0)), nullptr);

            if (e.hasProperty ("toPitch"))
                n->setNoteNumber ((int) e.getProperty ("toPitch", 60), nullptr);

            if (e.hasProperty ("moveTo"))
                n->setStartAndLength (n->getStartBeat() + te::BeatDuration::fromBeats ((double) e.getProperty ("moveTo", 0.0) - beatOf (*n)),
                                      n->getLengthBeats(), nullptr);

            ++changed;
        }

        for (auto* n : drop)
        {
            mc->getSequence().removeNote (*n, nullptr);
            ++deleted;
        }

        if (addSeq.getNumEvents() > 0)
            mergeNotesRelative (*mc, addSeq);

        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("notes", mc->getSequence().getNumNotes());
        recordNodeAction (ctx, "process", "setMidiNotes", juce::var (params), {}, true);

        auto r = resultObj (true, { { "track", trackIndex }, { "name", name }, { "changed", changed }, { "deleted", deleted },
                                    { "added", addSeq.getNumEvents() / 2 }, { "notes", mc->getSequence().getNumNotes() } });

        if (auto hint = patternHint (*tracks[trackIndex], *mc, "editNotes changes this clip only, setMidiNotes {..., copies: true} writes all of them"); hint.isNotEmpty())
            r.getDynamicObject()->setProperty ("hint", hint);

        return r;
    }

    // Controller-Kurve eines MIDI-Clips setzen (Mod-Wheel, Expression,
    // Sustain, Pitch-Bend): controller = CC-Nummer 0..127 oder "pitchBend",
    // points [{beat (ab Clip-Anfang, im Clip), value}] — CC 0..127,
    // pitchBend -8192..8191 (0 = Mitte). Ersetzt alle Events dieses
    // Controllers im Clip, mit fromBeat/toBeat nur die in diesem Bereich
    // (die neuen Punkte muessen dort liegen); points [] loescht. Eine Kurve
    // je Aufruf wie setAutomation; Folgeaufrufe auf denselben Clip sind
    // eine Aktion. copies: true = alle Clips desselben Patterns wie bei
    // setMidiNotes. Lesen: clipNotes (controllers).
    juce::var opSetControllers (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* mc = dynamic_cast<te::MidiClip*> (findClipByName (*tracks[trackIndex], name));

        if (mc == nullptr)
            return resultObj (false, { { "error", "no such MIDI clip: " + name } });

        const auto cv = cmd.getProperty ("controller", {});
        int type = -1;

        if (cv.toString() == "pitchBend")
            type = te::MidiControllerEvent::pitchWheelType;
        else if ((cv.isInt() || cv.isInt64() || cv.isDouble()) && (double) cv == std::floor ((double) cv) && (double) cv >= 0.0 && (double) cv <= 127.0)
            type = (int) cv;

        if (type < 0)
            return resultObj (false, { { "error", juce::String::fromUTF8 ("controller must be a CC number 0..127 (1 mod wheel, 11 expression, 64 sustain …) or \"pitchBend\"") } });

        const bool bend = type == te::MidiControllerEvent::pitchWheelType;
        auto* points = cmd.getProperty ("points", {}).getArray();

        if (points == nullptr)
            return resultObj (false, { { "error", "need points: [{beat, value}] ([] clears the controller)" } });

        juce::Array<te::MidiClip*> targets { mc };
        if (auto e = copiesError (cmd); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        const bool copies = (bool) cmd.getProperty ("copies", false);

        if (copies)
        {
            auto pattern = mc->state.getProperty ("dawdPattern").toString();

            if (pattern.isEmpty())
                return resultObj (false, { { "error", "clip has no copies (copies: true needs a clip from duplicateClip or its original)" } });

            for (auto* c : tracks[trackIndex]->getClips())
                if (auto* other = dynamic_cast<te::MidiClip*> (c); other != nullptr && other != mc
                    && other->state.getProperty ("dawdPattern").toString() == pattern)
                    targets.add (other);
        }

        const bool ranged = cmd.hasProperty ("fromBeat") || cmd.hasProperty ("toBeat");
        const double fromBeat = (double) cmd.getProperty ("fromBeat", 0.0);
        const double toBeat = (double) cmd.getProperty ("toBeat", 1.0e9);

        if (ranged && (fromBeat < 0.0 || toBeat <= fromBeat))
            return resultObj (false, { { "error", "need 0 <= fromBeat < toBeat" } });

        // Die Punkte muessen in jeden Ziel-Clip passen (Kopien koennen kuerzer sein)
        double clipBeats = 1.0e9;

        for (auto* clip : targets)
            clipBeats = juce::jmin (clipBeats, secondsToBeats (*ctx.edit, clip->getPosition().getStart().inSeconds(),
                                                               clip->getPosition().getLength().inSeconds()));

        const int lo = bend ? -8192 : 0, hi = bend ? 8191 : 127;
        std::vector<std::pair<double, int>> todo;

        // Erst alles pruefen — ein Fehler aendert nichts
        for (int i = 0; i < points->size(); ++i)
        {
            const auto& p = points->getReference (i);
            const auto where = "point " + juce::String (i + 1) + ": ";

            if (auto* o = p.getDynamicObject())
                for (auto& prop : o->getProperties())
                    if (prop.name.toString() != "beat" && prop.name.toString() != "value")
                        return resultObj (false, { { "error", where + "unknown field '" + prop.name.toString() + "' (points take beat and value)" } });

            const auto bv = p.getProperty ("beat", {}), vv = p.getProperty ("value", {});

            if (! (bv.isInt() || bv.isInt64() || bv.isDouble()) || ! (vv.isInt() || vv.isInt64() || vv.isDouble()))
                return resultObj (false, { { "error", where + "needs beat and value (numbers)" } });

            const double b = (double) bv;
            const int v = juce::roundToInt ((double) vv);

            if (b < 0.0 || b >= clipBeats - 1.0e-6)
                return resultObj (false, { { "error", where + "beat must be 0.." + juce::String (clipBeats, 2) + " (beats from the clip start, inside the clip)" } });

            if (ranged && (b < fromBeat - 1.0e-6 || b >= toBeat - 1.0e-6))
                return resultObj (false, { { "error", where + "beat is outside fromBeat..toBeat" } });

            if (v < lo || v > hi)
                return resultObj (false, { { "error", where + "value must be " + juce::String (lo) + ".." + juce::String (hi) + (bend ? " (0 = center)" : "") } });

            todo.push_back ({ b, v });
        }

        int removed = 0;
        juce::StringArray updated;

        for (auto* clip : targets)
        {
            auto& list = clip->getSequence();
            const auto clipStartBeat = clip->getStartBeat();
            juce::Array<te::MidiControllerEvent*> drop;

            for (auto* e : list.getControllerEvents())
            {
                const double b = (e->getEditBeats (*clip) - clipStartBeat).inBeats();

                if (e->getType() == type && (! ranged || (b >= fromBeat - 1.0e-6 && b < toBeat - 1.0e-6)))
                    drop.add (e);
            }

            for (auto* e : drop)
                list.removeControllerEvent (*e, nullptr);

            // Die Engine haelt CC-Werte in 14 Bit (Wert << 7), Pitch-Bend 0..16383;
            // Beats der Events zaehlen ab dem Inhaltsanfang des Clips
            const auto contentOffset = clipStartBeat - clip->getContentStartBeat();

            for (auto& [b, v] : todo)
                list.addControllerEvent (te::BeatPosition::fromBeats (b) + contentOffset, type, bend ? v + 8192 : v << 7, nullptr);

            if (clip == mc)
                removed = drop.size();

            updated.add (clip->getName());
        }

        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("controller", cv);
        params->setProperty ("points", (int) todo.size());

        if (copies)
            params->setProperty ("copies", updated.size() - 1);

        recordNodeAction (ctx, "process", "setControllers", juce::var (params), {}, true);

        auto r = resultObj (true, { { "track", trackIndex }, { "name", name }, { "controller", cv },
                                    { "points", (int) todo.size() }, { "removed", removed } });

        if (copies)
            r.getDynamicObject()->setProperty ("updated", juce::var (updated));

        return r;
    }

    namespace
    {
        // Controller-Event der Engine als MIDI-Nachricht (Werte in 14 Bit);
        // false fuer Typen ohne Nachricht (SysEx-Platzhalter)
        bool controllerMessage (const te::MidiControllerEvent& e, int ch, juce::MidiMessage& m)
        {
            const auto type = e.getType(), v = e.getControllerValue();

            if (type >= 0 && type < 128)
                m = juce::MidiMessage::controllerEvent (ch, type, v >> 7);
            else if (type == te::MidiControllerEvent::pitchWheelType)
                m = juce::MidiMessage::pitchWheel (ch, v);
            else if (type == te::MidiControllerEvent::programChangeType)
                m = juce::MidiMessage::programChange (ch, v >> 7);
            else if (type == te::MidiControllerEvent::channelPressureType)
                m = juce::MidiMessage::channelPressureChange (ch, v >> 7);
            else if (type == te::MidiControllerEvent::aftertouchType)
                m = juce::MidiMessage::aftertouchChange (ch, e.getMetadata(), v >> 7);
            else
                return false;

            return true;
        }

        // Was ein MIDI-Clip im Bereich [from, to) spielt, in Songzeit
        // (Sekunden): Noten, die dort beginnen (am Bereichs-/Clip-Ende
        // abgeschnitten), Controller darin, dazu je Controller der letzte
        // Wert davor am Anfang (die Engine schickt ihn beim Abspielen auch),
        // ohne Pitch-Bend dort die Bend-Mitte
        void addClipEvents (te::MidiClip& mc, double from, double to, juce::MidiMessageSequence& out, int& notes, int& controllers)
        {
            const auto pos = mc.getPosition();
            const double a = juce::jmax (from, pos.getStart().inSeconds()), b = juce::jmin (to, pos.getEnd().inSeconds());

            if (b <= a + 1.0e-6)
                return;

            auto& list = mc.getSequence();
            const int ch = juce::jlimit (1, 16, list.getMidiChannel().getChannelNumber());
            std::map<int, std::pair<double, te::MidiControllerEvent*>> before;
            juce::MidiMessage m;

            bool bendAtStart = false;

            for (auto* e : list.getControllerEvents())
            {
                const auto t = e->getEditTime (mc).inSeconds();

                if (t < a - 0.0005)
                    if (auto it = before.find (e->getType()); it == before.end() || it->second.first <= t)
                        before[e->getType()] = { t, e };

                if (e->getType() == te::MidiControllerEvent::pitchWheelType && t < a + 0.0005)
                    bendAtStart = true;
            }

            // Kein Pitch-Bend davor oder am Anfang: die Mitte, wie beim
            // Abspielen (Patch tracktion-midi-pitchbend-reset) — sonst bliebe
            // im Player der Bend eines frueheren Clips stehen. Zaehlt nicht
            // zu controllers (kein Event des Clips)
            if (! bendAtStart)
                out.addEvent (juce::MidiMessage::pitchWheel (ch, 8192), a);

            for (auto& [type, ev] : before)
                if (controllerMessage (*ev.second, ch, m))
                {
                    out.addEvent (m, a);
                    ++controllers;
                }

            for (auto* e : list.getControllerEvents())
                if (auto t = e->getEditTime (mc).inSeconds(); t >= a - 0.0005 && t < b - 0.0005 && controllerMessage (*e, ch, m))
                {
                    out.addEvent (m, juce::jmax (a, t));
                    ++controllers;
                }

            for (auto* n : list.getNotes())
            {
                auto r = n->getEditTimeRange (mc);
                auto s = r.getStart().inSeconds();

                if (n->isMute() || s < a - 0.0005 || s >= b - 0.0005)
                    continue;

                out.addEvent (juce::MidiMessage::noteOn (ch, n->getNoteNumber(), (juce::uint8) n->getVelocity()), juce::jmax (a, s));
                out.addEvent (juce::MidiMessage::noteOff (ch, n->getNoteNumber()), juce::jmin (b, r.getEnd().inSeconds()));
                ++notes;
            }
        }
    }

    // MIDI-Export als .mid (Typ 1, 960 Ticks je Viertel): ohne track der
    // Song (jede MIDI-Spur eine Datei-Spur mit ihrem Namen), track = eine
    // Spur (die Gruppe samt ihren Lanes, eine Lane allein), track + name =
    // ein Clip. section grenzt Song und Spur auf ihren Zeitbereich ein (Clips
    // daran abgeschnitten). Zeit 0 der Datei = Anfang des Bereichs (Song: 0,
    // Clip: sein Start). Die erste Datei-Spur traegt Tempo und Takt aus den
    // Sections (die Tempospur der Engine). Gemutete Lanes (Lane-Wahl) fehlen
    // wie im Render; Solo/Mute als Abhoer-Hilfe zaehlt nicht. Liest nur.
    juce::var opExportMidi (DawdContext& ctx, const juce::var& cmd)
    {
        auto path = cmd.getProperty ("file", {}).toString();

        if (path.isEmpty())
            return resultObj (false, { { "error", "no file" } });

        if (! juce::File::isAbsolutePath (path))
            return resultObj (false, { { "error", "file must be an absolute path" } });

        juce::File file (path);

        if (! file.hasFileExtension (".mid;.midi"))
            return resultObj (false, { { "error", "file must end in .mid" } });

        auto& edit = *ctx.edit;
        auto tracks = userTracks (edit);
        double from = 0.0, to = edit.getLength().inSeconds();
        te::MidiClip* onlyClip = nullptr;
        struct Unit { juce::String name; juce::Array<te::AudioTrack*> members; };
        std::vector<Unit> units;

        if (cmd.hasProperty ("name") && ! cmd.hasProperty ("track"))
            return resultObj (false, { { "error", "name (a clip) needs track" } });

        if (cmd.hasProperty ("name") && cmd.hasProperty ("section"))
            return resultObj (false, { { "error", "section limits a song or track export, not a clip" } });

        // Mute wie gespeichert (Lane-Wahl), nicht der Abhoer-Wert
        const auto& listening = listeningMutes (ctx, edit);
        auto plays = [&] (te::AudioTrack& t)
        {
            auto it = listening.find (t.itemID.toString());
            return ! (it != listening.end() ? it->second : t.isMuted (false));
        };

        auto groupMembers = [&] (te::AudioTrack& g)
        {
            juce::Array<te::AudioTrack*> m;

            for (auto* t : tracks)
                if ((t == &g || laneGroupOf (*t) == &g) && plays (*t))
                    m.add (t);

            return m;
        };

        if (cmd.hasProperty ("track"))
        {
            const int ti = (int) cmd.getProperty ("track", -1);

            if (ti < 0 || ti >= tracks.size())
                return resultObj (false, { { "error", "no such track" } });

            auto* t = tracks[ti];
            auto* g = laneGroupOf (*t) != nullptr ? laneGroupOf (*t) : t;

            if (cmd.hasProperty ("name"))
            {
                auto name = cmd.getProperty ("name", {}).toString();
                onlyClip = dynamic_cast<te::MidiClip*> (findClipByName (*t, name));

                if (onlyClip == nullptr)
                    return resultObj (false, { { "error", "no such MIDI clip: " + name } });

                from = onlyClip->getPosition().getStart().inSeconds();
                to = onlyClip->getPosition().getEnd().inSeconds();
                units.push_back ({ g->getName(), { t } });
            }
            else if (! isMidiTrack (*g))
                return resultObj (false, { { "error", "not a MIDI track: " + t->getName() } });
            else
                units.push_back ({ g->getName(), t != g ? juce::Array<te::AudioTrack*> { t } : groupMembers (*g) });
        }
        else
        {
            for (auto* t : tracks)
                if (laneGroupOf (*t) == nullptr && isMidiTrack (*t))
                    units.push_back ({ t->getName(), groupMembers (*t) });
        }

        if (cmd.hasProperty ("section"))
        {
            auto name = cmd.getProperty ("section", {}).toString();
            auto* sec = findSection (edit, name);

            if (sec == nullptr)
                return resultObj (false, { { "error", "no such section: " + name } });

            from = sec->getPosition().getStart().inSeconds();
            to = sec->getPosition().getEnd().inSeconds();
        }

        // Songzeit -> Ticks ab from (ueber die Tempospur, auch ueber Wechsel)
        constexpr int ppq = 960;
        auto& ts = edit.tempoSequence;
        auto tp = te::createPosition (ts);
        auto ppqAt = [&] (double t) { tp.set (te::TimePosition::fromSeconds (t)); return tp.getPPQTime(); };
        const double ppq0 = ppqAt (from);
        auto toTicks = [&] (double t) { return std::round ((ppqAt (t) - ppq0) * ppq); };

        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (ppq);
        juce::Array<juce::var> written;
        int totalEvents = 0;

        {
            // Tempo (Mikrosekunden je Viertel; dawd-BPM zaehlen Viertel) und
            // Takt: der Stand bei from, dann jeder Wechsel im Bereich
            juce::MidiMessageSequence meta;
            meta.addEvent (juce::MidiMessage::textMetaEvent (3, onlyClip != nullptr ? onlyClip->getName() : file.getFileNameWithoutExtension()), 0);

            auto addTempo = [&] (double t, double bpm)
            {
                meta.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / bpm)), toTicks (t));
            };
            auto addSig = [&] (double t, const te::TimeSigSetting& sig)
            {
                meta.addEvent (juce::MidiMessage::timeSignatureMetaEvent (sig.numerator, sig.denominator), toTicks (t));
            };

            addSig (from, ts.getTimeSigAt (te::TimePosition::fromSeconds (from)));
            addTempo (from, ts.getBpmAt (te::TimePosition::fromSeconds (from)));   // mitten in einer Rampe: das Tempo dort

            for (int i = 0; i < ts.getNumTimeSigs(); ++i)
                if (auto* sig = ts.getTimeSig (i))
                    if (auto t = ts.toTime (sig->getStartBeat()).inSeconds(); t > from + 1.0e-6 && t < to - 1.0e-6)
                        addSig (t, *sig);

            for (int i = 0; i < ts.getNumTempos(); ++i)
                if (auto* tempo = ts.getTempo (i))
                {
                    if (auto t = tempo->getStartTime().inSeconds(); t > from + 1.0e-6 && t < to - 1.0e-6)
                        addTempo (t, tempo->getBpm());

                    // Tempo-Rampe (Kurve 0 bis zum naechsten Punkt): die Datei
                    // kennt nur Spruenge — Stufen wie die Engine (4 je Beat,
                    // hoechstens 100), je mit dem Tempo an ihrem Anfang
                    auto* next = ts.getTempo (i + 1);

                    if (next != nullptr && tempo->getCurve() != 1.0f && tempo->getCurve() != -1.0f)
                    {
                        const double b0 = tempo->getStartBeat().inBeats(), beats = next->getStartBeat().inBeats() - b0;
                        const int n = (int) juce::jlimit (1.0, 100.0, 4.0 * beats);

                        for (int k = 1; k < n; ++k)
                            if (auto t = ts.toTime (te::BeatPosition::fromBeats (b0 + beats * k / n)).inSeconds(); t > from + 1.0e-6 && t < to - 1.0e-6)
                                addTempo (t, tempo->getBpm() + (next->getBpm() - tempo->getBpm()) * k / n);
                    }
                }

            meta.sort();
            meta.addEvent (juce::MidiMessage::endOfTrack(), toTicks (to));   // die Datei ist so lang wie der Bereich
            mf.addTrack (meta);
        }

        for (auto& u : units)
        {
            juce::MidiMessageSequence seq;
            int notes = 0, controllers = 0;

            for (auto* t : u.members)
                for (auto* c : t->getClips())
                    if (auto* mc = dynamic_cast<te::MidiClip*> (c); mc != nullptr && (onlyClip == nullptr || mc == onlyClip))
                        addClipEvents (*mc, from, to, seq, notes, controllers);

            if (seq.getNumEvents() == 0)
                continue;

            for (auto* e : seq)
                e->message.setTimeStamp (toTicks (e->message.getTimeStamp()));

            seq.sort();
            juce::MidiMessageSequence named;   // Spurname zuerst
            named.addEvent (juce::MidiMessage::textMetaEvent (3, u.name), 0);
            named.addSequence (seq, 0);
            named.addEvent (juce::MidiMessage::endOfTrack(), toTicks (to));
            mf.addTrack (named);
            totalEvents += seq.getNumEvents();
            written.add (resultObj (true, { { "name", u.name }, { "notes", notes }, { "controllers", controllers } }));
            written.getReference (written.size() - 1).getDynamicObject()->removeProperty ("ok");
        }

        if (totalEvents == 0)
            return resultObj (false, { { "error", "nothing to export: no MIDI notes or controllers in the range" } });

        if (file.existsAsFile() && ! file.deleteFile())
            return resultObj (false, { { "error", "cannot overwrite " + file.getFullPathName() } });

        if (! file.getParentDirectory().createDirectory().wasOk())
            return resultObj (false, { { "error", "cannot create " + file.getParentDirectory().getFullPathName() } });

        {
            juce::FileOutputStream out (file);

            if (! out.openedOk() || ! mf.writeTo (out, 1))
                return resultObj (false, { { "error", "cannot write " + file.getFullPathName() } });
        }

        return resultObj (true, { { "file", file.getFullPathName() }, { "bytes", (juce::int64) file.getSize() },
                                  { "tracks", written }, { "start", from }, { "end", to } });
    }

    // Noten eines MIDI-Clips aufs Raster ziehen (destruktiv, Undo): grid
    // ("1/16", "1/8T" …), strength 0..1 (Anteil des Wegs, Default 1), swing
    // 0..1 (ungerade Rasterplaetze um diesen Anteil des Rasters spaeter;
    // ~0.33 = triolisch), groove = Ergebnis von extractGroove (je Platz im
    // Takt die Abweichung, ersetzt swing), window 0..1 (nur Noten, die
    // naeher als window * halbes Raster liegen), ends (auch Notenenden).
    // Das Raster zaehlt ab dem Start der Section, im Tempo der Section.
    juce::var opQuantizeClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* mc = dynamic_cast<te::MidiClip*> (findClipByName (*tracks[trackIndex], name));

        if (mc == nullptr)
            return resultObj (false, { { "error", "no such MIDI clip: " + name } });

        double grid = 0.25;
        auto groove = cmd.getProperty ("groove", {});
        std::vector<double> grooveOffsets;

        if (groove.isObject())
        {
            // Raster des Grooves gilt, wenn keins angegeben ist
            if (auto err = parseGrid (cmd.getProperty ("grid", groove.getProperty ("grid", "1/16")), grid); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            auto* slots = groove.getProperty ("slots", {}).getArray();

            if (slots == nullptr || slots->isEmpty())
                return resultObj (false, { { "error", "groove needs slots [{slot, offset}] (from extractGroove)" } });

            grooveOffsets.assign ((size_t) slots->size(), 0.0);

            for (auto& sl : *slots)
            {
                auto k = (int) sl.getProperty ("slot", -1);

                if (k < 0 || k >= slots->size())
                    return resultObj (false, { { "error", "groove slot out of range" } });

                grooveOffsets[(size_t) k] = juce::jlimit (-0.5, 0.5, (double) sl.getProperty ("offset", 0.0));
            }
        }
        else if (auto err = parseGrid (cmd.getProperty ("grid", "1/16"), grid); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto num = [&] (const char* key, double def, double lo, double hi, double& out) -> juce::String
        {
            out = (double) cmd.getProperty (key, def);
            return out < lo || out > hi ? juce::String (key) + " must be " + juce::String (lo) + ".." + juce::String (hi) : juce::String();
        };

        double strength = 1.0, swing = 0.0, window = 1.0;

        for (auto e : { num ("strength", 1.0, 0.0, 1.0, strength), num ("swing", 0.0, 0.0, 1.0, swing), num ("window", 1.0, 0.0, 1.0, window) })
            if (e.isNotEmpty())
                return resultObj (false, { { "error", e } });

        if (! grooveOffsets.empty() && cmd.hasProperty ("swing"))
            return resultObj (false, { { "error", "use swing or groove, not both" } });

        const bool ends = (bool) cmd.getProperty ("ends", false);
        auto* sec = findSection (*ctx.edit, sectionAt (*ctx.edit, mc->getPosition().getStart().inSeconds()));

        if (sec == nullptr)
            return resultObj (false, { { "error", "clip lies outside every section" } });

        const auto secStart = sec->getPosition().getStart().inSeconds();
        auto& edit = *ctx.edit;

        // Zielposition des Rasterplatzes k in Section-Beats
        auto target = [&] (long k)
        {
            double off = 0.0;

            if (! grooveOffsets.empty())
            {
                auto n = (long) grooveOffsets.size();
                off = grooveOffsets[(size_t) (((k % n) + n) % n)];
            }
            else if ((k % 2 + 2) % 2 == 1)
                off = swing;

            return ((double) k + off) * grid;
        };

        auto nearest = [&] (double beat)
        {
            auto k0 = (long) std::floor (beat / grid);
            double best = target (k0);

            for (long k = k0 - 1; k <= k0 + 2; ++k)
                if (std::abs (target (k) - beat) < std::abs (best - beat))
                    best = target (k);

            return best;
        };

        juce::Array<te::MidiClip*> targets { mc };

        if (auto e = copiesError (cmd); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        if ((bool) cmd.getProperty ("copies", false))
        {
            auto pattern = mc->state.getProperty ("dawdPattern").toString();

            if (pattern.isEmpty())
                return resultObj (false, { { "error", "clip has no copies (copies: true needs a clip from duplicateClip or its original)" } });

            for (auto* c : tracks[trackIndex]->getClips())
                if (auto* other = dynamic_cast<te::MidiClip*> (c); other != nullptr && other != mc
                    && other->state.getProperty ("dawdPattern").toString() == pattern)
                    targets.add (other);
        }

        int moved = 0, total = 0;
        double maxShiftMs = 0.0;

        for (auto* clip : targets)
        {
            for (auto* note : clip->getSequence().getNotes())
            {
                ++total;
                auto t0 = clip->getTimeOfContentBeat (note->getStartBeat()).inSeconds();
                auto t1 = clip->getTimeOfContentBeat (note->getEndBeat()).inSeconds();
                auto b0 = secondsToBeats (edit, secStart, t0 - secStart);
                auto b1 = secondsToBeats (edit, secStart, t1 - secStart);
                auto q0 = nearest (b0);

                if (std::abs (q0 - b0) > window * grid * 0.5 + 1.0e-9)
                    continue;

                auto n0 = b0 + strength * (q0 - b0);
                auto n1 = b1 - b0 + n0;   // Laenge bleibt

                if (ends)
                {
                    auto e = std::round (b1 / grid) * grid;
                    n1 = juce::jmax (n0 + grid * 0.5, b1 + strength * (e - b1));
                }

                auto s0 = secStart + beatsToSeconds (edit, secStart, n0);
                auto s1 = secStart + beatsToSeconds (edit, secStart, n1);
                auto cb0 = clip->getContentBeatAtTime (te::TimePosition::fromSeconds (s0));
                auto cb1 = clip->getContentBeatAtTime (te::TimePosition::fromSeconds (s1));

                if (cb0.inBeats() < 0.0)
                    continue;   // vor den Clip-Anfang verschieben ginge verloren

                if (std::abs (s0 - t0) > 1.0e-6 || std::abs (s1 - t1) > 1.0e-6)
                {
                    note->setStartAndLength (cb0, cb1 - cb0, nullptr);
                    ++moved;
                    maxShiftMs = juce::jmax (maxShiftMs, std::abs (s0 - t0) * 1000.0);
                }
            }
        }

        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("grid", grid);
        params->setProperty ("strength", strength);
        recordNodeAction (ctx, "process", "quantizeClip", juce::var (params));

        return resultObj (true, { { "track", trackIndex }, { "name", name }, { "grid", grid },
                                  { "notes", total }, { "moved", moved },
                                  { "maxShiftMs", std::round (maxShiftMs * 10.0) / 10.0 },
                                  { "clips", targets.size() } });
    }

    juce::var opTrimClip (DawdContext& ctx, const juce::var& cmd)
    {
        // Trimmen: start = neue Startkante (das Material bleibt auf der
        // Timeline stehen — die Engine schiebt den Offset mit), length =
        // neue Laenge (Endkante, der Offset bleibt). Beides absolute
        // Sekunden wie in state.trackList.
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        bool hasStart = cmd.hasProperty ("start");
        auto newStart = (double) cmd.getProperty ("start", 0.0);
        bool hasLength = cmd.hasProperty ("length");
        auto length = (double) cmd.getProperty ("length", 0.0);

        if (name.isEmpty())
            return resultObj (false, { { "error", "no clip name" } });

        if (! hasStart && ! hasLength)
            return resultObj (false, { { "error", "no start or length" } });

        if (hasStart && newStart < 0.0)
            return resultObj (false, { { "error", "start must be >= 0" } });

        if (hasLength && length <= 0.0)
            return resultObj (false, { { "error", "length must be > 0" } });

        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        auto pos = clip->getPosition();

        if (hasStart && newStart >= pos.getEnd().inSeconds())
            return resultObj (false, { { "error", "start must be before the clip end" } });

        if (hasStart)
        {
            // MIDI: Noten liegen relativ zum Inhaltsanfang (Start - Offset).
            // Nach links ueber den Offset hinaus kappt die Engine den Offset
            // bei 0 und zieht den Inhalt mit — die Noten um den Rest
            // zurueckschieben, damit sie an ihrer Songzeit bleiben
            // (Praxistest 1m: alle Noten 0,11 s zu frueh)
            auto* mc = dynamic_cast<te::MidiClip*> (clip);
            const auto contentStart = mc != nullptr ? mc->getContentStartBeat() : te::BeatPosition();

            // Audio: ueber den Dateianfang hinaus zoege die Engine das Audio
            // ebenso mit — der Clip spielt dann eine Kopie mit Stille davor
            // (rueckwaerts: dahinter) ab der neuen Kante
            auto* ac = dynamic_cast<te::AudioClipBase*> (clip);

            if (ac != nullptr && ac->isLooping())
            {
                // Loop: der Offset laeuft im Loop rueckwaerts weiter (modulo
                // eines Durchgangs) — das Audio bleibt an seiner Songzeit
                auto pass = ac->getLoopLength().inSeconds();
                auto offset = std::fmod (pos.getOffset().inSeconds() + newStart - pos.getStart().inSeconds(), pass);
                ac->setPosition ({ { te::TimePosition::fromSeconds (newStart), pos.getEnd() },
                                   te::TimeDuration::fromSeconds (offset < 0.0 ? offset + pass : offset) });
            }
            else if (ac != nullptr && pos.getOffset().inSeconds() + newStart - pos.getStart().inSeconds() < -1.0e-6)
            {
                // Gewarpt: die Marker zeigen auf Stellen der Datei, eine
                // Kopie mit Stille davor verschoebe sie
                if (isWarped (*ac))
                    return resultObj (false, { { "error", "clip is warped - its audio cannot reach before the file start (warpClip clear: true first)" } });

                // Clip-FX: die gerechnete Datei beginnt am Start des
                // Originals, eine Kopie mit Stille davor verschoebe den Rueckweg
                if (isClipProcessed (*ac))
                    return resultObj (false, { { "error", "clip is processed - its audio cannot reach before the file start (processClip clear: true first)" } });

                auto error = padAudioClipStart (ctx, *ac, newStart);

                if (error.isNotEmpty())
                    return resultObj (false, { { "error", error } });
            }
            else
                clip->setStart (te::TimePosition::fromSeconds (newStart), true, false);

            if (mc != nullptr)
                mc->getSequence().moveAllBeatPositions (contentStart - mc->getContentStartBeat(), nullptr);
        }

        if (hasLength)
            clip->setLength (te::TimeDuration::fromSeconds (length), true);

        auto newPos = clip->getPosition();
        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("start", newPos.getStart().inSeconds());
        params->setProperty ("length", newPos.getLength().inSeconds());
        params->setProperty ("section", sectionAt (*ctx.edit, newPos.getStart().inSeconds()));
        recordNodeAction (ctx, "process", "trimClip", juce::var (params));
        recordAutoSection (ctx, newSection);

        return withAutoSection (resultObj (true, { { "start", newPos.getStart().inSeconds() },
                                                   { "length", newPos.getLength().inSeconds() } }), newSection);
    }

    juce::var opMoveClip (DawdContext& ctx, const juce::var& cmd)
    {
        // Verschieben: start = neuer Clip-Start in Sekunden, Laenge und
        // Inhalt bleiben (der Offset wandert mit — das Material folgt dem
        // Clip).
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto newStart = (double) cmd.getProperty ("start", -1.0);

        if (name.isEmpty())
            return resultObj (false, { { "error", "no clip name" } });

        if (newStart < 0.0)
            return resultObj (false, { { "error", "start must be >= 0" } });

        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        // preserveSync=false: der Inhalt wandert mit dem Clip, keepLength:
        // die Laenge bleibt
        clip->setStart (te::TimePosition::fromSeconds (newStart), false, true);

        auto newPos = clip->getPosition();
        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("start", newPos.getStart().inSeconds());
        params->setProperty ("section", sectionAt (*ctx.edit, newPos.getStart().inSeconds()));
        recordNodeAction (ctx, "process", "moveClip", juce::var (params));
        recordAutoSection (ctx, newSection);

        return withAutoSection (resultObj (true, { { "start", newPos.getStart().inSeconds() },
                                                   { "length", newPos.getLength().inSeconds() } }), newSection);
    }

    namespace
    {
        juce::var clipInfo (te::Clip& c)
        {
            auto p = c.getPosition();
            auto o = resultObj (true, { { "name", c.getName() }, { "start", p.getStart().inSeconds() },
                                        { "length", p.getLength().inSeconds() } });
            o.getDynamicObject()->removeProperty ("ok");
            return o;
        }
    }

    // Clip an einer Stelle teilen: at = Songsekunden oder atBeat = Beats ab
    // dem Clip-Start. Links behaelt den Namen, rechts bekommt einen freien
    // ("Notes 2"); Audio und MIDI bleiben, wo sie klingen.
    juce::var opSplitClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        if (cmd.hasProperty ("at") == cmd.hasProperty ("atBeat"))
            return resultObj (false, { { "error", "need at (song seconds) or atBeat (beats from the clip start)" } });

        auto pos = clip->getPosition();
        auto start = pos.getStart().inSeconds(), end = pos.getEnd().inSeconds();
        auto at = cmd.hasProperty ("at") ? (double) cmd.getProperty ("at", 0.0)
                                         : start + beatsToSeconds (*ctx.edit, start, (double) cmd.getProperty ("atBeat", 0.0));

        if (at <= start + 0.001 || at >= end - 0.001)
            return resultObj (false, { { "error", "split point must lie inside the clip ("
                                                  + juce::String (start, 3) + " .. " + juce::String (end, 3) + " s)" } });

        auto* right = splitClipAt (*clip, at);

        if (right == nullptr)
            return resultObj (false, { { "error", "split failed" } });

        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("at", at);
        params->setProperty ("right", right->getName());
        recordNodeAction (ctx, "process", "splitClip", juce::var (params));

        auto* left = findClipByName (*tracks[trackIndex], name);
        return resultObj (true, { { "track", trackIndex }, { "left", left != nullptr ? clipInfo (*left) : juce::var() },
                                  { "right", clipInfo (*right) } });
    }

    // Clip kopieren: ohne start direkt hinter den Clip, count-mal am Stueck
    // (eine Wiederholung je Kopie, 1..64); mit start an diese Songsekunde
    // (bzw. startBeat = Beats ab dem Start der Section, in der der Clip
    // liegt, mit section ab dieser — ein expliziter Start muss in einer
    // Section liegen). Die Kopien
    // laufen ueber Section-Grenzen; hinter der letzten Section waechst eine
    // Auto-Section mit. Freie Namen ("Notes 2" …).
    juce::var opDuplicateClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto count = (int) cmd.getProperty ("count", 1);
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        if (count < 1 || count > 64)
            return resultObj (false, { { "error", "count must be 1..64" } });

        if (cmd.hasProperty ("start") && cmd.hasProperty ("startBeat"))
            return resultObj (false, { { "error", "use start (seconds) or startBeat, not both" } });

        auto pos = clip->getPosition();
        auto length = pos.getLength().inSeconds();
        auto* home = findSection (*ctx.edit, sectionAt (*ctx.edit, pos.getStart().inSeconds()));
        double first = pos.getEnd().inSeconds();
        bool explicitStart = false;

        // section (Decision Matthias 2026-10-29, Todo clip-fallen): startBeat
        // zaehlt ab dieser Section wie bei addMidiClip; section allein = ihr
        // Anfang. Ohne section wie bisher ab der Section des Quell-Clips.
        const bool inSection = cmd.hasProperty ("section");

        if (inSection)
        {
            if (cmd.hasProperty ("start"))
                return resultObj (false, { { "error", "use section with startBeat (beats from its start), or start (song seconds) without section" } });

            auto secName = cmd.getProperty ("section", {}).toString();
            home = findSection (*ctx.edit, secName);

            if (home == nullptr)
                return resultObj (false, { { "error", "no such section: " + secName } });

            if (! cmd.hasProperty ("startBeat"))
            {
                first = home->getPosition().getStart().inSeconds();
                explicitStart = true;
            }
        }

        if (cmd.hasProperty ("start"))
        {
            first = (double) cmd.getProperty ("start", -1.0);
            explicitStart = true;
        }
        else if (cmd.hasProperty ("startBeat") && home != nullptr)
        {
            auto secStart = home->getPosition().getStart().inSeconds();
            first = (double) cmd.getProperty ("startBeat", -1.0) < 0.0 ? -1.0
                      : secStart + beatsToSeconds (*ctx.edit, secStart, (double) cmd.getProperty ("startBeat", 0.0));
            explicitStart = true;
        }

        // startBeat zaehlt ab dem Start der Section des Quell-Clips (nicht
        // ab dem Songanfang) — Fehlertexte nennen die Rechnung (Praxistest 3)
        const auto beatsFrom = cmd.hasProperty ("startBeat") && home != nullptr
                                 ? "startBeat " + cmd.getProperty ("startBeat", {}).toString() + " counts from the start of section '"
                                       + home->getName() + "' (" + juce::String (home->getPosition().getStart().inSeconds(), 2)
                                       + (inSection ? juce::String (" s)") : " s, the section of '" + name + "')")
                                 : juce::String();

        if (first < 0.0)
            return resultObj (false, { { "error", beatsFrom.isNotEmpty() ? "start must be >= 0 (" + beatsFrom + ")" : juce::String ("start must be >= 0") } });

        if (explicitStart && findSection (*ctx.edit, sectionAt (*ctx.edit, first)) == nullptr)
        {
            auto secs = sortedSections (*ctx.edit);
            const auto songEnd = secs.empty() ? 0.0 : secs.back()->getPosition().getEnd().inSeconds();
            return resultObj (false, { { "error", "start lies outside every section ("
                                                    + (beatsFrom.isNotEmpty() ? beatsFrom + " = " + juce::String (first, 2) + " s"
                                                                              : "start " + juce::String (first, 2) + " s")
                                                    + "; the sections end at " + juce::String (songEnd, 2) + " s)" } });
        }

        juce::Array<juce::var> made;

        // Original und Kopien bilden ein Pattern (Name des ersten Originals) —
        // setMidiNotes {copies: true} aendert alle auf einmal
        if (clip->state.getProperty ("dawdPattern").toString().isEmpty())
            clip->state.setProperty ("dawdPattern", clip->getName(), nullptr);

        for (int i = 0; i < count; ++i)
        {
            auto s = first + i * length;
            auto copy = te::duplicateClip (*clip);

            if (copy == nullptr)
                return resultObj (false, { { "error", "duplicate failed" }, { "clips", made } });

            copy->setName (uniqueClipName (*tracks[trackIndex], name));
            copy->setStart (te::TimePosition::fromSeconds (s), false, true);
            made.add (clipInfo (*copy));
        }

        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("start", first);
        params->setProperty ("count", made.size());
        recordNodeAction (ctx, "process", "duplicateClip", juce::var (params));
        recordAutoSection (ctx, newSection);

        auto res = resultObj (true, { { "track", trackIndex }, { "clips", made } });

        // Wo die Kopien landen und worauf sich startBeat bezog
        if (explicitStart)
            res.getDynamicObject()->setProperty ("section", sectionAt (*ctx.edit, first));

        if (beatsFrom.isNotEmpty())
            res.getDynamicObject()->setProperty ("startBeatFrom", home->getName());

        return withAutoSection (res, newSection);
    }

    // Clip-Eigenschaften: gainDb (Audio -48..24, MIDI -100..0 — der MIDI-Clip
    // kann nur leiser), fadeIn/fadeOut in Sekunden (Audio) mit fadeCurve,
    // transpose in Halbtoenen (MIDI: Noten verschieben; Audio braucht den
    // Timestretcher — noch nicht da), reverse (Audio, die Engine rendert eine
    // umgedrehte Kopie), normalize peak|rms mit targetDb (Audio: misst den
    // hoerbaren Bereich und setzt gainDb — nicht destruktiv, einmalig; spaeteres
    // Trimmen rechnet nicht neu), crossfade (Audio: Ueberlappungen mit den
    // Nachbarn blenden; Standard an, refreshAutoCrossfades). fadeCurve auto =
    // dawd fuehrt die Kurve. Erst alles pruefen, dann setzen.
    juce::var opSetClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        auto* ac = dynamic_cast<te::AudioClipBase*> (clip);
        auto* mc = dynamic_cast<te::MidiClip*> (clip);
        const bool any = cmd.hasProperty ("gainDb") || cmd.hasProperty ("fadeIn") || cmd.hasProperty ("fadeOut")
                      || cmd.hasProperty ("fadeCurve") || cmd.hasProperty ("transpose") || cmd.hasProperty ("reverse")
                      || cmd.hasProperty ("normalize") || cmd.hasProperty ("crossfade");

        if (cmd.hasProperty ("targetDb") && ! cmd.hasProperty ("normalize"))
            return resultObj (false, { { "error", "targetDb needs normalize (peak or rms)" } });

        if (! any)
            return resultObj (false, { { "error", "nothing to set (gainDb, fadeIn, fadeOut, fadeCurve, transpose, reverse, normalize, crossfade)" } });

        // Warp-Marker und Loop zeigen auf Stellen der vorwaerts laufenden Datei
        if (ac != nullptr && (bool) cmd.getProperty ("reverse", false) && ! ac->getIsReversed() && (isWarped (*ac) || ac->isLooping()))
            return resultObj (false, { { "error", isWarped (*ac) ? "clip is warped - clear the warp markers first (warpClip clear: true)"
                                                                     : "clip loops - turn the loop off first (loopClip on: false)" } });

        auto length = clip->getPosition().getLength().inSeconds();
        auto gain = (double) cmd.getProperty ("gainDb", 0.0);
        const auto normalize = cmd.getProperty ("normalize", {}).toString();
        juce::var normalized;

        if (cmd.hasProperty ("normalize"))
        {
            if (ac == nullptr)
                return resultObj (false, { { "error", "normalize works on audio clips only" } });

            if (normalize != "peak" && normalize != "rms")
                return resultObj (false, { { "error", "normalize must be peak or rms" } });

            if (cmd.hasProperty ("gainDb"))
                return resultObj (false, { { "error", juce::String::fromUTF8 ("normalize sets gainDb itself — send one of them") } });

            const bool rms = normalize == "rms";
            const double target = (double) cmd.getProperty ("targetDb", rms ? normalizeRmsDefaultDb : normalizePeakDefaultDb);

            if (target < -60.0 || target > 0.0)
                return resultObj (false, { { "error", "targetDb must be -60..0 dBFS" } });

            double measured = 0.0;
            const bool reversed = cmd.hasProperty ("reverse") ? (bool) cmd.getProperty ("reverse", false) : ac->getIsReversed();

            if (auto err = measureClipLevel (ctx, *ac, rms, reversed, measured); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            gain = std::round ((target - measured) * 100.0) / 100.0;

            if (gain < -48.0 || gain > 24.0)
                return resultObj (false, { { "error", "normalizing needs " + juce::String (gain, 1) + " dB of clip gain, more than -48..24 allows" } });

            normalized = resultObj (true, { { "mode", normalize }, { "measuredDb", std::round (measured * 100.0) / 100.0 },
                                            { "targetDb", target } });
            normalized.getDynamicObject()->removeProperty ("ok");
        }
        auto fadeIn = (double) cmd.getProperty ("fadeIn", 0.0), fadeOut = (double) cmd.getProperty ("fadeOut", 0.0);
        auto transpose = (int) cmd.getProperty ("transpose", 0);
        auto curveName = cmd.getProperty ("fadeCurve", "linear").toString();
        te::AudioFadeCurve::Type curve = te::AudioFadeCurve::linear;

        if (cmd.hasProperty ("gainDb") && (ac != nullptr ? (gain < -48.0 || gain > 24.0) : (gain < -100.0 || gain > 0.0)))
            return resultObj (false, { { "error", ac != nullptr ? "gainDb must be -48..24 for audio clips"
                                                                 : juce::String::fromUTF8 ("gainDb must be -100..0 for MIDI clips (they can only get quieter — use velocity or the track fader)") } });

        if ((cmd.hasProperty ("fadeIn") || cmd.hasProperty ("fadeOut") || cmd.hasProperty ("fadeCurve") || cmd.hasProperty ("reverse")
             || cmd.hasProperty ("crossfade")) && ac == nullptr)
            return resultObj (false, { { "error", "fades, crossfade and reverse work on audio clips only" } });

        if (cmd.hasProperty ("crossfade") && ! cmd.getProperty ("crossfade", {}).isBool())
            return resultObj (false, { { "error", "crossfade must be true or false" } });

        if (fadeIn < 0.0 || fadeOut < 0.0 || (ac != nullptr && (cmd.hasProperty ("fadeIn") ? fadeIn : ac->getFadeIn().inSeconds())
                                                               + (cmd.hasProperty ("fadeOut") ? fadeOut : ac->getFadeOut().inSeconds()) > length + 1e-6))
            return resultObj (false, { { "error", "fades must be >= 0 and fit into the clip together (" + juce::String (length, 3) + " s)" } });

        if (cmd.hasProperty ("fadeCurve"))
        {
            if (curveName == "linear") curve = te::AudioFadeCurve::linear;
            else if (curveName == "convex") curve = te::AudioFadeCurve::convex;
            else if (curveName == "concave") curve = te::AudioFadeCurve::concave;
            else if (curveName == "sCurve") curve = te::AudioFadeCurve::sCurve;
            else if (curveName != "auto") return resultObj (false, { { "error", "fadeCurve must be linear, convex, concave, sCurve or auto" } });
        }

        if (cmd.hasProperty ("transpose"))
        {
           #if ! UNIDAW_WITH_RUBBERBAND
            if (ac != nullptr && transpose != 0)
                return resultObj (false, { { "error", juce::String::fromUTF8 ("transposing audio needs the time-stretcher (run tools/fetch_ml.sh and rebuild) — use a Pitch Shifter module on the track meanwhile") } });
           #endif

            if (transpose < -48 || transpose > 48)
                return resultObj (false, { { "error", "transpose must be -48..48 semitones" } });

            if (mc != nullptr)
            {
                const int delta = transpose - (int) mc->state.getProperty (transposeProp, 0);

                for (auto* n : mc->getSequence().getNotes())
                    if (n->getNoteNumber() + delta < 0 || n->getNoteNumber() + delta > 127)
                        return resultObj (false, { { "error", "transpose moves a note out of 0..127" } });
            }
        }

        // Setzen
        if (cmd.hasProperty ("gainDb") || normalized.isObject())
        {
            if (ac != nullptr)
                ac->setGainDB ((float) gain);
            else if (mc != nullptr)
                mc->setVolumeDb ((float) gain);
        }

        if (ac != nullptr)
        {
            // fadeCurve: ausdruecklich gesetzt bleibt sie; "auto" gibt sie
            // an dawd zurueck (convex, wo der Clip ueberblendet, sonst linear)
            if (cmd.hasProperty ("fadeCurve"))
            {
                setFadeCurveAuto (*ac, curveName == "auto");

                if (curveName != "auto")
                {
                    ac->setFadeInType (curve);
                    ac->setFadeOutType (curve);
                }
            }

            if (cmd.hasProperty ("fadeIn"))
                ac->setFadeIn (te::TimeDuration::fromSeconds (fadeIn));

            if (cmd.hasProperty ("fadeOut"))
                ac->setFadeOut (te::TimeDuration::fromSeconds (fadeOut));

            // Crossfade (2026-10-25): autoCrossfade der Engine — ueberlappt
            // der Clip einen Nachbarn auf der Lane, blendet er ueber die
            // Ueberlappung (statt seiner Blende auf dieser Seite); folgt
            // Verschieben und Trimmen von selbst. Echte Ueberblendung = an
            // beiden Clips
            if (cmd.hasProperty ("crossfade"))
                ac->setAutoCrossfade ((bool) cmd.getProperty ("crossfade", false));

            if (cmd.hasProperty ("reverse"))
            {
                ac->setIsReversed ((bool) cmd.getProperty ("reverse", false));

                if (! waitForClipRenders (*ctx.edit))
                    return resultObj (false, { { "error", "reversing timed out" } });
            }
        }

        // MIDI absolut wie Audio (Praxistest 1d: transpose 0 setzte nicht
        // zurueck): verschoben wird um den Unterschied zum gemerkten Stand
        if (mc != nullptr && cmd.hasProperty ("transpose"))
        {
            const int delta = transpose - (int) mc->state.getProperty (transposeProp, 0);

            for (auto* n : mc->getSequence().getNotes())
                n->setNoteNumber (n->getNoteNumber() + delta, nullptr);

            mc->state.setProperty (transposeProp, transpose, nullptr);
        }

        // Audio: Tonhoehe ueber den Timestretcher (Tempo bleibt); absolut
        if (ac != nullptr && cmd.hasProperty ("transpose"))
        {
            // Echtzeit-Stretching statt Proxy-Datei: die Proxy rendert die
            // Engine ueber Timer des Message-Threads — ein Render davor
            // wartete darauf und blockierte
            ac->setUsesProxy (false);
            ac->setPitchChange ((float) transpose);
        }

        if (ac != nullptr)
            refreshAutoCrossfades (*ctx.edit);   // gefuehrte Kurve schon in der Antwort

        saveCurrentEdit (ctx);

        // Eine Aktion je Clip (coalesce ueber track + name); der Datensatz
        // haelt den letzten Befehl
        auto params = cmd.clone();
        params.getDynamicObject()->removeProperty ("op");
        recordNodeAction (ctx, "process", "setClip", params, {}, true);

        auto res = resultObj (true, { { "track", trackIndex }, { "name", name } });
        auto* o = res.getDynamicObject();

        if (ac != nullptr)
        {
            o->setProperty ("gainDb", std::round (ac->getGainDB() * 100.0) / 100.0);
            o->setProperty ("fadeIn", ac->getFadeIn().inSeconds());
            o->setProperty ("fadeOut", ac->getFadeOut().inSeconds());
            o->setProperty ("fadeCurve", fadeCurveName (ac->getFadeInType()));
            o->setProperty ("fadeCurveAuto", isFadeCurveAuto (*ac));
            o->setProperty ("reversed", ac->getIsReversed());
            o->setProperty ("transpose", (double) ac->getPitchChange());
            o->setProperty ("crossfade", ac->getAutoCrossfade());

            if (normalized.isObject())
                o->setProperty ("normalized", normalized);
        }
        else if (mc != nullptr)
        {
            o->setProperty ("gainDb", std::round (mc->getVolumeDb() * 100.0) / 100.0);
            o->setProperty ("transpose", (int) mc->state.getProperty (transposeProp, 0));

            // MIDI-Clip-Gain skaliert nur die Anschlaege (Engine: MidiNode,
            // multiplyVelocity) — wie viel leiser es wird, entscheidet das
            // Instrument (Praxistest 3: -13 dB am Pad gaben -0,3 LU; 4OSC Amp
            // Velocity 30 % = hoechstens -3 dB, Surge ohne Velocity->VCA kaum
            // etwas, Sampler hoechstens -20 dB)
            if (cmd.hasProperty ("gainDb") && gain < 0.0)
                o->setProperty ("hint", midiGainHint (*ctx.edit, *tracks[trackIndex], gain));
        }

        return res;
    }

    // Audio-Clip im Tempo aendern (Rubberband): ratio = Abspielgeschwindigkeit
    // (2 = doppelt so schnell; der Clip deckt danach dasselbe Material ab),
    // followTempo = der Clip folgt dem Tempo der Section(s) — sourceBpm ist
    // das Tempo der Aufnahme (ohne: detectTempo misst es). mode melodic
    // (Default) | percussive. Der Clip darf ueber das Section-Ende wachsen
    // (Auto-Section hinter der letzten).
    juce::var opStretchClip (DawdContext& ctx, const juce::var& cmd)
    {
       #if ! UNIDAW_WITH_RUBBERBAND
        juce::ignoreUnused (cmd);
        return resultObj (false, { { "error", juce::String::fromUTF8 ("unidaw was built without Rubberband — run tools/fetch_ml.sh, then rebuild (bash tests/run.sh)") } });
       #else
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* ac = dynamic_cast<te::AudioClipBase*> (findClipByName (*tracks[trackIndex], name));

        if (ac == nullptr)
            return resultObj (false, { { "error", "no such audio clip: " + name } });

        // Warp-Marker legen das Tempo selbst fest; einen Loop rechnet die
        // Engine beim Tempowechsel nicht richtig um (loopClip)
        if (isWarped (*ac))
            return resultObj (false, { { "error", "clip is warped - clear the warp markers first (warpClip clear: true)" } });

        if (ac->isLooping())
            return resultObj (false, { { "error", "clip loops - turn the loop off first (loopClip on: false)" } });

        const bool follow = (bool) cmd.getProperty ("followTempo", false);

        if (follow == cmd.hasProperty ("ratio"))
            return resultObj (false, { { "error", "need ratio (playback speed) or followTempo: true" } });

        auto ratio = (double) cmd.getProperty ("ratio", 1.0);

        if (! follow && (ratio < 0.25 || ratio > 4.0))
            return resultObj (false, { { "error", "ratio must be 0.25..4" } });

        auto mode = cmd.getProperty ("mode", "melodic").toString();

        if (mode != "melodic" && mode != "percussive")
            return resultObj (false, { { "error", "mode must be melodic or percussive" } });

        auto file = ac->getOriginalFile();
        te::AudioFile af (*ctx.engine, file);

        if (! af.isValid())
            return resultObj (false, { { "error", "cannot read the clip's audio" } });

        double sourceBpm = (double) cmd.getProperty ("sourceBpm", 0.0);

        if (follow && cmd.hasProperty ("sourceBpm") && (sourceBpm < 20.0 || sourceBpm > 300.0))
            return resultObj (false, { { "error", "sourceBpm must be 20..300" } });

        if (follow && sourceBpm <= 0.0)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));

            if (reader == nullptr)
                return resultObj (false, { { "error", "cannot read the clip's audio" } });

            auto tempo = analysis::detectTempo (analysis::computeFeatures (analysis::loadMono (*reader, 0, reader->lengthInSamples)));

            if (! tempo.valid)
                return resultObj (false, { { "error", juce::String::fromUTF8 ("no steady pulse in the clip — pass sourceBpm") } });

            sourceBpm = std::round (tempo.bpm * 10.0) / 10.0;
        }

        auto pos = ac->getPosition();
        auto start = pos.getStart().inSeconds();
        ac->setUsesProxy (false);   // Echtzeit-Stretching (siehe setClip transpose)
        ac->setTimeStretchMode (mode == "percussive" ? te::TimeStretcher::rubberbandPercussive : te::TimeStretcher::rubberbandMelodic);
        double newLength = pos.getLength().inSeconds();

        if (follow)
        {
            // Material in Beats der Aufnahme -> Laenge im Tempo des Songs
            auto materialBeats = (pos.getLength().inSeconds() * ac->getSpeedRatio()) * sourceBpm / 60.0;
            ac->getLoopInfo().setBpm (sourceBpm, af.getInfo());
            ac->setAutoTempo (true);
            newLength = beatsToSeconds (*ctx.edit, start, materialBeats);
        }
        else
        {
            newLength = pos.getLength().inSeconds() * ac->getSpeedRatio() / ratio;
            ac->setSpeedRatio (ratio);
        }

        ac->setLength (te::TimeDuration::fromSeconds (newLength), true);

        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = cmd.clone();
        params.getDynamicObject()->removeProperty ("op");
        params.getDynamicObject()->setProperty ("sourceBpm", sourceBpm);
        recordNodeAction (ctx, "process", "stretchClip", params, {}, true);
        recordAutoSection (ctx, newSection);

        auto res = resultObj (true, { { "track", trackIndex }, { "name", name },
                                      { "length", ac->getPosition().getLength().inSeconds() },
                                      { "followTempo", ac->getAutoTempo() }, { "mode", mode } });

        if (follow)
            res.getDynamicObject()->setProperty ("sourceBpm", sourceBpm);
        else
            res.getDynamicObject()->setProperty ("ratio", ac->getSpeedRatio());

        return withAutoSection (res, newSection);
       #endif
    }

    namespace
    {
        // Datei, die der Clip spielt
        void setClipFile (DawdContext& ctx, te::AudioClipBase& ac, const juce::File& f)
        {
            ac.getSourceFileReference().setToFile (f, te::SourceFileReference::PathStyle::chooseBest, true);
            ctx.engine->getAudioFileManager().validateFile (te::AudioFile (*ctx.engine, f), true);
        }

        // Stelle im Clip (Sekunden ab Start), an der die Quellsekunde
        // spielt — Umkehrung von clipSourceTime fuer Clips ohne Loop
        double clipTimeOfSource (te::AudioClipBase& ac, double source)
        {
            double lo = 0.0, hi = 1.0;

            if (clipSourceTime (ac, lo) >= source)
                return 0.0;

            while (clipSourceTime (ac, hi) < source && hi < 1.0e5)
                hi *= 2.0;

            for (int i = 0; i < 60; ++i)
            {
                auto mid = 0.5 * (lo + hi);
                (clipSourceTime (ac, mid) < source ? lo : hi) = mid;
            }

            return 0.5 * (lo + hi);
        }

        juce::String fmt3 (double v)    { return juce::String (v, 3); }

        bool isNumber (const juce::var& v)  { return v.isDouble() || v.isInt() || v.isInt64(); }
    }

    // Warp-Marker (2026-10-26): Quellsekunden des Audios an Beats ab dem
    // Clip-Start heften; zwischen den Markern streckt Rubberband beim
    // Abspielen in Echtzeit (seit 2026-10-31: Patch tracktion-warp-realtime,
    // ein Stretcher, kein Render, keine Datei). Der Clip folgt danach dem Tempo
    // (followTempo): Tempo des Materials = Mittel zwischen erstem und letztem
    // Marker, dort liegen Quelle und Warp gleich, ausserhalb spielt es in
    // diesem Tempo. markers ersetzt alle, detect setzt sie aus den Beats von
    // detectTempo, clear nimmt sie weg (der Clip folgt weiter dem Tempo).
    // Laenge: dasselbe Material wie vorher.
    juce::var opWarpClip (DawdContext& ctx, const juce::var& cmd)
    {
       #if ! UNIDAW_WITH_RUBBERBAND
        juce::ignoreUnused (ctx, cmd);
        return resultObj (false, { { "error", juce::String::fromUTF8 ("unidaw was built without Rubberband — run tools/fetch_ml.sh, then rebuild (bash tests/run.sh)") } });
       #else
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* ac = dynamic_cast<te::AudioClipBase*> (findClipByName (*tracks[trackIndex], name));

        if (ac == nullptr)
            return resultObj (false, { { "error", "no such audio clip: " + name } });

        const bool clear = (bool) cmd.getProperty ("clear", false);
        const bool detect = (bool) cmd.getProperty ("detect", false);

        if ((int) clear + (int) detect + (int) cmd.hasProperty ("markers") != 1)
            return resultObj (false, { { "error", "need markers: [{source, beat}], detect: true or clear: true" } });

        if (ac->getIsReversed())
            return resultObj (false, { { "error", "clip is reversed - warp markers need forward audio (setClip reverse: false first)" } });

        if (ac->isLooping())
            return resultObj (false, { { "error", "clip loops - turn the loop off first (loopClip on: false), warp, then loop again" } });

        auto mode = cmd.getProperty ("mode", {}).toString();

        if (cmd.hasProperty ("mode") && mode != "melodic" && mode != "percussive")
            return resultObj (false, { { "error", "mode must be melodic or percussive" } });

        if (cmd.hasProperty ("sourceBpm") && (! isNumber (cmd.getProperty ("sourceBpm", {}))
                                              || (double) cmd.getProperty ("sourceBpm", 0.0) < 30.0 || (double) cmd.getProperty ("sourceBpm", 0.0) > 300.0))
            return resultObj (false, { { "error", "sourceBpm must be 30..300" } });

        auto file = unwarpedFile (*ac);   // Marker zeigen immer auf die Originaldatei
        te::AudioFile af (*ctx.engine, file);

        if (! af.isValid())
            return resultObj (false, { { "error", "cannot read the clip's audio" } });

        const double fileLen = af.getLength();
        const auto pos = ac->getPosition();
        const double start = pos.getStart().inSeconds();
        const double fromSource = juce::jlimit (0.0, fileLen, clipSourceTime (*ac, 0.0));
        const double endSource = juce::jlimit (0.0, fileLen, clipSourceTime (*ac, pos.getLength().inSeconds()));

        auto detectedTempo = [&] () -> analysis::TempoResult
        {
            std::unique_ptr<juce::AudioFormatReader> reader (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));

            if (reader == nullptr)
                return {};

            return analysis::detectTempo (analysis::computeFeatures (analysis::loadMono (*reader, 0, reader->lengthInSamples)));
        };

        // Clip auf das Material legen: offsetBeats = Beats des Materials vor
        // dem Clip-Start, Ende = wo das alte End-Material jetzt liegt
        auto place = [&] (double offsetBeats, double bpm)
        {
            auto bps = ctx.edit->tempoSequence.getBeatsPerSecondAt (pos.getStart());
            auto newLen = pos.getLength().inSeconds();
            auto endBeat = sourceToWarp (*ac, endSource) * bpm / 60.0 - offsetBeats;

            if (endBeat > 1.0e-3)
                newLen = beatsToSeconds (*ctx.edit, start, endBeat);

            ac->setPosition ({ { pos.getStart(), pos.getStart() + te::TimeDuration::fromSeconds (newLen) },
                               te::TimeDuration::fromSeconds (offsetBeats / bps) });
        };

        double bpm = 0.0;
        juce::var clamped;   // clamp: der Marker, der an die Grenze rückte

        if (clear)
        {
            if (! isWarped (*ac))
                return resultObj (false, { { "error", "clip has no warp markers" } });

            bpm = ac->getLoopInfo().getBpm (af.getInfo());
            setClipFile (ctx, *ac, file);
            ac->state.removeChild (ac->state.getChildWithName (warpTreeId), nullptr);
            ac->getLoopInfo().setBpm (bpm, af.getInfo());
            place (fromSource * bpm / 60.0, bpm);
        }
        else
        {
            // Nutzer-Marker (Quelle, Beat ab Clip-Start)
            std::vector<std::pair<double, double>> user;

            if (detect)
            {
                auto tempo = detectedTempo();

                if (! tempo.valid || tempo.beats.size() < 2)
                    return resultObj (false, { { "error", juce::String::fromUTF8 ("no steady pulse in the clip — set markers yourself") } });

                // Der erste Beat im Clip bleibt, wo er liegt (auf den
                // naechsten ganzen Beat ab Clip-Start), jeder weitere einen
                // Beat spaeter
                size_t k0 = 0;

                while (k0 < tempo.beats.size() && tempo.beats[k0] < fromSource - 0.01)
                    ++k0;

                if (k0 >= tempo.beats.size())
                    return resultObj (false, { { "error", "no beat inside the clip" } });

                auto b0 = std::round (secondsToBeats (*ctx.edit, start, clipTimeOfSource (*ac, tempo.beats[k0])));

                for (size_t k = 0; k < tempo.beats.size(); ++k)
                    user.push_back ({ tempo.beats[k], b0 + (double) k - (double) k0 });
            }
            else
            {
                auto* arr = cmd.getProperty ("markers", {}).getArray();
                const juce::String shape ("markers must be a list of {source, beat} (source: seconds in the audio file, beat: beats from the clip start)");

                if (arr == nullptr || arr->isEmpty())
                    return resultObj (false, { { "error", shape } });

                for (auto& m : *arr)
                {
                    if (m.getDynamicObject() == nullptr || ! isNumber (m.getProperty ("source", {})) || ! isNumber (m.getProperty ("beat", {})))
                        return resultObj (false, { { "error", shape } });

                    auto s = (double) m.getProperty ("source", 0.0);

                    if (s < 0.0 || s > fileLen + 1.0e-6)
                        return resultObj (false, { { "error", "marker source must lie inside the audio file (0.." + fmt3 (fileLen) + " s)" } });

                    user.push_back ({ juce::jmin (s, fileLen), (double) m.getProperty ("beat", 0.0) });
                }

                std::sort (user.begin(), user.end());
            }

            // Regeln einer Liste ab zwei Markern: steigend, Tempo des Materials
            // (Mittel erster bis letzter Marker) 30..1000 BPM, zwischen
            // Nachbarn 0,25..4x, Material nicht erst nach dem Clip-Start
            auto rulesError = [] (const std::vector<std::pair<double, double>>& u) -> juce::String
            {
                for (size_t i = 1; i < u.size(); ++i)
                    if (u[i].first - u[i - 1].first < 1.0e-4 || u[i].second <= u[i - 1].second)
                        return "markers must rise together: a later source time needs a later beat (at " + fmt3 (u[i].first) + " s)";

                if (u.size() < 2)
                    return {};

                auto b = 60.0 * (u.back().second - u.front().second) / (u.back().first - u.front().first);

                if (b < 30.0 || b > 1000.0)
                    return "the markers give the audio " + juce::String (b, 1) + " BPM (first to last marker) - must be 30..1000";

                for (size_t i = 1; i < u.size(); ++i)
                {
                    auto speed = (u[i].first - u[i - 1].first) / ((u[i].second - u[i - 1].second) * 60.0 / b);

                    if (speed < 0.25 || speed > 4.0)
                        return "between the markers at " + fmt3 (u[i - 1].first) + " s and " + fmt3 (u[i].first)
                               + " s the audio would play at " + juce::String (speed, 2) + "x its tempo - allowed is 0.25..4";
                }

                if (u.front().first * b / 60.0 - u.front().second < -1.0e-6)
                    return "the audio would start after the clip start (marker at " + fmt3 (u.front().first) + " s on beat "
                           + juce::String (u.front().second, 3) + ") - lower the beats or move the clip later (moveClip)";

                return {};
            };

            // clamp: ein gezogener Marker (gleiche Quelle, anderer Beat als
            // gespeichert) über die Grenze rückt an sie heran statt abzulehnen
            // (Flow-UI; Handprüfung 2026-10-31) — gesucht zwischen altem und
            // gewünschtem Beat, die Regel bleibt hier
            if ((bool) cmd.getProperty ("clamp", false) && ! detect && rulesError (user).isNotEmpty())
            {
                auto old = warpMarkersOf (*ac);
                int moved = -1;
                double oldBeat = 0.0;

                if (old.size() == (int) user.size())
                    for (size_t i = 0; i < user.size(); ++i)
                    {
                        auto ob = (double) old[(int) i].getProperty ("beat", 0.0);

                        if (std::abs ((double) old[(int) i].getProperty ("source", 0.0) - user[i].first) > 2.0e-3)
                        {
                            moved = -2;
                            break;
                        }

                        if (std::abs (ob - user[i].second) > 1.0e-3)
                        {
                            if (moved != -1) { moved = -2; break; }
                            moved = (int) i;
                            oldBeat = ob;
                        }
                    }

                if (moved >= 0)
                {
                    auto want = user[(size_t) moved].second;
                    auto at = [&] (double t) { auto u = user; u[(size_t) moved].second = oldBeat + t * (want - oldBeat); return u; };

                    if (rulesError (at (0.0)).isEmpty())
                    {
                        double lo = 0.0, hi = 1.0;

                        for (int k = 0; k < 40; ++k)
                        {
                            auto mid = 0.5 * (lo + hi);
                            (rulesError (at (mid)).isEmpty() ? lo : hi) = mid;
                        }

                        // auf 1/1000 Beat, zur alten Stelle hin (bleibt erlaubt)
                        auto b = oldBeat + lo * (want - oldBeat);
                        b = want > oldBeat ? std::floor (b * 1000.0) / 1000.0 : std::ceil (b * 1000.0) / 1000.0;
                        user[(size_t) moved].second = b;
                        clamped = juce::var (new juce::DynamicObject());
                        clamped.getDynamicObject()->setProperty ("source", fmt3 (user[(size_t) moved].first).getDoubleValue());
                        clamped.getDynamicObject()->setProperty ("beat", b);
                        clamped.getDynamicObject()->setProperty ("asked", want);
                    }
                }
            }

            if (auto err = rulesError (user); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            // Tempo des Materials: Mittel zwischen erstem und letztem Marker;
            // ein Marker allein: sourceBpm, das bisherige Tempo oder gemessen
            if (user.size() >= 2)
                bpm = 60.0 * (user.back().second - user.front().second) / (user.back().first - user.front().first);
            else if (cmd.hasProperty ("sourceBpm"))
                bpm = (double) cmd.getProperty ("sourceBpm", 0.0);
            else if (ac->getAutoTempo())
                bpm = ac->getLoopInfo().getBpm (af.getInfo());
            else if (auto tempo = detectedTempo(); tempo.valid)
                bpm = std::round (tempo.bpm * 10.0) / 10.0;
            else
                return resultObj (false, { { "error", "one marker needs the tempo of the audio: pass sourceBpm (no steady pulse found)" } });

            if (bpm < 30.0 || bpm > 1000.0)
                return resultObj (false, { { "error", "the markers give the audio " + juce::String (bpm, 1) + " BPM (first to last marker) - must be 30..1000" } });

            // Beats des Materials vor dem Clip-Start: der erste Marker liegt
            // auf seiner Quellsekunde (Tempo 1 davor bis zum Dateianfang)
            auto offsetBeats = user.front().first * bpm / 60.0 - user.front().second;

            if (offsetBeats < -1.0e-6)
                return resultObj (false, { { "error", "the audio would start after the clip start (marker at " + fmt3 (user.front().first) + " s on beat "
                                                      + juce::String (user.front().second, 3) + ") - lower the beats or move the clip later (moveClip)" } });

            offsetBeats = juce::jmax (0.0, offsetBeats);
            std::vector<std::pair<double, double>> warp;

            for (auto& [source, beat] : user)
                warp.push_back ({ source, (beat + offsetBeats) * 60.0 / bpm });

            // Abgespielt wird die Originaldatei wie ein Clip mit followTempo;
            // die Engine fuehrt sie in Echtzeit durch die Marker
            // (getWarpPointsForClip): ein Stretcher, sein Verhaeltnis je
            // eingespeistem Stueck aus der Marker-Map, +-5 ms wie der fruehere
            // Offline-Render. Die WarpMap der Engine (zweiter Stretcher, Tempo
            // je Block) traf die Beats nur auf +-20 ms bzw. ~90 ms
            if (mode.isNotEmpty())
                ac->setTimeStretchMode (mode == "percussive" ? te::TimeStretcher::rubberbandPercussive : te::TimeStretcher::rubberbandMelodic);
            else if (ac->getTimeStretchMode() != te::TimeStretcher::rubberbandPercussive)
                ac->setTimeStretchMode (te::TimeStretcher::rubberbandMelodic);

            ac->setUsesProxy (false);   // Echtzeit-Stretching (siehe stretchClip)

            if (! ac->getAutoTempo())
                ac->setSpeedRatio (1.0);

            if (ac->getOriginalFile() != file)
                setClipFile (ctx, *ac, file);

            ac->state.removeChild (ac->state.getChildWithName (warpTreeId), nullptr);
            juce::ValueTree tree (warpTreeId);
            tree.setProperty ("source", file.getFullPathName(), nullptr);
            tree.setProperty ("length", fileLen, nullptr);
            tree.setProperty ("realtime", true, nullptr);

            for (auto& [source, w] : warp)
            {
                juce::ValueTree m ("MARKER");
                m.setProperty ("source", source, nullptr);
                m.setProperty ("warp", w, nullptr);
                tree.appendChild (m, nullptr);
            }

            ac->state.appendChild (tree, nullptr);
            ac->getLoopInfo().setBpm (bpm, af.getInfo());
            ac->setAutoTempo (true);
            place (offsetBeats, bpm);
        }

        // DAWDWARP aendert die Engine nicht von selbst am Graphen: neu bauen,
        // die Wiedergabe spielt sofort mit den neuen Markern weiter
        ctx.edit->restartPlayback();

        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = cmd.clone();
        params.getDynamicObject()->removeProperty ("op");
        recordNodeAction (ctx, "process", "warpClip", params, {}, true);
        recordAutoSection (ctx, newSection);

        auto newPos = ac->getPosition();
        auto res = resultObj (true, { { "track", trackIndex }, { "name", name }, { "warped", isWarped (*ac) },
                                      { "markers", warpMarkersOf (*ac) }, { "sourceBpm", std::round (bpm * 100.0) / 100.0 },
                                      { "start", newPos.getStart().inSeconds() }, { "length", newPos.getLength().inSeconds() } });

        if (clamped.isObject())
            res.getDynamicObject()->setProperty ("clamped", clamped);

        return withAutoSection (res, newSection);
       #endif
    }

    // Loop im Audio-Clip (2026-10-26): Loop-Bereich in Sekunden der Datei
    // (start/length) oder bei Clips, die dem Tempo folgen, in Beats des
    // Materials (startBeat/lengthBeats); offset = wo im Loop der Clip
    // beginnt; count = Laenge des Clips in Loop-Durchgaengen. Ohne Bereich:
    // der bisherige Loop bzw. das, was der Clip zeigt. on: false schaltet ab
    // (der Clip zeigt dann einen Durchgang). Zeit-Weg nur mit speed 1: die
    // Engine rechnet den Loop-Start dort nicht mit dem Tempo um.
    juce::var opLoopClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* ac = dynamic_cast<te::AudioClipBase*> (findClipByName (*tracks[trackIndex], name));

        if (ac == nullptr)
            return resultObj (false, { { "error", "no such audio clip: " + name } });

        auto has = [&] (const char* p) { return cmd.hasProperty (p); };
        const auto pos = ac->getPosition();
        const double start = pos.getStart().inSeconds();

        if (! (bool) cmd.getProperty ("on", true))
        {
            if (! ac->isLooping())
                return resultObj (false, { { "error", "clip does not loop" } });

            ac->disableLooping();
        }
        else
        {
            const bool beats = ac->getAutoTempo();

            if (ac->getIsReversed())
                return resultObj (false, { { "error", "clip is reversed - loops need forward audio (setClip reverse: false first)" } });

            if (! beats && std::abs (ac->getSpeedRatio() - 1.0) > 1.0e-6)
                return resultObj (false, { { "error", "clip plays at speed " + juce::String (ac->getSpeedRatio(), 3)
                                                      + " (stretchClip ratio) - a loop needs speed 1 or a clip that follows the tempo (stretchClip followTempo)" } });

            if ((has ("start") || has ("length") || has ("offset")) && (has ("startBeat") || has ("lengthBeats") || has ("offsetBeats")))
                return resultObj (false, { { "error", "use start/length/offset (seconds of the audio file) or startBeat/lengthBeats/offsetBeats, not both" } });

            if (! beats && (has ("startBeat") || has ("lengthBeats") || has ("offsetBeats")))
                return resultObj (false, { { "error", "startBeat/lengthBeats/offsetBeats need a clip that follows the tempo (stretchClip followTempo or warpClip) - use start/length/offset in seconds" } });

            for (auto* p : { "start", "length", "offset", "startBeat", "lengthBeats", "offsetBeats", "count" })
                if (has (p) && ! isNumber (cmd.getProperty (p, {})))
                    return resultObj (false, { { "error", juce::String (p) + " must be a number" } });

            te::AudioFile af (*ctx.engine, ac->getOriginalFile());

            if (! af.isValid())
                return resultObj (false, { { "error", "cannot read the clip's audio" } });

            const double fileLen = af.getLength();
            const double bpm = beats ? ac->getLoopInfo().getBpm (af.getInfo()) : 0.0;
            const double numBeats = sourceToWarp (*ac, fileLen) * bpm / 60.0;

            // Einheit: Sekunden der Datei (Zeit-Weg) bzw. Beats des Materials
            auto toUnit = [&] (double source) { return beats ? sourceToWarp (*ac, source) * bpm / 60.0 : source; };
            auto toSource = [&] (double unit) { return beats ? warpToSource (*ac, unit * 60.0 / bpm) : unit; };

            double ls = 0.0, ll = 0.0, off = 0.0;

            if (ac->isLooping())
            {
                ls = beats ? ac->getLoopStartBeats().inBeats() : ac->getLoopStart().inSeconds();
                ll = beats ? ac->getLoopLengthBeats().inBeats() : ac->getLoopLength().inSeconds();
                off = beats ? ac->getOffsetInBeats().inBeats() : pos.getOffset().inSeconds();
            }
            else
            {
                // was der Clip zeigt
                auto a = juce::jlimit (0.0, fileLen, clipSourceTime (*ac, 0.0));
                auto b = juce::jlimit (0.0, fileLen, clipSourceTime (*ac, pos.getLength().inSeconds()));
                ls = toUnit (a);
                ll = toUnit (b) - ls;
            }

            if (has ("startBeat") || has ("lengthBeats"))
            {
                auto keepEnd = ls + ll;
                ls = (double) cmd.getProperty ("startBeat", ls);
                ll = has ("lengthBeats") ? (double) cmd.getProperty ("lengthBeats", 0.0) : keepEnd - ls;
            }
            else if (has ("start") || has ("length"))
            {
                auto srcStart = has ("start") ? (double) cmd.getProperty ("start", 0.0) : toSource (ls);
                auto srcLen = has ("length") ? (double) cmd.getProperty ("length", 0.0) : toSource (ls + ll) - srcStart;

                if (srcStart < 0.0 || srcLen <= 0.0 || srcStart + srcLen > fileLen + 1.0e-6)
                    return resultObj (false, { { "error", "loop must lie inside the audio file (0.." + fmt3 (fileLen) + " s) with length > 0" } });

                ls = toUnit (srcStart);
                ll = toUnit (juce::jmin (fileLen, srcStart + srcLen)) - ls;
            }

            if (beats && (ls < -1.0e-6 || ll <= 0.0 || ls + ll > numBeats + 1.0e-3))
                return resultObj (false, { { "error", "loop must lie inside the material (0.." + juce::String (numBeats, 3) + " beats) with length > 0" } });

            if (! beats && (ls < 0.0 || ll <= 0.0 || ls + ll > fileLen + 1.0e-6))
                return resultObj (false, { { "error", "loop must lie inside the audio file (0.." + fmt3 (fileLen) + " s) with length > 0" } });

            if (has ("offsetBeats"))
                off = (double) cmd.getProperty ("offsetBeats", 0.0);
            else if (has ("offset"))
                off = toUnit (toSource (ls) + (double) cmd.getProperty ("offset", 0.0)) - ls;
            else if (! ac->isLooping())
                off = 0.0;

            if (off < -1.0e-6 || off >= ll - 1.0e-6)
                return resultObj (false, { { "error", "offset must lie inside the loop (0.." + (beats ? juce::String (ll, 3) + " beats)" : fmt3 (ll) + " s)") } });

            auto count = (double) cmd.getProperty ("count", 0.0);

            if (has ("count") && count <= 0.0)
                return resultObj (false, { { "error", "count must be > 0" } });

            auto length = pos.getLength().inSeconds();
            off = juce::jmax (0.0, off);

            if (beats)
            {
                ac->setLoopRangeBeats ({ te::BeatPosition::fromBeats (ls), te::BeatPosition::fromBeats (ls + ll) });
                auto bps = ctx.edit->tempoSequence.getBeatsPerSecondAt (pos.getStart());

                if (has ("count"))
                    length = beatsToSeconds (*ctx.edit, start, count * ll);

                ac->setPosition ({ { pos.getStart(), pos.getStart() + te::TimeDuration::fromSeconds (length) }, te::TimeDuration::fromSeconds (off / bps) });
            }
            else
            {
                ac->setLoopRange ({ te::TimePosition::fromSeconds (ls), te::TimePosition::fromSeconds (ls + ll) });

                if (has ("count"))
                    length = count * ll;

                ac->setPosition ({ { pos.getStart(), pos.getStart() + te::TimeDuration::fromSeconds (length) }, te::TimeDuration::fromSeconds (off) });
            }
        }

        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        auto params = cmd.clone();
        params.getDynamicObject()->removeProperty ("op");
        recordNodeAction (ctx, "process", "loopClip", params, {}, true);
        recordAutoSection (ctx, newSection);

        auto newPos = ac->getPosition();
        auto res = resultObj (true, { { "track", trackIndex }, { "name", name }, { "looping", ac->isLooping() },
                                      { "start", newPos.getStart().inSeconds() }, { "length", newPos.getLength().inSeconds() } });

        if (ac->isLooping())
            res.getDynamicObject()->setProperty ("loop", clipLoopOf (*ac));

        return withAutoSection (res, newSection);
    }
}

