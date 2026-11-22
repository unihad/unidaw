#include "PluginSandbox.h"
#include "PluginSandboxShared.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

extern char** environ;

// Stellvertreter in dawd (Phase 1–4, LV2 seit Phase 4). Der Host-Prozess steht in
// PluginHost.cpp, das Speicher-Layout in PluginSandboxShared.h.
//
// Phase 3: ein Host-Prozess kann mehrere Instanzen halten (Gruppierung,
// Standard "je Plugin"); stirbt oder haengt er, laedt jeder Stellvertreter
// sein Plugin mit dem letzten Zustand in einem neuen Host — ausser das Plugin
// stuerzte zu oft (Sperrliste), dann bleibt es stumm bis reloadPlugin.
namespace dawd
{
    using namespace sandbox;

    namespace
    {
        // Fristen. Laden darf dauern (grosse Instrumente). Schwere Steuer-Aufrufe
        // (Zustand, prepare, Fenster) muessen ankommen: nach 10 s gilt der Host
        // als haengend und wird beendet. Leichte (Texte, Namen, Editor-Frage)
        // geben nach 2 s auf und liefern einen Ersatz; der Host darf die Antwort
        // nachreichen, haengt er 10 s, beendet ihn der Waechter. Ein Render
        // wartet lange, Echtzeit hoechstens einen Block (mindestens 5 ms, fuer
        // sehr kurze Bloecke); ein Block vom Message-Thread ohne Render
        // (passPendingValuesToPlugin) wie ein schwerer Aufruf. Bleibt ein
        // Echtzeit-Block 5 s im Host, beendet ihn der Waechter.
        constexpr int loadTimeoutMs = 60000;
        constexpr int heavyTimeoutMs = 10000;
        constexpr int lightTimeoutMs = 2000;
        constexpr int quitTimeoutMs = 2000;
        constexpr int64_t pendingKillNs = 10LL * 1000000000LL;
        constexpr int64_t rtStuckNs = 5LL * 1000000000LL;
        constexpr int64_t renderDeadlineNs = 30LL * 1000000000LL;
        constexpr int64_t minLiveDeadlineNs = 5LL * 1000000LL;

        // Sperrliste: stuerzt ein Plugin so oft in dieser Zeit ab, laedt dawd es
        // nicht mehr von selbst neu (bis reloadPlugin)
        constexpr int crashLimit = 2;
        constexpr int64_t crashWindowNs = 60LL * 1000000000LL;

        enum class Wait { light, heavy, load, quit };

        int timeoutMs (Wait w)
        {
            switch (w)
            {
                case Wait::light: return lightTimeoutMs;
                case Wait::heavy: return heavyTimeoutMs;
                case Wait::load:  return loadTimeoutMs;
                case Wait::quit:  return quitTimeoutMs;
            }

            return heavyTimeoutMs;
        }

        // Ein Host-Prozess (dawd --plugin-host). Gehoert allen Instanzen darin
        // (shared_ptr); der letzte Kanal schliesst den Socket, dann endet er.
        struct HostProcess
        {
            pid_t pid = -1;
            int pidfd = -1;
            int sock = -1;                          // memfd je Instanz per SCM_RIGHTS
            juce::String identity, group;           // Plugin, "plugin" | "single"
            std::atomic<bool> dead { false };
            std::atomic<bool> crashNoted { false }; // Absturz einmal je Prozess gezaehlt
            std::mutex endLock;
            bool reaped = false;
            juce::String killReason, endText;

            ~HostProcess()
            {
                if (sock >= 0)
                    ::close (sock);   // der Host endet, sobald er das sieht

                if (pid > 0 && ! reaped)
                {
                    // hoechstens 2 s auf das Ende warten, dann hart
                    for (int i = 0; i < 200 && ! exited (0); ++i)
                        ::usleep (10000);

                    if (! exited (0))
                        ::kill (pid, SIGKILL);

                    ::waitpid (pid, nullptr, 0);
                    reaped = true;
                }

                if (pidfd >= 0)
                    ::close (pidfd);
            }

            // Prozess beendet? (pidfd wird lesbar; der Zombie bleibt bis waitpid)
            bool exited (int ms) const
            {
                if (pidfd < 0)
                    return true;

                pollfd p { pidfd, POLLIN, 0 };
                return ::poll (&p, 1, ms) > 0;
            }

            void markDead (const juce::String& why)
            {
                if (! dead.exchange (true))
                    std::cerr << "[sandbox] plugin host " << pid << " failed: " << why << "\n";
            }

            // Haengt (Frist verpasst): beenden statt weiterreden
            void kill (const juce::String& why)
            {
                {
                    const std::lock_guard<std::mutex> l (endLock);

                    if (killReason.isEmpty())
                        killReason = why;
                }

                markDead (why);

                if (pid > 0)
                    ::kill (pid, SIGKILL);
            }

            // Warum der Host weg ist (raeumt den Zombie, sobald er beendet ist)
            juce::String reap()
            {
                const std::lock_guard<std::mutex> l (endLock);

                if (! reaped && pid > 0)
                {
                    int status = 0;

                    if (::waitpid (pid, &status, WNOHANG) == pid)
                    {
                        reaped = true;

                        if (WIFSIGNALED (status))
                            endText = "crashed: signal " + juce::String (WTERMSIG (status))
                                    + " (" + juce::String (::strsignal (WTERMSIG (status))) + ")";
                        else
                            endText = "exited with code " + juce::String (WEXITSTATUS (status));
                    }
                }

                if (killReason.isNotEmpty())
                    return killReason;

                return endText.isNotEmpty() ? endText : juce::String ("stopped");
            }

