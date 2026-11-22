# Konzept: Dock entzerren, Referenz-Bereich, Analyse-Panel (Flow-UI)

Stand 2026-10-29 · Todo `todos/2026-10-29-ui-konzept-dock-referenz.md` ·
**Entwurf — Matthias entscheidet, dann Bau.** Am echten UI
(`dawd/ui/flow.html`) ist nichts geändert.

Grundlage: UI-Nachstell-Lauf 1 (`$AGENT_DAW_RUNS/ui-replay-1/`, Lauf-Ordner nicht im Repo,
`BERICHT.md`, `TABELLEN.md`, je Lauf und Fenstergröße `records.jsonl`).
Skizzen: `docs/ui-konzept/` (statisches HTML, im Browser öffnen).
Alle Zahlen: `docs/ui-konzept/auswertung.md` (erzeugt, siehe „Nachrechnen“).

## Kurz

- **Das Dock kostet 87 % aller Mausrad-Rasten** (17.379 von 19.957 in
  1920×1080; 1366×768: 19.975 von 21.674). Grund: Routing mit allen offenen
  Plugin-Karten, Rezepte und Referenz stehen untereinander in einem Behälter.
- **Fünf Arbeitssets** gehen aus den Protokollen hervor (Abschnitt 2). Das
  größte: *Spur-Klang* = Spurkopf + Kette + Plugin-Karte + Voice/Drums +
  Ergebnis.
- **Drei Skizzen** (A Reiter, B Seitenleiste, C Referenz-Spur) bringen die
  Dock-Rasten im Modell auf 316–464 (−98 % gegen gemessen, −90 bis −92 %
  gegen das nachgebaute heutige Dock mit gleichem Inhalt). Verdeckte Dock-Ziele und
  „Ergebnis nicht im Bild“ fallen auf 0.
- **Empfehlung: Variante B (Seitenleiste „Reference + Analysis“ rechts).**
  Die wenigsten Reiter-Wechsel (80 statt 148), spart unterm Strich auch
  Klicks (−9), passt ohne Scrollen in 1366×768, keine Karte über der
  Timeline.
- **Den größten Teil des Gewinns bringen die gemeinsamen Bausteine**, egal
  welche Variante: Dock-Reiter statt Stapel, eine Plugin-Karte mit Suche,
  Referenz-Bereich mit Quelle (Stems ohne Upload), Analyse-Panel,
  Statuszeile statt Toast. Bau-Reihenfolge in Abschnitt 6.
- Entscheidungen für Matthias: Abschnitt 7 (fünf Fragen mit Optionen).

## 1 Methode und Grenzen

- **Element-Gruppen.** Jedes benutzte Bedienelement im Protokoll (Feld
  `desc` in `ui.elems`) ist einer Gruppe zugeordnet (Regeln in
  `tools/ui_replay/konzept.py`, `RULES`), z. B. „Dock: Plugin-Karte“,
  „Dock: Referenz Spur-Knöpfe“, „Timeline: Clips“. Keine Beschreibung blieb
  ohne Gruppe.
- **Rasten je Bereich.** Eine Op mit Elementen nur in einem Bereich zählt
  ganz dort. Bei gemischten Ops (z. B. Spurkopf + Voice) wird der
  Timeline-Anteil mit dem Mittel reiner Timeline-Ops derselben Gruppe
  geschätzt, der Rest zählt zum Dock.
- **Arbeitssets.** Arbeitsschritt = Node im Baum (`step`). Die Art des
  Schritts kommt aus dem Node-Label der Agenten („Bass: Klang“, „Drums:
  Noten“, „Song: Mix“ …). Je Art: welche Gruppen in wie vielen Schritten
  vorkommen und was sie an Rasten kosten.
- **Skizzen messen** (`tools/ui_replay/skizze_mess.js`, Playwright wie der
  Nachstell-Lauf): je Skizze und Fenstergröße Screenshots und die Lage jedes
  Bedienelements (`data-ctl`): Reiter, scrollender Behälter, Versatz darin,
  im Bild ohne Scrollen, verdeckt (Statuszeile an), Größe, in einem Menü.
  Alle Elemente der Varianten A–C sind ≥ 24 px, keines ist verdeckt.
