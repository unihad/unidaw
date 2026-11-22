#include "PluginWindows.h"
#include "Dawd.h"
#include "OpRegistry.h"
#include "Routing.h"
#include "PluginSandbox.h"

#include <cstdlib>

namespace dawd
{
    namespace
    {
        juce::File positionsFile (DawdContext& ctx)
        {
            return ctx.tree ? ctx.tree->projectDir.getChildFile ("windows.json") : juce::File();
        }

        // Lagen des offenen Projekts (beim ersten Zugriff je Projekt gelesen)
        std::map<juce::String, juce::Point<int>>& positions (DawdContext& ctx)
        {
            auto& w = *ctx.windows;
            auto f = positionsFile (ctx);

            if (! w.loaded || f != w.loadedFor)
            {
                w.positions.clear();
                w.loadedFor = f;
                w.loaded = true;

                // Das geparste var muss leben, solange o benutzt wird — als
                // Temporaeres in der if-Bedingung war o danach freigegeben
                // (Absturz beim Oeffnen, sobald windows.json existierte)
                const auto json = f.existsAsFile() ? juce::JSON::parse (f) : juce::var();

                if (auto* o = json.getDynamicObject())
                    for (auto& p : o->getProperties())
                        if (p.value.hasProperty ("x") && p.value.hasProperty ("y"))
                            w.positions[p.name.toString()] = { (int) p.value.getProperty ("x", 0),
                                                               (int) p.value.getProperty ("y", 0) };
            }

            return w.positions;
        }

        void savePositions (DawdContext& ctx)
        {
            auto f = positionsFile (ctx);

            if (f == juce::File())
                return;

            auto o = new juce::DynamicObject();

            for (auto& [id, pt] : positions (ctx))
            {
                auto e = new juce::DynamicObject();
                e->setProperty ("x", pt.x);
                e->setProperty ("y", pt.y);
                o->setProperty (id, juce::var (e));
            }

            f.replaceWithText (juce::JSON::toString (juce::var (o)));
        }

        // Modul eines Plugins im offenen Edit ("" = keins)
        juce::String moduleIdOf (te::Edit& edit, te::Plugin& plugin)
        {
            for (auto& id : moduleIds (edit))
                if (auto* m = findModuleTrack (edit, id); m != nullptr && modulePlugin (*m) == &plugin)
                    return id;

            return {};
        }

