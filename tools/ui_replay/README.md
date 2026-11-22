# UI-Nachstell-Lauf (`tools/ui_replay/`)

Spielt die Op-Folge eines Agent-Laufs über das Flow-UI nach (Playwright,
headless Chromium) und misst den Bedienaufwand. Nicht Teil der Suite.
Todo: `todos/2026-10-29-ui-nachstell-lauf.md`.

## Ablauf

1. **Protokolle ziehen** (streamend, nur die Zeitfenster in `extract.py`):

   ```bash
   python3 tools/ui_replay/extract.py ~/dev/agent-daw/events.jsonl <ops-ordner>
   ```

   Schreibt `<ops-ordner>/<lauf>.ops.jsonl` (seq, ts, op, params, result).

2. **Vorlage für `--config`** anlegen (einmal): `Settings.xml` (Plugin-Liste,
   sonst fehlen Surge & Co.), `libraries.json`, `index/`, `recipes/` aus
   `~/.config/unidaw`, dazu `recipes-repo/` (Kopie von `recipes/` im Repo — die
   Instanz schreibt dort, nicht ins Repo).

3. **Ein Lauf in einer Fenstergröße**:

   ```bash
   OPS_DIR=<ops-ordner> BASE_CONFIG=<vorlage> WORK=<scratch> \
   RESULTS=~/dev/agent-daw-runs/ui-replay-1 \
     tools/ui_replay/run.sh remix-1 1920x1080 8100
   ```

   `run.sh` startet eine frische unidaw-Instanz (eigener Port, Kopie der
   Vorlage als `--config`, `--interface-sim --no-audio`, Flow-UI dieses
   Checkouts oder `UI_FILE`), ruft `replay.js` auf und beendet die Instanz.
   Das Binary kommt aus `DAWD` (Standard: main-Build). Optionen für
   `replay.js`: `--limit N`, `--from SEQ`, `--only SEQ,SEQ,…` (nur diese Ops
   über das UI, alle anderen schnell per API — Lese-Ops und lange Jobs
   fallen dabei weg; für die Gegenprobe einzelner Befunde, vorher/nachher
   mit `UI_FILE` auf die alte Fassung). `NODE_PATH` muss Playwright
   enthalten (Standard: `~/dev/unspill/node_modules`).

4. **Tabellen**: `python3 tools/ui_replay/report.py <RESULTS>` (Markdown).

## Was je Op passiert

- `replay.js`: liest die Ops, bildet Schritte (Node im Baum: Auto-Node,
  Branch, Checkout; Notizen beim genannten Node), schreibt Pfade und Ids um
  (Projekt → Ergebnisordner, Node-/Modul-Ids aus den Antworten), führt den
  UI-Weg aus und gleicht danach ab:
  - ein Befehl mit gleichen Parametern → „same request“;
  - sonst Zustand des UI-Wegs merken, per `undo` zurück, die Op per API
    ausführen, Fingerabdruck vergleichen → „same state“ oder „differs“
    (der Lauf geht immer mit dem API-Ergebnis weiter);
  - Plugin-Parameter prüft `verify` extra (der Fingerabdruck kennt sie nicht).
- `drivers.js`: UI-Weg je Op (Selektoren, Klickfolge, Eingaben) — seit
  2026-10-30 für Layout Variante B (Dock-Reiter, eine Plugin-Karte mit
  Suche und Zahlenfeld, Referenz + Analyse rechts: Referenz-Auswahl, Quelle
  Mix/Stems — Stems des Agenten unter `<config>/stems/<Referenz>[ a-b]/`
  werden zur Stem-Quelle der Referenz mit um a verschobenem Bereich —,
  Bereich/Grid einmal setzen, Menüs Describe/Part/Make, Analyze im Panel);
  für die Fassung davor `git show a7f074e:tools/ui_replay/drivers.js`. Kein Weg →
  `NoUi(Grund)` → API (markiert). Fehlende Parameter → `rec.partial`.
  Lange Jobs (Stems, matchSound, tuneSound, Klassen, Layers) bekommen das
  Original-Ergebnis aus dem Protokoll (Abkürzung, Rückmeldung wird geprüft).
  Schreibziele des Agenten außerhalb des Projekts landen unter
  `<ergebnis>/extern/` — die Original-Läufe bleiben unberührt.
