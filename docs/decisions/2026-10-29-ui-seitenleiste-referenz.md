# Flow-UI: Seitenleiste für Referenz und Analyse, Dock mit Reitern

- **Status:** gilt — gebaut 2026-10-30 auf Branch `ui-seitenleiste`
  (Todo `todos/2026-10-29-ui-konzept-dock-referenz.md`, Agent Log mit
  Nachmessung)
- **Datum:** 2026-10-29, Matthias
- **Grundlage:** Konzept [`docs/ui-konzept-dock-referenz.md`](../ui-konzept-dock-referenz.md),
  Skizze `docs/ui-konzept/v2-seitenleiste.html` (Variante B), Messung aus
  dem UI-Nachstell-Lauf 1 (`tools/ui_replay/`).

## Kontext

Der UI-Nachstell-Lauf spielte drei Agent-Läufe im Flow-UI nach. Das Dock
kostete 87 % aller Mausrad-Rasten: Routing mit Plugin-Karten, Rezepte und
Referenz stehen untereinander, offene Karten bleiben offen. Nur 9–17 von
77–109 Schritten je Lauf gingen ohne Scrollen, Wechsel oder Hover. Ein
Toast verdeckte bei 1366×768 die Referenz-Knöpfe, Analyse-Ergebnisse
verschwanden nach 3,2 s, derselbe Referenz-Bereich wurde bis zu 47-mal
neu angegeben, 54 von 77 Dateidialogen luden nur die Referenz oder einen
ihrer Stems. Ziel (Matthias): Aktionen, die man zusammen macht, ohne
Scrollen oder Klicken gleichzeitig sichtbar.

## Entscheidung

1. **Layout Variante B:** rechte Spalte „Reference + Analysis“ — oben die
   Referenz (Bereichs-Chip, Quelle, Teile, Felder, Song- und Spur-Knöpfe,
   seltene Knöpfe in Menüs), darunter das Analyse-Panel bis unten. Das Dock
   unten bekommt Reiter (Routing · Mixer · Notes · Recipes), unabhängig vom
   Modus Collect/Select/Process.
2. **Meldungen in einer Statuszeile unten** statt Toast; Ergebnisse gehen
   ins Analyse-Panel, die Statuszeile meldet nur.
3. **Referenz-Bereich einmal setzen** (Quelle, Start/Ende, BPM, erste Eins);
   alle Referenz-Knöpfe nutzen ihn. Gespeichert im Browser je Projekt
   (localStorage), keine neue Op.
4. **Eine Plugin-Karte zur Zeit**, neben der Kette im Routing-Reiter, mit
   Preset-Suche, Parameter-Suche und Zahlenfeld je Parameter.
5. **Stems der Referenz automatisch als Quelle** (Mix | Vocals | Bass |
   Drums | Other) aus dem Stem-Cache; fehlen sie, trennt ein Klick auf einen
   Stem-Knopf (Job mit Fortschritt) — kein Upload.

Dazu aus dem Konzept: Spurkopf-Klick setzt das Ziel für Voice/Drums/Match/
Tune und Rezept-Apply auch auf MIDI-Spuren; UI-Wege für describeVoice,
describeTimbre, describeDrums, compareStems, audioToMidi und analyze mit
Bereich oder Datei im Analyse-Panel.

## Folgen

- Die Timeline wird schmaler (1366×768: 806 statt 1062 px).
- Geschätzt sinken die Dock-Rasten in 1920×1080 von 17.379 auf ~373,
  Klicks unterm Strich −9; verdeckte Dock-Ziele und „Ergebnis nicht im
  Bild“ auf 0 (Modell, Abschnitt 6 des Konzepts). Nach dem Bau mit
  `tools/ui_replay/` nachmessen.
- `docs/ui-checklist.md` bekommt die neuen Muster (Reiter, Statuszeile,
  Bereichs-Chip, Analyse-Panel).
