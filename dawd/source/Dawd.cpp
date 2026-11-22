#include "Dawd.h"
#include "Routing.h"
#include "EngineHelpers.h"
#include "Feedback.h"
#include "Limiter.h"
#include "Sidechain.h"

#include "HeadlessUIBehaviour.h"
#include "PluginWindows.h"
#include "PluginSandbox.h"
#include "LiveLoudness.h"

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

    // Eine Zeile: Antworten und Event-Log ohne Einrueckung — eingerueckt war
    // state eines Agent-Projekts 257 statt 131 KB, und events.jsonl war kein
    // JSONL (ein Eintrag ueber viele Zeilen)
    juce::String jsonText (const juce::var& v)
    {
        return juce::JSON::toString (v, true);
    }

    namespace
    {
        // dawd braucht nur 2+2 Kanaele: die Default-Anfrage (512 Kanaele) laesst
        // PipeWire 128 Kanaele aufschlagen. Ohne eigene Beschreibung gruppiert
        // Tracktion die vom Geraet gemeldeten Maximal-Kanaele (hier 128) zu 64
        // Wave-Paaren — 63 Geister-Paare — und schreibt das Layout als
        // audiosettings_* in die Settings.xml, von wo es sonst jeden Start erneut
        // laedt. Darum explizit 2+2 anfragen und die Wave-Devices selbst
        // beschreiben; dann wird weder geladen noch gespeichert.
        // Engine-Einstellungen (Settings.xml: Audio-Geraet, Geraetetyp, MIDI)
        // im --config-Ordner statt immer in ~/.config/unidaw — sonst teilen
        // Suite, Mess-Werkzeuge und dawds aus anderen Zweigen eine Datei (ein
        // JACK-Probelauf hat so am 2026-10-16 allen anderen das Geraet genommen).
        // Der Cache-Ordner folgt (PropertyStorage::getAppCacheFolder).
        // audio_device_setup bleibt zusaetzlich nur im Speicher: ein JACK-Setup
        // in der Datei liesse den naechsten Start mit demselben --config aus
        // einem Build ohne JACK ohne Audio-Geraet starten (sampleRate 0).
        struct DawdPropertyStorage : public te::PropertyStorage
        {
            DawdPropertyStorage (const juce::String& appName, const juce::File& dir)
                : te::PropertyStorage (appName), folder (dir) {}

            juce::File getAppPrefsFolder() override
            {
                folder.createDirectory();
                return folder;
            }

            std::unique_ptr<juce::XmlElement> getXmlProperty (te::SettingID setting) override
            {
                if (setting == te::SettingID::audio_device_setup)
                    return deviceSetup != nullptr ? std::make_unique<juce::XmlElement> (*deviceSetup) : nullptr;

                return te::PropertyStorage::getXmlProperty (setting);
            }

            void setXmlProperty (te::SettingID setting, const juce::XmlElement& xml) override
            {
                if (setting == te::SettingID::audio_device_setup)
                    deviceSetup = std::make_unique<juce::XmlElement> (xml);
                else
                    te::PropertyStorage::setXmlProperty (setting, xml);
            }

            void removeProperty (te::SettingID setting) override
            {
                if (setting == te::SettingID::audio_device_setup)
                    deviceSetup.reset();

                // auch aus der Datei: Altlasten frueherer Starts raeumen
                te::PropertyStorage::removeProperty (setting);
            }

            juce::File folder;
            std::unique_ptr<juce::XmlElement> deviceSetup;
        };

        struct DawdEngineBehaviour : public te::EngineBehaviour
        {
            bool autoInitialiseDeviceManager() override { return false; }

            // Ladestand: externe Plugins einzeln melden (die teuren Schritte
            // beim Laden eines Nodes) — Verhalten wie die Basisklasse
            bool shouldLoadPlugin (te::ExternalPlugin& p) override
            {
                if (auto* s = engineLoadStatus())
                    s->pluginStarting (p.desc.name);

                return te::EngineBehaviour::shouldLoadPlugin (p);
            }

            void doAdditionalInitialisation (te::ExternalPlugin& p) override
            {
                if (auto* s = engineLoadStatus())
                    s->pluginLoaded();

                // Instanz beim Abbau sofort loeschen: die Engine loescht sonst
                // eine je 100-ms-Tick, Node-Wechsel blockieren die Ticks, und
                // bei schnellem Wechseln stauten sich Instanzen (nachbau-12:
                // 38 MB je Wechsel, 4,2 GB nach 140 Wechseln). dawd hat keine
                // Plugin-Fenster, fuer die das verzoegerte Loeschen gedacht ist.
                p.setDeletesPluginInstanceSynchronously (true);
            }

            // Warp-Marker (warpClip) rechnet die Engine beim Abspielen in
            // Echtzeit (Patch tracktion-warp-realtime)
            std::vector<std::pair<te::TimePosition, te::TimePosition>> getWarpPointsForClip (const te::AudioClipBase& ac) override
            {
                return realtimeWarpPoints (ac);
            }

            bool isDescriptionOfWaveDevicesSupported() override { return true; }

            void describeWaveDevices (std::vector<te::WaveDeviceDescription>& devices,
                                      juce::AudioIODevice& device, bool isInput) override
            {
                devices.clear();

                auto numChannels = juce::jmin (2, (isInput ? device.getInputChannelNames()
                                                           : device.getOutputChannelNames()).size());

                if (numChannels > 0)
                    devices.push_back (te::WaveDeviceDescription::withNumChannels ({}, 0, (uint32_t) numChannels, true));
            }
        };

        // UNIDAW_AUDIO=jack: JACK statt ALSA, gedacht fuer pipewire-jack (Start mit
        // pw-jack, Blockgroesse per PIPEWIRE_QUANTUM=64/48000). Ueber JACK entfaellt
        // der Vorlauf des PipeWire-ALSA-Plugins: Durchschleifen am Fireface UC bei
        // 64 Frames 8,9 statt 11,7 ms. JACK-"Geraete" sind JACK-Clients; Ausgang
        // UNIDAW_JACK_OUT (Standard der Loopback "Fireface UC Ausgang 1/2"), Eingang
        // UNIDAW_JACK_IN (Standard die Rohkanaele des UC, davon 1/2). Vorgegeben wird
        // das als Setup, das DeviceManager::initialise laedt (DawdPropertyStorage:
        // nur im Speicher).
        void selectAudioApi (te::Engine& engine)
        {
            auto& storage = engine.getPropertyStorage();

            if (! juce::SystemStats::getEnvironmentVariable ("UNIDAW_AUDIO", {}).equalsIgnoreCase ("jack"))
            {
                storage.removeProperty (te::SettingID::audio_device_setup);
                return;
            }

            std::unique_ptr<juce::AudioIODeviceType> jack (juce::AudioIODeviceType::createAudioIODeviceType_JACK());

            if (jack == nullptr)
            {
                std::cerr << "[audio] UNIDAW_AUDIO=jack, aber unidaw ist ohne JACK gebaut (jack/jack.h fehlte)\n";
                storage.removeProperty (te::SettingID::audio_device_setup);
                return;
            }

            jack->scanForDevices();
            auto outs = jack->getDeviceNames (false);
            auto ins = jack->getDeviceNames (true);

            auto pick = [] (const juce::StringArray& names, const juce::String& wanted,
                            std::function<bool (const juce::String&)> fallback)
            {
                if (names.contains (wanted))
                    return wanted;

                for (auto& n : names)
                    if (fallback (n))
                        return n;

                return names.isEmpty() ? juce::String() : names[0];
            };

            auto out = pick (outs, juce::SystemStats::getEnvironmentVariable ("UNIDAW_JACK_OUT", "Fireface UC Ausgang 1/2"),
                             [] (const juce::String& n) { return n.startsWith ("Fireface UC Ausgang 1/2"); });
            auto in = pick (ins, juce::SystemStats::getEnvironmentVariable ("UNIDAW_JACK_IN", {}),
                            [] (const juce::String& n) { return n.startsWith ("Fireface UC Mac") && n.contains ("Multichannel"); });

            std::cerr << "[audio] JACK: Ausgang \"" << out << "\", Eingang \"" << in << "\"\n";

            juce::XmlElement setup ("DEVICESETUP");
            setup.setAttribute ("deviceType", "JACK");
            setup.setAttribute ("audioOutputDeviceName", out);
            setup.setAttribute ("audioInputDeviceName", in);
            storage.setXmlProperty (te::SettingID::audio_device_setup, setup);
        }
    }

    namespace
    {
        // Hintergrund-Stand alle 500 ms nachtragen (Jobs laufen auf eigenen
        // Threads, ihre Liste gehoert dem Message-Thread)
        struct BackgroundStatusTimer : public juce::Timer
        {
            DawdContext& ctx;
            explicit BackgroundStatusTimer (DawdContext& c) : ctx (c) {}
            void timerCallback() override { updateBackgroundStatus (ctx); }
        };
    }

    DawdContext::DawdContext (const juce::String& appName, bool openAudio, const juce::File& prefsDir)
    : engine (std::make_unique<te::Engine> (std::make_unique<DawdPropertyStorage> (appName, prefsDir),
               std::make_unique<HeadlessUIBehaviour>(), std::make_unique<DawdEngineBehaviour>()))
    {
        // Feedback-Kanten der Routing-Matrix (vor dem ersten Edit)
        registerFeedbackPlugins (*engine);
        registerLimiterPlugin (*engine);
        registerSidechainPlugins (*engine);

        // Plugin-Sandbox (Schalter UNIDAW_PLUGIN_SANDBOX): VST3 in Host-Prozessen
        if (pluginSandboxRequested())
            installPluginSandbox (*engine);

        if (openAudio)
            selectAudioApi (*engine);

        // Alte audiosettings_*-Layouts (64 Geister-Paare aus Starts mit der
        // Engine-Default-Anfrage) aus der Settings.xml werfen — mit der eigenen
        // Wave-Device-Beschreibung sind sie tot, blieben aber sonst als
        // Datei-Leiche stehen.
        auto& props = engine->getPropertyStorage().getPropertiesFile();

        for (auto& key : juce::StringArray (props.getAllProperties().getAllKeys()))
            if (key.startsWith ("audiosettings_"))
                props.removeValue (key);

        // ohne Geraet: das Fireface bleibt frei (lange Offline-Laeufe wie
        // tools/profile_plugin.py neben Treiber-Arbeit)
        if (openAudio)
        {
            engine->getDeviceManager().initialise (2, 2);

            // Live-LUFS (levels): misst den fertigen Geraeteausgang
            auto meter = std::make_unique<LiveLoudness>();
            loudness = meter.get();
            engine->getDeviceManager().setGlobalOutputAudioProcessor (std::move (meter));
        }

        audioOpen = openAudio;
        // Plugin-Fenster baut dawd selbst (PluginWindows.cpp); main setzt
        // windows (echt oder --window-sim)
        static_cast<HeadlessUIBehaviour&> (engine->getUIBehaviour()).windowFactory
            = [this] (te::PluginWindowState& s) { return createPluginWindow (*this, s); };
        // Plugin-Sandbox: stirbt ein Host, geht das Fenster seines Moduls in
        // dawd zu (es gehoerte dem toten Prozess)
        setPluginHostLostHandler ([this] (juce::AudioPluginInstance& inst) { closeWindowOfLostHost (*this, inst); });
        playStop = std::make_unique<PlayStopTimer>();
        playStop->ctx = this;
        autoSave = std::make_unique<AutoSaveTimer>();
        autoSave->ctx = this;
        engineLoadStatus() = &load;
        loadTimer = std::make_unique<BackgroundStatusTimer> (*this);
        loadTimer->startTimer (500);

        // Monitoring: dawd gibt live ueber das Geraet wieder (Input-Monitoring
        // vor und waehrend der Aufnahme — Matthias-Anforderung). Die
        // Wave-Ausgaenge bleiben an.
    }

    void appendEvent (DawdContext& ctx, const juce::String& op, const juce::var& params, const juce::var& result)
    {
        auto entry = new juce::DynamicObject();
        entry->setProperty ("seq", ctx.nextSeq++);
        entry->setProperty ("ts", juce::Time::getCurrentTime().toISO8601 (true));
        entry->setProperty ("op", op);
        entry->setProperty ("params", params);
        entry->setProperty ("result", result);

        ctx.logFile.appendText (jsonText (juce::var (entry)) + "\n");
    }

    bool isRecording (DawdContext& ctx)
    {
        return ctx.edit != nullptr && ctx.edit->getTransport().isRecording();
    }

    void saveCurrentEdit (DawdContext& ctx)
    {
        ctx.load.setPhase ("saving");

        if (ctx.edit == nullptr)
            return;

        te::EditFileOperations ops (*ctx.edit);
        auto& mutes = listeningMutes (ctx, *ctx.edit);

        if (! anyTrackSolo (*ctx.edit) && mutes.empty())
        {
            ops.save (false, true, false);
            return;
        }

        // Solo und Abhoer-Mute gehoeren nicht in den Node: eine bereinigte
        // Kopie in einem Schritt schreiben (wie der Undo-Stand). Erst speichern
        // und dann bereinigen liess die Datei kurz mit Solo/Mute auf der Platte
        // — ein Leser (Test, Absturz) sah den Abhoer-Stand.
        auto file = ops.getEditFile();

        if (file == juce::File() || ctx.edit->isSaveInhibited())
            return;

        ctx.edit->getParameterControlMappings().saveToEdit();
        ctx.edit->flushState();
        auto state = ctx.edit->state.createCopy();
        applySavedListening (state, mutes);

        if (auto xml = state.createXml(); xml != nullptr && xml->writeTo (file))
            ctx.edit->resetChangedStatus();
        else
            std::cerr << "[save] cannot write " << file.getFullPathName() << "\n";
    }

    const std::map<juce::String, bool>& listeningMutes (DawdContext& ctx, te::Edit& edit)
    {
        static const std::map<juce::String, bool> none;

        if (ctx.listenState.isValid() && ! (ctx.edit != nullptr && ctx.edit->state == ctx.listenState))
        {
            ctx.savedMute.clear();   // anderer Edit geladen: dessen Datei hat den gespeicherten Stand
            ctx.listenState = {};
        }

        return &edit == ctx.edit.get() ? ctx.savedMute : none;
    }

    void listenMute (DawdContext& ctx, te::Track& track, bool on)
    {
        listeningMutes (ctx, *ctx.edit);
        ctx.listenState = ctx.edit->state;
        ctx.savedMute.emplace (track.itemID.toString(), track.isMuted (false));   // nur der erste Wert ist der gespeicherte
        track.setMute (on);
    }

    void forgetLaneListening (DawdContext& ctx, te::AudioTrack& lane)
    {
        listeningMutes (ctx, *ctx.edit);
        auto* head = laneGroupOf (lane) != nullptr ? laneGroupOf (lane) : &lane;

        for (auto* t : userTracks (*ctx.edit))
            if (t == head || laneGroupOf (*t) == head)
                ctx.savedMute.erase (t->itemID.toString());
    }

    ScopedListeningOff::ScopedListeningOff (DawdContext& ctx, te::Edit& edit)
    {
        auto& mutes = listeningMutes (ctx, edit);

        for (auto* t : te::getAllTracks (edit))
        {
            if (t->isSolo (false))
                soloed.add (t);

            if (auto it = mutes.find (t->itemID.toString()); it != mutes.end() && t->isMuted (false) != it->second)
                muted.add ({ t, t->isMuted (false) });
        }

        for (auto& t : soloed)
            t->setSolo (false);

        for (auto& [t, listening] : muted)
            t->setMute (! listening);
    }

    ScopedListeningOff::~ScopedListeningOff()
    {
        for (auto& t : soloed)
            t->setSolo (true);

        for (auto& [t, listening] : muted)
            t->setMute (listening);
    }

    namespace
    {
        // Buendelt Op-Folgen (Agent-Batches, Drags mit coalesce) zu einem
        // Speichern; ein Absturz verliert hoechstens dieses Fenster.
        constexpr int autosaveDelayMs = 400;
    }

    void AutoSaveTimer::timerCallback()
    {
        stopTimer();

        if (ctx->edit == nullptr || ! ctx->tree)
            return;

        if (isRecording (*ctx))
        {
            startTimer (autosaveDelayMs);   // nach dem Take speichern
            return;
        }

        saveCurrentEdit (*ctx);
    }

    void scheduleAutosave (DawdContext& ctx)
    {
        if (ctx.edit != nullptr && ctx.tree)
            ctx.autoSave->startTimer (autosaveDelayMs);
    }

    void flushAutosave (DawdContext& ctx)
    {
        // Offenes Plugin-Fenster: was im Editor gedreht wurde, steht nur im
        // Plugin — beim Beenden auch ohne geplanten Autosave speichern
        if (! ctx.autoSave->isTimerRunning() && openPluginWindowIds (ctx).isEmpty())
            return;

        ctx.autoSave->stopTimer();

        if (ctx.edit != nullptr && ctx.tree && ! isRecording (ctx))
            saveCurrentEdit (ctx);
    }

    void clearMeters (DawdContext& ctx)
    {
        for (auto& [id, m] : ctx.meters)
            if (m.measurer != nullptr && m.client != nullptr)
                m.measurer->removeClient (*m.client);

        ctx.meters.clear();
    }

    void updateSessionFile (DawdContext& ctx)
    {
        if (ctx.sessionFile == juce::File())
            return;

        auto dir = ctx.tree ? ctx.tree->projectDir.getFullPathName() : juce::String();

        if (dir == ctx.sessionDir)
            return;

        ctx.sessionDir = dir;
        auto o = new juce::DynamicObject();
        o->setProperty ("dir", dir);
        ctx.sessionFile.replaceWithText (jsonText (juce::var (o)));
    }

    juce::String readSessionDir (const juce::File& sessionFile)
    {
        if (! sessionFile.existsAsFile())
            return {};

        return juce::JSON::parse (sessionFile.loadFileAsString()).getProperty ("dir", {}).toString();
    }

    void releaseIdleGraph (DawdContext& ctx)
    {
        if (ctx.edit == nullptr)
            return;

        auto& tc = ctx.edit->getTransport();
        bool armed = false;

        for (auto in : ctx.edit->getAllInputDevices())
            for (auto t : in->getTargets())
                armed = armed || in->isRecordingEnabled (t);

        const auto now = juce::Time::getMillisecondCounter();

        // Diagnose (todos/2026-10-24-lane-active-wackelt.md): Aufnahme ohne
        // record off zu Ende — einmal je Take ins Log
        if (! ctx.armedTakes.empty() && ! tc.isRecording() && ctx.loggedTakeEnd != ctx.armedTakes.data())
        {
            ctx.loggedTakeEnd = ctx.armedTakes.data();
            std::cerr << "[record] recording ended without stop: position "
                      << tc.getPosition().inSeconds() << " s, playing " << (int) tc.isPlaying()
                      << ", armed " << (int) armed << "\n";
        }

        if (tc.isPlaying() || tc.isRecording() || ctx.monitorOn || armed || ctx.lastAudioUse == 0)
        {
            ctx.lastAudioUse = now;
            return;
        }

        if (tc.isPlayContextActive() && now - ctx.lastAudioUse >= 20000)
        {
            tc.freePlaybackContext();
            std::cerr << "[audio] idle: playback graph released\n";
        }
    }

    void disarmRecording (DawdContext& ctx)
    {
        for (auto in : ctx.edit->getAllInputDevices())
            for (auto t : in->getTargets())
                in->setRecordingEnabled (t, false);
    }

    void bindEditResolver (DawdContext& ctx, const juce::File& editFile)
    {
        if (ctx.edit == nullptr || editFile == juce::File())
            return;

        ctx.edit->editFileRetriever = [editFile] { return editFile; };
        ctx.edit->filePathResolver = [editFile] (const juce::String& p) -> juce::File
        {
            if (juce::File::isAbsolutePath (p))
                return juce::File (p);

            // getRelativePathFrom baut den Pfad je nachdem, ob die Edit-Datei beim
            // Einfuegen schon existierte, gegen den vollen Pfad (Datei als Ebene) oder
            // gegen das Elternverzeichnis. Eine Ebene Differenz — beides probieren,
            // die aufloesbare Variante gewinnt.
            auto viaChild = editFile.getChildFile (p);

            if (viaChild.existsAsFile())
                return viaChild;

            return editFile.getSiblingFile (p);
        };
    }

    namespace
    {
        // Externe Plugins im Edit (VST3/LV2: PLUGIN type="vst") — das Gesamt
        // fuer den Ladebalken; eingebaute laden ohne Meldung der Engine
        int countExternalPlugins (const juce::File& editFile)
        {
            auto text = editFile.loadFileAsString();
            int n = 0;

            for (int i = text.indexOf ("type=\"vst\""); i >= 0; i = text.indexOf (i + 1, "type=\"vst\""))
                ++n;

            return n;
        }
    }

    void updateBackgroundStatus (DawdContext& ctx)
    {
        juce::Array<juce::var> bg;

        for (auto& [id, j] : ctx.jobs->jobs)
            if (! j->done)
                bg.add (resultObj (true, { { "kind", "job" }, { "id", id }, { "op", j->kind },
                                           { "progress", std::round ((double) j->progress * 1000.0) / 1000.0 } }));

        if (ctx.library != nullptr)
        {
            auto p = ctx.library->sounds().progress();

            if (p.running)
                bg.add (resultObj (true, { { "kind", "soundIndex" }, { "done", p.done }, { "total", p.total } }));
        }

        for (auto& v : bg)
            v.getDynamicObject()->removeProperty ("ok");

        ctx.load.setBackground (bg);
    }

    void loadNodeEdit (DawdContext& ctx, const juce::File& editFile)
    {
        ctx.recordedTakes.clear();
        ctx.deletedClips.clear();

        // Ladestand: externe Plugins zaehlen (je PLUGIN type="vst" eins), die
        // Engine meldet jedes beim Laden (DawdEngineBehaviour)
        const auto external = countExternalPlugins (editFile);
        ctx.load.setPhase (external > 0 ? "plugins" : "node edit", external);
        // Offene Plugin-Fenster gehen mit dem alten Edit zu — im selben
        // Projekt (Branch, Checkout, Undo) im neuen wieder auf
        const auto windowIds = openPluginWindowIds (ctx);
        ctx.edit = te::loadEditFromFile (*ctx.engine, editFile);
        bindEditResolver (ctx, editFile);

        if (ctx.edit != nullptr)
        {
            ctx.load.setPhase ("routing");
            ensureRouting (*ctx.edit);
            applySectionTempos (*ctx.edit);
            refreshAutoCrossfades (*ctx.edit);
            playWarpSources (*ctx.edit);
            ctx.load.setPhase ("samples");
            settlePlugins (*ctx.edit);   // Sampler-Sounds sofort, nicht erst nach der Message-Loop
            reopenPluginWindows (ctx, windowIds);
        }
    }

    int recordNodeAction (DawdContext& ctx, const juce::String& kind, const juce::String& op,
                          const juce::var& params, const juce::String& file, bool coalesce)
    {
        if (! ctx.tree)
            return 0;

        if (coalesce)
            if (auto* n = ctx.tree->find (ctx.tree->current); n != nullptr && ! n->actions.isEmpty())
            {
                auto& last = n->actions.getReference (n->actions.size() - 1);

                if (last.kind == kind && last.op == op
                    && last.params.getProperty ("track", {}) == params.getProperty ("track", {})
                    && last.params.getProperty ("name", {}) == params.getProperty ("name", {})
                    && last.params.getProperty ("param", {}) == params.getProperty ("param", {})
                    && last.params.getProperty ("plugin", {}) == params.getProperty ("plugin", {})
                    && last.params.getProperty ("group", {}) == params.getProperty ("group", {}))
                {
                    last.params = params;
                    last.file = file;
                    last.created = juce::Time::getCurrentTime().toISO8601 (true);
                    ctx.tree->save();
                    return last.id;
                }
            }

        auto a = ctx.tree->appendAction (ctx.tree->current, kind, op, params, file);
        ctx.tree->save();
        return a.id;
    }

    void recordAutoSection (DawdContext& ctx, const juce::var& newSection)
    {
        if (newSection.isVoid())
            return;

        auto params = new juce::DynamicObject();
        params->setProperty ("name", newSection.getProperty ("name", {}));
        params->setProperty ("auto", true);
        recordNodeAction (ctx, "collect", "addSection", juce::var (params));
    }

    bool removeNodeAction (DawdContext& ctx, int actionId)
    {
        if (! ctx.tree)
            return false;

        auto* n = ctx.tree->find (ctx.tree->current);

        if (n == nullptr)
            return false;

        for (int i = 0; i < n->actions.size(); ++i)
            if (n->actions.getReference (i).id == actionId)
            {
                n->actions.remove (i);
                ctx.tree->save();
                return true;
            }

        return false;
    }

    bool isAncestorOf (const TreeState& t, int ancestor, int id)
    {
        for (auto cur = t.find (id); cur != nullptr && cur->parent != 0; cur = t.find (cur->parent))
            if (cur->parent == ancestor)
                return true;

        return false;
    }
}
