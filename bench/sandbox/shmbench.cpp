// Mess-Prototyp Plugin-Sandboxing: ein Audio-Block geht ueber Shared Memory
// in einen anderen Prozess ("Plugin-Host"), wird dort gerechnet und kommt im
// selben Block zurueck (synchron, keine zusaetzliche Latenz). Gemessen wird
// der Aufschlag gegenueber dem Aufruf im eigenen Prozess: Rundlauf minus
// Rechenzeit des Plugins. Kein Audio-Geraet, keine Engine — nur Prozesse,
// mmap und Futex. Siehe README.md.
//
// g++ -O2 -std=c++20 -pthread shmbench.cpp -o shmbench

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr int maxFrames = 4096;

    // Ein Kanal je Plugin-Instanz: Steuerwoerter + Audio hin und zurueck
    struct alignas (64) Slot
    {
        std::atomic<uint32_t> request { 0 };   // Host zaehlt hoch = "rechne Block n"
        alignas (64) std::atomic<uint32_t> done { 0 };      // Worker setzt n = "fertig"
        alignas (64) uint32_t frames = 0;
        int64_t workerDspNs = 0;                // Rechenzeit im Worker (zum Abziehen)
        float in[2][maxFrames];
        float out[2][maxFrames];
    };

    int64_t nowNs()
    {
        timespec t;
        clock_gettime (CLOCK_MONOTONIC, &t);
        return (int64_t) t.tv_sec * 1000000000LL + t.tv_nsec;
    }

    long futexWait (std::atomic<uint32_t>* addr, uint32_t expected, const timespec* timeout)
    {
        return syscall (SYS_futex, reinterpret_cast<uint32_t*> (addr), FUTEX_WAIT, expected, timeout, nullptr, 0);
    }

    void futexWake (std::atomic<uint32_t>* addr)
    {
        syscall (SYS_futex, reinterpret_cast<uint32_t*> (addr), FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    bool tryFifo (int prio)
    {
        sched_param sp {};
        sp.sched_priority = prio;
        return pthread_setschedparam (pthread_self(), SCHED_FIFO, &sp) == 0;
    }

    // "Plugin": Gain plus eine feste Rechenlast in Mikrosekunden (busy loop),
    // damit der Worker wirklich laeuft und die Zeit abziehbar ist
    void processBlock (float in[2][maxFrames], float out[2][maxFrames], int frames, int workUs)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = in[c][i] * 0.5f;

        if (workUs > 0)
        {
            auto end = nowNs() + workUs * 1000LL;
            volatile float x = 0.0f;

            while (nowNs() < end)
                x = x + std::sin ((float) x);
        }
    }

    struct Options
    {
        std::string name = "run";
        int workers = 1;          // 0 = im eigenen Prozess (Vergleich)
        int blocks = 2000;
        int frames = 512;
        int rate = 44100;
        bool paced = true;        // Bloecke im Takt der Samplerate (Aufwachen aus dem Leerlauf)
        bool parallel = false;    // je Worker ein Host-Thread (Graph mit Thread-Pool) statt Kette
        int spinUs = 0;           // vor dem Futex so lange aktiv warten
        int workUs = 0;
        bool fifo = false;
        int killAfter = -1;       // Absturz-Test: Worker 0 nach so vielen Bloecken mit SIGKILL
    };

    struct Stats
    {
        std::vector<int64_t> overheadNs;   // je Block und Worker: Rundlauf - Rechenzeit
        std::vector<int64_t> blockNs;      // je Block und Host-Thread: Summe ueber die Kette
        int missed = 0;                    // Frist (Block-Budget) verpasst -> Stille
        int64_t crashDetectNs = -1;
        int blocksAfterCrash = 0;
    };

    void workerLoop (Slot* s, int workUs, bool fifo)
    {
        if (fifo)
            tryFifo (70);

        uint32_t seen = 0;

        for (;;)
        {
            uint32_t r;

            while ((r = s->request.load (std::memory_order_acquire)) == seen)
                futexWait (&s->request, seen, nullptr);

            if (r == 0xffffffffu)
                _exit (0);

            seen = r;
            auto t0 = nowNs();
            processBlock (s->in, s->out, (int) s->frames, workUs);
            s->workerDspNs = nowNs() - t0;
            s->done.store (r, std::memory_order_release);
            futexWake (&s->done);
        }
    }

    // Ein Block an einen Worker, synchron mit Frist. false = Frist verpasst
    // (Worker haengt oder ist tot) -> der Host gibt Stille aus
    bool roundTrip (Slot* s, uint32_t seq, int frames, int64_t budgetNs, int spinUs, int64_t& overhead)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < frames; ++i)
                s->in[c][i] = (float) ((i + seq) % 100) * 0.01f;

        s->frames = (uint32_t) frames;
        auto t0 = nowNs();
        s->request.store (seq, std::memory_order_release);
        futexWake (&s->request);

        auto deadline = t0 + budgetNs;
        auto spinEnd = t0 + spinUs * 1000LL;

        for (;;)
        {
            // Erwarteten Wert genau einmal lesen: kam "fertig" zwischen Pruefen
            // und Warten, kehrt der Futex sofort zurueck (sonst schlief der
            // Host bis zur Frist — erster Lauf, Ausreisser von 11,6 ms)
            const auto d = s->done.load (std::memory_order_acquire);

            if (d == seq)
                break;

            auto t = nowNs();

            if (t >= deadline)
                return false;

            if (t < spinEnd)
                continue;

            auto left = deadline - t;
            timespec ts { (time_t) (left / 1000000000LL), (long) (left % 1000000000LL) };
            futexWait (&s->done, d, &ts);
        }

        overhead = (nowNs() - t0) - s->workerDspNs;
        return true;
    }

    Stats run (const Options& o)
    {
        Stats st;
        const int64_t budgetNs = (int64_t) o.frames * 1000000000LL / o.rate;
        const int n = std::max (1, o.workers);

        if (o.workers == 0)
        {
            // Vergleich: dasselbe "Plugin" im eigenen Prozess
            static Slot local;
            if (o.fifo) tryFifo (80);
            auto next = nowNs();

            for (int b = 0; b < o.blocks; ++b)
            {
                auto t0 = nowNs();
                processBlock (local.in, local.out, o.frames, o.workUs);
                st.overheadNs.push_back (nowNs() - t0 - o.workUs * 1000LL);
                st.blockNs.push_back (st.overheadNs.back());

                if (o.paced) { next += budgetNs; timespec ts { (time_t) (next / 1000000000LL), (long) (next % 1000000000LL) }; clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr); }
            }

            return st;
        }

        auto* slots = static_cast<Slot*> (mmap (nullptr, sizeof (Slot) * n, PROT_READ | PROT_WRITE,
                                                MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        for (int i = 0; i < n; ++i)
            new (&slots[i]) Slot();

        std::vector<pid_t> pids;

        for (int i = 0; i < n; ++i)
        {
            pid_t p = fork();

            if (p == 0)
                workerLoop (&slots[i], o.workUs, o.fifo);

            pids.push_back (p);
        }

        std::vector<std::vector<int64_t>> perThread (o.parallel ? n : 1);
        std::vector<std::vector<int64_t>> perBlock (o.parallel ? n : 1);
        std::vector<int> missed (o.parallel ? n : 1, 0);
        std::atomic<int64_t> crashAt { -1 };
        std::atomic<int64_t> crashDetected { -1 };
        std::atomic<int> afterCrash { 0 };

        auto drive = [&] (int first, int count, int idx)
        {
            if (o.fifo) tryFifo (80);
            auto next = nowNs();
            bool dead = false;

            for (int b = 1; b <= o.blocks; ++b)
            {
                int64_t sum = 0;

                for (int w = first; w < first + count; ++w)
                {
                    if (w == 0 && b == o.killAfter)
                    {
                        crashAt = nowNs();
                        kill (pids[0], SIGKILL);
                    }

                    if (w == 0 && dead)
                        continue;   // gestorbenes Plugin: Modul stumm, der Rest laeuft

                    int64_t ov = 0;

                    if (roundTrip (&slots[w], (uint32_t) b, o.frames, budgetNs, o.spinUs, ov))
                    {
                        perThread[idx].push_back (ov);
                        sum += ov;
                    }
                    else
                    {
                        ++missed[idx];

                        // Frist verpasst: lebt der Worker noch?
                        if (waitpid (pids[w], nullptr, WNOHANG) == pids[w])
                        {
                            if (w == 0) { dead = true; crashDetected = nowNs(); }
                        }
                    }
                }

                perBlock[idx].push_back (sum);

                if (dead)
                    ++afterCrash;

                if (o.paced) { next += budgetNs; timespec ts { (time_t) (next / 1000000000LL), (long) (next % 1000000000LL) }; clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr); }
            }
        };

        if (o.parallel)
        {
            std::vector<std::thread> ts;
            for (int i = 0; i < n; ++i)
                ts.emplace_back (drive, i, 1, i);
            for (auto& t : ts)
                t.join();
        }
        else
        {
            drive (0, n, 0);
        }

        for (int i = 0; i < n; ++i)
        {
            slots[i].request.store (0xffffffffu);
            futexWake (&slots[i].request);
        }

        for (auto p : pids)
            waitpid (p, nullptr, 0);

        for (auto& v : perThread)
            st.overheadNs.insert (st.overheadNs.end(), v.begin(), v.end());
        for (auto& v : perBlock)
            st.blockNs.insert (st.blockNs.end(), v.begin(), v.end());
        for (auto m : missed)
            st.missed += m;

        if (crashAt >= 0 && crashDetected >= 0)
            st.crashDetectNs = crashDetected - crashAt;
        st.blocksAfterCrash = afterCrash;

        munmap (slots, sizeof (Slot) * n);
        return st;
    }

    double pct (std::vector<int64_t>& v, double p)
    {
        if (v.empty()) return 0;
        auto k = (size_t) std::min ((double) v.size() - 1, p * (double) v.size());
        std::nth_element (v.begin(), v.begin() + (long) k, v.end());
        return (double) v[k] / 1000.0;
    }
}