- `meter.js`: misst am echten DOM — Klicks, Doppelklicks, Ziehen, Tasten
  (Freitext getrennt), Scrollen (Mausrad-Rasten à 100 px), Mausweg,
  Ansichtswechsel, Hover-only, Ziele < 24 px, verdeckte Ziele, Dateidialoge,
  Prompts. Ziehen wie eine Hand: ganze Pixel; Timeline herauszoomen, bis der
  Weg passt, zum Ablegen hineinzoomen; `drag` kann Tasten halten
  (`modifiers`, gezählt wie bei `click`).
- Clip-Lage (seit 2026-10-30, `drivers.js`): Ziel auf einem Raster (Takt,
  1/4, 1/8, 1/16; `gridOf` nimmt das gröbste) → Ziehen mit Einrasten (Zoom
  nur, bis das UI auf dieses Raster rastet — `CLIP_SNAP_PX` je Schritt),
  `duplicateClip` an eine Stelle per Ctrl + Ziehen; Ziel daneben → Felder im
  Clip-Popover (Start s, Length s, Copy at s), die linke Kante mit Shift
  (frei, ≥ 100 px/s). `startBeat` zählt wie im Backend ab der Section.
  Clip-Namen ohne die Marke „+N“ davor (sonst „drag handle gone“).
- Seit 2026-10-30 ohne Modus-Umschalter: kein Klick auf Collect/Select/
  Process mehr (`ensureProject` prüft nur das offene Projekt);
  `favoriteClip` über den Stern im Clip-Popover, `favoriteSection` über den
  Stern an der Section.
- Seit 2026-10-30 Knöpfe ohne Hover: Clip-Knöpfe nach Klick auf den Clip
  (Auswahl, Punkt im sichtbaren Teil von links her — rechts erscheint die
  klebende Leiste) statt Hover, ohne Weg zum Clip-Ende; `HOVER_CLIPS=1` =
  alter Weg. Node-Knöpfe über „⋯“, wenn es ihn gibt (ältere UI: Hover).
- Seit 2026-10-31 Export frei: Tail als Zahl, Ziel über das Feld „File“
  (relativ zum Projekt, ohne Endung; Stems/Batch: der Ordner), Batch-Nodes
  über „Pick nodes…“. Doppelklick zum Bearbeiten eines MIDI-Clips an einer
  freien Stelle (nicht auf der Knopfleiste); `addMidiClip` am Anfang einer
  Section per Klick auf die Section (der Cursor rastet auf 1/20 s).

## Gegenprobe der UI-Fehler (`fehler_check.sh`)