            bool launch (juce::String& error)
            {
                int sv[2];

                if (::socketpair (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0)
                {
                    error = "plugin sandbox: no socket (" + juce::String (std::strerror (errno)) + ")";
                    return false;
                }

                sock = sv[0];

                // Host = diese Binary; der Socket als fd 3, alle anderen fds
                // (HTTP-Socket, Dateien) bleiben zu
                auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
                auto parent = std::to_string (::getpid());
                std::vector<char*> argv { exe.data(), const_cast<char*> ("--plugin-host"), const_cast<char*> ("3"),
                                          const_cast<char*> ("--parent"), parent.data(), nullptr };

                posix_spawn_file_actions_t fa;
                posix_spawn_file_actions_init (&fa);
                posix_spawn_file_actions_adddup2 (&fa, sv[1], 3);
                posix_spawn_file_actions_addclosefrom_np (&fa, 4);
                auto rc = ::posix_spawn (&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
                posix_spawn_file_actions_destroy (&fa);
                ::close (sv[1]);

                if (rc != 0)
                {
                    pid = -1;
                    dead = true;
                    error = "plugin sandbox: cannot start host (" + juce::String (std::strerror (rc)) + ")";
                    return false;
                }

                pidfd = (int) ::syscall (SYS_pidfd_open, pid, 0);
                return true;
            }

            bool sendFd (int fd)
            {
                char byte = 'a';
                iovec io { &byte, 1 };
                alignas (cmsghdr) char ctrl[CMSG_SPACE (sizeof (int))] {};
                msghdr msg {};
                msg.msg_iov = &io;
                msg.msg_iovlen = 1;
                msg.msg_control = ctrl;
                msg.msg_controllen = sizeof (ctrl);
                auto* c = CMSG_FIRSTHDR (&msg);
                c->cmsg_level = SOL_SOCKET;
                c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN (sizeof (int));
                std::memcpy (CMSG_DATA (c), &fd, sizeof (int));
                return ::sendmsg (sock, &msg, MSG_NOSIGNAL) == 1;
            }
        };

        // Eine Instanz im Host: ihr Speicher (RT-Kanal, Steuerkanal, Rueckweg)
        struct Channel
        {
            std::shared_ptr<HostProcess> host;
            int fd = -1;
            Shm* shm = nullptr;
            std::mutex controlLock;
            std::atomic<uint32_t> ctlSeq { 0 };
            std::atomic<int64_t> sentNs { 0 };      // Steuer-Anfrage offen seit (0 = keine)
            std::atomic<bool> inCall { false };      // jemand wartet gerade selbst mit Frist
            std::atomic<uint32_t> rtSeq { 0 };       // nur der Audio-Thread schreibt (unter processMutex der Engine)
            std::atomic<int64_t> rtSentNs { 0 };     // Block geschickt um (Waechter)
            std::atomic<bool> rtSentNonRealtime { false };
            bool lost = false;                       // Message-Thread: Ausfall verbucht

            ~Channel()
            {
                if (shm != nullptr && ! dead())
                {
                    // nur diese Instanz; der Prozess endet mit dem letzten Kanal
                    juce::var reply;
                    juce::String err;
                    call (Op::quit, {}, nullptr, 0, reply, nullptr, Wait::quit, err);
                }

                if (shm != nullptr)
                    ::munmap (shm, shmSize);

                if (fd >= 0)
                    ::close (fd);
            }

            bool dead() const   { return host == nullptr || host->dead.load(); }

            bool open (std::shared_ptr<HostProcess> h, juce::String& error)
            {
                host = std::move (h);
                fd = ::memfd_create ("dawd-plugin-sandbox", MFD_CLOEXEC);

                if (fd < 0 || ::ftruncate (fd, (off_t) shmSize) != 0)
                {
                    error = "plugin sandbox: no shared memory (" + juce::String (std::strerror (errno)) + ")";
                    return false;
                }

                auto* mem = ::mmap (nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

                if (mem == MAP_FAILED)
                {
                    error = "plugin sandbox: mmap failed (" + juce::String (std::strerror (errno)) + ")";
                    return false;
                }

                shm = new (mem) Shm();
                shm->magic = shmMagic;
                shm->version = shmVersion;

                if (! host->sendFd (fd))
                {
                    if (host->exited (0))
                        host->markDead ("exited");

                    error = "plugin sandbox: host does not take the instance (" + juce::String (std::strerror (errno)) + ")";
                    return false;
                }

                // Host meldet sich, sobald er den Speicher hat
                auto deadline = nowNs() + 10LL * 1000000000LL;

                while (shm->hostReady.load (std::memory_order_acquire) == 0)
                {
                    if (host->exited (0))
                    {
                        host->markDead ("exited during start");
                        error = "plugin sandbox: host exited during start";
                        return false;
                    }

                    if (nowNs() > deadline)
                    {
                        host->kill ("not responding: no start within 10 s");
                        error = "plugin sandbox: host did not start within 10 s";
                        return false;
                    }

                    futexWait (shm->hostReady, 0, 50LL * 1000000LL);
                }

                return true;
            }

            // Steuer-Aufruf, synchron, aus jedem Thread (nicht dem Audio-Thread)
            bool call (Op op, const juce::var& args, const void* blob, size_t blobBytes,
                       juce::var& reply, juce::MemoryBlock* blobOut, Wait w, juce::String& error)
            {
                std::lock_guard<std::mutex> l (controlLock);

                if (dead())
                {
                    error = "plugin host is not running";
                    return false;
                }

                struct InCall
                {
                    explicit InCall (std::atomic<bool>& f) : flag (f)   { flag = true; }
                    ~InCall()                                         { flag = false; }
                    std::atomic<bool>& flag;
                } waiting (inCall);
                const auto deadline = nowNs() + (int64_t) timeoutMs (w) * 1000000LL;

                // Eine leichte Anfrage davor lief in die Frist und ist noch offen:
                // leichte geben sofort auf, die anderen warten darauf
                if (shm->ctlDone.v.load (std::memory_order_acquire) != ctlSeq.load())
                {
                    if (w == Wait::light || w == Wait::quit)
                    {
                        error = "plugin host is busy";
                        return false;
                    }

                    // der Host hatte seit dieser Anfrage hoechstens 10 s (wie der Waechter)
                    const auto since = sentNs.load();
                    const auto catchUp = since != 0 ? std::min<int64_t> (deadline, since + pendingKillNs) : deadline;

                    if (! waitDone (ctlSeq.load(), catchUp, w, error))
                        return false;
                }

                shm->ctl.op = op;

                if (! writeControl (shm, args, blob, blobBytes))
                {
                    error = "plugin sandbox: data too large (" + juce::String ((juce::int64) blobBytes) + " bytes)";
                    return false;
                }

                const auto seq = ctlSeq.load() + 1;
                ctlSeq = seq;
                sentNs = nowNs();
                shm->ctlRequest.v.store (seq, std::memory_order_release);
                futexWake (shm->ctlRequest.v);

                if (! waitDone (seq, deadline, w, error))
                    return false;

                sentNs = 0;
                reply = readControl (shm, blobOut);

                if (shm->ctl.ok == 0)
                {
                    error = reply.getProperty ("error", "plugin host error").toString();
                    return false;
                }

                return true;
            }

            bool waitDone (uint32_t seq, int64_t deadline, Wait w, juce::String& error)
            {
                // in Scheiben warten: ein toter Host faellt sofort auf
                while (! waitUntilEquals (shm->ctlDone.v, seq, std::min<int64_t> (deadline, nowNs() + 100LL * 1000000LL)))
                {
                    if (host->exited (0))
                    {
                        host->markDead ("exited");
                        error = "plugin host exited";
                        return false;
                    }

                    if (nowNs() >= deadline)
                    {
                        if (w == Wait::light || w == Wait::quit)
                        {
                            // Host darf nachreichen; der Waechter beendet ihn nach 10 s
                            error = "plugin host did not answer within " + juce::String (timeoutMs (w) / 1000) + " s";
                            return false;
                        }

                        host->kill ("not responding: no answer within " + juce::String (timeoutMs (w) / 1000) + " s");
                        error = "plugin host did not answer";
                        return false;
                    }
                }

                return true;
            }
        };

        //==============================================================================
        // Prozessweit: Gruppen (je Plugin ein Host), Abstuerze je Plugin,
        // Sperrliste, Einstellung der Gruppierung, Statuszaehler fuer das UI
        struct Registry
        {
            std::mutex lock;
            std::map<juce::String, std::weak_ptr<HostProcess>> groups;
            std::map<juce::String, std::vector<int64_t>> crashes;
            std::set<juce::String> blocked;
            std::map<juce::String, juce::String> modes;
            juce::File modesFile;
            std::atomic<uint32_t> statusSeq { 0 };
            std::function<void (juce::AudioPluginInstance&)> lostHandler;
        };

        Registry& registry()
        {
            static Registry r;
            return r;
        }

        void statusChanged()   { registry().statusSeq.fetch_add (1); }

        // Plugin, wie es die Einstellung und die Sperrliste kennen
        juce::String hostingIdentity (const juce::PluginDescription& d)
        {
            return d.pluginFormatName + ":" + d.name + ":" + juce::String::toHexString (d.uniqueId);
        }

        juce::String modeFor (const juce::String& identity)
        {
            auto& r = registry();
            const std::lock_guard<std::mutex> l (r.lock);
            auto it = r.modes.find (identity);
            return it != r.modes.end() ? it->second : juce::String ("plugin");
        }

        void noteCrash (const juce::String& identity)
        {
            auto& r = registry();

            {
                const std::lock_guard<std::mutex> l (r.lock);
                auto& times = r.crashes[identity];
                const auto now = nowNs();
                times.erase (std::remove_if (times.begin(), times.end(), [now] (int64_t t) { return now - t > crashWindowNs; }),
                             times.end());
                times.push_back (now);

                if ((int) times.size() >= crashLimit && r.blocked.insert (identity).second)
                    std::cerr << "[sandbox] " << identity << " crashed " << (int) times.size()
                              << " times within 60 s: not reloaded until reloadPlugin\n";
            }

            statusChanged();
        }

        bool isBlocked (const juce::String& identity)
        {
            auto& r = registry();
            const std::lock_guard<std::mutex> l (r.lock);
            return r.blocked.count (identity) > 0;
        }

        void unblock (const juce::String& identity)
        {
            auto& r = registry();

            {
                const std::lock_guard<std::mutex> l (r.lock);
                r.blocked.erase (identity);
                r.crashes.erase (identity);
            }

            statusChanged();
        }

        // Host fuer eine neue Instanz: "plugin" = der laufende Host dieses
        // Plugins (sonst ein neuer), "single" = immer ein eigener
        std::shared_ptr<HostProcess> acquireHost (const juce::String& identity, const juce::String& group, juce::String& error)
        {
            auto& r = registry();
            const std::lock_guard<std::mutex> l (r.lock);

            if (group == "plugin")
                if (auto h = r.groups[identity].lock(); h != nullptr && ! h->dead && ! h->exited (0))
                    return h;

            auto h = std::make_shared<HostProcess>();
            h->identity = identity;
            h->group = group;

            if (! h->launch (error))
                return {};

            if (group == "plugin")
                r.groups[identity] = h;

            return h;
        }

        juce::var loadArgs (const juce::PluginDescription& desc, double rate, int block)
        {
            auto args = new juce::DynamicObject();

            if (auto xml = desc.createXml())
                args->setProperty ("desc", xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

            args->setProperty ("rate", rate);
            args->setProperty ("block", block);
            return juce::var (args);
        }

        juce::AudioChannelSet channelSetFrom (const juce::var& types)
        {
            juce::Array<juce::AudioChannelSet::ChannelType> t;

            if (auto* a = types.getArray())
                for (auto& v : *a)
                    t.add ((juce::AudioChannelSet::ChannelType) (int) v);

            return juce::AudioChannelSet::channelSetWithChannels (t);
        }
    }

    //==============================================================================
    // Stellvertreter: fuer die Engine ein AudioPluginInstance wie jedes andere
    class RemotePluginInstance : public juce::AudioPluginInstance,
                                 private juce::Timer
    {
    public:
        struct RemoteParameter : public HostedParameter
        {
            RemoteParameter (RemotePluginInstance& o, int i, const juce::var& p)
                : owner (o), index (i),
                  id (p.getProperty ("id", juce::String (i)).toString()),
                  defaultValue ((float) p.getProperty ("default", 0.0)),
                  steps ((int) p.getProperty ("steps", AudioProcessor::getDefaultNumParameterSteps())),
                  discrete ((bool) p.getProperty ("discrete", false)),
                  boolean ((bool) p.getProperty ("boolean", false)),
                  automatable ((bool) p.getProperty ("automatable", true)),
                  meta ((bool) p.getProperty ("meta", false)),
                  inverted ((bool) p.getProperty ("inverted", false)),
                  value ((float) p.getProperty ("value", 0.0)),
                  name (p.getProperty ("name", {}).toString()),
                  label (p.getProperty ("label", {}).toString())
            {
            }

            float getValue() const override             { return value.load(); }

            // Aus jedem Thread: Wert in den Speicher, der Host uebernimmt ihn vor
            // dem naechsten Block oder Steuer-Aufruf
            void setValue (float v) override
            {
                value = v;
                owner.pushParameter (index, v);
            }

            float getDefaultValue() const override      { return defaultValue; }
            juce::String getName (int maxLen) const override
            {
                const juce::SpinLock::ScopedLockType l (textLock);
                return name.substring (0, maxLen);
            }

            juce::String getLabel() const override
            {
                const juce::SpinLock::ScopedLockType l (textLock);
                return label;
            }

            // Neuer Name/Einheit vom Host (dynamische Plugins, z. B. Surge XT
            // Effects nach dem ersten Block); true = geaendert
            bool setInfo (const juce::String& n, const juce::String& l)
            {
                const juce::SpinLock::ScopedLockType sl (textLock);

                if (n == name && l == label)
                    return false;

                name = n;
                label = l;
                return true;
            }
            int getNumSteps() const override            { return steps; }
            bool isDiscrete() const override            { return discrete; }
            bool isBoolean() const override             { return boolean; }
            bool isAutomatable() const override         { return automatable; }
            bool isMetaParameter() const override       { return meta; }
            bool isOrientationInverted() const override { return inverted; }
            juce::String getParameterID() const override { return id; }

            juce::String getText (float v, int maxLen) const override
            {
                return owner.parameterText (index, v).substring (0, maxLen);
            }

            float getValueForText (const juce::String& text) const override
            {
                return owner.parameterValueForText (index, text);
            }

            // Wert vom Host (nach setState/Programm, aus Editor oder Plugin):
            // ohne Rueckweg in den Speicher
            void updateFromHost (float v)
            {
                if (value.exchange (v) != v)
                    sendValueChangedMessageToListeners (v);
            }

            RemotePluginInstance& owner;
            const int index;
            const juce::String id;
            const float defaultValue;
            const int steps;
            const bool discrete, boolean, automatable, meta, inverted;
            std::atomic<float> value;

        private:
            juce::SpinLock textLock;
            juce::String name, label;
        };

        RemotePluginInstance (std::unique_ptr<Channel> c, const juce::var& info,
                              const juce::PluginDescription& req, const juce::String& ident)
            : AudioPluginInstance (busesFrom (info)), requested (req), identity (ident)
        {
            if (auto xml = juce::parseXML (info.getProperty ("desc", {}).toString()))
                description.loadFromXml (*xml);

            name = info.getProperty ("name", description.name).toString();
            tailSeconds = (double) info.getProperty ("tail", 0.0);
            midiIn = (bool) info.getProperty ("acceptsMidi", false);
            midiOut = (bool) info.getProperty ("producesMidi", false);
            midiEffect = (bool) info.getProperty ("isMidiEffect", false);
            setLatencySamples ((int) info.getProperty ("latency", 0));
            readPrograms (info);

            if (auto* ps = info.getProperty ("params", {}).getArray())
                for (int i = 0; i < ps->size(); ++i)
                    addHostedParameter (std::make_unique<RemoteParameter> (*this, i, (*ps)[i]));

            c->shm->rt.latency = getLatencySamples();
            live.store (c.get());
            channels.push_back (std::move (c));

            const auto n = (size_t) juce::jmin (getParameters().size(), maxParams);
            seenOut.resize (n);
            dirty = std::vector<std::atomic<bool>> (n);

            // Meldungen des Hosts abholen und auf Absturz/Haenger pruefen, auch
            // wenn keine Op kommt (syncFromHost, checkHost)
            startTimer (50);
        }

        ~RemotePluginInstance() override
        {
            stopTimer();
            live.store (nullptr);
            channels.clear();   // je Kanal Op quit; der letzte Kanal eines Hosts beendet ihn
        }

        // Busse wie im Host (nach dessen enableAllBuses)
        static BusesProperties busesFrom (const juce::var& info)
        {
            BusesProperties props;

            for (auto isInput : { true, false })
                if (auto* a = info.getProperty (isInput ? "inputs" : "outputs", {}).getArray())
                    for (auto& b : *a)
                        props.addBus (isInput, b.getProperty ("name", {}).toString(),
                                      channelSetFrom (b.getProperty ("types", {})),
                                      (bool) b.getProperty ("enabled", true));

            return props;
        }

        //==============================================================================
        const juce::String getName() const override             { return name; }
        void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }
        double getTailLengthSeconds() const override             { return tailSeconds; }
        bool acceptsMidi() const override                        { return midiIn; }
        bool producesMidi() const override                       { return midiOut; }
        bool isMidiEffect() const override                       { return midiEffect; }

        // Der Editor laeuft im Host-Prozess in dessen eigenem Fenster
        // (openWindow); in dawd gibt es keinen. Erst auf Anfrage beim Host
        // (PluginWindows fragt nach der Pruefung auf ein Display): JUCE legt
        // dafuer bei VST3 eine View an, LSP beendet ohne Display den Prozess
        bool hasEditor() const override
        {
            if (! editorKnown)
            {
                juce::var reply;
                juce::String err;

                if (! ch()->call (Op::hasEditor, {}, nullptr, 0, reply, nullptr, Wait::light, err))
                    return false;

                editor = (bool) reply.getProperty ("hasEditor", false);
                editorKnown = true;
            }

            return editor;
        }

        juce::AudioProcessorEditor* createEditor() override      { return nullptr; }

        // Bus-Layout (z. B. Sidechain-Bus ein/aus): der Host prueft und
        // uebernimmt es; die Engine gibt das Plugin vorher frei und bereitet es
        // danach neu vor (ExternalPlugin::setBusesLayout)
        bool isBusesLayoutSupported (const BusesLayout& l) const override
        {
            if (l == getBusesLayout())
                return true;

            juce::var reply;
            juce::String err;

            if (! ch()->call (Op::setLayout, layoutArgs (l, false), nullptr, 0, reply, nullptr, Wait::light, err))
            {
                std::cerr << "[sandbox] layout " << name << ": " << err << "\n";
                return false;
            }

            return (bool) reply.getProperty ("supported", false);
        }

        void processorLayoutsChanged() override
        {
            juce::var reply;
            juce::String err;

            if (! ch()->call (Op::setLayout, layoutArgs (getBusesLayout(), true), nullptr, 0, reply, nullptr, Wait::heavy, err)
                || ! (bool) reply.getProperty ("supported", false))
                std::cerr << "[sandbox] layout " << name << " not applied in the plugin host: " << err << "\n";
        }

        //==============================================================================
        void prepareToPlay (double rate, int block) override
        {
            setRateAndBufferSizeDetails (rate, block);
            midiScratch.ensureSize (16384);
            prepared = true;

            juce::var reply;
            juce::String err;

            if (ch()->call (Op::prepare, prepareArgs(), nullptr, 0, reply, nullptr, Wait::heavy, err))
                updateLatency ((int) reply.getProperty ("latency", getLatencySamples()));
            else
                std::cerr << "[sandbox] prepareToPlay " << name << ": " << err << "\n";
        }

        void releaseResources() override
        {
            prepared = false;
            juce::var reply;
            juce::String err;
            ch()->call (Op::release, {}, nullptr, 0, reply, nullptr, Wait::heavy, err);
        }

        // im naechsten Block, in Reihenfolge mit dem Audio (die Engine ruft
        // reset auch aus dem Audio-Thread)
        void reset() override                                     { pendingReset = true; }

        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override  { process (b, m, false); }
        void processBlockBypassed (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override { process (b, m, true); }
        using AudioPluginInstance::processBlock;
        using AudioPluginInstance::processBlockBypassed;

        //==============================================================================
        int getNumPrograms() override                             { return programNames.size(); }
        int getCurrentProgram() override                          { return currentProgram; }
        const juce::String getProgramName (int i) override        { return programNames[i]; }
        void changeProgramName (int index, const juce::String& newName) override
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("index", index);
            args->setProperty ("name", newName);
            juce::var reply;
            juce::String err;

            if (ch()->call (Op::setProgramName, juce::var (args), nullptr, 0, reply, nullptr, Wait::light, err))
                readProgramNames (reply);
            else
                std::cerr << "[sandbox] changeProgramName " << name << ": " << err << "\n";
        }

        void setCurrentProgram (int index) override
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("index", index);
            juce::var reply;
            juce::String err;

            if (ch()->call (Op::setProgram, juce::var (args), nullptr, 0, reply, nullptr, Wait::heavy, err))
                applyHostValues (reply);
            else
                std::cerr << "[sandbox] setCurrentProgram " << name << ": " << err << "\n";
        }

        // Zustand ueber den Steuerkanal; der letzte bekannte bleibt im
        // Stellvertreter — ist der Host weg, liefert er den statt nichts
        // (sonst loeschte das Speichern den Zustand im Edit), und ein neuer
        // Host startet mit ihm (Phase 3)
        void getStateInformation (juce::MemoryBlock& mb) override
        {
            juce::var reply;
            juce::String err;
            juce::MemoryBlock blob;

            if (ch()->call (Op::getState, {}, nullptr, 0, reply, &blob, Wait::heavy, err))
            {
                const std::lock_guard<std::mutex> l (stateLock);
                lastState = blob;
            }
            else
            {
                std::cerr << "[sandbox] getState " << name << ": " << err << " (last known state used)\n";
            }

            const std::lock_guard<std::mutex> l (stateLock);
            mb = lastState;
        }

        void setStateInformation (const void* data, int size) override
        {
            {
                const std::lock_guard<std::mutex> l (stateLock);
                lastState.replaceAll (data, (size_t) juce::jmax (0, size));
            }

            juce::var reply;
            juce::String err;

            if (ch()->call (Op::setState, {}, data, (size_t) juce::jmax (0, size), reply, nullptr, Wait::heavy, err))
                applyHostValues (reply);
            else
                std::cerr << "[sandbox] setState " << name << ": " << err << "\n";
        }

        //==============================================================================
        void pushParameter (int index, float v)
        {
            if (index >= maxParams)
                return;

            auto* c = ch();

            // Host weg: nach dem Neustart nachreichen
            if (c->dead() && juce::isPositiveAndBelow (index, (int) dirty.size()))
                dirty[(size_t) index] = true;

            c->shm->paramValue[index].store (v, std::memory_order_relaxed);
            c->shm->paramSeq[index].fetch_add (1, std::memory_order_release);
        }

        juce::String parameterText (int index, float v)
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("index", index);
            args->setProperty ("value", v);
            juce::var reply;
            juce::String err;

            if (ch()->call (Op::getText, juce::var (args), nullptr, 0, reply, nullptr, Wait::light, err))
                return reply.getProperty ("text", {}).toString();

            return juce::String (v, 2);
        }

