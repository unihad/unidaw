#include "Capture.h"

#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dawd
{
    namespace
    {
        // Programm im PATH suchen — vor dem fork, im Kind ist nur noch
        // execv erlaubt
        juce::String resolve (const juce::String& prog)
        {
            if (prog.containsChar ('/'))
                return prog;

            for (auto dir : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", "/usr/bin:/bin"), ":", ""))
                if (auto f = juce::File (dir).getChildFile (prog); f.existsAsFile())
                    return f.getFullPathName();

            return {};
        }

        juce::uint32 le32 (const juce::MemoryBlock& b, size_t at)
        {
            auto* p = static_cast<const juce::uint8*> (b.getData()) + at;
            return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24);
        }

        // Offset der Nutzdaten, Bytes je Frame und Rate aus dem Kopf (die
        // ersten 4 KB reichen fuer jeden Kopf, den pw-record schreibt)
        struct WavLayout
        {
            juce::int64 dataOffset = 0, sizeFieldAt = 0;
            int blockAlign = 0, rate = 0;
        };

        bool wavLayout (const juce::File& file, WavLayout& w)
        {
            juce::FileInputStream in (file);

            if (! in.openedOk())
                return false;

            juce::MemoryBlock head;
            in.readIntoMemoryBlock (head, 4096);
            auto* bytes = static_cast<const juce::uint8*> (head.getData());

            if (head.getSize() < 20 || std::memcmp (bytes, "RIFF", 4) != 0)
                return false;

            w = {};
            size_t pos = 12;

            while (pos + 8 <= head.getSize())
            {
                auto* id = bytes + pos;
                auto size = le32 (head, pos + 4);

                if (std::memcmp (id, "fmt ", 4) == 0 && pos + 22 <= head.getSize())
                {
                    w.rate = (int) le32 (head, pos + 12);
                    w.blockAlign = (int) (bytes[pos + 20] | (bytes[pos + 21] << 8));
                }

                if (std::memcmp (id, "data", 4) == 0)
                {
                    w.dataOffset = (juce::int64) pos + 8;
                    w.sizeFieldAt = (juce::int64) pos + 4;
                    return w.blockAlign > 0 && w.rate > 0;
                }

                pos += 8 + size + (size & 1);
            }

            return false;
        }

        juce::int64 framesOf (const juce::File& file)
        {
            WavLayout w;
            return wavLayout (file, w) ? std::max<juce::int64> (0, (file.getSize() - w.dataOffset) / w.blockAlign) : 0;
        }
    }

    SystemCapture::~SystemCapture()
    {
        if (active)
            stop();
    }

    juce::String SystemCapture::start (const juce::File& file, const juce::String& target, int rate)
    {
        if (active)
            return "capture in progress";

        juce::StringArray words;
        auto custom = juce::SystemStats::getEnvironmentVariable ("UNIDAW_CAPTURE_CMD", {});

        if (custom.isNotEmpty())
            words.addTokens (custom, " ", "\"'");
        else
        {
            words.addArray ({ "pw-record", "-P", "{ stream.capture.sink=true }", "--rate", juce::String (rate),
                              "--channels", "2", "--format", "s24" });

            if (target.isNotEmpty())
                words.addArray ({ "--target", target });
        }

        words.removeEmptyStrings();
        prog = resolve (words[0]);

        if (prog.isEmpty())
            return words[0] + " not found (needs PipeWire)";

        if (! file.getParentDirectory().createDirectory())
            return "cannot create " + file.getParentDirectory().getFullPathName();

        args.clear();
        for (auto& w : words)
            args.push_back (w.toStdString());

        current = file;
        log = file.withFileExtension (".log");
        segments.clear();
        restarts = 0;
        lastError = {};

        if (auto err = spawn (file); err.isNotEmpty())
            return err;

        active = true;
        started = juce::Time::getCurrentTime();
        startTimer (500);
        return {};
    }

    juce::String SystemCapture::spawn (const juce::File& dest)
    {
        auto argsNow = args;
        argsNow.push_back (dest.getFullPathName().toStdString());
        std::vector<char*> argv;
        for (auto& a : argsNow)
            argv.push_back (a.data());
        argv.push_back (nullptr);
        auto progPath = prog.toStdString(), logPath = log.getFullPathName().toStdString();
        auto parent = getpid();

        auto child = fork();

        if (child < 0)
            return "cannot start " + juce::String (args[0]);

        if (child == 0)
        {
            // stirbt dawd, endet auch der Mitschnitt (SIGINT: Header fertig)
            prctl (PR_SET_PDEATHSIG, SIGINT);

            if (getppid() != parent)
                _exit (127);

            int fd = open (logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

            if (fd >= 0)
            {
                dup2 (fd, 1);
                dup2 (fd, 2);
                close (fd);
            }

            int null = open ("/dev/null", O_RDONLY);
            if (null >= 0)
                dup2 (null, 0);

            execv (progPath.c_str(), argv.data());
            _exit (127);
        }

        pid = child;
        exited = false;
        segments.push_back ({ dest, juce::Time::getCurrentTime() });
        return {};
    }

    void SystemCapture::reap (bool block)
    {
        if (pid <= 0 || exited)
            return;

        int status = 0;

        if (waitpid (pid, &status, block ? 0 : WNOHANG) == pid)
        {
            exited = true;
            auto lines = juce::StringArray::fromLines (log.loadFileAsString().trim());
            lastError = lines.isEmpty() ? juce::String() : lines[lines.size() - 1].trim();
        }
    }

    // Endet das Programm mitten im Mitschnitt (PipeWire neu gestartet,
    // Ausgang weg), schreibt ein neues in ein weiteres Segment — hoechstens
    // einmal je Sekunde, solange PipeWire fehlt
    void SystemCapture::timerCallback()
    {
        reap (false);

        if (! active || ! exited || (juce::Time::getCurrentTime() - segments.back().started).inMilliseconds() < 1000)
            return;

        // leere Segmente (Programm kam nicht hoch) gleich wegwerfen
        if (segments.size() > 1 && framesOf (segments.back().file) == 0)
        {
            segments.back().file.deleteFile();
            segments.pop_back();
        }

        auto part = current.getSiblingFile (current.getFileNameWithoutExtension() + ".part" + juce::String (++restarts) + ".wav");
        part.deleteFile();
        spawn (part);
    }

    bool SystemCapture::running()
    {
        return active;
    }

    bool SystemCapture::reconnecting()
    {
        reap (false);
        return active && exited;
    }

    juce::String SystemCapture::failure()
    {
        reap (false);

        // Fehlstart = nichts geschrieben; mit Audio ist es ein Abriss, den der
        // Timer neu verbindet
        if (! active || ! exited || restarts > 0 || framesOf (current) > 0)
            return {};

        return "capture ended by itself" + (lastError.isNotEmpty() ? ": " + lastError : juce::String());
    }

    juce::String SystemCapture::stop()
    {
        if (! active)
            return "no capture";

        stopTimer();
        active = false;
        reap (false);

        if (pid > 0 && ! exited)
        {
            kill (pid, SIGINT);

            for (int i = 0; i < 60 && ! exited; ++i)
            {
                juce::Thread::sleep (50);
                reap (false);
            }

            if (! exited)
            {
                kill (pid, SIGKILL);
                reap (true);
            }
        }

        pid = -1;
        log.deleteFile();
        return joinSegments();
    }

    // Segmente in die erste Datei haengen, die Luecke zwischen zwei
    // Segmenten (Neustart) als Stille — die Zeitachse bleibt die der Uhr
    juce::String SystemCapture::joinSegments()
    {
        auto segs = segments;
        segments.clear();

        for (auto& sg : segs)
            if (sg.file.existsAsFile())
                repairWavHeader (sg.file);

        // ohne Frames (Programm kam nicht hoch) zaehlt ein Segment nicht
        std::vector<Segment> good;
        for (auto& sg : segs)
        {
            if (framesOf (sg.file) > 0)
                good.push_back (sg);
            else if (sg.file != current)
                sg.file.deleteFile();
        }

        if (good.empty())
            return "capture wrote no file" + (lastError.isNotEmpty() ? ": " + lastError : juce::String());

        if (good.front().file != current)
        {
            current.deleteFile();
            good.front().file.moveFileTo (current);
            good.front().file = current;
        }

        WavLayout base;

        if (! wavLayout (current, base))
            return "capture file is not a wav: " + current.getFileName();

        if (good.size() > 1)
        {
            juce::FileOutputStream out (current);

            if (! out.openedOk())
                return "cannot write " + current.getFullPathName();

            out.setPosition (base.dataOffset + framesOf (current) * base.blockAlign);
            out.truncate();
            auto end = good.front().started + juce::RelativeTime::seconds ((double) framesOf (current) / base.rate);

            for (size_t i = 1; i < good.size(); ++i)
            {
                WavLayout w;

                if (! wavLayout (good[i].file, w) || w.blockAlign != base.blockAlign || w.rate != base.rate)
                    continue;

                auto gap = juce::jlimit (0.0, 600.0, (good[i].started - end).inSeconds());
                auto silence = (juce::int64) std::round (gap * base.rate) * base.blockAlign;
                juce::HeapBlock<char> zeros (65536, true);

                for (juce::int64 left = silence; left > 0; left -= 65536)
                    out.write (zeros, (size_t) std::min<juce::int64> (left, 65536));

                juce::FileInputStream in (good[i].file);
                in.setPosition (w.dataOffset);
                auto frames = framesOf (good[i].file);
                out.writeFromInputStream (in, frames * w.blockAlign);
                end = good[i].started + juce::RelativeTime::seconds ((double) frames / base.rate);
                good[i].file.deleteFile();
            }

            out.flush();
        }

        if (! repairWavHeader (current))
            return "capture file is not a wav: " + current.getFileName();

        return {};
    }

    double SystemCapture::seconds() const
    {
        return active ? (juce::Time::getCurrentTime() - started).inSeconds() : 0.0;
    }

    double SystemCapture::levelDb()
    {
        WavLayout w;

        if (! active || reconnecting() || segments.empty())
            return -100.0;

        auto& file = segments.back().file;

        if (! wavLayout (file, w) || w.blockAlign != 6)
            return -100.0;

        // letzte 0,1 s; 6 Bytes je Frame = s24 stereo wie bei pw-record
        // bestellt, anderes misst nicht
        juce::FileInputStream in (file);
        auto frames = (in.getTotalLength() - w.dataOffset) / w.blockAlign;
        auto n = std::min<juce::int64> (frames, w.rate / 10);

        if (! in.openedOk() || n <= 0)
            return -100.0;

        in.setPosition (w.dataOffset + (frames - n) * w.blockAlign);
        juce::HeapBlock<juce::uint8> buf ((size_t) (n * w.blockAlign));
        in.read (buf, (int) (n * w.blockAlign));
        int peak = 0;

        for (juce::int64 i = 0; i < n * 2; ++i)
        {
            auto* p = buf + i * 3;
            int v = (int) ((juce::uint32) p[0] << 8 | (juce::uint32) p[1] << 16 | (juce::uint32) p[2] << 24) >> 8;
            peak = std::max (peak, std::abs (v));
        }

        return peak == 0 ? -100.0 : std::max (-100.0, 20.0 * std::log10 (peak / 8388608.0));
    }

    bool repairWavHeader (const juce::File& file)
    {
        WavLayout w;

        if (! wavLayout (file, w))
            return false;

        auto data = std::max<juce::int64> (0, (file.getSize() - w.dataOffset) / w.blockAlign * w.blockAlign);
        auto total = w.dataOffset + data;

        if (total - 8 > 0xffffffffLL)
            return false;

        juce::FileOutputStream out (file);

        if (! out.openedOk())
            return false;

        out.setPosition (total);
        out.truncate();   // halber Frame am Ende (abgebrochener Schreiber)
        out.setPosition (4);
        out.writeInt ((int) (juce::uint32) (total - 8));
        out.setPosition (w.sizeFieldAt);
        out.writeInt ((int) (juce::uint32) data);
        out.flush();
        return out.getStatus().wasOk();
    }

    std::vector<float> windowLevels (juce::AudioFormatReader& reader, double hop)
    {
        std::vector<float> out;
        auto win = std::max<juce::int64> (1, (juce::int64) std::round (hop * reader.sampleRate));
        auto chans = (int) reader.numChannels;
        juce::AudioBuffer<float> buf (chans, (int) win);

        for (juce::int64 pos = 0; pos < reader.lengthInSamples; pos += win)
        {
            auto n = (int) std::min (win, reader.lengthInSamples - pos);
            reader.read (&buf, 0, n, pos, true, chans > 1);
            double sum = 0.0;

            for (int c = 0; c < chans; ++c)
                for (auto* p = buf.getReadPointer (c), *e = p + n; p != e; ++p)
                    sum += (double) *p * *p;

            auto ms = sum / ((double) n * chans);
            out.push_back (ms <= 1e-12 ? -120.0f : (float) (10.0 * std::log10 (ms)));
        }

        return out;
    }

    BoundaryResult findSongs (const std::vector<float>& db, double hop, double duration, const BoundaryParams& p)
    {
        BoundaryResult r;
        r.duration = duration;

        // Laute Stuecke (Fenster-Indizes) zwischen Pausen ab minGapSec;
        // Stille am Anfang und Ende faellt immer weg
        struct Run { size_t a, b; };   // [a, b)
        std::vector<Run> loud;
        auto minGap = (size_t) std::max (1.0, std::round (p.minGapSec / hop));
        size_t i = 0, n = db.size();

        while (i < n)
        {
            while (i < n && db[i] < p.silenceDb)
                ++i;

            if (i >= n)
                break;

            auto a = i;
            size_t lastLoud = i;

            for (; i < n; ++i)
            {
                if (db[i] >= p.silenceDb)
                    lastLoud = i;
                else if (i - lastLoud >= minGap)
                    break;
            }

            loud.push_back ({ a, lastLoud + 1 });
        }

        // Zu kurze Stuecke (Pause mitten im Song) an den Nachbarn haengen —
        // ueber die kuerzere Pause, aber nur bis joinGapSec: ein Geraeusch
        // weit weg (Systemklang) bleibt allein und kommt als short heraus
        auto len = [&] (const Run& x) { return (double) (x.b - x.a) * hop; };
        auto gapSec = [&] (size_t k) { return (double) (loud[k + 1].a - loud[k].b) * hop; };   // nach Stueck k
        std::vector<bool> alone (loud.size(), false);

        for (;;)
        {
            size_t shortest = loud.size();

            for (size_t k = 0; k < loud.size(); ++k)
                if (! alone[k] && len (loud[k]) < p.minSongSec && (shortest == loud.size() || len (loud[k]) < len (loud[shortest])))
                    shortest = k;

            if (shortest == loud.size())
                break;

            auto k = shortest;
            auto before = k > 0 ? gapSec (k - 1) : 1e9, after = k + 1 < loud.size() ? gapSec (k) : 1e9;

            if (std::min (before, after) > p.joinGapSec)
            {
                alone[k] = true;
                continue;
            }

            auto into = before <= after ? k - 1 : k;
            loud[into] = { loud[into].a, loud[into + 1].b };
            loud.erase (loud.begin() + (long) into + 1);
            alone.erase (alone.begin() + (long) into + 1);
            alone[into] = false;
        }

        for (size_t k = 0; k < loud.size(); ++k)
        {
            SongSpan s;
            s.start = (double) loud[k].a * hop;
            s.end = std::min (duration, (double) loud[k].b * hop);

            for (auto j = loud[k].a; j < loud[k].b; ++j)
                s.levelDb = std::max (s.levelDb, (double) db[j]);

            if (k > 0)
                r.gaps.push_back ({ r.songs.back().end, s.start });

            r.songs.push_back (s);
        }

        return r;
    }
}
