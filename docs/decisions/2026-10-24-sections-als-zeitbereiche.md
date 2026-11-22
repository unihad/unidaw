# Sections als Zeitbereiche, Material über Section-Grenzen

- **Status:** gilt — umgesetzt mit den Merges f62007a und 2798421
- **Datum:** 2026-10-24, Matthias
- **Ersetzt:** aus `decision.2026-10-12-*` (ältere Decision, `README.md`) die Regel „Aufnahmen sind
  punch-bounded und stoppen am Sektionsende“.

## Kontext

Bisher begrenzte eine Section ihr Material: Einfügen kürzte am Section-Ende,
die Aufnahme stoppte dort, Section-Ops nahmen nur Clips mit, die in der
Section begannen. Aufnahmen und Clips, die länger als eine Section sind,
gingen so nicht.

## Entscheidung

- Sections bleiben Zeitbereiche ohne Lücken; Material (Audio, MIDI,
  Aufnahmen) und Automation dürfen über Section-Grenzen laufen.
- Die Aufnahme beginnt am Anfang der Section und läuft bis zum Stopp.
- Reicht Material hinter die letzte Section, entsteht automatisch eine neue
  Section (Auto-Section) bis zum Materialende.
- Section-Ops wirken auf den Zeitbereich: Löschen und Kürzen schneiden
  Material an der Grenze ab; Zeit einfügen (Section verlängern, davor
  einfügen) teilt einen Clip an der Einfügestelle; Verschieben und Kopieren
  nehmen die Clip-Teile und die Automation des Bereichs mit.
- treeCompact: Eine favorisierte Section schützt alle Clips, die sie
  berühren; ragt ein so geschützter Clip in eine nicht favorisierte Section,
  bleibt diese Section stehen (sie schützt dann nicht ihre anderen Clips).
- Die Auto-Section steht als Node-Aktion `addSection {auto: true}` im selben
  Node wie die auslösende Op.

## Folgen

- Details und Umsetzung: `ARCHITECTURE.md` („Sections als Zeitbereiche“),
  `todos/2026-10-24-sections-als-zeitbereiche.md`.
- Rezepte: „Automation am Ende der Ziel-Section abschneiden“ entfällt.
