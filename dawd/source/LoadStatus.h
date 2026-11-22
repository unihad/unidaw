#pragma once

#include <juce_core/juce_core.h>

#include <mutex>

// Ladestand fuer die UI (GET /progress): was der Message-Thread gerade tut
// (Op, Phase, Plugin i von n) und was im Hintergrund laeuft. Geschrieben auf
// dem Message-Thread (executeOp, loadNodeEdit, Plugin-Erzeugung), gelesen vom
// HTTP-Thread, waehrend der Message-Thread noch blockiert — darum unter einer
// Sperre und ohne Engine-Zugriff beim Lesen.
namespace dawd
{
    struct LoadStatus
    {
        // Eine Op beginnt (executeOp, nicht fuer Anzeige-Polls). Verschachtelt
        // (batch): die aeussere bleibt, die innere wird zur Phase.
        void begin (const juce::String& op)
        {
            std::lock_guard<std::mutex> l (lock);

            if (depth++ > 0)
            {
                phase = op;
                detail = {};
                done = total = 0;
                return;
            }

            current = op;
            phase = detail = {};
            done = total = plugins = 0;
            started = juce::Time::getMillisecondCounterHiRes();
        }

        void end()
        {
            std::lock_guard<std::mutex> l (lock);

            if (--depth > 0)
                return;

            depth = 0;
            lastOp = current;
            lastMs = juce::Time::getMillisecondCounterHiRes() - started;
            lastPlugins = plugins;
            current = phase = detail = {};
            done = total = plugins = 0;
        }

        // Phase einer laufenden Op ("node edit", "samples", "audio graph");
        // total > 0 = Schritte (Plugins)
        void setPhase (const juce::String& p, int stepsTotal = 0)
        {
            std::lock_guard<std::mutex> l (lock);

            if (depth == 0)
                return;

            phase = p;
            detail = {};
            done = 0;
            total = stepsTotal;
        }

        // Ein externes Plugin wird geladen (EngineBehaviour::shouldLoadPlugin)
        // bzw. ist instanziiert (doAdditionalInitialisation) — die teuren
        // Schritte beim Laden eines Nodes (Surge XT: 0,3-1 s je Instanz)
        void pluginStarting (const juce::String& name)
        {
            std::lock_guard<std::mutex> l (lock);

            if (depth == 0)
                return;

            detail = name;
        }

        void pluginLoaded()
        {
            std::lock_guard<std::mutex> l (lock);

            if (depth == 0)
                return;

            ++plugins;

            if (total > 0)
                done = juce::jmin (total, done + 1);
        }

        // Hintergrund (Jobs, Klang-Analyse): vom Message-Thread nachgetragen
        void setBackground (const juce::var& list)
        {
            std::lock_guard<std::mutex> l (lock);
            background = list;
        }

        juce::var snapshot() const
        {
            std::lock_guard<std::mutex> l (lock);
            auto o = new juce::DynamicObject();
            o->setProperty ("ok", true);
            o->setProperty ("busy", depth > 0);

            if (depth > 0)
            {
                o->setProperty ("op", current);
                o->setProperty ("phase", phase);
                o->setProperty ("detail", detail);
                o->setProperty ("done", done);
                o->setProperty ("total", total);
                o->setProperty ("plugins", plugins);
                o->setProperty ("ms", juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started));
            }

            o->setProperty ("background", background.isArray() ? background.clone() : juce::var (juce::Array<juce::var>()));

            if (lastOp.isNotEmpty())
            {
                auto last = new juce::DynamicObject();
                last->setProperty ("op", lastOp);
                last->setProperty ("ms", juce::roundToInt (lastMs));
                last->setProperty ("plugins", lastPlugins);
                o->setProperty ("last", juce::var (last));
            }

            return juce::var (o);
        }

    private:
        mutable std::mutex lock;
        int depth = 0;
        juce::String current, phase, detail, lastOp;
        int done = 0, total = 0, plugins = 0, lastPlugins = 0;
        double started = 0.0, lastMs = 0.0;
        juce::var background;
    };

    // Die Engine meldet Plugin-Erzeugung ueber das EngineBehaviour (Dawd.cpp),
    // das vor dem Kontext entsteht — darum ein Zeiger, den der Kontext setzt
    inline LoadStatus*& engineLoadStatus()
    {
        static LoadStatus* s = nullptr;
        return s;
    }
}