- **Modell** (`konzept.py --model`): dieselbe Op-Folge der drei Läufe läuft
  gegen die Geometrie der Skizze. Je Dock-Element: Reiter-Wechsel (1 Klick,
  wenn ein anderer Reiter offen ist), Menü (1 Klick), Scrollen im Behälter
  nach derselben Regel wie `meter.js` (Ziel 40 px innen, 100 px je Raste,
  Scroll-Stand bleibt je Behälter). Parameter liegen je Name an einer festen
  Zeile der Karte (40 Zeilen). Timeline, Baum und Popover bleiben, wie
  gemessen.
- **Warum nicht das Nachstell-Werkzeug direkt gegen die Skizze:** die
  Treiber (`drivers.js`) brauchen ein laufendes unidaw und das echte DOM der
  Ops (Antworten, Ids, Zustand). Eine statische Skizze hat beides nicht.
  Darum: Geometrie der Skizze messen, Op-Folge aus den Protokollen.
- **Eichung.** Das heutige Dock ist in `docs/ui-konzept/v0-heute.html` mit
  demselben Inhalt wie die Skizzen nachgebaut (6 Spuren + Master, drei offene
  Karten, 8 Rezepte, Referenz). Das Modell ergibt dort 4.066 Dock-Rasten
  (1920) statt gemessen 17.379: im echten Lauf war mehr Inhalt im Dock (mehr
  Spuren und Ketten, viele offene Karten, alle Rezepte). Die Ersparnis ist
  darum zweimal angegeben: gegen gemessen und, vorsichtig, gegen den Nachbau
  mit gleichem Inhalt.
- **Grenzen.** Inhalt der Skizzen fest (6 Spuren, eine Karte mit 40
  Parametern). Bei 15+ Spuren braucht auch die Kettenliste in A–C Scrollen;
  Abhilfe: Spurkopf-Klick holt die Kette der Spur ins Bild. Die Strategie
  des „Menschen“ aus dem Nachstell-Lauf bleibt (Vorbehalte im Bericht).

## 2 Analyse: was zusammen gebraucht wird

### Wo das Scrollen liegt (alle drei Läufe)

| Bereich | 1920×1080 Ops | Rasten | Anteil | 1366×768 Rasten | Anteil |
|---|---|---|---|---|---|
| Dock (nur Dock-Elemente) | 306 | 13.530 | 68 % | 15.834 | 73 % |
| gemischt (Dock + Timeline/Kopf) | 92 | 4.109 | 21 % | 4.330 | 20 % |
| Timeline | 193 | 2.159 | 11 % | 1.325 | 6 % |
| Baum, Popover, Kopfleiste | 309 | 159 | 1 % | 185 | 1 % |

Teuerste Dock-Gruppen (1920, Rasten anteilig): Plugin-Karte 4.500 (100
Ops), Referenz Spur-Knöpfe 2.273 (66), Routing-Kette 2.200 (55),
Referenz Song-Knöpfe 1.880 (60), Mixer 1.637 (30), Referenz laden 1.306
(77), Rezepte 988 (36).

### Arbeitssets (1920×1080, Anteil der Schritte dieser Art)

| Art des Schritts | Schritte | Rasten | gebraucht zusammen |
|---|---|---|---|
| Spur-Klang | 92 | 7.817 | Plugin-Karte 36 %, Referenz Voice/Drums 21 %, Analyse 21 %, Kette 18 %, (Rezepte 13 %, Spurkopf 12 %, Referenz laden 12 %) |
| Noten | 85 | 3.743 | Timeline-Clips 32 %, Spurkopf 25 %, Zoom/Lineal 18 %, Piano-Roll 15 %, (Referenz laden 13 %, Voice 11 %) |
| Spur-Mix | 41 | 1.927 | Mixer 54 % |
| Summe / Song-Klang | 39 | 3.984 | Referenz laden 44 %, Plugin-Karte 41 %, Compare/Tempo/Key 41 %, Analyse 28 %, Kette 26 % |
| Referenz-Analyse (Start) | 14 | 948 | Referenz laden, Song-Knöpfe, Stems/To MIDI, Sections, Spurkopf (je 21–29 %) |
| Rezept anwenden | 6 | 959 | Rezepte 83 %, Spurkopf 50 %, Compare 50 % |
| Arrangement | 5 | 579 | Sections, Zoom (je 60 %) |

