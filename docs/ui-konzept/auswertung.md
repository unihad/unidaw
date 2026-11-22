
## 1920x1080: Rasten je Bereich (UI-Ops mit Elementen)

| Bereich | Ops | Rasten | Anteil | ⌀ je Op |
|---|---|---|---|---|
| dock | 306 | 13530 | 68 % | 44 |
| gemischt | 92 | 4109 | 21 % | 45 |
| tl | 193 | 2159 | 11 % | 11 |
| tree | 209 | 132 | 1 % | 1 |
| pop | 85 | 27 | 0 % | 0 |
| top | 15 | 0 | 0 % | 0 |
| Summe | 900 | 19957 | | |

### 1920x1080: Gruppen (Rasten anteilig, wenn eine Op mehrere Gruppen hat)

| Gruppe | Ops | Rasten | Schritte mit Gruppe |
|---|---|---|---|
| Dock: Plugin-Karte (Parameter, Presets, Samples) | 100 | 4500 | 61 von 266 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | 66 | 2273 | 38 von 266 |
| Dock: Routing-Kette (Plugin wählen, + Plugin) | 55 | 2200 | 37 von 266 |
| Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | 60 | 1880 | 34 von 266 |
| Dock: Mixer | 30 | 1637 | 27 von 266 |
| Dock: Referenz laden | 77 | 1306 | 46 von 266 |
| Timeline: Clips und Clip-Popover | 152 | 1136 | 36 von 266 |
| Dock: Rezepte | 36 | 988 | 22 von 266 |
| Timeline: Zoom, Lineal | 61 | 839 | 21 von 266 |
| Kopfleiste (Modus, Play, Undo, Einfügen) | 60 | 776 | 55 von 266 |
| Timeline: Sections | 47 | 702 | 24 von 266 |
| Timeline: Spurkopf, neue Spur | 49 | 568 | 45 von 266 |
| Dock: Piano-Roll | 23 | 421 | 15 von 266 |
| Dock: Referenz Teil-Knöpfe (Repeats, Layers, Classes …) | 9 | 292 | 8 von 266 |
| Dock: Referenz erzeugt (To MIDI, Stems) | 17 | 185 | 13 von 266 |
| Baum (Node, Notiz, Fork, Terminate) | 213 | 148 | 157 von 266 |
| Timeline: Automation, LFO | 13 | 64 | 9 von 266 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | 62 | 27 | 38 von 266 |
| Projekt-Dialog | 5 | 16 | 5 von 266 |
| Export-Popover | 54 | 0 | 38 von 266 |

### 1920x1080: Paare im selben Arbeitsschritt (Node) — Top 25

| Gruppe A | Gruppe B | Schritte | Übergänge Op→Op |
|---|---|---|---|
| Kopfleiste (Modus, Play, Undo, Einfügen) | Baum (Node, Notiz, Fork, Terminate) | 32 | 26 |
| Dock: Referenz laden | Baum (Node, Notiz, Fork, Terminate) | 30 | 30 |
| Dock: Plugin-Karte (Parameter, Presets, Samples) | Dock: Routing-Kette (Plugin wählen, + Plugin) | 30 | 27 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | Export-Popover | 28 | 18 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | Baum (Node, Notiz, Fork, Terminate) | 26 | 28 |
| Timeline: Spurkopf, neue Spur | Kopfleiste (Modus, Play, Undo, Einfügen) | 25 | 4 |
| Dock: Referenz laden | Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | 25 | 7 |
| Timeline: Spurkopf, neue Spur | Baum (Node, Notiz, Fork, Terminate) | 25 | 16 |
| Dock: Referenz laden | Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | 24 | 28 |
| Dock: Referenz laden | Timeline: Spurkopf, neue Spur | 24 | 5 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | Timeline: Spurkopf, neue Spur | 24 | 9 |
| Dock: Plugin-Karte (Parameter, Presets, Samples) | Baum (Node, Notiz, Fork, Terminate) | 24 | 9 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | Kopfleiste (Modus, Play, Undo, Einfügen) | 23 | 11 |
| Dock: Plugin-Karte (Parameter, Presets, Samples) | Kopfleiste (Modus, Play, Undo, Einfügen) | 22 | 6 |
| Export-Popover | Baum (Node, Notiz, Fork, Terminate) | 22 | 24 |
| Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | Baum (Node, Notiz, Fork, Terminate) | 21 | 19 |
| Dock: Referenz laden | Kopfleiste (Modus, Play, Undo, Einfügen) | 20 | 5 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | Baum (Node, Notiz, Fork, Terminate) | 20 | 18 |
| Timeline: Clips und Clip-Popover | Timeline: Zoom, Lineal | 18 | 78 |
| Dock: Rezepte | Baum (Node, Notiz, Fork, Terminate) | 17 | 11 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | Dock: Plugin-Karte (Parameter, Presets, Samples) | 17 | 19 |
| Dock: Rezepte | Kopfleiste (Modus, Play, Undo, Einfügen) | 16 | 4 |
| Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | Dock: Plugin-Karte (Parameter, Presets, Samples) | 16 | 13 |
| Timeline: Sections | Baum (Node, Notiz, Fork, Terminate) | 15 | 12 |
| Dock: Referenz laden | Dock: Plugin-Karte (Parameter, Presets, Samples) | 15 | 9 |

