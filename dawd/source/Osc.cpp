#include "Osc.h"
#include "OpRegistry.h"
#include "EngineHelpers.h"
#include "Routing.h"
#include "PluginSettings.h"

#include <atomic>
#include <cstring>
#include <mutex>

// OSC-Fernsteuerung (Osc.h). Adressschema unter /unidaw/, Namen im Pfad (OSC
// verbietet Leerzeichen in Adressen: "_" steht fuer ein Leerzeichen, Spuren
// auch per Index). Nur diese Abbildung, keine generische Op-Durchreiche:
// OSC hat keine Anmeldung, also nur Transport, Mix und Parameter — nichts,
// was Dateien liest/schreibt, Projekte oeffnet oder den Daemon beendet.
namespace dawd
{
    namespace
    {
        constexpr int defaultPort = 9000;
        constexpr int faderMs = 60;   // wie die Fader der UI (mxSlider): hoechstens ein Befehl je 60 ms

        // UI-Fader (flow.html: MX_FLOOR, MX_TOP, mxDbOf): 0..1 linear in dB von
        // faderFloorDb bis faderTopDb, ganz unten -100 dB (= aus). Nur hier;
        // osc_flow prueft, dass flow.html dieselben Werte hat.
        constexpr double faderFloorDb = -60.0, faderTopDb = 6.0, faderOffDb = -100.0;

        double faderDb (double x)
        {
            return x <= 0.0 ? faderOffDb : faderFloorDb + x * (faderTopDb - faderFloorDb);
        }

        juce::var addressList()
        {
            juce::Array<juce::var> l;

            for (auto a : { "/unidaw/play", "/unidaw/stop", "/unidaw/play/from <seconds>", "/unidaw/loop <0|1>",
                            "/unidaw/section/<name>", "/unidaw/marker/<name>",
                            "/unidaw/track/<name|index>/volume <dB -100..6>", "/unidaw/track/<name|index>/fader <0..1>",
                            "/unidaw/track/<name|index>/pan <-1..1>",
                            "/unidaw/track/<name|index>/mute <0|1>", "/unidaw/track/<name|index>/solo <0|1>",
                            "/unidaw/plugin/<module>/<parameter> <0..1>" })
                l.add (juce::String (a));

            return l;
        }

        // Fader-Adressen: Werte je Adresse buendeln, nur der letzte zaehlt
        bool isFader (const juce::StringArray& p)
        {
            return p.size() == 4 && ((p[1] == "track" && (p[3] == "volume" || p[3] == "fader" || p[3] == "pan")) || p[1] == "plugin");
        }

        juce::StringArray partsOf (const juce::String& address)
        {
            auto p = juce::StringArray::fromTokens (address, "/", "");
            p.removeEmptyStrings();
            return p;
        }

        bool isNumber (const juce::OSCArgument& a) { return a.isInt32() || a.isFloat32(); }
        double numberOf (const juce::OSCArgument& a) { return a.isInt32() ? (double) a.getInt32() : (double) a.getFloat32(); }

        // Knopf: ohne Argument oder mit Zahl != 0 ausloesen; 0 ist das
        // Loslassen eines Tasters (TouchOSC & Co. senden 1, dann 0) — ignorieren
        juce::String trigger (const juce::OSCMessage& m, bool& fire)
        {
            fire = true;

            if (m.isEmpty())
                return {};

            if (! isNumber (m[0]))
                return "expects no argument or a number";

            fire = numberOf (m[0]) != 0.0;
            return {};
        }

        juce::String number (const juce::OSCMessage& m, double& v)
        {
            if (m.isEmpty() || ! isNumber (m[0]))
                return "expects a number (int or float)";

            v = numberOf (m[0]);
            return {};
        }

        juce::String spaced (const juce::String& s) { return s.replaceCharacter ('_', ' '); }

        int trackFor (te::Edit& edit, const juce::String& ref)
        {
            auto i = trackIndexFor (edit, ref);
            return i >= 0 || ! ref.containsChar ('_') ? i : trackIndexFor (edit, spaced (ref));
        }

        // Start einer Section bzw. Zeit eines Markers; Namen ohne Gross-/Kleinschreibung
        bool sectionStart (te::Edit& edit, const juce::String& ref, double& t)
        {
            for (auto* s : sortedSections (edit))
                if (s->getName().equalsIgnoreCase (ref) || s->getName().equalsIgnoreCase (spaced (ref)))
                {
                    t = s->getPosition().getStart().inSeconds();
                    return true;
                }

            return false;
        }

