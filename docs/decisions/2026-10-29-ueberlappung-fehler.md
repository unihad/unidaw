# Überlappender Clip ist ein Fehler, neue Lane nur auf Wunsch

- **Status:** gilt — umgesetzt im Branch `clip-fallen` (Todo
  `todos/2026-10-29-clip-fallen.md`)
- **Datum:** 2026-10-29, Matthias
- **Ersetzt:** aus `decision.2026-10-12-*` (ältere Decision, Aufnahme/Insert-
  Konventionen) die Regel „jeder Insert wird eine eigene Lane“ für
  `addClip`. Aufnahmen bleiben je Take eine eigene Lane.

## Kontext

`addClip` legte bei jedem Aufruf eine neue Lane an und schaltete die bisher
spielende stumm — ohne Meldung. `addMidiClip` tat es bei Überlappung, mit
einer Warnung am Rand. In Praxistest 2 (remix-1) war so die FX-Spur still:
ein zweites Sample, keine Überlappung. In Praxistest 3 (stil-1) war es die
ganze Basslinie: ein neuer Outro-Clip überlappte den alten, der noch nicht
gelöscht war.

## Entscheidung (Option A)

- Überlappt ein Clip (`addClip`, `addMidiClip`) einen Clip der Ziel-Lane,
  ist das ein Fehler, der den Clip nennt: „overlaps 'Bass Outro'
  (71.1-84.4 s) on Bass.1 — delete/trim it first, or newLane: true …“. Es
  entsteht nichts.
- Eine neue Lane gibt es nur für den ersten Clip einer Spur, mit
  `newLane: true` (ein Take) oder bei anderem Typ (MIDI-Datei auf einer
  Audio-Lane und umgekehrt). Die Antwort nennt die Lanes, die dabei stumm
  wurden (`mutedLanes`, `warning`).
- `addClip` geht wie `addMidiClip` in die spielende Lane (oder die als
  `track` genannte Lane), wenn dort Platz ist — zwei Samples auf einer Spur
  klingen beide.
- Die Flow-UI übergibt beim Insert `newLane: true`: ein Insert ist dort ein
  Take für das Comping.

## Folgen

- Agenten, die einen Clip ersetzen, löschen oder kürzen erst den alten
  (der Fehler sagt es).
- Tests, die mehrere Inserts auf einer Spur als Takes erwarten, übergeben
  `newLane: true`.
- Details: `ARCHITECTURE.md`, Aufnahme/Insert-Konventionen; MCP-Doku zu
  `addClip`/`addMidiClip`.