Daraus fünf Sets, die ohne Scrollen und Klicken zugleich sichtbar sein
sollen:

1. **Klang einer Spur:** Spurkopf (Ziel wählen) · Kette der Spur ·
   eine Plugin-Karte · Referenz-Quelle (meist ein Stem) · Voice/Drums ·
   letztes Ergebnis · Analyze der Spur.
2. **Klang der Summe:** Master-Kette · Karte · Compare · Analyze Song/
   Section · Ergebnis.
3. **Mix:** Mixer · Compare/Analyze.
4. **Noten:** Timeline-Clips · Piano-Roll · (Voice gegen den Stem).
5. **Referenz lesen:** Quelle · Bereich (Teile aus Structure) · Tempo/Key/
   Structure · Stems/To MIDI · Sections der eigenen Timeline.

Häufigste Paare im selben Schritt (ohne Baum/Kopfleiste): Plugin-Karte +
Kette (30 Schritte), Referenz laden + Voice/Drums (25), Referenz laden +
Compare (24), Voice/Drums + Spurkopf (24), Analyse + Plugin-Karte (17),
Compare + Plugin-Karte (16).

### Wo das heutige Dock Scrollen und Wechsel kostet

- **Stapel statt Nebeneinander:** Referenz ganz unten, darüber Rezepte und
  Routing mit allen offenen Karten. Jede Referenz-Op scrollt an Routing und
  Rezepten vorbei, jede Karte liegt unter allen Ketten
  (`befunde/dock-pluginkarten-1920.png`).
- **Modus-Klicks:** Routing, Rezepte, Referenz gibt es nur im Process-Modus
  — 39 Klicks auf „Process“ nur fürs Dock; 35 Umschalter Simple/Mixer.
- **Referenz laden 77×** (Dateidialog): 54 davon laden die Referenz selbst
  oder einen ihrer Stems neu, weil die UI nur eine Datei kennt.
- **Bereich nicht merkbar:** 149 Referenz-Ops tragen einen Bereich; derselbe
  kommt immer wieder (Nachbau 112,74–127,52 s ×47, Stil 13,31–53,27 ×18,
  Remix 58,624–79,94 ×11). Die UI kennt ihn nicht (Lücke oder Prompt).
- **Ergebnis weg:** 114 Ergebnisse stehen außerhalb des Bildes (unter der
  Referenz-Leiste), 96 nur im Toast (3,2 s).
- **Toast über dem Dock:** 39 (1920) bzw. 134 (1366) verdeckte Dock-Ziele.

## 3 Gemeinsame Bausteine (in allen drei Varianten)

- **Dock mit Reitern** statt Stapel: Routing · Mixer · Notes · Recipes
  (A zusätzlich: Reference & analysis). Reiter sind unabhängig vom Modus
  Collect/Select/Process (heute Fehler 8 im Bericht).
- **Eine Plugin-Karte zur Zeit**, neben der Kette (B, C) oder als Popover
  über der Timeline (A). In der Karte: Preset-Suche, **Parameter-Suche**,
  **Zahlenfeld** je Parameter (Bericht, Vorschlag 4), Hinweis „40 of 312
  parameters“.
- **Referenz-Bereich** (Abschnitt 5): Quelle + Bereich + BPM + erste Eins,
  einmal setzen, alle Referenz-Knöpfe nutzen ihn; als Chip immer sichtbar.