        bool markerTime (te::Edit& edit, const juce::String& ref, double& t)
        {
            for (auto& m : sortedMarkers (edit))
                if (m.name.equalsIgnoreCase (ref) || m.name.equalsIgnoreCase (spaced (ref)))
                {
                    t = m.time;
                    return true;
                }

            return false;
        }

        juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
        {
            auto o = new juce::DynamicObject();

            for (auto& [k, v] : props)
                o->setProperty (k, v);

            return juce::var (o);
        }

        // OSC-Nachricht -> Op + Befehl. Fehlertext oder ""; "" mit leerem op =
        // bewusst ignoriert (losgelassener Taster)
        juce::String translate (DawdContext& ctx, const juce::OSCMessage& m, juce::String& op, juce::var& cmd)
        {
            auto p = partsOf (m.getAddressPattern().toString());
            const auto n = p.size();

            if (n < 2 || p[0] != "unidaw")
                return "unknown address (oscSettings lists the addresses)";

            const auto& what = p[1];
            bool fire = true;
            double v = 0.0;

            if (n == 2 && (what == "play" || what == "stop"))
            {
                if (auto e = trigger (m, fire); e.isNotEmpty() || ! fire)
                    return e;

                op = "play";
                cmd = object ({ { "on", what == "play" } });
                return {};
            }

            if (n == 3 && what == "play" && p[2] == "from")
            {
                if (auto e = number (m, v); e.isNotEmpty())
                    return e;

                op = "play";
                cmd = object ({ { "on", true }, { "from", v } });
                return {};
            }

            if (n == 2 && what == "loop")
            {
                if (auto e = number (m, v); e.isNotEmpty())
                    return e;

                op = "loop";
                cmd = object ({ { "on", v != 0.0 } });
                return {};
            }

            // Ab hier mit Namen im Pfad: die brauchen einen Edit
            if ((n == 3 && (what == "section" || what == "marker")) || (n == 4 && (what == "track" || what == "plugin")))
                if (ctx.edit == nullptr)
                    return "no edit";

            if (n == 3 && (what == "section" || what == "marker"))
            {
                if (auto e = trigger (m, fire); e.isNotEmpty() || ! fire)
                    return e;

                double t = 0.0;

                if (what == "section" ? ! sectionStart (*ctx.edit, p[2], t) : ! markerTime (*ctx.edit, p[2], t))
                    return "no such " + what + ": " + p[2];

                op = "play";
                cmd = object ({ { "on", true }, { "from", t } });
                return {};
            }

            if (n == 4 && what == "track")
            {
                auto idx = trackFor (*ctx.edit, p[2]);

                if (idx < 0)
                    return "no such track: " + p[2];

                if (auto e = number (m, v); e.isNotEmpty())
                    return e;

                const auto& key = p[3];

                if (key == "volume")
                    cmd = object ({ { "track", idx }, { "volumeDb", v } });
                else if (key == "fader")
                {
                    if (v < 0.0 || v > 1.0)
                        return "fader must be 0..1 (0 = off, 1 = +6 dB, like the mixer fader)";

                    cmd = object ({ { "track", idx }, { "volumeDb", faderDb (v) } });
                }
                else if (key == "pan")
                    cmd = object ({ { "track", idx }, { "pan", v } });
                else if (key == "mute" || key == "solo")
                    cmd = object ({ { "track", idx }, { key.toRawUTF8(), v != 0.0 } });
                else
                    return "unknown track control: " + key + " (volume, fader, pan, mute, solo)";

                op = "trackMix";
                return {};
            }

            if (n == 4 && what == "plugin")
            {
                auto id = moduleIdFor (*ctx.edit, p[2]);

                if (id.isEmpty() && p[2].containsChar ('_'))
                    id = moduleIdFor (*ctx.edit, spaced (p[2]));

                auto* mod = id.isNotEmpty() ? findModuleTrack (*ctx.edit, id) : nullptr;

                if (mod == nullptr)
                    return "no such plugin: " + p[2];

                if (auto e = number (m, v); e.isNotEmpty())
                    return e;

                // Parametername: so, sonst mit Leerzeichen statt "_"; unbekannt
                // meldet setParameter selbst
                auto name = p[3];

                if (auto* plugin = modulePlugin (*mod); plugin != nullptr && findParameter (*plugin, name) == nullptr
                                                         && findParameter (*plugin, spaced (name)) != nullptr)
                    name = spaced (name);

                op = "setParameter";
                cmd = object ({ { "plugin", id }, { "name", name }, { "value", v } });
                return {};
            }

            return "unknown address (oscSettings lists the addresses)";
        }