### 1920x1080: Verdeckte Ziele nach Gruppe

Timeline: Clips und Clip-Popover: 23, Dock: Plugin-Karte (Parameter, Presets, Samples): 15, Dock: Piano-Roll: 13, Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune): 8, Dock: Referenz Teil-Knöpfe (Repeats, Layers, Classes …): 3

### 1920x1080: Ergebnis außerhalb des Bildes / nur Toast

- compareVoice — #refVoice (below the reference bar): 49
- compareReference — #refResult (below the reference bar): 27
- compareDrums — #refVoice (below the reference bar): 9
- analyzeStructure — #refStruct (below the reference bar): 8
- matchSound — #refVoice (below the reference bar): 5
- detectKey — #refKey (below the reference bar): 4
- detectTempo — #refTempo (below the reference bar): 4
- tuneSound — #refVoice (below the reference bar): 3
- analyze — toast (3.2 s): 2
- findRepeats — #refVoice (below the reference bar): 1
- describeLayers — #refVoice (below the reference bar): 1
- render — toast (3.2 s): 1

Prompts je Op: compareClasses 6, findRepeats 2, describeLayers 2

## 1366x768: Rasten je Bereich (UI-Ops mit Elementen)

| Bereich | Ops | Rasten | Anteil | ⌀ je Op |
|---|---|---|---|---|
| dock | 307 | 15834 | 73 % | 52 |
| gemischt | 90 | 4330 | 20 % | 48 |
| tl | 200 | 1325 | 6 % | 7 |
| tree | 208 | 129 | 1 % | 1 |
| pop | 85 | 56 | 0 % | 1 |
| top | 15 | 0 | 0 % | 0 |
| Summe | 905 | 21674 | | |

### 1366x768: Gruppen (Rasten anteilig, wenn eine Op mehrere Gruppen hat)

| Gruppe | Ops | Rasten | Schritte mit Gruppe |
|---|---|---|---|
| Dock: Plugin-Karte (Parameter, Presets, Samples) | 101 | 5300 | 62 von 265 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | 66 | 2601 | 38 von 265 |
| Dock: Routing-Kette (Plugin wählen, + Plugin) | 55 | 2497 | 37 von 265 |
| Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | 60 | 2128 | 34 von 265 |
| Dock: Mixer | 30 | 1828 | 27 von 265 |
| Dock: Referenz laden | 77 | 1466 | 46 von 265 |
| Dock: Rezepte | 36 | 1159 | 22 von 265 |
| Timeline: Clips und Clip-Popover | 149 | 902 | 36 von 265 |
| Kopfleiste (Modus, Play, Undo, Einfügen) | 57 | 805 | 53 von 265 |
| Timeline: Spurkopf, neue Spur | 52 | 614 | 44 von 265 |
| Timeline: Zoom, Lineal | 66 | 530 | 16 von 265 |
| Dock: Piano-Roll | 21 | 514 | 13 von 265 |
| Timeline: Sections | 47 | 470 | 24 von 265 |
| Dock: Referenz Teil-Knöpfe (Repeats, Layers, Classes …) | 9 | 351 | 8 von 265 |
| Dock: Referenz erzeugt (To MIDI, Stems) | 17 | 230 | 13 von 265 |
| Baum (Node, Notiz, Fork, Terminate) | 212 | 151 | 156 von 265 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | 62 | 56 | 38 von 265 |
| Timeline: Automation, LFO | 18 | 49 | 11 von 265 |
| Projekt-Dialog | 5 | 22 | 5 von 265 |
| Export-Popover | 54 | 0 | 38 von 265 |

