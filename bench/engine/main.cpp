// Benchmark: Tracktion Engine mit dawds Routing-Matrix
//
// tebench <dir> [render|blocks|helm|live]
//  render (Default) — 16 Nutzer-Spuren mit Audio-Clip, 0..128 Built-in-Module:
//      Graph-Neuaufbau (createNodeForEdit + prepareToPlay, Median aus 7),
//      eine Kante im Modell, Offline-Render 30 s mit 1 Thread / allen Kernen
//  blocks — dieselben Projekte, jeder 512er-Block einzeln gemessen: Mittel,
//      p99, Maximum und Bloecke ueber dem Echtzeit-Budget (11,6 ms) —
//      der Indikator fuer Aussetzer beim Live-Abspielen
//  helm — wie blocks, aber N MIDI-Spuren mit je einer Helm-Instanz (LV2,
//      echtes Fremd-Plugin) mit Akkorden
//  live — ueber das echte Audio-Geraet (ALSA, Master stumm) abspielen:
//      XRuns des Geraets und CPU-Last der Engine
#include "Routing.h"
#include "Feedback.h"
#include "HeadlessUIBehaviour.h"
#include <tracktion_graph/tracktion_graph.h>
#include <tracktion_graph/tracktion_graph/tracktion_PlayHead.h>
#include <tracktion_engine/playback/graph/tracktion_TracktionEngineNode.h>
#include <tracktion_engine/playback/graph/tracktion_EditNodeBuilder.h>
#include <tracktion_engine/playback/graph/tracktion_TracktionNodePlayer.h>

#include <chrono>
#include <cstdio>

namespace
{
    int cpusForAudio = 1;

    struct BenchBehaviour : public te::EngineBehaviour
    {
        bool autoInitialiseDeviceManager() override { return false; }
        int getNumberOfCPUsToUseForAudio() override { return cpusForAudio; }
    };

    using Clock = std::chrono::steady_clock;

    double msSince (Clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli> (Clock::now() - t0).count();
    }

    double median (std::vector<double> v)
    {
        std::sort (v.begin(), v.end());
        return v[v.size() / 2];
    }

