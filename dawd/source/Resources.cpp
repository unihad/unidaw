#include "Resources.h"

#include <cstdlib>

namespace dawd
{
    juce::File resourceDir (const juce::String& name)
    {
        // Links (z. B. /usr/local/bin/dawd) loest JUCE zum echten Ort auf
        auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        auto dataHome = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");

        if (auto* xdg = std::getenv ("XDG_DATA_HOME"); xdg != nullptr && juce::File::isAbsolutePath (xdg))
            dataHome = juce::File (xdg);

        const juce::File candidates[] = { exeDir.getChildFile (name),
                                          exeDir.getParentDirectory().getChildFile ("share/unidaw").getChildFile (name),
                                          dataHome.getChildFile ("unidaw").getChildFile (name) };

        for (auto& dir : candidates)
            if (dir.isDirectory())
                return dir;

        return candidates[2];
    }
}