        bool isIPv4 (const juce::String& s)
        {
            auto parts = juce::StringArray::fromTokens (s, ".", "");

            if (parts.size() != 4)
                return false;

            for (auto& p : parts)
                if (p.isEmpty() || p.length() > 3 || ! p.containsOnly ("0123456789") || p.getIntValue() > 255)
                    return false;

            return true;
        }

        // OSC 1.0 zerlegen (JUCEs Zerleger ist nicht oeffentlich, und sein
        // OSCReceiver nennt den Absender nicht): Nachricht oder Bundle (auch
        // geschachtelt, Zeitstempel ignoriert). Argumente i, f, s/S, d, h,
        // T, F; d/h werden float32/int32. Fehlertext oder "".
        struct OscReader
        {
            const char* p;
            const char* end;

            bool has (size_t n) const { return (size_t) (end - p) >= n; }

            bool str (juce::String& out)
            {
                auto* z = static_cast<const char*> (std::memchr (p, 0, (size_t) (end - p)));

                if (z == nullptr)
                    return false;

                out = juce::String::fromUTF8 (p, (int) (z - p));
                auto len = (size_t) (z - p) + 1;
                len = (len + 3) & ~(size_t) 3;

                if (! has (len))
                    return false;

                p += len;
                return true;
            }

            template <typename T>
            bool bigEndian (T& out)
            {
                if (! has (sizeof (T)))
                    return false;

                uint64_t v = 0;

                for (size_t i = 0; i < sizeof (T); ++i)
                    v = (v << 8) | (uint8_t) p[i];

                p += sizeof (T);

                if constexpr (sizeof (T) == 4)
                {
                    auto u = (uint32_t) v;
                    std::memcpy (&out, &u, 4);
                }
                else
                {
                    std::memcpy (&out, &v, 8);
                }

                return true;
            }
        };

        juce::String parseOsc (const char* data, size_t size, std::vector<juce::OSCMessage>& out, int depth = 0)
        {
            OscReader r { data, data + size };

            if (size >= 8 && std::memcmp (data, "#bundle", 8) == 0)
            {
                if (depth > 8 || size < 16)
                    return "bad bundle";

                r.p += 16;   // "#bundle\0" + Zeitstempel

                while (r.p < r.end)
                {
                    int32_t n = 0;

                    if (! r.bigEndian (n) || n <= 0 || ! r.has ((size_t) n))
                        return "bad bundle element";

                    if (auto e = parseOsc (r.p, (size_t) n, out, depth + 1); e.isNotEmpty())
                        return e;

                    r.p += n;
                }

                return {};
            }

            juce::String address, tags;

            if (! r.str (address) || ! address.startsWithChar ('/'))
                return "bad OSC message";

            try
            {
                juce::OSCMessage m { juce::OSCAddressPattern (address) };

                if (r.p < r.end)
                {
                    if (! r.str (tags) || ! tags.startsWithChar (','))
                        return "bad OSC type tags";

                    for (int i = 1; i < tags.length(); ++i)
                    {
                        const auto t = tags[i];
                        bool ok = true;

                        if (t == 'i') { int32_t v = 0; ok = r.bigEndian (v); m.addInt32 (v); }
                        else if (t == 'f') { float v = 0; ok = r.bigEndian (v); m.addFloat32 (v); }
                        else if (t == 'd') { double v = 0; ok = r.bigEndian (v); m.addFloat32 ((float) v); }
                        else if (t == 'h') { int64_t v = 0; ok = r.bigEndian (v); m.addInt32 ((int32_t) juce::jlimit<int64_t> (INT32_MIN, INT32_MAX, v)); }
                        else if (t == 's' || t == 'S') { juce::String v; ok = r.str (v); m.addString (v); }
                        else if (t == 'T' || t == 'F') m.addInt32 (t == 'T' ? 1 : 0);
                        else return "unsupported OSC argument type '" + juce::String::charToString (t) + "'";

                        if (! ok)
                            return "OSC message too short";
                    }
                }

                out.push_back (m);
                return {};
            }
            catch (const juce::OSCFormatError& e)
            {
                return "bad OSC address: " + juce::String (e.description);
            }
        }

