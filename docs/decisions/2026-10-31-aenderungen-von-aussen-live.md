# Flow-UI zeigt Änderungen von außen sofort

- **Status:** umgesetzt (2026-10-31, Handprüfung
  `todos/2026-10-29-handpruefungen-matthias.md`, OSC)
- **Datum:** 2026-10-31, Matthias

## Kontext

Das UI merkt am Positions-Poll (~120 ms), wenn sich die Version des
Projekts geändert hat, ohne dass es selbst etwas geschickt hat — ein Agent,
ein OSC-Controller oder ein Skript über die API. Bisher zeigte es dann nur
den Knopf „Refresh“; erst ein Klick zeichnete neu. Handprüfung: per OSC
gesendeter Fader und Mute kamen im Backend an (`trackMix`), im UI bewegte
sich nichts — bei einem Controller in der Hand erwartet man das live.

## Entscheidung

- Jede Änderung von außen zeichnet das UI sofort neu (Agent und OSC
  gleich); der Refresh-Knopf fällt weg.
- Nicht mitten in einer Bedienung: solange eine Maustaste gehalten ist
  (Ziehen), ein Feld den Fokus hat, ein Popover offen ist oder ein eigener
  Befehl läuft, wartet das Neuzeichnen bis zum nächsten Poll danach.
- Verworfen: nur OSC live, Agent weiter über den Knopf; so lassen.

## Folgen

- Arbeitet ein Agent, bewegt sich das UI mit seinen Schritten.
- Eigene Befehle ohne Versions-Abgleich (Schieberegler beim Ziehen) lösen
  nach dem Loslassen einmal ein Neuzeichnen aus — das UI zeigt danach den
  Stand des Backends.
- `docs/ui-checklist.md` (Zustand) und die `ui_html`-Marker ändern sich mit.