### 1366x768: Paare im selben Arbeitsschritt (Node) — Top 25

| Gruppe A | Gruppe B | Schritte | Übergänge Op→Op |
|---|---|---|---|
| Kopfleiste (Modus, Play, Undo, Einfügen) | Baum (Node, Notiz, Fork, Terminate) | 31 | 26 |
| Dock: Referenz laden | Baum (Node, Notiz, Fork, Terminate) | 31 | 29 |
| Dock: Plugin-Karte (Parameter, Presets, Samples) | Dock: Routing-Kette (Plugin wählen, + Plugin) | 31 | 28 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | Export-Popover | 28 | 16 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | Baum (Node, Notiz, Fork, Terminate) | 27 | 29 |
| Timeline: Spurkopf, neue Spur | Kopfleiste (Modus, Play, Undo, Einfügen) | 25 | 4 |
| Dock: Referenz laden | Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | 25 | 7 |
| Timeline: Spurkopf, neue Spur | Baum (Node, Notiz, Fork, Terminate) | 25 | 18 |
| Dock: Referenz laden | Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | 24 | 28 |
| Dock: Referenz laden | Timeline: Spurkopf, neue Spur | 23 | 6 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | Timeline: Spurkopf, neue Spur | 23 | 13 |
| Dock: Plugin-Karte (Parameter, Presets, Samples) | Baum (Node, Notiz, Fork, Terminate) | 23 | 9 |
| Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) | Kopfleiste (Modus, Play, Undo, Einfügen) | 22 | 11 |
| Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | Baum (Node, Notiz, Fork, Terminate) | 21 | 18 |
| Dock: Plugin-Karte (Parameter, Presets, Samples) | Kopfleiste (Modus, Play, Undo, Einfügen) | 21 | 5 |
| Export-Popover | Baum (Node, Notiz, Fork, Terminate) | 21 | 23 |
| Dock: Referenz laden | Kopfleiste (Modus, Play, Undo, Einfügen) | 19 | 5 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | Baum (Node, Notiz, Fork, Terminate) | 19 | 17 |
| Dock: Rezepte | Baum (Node, Notiz, Fork, Terminate) | 17 | 11 |
| Analyse (Spur-Meter, Export-Popover → Analyze) | Dock: Plugin-Karte (Parameter, Presets, Samples) | 17 | 19 |
| Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) | Dock: Plugin-Karte (Parameter, Presets, Samples) | 16 | 13 |
| Timeline: Sections | Baum (Node, Notiz, Fork, Terminate) | 15 | 11 |
| Dock: Rezepte | Kopfleiste (Modus, Play, Undo, Einfügen) | 15 | 4 |
| Dock: Referenz laden | Dock: Plugin-Karte (Parameter, Presets, Samples) | 15 | 9 |
| Timeline: Clips und Clip-Popover | Timeline: Zoom, Lineal | 15 | 95 |

### 1366x768: Verdeckte Ziele nach Gruppe

Dock: Piano-Roll: 44, Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune): 37, Dock: Plugin-Karte (Parameter, Presets, Samples): 30, Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure): 19, Timeline: Clips und Clip-Popover: 13, Timeline: Automation, LFO: 7, Timeline: Spurkopf, neue Spur: 6, Dock: Routing-Kette (Plugin wählen, + Plugin): 2, Dock: Referenz erzeugt (To MIDI, Stems): 1, Dock: Rezepte: 1, Timeline: Sections: 1

### 1366x768: Ergebnis außerhalb des Bildes / nur Toast

