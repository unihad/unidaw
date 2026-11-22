#include "Limiter.h"

namespace dawd
{
    const char* LimiterPlugin::xmlTypeName = "dawdLimiter";

    LimiterPlugin::LimiterPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        auto* um = getUndoManager();
        ceilingDb.referTo (state, "ceilingDb", um, -1.0f);
        gainDb.referTo (state, "gainDb", um, 0.0f);
        releaseMs.referTo (state, "releaseMs", um, 80.0f);

        ceilingParam = addParam ("ceiling", "Ceiling", { -24.0f, 0.0f }, [] (float v) { return juce::String (v, 1) + " dB"; }, nullptr);
        ceilingParam->attachToCurrentValue (ceilingDb);
        gainParam = addParam ("gain", "Gain", { 0.0f, 24.0f }, [] (float v) { return juce::String (v, 1) + " dB"; }, nullptr);
        gainParam->attachToCurrentValue (gainDb);
        releaseParam = addParam ("release", "Release", { 5.0f, 1000.0f }, [] (float v) { return juce::String (juce::roundToInt (v)) + " ms"; }, nullptr);
        releaseParam->attachToCurrentValue (releaseMs);

        // Interpolations-FIR (4x): gefensterter Sinc, Kaiser beta 8, DC-Verstaerkung 4
        double sum = 0.0;

        for (int i = 0; i < taps; ++i)
        {
            auto n = i - (taps - 1) / 2.0;
            auto x = n / os;
            auto sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            auto r = 2.0 * i / (taps - 1) - 1.0;
            auto kaiser = juce::dsp::SpecialFunctions::besselI0 (8.0 * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / juce::dsp::SpecialFunctions::besselI0 (8.0);
            fir[(size_t) i] = (float) (sinc * kaiser);
            sum += fir[(size_t) i];
        }

        for (auto& f : fir)
            f = (float) (f * os / sum);
    }

    LimiterPlugin::~LimiterPlugin()
    {
        notifyListenersOfDeletion();
        ceilingParam->detachFromCurrentValue();
        gainParam->detachFromCurrentValue();
        releaseParam->detachFromCurrentValue();
    }

    double LimiterPlugin::getLatencySeconds()
    {
        // 4x: hoch (taps-1)/2, Lookahead window-1, runter (taps-1)/2
        return ((taps - 1) + (window - 1)) / (double) os / sampleRate;
    }

