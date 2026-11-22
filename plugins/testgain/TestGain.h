#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class TestGainProcessor : public juce::AudioProcessor
{
public:
    TestGainProcessor()
        : AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (gain = new juce::AudioParameterFloat (
            juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 1.0f));
    }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == l.getMainOutputChannelSet()
            && ! l.getMainInputChannelSet().isDisabled();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (gain->get());
    }

    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override  { return nullptr; }
    bool hasEditor() const override                      { return false; }
    const juce::String getName() const override          { return "TestGain"; }
    bool acceptsMidi() const override                    { return false; }
    bool producesMidi() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }
    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& mb) override
    {
        juce::MemoryOutputStream (mb, true).writeFloat (gain->get());
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (sizeInBytes < 4)
            return;

        juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
        gain->setValueNotifyingHost (in.readFloat());
    }

private:
    juce::AudioParameterFloat* gain = nullptr;
};
