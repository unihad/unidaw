#pragma once

// Gemeinsamer Speicher zwischen dawd (Stellvertreter) und dem Plugin-Host-
// Prozess (dawd --plugin-host). Beide Seiten sind dieselbe Binary — das
// Layout ist darum byteweise gleich (PositionInfo wird roh kopiert).
//
// Ein memfd je Plugin-Instanz:
//  - RT-Kanal: rtRequest/rtDone (Futex), Kopf des Blocks, Audio in place,
//    MIDI hin und zurueck. dawd schreibt den Block, zaehlt rtRequest hoch,
//    weckt den Host und wartet mit Frist auf rtDone == rtRequest.
//  - Steuerkanal: ctlRequest/ctlDone (Futex), Op, JSON-Text und Blob im
//    Datenbereich am Ende (sparse: nur beschriebene Seiten belegen RAM).
//  - Parameter: Wert + Zaehler je Parameter. dawd schreibt sie aus jedem
//    Thread ohne Nachricht; der Host uebernimmt geaenderte Werte vor jedem
//    Block und vor jedem Steuer-Aufruf, der den Zustand liest.
//  - Rueckweg (Phase 2): was das Plugin selbst meldet (Editor, eigene
//    Modulation, Ausgangs-Parameter im Block, neue Namen) schreibt der Host
//    als Wert + Zaehler je Parameter, Gesten-Zaehler und infoSeq; jede
//    Meldung zaehlt hostEvents hoch. dawd fragt das auf dem Message-Thread ab
//    (Timer und vor jeder Op), ohne Nachricht und ohne Warten.
//  - Fenster (Phase 2): der Host meldet Lage und Klick auf Schliessen.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <type_traits>
#include <unistd.h>

namespace dawd::sandbox
{
    constexpr uint32_t shmMagic = 0x44535842;   // "DSXB"
    constexpr uint32_t shmVersion = 4;   // 3: Host-Prozess fuer mehrere Instanzen (memfd per Socket)
                                         // 4: outBase (Meldung kennt den dawd-Wert, Phase 4)
    constexpr int maxChannels = 32;
    constexpr int maxFrames = 8192;              // groessere Bloecke teilt der Stellvertreter
    constexpr int maxParams = 16384;
    constexpr size_t midiBytes = 256 * 1024;
    constexpr size_t controlBytes = 64u * 1024 * 1024;   // Zustand/JSON je Aufruf

    enum class Op : uint32_t
    {
        none = 0,
        load,            // {desc, rate, block} -> Beschreibung, Busse, Parameter, Programme
        prepare,         // {rate, block, nonRealtime} -> {latency}
        release,
        getState,        // -> Blob
        setState,        // Blob -> {values, latency, program}
        setProgram,      // {index} -> {values, program}
        getText,         // {index, value} -> {text}
        getValueForText, // {index, text} -> {value}
        quit,
        info,            // -> Namen/Einheiten, Werte, Programme, Latenz (nach infoSeq)
        setProgramName,  // {index, name} -> {programs}
        setLayout,       // {inputs, outputs, apply} -> {supported}
        openWindow,      // {title, x, y} -> {} (Editor im Host-Prozess)
        moveWindow,      // {x, y}
        closeWindow,
        editParam,       // {index, value}: Regler wie die Oberflaeche des Plugins (--window-sim)
        hasEditor        // -> {hasEditor}; erst auf Anfrage: JUCE legt dafuer bei VST3
                         // eine View an, LSP beendet ohne Display dabei den Prozess
    };

    enum RtFlags : uint32_t
    {
        rtNonRealtime = 1u << 0,
        rtBypassed    = 1u << 1,
        rtReset       = 1u << 2,
        rtHasPosition = 1u << 3
    };

    static_assert (std::is_trivially_copyable_v<juce::AudioPlayHead::PositionInfo>);
    static_assert (std::atomic<uint32_t>::is_always_lock_free);
    static_assert (std::atomic<float>::is_always_lock_free);

    struct alignas (64) Word
    {
        std::atomic<uint32_t> v { 0 };
    };

    struct Shm
    {
        uint32_t magic = 0, version = 0;
        std::atomic<int32_t> hostPid { 0 };
        std::atomic<uint32_t> hostReady { 0 };

