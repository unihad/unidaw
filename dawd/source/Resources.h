#pragma once

#include <juce_core/juce_core.h>

// Mitgelieferte Ressourcen (ui, kits, models, plugin-profiles, recipes)
// relativ zur Binary statt ueber Pfade in den Quellordner — so laeuft dawd
// auch auf fremden Rechnern und im AppImage. Gesucht wird in dieser Reihe:
//   1. <Ordner der Binary>/<name>         (Build-Ordner: CMake legt dort
//                                           Links in den Quellordner an;
//                                           portabler Ordner)
//   2. <Ordner der Binary>/../share/unidaw/<name>   (Installation, AppImage)
//   3. $XDG_DATA_HOME/unidaw/<name>, sonst ~/.local/share/unidaw/<name>
//                                          (Nutzerordner, z. B. Modelle)
// Fehlt der Ordner ueberall, kommt der Nutzerordner zurueck (dort kann der
// Nutzer ihn anlegen). --ui und --recipes ueberschreiben das weiter.
namespace dawd
{
    juce::File resourceDir (const juce::String& name);
}
