# Sample-Bibliothek (externe Platte)

Die Samples für unidaw liegen gesammelt auf der externen Platte (Seagate
Backup Plus Slim, 2 TB, NTFS, USB 3.0 — HDD). Ubuntu hängt sie beim
Einstecken lesend und schreibend ein, unter der NTFS-Seriennummer:

    /run/media/matthias/708E12148E11D406/Samples

Die Windows-Partition muss dafür nicht mehr eingehängt sein. Die Platte
trägt daneben private Backups (`Papa Festplatte`, `Fotos und Videos`,
`Ableton/` mit Projekten) — unidaw meldet nur `Samples/` an.

## Inhalt (Stand 2026-10-16)

| Ordner | Herkunft | Dateien | Größe |
|---|---|---|---|
| `NI/` | Windows `Users/Public/Documents` (Battery 4, Expansions, Library-Samples) | 36.754 | 32,9 GB |
| `Ableton/Core Library/` | Windows `ProgramData/Ableton/Live 12 Suite/Resources/Core Library` | 3.346 | 1,8 GB |
| `Ableton/Factory Packs/` | Windows `Users/matth/Documents/Ableton/Factory Packs` | 1.237 | 1,3 GB |
| `Ableton/User Library/` | Windows `Users/matth/Documents/Ableton/User Library` | 35 | — |
| `Arturia/` | Windows `ProgramData/Arturia/Samples` | 3.401 | 2,3 GB |
| `Landr/` | lag schon auf der Platte (`Samples Extern/Landr`) | 7.105 | 12 GB |

Kopiert wurde nur, was dawds Index lesen kann (`source/SampleLibrary.cpp`):
wav, aif, aiff, flac, ogg, mp3; ohne Punkt-Ordner, `Previews` und
`Ableton Folder Info`; ohne AIFF-C im "able"-Codec. Ordnerstruktur wie im
Original. Die Windows-Partition blieb unverändert (nur lesend eingehängt).

## Bewusst nicht kopiert

- **Kontakt-Libraries** (`.nkx`/`.ncw`, ~229 GB: Kontakt Factory Library 2,
  Session Guitarist/Bassist/Strings/Horns, Noire, Una Corda, Studio
  Drummer, Scarbee …) — nur Kontakt kann sie öffnen, und Kontakt läuft hier
  nicht (Entscheidung Matthias, 2026-10-16).
- **Ableton-Packs im "able"-Codec** (~37.000 Dateien, ~82 GB, fast alle
  Factory Packs) — nur Live dekodiert sie.
- **NI Previews** (`Users/Public/Documents/Previews`, ~9 GB) — Vorhörer für
  Presets, keine Samples.
- Die drei weiteren Kopien der Core Library (Live 12 Beta, Beta.Old,
  .Live 12 Suite_updated) — Duplikate.
- Plugin-interne Wellenformen unter `Program Files` (Massive, FM8, Kontakt …),
  Projekte und private Dateien.

## In unidaw anmelden

    sampleLibraries {add: "/run/media/matthias/708E12148E11D406/Samples"}

Eine Wurzel für alles; `findSamples {dir: "NI/Battery 4 Factory Library"}`
o. ä. grenzt ein. Der erste Index von der HDD dauert kalt einige Minuten.
Ohne eingesteckte Platte findet die Suche nichts; Projekte bleiben
vollständig, weil `setSamples` benutzte Samples nach `media/samples/`
kopiert.

## Suche nach Klang

Neben der Namenssuche findet unidaw Samples nach Beschreibung und nach
Referenz (`findSamples {text}` / `{like}`, im UI: Sampler-Karte, Umschalter
Name/Sound und ≈ an jedem Sample). Dafür braucht jede Datei eine
Klang-Analyse — CLAP-Vektoren und gemessene Merkmale, im Hintergrund:

    sampleLibraries {analyze: true}          # alle Bibliotheken
    sampleLibraries {analyze: "<ordner>"}    # nur ein Teil
    sampleLibraries {analyze: false}         # anhalten
    sampleLibraries {noAnalysis: "<ordner>"}    # Ordner nie analysieren
    sampleLibraries {allowAnalysis: "<ordner>"} # wieder freigeben

Lizenzen: NI (EULA 3.1) verbietet Data Mining, LANDR (Samples Licence
(iv)) jede Nutzung mit KI-Systemen. Welche Ordner ohne Analyse bleiben,
steht in der Decision
[2026-11-18 Klang-Analyse je Hersteller](decisions/2026-11-18-klang-analyse-je-hersteller.md).
`noAnalysis` löscht die schon gerechneten Klang-Daten des Ordners; die
Suche nach Namen bleibt. Im UI: Sampler-Karte, Suche "Sound", "No
analysis…" und ein Chip je gesperrtem Ordner (× gibt frei).

Gemessen (Ryzen 7 4800H, 7 Worker, HDD): ~0,12 s je Datei, die ganze Platte
(~52.000 Dateien) also gut 1,5 Stunden — einmalig; danach nur neue und
geänderte Dateien. Abbrechen verliert nichts (jede fertige Datei liegt
sofort in `<config>/index/<hash>.sound`). Die Worker laufen mit nice 19;
während einer Aufnahme trotzdem lieber anhalten.

Was im Test (Stichprobe 2.000 Dateien, 2026-10-16) trug und was nicht:

- CLAP (LAION `larger_clap_general`): Drum-One-Shots nach Beschreibung gut
  ("metallic snare" 7/10 Snares in den Top 10, "closed hi-hat" 7/10, "punchy
  kick" 10/10); Referenzsuche sehr gut (erst Varianten desselben Kits, dann
  klanglich Passendes derselben Kategorie).
- Ohne Kategorie-Filter kommt bei "metallic snare" auch Spring-Reverb-Gitarre
  und Gong — darum filtert ein Kategoriewort im Text automatisch.
- Vibrato hört CLAP nicht; das gemessene Vibrato (YIN-Tonhöhe, regelmäßige
  Schwankung 3,5–9 Hz) findet 6 von 7 Gitarren mit "Vibrato" im Namen;
  mehrstimmige Vocal-Akkorde können durchrutschen.
- Ein gemessenes "Klingen" (Tonalität im Ausklang) hing nicht mit
  "metallisch" zusammen und ist wieder raus — "metallisch" kann CLAP selbst.
