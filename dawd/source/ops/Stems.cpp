#include "Ops.h"
#include "AnalysisSource.h"
#include "../Origins.h"
#include "../Routing.h"
#include "../Separation.h"
#include "../Transcription.h"

#include <juce_dsp/juce_dsp.h>
#include <numeric>

// Stem-Trennung als Hintergrund-Job (Familie processing): separateStems
// startet, jobStatus liest Fortschritt und Ergebnis. Die Stems landen als
// WAV in media/stems/ des Projekts (Mix-Quelle oder insertSection), die einer
// Datei als Analysematerial unter <config>/stems/ (wiederverwendet) oder in
// dir; das Ergebnis bringt fertige addClip-Befehle fuer batch mit, wenn
// insertSection angegeben war.
namespace dawd
{
    namespace
    {
        bool writeWav (const juce::File& f, const std::array<std::vector<float>, 2>& ch, double rate)
        {
            f.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (
                new juce::FileOutputStream (f), rate, 2, 24, {}, 0));

            if (w == nullptr)
                return false;

            const float* ptrs[] = { ch[0].data(), ch[1].data() };
            return w->writeFromFloatArrays (ptrs, 2, (int) ch[0].size());
        }

        // Analysematerial einer Datei: <config>/stems/<Datei Bereich>[ 6s]/ —
        // ein zweiter Aufruf mit derselben Datei und demselben Bereich findet
        // die Stems wieder; other u. a. klingen je Modell anders, darum ein
        // eigener Ordner fuer sechs Stems
        juce::File stemCacheDir (DawdContext& ctx, const juce::File& file, const juce::var& cmd, int model)
        {
            auto tag = file.getFileNameWithoutExtension();

            if (cmd.hasProperty ("start") || cmd.hasProperty ("end"))
                tag << " " << juce::String ((double) cmd.getProperty ("start", 0.0), 3) << "-"
                    << (cmd.hasProperty ("end") ? juce::String ((double) cmd.getProperty ("end", 0.0), 3) : juce::String ("end"));

            if (model == 6)
                tag << " 6s";
            else if (model == 53)
                tag << " 53";

            return ctx.configDir.getChildFile ("stems").getChildFile (juce::File::createLegalFileName (tag));
        }

        // Name des Stems s eines Modells (4/6: die ersten vier wie stemNames)
        juce::String stemName (int model, size_t s)
        {
            return model == 53 ? juce::String (separation::classNames53[s]) : juce::String (separation::stemNames6[s]);
        }

        bool isClassName (const juce::String& n)
        {
            return std::find_if (separation::classNames53.begin(), separation::classNames53.end(),
                                 [&] (const char* c) { return n == c; }) != separation::classNames53.end();
        }

        // Mittlere Leistung eines Stems in dB (beide Kanaele)
        double stemDb (const std::array<std::vector<float>, 2>& ch)
        {
            double p = 0.0;
            size_t n = 0;

            for (auto& c : ch)
            {
                for (auto v : c)
                    p += (double) v * v;

                n += c.size();
            }

            return n > 0 && p > 0.0 ? 10.0 * std::log10 (p / (double) n) : -120.0;
        }