        struct OscControl;

        // Empfangs-Thread: liest Pakete samt Absender, verwirft fremde
        // Absender (nur gezaehlt), zerlegt das Paket und reicht die
        // Nachrichten an den Message-Thread. Liste der Absender fest je Thread
        // (oscSettings oeffnet neu).
        struct OscListener : public juce::Thread
        {
            OscListener (std::unique_ptr<juce::DatagramSocket> s, juce::StringArray allowList, juce::WeakReference<OscControl> c)
                : juce::Thread ("dawd osc"), socket (std::move (s)), allow (std::move (allowList)), owner (c)
            {
                allow.addIfNotAlreadyThere ("127.0.0.1");   // dieser Rechner immer
            }

            ~OscListener() override { stopThread (2000); }

            void run() override;

            std::unique_ptr<juce::DatagramSocket> socket;
            juce::StringArray allow;
            juce::WeakReference<OscControl> owner;
            std::atomic<int> rejected { 0 };
            std::mutex lastLock;
            juce::String lastRejected;
        };

        struct OscControl : private juce::Timer
        {
            explicit OscControl (DawdContext& c) : ctx (c) {}

            ~OscControl() override
            {
                stopTimer();
                close();
            }

            // Lauschen auf address:port; "" = ok. Ohne Port-Wiederverwendung
            // (JUCE setzt sie fuer UDP): ein belegter Port ist ein Fehler, statt
            // dass zwei Instanzen sich die Pakete teilen
            juce::String open (const juce::String& addr, int p, const juce::StringArray& allowList)
            {
                close();
                auto s = std::make_unique<juce::DatagramSocket> (false);
                s->setEnablePortReuse (false);

                if (! s->bindToPort (p, addr))
                    return "cannot listen on " + addr + ":" + juce::String (p) + " (port in use or address not on this computer)";

                listener = std::make_unique<OscListener> (std::move (s), allowList, this);
                listener->startThread();
                return {};
            }

            void close()
            {
                if (listener != nullptr)
                {
                    // Zaehler der Absender-Sperre ueber das Neu-Oeffnen behalten
                    rejectedBefore += listener->rejected.load();
                    std::lock_guard<std::mutex> l (listener->lastLock);

                    if (listener->lastRejected.isNotEmpty())
                        lastRejectedBefore = listener->lastRejected;
                }

                listener.reset();
            }

            bool listening() const { return listener != nullptr; }

            juce::var status()
            {
                juce::Array<juce::var> al;

                for (auto& a : allow)
                    al.add (a);

                auto rej = rejectedBefore;
                auto lastRej = lastRejectedBefore;

                if (listener != nullptr)
                {
                    rej += listener->rejected.load();
                    std::lock_guard<std::mutex> l (listener->lastLock);

                    if (listener->lastRejected.isNotEmpty())
                        lastRej = listener->lastRejected;
                }

                auto r = resultObj (true, { { "enabled", enabled }, { "address", address }, { "port", port },
                                            { "allow", al }, { "listening", listening() }, { "received", received },
                                            { "ran", ran }, { "failed", failed }, { "rejected", rej },
                                            { "addresses", addressList() } });

                if (lastRej.isNotEmpty())
                    r.getDynamicObject()->setProperty ("lastRejected", lastRej);

                if (error.isNotEmpty())
                    r.getDynamicObject()->setProperty ("error", error);

                if (last.isObject())
                    r.getDynamicObject()->setProperty ("last", last);

                return r;
            }

            // Vom Empfangs-Thread ueber die Message-Queue: zerlegtes Paket
            void deliver (const std::vector<juce::OSCMessage>& msgs, const juce::String& parseError)
            {
                if (parseError.isNotEmpty())
                {
                    ++received;
                    ++failed;
                    auto l = new juce::DynamicObject();
                    l->setProperty ("ok", false);
                    l->setProperty ("error", parseError);
                    last = juce::var (l);
                    return;
                }

                for (auto& m : msgs)
                    add (m);

                kick();
            }

