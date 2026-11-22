#include "SampleLibrary.h"
#include "Resources.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace dawd
{
    juce::File kitsDirectory()
    {
        return resourceDir ("kits");
    }

    SampleLibrary::SampleLibrary (const juce::File& config, const juce::File& kitsDir)
        : configDir (config)
    {
        soundIndex = std::make_unique<SoundIndex> (config);

        // Mitgelieferte Kits: klein, gleich indizieren
        auto kits = std::make_shared<Index>();
        kits->root = kitsDir;
        kits->builtin = true;

        for (const auto& e : juce::RangedDirectoryIterator (kitsDir, true, "*", juce::File::findFiles))
            if (isAudioFile (e.getFile()))
                kits->paths.push_back (e.getFile().getRelativePathFrom (kitsDir).replaceCharacter ('\\', '/'));

        indexes.push_back (kits);

        // Angemeldete Ordner: Index aus dem Cache, sonst im Hintergrund bauen
        auto json = juce::JSON::parse (configDir.getChildFile ("libraries.json"));

        if (auto* roots = json.getProperty ("libraries", {}).getArray())
            for (auto& r : *roots)
            {
                auto idx = std::make_shared<Index>();
                idx->root = juce::File (r.toString());
                juce::StringArray lines;
                lines.addLines (cacheFileFor (idx->root).loadFileAsString());

                if (lines.size() > 0 && lines[0] == idx->root.getFullPathName())
                {
                    for (int i = 1; i < lines.size(); ++i)
                        if (lines[i].isNotEmpty())
                            idx->paths.push_back (lines[i]);

                    indexes.push_back (idx);
                }
                else
                {
                    indexes.push_back (idx);
                    startIndexing (idx);
                }
            }

        if (auto* blocked = json.getProperty ("noAnalysis", {}).getArray())
            for (auto& b : *blocked)
                noAnalysis.push_back (juce::File (b.toString()));

        for (auto& idx : indexes)
            soundIndex->load (idx->root);

        // Klang-Daten gesperrter Ordner nie laden (auch wenn libraries.json
        // von Hand geaendert wurde)
        for (auto& b : noAnalysis)
            soundIndex->forgetUnder (b);
    }

    SampleLibrary::~SampleLibrary()
    {
        soundIndex->stop();
        cancelled->store (true);
        pool.removeAllJobs (true, 10000);
    }

    bool SampleLibrary::isAudioFile (const juce::File& f)
    {
        auto ext = f.getFileExtension().toLowerCase();
        return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
    }

    namespace
    {
        // Ableton-Packs liegen teils als AIFF-C im eigenen "able"-Codec vor,
        // den nur Live lesen kann — solche Dateien gar nicht erst indizieren
        bool isUnreadableAiff (const juce::File& f)
        {
            auto ext = f.getFileExtension().toLowerCase();

            if (ext != ".aif" && ext != ".aiff")
                return false;

            juce::FileInputStream in (f);
            char head[128] = {};

            if (! in.openedOk() || in.read (head, sizeof (head)) < 12 || std::memcmp (head + 8, "AIFC", 4) != 0)
                return false;

            for (size_t i = 12; i + 4 <= sizeof (head); ++i)
                if (std::memcmp (head + i, "able", 4) == 0)
                    return true;

            return false;
        }
    }

    juce::File SampleLibrary::cacheFileFor (const juce::File& root) const
    {
        return configDir.getChildFile ("index").getChildFile (
            juce::String::toHexString (root.getFullPathName().hashCode64()) + ".txt");
    }

    void SampleLibrary::saveRoots() const
    {
        juce::Array<juce::var> roots;

        for (auto& idx : indexes)
            if (! idx->builtin)
                roots.add (idx->root.getFullPathName());

        auto o = new juce::DynamicObject();
        o->setProperty ("libraries", roots);

        if (! noAnalysis.empty())
        {
            juce::Array<juce::var> blocked;

            for (auto& b : noAnalysis)
                blocked.add (b.getFullPathName());

            o->setProperty ("noAnalysis", blocked);
        }

        configDir.createDirectory();
        configDir.getChildFile ("libraries.json").replaceWithText (juce::JSON::toString (juce::var (o)));
    }

    void SampleLibrary::startIndexing (std::shared_ptr<Index> idx)
    {
        idx->indexing = true;
        auto cache = cacheFileFor (idx->root);
        auto stop = cancelled;

        pool.addJob (std::function<juce::ThreadPoolJob::JobStatus()> ([this, idx, cache, stop]
        {
            std::vector<juce::String> found;
            juce::String text = idx->root.getFullPathName() + "\n";

            // std::filesystem folgt Verzeichnis-Symlinks nicht — Windows-
            // Junctions ("Eigene Musik" …) auf einer NTFS-Partition liessen
            // JUCEs Iterator im Kreis laufen
            namespace fs = std::filesystem;
            std::error_code ec;
            const auto rootPath = fs::path (idx->root.getFullPathName().toStdString());

            for (fs::recursive_directory_iterator it (rootPath, fs::directory_options::skip_permission_denied, ec), end;
                 ! ec && it != end; it.increment (ec))
            {
                if (stop->load())
                    return juce::ThreadPoolJob::jobHasFinished;

                std::error_code fec;

                // Vorhoer-Schnipsel von Presets sind keine Samples: versteckte
                // Ordner (NI ".previews") und Abletons "Previews"/"Ableton
                // Folder Info" nicht betreten
                auto fname = juce::String (it->path().filename().string());

                if (fname.startsWithChar ('.') || fname.equalsIgnoreCase ("Previews") || fname == "Ableton Folder Info")
                {
                    if (it->is_directory (fec))
                        it.disable_recursion_pending();

                    continue;
                }

                if (! it->is_regular_file (fec))
                    continue;

                juce::File file (juce::String (it->path().string()));

                if (isAudioFile (file) && ! isUnreadableAiff (file))
                {
                    // lexikalisch: fs::relative loeste Datei-Symlinks auf (Pfad zeigte
                    // aus der Bibliothek heraus)
                    auto rel = juce::String (it->path().lexically_relative (rootPath).generic_string());
                    found.push_back (rel);
                    text << rel << "\n";
                }
            }

            cache.getParentDirectory().createDirectory();
            cache.replaceWithText (text);

            {
                const std::lock_guard<std::mutex> sl (lock);
                idx->paths = std::move (found);
            }

            idx->indexing = false;
            return juce::ThreadPoolJob::jobHasFinished;
        }));
    }

    std::vector<SampleLibrary::Info> SampleLibrary::libraries() const
    {
        const std::lock_guard<std::mutex> sl (lock);
        std::vector<Info> out;

        for (auto& idx : indexes)
            out.push_back ({ idx->root, (int) idx->paths.size(), idx->indexing.load(), idx->builtin });

        return out;
    }

    juce::String SampleLibrary::addRoot (const juce::File& dir)
    {
        if (! dir.isDirectory())
            return "no such dir: " + dir.getFullPathName();

        std::shared_ptr<Index> idx;

        {
            const std::lock_guard<std::mutex> sl (lock);

            for (auto& i : indexes)
                if (i->root == dir || dir.isAChildOf (i->root))
                    return "already in the library: " + i->root.getFullPathName();

            idx = std::make_shared<Index>();
            idx->root = dir;
            indexes.push_back (idx);
            saveRoots();
        }

        soundIndex->load (dir);
        startIndexing (idx);
        return {};
    }

    juce::String SampleLibrary::removeRoot (const juce::File& dir)
    {
        const std::lock_guard<std::mutex> sl (lock);

        for (size_t i = 0; i < indexes.size(); ++i)
            if (indexes[i]->root == dir)
            {
                if (indexes[i]->builtin)
                    return "the built-in kits cannot be removed";

                cacheFileFor (dir).deleteFile();
                soundIndex->forget (dir);
                indexes.erase (indexes.begin() + (long) i);
                saveRoots();
                return {};
            }

        return "not in the library: " + dir.getFullPathName();
    }

    void SampleLibrary::refresh()
    {
        std::vector<std::shared_ptr<Index>> todo;

        {
            const std::lock_guard<std::mutex> sl (lock);

            for (auto& idx : indexes)
                if (! idx->builtin && ! idx->indexing)
                    todo.push_back (idx);
        }

        for (auto& idx : todo)
            startIndexing (idx);
    }

    std::vector<SoundIndex::Item> SampleLibrary::items (const juce::File& within) const
    {
        std::vector<SoundIndex::Item> out;
        const std::lock_guard<std::mutex> sl (lock);

        for (auto& idx : indexes)
        {
            juce::String prefix;

            if (within != juce::File())
            {
                if (within != idx->root && ! within.isAChildOf (idx->root) && ! idx->root.isAChildOf (within))
                    continue;

                if (within.isAChildOf (idx->root))
                    prefix = within.getRelativePathFrom (idx->root).replaceCharacter ('\\', '/') + "/";
            }

            // Gesperrte Ordner unter dieser Wurzel als relative Praefixe
            std::vector<juce::String> blocked;
            bool rootBlocked = false;

            for (auto& b : noAnalysis)
            {
                if (b == idx->root || idx->root.isAChildOf (b))
                    rootBlocked = true;
                else if (b.isAChildOf (idx->root))
                    blocked.push_back (b.getRelativePathFrom (idx->root).replaceCharacter ('\\', '/') + "/");
            }

            if (rootBlocked)
                continue;

            for (auto& rel : idx->paths)
                if ((prefix.isEmpty() || rel.startsWith (prefix))
                    && std::none_of (blocked.begin(), blocked.end(), [&] (auto& p) { return rel.startsWith (p); }))
                    out.push_back ({ idx->root, rel });
        }

        return out;
    }

    juce::String SampleLibrary::blockAnalysis (const juce::File& dir, int& removed)
    {
        removed = 0;

        if (auto by = analysisBlockedBy (dir); by != juce::File())
            return "already without sound analysis: " + by.getFullPathName();

        // Laufende Analyse anhalten (stop wartet auf den Worker), dann die
        // Klang-Daten darunter loeschen
        soundIndex->stop();
        removed = soundIndex->forgetUnder (dir);

        const std::lock_guard<std::mutex> sl (lock);
        // Engere Sperren darunter gehen in der neuen auf
        noAnalysis.erase (std::remove_if (noAnalysis.begin(), noAnalysis.end(), [&] (auto& b) { return b.isAChildOf (dir); }),
                          noAnalysis.end());
        noAnalysis.push_back (dir);
        saveRoots();
        return {};
    }

    juce::String SampleLibrary::allowAnalysis (const juce::File& dir)
    {
        const std::lock_guard<std::mutex> sl (lock);

        for (size_t i = 0; i < noAnalysis.size(); ++i)
            if (noAnalysis[i] == dir)
            {
                noAnalysis.erase (noAnalysis.begin() + (long) i);
                saveRoots();
                return {};
            }

        for (auto& b : noAnalysis)
            if (dir.isAChildOf (b))
                return "blocked through its parent " + b.getFullPathName() + " - allow that folder instead";

        return "not blocked: " + dir.getFullPathName();
    }

    std::vector<juce::File> SampleLibrary::blockedForAnalysis() const
    {
        const std::lock_guard<std::mutex> sl (lock);
        return noAnalysis;
    }

    juce::File SampleLibrary::analysisBlockedBy (const juce::File& f) const
    {
        const std::lock_guard<std::mutex> sl (lock);

        for (auto& b : noAnalysis)
            if (f == b || f.isAChildOf (b))
                return b;

        return {};
    }

    std::vector<SampleLibrary::Hit> SampleLibrary::find (const juce::String& query, const juce::File& within,
                                                         int limit, bool& indexing, int& total) const
    {
        juce::StringArray words;
        words.addTokens (query.toLowerCase(), " _-/", "");
        words.removeEmptyStrings();

        std::vector<Hit> hits;
        indexing = false;
        const std::lock_guard<std::mutex> sl (lock);

        for (auto& idx : indexes)
        {
            indexing = indexing || idx->indexing;
            juce::String prefix;

            if (within != juce::File())
            {
                if (within != idx->root && ! within.isAChildOf (idx->root))
                    continue;

                prefix = within == idx->root ? juce::String() : within.getRelativePathFrom (idx->root).replaceCharacter ('\\', '/') + "/";
            }

            for (auto& rel : idx->paths)
            {
                if (prefix.isNotEmpty() && ! rel.startsWith (prefix))
                    continue;

                auto lower = rel.toLowerCase();
                auto name = lower.fromLastOccurrenceOf ("/", false, false);
                double score = 0.0;
                bool all = true;

                for (auto& w : words)
                {
                    if (name.contains (w))
                        score += name.startsWith (w) ? 4.0 : 3.0;
                    else if (lower.contains (w))
                        score += 1.0;
                    else
                    {
                        all = false;
                        break;
                    }
                }

                if (! all)
                    continue;

                // Kuerzere Namen und flachere Pfade zuerst (spezifischer)
                score -= name.length() * 0.01 + lower.length() * 0.001;
                hits.push_back ({ idx->root.getChildFile (rel), idx->root, score });
            }
        }

        std::sort (hits.begin(), hits.end(), [] (auto& a, auto& b) { return a.score > b.score; });
        total = (int) hits.size();

        if (limit > 0 && (int) hits.size() > limit)
            hits.resize ((size_t) limit);

        return hits;
    }
}
