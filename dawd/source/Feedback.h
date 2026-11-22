#pragma once

#include "Dawd.h"

// Feedback-Kanten: der Tracktion-Graph ist azyklisch (ReturnNode wirft
// Sends, die in sich zurueckfuehren, still raus). Eine Schleife laeuft
// deshalb ueber ein eigenes Plugin-Paar mit Ringpuffer:
//
//   FeedbackSend   am Ende der Kette der Quelle — schreibt ihr Signal
//   FeedbackReturn im Ziel-Modul vor dem Plugin — liest es verzoegert
//                  (mindestens ein Audio-Block), Pegel, Soft-Clip (tanh)
//
// Beide teilen einen Puffer ueber (Edit, Schluessel). Die Position zaehlt
// in Bloecken: beide laufen einmal je Block, die Verzoegerung >= ein Block
// liest nur Bloecke, die schon fertig geschrieben sind — unabhaengig davon,
// welcher Thread zuerst dran ist. Laeuft der Send nicht (z. B. Spur ohne
// Material), synchronisiert der Return neu und liefert Stille.
namespace dawd
{
    struct FeedbackRing;

    class FeedbackSendPlugin : public te::Plugin
    {
    public:
        explicit FeedbackSendPlugin (te::PluginCreationInfo);
        ~FeedbackSendPlugin() override;

        static const char* xmlTypeName;

        juce::String getName() const override                        { return "Feedback Send"; }
        juce::String getPluginType() override                        { return xmlTypeName; }
        juce::String getSelectableDescription() override             { return getName(); }
        int getNumOutputChannelsGivenInputs (int numInputs) override { return numInputs; }
        BusLayout getBusses() const override                         { return BusLayout::singlePassThrough(); }
        bool takesMidiInput() override                               { return true; }
        bool canBeAddedToClip() override                             { return false; }
        bool canBeAddedToRack() override                             { return false; }

        void initialise (const te::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const te::PluginRenderContext&) override;
        void restorePluginStateFromValueTree (const juce::ValueTree&) override;

        juce::CachedValue<int> key;

    private:
        std::shared_ptr<FeedbackRing> ring;
    };

    class FeedbackReturnPlugin : public te::Plugin
    {
    public:
        explicit FeedbackReturnPlugin (te::PluginCreationInfo);
        ~FeedbackReturnPlugin() override;

        static const char* xmlTypeName;

        juce::String getName() const override                        { return "Feedback Return"; }
        juce::String getPluginType() override                        { return xmlTypeName; }
        juce::String getSelectableDescription() override             { return getName(); }
        int getNumOutputChannelsGivenInputs (int numInputs) override { return juce::jmax (2, numInputs); }
        BusLayout getBusses() const override                         { return BusLayout::singlePassThrough(); }
        bool takesMidiInput() override                               { return true; }
        bool producesAudioWhenNoAudioInput() override                { return true; }
        bool canBeAddedToClip() override                             { return false; }
        bool canBeAddedToRack() override                             { return false; }

        void initialise (const te::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const te::PluginRenderContext&) override;
        void restorePluginStateFromValueTree (const juce::ValueTree&) override;

        // Kleinste Verzoegerung in ms (ein Block) — 0 bis zum ersten initialise
        double minDelayMs() const;

        juce::CachedValue<int> key;
        juce::CachedValue<float> level, delayMs;

        // Automatisierbar (Songzeit): haengen an level/delayMs. Setzen nur ueber
        // setParameter — ein direkt gesetzter CachedValue erreicht den
        // Parameter nicht.
        te::AutomatableParameter::Ptr levelParam, delayParam;

    private:
        std::shared_ptr<FeedbackRing> ring;
        juce::int64 readPos = 0;
        double sampleRate = 44100.0;
        int blockSize = 0;
    };

    // Beim Engine-Start registrieren (vor dem ersten Laden eines Edits).
    void registerFeedbackPlugins (te::Engine&);

    constexpr double maxFeedbackDelayMs = 2000.0;
}
