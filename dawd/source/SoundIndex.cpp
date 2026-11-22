#include "SoundIndex.h"
#include "Clap.h"
#include "Transcription.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <set>

#if JUCE_LINUX
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

namespace dawd
{
    namespace
    {
        constexpr int recordMagic = 0x31444E53;   // "SND1"
        constexpr int maxWindows = 6;               // 60 s je Datei
        const char* header = "dawd-sound 1 clap-general-q8 f11\n";

        // Kategorie: API-Name, Text fuer CLAP (Zero-Shot), Teilworte im Pfad,
        // ganze Worte im Pfad. Reihenfolge = Vorrang (Kick vor Bass, Pad vor
        // Synth).
        struct Category
        {
            const char* name;
            const char* prompt;
            std::vector<const char*> parts, words;
        };

        const std::vector<Category>& categories()
        {
            static const std::vector<Category> c {
                { "kick", "a kick drum hit", { "kick", "bassdrum", "bass drum" }, { "bd", "kck" } },
                { "snare", "a snare drum hit", { "snare", "rimshot" }, { "snr", "rim" } },
                { "clap", "a hand clap", { "clap" }, {} },
                { "hihat", "a hi-hat cymbal", { "hihat", "hi-hat", "hi hat" }, { "hh", "hat", "hats" } },
                { "cymbal", "a crash or ride cymbal", { "cymbal", "crash", "splash" }, { "ride", "china" } },
                { "tom", "a tom drum hit", {}, { "tom", "toms" } },
                { "percussion", "a percussion instrument like shaker, conga or cowbell",
                  { "shaker", "conga", "bongo", "tambourine", "cowbell", "percussion", "clave", "woodblock", "guiro",
                    "cabasa", "triangle", "timbale", "djembe" }, { "perc", "tamb" } },
                { "drums", "a drum loop with a full drum kit", { "drumloop", "drum loop", "breakbeat" },
                  { "beat", "beats", "break", "drums", "groove" } },
                { "vocal", "a human voice singing or speaking", { "vocal", "voice", "choir", "acapella", "singer" },
                  { "vox", "voc", "sing" } },
                { "bass", "a bass note", { "bass" }, { "sub", "808" } },
                { "keys", "a piano or keyboard", { "piano", "rhodes", "wurli", "organ", "clavinet", "epiano", "harpsichord" },
                  { "keys", "ep" } },   // nicht "key": Landr schreibt die Tonart so ("Guitar_keyC")
                { "guitar", "an electric or acoustic guitar", { "guitar", "dobro", "banjo", "ukulele" }, { "gtr", "tele", "strat" } },
                { "strings", "a string section with violins and cellos", { "string", "violin", "viola", "cello", "orchestra", "pizzicato", "harp" }, {} },
                { "winds", "a brass or woodwind instrument", { "brass", "horn", "trumpet", "trombone", "sax", "flute", "clarinet", "oboe", "bassoon", "tuba" }, {} },
                { "pad", "an ambient synth pad", {}, { "pad", "pads" } },
                { "synth", "a synthesizer lead or pluck", { "synth", "pluck", "arpegg" }, { "lead", "arp", "stab" } },
                { "fx", "a sound effect or noise", { "riser", "impact", "sweep", "whoosh", "vinyl", "foley", "noise", "downlifter",
                  "uplifter", "texture", "atmos" }, { "fx", "sfx" } },
            };
            return c;
        }

        // Worte eines Pfadteils: CamelCase und Ziffern trennen, klein
        std::set<juce::String> wordsOf (const juce::String& s)
        {
            std::set<juce::String> out;
            juce::String cur;
            juce::juce_wchar prev = 0;

            for (auto p = s.getCharPointer(); ! p.isEmpty();)
            {
                auto ch = p.getAndAdvance();
                bool alnum = juce::CharacterFunctions::isLetterOrDigit (ch);
                bool split = ! alnum
                          || (juce::CharacterFunctions::isUpperCase (ch) && juce::CharacterFunctions::isLowerCase (prev))
                          || (juce::CharacterFunctions::isDigit (ch) != juce::CharacterFunctions::isDigit (prev) && prev != 0
                              && juce::CharacterFunctions::isLetterOrDigit (prev));

                if (split && cur.isNotEmpty())
                {
                    out.insert (cur.toLowerCase());
                    cur.clear();
                }

                if (alnum)
                    cur += juce::String::charToString (ch);

                prev = ch;
            }

            if (cur.isNotEmpty())
                out.insert (cur.toLowerCase());

            return out;
        }

