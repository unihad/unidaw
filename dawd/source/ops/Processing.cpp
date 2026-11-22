#include "Ops.h"
#include "../Origins.h"
#include "AnalysisSource.h"
#include "../EngineHelpers.h"
#include "../Transcription.h"
#include "../VoiceAnalysis.h"
#include "../SampleLibrary.h"
#include "../PluginSettings.h"
#include "../Limiter.h"
#include "../Sidechain.h"
#include "../MusicAnalysis.h"
#include "../PluginProfiles.h"
#include "../Presets.h"
#include "../PluginWindows.h"
#include "../PluginSandbox.h"

#include <map>

// Verarbeitung am Knoten: Plugins, Parameter, Render und Analyse. Kommende
// KI-Optionen (Bounce, Quantize, Stems ...) landen hier als weitere Handler.
namespace dawd
{
    namespace
    {
        // Render-Optionen aus dem Befehl in die Renderer-Parameter: Format
        // (Name oder Dateiendung), Samplerate, Bittiefe, Dither, Bereich
        // (section oder start/end), Ausklang, Normalisieren, Stille trimmen.
        // Ohne Optionen wie bisher: WAV, 24 Bit, 44,1 kHz, ganze Laenge.
        // Rueckgabe: Fehlertext oder "" (API-Vertrag).
        juce::String applyRenderOptions (DawdContext& ctx, const juce::var& cmd, te::Renderer::Parameters& r)
        {
            auto& fm = ctx.engine->getAudioFileFormatManager();
            auto formatName = cmd.getProperty ("format", {}).toString().toLowerCase();

            if (formatName.isEmpty())
                formatName = r.destFile.getFileExtension().trimCharactersAtStart (".").toLowerCase();

            if (formatName.isEmpty() || formatName == "wave")
                formatName = "wav";

            if (formatName == "aif")
                formatName = "aiff";

            juce::AudioFormat* format = formatName == "wav"  ? fm.getWavFormat()
                                      : formatName == "aiff" ? fm.getAiffFormat()
                                      : formatName == "flac" ? fm.getFlacFormat()
                                      : formatName == "ogg"  ? fm.getOggFormat()
                                      : nullptr;

            if (formatName == "mp3")
                return "mp3 needs the LAME encoder, which is not installed; use wav, aiff, flac or ogg";

            if (format == nullptr)
                return "format must be wav, aiff, flac or ogg";

            auto sampleRate = (int) (double) cmd.getProperty ("sampleRate", 44100.0);

            if (sampleRate != 44100 && sampleRate != 48000 && sampleRate != 88200 && sampleRate != 96000)
                return "sampleRate must be 44100, 48000, 88200 or 96000";

            if (! format->getPossibleSampleRates().isEmpty() && ! format->getPossibleSampleRates().contains (sampleRate))
                return "sampleRate " + juce::String (sampleRate) + " is not supported by " + formatName;

            // Default 24 Bit, sonst die Tiefe des Formats (ogg kennt nur 32)
            auto depths = format->getPossibleBitDepths();
            auto bitDepth = cmd.hasProperty ("bitDepth") ? (int) (double) cmd.getProperty ("bitDepth", 24.0)
                                                         : (depths.contains (24) ? 24 : depths.getLast());

            if (! depths.contains (bitDepth))
            {
                juce::StringArray ds;

                for (auto d : depths)
                    ds.add (juce::String (d));

                return "bitDepth must be " + ds.joinIntoString (", ") + " for " + formatName;
            }

            r.audioFormat = format;
            r.sampleRateForAudio = sampleRate;
            r.bitDepth = bitDepth;
            r.ditheringEnabled = (bool) cmd.getProperty ("dither", bitDepth == 16);

            // Bereich: ganze Laenge, eine Section oder start/end in Sekunden —
            // im Edit des Renders (auch ein anderer Node: renderNode, renderBatch)
            auto& edit = r.edit != nullptr ? *r.edit : *ctx.edit;
            r.time = { 0_tp, edit.getLength() };

            if (cmd.hasProperty ("section"))
            {
                auto name = cmd.getProperty ("section", {}).toString();
                auto* sec = findSection (edit, name);

                if (sec == nullptr)
                    return "no such section: " + name;

                r.time = sec->getPosition().time;

                // start/end dazu: Sekunden ab Beginn der Section (Praxistest
                // 1f: describeVoice {section, track, start, end} nahm still
                // die ganze Section)
                if (cmd.hasProperty ("start") || cmd.hasProperty ("end"))
                {
                    const double s0 = r.time.getStart().inSeconds(), len = r.time.getLength().inSeconds();
                    const double a = (double) cmd.getProperty ("start", 0.0), e = (double) cmd.getProperty ("end", len);

                    if (a < 0.0 || e <= a || a >= len)
                        return "with section, start/end are seconds from the section start: need 0 <= start < end, start < " + juce::String (len, 3);

                    r.time = { te::TimePosition::fromSeconds (s0 + a), te::TimePosition::fromSeconds (s0 + juce::jmin (e, len)) };
                }
            }
            else if (cmd.hasProperty ("start") || cmd.hasProperty ("end"))
            {
                auto start = (double) cmd.getProperty ("start", 0.0);
                auto end = (double) cmd.getProperty ("end", edit.getLength().inSeconds());

                if (start < 0.0 || end <= start)
                    return "need 0 <= start < end";

                r.time = { te::TimePosition::fromSeconds (start), te::TimePosition::fromSeconds (end) };
            }

            // Ausklang hinter dem Bereich (Hall, Delays)
            auto tail = (double) cmd.getProperty ("tail", 0.0);

            if (tail < 0.0 || tail > 30.0)
                return "tail must be 0..30 s";

            // endAllowance greift im Datei-Render nicht — Bereich selbst verlaengern
            r.time = r.time.withEnd (r.time.getEnd() + te::TimeDuration::fromSeconds (tail));

            auto normalize = cmd.getProperty ("normalize", {}).toString();

            if (normalize == "peak")
            {
                r.shouldNormalise = true;
                r.normaliseToLevelDb = (float) (double) cmd.getProperty ("level", -1.0);
            }
            else if (normalize == "lufs")
            {
                r.shouldNormaliseByLUFS = true;
                r.normaliseToLevelDb = (float) (double) cmd.getProperty ("level", -14.0);
                r.limitTruePeak = true;
                r.truePeakCeilingDb = -1.0f;
            }
            else if (normalize.isNotEmpty())
            {
                return "normalize must be peak or lufs";
            }
            else if (cmd.hasProperty ("level"))
            {
                // Frueher still uebergangen (Praxistest 1c: level -6.9 ohne
                // normalize ergab -8,6 LUFS)
                return juce::String::fromUTF8 ("level needs normalize: \"lufs\" (integrated loudness, true peak <= -1 dB) or \"peak\" (sample peak)");
            }

            if (normalize.isNotEmpty() && (r.normaliseToLevelDb > 0.0f || r.normaliseToLevelDb < -60.0f))
                return "level must be -60..0";

            r.trimSilenceAtEnds = (bool) cmd.getProperty ("trimSilence", false);
            r.checkNodesForAudio = ! (bool) cmd.getProperty ("force", false);
            return {};
        }

        // Rendern mit fertigen Parametern; Fehlertext oder ""
        juce::String runRender (DawdContext& ctx, te::Renderer::Parameters& r)
        {
            // Der Writer haengt an eine vorhandene Datei an (der WAV-Header zeigt
            // dann weiter den alten Inhalt) — ein zweiter Export derselben Node
            // wuerde den alten Stand liefern
            if (r.destFile.existsAsFile() && ! r.destFile.deleteFile())
                return "cannot overwrite " + r.destFile.getFullPathName();

            if (! r.destFile.getParentDirectory().createDirectory().wasOk())
                return "cannot create " + r.destFile.getParentDirectory().getFullPathName();

            juce::String error = "render failed";

            // ScopedRenderStatus wie in renderToFile: gibt den Playback-
            // Kontext frei, bevor der Render-Graph gebaut wird (sonst
            // "Rendering whilst attached to audio device" — das Device
            // bleibt danach stumm), und holt ihn danach zurueck.
            // Rueckwaerts-Kopien muessen fertig sein, sonst klingt das Original;
            // Sampler muessen ihre Sounds gebaut haben (Praxistest 1d)
            juce::ignoreUnused (ctx);
            auto& edit = r.edit != nullptr ? *r.edit : *ctx.edit;   // auch ein zweites, nur zum Rendern geladenes Edit (renderNode)

            // Fehlt eine Datei, die dieser Render liest, wartet die Engine je
            // Block 5 s auf sie — vorher mit Fehler zurueck (Todo
            // 2026-10-26-fehlende-mediendatei-haengt). Alle Offline-Renders
            // (render, Analyse, Freeze, processClip, Bounce) laufen hier durch
            if (auto missing = missingMediaError (missingMedia (edit, &r)); missing.isNotEmpty())
                return missing;

            settlePlugins (edit);
            waitForClipRenders (edit);

            const te::Edit::ScopedRenderStatus srs (edit, true);
            const ScopedListeningOff noListening (ctx, edit);   // Solo/Mute sind Abhoer-Hilfen, nicht Mix
            const auto sandboxFailures = sandboxFailureCount (edit);

            if (auto task = te::render_utils::createRenderTask (r, "export", nullptr, nullptr))
            {
                while (task->runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
                {}

                error = task->errorMessage;
            }

            if (error.isEmpty() && ! r.destFile.existsAsFile())
                error = "render failed";

            // Plugin-Sandbox: ein Block ohne Antwort des Host-Prozesses waere
            // Stille in der Datei — dann keine Datei, sondern ein Fehler
            if (error.isEmpty() && sandboxFailureCount (edit) != sandboxFailures)
            {
                r.destFile.deleteFile();
                error = sandboxFailureText (edit);
            }

            return error;
        }

        // Ganzzahlige Dateien (16/24 bit) schneiden alles ueber 0 dBFS ab, die
        // Analyse rechnet in float und sieht das nicht (Q5-c, Matthias
        // 2026-10-25). Zaehlt Samples in Laeufen ab 3 am Vollausschlag — ein
        // einzelnes Spitzen-Sample bei 0 dBFS ist kein Clipping (Schwelle
        // gewaehlt, nicht belegt)
        juce::int64 clippedSamples (const te::Renderer::Parameters& r)
        {
            if (r.bitDepth > 24)
                return 0;

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (r.destFile));

            if (reader == nullptr)
                return 0;

            const float full = 1.0f - 1.5f / (float) (1 << (r.bitDepth - 1));
            const int channels = (int) reader->numChannels;
            juce::AudioBuffer<float> buf (channels, 65536);
            std::vector<int> run ((size_t) channels, 0);
            juce::int64 clipped = 0;

            for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += buf.getNumSamples())
            {
                const int n = (int) std::min ((juce::int64) buf.getNumSamples(), reader->lengthInSamples - pos);
                reader->read (&buf, 0, n, pos, true, true);

                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < n; ++i)
                    {
                        if (std::abs (buf.getSample (c, i)) < full)
                            run[(size_t) c] = 0;
                        else if (++run[(size_t) c] == 3)
                            clipped += 3;
                        else if (run[(size_t) c] > 3)
                            ++clipped;
                    }
            }

