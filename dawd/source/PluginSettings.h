#pragma once

#include "Dawd.h"

#include <functional>

// Parameter fuer Agenten lesbar und setzbar machen: eindeutige Namen (4OSC
// hat z. B. dreimal "Mix" — Reverb, Delay, Chorus), Werte in echten
// Einheiten (Hz, dB) neben dem normierten 0..1, und Einstellungen, die keine
// automatisierbaren Parameter sind (4OSC: Wellenform, Filtertyp, Effekte
// an/aus) — ohne sie klingt 4OSC wie ein Sinus und Filter/Effekte wirken nicht.
namespace dawd
{
    // Eindeutiger Name eines Parameters im Plugin: der eigene Name, bei
    // Doppelten aus der Parameter-Id ("reverbMix" -> "Reverb Mix")
    juce::String parameterLabel (te::Plugin&, te::AutomatableParameter&);
    // Alle Namen in einem Lauf (gleiche Regel wie parameterLabel, in der
    // Reihenfolge von getAutomatableParameters) — parameterLabel je
    // Parameter läuft über alle, bei Surge XT (778) wurde das kubisch
    juce::StringArray parameterLabels (te::Plugin&);

    // Parameter per eindeutigem Namen, sonst eigenem Namen, sonst Id
    te::AutomatableParameter* findParameter (te::Plugin&, const juce::String& name);

    // Die aktuellen Namen aller Parameter merken: findParameter trifft sie
    // auch nach dem naechsten Neuladen, solange das Plugin dort noch
    // Rohnamen meldet (Auto-Node in derselben Op, settleParameterNames)
    void rememberParameterNames (te::Plugin&);

    // Normierten Wert (0..1) zu einem Anzeigetext finden ("1.2 kHz", "-6 dB",
    // "Saw"): Zahlen per Binaersuche ueber den Text des Plugins (k/m vor der
    // Einheit eingerechnet), Woerter per Abtasten. Fehlertext oder "".
    juce::String valueForText (te::AutomatableParameter&, const juce::String& text, float& normalised);
    juce::String valueForNumberText (te::AutomatableParameter&, const juce::String& text, float& normalised, bool scaleMilli);

    // "real" = Zahl in der angezeigten Einheit (dB, Hz, ms, %): stimmt der
    // Wertebereich des Parameters damit ueberein (EQ, Limiter), direkt, sonst
    // ueber den Anzeigetext (Kompressor, externe Plugins); Zeiten, deren Text
    // zwischen ms und s wechselt, immer in ms. Fehlertext (mit Einheit) oder "".
    bool realIsDisplay (te::AutomatableParameter&);
    juce::String valueForReal (te::AutomatableParameter&, double real, float& normalised);

    // Verlauf eines Parameters ueber 0..1 ohne ihn zu aendern: texts an n
    // Stuetzstellen [[0..1, Text], …], scale linear | log | stepped | choice
    // (choice: options), default = Wert beim Laden des Plugins
    juce::var parameterShape (te::AutomatableParameter&, int samples);

    // {index, name, value (0..1), real, text, unit, min, max}
    juce::var parameterVar (te::Plugin&, te::AutomatableParameter&, int index);

    struct PluginSetting
    {
        juce::String name;
        juce::String kind;               // choice | int | bool | float
        juce::StringArray options;       // choice
        int min = 0, max = 1;            // int
        double fmin = 0.0, fmax = 1.0;   // float
        juce::String unit;               // float/int: Einheit ("ms", "Hz")
        std::function<juce::var()> get;
        std::function<void (const juce::var&)> set;
    };

    // Einstellungen eines Plugins ohne Automations-Parameter: 4OSC (Wellen,
    // Filter, Effekte), Chorus und Phaser (alle Regler), Delay (Laenge) —
    // leer fuer andere
    std::vector<PluginSetting> pluginSettings (te::Plugin&);

    // Wert pruefen und in die Form der Einstellung bringen; Fehlertext oder ""
    juce::String checkSetting (const PluginSetting&, const juce::var& in, juce::var& out);

    // {name, kind, value, options | min/max}
    juce::var settingVar (const PluginSetting&);
}
