# Clip-Leiste nur am gewählten Clip, Klick verzögert

- **Status:** umgesetzt (2026-10-31, Handprüfung
  `todos/2026-10-29-handpruefungen-matthias.md`, Flow-UI Punkt 2)
- **Datum:** 2026-10-31, Matthias

## Kontext

Seit [I] (`todos/2026-10-30-ui-knoepfe-sichtbar.md`) bleibt die Knopfleiste
eines Clips nach einem Klick stehen; sie erschien aber weiter auch beim
Überfahren mit der Maus (`.clip:hover .clbtns`). Die Leiste sitzt im Clip,
sobald er breiter ist als sie plus 30 px — bei einem 3,56-s-Clip ab
~84 px/s. Dann deckt sie fast den ganzen Clip (180 von 299 px Breite, 24
von 31 px Höhe).

Handprüfung: ein Doppelklick in „Crash 2“ setzte keinen Warp-Marker. Im
Nachstellen traf der Doppelklick einen Knopf: „→ MIDI“ lief zweimal los,
ein anderer Versuch rendert dreimal. Bei MIDI-Clips gilt dasselbe für den
Doppelklick zur Piano-Roll (Nachstell-Lauf stil-1 #4062/#4155: Q und ⧉).

## Entscheidung

- Die Leiste erscheint nur am gewählten Clip, nicht mehr beim Überfahren.
- Der Klick auf einen Clip wählt ihn ≈ 0,26 s verzögert (wie bei
  Sections): kommt ein zweiter Klick, ist es ein Doppelklick, und die
  Leiste erscheint gar nicht erst.
- Verworfen: Hover behalten und die Leiste nur bei deutlich breiteren
  Clips in den Clip setzen — bei breiten Clips hätte der Doppelklick sie
  weiter treffen können.

## Folgen

- Doppelklick auf einen nicht gewählten Clip trifft immer den Clip
  (Warp-Marker bzw. Piano-Roll).
- Wer einen Knopf will, klickt den Clip erst an; die Auswahl erscheint
  eine Viertelsekunde später.
- `docs/ui-checklist.md`, `ui_html`-Marker, `fehler_check` (Check „W“) und
  der Nachstell-Treiber (wartet nach dem Auswahl-Klick) ändern sich mit;
  der alte Weg `HOVER_CLIPS=1` im Treiber greift nicht mehr.