            return clipped;
        }

        juce::var renderResult (const te::Renderer::Parameters& r)
        {
            auto o = resultObj (true, { { "file", r.destFile.getFullPathName() },
                                        { "bytes", (juce::int64) r.destFile.getSize() },
                                        { "format", r.audioFormat->getFormatName() },
                                        { "sampleRate", r.sampleRateForAudio },
                                        { "bitDepth", r.bitDepth },
                                        { "start", r.time.getStart().inSeconds() },
                                        { "end", r.time.getEnd().inSeconds() } });

            if (auto n = clippedSamples (r); n > 0)
            {
                o.getDynamicObject()->setProperty ("clippedSamples", n);
                o.getDynamicObject()->setProperty ("hints", juce::Array<juce::var> { juce::String (n) + " samples clipped: the "
                    + juce::String (r.bitDepth) + juce::String::fromUTF8 ("-bit file cuts everything above 0 dBFS (analysis does not) — lower the mix or a limiter ceiling, use normalize, or bitDepth 32") });
            }

            return o;
        }
    }

    namespace
    {
        // Ein Modul fuer einen Render umgehen: sein Rack, nicht das Plugin
        // darin. Ein abgeschaltetes Plugin im Rack bleibt im Render-Graphen
        // und meldet seine Latenz weiter, ohne zu verzoegern (nur externe
        // Plugins gleichen das aus); der Render wirft die gemeldete Latenz
        // am Anfang weg — die Spur lag um sie zu frueh (Limiter 78 Samples,
        // Pitch Shifter 8192; Paket 13, D2). Ein abgeschaltetes Rack faellt
        // ganz aus dem Graphen (wie beim Einfrieren).
        te::Plugin* bypassTarget (te::AudioTrack& moduleTrack)
        {
            if (auto* rack = moduleRackOf (moduleTrack))
                return rack;

            return modulePlugin (moduleTrack);
        }

        // Master-Kette fuer einen Teil-Render umgehen (Clip: pegeltreu wie im
        // Mix, wie die Stems), danach wie vorher; on = false laesst sie an
        struct ScopedMasterBypass
        {
            ScopedMasterBypass (te::Edit& edit, bool on)
            {
                if (on)
                    for (auto& id : masterModules (edit))
                        if (auto* mt = findModuleTrack (edit, id))
                            if (auto* p = bypassTarget (*mt); p != nullptr && p->isEnabled())
                            {
                                p->state.setProperty (te::IDs::enabled, false, nullptr);
                                bypassed.add (p);
                            }
            }

            ~ScopedMasterBypass()
            {
                for (auto* p : bypassed)
                    p->state.setProperty (te::IDs::enabled, true, nullptr);
            }

            juce::Array<te::Plugin*> bypassed;
        };

        // Clip-Render (render {track, name}): nur dieser Clip, durch das
        // Routing seiner Spur bis Main (Kette, Gruppen, Sends) — ein
        // MIDI-Clip klingt erst durch sein Instrument, ein Audio-Clip roh
        // ist schon eine Datei. Ohne Master-Kette wie die Stems (master:
        // true mit). Bereich = der Clip, tail haengt den Ausklang an. Ein
        // Clip auf einer stummen Lane (anderer Take) klingt fuer den Render.
        // Fehlertext oder ""
        juce::String setupClipRender (DawdContext& ctx, const juce::var& cmd, te::Renderer::Parameters& r,
                                      te::AudioTrack*& unmute)
        {
            if (! cmd.hasProperty ("name"))
                return "track needs name (a clip); for a whole track use renderStems or analyze {track}";

            if (! cmd.hasProperty ("track"))
                return "name (a clip) needs track";

            if (cmd.hasProperty ("section") || cmd.hasProperty ("start") || cmd.hasProperty ("end"))
                return "section/start/end limit a song render, not a clip (the clip is the range; tail adds the decay)";

            auto& edit = *ctx.edit;
            auto users = userTracks (edit);
            const int ti = (int) cmd.getProperty ("track", -1);

            if (ti < 0 || ti >= users.size())
                return "no such track: " + cmd.getProperty ("track", {}).toString();

            auto name = cmd.getProperty ("name", {}).toString();
            auto* clip = findClipByName (*users[ti], name);

            if (clip == nullptr)
                return "no such clip: " + name;

            auto* group = laneGroupOf (*users[ti]) != nullptr ? laneGroupOf (*users[ti]) : users[ti];

            if (auto err = frozenTrackError (ctx, users.indexOf (group)); err.isNotEmpty())
                return err;

            r.allowedClips.add (clip);
            const auto tail = te::TimeDuration::fromSeconds ((double) cmd.getProperty ("tail", 0.0));
            r.time = { clip->getPosition().getStart(), clip->getPosition().getEnd() + tail };
            r.checkNodesForAudio = false;   // Synth-Graph: Precheck der Engine schlaegt sonst fehl
            r.canRenderInMono = false;

            if (users[ti]->isMuted (false))
                unmute = users[ti];

            return {};
        }
    }

    juce::String renderWithParams (DawdContext& ctx, te::Renderer::Parameters& r)
    {
        return runRender (ctx, r);
    }

    juce::var opRender (DawdContext& ctx, const juce::var& cmd)
    {
        auto path = cmd.getProperty ("file", {}).toString();
        auto fail = [&] (const juce::String& err) { return resultObj (false, { { "file", path }, { "error", err }, { "bytes", 0 } }); };

        te::Renderer::Parameters r (*ctx.edit);
        r.destFile = juce::File (path);
        r.blockSizeForAudio = 512;
        r.tracksToDo.setRange (0, (int) te::getAllTracks (*ctx.edit).size(), true);

        if (auto err = applyRenderOptions (ctx, cmd, r); err.isNotEmpty())
            return fail (err);

        const bool clip = cmd.hasProperty ("track") || cmd.hasProperty ("name");
        te::AudioTrack* unmute = nullptr;

        if (clip)
            if (auto err = setupClipRender (ctx, cmd, r, unmute); err.isNotEmpty())
                return fail (err);

        const ScopedMasterBypass master (*ctx.edit, clip && ! (bool) cmd.getProperty ("master", false));

        if (unmute != nullptr)
            unmute->setMute (false);

        auto err = runRender (ctx, r);

        if (unmute != nullptr)
            unmute->setMute (true);

        if (err.isNotEmpty())
            return fail (err);

        auto o = renderResult (r);
        o.getDynamicObject()->setProperty ("error", juce::String());

        if (clip)
        {
            o.getDynamicObject()->setProperty ("track", cmd.getProperty ("track", {}));
            o.getDynamicObject()->setProperty ("name", cmd.getProperty ("name", {}));

            if (! master.bypassed.isEmpty())
                o.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("rendered without the master chain (level-true part); master: true renders it through"));
        }

        return o;
    }

    std::unique_ptr<te::Edit> loadEditForRender (DawdContext& ctx, const juce::File& editFile)
    {
        auto other = te::loadEditFromFile (*ctx.engine, editFile);

        if (other == nullptr)
            return {};

        other->editFileRetriever = [editFile] { return editFile; };
        other->filePathResolver = [editFile] (const juce::String& p) -> juce::File
        {
            if (juce::File::isAbsolutePath (p))
                return juce::File (p);

            auto viaChild = editFile.getChildFile (p);
            return viaChild.existsAsFile() ? viaChild : editFile.getSiblingFile (p);
        };
        ensureRouting (*other);
        applySectionTempos (*other);
        refreshAutoCrossfades (*other);   // Blendlaengen haelt die Engine nur im Speicher
        return other;
    }

    juce::var renderEditMix (DawdContext& ctx, te::Edit& edit, const juce::var& opts, const juce::File& dest)
    {
        te::Renderer::Parameters r (edit);
        r.destFile = dest;
        r.blockSizeForAudio = 512;
        r.tracksToDo.setRange (0, (int) te::getAllTracks (edit).size(), true);

        if (auto err = applyRenderOptions (ctx, opts, r); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        r.canRenderInMono = false;

        if (auto err = runRender (ctx, r); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        return renderResult (r);
    }

    // Render eines beliebigen Nodes fuer den A/B-Vergleich im Flow-UI (Node-
    // Wechsel waehrend Play): das Edit des Nodes wird zusaetzlich geladen
    // (das offene Projekt bleibt, wie es ist), gerendert und unter
    // renders/ des Projekts abgelegt; Schluessel = Node, Section und Stand
    // der Edit-Datei — ein unveraenderter Node rendert nicht neu.
    juce::var opRenderNode (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        const int id = (int) (double) cmd.getProperty ("node", (double) t.current);
        auto* n = t.find (id);

        if (n == nullptr)
            return resultObj (false, { { "error", "no such node: " + juce::String (id) } });

        if (ctx.edit == nullptr)
            return resultObj (false, { { "error", "no edit" } });   // applyRenderOptions liest das offene Edit

        if (id == t.current)
            saveCurrentEdit (ctx);   // der Stand auf der Platte = der gehoerte

        auto editFile = t.projectDir.getChildFile (n->file);

        if (! editFile.existsAsFile())
            return resultObj (false, { { "error", "node " + juce::String (id) + " has no edit file" } });

        const auto section = cmd.getProperty ("section", {}).toString();
        auto key = "node-" + juce::String (id) + "-" + juce::String::toHexString (editFile.getLastModificationTime().toMilliseconds())
                   + (section.isNotEmpty() ? "-" + juce::File::createLegalFileName (section) : juce::String());
        auto out = t.projectDir.getChildFile ("renders").getChildFile (key + ".wav");
        auto res = [&] (bool cached)
        {
            return resultObj (true, { { "node", id }, { "file", out.getFullPathName() }, { "cached", cached }, { "label", n->label } });
        };

        if (out.existsAsFile())
            return res (true);

        // Alte Renders dieses Nodes (anderer Stand) wegraeumen
        for (auto& f : out.getParentDirectory().findChildFiles (juce::File::findFiles, false, "node-" + juce::String (id) + "-*.wav"))
            if (section.isEmpty() ? f.getFileNameWithoutExtension().fromLastOccurrenceOf ("-", false, false).length() > 8
                                  : f.getFileName().endsWith ("-" + juce::File::createLegalFileName (section) + ".wav"))
                f.deleteFile();

        std::unique_ptr<te::Edit> other;
        te::Edit* edit = ctx.edit.get();

        if (id != t.current)
        {
            other = loadEditForRender (ctx, editFile);

            if (other == nullptr)
                return resultObj (false, { { "error", "cannot load node " + juce::String (id) } });

            edit = other.get();
        }

        te::Renderer::Parameters r (*edit);
        r.destFile = out;
        r.blockSizeForAudio = 512;
        r.tracksToDo.setRange (0, (int) te::getAllTracks (*edit).size(), true);
        r.canRenderInMono = false;

        if (auto err = applyRenderOptions (ctx, juce::var (new juce::DynamicObject()), r); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        r.edit = edit;
        r.checkNodesForAudio = false;   // ein stiller Node ist auch ein Vergleich
        r.time = te::TimeRange (te::TimePosition(), edit->getLength());

        if (section.isNotEmpty())
        {
            auto* sec = findSection (*edit, section);

            if (sec == nullptr)
                return resultObj (false, { { "error", "node " + juce::String (id) + " has no section " + section } });

            r.time = sec->getPosition().time;
        }

        if (auto err = runRender (ctx, r); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        return res (false);
    }

    // Stems: je Gruppenspur eine Datei — die Spur samt ihren Lanes durch ihr
    // Routing (Module, Busse, Feedback) bis Main. Gefiltert wird ueber die
    // Clips (allowedClips): alle Spuren laufen in die Void-Spur, und der
    // Renderer nimmt Kind-Spuren einer erlaubten Spur immer mit — eine
    // Spur-Auswahl (tracksToDo) wirkt deshalb nicht. Klangerzeuger ohne
    // Eingang (Drones) wuerden in jedem Stem klingen. Spuren ohne Clips werden
    // uebersprungen. Stereo erzwungen (Deliverables).
    juce::var opRenderStems (DawdContext& ctx, const juce::var& cmd)
    {
        auto dir = juce::File (cmd.getProperty ("dir", {}).toString());

        if (cmd.getProperty ("dir", {}).toString().isEmpty())
            return resultObj (false, { { "error", "no dir" } });

        return renderStemsOf (ctx, *ctx.edit, cmd, dir);
    }

    juce::var renderStemsOf (DawdContext& ctx, te::Edit& edit, const juce::var& cmd, const juce::File& dir)
    {
        if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
            return resultObj (false, { { "error", "cannot create dir: " + dir.getFullPathName() } });

        auto all = te::getAllTracks (edit);
        auto users = userTracks (edit);
        juce::Array<juce::var> stems, skipped;

        // Format/Optionen einmal pruefen (Datei pro Stem folgt)
        {
            te::Renderer::Parameters probe (edit);
            probe.destFile = dir.getChildFile ("probe." + cmd.getProperty ("format", "wav").toString());

            if (auto err = applyRenderOptions (ctx, cmd, probe); err.isNotEmpty())
                return resultObj (false, { { "error", err } });
        }

        // Stems ohne die Master-Kette (Praxistest 1e: jeder Stem lief
        // einzeln durch den Limiter und war lauter als im Mix — nicht
        // pegeltreu); master: true rendert sie durch die Kette
        juce::Array<te::Plugin*> bypassed;

        if (! (bool) cmd.getProperty ("master", false))
            for (auto& id : masterModules (edit))
                if (auto* mt = findModuleTrack (edit, id))
                    if (auto* p = bypassTarget (*mt); p != nullptr && p->isEnabled())
                    {
                        p->state.setProperty (te::IDs::enabled, false, nullptr);
                        bypassed.add (p);
                    }

        const juce::ScopeGuard restoreMaster { [&] { for (auto* p : bypassed) p->state.setProperty (te::IDs::enabled, true, nullptr); } };

        // groups: true (Gruppen/Submix) — ein Stem je Gruppe (die aeusserste,
        // samt verschachtelter Gruppen), die uebrigen Spuren einzeln: zusammen
        // ergeben die Stems den Mix
        juce::StringArray stemGroups;
        juce::Array<int> grouped;

        if ((bool) cmd.getProperty ("groups", false))
            for (auto& g : groupIds (edit))
                if (groupPathOf (edit, g).groups.isEmpty())
                {
                    stemGroups.add (g);
                    grouped.addArray (groupMembers (edit, g, true));
                }

        // Ein Stem aus den Clips der Spuren samt Lanes (und gefrorenem
        // Audio); key/id/name stehen im Ergebnis. Fehlertext oder ""
        auto renderStem = [&] (const juce::Array<int>& tracks, const juce::String& fileName,
                               const juce::String& key, const juce::var& id, const juce::String& name) -> juce::String
        {
            juce::Array<te::AudioTrack*> members;
            int clips = 0;

            for (auto i : tracks)
            {
                auto* group = users[i];

                // Gruppe = die Spur und ihre Lanes
                for (auto* t : users)
                    if (t == group || laneGroupOf (*t) == group)
                    {
                        members.add (t);
                        clips += t->getClips().size();
                    }

                if (auto* ft = freezeTrackOf (edit, *group))   // gefroren: das Audio statt der stummen Clips
                    members.add (ft);
            }

            if (clips == 0)
            {
                skipped.add (resultObj (true, { { key, id }, { "name", name }, { "reason", "no clips" } }));
                return {};
            }

            te::Renderer::Parameters r (edit);
            r.blockSizeForAudio = 512;
            r.tracksToDo.setRange (0, all.size(), true);

            for (auto* m : members)
                for (auto* c : m->getClips())
                    r.allowedClips.add (c);

            auto ext = cmd.getProperty ("format", {}).toString();
            r.destFile = dir.getChildFile (fileName + "." + (ext.isEmpty() ? juce::String ("wav") : ext.toLowerCase()));

            auto opts = cmd.clone();
            opts.getDynamicObject()->setProperty ("force", true);   // stille Stems sind kein Fehler
            applyRenderOptions (ctx, opts, r);
            r.canRenderInMono = false;

            if (auto err = runRender (ctx, r); err.isNotEmpty())
                return name + ": " + err;

            auto o = renderResult (r);
            o.getDynamicObject()->setProperty (key, id);
            o.getDynamicObject()->setProperty ("name", name);
            o.getDynamicObject()->removeProperty ("ok");
            stems.add (o);
            return {};
        };

        for (int i = 0; i < users.size(); ++i)
        {
            if (laneGroupOf (*users[i]) != nullptr || grouped.contains (i))
                continue;

            if (auto err = renderStem ({ i }, juce::String (i + 1).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (users[i]->getName()),
                                       "track", i, users[i]->getName()); err.isNotEmpty())
                return resultObj (false, { { "error", err }, { "stems", stems } });
        }

        for (auto& g : stemGroups)
        {
            const auto name = moduleLabel (*findGroupTrack (edit, g));

            if (auto err = renderStem (groupMembers (edit, g, true), g + " " + juce::File::createLegalFileName (name), "group", g, name); err.isNotEmpty())
                return resultObj (false, { { "error", err }, { "stems", stems } });
        }

        auto res = resultObj (true, { { "dir", dir.getFullPathName() }, { "stems", stems }, { "skipped", skipped } });

        if (! bypassed.isEmpty())
            res.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("rendered without the master chain (level-true parts); master: true renders each through it"));

        return res;
    }

    // Batch-Render (Vorbild REAPER-Render-Matrix, Pro Tools Bounce Factory):
    // viele Varianten in einem Lauf — Nodes x Bereiche (Song, Sections) x
    // Formate, je Variante der Mix oder die Stems; die uebrigen
    // Render-Optionen gelten fuer alle. Dateien in dir: "n<id> <Label>[ -
    // <Section>].<ext>", Stems im Ordner "<derselbe Name> stems". Andere
    // Nodes werden nur zum Rendern geladen (das offene Projekt bleibt, wie
    // es ist), jeder einmal. Eine Variante, die scheitert (Node ohne die
    // Section), bricht den Lauf nicht ab: sie steht in failed, ok ist dann
    // false. Laeuft synchron (Render auf dem Message-Thread, wie render);
    // GET /progress zeigt "render i/n" je Node.
    juce::var opRenderBatch (DawdContext& ctx, const juce::var& cmd)
    {
        auto& t = *ctx.tree;
        auto fail = [] (const juce::String& e) { return resultObj (false, { { "error", e } }); };
        const auto dirText = cmd.getProperty ("dir", {}).toString();

        if (dirText.isEmpty())
            return fail ("no dir");

        if (! juce::File::isAbsolutePath (dirText))
            return fail ("dir must be an absolute path");

        if (cmd.hasProperty ("section") || cmd.hasProperty ("start") || cmd.hasProperty ("end") || cmd.hasProperty ("file"))
            return fail ("a batch takes sections (a list of names or \"each\") and song, not section/start/end/file");

        const juce::File dir (dirText);

        // Nodes (Default: der offene)
        juce::Array<int> nodes;
        const auto nv = cmd.getProperty ("nodes", {});

        if (nv.isVoid())
            nodes.add (t.current);
        else if (! nv.isArray() || nv.size() == 0)
            return fail ("nodes must be a list of node ids");
        else
            for (auto& v : *nv.getArray())
            {
                const int id = (int) v;

                if (t.find (id) == nullptr)
                    return fail ("no such node: " + v.toString());

                nodes.addIfNotAlreadyThere (id);
            }

        // Formate (Default: format oder wav)
        juce::StringArray formats;
        const auto fv = cmd.getProperty ("formats", {});

        if (fv.isVoid())
            formats.add (cmd.getProperty ("format", "wav").toString().toLowerCase());
        else if (! fv.isArray() || fv.size() == 0)
            return fail ("formats must be a list like [\"wav\", \"flac\"]");
        else
            for (auto& v : *fv.getArray())
                formats.addIfNotAlreadyThere (v.toString().toLowerCase());

        // Bereiche: sections (Namen oder "each" = jede Section des Nodes),
        // song = der ganze Song (Default: nur ohne sections)
        const auto sv = cmd.getProperty ("sections", {});
        const bool each = sv.isString() && sv.toString() == "each";
        juce::StringArray sections;

        if (! sv.isVoid() && ! each)
        {
            if (! sv.isArray() || sv.size() == 0)
                return fail ("sections must be a list of section names or \"each\"");

            for (auto& v : *sv.getArray())
                sections.addIfNotAlreadyThere (v.toString());
        }

        const bool song = (bool) cmd.getProperty ("song", sv.isVoid());

        if (! song && sv.isVoid())
            return fail ("song: false needs sections");

        const bool stems = (bool) cmd.getProperty ("stems", false);

        // Optionen einer Variante: alles aus dem Befehl ausser den Achsen
        auto optsFor = [&] (const juce::String& format, const juce::String& section)
        {
            auto o = cmd.clone();
            auto* d = o.getDynamicObject();

            for (auto key : { "op", "dir", "nodes", "formats", "format", "sections", "song", "stems" })
                d->removeProperty (key);

            d->setProperty ("format", format);

            if (format == "ogg")
                d->removeProperty ("bitDepth");   // ogg hat nur seine eine Tiefe

            if (section.isNotEmpty())
                d->setProperty ("section", section);

            return o;
        };

        // Format und Optionen vorab pruefen — ein Tippfehler kostet keinen Lauf
        for (auto& f : formats)
        {
            te::Renderer::Parameters probe (*ctx.edit);
            probe.destFile = dir.getChildFile ("probe." + f);

            if (auto err = applyRenderOptions (ctx, optsFor (f, {}), probe); err.isNotEmpty())
                return fail (err);
        }

        juce::Array<juce::var> renders, failed;
        const auto started = juce::Time::getMillisecondCounterHiRes();

        for (int ni = 0; ni < nodes.size(); ++ni)
        {
            const int id = nodes[ni];
            const auto label = t.find (id)->label;
            std::unique_ptr<te::Edit> other;
            te::Edit* edit = ctx.edit.get();
            auto item = [&] (const juce::String& format, const juce::String& section)
            {
                auto o = resultObj (true, { { "node", id }, { "label", label }, { "format", format } });
                o.getDynamicObject()->removeProperty ("ok");

                if (section.isNotEmpty())
                    o.getDynamicObject()->setProperty ("section", section);

                return o;
            };

            if (id != t.current)
            {
                ctx.load.setPhase ("loading node " + juce::String (id));
                other = loadEditForRender (ctx, t.projectDir.getChildFile (t.find (id)->file));

                if (other == nullptr)
                {
                    auto o = item ({}, {});
                    o.getDynamicObject()->setProperty ("error", "cannot load node " + juce::String (id));
                    failed.add (o);
                    continue;
                }

                edit = other.get();
            }

            juce::StringArray ranges;   // "" = der ganze Song

            if (song)
                ranges.add ({});

            if (each)
                for (auto* m : sortedSections (*edit))
                    ranges.add (m->getName());
            else
                ranges.addArray (sections);

            const auto base = "n" + juce::String (id) + " " + juce::File::createLegalFileName (label);
            int step = 0;

            for (auto& range : ranges)
                for (auto& format : formats)
                {
                    ctx.load.setPhase ("node " + juce::String (ni + 1) + "/" + juce::String (nodes.size())
                                       + ", render " + juce::String (++step) + "/" + juce::String (ranges.size() * formats.size()));
                    const auto name = base + (range.isNotEmpty() ? " - " + juce::File::createLegalFileName (range) : juce::String());
                    const auto opts = optsFor (format, range);
                    auto o = item (format, range);
                    auto* d = o.getDynamicObject();
                    auto r = stems ? renderStemsOf (ctx, *edit, opts, dir.getChildFile (name + " stems"))
                                   : renderEditMix (ctx, *edit, opts, dir.getChildFile (name + "." + format));

                    if (! (bool) r.getProperty ("ok", false))
                    {
                        d->setProperty ("error", r.getProperty ("error", {}));
                        failed.add (o);
                        continue;
                    }

                    if (auto* ro = r.getDynamicObject())
                        for (auto& p : ro->getProperties())
                            if (p.name.toString() != "ok" && p.name.toString() != "format")
                                d->setProperty (p.name, p.value);

                    renders.add (o);
                }
        }

        auto res = resultObj (failed.isEmpty(), { { "dir", dir.getFullPathName() }, { "renders", renders }, { "failed", failed },
                                                  { "renderSec", std::round ((juce::Time::getMillisecondCounterHiRes() - started) / 100.0) / 10.0 } });

        if (! failed.isEmpty())
            res.getDynamicObject()->setProperty ("error", juce::String (failed.size()) + " of " + juce::String (failed.size() + renders.size())
                                                          + " renders failed (see failed)");

        return res;
    }

    juce::String renderMixRange (DawdContext& ctx, const juce::var& cmd, const juce::File& dest,
                                 double& start, double& end, int& trackIndex)
    {
        auto& edit = *ctx.edit;
        auto opts = cmd.clone();
        auto* o = opts.getDynamicObject();

        if (o == nullptr)
            return "bad command";

        for (auto key : { "format", "bitDepth", "normalize", "level", "trimSilence", "dither" })
            o->removeProperty (key);

        o->setProperty ("format", "wav");
        o->setProperty ("bitDepth", 32);
        o->setProperty ("force", true);

        te::Renderer::Parameters r (edit);
        r.destFile = dest;
        r.blockSizeForAudio = 512;
        r.canRenderInMono = false;
        r.tracksToDo.setRange (0, (int) te::getAllTracks (edit).size(), true);

        if (auto err = applyRenderOptions (ctx, opts, r); err.isNotEmpty())
            return err;

        trackIndex = -1;

        if (cmd.hasProperty ("track"))
        {
            trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
            auto users = userTracks (edit);

            if (trackIndex < 0 || trackIndex >= users.size())
                return "no such track: " + juce::String (trackIndex);

            auto* group = laneGroupOf (*users[trackIndex]) != nullptr ? laneGroupOf (*users[trackIndex]) : users[trackIndex];

            for (auto* t : users)
                if (t == group || laneGroupOf (*t) == group)
                    for (auto* c : t->getClips())
                        r.allowedClips.add (c);

            if (auto* ft = freezeTrackOf (edit, *group))   // gefroren: das Audio statt der stummen Clips
                for (auto* c : ft->getClips())
                    r.allowedClips.add (c);

            if (r.allowedClips.isEmpty())
                return "track has no clips";
        }

        // Eine Spur misst sich ohne die Master-Kette (Praxistest 1c: jede
        // Spur hatte als True Peak die Decke des Master-Limiters) — die
        // Module kurz umgehen, ohne Undo-Eintrag, danach wie vorher
        juce::Array<te::Plugin*> bypassed;

        if (trackIndex >= 0)
            for (auto& id : masterModules (edit))
                if (auto* mt = findModuleTrack (edit, id))
                    if (auto* p = bypassTarget (*mt); p != nullptr && p->isEnabled())
                    {
                        p->state.setProperty (te::IDs::enabled, false, nullptr);
                        bypassed.add (p);
                    }

        const juce::ScopeGuard restoreMaster { [&] { for (auto* p : bypassed) p->state.setProperty (te::IDs::enabled, true, nullptr); } };

        // Fester Vorlauf (Todo 2026-10-30-analyse-vorlauf, Entscheidung
        // Matthias): ein Instrument mit langsamem Einsatz oder Hall begann
        // im Fenster aus der Stille bzw. mit dem Rest des vorigen Renders
        // (Surge "Cyber Pad", 2,00-2,06 s: -44 / -32 dB, voller Render -30,6).
        // Bis zu 2 s vor start mitrendern (ab 0 s), die Datei enthaelt nur
        // das Fenster — fuer alle Aufrufer, auch Dateien fuer den Nutzer
        const auto window = r.time;
        const auto preroll = te::TimeDuration::fromSeconds (juce::jmin (2.0, window.getStart().inSeconds()));
        auto rendered = dest;

        // Limiter messen nur das Fenster (analyze: limiters)
        for (auto& id : moduleIds (edit))
            if (auto* mt = findModuleTrack (edit, id))
                if (auto* l = dynamic_cast<LimiterPlugin*> (modulePlugin (*mt)))
                    l->resetStats (window.getStart().inSeconds());

        if (preroll > 0_td)
        {
            r.time = window.withStart (window.getStart() - preroll);
            rendered = uniqueTempFile ("dawd-preroll", ".wav");
            r.destFile = rendered;
        }

        const juce::ScopeGuard removeRendered { [&] { if (rendered != dest) rendered.deleteFile(); } };

        if (auto err = runRender (ctx, r); err.isNotEmpty())
            return err;

        if (rendered != dest)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (
                juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (rendered), true));

            if (reader == nullptr)
                return "cannot read render";

            const auto skip = juce::jmin (reader->lengthInSamples, (juce::int64) te::toSamples (window.getStart(), reader->sampleRate)
                                                                   - (juce::int64) te::toSamples (r.time.getStart(), reader->sampleRate));
            dest.deleteFile();   // der Writer haengt sonst an

            std::unique_ptr<juce::AudioFormatWriter> writer (
                juce::WavAudioFormat().createWriterFor (new juce::FileOutputStream (dest), reader->sampleRate,
                                                        reader->numChannels, 32, {}, 0));

            if (writer == nullptr || ! writer->writeFromAudioReader (*reader, skip, reader->lengthInSamples - skip))
                return "cannot write " + dest.getFullPathName();
        }

        start = window.getStart().inSeconds();
        end = window.getEnd().inSeconds();
        return {};
    }

    namespace
    {
        // Oktavbaender fuer die Spektral-Balance (Mittenfrequenzen in Hz)
        constexpr double bandCentres[] = { 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
        constexpr int numBands = (int) std::size (bandCentres);

        double round1 (double v) { return std::round (v * 10.0) / 10.0; }

        // Messung eines Bereichs einer Audiodatei: EBU R128 (LoudnessMeter der
        // Engine), RMS, Spektral-Balance (Oktavbaender in dB relativ zur
        // Gesamtleistung — unabhaengig von der Lautstaerke vergleichbar).
        // Baender ueber 0,9 * Nyquist sind nicht messbar und stehen als null da.
        juce::var measureReader (juce::AudioFormatReader& reader, juce::int64 start, juce::int64 length)
        {
            // Mono als Dual-Mono messen (wie es ueber Stereo klingt) — der Mix
            // wird stereo gerendert, sonst stuende eine Mono-Referenz 3 LU leiser da
            const int chans = (int) juce::jmax (2u, reader.numChannels);
            constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
            te::LoudnessMeter meter;
            meter.prepare (reader.sampleRate, chans, fftSize);
            juce::AudioBuffer<float> buf (chans, fftSize);
            juce::dsp::FFT fft (fftOrder);
            juce::dsp::WindowingFunction<float> window (fftSize, juce::dsp::WindowingFunction<float>::hann);
            std::vector<float> fftData (2 * fftSize);
            std::vector<double> power (fftSize / 2 + 1, 0.0), sidePower (fftSize / 2 + 1, 0.0);
            double sumSq = 0.0, sLL = 0.0, sRR = 0.0, sLR = 0.0;
            juce::int64 count = 0;
            length = juce::jmin (length, reader.lengthInSamples - start);

            // Je Block (fftSize Samples): Leistung je Oktavband und Pegel —
            // daraus die Dynamik je Band (rangeDb) und der Rauschteppich
            std::vector<std::array<double, numBands>> blockBands;
            std::vector<double> blockDb;
            std::array<double, numBands> frameBands {};

            // Spektrum eines Signals auf acc (und side) aufsummieren
            auto addSpectrum = [&] (const float* x, int n, std::vector<double>* side)
            {
                std::fill (fftData.begin(), fftData.end(), 0.0f);
                std::copy (x, x + n, fftData.begin());
                window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
                fft.performFrequencyOnlyForwardTransform (fftData.data());

                for (size_t b = 0; b < power.size(); ++b)
                {
                    auto pw = (double) fftData[b] * fftData[b];
                    power[b] += pw;

                    if (side != nullptr)
                        (*side)[b] += pw;

                    auto hz = (double) b * reader.sampleRate / fftSize;

                    for (int k = 0; k < numBands; ++k)
                        if (hz >= bandCentres[k] / std::sqrt (2.0) && hz < bandCentres[k] * std::sqrt (2.0))
                        {
                            frameBands[(size_t) k] += pw;
                            break;
                        }
                }
            };

            // Clipping: flache Spitzen — mindestens 3 gleiche Samples in Folge
            // auf demselben Betrag; gezaehlt je Betrag, nach dem Lesen gelten
            // nur die auf Hoehe des Maximums (Bass-Plateaus leiser Dateien
            // zaehlen so nicht). Erst ab -6 dBFS.
            std::map<float, std::pair<int, juce::int64>> flatRuns;   // Betrag -> (Laeufe, Samples)
            std::vector<float> lastAbs ((size_t) chans, -1.0f);
            std::vector<int> runLen ((size_t) chans, 0);
            float peakAbs = 0.0f;
            constexpr float flatFloor = 0.5f;

            auto endRun = [&] (int c)
            {
                if (runLen[(size_t) c] >= 3 && lastAbs[(size_t) c] >= flatFloor)
                {
                    auto& e = flatRuns[lastAbs[(size_t) c]];
                    ++e.first;
                    e.second += runLen[(size_t) c];
                }
            };

            std::vector<float> mid ((size_t) fftSize), sideSig ((size_t) fftSize);

            for (juce::int64 pos = 0; pos < length; pos += fftSize)
            {
                auto n = (int) juce::jmin ((juce::int64) fftSize, length - pos);
                buf.clear();
                reader.read (&buf, 0, n, start + pos, true, true);   // Mono fuellt beide Kanaele
                meter.process (buf.getArrayOfReadPointers(), chans, n);
                frameBands.fill (0.0);
                const auto sumBefore = sumSq;

                for (int c = 0; c < chans; ++c)
                {
                    auto* d = buf.getReadPointer (c);

                    for (int i = 0; i < n; ++i)
                    {
                        const auto a = std::abs (d[i]);
                        peakAbs = juce::jmax (peakAbs, a);

                        if (a == lastAbs[(size_t) c])
                            ++runLen[(size_t) c];
                        else
                        {
                            endRun (c);
                            lastAbs[(size_t) c] = a;
                            runLen[(size_t) c] = 1;
                        }
                    }
                }

                // Leistung als Mitte + Seite (orthonormal, (L+R)/√2 und
                // (L-R)/√2) — gleiche Summe wie L + R, ein Kanalmittel loeschte
                // gegenphasige Anteile aus; die Seite liefert die Stereobreite
                auto* l = buf.getReadPointer (0);
                auto* r = buf.getReadPointer (1);

                for (int i = 0; i < n; ++i)
                {
                    sumSq += (double) l[i] * l[i] + (double) r[i] * r[i];
                    sLL += (double) l[i] * l[i];
                    sRR += (double) r[i] * r[i];
                    sLR += (double) l[i] * r[i];
                    mid[(size_t) i] = (l[i] + r[i]) * juce::MathConstants<float>::sqrt2 * 0.5f;
                    sideSig[(size_t) i] = (l[i] - r[i]) * juce::MathConstants<float>::sqrt2 * 0.5f;
                }

                addSpectrum (mid.data(), n, nullptr);
                addSpectrum (sideSig.data(), n, &sidePower);

                for (int c = 2; c < chans; ++c)
                {
                    auto* d = buf.getReadPointer (c);

                    for (int i = 0; i < n; ++i)
                        sumSq += (double) d[i] * d[i];

                    addSpectrum (d, n, nullptr);
                }

                count += (juce::int64) n * chans;

                // Digitale Stille (Vor- und Nachlauf) zaehlt weder fuer den
                // Rauschteppich noch fuer die Dynamik der Baender
                if (sumSq > sumBefore)
                {
                    blockDb.push_back (10.0 * std::log10 ((sumSq - sumBefore) / ((double) n * chans)));
                    blockBands.push_back (frameBands);
                }
            }

            for (int c = 0; c < chans; ++c)
                endRun (c);

            meter.flush();
            auto rd = meter.getReadings();

            // Wert beim Anteil q (0..1) einer Liste
            auto quantile = [] (std::vector<double> v, double q)
            {
                std::sort (v.begin(), v.end());
                return v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) std::floor (q * (double) (v.size() - 1) + 0.5))];
            };

            double total = 0.0, sideTotal = 0.0, allPower = 0.0;
            std::vector<double> bandPower (numBands, 0.0), bandSide (numBands, 0.0);

            for (size_t b = 1; b < power.size(); ++b)
            {
                auto hz = b * reader.sampleRate / fftSize;
                allPower += power[b];
                sideTotal += sidePower[b];

                for (int k = 0; k < numBands; ++k)
                    if (hz >= bandCentres[k] / std::sqrt (2.0) && hz < bandCentres[k] * std::sqrt (2.0))
                    {
                        bandPower[(size_t) k] += power[b];
                        bandSide[(size_t) k] += sidePower[b];
                        total += power[b];
                        break;
                    }
            }

            // Breite = Seite relativ zur Mitte in dB (-100 = mono, 0 = Seite so
            // laut wie Mitte, > 0 = ueberwiegend gegenphasig)
            auto widthOf = [] (double side, double all)
            {
                auto midP = all - side;
                return side <= 1.0e-12 || all <= 1.0e-12 ? -100.0
                       : midP <= 1.0e-12 ? 40.0 : juce::jlimit (-100.0, 40.0, round1 (10.0 * std::log10 (side / midP)));
            };

            // Baender ab 0,9 * Nyquist zaehlen nicht (bei 32 kHz laege das
            // 16-kHz-Band an Nyquist) — auch nicht in der Gesamtleistung
            auto measurable = [&] (int k) { return bandCentres[k] < 0.45 * reader.sampleRate; };

            for (int k = 0; k < numBands; ++k)
                if (! measurable (k))
                    total -= bandPower[(size_t) k];

            juce::Array<juce::var> bands;
            const auto rmsAll = count > 0 ? juce::Decibels::gainToDecibels (std::sqrt (sumSq / (double) count), -100.0) : -100.0;

            for (int k = 0; k < numBands; ++k)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("hz", bandCentres[k]);

                // Absoluter Pegel des Bands (RMS-Anteil): db ist relativ zur
                // Gesamtleistung — ein Boost hebt auch das Ganze an und
                // erscheint dort nur zu ~60 % (Praxistest 1d: "EQ wirkt nicht")
                o->setProperty ("dbFs", ! measurable (k) || total <= 0.0 || bandPower[(size_t) k] <= 0.0 ? juce::var()
                                            : juce::var (round1 (rmsAll + 10.0 * std::log10 (bandPower[(size_t) k] / total))));
                o->setProperty ("db", ! measurable (k) ? juce::var()
                                    : total > 0.0 && bandPower[(size_t) k] > 0.0
                                          ? juce::var (round1 (10.0 * std::log10 (bandPower[(size_t) k] / total)))
                                          : juce::var (-100.0));
                o->setProperty ("widthDb", ! measurable (k) || bandPower[(size_t) k] <= 0.0 ? juce::var()
                                                : juce::var (widthOf (bandSide[(size_t) k], bandPower[(size_t) k])));

                // Dynamik des Bands: wie weit sein Pegel von Block zu Block
                // (93 ms) schwankt — Abstand lauteste 5 % zu leisesten 10 %
                // der Bloecke, in denen das Band klingt (bis 40 dB unter seinem
                // lautesten Block). Kompression macht den Wert kleiner.
                juce::var range;

                // Nur Baender mit nennenswerter Energie (wie im Vergleich:
                // ab -40 dB relativ zur Gesamtleistung) — sonst misst es Leckage
                if (measurable (k) && blockBands.size() >= 4 && total > 0.0 && bandPower[(size_t) k] >= total * 1.0e-4)
                {
                    std::vector<double> lv;
                    double top = 0.0;

                    for (auto& fb : blockBands)
                        top = juce::jmax (top, fb[(size_t) k]);

                    for (auto& fb : blockBands)
                        if (top > 0.0 && fb[(size_t) k] >= top * 1.0e-4)
                            lv.push_back (10.0 * std::log10 (fb[(size_t) k]));

                    if (lv.size() >= 4)
                        range = round1 (quantile (lv, 0.95) - quantile (lv, 0.10));
                }

                o->setProperty ("rangeDb", range);
                bands.add (juce::var (o));
            }

            // Dynamik gesamt: Crest (Spitze ueber RMS), PLR (True Peak ueber
            // integrierter Lautheit — klein = stark begrenzt/komprimiert) und
            // der Rauschteppich (leisestes Zehntel der Bloecke ohne digitale
            // Stille: Rauschen in Pausen, sonst die leisesten Stellen)
            auto dynamics = new juce::DynamicObject();
            const auto rmsTotal = count > 0 ? juce::Decibels::gainToDecibels (std::sqrt (sumSq / (double) count), -100.0) : -100.0;
            dynamics->setProperty ("crestDb", rd.samplePeakDb <= -99.0 ? juce::var() : juce::var (round1 (rd.samplePeakDb - rmsTotal)));
            dynamics->setProperty ("plrDb", rd.integratedValid ? juce::var (round1 (rd.truePeakDb - rd.integratedLufs)) : juce::var());
            dynamics->setProperty ("floorDb", blockDb.empty() ? juce::var() : juce::var (round1 (juce::jmax (-100.0, quantile (blockDb, 0.10)))));

            // Clipping: Laeufe flacher Spitzen auf Hoehe des Maximums (bis
            // 0,01 dB darunter)
            int clipRuns = 0;
            juce::int64 clipSamples = 0;

            for (auto& [level, e] : flatRuns)
                if (level >= peakAbs * 0.99885f)
                {
                    clipRuns += e.first;
                    clipSamples += e.second;
                }

            auto clipping = new juce::DynamicObject();
            clipping->setProperty ("runs", clipRuns);
            clipping->setProperty ("samples", (int) clipSamples);
            clipping->setProperty ("levelDb", clipRuns > 0 ? juce::var (round1 (juce::Decibels::gainToDecibels (peakAbs, -100.0f))) : juce::var());

            auto stereo = new juce::DynamicObject();
            stereo->setProperty ("correlation", sLL > 1.0e-12 && sRR > 1.0e-12
                                                    ? juce::var (std::round (sLR / std::sqrt (sLL * sRR) * 100.0) / 100.0)
                                                    : juce::var());
            stereo->setProperty ("widthDb", widthOf (sideTotal, allPower));
            stereo->setProperty ("mono", sideTotal <= 1.0e-9 * juce::jmax (1.0, allPower));

            auto rms = count > 0 ? juce::Decibels::gainToDecibels (std::sqrt (sumSq / (double) count), -100.0) : -100.0;
            auto r = resultObj (true, { { "lufs", rd.integratedValid ? juce::var (round1 (rd.integratedLufs)) : juce::var() },
                                        { "lra", rd.loudnessRangeValid ? juce::var (round1 (rd.loudnessRangeLu)) : juce::var() },
                                        { "maxShortTermLufs", rd.shortTermValid ? juce::var (round1 (rd.maxShortTermLufs)) : juce::var() },
                                        { "truePeakDb", round1 (rd.truePeakDb) },
                                        { "peakDb", round1 (rd.samplePeakDb) },
                                        { "rmsDb", round1 (rms) },
                                        { "duration", round1 ((double) length / reader.sampleRate) },
                                        { "silent", rd.samplePeakDb <= -99.0 },
                                        { "dynamics", juce::var (dynamics) },
                                        { "clipping", juce::var (clipping) },
                                        { "stereo", juce::var (stereo) },
                                        { "bands", bands } });
            r.getDynamicObject()->removeProperty ("ok");
            return r;
        }

        // Limiter im Render (Praxistests 1e-1m: mehr Gain brachte kaum
        // Lautheit, die Gain-Reduction war nicht zu sehen): je Limiter im
        // Weg Reduktion (Mittel ueber die Stellen mit Signal, Maximum) und
        // Crest-Faktor an seinem Eingang, dazu Hinweise. Limiter, die im
        // Render nichts bekamen (umgangen, stille Spur), fehlen.
        void limiterReport (te::Edit& edit, const std::vector<std::pair<juce::String, LimiterPlugin*>>& lims,
                            juce::DynamicObject& out)
        {
            juce::Array<juce::var> list, hints;

            for (auto& [id, l] : lims)
            {
                const auto st = l->renderStats();

                if (st.inCount == 0)
                    continue;

                const auto grAvg = st.grCount > 0 ? st.grSumDb / (double) st.grCount : 0.0;
                const auto peakDb = juce::Decibels::gainToDecibels ((double) st.inPeak, -100.0);
                const auto crest = peakDb - juce::Decibels::gainToDecibels (std::sqrt (st.inSumSq / (double) st.inCount), -100.0);
                const auto release = (double) l->releaseParam->getCurrentValue();
                auto* mt = findModuleTrack (edit, id);
                const auto label = mt != nullptr ? moduleLabel (*mt) : juce::String();
                const auto who = label.isNotEmpty() ? label + " (" + id + ")" : id;

                auto* o = new juce::DynamicObject();
                o->setProperty ("plugin", id);

                if (label.isNotEmpty())
                    o->setProperty ("label", label);

                o->setProperty ("gainDb", round1 (l->gainParam->getCurrentValue()));
                o->setProperty ("ceilingDb", round1 (l->ceilingParam->getCurrentValue()));
                o->setProperty ("releaseMs", std::round (release));
                o->setProperty ("grAvgDb", round1 (grAvg));
                o->setProperty ("grMaxDb", round1 (st.grMaxDb));
                o->setProperty ("inputPeakDb", round1 (peakDb));
                o->setProperty ("inputCrestDb", round1 (crest));
                list.add (juce::var (o));

                if (grAvg >= 3.0)
                    hints.add ("limiter " + who + " reduces " + juce::String (round1 (grAvg)) + " dB on average (max "
                               + juce::String (round1 (st.grMaxDb)) + juce::String::fromUTF8 (" dB gain reduction) — more Gain mostly turns into gain reduction, not loudness")
                               + (release >= 50.0 && st.grMaxDb - grAvg >= 3.0 ? "; a shorter Release (now " + juce::String (juce::roundToInt (release)) + " ms) recovers faster between peaks"
                                                  : juce::String()));

                if (crest >= 15.0 && st.grMaxDb >= 3.0)
                    hints.add ("limiter " + who + ": input crest " + juce::String (round1 (crest))
                               + juce::String::fromUTF8 (" dB — a few sharp peaks set the gain reduction; tame them before the limiter "
                                                         "(Compressor with fast attack on the peaky track, analyze {track} dynamics.crestDb finds it) instead of more Gain"));
            }

            out.setProperty ("limiters", list);

            if (! hints.isEmpty())
                out.setProperty ("hints", hints);
        }

        // Mix-Bereich rendern und messen (Song, section, start/end; track =
        // nur diese Spur samt Lanes wie ein Stem). Fehlertext in err.
        // keep: der Render bleibt unter diesem Namen liegen (compareReference
        // liest ihn fuer die Artikulation noch einmal, statt neu zu rendern)
        juce::var analyzeMix (DawdContext& ctx, const juce::var& cmd, juce::String& err,
                              analysis::Features* features = nullptr, const juce::File* keep = nullptr)
        {
            auto tmp = uniqueTempFile ("dawd-analyze", ".wav");
            const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };   // auf allen Wegen
            double start = 0.0, end = 0.0;
            int trackIndex = -1;
            std::vector<std::pair<juce::String, LimiterPlugin*>> lims;

            for (auto& id : moduleIds (*ctx.edit))
                if (auto* mt = findModuleTrack (*ctx.edit, id))
                    if (auto* l = dynamic_cast<LimiterPlugin*> (modulePlugin (*mt)))
                        lims.push_back ({ id, l });   // renderMixRange setzt die Messung zurueck

            if ((err = renderMixRange (ctx, cmd, tmp, start, end, trackIndex)).isNotEmpty())
                return {};

            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().getWavFormat()->createReaderFor (new juce::FileInputStream (tmp), true));

            if (reader == nullptr)
            {
                err = "cannot read render";
                return {};
            }

            auto m = measureReader (*reader, 0, reader->lengthInSamples);

            if (features != nullptr)
                *features = analysis::computeFeatures (analysis::loadMono (*reader, 0, reader->lengthInSamples));

            reader.reset();   // Datei freigeben, bevor removeTmp loescht

            if (keep != nullptr)
                tmp.moveFileTo (*keep);

            m.getDynamicObject()->setProperty ("start", start);
            m.getDynamicObject()->setProperty ("end", end);

            if (trackIndex >= 0)
                m.getDynamicObject()->setProperty ("track", trackIndex);   // der gepruefte Index

            limiterReport (*ctx.edit, lims, *m.getDynamicObject());
            return m;
        }

        juce::var diffOf (const juce::var& a, const juce::var& b)
        {
            return a.isVoid() || b.isVoid() ? juce::var() : juce::var (round1 ((double) a - (double) b));
        }

        // Artikulation und Lage einer Seite fuer compareReference (Praxistest
        // 1c: Baender, Stereo und Tonklassen waren "nah", die Referenz spielte
        // legato mit Glides, der Nachbau Achtel-Staccato). Noten wie
        // describeVoice: basic-pitch mit Contour, Fragmente gehaltener Toene
        // verschmolzen, Obertoene weg. Fehlertext oder "".
        juce::String articulationOf (DawdContext& ctx, const juce::var& spec, juce::var& out, voice::Voice& v)
        {
            AnalysisSource src;
            src.want22k = src.wantStereo = true;
            src.wantMono = false;
            src.ml = true;   // basic-pitch: Gruppe A der Lizenz-Sperre

            if (auto e = loadAnalysisSource (ctx, spec, src); e.isNotEmpty())
                return e;

            juce::String err;
            transcription::Contour contour;
            auto notes = transcription::transcribe (src.audio22k, transcription::Options(), err, &contour);

            if (err.isNotEmpty())
                return err;

            v = voice::describeVoice (notes, contour, src.stereo, AnalysisSource::stereoRate, voice::Grid());
            auto o = new juce::DynamicObject();
            out = juce::var (o);
            o->setProperty ("notes", (int) v.notes.size());

            if (v.notes.empty())
                return {};

            auto num = [] (double x, double step) { return std::isnan (x) ? juce::var() : juce::var (std::round (x / step) * step); };

            // Je Note: Laenge gegen den Abstand zum naechsten Einsatz (wie
            // legato in describeVoice) — Anteil gebunden (>= 0,9) und kurz (< 0,5)
            int bound = 0, detached = 0, counted = 0;
            std::vector<double> lens;

            for (size_t i = 0; i < v.notes.size(); ++i)
            {
                const auto& n = v.notes[i];
                lens.push_back (n.end - n.start);

                for (size_t j = i + 1; j < v.notes.size(); ++j)
                    if (v.notes[j].start > n.start + 0.05)
                    {
                        const double r = (n.end - n.start) / (v.notes[j].start - n.start);
                        bound += r >= 0.9 ? 1 : 0;
                        detached += r < 0.5 ? 1 : 0;
                        ++counted;
                        break;
                    }
            }

            std::sort (lens.begin(), lens.end());
            o->setProperty ("legato", num (v.legato, 0.01));
            o->setProperty ("legatoShare", counted > 0 ? num ((double) bound / counted, 0.01) : juce::var());
            o->setProperty ("staccatoShare", counted > 0 ? num ((double) detached / counted, 0.01) : juce::var());
            o->setProperty ("medianLengthSec", num (lens[lens.size() / 2], 0.001));
            o->setProperty ("glideShare", num (v.glideShare, 0.01));
            o->setProperty ("glideMs", num (v.glideMs, 1.0));
            o->setProperty ("vibratoShare", num (v.vibratoShare, 0.01));
            o->setProperty ("vibratoCents", num (v.vibratoCents, 1.0));
            o->setProperty ("vibratoHz", num (v.vibratoHz, 0.1));
            o->setProperty ("voices", num (v.voices, 0.1));

            // Tonhoehen mit Oktavlage, gewichtet nach Dauer x Pegel (wie
            // meanPitch): Anteil je Oktave (C bis H) und die staerksten Toene
            std::map<int, double> byPitch;
            double total = 0.0;

            for (auto& n : v.notes)
            {
                const double w = (n.end - n.start) * std::pow (10.0, (std::isnan (n.velDb) ? 0.0 : n.velDb) / 20.0);
                byPitch[n.pitch] += w;
                total += w;
            }

            std::map<int, double> byOctave;

            for (auto& [p, w] : byPitch)
                byOctave[p / 12 - 1] += w;

            juce::Array<juce::var> octaves, top;

            for (auto& [oct, w] : byOctave)
                if (total > 0.0 && w / total >= 0.01)
                {
                    auto e = new juce::DynamicObject();
                    e->setProperty ("octave", "C" + juce::String (oct));
                    e->setProperty ("share", std::round (w / total * 100.0) / 100.0);
                    octaves.add (juce::var (e));
                }

            std::vector<std::pair<double, int>> order;

            for (auto& [p, w] : byPitch)
                order.push_back ({ w, p });

            std::sort (order.rbegin(), order.rend());

            for (size_t i = 0; i < order.size() && i < 8 && total > 0.0 && order[i].first / total >= 0.03; ++i)
            {
                auto e = new juce::DynamicObject();
                e->setProperty ("note", voice::noteName (order[i].second));
                e->setProperty ("share", std::round (order[i].first / total * 100.0) / 100.0);
                top.add (juce::var (e));
            }

            auto pitch = new juce::DynamicObject();
            pitch->setProperty ("lowest", voice::noteName (v.lowest));
            pitch->setProperty ("highest", voice::noteName (v.highest));
            pitch->setProperty ("mean", voice::noteName ((int) std::round (v.meanPitch)));
            pitch->setProperty ("meanMidi", std::round (v.meanPitch * 10.0) / 10.0);
            pitch->setProperty ("octaves", octaves);
            pitch->setProperty ("top", top);
            o->setProperty ("pitch", juce::var (pitch));
            return {};
        }
    }

    // Analyse fuer Agenten (die nicht hoeren): Lautheit nach EBU R128, Pegel,
    // Spektral-Balance des Mix-Bereichs. Read-only.
    juce::var opAnalyze (DawdContext& ctx, const juce::var& cmd)
    {
        // Eine Datei (Stem, Referenz, Sample) statt des Projekts — frueher
        // still uebergangen, das Ergebnis galt dann dem Projekt (Praxistest 1b)
        if (cmd.hasProperty ("file"))
        {
            auto path = cmd.getProperty ("file", {}).toString();
            juce::File f (path);

            if (path.isEmpty() || ! f.existsAsFile())
                return resultObj (false, { { "error", "no such file: " + path } });

            if (cmd.hasProperty ("track") || cmd.hasProperty ("section"))
                return resultObj (false, { { "error", juce::String::fromUTF8 ("file analyses the file — track and section only apply to the project") } });

            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f));

            if (reader == nullptr)
                return resultObj (false, { { "error", "cannot read: " + f.getFileName() } });

            auto sr = reader->sampleRate;
            auto from = (double) cmd.getProperty ("start", 0.0);
            auto to = cmd.hasProperty ("end") ? (double) cmd.getProperty ("end", 0.0) : (double) reader->lengthInSamples / sr;

            if (from < 0.0 || to <= from || to * sr > (double) reader->lengthInSamples + 1.0)
                return resultObj (false, { { "error", "need 0 <= start < end within the file" } });

            auto m = measureReader (*reader, (juce::int64) (from * sr), (juce::int64) ((to - from) * sr));
            auto* o = m.getDynamicObject();
            o->setProperty ("ok", true);
            o->setProperty ("source", "file");
            o->setProperty ("file", f.getFullPathName());
            o->setProperty ("start", from);
            o->setProperty ("end", to);

            // embed: CLAP-Vektor und Klang-Merkmale wie in der Sample-Suche
            // (fuer Preset-/Klang-Kataloge, tools/profile_presets.py)
            // (Lizenz-Sperre: wie die Sample-Suche nicht auf gesperrten Dateien)
            if ((bool) cmd.getProperty ("embed", false))
                if (auto e = blockedFileError (ctx, f); e.isNotEmpty() || (e = embedSoundFile (ctx, f, from, to, *o)).isNotEmpty())
                    return resultObj (false, { { "error", e } });

            return m;
        }

        // Das Projekt braucht ein Edit; eine Datei (oben) nicht — so geht die
        // Analyse einer Referenz auch vor dem ersten Song (Flow-UI ohne Projekt)
        if (ctx.edit == nullptr)
            return resultObj (false, { { "error", "no edit" } });

        juce::String err;
        auto m = analyzeMix (ctx, cmd, err);

        if (err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        m.getDynamicObject()->setProperty ("ok", true);
        return m;
    }

    // Vergleich mit einem Referenztrack (Datei: wav, aiff, flac, ogg, mp3):
    // beide gleich gemessen, Differenz Mix - Referenz und kurze Hinweise.
    // Die Spektral-Balance ist lautheitsunabhaengig (Baender relativ zur
    // Gesamtleistung) — sie vergleicht die Klangfarbe. Read-only.
    juce::var opCompareReference (DawdContext& ctx, const juce::var& cmd)
    {
        auto refFile = juce::File (cmd.getProperty ("file", {}).toString());

        if (cmd.getProperty ("file", {}).toString().isEmpty() || ! refFile.existsAsFile())
            return resultObj (false, { { "error", "no such file: " + cmd.getProperty ("file", {}).toString() } });

        if (auto e = blockedFileError (ctx, refFile); e.isNotEmpty())
            return resultObj (false, { { "error", e } });

        std::unique_ptr<juce::AudioFormatReader> reader (
            ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (refFile));

        if (reader == nullptr)
            return resultObj (false, { { "error", "cannot read reference: " + refFile.getFileName() } });

        auto refStart = (juce::int64) ((double) cmd.getProperty ("referenceStart", 0.0) * reader->sampleRate);
        auto refEnd = cmd.hasProperty ("referenceEnd")
                          ? (juce::int64) ((double) cmd.getProperty ("referenceEnd", 0.0) * reader->sampleRate)
                          : reader->lengthInSamples;

        if (refStart < 0 || refEnd <= refStart || refEnd > reader->lengthInSamples)
            return resultObj (false, { { "error", "need 0 <= referenceStart < referenceEnd within the file" } });

        juce::String err;
        analysis::Features mixFeatures;
        // Der Render bleibt fuer die Artikulation (ein Render statt zwei,
        // Entscheidung Matthias 2026-10-29)
        auto mixRender = uniqueTempFile ("dawd-compare", ".wav");
        const juce::ScopeGuard removeMixRender { [&] { mixRender.deleteFile(); } };
        auto mix = analyzeMix (ctx, cmd, err, &mixFeatures, &mixRender);

        if (err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // Lizenz-Sperre: messen mit Hinweis; die Artikulation (basic-pitch)
        // laeuft nicht ueber gesperrtes Material
        const double mixStart = mix.getProperty ("start", 0.0), mixEnd = mix.getProperty ("end", 0.0);
        const int mixTrack = mix.getProperty ("track", -1);
        checkMixLicence (ctx, mixStart, mixEnd, mixTrack, false);
        const auto articulationBlocked = checkMixLicence (ctx, mixStart, mixEnd, mixTrack, true);

        // Stille mit der Referenz zu vergleichen ergab frueher still -100 dB
        if ((bool) mix.getProperty ("silent", false))
            return resultObj (false, { { "error", "the project is silent in " + juce::String ((double) mix.getProperty ("start", 0.0), 2) + "-"
                                                       + juce::String ((double) mix.getProperty ("end", 0.0), 2)
                                                       + " s - nothing to compare (compareReference measures the project mix against 'file'; "
                                                         "build or unmute tracks, or pick start/end/section)" } });

        auto ref = measureReader (*reader, refStart, refEnd - refStart);

        auto diff = new juce::DynamicObject();

        for (auto key : { "lufs", "lra", "maxShortTermLufs", "truePeakDb", "peakDb", "rmsDb" })
            diff->setProperty (key, diffOf (mix.getProperty (key, {}), ref.getProperty (key, {})));

        juce::Array<juce::var> bandDiff;
        juce::StringArray hints;
        auto* mb = mix.getProperty ("bands", {}).getArray();
        auto* rb = ref.getProperty ("bands", {}).getArray();
        const int nb = mb != nullptr && rb != nullptr ? juce::jmin (mb->size(), rb->size()) : 0;
        auto bandDb = [] (juce::Array<juce::var>& bs, int k) { return bs.getReference (k).getProperty ("db", {}); };
        auto common = [&] (int k) { return ! bandDb (*mb, k).isVoid() && ! bandDb (*rb, k).isVoid(); };

        // Fehlt ein Band auf einer Seite (ueber 0,9 * Nyquist), beziehen sich
        // beide Seiten auf die Leistung der gemeinsamen Baender
        auto commonOffset = [&] (juce::Array<juce::var>& bs)
        {
            double sum = 0.0;

            for (int k = 0; k < nb; ++k)
                if (common (k))
                    sum += std::pow (10.0, (double) bandDb (bs, k) / 10.0);

            return sum > 0.0 ? 10.0 * std::log10 (sum) : 0.0;
        };

        double mOff = 0.0, rOff = 0.0;

        for (int k = 0; k < nb; ++k)
            if (bandDb (*mb, k).isVoid() != bandDb (*rb, k).isVoid())
            {
                mOff = commonOffset (*mb);
                rOff = commonOffset (*rb);
                break;
            }

        for (int k = 0; k < nb; ++k)
        {
            auto hz = (double) mb->getReference (k).getProperty ("hz", 0.0);
            auto o = new juce::DynamicObject();
            o->setProperty ("hz", hz);
            o->setProperty ("db", juce::var());
            bandDiff.add (juce::var (o));

            if (! common (k))
                continue;

            auto md = (double) bandDb (*mb, k) - mOff;
            auto rd = (double) bandDb (*rb, k) - rOff;
            // Nur Baender mit nennenswerter Energie auf einer Seite (> -40 dB
            // relativ zur Gesamtleistung) — darunter liegt Rauschen
            const bool relevant = juce::jmax (md, rd) > -40.0;

            if (relevant)
                o->setProperty ("db", round1 (juce::jlimit (-40.0, 40.0, md - rd)));

            auto name = hz >= 1000.0 ? juce::String (hz / 1000.0) + " kHz" : juce::String (hz) + " Hz";

            if (relevant && md - rd <= -40.0)
                hints.add (name + ": missing in the mix (present in the reference)");
            else if (relevant && md - rd >= 40.0)
                hints.add (name + ": strong in the mix, not in the reference");
            else if (relevant && std::abs (md - rd) >= 3.0)
                hints.add (name + ": " + (md > rd ? "+" : "") + juce::String (round1 (md - rd)) + " dB vs reference (tonal balance)");
        }

        diff->setProperty ("bands", bandDiff);

        // Dynamik: gesamt und je Band (rangeDb); Clipping nur als Hinweis
        auto md = mix.getProperty ("dynamics", {}), rdyn = ref.getProperty ("dynamics", {});
        auto dynDiff = new juce::DynamicObject();

        for (auto key : { "crestDb", "plrDb", "floorDb" })
            dynDiff->setProperty (key, diffOf (md.getProperty (key, {}), rdyn.getProperty (key, {})));

        diff->setProperty ("dynamics", juce::var (dynDiff));

        if (auto pd = dynDiff->getProperty ("plrDb"); ! pd.isVoid() && std::abs ((double) pd) >= 2.0)
            hints.add (juce::String ("mix is ") + ((double) pd < 0 ? "more" : "less") + " compressed/limited than the reference (PLR "
                       + juce::String ((double) md.getProperty ("plrDb", 0.0)) + " vs " + juce::String ((double) rdyn.getProperty ("plrDb", 0.0))
                       + " dB: true peak above loudness)");

        if (auto fd = dynDiff->getProperty ("floorDb"); ! fd.isVoid() && (double) fd >= 10.0)
            hints.add ("quietest moments of the mix are " + juce::String ((double) fd) + " dB louder than in the reference (floor "
                       + juce::String ((double) md.getProperty ("floorDb", 0.0)) + " vs " + juce::String ((double) rdyn.getProperty ("floorDb", 0.0))
                       + " dB: noise, a tail or a pad that never stops)");

        if (auto runs = (int) mix.getProperty ("clipping", {}).getProperty ("runs", 0); runs >= 3)
            hints.add ("mix clips: " + juce::String (runs) + " flat peaks at " + juce::String ((double) mix.getProperty ("clipping", {}).getProperty ("levelDb", 0.0))
                       + " dBFS (reference: " + juce::String ((int) ref.getProperty ("clipping", {}).getProperty ("runs", 0)) + ")");

        for (int k = 0; k < nb; ++k)
        {
            auto mr = mb->getReference (k).getProperty ("rangeDb", {}), rr = rb->getReference (k).getProperty ("rangeDb", {});

            if (bandDiff.getReference (k).getProperty ("db", {}).isVoid() || mr.isVoid() || rr.isVoid())
                continue;

            auto rdk = round1 ((double) mr - (double) rr);
            bandDiff.getReference (k).getDynamicObject()->setProperty ("rangeDb", rdk);
            auto hz = (double) mb->getReference (k).getProperty ("hz", 0.0);

            if (std::abs (rdk) >= 6.0)
                hints.add ((hz >= 1000.0 ? juce::String (hz / 1000.0) + " kHz" : juce::String (hz) + " Hz") + ": level moves "
                           + (rdk < 0 ? "less" : "more") + " than in the reference (range " + juce::String ((double) mr) + " vs "
                           + juce::String ((double) rr) + " dB)" + (rdk < 0 ? juce::String::fromUTF8 (" — more compressed") : juce::String()));
        }

        // Stereobreite: gesamt, Korrelation, je Band (nur wo beide Energie haben)
        auto ms = mix.getProperty ("stereo", {}), rs = ref.getProperty ("stereo", {});
        // Differenzen tragen "Delta" im Namen und die Messwerte beider
        // Seiten stehen daneben — Praxistest 1d las diff.correlation 0,31
        // als Korrelation des Mix (die war 0,15, die Referenz -0,16)
        auto stereoDiff = new juce::DynamicObject();
        stereoDiff->setProperty ("widthDbDelta", diffOf (ms.getProperty ("widthDb", {}), rs.getProperty ("widthDb", {})));
        stereoDiff->setProperty ("correlationDelta", ms.getProperty ("correlation", {}).isVoid() || rs.getProperty ("correlation", {}).isVoid()
                                                     ? juce::var()
                                                     : juce::var (std::round (((double) ms.getProperty ("correlation", 0.0)
                                                                               - (double) rs.getProperty ("correlation", 0.0)) * 100.0) / 100.0));
        stereoDiff->setProperty ("mixCorrelation", ms.getProperty ("correlation", {}));
        stereoDiff->setProperty ("referenceCorrelation", rs.getProperty ("correlation", {}));
        diff->setProperty ("stereo", juce::var (stereoDiff));

        if ((bool) rs.getProperty ("mono", false) == false && (bool) ms.getProperty ("mono", false))
            hints.add ("mix is mono, the reference is stereo");
        else if (auto wd = stereoDiff->getProperty ("widthDbDelta"); ! wd.isVoid() && std::abs ((double) wd) >= 3.0)
            hints.add (juce::String ("mix is ") + ((double) wd < 0 ? "narrower" : "wider") + " than the reference (side/mid "
                       + juce::String ((double) wd) + " dB)");

        for (int k = 0; k < nb; ++k)
        {
            auto mw = mb->getReference (k).getProperty ("widthDb", {}), rw = rb->getReference (k).getProperty ("widthDb", {});

            if (bandDiff.getReference (k).getProperty ("db", {}).isVoid() || mw.isVoid() || rw.isVoid())
                continue;

            auto wdk = round1 ((double) mw - (double) rw);
            bandDiff.getReference (k).getDynamicObject()->setProperty ("widthDb", wdk);
            auto hz = (double) mb->getReference (k).getProperty ("hz", 0.0);

            if (std::abs (wdk) >= 6.0 && (double) rw > -60.0)
                hints.add ((hz >= 1000.0 ? juce::String (hz / 1000.0) + " kHz" : juce::String (hz) + " Hz") + ": "
                           + (wdk < 0 ? "narrower" : "wider") + " than the reference (" + juce::String (wdk) + " dB side/mid)");
        }

        // Harmonie je Takt: Tonklassen-Profil (Chroma) von Mix und Referenz,
        // beide ab ihrem Start im Taktraster des Projekts, als Korrelation
        // (-1..1, 1 = gleiche Tonklassen-Gewichtung). Robuster als Akkordnamen,
        // die bei kleinen Mixaenderungen zwischen verwandten Akkorden kippen.
        {
            auto refFeatures = analysis::computeFeatures (analysis::loadMono (*reader, refStart, refEnd - refStart));
            const auto mixStart = (double) mix.getProperty ("start", 0.0);
            const int beatsPerBar = juce::jmax (1, ctx.edit->tempoSequence.getTimeSigAt (te::TimePosition::fromSeconds (mixStart)).numerator.get());
            const auto bar = beatsToSeconds (*ctx.edit, mixStart, (double) beatsPerBar);
            const auto span = juce::jmin (mixFeatures.duration, refFeatures.duration);

            // downbeat: erste Takt-Eins in Sekunden ab Start (beide Seiten
            // gleich) — sonst schneiden die Taktfenster quer durch die Takte
            const auto off = juce::jlimit (0.0, juce::jmax (0.0, span - 0.1), (double) cmd.getProperty ("downbeat", 0.0));

            // Die drei staerksten Tonklassen eines Profils als Text ("A F C")
            auto topNotes = [] (const std::array<double, 12>& p)
            {
                std::array<int, 12> order {};
                std::iota (order.begin(), order.end(), 0);
                std::sort (order.begin(), order.end(), [&p] (int a, int b) { return p[(size_t) a] > p[(size_t) b]; });
                juce::StringArray out;

                for (int i = 0; i < 3 && p[(size_t) order[(size_t) i]] > 0.25 * p[(size_t) order[0]]; ++i)
                    out.add (analysis::pitchName (order[(size_t) i], false));

                return out.joinIntoString (" ");
            };

            auto profile = [] (const analysis::Features& f, double from, double to, double& energy)
            {
                std::array<double, 12> p {};
                energy = 0.0;

                if (f.chromaFps <= 0.0)
                    return p;

                for (auto i = (size_t) std::max (0.0, std::floor (from * f.chromaFps)); i < f.chroma.size() && (double) i / f.chromaFps < to; ++i)
                {
                    for (int k = 0; k < 12; ++k)
                        p[(size_t) k] += f.chroma[i][(size_t) k] * f.chromaEnergy[i];

                    energy += f.chromaEnergy[i];
                }

                return p;
            };

            auto correlation = [] (const std::array<double, 12>& a, const std::array<double, 12>& b)
            {
                double ma = 0.0, mb2 = 0.0;

                for (int k = 0; k < 12; ++k) { ma += a[(size_t) k] / 12.0; mb2 += b[(size_t) k] / 12.0; }

                double num = 0.0, da = 0.0, db = 0.0;

                for (int k = 0; k < 12; ++k)
                {
                    num += (a[(size_t) k] - ma) * (b[(size_t) k] - mb2);
                    da += (a[(size_t) k] - ma) * (a[(size_t) k] - ma);
                    db += (b[(size_t) k] - mb2) * (b[(size_t) k] - mb2);
                }

                return da > 1.0e-12 && db > 1.0e-12 ? num / std::sqrt (da * db) : 0.0;
            };

            // Ohne Tonhoehe (Drum-Stem) ist eine Harmonie-Aehnlichkeit Rauschen
            // (Praxistest 1d) — weglassen und sagen, warum
            const auto refKey = analysis::detectKey (refFeatures, {});
            const bool tonal = refKey.valid && refKey.confidence >= 0.3;

            if (! tonal)
                hints.add (juce::String::fromUTF8 ("reference has no clear pitch content (drums?) — harmony not compared"));

            if (tonal && bar > 0.05 && span - off >= bar * 0.9)
            {
                juce::Array<juce::var> bars;
                juce::StringArray weak;
                double sum = 0.0;
                int counted = 0;
                double mixMaxE = 0.0, refMaxE = 0.0;
                const int numBars = (int) std::floor ((span - off) / bar + 0.1);
                std::vector<std::array<double, 12>> mp ((size_t) numBars), rp ((size_t) numBars);
                std::vector<double> me ((size_t) numBars), re ((size_t) numBars);

                for (int b = 0; b < numBars; ++b)
                {
                    mp[(size_t) b] = profile (mixFeatures, off + b * bar, off + (b + 1) * bar, me[(size_t) b]);
                    rp[(size_t) b] = profile (refFeatures, off + b * bar, off + (b + 1) * bar, re[(size_t) b]);
                    mixMaxE = juce::jmax (mixMaxE, me[(size_t) b]);
                    refMaxE = juce::jmax (refMaxE, re[(size_t) b]);
                }

                for (int b = 0; b < numBars; ++b)
                {
                    // Fast stille Takte zaehlen nicht
                    if (me[(size_t) b] < 0.05 * mixMaxE || re[(size_t) b] < 0.05 * refMaxE)
                    {
                        bars.add (resultObj (true, { { "bar", b + 1 }, { "similarity", juce::var() } }));
                        continue;
                    }

                    auto c = std::round (correlation (mp[(size_t) b], rp[(size_t) b]) * 100.0) / 100.0;
                    bars.add (resultObj (true, { { "bar", b + 1 }, { "similarity", c } }));

                    // Bei Abweichung die staerksten Tonklassen beider Seiten —
                    // "D F# A gegen D F A" sagt mehr als 0,24
                    if (c < 0.7)
                    {
                        bars.getReference (bars.size() - 1).getDynamicObject()->setProperty ("mixNotes", topNotes (mp[(size_t) b]));
                        bars.getReference (bars.size() - 1).getDynamicObject()->setProperty ("refNotes", topNotes (rp[(size_t) b]));
                    }
                    sum += c;
                    ++counted;

                    if (c < 0.5)
                        weak.add (juce::String (b + 1));
                }

                for (auto& bv : bars)
                    bv.getDynamicObject()->removeProperty ("ok");

                auto harmony = new juce::DynamicObject();
                harmony->setProperty ("barSeconds", std::round (bar * 1000.0) / 1000.0);
                harmony->setProperty ("downbeat", off);
                harmony->setProperty ("similarity", counted > 0 ? juce::var (std::round (sum / counted * 100.0) / 100.0) : juce::var());
                harmony->setProperty ("bars", bars);
                diff->setProperty ("harmony", juce::var (harmony));

                if (! weak.isEmpty())
                    hints.add ("harmony differs in bar" + juce::String (weak.size() > 1 ? "s " : " ") + weak.joinIntoString (", ")
                               + " (pitch-class similarity < 0.5)");
            }
        }

        // Artikulation und Lage beider Seiten (Noten aus basic-pitch); ohne
        // ONNX Runtime/Modell nicht. Standard nur bis 60 s Vergleich (ein
        // Teil, ein Stem): es kostet ~0,11 s je Sekunde, ein ganzer Song beim
        // Mastern wuerde ~3,5-mal langsamer (gemessen 2026-10-27)
        const double span = (double) mix.getProperty ("end", 0.0) - (double) mix.getProperty ("start", 0.0);
        const bool articulation = (bool) cmd.getProperty ("articulation", span <= 60.0);

        if (! cmd.hasProperty ("articulation") && ! articulation)
            hints.add (juce::String::fromUTF8 ("articulation not compared (longer than 60 s) — articulation: true or a shorter part"));

        if (articulation && articulationBlocked.isNotEmpty())
            hints.add ("articulation not compared - " + articulationBlocked);
        else if (articulation && transcription::available().isEmpty())
        {
            auto mixSpec = new juce::DynamicObject();
            mixSpec->setProperty ("file", mixRender.getFullPathName());

            auto refSpec = new juce::DynamicObject();
            refSpec->setProperty ("file", refFile.getFullPathName());
            refSpec->setProperty ("start", (double) refStart / reader->sampleRate);
            refSpec->setProperty ("end", (double) refEnd / reader->sampleRate);

            juce::var ma, ra;
            voice::Voice mv, rv;

            if (auto e = articulationOf (ctx, juce::var (mixSpec), ma, mv); e.isNotEmpty())
                return resultObj (false, { { "error", "articulation (mix): " + e } });

            // Die Referenz aendert sich zwischen Aufrufen selten: ihre Noten
            // aus dem Speicher (Datei, Stand, Bereich) — der Vergleich ueber
            // einen ganzen Song dauerte ~2 min (Praxistest stil-1), die
            // Referenz ist meist der laengere Teil
            struct CachedArticulation { juce::String key; juce::var out; voice::Voice v; };
            static std::vector<CachedArticulation> refCache;
            const auto refKey = refFile.getFullPathName() + "|" + juce::String (refFile.getLastModificationTime().toMilliseconds())
                              + "|" + juce::String (refFile.getSize()) + "|" + juce::String (refStart) + "|" + juce::String (refEnd);
            auto hit = std::find_if (refCache.begin(), refCache.end(), [&] (const CachedArticulation& c) { return c.key == refKey; });

            if (hit != refCache.end())
            {
                ra = hit->out.clone();
                rv = hit->v;
            }
            else
            {
                if (auto e = articulationOf (ctx, juce::var (refSpec), ra, rv); e.isNotEmpty())
                    return resultObj (false, { { "error", "articulation (reference): " + e } });

                if (refCache.size() >= 4)
                    refCache.erase (refCache.begin());

                refCache.push_back ({ refKey, ra.clone(), rv });
            }

            mix.getDynamicObject()->setProperty ("articulation", ma);
            ref.getDynamicObject()->setProperty ("articulation", ra);
            auto ad = new juce::DynamicObject();
            auto delta = [&] (const char* key, double step)
            {
                auto a = ma.getProperty (key, {}), b = ra.getProperty (key, {});
                return a.isVoid() || b.isVoid() ? juce::var() : juce::var (std::round (((double) a - (double) b) / step) * step);
            };

            for (auto key : { "legato", "legatoShare", "staccatoShare", "glideShare", "vibratoShare" })
                ad->setProperty (juce::String (key) + "Delta", delta (key, 0.01));

            ad->setProperty ("medianLengthSecDelta", delta ("medianLengthSec", 0.001));
            const bool both = ! mv.notes.empty() && ! rv.notes.empty();

            if (both)
                ad->setProperty ("meanPitchDelta", std::round ((mv.meanPitch - rv.meanPitch) * 10.0) / 10.0);

            diff->setProperty ("articulation", juce::var (ad));

            // Hinweise nur bei deutlichen Unterschieden und genug Noten
            if (both && mv.notes.size() >= 4 && rv.notes.size() >= 4)
            {
                auto f2 = [] (double x) { return juce::String (x, 2); };

                if (rv.legato >= 0.85 && mv.legato <= 0.6)
                    hints.add ("articulation: reference legato " + f2 (rv.legato) + ", mix detached " + f2 (mv.legato)
                               + juce::String::fromUTF8 (" — lengthen the notes to the next onset (audioToMidi legato: true; a mono/legato patch)"));
                else if (rv.legato <= 0.6 && mv.legato >= 0.85)
                    hints.add ("articulation: reference detached " + f2 (rv.legato) + ", mix legato " + f2 (mv.legato)
                               + juce::String::fromUTF8 (" — shorten the notes"));

                if (rv.glideShare >= 0.25 && mv.glideShare < 0.1)
                    hints.add ("reference glides into " + juce::String (juce::roundToInt (rv.glideShare * 100.0)) + " % of its notes (~"
                               + juce::String (juce::roundToInt (std::isnan (rv.glideMs) ? 0.0 : rv.glideMs))
                               + juce::String::fromUTF8 (" ms), the mix does not — mono/legato with portamento, or pitch bends (audioToMidi bends: true)"));

                if (rv.vibratoShare >= 0.3 && mv.vibratoShare < 0.1)
                    hints.add ("reference has vibrato on " + juce::String (juce::roundToInt (rv.vibratoShare * 100.0))
                               + juce::String::fromUTF8 (" % of its notes, the mix none — an LFO on pitch or the mod wheel"));

                const double dp = mv.meanPitch - rv.meanPitch;

                if (std::abs (dp) >= 5.0)
                    hints.add ("mix sits " + juce::String (std::abs (dp), 1) + " semitones " + (dp < 0 ? "lower" : "higher")
                               + " than the reference (mean " + voice::noteName ((int) std::round (mv.meanPitch)) + " vs "
                               + voice::noteName ((int) std::round (rv.meanPitch)) + ")"
                               + (std::abs (std::abs (dp) - 12.0) <= 2.0 ? juce::String::fromUTF8 (" — about an octave") : juce::String()));
            }
        }

        // Hinweise in Worten — die Zahlen stehen in diff
        auto ld = diffOf (mix.getProperty ("lufs", {}), ref.getProperty ("lufs", {}));

        if (! ld.isVoid() && std::abs ((double) ld) >= 1.0)
            hints.insert (0, juce::String ("mix is ") + juce::String (std::abs ((double) ld)) + " LU "
                                 + ((double) ld < 0 ? "quieter" : "louder") + " than the reference");

        auto td = (double) mix.getProperty ("truePeakDb", -100.0);

        if (td > -1.0)
            hints.add ("mix true peak " + juce::String (td) + " dBTP is above -1 dBTP");

        auto lr = diffOf (mix.getProperty ("lra", {}), ref.getProperty ("lra", {}));

        if (! lr.isVoid() && std::abs ((double) lr) >= 3.0)
            hints.add (juce::String ("mix is ") + ((double) lr < 0 ? "less" : "more") + " dynamic (LRA "
                       + juce::String ((double) lr) + " LU vs reference)");

        // Limiter-Hinweise des Mix (analyzeMix) gehoeren zu den Hinweisen
        if (auto* mh = mix.getProperty ("hints", {}).getArray())
            for (auto& h : *mh)
                hints.add (h.toString());

        mix.getDynamicObject()->removeProperty ("hints");
        juce::Array<juce::var> hintList;

        for (auto& h : hints)
            hintList.add (h);

        ref.getDynamicObject()->setProperty ("file", refFile.getFullPathName());
        return resultObj (true, { { "mix", mix }, { "reference", ref }, { "diff", juce::var (diff) }, { "hints", hintList } });
    }

    juce::var opPeak (DawdContext& ctx, const juce::var&)
    {
        auto& edit = *ctx.edit;
        juce::BigInteger tracks;
        tracks.setRange (0, (int) te::getAllTracks (edit).size(), true);

        // Geraet zuerst frei, dann Solo aus — sonst waere der Mix ohne Solo
        // kurz hoerbar (measureStatistics gibt es selbst erst danach frei)
        const te::Edit::ScopedRenderStatus srs (edit, true);
        const ScopedListeningOff noListening (ctx, edit);
        auto stats = te::Renderer::measureStatistics ("peak", edit, { 0_tp, edit.getLength() }, tracks, 512);
        return resultObj (true, { { "peak", stats.peak }, { "average", stats.average },
                                  { "audioDuration", stats.audioDuration } });
    }

    juce::var opDump (DawdContext& ctx, const juce::var&)
    {
        auto xml = ctx.edit->state.createXml()->toString();
        return resultObj (true, { { "xml", xml } });
    }

    namespace
    {
        // Built-in-Module ohne Plugin-Scan: Name (wie listPlugins ihn meldet),
        // Tracktion-Typ (uid), Klasse
        struct Builtin { const char* name; const char* type; const char* cls; const char* kit = nullptr; };

        const Builtin builtins[] = {
            { "4OSC",          te::FourOscPlugin::xmlTypeName,      "instrument" },
            { "Sampler",       te::SamplerPlugin::xmlTypeName,      "instrument" },
            { "Drum Kit",      te::SamplerPlugin::xmlTypeName,      "instrument", "basic" },
            { "MIDI Modifier", te::MidiModifierPlugin::xmlTypeName, "midiFx" },
            { "Reverb",        te::ReverbPlugin::xmlTypeName,       "audioFx" },
            { "Delay",         te::DelayPlugin::xmlTypeName,        "audioFx" },
            { "Chorus",        te::ChorusPlugin::xmlTypeName,       "audioFx" },
            { "Compressor",    te::CompressorPlugin::xmlTypeName,   "audioFx" },
            { "Limiter",       LimiterPlugin::xmlTypeName,          "audioFx" },
            { "Equaliser",     te::EqualiserPlugin::xmlTypeName,    "audioFx" },
            { "Low Pass",      te::LowPassPlugin::xmlTypeName,      "audioFx" },
            { "Phaser",        te::PhaserPlugin::xmlTypeName,       "audioFx" },
            { "Pitch Shifter", te::PitchShiftPlugin::xmlTypeName,   "audioFx" },
        };

        // Modul per Id aufloesen und sein Plugin holen
        te::Plugin* pluginFor (DawdContext& ctx, const juce::var& cmd, juce::String& id)
        {
            id = cmd.getProperty ("plugin", {}).toString();

            if (auto* m = findModuleTrack (*ctx.edit, id))
                return modulePlugin (*m);

            return nullptr;
        }

        // Kantenaenderungen eines Splices erst pruefen, dann ausfuehren —
        // ein Fehler laesst das Routing unveraendert
        juce::String checkClass (const juce::String& out, const juce::String& in, const juce::String& to)
        {
            if (out == "midi" && in == "audio")
                return to == "main" ? "midi cannot reach main" : "midi needs an instrument or midi fx";

            if (out == "audio" && in == "midi")
                return "audio cannot enter a midi plugin";

            return {};
        }

        struct SplicePlan
        {
            juce::String error, trackId, at, after, before;
            juce::StringArray dropFrom, dropTo, linkIn, linkOut;
            std::map<juce::String, double> inLevel, outLevel;   // Pegel der ersetzten Kanten

            // Post-Fader-Sends, die ans neue Kettenende ziehen (from -> to
            // wird neu -> to, Pegel und Art bleiben)
            struct Move { juce::String from, to; double level; bool send; };
            std::vector<Move> moveSends;
        };

        // Splice vorab pruefen (track + at): welche Kanten fallen weg, welche
        // entstehen — ein Fehler laesst das Routing unveraendert
        SplicePlan planSplice (te::Edit& edit, const juce::var& cmd, const juce::String& cls)
        {
            SplicePlan plan;
            auto at = cmd.getProperty ("at", {}).toString();

            // at: "after" mit after meint after allein; track ohne at das
            // Ende der Kette (Praxistest remix-1: beides war ein Fehler)
            if (at == "after" && cmd.hasProperty ("after"))
                at = {};
            else if (at == "after")
                { plan.error = "at: 'after' needs after: <node id or name>"; return plan; }

            if (at.isEmpty() && cmd.hasProperty ("track") && ! cmd.hasProperty ("after"))
                at = "end";

            plan.at = at;

            // Direkt hinter ein Modul (oder eine Spur): das neue Modul
            // uebernimmt dessen Ziele samt Pegeln — mitten in eine Kette in
            // einem Befehl statt disconnect/connect/connect
            if (cmd.hasProperty ("after"))
            {
                if (cmd.hasProperty ("track") || at.isNotEmpty())
                    { plan.error = juce::String::fromUTF8 ("after replaces track/at — use one"); return plan; }

                auto fromId = cmd.getProperty ("after", {}).toString();
                auto src = findRouteNode (edit, fromId);

                if (src.error.isNotEmpty() || src.kind == "main")
                    { plan.error = src.error.isNotEmpty() ? src.error : juce::String ("nothing comes after main"); return plan; }

                if (auto err = checkClass (src.out, classInput (cls), {}); err.isNotEmpty())
                    { plan.error = err; return plan; }

                for (auto& to : targetsOf (edit, src))
                {
                    if (auto err = checkClass (classOutput (cls), findRouteNode (edit, to).in, to); err.isNotEmpty())
                        { plan.error = err; return plan; }

                    plan.dropFrom.add (fromId); plan.dropTo.add (to);
                    plan.linkOut.add (to);
                    plan.outLevel[to] = edgeLevel (edit, fromId, to);
                }

                plan.linkIn.add (fromId);
                plan.after = fromId;
                return plan;
            }

            if (cmd.hasProperty ("before") && at != "master")
                { plan.error = "before needs at: master"; return plan; }

            // Summenkette: alle Wege nach Main laufen durch das neue Modul
            // (Pegel der Kanten bleiben), es geht selbst nach Main; mit
            // before vor ein Modul der Kette (Praxistest 1l: sonst immer
            // hinter den Limiter)
            if (at == "master")
            {
                if (classInput (cls) != "audio" || classOutput (cls) != "audio")
                    { plan.error = "at master needs an audio effect"; return plan; }

                if (cmd.hasProperty ("before"))
                {
                    auto before = cmd.getProperty ("before", {}).toString();

                    if (findModuleTrack (edit, before) == nullptr)
                        { plan.error = "no such plugin: " + before; return plan; }

                    if (! isMasterModule (edit, before))
                        { plan.error = "before must be a module of the master chain: " + before; return plan; }

                    for (auto& [from, to] : routingEdges (edit))
                        if (to == before)
                        {
                            plan.dropFrom.add (from); plan.dropTo.add (before);
                            plan.linkIn.add (from);
                            plan.inLevel[from] = edgeLevel (edit, from, before);
                        }

                    plan.linkOut.add (before);
                    plan.before = before;
                    return plan;
                }

                for (auto& [from, to] : routingEdges (edit))
                    if (to == "main")
                    {
                        plan.dropFrom.add (from); plan.dropTo.add ("main");
                        plan.linkIn.add (from);
                        plan.inLevel[from] = edgeLevel (edit, from, "main");
                    }

                plan.linkOut.add ("main");
                return plan;
            }

            if (cmd.hasProperty ("track") || at.isNotEmpty())
            {
                if (at != "start" && at != "end")
                    { plan.error = "at must be start, end or master"; return plan; }

                auto tracks = userTracks (edit);
                auto ti = (int) (double) cmd.getProperty ("track", -1.0);

                if (ti < 0 || ti >= tracks.size())
                    { plan.error = "no such track"; return plan; }

                // Eine Lane meint ihre Gruppe
                if (auto* g = laneGroupOf (*tracks[ti]))
                    ti = tracks.indexOf (g);

                plan.trackId = "t" + juce::String (ti);
                auto src = findRouteNode (edit, plan.trackId);

                if (at == "start")
                {
                    // Audio-Effekt auf einer MIDI-Spur: an den Anfang des
                    // Audio-Teils, also direkt hinter ihr Instrument
                    auto fromId = plan.trackId;

                    if (src.out == "midi" && classInput (cls) == "audio")
                    {
                        juce::String inst;

                        if (faderTrackOf (edit, *tracks[ti], nullptr, &inst) == nullptr || inst.isEmpty())
                            { plan.error = juce::String::fromUTF8 ("MIDI track has no instrument yet — add one first (pickInstrument), then the effect goes behind it"); return plan; }

                        fromId = inst;
                        src = findRouteNode (edit, inst);
                    }

                    if (auto err = checkClass (src.out, classInput (cls), {}); err.isNotEmpty())
                        { plan.error = err; return plan; }

                    for (auto& to : targetsOf (edit, src))
                    {
                        if (auto err = checkClass (classOutput (cls), findRouteNode (edit, to).in, to); err.isNotEmpty())
                            { plan.error = err; return plan; }

                        plan.dropFrom.add (fromId); plan.dropTo.add (to);
                        plan.linkOut.add (to);
                        plan.outLevel[to] = edgeLevel (edit, fromId, to);
                    }

                    plan.linkIn.add (fromId);
                }
                else
                {
                    // Alle Knoten auf den Wegen der Spur — die Master-Kette
                    // gehoert allen Spuren und zaehlt wie Main, sonst landet
                    // ein Spur-Effekt hinter dem Master-Limiter; ebenso eine
                    // Gruppe (Submix): der Effekt kommt vor sie
                    auto isEnd = [&edit] (const juce::String& id) { return id == "main" || isMasterModule (edit, id) || id.startsWithChar ('g'); };

                    // Sends sind kein Weg der Spur (Matthias 2026-10-24, R1):
                    // markierte Kanten und die Post-Sends hinter dem letzten
                    // Modul bleiben liegen, sonst landete das Modul auch
                    // hinter dem Hall
                    juce::StringArray complex, sends, post;
                    auto chain = ownChainOf (edit, plan.trackId, complex, sends, &post);
                    const auto last = chain.isEmpty() ? juce::String() : chain[chain.size() - 1];
                    auto pathTargets = [&] (const juce::String& id)
                    {
                        juce::StringArray out;

                        for (auto& to : targetsOf (edit, findRouteNode (edit, id)))
                            if (! isSendEdge (edit, id, to) && ! (id == last && post.contains (to)))
                                out.add (to);

                        return out;
                    };

                    juce::StringArray reach, todo { plan.trackId };

                    while (! todo.isEmpty())
                    {
                        auto id = todo[0];
                        todo.remove (0);

                        if (isEnd (id) || reach.contains (id))
                            continue;

                        reach.add (id);
                        todo.addArray (pathTargets (id));
                    }

                    for (auto& id : reach)
                        for (auto& to : pathTargets (id))
                            if (isEnd (to))
                            {
                                plan.dropFrom.add (id); plan.dropTo.add (to);
                                plan.linkIn.addIfNotAlreadyThere (id);
                                plan.linkOut.addIfNotAlreadyThere (to);
                                plan.inLevel[id] = edgeLevel (edit, id, to);
                            }

                    if (! plan.linkIn.isEmpty())
                    {
                        if (auto err = checkClass (classOutput (cls), "audio", "main"); err.isNotEmpty())
                            { plan.error = err; return plan; }
                    }
                    else
                    {
                        // Kein Weg nach Main: ans Ende der Kette (Knoten ohne Ziel)
                        for (auto& id : reach)
                            if (pathTargets (id).isEmpty())
                                plan.linkIn.add (id);
                    }

                    for (auto& id : plan.linkIn)
                        if (auto err = checkClass (findRouteNode (edit, id).out, classInput (cls), {}); err.isNotEmpty())
                            { plan.error = err; return plan; }

                    // Die Post-Sends ziehen ans neue Kettenende mit (wie
                    // setChain): Kette simple, Fader am Ende
                    if (plan.linkIn.contains (last))
                        for (auto& to : post)
                            plan.moveSends.push_back ({ last, to, edgeLevel (edit, last, to), isSendEdge (edit, last, to) });
                }
            }

            return plan;
        }

        void applySplice (te::Edit& edit, const SplicePlan& plan, const juce::String& id)
        {
            for (int i = 0; i < plan.dropFrom.size(); ++i)
                disconnectNodes (edit, plan.dropFrom[i], plan.dropTo[i]);

            for (auto& from : plan.linkIn)
                connectNodes (edit, from, id, plan.inLevel.count (from) > 0 ? plan.inLevel.at (from) : 1.0);

            for (auto& to : plan.linkOut)
                connectNodes (edit, id, to, plan.outLevel.count (to) > 0 ? plan.outLevel.at (to) : 1.0);

            for (auto& m : plan.moveSends)
            {
                disconnectNodes (edit, m.from, m.to);
                connectNodes (edit, id, m.to, m.level, m.send);
            }
        }
    }

    namespace
    {
        // Plugin-Ordner aller Formate durchsuchen und in die knownPluginList
        // der Engine eintragen; jedes Plugin nur einmal (LV2 liefert dasselbe
        // ueber mehrere Suchpfade/Bundles)
        // Je Plugin-Datei das Ergebnis des letzten Scans (Prozess-Lebensdauer,
        // nur Message-Thread): findAllTypesForFile laedt jedes Plugin — der
        // volle Scan dauerte 15 s auf dem Message-Thread (Play und alles
        // andere warteten), bei jedem listPlugins und jedem UI-Neuladen.
        // Neu gescannt wird nur eine neue oder geaenderte Datei (Aenderungszeit;
        // LV2 liefert URIs ohne Datei — die bleiben bis zum Neustart).
        // Mit Plugin-Sandbox scannt ein Kindprozess (dawd --plugin-scan): ein
        // Plugin, das beim Scan abstuerzt, reisst dawd nicht mit; die Datei
        // steht dann mit error im Cache (bis sie sich aendert) und in
        // listPlugins scanErrors.
        struct ScannedFile
        {
            juce::Time modified;
            std::vector<juce::PluginDescription> descs;
            juce::String format, file, error;
        };

        std::map<juce::String, ScannedFile>& scanCache()
        {
            static std::map<juce::String, ScannedFile> cache;
            return cache;
        }

        std::vector<juce::PluginDescription> scanPlugins (DawdContext& ctx)
        {
            auto& pm = ctx.engine->getPluginManager();
            std::vector<juce::PluginDescription> out;
            juce::StringArray seen;
            auto& cache = scanCache();

            for (auto format : pm.pluginFormatManager.getFormats())
            {
                if (! format->canScanForPlugins())
                    continue;

                const auto files = format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true);
                auto modifiedOf = [] (const juce::String& file)
                {
                    return juce::File::isAbsolutePath (file) ? juce::File (file).getLastModificationTime() : juce::Time();
                };

                // Neue oder geaenderte Dateien: mit Sandbox in einem Kindprozess
                juce::StringArray todo;

                for (auto& file : files)
                    if (auto it = cache.find (format->getName() + "|" + file); it == cache.end() || it->second.modified != modifiedOf (file))
                        todo.add (file);

                if (! todo.isEmpty())
                {
                    std::vector<PluginScanResult> scanned;

                    if (pluginSandboxRequested())
                    {
                        scanned = scanPluginFilesInChild (format->getName(), todo);
                    }
                    else
                    {
                        for (auto& file : todo)
                        {
                            juce::OwnedArray<juce::PluginDescription> found;
                            format->findAllTypesForFile (found, file);
                            PluginScanResult r;

                            for (auto* d : found)
                                r.descs.push_back (*d);

                            scanned.push_back (std::move (r));
                        }
                    }

                    for (int i = 0; i < todo.size(); ++i)
                        cache.insert_or_assign (format->getName() + "|" + todo[i],
                                                ScannedFile { modifiedOf (todo[i]), std::move (scanned[(size_t) i].descs),
                                                              format->getName(), todo[i], scanned[(size_t) i].error });
                }

                for (auto file : files)
                {
                    auto it = cache.find (format->getName() + "|" + file);

                    if (it == cache.end())
                        continue;

                    for (auto& desc : it->second.descs)
                    {
                        auto* d = &desc;
                        auto key = d->pluginFormatName + ":" + d->name + ":" + juce::String::toHexString (d->uniqueId);

                        if (seen.contains (key))
                            continue;

                        seen.add (key);
                        pm.knownPluginList.addType (*d);
                        out.push_back (*d);
                    }
                }
            }

            return out;
        }

        // Dateien, deren Scan scheiterte (Absturz, Frist) — aus dem Cache
        juce::Array<juce::var> scanErrors()
        {
            juce::Array<juce::var> errors;

            for (auto& [key, sf] : scanCache())
                if (sf.error.isNotEmpty())
                {
                    auto o = new juce::DynamicObject();
                    o->setProperty ("format", sf.format);
                    o->setProperty ("file", sf.file);
                    o->setProperty ("error", sf.error);
                    errors.add (juce::var (o));
                }

            return errors;
        }
    }

    // Plugins: alles Installierte (Default — das UI) oder mit core: true nur
    // das Kernset (dawd/plugin-profiles/coreset.json, je Plugin ein Eintrag im
    // bevorzugten Format, formats = alle vorhandenen) — die MCP-Schicht setzt
    // core fuer Agenten. query = alle Worte in Name/Kategorie/Zweck/Hersteller,
    // class = instrument | audioFx | midiFx.
    juce::var opListPlugins (DawdContext& ctx, const juce::var& cmd)
    {
        struct Entry { juce::String name, format, uid, cls, vendor, version; };
        std::vector<Entry> all;

        for (auto& b : builtins)
            all.push_back ({ b.name, "builtin", b.type, b.cls, "unidaw", {} });

        for (auto& d : scanPlugins (ctx))
            all.push_back ({ d.name, d.pluginFormatName, juce::String::toHexString (d.uniqueId),
                             d.isInstrument ? "instrument" : "audioFx", d.manufacturerName, d.version });

        const bool core = (bool) cmd.getProperty ("core", false);
        auto cls = cmd.getProperty ("class", {}).toString();
        auto words = juce::StringArray::fromTokens (cmd.getProperty ("query", {}).toString().toLowerCase(), true);
        words.removeEmptyStrings();

        auto entryVar = [] (const Entry& e, const CoreEntry* c)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("name", e.name);
            o->setProperty ("format", e.format);
            o->setProperty ("uid", e.uid);
            o->setProperty ("class", e.cls);

            if (e.vendor.isNotEmpty())
                o->setProperty ("vendor", e.vendor);

            if (e.version.isNotEmpty())
                o->setProperty ("version", e.version);

            if (c != nullptr)
            {
                o->setProperty ("category", c->category);
                o->setProperty ("purpose", c->purpose);
            }

            auto slug = profileSlug (e.format, e.uid);
            o->setProperty ("profile", profilesDirectory().getChildFile (slug + ".json").existsAsFile());
            return o;
        };

        auto matches = [&] (const Entry& e, const CoreEntry* c)
        {
            if (cls.isNotEmpty() && e.cls != cls)
                return false;

            auto hay = (e.name + " " + e.vendor + (c != nullptr ? " " + c->category + " " + c->purpose : juce::String())).toLowerCase();

            // je Suchwort ein Wort, das damit beginnt — "amp" trifft "amp-like",
            // nicht "Sampler" (Praxistest 1k)
            juce::StringArray tokens;
            juce::String cur;

            for (auto ch : hay)
            {
                if (juce::CharacterFunctions::isLetterOrDigit (ch))
                    cur += juce::String::charToString (ch);
                else if (cur.isNotEmpty())
                    tokens.add (cur), cur.clear();
            }

            if (cur.isNotEmpty())
                tokens.add (cur);

            for (auto& w : words)
            {
                bool found = false;

                for (auto& t : tokens)
                    found = found || t.startsWith (w);

                if (! found && ! (w.containsChar (' ') && hay.contains (w)))
                    return false;
            }

            return true;
        };

        juce::Array<juce::var> plugins;

        if (core)
        {
            for (auto& c : coreSet())
            {
                const Entry* pick = nullptr;
                juce::StringArray formats;

                for (auto& e : all)
                    if (e.name.equalsIgnoreCase (c.name))
                    {
                        formats.addIfNotAlreadyThere (e.format);

                        if (pick == nullptr || e.format.equalsIgnoreCase (c.format))
                            pick = &e;
                    }

                if (pick == nullptr || ! matches (*pick, &c))
                    continue;

                auto* o = entryVar (*pick, &c);
                o->setProperty ("formats", formats);
                plugins.add (juce::var (o));
            }
        }
        else
        {
            for (auto& e : all)
                if (matches (e, coreEntryFor (e.name)))
                {
                    plugins.add (juce::var (entryVar (e, coreEntryFor (e.name))));
                }
        }

        auto r = resultObj (true, { { "plugins", plugins } });

        // Plugin-Dateien, deren Scan scheiterte (Absturz im Scan-Kind, Frist)
        if (auto errors = scanErrors(); ! errors.isEmpty())
            r.getDynamicObject()->setProperty ("scanErrors", errors);

        if (core)
            r.getDynamicObject()->setProperty ("note", juce::String::fromUTF8 ("core set only — listPlugins {all: true} lists every installed plugin"));

        return r;
    }

    // Plugin ins Projekt: ein Modul in der Routing-Matrix, unverdrahtet.
    // Abkuerzung track + at: "start" (direkt hinter die Spur, vor ihre
    // bisherigen Ziele) oder "end" (vor Main bzw. der Master-Kette auf den Wegen der Spur, ohne
    // Weg nach Main ans Ende ihrer Kette) — nur wenn die Typen passen.
    namespace
    {
        // Plugin aufloesen: Built-in (Name oder Typ) oder gescanntes Plugin —
        // namensgleiche Plugins mehrerer Formate (z. B. testgain als VST3 und
        // LV2) unterscheidet der optionale Filter format/uid aus listPlugins
        struct ResolvedPlugin
        {
            juce::ValueTree state;
            juce::String name, format, uid, cls, error;
        };

        juce::String loadKit (te::SamplerPlugin& sampler, const juce::String& kit)
        {
            auto dir = kitsDirectory().getChildFile (kit);
            auto json = juce::JSON::parse (dir.getChildFile ("kit.json"));
            auto* sounds = json.getProperty ("sounds", {}).getArray();

            if (sounds == nullptr || sounds->isEmpty())
                return "drum kit missing: " + dir.getChildFile ("kit.json").getFullPathName();

            for (auto& s : *sounds)
            {
                auto file = dir.getChildFile (s.getProperty ("file", {}).toString());
                auto note = (int) s.getProperty ("note", 36);

                if (! file.existsAsFile())
                    return "drum kit sample missing: " + file.getFullPathName();

                if (auto err = sampler.addSound (file.getFullPathName(), s.getProperty ("name", {}).toString(), 0.0, 0.0, 0.0f); err.isNotEmpty())
                    return err;

                auto idx = sampler.getNumSounds() - 1;
                sampler.setSoundParams (idx, note, note, note);
                sampler.setSoundOpenEnded (idx, true);
                sampler.setSoundGroups (idx, 0, (int) s.getProperty ("choke", 0));   // Hats: geschlossene stoppt offene
            }

            return {};
        }

        ResolvedPlugin resolvePlugin (DawdContext& ctx, const juce::String& name, const juce::String& format,
                                      const juce::String& uid)
        {
            ResolvedPlugin r;
            r.name = name;

            // Anzeigename der Engine je eingebautem Typ ("4-Band Equaliser"
            // fuer "Equaliser") — Rezepte speicherten ihn (Praxistest 1f:
            // applyRecipe "unknown plugin: 4-Band Equaliser")
            static std::map<juce::String, juce::String> engineNames;

            if (engineNames.empty())
                for (auto& b : builtins)
                    if (auto p = ctx.edit->getPluginCache().createNewPlugin (b.type, {}))
                        engineNames[b.type] = p->getName();

            if (format.isEmpty() || format.equalsIgnoreCase ("builtin"))
                for (auto& b : builtins)
                    if (name.equalsIgnoreCase (b.name) || name.equalsIgnoreCase (b.type) || name.equalsIgnoreCase (engineNames[b.type])
                        || (uid.isNotEmpty() && uid.equalsIgnoreCase (b.type)))
                    {
                        if (auto p = ctx.edit->getPluginCache().createNewPlugin (b.type, {}))
                        {
                            if (b.kit != nullptr)
                                if (auto* sp = dynamic_cast<te::SamplerPlugin*> (p.get()))
                                    if (auto err = loadKit (*sp, b.kit); err.isNotEmpty())
                                    {
                                        r.error = err;
                                        return r;
                                    }

                            r.state = p->state;
                            r.name = b.name;
                            r.format = "builtin";
                            r.uid = b.type;
                            r.cls = b.cls;
                        }

                        break;
                    }

            if (! r.state.isValid())
            {
                juce::PluginDescription desc;

                auto find = [&]
                {
                    for (auto& t : ctx.engine->getPluginManager().knownPluginList.getTypes())
                        if (t.name.equalsIgnoreCase (name)
                            && (format.isEmpty() || t.pluginFormatName.equalsIgnoreCase (format))
                            && (uid.isEmpty() || juce::String::toHexString (t.uniqueId).equalsIgnoreCase (uid)))
                            { desc = t; return; }
                };

                find();

                // Neu installiert und noch nicht gescannt (Dexed nach der
                // Installation): einmal die Plugin-Ordner durchsuchen
                if (desc.name.isEmpty())
                {
                    scanPlugins (ctx);
                    find();
                }

                if (desc.name.isEmpty())
                {
                    r.error = "unknown plugin: " + name;
                    return r;
                }

                r.state = te::ExternalPlugin::create (*ctx.engine, desc);
                r.format = desc.pluginFormatName;
                r.uid = juce::String::toHexString (desc.uniqueId);
                r.cls = desc.isInstrument ? "instrument" : "audioFx";
            }

            return r;
        }
    }

    juce::ValueTree newPluginState (DawdContext& ctx, const juce::String& name, const juce::String& format,
                                    const juce::String& uid, juce::String& error)
    {
        auto r = resolvePlugin (ctx, name, format, uid);
        error = r.error;
        return r.error.isEmpty() ? r.state : juce::ValueTree();
    }

    namespace
    {
        // paramQuery von pluginInfo und listParameters: ein Name mit allen
        // Woertern ("filter cutoff"); hat keiner alle, Namen mit einem davon
        // (anyWord, die Antwort traegt dann note) — Agenten fragen mehrere
        // Regler auf einmal ab ("filter amp attack", Praxistest 1k/1m/1n:
        // 0 Treffer). '|' trennt Alternativen ("tune|amp attack": ein Name
        // mit allen Woertern einer davon; Praxistest stil-1: 0 Treffer ohne
        // Hinweis)
        struct ParamQuery
        {
            static constexpr const char* note = "no parameter name has all words of paramQuery: listed are the names with any of them";

            ParamQuery (te::Plugin& p, const juce::var& cmd) : plugin (p)
            {
                text = cmd.getProperty ("paramQuery", {}).toString().trim();

                for (auto& alt : juce::StringArray::fromTokens (text.toLowerCase(), "|", {}))
                {
                    auto w = juce::StringArray::fromTokens (alt, true);
                    w.removeEmptyStrings();

                    if (! w.isEmpty())
                        alternatives.push_back (w);
                }

                if (alternatives.size() == 1)
                    words = alternatives.front();

                anyWord = words.size() > 1;

                for (auto* param : plugin.getAutomatableParameters())
                    anyWord = anyWord && ! matches (*param, true);
            }

            bool matches (te::AutomatableParameter& p) const { return matches (p, ! anyWord); }

            // Hinweis zur Antwort: anyWord, oder kein Treffer
            juce::String noteFor (int matched) const
            {
                if (anyWord)
                    return note;

                if (matched == 0 && ! alternatives.empty())
                    return juce::String::fromUTF8 ("no parameter name matches paramQuery '") + text
                           + juce::String::fromUTF8 ("' — words match parts of names, '|' separates alternatives; without paramQuery all are listed");

                return {};
            }

            te::Plugin& plugin;
            juce::String text;
            juce::StringArray words;
            std::vector<juce::StringArray> alternatives;
            bool anyWord = false;

        private:
            bool matches (te::AutomatableParameter& p, bool all) const
            {
                const auto label = parameterLabel (plugin, p).toLowerCase();

                if (alternatives.size() > 1)
                {
                    for (auto& alt : alternatives)
                    {
                        bool hit = true;

                        for (auto& w : alt)
                            hit = hit && label.contains (w);

                        if (hit)
                            return true;
                    }

                    return false;
                }

                bool hit = all || words.isEmpty();

                for (auto& w : words)
                    hit = all ? hit && label.contains (w) : hit || label.contains (w);

                return hit;
            }
        };

        // Zahlen eines Namens ("Pulse Width 2" -> {"2"})
        juce::StringArray numbersIn (const juce::String& s)
        {
            juce::StringArray out;
            juce::String cur;

            for (auto c : s + " ")
            {
                if (juce::CharacterFunctions::isDigit (c))
                    cur += c;
                else if (cur.isNotEmpty())
                    out.add (cur), cur.clear();
            }

            return out;
        }

        // Ganze Zahl from -> to ersetzen ("Wave 1 = square" -> "Wave 2 = square",
        // aber nicht die 1 in "12")
        juce::String replaceNumber (const juce::String& s, const juce::String& from, const juce::String& to)
        {
            juce::String out;
            int i = 0;

            while (i < s.length())
            {
                auto isDigitAt = [&s] (int k) { return k >= 0 && k < s.length() && juce::CharacterFunctions::isDigit (s[k]); };

                if (s.substring (i).startsWith (from) && ! isDigitAt (i - 1) && ! isDigitAt (i + from.length()))
                {
                    out += to;
                    i += from.length();
                }
                else
                {
                    out += s[i++];
                }
            }

            return out;
        }

        // Messung eines Parameters (sameAs folgt dem gemessenen Vertreter; die
        // Nummer des Vertreters wird in needs auf die eigene umgeschrieben —
        // "Pulse Width 2" braucht "Wave 2 = square", nicht "Wave 1")
        juce::var measuredFor (const juce::var& measured, const juce::String& name)
        {
            auto m = measured.getProperty (juce::Identifier (name), {});

            if (! m.hasProperty ("sameAs"))
                return m;

            auto source = m.getProperty ("sameAs", {}).toString();
            m = measured.getProperty (juce::Identifier (source), {});
            auto* needs = m.getProperty ("needs", {}).getArray();
            auto from = numbersIn (source), to = numbersIn (name);

            if (needs == nullptr || from.size() != to.size() || from == to)
                return m;

            juce::Array<juce::var> renamed;

            for (auto& n : *needs)
            {
                auto s = n.toString();

                for (int k = 0; k < from.size(); ++k)
                    if (from[k] != to[k])
                        s = replaceNumber (s, from[k], to[k]);

                renamed.add (s);
            }

            // Kopie — der geladene Steckbrief bleibt unveraendert
            auto copy = new juce::DynamicObject (*m.getDynamicObject());
            copy->setProperty ("needs", renamed);
            return juce::var (copy);
        }
    }

    juce::var profileMeasurement (const juce::var& measured, const juce::String& name)
    {
        return measuredFor (measured, name);
    }

    // Steckbrief eines Plugins: {plugin} (ein Modul im Projekt) oder {name,
    // format?, uid?} (wird kurz geladen). Kernset-Eintrag (Kategorie, Zweck),
    // Parameter mit Verlauf (paramQuery/limit), Presets (Kategorien),
    // gemessener Steckbrief und Kurznotiz aus dawd/plugin-profiles.
    juce::var opPluginInfo (DawdContext& ctx, const juce::var& cmd)
    {
        te::Plugin::Ptr temp;
        te::Plugin* plugin = nullptr;
        juce::String id;

        if (cmd.hasProperty ("plugin"))
        {
            plugin = pluginFor (ctx, cmd, id);

            if (plugin == nullptr)
                return resultObj (false, { { "error", "no such plugin: " + id } });
        }
        else
        {
            auto resolved = resolvePlugin (ctx, cmd.getProperty ("name", {}).toString(),
                                           cmd.getProperty ("format", {}).toString(), cmd.getProperty ("uid", {}).toString());

            if (resolved.error.isNotEmpty())
                return resultObj (false, { { "error", resolved.error } });

            temp = ctx.edit->getPluginCache().createNewPlugin (resolved.state);

            if (temp == nullptr)
                return resultObj (false, { { "error", "cannot load " + resolved.name } });

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (temp.get()))
                ep->initialiseFully();

            plugin = temp.get();
        }

        juce::String name = plugin->getName(), format = "builtin", uid = plugin->getPluginType(), vendor = "unidaw", version;

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (plugin))
        {
            name = ep->desc.name;
            format = ep->desc.pluginFormatName;
            uid = juce::String::toHexString (ep->desc.uniqueId);
            vendor = ep->desc.manufacturerName;
            version = ep->desc.version;
        }
        else if (cmd.hasProperty ("name"))
            name = cmd.getProperty ("name", {}).toString();   // "Drum Kit" ist ein Sampler mit Kit

        auto o = new juce::DynamicObject();
        o->setProperty ("name", name);
        o->setProperty ("format", format);
        o->setProperty ("uid", uid);
        o->setProperty ("vendor", vendor);

        if (version.isNotEmpty())
            o->setProperty ("version", version);

        if (id.isNotEmpty())
            o->setProperty ("plugin", id);

        if (auto* c = coreEntryFor (name))
        {
            o->setProperty ("core", true);
            o->setProperty ("category", c->category);
            o->setProperty ("purpose", c->purpose);
        }
        else
            o->setProperty ("core", false);

        // Gemessener Steckbrief (tools/profile_plugin.py): je Parameter, was er tut
        auto slug = profileSlug (format, uid);
        auto prof = loadProfile (slug);
        auto measured = prof.getProperty ("measured", {}).getProperty ("parameters", {});

        // Parameter: Name, Skala, Bereich als Text; Filter fuer grosse Synths
        const ParamQuery query (*plugin, cmd);
        const int limit = juce::jlimit (1, 1000, (int) cmd.getProperty ("limit", 80));
        juce::Array<juce::var> params;
        int total = 0, matched = 0;

        for (auto* p : plugin->getAutomatableParameters())
        {
            ++total;
            auto label = parameterLabel (*plugin, *p);

            if (! query.matches (*p) || ++matched > limit)
                continue;

            auto shape = parameterShape (*p, 5);
            auto* texts = shape.getProperty ("texts", {}).getArray();
            auto po = new juce::DynamicObject();
            po->setProperty ("name", label);
            po->setProperty ("scale", shape.getProperty ("scale", {}));
            po->setProperty ("from", texts != nullptr ? texts->getFirst()[1] : juce::var());
            po->setProperty ("to", texts != nullptr ? texts->getLast()[1] : juce::var());
            po->setProperty ("now", p->getCurrentValueAsString());

            if (shape.hasProperty ("options"))
                po->setProperty ("options", shape.getProperty ("options", {}));

            if (auto m = measuredFor (measured, label); m.isObject())
            {
                po->setProperty ("does", m.getProperty ("summary", {}));
                po->setProperty ("strength", m.getProperty ("strength", {}));

                if (m.hasProperty ("needs"))
                    po->setProperty ("needs", m.getProperty ("needs", {}));
            }

            params.add (juce::var (po));
        }

        o->setProperty ("parameterCount", total);
        o->setProperty ("parameters", params);

        if (matched > limit)
            o->setProperty ("parametersTruncated", juce::String (matched - limit) + juce::String::fromUTF8 (" more — narrow with paramQuery or raise limit"));

        if (auto n = query.noteFor (matched); n.isNotEmpty())
            o->setProperty ("note", n);

        juce::Array<juce::var> settings;

        for (auto& st : pluginSettings (*plugin))
            settings.add (settingVar (st));

        if (! settings.isEmpty())
            o->setProperty ("settings", settings);

        // Presets: nur der Ueberblick (listPresets fuer die Liste)
        {
            auto presets = presetsFor (ctx, *plugin);
            std::map<juce::String, int> cats;

            for (auto& e : presets)
                cats[e.category]++;

            juce::Array<juce::var> catList;

            for (auto& [c, n] : cats)
            {
                auto co = new juce::DynamicObject();
                co->setProperty ("name", c);
                co->setProperty ("count", n);
                catList.add (juce::var (co));
            }

            o->setProperty ("presets", (int) presets.size());
            o->setProperty ("presetCategories", catList);
        }

        o->setProperty ("profileId", slug);

        if (! prof.isVoid())
        {
            // Kurzfassung — die Messpunkte je Parameter stehen in der Datei
            auto m = prof.getProperty ("measured", {});
            auto mo = new juce::DynamicObject();
            mo->setProperty ("signal", m.getProperty ("signal", {}));
            mo->setProperty ("generated", prof.getProperty ("generated", {}));
            mo->setProperty ("file", profilesDirectory().getChildFile (slug + ".json").getFullPathName());
            o->setProperty ("measured", juce::var (mo));
        }

        if (auto notes = loadNotes (slug); notes.isNotEmpty())
            o->setProperty ("notes", notes);

        // Rezepte, in denen das Plugin vorkommt (Rezept-Katalog)
        o->setProperty ("recipes", recipesWithPlugin (ctx, format, uid, 10));

        o->setProperty ("ok", true);
        return juce::var (o);
    }

    juce::var addInstrumentToTrack (DawdContext& ctx, int trackIndex, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto users = userTracks (edit);

        if (trackIndex < 0 || trackIndex >= users.size())
            return resultObj (false, { { "error", "no such track: " + juce::String (trackIndex) } });

        auto* track = users[trackIndex];

        if (laneGroupOf (*track) != nullptr)
            return resultObj (false, { { "error", "pick the instrument on the group track, not a lane" } });

        if (! isMidiTrack (*track))
            return resultObj (false, { { "error", "not a MIDI track (createTrack {type: 'midi'} makes one)" } });

        auto resolved = resolvePlugin (ctx, cmd.getProperty ("instrument", {}).toString(),
                                       cmd.getProperty ("format", {}).toString(), cmd.getProperty ("uid", {}).toString());

        if (resolved.error.isNotEmpty())
            return resultObj (false, { { "error", resolved.error } });

        if (resolved.cls != "instrument")
            return resultObj (false, { { "error", "not an instrument: " + resolved.name } });

        auto trackId = "t" + juce::String (trackIndex);

        // Hat die Spur schon ein Instrument (direkt oder hinter MIDI-
        // Effekten), wird es an seiner Stelle ersetzt — mit seinen
        // Ausgaengen (Effektkette) und seinem Namen (Praxistest 1e: das
        // neue lief parallel an der Kette vorbei, das alte spielte weiter)
        juce::String oldId, feeder = trackId;

        for (auto& t1 : targetsOf (edit, findRouteNode (edit, trackId)))
        {
            auto n1 = findRouteNode (edit, t1);

            if (n1.cls == "instrument")
            {
                oldId = t1;
                break;
            }

            if (n1.cls == "midiFx")
                for (auto& t2 : targetsOf (edit, n1))
                    if (findRouteNode (edit, t2).cls == "instrument")
                    {
                        oldId = t2, feeder = t1;
                        break;
                    }

            if (oldId.isNotEmpty())
                break;
        }

        auto id = addPluginModule (edit, resolved.state, "instrument");

        if (id.isEmpty())
            return resultObj (false, { { "error", "insert failed" } });

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (modulePlugin (*findModuleTrack (edit, id))))
            ep->initialiseFully();

        if (oldId.isNotEmpty())
        {
            auto* oldTrack = findModuleTrack (edit, oldId);
            const auto oldName = oldTrack != nullptr && modulePlugin (*oldTrack) != nullptr ? modulePlugin (*oldTrack)->getName() : oldId;
            const auto oldLabel = oldTrack != nullptr ? moduleLabel (*oldTrack) : juce::String();
            juce::StringArray outs;

            for (auto& [from, to] : routingEdges (edit))
                if (from == oldId)
                    outs.add (to);

            std::vector<double> levels;

            for (auto& to : outs)
                levels.push_back (edgeLevel (edit, oldId, to));

            removePluginModule (edit, oldId);

            if (auto err = connectNodes (edit, feeder, id); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            for (int i = 0; i < outs.size(); ++i)
                connectNodes (edit, id, outs[i], levels[(size_t) i]);

            if (oldLabel.isNotEmpty())
                setModuleLabel (*findModuleTrack (edit, id), oldLabel);

            auto res = resultObj (true, { { "id", id }, { "instrument", resolved.name },
                                          { "format", resolved.format }, { "uid", resolved.uid }, { "output", outs.joinIntoString (", ") } });
            res.getDynamicObject()->setProperty ("replaced", oldId + " (" + oldName + ")");
            return res;
        }

        if (auto err = connectNodes (edit, trackId, id); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // Hinter das Instrument die Master-Kette, wenn es eine gibt (Praxistest
        // 1b: neue Instrumente liefen an EQ und Limiter vorbei)
        auto out = mainEntry (edit);
        connectNodes (edit, id, out);
        return resultObj (true, { { "id", id }, { "instrument", resolved.name },
                                  { "format", resolved.format }, { "uid", resolved.uid }, { "output", out } });
    }

    // Instrument fuer eine (stumme) MIDI-Spur im Collect: Spur -> Instrument
    // -> Main, als collect-Aktion (Material reinholen hoerbar machen, kein
    // Sorten-Wechsel)
    juce::var opPickInstrument (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto r = addInstrumentToTrack (ctx, trackIndex, cmd);

        if (! (bool) r.getProperty ("ok", false))
            return r;

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("instrument", r.getProperty ("instrument", {}));
        params->setProperty ("id", r.getProperty ("id", {}));

        if (r.hasProperty ("replaced"))
            params->setProperty ("replaced", r.getProperty ("replaced", {}));

        recordNodeAction (ctx, "collect", "pickInstrument", juce::var (params));
        return r;
    }

    juce::var opAddPlugin (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto name = cmd.getProperty ("name", {}).toString();
        auto format = cmd.getProperty ("format", {}).toString();
        auto uid = cmd.getProperty ("uid", {}).toString();
        auto cls = cmd.getProperty ("class", {}).toString();

        if (cls.isNotEmpty() && ! isValidPluginClass (cls))
            return resultObj (false, { { "error", "class must be midiFx, instrument or audioFx" } });

        auto resolved = resolvePlugin (ctx, name, format, uid);

        if (resolved.error.isNotEmpty())
            return resultObj (false, { { "error", resolved.error } });

        juce::ValueTree pluginState = resolved.state;
        juce::String resolvedFormat = resolved.format, resolvedUid = resolved.uid, defaultCls = resolved.cls;
        name = resolved.name;

        if (cls.isEmpty())
            cls = defaultCls;

        auto plan = planSplice (edit, cmd, cls);

        if (plan.error.isNotEmpty())
            return resultObj (false, { { "error", plan.error } });

        const auto label = cmd.getProperty ("label", {}).toString().trim();

        if (auto err = checkModuleLabel (edit, {}, label); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto id = addPluginModule (edit, pluginState, cls);

        if (id.isEmpty())
            return resultObj (false, { { "error", "insert failed" } });

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (modulePlugin (*findModuleTrack (edit, id))))
            ep->initialiseFully();

        // MDA Image (mda.lv2): Mode 0,0 (Default, "Stereo image adjustment")
        // rechnet den Eingang als Seite/Mitte — ein Mono-Signal landet fast nur
        // links; L/R bleibt erst ab 0,6 (gemessen 2026-10-29, Praxistest
        // stil-1). dawd startet es auf 0,75 (Entscheidung Matthias 2026-10-29)
        juce::String startNote;

        if (resolvedFormat == "LV2" && resolvedUid == "6d12eb93")
            if (auto* p = findParameter (*modulePlugin (*findModuleTrack (edit, id)), "Mode"))
            {
                p->setNormalisedParameter (0.75f, juce::sendNotification);
                startNote = juce::String::fromUTF8 ("Mode set to 0.75 (left/right in, left/right out) — the plugin's own default 0.0 "
                                                    "treats left as side and right as mid, a mono source would end up on the left");
            }

        applySplice (edit, plan, id);

        if (label.isNotEmpty())
            setModuleLabel (*findModuleTrack (edit, id), label);

        // Hinter einem Master-Modul gehoert das neue zur Master-Kette
        if (plan.at == "master" || (plan.after.isNotEmpty() && isMasterModule (edit, plan.after)))
            markMasterModule (edit, id);

        // Processing am Node (kind process): das Modul ist die Aktion,
        // Parameter-Tweaks sind Verfeinerung und werden nur geloggt.
        // Format und uid stehen als geladene Aufloesung im Datensatz.
        auto params = new juce::DynamicObject();
        params->setProperty ("plugin", name);
        params->setProperty ("id", id);
        params->setProperty ("class", cls);
        params->setProperty ("format", resolvedFormat);
        params->setProperty ("uid", resolvedUid);

        if (plan.trackId.isNotEmpty())
        {
            params->setProperty ("track", plan.trackId.substring (1).getIntValue());
            params->setProperty ("at", plan.at);
        }

        if (plan.after.isNotEmpty())
            params->setProperty ("after", plan.after);

        if (plan.at == "master")
            params->setProperty ("at", plan.at);

        if (plan.before.isNotEmpty())
            params->setProperty ("before", plan.before);

        recordNodeAction (ctx, "process", "addPlugin", juce::var (params));

        auto res = resultObj (true, { { "id", id }, { "name", name }, { "class", cls },
                                      { "format", resolvedFormat }, { "uid", resolvedUid } });

        if (label.isNotEmpty())
            res.getDynamicObject()->setProperty ("label", label);

        if (startNote.isNotEmpty())
            res.getDynamicObject()->setProperty ("note", startNote);

        return res;
    }

    // Verlinkte Instanz: Kopie eines Moduls mit gleichen Einstellungen und
    // eigenem Signal (Simple-Mode: derselbe Reverb in zwei Ketten an
    // verschiedener Stelle); track + at wie bei addPlugin
    juce::var opLinkPlugin (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto src = cmd.getProperty ("plugin", {}).toString();
        auto* srcTrack = findModuleTrack (edit, src);

        if (srcTrack == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + src } });

        auto cls = findRouteNode (edit, src).cls;
        auto plan = planSplice (edit, cmd, cls);

        if (plan.error.isNotEmpty())
            return resultObj (false, { { "error", plan.error } });

        auto id = linkPluginModule (edit, src);

        if (id.isEmpty())
            return resultObj (false, { { "error", "link failed" } });

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (modulePlugin (*findModuleTrack (edit, id))))
            ep->initialiseFully();

        applySplice (edit, plan, id);

        if (plan.at == "master")
            markMasterModule (edit, id);

        auto link = moduleLink (*findModuleTrack (edit, id));

        auto params = new juce::DynamicObject();
        params->setProperty ("plugin", src);
        params->setProperty ("id", id);
        params->setProperty ("link", link);

        if (plan.trackId.isNotEmpty())
        {
            params->setProperty ("track", plan.trackId.substring (1).getIntValue());
            params->setProperty ("at", plan.at);
        }

        recordNodeAction (ctx, "process", "linkPlugin", juce::var (params));
        return resultObj (true, { { "id", id }, { "link", link }, { "class", cls } });
    }

    // Kette einer Spur setzen (Simple-Mode): Reihenfolge und Inhalt; mit
    // group statt track die eigene Kette einer Gruppe (Submix)
    juce::var opSetChain (DawdContext& ctx, const juce::var& cmd)
    {
        auto track = (int) (double) cmd.getProperty ("track", -1.0);
        const auto group = cmd.getProperty ("group", {}).toString();
        juce::StringArray plugins;

        if (auto* arr = cmd.getProperty ("plugins", {}).getArray())
            for (auto& p : *arr)
                plugins.add (p.toString());
        else
            return resultObj (false, { { "error", "plugins must be a list" } });

        if (cmd.hasProperty ("group") && findGroupTrack (*ctx.edit, group) == nullptr)
            return resultObj (false, { { "error", "no such group: " + group } });

        if (auto err = cmd.hasProperty ("group") ? setNodeChain (*ctx.edit, group, plugins)
                                                 : setTrackChain (*ctx.edit, track, plugins); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto params = new juce::DynamicObject();

        if (cmd.hasProperty ("group"))
            params->setProperty ("group", group);
        else
            params->setProperty ("track", track);

        params->setProperty ("plugins", cmd.getProperty ("plugins", {}));
        recordNodeAction (ctx, "process", "setChain", juce::var (params));

        if (cmd.hasProperty ("group"))
            return resultObj (true, { { "group", group }, { "plugins", cmd.getProperty ("plugins", {}) } });

        return resultObj (true, { { "track", track }, { "plugins", cmd.getProperty ("plugins", {}) } });
    }

    // Modul benennen: {plugin, name} ("" loescht den Namen) — der Name
    // unterscheidet gleiche Plugins ("Drums-Limiter", "Bass-Limiter") und
    // gilt ueberall als Adresse statt der Id. Verfeinerung: Undo, keine
    // Node-Aktion.
    juce::var opRenamePlugin (DawdContext& ctx, const juce::var& cmd)
    {
        auto ref = cmd.getProperty ("plugin", {}).toString();
        auto* m = findModuleTrack (*ctx.edit, ref);

        if (m == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + ref } });

        if (! cmd.hasProperty ("name"))
            return resultObj (false, { { "error", "need name (\"\" removes it)" } });

        auto id = moduleIdFor (*ctx.edit, ref);
        auto name = cmd.getProperty ("name", {}).toString().trim();

        if (auto err = checkModuleLabel (*ctx.edit, id, name); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        setModuleLabel (*m, name);
        saveCurrentEdit (ctx);
        auto* p = modulePlugin (*m);
        return resultObj (true, { { "plugin", id }, { "label", name.isEmpty() ? juce::var() : juce::var (name) },
                                  { "pluginName", p != nullptr ? p->getName() : juce::String() } });
    }

    // Eigene Oberflaeche eines Moduls (Editor des Plugins) in einem Fenster
    // auf dem Desktop: {plugin, open?, x?, y?} — ohne open umschalten, x/y
    // legen das Fenster dorthin. Oeffnen und Schliessen ohne Aenderung sind
    // keine Aenderung am Song: kein Undo, keine Node-Aktion, kein
    // Auto-Branch, read-only in der Registry. Hat sich der Plugin-Zustand
    // im Editor geaendert, bucht das Schliessen eine zusammengefasste
    // process-Aktion pluginWindow mit Undo und Auto-Branch (executeOp
    // bereitet das vor, Antwort changed: true). Die Lage gilt je Modul im
    // Projekt (windows.json).
    juce::var opPluginWindow (DawdContext& ctx, const juce::var& cmd)
    {
        auto ref = cmd.getProperty ("plugin", {}).toString();
        auto* m = findModuleTrack (*ctx.edit, ref);
        auto* p = m != nullptr ? modulePlugin (*m) : nullptr;

        if (p == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + ref } });

        auto id = moduleIdFor (*ctx.edit, ref);
        const bool open = cmd.hasProperty ("open") ? (bool) cmd.getProperty ("open", false) : ! pluginWindowOpen (*p);
        std::optional<juce::Point<int>> at;
        bool changed = false;

        if (cmd.hasProperty ("x") || cmd.hasProperty ("y"))
        {
            auto x = cmd.getProperty ("x", {}), y = cmd.getProperty ("y", {});

            if (! (x.isInt() || x.isInt64() || x.isDouble()) || ! (y.isInt() || y.isInt64() || y.isDouble()))
                return resultObj (false, { { "error", "need x and y (screen pixels)" } });

            if (! open)
                return resultObj (false, { { "error", "x and y place an open window (open: true)" } });

            at = juce::Point<int> (juce::roundToInt ((double) x), juce::roundToInt ((double) y));
        }

        if (open)
        {
            if (auto err = openPluginWindow (ctx, id, at); err.isNotEmpty())
                return resultObj (false, { { "error", err } });
        }
        else
        {
            changed = closePluginWindow (ctx, id);
        }

        auto res = resultObj (true, { { "plugin", id }, { "open", pluginWindowOpen (*p) } });

        if (changed)
            res.getDynamicObject()->setProperty ("changed", true);

        if (auto pos = pluginWindowPosition (ctx, id))
        {
            res.getDynamicObject()->setProperty ("x", pos->x);
            res.getDynamicObject()->setProperty ("y", pos->y);
        }

        if (ctx.windows->simulated)
            res.getDynamicObject()->setProperty ("simulated", true);

        return res;
    }

    // Plugin-Sandbox (Phase 3): {plugin} laedt ein Modul, dessen Host-Prozess
    // weg ist (Absturz, Haenger, Sperrliste), mit dem letzten Zustand neu und
    // hebt die Sperre fuer das Plugin auf. Laeuft der Host, nur die Sperre.
    // Kein Teil des Songs (keine Version, kein Undo).
    juce::var opReloadPlugin (DawdContext& ctx, const juce::var& cmd)
    {
        auto ref = cmd.getProperty ("plugin", {}).toString();
        auto* m = findModuleTrack (*ctx.edit, ref);
        auto* p = m != nullptr ? modulePlugin (*m) : nullptr;

        if (p == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + ref } });

        if (auto err = reloadSandboxedPlugin (*p); err.isNotEmpty())
            return resultObj (false, { { "error", err }, { "sandbox", sandboxInfo (*p) } });

        return resultObj (true, { { "plugin", moduleIdFor (*ctx.edit, ref) }, { "sandbox", sandboxInfo (*p) } });
    }

    // Plugin-Sandbox (Phase 3): {plugin, mode?} — wie dawd dieses Plugin hostet,
    // je Plugin (alle Module damit), in <config>/plugin-hosting.json:
    // plugin (Standard: alle Instanzen des Plugins in einem Host-Prozess),
    // single (je Instanz ein Prozess), dawd (im Prozess von dawd). Gilt, wenn
    // das Plugin neu geladen wird (Projekt oeffnen, Node wechseln, Undo).
    juce::var opPluginHosting (DawdContext& ctx, const juce::var& cmd)
    {
        auto ref = cmd.getProperty ("plugin", {}).toString();
        auto* m = findModuleTrack (*ctx.edit, ref);
        auto* p = m != nullptr ? modulePlugin (*m) : nullptr;

        if (p == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + ref } });

        if (! pluginSandboxRequested())
            return resultObj (false, { { "error", "the plugin sandbox is off (UNIDAW_PLUGIN_SANDBOX=0)" } });

        if (cmd.hasProperty ("mode"))
            if (auto err = setPluginHostingMode (*p, cmd.getProperty ("mode", {}).toString()); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        auto mode = pluginHostingMode (*p);

        if (mode.isEmpty())
            return resultObj (false, { { "error", setPluginHostingMode (*p, "?") } });

        auto sb = sandboxInfo (*p);
        const auto now = sb.isVoid() ? juce::String ("dawd") : sb.getProperty ("group", {}).toString();
        auto res = resultObj (true, { { "plugin", moduleIdFor (*ctx.edit, ref) }, { "name", p->getName() },
                                      { "mode", mode }, { "running", now } });

        if (now != mode)
            res.getDynamicObject()->setProperty ("note", "takes effect when the plugin is loaded again "
                                                         "(open the project again or switch node)");

        return res;
    }

    // Nur mit --window-sim (Test-Suite): {plugin, param, value 0..1} dreht
    // einen Regler im simulierten Editor, wie die Oberflaeche des Plugins —
    // keine Op-Aenderung, das Schliessen des Fensters bucht sie
    juce::var opPluginWindowSim (DawdContext& ctx, const juce::var& cmd)
    {
        auto v = cmd.getProperty ("value", {});

        if (! (v.isInt() || v.isInt64() || v.isDouble()))
            return resultObj (false, { { "error", "need param and value (0..1)" } });

        auto id = cmd.getProperty ("plugin", {}).toString();   // Name -> Id macht executeOp

        if (auto err = turnSimulatedKnob (ctx, id, cmd.getProperty ("param", {}).toString(), (double) v); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        return resultObj (true, { { "plugin", id } });
    }

    juce::var opRemovePlugin (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("plugin", {}).toString();
        auto* m = findModuleTrack (*ctx.edit, id);

        if (m == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        auto* p = modulePlugin (*m);
        auto name = p != nullptr ? p->getName() : m->getName();

        // Kette schliessen: jeder Vorgaenger an jeden Nachfolger (Praxistest
        // 1e: Hall am Ende der Drums-Kette entfernt — die Drums hingen
        // danach nicht mehr an Main und waren still, ohne Warnung)
        std::vector<std::pair<juce::String, double>> ins, outs;

        for (auto& [from, to] : routingEdges (*ctx.edit))
        {
            if (to == id)
                ins.push_back ({ from, edgeLevel (*ctx.edit, from, to) });

            if (from == id)
                outs.push_back ({ to, edgeLevel (*ctx.edit, from, to) });
        }

        removePluginModule (*ctx.edit, id);
        juce::Array<juce::var> bridged;
        juce::StringArray warnings;

        for (auto& [from, inLevel] : ins)
            for (auto& [to, outLevel] : outs)
            {
                const auto err = connectNodes (*ctx.edit, from, to, juce::jlimit (0.0, 1.0, inLevel * outLevel));

                if (err.isEmpty())
                    bridged.add (from + juce::String::fromUTF8 (" → ") + to);
                else if (err != "already connected")
                    warnings.add (from + " can not go to " + to + " (" + err + ")");
            }

        for (auto& [from, inLevel] : ins)
        {
            bool goes = false;

            for (auto& [f, t] : routingEdges (*ctx.edit))
                goes = goes || f == from;

            if (! goes)
                warnings.add (from + juce::String::fromUTF8 (" has no output now — it is silent until you connect it (connect {from, to})"));
        }

        auto params = new juce::DynamicObject();
        params->setProperty ("plugin", id);
        params->setProperty ("name", name);
        recordNodeAction (ctx, "process", "removePlugin", juce::var (params));

        auto res = resultObj (true, { { "plugin", id }, { "name", name } });

        if (! bridged.isEmpty())
            res.getDynamicObject()->setProperty ("reconnected", bridged);

        if (! warnings.isEmpty())
            res.getDynamicObject()->setProperty ("warning", warnings.joinIntoString ("; "));

        return res;
    }

    juce::var opConnect (DawdContext& ctx, const juce::var& cmd)
    {
        auto from = cmd.getProperty ("from", {}).toString();
        auto to = cmd.getProperty ("to", {}).toString();
        auto feedback = (bool) cmd.getProperty ("feedback", false);

        // Feedback-Kante: Default 50 % und kleinste Verzoegerung (ein Block)
        auto level = (double) cmd.getProperty ("level", feedback ? 0.5 : 1.0);
        auto delayMs = (double) cmd.getProperty ("delayMs", 0.0);

        auto asSend = (bool) cmd.getProperty ("send", false);

        // Sidechain-Kante: Schluessel in den zweiten Eingang des Moduls (kein Pegel)
        if ((bool) cmd.getProperty ("sidechain", false))
        {
            if (auto err = connectSidechain (*ctx.edit, from, to); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            auto params = new juce::DynamicObject();
            params->setProperty ("from", from);
            params->setProperty ("to", to);
            params->setProperty ("sidechain", true);
            recordNodeAction (ctx, "process", "connect", juce::var (params));
            return resultObj (true, { { "from", from }, { "to", to }, { "sidechain", true } });
        }

        // Hat Main eine Master-Kette, laeuft alles, was nicht selbst zu ihr
        // gehoert, in ihren Eingang — wie neue Spuren und setChain
        // (Praxistest 1l: der Hall-Return auf main umging den Limiter)
        juce::String note;

        if (! feedback && to == "main")
        {
            const auto entry = mainEntry (*ctx.edit);
            const auto fromId = moduleIdFor (*ctx.edit, from);

            if (entry != "main" && ! (fromId.isNotEmpty() && isMasterModule (*ctx.edit, fromId)))
            {
                auto* head = findModuleTrack (*ctx.edit, entry);
                auto* plugin = head != nullptr ? modulePlugin (*head) : nullptr;
                auto name = head != nullptr ? moduleLabel (*head) : juce::String();

                if (name.isEmpty() && plugin != nullptr)
                    name = plugin->getName();

                to = entry;
                note = "main has a master chain: connected into its input " + entry + (name.isNotEmpty() ? " (" + name + ")" : juce::String());
            }
        }

        auto err = feedback ? connectFeedback (*ctx.edit, from, to, level, delayMs)
                            : connectNodes (*ctx.edit, from, to, level, asSend);

        if (err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto params = new juce::DynamicObject();
        params->setProperty ("from", from);
        params->setProperty ("to", to);
        params->setProperty ("level", level);

        if (feedback)
        {
            params->setProperty ("feedback", true);
            params->setProperty ("delayMs", delayMs);
        }

        if (asSend)
            params->setProperty ("send", true);

        recordNodeAction (ctx, "process", "connect", juce::var (params));

        auto res = resultObj (true, { { "from", from }, { "to", to }, { "level", level },
                                      { "feedback", feedback } });

        if (note.isNotEmpty())
            res.getDynamicObject()->setProperty ("note", note);

        return res;
    }

    // Pegel einer Kante (linear 0..1) — Verfeinerung wie setParameter: nur
    // geloggt, keine Node-Aktion
    juce::var opSetEdge (DawdContext& ctx, const juce::var& cmd)
    {
        auto from = cmd.getProperty ("from", {}).toString();
        auto to = cmd.getProperty ("to", {}).toString();

        if ((bool) cmd.getProperty ("feedback", false))
        {
            if (auto err = setFeedback (*ctx.edit, from, to, cmd.getProperty ("level", {}),
                                        cmd.getProperty ("delayMs", {})); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            return resultObj (true, { { "from", from }, { "to", to }, { "feedback", true } });
        }

        auto level = (double) cmd.getProperty ("level", -1.0);

        if (auto err = setEdgeLevel (*ctx.edit, from, to, level); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        return resultObj (true, { { "from", from }, { "to", to },
                                  { "level", edgeLevel (*ctx.edit, from, to) } });
    }

    // Dry/Wet eines Moduls (0..1) — Verfeinerung, nur geloggt
    juce::var opSetWet (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("plugin", {}).toString();
        auto wet = (double) cmd.getProperty ("wet", -1.0);

        if (auto err = setModuleWet (*ctx.edit, id, wet); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        return resultObj (true, { { "plugin", id }, { "wet", moduleWet (*findModuleTrack (*ctx.edit, id)) } });
    }

    juce::var opDisconnect (DawdContext& ctx, const juce::var& cmd)
    {
        auto from = cmd.getProperty ("from", {}).toString();
        auto to = cmd.getProperty ("to", {}).toString();
        auto feedback = (bool) cmd.getProperty ("feedback", false);
        auto sidechain = (bool) cmd.getProperty ("sidechain", false);

        auto err = sidechain ? disconnectSidechain (*ctx.edit, from, to)
                 : feedback  ? disconnectFeedback (*ctx.edit, from, to)
                             : disconnectNodes (*ctx.edit, from, to);

        if (err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto params = new juce::DynamicObject();
        params->setProperty ("from", from);
        params->setProperty ("to", to);

        if (feedback)
            params->setProperty ("feedback", true);

        if (sidechain)
            params->setProperty ("sidechain", true);
        recordNodeAction (ctx, "process", "disconnect", juce::var (params));

        return resultObj (true, { { "from", from }, { "to", to } });
    }

    juce::var opRouting (DawdContext& ctx, const juce::var& cmd)
    {
        return routingState (*ctx.edit, (bool) cmd.getProperty ("compact", false), (bool) cmd.getProperty ("reasons", false));
    }

    // Parameter setzen: name + value (normiert 0..1) oder name + real (in der
    // Einheit des Parameters, z. B. Hz/dB), oder mehrere auf einmal mit
    // values {name: 0..1} bzw. reals {name: real}; Einstellungen (4OSC:
    // Wellenform, Filtertyp, Effekte an/aus) ueber settings {name: wert} oder
    // name + value. Erst alles pruefen, dann setzen. Verlinkte Instanzen folgen.
    juce::var opSetParameter (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id;
        auto* plugin = pluginFor (ctx, cmd, id);

        if (plugin == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        struct Change { te::AutomatableParameter* param = nullptr; const PluginSetting* setting = nullptr; float value = 0.0f; bool real = false; juce::var settingValue; };
        std::vector<Change> changes;
        auto settings = pluginSettings (*plugin);

        auto findSetting = [&] (const juce::String& n) -> const PluginSetting*
        {
            for (auto& st : settings)
                if (st.name == n)
                    return &st;

            return nullptr;
        };

        auto add = [&] (const juce::String& name, const juce::var& v, bool real) -> juce::String
        {
            if (auto* st = findSetting (name))
            {
                Change c;
                c.setting = st;

                if (auto err = checkSetting (*st, v, c.settingValue); err.isNotEmpty())
                    return err;

                changes.push_back (c);
                return {};
            }

            auto* p = findParameter (*plugin, name);

            if (p == nullptr)
                return "no such parameter: " + name + " (see listParameters)";

            // Anzeigetext ("1.2 kHz", "-6 dB", "Saw") — der Weg fuer externe
            // Plugins, deren Werte nur als 0..1 kommen
            if (v.isString())
            {
                float n = 0.0f;

                if (auto e = valueForText (*p, v.toString(), n); e.isNotEmpty())
                    return name + ": " + e;

                changes.push_back ({ p, nullptr, n, false, {} });
                return {};
            }

            if (! (v.isInt() || v.isDouble() || v.isInt64()))
                return name + ": value must be a number";

            auto x = (double) v;

            // real = Zahl in der angezeigten Einheit, ueber den Anzeigetext
            // auch bei Plugins mit internem 0..1 (Praxistest 1d)
            if (real)
            {
                float n = 0.0f;

                if (auto e = valueForReal (*p, x, n); e.isNotEmpty())
                    return name + ": " + e;

                changes.push_back ({ p, nullptr, n, false, {} });
                return {};
            }

            if (x < 0.0 || x > 1.0)
                return name + juce::String::fromUTF8 (": value must be 0..1 (normalised) — use real for the displayed unit, or text");

            changes.push_back ({ p, nullptr, (float) x, false, {} });
            return {};
        };

        juce::String err;

        if (cmd.hasProperty ("name"))
        {
            auto name = cmd.getProperty ("name", {}).toString();

            if (cmd.hasProperty ("text"))
                err = add (name, cmd.getProperty ("text", {}).toString(), false);
            else if (cmd.hasProperty ("real"))
                err = add (name, cmd.getProperty ("real", {}), true);
            else if (cmd.hasProperty ("value"))
                err = add (name, cmd.getProperty ("value", {}), false);
            else
                err = "need value (0..1) or real";
        }

        for (auto [key, real] : { std::pair<const char*, bool> { "values", false }, { "reals", true }, { "settings", false }, { "texts", false } })
            if (auto* obj = cmd.getProperty (key, {}).getDynamicObject(); obj != nullptr && err.isEmpty())
                for (auto& prop : obj->getProperties())
                    if ((err = add (prop.name.toString(), juce::String (key) == "texts" ? juce::var (prop.value.toString()) : prop.value, real)).isNotEmpty())
                        break;

        if (err.isEmpty() && changes.empty())
            err = juce::String::fromUTF8 ("nothing to set — name + value | name + real | name + text | values {} | reals {} | texts {} | settings {}");

        if (err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        juce::Array<juce::var> done, linked;
        auto linkedPlugins = linkedModules (*ctx.edit, id);

        for (auto& c : changes)
        {
            if (c.setting != nullptr)
            {
                c.setting->set (c.settingValue);

                for (auto* m : linkedPlugins)
                    if (auto* lp = modulePlugin (*m))
                        for (auto& st : pluginSettings (*lp))
                            if (st.name == c.setting->name)
                                st.set (c.settingValue);

                done.add (settingVar (*c.setting));
                continue;
            }

            auto label = parameterLabel (*plugin, *c.param);

            if (c.real)
                c.param->setParameter (c.value, juce::sendNotification);
            else
                c.param->setNormalisedParameter (c.value, juce::sendNotification);

            for (auto* m : linkedPlugins)
                if (auto* lp = modulePlugin (*m))
                    if (auto* q = findParameter (*lp, label))
                    {
                        if (c.real)
                            q->setParameter (c.value, juce::sendNotification);
                        else
                            q->setNormalisedParameter (c.value, juce::sendNotification);
                    }

            done.add (parameterVar (*plugin, *c.param, plugin->getAutomatableParameters().indexOf (c.param)));
        }

        for (auto* m : linkedPlugins)
            linked.add ("p" + m->state.getProperty ("dawdModule").toString());

        auto res = resultObj (true, { { "plugin", id }, { "set", done }, { "linked", linked } });

        // Einzelner Parameter: name/value wie bisher oben in der Antwort
        if (done.size() == 1)
        {
            res.getDynamicObject()->setProperty ("name", done[0].getProperty ("name", {}));
            res.getDynamicObject()->setProperty ("value", done[0].getProperty ("value", {}));
        }

        return res;
    }

    juce::var opListParameters (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id;
        auto* plugin = pluginFor (ctx, cmd, id);

        if (plugin == nullptr)
            return resultObj (false, { { "error", "no such plugin: " + id } });

        juce::Array<juce::var> params, settings;
        int i = 0;
        const int samples = (int) cmd.getProperty ("samples", 0);   // Verlauf je Parameter (Steckbrief)

        // Gemessene Wirkung je Parameter, wenn es einen Steckbrief gibt
        juce::String fmt = "builtin", uid = plugin->getPluginType();

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (plugin))
            fmt = ep->desc.pluginFormatName, uid = juce::String::toHexString (ep->desc.uniqueId);

        auto measured = loadProfile (profileSlug (fmt, uid)).getProperty ("measured", {}).getProperty ("parameters", {});

        // Filter und Limit wie bei pluginInfo — Surge XT hat ueber 700
        // Parameter, ungefiltert sprengt das jeden Kontext
        const ParamQuery query (*plugin, cmd);
        const int limit = juce::jlimit (1, 1000, (int) cmd.getProperty ("limit", 80));
        int total = 0, matched = 0;

        for (auto* p : plugin->getAutomatableParameters())
        {
            const int index = i++;
            ++total;
            auto label = parameterLabel (*plugin, *p);

            if (! query.matches (*p) || ++matched > limit)
                continue;

            auto v = parameterVar (*plugin, *p, index);

            if (auto m = measuredFor (measured, label); m.isObject())
            {
                v.getDynamicObject()->setProperty ("does", m.getProperty ("summary", {}));

                if (m.hasProperty ("needs"))
                    v.getDynamicObject()->setProperty ("needs", m.getProperty ("needs", {}));
            }

            if (samples > 0)
                if (auto shape = parameterShape (*p, samples); auto* so = shape.getDynamicObject())
                    for (auto& prop : so->getProperties())
                        v.getDynamicObject()->setProperty (prop.name, prop.value);

            params.add (v);
        }

        for (auto& st : pluginSettings (*plugin))
            settings.add (settingVar (st));

        auto res = resultObj (true, { { "plugin", id }, { "pluginName", plugin->getName() },
                                      { "parameterCount", total }, { "parameters", params } });

        if (matched > limit)
            res.getDynamicObject()->setProperty ("parametersTruncated", juce::String (matched - limit) + juce::String::fromUTF8 (" more — narrow with paramQuery or raise limit"));

        if (auto n = query.noteFor (matched); n.isNotEmpty())
            res.getDynamicObject()->setProperty ("note", n);

        if (! settings.isEmpty())
            res.getDynamicObject()->setProperty ("settings", settings);

        return res;
    }

    // Inhalt eines Clips fuer die Anzeige: Audio als Peaks (max |Pegel| je
    // Bucket ueber den hoerbaren Bereich), MIDI als Noten und Controller
    // (Sekunden und Beats relativ zum Clip-Start). Read-only — das UI zeichnet daraus Waveform/Noten.
    juce::var opClipPeaks (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto buckets = juce::jlimit (8, 2000, (int) (double) cmd.getProperty ("buckets", 200.0));
        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* clip = findClipByName (*tracks[trackIndex], name);

        if (clip == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        auto pos = clip->getPosition();

        if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
        {
            juce::Array<juce::var> notes;
            auto clipStart = pos.getStart().inSeconds();

            for (auto* n : mc->getSequence().getNotes())
            {
                auto r = n->getEditTimeRange (*mc);
                auto s0 = r.getStart().inSeconds() - clipStart;

                // Nur, was klingt: Noten vor dem Clip-Start oder hinter dem
                // Ende (nach trimClip/splitClip verdeckt) nicht melden
                if (s0 < -0.0005 || s0 >= pos.getLength().inSeconds() - 0.0005)
                    continue;
                auto b0 = secondsToBeats (*ctx.edit, clipStart, s0);
                notes.add (resultObj (true, { { "pitch", n->getNoteNumber() },
                                              { "start", s0 },
                                              { "length", r.getLength().inSeconds() },
                                              { "beat", std::round (b0 * 1000.0) / 1000.0 },
                                              { "beats", std::round (secondsToBeats (*ctx.edit, clipStart + s0, r.getLength().inSeconds()) * 1000.0) / 1000.0 },
                                              { "velocity", n->getVelocity() } }));
            }

            return resultObj (true, { { "type", "midi" }, { "notes", notes }, { "controllers", clipControllers (*ctx.edit, *mc) } });
        }

        auto* ac = dynamic_cast<te::AudioClipBase*> (clip);

        if (ac == nullptr)
            return resultObj (false, { { "error", "no audio in clip" } });

        // Gewarpt: die Originaldatei (clipSourceTime rechnet in ihren Sekunden)
        std::unique_ptr<juce::AudioFormatReader> reader (
            te::AudioFileUtils::createReaderFor (*ctx.engine, unwarpedFile (*ac)));

        if (reader == nullptr && ! unwarpedFile (*ac).existsAsFile())
            return resultObj (false, { { "error", "audio file missing: " + unwarpedFile (*ac).getFullPathName() } });

        if (reader == nullptr)
            return resultObj (false, { { "error", "cannot read clip file" } });

        auto sr = reader->sampleRate;
        auto len = pos.getLength().inSeconds();
        juce::Array<juce::var> peaks;

        for (int b = 0; b < buckets; ++b)
        {
            // Bucket in Clip-Zeit -> Stelle in der Datei (Tempo, Loop,
            // Warp-Marker: clipSourceTime); ueber das Loop-Ende hinweg die
            // Bucket-Dauer ab dem Anfang
            auto t0 = len * b / buckets, t1 = len * (b + 1) / buckets;
            auto s0 = clipSourceTime (*ac, t0), s1 = clipSourceTime (*ac, t1);

            if (s1 <= s0)
                s1 = s0 + (t1 - t0);

            auto start = (juce::int64) (s0 * sr);
            auto per = juce::jmax ((juce::int64) 1, (juce::int64) ((s1 - s0) * sr));
            float lo = 0, hi = 0, mx = 0;

            if (start >= 0 && start < reader->lengthInSamples)
            {
                juce::Range<float> lev[2];
                auto ch = (int) juce::jmin (2u, reader->numChannels);
                reader->readMaxLevels (start, (int) juce::jmin (per, reader->lengthInSamples - start), lev, ch);

                for (int c = 0; c < ch; ++c)
                {
                    lo = juce::jmin (lo, lev[c].getStart());
                    hi = juce::jmax (hi, lev[c].getEnd());
                }

                mx = juce::jmax (-lo, hi);
            }

            peaks.add (std::round (mx * 1000.0f) / 1000.0f);
        }

        // Dateisekunden an Clip-Anfang und -Ende (Originaldatei, auch
        // gewarpt): das Flow-UI setzt damit Warp-Marker an die Stelle, die
        // dort gerade spielt
        auto r3 = [] (double v) { return std::round (v * 1000.0) / 1000.0; };
        return resultObj (true, { { "type", "audio" }, { "peaks", peaks },
                                  { "sourceStart", r3 (clipSourceTime (*ac, 0.0)) }, { "sourceEnd", r3 (clipSourceTime (*ac, len)) } });
    }

    namespace
    {
        // Belegung eines Samplers als Liste (listSamples, Antwort von setSamples)
        juce::var samplesOf (te::SamplerPlugin& sp)
        {
            juce::Array<juce::var> out;

            for (int i = 0; i < sp.getNumSounds(); ++i)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("index", i);
                o->setProperty ("name", sp.getSoundName (i));
                o->setProperty ("file", sp.getSoundMedia (i));

                if (! te::SourceFileReference::findFileFromString (sp.edit, sp.getSoundMedia (i)).existsAsFile())
                    o->setProperty ("missing", true);   // Datei fehlt: die Taste bleibt stumm
                o->setProperty ("root", sp.getKeyNote (i));
                o->setProperty ("low", sp.getMinKey (i));
                o->setProperty ("high", sp.getMaxKey (i));
                o->setProperty ("gainDb", (double) sp.getSoundGainDb (i));
                o->setProperty ("pan", (double) sp.getSoundPan (i));
                o->setProperty ("oneShot", sp.isSoundOpenEnded (i));
                o->setProperty ("start", sp.getSoundStartTime (i));
                o->setProperty ("length", sp.getSoundLength (i));

                // Velocity-Layer nur, wenn gesetzt (alte Belegungen bleiben kurz)
                if (sp.getSoundMinVelocity (i) != 1 || sp.getSoundMaxVelocity (i) != 127)
                {
                    o->setProperty ("velLow", sp.getSoundMinVelocity (i));
                    o->setProperty ("velHigh", sp.getSoundMaxVelocity (i));
                }

                if (sp.getSoundRoundRobinGroup (i) > 0)
                    o->setProperty ("roundRobin", sp.getSoundRoundRobinGroup (i));

                if (sp.getSoundChokeGroup (i) > 0)
                    o->setProperty ("choke", sp.getSoundChokeGroup (i));

                if (sp.getSoundAttack (i) > 0.0)
                    o->setProperty ("attack", sp.getSoundAttack (i));

                if (sp.getSoundRelease (i) > 0.0)
                    o->setProperty ("release", sp.getSoundRelease (i));

                out.add (juce::var (o));
            }

            return out;
        }

        // Release wirkt erst ab dem Note-off — One-Shots spielen bis zum Ende
        void warnReleaseOnOneShot (te::SamplerPlugin& sp, int idx, juce::Array<juce::var>& warnings)
        {
            if (sp.getSoundRelease (idx) > 0.0 && sp.isSoundOpenEnded (idx))
                warnings.add (sp.getSoundName (idx) + ": release has no effect on a one-shot (it plays to its end) - set oneShot false");
        }

        te::SamplerPlugin* samplerFor (DawdContext& ctx, const juce::var& cmd, juce::String& id, juce::String& err)
        {
            auto* plugin = pluginFor (ctx, cmd, id);

            if (plugin == nullptr)
            {
                err = "no such plugin: " + id;
                return nullptr;
            }

            auto* sp = dynamic_cast<te::SamplerPlugin*> (plugin);

            if (sp == nullptr)
                err = "not a sampler: " + plugin->getName() + " (use a Sampler or Drum Kit module)";

            return sp;
        }
    }

    // Samples auf Tasten eines Sampler-Moduls legen: note = eine Taste
    // (Drums, One-Shot), sonst low/high/root (melodisch, transponiert ab
    // root). start/length schneiden einen Ausschnitt (z. B. ein Slice aus
    // einem Song). Ersetzt die Belegung, append haengt an. Erst alles
    // pruefen, dann setzen — ein Fehler laesst den Sampler unveraendert.
    juce::var opSetSamples (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id, err;
        auto* sp = samplerFor (ctx, cmd, id, err);

        if (sp == nullptr)
            return resultObj (false, { { "error", err } });

        // Einzelne Samples aendern, ohne die Liste neu zu senden (Praxistest
        // 1b: fuer den Gain eines Samples musste alles neu)
        if (cmd.hasProperty ("update"))
        {
            if (cmd.hasProperty ("samples"))
                return resultObj (false, { { "error", "use samples (replace/append) or update, not both" } });

            auto* ups = cmd.getProperty ("update", {}).getArray();

            if (ups == nullptr || ups->isEmpty())
                return resultObj (false, { { "error", "update must be a list of {index | name, gainDb?, pan?, note? | root?/low?/high?, velLow?/velHigh?, roundRobin?, choke?, attack?, release?, oneShot?, start?, length?, rename?}" } });

            static const juce::StringArray upKnown { "index", "name", "gainDb", "pan", "note", "root", "low", "high", "velLow", "velHigh", "roundRobin",
                                                     "choke", "attack", "release", "oneShot", "start", "length", "rename" };
            sp->flushPendingUpdates();

            struct Up { int idx; juce::var v; };
            std::vector<Up> todo;

            // Erst alles pruefen, dann aendern — kein halbes Update
            for (int i = 0; i < ups->size(); ++i)
            {
                auto& u = ups->getReference (i);
                auto where = "update " + juce::String (i) + ": ";

                if (auto* so = u.getDynamicObject())
                    for (auto& prop : so->getProperties())
                        if (! upKnown.contains (prop.name.toString()))
                            return resultObj (false, { { "error", where + "unknown field " + prop.name.toString()
                                                                    + " (use: " + upKnown.joinIntoString (", ") + ")" } });
                int idx = -1;

                if (u.hasProperty ("index"))
                    idx = (int) u.getProperty ("index", -1);
                else
                    for (int k = 0; k < sp->getNumSounds(); ++k)
                        if (sp->getSoundName (k) == u.getProperty ("name", {}).toString())
                            { idx = k; break; }

                if (idx < 0 || idx >= sp->getNumSounds())
                    return resultObj (false, { { "error", where + "no such sample (give index from listSamples or its name)" } });

                auto num = [&] (const char* key, double lo, double hi) -> juce::String
                {
                    if (u.hasProperty (key) && ((double) u.getProperty (key, 0.0) < lo || (double) u.getProperty (key, 0.0) > hi))
                        return where + juce::String (key) + " must be " + juce::String (lo) + ".." + juce::String (hi);
                    return {};
                };

                for (auto e : { num ("gainDb", -48, 48), num ("pan", -1, 1), num ("note", 0, 127), num ("root", 0, 127),
                                num ("low", 0, 127), num ("high", 0, 127), num ("velLow", 1, 127), num ("velHigh", 1, 127),
                                num ("roundRobin", 0, 127), num ("choke", 0, 127), num ("attack", 0, 10),
                                num ("release", 0, 10), num ("start", 0, 1.0e6), num ("length", 0, 1.0e6) })
                    if (e.isNotEmpty())
                        return resultObj (false, { { "error", e } });

                if ((int) u.getProperty ("velLow", sp->getSoundMinVelocity (idx)) > (int) u.getProperty ("velHigh", sp->getSoundMaxVelocity (idx)))
                    return resultObj (false, { { "error", where + "velLow must not be above velHigh" } });

                todo.push_back ({ idx, u });
            }

            for (auto& [idx, u] : todo)
            {
                int root = sp->getKeyNote (idx), low = sp->getMinKey (idx), high = sp->getMaxKey (idx);

                if (u.hasProperty ("note"))
                {
                    root = low = high = (int) u.getProperty ("note", root);
                    sp->setSoundOpenEnded (idx, true);
                }

                root = (int) u.getProperty ("root", root);
                low = (int) u.getProperty ("low", low);
                high = (int) u.getProperty ("high", high);

                if (low > high)
                    return resultObj (false, { { "error", "low must not be above high" } });

                sp->setSoundParams (idx, root, low, high);
                sp->setSoundGains (idx, (float) (double) u.getProperty ("gainDb", (double) sp->getSoundGainDb (idx)),
                                   (float) (double) u.getProperty ("pan", (double) sp->getSoundPan (idx)));

                if (u.hasProperty ("oneShot"))
                    sp->setSoundOpenEnded (idx, (bool) u.getProperty ("oneShot", false));

                if (u.hasProperty ("velLow") || u.hasProperty ("velHigh"))
                    sp->setSoundVelocityRange (idx, (int) u.getProperty ("velLow", sp->getSoundMinVelocity (idx)),
                                               (int) u.getProperty ("velHigh", sp->getSoundMaxVelocity (idx)));

                if (u.hasProperty ("roundRobin") || u.hasProperty ("choke"))
                    sp->setSoundGroups (idx, (int) u.getProperty ("roundRobin", sp->getSoundRoundRobinGroup (idx)),
                                        (int) u.getProperty ("choke", sp->getSoundChokeGroup (idx)));

                if (u.hasProperty ("attack") || u.hasProperty ("release"))
                    sp->setSoundEnvelope (idx, (double) u.getProperty ("attack", sp->getSoundAttack (idx)),
                                          (double) u.getProperty ("release", sp->getSoundRelease (idx)));

                if (u.hasProperty ("start") || u.hasProperty ("length"))
                    sp->setSoundExcerpt (idx, (double) u.getProperty ("start", sp->getSoundStartTime (idx)),
                                         (double) u.getProperty ("length", sp->getSoundLength (idx)));

                if (u.hasProperty ("rename"))
                    sp->setSoundName (idx, u.getProperty ("rename", {}).toString());
            }

            sp->flushPendingUpdates();
            juce::Array<juce::var> warnings;

            for (auto& t : todo)
                warnReleaseOnOneShot (*sp, t.idx, warnings);

            auto params = new juce::DynamicObject();
            params->setProperty ("plugin", id);
            params->setProperty ("param", id);
            params->setProperty ("samples", sp->getNumSounds());
            params->setProperty ("updated", (int) todo.size());
            recordNodeAction (ctx, "process", "setSamples", juce::var (params), {}, true);
            auto r = resultObj (true, { { "plugin", id }, { "samples", samplesOf (*sp) } });

            if (! warnings.isEmpty())
                r.getDynamicObject()->setProperty ("warnings", warnings);

            return r;
        }

        auto* list = cmd.getProperty ("samples", {}).getArray();

        if (list == nullptr)
            return resultObj (false, { { "error", "samples must be a list of {file, note | low/high/root, ...}" } });

        const bool append = (bool) cmd.getProperty ("append", false);

        if ((append ? sp->getNumSounds() : 0) + list->size() > 64)
            return resultObj (false, { { "error", "too many samples (a sampler holds at most 64)" } });

        struct Spec
        {
            juce::File file;
            juce::String name;
            int root = 60, low = 0, high = 127, velLow = 1, velHigh = 127, roundRobin = 0, choke = 0;
            float gainDb = 0.0f, pan = 0.0f;
            bool oneShot = false;
            double start = 0.0, length = 0.0, attack = 0.0, release = 0.0;
        };

        std::vector<Spec> specs;

        for (int i = 0; i < list->size(); ++i)
        {
            auto& s = list->getReference (i);
            auto where = "sample " + juce::String (i) + ": ";

            // Unbekannte Felder nicht still schlucken (keyNote/minNote/maxNote
            // aus der Engine heissen hier root/low/high)
            static const juce::StringArray known { "file", "note", "root", "low", "high", "velLow", "velHigh", "roundRobin", "choke", "name", "gainDb", "pan",
                                                   "oneShot", "start", "length", "attack", "release" };

            if (auto* so = s.getDynamicObject())
                for (auto& prop : so->getProperties())
                    if (! known.contains (prop.name.toString()))
                        return resultObj (false, { { "error", where + "unknown field " + prop.name.toString()
                                                                + " (use: " + known.joinIntoString (", ") + ")" } });
            auto path = s.getProperty ("file", {}).toString();

            if (path.isEmpty() || ! juce::File (path).existsAsFile())
                return resultObj (false, { { "error", where + "no such file: " + path } });

            Spec spec;
            juce::String decodeErr;
            spec.file = playableAudioFile (ctx, juce::File (path), decodeErr);

            if (decodeErr.isNotEmpty())
                return resultObj (false, { { "error", where + decodeErr } });

            if (! te::AudioFile (*ctx.engine, spec.file).isValid())
                return resultObj (false, { { "error", where + "cannot read audio file: " + juce::File (path).getFileName() } });

            // Samples von ausserhalb (Bibliothek, eingehaengte Platte) ins
            // Projekt kopieren — es bleibt vollstaendig, auch wenn die Quelle
            // verschwindet; die mitgelieferten Kits bleiben, wo sie sind
            if (ctx.tree && ! spec.file.isAChildOf (ctx.tree->projectDir) && ! spec.file.isAChildOf (kitsDirectory()))
            {
                auto dir = ctx.tree->projectDir.getChildFile ("media").getChildFile ("samples");
                auto dest = dir.getChildFile (spec.file.getFileName());

                for (int n = 2; dest.existsAsFile() && dest.getSize() != spec.file.getSize(); ++n)
                    dest = dir.getChildFile (spec.file.getFileNameWithoutExtension() + " (" + juce::String (n) + ")"
                                             + spec.file.getFileExtension());

                if (! dest.existsAsFile() && ! (dir.createDirectory().wasOk() && spec.file.copyFileTo (dest)))
                    return resultObj (false, { { "error", where + "cannot copy into the project: " + dest.getFullPathName() } });

                recordOrigins (dest, { spec.file.getFullPathName() });   // Lizenz-Sperre: Quelle der Kopie
                spec.file = dest;
            }

            auto inRange = [] (const juce::var& v) { return (double) v >= 0.0 && (double) v <= 127.0; };

            if (s.hasProperty ("note"))
            {
                if (! inRange (s.getProperty ("note", -1)))
                    return resultObj (false, { { "error", where + "note must be 0..127" } });

                spec.root = spec.low = spec.high = (int) s.getProperty ("note", 60);
                spec.oneShot = true;

                // root dazu: die Taste bleibt, das Sample klingt verstimmt
                // (wie update; Praxistest 1e: die volle Liste verwarf root,
                // die hochgestimmte Kick fiel beim Austausch der Hat zurueck)
                if (s.hasProperty ("root"))
                {
                    if (! inRange (s.getProperty ("root", -1)))
                        return resultObj (false, { { "error", where + "root must be 0..127" } });

                    spec.root = (int) s.getProperty ("root", spec.root);
                }
            }
            else
            {
                spec.root = (int) s.getProperty ("root", 60);
                spec.low = (int) s.getProperty ("low", juce::jmax (0, spec.root - 24));
                spec.high = (int) s.getProperty ("high", juce::jmin (127, spec.root + 24));

                for (auto key : { "root", "low", "high" })
                    if (s.hasProperty (key) && ! inRange (s.getProperty (key, -1)))
                        return resultObj (false, { { "error", where + juce::String (key) + " must be 0..127" } });

                if (spec.low > spec.high)
                    return resultObj (false, { { "error", where + "low must not be above high" } });
            }

            // Velocity-Layer: das Sample klingt nur bei Anschlaegen velLow..velHigh
            for (auto key : { "velLow", "velHigh" })
                if (s.hasProperty (key) && ((double) s.getProperty (key, 0) < 1.0 || (double) s.getProperty (key, 0) > 127.0))
                    return resultObj (false, { { "error", where + juce::String (key) + " must be 1..127" } });

            spec.velLow = (int) s.getProperty ("velLow", 1);
            spec.velHigh = (int) s.getProperty ("velHigh", 127);

            if (spec.velLow > spec.velHigh)
                return resultObj (false, { { "error", where + "velLow must not be above velHigh" } });

            // Round Robin: Samples derselben Gruppe auf einer Taste wechseln
            // sich ab; Choke: ein Sample stoppt die klingenden seiner Gruppe
            for (auto key : { "roundRobin", "choke" })
                if (s.hasProperty (key) && ((double) s.getProperty (key, 0) < 0.0 || (double) s.getProperty (key, 0) > 127.0))
                    return resultObj (false, { { "error", where + juce::String (key) + " must be 0..127 (0 = none)" } });

            spec.roundRobin = (int) s.getProperty ("roundRobin", 0);
            spec.choke = (int) s.getProperty ("choke", 0);

            // Huellkurve: Attack ab Notenbeginn, Release ab Note-off (Sekunden)
            for (auto key : { "attack", "release" })
                if (s.hasProperty (key) && ((double) s.getProperty (key, 0) < 0.0 || (double) s.getProperty (key, 0) > 10.0))
                    return resultObj (false, { { "error", where + juce::String (key) + " must be 0..10 seconds" } });

            spec.attack = (double) s.getProperty ("attack", 0.0);
            spec.release = (double) s.getProperty ("release", 0.0);

            spec.oneShot = (bool) s.getProperty ("oneShot", spec.oneShot);
            spec.gainDb = (float) (double) s.getProperty ("gainDb", 0.0);
            spec.pan = (float) (double) s.getProperty ("pan", 0.0);
            spec.start = (double) s.getProperty ("start", 0.0);
            spec.length = (double) s.getProperty ("length", 0.0);
            spec.name = s.getProperty ("name", juce::File (path).getFileNameWithoutExtension()).toString();

            if (spec.gainDb < -48.0f || spec.gainDb > 48.0f || spec.pan < -1.0f || spec.pan > 1.0f)
                return resultObj (false, { { "error", where + "gainDb must be -48..48, pan -1..1" } });

            if (spec.start < 0.0 || spec.length < 0.0)
                return resultObj (false, { { "error", where + "start and length must not be negative" } });

            specs.push_back (spec);
        }

        if (! append)
            while (sp->getNumSounds() > 0)
                sp->removeSound (sp->getNumSounds() - 1);

        for (auto& spec : specs)
        {
            if (auto e = sp->addSound (spec.file.getFullPathName(), spec.name, spec.start, spec.length, spec.gainDb); e.isNotEmpty())
                return resultObj (false, { { "error", e } });

            auto idx = sp->getNumSounds() - 1;
            sp->setSoundParams (idx, spec.root, spec.low, spec.high);
            sp->setSoundGains (idx, spec.gainDb, spec.pan);
            sp->setSoundOpenEnded (idx, spec.oneShot);
            sp->setSoundVelocityRange (idx, spec.velLow, spec.velHigh);
            sp->setSoundGroups (idx, spec.roundRobin, spec.choke);
            sp->setSoundEnvelope (idx, spec.attack, spec.release);
        }

        sp->flushPendingUpdates();

        // Weit verstimmte Samples klingen dumpf bzw. quaekig (Praxistest 1b:
        // ein Rhodes-Ton ueber anderthalb Oktaven nach unten gespielt)
        juce::Array<juce::var> warnings;

        for (auto& spec : specs)
            if (! (spec.low == spec.high) && juce::jmax (spec.root - spec.low, spec.high - spec.root) > 12)
                warnings.add (spec.name + ": plays up to " + juce::String (juce::jmax (spec.root - spec.low, spec.high - spec.root))
                              + " semitones away from its root " + juce::String (spec.root)
                              + " - it will sound dull or thin there; narrow low/high or add samples for other ranges (multisample)");

        for (int i = sp->getNumSounds() - (int) specs.size(); i < sp->getNumSounds(); ++i)
            warnReleaseOnOneShot (*sp, i, warnings);

        // Process-Aktion; wiederholtes setSamples am selben Modul bleibt eine
        auto params = new juce::DynamicObject();
        params->setProperty ("plugin", id);
        params->setProperty ("param", id);
        params->setProperty ("samples", sp->getNumSounds());
        params->setProperty ("append", append);
        recordNodeAction (ctx, "process", "setSamples", juce::var (params), {}, true);

        auto r = resultObj (true, { { "plugin", id }, { "samples", samplesOf (*sp) } });

        if (! warnings.isEmpty())
            r.getDynamicObject()->setProperty ("warnings", warnings);

        return r;
    }

    juce::var opListSamples (DawdContext& ctx, const juce::var& cmd)
    {
        juce::String id, err;
        auto* sp = samplerFor (ctx, cmd, id, err);

        if (sp == nullptr)
            return resultObj (false, { { "error", err } });

        sp->flushPendingUpdates();
        return resultObj (true, { { "plugin", id }, { "samples", samplesOf (*sp) } });
    }
}

