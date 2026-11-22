#include "PluginProfiles.h"
#include "Resources.h"

namespace dawd
{
    juce::File profilesDirectory()
    {
        return resourceDir ("plugin-profiles");
    }

    const std::vector<CoreEntry>& coreSet()
    {
        // Einmal lesen — die Datei aendert sich nur mit dem Repo
        static const std::vector<CoreEntry> entries = []
        {
            std::vector<CoreEntry> out;
            auto json = juce::JSON::parse (profilesDirectory().getChildFile ("coreset.json"));

            if (auto* list = json.getProperty ("plugins", {}).getArray())
                for (auto& v : *list)
                    out.push_back ({ v.getProperty ("name", {}).toString(), v.getProperty ("format", {}).toString(),
                                     v.getProperty ("category", {}).toString(), v.getProperty ("purpose", {}).toString() });

            return out;
        }();

        return entries;
    }

    const CoreEntry* coreEntryFor (const juce::String& name)
    {
        for (auto& e : coreSet())
            if (e.name.equalsIgnoreCase (name))
                return &e;

        return nullptr;
    }

    juce::String profileSlug (const juce::String& format, const juce::String& uid)
    {
        return juce::File::createLegalFileName ((format.isEmpty() ? juce::String ("builtin") : format).toLowerCase() + "-" + uid.toLowerCase());
    }

    juce::var loadProfile (const juce::String& slug)
    {
        auto f = profilesDirectory().getChildFile (slug + ".json");
        return f.existsAsFile() ? juce::JSON::parse (f) : juce::var();
    }

    juce::String loadNotes (const juce::String& slug)
    {
        auto f = profilesDirectory().getChildFile ("notes").getChildFile (slug + ".md");
        return f.existsAsFile() ? f.loadFileAsString() : juce::String();
    }
}