        juce::AudioPluginInstance* instanceOf (te::Plugin& p)
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);
            return ep != nullptr ? ep->getAudioPluginInstance() : nullptr;
        }

        EditorState pluginState (te::Plugin& p)
        {
            EditorState s;

            // Plugin-Sandbox: Aenderungen aus dem Editor im Host erst abholen
            syncSandboxedPlugin (p);

            if (auto* inst = instanceOf (p))
            {
                // Aenderungen im Editor eines LV2 warten auf den naechsten Block
                static_cast<te::ExternalPlugin&> (p).passPendingValuesToPlugin();
                inst->getStateInformation (s.chunk);

                for (auto* prm : inst->getParameters())
                    s.values.add (prm->getValue());
            }

            return s;
        }

        void restoreState (te::Plugin& p, const EditorState& s)
        {
            if (auto* inst = instanceOf (p))
            {
                if (s.chunk.getSize() > 0)
                    inst->setStateInformation (s.chunk.getData(), (int) s.chunk.getSize());

                auto& prms = inst->getParameters();

                for (int i = 0; i < juce::jmin (prms.size(), s.values.size()); ++i)
                    if (prms[i]->getValue() != s.values[i])
                        prms[i]->setValueNotifyingHost (s.values[i]);

                p.flushPluginStateToValueTree();
            }
        }

        te::Plugin* pluginOf (DawdContext& ctx, const juce::String& moduleId)
        {
            auto* m = ctx.edit != nullptr ? findModuleTrack (*ctx.edit, moduleId) : nullptr;
            return m != nullptr ? modulePlugin (*m) : nullptr;
        }

        juce::String displayError()
        {
            // Leeres DISPLAY: JUCE nimmt dann still ":0" — dawd nicht
            auto* d = std::getenv ("DISPLAY");

            if (d == nullptr || *d == 0)
                return "no display: unidaw runs without DISPLAY (start it in the desktop session, e.g. systemd-run -E DISPLAY=:0)";

            if (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() == nullptr)
                return "no display: cannot reach the X server at " + juce::String (d);

            return {};
        }

        // Echtes Fenster: Editor der Engine (te::Plugin::createEditor) in
        // einem nativen Fenster (NativeEditorWindow). Die Lage wandert beim
        // Verschieben mit, beim Schliessen landet sie in windows.json.
        struct EditorWindow : public NativeEditorWindow
        {
            EditorWindow (DawdContext& c, const juce::String& moduleId, te::PluginWindowState& s,
                          std::unique_ptr<te::Plugin::EditorComponent> editor, const juce::String& title, juce::Point<int> at)
                : NativeEditorWindow (title, editor.get(), editor->allowWindowResizing(), at),
                  ctx (c), id (moduleId), state (s)
            {
                editor.release();
                tracking = true;
            }

            ~EditorWindow() override
            {
                savePositions (ctx);
            }

            void moved() override
            {
                juce::DocumentWindow::moved();

                if (tracking)
                    if (auto o = outerPosition())
                        positions (ctx)[id] = *o;
            }

            void closeButtonPressed() override
            {
                // Nicht im eigenen Callback loeschen: danach, ueber die Engine.
                // Derselbe Weg wie die Op (executeOp): eine Aenderung im
                // Editor bucht so Aktion, Auto-Branch, Undo und Version wie
                // ueber die API, und das Log zeigt das Schliessen
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<EditorWindow> (this)]
                {
                    if (safe == nullptr)
                        return;

                    auto& c = safe->ctx;
                    auto cmd = new juce::DynamicObject();
                    cmd->setProperty ("plugin", safe->id);
                    cmd->setProperty ("open", false);
                    executeOp (c, "pluginWindow", juce::var (cmd));
                });
            }

            DawdContext& ctx;
            juce::String id;
            te::PluginWindowState& state;
            bool tracking = false;
        };

        // Plugin-Sandbox (Phase 2): der Editor laeuft im Host-Prozess in dessen
        // eigenem Fenster (NativeEditorWindow, gleiche Lage-Logik). In dawd
        // steht ein Component ohne Desktop-Fenster (wie SimWindow) — die Engine
        // sieht ein offenes Fenster. Der Host meldet Lage und Klick auf
        // Schliessen; der Timer traegt beides nach (windows.json, Op wie
        // EditorWindow::closeButtonPressed).
        struct RemoteWindow : public juce::Component, private juce::Timer
        {
            RemoteWindow (DawdContext& c, const juce::String& moduleId, te::Plugin& p, juce::Point<int> at)
                : ctx (c), id (moduleId), plugin (p)
            {
                setBounds (at.x, at.y, 400, 300);
            }

            bool open (const juce::String& title)
            {
                auto* inst = instanceOf (plugin);

                if (inst == nullptr)
                    return false;

                if (auto err = openSandboxWindow (*inst, title, getPosition()); err.isNotEmpty())
                {
                    std::cerr << "[windows] " << id << ": " << err << "\n";
                    return false;
                }

                startTimer (100);
                return true;
            }

            ~RemoteWindow() override
            {
                stopTimer();

                if (auto* inst = instanceOf (plugin))
                    closeSandboxWindow (*inst);

                savePositions (ctx);
            }

            // Lage samt Titelleiste, sobald der Host sie gemeldet hat
            std::optional<juce::Point<int>> outerPosition() const   { return outer; }

            void place (juce::Point<int> at)
            {
                setTopLeftPosition (at);
                outer.reset();

                if (auto* inst = instanceOf (plugin))
                    moveSandboxWindow (*inst, at);

                timerCallback();
            }

            void timerCallback() override
            {
                auto* inst = instanceOf (plugin);

                if (inst == nullptr)
                    return;

                // LV2: Aenderungen im Editor warten im Host auf einen Block —
                // auch ein Sichern ohne Op (Autosave) soll sie mitnehmen
                markLv2EditorPending (plugin);

                auto ev = pollSandboxWindow (*inst);

                if (ev.outer && ev.outer != outer)
                {
                    outer = ev.outer;
                    positions (ctx)[id] = *outer;
                }

                if (ev.closeRequested)
                    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<RemoteWindow> (this)]
                    {
                        if (safe == nullptr)
                            return;

                        auto& c = safe->ctx;
                        auto cmd = new juce::DynamicObject();
                        cmd->setProperty ("plugin", safe->id);
                        cmd->setProperty ("open", false);
                        executeOp (c, "pluginWindow", juce::var (cmd));
                    });
            }

            DawdContext& ctx;
            juce::String id;
            te::Plugin& plugin;
            std::optional<juce::Point<int>> outer;
        };

        // Simulation (--window-sim): ein Component ohne Desktop-Fenster — die
        // Engine sieht ein sichtbares Fenster, der Bildschirm nichts
        struct SimWindow : public juce::Component
        {
            SimWindow (DawdContext& c, const juce::String& moduleId, juce::Point<int> at)
                : ctx (c), id (moduleId)
            {
                setBounds (at.x, at.y, 400, 300);
                tracking = true;
            }

            ~SimWindow() override
            {
                savePositions (ctx);
            }

            void moved() override
            {
                if (tracking)
                    positions (ctx)[id] = getPosition();
            }

            DawdContext& ctx;
            juce::String id;
            bool tracking = false;
        };
    }

    std::unique_ptr<juce::Component> createPluginWindow (DawdContext& ctx, te::PluginWindowState& s)
    {
        auto* ws = dynamic_cast<te::Plugin::WindowState*> (&s);

        if (ctx.windows == nullptr || ws == nullptr || ctx.edit == nullptr)
            return {};

        auto& plugin = ws->plugin;
        auto id = moduleIdOf (*ctx.edit, plugin);

        if (id.isEmpty())
            return {};

        auto& pos = positions (ctx);
        auto known = pos.find (id);

        if (ctx.windows->simulated)
        {
            // feste Staffel ohne Bildschirm: 100, 130, 160 … je offenem Fenster
            auto at = known != pos.end() ? known->second
                                         : juce::Point<int> (100, 100) + juce::Point<int> (30, 30) * (int) te::PluginWindowState::getNumOpenWindows (*ctx.edit);
            return std::make_unique<SimWindow> (ctx, id, at);
        }

        auto at = known != pos.end() ? known->second : s.choosePositionForPluginWindow();
        auto* m = findModuleTrack (*ctx.edit, id);
        auto label = m != nullptr ? moduleLabel (*m) : juce::String();
        auto title = (label.isNotEmpty() ? label : plugin.getName()) + " (" + id + ") - unidaw";

        // Plugin-Sandbox: das Fenster oeffnet der Host-Prozess
        if (isSandboxed (plugin))
        {
            auto w = std::make_unique<RemoteWindow> (ctx, id, plugin, at);

            if (! w->open (title))
                return {};

            return w;
        }

        auto editor = plugin.createEditor();

        if (editor == nullptr)
            return {};

        return std::make_unique<EditorWindow> (ctx, id, s, std::move (editor), title, at);
    }

    bool pluginWindowOpen (te::Plugin& p)
    {
        return p.windowState != nullptr && p.windowState->isWindowShowing();
    }

    juce::String openPluginWindow (DawdContext& ctx, const juce::String& moduleId, std::optional<juce::Point<int>> at)
    {
        auto* m = findModuleTrack (*ctx.edit, moduleId);
        auto* p = m != nullptr ? modulePlugin (*m) : nullptr;

        if (p == nullptr)
            return "no such plugin: " + moduleId;

        if (! ctx.windows->simulated)
            if (auto err = displayError(); err.isNotEmpty())
                return err;

        // Nur Fremd-Plugins bringen eine eigene Oberflaeche mit; die
        // eingebauten (4OSC, Sampler, Limiter …) laufen ueber die Parameter
        auto* ep = dynamic_cast<te::ExternalPlugin*> (p);
        const auto noEditor = "no editor: " + p->getName() + " has no window of its own (listParameters/setParameter)";

        if (ep == nullptr)
            return noEditor;

        auto* inst = ep->getAudioPluginInstance();

        if (inst == nullptr)
            return "plugin not loaded: " + p->getName();

        // Plugin-Sandbox: Host abgestuerzt und (noch) nicht neu geladen
        if (auto err = sandboxProblem (*inst); err.isNotEmpty())
            return err;

        if (! inst->hasEditor())
            return noEditor;

        if (at)
        {
            positions (ctx)[moduleId] = *at;
            savePositions (ctx);
        }

        const bool wasOpen = pluginWindowOpen (*p);
        p->windowState->showWindowExplicitly();

        if (! pluginWindowOpen (*p))
            return "editor could not be created: " + p->getName();

        if (! wasOpen)
            ctx.windows->openedState[moduleId] = pluginState (*p);

        if (wasOpen && at)
        {
            if (auto* w = dynamic_cast<EditorWindow*> (p->windowState->pluginWindow.get()))
                w->place (*at);
            else if (auto* rw = dynamic_cast<RemoteWindow*> (p->windowState->pluginWindow.get()))
                rw->place (*at);
            else
                p->windowState->pluginWindow->setTopLeftPosition (*at);
        }

        return {};
    }

    bool closePluginWindow (DawdContext& ctx, const juce::String& moduleId)
    {
        // Von executeOp vorbereitet (Aenderung im Editor): das Fenster ist zu
        if (applyPluginWindowChange (ctx, moduleId))
            return true;

        auto* p = pluginOf (ctx, moduleId);
        ctx.windows->openedState.erase (moduleId);

        if (p != nullptr && pluginWindowOpen (*p))
        {
            p->windowState->closeWindowExplicitly();
            scheduleAutosave (ctx);
        }

        return false;
    }

    bool closesChangedPluginWindow (DawdContext& ctx, const juce::var& cmd)
    {
        if (! ctx.tree || ctx.windows == nullptr || cmd.hasProperty ("x") || cmd.hasProperty ("y")
            || (cmd.hasProperty ("open") && (bool) cmd.getProperty ("open", false)))
            return false;

        const auto id = cmd.getProperty ("plugin", {}).toString();
        auto* p = pluginOf (ctx, id);
        auto it = ctx.windows->openedState.find (id);
        return p != nullptr && pluginWindowOpen (*p) && it != ctx.windows->openedState.end()
            && pluginState (*p) != it->second;
    }

    void beginPluginWindowChange (DawdContext& ctx, const juce::String& moduleId)
    {
        auto* p = pluginOf (ctx, moduleId);
        auto& w = *ctx.windows;

        if (p == nullptr)
            return;

        w.closingState[moduleId] = pluginState (*p);

        if (pluginWindowOpen (*p))
            p->windowState->closeWindowExplicitly();

        restoreState (*p, w.openedState[moduleId]);
        w.openedState.erase (moduleId);
    }

    bool applyPluginWindowChange (DawdContext& ctx, const juce::String& moduleId)
    {
        auto& w = *ctx.windows;
        auto it = w.closingState.find (moduleId);

        if (it == w.closingState.end())
            return false;

        if (auto* p = pluginOf (ctx, moduleId))
        {
            restoreState (*p, it->second);
            saveCurrentEdit (ctx);
        }

        w.closingState.erase (it);
        return true;
    }

    void pluginWindowSettled (DawdContext& ctx, const juce::String& moduleId)
    {
        if (ctx.windows == nullptr || ctx.windows->openedState.count (moduleId) == 0)
            return;

        if (auto* p = pluginOf (ctx, moduleId); p != nullptr && pluginWindowOpen (*p))
            ctx.windows->openedState[moduleId] = pluginState (*p);
    }

    juce::String turnSimulatedKnob (DawdContext& ctx, const juce::String& moduleId, const juce::String& param, double value)
    {
        if (! ctx.windows->simulated)
            return "plugin windows are not simulated (start unidaw with --window-sim)";

        auto* p = pluginOf (ctx, moduleId);

        if (p == nullptr)
            return "no such plugin: " + moduleId;

        if (! pluginWindowOpen (*p))
            return "window is not open: " + moduleId;

        if (auto* inst = instanceOf (*p))
            for (auto* prm : inst->getParameters())
                if (prm->getName (256).equalsIgnoreCase (param))
                {
                    // Plugin-Sandbox: der Regler dreht sich im Host-Prozess, die
                    // Aenderung kommt ueber den Rueckweg wie aus einem Editor
                    if (isSandboxed (inst))
                        return turnSandboxedKnob (*inst, prm->getParameterIndex(), (float) juce::jlimit (0.0, 1.0, value));

                    // wie die Oberflaeche des Plugins: Geste und Meldung an den Host
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost ((float) juce::jlimit (0.0, 1.0, value));
                    prm->endChangeGesture();
                    return {};
                }

        return "no such parameter: " + param;
    }

    void closeWindowOfLostHost (DawdContext& ctx, juce::AudioPluginInstance& inst)
    {
        if (ctx.edit == nullptr || ctx.windows == nullptr)
            return;

        for (auto& id : moduleIds (*ctx.edit))
            if (auto* p = pluginOf (ctx, id); p != nullptr && instanceOf (*p) == &inst && pluginWindowOpen (*p))
            {
                ctx.windows->openedState.erase (id);
                p->windowState->closeWindowExplicitly();
                std::cerr << "[windows] " << id << ": closed, its plugin host is gone\n";
            }
    }

    std::optional<juce::Point<int>> pluginWindowPosition (DawdContext& ctx, const juce::String& moduleId)
    {
        auto& pos = positions (ctx);
        auto known = pos.find (moduleId);

        if (auto* m = findModuleTrack (*ctx.edit, moduleId))
            if (auto* p = modulePlugin (*m); p != nullptr && pluginWindowOpen (*p))
            {
                auto* w = p->windowState->pluginWindow.get();
                auto* ew = dynamic_cast<EditorWindow*> (w);
                auto* rw = dynamic_cast<RemoteWindow*> (w);

                if (ew == nullptr && rw == nullptr)
                    return w->getPosition();

                // Echtes Fenster samt Titelleiste; solange der Rahmen
                // unbekannt ist, die angefragte Lage (gemerkt oder gesetzt)
                if (auto o = ew != nullptr ? ew->outerPosition() : rw->outerPosition())
                    return o;

                if (known == pos.end())
                    return w->getPosition();
            }

        if (known != pos.end())
            return known->second;

        return {};
    }

    juce::StringArray openPluginWindowIds (DawdContext& ctx)
    {
        juce::StringArray ids;

        if (ctx.edit != nullptr && ctx.tree && ctx.windows != nullptr)
            for (auto& id : moduleIds (*ctx.edit))
                if (auto* m = findModuleTrack (*ctx.edit, id))
                    if (auto* p = modulePlugin (*m); p != nullptr && pluginWindowOpen (*p))
                        ids.add (id);

        return ids;
    }

    void reopenPluginWindows (DawdContext& ctx, const juce::StringArray& ids)
    {
        if (ctx.edit == nullptr || ctx.windows == nullptr)
            return;

        for (auto& id : ids)
            if (findModuleTrack (*ctx.edit, id) != nullptr)
                if (auto err = openPluginWindow (ctx, id, {}); err.isNotEmpty())
                    std::cerr << "[windows] " << id << " not reopened: " << err << "\n";
    }

    //==============================================================================
    NativeEditorWindow::NativeEditorWindow (const juce::String& title, juce::Component* contentToOwn, bool resizable, juce::Point<int> at)
        : juce::DocumentWindow (title, juce::Colours::black, closeButton | minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (contentToOwn, true);
        setResizable (resizable, false);
        place (at);
        startTimer (50);
    }

    void NativeEditorWindow::timerCallback()
    {
        dispatchQueuedXEvents();
    }

    std::optional<juce::Point<int>> NativeEditorWindow::frameOffset()
    {
        if (auto* peer = getPeer())
            if (auto frame = peer->getFrameSizeIfPresent())
                return juce::Point<int> (frame->getLeft(), frame->getTop());

        return {};
    }

    std::optional<juce::Point<int>> NativeEditorWindow::outerPosition()
    {
        if (auto f = frameOffset())
            return getPosition() - *f;

        return {};
    }

    void NativeEditorWindow::place (juce::Point<int> at)
    {
        setTopLeftPosition (at + frameOffset().value_or (juce::Point<int>()));
    }
}
