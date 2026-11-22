#include "Ops.h"
#include "../Osc.h"

// OSC-Fernsteuerung (Familie session): oscSettings {enabled?, port?, address?}
// — wie audioSettings ein Einstellungs-Op ohne Edit, ohne Node-Aktion und
// ohne Undo; gespeichert in osc.json im --config-Ordner. Empfang, Adressen
// und Abbildung auf die Ops: Osc.{h,cpp}.
namespace dawd
{
    juce::var opOscSettings (DawdContext& ctx, const juce::var& cmd)
    {
        return oscSettings (ctx, cmd);
    }
}
