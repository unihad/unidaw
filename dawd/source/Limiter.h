#pragma once

#include "Dawd.h"

#include <deque>

// Limiter fuer die Summe (Built-in "Limiter"): die Engine bringt keinen mit,
// der Kompressor faengt Spitzen kaum. True-Peak-Limiter: das Signal laeuft
// 4-fach ueberabgetastet (Polyphasen-FIR hoch und runter), dort bestimmt ein
// Minimum ueber das Lookahead-Fenster (1,5 ms) die noetige Verstaerkung,
// danach Release und ein gleitender Mittelwert gleicher Laenge — die
// Verstaerkung ist an jeder Spitze klein genug und trotzdem glatt (keine
// Ueberschwinger zwischen den Samples durch Regel-Spruenge). Decke in dB
// (intern 0,2 dB Reserve fuer das Runterrechnen), Gain davor, Release. Die
// Latenz (Lookahead + Filter) meldet das Plugin der Engine.
namespace dawd
{
    class LimiterPlugin : public te::Plugin
    {
    public:
        explicit LimiterPlugin (te::PluginCreationInfo);
        ~LimiterPlugin() override;

        static const char* xmlTypeName;

        juce::String getName() const override                        { return "Limiter"; }
        juce::String getPluginType() override                        { return xmlTypeName; }
        juce::String getSelectableDescription() override             { return getName(); }
        int getNumOutputChannelsGivenInputs (int numInputs) override { return numInputs; }
        double getLatencySeconds() override;
        BusLayout getBusses() const override                         { return BusLayout::singlePassThrough(); }

        void initialise (const te::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void reset() override;
        void applyToBuffer (const te::PluginRenderContext&) override;
        void restorePluginStateFromValueTree (const juce::ValueTree&) override;

        juce::CachedValue<float> ceilingDb, gainDb, releaseMs;
        te::AutomatableParameter::Ptr ceilingParam, gainParam, releaseParam;

        static constexpr double lookaheadSeconds = 0.0015;
        static constexpr int os = 4, taps = 48;

        // Messung im Render (analyze/compareReference): Gain-Reduction und
        // Eingang (vor Gain), nur in Render-Durchlaeufen, nicht beim
        // Abspielen. resetStats vor dem Render, renderStats danach (Message-
        // Thread; der Render laeuft synchron). from: erst ab dieser Edit-Zeit
        // messen (Sekunden) — der Vorlauf von renderMixRange zaehlt nicht.
        struct Stats
        {
            double grSumDb = 0.0, inSumSq = 0.0;
            juce::int64 grCount = 0, inCount = 0;
            float grMaxDb = 0.0f, inPeak = 0.0f;
        };

        void resetStats (double from = 0.0)                          { stats = {}; measureFrom = from; }
        Stats renderStats() const                                    { return stats; }

    private:
        Stats stats;
        double measureFrom = 0.0;
        double sampleRate = 44100.0;
        int window = 264;                                      // Lookahead in 4x-Samples
        std::array<float, taps> fir {};

        struct Channel
        {
            std::array<float, taps / os> upHist {};            // letzte 12 Eingangswerte
            std::vector<float> delay;                          // 4x-Verzoegerung (window - 1)
            std::array<float, taps> downHist {};               // letzte 48 4x-Werte vor dem Runterrechnen
            int downPos = 0;
        };

        std::vector<Channel> channels;
        std::deque<std::pair<long long, float>> minQ;          // (Index, need) aufsteigend
        std::vector<float> avgRing;
        double avgSum = 0.0;
        long long index = 0;
        int delayPos = 0, avgPos = 0, upPos = 0;
        float released = 1.0f;

        void prepare (int numChannels);
    };

    void registerLimiterPlugin (te::Engine&);
}