        float parameterValueForText (int index, const juce::String& text)
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("index", index);
            args->setProperty ("text", text);
            juce::var reply;
            juce::String err;

            if (ch()->call (Op::getValueForText, juce::var (args), nullptr, 0, reply, nullptr, Wait::light, err))
                return (float) reply.getProperty ("value", 0.0);

            return text.getFloatValue();
        }

        juce::int64 failureCount() const     { return failures.load(); }

        bool running() const                 { return ! ch()->dead(); }

        // running | restarting | blocked | failed
        juce::String state() const
        {
            if (running())
                return "running";

            if (isBlocked (identity))
                return "blocked";

            return restartError.isNotEmpty() ? "failed" : "restarting";
        }

        // Hinweis fuer Nutzer und Agent (Englisch, wie alle Antworten)
        juce::String message() const
        {
            const auto s = state();

            if (s == "blocked")
                return "the plugin host crashed " + juce::String (crashLimit) + " times within "
                       + juce::String (crashWindowNs / 1000000000LL) + " s (last: " + lastReason
                       + "); the plugin stays silent until you reload it (reloadPlugin)";

            if (s == "failed")
                return "the plugin could not be reloaded after its host stopped (" + restartError
                       + "); it stays silent until you reload it (reloadPlugin)";

            if (s == "restarting")
                return "the plugin host stopped (" + lastReason + "); reloading the plugin with its last known state";

            if (crashes > 0)
                return "the plugin host stopped (" + lastReason + ") and the plugin was reloaded with its last known "
                       "state; changes made in its window since then are lost";

            return {};
        }