- **Analyse-Panel** statt Toast (Abschnitt 5).
- **Statuszeile unten** (eigene Zeile, 28 px) statt Toast: liegt über
  nichts. Ergebnisse gehen ins Analyse-Panel, die Statuszeile meldet nur.
- **Spurkopf-Klick setzt das Ziel** für Voice/Drums/Match/Tune und
  Rezept-Apply, auch auf MIDI-Spuren (Bericht, Fehler 4; Zielname steht an
  den Knöpfen: „Track Bass: Voice …“, „Apply to Bass“).

## 4 Varianten

Skizzen klickbar: Reiter, Ketten-Slot öffnet die Karte, Parameter-Suche,
Quelle und Teile setzen den Bereich, Referenz-Knöpfe legen ein Ergebnis ins
Panel, „Compare stems“ öffnet den Paar-Dialog, Spurkopf wählt das Ziel; in C
setzt Ziehen auf der Wellenform den Bereich. Werte sind erfundene Beispiele.

### A — Dock mit Reitern (`docs/ui-konzept/v1-reiter.html`)

- Dock wie heute unten (45 %), Reiter Routing · Mixer · Notes · Recipes ·
  **Reference & analysis**. Rechts in der Reiterleiste der Bereichs-Chip.
- Plugin-Karte als Popover über der Timeline (die Reiterleiste bleibt frei).
- Referenz-Reiter: links Quelle, Bereich, alle Knöpfe flach; rechts das
  Analyse-Panel.
- Fotos: `shots/v1-reiter-1920x1080.png`, `shots/v1-reiter-1366x768.png`,
  Referenz-Reiter `shots/v1-reiter-reference-1920x1080.png`,
  `shots/v1-reiter-reference-1366x768.png`.
- Plus: kleinster Umbau, Timeline volle Breite.
- Minus: Klang-Set braucht Wechsel Routing ↔ Reference (148 Reiter-Wechsel);
  Karten-Popover deckt Spurköpfe ab (27/31× im Modell wird danach die
  Timeline gebraucht); in 1366×768 passt die Zeile „Make“ nicht mehr
  (Scrollen im Reiter); Ergebnis nur sichtbar, solange der Referenz-Reiter
  offen ist.

### B — Seitenleiste „Reference + Analysis“ (`docs/ui-konzept/v2-seitenleiste.html`) — empfohlen

- Rechte Spalte (368 px; unter 1500 px Breite 320 px, Baum dann 240 px):
  oben Referenz (Chip, Quelle, Teile, Felder, Song- und Spur-Knöpfe direkt,
  Describe/Part/Make als Menü), darunter das Analyse-Panel bis unten.
- Dock unten (42 %) mit Reitern Routing · Mixer · Notes · Recipes; im
  Routing links die Ketten, rechts die Karte des gewählten Slots.
- Fotos: `shots/v2-seitenleiste-1920x1080.png`,
  `shots/v2-seitenleiste-1366x768.png`.
- Plus: Klang-Set und Summen-Set ganz in einem Blick (Spurkopf, Kette,
  Karte, Voice/Compare, Ergebnis); Referenz und Ergebnis in jedem Reiter und
  Modus sichtbar; passt in 1366×768 ohne Scrollen.
- Minus: Timeline schmaler (1366: 806 px statt 1062 px Breite); seltene
  Knöpfe hinter Menüs (32 Menü-Klicks über drei Läufe).

### C — Referenz-Spur über der Timeline (`docs/ui-konzept/v3-referenzspur.html`)

- Feste Spur oben: Kopf mit Datei und Quelle (Mix/Vox/Bass/Drm/Oth), daneben
  Chip, Compare, Voice, Drums direkt, Detect/Sound/Describe/Part/Make als
  Menüs, darunter die ganze Referenz als Wellenform mit ihren Teilen;
  **Bereich per Ziehen**, Klick nimmt einen Teil.
- Dock unten geteilt: links Reiter (Karte neben der Kette), rechts das
  Analyse-Panel.