        Word rtRequest, rtDone, ctlRequest, ctlDone;

        // RT-Block: dawd -> Host
        struct alignas (64) Rt
        {
            int32_t numSamples = 0;
            int32_t numChannels = 0;
            uint32_t flags = 0;
            uint32_t midiInBytes = 0;
            juce::AudioPlayHead::PositionInfo position;
            // Host -> dawd
            uint32_t midiOutBytes = 0;
            int32_t latency = 0;
            int64_t dspNs = 0;          // Rechenzeit im Host (Messung)
        } rt;

        // Steuerkanal
        struct alignas (64) Ctl
        {
            Op op = Op::none;
            uint32_t ok = 0;
            uint32_t jsonBytes = 0;
            uint64_t blobBytes = 0;
        } ctl;

        // Parameter dawd -> Host; paramApplied = der Zaehler, den der Host
        // zuletzt uebernommen hat (ungleich paramSeq: dawd-Wert noch offen)
        std::atomic<uint32_t> paramSeq[maxParams];
        std::atomic<float> paramValue[maxParams];
        std::atomic<uint32_t> paramApplied[maxParams];

        // Rueckweg Host -> dawd: was das Plugin selbst meldet
        Word hostEvents;
        std::atomic<uint32_t> infoSeq { 0 };          // Namen/Programme geaendert
        std::atomic<uint32_t> outSeq[maxParams];
        std::atomic<float> outValue[maxParams];
        // paramApplied zur Zeit der Meldung: ist paramSeq inzwischen weiter,
        // setzte dawd danach einen neuen Wert — die Meldung ist veraltet (z. B.
        // LV2 meldet beim prepare/setState alle alten Werte aus den Ports, bevor
        // der Host den offenen dawd-Wert uebernimmt)
        std::atomic<uint32_t> outBase[maxParams];
        std::atomic<uint32_t> gestureBegin[maxParams];
        std::atomic<uint32_t> gestureEnd[maxParams];

        // Editor-Fenster im Host: Lage samt Titelleiste (sobald der Rahmen
        // bekannt ist) und Klicks auf Schliessen
        std::atomic<uint32_t> windowMoves { 0 }, windowCloseRequests { 0 };
        std::atomic<int32_t> windowX { 0 }, windowY { 0 };

        float audio[maxChannels][maxFrames];
        uint8_t midiIn[midiBytes];
        uint8_t midiOut[midiBytes];
    };

    constexpr size_t shmSize = sizeof (Shm) + controlBytes;

    inline uint8_t* controlData (Shm* s)   { return reinterpret_cast<uint8_t*> (s) + sizeof (Shm); }

    inline int64_t nowNs()
    {
        timespec t;
        clock_gettime (CLOCK_MONOTONIC, &t);
        return (int64_t) t.tv_sec * 1000000000LL + t.tv_nsec;
    }

    // Prozessuebergreifend: kein FUTEX_PRIVATE_FLAG
    inline void futexWait (std::atomic<uint32_t>& a, uint32_t expected, int64_t timeoutNs)
    {
        timespec ts { (time_t) (timeoutNs / 1000000000LL), (long) (timeoutNs % 1000000000LL) };
        syscall (SYS_futex, reinterpret_cast<uint32_t*> (&a), FUTEX_WAIT, expected, &ts, nullptr, 0);
    }

    // Host meldet etwas (Wert, Geste, Info, Fenster) — dawd fragt ab
    inline void bumpHostEvents (Shm* s)
    {
        s->hostEvents.v.fetch_add (1, std::memory_order_release);
    }

