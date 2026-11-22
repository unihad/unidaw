#include "Ops.h"
#include "AnalysisSource.h"
#include "../Capture.h"

// Referenzen aus dem Systemaudio (Familie media, kein Edit noetig): capture
// schneidet mit, was auf dem Rechner laeuft, detectSongBoundaries findet die
// Pausen zwischen den Songs, saveCaptureSongs legt die Songs als WAV unter
// <config>/references/ ab — von dort nimmt sie das Flow-UI als Referenz des
// Projekts (compareReference, detectTempo, ... mit file). Nichts davon
// beruehrt Edit oder Song-Baum.
namespace dawd
{
    namespace
    {
        double round2 (double v) { return std::round (v * 100.0) / 100.0; }
        double round3 (double v) { return std::round (v * 1000.0) / 1000.0; }

        juce::File capturesDir (DawdContext& ctx) { return ctx.configDir.getChildFile ("captures"); }
        juce::File referencesDir (DawdContext& ctx) { return ctx.configDir.getChildFile ("references"); }

        std::unique_ptr<juce::AudioFormatReader> openReader (DawdContext& ctx, const juce::File& file)
        {
            return std::unique_ptr<juce::AudioFormatReader> (
                ctx.engine->getAudioFileFormatManager().readFormatManager.createReaderFor (file));
        }

        double lengthOf (DawdContext& ctx, const juce::File& file)
        {
            auto r = openReader (ctx, file);
            return r != nullptr && r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
        }

        // Fertige Mitschnitte, neueste zuerst (der laufende fehlt)
        juce::var listCaptures (DawdContext& ctx)
        {
            auto files = capturesDir (ctx).findChildFiles (juce::File::findFiles, false, "*.wav");
            std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                       { return a.getLastModificationTime() > b.getLastModificationTime(); });
            juce::Array<juce::var> out;

            for (auto& f : files)
            {
                if (ctx.capture->running() && f == ctx.capture->file())
                    continue;

                auto* o = new juce::DynamicObject();
                o->setProperty ("file", f.getFullPathName());
                o->setProperty ("name", f.getFileNameWithoutExtension());
                o->setProperty ("seconds", round2 (lengthOf (ctx, f)));
                out.add (juce::var (o));
            }

            return out;
        }

