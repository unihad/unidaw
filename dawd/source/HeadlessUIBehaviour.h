#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

class HeadlessUIBehaviour : public te::UIBehaviour
{
public:
    void showWarningMessage (const juce::String& message) override
    {
        std::cerr << "[ui] warning: " << message << "\n";
    }

    // Editor-Fenster der Plugin-Module (PluginWindows.h) — die Engine ruft
    // das aus PluginWindowState::showWindow; dawd setzt die Fabrik nach dem
    // Start (echt oder --window-sim)
    std::function<std::unique_ptr<juce::Component> (te::PluginWindowState&)> windowFactory;

    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& s) override
    {
        return windowFactory ? windowFactory (s) : nullptr;
    }

    void runTaskWithProgressBar (te::ThreadPoolJobWithProgress& task) override
    {
        std::cerr << "[ui] task start: " << task.getJobName() << "\n";
        TaskRunner runner (task);

        while (runner.isThreadRunning())
            if (! juce::MessageManager::getInstance()->runDispatchLoopUntil (10))
                break;

        std::cerr << "[ui] task done: " << task.getJobName() << "\n";
    }

private:
    struct TaskRunner  : public juce::Thread
    {
        TaskRunner (te::ThreadPoolJobWithProgress& t)
            : juce::Thread (t.getJobName()), task (t)
        {
            startThread();
        }

        ~TaskRunner() override
        {
            task.signalJobShouldExit();
            waitForThreadToExit (10000);
        }

        void run() override
        {
            while (! threadShouldExit())
                if (task.runJob() == juce::ThreadPoolJob::jobHasFinished)
                    break;
        }

        te::ThreadPoolJobWithProgress& task;
    };
};