int main (int argc, char** argv)
{
    Options o;

    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--name") o.name = v;
        else if (k == "--workers") o.workers = std::atoi (v.c_str());
        else if (k == "--blocks") o.blocks = std::atoi (v.c_str());
        else if (k == "--frames") o.frames = std::atoi (v.c_str());
        else if (k == "--rate") o.rate = std::atoi (v.c_str());
        else if (k == "--paced") o.paced = v == "1";
        else if (k == "--parallel") o.parallel = v == "1";
        else if (k == "--spin-us") o.spinUs = std::atoi (v.c_str());
        else if (k == "--work-us") o.workUs = std::atoi (v.c_str());
        else if (k == "--fifo") o.fifo = v == "1";
        else if (k == "--kill-after") o.killAfter = std::atoi (v.c_str());
    }

    if (o.frames > maxFrames)
        return 1;

    const bool fifoOk = o.fifo ? tryFifo (80) : false;
    auto st = run (o);
    auto v = st.overheadNs;
    auto bl = st.blockNs;
    const double blockMaxUs = bl.empty() ? 0 : (double) *std::max_element (bl.begin(), bl.end()) / 1000.0;
    const double budgetUs = o.frames * 1e6 / o.rate;
    const double maxUs = v.empty() ? 0 : (double) *std::max_element (v.begin(), v.end()) / 1000.0;

    // CSV: name,workers,parallel,frames,rate,paced,spinUs,workUs,fifo,blocks,
    //      medianUs,p99Us,p999Us,maxUs (je Rundlauf), blockMedianUs,blockP999Us,blockMaxUs
    //      (Summe je Block und Host-Thread), budgetUs,missed,crashDetectMs,blocksAfterCrash
    std::printf ("%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%d,%.2f,%d\n",
                 o.name.c_str(), o.workers, o.parallel ? 1 : 0, o.frames, o.rate, o.paced ? 1 : 0,
                 o.spinUs, o.workUs, fifoOk ? 1 : 0, o.blocks,
                 pct (v, 0.5), pct (v, 0.99), pct (v, 0.999), maxUs,
                 pct (bl, 0.5), pct (bl, 0.999), blockMaxUs, budgetUs, st.missed,
                 st.crashDetectNs >= 0 ? st.crashDetectNs / 1e6 : -1.0, st.blocksAfterCrash);
    return 0;
}
