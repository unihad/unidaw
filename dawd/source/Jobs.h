#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

// Hintergrund-Jobs fuer lange Ops (Stem-Trennung): laufen auf einem eigenen
// Thread, der Message-Thread bleibt frei. jobStatus liest Fortschritt und
// Ergebnis; cancel bricht beim naechsten Schritt ab. Ein Job schreibt nur
// Dateien — Aenderungen am Edit macht danach der Agent (z. B. addClip).
//
// Jobs in Schritten auf dem Message-Thread (matchSound, tuneSound): sie
// rendern, und die Engine ist nicht thread-safe. Jeder Schritt (ein Render
// und eine Messung) ist eine eigene Nachricht in der Queue, Befehle laufen
// dazwischen. Ihr Zustand (work, mit einem Wegwerf-Edit) gehoert dem Job und
// stirbt mit der JobList vor der Engine; die Nachrichten halten nur einen
// weak_ptr.
namespace dawd
{
    struct JobWork
    {
        virtual ~JobWork() = default;
    };

    struct Job
    {
        juce::String id, kind;
        std::atomic<double> progress { 0.0 };
        std::atomic<bool> done { false }, cancel { false };
        std::mutex lock;
        juce::String error;      // unter lock, gueltig wenn done
        juce::var result;        // unter lock, gueltig wenn done
        juce::Time started = juce::Time::getCurrentTime();
        bool onMessageThread = false;    // in Schritten auf dem Message-Thread (jobStatus wartet dort nicht)
        std::shared_ptr<JobWork> work;   // nur Message-Thread
    };

    struct JobList
    {
        std::map<juce::String, std::shared_ptr<Job>> jobs;
        int next = 1;
        juce::ThreadPool pool { 1 };   // ein Job nach dem anderen — die Modelle nutzen selbst alle Kerne

        ~JobList()
        {
            for (auto& [id, j] : jobs)
                j->cancel = true;

            pool.removeAllJobs (true, 30000);
        }

        std::shared_ptr<Job> create (const juce::String& kind)
        {
            auto j = std::make_shared<Job>();
            j->id = "j" + juce::String (next++);
            j->kind = kind;
            jobs[j->id] = j;
            return j;
        }
    };
}