    juce::File writeSine (const juce::File& dir, int idx, double seconds)
    {
        auto f = dir.getChildFile ("sine" + juce::String (idx) + ".wav");

        if (f.existsAsFile())
            return f;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), 44100.0, 2, 24, {}, 0));
        const int n = (int) (seconds * 44100.0);
        juce::AudioBuffer<float> b (2, n);
        const double freq = 110.0 * (1 + idx);

        for (int i = 0; i < n; ++i)
        {
            auto s = (float) (0.1 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / 44100.0));
            b.setSample (0, i, s);
            b.setSample (1, i, s);
        }

        w->writeFromAudioSampleBuffer (b, 0, n);
        return f;
    }

    std::unique_ptr<te::Edit> makeEdit (te::Engine& engine, const juce::File& dir, int numTracks, int modulesPerTrack)
    {
        auto edit = te::Edit::createSingleTrackEdit (engine);

        // Die Default-Spur entfernen, dawd-Routing anlegen
        for (auto t : te::getAudioTracks (*edit))
            edit->deleteTrack (t);

        dawd::ensureRouting (*edit);

        const juce::StringArray fx { te::EqualiserPlugin::xmlTypeName, te::CompressorPlugin::xmlTypeName,
                                     te::DelayPlugin::xmlTypeName, te::ReverbPlugin::xmlTypeName,
                                     te::ChorusPlugin::xmlTypeName, te::PhaserPlugin::xmlTypeName,
                                     te::LowPassPlugin::xmlTypeName };
        int fxIdx = 0;

        for (int i = 0; i < numTracks; ++i)
        {
            auto track = dawd::appendUserTrack (*edit);
            auto file = writeSine (dir, i, 30.0);
            track->insertWaveClip ("c" + juce::String (i), file,
                                   { { te::TimePosition(), te::TimePosition::fromSeconds (30.0) }, te::TimeDuration() }, false);
            const auto trackId = "t" + juce::String (i);

            if (modulesPerTrack == 0)
                continue;   // haengt schon an Main

            dawd::disconnectNodes (*edit, trackId, "main");
            juce::String prev = trackId;

            for (int m = 0; m < modulesPerTrack; ++m)
            {
                auto p = edit->getPluginCache().createNewPlugin (fx[fxIdx++ % fx.size()], {});
                auto id = dawd::addPluginModule (*edit, p->state, "audioFx");
                auto err = dawd::connectNodes (*edit, prev, id);

                if (err.isNotEmpty())
                    std::fprintf (stderr, "connect %s->%s: %s\n", prev.toRawUTF8(), id.toRawUTF8(), err.toRawUTF8());

                prev = id;
            }

            dawd::connectNodes (*edit, prev, "main");
        }

        return edit;
    }

    double buildGraphMs (te::Edit& edit)
    {
        tracktion::graph::PlayHead playHead;
        tracktion::graph::PlayHeadState playHeadState { playHead };
        te::ProcessState processState { playHeadState, edit.tempoSequence };
        te::CreateNodeParams cnp { processState, 44100.0, 512 };

        auto t0 = Clock::now();
        auto editNode = te::createNodeForEdit (edit, cnp);
        [[maybe_unused]] auto graph = tracktion::graph::node_player_utils::prepareToPlay (std::move (editNode), nullptr,
                                                                                          cnp.sampleRate, cnp.blockSize);
        return msSince (t0);
    }

    double renderMs (te::Edit& edit, const juce::File& out, int cpus)
    {
        cpusForAudio = cpus;
        te::Renderer::Parameters r (edit);
        r.destFile = out;
        r.audioFormat = edit.engine.getAudioFileFormatManager().getDefaultFormat();
        r.bitDepth = 24;
        r.sampleRateForAudio = 44100.0;
        r.blockSizeForAudio = 512;
        r.time = { te::TimePosition(), te::TimePosition::fromSeconds (30.0) };
        r.usePlugins = true;
        r.useMasterPlugins = true;
        r.tracksToDo.setRange (0, (int) te::getAllTracks (edit).size(), true);

        auto t0 = Clock::now();

        if (auto task = te::render_utils::createRenderTask (r, "bench", nullptr, nullptr))
        {
            while (task->runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
            {}

            if (task->errorMessage.isNotEmpty())
                std::fprintf (stderr, "render error: %s\n", task->errorMessage.toRawUTF8());
        }

        return msSince (t0);
    }
}

namespace
{
    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr double budgetMs = blockSize * 1000.0 / sr;   // 11,6 ms

    struct BlockStats { double meanMs = 0, p99Ms = 0, maxMs = 0, peak = 0; int over = 0, blocks = 0; };