- Fotos: `shots/v3-referenzspur-1920x1080.png`,
  `shots/v3-referenzspur-1366x768.png`.
- Plus: Bereich direkt am Material (Ziehen wie im Lineal); Timeline volle
  Breite.
- Minus: Timeline verliert ≈ 82 px Höhe (1366: ≈ 116 px, die Knöpfe brechen
  in zwei Zeilen); mehr Menüs (68 Menü-Klicks); Analyse-Panel nur so hoch wie
  das Dock.

## 5 Referenz-Bereich, Analyse-Panel, fehlende UI-Wege

### Referenz-Bereich einmal setzen

- **Ein Objekt:** Quelle · Start · Ende · BPM · erste Takt-Eins. Das ist
  genau die Form der API (`reference: {file, start, end, bpm,
  firstDownbeat}` bei compareVoice/compareDrums/matchSound/tuneSound/
  compareStems/compareSeparated; `file/start/end/bpm/firstDownbeat` bei
  detectKey, analyzeStructure, findRepeats, describeLayers, describe*,
  audioToMidi; `referenceStart/End` bei compareReference und
  compareClasses — MCP-Doku in `mcp/unidaw_mcp.py`).
- **Setzen:** Teil-Chips aus „Structure“ (ein Klick), Felder, „From
  section“ (Länge der gewählten Section), in C Ziehen auf der Wellenform.
  BPM und erste Eins kommen aus „Tempo“ und werden vorbelegt.
- **Quelle:** Mix · Vocals · Bass · Drums · Other (Stems der Referenz,
  ohne Upload; der Stem-Cache liegt schon unter
  `~/.config/unidaw/stems/<Referenz> <Bereich>/`, Beleg: compareStems-Parameter
  im Stil-Lauf) · Layer-Dateien aus Layers/Split · eigene Renders ·
  „Load file…“.
- **Alle Referenz-Knöpfe nutzen ihn**, keiner fragt mehr per Prompt
  (heute: Classes, Repeats, Layers, Match, Tune, Cross-check). Der Chip
  „Ref Bass 112.74–127.52 s · 130 BPM“ zeigt, womit gerechnet wird;
  „Whole“ = ganze Datei.
- **Wirkung (Modell, drei Läufe):** 149 Referenz-Ops mit Bereich brauchen
  64-mal „Bereich setzen“; 54 Dateidialoge entfallen, dafür 39 Klicks auf die
  Quelle; 23 Dateidialoge bleiben (fremde Dateien). Die Merklast „112.74
  sechsmal getippt“ entfällt.

### Analyse-Panel statt Toast

- Liste der Ergebnisse, neuestes oben und markiert, bleibt stehen (auch
  über Reiter und Modus hinweg); je Eintrag: was (Analyze, Compare, Voice …),
  Ziel und Bereich („track Bass · section Chorus“, „ref Bass
  112.74–127.52“), Kennwerte, Hinweis.
- Kopf: **Analyze** mit Ziel (Selected track · Song · Reference source ·
  File…) × Bereich (Selected section · Whole · Ruler selection · Reference
  range). Ersetzt den Umweg über das Export-Popover (32× im Modell) und den
  4 px hohen Spur-Meter als Knopf.
- Wirkung (Modell): Ergebnis nicht im Bild 114 → 0; 96 Ergebnisse, die heute
  nur 3,2 s im Toast stehen, bleiben im Panel.

### UI-Wege für Analyse-Ops ohne UI-Weg