            DawdContext& ctx;
            std::unique_ptr<OscListener> listener;
            bool enabled = false;
            juce::String address { "127.0.0.1" };
            int port = defaultPort;
            juce::StringArray allow;   // erlaubte Absender ausser 127.0.0.1
            juce::String error;   // Lauschen beim Start fehlgeschlagen
            int received = 0, ran = 0, failed = 0, rejectedBefore = 0;
            juce::String lastRejectedBefore;
            juce::var last;

        private:
            struct Pending
            {
                juce::OSCMessage msg;
                bool fader;
            };

            std::vector<Pending> queue;

            void add (const juce::OSCMessage& m)
            {
                ++received;
                const bool fader = isFader (partsOf (m.getAddressPattern().toString()));

                if (fader)
                    for (auto& q : queue)
                        if (q.fader && q.msg.getAddressPattern().toString() == m.getAddressPattern().toString())
                        {
                            q.msg = m;
                            return;
                        }

                queue.push_back ({ m, fader });
            }

            // Knoepfe sofort (mit allem, was davor kam), Fader gebuendelt
            void kick()
            {
                if (std::any_of (queue.begin(), queue.end(), [] (const Pending& q) { return ! q.fader; }))
                    flush();
                else if (! isTimerRunning())
                    startTimer (faderMs);
            }

            void timerCallback() override { flush(); }

            void flush()
            {
                // Laeuft gerade eine Op (verschachtelte Message-Loop, z. B.
                // loadPreset/Plugin-Laden), nicht mitten hinein: spaeter
                if ((bool) ctx.load.snapshot().getProperty ("busy", false))
                {
                    startTimer (10);
                    return;
                }

                stopTimer();
                auto items = std::move (queue);
                queue.clear();

                for (auto& q : items)
                    run (q.msg);
            }

            void run (const juce::OSCMessage& m)
            {
                const auto addr = m.getAddressPattern().toString();
                juce::String op;
                juce::var cmd;
                auto err = translate (ctx, m, op, cmd);
                auto l = new juce::DynamicObject();
                l->setProperty ("address", addr);

                if (err.isEmpty() && op.isEmpty())
                {
                    l->setProperty ("ignored", true);
                }
                else if (err.isNotEmpty())
                {
                    ++failed;
                    l->setProperty ("ok", false);
                    l->setProperty ("error", err);
                    std::cerr << "[osc] " << addr << ": " << err << "\n";
                }
                else
                {
                    cmd.getDynamicObject()->setProperty ("osc", addr);   // Herkunft im Event-Log; ein Undo-Schritt je Geste
                    auto r = executeOp (ctx, op, cmd);
                    ++ran;
                    const bool ok = (bool) r.getProperty ("ok", false);
                    l->setProperty ("op", op);
                    l->setProperty ("ok", ok);

                    if (! ok)
                    {
                        ++failed;
                        l->setProperty ("error", r.getProperty ("error", {}));
                    }
                }

                last = juce::var (l);
            }

            JUCE_DECLARE_WEAK_REFERENCEABLE (OscControl)
        };

        void OscListener::run()
        {
            juce::HeapBlock<char> buffer (65536);

            while (! threadShouldExit())
            {
                auto ready = socket->waitUntilReady (true, 100);

                if (ready < 0 || threadShouldExit())
                    return;

                if (ready == 0)
                    continue;

                juce::String sender;
                int senderPort = 0;
                auto n = socket->read (buffer.getData(), 65536, false, sender, senderPort);

                if (n <= 0)
                    continue;

                if (! allow.contains (sender))
                {
                    ++rejected;
                    std::lock_guard<std::mutex> l (lastLock);
                    lastRejected = sender;
                    continue;
                }

                std::vector<juce::OSCMessage> msgs;
                auto err = parseOsc (buffer.getData(), (size_t) n, msgs);
                auto target = owner;

                juce::MessageManager::callAsync ([target, msgs = std::move (msgs), err]
                {
                    if (auto* c = target.get())
                        c->deliver (msgs, err);
                });
            }
        }

        std::unique_ptr<OscControl>& control()
        {
            static std::unique_ptr<OscControl> c;
            return c;
        }

        juce::File settingsFile (DawdContext& ctx) { return ctx.configDir.getChildFile ("osc.json"); }

        juce::var fail (const juce::String& e) { return resultObj (false, { { "error", e } }); }

