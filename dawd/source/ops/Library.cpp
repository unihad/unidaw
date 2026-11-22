#include "Ops.h"
#include "AnalysisSource.h"
#include "../Clap.h"
#include "../Transcription.h"

#include <cmath>
#include <cstring>
#include <map>

// Sample-Bibliotheken (Familie media, read-only bis auf die Anmeldung): das
// Gegenstueck zum Browser einer DAW — Ordner anmelden, nach Namen suchen,
// Treffer per setSamples auf einen Sampler oder per addClip in den Song.
// Dazu die Suche nach Klang: Text ("metallic snare") und Referenz (eine
// Datei, ein Ausschnitt, der Mix) ueber CLAP-Vektoren, Filter und Sortierung
// ueber gemessene Merkmale (SoundIndex). Die Analyse dafuer laeuft im
// Hintergrund (sampleLibraries {analyze}).
namespace dawd
{
    namespace
    {
        double round1 (double v) { return std::round (v * 10.0) / 10.0; }
        double round3 (double v) { return std::round (v * 1000.0) / 1000.0; }

        juce::var librariesVar (DawdContext& ctx)
        {
            juce::Array<juce::var> out;

            for (auto& l : ctx.library->libraries())
                out.add (resultObj (true, { { "dir", l.root.getFullPathName() }, { "files", l.files },
                                            { "indexing", l.indexing }, { "builtin", l.builtin },
                                            { "analyzed", ctx.library->sounds().analyzedCount (l.root) } }));

            const bool anyBlocked = ! ctx.library->blockedForAnalysis().empty();

            for (auto& v : out)
            {
                v.getDynamicObject()->removeProperty ("ok");

                if (! anyBlocked)
                    continue;

                // Dateien in Ordnern ohne Klang-Analyse (nur wenn es welche gibt)
                juce::File root (v.getProperty ("dir", {}).toString());
                auto blocked = (int) v.getProperty ("files", 0) - (int) ctx.library->items (root).size();

                if (blocked > 0)
                    v.getDynamicObject()->setProperty ("noAnalysis", blocked);
            }

            return out;
        }

        juce::var analysisVar (DawdContext& ctx)
        {
            auto p = ctx.library->sounds().progress();
            auto o = new juce::DynamicObject();
            o->setProperty ("running", p.running);
            o->setProperty ("done", p.done);
            o->setProperty ("total", p.total);
            o->setProperty ("failed", p.failed);
            o->setProperty ("unchanged", p.skipped);

            if (p.running && p.etaSeconds > 0.0)
                o->setProperty ("etaSeconds", (int) std::ceil (p.etaSeconds));

            if (p.error.isNotEmpty())
                o->setProperty ("error", p.error);

            return juce::var (o);
        }