    // Wie der Renderer (NodeRenderContext), aber jeder Block einzeln gestoppt
    BlockStats blockTimes (te::Edit& edit, int threads, double seconds)
    {
        tracktion::graph::PlayHead playHead;
        tracktion::graph::PlayHeadState playHeadState { playHead };
        te::ProcessState processState { playHeadState, edit.tempoSequence };
        te::CreateNodeParams cnp { processState };
        cnp.sampleRate = sr;
        cnp.blockSize = blockSize;
        cnp.forRendering = true;
        cnp.includeBypassedPlugins = false;

        te::TracktionNodePlayer player (te::createNodeForEdit (edit, cnp), processState, sr, blockSize,
                                        tracktion::graph::getPoolCreatorFunction (
                                            (tracktion::graph::ThreadPoolStrategy) te::EditPlaybackContext::getThreadPoolStrategy()));
        player.setNumThreads ((size_t) juce::jmax (0, threads - 1));

        playHead.stop();
        playHead.setPosition (0);
        playHead.playSyncedToRange ({ 0, (int64_t) 1e12 });
        playHeadState.update ({ 0, blockSize });

        // Datei-Leser und Proxies fertig werden lassen (wie renderMidi)
        for (int i = 0; i < 100; ++i)
        {
            bool ready = true;

            for (auto* n : tracktion::graph::getNodes (*player.getNode(), tracktion::graph::VertexOrdering::postordering))
            {
                n->prepareForNextBlock ({ 0, blockSize });

                if (n->getDirectInputNodes().empty() && ! n->isReadyToProcess())
                    ready = false;
            }

            if (ready)
                break;

            juce::Thread::sleep (50);
        }

        juce::AudioBuffer<float> buffer (2, blockSize);
        te::MidiMessageArray midi;
        std::vector<double> times;
        double peak = 0;   // klingt es ueberhaupt? (Pruefung, kein Messwert)
        const auto total = (int64_t) (seconds * sr);

        for (int64_t pos = 0; pos + blockSize <= total; pos += blockSize)
        {
            buffer.clear();
            midi.clear();
            edit.updateModifierTimers (te::TimePosition::fromSamples (pos, sr), blockSize);
            auto view = choc::buffer::createChannelArrayView (buffer.getArrayOfWritePointers(), 2u, (choc::buffer::FrameCount) blockSize);
            auto t0 = Clock::now();
            player.process ({ (choc::buffer::FrameCount) blockSize, { pos, pos + blockSize }, { view, midi } });
            times.push_back (msSince (t0));
            peak = juce::jmax (peak, (double) buffer.getMagnitude (0, blockSize));
        }

        BlockStats st;
        st.peak = peak;
        st.blocks = (int) times.size();

        for (auto t : times)
        {
            st.meanMs += t / times.size();
            st.over += t > budgetMs ? 1 : 0;
        }

        auto sorted = times;
        std::sort (sorted.begin(), sorted.end());
        st.p99Ms = sorted[(size_t) (0.99 * (sorted.size() - 1))];
        st.maxMs = sorted.back();
        return st;
    }

    // Helm (LV2) finden — echtes Fremd-Plugin, ein Synth mit ordentlich CPU-Last
    juce::PluginDescription findHelm (te::Engine& engine)
    {
        auto& pm = engine.getPluginManager();

        for (auto* f : pm.pluginFormatManager.getFormats())
            if (f->getName() == "LV2")
                for (auto& id : f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true))
                {
                    juce::OwnedArray<juce::PluginDescription> found;
                    f->findAllTypesForFile (found, id);

                    for (auto* d : found)
                        if (d->name.containsIgnoreCase ("helm"))
                        {
                            pm.knownPluginList.addType (*d);
                            return *d;
                        }
                }