        int categoryOfPart (const juce::String& part)
        {
            auto lower = part.toLowerCase();
            auto words = wordsOf (part);
            const auto& cats = categories();

            for (size_t i = 0; i < cats.size(); ++i)
            {
                for (auto* p : cats[i].parts)
                    if (lower.contains (p))
                        return (int) i;

                for (auto* w : cats[i].words)
                    if (words.count (w) > 0)
                        return (int) i;
            }

            return -1;
        }

        void lowerPriority()
        {
           #if JUCE_LINUX
            setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), 19);
           #endif
        }

        std::string keyOf (const juce::File& f) { return f.getFullPathName().toStdString(); }
    }

    const juce::StringArray& SoundIndex::categoryNames()
    {
        static const juce::StringArray names = []
        {
            juce::StringArray a;

            for (auto& c : categories())
                a.add (c.name);

            return a;
        }();

        return names;
    }

    int SoundIndex::categoryFromName (const juce::String& relPath)
    {
        // Dateiname zuerst, dann die Ordner vom naechsten aufwaerts
        juce::StringArray parts;
        parts.addTokens (relPath, "/", "");

        if (parts.isEmpty())
            return -1;

        parts.set (parts.size() - 1, parts[parts.size() - 1].upToLastOccurrenceOf (".", false, false));

        for (int i = parts.size(); --i >= 0;)
            if (auto c = categoryOfPart (parts[i]); c >= 0)
                return c;

        return -1;
    }

    int SoundIndex::categoryFromText (const juce::String& text)
    {
        return categoryOfPart (text);
    }

    SoundIndex::SoundIndex (const juce::File& config) : configDir (config) {}

    SoundIndex::~SoundIndex()
    {
        stop();
    }

    juce::File SoundIndex::fileFor (const juce::File& root) const
    {
        return configDir.getChildFile ("index").getChildFile (
            juce::String::toHexString (root.getFullPathName().hashCode64()) + ".sound");
    }

    void SoundIndex::load (const juce::File& root)
    {
        Root r;
        r.root = root;
        r.file = fileFor (root);
        juce::MemoryBlock data;

        if (r.file.existsAsFile() && r.file.loadFileAsData (data))
        {
            juce::MemoryInputStream in (data, false);
            auto first = in.readNextLine() + "\n";
            auto rootLine = in.readNextLine();

            if (first == header && rootLine == root.getFullPathName())
            {
                const std::lock_guard<std::mutex> sl (lock);

                while (in.getNumBytesRemaining() >= 8)
                {
                    if (in.readInt() != recordMagic)
                        break;

                    auto len = in.readInt();

                    if (len <= 0 || len > in.getNumBytesRemaining())
                        break;   // abgeschnitten (Absturz beim Schreiben)

                    juce::MemoryBlock payload;
                    in.readIntoMemoryBlock (payload, len);
                    juce::MemoryInputStream p (payload, false);
                    auto relLen = p.readInt();
                    juce::MemoryBlock relBytes;
                    p.readIntoMemoryBlock (relBytes, relLen);
                    auto rel = juce::String::fromUTF8 ((const char*) relBytes.getData(), (int) relBytes.getSize());
                    Record rec;
                    rec.size = p.readInt64();
                    rec.mtime = p.readInt64();
                    rec.ok = p.readByte() != 0;
                    rec.entry.category = (int) (signed char) p.readByte();
                    rec.entry.categoryFromName = p.readByte() != 0;
                    std::array<float, sound::Features::count> fv {};

                    for (auto& v : fv)
                        v = p.readFloat();

                    rec.entry.features = sound::Features::unpack (fv.data());
                    auto windows = p.readInt();

                    if (windows < 0 || windows > maxWindows)
                        break;

                    for (int w = 0; w < windows; ++w)
                    {
                        rec.entry.starts.push_back (p.readFloat());

                        for (int k = 0; k < clap::dim; ++k)
                            rec.entry.vecs.push_back (p.readFloat());
                    }

                    records[keyOf (root.getChildFile (rel))] = std::move (rec);
                    ++r.records;
                }
            }
        }

        const std::lock_guard<std::mutex> sl (lock);
        roots.erase (std::remove_if (roots.begin(), roots.end(), [&] (auto& x) { return x.root == root; }), roots.end());
        roots.push_back (r);
    }

    void SoundIndex::forget (const juce::File& root)
    {
        const std::lock_guard<std::mutex> sl (lock);
        auto prefix = root.getFullPathName().toStdString() + "/";

        for (auto it = records.begin(); it != records.end();)
            it = it->first.rfind (prefix, 0) == 0 ? records.erase (it) : std::next (it);

        fileFor (root).deleteFile();
        roots.erase (std::remove_if (roots.begin(), roots.end(), [&] (auto& x) { return x.root == root; }), roots.end());
    }

    int SoundIndex::forgetUnder (const juce::File& dir)
    {
        const std::lock_guard<std::mutex> sl (lock);
        auto prefix = dir.getFullPathName().toStdString() + "/";
        int removed = 0;

        for (auto it = records.begin(); it != records.end();)
            if (it->first.rfind (prefix, 0) == 0)
            {
                it = records.erase (it);
                ++removed;
            }
            else
                ++it;

        if (removed == 0)
            return 0;

        // Betroffene Dateien neu schreiben; ohne verbliebene Datensaetze weg
        for (auto& r : roots)
        {
            if (! (r.root == dir || dir.isAChildOf (r.root) || r.root.isAChildOf (dir)))
                continue;

            auto rootPrefix = r.root.getFullPathName().toStdString() + "/";
            bool any = false;

            for (auto& [key, rec] : records)
                if (key.rfind (rootPrefix, 0) == 0)
                {
                    any = true;
                    break;
                }

            if (any)
                rewrite (r);
            else
            {
                r.file.deleteFile();
                r.records = 0;
            }
        }

        return removed;
    }

    void SoundIndex::appendRecord (Root& r, const std::string& key, const juce::String& rel, const Record& rec)
    {
        juce::ignoreUnused (key);
        juce::MemoryOutputStream p;
        auto relUtf8 = rel.toStdString();
        p.writeInt ((int) relUtf8.size());
        p.write (relUtf8.data(), relUtf8.size());
        p.writeInt64 (rec.size);
        p.writeInt64 (rec.mtime);
        p.writeByte (rec.ok ? 1 : 0);
        p.writeByte ((char) (signed char) rec.entry.category);
        p.writeByte (rec.entry.categoryFromName ? 1 : 0);

        for (auto v : rec.entry.features.pack())
            p.writeFloat (v);

        p.writeInt ((int) rec.entry.starts.size());

        for (size_t w = 0; w < rec.entry.starts.size(); ++w)
        {
            p.writeFloat (rec.entry.starts[w]);

            for (int k = 0; k < clap::dim; ++k)
                p.writeFloat (rec.entry.vecs[w * clap::dim + (size_t) k]);
        }

        if (! r.file.existsAsFile())
        {
            r.file.getParentDirectory().createDirectory();
            r.file.replaceWithText (juce::String (header) + r.root.getFullPathName() + "\n");
            r.records = 0;
        }

        juce::FileOutputStream out (r.file);   // haengt an

        if (out.openedOk())
        {
            out.writeInt (recordMagic);
            out.writeInt ((int) p.getDataSize());
            out.write (p.getData(), p.getDataSize());
            ++r.records;
        }
    }

    void SoundIndex::rewrite (Root& r)
    {
        auto tmp = r.file.getSiblingFile (r.file.getFileName() + ".tmp");
        tmp.deleteFile();
        Root fresh = r;
        fresh.file = tmp;
        auto prefix = r.root.getFullPathName().toStdString() + "/";

        for (auto& [key, rec] : records)
            if (key.rfind (prefix, 0) == 0)
                appendRecord (fresh, key, juce::String::fromUTF8 (key.c_str() + prefix.size()), rec);

        if (tmp.existsAsFile() && tmp.moveFileTo (r.file))
            r.records = fresh.records;
    }

    juce::String SoundIndex::start (std::vector<Item> items)
    {
        if (auto err = clap::available(); err.isNotEmpty())
            return err;

        if (running)
            return {};

        if (manager.joinable())
            manager.join();

        cancel = false;
        done = 0;
        total = (int) items.size();
        failed = 0;
        skipped = 0;
        startedAt = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        {
            const std::lock_guard<std::mutex> sl (lock);
            lastError.clear();
        }
        running = true;
        manager = std::thread ([this, items = std::move (items)]() mutable { run (std::move (items)); });
        return {};
    }

    void SoundIndex::stop()
    {
        cancel = true;

        if (manager.joinable())
            manager.join();
    }

    SoundIndex::Progress SoundIndex::progress() const
    {
        Progress p;
        p.running = running;
        p.done = done;
        p.total = total;
        p.failed = failed;
        p.skipped = skipped;
        auto worked = p.done - p.skipped;

        if (p.running && worked > 0)
            p.etaSeconds = (juce::Time::getMillisecondCounterHiRes() / 1000.0 - startedAt) / worked * (p.total - p.done);

        const std::lock_guard<std::mutex> sl (lock);
        p.error = lastError;
        return p;
    }

    int SoundIndex::analyzedCount (const juce::File& root) const
    {
        const std::lock_guard<std::mutex> sl (lock);
        auto prefix = root.getFullPathName().toStdString() + "/";
        int n = 0;

        for (auto& [key, rec] : records)
            if (rec.ok && key.rfind (prefix, 0) == 0)
                ++n;

        return n;
    }

    std::vector<SoundIndex::Brief> SoundIndex::brief (const std::vector<juce::File>& files, const std::vector<float>* query) const
    {
        std::vector<Brief> out (files.size());
        const std::lock_guard<std::mutex> sl (lock);

        for (size_t i = 0; i < files.size(); ++i)
        {
            auto it = records.find (keyOf (files[i]));

            if (it == records.end() || ! it->second.ok)
                continue;

            auto& e = it->second.entry;
            auto& b = out[i];
            b.analyzed = true;
            b.features = e.features;
            b.category = e.category;
            b.categoryFromName = e.categoryFromName;

            if (query != nullptr && query->size() == (size_t) clap::dim)
            {
                b.score = -2.0;

                for (size_t w = 0; w < e.starts.size(); ++w)
                {
                    const float* v = e.vecs.data() + w * clap::dim;
                    double s = 0.0;

                    for (int k = 0; k < clap::dim; ++k)
                        s += (double) v[k] * (*query)[(size_t) k];

                    if (s > b.score)
                    {
                        b.score = s;
                        b.at = e.starts[w];
                    }
                }
            }
        }

        return out;
    }

    SoundIndex::Record SoundIndex::analyzeFile (const juce::File& file, const juce::String& rel, juce::AudioFormatManager& formats,
                                                clap::AudioEncoder& encoder, const std::vector<std::vector<float>>& labels,
                                                juce::String& error)
    {
        Record rec;
        rec.size = file.getSize();
        rec.mtime = file.getLastModificationTime().toMilliseconds();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return rec;   // unlesbar: als fehlgeschlagen merken, nicht erneut versuchen

        const double duration = (double) reader->lengthInSamples / reader->sampleRate;
        auto length = juce::jmin (reader->lengthInSamples, (juce::int64) (maxWindows * 10.0 * reader->sampleRate));
        auto ch = transcription::loadResampled (*reader, 0, length, clap::sampleRate, 1);

        if (ch.empty() || ch[0].empty())
            return rec;

        auto& audio = ch[0];
        auto& e = rec.entry;

        for (int w = 0; w < maxWindows; ++w)
        {
            size_t from = (size_t) w * clap::windowSamples;

            if (from >= audio.size() || (w > 0 && audio.size() - from < (size_t) (3.0 * clap::sampleRate)))
                break;

            auto len = std::min ((size_t) clap::windowSamples, audio.size() - from);
            auto v = encoder.embed (audio.data() + from, len, error);

            if (v.empty())
                return rec;

            e.starts.push_back ((float) (w * 10.0));
            e.vecs.insert (e.vecs.end(), v.begin(), v.end());
        }

        std::vector<float> head (audio.begin(), audio.begin() + (long) std::min (audio.size(), (size_t) (30.0 * clap::sampleRate)));
        e.features = sound::measure (head, duration);

        // Kategorie: der Name weiss es meist; sonst der naechste Kategorie-Text
        e.category = categoryFromName (rel);
        e.categoryFromName = e.category >= 0;

        if (e.category < 0 && ! labels.empty())
        {
            double best = -2.0;

            for (size_t c = 0; c < labels.size(); ++c)
            {
                double s = 0.0;

                for (int k = 0; k < clap::dim; ++k)
                    s += (double) e.vecs[(size_t) k] * labels[c][(size_t) k];

                if (s > best)
                {
                    best = s;
                    e.category = (int) c;
                }
            }
        }

        rec.ok = true;
        return rec;
    }

    void SoundIndex::run (std::vector<Item> items)
    {
        lowerPriority();

        // Kategorie-Texte einmal je Lauf
        std::vector<std::vector<float>> labels;

        for (auto& c : categories())
        {
            juce::String err;
            auto v = clap::embedText (c.prompt, err);

            if (v.empty())
            {
                labels.clear();
                break;
            }

            labels.push_back (std::move (v));
        }

        // Unveraenderte Dateien ueberspringen (Groesse + Aenderungszeit)
        std::vector<size_t> todo;

        for (size_t i = 0; i < items.size() && ! cancel; ++i)
        {
            auto f = items[i].root.getChildFile (items[i].rel);
            auto size = f.getSize();
            auto mtime = f.getLastModificationTime().toMilliseconds();
            bool same = false;
            {
                const std::lock_guard<std::mutex> sl (lock);
                auto it = records.find (keyOf (f));
                same = it != records.end() && it->second.size == size && it->second.mtime == mtime;
            }

            if (same)
            {
                ++skipped;
                ++done;
            }
            else
                todo.push_back (i);
        }

        std::atomic<size_t> next { 0 };
        const int workers = juce::jlimit (1, 8, juce::SystemStats::getNumPhysicalCpus() - 1);
        std::vector<std::thread> pool;

        for (int w = 0; w < workers; ++w)
            pool.emplace_back ([&]
            {
                lowerPriority();
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                clap::AudioEncoder encoder;

                while (! cancel)
                {
                    auto k = next++;

                    if (k >= todo.size())
                        break;

                    auto& item = items[todo[k]];
                    auto file = item.root.getChildFile (item.rel);
                    juce::String err;
                    auto rec = analyzeFile (file, item.rel, formats, encoder, labels, err);

                    if (err.isNotEmpty())
                    {
                        // Modellfehler: alles abbrechen, nichts als kaputt merken
                        const std::lock_guard<std::mutex> sl (lock);
                        lastError = err;
                        cancel = true;
                        break;
                    }

                    if (! rec.ok)
                        ++failed;

                    {
                        const std::lock_guard<std::mutex> sl (lock);
                        auto key = keyOf (file);

                        for (auto& r : roots)
                            if (r.root == item.root)
                                appendRecord (r, key, item.rel, rec);

                        records[key] = std::move (rec);
                    }

                    ++done;
                }
            });

        for (auto& t : pool)
            t.join();

        // Ueberholte Datensaetze aus den Dateien raeumen
        {
            const std::lock_guard<std::mutex> sl (lock);

            for (auto& r : roots)
            {
                auto prefix = r.root.getFullPathName().toStdString() + "/";
                int live = 0;

                for (auto& [key, rec] : records)
                    if (key.rfind (prefix, 0) == 0)
                        ++live;

                if (r.records > live + live / 4 + 16)
                    rewrite (r);
            }
        }

        running = false;
    }
}
