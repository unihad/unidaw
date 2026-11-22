#include "Dawd.h"
#include "EngineHelpers.h"

// Aufnahme-Ende: Take einsammeln (Datei oder MIDI-Render), Node als acted
// markieren, Take-Daten fuer den + Button am Node ablegen. Die Aufnahme
// laeuft bis zum Stopp (seit 2026-10-24 kein Auto-Stop am Section-Ende);
// reicht ein Take hinter die letzte Section, waechst eine Auto-Section mit.
namespace dawd
{
    juce::var stopRecordingCollect (DawdContext& ctx)
    {
        auto takes = std::move (ctx.armedTakes);
        ctx.armedTakes.clear();
        auto takeSection = ctx.pendingTakeSection;
        ctx.pendingTakeSection = {};

        ctx.edit->getTransport().stop (false, true);
        disarmRecording (ctx);

        // Die Aufnahme hoert live mit (monitorOn waehrend des Takes) — danach
        // gilt wieder, was der Monitor-Schalter gesetzt hat: an, wenn ein
        // Geraet auf MonitorMode::on steht, sonst aus
        {
            auto& dm = ctx.engine->getDeviceManager();
            bool on = false;

            for (auto* wi : dm.getWaveInputDevices())
                on = on || wi->getMonitorMode() == te::InputDevice::MonitorMode::on;

            for (auto mi : dm.getMidiInDevices())
                on = on || mi->getMonitorMode() == te::InputDevice::MonitorMode::on;

            ctx.monitorOn = on;
        }

        auto tracks = userTracks (*ctx.edit);

        // Zuweisung zurueck auf den Zieltrack — der Take lebt in der Lane,
        // die Zuweisung (assignInput) bleibt auf der Spur. Erste Zuweisung
        // ergaenzen (move=false), dann die Lane-Ziele abziehen: ein Geraet,
        // das mehrere Lanes gespeist hat, faedelt wieder alle Ziel-Spuren.
        for (auto& take : takes)
            if (take.targetIndex >= 0 && take.targetIndex < tracks.size()
                && take.laneIndex >= 0 && take.laneIndex < tracks.size())
                for (auto in : ctx.edit->getAllInputDevices())
                    if (in->getTargets().contains (tracks[take.laneIndex]->itemID))
                    {
                        [[maybe_unused]] auto res = in->setTarget (
                            tracks[take.targetIndex]->itemID, false, nullptr, 0);
                        [[maybe_unused]] auto rm = in->removeTarget (
                            tracks[take.laneIndex]->itemID, nullptr);
                        break;
                    }

        auto newSection = autoSection (*ctx.edit);
        saveCurrentEdit (ctx);

        juce::Array<juce::var> files;
        juce::Array<juce::var> takeList;
        int notes = 0;

        for (auto& take : takes)
        {
            if (take.laneIndex < 0 || take.laneIndex >= tracks.size())
                continue;

            juce::Array<juce::var> takeFiles;
            std::vector<te::EditItemID> takeClipIDs;
            juce::Array<te::MidiClip*> midiTakes;
            int takeNotes = 0;

            for (auto* clip : tracks[take.laneIndex]->getClips())
                if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
                {
                    takeClipIDs.push_back (mc->itemID);
                    midiTakes.add (mc);
                    takeNotes += mc->getSequence().getNumNotes();
                }
                else if (auto* wc = dynamic_cast<te::WaveAudioClip*> (clip))
                {
                    takeFiles.add (wc->getCurrentSourceFile().getFullPathName());
                    takeClipIDs.push_back (wc->itemID);

                    // Take-Datei synchron validieren — der Cache scannt asynchron,
                    // sonst bleibt der Take bis zum naechsten Scan stumm.
                    ctx.engine->getAudioFileManager().validateFile (
                        te::AudioFile (*ctx.engine, wc->getCurrentSourceFile()), true);
                }

            // MIDI-Takes haben keine Datei — den Take als WAV rendern
            // (Clip-Range, durch das Routing der Spur), damit Listen-to-take klingt.
            // Direktes Task-Muster wie im render op: renderToFile mit
            // useThread geht ueber die Progressbar-UI (headless tot) und
            // ueberschreibt die Sample-Rate mit dem DeviceManager.
            for (auto* mc : midiTakes)
            {
                // Take-WAV beim Edit ablegen, nicht im CWD — Audio-Takes landen
                // ohnehin neben der Edit-Datei (Projektordner).
                auto editDir = te::EditFileOperations (*ctx.edit).getEditFile().getParentDirectory();
                auto dir = editDir.isDirectory() ? editDir : juce::File::getCurrentWorkingDirectory();
                auto f = dir.getChildFile (juce::File::createLegalFileName (mc->getName()) + ".wav")
                             .getNonexistentSibling();

                te::Renderer::Parameters r (*ctx.edit);
                r.destFile = f;
                r.audioFormat = ctx.engine->getAudioFileFormatManager().getDefaultFormat();
                r.bitDepth = 24;
                r.sampleRateForAudio = 44100.0;
                r.blockSizeForAudio = 512;
                r.time = mc->getPosition().time;
                r.usePlugins = true;
                r.useMasterPlugins = true;
                r.allowedClips.add (mc);

                // Alle Spuren im Render-Graph: die Lane laeuft in ihre Gruppe,
                // die ueber das Routing (Module, Void-Spur) bis Main — allowedClips
                // laesst nur den Take klingen. Ohne Instrument im Routing bleibt
                // der Take stumm ("Didn't find any audio to render").
                r.tracksToDo.setRange (0, (int) te::getAllTracks (*ctx.edit).size(), true);

                juce::String error;

                // ScopedRenderStatus wie in renderToFile: gibt den Playback-
                // Kontext frei (sonst "Rendering whilst attached to audio
                // device" — der Render deinitialisiert die Plugins unterm
                // laufenden Graph, das Device bleibt stumm) und holt ihn
                // danach zurueck, damit das Monitoring weiterlaeuft.
                const te::Edit::ScopedRenderStatus srs (*ctx.edit, true);
                const ScopedListeningOff noListening (ctx, *ctx.edit);   // sonst ist der Take stumm, wenn eine andere Spur Solo hat

                if (auto task = te::render_utils::createRenderTask (r, "MIDI take", nullptr, nullptr))
                {
                    while (task->runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
                    {}

                    error = task->errorMessage;
                }

                if (error.isEmpty() && f.existsAsFile())
                    takeFiles.add (f.getFullPathName());
                else
                    std::cout << "midi take render failed: " << error << std::endl;
            }

            ctx.recordedTakes.push_back (std::move (takeClipIDs));

            // Aktion am Node je Lane (kind collect): Take-Typ, Section,
            // Zieltrack (fuer den + Button) und die Take-Datei zum Anhoeren
            // — acted und die Take-Felder im treeState leiten sich daraus ab.
            if (! takeFiles.isEmpty() || ! midiTakes.isEmpty())
            {
                auto params = new juce::DynamicObject();
                params->setProperty ("type", take.midi ? "midi" : "audio");
                params->setProperty ("section", takeSection);
                params->setProperty ("track", take.targetIndex);
                recordNodeAction (ctx, "collect", "record", juce::var (params),
                                  takeFiles.isEmpty() ? juce::String() : takeFiles[0].toString());
            }

            takeList.add (resultObj (true, {
                { "track", take.laneIndex }, { "laneOf", take.targetIndex },
                { "type", take.midi ? "midi" : "audio" },
                { "files", takeFiles }, { "notes", takeNotes } }));

            for (auto& f : takeFiles)
                files.add (f);

            notes += takeNotes;
        }

        recordAutoSection (ctx, newSection);

        // Einzeltake kompatibel: erster Take oben, alle Lanes im takes-Array
        auto res = resultObj (true, { { "track", takes.empty() ? -1 : takes[0].laneIndex },
                                  { "laneOf", takes.empty() ? -1 : takes[0].targetIndex },
                                  { "type", takes.empty() ? juce::String()
                                                          : (takes[0].midi ? "midi" : "audio") },
                                  { "files", files },
                                  { "notes", notes },
                                  { "takes", takeList } });

        if (! newSection.isVoid())
            res.getDynamicObject()->setProperty ("autoSection", newSection);

        return res;
    }
}