- compareVoice — #refVoice (below the reference bar): 49
- compareReference — #refResult (below the reference bar): 27
- compareDrums — #refVoice (below the reference bar): 9
- analyzeStructure — #refStruct (below the reference bar): 8
- matchSound — #refVoice (below the reference bar): 5
- detectKey — #refKey (below the reference bar): 4
- detectTempo — #refTempo (below the reference bar): 4
- tuneSound — #refVoice (below the reference bar): 3
- analyze — toast (3.2 s): 2
- findRepeats — #refVoice (below the reference bar): 1
- render — toast (3.2 s): 1

Prompts je Op: compareClasses 12, findRepeats 2, describeLayers 2

## Arbeitsschritt-Arten (1920x1080): welche Gruppen zusammen gebraucht werden

Gruppen in mindestens 15 % der Schritte dieser Art (ohne Baum und Kopfleiste); in Klammern Rasten.

| Art | Schritte | Ops | Rasten | Gruppen (Anteil der Schritte, Rasten) |
|---|---|---|---|---|
| Spur-Klang | 92 | 566 | 7817 | Dock: Plugin-Karte (Parameter, Presets, Samples) 36 % (2466); Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) 21 % (1843); Analyse (Spur-Meter, Export-Popover → Analyze) 21 % (20); Dock: Routing-Kette (Plugin wählen, + Plugin) 18 % (968); Export-Popover 17 % (0) |
| Noten | 85 | 362 | 3743 | Timeline: Clips und Clip-Popover 32 % (935); Timeline: Spurkopf, neue Spur 25 % (238); Timeline: Zoom, Lineal 18 % (506); Dock: Piano-Roll 15 % (342) |
| Spur-Mix | 41 | 66 | 1927 | Dock: Mixer 54 % (1190) |
| Summe / Song-Klang | 39 | 205 | 3984 | Dock: Referenz laden 44 % (540); Dock: Plugin-Karte (Parameter, Presets, Samples) 41 % (1526); Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) 41 % (878); Export-Popover 31 % (0); Analyse (Spur-Meter, Export-Popover → Analyze) 28 % (7); Dock: Routing-Kette (Plugin wählen, + Plugin) 26 % (578); Timeline: Spurkopf, neue Spur 15 % (45); Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) 15 % (39) |
| Referenz-Analyse (Start) | 14 | 100 | 948 | Projekt-Dialog 29 % (16); Dock: Referenz laden 29 % (74); Timeline: Spurkopf, neue Spur 29 % (35); Dock: Referenz erzeugt (To MIDI, Stems) 21 % (84); Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) 21 % (72); Timeline: Sections 21 % (15); Timeline: Clips und Clip-Popover 21 % (15) |
| Rezept anwenden | 6 | 78 | 959 | Dock: Rezepte 83 % (132); Timeline: Spurkopf, neue Spur 50 % (37); Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure) 50 % (98); Dock: Referenz laden 33 % (129); Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune) 33 % (60); Dock: Plugin-Karte (Parameter, Presets, Samples) 33 % (80); Timeline: Sections 33 % (75); Dock: Referenz Teil-Knöpfe (Repeats, Layers, Classes …) 17 % (0); Analyse (Spur-Meter, Export-Popover → Analyze) 17 % (0); Dock: Mixer 17 % (142); Dock: Routing-Kette (Plugin wählen, + Plugin) 17 % (140) |
| Arrangement | 5 | 35 | 579 | Timeline: Sections 60 % (333); Timeline: Zoom, Lineal 60 % (229); Timeline: Clips und Clip-Popover 20 % (17) |

## Referenz-Bereiche je Lauf (1920x1080): wie oft derselbe Bereich

| Lauf | Referenz-Ops | ohne Bereich (ganze Datei) | häufigste Bereiche (s) |
|---|---|---|---|
| nachbau-14 | 92 | 4 | 112.74–127.52 ×47, 3.95–18.72 ×20, 12.74–27.52 ×10 |
| remix-1 | 35 | 10 | 58.624–79.94 ×11, 0–32 ×6, 56–82 ×2 |
| stil-1 | 79 | 43 | 13.31–53.27 ×18, 119.87–159.83 ×9, 44.44–71.11 ×3 |

## Eichung: Modell gegen das nachgebaute heutige Dock (v0-heute, gleicher Inhalt wie die Skizzen)

