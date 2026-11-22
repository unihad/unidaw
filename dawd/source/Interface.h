#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <vector>

// Audio-Interface (RME Fireface UC): Hardware-Monitoring-Matrix, Fader,
// Eingangs- und Ausgangsregler ueber die ALSA-Mixer-Elemente des
// snd-usb-audio-Quirks (~/dev/fireface-uc). Die Regler gehoeren nicht zum
// Edit — sie liegen je Projekt in interface.json (ops/Interface.cpp).
namespace dawd
{
    // Ein Regler (ALSA-Mixer-Element mit einem Wert)
    struct IfControl
    {
        juce::String name;
        bool isBool = false;
        int min = 0, max = 0, value = 0;
        std::shared_ptr<const std::vector<double>> db;   // dB je Stufe (value - min), -inf = stumm; null = ohne dB
    };

    // Pegelanzeigen des Geraets (Treiber-Regler "Level Meter Peak"/"RMS"):
    // je Kanal linear 0..1 (1 = 0 dBFS) in der Folge Eingaenge 1-18,
    // Wiedergabe 1-18, Ausgaenge 1-18. Die Spitze gilt seit dem letzten
    // Lesen (jeder Leser setzt sie zurueck), RMS glaettet das Geraet.
    struct IfMeters
    {
        static constexpr int perGroup = 18;
        static constexpr int channels = 3 * perGroup;
        std::vector<float> peak, rms;
    };

    // Kanalnamen in Treiber-Folge (wie in den Matrix-Reglernamen)
    const juce::StringArray& interfaceInputNames();
    const juce::StringArray& interfaceOutputNames();

    // Zugriff auf die Regler: echt (ALSA) oder simuliert (Test-Suite,
    // --interface-sim — kein Test fasst das echte Geraet an)
    struct InterfaceBackend
    {
        virtual ~InterfaceBackend() = default;
        virtual bool simulated() const = 0;

        // Kennung des angeschlossenen Geraets — "" = nicht da; aendert sich
        // beim Aus-/Einstecken (USB-Geraetenummer)
        virtual juce::String token() = 0;
        virtual juce::String deviceName() = 0;
        virtual bool read (std::vector<IfControl>& out) = 0;

        // Werte schreiben (Name -> Stufe); "" = ok, sonst Fehlertext
        virtual juce::String write (const std::vector<std::pair<juce::String, int>>& values) = 0;

        // Pegelanzeigen; false = kein Geraet oder Treiber ohne Pegelanzeigen.
        // Liest nur die zwei Meter-Regler — billig genug fuer ~15 Abfragen/s
        virtual bool meters (IfMeters& out) = 0;

        // Nur Simulation: Geraet abziehen / einstecken (Werte wie nach dem
        // Einschalten); Pegel eines Kanals setzen (linear, bleibt stehen)
        virtual void simulatePresent (bool) {}
        virtual void simulateMeter (int /*channel*/, float /*peak*/, float /*rms*/) {}
    };

    std::unique_ptr<InterfaceBackend> makeAlsaInterface();
    std::unique_ptr<InterfaceBackend> makeSimInterface();

    // Sicht auf die Regler: Matrix (Ausgang x Quelle), Fader je Ausgang,
    // Eingangsregler, Ausgangspegel. Ausgaenge und Quellen kommen aus den
    // Reglernamen ("<Ausgang> from <Quelle> Playback Volume",
    // "<Ausgang> Mix Playback Volume").
    struct InterfaceModel
    {
        std::vector<IfControl> controls;
        std::map<juce::String, size_t> index;
        juce::StringArray outputs, sources;

        static InterfaceModel build (std::vector<IfControl> controls);

        const IfControl* find (const juce::String& name) const;

        static juce::String matrixName (const juce::String& out, const juce::String& src);
        static juce::String faderName (const juce::String& out);

        // Matrix-Kreuzpunkt oder Fader — der Monitor-Mix; Pegel und Gains
        // gehoeren dem Geraet (Drehregler vorn), nicht dem Song
        static bool isMixControl (const juce::String& name);
        juce::String outputLevelName (const juce::String& out) const;   // "" = kein Regler

        // Ein Kanal oder ein Paar ("Phones", "Line 1/2", "PCM 3/4") -> Kanaele;
        // leer = unbekannt
        juce::StringArray resolve (const juce::String& name, bool output) const;
        juce::StringArray pairs (bool output) const;

        // Startzustand des Geraets: PCM n -> Ausgang n, PCM 1/2 auch auf den
        // Kopfhoerer, Eingaenge stumm, Fader 0 dB
        std::map<juce::String, int> defaultMix() const;

        static double toDb (const IfControl& c, int value);         // -inf = stumm
        static int fromDb (const IfControl& c, double db);          // naechste Stufe, nie stumm
        static int muteValue (const IfControl& c);                  // -1 = keine Stumm-Stufe
    };
}