        // allow: Liste von IPv4-Adressen (ohne 0.0.0.0); Fehlertext oder ""
        juce::String readAllow (const juce::var& v, juce::StringArray& out)
        {
            const juce::String err = "allow must be a list of IPv4 sender addresses, e.g. [\"192.168.1.20\"] "
                                     "(127.0.0.1 is always allowed, [] = only this computer)";

            if (! v.isArray() || v.size() > 64)
                return err;

            for (auto& a : *v.getArray())
            {
                auto s = a.toString().trim();

                if (! a.isString() || ! isIPv4 (s) || s == "0.0.0.0")
                    return err;

                if (s != "127.0.0.1")
                    out.addIfNotAlreadyThere (s);
            }

            return {};
        }
    }

    void oscStartup (DawdContext& ctx)
    {
        auto& c = control();
        c = std::make_unique<OscControl> (ctx);

        auto json = juce::JSON::parse (settingsFile (ctx));
        auto addr = json.getProperty ("address", "127.0.0.1").toString();
        auto port = (int) json.getProperty ("port", defaultPort);
        juce::StringArray allow;

        if (isIPv4 (addr))
            c->address = addr;

        if (port >= 1024 && port <= 65535)
            c->port = port;

        if (json.hasProperty ("allow") && readAllow (json.getProperty ("allow", {}), allow).isEmpty())
            c->allow = allow;

        c->enabled = (bool) json.getProperty ("enabled", false);

        if (c->enabled)
        {
            c->error = c->open (c->address, c->port, c->allow);

            if (c->error.isNotEmpty())
                std::cerr << "[osc] " << c->error << "\n";
            else
                std::cerr << "[osc] listening on " << c->address << ":" << c->port << "\n";
        }
    }

    void oscShutdown()
    {
        control().reset();
    }

    juce::var oscSettings (DawdContext& ctx, const juce::var& cmd)
    {
        auto& cp = control();

        if (cp == nullptr)
            return fail ("OSC not started");

        auto& c = *cp;
        auto enabledV = cmd.getProperty ("enabled", c.enabled);
        auto portV = cmd.getProperty ("port", c.port);
        auto addrV = cmd.getProperty ("address", c.address);
        auto allow = c.allow;

        if (! enabledV.isBool() && ! enabledV.isInt())
            return fail ("enabled must be true or false");

        if (! (portV.isInt() || portV.isInt64() || (portV.isDouble() && (double) portV == std::floor ((double) portV)))
            || (int) portV < 1024 || (int) portV > 65535)
            return fail ("port must be a whole number 1024..65535");

        if (! addrV.isString() || ! isIPv4 (addrV.toString().trim()))
            return fail ("address must be an IPv4 address: 127.0.0.1 (this computer only, default), "
                         "an address of this computer, or 0.0.0.0 (all networks; only senders in allow get through)");

        if (cmd.hasProperty ("allow"))
        {
            allow.clear();

            if (auto err = readAllow (cmd.getProperty ("allow", {}), allow); err.isNotEmpty())
                return fail (err);
        }

        const bool enabled = (bool) enabledV;
        const int port = (int) portV;
        const auto addr = addrV.toString().trim();

        if (enabled != c.enabled || port != c.port || addr != c.address || allow != c.allow || (enabled && ! c.listening()))
        {
            const bool wasListening = c.listening();

            if (enabled)
            {
                if (auto err = c.open (addr, port, allow); err.isNotEmpty())
                {
                    if (wasListening)
                        c.open (c.address, c.port, c.allow);   // alter Stand bleibt

                    return fail (err);
                }
            }
            else
            {
                c.close();
            }

            auto o = new juce::DynamicObject();
            juce::Array<juce::var> al;

            for (auto& a : allow)
                al.add (a);

            o->setProperty ("enabled", enabled);
            o->setProperty ("address", addr);
            o->setProperty ("port", port);
            o->setProperty ("allow", al);

            if (! settingsFile (ctx).replaceWithText (juce::JSON::toString (juce::var (o))))
            {
                // nicht gespeichert: alter Stand
                c.close();

                if (c.enabled)
                    c.open (c.address, c.port, c.allow);

                return fail ("cannot write " + settingsFile (ctx).getFullPathName());
            }

            c.enabled = enabled;
            c.port = port;
            c.address = addr;
            c.allow = allow;
            c.error = {};
        }

        return c.status();
    }
}