`tools/ui_replay/fehler_check.sh <port>` startet eine eigene Instanz (frische
`--config`, Song im tmpfs), baut per API einen kleinen Song (Audio-Spur mit
gewarptem Clip, Marker an Anfang und Ende, Plugin, MIDI-Spur mit 4OSC, ein
Clip mit Kopie an derselben Stelle) und bedient das UI wie eine Hand: Spur-
und Modulnamen per Doppelklick (10 Versuche), Namen mit Leerzeichen/R/M,
Voice/Drums/Match/Tune/Apply nach Klick auf MIDI- bzw. Audio-Spurkopf
(Anfragen abgefangen, nichts gerechnet), Ziehen an den Kanten des gewarpten
Clips (oben/Mitte/unten), den verdeckten Clip wählen, trimmen und kopieren (⧉);
vorneweg die Dock-Höhe (Checks „D“: Kante ziehen, Neuladen, Grenzen,
Einklappen per Reiter, Doppelklick), am Ende Clips genau setzen (Checks „C“:
Einrasten beim Verschieben und an beiden Kanten, Shift frei, Ctrl-Kopie,
Felder Start/Length/Copy at, Popover im Bild bei 560 px Höhe, Clip unter
zwei Nachbarn, Kopie eines verdeckten Clips ziehen; Raster je Zoom mit
Label, Felder mit „bar 3.2“ über zwei Sections mit eigenem Tempo/Takt,
unlesbare Eingabe), zuletzt ohne Modus-Umschalter (Checks „M“: alle
Knöpfe da, Stern im Clip-Popover und an der Section, Record im
process-Node mit Tooltip nach Auto-Branch, `record` abgefangen), ganz am
Ende Knöpfe ohne Hover (Checks „K“: langer Clip gewählt, Maus weg —
Leiste im Bild, ⧉ per Klick; aktueller Node mit ✎, „⋯“ an den anderen,
Klick öffnet, Klick daneben schließt; Spurkopf gefroren, in einer Gruppe
und Gruppen-Kopf ohne Abschnitt, alle ≥ 24 px), danach die Piano-Roll
(Checks „P“: Tonumfang 0–127 mit Start um C4 bzw. ab 36 auf „Drums“,
Noten 36–96 mit Velocity aus dem Feld und Name „Kick A“ anlegen,
Velocity-Spur ziehen und per Feld genau setzen, Section mit 48 s; mit
`--shots` Bilder `piano-*-<BxH>.png`), dann der Export (Checks „E“, Render-
Ops abgefangen: Tail 1,5 s, Feld „File“ relativ zum Projekt und leer,
Batch „Pick nodes…“ und Ordner, Popover im Bild), zuletzt ohne Projekt
(Checks „R“: Start-Panel neben Referenz + Analyse, Load nach
`<config>/references/`, Tempo und Analyze laufen, Projekt-Knöpfe aus, das
danach geöffnete Projekt übernimmt die Referenz). Ausgabe je
Check ok/FEHLT mit Zahl; Exit 1, wenn einer fehlt. `UI_FILE=<alte flow.html>`
zeigt den Stand vorher — als Kopie neben einem Link `vendor` →
`dawd/ui/vendor` (unidaw liefert `/vendor/` aus dem Ordner der UI-Datei;
fehlt er, liegt der Ordner-Dialog ungestylt unter der Seite und D/M
scheitern). Todo: `todos/2026-10-29-ui-fehler-nachstell-lauf.md`.

## Platz

Ein Lauf braucht bis zu 2 GB (das UI lädt jede eingefügte Datei und jede
Referenz nach `media/` hoch; Warps und Renders kommen dazu). `RESULTS` am
besten auf ein tmpfs legen und danach nur `records.jsonl`, `replay.log`,
`shots/`, Baum und Edits aufheben (ohne `media`, `stems*`, `renders`,
`exports`, `extern`, `*.wav`). Am 2026-10-29 lief `/` dabei voll.

## Grenzen

- Der Dateidialog des Betriebssystems und der Farbdialog zählen als je ein
  „Dateidialog“, nicht als Klicks.
- Merklast ist eine Heuristik: eine getippte Zahl mit Nachkommastelle (oder
  ≥ 3 Stellen), die vorher in einer Antwort einer Lese-Op stand.
- Ergebnisse: `<RESULTS>/<lauf>-<BxH>/records.jsonl`, `shots/`,
  `replay.log`, `events.jsonl` der Instanz.

## Konzept Dock/Referenz (Auswertung und Skizzen)

- `konzept.py <RESULTS> [--model mess.json]`: Element-Gruppen und Bereiche
  je Bedienelement, Rasten je Bereich, Arbeitssets je Art des Schritts,
  Referenz-Bereiche; mit `--model` dieselbe Op-Folge gegen die Geometrie der
  Skizzen (Reiter-Wechsel, Menüs, Scrollen, verdeckt, Ergebnis im Bild).
- `skizze_mess.js <out.json>`: misst die statischen Skizzen in
  `docs/ui-konzept/` (Playwright) und macht die Screenshots.
- Ergebnis und Methode: `docs/ui-konzept-dock-referenz.md`.