        juce::String validFile (const juce::var& cmd, juce::File& file)
        {
            auto path = cmd.getProperty ("file", {}).toString();
            file = juce::File (path);

            if (path.isEmpty() || ! juce::File::isAbsolutePath (path) || ! file.existsAsFile())
                return "no such file: " + path;

            return {};
        }
    }

    // on: true startet, false stoppt, ohne on wird umgeschaltet. target =
    // Sink-Name (Standard-Ausgang, wenn leer), rate (48000), name (Datei-
    // name ohne .wav, sonst Datum und Uhrzeit).
    juce::var opCapture (DawdContext& ctx, const juce::var& cmd)
    {
        auto onVar = cmd.getProperty ("on", {});
        auto on = onVar.isVoid() ? ! ctx.capture->running() : (bool) onVar;

        if (! on)
        {
            if (! ctx.capture->running())
                return resultObj (false, { { "error", "no capture" } });

            auto file = ctx.capture->file();
            auto reconnects = ctx.capture->reconnects();

            if (auto err = ctx.capture->stop(); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

            return resultObj (true, { { "capturing", false }, { "file", file.getFullPathName() },
                                      { "seconds", round2 (lengthOf (ctx, file)) }, { "reconnects", reconnects } });
        }

        if (ctx.capture->running())
            return resultObj (false, { { "error", "capture in progress" } });

        auto rate = (int) cmd.getProperty ("rate", 48000);

        if (rate < 8000 || rate > 192000)
            return resultObj (false, { { "error", "rate must be 8000 .. 192000" } });

        auto name = cmd.getProperty ("name", {}).toString().trim();

        if (name.isEmpty())
            name = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");

        auto file = capturesDir (ctx).getChildFile (juce::File::createLegalFileName (name) + ".wav");

        if (file.exists())
            return resultObj (false, { { "error", "capture exists: " + file.getFileName() } });

        if (auto err = ctx.capture->start (file, cmd.getProperty ("target", {}).toString(), rate); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        // pw-record ohne PipeWire endet sofort — dann gleich melden
        juce::Thread::sleep (150);

        if (auto failed = ctx.capture->failure(); failed.isNotEmpty())
        {
            ctx.capture->stop();
            file.deleteFile();
            return resultObj (false, { { "error", failed } });
        }

        return resultObj (true, { { "capturing", true }, { "file", file.getFullPathName() } });
    }

    // Laeuft ein Mitschnitt (Sekunden, Pegel der letzten 0,1 s, wie oft er
    // sich nach einem PipeWire-Neustart neu verbunden hat, ob er gerade
    // darauf wartet), und die fertigen Mitschnitte
    juce::var opCaptureStatus (DawdContext& ctx, const juce::var&)
    {
        auto* o = new juce::DynamicObject();
        auto running = ctx.capture->running();
        o->setProperty ("capturing", running);

        if (running)
        {
            o->setProperty ("file", ctx.capture->file().getFullPathName());
            o->setProperty ("seconds", round2 (ctx.capture->seconds()));
            o->setProperty ("levelDb", round2 (ctx.capture->levelDb()));
            o->setProperty ("reconnects", ctx.capture->reconnects());
            o->setProperty ("reconnecting", ctx.capture->reconnecting());
        }

        o->setProperty ("captures", listCaptures (ctx));
        o->setProperty ("ok", true);
        return juce::var (o);
    }

    // Songs in einem Mitschnitt: Pausen ab minGapSec unter silenceDb trennen,
    // kuerzere Stuecke als minSongSec haengen am Nachbarn, wenn die Pause
    // hoechstens joinGapSec lang ist; sonst kommen sie als short heraus
    juce::var opDetectSongBoundaries (DawdContext& ctx, const juce::var& cmd)
    {
        juce::File file;

        if (auto err = validFile (cmd, file); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (ctx.capture->running() && file == ctx.capture->file())
            return resultObj (false, { { "error", "capture in progress" } });

        if (auto err = blockedFileError (ctx, file); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto reader = openReader (ctx, file);

        if (reader == nullptr)
            return resultObj (false, { { "error", "cannot read audio file: " + file.getFileName() } });

        BoundaryParams p;
        p.silenceDb = (double) cmd.getProperty ("silenceDb", p.silenceDb);
        p.minGapSec = (double) cmd.getProperty ("minGapSec", p.minGapSec);
        p.minSongSec = (double) cmd.getProperty ("minSongSec", p.minSongSec);
        p.joinGapSec = (double) cmd.getProperty ("joinGapSec", p.joinGapSec);

        if (p.silenceDb < -120.0 || p.silenceDb > -10.0)
            return resultObj (false, { { "error", "silenceDb must be -120 .. -10" } });

        if (p.minGapSec < 0.1 || p.minGapSec > 30.0)
            return resultObj (false, { { "error", "minGapSec must be 0.1 .. 30" } });

        if (p.minSongSec < 0.0 || p.minSongSec > 1800.0)
            return resultObj (false, { { "error", "minSongSec must be 0 .. 1800" } });

        if (p.joinGapSec < 0.0 || p.joinGapSec > 120.0)
            return resultObj (false, { { "error", "joinGapSec must be 0 .. 120" } });

        const double hop = 0.05;
        auto duration = (double) reader->lengthInSamples / reader->sampleRate;
        auto r = findSongs (windowLevels (*reader, hop), hop, duration, p);

        juce::Array<juce::var> songs, gaps;

        for (size_t i = 0; i < r.songs.size(); ++i)
        {
            auto& s = r.songs[i];
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", (int) i + 1);
            o->setProperty ("start", round3 (s.start));
            o->setProperty ("end", round3 (s.end));
            o->setProperty ("seconds", round2 (s.end - s.start));
            o->setProperty ("levelDb", round2 (s.levelDb));

            if (s.end - s.start < p.minSongSec)
                o->setProperty ("short", true);

            songs.add (juce::var (o));
        }

        for (auto& [a, b] : r.gaps)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("start", round3 (a));
            o->setProperty ("end", round3 (b));
            gaps.add (juce::var (o));
        }

        return resultObj (true, { { "file", file.getFullPathName() }, { "duration", round2 (duration) },
                                  { "songs", songs }, { "gaps", gaps } });
    }

    // Songs eines Mitschnitts als WAV nach <config>/references/: songs =
    // [{start, end, name?}] (Sekunden der Datei, z. B. aus
    // detectSongBoundaries). Ohne name: Mitschnitt-Name + Nummer. Vorhandene
    // Dateien bleiben, der neue Name bekommt " (2)".
    juce::var opSaveCaptureSongs (DawdContext& ctx, const juce::var& cmd)
    {
        juce::File file;

        if (auto err = validFile (cmd, file); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (ctx.capture->running() && file == ctx.capture->file())
            return resultObj (false, { { "error", "capture in progress" } });

        auto reader = openReader (ctx, file);

        if (reader == nullptr)
            return resultObj (false, { { "error", "cannot read audio file: " + file.getFileName() } });

        auto* list = cmd.getProperty ("songs", {}).getArray();

        if (list == nullptr || list->isEmpty())
            return resultObj (false, { { "error", "need songs: [{start, end, name?}]" } });

        auto duration = (double) reader->lengthInSamples / reader->sampleRate;

        // erst alles pruefen, dann schreiben — ein Fehler schreibt nichts
        for (auto& s : *list)
        {
            auto a = (double) s.getProperty ("start", -1.0), b = (double) s.getProperty ("end", -1.0);

            if (! s.isObject() || a < 0.0 || b <= a || b > duration + 1e-3)
                return resultObj (false, { { "error", "need 0 <= start < end <= file length (" + juce::String (round3 (duration)) + " s)" } });
        }

        auto dir = referencesDir (ctx);

        if (! dir.createDirectory())
            return resultObj (false, { { "error", "cannot create " + dir.getFullPathName() } });

        juce::WavAudioFormat wav;
        auto bits = juce::jlimit (16, 24, (int) reader->bitsPerSample);
        juce::Array<juce::var> saved;
        int number = 0;

        for (auto& s : *list)
        {
            ++number;
            auto name = s.getProperty ("name", {}).toString().trim();

            if (name.isEmpty())
                name = file.getFileNameWithoutExtension() + " " + juce::String (number).paddedLeft ('0', 2);

            auto legal = juce::File::createLegalFileName (name);
            auto dest = dir.getChildFile (legal + ".wav");

            for (int k = 2; dest.exists(); ++k)
                dest = dir.getChildFile (legal + " (" + juce::String (k) + ").wav");
            auto s0 = (juce::int64) ((double) s.getProperty ("start", 0.0) * reader->sampleRate);
            auto s1 = std::min (reader->lengthInSamples, (juce::int64) ((double) s.getProperty ("end", 0.0) * reader->sampleRate));

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (new juce::FileOutputStream (dest), reader->sampleRate, reader->numChannels, bits, {}, 0));

            if (writer == nullptr)
                return resultObj (false, { { "error", "cannot write " + dest.getFullPathName() } });

            if (! writer->writeFromAudioReader (*reader, s0, s1 - s0))
                return resultObj (false, { { "error", "cannot write " + dest.getFullPathName() } });

            writer.reset();

            auto* o = new juce::DynamicObject();
            o->setProperty ("file", dest.getFullPathName());
            o->setProperty ("name", dest.getFileNameWithoutExtension());
            o->setProperty ("seconds", round2 ((double) (s1 - s0) / reader->sampleRate));
            saved.add (juce::var (o));
        }

        return resultObj (true, { { "saved", saved }, { "dir", dir.getFullPathName() } });
    }
}