| Op (Anzahl ohne UI-Weg, 1920) | Weg in den Skizzen | Parameter aus dem UI |
|---|---|---|
| describeVoice (9), describeTimbre (14), describeDrums (6) | „Describe“ (A: Zeile, B/C: Menü) mit Umschalter **Reference \| \<Spur\>** | Reference: `file` = Quelle, `start/end/bpm/firstDownbeat` = Bereich. Spur: `track` = gewählte Spur, `section` = gewählte Section (so 9× im Stil-Lauf) |
| compareStems (3) | „Compare stems“ (Make) → Dialog mit Paaren, aus den Namen vorgeschlagen (Vox ↔ Vocals …) | `pairs[{track, file}]` aus dem Dialog, `reference` = Bereich, `section` = gewählte Section |
| audioToMidi (1) | „Notes only“ (Make) neben „To MIDI“; Noten erscheinen im Analyse-Panel, dort „Add as clip“ | `file` = Quelle, Bereich, Modus (melody/bass/all/chords) |
| analyze mit Zeitfenster (22) | Analyze, Bereich „Ruler selection“ (Auswahl im Lineal ziehen) oder „Reference range“ | `start/end`, mit Ziel Spur dazu `track` |
| analyze mit Datei (13) | Analyze, Ziel „File…“ oder „Reference source“ (Renders und Layer-Dateien stehen in der Quellen-Liste) | `file`, optional `start/end` |
| compareVoice/compareDrums gegen einen Render (5) | Voice/Drums mit eigener Seite „File…“ statt Spur (Option im Knopf-Menü) | `file` statt `track` |

## 6 Schätzung je Variante

Modell über alle drei Läufe; „heute“ = gemessen im Nachstell-Lauf.

| | heute | A Reiter | B Seitenleiste | C Referenz-Spur |
|---|---|---|---|---|
| **1920×1080** Rasten gesamt | 19.957 | 2.894 (−85 %) | 2.951 (−85 %) | 2.959 (−85 %) |
| davon Dock | 17.379 (Nachbau gleicher Inhalt: 4.066) | 316 | 373 | 381 |
| neue Klicks: Reiter / Menü / Quelle / Karte aus dem Weg | — | 148 / 0 / 39 / 27 | 80 / 32 / 39 / 0 | 80 / 68 / 39 / 0 |
| gesparte Klicks: Modus / Simple-Mixer / Export-Popover / Dateidialoge | — | 39 / 35 / 32 / 54 | gleich | gleich |
| **Bilanz Klicks** (neu − gespart) | — | **+54** | **−9** | **+27** |
| verdeckte Dock-Ziele | 39 | 0 | 0 | 0 |
| Ergebnis nicht im Bild | 114 | 0 | 0 | 0 |
| Schritte ohne Aufdecken (von 266) | 31 | 46 | 50 | 50 |
| … dazu ✎ am Node immer sichtbar | — | 108 | 112 | 112 |
| **1366×768** Rasten gesamt | 21.674 | 2.102 (−90 %) | 2.163 (−90 %) | 2.163 (−90 %) |
| davon Dock | 19.975 (Nachbau: 4.443) | 403 | 464 | 464 |
| neue Klicks: Reiter / Menü / Quelle / Karte aus dem Weg | — | 144 / 0 / 39 / 31 | 77 / 32 / 39 / 0 | 77 / 68 / 39 / 0 |
| **Bilanz Klicks** | — | **+57** | **−9** | **+27** |
| verdeckte Dock-Ziele | 134 | 0 | 0 | 0 |
| Schritte ohne Aufdecken (von 265) | 30 | 40 | 49 | 49 |

- **Rest-Rasten im Dock** sind fast nur Parameter weiter unten in der Karte
  (98–107 Fälle). Mit der Parameter-Suche werden daraus je 1 Klick +
  ~5 Tasten statt Scrollen.
- **Schritte ohne Aufdecken** steigen mit dem Dock allein nur mäßig: der
  größte Rest ist Hover (✎ Notiz am Baum, 103 Schritte), dann Timeline-
  Scrollen (77) und Popover (Instrument, Clip, Tempo). Mit ✎ immer sichtbar
  am aktuellen Node (Bericht, Vorschlag 9) sind es 108–112 von 266.
- **Bereich setzen**: 64 Aktionen für 149 Ops (alle Varianten gleich).
- Die Zahlen zwischen B und C unterscheiden sich kaum; der Unterschied liegt
  in Menü-Klicks, Timeline-Höhe und darin, wo der Bereich gesetzt wird.

