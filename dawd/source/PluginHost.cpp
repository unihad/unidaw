#include "PluginSandbox.h"
#include "PluginSandboxShared.h"
#include "PluginWindows.h"

#include <cerrno>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>

// Host-Prozess der Plugin-Sandbox (dawd --plugin-host <socket-fd> --parent <pid>).
// Laedt Plugins ueber die JUCE-Formate (dieselbe Binary, dieselben JUCE-Patches
// wie dawd — Zustands-Blobs sind byteweise gleich). Ein Prozess haelt eine
// oder mehrere Instanzen (Gruppierung, Phase 3: Standard "je Plugin" = alle
// Instanzen desselben Plugins in einem Prozess). dawd schickt je Instanz einen
// memfd ueber den Socket (SCM_RIGHTS); je Instanz zwei Threads:
//  - Steuer-Thread: wartet auf ctlRequest und reicht an den Message-Thread
//  - RT-Thread: wartet auf rtRequest, rechnet den Block, meldet rtDone
// Der Message-Thread (gemeinsam) laedt, bereitet vor, liest/setzt Zustand,
// Programme, Texte und fuehrt die Editor-Fenster (VST3 verlangt ihn; die
// JUCE-Run-Loop bedient X11 und die IRunLoop der VST3-Editoren).
// Was das Plugin selbst meldet (Editor, eigene Modulation, neue Namen),
// schreibt ein AudioProcessorListener in den Speicher (Rueckweg, Phase 2).
// Schliesst dawd den Socket (keine Instanz mehr) oder ist dawd weg
// (Elternprozess alle 0,5 s geprueft), endet der Host.
namespace dawd
{
    using namespace sandbox;

    namespace
    {
        struct HostPlayHead : public juce::AudioPlayHead
        {
            juce::Optional<PositionInfo> getPosition() const override
            {
                if (! valid)
                    return {};

                return info;
            }

            PositionInfo info;
            bool valid = false;
        };

        // Meldungen des Plugins an seinen Host (setValueNotifyingHost aus dem
        // Editor, Ausgangs-Parameter im Block, Gesten, neue Namen/Programme)
        // in den Speicher — aus jedem Thread, ohne Sperre
        struct Reporter : public juce::AudioProcessorListener
        {
            explicit Reporter (Shm* s) : shm (s) {}

            void audioProcessorParameterChanged (juce::AudioProcessor*, int i, float v) override
            {
                if (! juce::isPositiveAndBelow (i, maxParams))
                    return;

                shm->outValue[i].store (v, std::memory_order_relaxed);
                shm->outBase[i].store (shm->paramApplied[i].load (std::memory_order_acquire), std::memory_order_relaxed);
                shm->outSeq[i].fetch_add (1, std::memory_order_release);
                bumpHostEvents (shm);
            }

            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
            {
                // nur, was dawd nachfragen muss; die Latenz kommt mit jedem Block
                if (d.parameterInfoChanged || d.programChanged)
                {
                    shm->infoSeq.fetch_add (1, std::memory_order_release);
                    bumpHostEvents (shm);
                }
            }

            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int i) override
            {
                if (juce::isPositiveAndBelow (i, maxParams))
                {
                    shm->gestureBegin[i].fetch_add (1, std::memory_order_release);
                    bumpHostEvents (shm);
                }
            }

            void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int i) override
            {
                if (juce::isPositiveAndBelow (i, maxParams))
                {
                    shm->gestureEnd[i].fetch_add (1, std::memory_order_release);
                    bumpHostEvents (shm);
                }
            }

