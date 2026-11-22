#include "TestGain.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestGainProcessor();
}
