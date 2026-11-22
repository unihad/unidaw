#include "HttpServer.h"
#include "Dawd.h"
#include "OpRegistry.h"
#include "Interface.h"
#include "PluginWindows.h"
#include "PluginSandbox.h"
#include "Osc.h"
#include "Resources.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace dawd;

namespace
{
    // SIGTERM/SIGINT (systemctl stop/restart, Ctrl-C): nicht hart sterben,
    // sondern den Dispatch-Loop beenden — danach schreibt main den
    // ausstehenden Autosave. Der Handler setzt nur das Flag (signal-sicher).
    std::atomic<bool> stopRequested { false };

    extern "C" void onStopSignal (int) { stopRequested = true; }

    struct StopSignalTimer : public juce::Timer
    {
        void timerCallback() override
        {
            if (stopRequested)
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();
            }
        }
    };

    // Token der Sitzung (localhost-API absichern, ARCHITECTURE.md): aus
    // UNIDAW_TOKEN (Starter, Test-Suite) oder 32 Zufallsbytes aus /dev/urandom
    std::string sessionToken()
    {
        if (auto* fromEnv = std::getenv ("UNIDAW_TOKEN"); fromEnv != nullptr && *fromEnv != 0)
            return fromEnv;

        unsigned char bytes[32] = {};
        juce::FileInputStream in (juce::File ("/dev/urandom"));

        if (! in.openedOk() || in.read (bytes, sizeof (bytes)) != (int) sizeof (bytes))
            return {};

        return juce::String::toHexString (bytes, sizeof (bytes), 0).toStdString();
    }

    // Datei nur fuer den Nutzer lesbar (0600), auch wenn sie schon da war
    bool writeTokenFile (const juce::File& f, const std::string& token)
    {
        f.getParentDirectory().createDirectory();
        const int fd = ::open (f.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (fd < 0)
            return false;

        const bool ok = ::fchmod (fd, 0600) == 0
                     && ::write (fd, token.data(), token.size()) == (ssize_t) token.size();
        ::close (fd);
        return ok;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    // vor dem ersten Fenster, auch fuer Host und Scan-Kind (PluginWindows.h)
    installXErrorHandler();

    // Host-Prozess der Plugin-Sandbox (PluginSandbox.h): dawd startet sich
    // selbst so, mit dem Shared Memory als fd und der eigenen PID
    if (argc >= 5 && juce::String (argv[1]) == "--plugin-host" && juce::String (argv[3]) == "--parent")
        return runPluginHost (juce::String (argv[2]).getIntValue(), juce::String (argv[4]).getIntValue());

    // Plugin-Scan im Kindprozess (Plugin-Sandbox): stuerzt ein Plugin beim
    // Scan ab, trifft es nur dieses Kind
    if (argc >= 4 && juce::String (argv[1]) == "--plugin-scan")
    {
        juce::StringArray files;

        for (int i = 4; i < argc; ++i)
            files.add (juce::String::fromUTF8 (argv[i]));

        return runPluginScan (juce::String (argv[2]).getIntValue(), juce::String (argv[3]), files);
    }

    int port = 7400;
    juce::String logPath = "events.jsonl";
    // UI und gepruefte Rezepte liegen bei der Binary (Resources.h); --ui und
    // --recipes zeigen fuer Entwicklung und Suite auf einen anderen Ort
    juce::String uiPath = resourceDir ("ui").getChildFile ("flow.html").getFullPathName();
    juce::String sessionPath;
    bool interfaceSim = false;   // simuliertes Fireface UC (Test-Suite)
    bool windowSim = false;      // Plugin-Fenster ohne Bildschirm (Test-Suite)
    bool noAudio = false;        // kein Audio-/MIDI-Geraet (nur Offline-Render)
    // Nutzer-Einstellungen (Sample-Bibliotheken samt Index); die Test-Suite
    // gibt einen eigenen Ordner mit. Gibt es nur den Ordner aus der Zeit vor
    // dem Namen unidaw (~/.config/dawd), bleibt er in Gebrauch.
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    auto configDefault = home.getChildFile (".config/unidaw");
    if (! configDefault.isDirectory() && home.getChildFile (".config/dawd").isDirectory())
        configDefault = home.getChildFile (".config/dawd");
    juce::String configPath = configDefault.getFullPathName();
    // Gepruefte Rezepte (Repo recipes/); die Test-Suite gibt einen eigenen
    // Ordner mit, damit sie das Repo nicht beschreibt
    juce::String recipesPath = resourceDir ("recipes").getFullPathName();

    for (int i = 1; i < argc; ++i)
    {
        juce::String a (argv[i]);

        if (a == "--port" && i + 1 < argc)
            port = juce::String (argv[++i]).getIntValue();
        else if (a == "--log" && i + 1 < argc)
            logPath = juce::String (argv[++i]);
        else if (a == "--ui" && i + 1 < argc)
            uiPath = juce::String (argv[++i]);
        else if (a == "--session" && i + 1 < argc)
            sessionPath = juce::String (argv[++i]);
        else if (a == "--config" && i + 1 < argc)
            configPath = juce::String (argv[++i]);
        else if (a == "--recipes" && i + 1 < argc)
            recipesPath = juce::String (argv[++i]);
        else if (a == "--interface-sim")
            interfaceSim = true;
        else if (a == "--window-sim")
            windowSim = true;
        else if (a == "--no-audio")
            noAudio = true;
    }

    const auto configDir = juce::File::getCurrentWorkingDirectory().getChildFile (configPath);
    // Tracktions Audio-Worker mit niedriger Echtzeitpriorität (dawd/patches/
    // tracktion-linux-worker-priority.patch): unter dem PipeWire-Daemon (20),
    // über allem Normalen — sonst Aussetzer unter CPU-Last. Von außen überschreibbar.
    setenv ("TRACKTION_RT_PRIORITY", "10", 0);

    DawdContext ctx ("dawd", ! noAudio, configDir);
    ctx.logFile = juce::File::getCurrentWorkingDirectory().getChildFile (logPath);
    ctx.configDir = configDir;
    // Gruppierung der Plugin-Sandbox je Plugin (pluginHosting)
    setPluginHostingFile (configDir.getChildFile ("plugin-hosting.json"));
    ctx.recipesDir = juce::File::getCurrentWorkingDirectory().getChildFile (recipesPath);
    ctx.library = std::make_unique<SampleLibrary> (ctx.configDir, kitsDirectory());

    // Audio-Interface: echtes Geraet ueber ALSA, mit --interface-sim eine
    // Simulation (die Suite fasst das Geraet nie an); der Timer laedt den
    // Projekt-Mix, wenn das Geraet erscheint
    ctx.iface = interfaceSim ? makeSimInterface() : makeAlsaInterface();
    // Plugin-Fenster (pluginWindow): echt ueber X11/XWayland oder, mit
    // --window-sim, ohne Bildschirm (die Suite oeffnet nie ein Fenster)
    ctx.windows = std::make_shared<PluginWindows> (windowSim);
    audioStartup (ctx);   // Puffergroesse aus audio.json (--config)
    oscStartup (ctx);     // OSC-Fernsteuerung aus osc.json (--config), Standard aus
    ctx.ifaceTimer = std::make_unique<InterfaceTimer>();
    ctx.ifaceTimer->ctx = &ctx;
    ctx.ifaceTimer->startTimer (2000);

    // Mit --session: das zuletzt offene Projekt wieder oeffnen (Neustart nach
    // Absturz oder Dienst-Neustart). Laeuft als erster Befehl ueber executeOp,
    // steht also im Event-Log; vor dem Server gepostet, kommt kein Request davor.
    if (sessionPath.isNotEmpty())
    {
        ctx.sessionFile = juce::File::getCurrentWorkingDirectory().getChildFile (sessionPath);
        auto dir = readSessionDir (ctx.sessionFile);

        if (dir.isNotEmpty())
            juce::MessageManager::callAsync ([&ctx, dir]
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("op", "treeOpen");
                o->setProperty ("dir", dir);
                o->setProperty ("restored", true);
                auto r = executeOp (ctx, "treeOpen", juce::var (o));
                std::cerr << "[main] session restore " << dir << ": " << jsonText (r) << "\n";
            });
    }

    std::signal (SIGTERM, onStopSignal);
    std::signal (SIGINT, onStopSignal);
    // Bricht ein Client ab, waehrend sein Befehl laeuft (Seite neu geladen,
    // Tab zu, Agent-Timeout), schrieb die Antwort in einen geschlossenen
    // Socket: SIGPIPE beendete dawd. Ohne Signal schlaegt nur das write fehl.
    std::signal (SIGPIPE, SIG_IGN);
    StopSignalTimer stopSignalTimer;
    stopSignalTimer.startTimer (100);

    HttpServer server;

    // Antworten ohne Befehl (UI, Dateien, Upload) — laufen auf dem
    // Antwort-Thread der Verbindung (HttpServer.h)
    auto serveFile = [&] (const juce::String& method, const juce::String& path, const std::string& body) -> HttpReply
    {

        auto pathOnly = path.upToFirstOccurrenceOf ("?", false, false);

        if (method == "GET" && (pathOnly == "/" || pathOnly == "/ui"))
        {
            // UI pro Request aus der Datei lesen — Änderungen ohne Build und Neustart
            auto f = juce::File (uiPath);

            if (! juce::File::isAbsolutePath (uiPath))
                f = juce::File::getCurrentWorkingDirectory().getChildFile (uiPath);

            if (! f.existsAsFile())
                return { "text/html; charset=utf-8",
                         "<html><body><h1>unidaw flow</h1><p>UI not found: "
                             + f.getFullPathName().toStdString()
                             + " (start with --ui &lt;flow.html&gt;)</p></body></html>" };

            // Token fuer die Befehle der Seite (fetch liest das meta-Tag);
            // eine fremde Seite liest die Antwort nicht (Same-Origin, Host)
            auto html = f.loadFileAsString().toStdString();
            auto head = html.find ("<head>");

            if (head != std::string::npos)
                html.insert (head + 6, "\n<meta name=\"dawd-token\" content=\"" + server.token + "\">");

            return { "text/html; charset=utf-8", html };
        }

        if (method == "GET" && pathOnly.startsWith ("/vendor/"))
        {
            // Vendor-Dateien (Web Awesome) aus dem UI-Ordner — Änderungen ohne Build und Neustart
            auto rel = juce::URL::removeEscapeChars (
                        pathOnly.fromFirstOccurrenceOf ("/vendor/", false, false));

            if (rel.isEmpty() || rel.contains (".."))
                return { "application/json", jsonText (resultObj (false, { { "error", "not found" } })).toStdString() };

            auto uiFile = juce::File (uiPath);

            if (! juce::File::isAbsolutePath (uiPath))
                uiFile = juce::File::getCurrentWorkingDirectory().getChildFile (uiPath);

            auto f = uiFile.getParentDirectory().getChildFile ("vendor/" + rel);

            if (! f.existsAsFile())
                return { "application/json", jsonText (resultObj (false, { { "error", "not found" } })).toStdString() };

            auto ext = f.getFileExtension().toLowerCase();
            auto contentType = ext == ".js"   ? "text/javascript; charset=utf-8"
                             : ext == ".css"  ? "text/css; charset=utf-8"
                             : ext == ".json" ? "application/json"
                             : ext == ".svg"  ? "image/svg+xml"
                             : ext == ".woff2" ? "font/woff2"
                             : "application/octet-stream";

            juce::MemoryBlock mb;
            f.loadFileAsData (mb);
            return { contentType, std::string ((const char*) mb.getData(), mb.getSize()) };
        }

        if (method == "GET" && pathOnly == "/health")
            return { "application/json", jsonText (resultObj (true)).toStdString() };

        if (method == "GET" && path.startsWith ("/audio?"))
        {
            auto f = juce::File (juce::URL::removeEscapeChars (
                        path.fromFirstOccurrenceOf ("path=", false, false)));

            if (! f.existsAsFile())
                return { "application/json", jsonText (resultObj (false, { { "error", "not found" } })).toStdString() };

            auto ext = f.getFileExtension().toLowerCase();
            auto contentType = ext == ".wav" ? "audio/wav"
                             : ext == ".mp3" ? "audio/mpeg"
                             : ext == ".ogg" ? "audio/ogg"
                             : ext == ".flac" ? "audio/flac"
                             : ext == ".aif" || ext == ".aiff" ? "audio/aiff"
                             : "application/octet-stream";

            juce::MemoryBlock mb;
            f.loadFileAsData (mb);
            return { contentType, std::string ((const char*) mb.getData(), mb.getSize()) };
        }

        if (method == "POST" && pathOnly == "/upload")
        {
            // Projektordner auf dem Message-Thread lesen (ctx.tree gehoert ihm)
            auto dirPromise = std::make_shared<std::promise<juce::File>>();
            auto dirFuture = dirPromise->get_future();
            juce::MessageManager::callAsync ([&ctx, dirPromise] { dirPromise->set_value (ctx.tree ? ctx.tree->projectDir : juce::File()); });

            if (dirFuture.wait_for (std::chrono::seconds (600)) != std::future_status::ready)
                return { "application/json", jsonText (resultObj (false, { { "error", "timeout" } })).toStdString() };

            auto projectDir = dirFuture.get();

            auto name = juce::File::createLegalFileName (juce::URL::removeEscapeChars (
                            path.fromFirstOccurrenceOf ("name=", false, false)));

            if (name.isEmpty())
                return { "application/json", jsonText (resultObj (false, { { "error", "no file name" } })).toStdString() };

            // Ohne Projekt nur eine Referenz (UI: Analyse vor dem ersten
            // Song) — sie liegt bei den Mitschnitt-Songs, <config>/references/
            auto mediaDir = projectDir == juce::File() ? ctx.configDir.getChildFile ("references")
                                                       : projectDir.getChildFile ("media");

            if (! mediaDir.isDirectory() && ! mediaDir.createDirectory().wasOk())
                return { "application/json", jsonText (resultObj (false, { { "error", "cannot create " + mediaDir.getFileName() + " dir" } })).toStdString() };

            // Dieselbe Datei noch einmal (gleicher Name, gleicher Inhalt): die
            // vorhandene nehmen statt "-2" anzulegen (UI-Nachstell-Lauf 1,
            // remix-1 #1151 "vocals-2")
            auto sameContent = [&body] (const juce::File& f)
            {
                juce::MemoryBlock mb;
                return f.getSize() == (juce::int64) body.size() && f.loadFileAsData (mb)
                    && std::memcmp (mb.getData(), body.data(), body.size()) == 0;
            };

            auto target = mediaDir.getChildFile (name);
            auto stem = target.getFileNameWithoutExtension();
            auto ext = target.getFileExtension();
            int n = 2;

            while (target.existsAsFile() && ! sameContent (target))
                target = mediaDir.getChildFile (stem + "-" + juce::String (n++) + ext);

            if (target.existsAsFile())
                return { "application/json", jsonText (resultObj (true, { { "file", target.getFullPathName() },
                                                                           { "bytes", (juce::int64) body.size() },
                                                                           { "existing", true } })).toStdString() };

            juce::FileOutputStream out (target);

            if (! out.openedOk())
                return { "application/json", jsonText (resultObj (false, { { "error", "cannot write " + target.getFileName() } })).toStdString() };

            out.write (body.data(), body.size());
            out.flush();

            if (out.getStatus().failed())
                return { "application/json", jsonText (resultObj (false, { { "error", "cannot write " + target.getFileName() } })).toStdString() };

            return { "application/json", jsonText (resultObj (true, { { "file", target.getFullPathName() },
                                                                       { "bytes", (juce::int64) body.size() } })).toStdString() };
        }

        return { "application/json", jsonText (resultObj (false, { { "error", "not found" } })).toStdString() };
    };

    // Laeuft auf dem Server-Thread, Anfrage fuer Anfrage (Eingangsreihenfolge):
    // Befehle gehen hier in die Message-Queue, die Antwort wartet auf dem
    // Thread der Verbindung
    server.handle = [&] (const juce::String& method, const juce::String& path, const std::string& body) -> HttpServer::Reply
    {
        auto pathOnly = path.upToFirstOccurrenceOf ("?", false, false);

        // Ladestand (LoadStatus.h): ohne Message-Thread, antwortet auch waehrend
        // ein langer Befehl laeuft; die UI pollt ihn — darum ohne Log-Zeile
        if (method == "GET" && pathOnly == "/progress")
            return [&ctx] { return HttpReply { "application/json", jsonText (ctx.load.snapshot()).toStdString() }; };

        std::cerr << "[server] request " << method << " " << path << "\n";

        if (method == "POST" && pathOnly == "/command")
        {
            auto cmd = juce::JSON::parse (juce::String::fromUTF8 (body.data(), (int) body.size()));
            auto op = cmd.getProperty ("op", {}).toString();

            // Anzeige-Polls (Position, Eingangspegel): cerr-Ausgabe daempfen,
            // sonst spammt er mit ~10 Hz
            const bool quiet = op == "position" || op == "inputLevel" || op == "levels";

            if (! quiet)
                std::cerr << "[server] posting callAsync for " << op << "\n";

            // jobStatus {wait}: hier auf dem Thread der Verbindung warten und
            // den Stand alle 100 ms auf dem Message-Thread abfragen — Jobs in
            // Schritten auf dem Message-Thread (matchSound, tuneSound) liefen
            // sonst waehrend des Wartens nicht weiter, und Befehle der UI
            // stauten sich
            if (op == "jobStatus" && (double) cmd.getProperty ("wait", 0.0) > 0.0)
            {
                const auto waitMs = (juce::uint32) (juce::jlimit (0.0, 60.0, (double) cmd.getProperty ("wait", 0.0)) * 1000.0);
                auto once = juce::JSON::parse (juce::JSON::toString (cmd));
                once.getDynamicObject()->removeProperty ("wait");
                // Ins Log nur die letzte Abfrage (executeOp, waitPoll) — sonst
                // ~10 Zeilen/s (Praxistest remix-1: 1123 in 2 min)
                once.getDynamicObject()->setProperty ("waitPoll", cmd.getProperty ("wait", 0.0));

                return [&ctx, once, waitMs]
                {
                    auto ask = [&ctx] (const juce::var& c)
                    {
                        auto p = std::make_shared<std::promise<juce::String>>();
                        auto f = p->get_future();
                        juce::MessageManager::callAsync ([&ctx, p, c] { p->set_value (jsonText (executeOp (ctx, "jobStatus", c))); });

                        if (f.wait_for (std::chrono::seconds (600)) != std::future_status::ready)
                            return jsonText (resultObj (false, { { "error", "timeout" } }));

                        return f.get();
                    };

                    auto c = once;
                    const auto end = juce::Time::getMillisecondCounter() + waitMs;

                    for (;;)
                    {
                        const bool last = juce::Time::getMillisecondCounter() >= end;

                        if (last)
                        {
                            c = juce::JSON::parse (juce::JSON::toString (c));
                            c.getDynamicObject()->setProperty ("waitLast", true);
                        }

                        auto text = ask (c);
                        auto r = juce::JSON::parse (text);

                        if (! (bool) r.getProperty ("ok", false) || r.hasProperty ("jobs") || r.getProperty ("status", {}).toString() != "running"
                            || last)
                            return HttpReply { "application/json", text.toStdString() };

                        if (c.hasProperty ("cancel"))
                        {
                            c = juce::JSON::parse (juce::JSON::toString (c));
                            c.getDynamicObject()->removeProperty ("cancel");
                        }

                        std::this_thread::sleep_for (std::chrono::milliseconds (100));
                    }
                };
            }

            // JSON auf dem Message-Thread: Ergebnisse koennen Objekte des
            // Kontexts teilen (Aktions-params), die der naechste Befehl aendert
            auto promise = std::make_shared<std::promise<std::string>>();
            auto future = std::make_shared<std::future<std::string>> (promise->get_future());

            juce::MessageManager::callAsync ([&ctx, promise, op, cmd]
                                             { promise->set_value (jsonText (executeOp (ctx, op, cmd)).toStdString()); });

            return [future, op, quiet]
            {
                if (future->wait_for (std::chrono::seconds (600)) != std::future_status::ready)
                    return HttpReply { "application/json", jsonText (resultObj (false, { { "error", "timeout" } })).toStdString() };

                if (! quiet)
                    std::cerr << "[server] future ready for " << op << "\n";

                return HttpReply { "application/json", future->get() };
            };
        }

        return [=] { return serveFile (method, path, body); };
    };

    if (! server.start (port))
    {
        std::cerr << "cannot listen on port " << port << "\n";
        return 1;
    }

    // Token pro Sitzung, fuer Skripte und MCP in <config>/api-token-<port>;
    // erst nach dem Listen geschrieben (ein zweiter Start auf belegtem Port
    // ueberschreibt die Datei der laufenden Instanz nicht), vor dem ersten
    // angenommenen Request (server.run)
    const auto tokenFile = configDir.getChildFile ("api-token-" + juce::String (port));
    server.token = sessionToken();
    server.tokenFile = tokenFile.getFullPathName();

    if (server.token.empty() || ! writeTokenFile (tokenFile, server.token))
    {
        std::cerr << "cannot write the API token " << tokenFile.getFullPathName() << "\n";
        return 1;
    }

    std::thread serverThread ([&] { server.run(); });

    std::cout << "unidaw listening on 127.0.0.1:" << port << ", log: "
              << ctx.logFile.getFullPathName() << ", token: " << tokenFile.getFullPathName() << "\n" << std::flush;

    std::cerr << "[main] entering runDispatchLoop\n";
    juce::MessageManager::getInstance()->runDispatchLoop();
    std::cerr << "[main] runDispatchLoop returned\n";

    oscShutdown();   // kein OSC-Befehl mehr nach dem Dispatch-Loop

    // Ausstehende Mutationen sichern (Signal-Stopp; quit hat schon geflusht)
    flushAutosave (ctx);
    clearMeters (ctx);
    audioShutdown (ctx);   // PipeWire-Vorgaben (force-rate/-quantum) loesen

    server.stop();
    serverThread.join();
    tokenFile.deleteFile();   // der Token gilt nur fuer diese Sitzung

    ctx.edit.reset();

    return 0;
}