            Shm* shm;
        };

        // Editor-Fenster im Host: dieselbe Lage-Logik wie in dawd
        // (NativeEditorWindow); Lage und Schliessen meldet es dawd, dawd
        // schliesst es ueber die Op pluginWindow (Aktion, Undo, windows.json)
        struct HostEditorWindow : public NativeEditorWindow
        {
            HostEditorWindow (Shm* s, const juce::String& title, juce::AudioProcessorEditor* editor, juce::Point<int> at)
                : NativeEditorWindow (title, editor, editor->isResizable(), at), shm (s)
            {
                tracking = true;
            }

            void moved() override
            {
                juce::DocumentWindow::moved();

                if (tracking)
                    if (auto o = outerPosition())
                    {
                        shm->windowX.store (o->x, std::memory_order_relaxed);
                        shm->windowY.store (o->y, std::memory_order_relaxed);
                        shm->windowMoves.fetch_add (1, std::memory_order_release);
                        bumpHostEvents (shm);
                    }
            }

            void closeButtonPressed() override
            {
                shm->windowCloseRequests.fetch_add (1, std::memory_order_release);
                bumpHostEvents (shm);
            }

            Shm* shm;
            bool tracking = false;
        };

        // LV2-Klassen geladen (searchPathsForPlugins, einmal je Prozess; Message-Thread)
        bool lv2ClassesLoaded = false;

        // Eine Plugin-Instanz im Host: ihr Speicher (memfd von dawd), ihre
        // Steuer- und RT-Threads. Op quit entfernt nur sie, nicht den Prozess.
        struct PluginHost
        {
            PluginHost (Shm* s, int memFd, juce::AudioPluginFormatManager& f, std::function<void (PluginHost*)> remove)
                : shm (s), fd (memFd), formats (f), removeMe (std::move (remove)), reporter (s)
            {
                midi.ensureSize (16384);
            }

            ~PluginHost()
            {
                stopThreads();

                // Fenster (Editor) vor dem Plugin
                window.reset();

                if (plugin != nullptr)
                    plugin->removeListener (&reporter);

                plugin.reset();
                ::munmap (shm, shmSize);
                ::close (fd);
            }

            void stopThreads()
            {
                quit = true;
                // Wecken ohne neuen Zaehler: die Schleifen pruefen quit
                futexWake (shm->ctlRequest.v);
                futexWake (shm->rtRequest.v);

                if (controlThread.joinable() && controlThread.get_id() != std::this_thread::get_id())
                    controlThread.join();

                if (rtThread.joinable())
                    rtThread.join();
            }

            Shm* shm;
            const int fd;
            juce::AudioPluginFormatManager& formats;
            std::function<void (PluginHost*)> removeMe;
            std::unique_ptr<juce::AudioPluginInstance> plugin;
            juce::Array<juce::AudioProcessorParameter*> params;
            int numApplied = 0;                       // Parameter mit Platz im Speicher
            Reporter reporter;
            std::unique_ptr<HostEditorWindow> window;
            std::mutex instanceLock;                  // prepare/release/setState/Programm gegen den Block
            std::atomic<bool> quit { false };
            bool prepared = false;
            int channels = 0;                         // max(Ein, Aus) des Plugins
            HostPlayHead playHead;
            juce::MidiBuffer midi;
            std::thread rtThread, controlThread;

            // Werte aus dem Speicher uebernehmen, die dawd seit dem letzten Mal
            // gesetzt hat (vor jedem Block und vor Zustand/Text). Der RT- und
            // der Message-Thread duerfen beide: derselbe Wert zweimal schadet nicht.
            void syncParameters()
            {
                for (int i = 0; i < numApplied; ++i)
                {
                    auto seq = shm->paramSeq[i].load (std::memory_order_acquire);

                    if (shm->paramApplied[i].exchange (seq, std::memory_order_acq_rel) != seq)
                        params.getUnchecked (i)->setValue (shm->paramValue[i].load (std::memory_order_relaxed));
                }
            }

            juce::var programNames()
            {
                juce::Array<juce::var> programs;

                for (int i = 0; i < plugin->getNumPrograms(); ++i)
                    programs.add (plugin->getProgramName (i));

                return programs;
            }

            juce::var currentValues()
            {
                juce::Array<juce::var> vs;

                for (auto* p : params)
                    vs.add (p->getValue());

                return vs;
            }

            //==============================================================================
            // seen: rtRequest vor dem Start des Threads gelesen (siehe load)
            void rtLoop (uint32_t seen)
            {
                // Wie Tracktions Audio-Worker (TRACKTION_RT_PRIORITY, dawd setzt 10);
                // ohne Recht bleibt es beim normalen Scheduler
                sched_param sp {};
                sp.sched_priority = juce::jlimit (1, 99, juce::SystemStats::getEnvironmentVariable ("TRACKTION_RT_PRIORITY", "10").getIntValue());
                pthread_setschedparam (pthread_self(), SCHED_FIFO, &sp);

                while (! quit)
                {
                    auto r = waitForChange (shm->rtRequest.v, seen, 500LL * 1000000LL);

                    if (r == seen)
                        continue;

                    seen = r;
                    auto t0 = nowNs();
                    processBlock();
                    shm->rt.dspNs = nowNs() - t0;
                    shm->rtDone.v.store (seen, std::memory_order_release);
                    futexWake (shm->rtDone.v);
                }
            }

            void processBlock()
            {
                auto& rt = shm->rt;
                const int n = juce::jlimit (0, maxFrames, (int) rt.numSamples);
                const int sent = juce::jlimit (0, maxChannels, (int) rt.numChannels);
                rt.midiOutBytes = 0;

                const std::lock_guard<std::mutex> l (instanceLock);

                if (plugin == nullptr || ! prepared || n == 0)
                {
                    for (int c = 0; c < sent; ++c)
                        juce::FloatVectorOperations::clear (shm->audio[c], n);

                    return;
                }

                if ((rt.flags & rtReset) != 0)
                    plugin->reset();

                const bool nonRealtime = (rt.flags & rtNonRealtime) != 0;

                if (plugin->isNonRealtime() != nonRealtime)
                    plugin->setNonRealtime (nonRealtime);

                syncParameters();

                playHead.valid = (rt.flags & rtHasPosition) != 0;

                if (playHead.valid)
                    playHead.info = rt.position;

                // Das Plugin bekommt immer so viele Kanaele, wie es hat
                for (int c = sent; c < channels; ++c)
                    juce::FloatVectorOperations::clear (shm->audio[c], n);

                float* chans[maxChannels];

                for (int c = 0; c < channels; ++c)
                    chans[c] = shm->audio[c];

                juce::AudioBuffer<float> buffer (chans, channels, n);
                midi.clear();
                readMidi (shm->midiIn, juce::jmin<uint32_t> (rt.midiInBytes, (uint32_t) midiBytes), midi, 0);

                if ((rt.flags & rtBypassed) != 0)
                    plugin->processBlockBypassed (buffer, midi);
                else
                    plugin->processBlock (buffer, midi);

                rt.midiOutBytes = writeMidi (shm->midiOut, midiBytes, midi, 0, n, 0);
                rt.latency = plugin->getLatencySamples();
            }

            //==============================================================================
            // seen: ctlRequest vor dem Start des Threads gelesen — las der
            // Thread ihn selbst, konnte die erste Anfrage (load) schon da sein
            // und ging verloren (Host hing bis zur Frist, 2026-10-26)
            void controlLoop (uint32_t seen)
            {
                while (! quit)
                {
                    auto r = waitForChange (shm->ctlRequest.v, seen, 500LL * 1000000LL);

                    if (r == seen || quit)
                        continue;

                    seen = r;
                    juce::WaitableEvent done;
                    bool stop = false;

                    juce::MessageManager::callAsync ([this, &done, &stop]
                    {
                        stop = handleControl();
                        done.signal();
                    });

                    done.wait();
                    shm->ctlDone.v.store (seen, std::memory_order_release);
                    futexWake (shm->ctlDone.v);

                    if (stop)
                    {
                        // nur diese Instanz: der Prozess endet, wenn dawd den Socket schliesst
                        quit = true;
                        juce::MessageManager::callAsync ([this] { removeMe (this); });
                    }
                }
            }

            void reply (bool ok, const juce::var& json, const void* blob = nullptr, size_t blobBytes = 0)
            {
                if (! writeControl (shm, json, blob, blobBytes))
                {
                    auto o = new juce::DynamicObject();
                    o->setProperty ("error", "plugin sandbox: data too large (" + juce::String ((juce::int64) blobBytes) + " bytes)");
                    writeControl (shm, juce::var (o), nullptr, 0);
                    ok = false;
                }

                shm->ctl.ok = ok ? 1 : 0;
            }

            void fail (const juce::String& error)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("error", error);
                reply (false, juce::var (o));
            }

            // Auf dem Message-Thread; true = Host beenden
            bool handleControl()
            {
                juce::MemoryBlock blob;
                auto args = readControl (shm, &blob);
                const auto op = shm->ctl.op;

                if (op == Op::quit)
                {
                    window.reset();
                    reply (true, {});
                    return true;
                }

                if (op == Op::load)
                {
                    load (args);
                    return false;
                }

                if (plugin == nullptr)
                {
                    fail ("no plugin loaded");
                    return false;
                }

                auto o = new juce::DynamicObject();
                juce::var out (o);

                switch (op)
                {
                    case Op::prepare:
                    {
                        const std::lock_guard<std::mutex> l (instanceLock);
                        plugin->setNonRealtime ((bool) args.getProperty ("nonRealtime", false));
                        plugin->prepareToPlay ((double) args.getProperty ("rate", 44100.0), (int) args.getProperty ("block", 512));
                        prepared = true;
                        o->setProperty ("latency", plugin->getLatencySamples());
                        reply (true, out);
                        break;
                    }

                    case Op::release:
                    {
                        const std::lock_guard<std::mutex> l (instanceLock);
                        plugin->releaseResources();
                        prepared = false;
                        reply (true, out);
                        break;
                    }

                    case Op::getState:
                    {
                        syncParameters();
                        juce::MemoryBlock mb;
                        plugin->getStateInformation (mb);
                        reply (true, out, mb.getData(), mb.getSize());
                        break;
                    }

                    case Op::setState:
                    {
                        const std::lock_guard<std::mutex> l (instanceLock);
                        plugin->setStateInformation (blob.getData(), (int) blob.getSize());
                        o->setProperty ("values", currentValues());
                        o->setProperty ("latency", plugin->getLatencySamples());
                        o->setProperty ("program", plugin->getCurrentProgram());
                        reply (true, out);
                        break;
                    }

                    case Op::setProgram:
                    {
                        const std::lock_guard<std::mutex> l (instanceLock);
                        plugin->setCurrentProgram ((int) args.getProperty ("index", 0));
                        o->setProperty ("values", currentValues());
                        o->setProperty ("program", plugin->getCurrentProgram());
                        reply (true, out);
                        break;
                    }

                    case Op::getText:
                    case Op::getValueForText:
                    {
                        const int i = (int) args.getProperty ("index", -1);

                        if (! juce::isPositiveAndBelow (i, params.size()))
                        {
                            fail ("no such parameter: " + juce::String (i));
                            break;
                        }

                        if (op == Op::getText)
                            o->setProperty ("text", params[i]->getText ((float) args.getProperty ("value", 0.0), 1024));
                        else
                            o->setProperty ("value", params[i]->getValueForText (args.getProperty ("text", {}).toString()));

                        reply (true, out);
                        break;
                    }

                    case Op::info:
                    {
                        // Werte erst nach den offenen von dawd: sonst ueberschriebe
                        // ein alter Wert hier einen neueren in dawd
                        syncParameters();

                        // VST3: Controller mit dem Zustand der Komponente abgleichen
                        // (Surge XT uebernimmt einen Zustand erst im Block und meldet
                        // die Werte nicht; Patch juce-vst3-sync-controller)
                        if ((bool) args.getProperty ("syncController", false))
                            if (auto* vst3 = plugin->getVST3Client())
                                vst3->syncControllerWithComponent();

                        juce::Array<juce::var> names, labels;

                        for (auto* prm : params)
                        {
                            names.add (prm->getName (1024));
                            labels.add (prm->getLabel());
                        }

                        o->setProperty ("names", names);
                        o->setProperty ("labels", labels);
                        o->setProperty ("values", currentValues());
                        o->setProperty ("programs", programNames());
                        o->setProperty ("program", plugin->getCurrentProgram());
                        o->setProperty ("latency", plugin->getLatencySamples());
                        reply (true, out);
                        break;
                    }

                    case Op::setProgramName:
                    {
                        plugin->changeProgramName ((int) args.getProperty ("index", 0), args.getProperty ("name", {}).toString());
                        o->setProperty ("programs", programNames());
                        reply (true, out);
                        break;
                    }

                    case Op::setLayout:
                    {
                        const auto layout = layoutFrom (args);

                        if (! (bool) args.getProperty ("apply", false))
                        {
                            o->setProperty ("supported", plugin->checkBusesLayoutSupported (layout));
                            reply (true, out);
                            break;
                        }

                        const std::lock_guard<std::mutex> l (instanceLock);

                        if (prepared)
                        {
                            plugin->releaseResources();
                            prepared = false;
                        }

                        const bool ok = plugin->setBusesLayout (layout);
                        const int n = juce::jmax (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());

                        if (ok && n <= maxChannels)
                            channels = n;

                        o->setProperty ("supported", ok && n <= maxChannels);
                        reply (true, out);
                        break;
                    }

                    case Op::openWindow:
                    {
                        const juce::Point<int> at ((int) args.getProperty ("x", 100), (int) args.getProperty ("y", 100));

                        if (window == nullptr)
                        {
                            auto* editor = plugin->createEditorAndMakeActive();

                            if (editor == nullptr)
                            {
                                fail ("editor could not be created: " + plugin->getName());
                                break;
                            }

                            window = std::make_unique<HostEditorWindow> (shm, args.getProperty ("title", plugin->getName()).toString(), editor, at);
                        }

                        window->setVisible (true);
                        window->toFront (false);
                        reply (true, out);
                        break;
                    }

                    case Op::moveWindow:
                    {
                        if (window != nullptr)
                            window->place ({ (int) args.getProperty ("x", 0), (int) args.getProperty ("y", 0) });

                        reply (true, out);
                        break;
                    }

                    case Op::closeWindow:
                    {
                        window.reset();
                        reply (true, out);
                        break;
                    }

                    case Op::hasEditor:
                    {
                        o->setProperty ("hasEditor", plugin->hasEditor());
                        reply (true, out);
                        break;
                    }

                    case Op::editParam:
                    {
                        const int i = (int) args.getProperty ("index", -1);

                        if (! juce::isPositiveAndBelow (i, params.size()))
                        {
                            fail ("no such parameter: " + juce::String (i));
                            break;
                        }

                        // wie die Oberflaeche des Plugins: Geste und Meldung an den Host
                        auto* prm = params[i];
                        prm->beginChangeGesture();
                        prm->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) args.getProperty ("value", 0.0)));
                        prm->endChangeGesture();
                        reply (true, out);
                        break;
                    }

                    case Op::none:
                    case Op::load:
                    case Op::quit:
                    default:
                        fail ("unknown op");
                        break;
                }

                return false;
            }

            static juce::var busesVar (juce::AudioProcessor& p, bool isInput)
            {
                juce::Array<juce::var> buses;

                for (int i = 0; i < p.getBusCount (isInput); ++i)
                {
                    auto* bus = p.getBus (isInput, i);
                    juce::Array<juce::var> types;

                    for (auto t : bus->getLastEnabledLayout().getChannelTypes())
                        types.add ((int) t);

                    auto b = new juce::DynamicObject();
                    b->setProperty ("name", bus->getName());
                    b->setProperty ("types", types);
                    b->setProperty ("enabled", bus->isEnabled());
                    buses.add (juce::var (b));
                }

                return buses;
            }

            void load (const juce::var& args)
            {
                if (plugin != nullptr)
                {
                    fail ("plugin already loaded");
                    return;
                }

                juce::PluginDescription desc;
                auto xml = juce::parseXML (args.getProperty ("desc", {}).toString());

                if (xml == nullptr || ! desc.loadFromXml (*xml))
                {
                    fail ("bad plugin description");
                    return;
                }

                // LV2: wie im Scan-Kind (runPluginScan) einmal searchPathsForPlugins,
                // sonst fehlen lilv die Plugin-Klassen und die Beschreibung der
                // Instanz sagt Kategorie "Plugin", isInstrument false — die Engine
                // uebernimmt sie (ExternalPlugin::initialise), isSynth waere falsch
                if (desc.pluginFormatName == "LV2" && ! lv2ClassesLoaded)
                {
                    lv2ClassesLoaded = true;

                    for (auto* f : formats.getFormats())
                        if (f->getName() == "LV2")
                            f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, false);
                }

                juce::String error;
                auto p = formats.createPluginInstance (desc, (double) args.getProperty ("rate", 44100.0),
                                                       (int) args.getProperty ("block", 512), error);

                if (p == nullptr)
                {
                    fail (error.isNotEmpty() ? error : "cannot load " + desc.name);
                    return;
                }

                p->enableAllBuses();
                channels = juce::jmax (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels());

                if (channels > maxChannels || p->getParameters().size() > maxParams)
                {
                    fail ("plugin sandbox: " + desc.name + " has too many channels or parameters ("
                          + juce::String (channels) + ", " + juce::String (p->getParameters().size()) + ")");
                    return;
                }

                p->setPlayHead (&playHead);
                params = p->getParameters();

                auto o = new juce::DynamicObject();
                juce::var out (o);

                if (auto d = p->getPluginDescription().createXml())
                    o->setProperty ("desc", d->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

                o->setProperty ("name", p->getName());
                o->setProperty ("latency", p->getLatencySamples());
                o->setProperty ("tail", p->getTailLengthSeconds());
                o->setProperty ("acceptsMidi", p->acceptsMidi());
                o->setProperty ("producesMidi", p->producesMidi());
                o->setProperty ("isMidiEffect", p->isMidiEffect());
                o->setProperty ("inputs", busesVar (*p, true));
                o->setProperty ("outputs", busesVar (*p, false));

                juce::Array<juce::var> ps;

                for (auto* prm : params)
                {
                    auto q = new juce::DynamicObject();

                    if (auto* h = dynamic_cast<juce::HostedAudioProcessorParameter*> (prm))
                        q->setProperty ("id", h->getParameterID());

                    q->setProperty ("name", prm->getName (1024));
                    q->setProperty ("label", prm->getLabel());
                    q->setProperty ("default", prm->getDefaultValue());
                    q->setProperty ("value", prm->getValue());
                    q->setProperty ("steps", prm->getNumSteps());
                    q->setProperty ("discrete", prm->isDiscrete());
                    q->setProperty ("boolean", prm->isBoolean());
                    q->setProperty ("automatable", prm->isAutomatable());
                    q->setProperty ("meta", prm->isMetaParameter());
                    q->setProperty ("inverted", prm->isOrientationInverted());
                    ps.add (juce::var (q));
                }

                o->setProperty ("params", ps);

                {
                    const std::lock_guard<std::mutex> l (instanceLock);
                    plugin = std::move (p);
                    numApplied = params.size();
                }

                o->setProperty ("programs", programNames());
                o->setProperty ("program", plugin->getCurrentProgram());

                // ab jetzt meldet das Plugin seine Aenderungen an dawd
                plugin->addListener (&reporter);

                std::cerr << "[plugin-host " << ::getpid() << "] loaded " << desc.name << "\n";

                // dawd wartet gerade auf diese Antwort: noch kein Block unterwegs
                if (! rtThread.joinable())
                    rtThread = std::thread ([this, seen = shm->rtRequest.v.load()] { rtLoop (seen); });

                reply (true, out);
            }
        };
    }

    namespace
    {
        // Der Prozess: Socket zu dawd, Liste der Instanzen. Der Socket-Thread
        // nimmt neue Instanzen an (memfd per SCM_RIGHTS) und prueft dawd.
        struct HostProcess
        {
            HostProcess (int s, int parent) : sock (s), parentPid (parent)
            {
                juce::addDefaultFormatsToManager (formats);
            }

            const int sock, parentPid;
            juce::AudioPluginFormatManager formats;
            std::mutex instancesLock;
            std::vector<std::unique_ptr<PluginHost>> instances;
            std::atomic<bool> stop { false };
            std::thread socketThread;

            // memfd aus der Nachricht; -1 = Socket zu (dawd fertig oder weg)
            int receiveFd()
            {
                char byte = 0;
                iovec io { &byte, 1 };
                alignas (cmsghdr) char ctrl[CMSG_SPACE (sizeof (int))] {};
                msghdr msg {};
                msg.msg_iov = &io;
                msg.msg_iovlen = 1;
                msg.msg_control = ctrl;
                msg.msg_controllen = sizeof (ctrl);

                auto n = ::recvmsg (sock, &msg, MSG_CMSG_CLOEXEC);

                if (n <= 0)
                    return n < 0 && errno == EINTR ? -2 : -1;

                for (auto* c = CMSG_FIRSTHDR (&msg); c != nullptr; c = CMSG_NXTHDR (&msg, c))
                    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                    {
                        int fd = -1;
                        std::memcpy (&fd, CMSG_DATA (c), sizeof (int));
                        return fd;
                    }

                return -2;   // Nachricht ohne fd: ignorieren
            }

            void socketLoop()
            {
                while (! stop)
                {
                    pollfd p { sock, POLLIN, 0 };
                    const int rc = ::poll (&p, 1, 500);

                    if (::getppid() != parentPid)
                    {
                        std::cerr << "[plugin-host " << ::getpid() << "] dawd is gone, exiting\n";
                        std::_Exit (0);
                    }

                    if (rc <= 0)
                        continue;

                    const int fd = receiveFd();

                    if (fd == -2)
                        continue;

                    if (fd < 0)
                    {
                        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
                        return;
                    }

                    addInstance (fd);
                }
            }

            void addInstance (int fd)
            {
                struct stat st {};

                if (::fstat (fd, &st) != 0 || (size_t) st.st_size < shmSize)
                {
                    std::cerr << "[plugin-host] no shared memory in the message\n";
                    ::close (fd);
                    return;
                }

                auto* mem = ::mmap (nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

                if (mem == MAP_FAILED)
                {
                    std::cerr << "[plugin-host] mmap failed\n";
                    ::close (fd);
                    return;
                }

                auto* shm = static_cast<Shm*> (mem);

                if (shm->magic != shmMagic || shm->version != shmVersion)
                {
                    std::cerr << "[plugin-host] shared memory has wrong version\n";
                    ::munmap (mem, shmSize);
                    ::close (fd);
                    return;
                }

                auto inst = std::make_unique<PluginHost> (shm, fd, formats, [this] (PluginHost* h) { remove (h); });
                // dawd schreibt die erste Anfrage erst nach hostReady
                inst->controlThread = std::thread ([h = inst.get(), seen = shm->ctlRequest.v.load()] { h->controlLoop (seen); });

                {
                    const std::lock_guard<std::mutex> l (instancesLock);
                    instances.push_back (std::move (inst));
                }

                shm->hostPid = (int32_t) ::getpid();
                shm->hostReady.store (1, std::memory_order_release);
                futexWake (shm->hostReady);
            }

            // Auf dem Message-Thread (nach Op quit der Instanz)
            void remove (PluginHost* h)
            {
                std::unique_ptr<PluginHost> gone;

                {
                    const std::lock_guard<std::mutex> l (instancesLock);

                    for (auto it = instances.begin(); it != instances.end(); ++it)
                        if (it->get() == h)
                        {
                            gone = std::move (*it);
                            instances.erase (it);
                            break;
                        }
                }

                gone.reset();
            }
        };
    }

    int runPluginHost (int sock, int parentPid)
    {
        struct stat st {};

        if (::fstat (sock, &st) != 0 || ! S_ISSOCK (st.st_mode))
        {
            std::cerr << "[plugin-host] no socket on fd " << sock << "\n";
            return 2;
        }

        if (::getppid() != parentPid)
            return 0;

        auto host = std::make_unique<HostProcess> (sock, parentPid);
        host->socketThread = std::thread ([h = host.get()] { h->socketLoop(); });

        juce::MessageManager::getInstance()->runDispatchLoop();

        host->stop = true;

        if (host->socketThread.joinable())
            host->socketThread.join();

        // Instanzen: Threads, Fenster, Plugins
        std::vector<std::unique_ptr<PluginHost>> instances;

        {
            const std::lock_guard<std::mutex> l (host->instancesLock);
            instances.swap (host->instances);
        }

        instances.clear();
        ::close (sock);
        return 0;
    }

    // Scan-Kind (dawd --plugin-scan <fd> <format> <datei>...): je Datei eine
    // JSON-Zeile {i, descs} auf fd — nicht stdout, dort schreiben manche
    // Plugins selbst hin. Stuerzt ein Plugin ab, sieht dawd, bei welcher Datei.
    int runPluginScan (int outFd, const juce::String& formatName, const juce::StringArray& files)
    {
        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);
        juce::AudioPluginFormat* format = nullptr;

        for (auto* f : formats.getFormats())
            if (f->getName() == formatName)
                format = f;

        if (format == nullptr)
        {
            std::cerr << "[plugin-scan] unknown format " << formatName << "\n";
            return 2;
        }

        // LV2: JUCE laedt seine eingebauten Bundles (lv2core mit den
        // Plugin-Klassen) erst nach dem ersten lilv_world_load_all; Klassen
        // registriert lilv nur dort. Ohne zweites load_all (wie der Scan im
        // Prozess ueber searchPathsForPlugins) hiesse jedes LV2 "Plugin",
        // isInstrument = false — ohne lv2core im System (hier der Fall)
        if (formatName == "LV2")
            format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false);

        for (int i = 0; i < files.size(); ++i)
        {
            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, files[i]);
            juce::Array<juce::var> descs;

            for (auto* d : found)
                if (auto xml = d->createXml())
                    descs.add (xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

            auto o = new juce::DynamicObject();
            o->setProperty ("i", i);
            o->setProperty ("descs", descs);
            const auto line = (juce::JSON::toString (juce::var (o), true) + "\n").toStdString();

            for (size_t done = 0; done < line.size();)
            {
                const auto n = ::write (outFd, line.data() + done, line.size() - done);

                if (n <= 0)
                    return 3;

                done += (size_t) n;
            }
        }

        return 0;
    }
}
