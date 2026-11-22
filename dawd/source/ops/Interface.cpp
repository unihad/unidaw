#include "Ops.h"

#include "../Interface.h"

#include <cmath>

// Audio-Interface (Familie interface): Hardware-Monitoring des Fireface UC —
// Matrix, Fader, Eingangs- und Ausgangsregler. Kein Teil des Edits: kein
// Undo, keine Node-Aktion, kein Auto-Branch. Mit offenem Projekt folgt
// interface.json im Projektordner jeder Aenderung des Monitor-Mixes (Matrix
// und Fader, pro Song); treeOpen und das Einstecken des Geraets laden ihn aufs
// Geraet. Ausgangspegel und Gains stellt auch der Drehregler am Geraet — sie
// gehoeren dem Geraet und kommen nicht mit dem Song (sonst ueberschriebe jedes
// Oeffnen eines Songs die Kopfhoererlautstaerke).
namespace dawd
{
    namespace
    {
        InterfaceBackend& backend (DawdContext& ctx)
        {
            if (ctx.iface == nullptr)
                ctx.iface = makeAlsaInterface();

            return *ctx.iface;
        }

        bool readModel (DawdContext& ctx, InterfaceModel& m)
        {
            std::vector<IfControl> controls;

            if (! backend (ctx).read (controls))
                return false;

            m = InterfaceModel::build (std::move (controls));
            return true;
        }

        juce::var dbVar (double db)
        {
            if (std::isinf (db))
                return "off";

            return std::round (db * 10.0) / 10.0;
        }

        juce::File mixFile (DawdContext& ctx)
        {
            return ctx.tree ? ctx.tree->projectDir.getChildFile ("interface.json") : juce::File();
        }

        // Gespeicherter Projekt-Mix (Reglername -> Stufe, nur Matrix und Fader —
        // aeltere Dateien enthalten auch Pegel/Gains); false = keiner
        bool loadStored (DawdContext& ctx, std::map<juce::String, int>& values)
        {
            auto f = mixFile (ctx);

            if (! f.existsAsFile())
                return false;

            auto json = juce::JSON::parse (f);
            auto* obj = json.getProperty ("values", {}).getDynamicObject();

            if (obj == nullptr)
                return false;

            for (auto& p : obj->getProperties())
                if (InterfaceModel::isMixControl (p.name.toString()))
                    values[p.name.toString()] = (int) p.value;

            return true;
        }

        bool storeSnapshot (DawdContext& ctx, const InterfaceModel& m)
        {
            auto f = mixFile (ctx);

            if (f == juce::File())
                return false;

            auto values = new juce::DynamicObject();

            for (auto& c : m.controls)
                if (InterfaceModel::isMixControl (c.name))
                    values->setProperty (c.name, c.value);

            auto o = new juce::DynamicObject();
            o->setProperty ("device", backend (ctx).deviceName());
            o->setProperty ("values", juce::var (values));
            return f.replaceWithText (juce::JSON::toString (juce::var (o)));
        }

        // Werte schreiben; onlyChanged: nur abweichende (Projekt laden) — sonst
        // alle, ein gleicher Wert stoesst einen Resync des Geraets an
        juce::String applyValues (DawdContext& ctx, const InterfaceModel& m,
                                  const std::map<juce::String, int>& values, bool onlyChanged, int& written)
        {
            std::vector<std::pair<juce::String, int>> w;

            for (auto& [n, v] : values)
                if (auto* c = m.find (n))
                    if (! onlyChanged || c->value != v)
                        w.push_back ({ n, juce::jlimit (c->min, c->max, v) });

            written = (int) w.size();
            return w.empty() ? juce::String() : backend (ctx).write (w);
        }

        int countDiffs (const InterfaceModel& m, const std::map<juce::String, int>& stored)
        {
            int n = 0;

            for (auto& c : m.controls)
                if (auto it = stored.find (c.name); it != stored.end() && it->second != c.value)
                    ++n;

            return n;
        }

        juce::var stateOf (DawdContext& ctx, const InterfaceModel& m)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("present", true);
            o->setProperty ("device", backend (ctx).deviceName());
            o->setProperty ("simulated", backend (ctx).simulated());

            auto arr = [] (const juce::StringArray& a)
            {
                juce::Array<juce::var> v;

                for (auto& s : a)
                    v.add (s);

                return juce::var (v);
            };

            o->setProperty ("outputs", arr (m.outputs));
            o->setProperty ("outputPairs", arr (m.pairs (true)));
            o->setProperty ("sources", arr (m.sources));
            o->setProperty ("sourcePairs", arr (m.pairs (false)));

            // Nur geroutete Kreuzpunkte — die Matrix hat 648
            juce::Array<juce::var> routes;
            auto faders = new juce::DynamicObject();
            auto levels = new juce::DynamicObject();