        // Deutsche Klangworte -> Englisch (CLAP kennt nur Englisch). Umlaute
        // vorher umschreiben, Endungen (e, en, er, es, em) probeweise abwerfen;
        // unbekannte Worte bleiben stehen.
        juce::String toEnglish (const juce::String& text, bool& changed)
        {
            static const std::map<juce::String, juce::String> dict {
                { "bassdrum", "kick drum" }, { "basstrommel", "kick drum" }, { "schnarrtrommel", "snare" },
                { "becken", "cymbal" }, { "klatschen", "clap" }, { "trommel", "drum" }, { "schlagzeug", "drums" },
                { "gesang", "vocals" }, { "stimme", "voice" }, { "chor", "choir" }, { "weiblich", "female" },
                { "frau", "female" }, { "frauen", "female" }, { "maennlich", "male" }, { "mann", "male" },
                { "maenner", "male" }, { "metallisch", "metallic" }, { "druckvoll", "punchy" }, { "knackig", "punchy" },
                { "warm", "warm" }, { "hell", "bright" }, { "dunkel", "dark" }, { "dumpf", "muffled" },
                { "tief", "deep" }, { "hoch", "high" }, { "kurz", "short" }, { "lang", "long" },
                { "trocken", "dry" }, { "hall", "reverb" }, { "verhallt", "reverberant" }, { "verzerrt", "distorted" },
                { "weich", "soft" }, { "hart", "hard" }, { "laut", "loud" }, { "leise", "quiet" },
                { "luftig", "airy" }, { "flaeche", "pad" }, { "klavier", "piano" }, { "gitarre", "guitar" },
                { "streicher", "strings" }, { "geige", "violin" }, { "blaeser", "brass" }, { "floete", "flute" },
                { "rauschen", "noise" }, { "knistern", "crackle" }, { "glocke", "bell" }, { "schlag", "hit" },
                { "viel", "lots of" }, { "vielen", "lots of" }, { "wenig", "little" }, { "mit", "with" },
                { "ohne", "without" }, { "und", "and" }, { "ein", "a" }, { "eine", "a" }, { "einer", "a" },
                { "einem", "a" }, { "einen", "a" }, { "sehr", "very" }, { "stark", "strong" }, { "starkem", "strong" },
                { "schnell", "fast" }, { "langsam", "slow" }, { "sauber", "clean" }, { "dreckig", "dirty" },
                { "schmutzig", "dirty" }, { "rund", "round" }, { "fett", "fat" }, { "duenn", "thin" },
                { "breit", "wide" }, { "glaenzend", "shiny" }, { "analog", "analog" }, { "kick", "kick" },
                { "raum", "room" }, { "raeumlich", "spacious" }, { "hallig", "reverberant" }, { "kalt", "cold" },
                { "sanft", "gentle" }, { "rau", "rough" }, { "rauh", "rough" }, { "kratzig", "scratchy" },
                { "rauschig", "noisy" }, { "verrauscht", "noisy" }, { "zischend", "hissing" }, { "brummend", "humming" },
                { "knurrend", "growling" }, { "schrill", "shrill" }, { "hohl", "hollow" }, { "holzig", "woody" },
                { "glaesern", "glassy" }, { "klar", "clear" }, { "voll", "full" }, { "satt", "full" },
                { "bassig", "bassy" }, { "duester", "dark" }, { "unheimlich", "eerie" }, { "bedrohlich", "ominous" },
                { "traurig", "sad" }, { "froehlich", "happy" }, { "episch", "epic" }, { "vertraeumt", "dreamy" },
                { "mystisch", "mystical" }, { "atmosphaerisch", "atmospheric" }, { "schwebend", "floating" },
                { "pulsierend", "pulsing" }, { "perkussiv", "percussive" }, { "glitzernd", "sparkling" },
                { "funkelnd", "sparkling" }, { "edel", "noble" }, { "teuer", "expensive" },
            };

            auto t = text.toLowerCase()
                         .replace (juce::String::fromUTF8 ("\xc3\xa4"), "ae")
                         .replace (juce::String::fromUTF8 ("\xc3\xb6"), "oe")
                         .replace (juce::String::fromUTF8 ("\xc3\xbc"), "ue")
                         .replace (juce::String::fromUTF8 ("\xc3\x9f"), "ss");
            juce::StringArray words;
            words.addTokens (t, " ,;", "");
            words.removeEmptyStrings();
            juce::StringArray out;

            for (auto& w : words)
            {
                auto hit = dict.find (w);

                for (const char* ending : { "en", "er", "es", "em", "e" })
                    if (hit == dict.end() && w.endsWith (ending) && w.length() > 4)
                    {
                        auto stem = w.dropLastCharacters ((int) std::strlen (ending));
                        hit = dict.find (stem);

                        // ausgefallenes e: dunkle -> dunkel, teure -> teuer
                        if (hit == dict.end() && (stem.endsWith ("l") || stem.endsWith ("r")))
                            hit = dict.find (stem.dropLastCharacters (1) + "e" + stem.getLastCharacters (1));
                    }

                if (hit != dict.end() && hit->second != w)
                {
                    out.add (hit->second);
                    changed = true;
                }
                else
                    out.add (w);
            }

            return changed ? out.joinIntoString (" ") : text;
        }

        // Merkmale fuer die Antwort (nur, was etwas aussagt)
        juce::var featuresVar (const sound::Features& f)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("oneShot", f.oneShot);
            o->setProperty ("attackMs", round1 (f.attackMs));
            o->setProperty ("punchDb", round1 (f.punchDb));
            o->setProperty ("low", round3 (f.low));
            o->setProperty ("brightness", (int) std::round (f.brightness));

