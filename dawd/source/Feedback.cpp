#include "Feedback.h"

#include <map>
#include <mutex>

namespace dawd
{
    namespace
    {
        const juce::Identifier keyProp ("fbKey");
        const juce::Identifier levelProp ("fbLevel");
        const juce::Identifier delayProp ("fbDelayMs");
    }

    // Ringpuffer einer Feedback-Kante. written zaehlt die geschriebenen
    // Samples seit dem Anlegen; der Return liest hinter dieser Marke.
    struct FeedbackRing
    {
        juce::AudioBuffer<float> buffer;
        std::atomic<juce::int64> written { 0 };
        std::mutex prepareLock;

        // Groesse nur aendern, wenn sie nicht reicht — ein zweites
        // initialise (Send nach Return) darf den Puffer nicht leeren
        void prepare (double sampleRate, int blockSize)
        {
            std::lock_guard<std::mutex> l (prepareLock);
            auto size = (int) std::ceil (sampleRate * maxFeedbackDelayMs / 1000.0) + 4 * juce::jmax (blockSize, 512);

            if (buffer.getNumSamples() < size)
            {
                buffer.setSize (2, size);
                buffer.clear();
                written = 0;
            }
        }
    };

    namespace
    {
        std::mutex registryLock;
        std::map<std::pair<const te::Edit*, int>, std::weak_ptr<FeedbackRing>> registry;

        std::shared_ptr<FeedbackRing> ringFor (const te::Edit& edit, int key)
        {
            std::lock_guard<std::mutex> l (registryLock);
            auto& slot = registry[{ &edit, key }];

            if (auto r = slot.lock())
                return r;

            auto r = std::make_shared<FeedbackRing>();
            slot = r;
            return r;
        }
    }

    //==============================================================================
    const char* FeedbackSendPlugin::xmlTypeName = "dawdFeedbackSend";

    FeedbackSendPlugin::FeedbackSendPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        key.referTo (state, keyProp, getUndoManager());
    }

    FeedbackSendPlugin::~FeedbackSendPlugin()
    {
        notifyListenersOfDeletion();
    }

    void FeedbackSendPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        ring = ringFor (edit, key.get());
        ring->prepare (info.sampleRate, info.blockSizeSamples);
    }

    void FeedbackSendPlugin::deinitialise()
    {
    }

    void FeedbackSendPlugin::applyToBuffer (const te::PluginRenderContext& fc)
    {
        if (ring == nullptr || fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        auto& buf = ring->buffer;
        auto size = buf.getNumSamples();

        if (size == 0)
            return;

        auto w = ring->written.load();
        auto chans = fc.destBuffer->getNumChannels();

        for (int c = 0; c < 2; ++c)
        {
            auto* src = chans > 0 ? fc.destBuffer->getReadPointer (juce::jmin (c, chans - 1), fc.bufferStartSample) : nullptr;
            auto* dst = buf.getWritePointer (c);

            for (int i = 0; i < fc.bufferNumSamples; ++i)
                dst[(w + i) % size] = src != nullptr ? src[i] : 0.0f;
        }

        ring->written = w + fc.bufferNumSamples;
    }

    void FeedbackSendPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
    {
        te::copyPropertiesToCachedValues (v, key);
    }

    //==============================================================================
    const char* FeedbackReturnPlugin::xmlTypeName = "dawdFeedbackReturn";

    FeedbackReturnPlugin::FeedbackReturnPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        auto* um = getUndoManager();
        key.referTo (state, keyProp, um);
        level.referTo (state, levelProp, um, 0.5f);
        delayMs.referTo (state, delayProp, um, 0.0f);

        levelParam = addParam ("level", "Level", { 0.0f, 1.0f });
        levelParam->attachToCurrentValue (level);
        delayParam = addParam ("delayMs", "Delay", { 0.0f, (float) maxFeedbackDelayMs });
        delayParam->attachToCurrentValue (delayMs);
    }

    FeedbackReturnPlugin::~FeedbackReturnPlugin()
    {
        notifyListenersOfDeletion();
        levelParam->detachFromCurrentValue();
        delayParam->detachFromCurrentValue();
    }

    double FeedbackReturnPlugin::minDelayMs() const
    {
        return blockSize > 0 ? blockSize * 1000.0 / sampleRate : 0.0;
    }

    void FeedbackReturnPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        sampleRate = info.sampleRate;
        blockSize = info.blockSizeSamples;
        ring = ringFor (edit, key.get());
        ring->prepare (info.sampleRate, info.blockSizeSamples);
        readPos = ring->written.load();
    }

    void FeedbackReturnPlugin::deinitialise()
    {
    }

    void FeedbackReturnPlugin::applyToBuffer (const te::PluginRenderContext& fc)
    {
        auto n = fc.bufferNumSamples;

        if (ring == nullptr || fc.destBuffer == nullptr || n <= 0)
            return;

        auto& buf = ring->buffer;
        auto size = (juce::int64) buf.getNumSamples();

        if (size == 0)
            return;

        // Der Send laeuft in diesem Block vor oder nach uns: written liegt
        // bei readPos oder readPos + n. Alles andere (Send uebersprungen,
        // neu gestartet) heisst neu einrasten.
        auto w = ring->written.load();

        if (w < readPos || w > readPos + n)
            readPos = w;

        // Parameter statt CachedValue: folgt der Automation
        auto delay = (juce::int64) std::round (juce::jlimit (0.0, maxFeedbackDelayMs, (double) delayParam->getCurrentValue()) * sampleRate / 1000.0);
        delay = juce::jlimit ((juce::int64) juce::jmax (n, blockSize), size - 2 * (juce::int64) n, delay);
        auto start = readPos - delay;
        auto gain = juce::jlimit (0.0f, 1.0f, levelParam->getCurrentValue());
        auto chans = fc.destBuffer->getNumChannels();

        for (int c = 0; c < chans; ++c)
        {
            auto* src = buf.getReadPointer (juce::jmin (c, 1));
            auto* dst = fc.destBuffer->getWritePointer (c, fc.bufferStartSample);

            for (int i = 0; i < n; ++i)
            {
                auto idx = start + i;

                // Noch nichts geschrieben (Anlauf): Stille
                if (idx < 0 || idx >= w)
                    continue;

                // Soft-Clip haelt die Schleife beschraenkt, auch bei 100 %
                dst[i] += std::tanh (gain * src[idx % size]);
            }
        }

        readPos += n;
    }

    void FeedbackReturnPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
    {
        te::copyPropertiesToCachedValues (v, key, level, delayMs);
    }

    void registerFeedbackPlugins (te::Engine& engine)
    {
        engine.getPluginManager().createBuiltInType<FeedbackSendPlugin>();
        engine.getPluginManager().createBuiltInType<FeedbackReturnPlugin>();
    }
}