            for (auto& out : m.outputs)
            {
                for (auto& src : m.sources)
                    if (auto* c = m.find (InterfaceModel::matrixName (out, src)))
                        if (c->value != InterfaceModel::muteValue (*c))
                        {
                            auto r = new juce::DynamicObject();
                            r->setProperty ("out", out);
                            r->setProperty ("src", src);
                            r->setProperty ("db", dbVar (InterfaceModel::toDb (*c, c->value)));
                            routes.add (juce::var (r));
                        }

                if (auto* f = m.find (InterfaceModel::faderName (out)))
                    faders->setProperty (out, dbVar (InterfaceModel::toDb (*f, f->value)));

                if (auto n = m.outputLevelName (out); n.isNotEmpty())
                    if (auto* c = m.find (n))
                        levels->setProperty (out, dbVar (InterfaceModel::toDb (*c, c->value)));
            }

            o->setProperty ("routes", routes);
            o->setProperty ("faders", juce::var (faders));
            o->setProperty ("outputLevels", juce::var (levels));

            // Eingangsregler je Quelle (Gain, 48 V, Hi-Z, PAD — was es gibt)
            juce::Array<juce::var> inputs;

            for (auto& src : m.sources)
            {
                auto in = new juce::DynamicObject();
                in->setProperty ("input", src);

                if (auto* g = m.find (src + " Capture Volume"))
                {
                    in->setProperty ("gainDb", dbVar (InterfaceModel::toDb (*g, g->value)));
                    in->setProperty ("gainMinDb", InterfaceModel::toDb (*g, g->min));
                    in->setProperty ("gainMaxDb", InterfaceModel::toDb (*g, g->max));
                }

                if (auto* c = m.find (src + " 48V Phantom Power Switch"))
                    in->setProperty ("phantom", c->value != 0);

                if (auto* c = m.find (src + " Hi-Z Switch"))
                    in->setProperty ("hiZ", c->value != 0);

                if (auto* c = m.find (src + " PAD Switch"))
                    in->setProperty ("pad", c->value != 0);

                if (in->getProperties().size() == 1)
                {
                    delete in;
                    continue;
                }

                inputs.add (juce::var (in));
            }

            o->setProperty ("inputs", inputs);

            auto proj = new juce::DynamicObject();
            std::map<juce::String, int> stored;
            auto hasStored = loadStored (ctx, stored);
            proj->setProperty ("open", ctx.tree.has_value());
            proj->setProperty ("stored", hasStored);
            proj->setProperty ("differs", hasStored ? countDiffs (m, stored) : 0);
            o->setProperty ("project", juce::var (proj));

            o->setProperty ("ok", true);
            return juce::var (o);
        }

        // Pegel-Angabe: Zahl (dB) oder "off"/null/fehlend = stumm
        bool isOff (const juce::var& db)
        {
            return db.isVoid() || db.isUndefined() || db.toString() == "off" || (db.isDouble() && std::isinf ((double) db));
        }

        int stepFor (const IfControl& c, const juce::var& db)
        {
            if (isOff (db))
            {
                auto mute = InterfaceModel::muteValue (c);
                return mute >= 0 ? mute : c.min;
            }

            return InterfaceModel::fromDb (c, (double) db);
        }

        juce::var fail (const juce::String& e) { return resultObj (false, { { "error", e } }); }

