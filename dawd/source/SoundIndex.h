#pragma once

#include "SoundFeatures.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Klang-Index der Sample-Bibliotheken: je Datei CLAP-Vektoren (einer je
// 10-s-Fenster, hoechstens 6), gemessene Merkmale und eine Kategorie (aus dem
// Namen, sonst per CLAP gegen Kategorie-Texte). Gebaut im Hintergrund mit
// mehreren Workern (je eine ONNX-Session, ein Thread, nice 19 — die Audio-
// Threads haben Vorrang), inkrementell (Groesse + Aenderungszeit) und
// fortsetzbar: jede fertige Datei wird an <config>/index/<hash>.sound
// angehaengt, ein Abbruch verliert nichts. Die Suche selbst vergleicht nur
// Vektoren im Speicher (Brute Force, ~52.000 Dateien in Millisekunden).
namespace juce { class AudioFormatManager; }
namespace dawd::clap { class AudioEncoder; }

namespace dawd
{
    class SoundIndex
    {
    public:
        explicit SoundIndex (const juce::File& configDir);
        ~SoundIndex();

        // Kategorien (API-Namen); -1 = unbekannt
        static const juce::StringArray& categoryNames();
        // Kategorie aus Pfadworten ("Drums/Snare/Snare Tight.wav" -> snare)
        // bzw. aus den Worten einer Anfrage; -1 ohne Treffer
        static int categoryFromName (const juce::String& relPath);
        static int categoryFromText (const juce::String& text);

        void load (const juce::File& root);
        void forget (const juce::File& root);
        // Datensaetze unter dir loeschen, auch aus der Datei (Ordner fuer die
        // Analyse gesperrt); Anzahl der geloeschten
        int forgetUnder (const juce::File& dir);

        struct Item
        {
            juce::File root;
            juce::String rel;
        };

        // Analyse starten (laufende wird nicht doppelt gestartet); "" oder Fehler
        juce::String start (std::vector<Item> items);
        void stop();

        struct Progress
        {
            bool running = false;
            int done = 0, total = 0, failed = 0, skipped = 0;
            double etaSeconds = 0.0;
            juce::String error;
        };

        Progress progress() const;
        int analyzedCount (const juce::File& root) const;

        struct Entry
        {
            sound::Features features;
            int category = -1;
            bool categoryFromName = false;
            std::vector<float> starts;   // Fensterbeginn je Vektor, Sekunden
            std::vector<float> vecs;     // starts.size() * clap::dim
        };

        // Kurzinfo je Datei in einem Durchgang (eine Sperre fuer alle):
        // Merkmale, Kategorie und — mit query — die hoechste Aehnlichkeit zu
        // einem ihrer Fenster samt Fensterbeginn
        struct Brief
        {
            bool analyzed = false;
            sound::Features features;
            int category = -1;
            bool categoryFromName = false;
            double score = 0.0, at = 0.0;
        };

        std::vector<Brief> brief (const std::vector<juce::File>& files, const std::vector<float>* query) const;

    private:
        struct Record
        {
            juce::int64 size = 0, mtime = 0;
            bool ok = false;
            Entry entry;
        };

        struct Root
        {
            juce::File root, file;
            int records = 0;   // im File, auch ueberholte
        };

        juce::File fileFor (const juce::File& root) const;
        void appendRecord (Root& r, const std::string& key, const juce::String& rel, const Record& rec);
        void rewrite (Root& r);
        void run (std::vector<Item> items);
        static Record analyzeFile (const juce::File& file, const juce::String& rel, juce::AudioFormatManager& formats,
                                   clap::AudioEncoder& encoder, const std::vector<std::vector<float>>& labels,
                                   juce::String& error);

        juce::File configDir;
        mutable std::mutex lock;
        std::unordered_map<std::string, Record> records;   // absoluter Pfad -> Datensatz
        std::vector<Root> roots;

        std::thread manager;
        std::atomic<bool> running { false }, cancel { false };
        std::atomic<int> done { 0 }, total { 0 }, failed { 0 }, skipped { 0 };
        std::atomic<double> startedAt { 0.0 };
        juce::String lastError;   // unter lock
    };
}