            if (f.pitchHz > 0.0)
            {
                o->setProperty ("pitchHz", round1 (f.pitchHz));
                o->setProperty ("note", sound::noteName (f.pitchHz));
            }

            if (f.vibratoCents > 0.0)
            {
                o->setProperty ("vibratoCents", round1 (f.vibratoCents));
                o->setProperty ("vibratoHz", round1 (f.vibratoHz));
            }

            if (f.bpm > 0.0)
                o->setProperty ("bpm", round1 (f.bpm));

            o->setProperty ("peakDb", round1 (f.peakDb));
            return juce::var (o);
        }

        // Sortier-Merkmale: Name -> (Wert, absteigend als Default)
        bool sortValue (const juce::String& key, const sound::Features& f, double& value, bool& descending)
        {
            descending = true;

            if (key == "punch")       value = f.punchDb;
            else if (key == "attack") { value = f.attackMs; descending = false; }
            else if (key == "low")    value = f.low;
            else if (key == "brightness") value = f.brightness;
            else if (key == "vibrato") value = f.vibratoCents;
            else if (key == "duration") { value = f.duration; descending = false; }
            else if (key == "pitch")  { value = f.pitchHz; descending = false; }
            else if (key == "loudness") value = f.peakDb;
            else return false;

            return true;
        }

        // Ein Wort der Anfrage, das ein messbares Merkmal meint ("punchy",
        // "vibrato") -> Sortierung danach (vibrato filtert ausserdem); sonst ""
        juce::String sortFromText (const juce::String& text)
        {
            auto t = text.toLowerCase();

            if (t.contains ("vibrato"))
                return "vibrato";

            if (t.contains ("punch"))
                return "punch";

            return {};
        }

        clap::AudioEncoder& referenceEncoder()
        {
            static clap::AudioEncoder e;   // Message-Thread
            return e;
        }

        // Referenz laden: "<Pfad>", {file, start?, end?} oder der Mix
        // ({section} | {start, end} | {track}) — Mono 48 kHz, hoechstens 60 s
        juce::String loadReference (DawdContext& ctx, const juce::var& likeIn, std::vector<float>& audio, double& duration,
                                    juce::String& label)
        {
            // Pfad als Text = {file} (Praxistest 1i/1m)
            auto like = likeIn;

            if (like.isString())
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("file", likeIn);
                like = juce::var (o);
            }

            if (! like.isObject())
                return "like needs a file path, {file, start?, end?} or a mix range {section | start, end | track}";