        bool isNumber (const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }
    }

    void syncInterfaceProject (DawdContext& ctx)
    {
        // Ohne Projekt nichts zu laden; ein Projekt laedt einmal je
        // Einstecken (Token) — abgezogen bleibt der Token stehen, das naechste
        // Einstecken bringt einen neuen
        if (! ctx.tree)
        {
            ctx.ifaceAppliedToken = {};
            return;
        }

        auto tok = backend (ctx).token();

        if (tok.isEmpty() || tok == ctx.ifaceAppliedToken)
            return;

        ctx.ifaceAppliedToken = tok;
        std::map<juce::String, int> stored;
        InterfaceModel m;

        if (! loadStored (ctx, stored) || ! readModel (ctx, m))
            return;

        int written = 0;
        auto err = applyValues (ctx, m, stored, true, written);
        std::cerr << "[interface] project mix applied (" << written << " values)"
                  << (err.isEmpty() ? juce::String() : ": " + err) << "\n";
    }

    void interfaceProjectChanged (DawdContext& ctx)
    {
        ctx.ifaceAppliedToken = {};
        syncInterfaceProject (ctx);
    }

    void InterfaceTimer::timerCallback()
    {
        if (ctx != nullptr)
        {
            syncInterfaceProject (*ctx);
            releaseIdleGraph (*ctx);
        }
    }

    juce::var opInterfaceState (DawdContext& ctx, const juce::var&)
    {
        syncInterfaceProject (ctx);
        InterfaceModel m;

        if (! readModel (ctx, m))
            return resultObj (true, { { "present", false }, { "simulated", backend (ctx).simulated() } });

        return stateOf (ctx, m);
    }

    juce::var opSetInterface (DawdContext& ctx, const juce::var& cmd)
    {
        syncInterfaceProject (ctx);
        InterfaceModel m;

        if (! readModel (ctx, m))
            return fail ("no interface");

        auto reset = (bool) cmd.getProperty ("reset", false);
        auto restore = (bool) cmd.getProperty ("restore", false);
        auto save = (bool) cmd.getProperty ("save", false);

        if ((restore || save) && ! ctx.tree)
            return fail ("no tree open");

        // Erst alles pruefen, dann schreiben — ein Fehler aendert nichts
        std::map<juce::String, int> values, stored;

        if (restore)
        {
            if (! loadStored (ctx, stored))
                return fail ("no interface mix stored in this project");

            values = stored;
        }

        if (reset)
            for (auto& [n, v] : m.defaultMix())
                values[n] = v;

        auto list = [&cmd] (const char* key) -> juce::Array<juce::var>
        {
            if (auto* a = cmd.getProperty (key, {}).getArray())
                return *a;

            return {};
        };

        for (auto& r : list ("routes"))
        {
            auto outName = r.getProperty ("out", {}).toString();
            auto srcName = r.getProperty ("src", {}).toString();
            auto outs = m.resolve (outName, true);
            auto srcs = m.resolve (srcName, false);

            if (outs.isEmpty())
                return fail ("unknown output: " + outName);

            if (srcs.isEmpty())
                return fail ("unknown source: " + srcName);

            auto db = r.getProperty ("db", {});

            if (! isOff (db) && ! isNumber (db))
                return fail ("db must be a number or \"off\": " + outName + " from " + srcName);

            // Paar auf Paar = links auf links, rechts auf rechts; sonst jeder
            // auf jeden (Mono-Quelle in beide Seiten)
            for (int o = 0; o < outs.size(); ++o)
                for (int s = 0; s < srcs.size(); ++s)
                {
                    if (outs.size() == 2 && srcs.size() == 2 && o != s)
                        continue;

                    if (auto* c = m.find (InterfaceModel::matrixName (outs[o], srcs[s])))
                        values[c->name] = stepFor (*c, db);
                }
        }

        for (auto& f : list ("faders"))
        {
            auto outName = f.getProperty ("out", {}).toString();
            auto outs = m.resolve (outName, true);

            if (outs.isEmpty())
                return fail ("unknown output: " + outName);

            auto db = f.getProperty ("db", {});

            if (! isOff (db) && ! isNumber (db))
                return fail ("db must be a number or \"off\": fader " + outName);

            for (auto& out : outs)
                if (auto* c = m.find (InterfaceModel::faderName (out)))
                    values[c->name] = stepFor (*c, db);
        }

        for (auto& l : list ("outputLevels"))
        {
            auto outName = l.getProperty ("out", {}).toString();
            auto outs = m.resolve (outName, true);

            if (outs.isEmpty())
                return fail ("unknown output: " + outName);

            auto db = l.getProperty ("db", {});

            if (! isOff (db) && ! isNumber (db))
                return fail ("db must be a number or \"off\": level " + outName);

            for (auto& out : outs)
            {
                auto n = m.outputLevelName (out);

                if (n.isEmpty())
                    return fail ("output has no level control: " + out);

                values[n] = stepFor (*m.find (n), db);
            }
        }

        for (auto& in : list ("inputs"))
        {
            auto inName = in.getProperty ("input", {}).toString();
            auto ins = m.resolve (inName, false);

            if (ins.isEmpty())
                return fail ("unknown input: " + inName);

            for (auto& src : ins)
            {
                // Feld -> Reglername; ein Feld ohne Regler ist ein Fehler
                for (auto [key, suffix] : { std::pair { "gainDb", " Capture Volume" },
                                            std::pair { "phantom", " 48V Phantom Power Switch" },
                                            std::pair { "hiZ", " Hi-Z Switch" },
                                            std::pair { "pad", " PAD Switch" } })
                {
                    if (! in.hasProperty (key))
                        continue;

                    auto* c = m.find (src + suffix);

                    if (c == nullptr)
                        return fail (src + " has no " + key);

                    auto v = in.getProperty (key, {});

                    if (! c->isBool && ! isNumber (v))
                        return fail (juce::String (key) + " must be a number: " + src);
                    values[c->name] = c->isBool ? ((bool) v ? 1 : 0) : InterfaceModel::fromDb (*c, (double) v);
                }
            }
        }

        // Rohe Regler (Name aus amixer), fuer alles ohne eigenes Feld
        for (auto& c : list ("controls"))
        {
            auto n = c.getProperty ("name", {}).toString();
            auto* ctl = m.find (n);

            if (ctl == nullptr)
                return fail ("no such control: " + n);

            if (! c.hasProperty ("value"))
                return fail ("no value for control: " + n);

            auto v = (int) c.getProperty ("value", 0);

            if (v < ctl->min || v > ctl->max)
                return fail ("value out of range for " + n + ": " + juce::String (ctl->min) + ".." + juce::String (ctl->max));

            values[n] = v;
        }

        int written = 0;

        // Laden/Zuruecksetzen schreibt nur Abweichungen (sonst 666 USB-Befehle)
        if (auto err = applyValues (ctx, m, values, restore || reset, written); err.isNotEmpty())
            return fail (err);

        if (! readModel (ctx, m))
            return fail ("interface vanished");

        // Mix pro Song: jede Aenderung (und save) landet im Projekt
        auto storedNow = false;

        if (ctx.tree)
        {
            if (! storeSnapshot (ctx, m))
                return fail ("cannot write interface.json");

            storedNow = true;
            ctx.ifaceAppliedToken = backend (ctx).token();
        }

        auto r = stateOf (ctx, m);
        r.getDynamicObject()->setProperty ("written", written);
        r.getDynamicObject()->setProperty ("savedToProject", storedNow);
        return r;
    }

    juce::var opInterfaceMeters (DawdContext& ctx, const juce::var&)
    {
        // Ohne syncInterfaceProject und ohne die 666 Regler: das UI fragt
        // ~12-mal je Sekunde
        auto& b = backend (ctx);
        IfMeters m;

        if (! b.meters (m))
            return resultObj (true, { { "present", b.token().isNotEmpty() }, { "meters", false },
                                      { "simulated", b.simulated() } });

        auto group = [&m] (int first, auto nameOf)
        {
            auto g = new juce::DynamicObject();

            for (int i = 0; i < IfMeters::perGroup; ++i)
            {
                auto toDb = [] (float v) { return v > 0.0f ? 20.0 * std::log10 ((double) v) : -INFINITY; };
                auto ch = new juce::DynamicObject();
                ch->setProperty ("peakDb", dbVar (toDb (m.peak[(size_t) (first + i)])));
                ch->setProperty ("rmsDb", dbVar (toDb (m.rms[(size_t) (first + i)])));
                g->setProperty (nameOf (i), juce::var (ch));
            }

            return juce::var (g);
        };

        auto r = resultObj (true, { { "present", true }, { "meters", true }, { "simulated", b.simulated() } });
        auto* o = r.getDynamicObject();
        o->setProperty ("inputs", group (0, [] (int i) { return interfaceInputNames()[i]; }));
        o->setProperty ("playback", group (IfMeters::perGroup, [] (int i) { return "PCM " + juce::String (i + 1); }));
        o->setProperty ("outputs", group (2 * IfMeters::perGroup, [] (int i) { return interfaceOutputNames()[i]; }));
        return r;
    }

    juce::var opInterfaceSim (DawdContext& ctx, const juce::var& cmd)
    {
        // Nur mit --interface-sim: Abziehen/Einstecken und Pegel fuer die
        // Test-Suite; meters [{group: inputs|playback|outputs, name, peakDb, rmsDb?}]
        if (! backend (ctx).simulated())
            return fail ("interface is not simulated (start unidaw with --interface-sim)");

        if (auto* meters = cmd.getProperty ("meters", {}).getArray())
        {
            for (auto& mv : *meters)
            {
                auto group = mv.getProperty ("group", "").toString();
                auto name = mv.getProperty ("name", "").toString();
                auto names = group == "inputs" ? interfaceInputNames()
                           : group == "outputs" ? interfaceOutputNames() : juce::StringArray();

                if (group == "playback")
                    for (int i = 1; i <= IfMeters::perGroup; ++i)
                        names.add ("PCM " + juce::String (i));

                auto idx = names.indexOf (name);

                if (idx < 0)
                    return fail ("no such meter: " + group + " " + name);

                auto first = group == "inputs" ? 0 : group == "playback" ? IfMeters::perGroup : 2 * IfMeters::perGroup;
                auto lin = [] (const juce::var& db) { return db.isString() ? 0.0f : std::pow (10.0f, (float) db / 20.0f); };
                auto peak = mv.getProperty ("peakDb", "off");
                backend (ctx).simulateMeter (first + idx, lin (peak), lin (mv.getProperty ("rmsDb", peak)));
            }
        }

        if (cmd.hasProperty ("present"))
            backend (ctx).simulatePresent ((bool) cmd.getProperty ("present", true));

        return resultObj (true, { { "present", backend (ctx).token().isNotEmpty() } });
    }
}