        juce::var info() const
        {
            auto* c = ch();
            auto o = new juce::DynamicObject();
            o->setProperty ("pid", c->host != nullptr ? (int) c->host->pid : -1);
            o->setProperty ("missedBlocks", (juce::int64) failures.load());
            o->setProperty ("failed", c->dead());
            o->setProperty ("group", c->host != nullptr ? c->host->group : juce::String());
            o->setProperty ("state", state());
            o->setProperty ("crashes", crashes);
            o->setProperty ("restarts", restarts);

            if (auto m = message(); m.isNotEmpty())
                o->setProperty ("message", m);

            return juce::var (o);
        }

        // Message-Thread: Host noch da? Haengt er (Waechter)? Weg: Ausfall
        // verbuchen, Fenster in dawd zu, neu laden — ausser gesperrt
        void checkHost()
        {
            auto* c = ch();

            if (c == nullptr)
                return;

            if (! c->dead())
            {
                if (c->host->exited (0))
                    c->host->markDead ("exited");
                else
                    watchdog (*c);
            }

            if (! c->dead())
                return;

            if (! c->lost)
                hostLost (*c);

            if (restarting || restartError.isNotEmpty() || isBlocked (identity))
                return;

            juce::String err;
            restart (err);
        }

        // reloadPlugin: Sperre fuer dieses Plugin aufheben, neu laden, wenn
        // der Host nicht laeuft ("" = laeuft)
        juce::String reload()
        {
            unblock (identity);
            restartError = {};
            auto* c = ch();

            if (! c->dead())
                return {};

            if (! c->lost)
                hostLost (*c);

            juce::String err;
            return restart (err) ? juce::String() : (err.isNotEmpty() ? err : juce::String ("plugin host stopped again"));
        }

        // Auf dem Message-Thread: was der Host gemeldet hat — Werte aus dem
        // Editor oder dem Plugin selbst, Gesten, neue Namen/Programme. Werte
        // gehen wie im Prozess an die Listener (ExternalAutomatableParameter,
        // ProcessorChangedManager): Automation, Undo und Zustand sehen sie.
        // Ein Wert, den dawd gesetzt und der Host noch nicht uebernommen hat,
        // gewinnt (der Host uebernimmt ihn gleich) — wie die Automation im
        // Prozess.
        void syncFromHost()
        {
            auto& s = *ch()->shm;
            const auto ev = s.hostEvents.v.load (std::memory_order_acquire);

            if (ev == seenEvents)
                return;

            seenEvents = ev;

            if (auto i = s.infoSeq.load (std::memory_order_acquire); i != seenInfo)
            {
                seenInfo = i;
                refreshInfo();
            }

            auto& ps = getParameters();

            for (int i = 0; i < (int) seenOut.size(); ++i)
            {
                auto* rp = static_cast<RemoteParameter*> (ps.getUnchecked (i));
                auto& seen = seenOut[(size_t) i];
                const auto b = s.gestureBegin[i].load (std::memory_order_acquire);
                const auto v = s.outSeq[i].load (std::memory_order_acquire);
                const auto e = s.gestureEnd[i].load (std::memory_order_acquire);

                if (b != seen.begin)
                {
                    seen.begin = b;
                    rp->beginChangeGesture();
                }

                if (v != seen.value)
                {
                    seen.value = v;

                    if (! dawdValuePending (i) && ! reportOutdated (i))
                        rp->updateFromHost (s.outValue[i].load (std::memory_order_relaxed));
                }

                if (e != seen.end)
                {
                    seen.end = e;
                    rp->endChangeGesture();
                }
            }
        }

