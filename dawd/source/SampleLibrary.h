#pragma once

#include "SoundIndex.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// Sample-Bibliotheken fuer die Suche (findSamples) — das Gegenstueck zum
// Browser einer DAW. Wurzeln: die mitgelieferten Kits (immer) und Ordner, die
// der Nutzer anmeldet (<config>/libraries.json). Je Wurzel ein Index der
// Audiodateien (relative Pfade), im Hintergrund aufgebaut und als Textdatei
// in <config>/index/ zwischengespeichert — eine grosse Bibliothek auf einer
// langsamen Platte wird so nur einmal durchlaufen.
namespace dawd
{
    // Mitgelieferte Kits: dawd/kits/<name>/kit.json (GM-Belegung, One-Shots)
    // — erzeugt von dawd/kits/make_basic_kit.py
    juce::File kitsDirectory();

    class SampleLibrary
    {
    public:
        SampleLibrary (const juce::File& configDir, const juce::File& kitsDir);
        ~SampleLibrary();

        struct Info
        {
            juce::File root;
            int files = 0;
            bool indexing = false, builtin = false;
        };

        std::vector<Info> libraries() const;

        // Fehlertext oder ""; addRoot startet das Indizieren im Hintergrund
        juce::String addRoot (const juce::File& dir);
        juce::String removeRoot (const juce::File& dir);
        void refresh();

        struct Hit
        {
            juce::File file, root;
            double score = 0.0;
        };

        // Alle Worte der Anfrage muessen im Pfad (relativ zur Wurzel)
        // vorkommen; Treffer im Dateinamen zaehlen mehr. within: nur unter
        // diesem Ordner. indexing: true, solange eine Wurzel noch laeuft.
        std::vector<Hit> find (const juce::String& query, const juce::File& within, int limit, bool& indexing, int& total) const;

        static bool isAudioFile (const juce::File& f);

        // Alle indizierten Dateien (unter within, sonst alle) — Futter fuer
        // die Klang-Analyse; ohne die Dateien in gesperrten Ordnern
        std::vector<SoundIndex::Item> items (const juce::File& within) const;

        // Klang-Index (CLAP-Vektoren, Merkmale) zu denselben Wurzeln
        SoundIndex& sounds() { return *soundIndex; }

        // Ordner ohne Klang-Analyse (Lizenz verbietet Data Mining / KI-
        // Nutzung; docs/decisions/2026-11-18-klang-analyse-je-hersteller.md),
        // gespeichert in libraries.json. blockAnalysis haelt eine laufende
        // Analyse an und loescht die Klang-Daten darunter (removed: Anzahl).
        // Fehlertext oder "".
        juce::String blockAnalysis (const juce::File& dir, int& removed);
        juce::String allowAnalysis (const juce::File& dir);
        std::vector<juce::File> blockedForAnalysis() const;
        // Der gesperrte Ordner, unter dem f liegt; sonst juce::File()
        juce::File analysisBlockedBy (const juce::File& f) const;

    private:
        struct Index
        {
            juce::File root;
            bool builtin = false;
            std::vector<juce::String> paths;   // relativ, '/' als Trenner
            std::atomic<bool> indexing { false };
        };

        void startIndexing (std::shared_ptr<Index> index);
        juce::File cacheFileFor (const juce::File& root) const;
        void saveRoots() const;

        juce::File configDir;
        mutable std::mutex lock;
        std::vector<std::shared_ptr<Index>> indexes;
        std::vector<juce::File> noAnalysis;   // unter lock
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>> (false);
        juce::ThreadPool pool { 2 };
        std::unique_ptr<SoundIndex> soundIndex;   // zuletzt angelegt, zuerst beendet
    };
}
