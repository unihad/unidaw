#include "Origins.h"
#include "Routing.h"

#include <mutex>

namespace dawd
{
    namespace
    {
        // Stems laufen als Job im Hintergrund: Lesen und Schreiben der
        // Seitendatei nur unter dieser Sperre
        std::mutex originsLock;

        juce::File sidecarOf (const juce::File& f)
        {
            return f.getParentDirectory().getChildFile (".dawd-origins.json");
        }

        juce::StringArray recorded (const juce::File& f)
        {
            juce::StringArray out;
            auto side = sidecarOf (f);

            if (! side.existsAsFile())
                return out;

            const auto list = juce::JSON::parse (side).getProperty (juce::Identifier (f.getFileName()), {});

            if (auto* a = list.getArray())
                for (auto& s : *a)
                    out.addIfNotAlreadyThere (s.toString());

            return out;
        }
    }

    juce::StringArray originsOf (const juce::File& f)
    {
        juce::StringArray out { f.getFullPathName() };
        const std::lock_guard<std::mutex> l (originsLock);
        out.addArray (recorded (f));
        out.removeDuplicates (false);
        return out;
    }

    void recordOrigins (const juce::File& derived, const juce::StringArray& inputs)
    {
        juce::StringArray sources;

        for (auto& in : inputs)
            sources.addArray (originsOf (juce::File (in)));

        sources.removeDuplicates (false);
        sources.removeString (derived.getFullPathName());

        if (sources.isEmpty())
            return;

        const std::lock_guard<std::mutex> l (originsLock);
        auto side = sidecarOf (derived);
        auto json = side.existsAsFile() ? juce::JSON::parse (side) : juce::var();

        if (! json.isObject())
            json = juce::var (new juce::DynamicObject());

        juce::Array<juce::var> list;

        for (auto& s : sources)
            list.add (s);

        json.getDynamicObject()->setProperty (juce::Identifier (derived.getFileName()), list);
        side.getParentDirectory().createDirectory();
        side.replaceWithText (juce::JSON::toString (json));
    }

    std::vector<SoundingFile> soundingFiles (te::Edit& edit, double start, double end, int track)
    {
        std::vector<SoundingFile> out;
        juce::Array<te::AudioTrack*> tracks;
        auto users = userTracks (edit);

        if (track >= 0 && track < users.size())
        {
            auto* group = laneGroupOf (*users[track]) != nullptr ? laneGroupOf (*users[track]) : users[track];

            for (auto* t : users)
                if (t == group || laneGroupOf (*t) == group)
                    tracks.add (t);

            if (auto* ft = freezeTrackOf (edit, *group))
                tracks.add (ft);
        }
        else
            for (auto* t : te::getAudioTracks (edit))
                tracks.add (t);

        auto add = [&out] (const juce::String& name, const juce::File& f)
        {
            for (auto& s : out)
                if (s.file == f)
                    return;

            out.push_back ({ name, f });
        };

        for (auto* t : tracks)
        {
            auto* group = laneGroupOf (*t) != nullptr ? laneGroupOf (*t) : t;
            bool notes = false;

            for (auto* c : t->getClips())
            {
                auto pos = c->getPosition();

                if (pos.getEnd().inSeconds() <= start || pos.getStart().inSeconds() >= end)
                    continue;

                if (auto* ac = dynamic_cast<te::AudioClipBase*> (c))
                    add (group->getName(), ac->getSourceFileReference().getFile());
                else if (dynamic_cast<te::MidiClip*> (c) != nullptr)
                    notes = true;
            }

            // MIDI im Bereich: die Samples des Samplers, den die Spur spielt
            if (! notes)
                continue;

            juce::String id;
            faderTrackOf (edit, *group, nullptr, &id);

            if (auto* mt = id.isNotEmpty() ? findModuleTrack (edit, id) : nullptr)
                if (auto* sp = dynamic_cast<te::SamplerPlugin*> (modulePlugin (*mt)))
                    for (int i = 0; i < sp->getNumSounds(); ++i)
                        add (group->getName(), te::SourceFileReference::findFileFromString (edit, sp->getSoundMedia (i)));
        }

        return out;
    }

    juce::StringArray soundingOrigins (te::Edit& edit, double start, double end, int track)
    {
        juce::StringArray out;

        for (auto& s : soundingFiles (edit, start, end, track))
            out.addArray (originsOf (s.file));

        out.removeDuplicates (false);
        return out;
    }
}