        // mega53 rechnet ~3,6 s je Sekunde Audio und braucht ~10 GB
        constexpr double maxSeconds53 = 60.0;
        constexpr double silentDb53 = -70.0;   // leiser: Klasse klingt nicht, keine Datei
    }

    // Stand eines Jobs fuer die Antwort (auch matchSound/tuneSound)
    juce::var jobVar (Job& j)
    {
        auto res = resultObj (true, { { "job", j.id }, { "kind", j.kind } });
        auto* o = res.getDynamicObject();
        const bool done = j.done;
        o->setProperty ("status", ! done ? "running" : (j.error.isEmpty() ? "done" : (j.cancel ? "cancelled" : "failed")));
        o->setProperty ("progress", std::round (j.progress.load() * 100.0) / 100.0);
        o->setProperty ("seconds", std::round ((juce::Time::getCurrentTime() - j.started).inSeconds()));

        if (done)
        {
            const std::lock_guard<std::mutex> sl (j.lock);

            if (j.error.isNotEmpty())
                o->setProperty ("error", j.error);

            if (auto* r = j.result.getDynamicObject())
                for (auto& p : r->getProperties())
                    o->setProperty (p.name, p.value);
        }

        return res;
    }

    // Quelle: file (+ start/end) oder der Mix (section, start/end, track) —
    // stereo bei 44,1 kHz geladen, dann auf dem Job-Thread getrennt.
    // stems = 4 oder 6 (Modell htdemucs_6s mit guitar und piano) oder eine
    // Auswahl nach Namen (Default alle vier; guitar/piano waehlen das
    // 6-Stem-Modell), insertSection/insertStart fuer die addClip-Befehle im
    // Ergebnis.
    juce::var opSeparateStems (DawdContext& ctx, const juce::var& cmd)
    {
        // Modell und Auswahl zuerst: stems 6 kam in Praxistest 1h ohne
        // Meldung als vier Stems zurueck
        juce::StringArray wanted;
        int model = 4;
        const auto stemsArg = cmd.getProperty ("stems", {});

        if (auto* list = stemsArg.getArray())
        {
            auto isStem6 = [] (const juce::String& n)
            {
                return std::find_if (separation::stemNames6.begin(), separation::stemNames6.end(),
                                     [&] (const char* c) { return n == c; }) != separation::stemNames6.end();
            };

            // Ein Name, den nur das 53er-Modell kennt (synth, strings …),
            // waehlt es — dann muessen alle Namen Klassen sein
            for (auto& s : *list)
                if (! isStem6 (s.toString()) && isClassName (s.toString()))
                    model = 53;

            for (auto& s : *list)
            {
                if (model == 53 ? ! isClassName (s.toString()) : ! isStem6 (s.toString()))
                    return resultObj (false, { { "error", model == 53
                        ? "unknown class " + s.toString() + juce::String::fromUTF8 (" — the 53-class model knows ") + juce::StringArray (separation::classNames53.data(), (int) separation::classNames53.size()).joinIntoString (", ")
                        : juce::String::fromUTF8 ("stems are drums, bass, other, vocals, guitar, piano (or classes of the 53-class model) — got ") + s.toString() } });

                wanted.addIfNotAlreadyThere (s.toString());

                if (model != 53 && (s.toString() == "guitar" || s.toString() == "piano"))
                    model = 6;
            }
        }
        else if (stemsArg.isInt() || stemsArg.isDouble() || stemsArg.isInt64())
        {
            model = (int) stemsArg;

            if ((double) stemsArg != 4.0 && (double) stemsArg != 6.0 && (double) stemsArg != 53.0)
                return resultObj (false, { { "error", "stems must be 4, 6, 53 or a list of stem names" } });
        }
        else if (! stemsArg.isVoid())
            return resultObj (false, { { "error", "stems must be 4, 6, 53 or a list of stem names" } });

        if (wanted.isEmpty() && stemsArg.getArray() == nullptr)
        {
            if (model == 53)
                for (auto* n : separation::classNames53)
                    wanted.add (n);
            else if (model == 6)
                for (auto* n : separation::stemNames6)
                    wanted.add (n);
            else
                for (auto* n : separation::stemNames)
                    wanted.add (n);
        }

        if (wanted.isEmpty())
            return resultObj (false, { { "error", "stems must not be empty" } });

        if (auto err = separation::available (model); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // Eine Datei laesst sich ohne Projekt trennen (Praxistest 1e: nach
        // treeClose meldete separateStems {file, dir} "no tree / no edit");
        // Mix, Section oder Spur brauchen Baum und Edit (Guard-Text wie die Tabelle)
        if (! cmd.hasProperty ("file") && (! ctx.tree.has_value() || ctx.edit == nullptr))
            return resultObj (false, { { "error", "no tree / no edit" } });

        // Quelle laden (auf dem Message-Thread — Render und Leser brauchen die Engine)
        std::vector<std::vector<float>> audio;
        juce::String baseName;
        double srcStart = 0.0;
        auto insertSection = cmd.getProperty ("insertSection", {}).toString();
        auto insertStart = (double) cmd.getProperty ("insertStart", 0.0);

        // Ziel: dir ausdruecklich; Stems einer Datei ohne insertSection sind
        // Analysematerial und landen ausserhalb jedes Projekts unter
        // <config>/stems/<Datei Bereich>/ (Praxistest 1d: sie lagen im
        // Nachbau-Projekt bzw. im Projekt, das beim Start noch offen war) —
        // dort findet sie ein zweiter Aufruf wieder (keine neue Trennung)
        juce::File dir;
        bool reuse = false;
        juce::StringArray stemInputs;   // Herkunft der Stems (originsOf)

        if (cmd.hasProperty ("dir"))
        {
            dir = juce::File (cmd.getProperty ("dir", {}).toString());

            if (! juce::File::isAbsolutePath (cmd.getProperty ("dir", {}).toString()))
                return resultObj (false, { { "error", "dir must be an absolute path" } });
        }

        if (cmd.hasProperty ("file"))
        {
            auto file = juce::File (cmd.getProperty ("file", {}).toString());

            if (! file.existsAsFile())
                return resultObj (false, { { "error", "no such file: " + cmd.getProperty ("file", {}).toString() } });

            if (auto e = blockedFileError (ctx, file); e.isNotEmpty())
                return resultObj (false, { { "error", e } });

            stemInputs.add (file.getFullPathName());

            if (dir == juce::File() && insertSection.isEmpty())
            {
                dir = stemCacheDir (ctx, file, cmd, model);
                reuse = true;

                // 53 Klassen: stille stehen in silent.json statt als Datei
                auto silent = juce::JSON::parse (dir.getChildFile ("silent.json"));

                for (auto& w : wanted)
                    reuse = reuse && (dir.getChildFile (w + ".wav").existsAsFile()
                                      || (silent.getArray() != nullptr && silent.getArray()->contains (w)));
            }

            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));

            if (reader == nullptr)
                return resultObj (false, { { "error", "cannot read audio file: " + file.getFileName() } });

            auto length = (double) reader->lengthInSamples / reader->sampleRate;
            srcStart = (double) cmd.getProperty ("start", 0.0);
            auto end = cmd.hasProperty ("end") ? (double) cmd.getProperty ("end", 0.0) : length;

            if (srcStart < 0.0 || end <= srcStart || end > length + 1e-3)
                return resultObj (false, { { "error", "need 0 <= start < end <= file length (" + juce::String (length, 3) + " s)" } });

            if (! reuse)
                audio = transcription::loadResampled (*reader, (juce::int64) (srcStart * reader->sampleRate),
                                                      (juce::int64) ((end - srcStart) * reader->sampleRate), separation::sampleRate, 2);
            else
                audio = { std::vector<float> ((size_t) ((end - srcStart) * separation::sampleRate)), {} };   // nur fuer die Laenge
            baseName = file.getFileNameWithoutExtension();
        }
        else
        {
            auto tmp = uniqueTempFile ("dawd-stems", ".wav");
            const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };
            double s0 = 0.0, s1 = 0.0;
            int track = -1;

            if (auto err = renderMixRange (ctx, cmd, tmp, s0, s1, track); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            if (auto e = checkMixLicence (ctx, s0, s1, track, true); e.isNotEmpty())
                return resultObj (false, { { "error", e } });

            stemInputs = soundingOrigins (*ctx.edit, s0, s1, track);

            std::unique_ptr<juce::AudioFormatReader> reader (
                ctx.engine->getAudioFileFormatManager().getWavFormat()->createReaderFor (new juce::FileInputStream (tmp), true));

            if (reader == nullptr)
                return resultObj (false, { { "error", "cannot read render" } });

            audio = transcription::loadResampled (*reader, 0, reader->lengthInSamples, separation::sampleRate, 2);
            baseName = cmd.hasProperty ("section") ? cmd.getProperty ("section", {}).toString() : juce::String ("mix");
            srcStart = s0;
        }

        if (audio.size() < 2 || audio[0].size() < 4410)
            return resultObj (false, { { "error", "source too short (need at least 0.1 s)" } });

        if (model == 53 && (double) audio[0].size() / separation::sampleRate > maxSeconds53 + 0.01)
            return resultObj (false, { { "error", "the 53-class model takes at most " + juce::String ((int) maxSeconds53)
                                                    + juce::String::fromUTF8 (" s per call (it runs ~3.6 s per second of audio) — give start/end or a section") } });

        if (dir == juce::File())
        {
            if (! ctx.tree.has_value() || ! ctx.tree->projectDir.isDirectory())
                return resultObj (false, { { "error", juce::String::fromUTF8 ("no project open — give dir, or a file source (stems go to the config folder)") } });

            dir = ctx.tree->projectDir.getChildFile ("media").getChildFile ("stems")
                      .getNonexistentChildFile (juce::File::createLegalFileName (baseName), {}, false);
        }

        // Stems sind aus der Quelle gemacht: Herkunft notieren (gilt, wenn
        // ein Ordner spaeter gesperrt wird)
        for (auto& w : wanted)
            recordOrigins (dir.getChildFile (w + ".wav"), stemInputs);

        // addClip-Befehle fuer das Ergebnis (nur mit insertSection)
        const double seconds = (double) audio[0].size() / separation::sampleRate;

        auto job = ctx.jobs->create ("separateStems");
        auto left = std::make_shared<std::vector<float>> (std::move (audio[0]));
        auto right = std::make_shared<std::vector<float>> (std::move (audio[1]));

        std::function<juce::ThreadPoolJob::JobStatus()> work ([job, left, right, dir, wanted, insertSection, insertStart, reuse, model]
        {
            separation::Stems out;
            juce::String err;
            std::vector<bool> keep ((size_t) model);

            for (size_t s = 0; s < keep.size(); ++s)
                keep[s] = wanted.contains (stemName (model, s));

            auto ok = reuse || separation::separate (*left, *right, model, out, [job] (double p) { job->progress = 0.95 * p; return ! job->cancel.load(); }, err, keep);
            auto result = new juce::DynamicObject();
            juce::Array<juce::var> stems, commands, silent;

            // Das geparste var muss leben, solange known benutzt wird (als
            // Temporaeres in der if-Bedingung zeigte known auf Freigegebenes)
            const auto knownSilent = reuse && model == 53 ? juce::JSON::parse (dir.getChildFile ("silent.json")) : juce::var();

            if (auto* known = knownSilent.getArray())
                for (auto& w : wanted)
                    if (known->contains (w))
                        silent.add (w);

            if (ok)
            {
                dir.createDirectory();

                for (size_t s = 0; s < (size_t) model; ++s)
                {
                    auto name = stemName (model, s);

                    if (! wanted.contains (name) || silent.contains (name))
                        continue;

                    // 53 Klassen: eine, die nicht klingt, bekommt keine Datei
                    if (! reuse && model == 53 && stemDb (out[s]) < silentDb53)
                    {
                        silent.add (name);
                        continue;
                    }

                    auto f = dir.getChildFile (name + ".wav");

                    if (! reuse && ! writeWav (f, out[s], separation::sampleRate))
                    {
                        err = "cannot write " + f.getFullPathName();
                        break;
                    }

                    auto so = new juce::DynamicObject();
                    so->setProperty ("stem", name);
                    so->setProperty ("file", f.getFullPathName());
                    stems.add (juce::var (so));

                    if (insertSection.isNotEmpty())
                    {
                        auto co = new juce::DynamicObject();
                        co->setProperty ("op", "addClip");
                        co->setProperty ("track", -1);
                        co->setProperty ("file", f.getFullPathName());
                        co->setProperty ("section", insertSection);
                        co->setProperty ("start", insertStart);
                        commands.add (juce::var (co));
                    }
                }
            }

            if (ok && ! reuse && model == 53)
                dir.getChildFile ("silent.json").replaceWithText (juce::JSON::toString (juce::var (silent), true));

            result->setProperty ("dir", dir.getFullPathName());
            result->setProperty ("model", model == 53 ? "mega53" : model == 6 ? "htdemucs_6s" : "htdemucs");
            result->setProperty ("stems", stems);

            if (model == 53)
                result->setProperty ("silent", silent);   // Klassen ohne Klang (keine Datei)

            if (reuse)
                result->setProperty ("cached", true);   // schon getrennt, nichts neu gerechnet

            if (insertSection.isNotEmpty())
                result->setProperty ("addClipCommands", commands);

            {
                const std::lock_guard<std::mutex> sl (job->lock);
                job->error = err;
                job->result = juce::var (result);
            }

            job->progress = 1.0;
            job->done = true;
            return juce::ThreadPoolJob::jobHasFinished;
        });

        // Treffer im Cache: nichts zu rechnen, gleich fertig. Im Pool (ein
        // Thread) wartete er sonst hinter einer laufenden Trennung (Todo
        // 2026-11-09-stems-flow-cache-wartet-unter-last)
        if (reuse)
            work();
        else
            ctx.jobs->pool.addJob (std::move (work));

        auto res = jobVar (*job);
        res.getDynamicObject()->setProperty ("seconds", std::round (seconds * 10.0) / 10.0);
        res.getDynamicObject()->setProperty ("sourceStart", srcStart);
        return res;
    }

    namespace
    {
        using Stereo = std::array<std::vector<float>, 2>;

        // Pegel, Schwerpunkt und Breite einer Klasse; Pegel relativ zum Mix
        struct ClassStats { double db = -120.0, centroidHz = 0.0, correlation = 0.0; };

        ClassStats statsOf (const Stereo& x, double mixPower)
        {
            ClassStats st;
            const size_t n = juce::jmin (x[0].size(), x[1].size());

            if (n == 0)
                return st;

            double ll = 0.0, rr = 0.0, lr = 0.0;

            for (size_t i = 0; i < n; ++i)
                ll += (double) x[0][i] * x[0][i], rr += (double) x[1][i] * x[1][i], lr += (double) x[0][i] * x[1][i];

            const double p = (ll + rr) / (2.0 * (double) n);
            st.db = p > 0.0 && mixPower > 0.0 ? 10.0 * std::log10 (p / mixPower) : -120.0;
            st.correlation = ll > 1.0e-12 && rr > 1.0e-12 ? lr / std::sqrt (ll * rr) : 0.0;

            // Schwerpunkt der Leistung (Mitte L+R, Hann, 4096er-Bloecke)
            constexpr int order = 12, size = 1 << order;
            juce::dsp::FFT fft (order);
            juce::dsp::WindowingFunction<float> win ((size_t) size, juce::dsp::WindowingFunction<float>::hann);
            std::vector<float> buf (2 * size);
            double num = 0.0, den = 0.0;

            for (size_t pos = 0; pos + size <= n; pos += size)
            {
                for (int i = 0; i < size; ++i)
                    buf[(size_t) i] = 0.5f * (x[0][pos + (size_t) i] + x[1][pos + (size_t) i]);

                std::fill (buf.begin() + size, buf.end(), 0.0f);
                win.multiplyWithWindowingTable (buf.data(), (size_t) size);
                fft.performFrequencyOnlyForwardTransform (buf.data());

                for (int b = 1; b <= size / 2; ++b)
                {
                    const double pw = (double) buf[(size_t) b] * buf[(size_t) b];
                    num += pw * b * separation::sampleRate / size;
                    den += pw;
                }
            }

            st.centroidHz = den > 0.0 ? num / den : 0.0;
            return st;
        }

        double meanPower (const Stereo& x)
        {
            double p = 0.0;

            for (auto& c : x)
                for (auto v : c)
                    p += (double) v * v;

            return p / (double) juce::jmax ((size_t) 1, x[0].size() + x[1].size());
        }

        double dot (const Stereo& a, const Stereo& b)
        {
            double s = 0.0;

            for (size_t c = 0; c < 2; ++c)
                for (size_t i = 0, n = juce::jmin (a[c].size(), b[c].size()); i < n; ++i)
                    s += (double) a[c][i] * b[c][i];

            return s;
        }

        // Leistungsspektrogramm (L und R addiert, Hann 2048, Schritt 1024)
        // als frames x bins — phasenunabhaengig: zwei Renders mit
        // freilaufenden Oszillatoren (Surge, 4OSC) haben dieselbe Energie
        // je Zelle, aber kein Skalarprodukt
        std::vector<float> powerSpec (const Stereo& x)
        {
            constexpr int order = 11, size = 1 << order, hop = size / 2, bins = size / 2 + 1;
            const size_t n = juce::jmin (x[0].size(), x[1].size());
            std::vector<float> out;

            if (n < (size_t) size)
                return out;

            juce::dsp::FFT fft (order);
            juce::dsp::WindowingFunction<float> win ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
            std::vector<float> buf (2 * size);

            for (size_t pos = 0; pos + size <= n; pos += hop)
            {
                const size_t row = out.size();
                out.resize (row + bins, 0.0f);

                for (size_t c = 0; c < 2; ++c)
                {
                    std::copy (x[c].begin() + (std::ptrdiff_t) pos, x[c].begin() + (std::ptrdiff_t) (pos + size), buf.begin());
                    std::fill (buf.begin() + size, buf.end(), 0.0f);
                    win.multiplyWithWindowingTable (buf.data(), (size_t) size);
                    fft.performFrequencyOnlyForwardTransform (buf.data());

                    for (int b = 0; b < bins; ++b)
                        out[row + (size_t) b] += buf[(size_t) b] * buf[(size_t) b];
                }
            }

            return out;
        }

        // Klassen, die in einer anderen enthalten sind (drums, vocal) —
        // ohne eigene Hinweise, sonst meldete jede fehlende Snare doppelt
        bool childClass (const juce::String& c)
        {
            return c == "kick" || c == "snare" || c == "hh" || c == "toms" || c == "lead-vocal" || c == "back-vocal";
        }

        juce::String dbText (double v) { return v <= silentDb53 ? juce::String ("none") : juce::String (v, 1) + " dB"; }

        Stereo readStereo (const juce::File& f)
        {
            Stereo out;
            std::unique_ptr<juce::AudioFormatReader> r (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (f), true));

            if (r == nullptr)
                return out;

            juce::AudioBuffer<float> buf ((int) juce::jmax (2u, r->numChannels), (int) r->lengthInSamples);
            r->read (&buf, 0, (int) r->lengthInSamples, 0, true, true);

            for (int c = 0; c < 2; ++c)
                out[(size_t) c].assign (buf.getReadPointer (c), buf.getReadPointer (c) + buf.getNumSamples());

            return out;
        }
    }

    // compareClasses: Instrumentenklassen (mega53) des eigenen Mix und der
    // Referenz als Hintergrund-Job — je Klasse Pegel relativ zum jeweiligen
    // Mix, Schwerpunkt und Breite; je eigener Spur, in welchen Klassen sie
    // landet (ihr Render auf die Klassen des eigenen Mix projiziert, ohne
    // weiteren Modelllauf). Praxistest 1m: Pad, Offbeat-Synth und Gitarre
    // landeten alle in synth, das Original hatte guitar/strings — kein
    // Vergleich je Spur gegen ihren Stem zeigte das. Die Trennung der
    // Referenz liegt im Cache wie bei separateStems {stems: 53}.
    juce::var opCompareClasses (DawdContext& ctx, const juce::var& cmd)
    {
        // ignore: Klassen, die der Nachbau bewusst weglaesst (vocal, wenn der
        // Auftrag ohne Gesang baut) — in der Tabelle, ohne Hinweis
        juce::StringArray ignore;

        if (auto* list = cmd.getProperty ("ignore", {}).getArray())
            for (auto& v : *list)
            {
                if (! isClassName (v.toString()))
                    return resultObj (false, { { "error", "unknown class " + v.toString() + juce::String::fromUTF8 (" in ignore — the 53-class model knows ")
                                                          + juce::StringArray (separation::classNames53.data(), (int) separation::classNames53.size()).joinIntoString (", ") } });

                ignore.add (v.toString());
            }
        else if (! cmd.getProperty ("ignore", {}).isVoid())
            return resultObj (false, { { "error", "ignore must be a list of class names" } });

        auto refFile = juce::File (cmd.getProperty ("file", {}).toString());

        if (cmd.getProperty ("file", {}).toString().isEmpty() || ! refFile.existsAsFile())
            return resultObj (false, { { "error", "no such file: " + cmd.getProperty ("file", {}).toString() } });

        if (auto err = blockedFileError (ctx, refFile); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (auto err = separation::available (53); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // Eigene Seite: der Mix-Bereich (section, start/end) wie analyze
        auto mixCmd = cmd.clone();
        mixCmd.getDynamicObject()->removeProperty ("track");
        auto tmp = uniqueTempFile ("dawd-classes", ".wav");
        const juce::ScopeGuard removeTmp { [&] { tmp.deleteFile(); } };
        double s0 = 0.0, s1 = 0.0;
        int unused = -1;

        if (auto err = renderMixRange (ctx, mixCmd, tmp, s0, s1, unused); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (auto err = checkMixLicence (ctx, s0, s1, -1, true); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto loadFile = [&ctx] (const juce::File& f, double from, double to, Stereo& out) -> juce::String
        {
            std::unique_ptr<juce::AudioFormatReader> reader (ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (f));

            if (reader == nullptr)
                return "cannot read audio file: " + f.getFileName();

            const auto length = (double) reader->lengthInSamples / reader->sampleRate;

            if (to < 0.0)
                to = length;

            if (from < 0.0 || to <= from || from >= length)
                return "need 0 <= referenceStart < referenceEnd within the file (" + juce::String (length, 3) + " s)";

            to = juce::jmin (to, length);
            auto a = transcription::loadResampled (*reader, (juce::int64) (from * reader->sampleRate),
                                                   (juce::int64) ((to - from) * reader->sampleRate), separation::sampleRate, 2);

            if (a.size() < 2)
                return "cannot read audio file: " + f.getFileName();

            out = { std::move (a[0]), std::move (a[1]) };
            return {};
        };

        auto mine = std::make_shared<Stereo>();

        if (auto err = loadFile (tmp, 0.0, -1.0, *mine); err.isNotEmpty())
            return resultObj (false, { { "error", "cannot read render" } });

        const double seconds = (double) (*mine)[0].size() / separation::sampleRate;

        if (seconds > maxSeconds53 + 0.01)
            return resultObj (false, { { "error", "the 53-class model takes at most " + juce::String ((int) maxSeconds53)
                                                    + juce::String::fromUTF8 (" s per side (it runs ~3.6 s per second of audio) — give a section or start/end") } });

        if (meanPower (*mine) <= 1.0e-12)
            return resultObj (false, { { "error", "the project is silent in " + juce::String (s0, 2) + "-" + juce::String (s1, 2) + " s - nothing to compare" } });

        // Referenz: ab referenceStart so lang wie die eigene Seite (oder bis referenceEnd)
        const double refStart = (double) cmd.getProperty ("referenceStart", 0.0);
        const double refEnd = cmd.hasProperty ("referenceEnd") ? (double) cmd.getProperty ("referenceEnd", 0.0) : refStart + seconds;
        auto ref = std::make_shared<Stereo>();

        if (refEnd - refStart > maxSeconds53 + 0.01)
            return resultObj (false, { { "error", "the 53-class model takes at most " + juce::String ((int) maxSeconds53) + juce::String::fromUTF8 (" s per side — shorten referenceStart/referenceEnd") } });

        if (auto err = loadFile (refFile, refStart, refEnd, *ref); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto refRange = juce::var (new juce::DynamicObject());
        refRange.getDynamicObject()->setProperty ("start", refStart);
        refRange.getDynamicObject()->setProperty ("end", refStart + (double) (*ref)[0].size() / separation::sampleRate);
        const auto refDir = stemCacheDir (ctx, refFile, refRange, 53);

        // Eigene Spuren einzeln (ohne Master-Kette), fuer die Zuordnung
        struct TrackAudio { int index; juce::String name; Stereo audio; };
        auto tracks = std::make_shared<std::vector<TrackAudio>>();

        if ((bool) cmd.getProperty ("tracks", true))
        {
            auto users = userTracks (*ctx.edit);

            for (int i = 0; i < users.size(); ++i)
            {
                if (laneGroupOf (*users[i]) != nullptr)
                    continue;   // Lane: gehoert zur Spur davor

                auto tc = mixCmd.clone();
                tc.getDynamicObject()->setProperty ("track", i);
                auto tt = uniqueTempFile ("dawd-classes-track", ".wav");
                const juce::ScopeGuard removeTt { [&] { tt.deleteFile(); } };
                double a = 0.0, b = 0.0;
                int ti = -1;

                if (renderMixRange (ctx, tc, tt, a, b, ti).isNotEmpty())
                    continue;   // keine Clips im Bereich

                TrackAudio ta { i, users[i]->getName(), {} };

                if (loadFile (tt, 0.0, -1.0, ta.audio).isEmpty() && meanPower (ta.audio) > 1.0e-10)
                    tracks->push_back (std::move (ta));
            }
        }

        auto job = ctx.jobs->create ("compareClasses");

        ctx.jobs->pool.addJob (std::function<juce::ThreadPoolJob::JobStatus()> ([job, mine, ref, refDir, tracks, s0, s1, refStart, ignore]
        {
            juce::String err;
            separation::Stems refOut, mineOut;
            constexpr size_t count = separation::classNames53.size();

            // Referenz aus dem Cache, wenn alle Klassen da sind (Datei oder silent.json)
            bool cached = refDir.isDirectory();
            auto silentList = juce::JSON::parse (refDir.getChildFile ("silent.json"));

            for (auto* c : separation::classNames53)
                cached = cached && (refDir.getChildFile (juce::String (c) + ".wav").existsAsFile()
                                    || (silentList.getArray() != nullptr && silentList.getArray()->contains (juce::String (c))));

            if (cached)
            {
                refOut.assign (count, {});

                for (size_t k = 0; k < count; ++k)
                    if (auto f = refDir.getChildFile (juce::String (separation::classNames53[k]) + ".wav"); f.existsAsFile())
                        refOut[k] = readStereo (f);
            }

            bool ok = cached || separation::separate ((*ref)[0], (*ref)[1], 53, refOut,
                                                      [job] (double p) { job->progress = 0.48 * p; return ! job->cancel.load(); }, err);

            if (ok && ! cached)
            {
                // Fuer den naechsten Vergleich gegen dieselbe Referenz
                refDir.createDirectory();
                juce::Array<juce::var> silent;

                for (size_t k = 0; k < count; ++k)
                    if (stemDb (refOut[k]) < silentDb53)
                        silent.add (juce::String (separation::classNames53[k]));
                    else
                        writeWav (refDir.getChildFile (juce::String (separation::classNames53[k]) + ".wav"), refOut[k], separation::sampleRate);

                refDir.getChildFile ("silent.json").replaceWithText (juce::JSON::toString (juce::var (silent), true));
            }

            ok = ok && separation::separate ((*mine)[0], (*mine)[1], 53, mineOut,
                                             [job] (double p) { job->progress = 0.5 + 0.48 * p; return ! job->cancel.load(); }, err);

            auto result = new juce::DynamicObject();

            if (ok)
            {
                const double refP = meanPower (*ref), mineP = meanPower (*mine);
                std::vector<ClassStats> rs (count), ys (count);
                juce::Array<juce::var> classes, trackList;
                juce::StringArray hints;

                for (size_t k = 0; k < count; ++k)
                {
                    rs[k] = statsOf (refOut[k], refP);
                    ys[k] = statsOf (mineOut[k], mineP);
                }

                // Zuordnung: je Zeit-Frequenz-Zelle der Anteil der Klasse k
                // an der Mix-Energie, gewichtet mit der Energie der Spur dort
                // — unabhaengig von Fader, Master-Kette und Phase (ein
                // Skalarprodukt Spur/Klasse traf bei freilaufenden
                // Oszillatoren mal 96 %, mal nichts); die Klassen
                // ueberlappen, die Anteile summieren sich nicht zu 1
                std::vector<std::vector<std::pair<size_t, double>>> shares (tracks->size());
                const auto mixSpec = powerSpec (*mine);
                std::vector<std::vector<float>> classSpec (count);

                if (! tracks->empty())
                    for (size_t k = 0; k < count; ++k)
                        if (ys[k].db > -40.0)
                            classSpec[k] = powerSpec (mineOut[k]);

                for (size_t t = 0; t < tracks->size(); ++t)
                {
                    const auto& ta = (*tracks)[t];
                    const auto spec = powerSpec (ta.audio);
                    const size_t cells = juce::jmin (spec.size(), mixSpec.size());
                    const double total = std::accumulate (spec.begin(), spec.begin() + (std::ptrdiff_t) cells, 0.0);
                    auto to = new juce::DynamicObject();
                    juce::Array<juce::var> cls;

                    if (total > 0.0)
                    {
                        for (size_t k = 0; k < count; ++k)
                        {
                            if (classSpec[k].size() < cells)
                                continue;

                            double in = 0.0;

                            for (size_t i = 0; i < cells; ++i)
                                in += (double) spec[i] * juce::jmin (1.0, (double) classSpec[k][i] / ((double) mixSpec[i] + 1.0e-9));

                            if (auto sh = in / total; sh >= 0.15)
                                shares[t].push_back ({ k, sh });
                        }

                        std::sort (shares[t].begin(), shares[t].end(), [] (auto& a, auto& b) { return a.second > b.second; });
                    }

                    for (size_t j = 0; j < shares[t].size() && j < 4; ++j)
                    {
                        auto co = new juce::DynamicObject();
                        co->setProperty ("class", separation::classNames53[shares[t][j].first]);
                        co->setProperty ("share", std::round (juce::jmin (1.0, shares[t][j].second) * 100.0) / 100.0);
                        cls.add (juce::var (co));
                    }

                    to->setProperty ("track", ta.index);
                    to->setProperty ("name", ta.name);
                    to->setProperty ("classes", cls);
                    trackList.add (juce::var (to));
                }

                // Spuren, die in Klasse k landen (Anteil >= 0,3), als Text
                auto tracksIn = [&] (size_t k)
                {
                    juce::StringArray out;

                    for (size_t t = 0; t < tracks->size(); ++t)
                        for (auto& [c, sh] : shares[t])
                            if (c == k && sh >= 0.3)
                                out.add ((*tracks)[t].name + " " + juce::String (juce::roundToInt (juce::jmin (1.0, sh) * 100.0)) + " %");

                    return out.joinIntoString (", ");
                };

                // Die staerksten Klassen der Referenz, die im eigenen Mix fehlen
                juce::StringArray refOnly, mineOnly;
                std::vector<size_t> order (count);
                std::iota (order.begin(), order.end(), (size_t) 0);
                std::sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return juce::jmax (rs[a].db, ys[a].db) > juce::jmax (rs[b].db, ys[b].db); });

                // Klassen mit demselben Signal (Korrelation >= 0,9 auf einer
                // Seite) sind eine Gruppe: ein Hinweis unter dem Namen
                // "guitar/electric-guitar/strings" statt drei gleicher
                // (Praxistest 1m: die drei trugen im Refrain dasselbe)
                std::vector<size_t> rep (count);
                std::iota (rep.begin(), rep.end(), (size_t) 0);
                std::function<size_t (size_t)> root = [&] (size_t k) { return rep[k] == k ? k : rep[k] = root (rep[k]); };

                for (auto [out, st] : { std::pair { &refOut, &rs }, std::pair { &mineOut, &ys } })
                    for (size_t a = 0; a < count; ++a)
                        for (size_t b = a + 1; b < count && (*st)[a].db > -40.0; ++b)
                            if ((*st)[b].db > -40.0 && ! childClass (separation::classNames53[a]) && ! childClass (separation::classNames53[b]))
                                if (dot ((*out)[a], (*out)[b]) >= 0.9 * std::sqrt (dot ((*out)[a], (*out)[a]) * dot ((*out)[b], (*out)[b])))
                                    rep[root (b)] = root (a);

                // Name einer Gruppe in der Reihenfolge von order (lauteste zuerst)
                auto groupName = [&] (size_t k)
                {
                    juce::StringArray members;

                    for (auto j : order)
                        if (root (j) == root (k))
                            members.add (separation::classNames53[j]);

                    return members.joinIntoString ("/");
                };

                std::vector<bool> hinted (count, false);

                for (auto k : order)
                {
                    const auto rdb = juce::jmax (rs[k].db, silentDb53), ydb = juce::jmax (ys[k].db, silentDb53);

                    if (juce::jmax (rdb, ydb) < -40.0)
                        continue;

                    const auto group = groupName (k);
                    auto co = new juce::DynamicObject();
                    co->setProperty ("class", juce::String (separation::classNames53[k]));

                    if (group.contains ("/"))
                        co->setProperty ("group", group);
                    co->setProperty ("referenceDb", rdb <= silentDb53 ? juce::var() : juce::var (std::round (rdb * 10.0) / 10.0));
                    co->setProperty ("yoursDb", ydb <= silentDb53 ? juce::var() : juce::var (std::round (ydb * 10.0) / 10.0));
                    co->setProperty ("deltaDb", std::round ((ydb - rdb) * 10.0) / 10.0);

                    if (rdb > -40.0)
                    {
                        co->setProperty ("referenceCentroidHz", juce::roundToInt (rs[k].centroidHz));
                        co->setProperty ("referenceCorrelation", std::round (rs[k].correlation * 100.0) / 100.0);
                    }

                    if (ydb > -40.0)
                    {
                        co->setProperty ("yoursCentroidHz", juce::roundToInt (ys[k].centroidHz));
                        co->setProperty ("yoursCorrelation", std::round (ys[k].correlation * 100.0) / 100.0);
                    }

                    if (auto in = tracksIn (k); in.isNotEmpty())
                        co->setProperty ("yourTracks", in);

                    classes.add (juce::var (co));

                    // Ein Hinweis je Gruppe (von der lautesten Klasse); keiner
                    // fuer Teilklassen und bewusst weggelassene Klassen
                    if (childClass (separation::classNames53[k]) || ignore.contains (separation::classNames53[k]) || hinted[root (k)])
                        continue;

                    hinted[root (k)] = true;
                    const auto& name = group;

                    if (rdb >= -30.0 && ydb < rdb - 15.0)
                    {
                        hints.add (name + " is in the reference (" + dbText (rdb) + ") but nearly not in your mix (" + dbText (ydb) + ")");
                        refOnly.add (name + " " + dbText (rdb));
                    }
                    else if (ydb >= -30.0 && rdb < ydb - 15.0)
                    {
                        auto in = tracksIn (k);
                        hints.add (name + " is in your mix (" + dbText (ydb) + (in.isNotEmpty() ? ": " + in : juce::String())
                                   + ") but nearly not in the reference (" + dbText (rdb) + ")");
                        mineOnly.add (name + (in.isNotEmpty() ? " (" + in + ")" : juce::String()));
                    }
                    else if (rdb >= -30.0 && ydb >= -30.0 && std::abs (ydb - rdb) >= 6.0)
                    {
                        auto in = tracksIn (k);
                        hints.add (name + " is " + juce::String (std::abs (ydb - rdb), 1) + " dB " + (ydb > rdb ? "louder" : "quieter")
                                   + " in your mix (" + dbText (ydb) + (in.isNotEmpty() ? ": " + in : juce::String()) + " vs " + dbText (rdb) + ")");

                        // Praxistest 1m: Pad, Offbeat-Synth und Bass alle in
                        // synth, 12,6 dB ueber der Referenz — das gehoert in
                        // die Zusammenfassung, nicht nur Klassen, die fehlen
                        if (ydb > rdb)
                            mineOnly.add (name + " +" + juce::String (ydb - rdb, 1) + " dB" + (in.isNotEmpty() ? " (" + in + ")" : juce::String()));
                    }
                    else if (rdb >= -30.0 && ydb >= -30.0 && rs[k].centroidHz > 0.0 && ys[k].centroidHz > 0.0
                             && std::abs (std::log2 (ys[k].centroidHz / rs[k].centroidHz)) >= 0.6)
                        hints.add (name + " is " + (ys[k].centroidHz > rs[k].centroidHz ? "brighter" : "darker") + " in your mix (centroid "
                                   + juce::String (juce::roundToInt (ys[k].centroidHz)) + " vs " + juce::String (juce::roundToInt (rs[k].centroidHz)) + " Hz)");
                }

                // Zusammenfassung: eigene Spuren in einer Klasse, die der
                // Referenz fehlt, und was die Referenz stattdessen hat
                if (! mineOnly.isEmpty() && ! refOnly.isEmpty())
                    hints.insert (0, "your " + mineOnly.joinIntoString (", ") + " part(s) may be the wrong instrument: the reference has "
                                         + refOnly.joinIntoString (", ") + " instead");

                juce::Array<juce::var> hintList;

                for (auto& h : hints)
                    hintList.add (h);

                result->setProperty ("model", "mega53");
                result->setProperty ("start", s0);
                result->setProperty ("end", s1);
                result->setProperty ("referenceStart", refStart);
                result->setProperty ("referenceCached", cached);
                result->setProperty ("referenceDir", refDir.getFullPathName());
                result->setProperty ("classes", classes);
                result->setProperty ("tracks", trackList);
                result->setProperty ("hints", hintList);
            }

            {
                const std::lock_guard<std::mutex> sl (job->lock);
                job->error = err;
                job->result = juce::var (result);
            }

            job->progress = 1.0;
            job->done = true;
            return juce::ThreadPoolJob::jobHasFinished;
        }));

        auto res = jobVar (*job);
        res.getDynamicObject()->setProperty ("seconds", std::round (seconds * 10.0) / 10.0);
        return res;
    }

    juce::String fileStemsNow (DawdContext& ctx, const juce::var& cmd, int model, juce::File& dir, bool& cached)
    {
        if (auto err = separation::available (model); err.isNotEmpty())
            return err;

        auto file = juce::File (cmd.getProperty ("file", {}).toString());

        if (! file.existsAsFile())
            return "no such file: " + cmd.getProperty ("file", {}).toString();

        dir = stemCacheDir (ctx, file, cmd, model);
        cached = true;

        for (int s = 0; s < model; ++s)
            cached = cached && dir.getChildFile (juce::String (separation::stemNames6[(size_t) s]) + ".wav").existsAsFile();

        if (cached)
            return {};

        std::unique_ptr<juce::AudioFormatReader> reader (
            ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));

        if (reader == nullptr)
            return "cannot read audio file: " + file.getFileName();

        const auto length = (double) reader->lengthInSamples / reader->sampleRate;
        const auto start = (double) cmd.getProperty ("start", 0.0);
        const auto end = cmd.hasProperty ("end") ? (double) cmd.getProperty ("end", 0.0) : length;

        if (start < 0.0 || end <= start || end > length + 1e-3)
            return "need 0 <= start < end <= file length (" + juce::String (length, 3) + " s)";

        auto audio = transcription::loadResampled (*reader, (juce::int64) (start * reader->sampleRate),
                                                   (juce::int64) ((end - start) * reader->sampleRate), separation::sampleRate, 2);

        if (audio.size() < 2 || audio[0].size() < 4410)
            return "source too short (need at least 0.1 s)";

        separation::Stems out;
        juce::String err;

        if (! separation::separate (audio[0], audio[1], model, out, [] (double) { return true; }, err))
            return err;

        dir.createDirectory();

        for (size_t s = 0; s < out.size(); ++s)
        {
            auto f = dir.getChildFile (juce::String (separation::stemNames6[s]) + ".wav");

            if (! writeWav (f, out[s], separation::sampleRate))
                return "cannot write " + f.getFullPathName();
        }

        return {};
    }

    // Stand eines Jobs; wait (Sekunden, bis 60) wartet auf das Ende, cancel
    // bricht ab. Ohne job: alle Jobs.
    juce::var opJobStatus (DawdContext& ctx, const juce::var& cmd)
    {
        auto id = cmd.getProperty ("job", {}).toString();

        // Ohne job: die Liste ohne Ergebnisse (Praxistest remix-1: alle
        // Ergebnisse waren sehr lang) — das Ergebnis mit jobStatus {job}
        if (id.isEmpty())
        {
            juce::Array<juce::var> all;

            for (auto& [jid, j] : ctx.jobs->jobs)
            {
                auto full = jobVar (*j);
                auto o = new juce::DynamicObject();

                for (auto key : { "job", "kind", "status", "progress", "seconds", "error" })
                    if (full.hasProperty (key))
                        o->setProperty (key, full.getProperty (key, {}));

                all.add (juce::var (o));
            }

            return resultObj (true, { { "jobs", all }, { "note", "results only with jobStatus {job}" } });
        }

        auto it = ctx.jobs->jobs.find (id);

        if (it == ctx.jobs->jobs.end())
            return resultObj (false, { { "error", "no such job: " + id } });

        auto& j = *it->second;

        if ((bool) cmd.getProperty ("cancel", false))
            j.cancel = true;

        // Ein Job auf dem Message-Thread kaeme hier nicht weiter: wait gilt
        // fuer ihn nur ueber HTTP (main.cpp wartet auf dem Thread der
        // Verbindung), in batch antwortet jobStatus sofort
        auto waitMs = j.onMessageThread ? 0 : (int) (juce::jlimit (0.0, 60.0, (double) cmd.getProperty ("wait", 0.0)) * 1000.0);

        for (auto end = juce::Time::getMillisecondCounter() + (juce::uint32) waitMs;
             ! j.done && juce::Time::getMillisecondCounter() < end;)
            juce::Thread::sleep (50);

        return jobVar (j);
    }
}
