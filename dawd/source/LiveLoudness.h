#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace dawd
{
    // Live-LUFS am Main-Ausgang (levels, 2026-10-26): globaler
    // Ausgangs-Prozessor des DeviceManagers — sieht den fertigen
    // Geraeteausgang (Master-Kette, alle spielenden Edits, Monitoring), nur im
    // Audio-Callback, nie im Render. Misst das Kanalpaar des
    // Standard-Ausgangs (dorthin geht der Master) mit dem LoudnessMeter der
    // Engine (EBU R128 wie analyze; process ist frei von Locks und
    // Allokationen). Rechnet nur, solange jemand levels pollt (letzter Poll
    // vor weniger als idleMs); nach einer Pause beginnt die Messung neu.
    // Integrated laeuft seit dem letzten restart (play, record).
    class LiveLoudness : public juce::AudioProcessor
    {
    public:
        static constexpr juce::uint32 idleMs = 3000;

        // Message-Thread (levels): Poll vermerken, Kanaele des Main-Paars
        // (Index im Geraet, -1 = keiner)
        void touch (int leftChannel, int rightChannel)
        {
            left.store (leftChannel, std::memory_order_relaxed);
            right.store (rightChannel, std::memory_order_relaxed);
            lastPoll.store (juce::Time::getApproximateMillisecondCounter(), std::memory_order_relaxed);
        }

        void restart() { meter.requestReset(); }
        te::LoudnessMeter::Readings readings() const { return meter.getReadings(); }

        // AudioProcessor
        const juce::String getName() const override { return "dawd live loudness"; }

        // Nie parallel zu processBlock (DeviceManager: vor dem Einsetzen bzw.
        // im audioDeviceAboutToStart unter dem Callback-Lock)
        void prepareToPlay (double sampleRate, int blockSize) override
        {
            prepared = sampleRate > 0.0 && blockSize > 0;

            if (prepared)
                meter.prepare (sampleRate, 2, blockSize);
        }

        void releaseResources() override {}

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            if (! prepared)
                return;

            // Niemand pollt: nichts rechnen; beim naechsten Poll neu anfangen
            const auto polled = lastPoll.load (std::memory_order_relaxed);

            if (polled == 0 || juce::Time::getApproximateMillisecondCounter() - polled > idleMs)
            {
                idle = true;
                return;
            }

            if (idle)
            {
                meter.requestReset();
                idle = false;
            }

            const int n = buffer.getNumChannels();
            const int l = left.load (std::memory_order_relaxed), r = right.load (std::memory_order_relaxed);

            if (l < 0 || l >= n || buffer.getReadPointer (l) == nullptr)
                return;

            const bool stereo = r >= 0 && r < n && r != l && buffer.getReadPointer (r) != nullptr;
            const float* chans[2] = { buffer.getReadPointer (l), stereo ? buffer.getReadPointer (r) : nullptr };
            meter.process (chans, stereo ? 2 : 1, buffer.getNumSamples());
        }

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        te::LoudnessMeter meter;
        bool prepared = false, idle = true;   // nur Audio-Thread bzw. prepareToPlay
        std::atomic<int> left { 0 }, right { 1 };
        std::atomic<juce::uint32> lastPoll { 0 };
    };
}