| Größe | Dock-Rasten gemessen (echtes UI) | Modell gegen v0-heute | Rest nach Element |
|---|---|---|---|
| 1920x1080 | 17379 | 4066 | route.card 1226, route.add 661, route.chain 583, analysis.analyze 404, ref:Voice 336, ref.source 292, ref:Compare 189, route.preset 116, ref:Classes 63, recipe.apply 61, ref.file 47, recipe.search 44, ref:Drums 23, ref:Stems 11, ref:Structure 4, ref:To MIDI 2, ref:Tempo 2, ref:Key 2 |
| 1366x768 | 19975 | 4443 | route.card 1405, route.add 688, route.chain 634, analysis.analyze 421, ref:Voice 361, ref.source 310, ref:Compare 191, route.preset 124, recipe.apply 89, ref:Classes 64, ref.file 54, recipe.search 50, ref:Drums 29, ref:Stems 12, ref:Structure 4, ref:Key 3, ref:To MIDI 2, ref:Tempo 2 |

## Modell: dieselbe Op-Folge gegen die Skizzen (alle drei Läufe)

| Größe | Skizze | Rasten gesamt heute → Skizze | davon Dock heute → Skizze | Reiter-Wechsel | Menü-Klicks | Quellen-Klicks | gesparte Modus-/Routing-/Export-Klicks | Dateidialoge gespart | verdeckte Dock-Ziele heute → Skizze | Ergebnis nicht im Bild heute → Skizze | Schritte ohne Aufdecken heute → Skizze | Bereich setzen (Ops mit Bereich) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1920x1080 | v1-reiter | 19957 → 2894 | 17379 → 316 | 148 | 0 | 39 | 106 (39/35/32) | 54 (bleiben 23) | 39 → 0 | 114 → 0 | 31 → 46 von 266 | 64 (149) |
| 1920x1080 | v2-seitenleiste | 19957 → 2951 | 17379 → 373 | 80 | 32 | 39 | 106 (39/35/32) | 54 (bleiben 23) | 39 → 0 | 114 → 0 | 31 → 50 von 266 | 64 (149) |
| 1920x1080 | v3-referenzspur | 19957 → 2959 | 17379 → 381 | 80 | 68 | 39 | 106 (39/35/32) | 54 (bleiben 23) | 39 → 0 | 114 → 0 | 31 → 50 von 266 | 64 (149) |
| 1366x768 | v1-reiter | 21674 → 2102 | 19975 → 403 | 144 | 0 | 39 | 103 (36/35/32) | 54 (bleiben 23) | 134 → 0 | 113 → 0 | 30 → 40 von 265 | 64 (149) |
| 1366x768 | v2-seitenleiste | 21674 → 2163 | 19975 → 464 | 77 | 32 | 39 | 103 (36/35/32) | 54 (bleiben 23) | 134 → 0 | 113 → 0 | 30 → 49 von 265 | 64 (149) |
| 1366x768 | v3-referenzspur | 21674 → 2163 | 19975 → 464 | 77 | 68 | 39 | 103 (36/35/32) | 54 (bleiben 23) | 134 → 0 | 113 → 0 | 30 → 49 von 265 | 64 (149) |

| Größe | Skizze | Ergebnis nur als Toast heute | Karte (Popover) über der Timeline | Schritte ohne Aufdecken, dazu ✎ immer sichtbar | Parameter außerhalb der Karte (Suche statt Scrollen) | Rest-Rasten im Dock nach Element |
|---|---|---|---|---|---|---|
| 1920x1080 | v1-reiter | 96 | 27 | 108 von 266 | 98 | route.card 316 |
| 1920x1080 | v2-seitenleiste | 96 | 0 | 112 von 266 | 103 | route.card 373 |
| 1920x1080 | v3-referenzspur | 96 | 0 | 112 von 266 | 103 | route.card 381 |
| 1366x768 | v1-reiter | 96 | 31 | 99 von 265 | 103 | route.card 373, ref:Stems 10, ref.source 8, ref.file 7, ref:To MIDI 5 |
| 1366x768 | v2-seitenleiste | 96 | 0 | 108 von 265 | 107 | route.card 464 |
| 1366x768 | v3-referenzspur | 96 | 0 | 108 von 265 | 107 | route.card 464 |