        //==============================================================================
        // Editor-Fenster im Host (PluginWindows: RemoteWindow)
        juce::String openWindow (const juce::String& title, juce::Point<int> at)
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("title", title);
            args->setProperty ("x", at.x);
            args->setProperty ("y", at.y);
            juce::var reply;
            juce::String err;
            auto* c = ch();

            // fruehere Meldungen gelten nicht fuer dieses Fenster
            windowOuter.reset();
            seenMoves = c->shm->windowMoves.load (std::memory_order_acquire);
            seenCloseRequests = c->shm->windowCloseRequests.load (std::memory_order_acquire);

            if (! c->call (Op::openWindow, juce::var (args), nullptr, 0, reply, nullptr, Wait::heavy, err))
                return err;

            return {};
        }

        void moveWindow (juce::Point<int> at)
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("x", at.x);
            args->setProperty ("y", at.y);
            juce::var reply;
            juce::String err;
            // die alte Lage gilt nicht mehr; die neue meldet der Host
            windowOuter.reset();
            ch()->call (Op::moveWindow, juce::var (args), nullptr, 0, reply, nullptr, Wait::light, err);
        }

        void closeWindow()
        {
            juce::var reply;
            juce::String err;

            if (! ch()->dead())
                ch()->call (Op::closeWindow, {}, nullptr, 0, reply, nullptr, Wait::heavy, err);
        }

        SandboxWindowEvents pollWindow()
        {
            auto& s = *ch()->shm;
            SandboxWindowEvents ev;

            if (auto m = s.windowMoves.load (std::memory_order_acquire); m != seenMoves)
            {
                seenMoves = m;
                windowOuter = juce::Point<int> (s.windowX.load (std::memory_order_relaxed), s.windowY.load (std::memory_order_relaxed));
            }

            if (auto c = s.windowCloseRequests.load (std::memory_order_acquire); c != seenCloseRequests)
            {
                seenCloseRequests = c;
                ev.closeRequested = true;
            }

            ev.outer = windowOuter;
            return ev;
        }

        // VST3: Controller im Host mit der Komponente abgleichen, Namen und
        // Werte holen (syncSandboxedController)
        bool syncController()
        {
            return running() && refreshInfo (true);
        }

        // --window-sim: Regler im Host drehen wie die Oberflaeche des Plugins;
        // die Meldung kommt ueber den Rueckweg wie aus einem echten Editor
        juce::String editParameter (int index, float v)
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("index", index);
            args->setProperty ("value", v);
            juce::var reply;
            juce::String err;

            if (! ch()->call (Op::editParam, juce::var (args), nullptr, 0, reply, nullptr, Wait::light, err))
                return err;

            syncFromHost();
            return {};
        }

    private:
        Channel* ch() const   { return live.load (std::memory_order_acquire); }

        void timerCallback() override
        {
            checkHost();
            syncFromHost();
        }

        // Haengt der Host? Eine Steuer-Antwort, auf die niemand mehr wartet,
        // 10 s offen, oder ein Echtzeit-Block 5 s im Host: beenden — dann
        // greift der Neustart
        void watchdog (Channel& c)
        {
            const auto now = nowNs();

            if (! c.inCall)
                if (auto sent = c.sentNs.load(); sent != 0 && now - sent > pendingKillNs
                    && c.shm->ctlDone.v.load (std::memory_order_acquire) != c.ctlSeq.load())
                    c.host->kill ("not responding: no answer for " + juce::String (pendingKillNs / 1000000000LL) + " s");

            if (auto sent = c.rtSentNs.load(); sent != 0 && ! c.rtSentNonRealtime && now - sent > rtStuckNs
                && c.shm->rtDone.v.load (std::memory_order_acquire) != c.rtSeq.load())
                c.host->kill ("not responding: audio block stuck for " + juce::String (rtStuckNs / 1000000000LL) + " s");
        }

        // Host weg (Absturz, beendet, haengend beendet): einmal je Kanal
        void hostLost (Channel& c)
        {
            c.lost = true;
            lastReason = c.host->reap();

            if (! c.host->crashNoted.exchange (true))
                noteCrash (identity);

            ++crashes;
            statusChanged();
            std::cerr << "[sandbox] " << name << ": plugin host " << c.host->pid << " lost (" << lastReason << ")\n";

            // Fenster in dawd zu (gehoerte dem toten Host)
            if (auto& h = registry().lostHandler)
                h (*this);
        }

        // Neuer Host (oder der laufende der Gruppe), gleiches Plugin, letzter
        // Zustand, gleiches Bus-Layout, vorbereitet wie zuletzt. Erst wenn alles
        // steht, nimmt der Audio-Thread den neuen Kanal (live). Message-Thread.
        bool restart (juce::String& error)
        {
            const juce::ScopedValueSetter<bool> busy (restarting, true);

            // "dawd" (im Prozess) geht nur ueber neues Laden (Projekt oeffnen)
            const auto group = modeFor (identity) == "single" ? juce::String ("single") : juce::String ("plugin");
            auto host = acquireHost (identity, group, error);

            if (host == nullptr)
            {
                restartError = error;
                statusChanged();
                return false;
            }

            auto c = std::make_unique<Channel>();
            juce::var loaded, reply;
            const double rate = getSampleRate() > 0 ? getSampleRate() : 44100.0;
            const int block = getBlockSize() > 0 ? getBlockSize() : 512;

            auto stoppedAgain = [&]
            {
                // Der neue Host starb beim Laden: zaehlt als Absturz
                if (host->exited (0))
                    host->markDead ("exited");

                if (! host->dead)
                    return false;

                c->lost = true;
                lastReason = host->reap();

                if (! host->crashNoted.exchange (true))
                    noteCrash (identity);

                ++crashes;
                statusChanged();
                return true;
            };

            if (! c->open (host, error)
                || ! c->call (Op::load, loadArgs (requested, rate, block), nullptr, 0, loaded, nullptr, Wait::load, error))
            {
                if (! stoppedAgain())
                    restartError = error;

                statusChanged();
                std::cerr << "[sandbox] " << name << ": reload failed: " << error << "\n";
                return false;
            }

            if (auto* ps = loaded.getProperty ("params", {}).getArray(); ps == nullptr || ps->size() != getParameters().size())
            {
                restartError = error = "the plugin has a different parameter list now";
                statusChanged();
                return false;
            }

            juce::String err;

            if (! c->call (Op::setLayout, layoutArgs (getBusesLayout(), true), nullptr, 0, reply, nullptr, Wait::heavy, err))
                std::cerr << "[sandbox] " << name << ": layout after reload: " << err << "\n";

            juce::MemoryBlock state;

            {
                const std::lock_guard<std::mutex> l (stateLock);
                state = lastState;
            }

            juce::var stateReply;

            if (state.getSize() > 0
                && ! c->call (Op::setState, {}, state.getData(), state.getSize(), stateReply, nullptr, Wait::heavy, err))
                std::cerr << "[sandbox] " << name << ": state after reload: " << err << "\n";

            if (prepared && ! c->call (Op::prepare, prepareArgs(), nullptr, 0, reply, nullptr, Wait::heavy, err))
                std::cerr << "[sandbox] " << name << ": prepare after reload: " << err << "\n";

            // LV2 meldet wiederhergestellte Patch-Parameter erst aus einem Block
            // (wie ExternalPlugin nach dem Wiederherstellen,
            // passPendingValuesToPlugin): 1 Sample Stille im neuen Host, bevor
            // der Audio-Thread ihn nimmt. Die Werte aus setState waeren davor
            // die Standardwerte — refreshInfo unten liest die richtigen.
            const bool passed = requested.pluginFormatName == "LV2" && prepared && passOneSample (*c);

            if (stoppedAgain())
            {
                error = "plugin host stopped again (" + lastReason + ")";
                statusChanged();
                return false;
            }

            // Ab jetzt rechnet der neue Host; der alte Kanal bleibt bis zum Ende
            // des Stellvertreters stehen (der Audio-Thread kann ihn noch halten)
            c->shm->rt.latency = getLatencySamples();
            auto* fresh = c.get();
            channels.push_back (std::move (c));
            live.store (fresh, std::memory_order_release);

            // Rueckweg: der neue Speicher zaehlt von vorn
            seenEvents = seenInfo = seenMoves = seenCloseRequests = 0;
            std::fill (seenOut.begin(), seenOut.end(), SeenOut {});
            windowOuter.reset();

            // Werte, die dawd setzte, waehrend der Host weg war
            auto& ps = getParameters();

            for (size_t i = 0; i < dirty.size(); ++i)
                if (dirty[i].exchange (false))
                    pushParameter ((int) i, ps.getUnchecked ((int) i)->getValue());

            applyHostValues (passed ? juce::var() : stateReply);
            updateLatency ((int) loaded.getProperty ("latency", getLatencySamples()));
            restartError = {};
            ++restarts;
            statusChanged();
            std::cerr << "[sandbox] " << name << " reloaded in plugin host " << host->pid << "\n";
            refreshInfo();
            return true;
        }

        // Ein Block mit 1 Sample Stille ueber einen Kanal, den der Audio-Thread
        // (noch) nicht nimmt — Message-Thread, wartet wie ein schwerer Aufruf
        bool passOneSample (Channel& c)
        {
            juce::AudioBuffer<float> buffer (juce::jlimit (1, maxChannels, juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels())), 1);
            buffer.clear();
            juce::MidiBuffer in, out;
            return roundTrip (c, buffer, in, out, 0, 1, {}, false, false, true);
        }

        juce::var prepareArgs() const
        {
            auto args = new juce::DynamicObject();
            args->setProperty ("rate", getSampleRate());
            args->setProperty ("block", getBlockSize());
            args->setProperty ("nonRealtime", isNonRealtime());
            return juce::var (args);
        }

        // Hat dawd diesem Parameter einen Wert gegeben, den der Host noch nicht
        // uebernommen hat?
        bool dawdValuePending (int i) const
        {
            auto& s = *ch()->shm;
            return i < maxParams && s.paramSeq[i].load (std::memory_order_acquire) != s.paramApplied[i].load (std::memory_order_acquire);
        }

        // Meldete der Host den Wert, bevor er den letzten dawd-Wert dieses
        // Parameters uebernommen hatte? Dann ist sie aelter als der dawd-Wert.
        bool reportOutdated (int i) const
        {
            auto& s = *ch()->shm;
            return i < maxParams && s.outBase[i].load (std::memory_order_relaxed) != s.paramSeq[i].load (std::memory_order_acquire);
        }

        static juce::var layoutArgs (const BusesLayout& l, bool apply)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("inputs", layoutVar (l, true));
            o->setProperty ("outputs", layoutVar (l, false));
            o->setProperty ("apply", apply);
            return juce::var (o);
        }

        // true = Liste geaendert
        bool readProgramNames (const juce::var& reply)
        {
            juce::StringArray names;

            if (auto* a = reply.getProperty ("programs", {}).getArray())
                for (auto& p : *a)
                    names.add (p.toString());

            if (names == programNames)
                return false;

            programNames = names;
            return true;
        }

        void readPrograms (const juce::var& info)
        {
            readProgramNames (info);
            currentProgram = (int) info.getProperty ("program", 0);
        }

        // Namen, Einheiten, Werte, Programme neu vom Host (er meldete
        // parameterInfoChanged/programChanged) — wie im Prozess danach
        // updateHostDisplay, die Engine liest Namen und Werte neu.
        // withController: der Host gleicht vorher den VST3-Controller mit der
        // Komponente ab (syncPluginParameters)
        bool refreshInfo (bool withController = false)
        {
            juce::var reply;
            juce::String err;
            auto args = new juce::DynamicObject();

            if (withController)
                args->setProperty ("syncController", true);

            if (! ch()->call (Op::info, juce::var (args), nullptr, 0, reply, nullptr, Wait::light, err))
            {
                std::cerr << "[sandbox] info " << name << ": " << err << "\n";
                return false;
            }

            bool changed = false;
            auto* names = reply.getProperty ("names", {}).getArray();
            auto* labels = reply.getProperty ("labels", {}).getArray();
            auto& ps = getParameters();

            if (names != nullptr && labels != nullptr)
                for (int i = 0; i < juce::jmin (names->size(), labels->size(), ps.size()); ++i)
                    changed = static_cast<RemoteParameter*> (ps.getUnchecked (i))->setInfo ((*names)[i].toString(), (*labels)[i].toString()) || changed;

            const int program = currentProgram;
            changed = readProgramNames (reply) || changed;
            applyHostValues (reply);

            if (changed || program != currentProgram)
                updateHostDisplay (ChangeDetails().withParameterInfoChanged (true).withProgramChanged (true));

            return true;
        }

        void applyHostValues (const juce::var& reply)
        {
            if (auto* vs = reply.getProperty ("values", {}).getArray())
            {
                auto& ps = getParameters();

                for (int i = 0; i < juce::jmin (vs->size(), ps.size()); ++i)
                    if (auto* rp = dynamic_cast<RemoteParameter*> (ps.getUnchecked (i)); rp != nullptr && ! dawdValuePending (i))
                        rp->updateFromHost ((float) (*vs)[i]);
            }

            if (reply.hasProperty ("program"))
                currentProgram = (int) reply.getProperty ("program", 0);

            if (reply.hasProperty ("latency"))
                updateLatency ((int) reply.getProperty ("latency", 0));
        }

        void updateLatency (int l)
        {
            if (l != getLatencySamples())
                setLatencySamples (l);
        }

        // Ein Block hin und zurueck (Audio in place, MIDI ersetzt). Bloecke
        // ueber maxFrames gehen in Teilen.
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool bypassed)
        {
            const int n = buffer.getNumSamples();

            if (n <= 0)
                return;

            auto* c = ch();

            if (c == nullptr || c->dead())
            {
                buffer.clear();
                midi.clear();
                failures.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            juce::AudioPlayHead::PositionInfo pos;
            bool hasPos = false;

            if (auto* ph = getPlayHead())
                if (auto p = ph->getPosition())
                {
                    pos = *p;
                    hasPos = true;
                }

            midiScratch.clear();

            // Ein Block vom Message-Thread ausserhalb eines Renders kommt aus
            // ExternalPlugin::passPendingValuesToPlugin (1 Sample, kein Graph):
            // er bringt offene Werte, LV2-Patch-Parameter und Meldungen eines
            // offenen LV2-Editors zum Plugin. Er wartet wie ein schwerer
            // Steuer-Aufruf statt einen Echtzeit-Block lang, und was das Plugin
            // darin meldet, gilt danach sofort — wie im Prozess, wo es im Block
            // selbst an die Listener geht.
            const bool passing = ! isNonRealtime() && juce::MessageManager::existsAndIsCurrentThread();

            for (int start = 0; start < n; start += maxFrames)
            {
                const int len = juce::jmin (maxFrames, n - start);

                if (! roundTrip (*c, buffer, midi, midiScratch, start, len, pos, hasPos, bypassed, passing))
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        buffer.clear (ch, start, len);

                    failures.fetch_add (1, std::memory_order_relaxed);
                }

                if (hasPos)
                    advance (pos, len);
            }

            midi.swapWith (midiScratch);

            if (passing)
                syncFromHost();
        }

        void advance (juce::AudioPlayHead::PositionInfo& pos, int len) const
        {
            const auto sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;

            if (auto t = pos.getTimeInSamples())
                pos.setTimeInSamples (*t + len);

            if (auto t = pos.getTimeInSeconds())
                pos.setTimeInSeconds (*t + len / sr);

            if (auto ppq = pos.getPpqPosition())
                if (auto bpm = pos.getBpm())
                    pos.setPpqPosition (*ppq + len / sr * *bpm / 60.0);
        }

        bool roundTrip (Channel& c, juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, juce::MidiBuffer& midiOut,
                        int start, int len,
                        const juce::AudioPlayHead::PositionInfo& pos, bool hasPos, bool bypassed, bool passing)
        {
            auto& s = *c.shm;

            // Host noch am letzten Block (Frist verpasst): nicht in seine Daten
            // schreiben, diesen Block auslassen (der Waechter beendet ihn, wenn
            // er dort haengt)
            if (s.rtDone.v.load (std::memory_order_acquire) != c.rtSeq.load (std::memory_order_relaxed))
            {
                if (c.host->exited (0))
                    c.host->markDead ("exited");

                return false;
            }

            const int chans = juce::jmin (buffer.getNumChannels(), maxChannels);
            s.rt.numSamples = len;
            s.rt.numChannels = chans;

            for (int i = 0; i < chans; ++i)
                std::memcpy (s.audio[i], buffer.getReadPointer (i, start), sizeof (float) * (size_t) len);

            s.rt.midiInBytes = writeMidi (s.midiIn, midiBytes, midi, start, len, 0);
            s.rt.flags = (isNonRealtime() ? rtNonRealtime : 0u) | (bypassed ? rtBypassed : 0u)
                       | (pendingReset.exchange (false) ? rtReset : 0u) | (hasPos ? rtHasPosition : 0u);

            if (hasPos)
                s.rt.position = pos;

            const auto seq = c.rtSeq.load (std::memory_order_relaxed) + 1;
            c.rtSeq.store (seq, std::memory_order_relaxed);
            c.rtSentNonRealtime = isNonRealtime();
            c.rtSentNs = nowNs();
            s.rtRequest.v.store (seq, std::memory_order_release);
            futexWake (s.rtRequest.v);

            const auto sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;
            const bool longWait = isNonRealtime() || passing;
            const int64_t deadline = nowNs() + (isNonRealtime() ? renderDeadlineNs
                                             : passing ? (int64_t) heavyTimeoutMs * 1000000LL
                                                       : std::max (minLiveDeadlineNs, (int64_t) (len / sr * 1.0e9)));

            // Render (und Block vom Message-Thread): in Scheiben warten, ein
            // toter Host faellt nach hoechstens 50 ms auf statt nach der vollen Frist
            const int64_t slice = longWait ? 50LL * 1000000LL : renderDeadlineNs;

            while (! waitUntilEquals (s.rtDone.v, seq, std::min (deadline, nowNs() + slice)))
            {
                if (c.host->exited (0))
                {
                    c.host->markDead ("exited");
                    return false;
                }

                if (nowNs() >= deadline)
                {
                    if (longWait)
                        c.host->kill ("not responding: no block within "
                                      + juce::String ((isNonRealtime() ? renderDeadlineNs / 1000000000LL : heavyTimeoutMs / 1000)) + " s");

                    return false;
                }
            }

            c.rtSentNs = 0;

            for (int i = 0; i < chans; ++i)
                std::memcpy (buffer.getWritePointer (i, start), s.audio[i], sizeof (float) * (size_t) len);

            readMidi (s.midiOut, juce::jmin<uint32_t> (s.rt.midiOutBytes, (uint32_t) midiBytes), midiOut, start);

            if (s.rt.latency != getLatencySamples())
                setLatencySamples (s.rt.latency);

            return true;
        }

        // Kanaele: der aktuelle (live, den der Audio-Thread liest) und die
        // frueherer, toter Hosts (bleiben bis zum Ende stehen)
        std::vector<std::unique_ptr<Channel>> channels;
        std::atomic<Channel*> live { nullptr };

        const juce::PluginDescription requested;   // so neu laden
        const juce::String identity;
        juce::PluginDescription description;
        juce::String name;
        double tailSeconds = 0.0;
        bool midiIn = false, midiOut = false, midiEffect = false;
        mutable bool editor = false, editorKnown = false;
        juce::StringArray programNames;
        std::atomic<int> currentProgram { 0 };
        std::atomic<bool> pendingReset { false };
        std::atomic<bool> prepared { false };
        std::atomic<juce::int64> failures { 0 };
        juce::MidiBuffer midiScratch;
        std::mutex stateLock;
        juce::MemoryBlock lastState;
        std::vector<std::atomic<bool>> dirty;      // gesetzt, waehrend der Host weg war

        // Absturz und Neustart (Message-Thread)
        int crashes = 0, restarts = 0;
        juce::String lastReason, restartError;
        bool restarting = false;

        // Rueckweg (nur Message-Thread): zuletzt gesehene Zaehler des Hosts
        struct SeenOut { uint32_t begin = 0, value = 0, end = 0; };
        std::vector<SeenOut> seenOut;
        uint32_t seenEvents = 0, seenInfo = 0, seenMoves = 0, seenCloseRequests = 0;
        std::optional<juce::Point<int>> windowOuter;
    };

    //==============================================================================
    namespace
    {
        std::unique_ptr<juce::AudioPluginInstance> createSandboxed (const juce::PluginDescription& desc, const juce::String& group,
                                                                    double rate, int block, juce::String& error)
        {
            const auto identity = hostingIdentity (desc);
            auto host = acquireHost (identity, group, error);

            if (host == nullptr)
                return {};

            auto c = std::make_unique<Channel>();
            juce::var info;

            if (! c->open (host, error)
                || ! c->call (Op::load, loadArgs (desc, rate, block), nullptr, 0, info, nullptr, Wait::load, error))
            {
                error = "plugin sandbox: " + error;
                return {};
            }

            std::cerr << "[sandbox] " << desc.name << " in plugin host " << host->pid << " (" << group << ")\n";
            return std::make_unique<RemotePluginInstance> (std::move (c), info, desc, identity);
        }

        RemotePluginInstance* remoteOf (te::Plugin& p)
        {
            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
                return dynamic_cast<RemotePluginInstance*> (ep->getAudioPluginInstance());

            return nullptr;
        }

        RemotePluginInstance* remoteOf (juce::AudioPluginInstance& pi)
        {
            return dynamic_cast<RemotePluginInstance*> (&pi);
        }

        // Formate, die in einen Host-Prozess koennen (LV2 seit Phase 4)
        bool sandboxable (const juce::PluginDescription& d)
        {
            return d.pluginFormatName == "VST3" || d.pluginFormatName == "LV2";
        }

        bool sandboxInstalled = false;
    }

    bool pluginSandboxRequested()
    {
        // Standard an (nach Phase 3, Decision); UNIDAW_PLUGIN_SANDBOX=0 = wie vorher
        return juce::SystemStats::getEnvironmentVariable ("UNIDAW_PLUGIN_SANDBOX", {}) != "0";
    }

    void installPluginSandbox (te::Engine& engine)
    {
        auto& pm = engine.getPluginManager();
        auto inProcess = pm.createPluginInstance;

        pm.createPluginInstance = [inProcess] (const juce::PluginDescription& desc, double rate, int block,
                                               juce::String& error) -> std::unique_ptr<juce::AudioPluginInstance>
        {
            // VST3 und LV2 in einen Host-Prozess (Gruppierung laut Einstellung,
            // "dawd" = im Prozess); alles andere wie bisher
            if (sandboxable (desc))
                if (auto mode = modeFor (hostingIdentity (desc)); mode != "dawd")
                    return createSandboxed (desc, mode, rate, block, error);

            return inProcess (desc, rate, block, error);
        };

        sandboxInstalled = true;
        std::cerr << "[sandbox] VST3 and LV2 plugins run in plugin host processes (UNIDAW_PLUGIN_SANDBOX=0 turns it off)\n";
    }

    void setPluginHostingFile (const juce::File& f)
    {
        auto& r = registry();
        const std::lock_guard<std::mutex> l (r.lock);
        r.modesFile = f;
        r.modes.clear();

        const auto json = f.existsAsFile() ? juce::JSON::parse (f) : juce::var();

        if (auto* o = json.getDynamicObject())
            for (auto& p : o->getProperties())
                if (auto m = p.value.toString(); m == "plugin" || m == "single" || m == "dawd")
                    r.modes[p.name.toString()] = m;
    }

    juce::String pluginHostingMode (te::Plugin& p)
    {
        auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

        if (! sandboxInstalled || ep == nullptr || ! sandboxable (ep->desc))
            return {};

        return modeFor (hostingIdentity (ep->desc));
    }

    juce::String setPluginHostingMode (te::Plugin& p, const juce::String& mode)
    {
        auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

        if (ep == nullptr)
            return "no plugin host for built-in plugins: " + p.getName();

        if (! sandboxable (ep->desc))
            return "only VST3 and LV2 plugins run in a plugin host: " + p.getName() + " is " + ep->desc.pluginFormatName;

        if (mode != "plugin" && mode != "single" && mode != "dawd")
            return "mode must be plugin, single or dawd";

        auto& r = registry();
        const std::lock_guard<std::mutex> l (r.lock);
        const auto identity = hostingIdentity (ep->desc);

        if (mode == "plugin")
            r.modes.erase (identity);
        else
            r.modes[identity] = mode;

        if (r.modesFile != juce::File())
        {
            auto o = new juce::DynamicObject();

            for (auto& [k, v] : r.modes)
                o->setProperty (k, v);

            r.modesFile.getParentDirectory().createDirectory();
            r.modesFile.replaceWithText (juce::JSON::toString (juce::var (o)));
        }

        return {};
    }

    bool isSandboxed (const juce::AudioPluginInstance* pi)
    {
        return dynamic_cast<const RemotePluginInstance*> (pi) != nullptr;
    }

    bool isSandboxed (te::Plugin& p)
    {
        return remoteOf (p) != nullptr;
    }

    juce::var sandboxInfo (te::Plugin& p)
    {
        if (auto* r = remoteOf (p))
            return r->info();

        return {};
    }

    juce::String sandboxProblem (juce::AudioPluginInstance& pi)
    {
        if (auto* r = remoteOf (pi); r != nullptr && ! r->running())
            return "plugin host not running: " + r->getName() + " (" + r->message() + ")";

        return {};
    }

    juce::String reloadSandboxedPlugin (te::Plugin& p)
    {
        auto* r = remoteOf (p);

        if (r == nullptr)
            return "not in a plugin host: " + p.getName();

        return r->reload();
    }

    uint32_t sandboxStatusSeq()
    {
        return registry().statusSeq.load();
    }

    void setPluginHostLostHandler (std::function<void (juce::AudioPluginInstance&)> h)
    {
        registry().lostHandler = std::move (h);
    }

    void syncSandboxedPlugin (te::Plugin& p)
    {
        if (auto* r = remoteOf (p))
        {
            r->checkHost();
            r->syncFromHost();
            markLv2EditorPending (p);
        }
    }

    bool syncSandboxedController (te::Plugin& p)
    {
        auto* r = remoteOf (p);
        return r != nullptr && r->syncController();
    }

    void markLv2EditorPending (te::Plugin& p)
    {
        // Ein LV2-Editor stellt seine Aenderungen fuer den naechsten Block an
        // (im Host-Prozess). Im Prozess zaehlt die Engine offene Editoren
        // (numOpenEditors, Patch tracktion-flush-pending-values) und gibt dem
        // Plugin beim Sichern ohne Graph dafuer 1 Sample; das Fenster im Host
        // sieht sie nicht — hier dasselbe ueber markValuesPending.
        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);
            ep != nullptr && ep->desc.pluginFormatName == "LV2" && remoteOf (p) != nullptr
            && p.windowState != nullptr && p.windowState->isWindowShowing())
            ep->markValuesPending();
    }

    void syncSandboxedPlugins (te::Edit& edit)
    {
        for (auto* p : te::getAllPlugins (edit, false))
            syncSandboxedPlugin (*p);
    }

    juce::String openSandboxWindow (juce::AudioPluginInstance& pi, const juce::String& title, juce::Point<int> at)
    {
        auto* r = remoteOf (pi);
        return r != nullptr ? r->openWindow (title, at) : juce::String ("not in the plugin sandbox");
    }

    void moveSandboxWindow (juce::AudioPluginInstance& pi, juce::Point<int> at)
    {
        if (auto* r = remoteOf (pi))
            r->moveWindow (at);
    }

    void closeSandboxWindow (juce::AudioPluginInstance& pi)
    {
        if (auto* r = remoteOf (pi))
            r->closeWindow();
    }

    SandboxWindowEvents pollSandboxWindow (juce::AudioPluginInstance& pi)
    {
        auto* r = remoteOf (pi);
        return r != nullptr ? r->pollWindow() : SandboxWindowEvents();
    }

    juce::String turnSandboxedKnob (juce::AudioPluginInstance& pi, int index, float value)
    {
        auto* r = remoteOf (pi);
        return r != nullptr ? r->editParameter (index, value) : juce::String ("not in the plugin sandbox");
    }

    juce::int64 sandboxFailureCount (te::Edit& edit)
    {
        juce::int64 n = 0;

        for (auto* p : te::getAllPlugins (edit, false))
            if (auto* r = remoteOf (*p))
                n += r->failureCount();

        return n;
    }

    juce::String sandboxFailureText (te::Edit& edit)
    {
        juce::StringArray names;

        for (auto* p : te::getAllPlugins (edit, false))
            if (auto* r = remoteOf (*p); r != nullptr && r->failureCount() > 0)
                names.addIfNotAlreadyThere (p->getName());

        return "plugin host failed during render (output incomplete): " + names.joinIntoString (", ");
    }

    //==============================================================================
    // Plugin-Scan im Kindprozess: ein Kind fuer alle Dateien eines Formats;
    // stirbt es, gilt die Datei in Arbeit als kaputt und ein neues Kind macht
    // mit der naechsten weiter. Frist je Datei 120 s (grosse Buendel wie LSP).
    std::vector<PluginScanResult> scanPluginFilesInChild (const juce::String& format, const juce::StringArray& files)
    {
        constexpr int64_t fileTimeoutNs = 120LL * 1000000000LL;
        std::vector<PluginScanResult> results ((size_t) files.size());
        std::vector<bool> done ((size_t) files.size(), false);
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
        const auto formatArg = format.toStdString();
        int next = 0;

        auto failRest = [&] (int from, const juce::String& why)
        {
            for (int i = from; i < files.size(); ++i)
                if (! done[(size_t) i])
                {
                    results[(size_t) i].error = why;
                    done[(size_t) i] = true;
                }
        };

        while (next < files.size())
        {
            int p[2];

            if (::pipe2 (p, O_CLOEXEC) != 0)
            {
                failRest (next, "cannot start the scan process (" + juce::String (std::strerror (errno)) + ")");
                break;
            }

            const int base = next;
            std::vector<std::string> args { exe, "--plugin-scan", "3", formatArg };

            for (int i = base; i < files.size(); ++i)
                args.push_back (files[i].toStdString());

            std::vector<char*> argv;

            for (auto& a : args)
                argv.push_back (a.data());

            argv.push_back (nullptr);

            pid_t pid = -1;
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init (&fa);
            posix_spawn_file_actions_adddup2 (&fa, p[1], 3);
            posix_spawn_file_actions_addclosefrom_np (&fa, 4);
            const auto rc = ::posix_spawn (&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy (&fa);
            ::close (p[1]);

            if (rc != 0)
            {
                ::close (p[0]);
                failRest (next, "cannot start the scan process (" + juce::String (std::strerror (rc)) + ")");
                break;
            }

            std::string buf;
            auto last = nowNs();
            bool timedOut = false;
            int current = base;

            for (;;)
            {
                pollfd pf { p[0], POLLIN, 0 };
                const int pr = ::poll (&pf, 1, 500);

                if (pr > 0)
                {
                    char tmp[65536];
                    const auto n = ::read (p[0], tmp, sizeof (tmp));

                    if (n <= 0)
                        break;

                    buf.append (tmp, (size_t) n);

                    for (size_t nl; (nl = buf.find ('\n')) != std::string::npos;)
                    {
                        auto j = juce::JSON::parse (juce::String::fromUTF8 (buf.data(), (int) nl));
                        buf.erase (0, nl + 1);
                        const int i = base + (int) j.getProperty ("i", -1);

                        if (! juce::isPositiveAndBelow (i, files.size()))
                            continue;

                        if (auto* ds = j.getProperty ("descs", {}).getArray())
                            for (auto& d : *ds)
                                if (auto xml = juce::parseXML (d.toString()))
                                {
                                    juce::PluginDescription desc;

                                    if (desc.loadFromXml (*xml))
                                        results[(size_t) i].descs.push_back (desc);
                                }

                        done[(size_t) i] = true;
                        current = i + 1;
                        last = nowNs();
                    }
                }
                else if (pr == 0 && ! timedOut && nowNs() - last > fileTimeoutNs)
                {
                    ::kill (pid, SIGKILL);
                    timedOut = true;
                }
            }

            ::close (p[0]);
            int status = 0;
            ::waitpid (pid, &status, 0);

            while (current < files.size() && done[(size_t) current])
                ++current;

            if (current >= files.size())
                break;

            auto& r = results[(size_t) current];

            if (timedOut)
                r.error = "timed out while scanning (" + juce::String (fileTimeoutNs / 1000000000LL) + " s)";
            else if (WIFSIGNALED (status))
                r.error = "crashed while scanning: signal " + juce::String (WTERMSIG (status))
                        + " (" + juce::String (::strsignal (WTERMSIG (status))) + ")";
            else
                r.error = "scan process ended early (exit code " + juce::String (WEXITSTATUS (status)) + ")";

            done[(size_t) current] = true;
            std::cerr << "[scan] " << format << " " << files[current] << ": " << r.error << "\n";
            next = current + 1;
        }

        return results;
    }
}