            std::unique_ptr<juce::AudioFormatReader> reader;
            double start = 0.0, end = 0.0;
            auto tmp = uniqueTempFile ("dawd-like", ".wav");
            const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };

            if (like.hasProperty ("file"))
            {
                auto path = like.getProperty ("file", {}).toString();
                juce::File file (path);

                if (path.isEmpty() || ! file.existsAsFile())
                    return "no such file: " + path;

                // Ordner ohne Klang-Analyse (Lizenz): auch nicht als Referenz
                if (auto by = ctx.library->analysisBlockedBy (file); by != juce::File())
                    return "no sound analysis for files in " + by.getFullPathName()
                         + " (blocked for its licence) - search there by name (query), or allowAnalysis lifts it";

                // ... auch keine Kopie davon (Herkunft, z. B. media/samples)
                if (auto e = blockedFileError (ctx, file); e.isNotEmpty())
                    return e;

                reader.reset (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));

                if (reader == nullptr)
                    return "cannot read audio file: " + file.getFileName();

                auto length = (double) reader->lengthInSamples / reader->sampleRate;
                start = (double) like.getProperty ("start", 0.0);
                end = like.hasProperty ("end") ? (double) like.getProperty ("end", 0.0) : length;

                if (start < 0.0 || end <= start || end > length + 1e-3)
                    return "need 0 <= start < end <= file length (" + juce::String (round3 (length)) + " s)";

                label = file.getFullPathName();
            }
            else
            {
                if (ctx.edit == nullptr)
                    return "no edit";

                int track = -1;

                if (auto err = renderMixRange (ctx, like, tmp, start, end, track); err.isNotEmpty())
                    return err;

                if (auto err = checkMixLicence (ctx, start, end, track, true); err.isNotEmpty())
                    return err;

                reader.reset (ctx.engine->getAudioFileFormatManager().getWavFormat()->createReaderFor (new juce::FileInputStream (tmp), true));

                if (reader == nullptr)
                    return "cannot read render";

                label = "mix " + juce::String (round3 (start)) + "-" + juce::String (round3 (end)) + " s";
                end -= start;
                start = 0.0;
            }

            duration = juce::jmin (end - start, 60.0);
            auto s0 = (juce::int64) (start * reader->sampleRate), n = (juce::int64) (duration * reader->sampleRate);
            auto ch = transcription::loadResampled (*reader, s0, n, clap::sampleRate, 1);

            if (ch.empty() || ch[0].empty())
                return "reference is empty";

            audio = std::move (ch[0]);
            return {};
        }
    }

    // Mittelwert der CLAP-Vektoren ueber bis zu sechs 10-s-Fenster (ein Rest
    // unter 3 s faellt weg), normiert. audio: Mono, 48 kHz.
    std::vector<float> embedAudio (const std::vector<float>& audio, juce::String& err)
    {
        std::vector<double> sum ((size_t) clap::dim, 0.0);
        int windows = 0;

        for (size_t from = 0; from < audio.size() && windows < 6; from += clap::windowSamples)
        {
            if (windows > 0 && audio.size() - from < (size_t) (3.0 * clap::sampleRate))
                break;

            auto v = referenceEncoder().embed (audio.data() + from, std::min ((size_t) clap::windowSamples, audio.size() - from), err);

            if (v.empty())
                return {};

            for (int k = 0; k < clap::dim; ++k)
                sum[(size_t) k] += v[(size_t) k];

            ++windows;
        }

        double norm = 0.0;

        for (auto v : sum)
            norm += v * v;

        norm = std::sqrt (std::max (norm, 1e-20));
        std::vector<float> out;

        for (auto v : sum)
            out.push_back ((float) (v / norm));

        return out;
    }

    juce::String soundQuery (DawdContext& ctx, const juce::String& text, const juce::var& like, std::vector<float>& qvec,
                             juce::var& reference, juce::String* english)
    {
        if (text.isNotEmpty() && ! like.isVoid())
            return "text or like, not both";

        if (auto err = clap::available(); err.isNotEmpty())
            return err;

        juce::String err;

        if (text.isNotEmpty())
        {
            bool translated = false;
            auto en = toEnglish (text, translated);

            if (english != nullptr)
                *english = en;

            qvec = clap::embedText (en, err);
            return qvec.empty() ? err : juce::String();
        }

        std::vector<float> audio;
        double duration = 0.0;
        juce::String label;

        if (auto e = loadReference (ctx, like, audio, duration, label); e.isNotEmpty())
            return e;

        qvec = embedAudio (audio, err);

        if (qvec.empty())
            return err;

        std::vector<float> head (audio.begin(), audio.begin() + (long) std::min (audio.size(), (size_t) (30.0 * clap::sampleRate)));
        auto ref = new juce::DynamicObject();
        ref->setProperty ("source", label);
        ref->setProperty ("features", featuresVar (sound::measure (head, duration)));
        reference = juce::var (ref);
        return {};
    }

    juce::String embedSoundFile (DawdContext& ctx, const juce::File& file, double start, double end, juce::DynamicObject& out)
    {
        if (auto err = clap::available(); err.isNotEmpty())
            return err;

        auto like = new juce::DynamicObject();
        like->setProperty ("file", file.getFullPathName());
        like->setProperty ("start", start);
        like->setProperty ("end", end);
        std::vector<float> audio;
        double duration = 0.0;
        juce::String label, err;

        if (auto e = loadReference (ctx, juce::var (like), audio, duration, label); e.isNotEmpty())
            return e;

        auto v = embedAudio (audio, err);

        if (v.empty())
            return err;

        std::vector<float> head (audio.begin(), audio.begin() + (long) std::min (audio.size(), (size_t) (30.0 * clap::sampleRate)));
        out.setProperty ("clap", clap::encode (v));
        out.setProperty ("sound", featuresVar (sound::measure (head, duration)));
        return {};
    }

    // Ohne Parameter: die Bibliotheken; add/remove melden einen Ordner an
    // oder ab (bleibt ueber Neustarts), refresh baut die Indizes neu;
    // analyze true | "<dir>" startet die Klang-Analyse (alle oder ein
    // Ordner; Unveraendertes wird uebersprungen), false haelt sie an;
    // noAnalysis/allowAnalysis "<dir>" sperrt einen Ordner fuer die
    // Analyse bzw. gibt ihn frei
    juce::var opSampleLibraries (DawdContext& ctx, const juce::var& cmd)
    {
        // Schon angemeldet ist kein Fehler (Praxistest remix-1): ok mit note,
        // die Liste bleibt
        juce::String note;

        if (cmd.hasProperty ("add"))
            if (auto err = ctx.library->addRoot (juce::File (cmd.getProperty ("add", {}).toString())); err.isNotEmpty())
            {
                if (! err.startsWith ("already in the library"))
                    return resultObj (false, { { "error", err } });

                note = err;
            }

        if (cmd.hasProperty ("remove"))
            if (auto err = ctx.library->removeRoot (juce::File (cmd.getProperty ("remove", {}).toString())); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        if ((bool) cmd.getProperty ("refresh", false))
            ctx.library->refresh();

        // Klang-Analyse je Ordner sperren/freigeben (Lizenz; Decision
        // 2026-11-18-klang-analyse-je-hersteller): noAnalysis loescht die
        // Klang-Daten darunter, die Namenssuche bleibt
        int removed = -1;

        for (auto key : { "noAnalysis", "allowAnalysis" })
            if (cmd.hasProperty (key))
            {
                auto path = cmd.getProperty (key, {}).toString();

                if (! juce::File::isAbsolutePath (path))
                    return resultObj (false, { { "error", juce::String (key) + " needs an absolute folder path" } });

                const bool wasRunning = ctx.library->sounds().progress().running;
                auto err = juce::String (key) == "noAnalysis" ? ctx.library->blockAnalysis (juce::File (path), removed)
                                                              : ctx.library->allowAnalysis (juce::File (path));

                if (err.isNotEmpty())
                    return resultObj (false, { { "error", err } });

                if (juce::String (key) == "noAnalysis" && wasRunning)
                    note = "the running analysis was stopped - start it again for the other folders";
            }

        if (cmd.hasProperty ("analyze"))
        {
            auto a = cmd.getProperty ("analyze", {});

            if (a.isBool() && ! (bool) a)
                ctx.library->sounds().stop();
            else
            {
                juce::File within;

                if (a.isString())
                {
                    within = juce::File (a.toString());

                    if (! within.isDirectory())
                        return resultObj (false, { { "error", "no such dir: " + a.toString() } });

                    if (auto by = ctx.library->analysisBlockedBy (within); by != juce::File())
                        return resultObj (false, { { "error", "no sound analysis in " + by.getFullPathName()
                                                              + " (blocked for its licence; allowAnalysis lifts it)" } });
                }

                auto items = ctx.library->items (within);

                if (items.empty())
                    return resultObj (false, { { "error", "nothing to analyze (no indexed samples there yet, or all in folders without sound analysis)" } });

                if (auto err = ctx.library->sounds().start (std::move (items)); err.isNotEmpty())
                    return resultObj (false, { { "error", err } });
            }
        }

        juce::Array<juce::var> blocked;

        for (auto& b : ctx.library->blockedForAnalysis())
            blocked.add (b.getFullPathName());

        auto res = resultObj (true, { { "libraries", librariesVar (ctx) }, { "analysis", analysisVar (ctx) },
                                      { "noAnalysis", blocked } });

        if (removed >= 0)
            res.getDynamicObject()->setProperty ("removed", removed);

        if (note.isNotEmpty())
            res.getDynamicObject()->setProperty ("note", note);

        return res;
    }

    // Suche ueber alle Bibliotheken: alle Worte muessen im Pfad stehen
    // (Ordnernamen zaehlen — "kick 808", "snare brush", "loop 90"), Treffer
    // im Dateinamen zuerst; dir schraenkt auf einen Ordner ein (ohne query
    // = blaettern). Je Treffer Dauer und Kanaele.
    //
    // Nach Klang (braucht die Analyse): text (Beschreibung, Englisch oder
    // gaengige deutsche Klangworte) oder like (Referenz) ordnen nach
    // CLAP-Aehnlichkeit; category filtert (aus text erkannt, "any" schaltet
    // das ab), oneShot/minDuration/maxDuration auch; sort ordnet nach einem
    // Merkmal (mit text/like: die aehnlichsten zuerst auswaehlen, dann
    // sortieren). query/dir grenzen vorher ein.
    juce::var opFindSamples (DawdContext& ctx, const juce::var& cmd)
    {
        auto query = cmd.getProperty ("query", {}).toString();
        auto dirText = cmd.getProperty ("dir", {}).toString();
        auto within = dirText.isNotEmpty() ? juce::File (dirText) : juce::File();
        auto limit = juce::jlimit (1, 200, (int) cmd.getProperty ("limit", 30));
        auto text = cmd.getProperty ("text", {}).toString().trim();
        auto like = cmd.getProperty ("like", {});
        auto categoryText = cmd.getProperty ("category", {}).toString();
        auto sortKey = cmd.getProperty ("sort", {}).toString();
        const bool bySound = text.isNotEmpty() || cmd.hasProperty ("like") || categoryText.isNotEmpty()
                          || sortKey.isNotEmpty() || cmd.hasProperty ("oneShot") || cmd.hasProperty ("minDuration")
                          || cmd.hasProperty ("maxDuration");

        if (! bySound && query.trim().isEmpty() && dirText.isEmpty())
            return resultObj (false, { { "error", "need a query (words in the file path), a dir to browse, or text/like to search by sound" } });

        if (dirText.isNotEmpty() && ! within.isDirectory())
            return resultObj (false, { { "error", "no such dir: " + dirText } });

        if (text.isNotEmpty() && cmd.hasProperty ("like"))
            return resultObj (false, { { "error", "text or like, not both" } });

        // channels: nur Dateien mit so vielen Kanaelen (1 = mono, 2 = stereo)
        const int channels = (int) cmd.getProperty ("channels", 0);

        if (cmd.hasProperty ("channels") && channels != 1 && channels != 2)
            return resultObj (false, { { "error", "channels is 1 (mono) or 2 (stereo)" } });

        // Treffer oeffnen: was sich nicht lesen laesst (fremde Codecs), fehlt
        // (Bibliotheks-Platte nicht eingehaengt) oder eine andere Kanalzahl
        // hat, faellt weg. Hoechstens maxChecks Dateien je Suche.
        const int maxChecks = channels > 0 ? std::max (limit * 4 + 20, 500) : limit * 4 + 20;
        int skipped = 0, missing = 0, checked = 0;

        auto openHit = [&] (const juce::File& f) -> std::unique_ptr<juce::AudioFormatReader>
        {
            ++checked;

            if (! f.existsAsFile())
            {
                ++missing;
                ++skipped;
                return {};
            }

            std::unique_ptr<juce::AudioFormatReader> reader { ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f) };

            if (reader == nullptr || (channels > 0 && (int) reader->numChannels != channels))
            {
                ++skipped;
                return {};
            }

            return reader;
        };

        // Warum die Liste kuerzer ist als total vermuten liesse
        auto noteSkipped = [&] (juce::DynamicObject& r, bool capped)
        {
            if (missing > 0)
            {
                r.setProperty ("missing", missing);
                r.setProperty ("hint", juce::String (missing) + " of the checked files are missing on disk - is the library drive mounted? (sampleLibraries lists the folders)");
            }
            else if (capped && channels > 0)
                r.setProperty ("hint", "channels checked in the first " + juce::String (checked) + " files only - narrow the query or dir for more");
        };

        // Kategorie: ausdruecklich, sonst aus dem Text erkannt
        int category = -1;
        bool autoCategory = false;
        bool translated = false;
        auto english = text.isNotEmpty() ? toEnglish (text, translated) : juce::String();

        if (categoryText.isNotEmpty() && categoryText != "any")
        {
            category = SoundIndex::categoryNames().indexOf (categoryText);

            if (category < 0)
                return resultObj (false, { { "error", "unknown category: " + categoryText + " (" + SoundIndex::categoryNames().joinIntoString (", ") + ")" } });
        }
        else if (categoryText.isEmpty() && english.isNotEmpty())
        {
            category = SoundIndex::categoryFromText (english);
            autoCategory = category >= 0;
        }

        bool descending = true;
        double dummy = 0.0;
        bool autoSort = false;

        if (sortKey.isEmpty() && english.isNotEmpty())
        {
            sortKey = sortFromText (english);
            autoSort = sortKey.isNotEmpty();
        }

        // "vibrato" im Text ist eine Bedingung, keine Reihenfolge: nur
        // Treffer mit gemessenem Vibrato (sonst stehen oben die
        // aehnlichsten Vocals, und keins davon hat Vibrato)
        const bool needVibrato = autoSort && sortKey == "vibrato";

        if (sortKey.isNotEmpty() && ! sortValue (sortKey, {}, dummy, descending))
            return resultObj (false, { { "error", "unknown sort: " + sortKey + " (punch, attack, low, brightness, vibrato, duration, pitch, loudness)" } });

        if (cmd.hasProperty ("order"))
        {
            auto order = cmd.getProperty ("order", {}).toString();

            if (order != "asc" && order != "desc")
                return resultObj (false, { { "error", "order is asc or desc" } });

            descending = order == "desc";
        }

        // Anfrage-Vektor
        std::vector<float> qvec;
        juce::var reference;

        if (english.isNotEmpty() || cmd.hasProperty ("like"))
            if (auto err = soundQuery (ctx, text, like, qvec, reference); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        bool indexing = false;
        int total = 0;

        if (! bySound)
        {
            // Mehr Kandidaten holen als gebraucht: was openHit verwirft, faellt
            // hier weg
            auto hits = ctx.library->find (query, within, maxChecks, indexing, total);
            juce::Array<juce::var> out;

            for (auto& h : hits)
            {
                if (out.size() >= limit)
                    break;

                auto reader = openHit (h.file);

                if (reader == nullptr)
                    continue;

                auto o = new juce::DynamicObject();
                o->setProperty ("file", h.file.getFullPathName());
                o->setProperty ("name", h.file.getFileNameWithoutExtension());
                o->setProperty ("folder", h.file.getParentDirectory().getRelativePathFrom (h.root).replaceCharacter ('\\', '/'));
                o->setProperty ("library", h.root.getFullPathName());

                o->setProperty ("duration", std::round ((double) reader->lengthInSamples / reader->sampleRate * 1000.0) / 1000.0);
                o->setProperty ("channels", (int) reader->numChannels);

                out.add (juce::var (o));
            }

            auto res = resultObj (true, { { "query", query }, { "total", total - skipped }, { "samples", out } });
            noteSkipped (*res.getDynamicObject(), out.size() < limit && (int) hits.size() < total);

            if (indexing)
                res.getDynamicObject()->setProperty ("indexing", true);

            return res;
        }

        // Nach Klang: alle Kandidaten (Name/Ordner grenzen ein), dann die
        // analysierten filtern, bewerten, sortieren
        auto hits = ctx.library->find (query, within, 0, indexing, total);
        std::vector<juce::File> files;
        files.reserve (hits.size());

        for (auto& h : hits)
            files.push_back (h.file);

        auto briefs = ctx.library->sounds().brief (files, qvec.empty() ? nullptr : &qvec);
        auto likeFile = like.isString() ? juce::File (like.toString())
                      : like.isObject() && like.hasProperty ("file") && ! like.hasProperty ("start") && ! like.hasProperty ("end")
                          ? juce::File (like.getProperty ("file", {}).toString()) : juce::File();
        const bool wantOneShot = (bool) cmd.getProperty ("oneShot", false);
        const double minDur = (double) cmd.getProperty ("minDuration", 0.0), maxDur = (double) cmd.getProperty ("maxDuration", 1e9);
        std::vector<size_t> keep;
        int analyzed = 0;

        for (size_t i = 0; i < hits.size(); ++i)
        {
            auto& b = briefs[i];

            if (! b.analyzed)
                continue;

            ++analyzed;

            if ((category >= 0 && b.category != category)
                || (cmd.hasProperty ("oneShot") && b.features.oneShot != wantOneShot)
                || b.features.duration < minDur || b.features.duration > maxDur
                || (needVibrato && b.features.vibratoCents <= 0.0)
                || (likeFile != juce::File() && hits[i].file == likeFile))
                continue;

            keep.push_back (i);
        }

        const int matched = (int) keep.size();
        auto bySortKey = [&] (size_t a, size_t b)
        {
            double va = 0.0, vb = 0.0;
            bool d = true;
            sortValue (sortKey, briefs[a].features, va, d);
            sortValue (sortKey, briefs[b].features, vb, d);
            return descending ? va > vb : va < vb;
        };

        if (! qvec.empty())
        {
            std::stable_sort (keep.begin(), keep.end(), [&] (size_t a, size_t b) { return briefs[a].score > briefs[b].score; });

            if (sortKey.isNotEmpty())
            {
                // die aehnlichsten zuerst auswaehlen, dann nach dem Merkmal
                keep.resize (std::min (keep.size(), (size_t) std::max (limit * 3, 30)));
                std::stable_sort (keep.begin(), keep.end(), bySortKey);
            }
        }
        else if (sortKey.isNotEmpty())
            std::stable_sort (keep.begin(), keep.end(), bySortKey);

        juce::Array<juce::var> out;

        for (auto i : keep)
        {
            if (out.size() >= limit || checked >= maxChecks)
                break;

            auto& h = hits[i];
            auto& b = briefs[i];
            auto reader = openHit (h.file);

            if (reader == nullptr)
                continue;

            auto o = new juce::DynamicObject();
            o->setProperty ("file", h.file.getFullPathName());
            o->setProperty ("name", h.file.getFileNameWithoutExtension());
            o->setProperty ("folder", h.file.getParentDirectory().getRelativePathFrom (h.root).replaceCharacter ('\\', '/'));
            o->setProperty ("library", h.root.getFullPathName());
            o->setProperty ("duration", round3 (b.features.duration));
            o->setProperty ("channels", (int) reader->numChannels);
            o->setProperty ("category", b.category >= 0 ? SoundIndex::categoryNames()[b.category] : juce::String ("unknown"));

            if (! qvec.empty())
                o->setProperty ("similarity", round3 (b.score));

            if (b.at > 0.0)
                o->setProperty ("at", b.at);

            o->setProperty ("features", featuresVar (b.features));
            out.add (juce::var (o));
        }

        auto res = resultObj (true, { { "query", query }, { "total", matched - skipped }, { "samples", out },
                                      { "analyzed", analyzed }, { "notAnalyzed", (int) hits.size() - analyzed } });
        auto* r = res.getDynamicObject();

        if (english.isNotEmpty())
            r->setProperty ("text", english);

        if (category >= 0)
        {
            r->setProperty ("category", SoundIndex::categoryNames()[category]);

            if (autoCategory)
                r->setProperty ("categoryFromText", true);
        }

        if (sortKey.isNotEmpty())
        {
            r->setProperty ("sortedBy", sortKey + (descending ? " desc" : " asc"));

            if (autoSort)
                r->setProperty ("sortFromText", true);

            if (needVibrato)
                r->setProperty ("onlyWithVibrato", true);
        }

        if (reference.isObject())
            r->setProperty ("reference", reference);

        if (auto by = within != juce::File() ? ctx.library->analysisBlockedBy (within) : juce::File(); by != juce::File())
            r->setProperty ("hint", "no sound analysis in " + by.getFullPathName() + " (blocked for its licence) - search there by name (query)");
        else if (analyzed == 0)
            r->setProperty ("hint", "no analyzed samples here yet - start the analysis with sampleLibraries {analyze: true}");

        noteSkipped (*r, out.size() < limit && checked >= maxChecks && checked < (int) keep.size());

        if (ctx.library->sounds().progress().running)
            r->setProperty ("analysis", analysisVar (ctx));

        if (indexing)
            r->setProperty ("indexing", true);

        return res;
    }
}