        return {};
    }

    // N MIDI-Spuren, je eine Helm-Instanz als Instrument-Modul -> Main;
    // Vierklang alle 2 Beats ueber die ganze Laenge
    std::unique_ptr<te::Edit> makeHelmEdit (te::Engine& engine, const juce::PluginDescription& helm, int instances, double seconds)
    {
        auto edit = te::Edit::createSingleTrackEdit (engine);

        for (auto t : te::getAudioTracks (*edit))
            edit->deleteTrack (t);

        dawd::ensureRouting (*edit);
        const int chord[] = { 0, 4, 7, 11 };

        for (int i = 0; i < instances; ++i)
        {
            auto* track = dawd::appendUserTrack (*edit);
            dawd::setTrackType (*edit, *track, true);
            auto clip = track->insertMIDIClip ({ te::TimePosition(), te::TimePosition::fromSeconds (seconds) }, nullptr);
            auto beats = edit->tempoSequence.toBeats (te::TimePosition::fromSeconds (seconds)).inBeats();

            for (double b = 0; b + 2 <= beats; b += 2)
                for (auto n : chord)
                    clip->getSequence().addNote (48 + (i % 12) + n, te::BeatPosition::fromBeats (b),
                                                 te::BeatDuration::fromBeats (1.5), 100, 0, nullptr);

            auto id = dawd::addPluginModule (*edit, te::ExternalPlugin::create (engine, helm), "instrument");

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (dawd::modulePlugin (*dawd::findModuleTrack (*edit, id))))
                ep->initialiseFully();

            auto trackId = "t" + juce::String (i);

            if (auto err = dawd::connectNodes (*edit, trackId, id); err.isNotEmpty())
                std::fprintf (stderr, "connect %s: %s\n", trackId.toRawUTF8(), err.toRawUTF8());

            dawd::connectNodes (*edit, id, "main");
        }

        return edit;
    }

    void printBlocks (const char* label, int n, const BlockStats& s1, const BlockStats& sAll)
    {
        std::printf ("%s,%d,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%d,%d,%.3f\n", label, n,
                     s1.meanMs, s1.p99Ms, s1.maxMs, s1.over,
                     sAll.meanMs, sAll.p99Ms, sAll.maxMs, sAll.over, sAll.blocks, sAll.peak);
        std::fflush (stdout);
    }

    // Echtes Geraet: abspielen (Master stumm), XRuns und CPU-Last mitschreiben
    void liveRun (te::Engine& engine, te::Edit& edit, const char* label, double seconds)
    {
        auto& dm = engine.getDeviceManager();
        auto* dev = dm.deviceManager.getCurrentAudioDevice();

        if (dev == nullptr)
        {
            std::printf ("%s,no audio device\n", label);
            return;
        }

        if (auto* master = edit.getMasterVolumePlugin().get())
            master->setVolumeDb (-100.0f);

        auto& transport = edit.getTransport();
        transport.setLoopRange ({ te::TimePosition(), te::TimePosition::fromSeconds (30.0) });
        transport.looping = true;
        transport.ensureContextAllocated();
        transport.play (false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1000);   // einschwingen

        auto xr0 = dev->getXRunCount();
        double maxCpu = 0, sumCpu = 0;
        int samples = 0;
        auto end = Clock::now() + std::chrono::milliseconds ((int) (seconds * 1000));

        while (Clock::now() < end)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            auto cpu = (double) dm.getCpuUsage();
            maxCpu = juce::jmax (maxCpu, cpu);
            sumCpu += cpu;
            ++samples;
        }

        auto xr1 = dev->getXRunCount();
        transport.stop (false, false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        std::printf ("%s,%s,%.0f,%d,%.0f,%d,%.1f,%.1f\n", label, dev->getName().toRawUTF8(),
                     dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples(), seconds,
                     xr0 >= 0 && xr1 >= 0 ? xr1 - xr0 : -1, 100.0 * sumCpu / juce::jmax (1, samples), 100.0 * maxCpu);
        std::fflush (stdout);
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    te::Engine engine ("tebench", std::make_unique<HeadlessUIBehaviour>(), std::make_unique<BenchBehaviour>());
    dawd::registerFeedbackPlugins (engine);

    auto dir = juce::File (argc > 1 ? argv[1] : "/tmp").getChildFile ("tebench-media");
    dir.createDirectory();
    const juce::String mode = argc > 2 ? argv[2] : "render";

    const int numTracks = 16;
    const int numCpus = juce::SystemStats::getNumCpus();
    std::printf ("mode=%s cpus=%d sr=44100 block=512 budgetMs=%.2f\n", mode.toRawUTF8(), numCpus, budgetMs);

    if (mode == "blocks" || mode == "helm")
    {
        std::printf ("scenario,n,mean1Ms,p99_1Ms,max1Ms,over1,meanAllMs,p99AllMs,maxAllMs,overAll,blocks,peak\n");
        auto measure = [&] (te::Edit& edit, const char* label, int n)
        {
            blockTimes (edit, numCpus, 5.0);   // Warm-up
            auto s1 = blockTimes (edit, 1, 30.0);
            auto sAll = blockTimes (edit, numCpus, 30.0);
            printBlocks (label, n, s1, sAll);
        };

        if (mode == "blocks")
            for (int perTrack : { 0, 1, 2, 4, 8 })
            {
                auto edit = makeEdit (engine, dir, numTracks, perTrack);
                measure (*edit, "builtinModules", perTrack * numTracks);
            }
        else
        {
            auto helm = findHelm (engine);

            if (helm.name.isEmpty())
            {
                std::printf ("Helm (LV2) not found\n");
                return 1;
            }

            for (int n : { 1, 4, 8, 16, 32 })
            {
                auto edit = makeHelmEdit (engine, helm, n, 30.0);
                measure (*edit, "helmInstances", n);
            }
        }

        return 0;
    }

    if (mode == "live")
    {
        cpusForAudio = numCpus;
        engine.getDeviceManager().initialise (2, 2);
        std::printf ("scenario,device,sampleRate,bufferSize,seconds,xruns,cpuAvgPct,cpuMaxPct\n");

        for (int perTrack : { 0, 8 })
        {
            auto edit = makeEdit (engine, dir, numTracks, perTrack);
            liveRun (engine, *edit, perTrack == 0 ? "builtin0" : "builtin128", 15.0);
        }

        if (auto helm = findHelm (engine); helm.name.isNotEmpty())
            for (int n : { 8, 16 })
            {
                auto edit = makeHelmEdit (engine, helm, n, 30.0);
                liveRun (engine, *edit, n == 8 ? "helm8" : "helm16", 15.0);
            }

        return 0;
    }

    std::printf ("modules,modulesPerTrack,graphTracks,buildMedianMs,buildMaxMs,edgeOpUs,render1ThreadMs,renderAllMs,rtf1,rtfAll\n");

    for (int perTrack : { 0, 1, 2, 4, 8 })
    {
        auto edit = makeEdit (engine, dir, numTracks, perTrack);
        const int modules = perTrack * numTracks;

        // Warm-up (Datei-Cache, Plugin-Init), dann messen
        buildGraphMs (*edit);
        std::vector<double> builds;

        for (int i = 0; i < 7; ++i)
            builds.push_back (buildGraphMs (*edit));

        // eine Kante: disconnect + connect (nur Modell, ohne Graph)
        double edgeUs = 0;

        if (perTrack > 0)
        {
            auto first = dawd::targetsOf (*edit, dawd::findRouteNode (*edit, "t0"))[0];
            auto t0 = Clock::now();

            for (int i = 0; i < 20; ++i)
            {
                dawd::disconnectNodes (*edit, "t0", first);
                dawd::connectNodes (*edit, "t0", first);
            }

            edgeUs = msSince (t0) * 1000.0 / 40.0;
        }

        auto out = dir.getChildFile ("render.wav");
        renderMs (*edit, out, numCpus);   // Warm-up
        auto r1 = renderMs (*edit, out, 1);
        auto rAll = renderMs (*edit, out, numCpus);

        std::printf ("%d,%d,%d,%.2f,%.2f,%.1f,%.0f,%.0f,%.1f,%.1f\n",
                     modules, perTrack, (int) te::getAllTracks (*edit).size(),
                     median (builds), *std::max_element (builds.begin(), builds.end()), edgeUs,
                     r1, rAll, 30000.0 / r1, 30000.0 / rAll);
        std::fflush (stdout);
    }

    return 0;
}

// aus dawd/source/Dawd.cpp (Rest von Dawd.cpp braucht den Daemon-Kontext)
namespace dawd
{
    juce::var resultObj (bool ok, std::initializer_list<std::pair<juce::Identifier, juce::var>> props)
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("ok", ok);
        for (auto& p : props)
            o->setProperty (p.first, p.second);
        return juce::var (o);
    }
}