### Nachrechnen

```bash
NODE_PATH=~/dev/unspill/node_modules node tools/ui_replay/skizze_mess.js docs/ui-konzept/mess.json
python3 tools/ui_replay/konzept.py $AGENT_DAW_RUNS/ui-replay-1 --model docs/ui-konzept/mess.json > docs/ui-konzept/auswertung.md
```

## 7 Empfehlung und Bau-Reihenfolge

**Empfehlung: B.** Begründung:

- Die zwei teuersten Arbeitssets (Spur-Klang 7.817 Rasten, Summe 3.984)
  brauchen Karte, Referenz und Ergebnis zugleich. Nur B (und C) zeigen das
  ohne Wechsel; B mit weniger Menüs als C.
- Klick-Bilanz: B −9, C +27, A +54 — bei gleicher Rasten-Ersparnis.
- 1366×768: B passt ohne Scrollen; A muss im Referenz-Reiter scrollen, C
  kostet eine zweite Knopfzeile und Timeline-Höhe.
- Keine Karte über Spurköpfen (A: 27–31 Konflikte).
- Kosten von B: Timeline schmaler. Abhilfe: Seitenleiste einklappbar wie
  der Baum (`#sideBtn` gibt es schon für links).

**Bau in Schritten** (jeder für sich nützlich, jeder mit Prüfung über das
Nachstell-Werkzeug):

1. Statuszeile statt Toast; Spurkopf setzt das Ziel auch für MIDI-Spuren
   (Fehler 4, Todo `todos/2026-10-29-ui-fehler-nachstell-lauf.md`,
   parallel in Arbeit).
2. Dock-Reiter statt Stapel, unabhängig vom Modus; eine Karte zur Zeit mit
   Parameter-Suche und Zahlenfeld.
3. Analyse-Panel (Ergebnisse bleiben, Analyze mit Ziel × Bereich).
4. Referenz-Bereich mit Quelle (Stems, Layer-Dateien, Renders) und Chip;
   Prompts weg; Describe/Compare stems/Notes only.
5. Layout B (Seitenleiste) — oder die Variante, die Matthias wählt.

Schritte 1–4 sind in allen Varianten gleich. Danach den Nachstell-Lauf
wiederholen und gegen diese Schätzung prüfen.

## 8 Entscheidungen für Matthias

1. **Layout:** (a) A Reiter · (b) **B Seitenleiste** · (c) C Referenz-Spur.
   Empfehlung (b) — wenigste Wechsel, passt in 1366×768.
2. **Meldungen:** (a) **Statuszeile unten**, Ergebnisse ins Panel · (b) Toast
   bleibt, aber oben über der Timeline · (c) Toast wie heute, nur nicht über
   dem Dock. Empfehlung (a) — liegt über nichts; ein Toast oben mittig
   verdeckte im ersten Messlauf der Skizzen die Sections (A, B in 1366×768)
   bzw. die Referenz-Knöpfe (C).
3. **Wo der Referenz-Bereich lebt:** (a) **im Browser je Projekt** (wie die
   Referenz-Datei heute, `localStorage`) · (b) im Projekt über eine neue Op,
   Agent und UI teilen ihn · (c) nur bis zum Neuladen. Empfehlung (a) —
   kein API-Umbau; (b) später, wenn Agenten ihn lesen sollen.
4. **Plugin-Karten:** (a) **eine Karte zur Zeit** neben der Kette, mit Suche
   und Zahlenfeld · (b) mehrere offene Karten als Reiter · (c) wie heute alle
   untereinander, aber einklappbar. Empfehlung (a) — 30 Schritte brauchen
   Karte + Kette zusammen, kaum zwei Karten zugleich.
5. **Stems der Referenz als Quelle:** (a) **automatisch anbieten**, wenn sie
   im Stem-Cache liegen, sonst füllt „Stems“ sie · (b) nur nach Klick auf
   „Stems“ · (c) Upload wie heute. Empfehlung (a) — spart 54 Dateidialoge.
