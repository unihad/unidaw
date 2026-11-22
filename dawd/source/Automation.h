#pragma once

#include "Dawd.h"

#include <functional>

// Automation: Parameter ueber die Songzeit. Jedes Ziel hat eine API-Id, die
// Agenten und UI benutzen; dahinter haengen ein oder mehrere Engine-Parameter
// mit ihrer AutomationCurve (im Edit, also je Baum-Knoten — Branches erben):
//
//   t<i>.volume                 Spur-Lautstaerke in dB (-100..6), VolumeAndPan
//   t<i>.pan                    Panorama -1..1
//   g<N>.volume / g<N>.pan      Fader einer Gruppe (hinter ihrer Kette), wie t<i>
//   p<N>.wet                    Dry/Wet des Moduls 0..1 (Rack wet + dry = 1 - wet)
//   p<N>.<Parametername>        Plugin-Parameter normalisiert 0..1 (verlinkte
//                               Instanzen folgen, wie bei setParameter)
//   edge:<von>><nach>           Pegel einer Kante 0..1 (AuxSend) — eine Kante
//                               "an/aus ueber die Zeit" ist eine Stufe 0 <-> Pegel
//   feedback:<von>><nach>.level / .delayMs   Feedback-Kante
//
// Punkte: [{time (s), value (API-Einheit), curve: "linear" | "step"}];
// bei p<N>.<Parametername> statt value auch real (angezeigte Zahl in der
// Einheit des Parameters wie setParameter — Decision 2026-10-24-einheiten-real).
// "step" haelt den Wert bis zum naechsten Punkt (in der Kurve ein
// Zusatzpunkt zur Zeit des naechsten Punkts).
namespace dawd
{
    struct AutomationTarget
    {
        juce::String id, label, unit, error;
        double min = 0.0, max = 1.0;

        struct Binding
        {
            te::AutomatableParameter* param = nullptr;
            std::function<float (double)> toParam;   // API-Wert -> Parameterwert
        };

        std::vector<Binding> bindings;            // [0] = primaer (wird gelesen)
        std::function<double (float)> fromParam;  // primaerer Parameterwert -> API-Wert

        // real -> API-Wert (Plugin-Parameter: 0..1); Fehlertext oder "".
        // Leer = das Ziel nimmt kein real.
        std::function<juce::String (double real, double& value)> fromReal;
    };

    // Ziel aufloesen; error gesetzt, wenn die Id nicht passt (API-Vertrag).
    AutomationTarget findAutomationTarget (te::Edit&, const juce::String& id);

    // Alle Ziele; target filtert auf einen Knoten ("t0", "p3": seine Werte und
    // die Kanten von/zu ihm). Leer = alle.
    std::vector<AutomationTarget> listAutomationTargets (te::Edit&, const juce::String& target = {}, bool onlyAutomated = false);

    // Punkte mit real in Punkte mit value umrechnen (out); Fehlertext oder ""
    // (beides an einem Punkt, real am falschen Ziel, real ausserhalb).
    juce::String resolveAutomationReals (const AutomationTarget&, const juce::var& points, juce::var& out);

    // Fehlertext oder "" (Zeiten >= 0 aufsteigend, Werte im Bereich, curve).
    juce::String validateAutomationPoints (const AutomationTarget&, const juce::var& points);

    // Punkte ersetzen (leer = Automation weg, der Basiswert gilt wieder).
    void writeAutomation (te::Edit&, const AutomationTarget&, const juce::var& points);

    // Punkte in API-Einheiten (Stufen zurueckgefaltet).
    juce::var readAutomation (const AutomationTarget&);

    // {param, label, unit, min, max, value, automated, points, modifier?}
    juce::var describeAutomationTarget (const AutomationTarget&, bool withPoints);

    // Modifier: ein LFO (te::LFOModifier) oder Step-Modifier
    // (te::StepModifier) der Engine je Ziel, zur Kurve dazu. Er liegt in
    // der ModifierList der Spur, die den Parameter traegt (Fader-, Modul-,
    // Gruppen- bzw. Quell-Spur der Kante), die Zuordnungen in den Plugins
    // (MODIFIERASSIGNMENTS) — alles im Edit, also je Baum-Knoten. Immer im
    // Transport synchron (gleicher Render bei gleicher Songzeit).
    // depth = Ausschlag in Anteilen des Regelbereichs (normalisiert),
    // +-depth um den Basiswert; dry (= 1 - wet) bekommt -depth.
    //
    // Felder: {type?, rate?, depth?} und je Typ lfo {wave?, phase?} bzw.
    // step {steps (2..64 Werte -1..1, Pflicht beim Anlegen)}; fehlende
    // bleiben (neu: lfo, sine, 1/4, 0.25, 0; step 1/16 je Schritt). Ohne
    // type gilt der vorhandene Typ; ein anderer Typ ersetzt den Modifier
    // (Tiefe bleibt). Fehlertext oder "".
    juce::String validateModifier (const AutomationTarget&, const juce::var& settings);
    void writeModifier (te::Edit&, const AutomationTarget&, const juce::var& settings);
    void clearModifier (te::Edit&, const AutomationTarget&);

    // {type: lfo, wave, rate, depth, phase, low, high} bzw. {type: step,
    // steps, rate, depth, low, high} oder void (kein Modifier); low/high =
    // Ausschlag (+-depth) in API-Einheiten um den aktuellen Basiswert
    juce::var describeModifier (const AutomationTarget&);

    // Der aktuelle Wert eines Parameters mit Modifier schwingt (getVolumeDb
    // u. a.). Vor dem Umziehen eines Faders: den eingestellten Wert ohne
    // Modifier-Anteil setzen, sonst wanderte er um den Ausschlag.
    void settleModifierBase (te::AutomatableParameter&);

    // Eingestellter Fader-Wert ohne Modifier-Ausschlag: mit Kurve ihr Wert
    // am Playhead, sonst der gesetzte (trackMix/groupMix/state melden ihn —
    // Praxistest remix-1: trackMix {volumeDb: -1} meldete -5,6 dB, den
    // LFO-Stand an der Stopp-Position)
    double faderSetDb (te::VolumeAndPanPlugin&);
    double faderSetPan (te::VolumeAndPanPlugin&);

    // Fader wandert (syncFaders): der Modifier zieht mit
    void moveModifier (te::Edit&, te::AutomatableParameter& from, te::AutomatableParameter& to);
}
