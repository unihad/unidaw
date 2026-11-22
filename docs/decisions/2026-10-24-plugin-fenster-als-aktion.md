# Änderungen im Plugin-Fenster sind eine Node-Aktion

- **Status:** gilt — umgesetzt in 3f291a3 (Branch `folge-ui`)
- **Datum:** 2026-10-24, Matthias

## Kontext

`pluginWindow` öffnet den Editor eines Moduls (Merge 13327a2). Das Fenster
selbst ändert den Edit nicht (kein Undo, keine Aktion). Was man im Editor
dreht, landete bisher still im Plugin-Zustand des aktuellen Nodes — ohne
Aktion, Undo oder Auto-Branch.

## Entscheidung

Beim Schließen des Fensters bucht dawd eine zusammengefasste process-Aktion
`pluginWindow` am Node, wenn sich der Plugin-Zustand geändert hat — mit Undo
und Auto-Branch wie bei `setParameter`.

## Folgen

- Öffnen und Schließen ohne Änderung bleiben ohne Aktion.
- Auto-Branch greift erst beim Schließen (nicht während des Drehens).