    inline void futexWake (std::atomic<uint32_t>& a)
    {
        syscall (SYS_futex, reinterpret_cast<uint32_t*> (&a), FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    // Wartet, bis a == target, hoechstens bis deadlineNs (nowNs-Zeit)
    inline bool waitUntilEquals (std::atomic<uint32_t>& a, uint32_t target, int64_t deadlineNs)
    {
        for (;;)
        {
            auto v = a.load (std::memory_order_acquire);

            if (v == target)
                return true;

            auto left = deadlineNs - nowNs();

            if (left <= 0)
                return false;

            futexWait (a, v, left);
        }
    }

    // Wartet, bis a != seen, hoechstens timeoutNs; gibt den Wert zurueck
    inline uint32_t waitForChange (std::atomic<uint32_t>& a, uint32_t seen, int64_t timeoutNs)
    {
        auto v = a.load (std::memory_order_acquire);

        if (v == seen)
        {
            futexWait (a, seen, timeoutNs);
            v = a.load (std::memory_order_acquire);
        }

        return v;
    }

    // Steuer-Nutzlast: JSON-Text, dahinter ein Blob (Zustand). false = zu gross
    inline bool writeControl (Shm* s, const juce::var& json, const void* blob, size_t blobBytes)
    {
        auto text = juce::JSON::toString (json, true).toStdString();

        if (text.size() + blobBytes > controlBytes)
            return false;

        auto* d = controlData (s);
        std::memcpy (d, text.data(), text.size());

        if (blobBytes > 0)
            std::memcpy (d + text.size(), blob, blobBytes);

        s->ctl.jsonBytes = (uint32_t) text.size();
        s->ctl.blobBytes = blobBytes;
        return true;
    }

    inline juce::var readControl (Shm* s, juce::MemoryBlock* blob)
    {
        auto jsonBytes = std::min<size_t> (s->ctl.jsonBytes, controlBytes);
        auto blobBytes = std::min<size_t> (s->ctl.blobBytes, controlBytes - jsonBytes);
        auto* d = controlData (s);

        if (blob != nullptr)
            blob->replaceAll (d + jsonBytes, blobBytes);

        return juce::JSON::parse (juce::String::fromUTF8 ((const char*) d, (int) jsonBytes));
    }

    // Bus-Layout im Steuerkanal: {inputs, outputs}, je Bus die Kanal-Typen
    // (leer = Bus aus)
    inline juce::var layoutVar (const juce::AudioProcessor::BusesLayout& l, bool isInput)
    {
        juce::Array<juce::var> buses;

        for (auto& set : isInput ? l.inputBuses : l.outputBuses)
        {
            juce::Array<juce::var> types;

            for (auto t : set.getChannelTypes())
                types.add ((int) t);

            buses.add (types);
        }

        return buses;
    }

    inline juce::AudioProcessor::BusesLayout layoutFrom (const juce::var& args)
    {
        juce::AudioProcessor::BusesLayout l;

        for (auto isInput : { true, false })
            if (auto* a = args.getProperty (isInput ? "inputs" : "outputs", {}).getArray())
                for (auto& b : *a)
                {
                    juce::Array<juce::AudioChannelSet::ChannelType> t;

                    if (auto* ts = b.getArray())
                        for (auto& v : *ts)
                            t.add ((juce::AudioChannelSet::ChannelType) (int) v);

                    (isInput ? l.inputBuses : l.outputBuses).add (juce::AudioChannelSet::channelSetWithChannels (t));
                }

        return l;
    }

    // MIDI im Speicher: [int32 Position][uint16 Laenge][Bytes], auf 4 ausgerichtet
    inline uint32_t writeMidi (uint8_t* dest, size_t capacity, const juce::MidiBuffer& midi, int start, int length, int offset)
    {
        size_t pos = 0;

        for (const auto m : midi)
        {
            if (m.samplePosition < start || m.samplePosition >= start + length)
                continue;

            auto need = (size_t) ((6 + m.numBytes + 3) & ~3);

            if (pos + need > capacity)
                break;

            int32_t at = m.samplePosition - start + offset;
            uint16_t n = (uint16_t) m.numBytes;
            std::memcpy (dest + pos, &at, 4);
            std::memcpy (dest + pos + 4, &n, 2);
            std::memcpy (dest + pos + 6, m.data, (size_t) m.numBytes);
            pos += need;
        }

        return (uint32_t) pos;
    }

    inline void readMidi (const uint8_t* src, uint32_t bytes, juce::MidiBuffer& out, int offset)
    {
        size_t pos = 0;

        while (pos + 6 <= bytes)
        {
            int32_t at;
            uint16_t n;
            std::memcpy (&at, src + pos, 4);
            std::memcpy (&n, src + pos + 4, 2);

            if (pos + 6 + n > bytes)
                break;

            out.addEvent (src + pos + 6, (int) n, at + offset);
            pos += (size_t) ((6 + n + 3) & ~3);
        }
    }
}