    void LimiterPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        sampleRate = info.sampleRate;
        window = juce::jmax (os, (int) std::round (lookaheadSeconds * sampleRate * os));
        reset();
    }

    void LimiterPlugin::deinitialise() {}

    void LimiterPlugin::prepare (int numChannels)
    {
        channels.assign ((size_t) numChannels, Channel());

        for (auto& c : channels)
            c.delay.assign ((size_t) juce::jmax (1, window - 1), 0.0f);

        minQ.clear();
        avgRing.assign ((size_t) window, 1.0f);
        avgSum = window;
        index = 0;
        delayPos = avgPos = upPos = 0;
        released = 1.0f;
    }

    void LimiterPlugin::reset()
    {
        prepare ((int) juce::jmax ((size_t) 2, channels.size()));
    }

    void LimiterPlugin::applyToBuffer (const te::PluginRenderContext& fc)
    {
        if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        const int chans = fc.destBuffer->getNumChannels();

        if ((int) channels.size() != chans)
            prepare (chans);

        // Parameter statt CachedValue: folgen der Automation; 0,2 dB Reserve
        const float ceiling = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 0.0f, ceilingParam->getCurrentValue()) - 0.2f);
        const float hardCeiling = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 0.0f, ceilingParam->getCurrentValue()));
        const float inGain = juce::Decibels::decibelsToGain (juce::jlimit (0.0f, 24.0f, gainParam->getCurrentValue()));
        const float rel = (float) (1.0 - std::exp (-1.0 / (juce::jlimit (5.0f, 1000.0f, releaseParam->getCurrentValue()) * 0.001 * sampleRate * os)));
        const int delayLen = (int) channels[0].delay.size();
        // Messen nur im Render und erst ab measureFrom (Vorlauf weglassen)
        const int measureStart = ! fc.isRendering ? fc.bufferNumSamples
                               : juce::jlimit (0, fc.bufferNumSamples, (int) std::ceil ((measureFrom - fc.editTime.getStart().inSeconds()) * sampleRate));
        constexpr int phaseTaps = taps / os;
        std::array<std::array<float, os>, 8> up {};             // je Kanal (bis 8) die 4 Werte dieses Samples

        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            const int s = fc.bufferStartSample + i;
            const bool measure = i >= measureStart;

            // 1. Hochrechnen: 4 Werte je Eingangssample (Polyphasen-FIR)
            for (int c = 0; c < chans && c < 8; ++c)
            {
                auto& ch = channels[(size_t) c];
                const float in = fc.destBuffer->getSample (c, s);
                ch.upHist[(size_t) upPos] = in * inGain;

                // Pausen (unter -80 dBFS) zaehlen nicht zum RMS des Crest
                if (measure && std::abs (in) > 1.0e-4f)
                {
                    stats.inSumSq += (double) in * in;
                    stats.inPeak = juce::jmax (stats.inPeak, std::abs (in));
                    ++stats.inCount;
                }

                for (int p = 0; p < os; ++p)
                {
                    float acc = 0.0f;

                    for (int k = 0; k < phaseTaps; ++k)
                        acc += fir[(size_t) (p + k * os)] * ch.upHist[(size_t) ((upPos - k + phaseTaps) % phaseTaps)];

                    up[(size_t) c][(size_t) p] = acc;
                }
            }

            upPos = (upPos + 1) % phaseTaps;
            float outVal[8] {};

            for (int p = 0; p < os; ++p)
            {
                // 2. Noetige Verstaerkung, Minimum ueber das Fenster (Deque)
                float peak = 0.0f;

                for (int c = 0; c < chans && c < 8; ++c)
                    peak = juce::jmax (peak, std::abs (up[(size_t) c][(size_t) p]));

                const float need = peak > ceiling ? ceiling / peak : 1.0f;

                while (! minQ.empty() && minQ.back().second >= need)
                    minQ.pop_back();

                minQ.emplace_back (index, need);

                while (minQ.front().first <= index - window)
                    minQ.pop_front();

                const float m = minQ.front().second;

                // 3. Release (sofort runter, exponentiell rauf), dann Mittel ueber das Fenster
                released = m < released ? m : released + rel * (m - released);
                avgSum += released - avgRing[(size_t) avgPos];
                avgRing[(size_t) avgPos] = released;
                avgPos = (avgPos + 1) % window;
                const float g = (float) (avgSum / window);

                // Reduktion nur, wo Signal ist (ab -60 dBFS nach Gain) —
                // Pausen wuerden das Mittel sonst kleinrechnen
                if (measure && peak > 0.001f)
                {
                    const float grDb = -juce::Decibels::gainToDecibels (g, -100.0f);
                    stats.grSumDb += grDb;
                    stats.grMaxDb = juce::jmax (stats.grMaxDb, grDb);
                    ++stats.grCount;
                }

                // 4. Verzoegerung (window-1) anwenden, Verstaerkung, runterrechnen
                for (int c = 0; c < chans && c < 8; ++c)
                {
                    auto& ch = channels[(size_t) c];
                    const float delayed = delayLen > 0 ? ch.delay[(size_t) delayPos] : up[(size_t) c][(size_t) p];

                    if (delayLen > 0)
                        ch.delay[(size_t) delayPos] = up[(size_t) c][(size_t) p];

                    ch.downHist[(size_t) ch.downPos] = delayed * g;
                    ch.downPos = (ch.downPos + 1) % taps;

                    // Ausgang einmal je Eingangssample (beim letzten 4x-Wert)
                    if (p == os - 1)
                    {
                        float acc = 0.0f;

                        for (int k = 0; k < taps; ++k)
                            acc += fir[(size_t) k] * ch.downHist[(size_t) ((ch.downPos - 1 - k + 2 * taps) % taps)];

                        outVal[c] = acc / os;
                    }
                }

                if (delayLen > 0)
                    delayPos = (delayPos + 1) % delayLen;

                ++index;
            }

            for (int c = 0; c < chans && c < 8; ++c)
                fc.destBuffer->setSample (c, s, juce::jlimit (-hardCeiling, hardCeiling, outVal[c]));
        }
    }

    void LimiterPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
    {
        te::copyPropertiesToCachedValues (v, ceilingDb, gainDb, releaseMs);

        // Die Parameter lesen den neuen Wert nicht von selbst (wie
        // ParameterWithStateValue::setFromValueTree der Engine) — sonst
        // behielt ein Rezept-Limiter Gain 0 / Ceiling -1 (Praxistest 1k)
        for (auto& p : { ceilingParam, gainParam, releaseParam })
            p->updateFromAttachedValue();
    }

    void registerLimiterPlugin (te::Engine& engine)
    {
        engine.getPluginManager().createBuiltInType<LimiterPlugin>();
    }
}
