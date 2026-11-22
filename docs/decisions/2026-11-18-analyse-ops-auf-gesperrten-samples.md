# Analyse-Ops auf Samples gesperrter Hersteller

- **Status:** Entschieden — Matthias hat den Vorschlag am 2026-11-18
  angenommen (Antwort im Todo
  `todos/2026-11-18-analyse-ops-auf-gesperrten-samples.md`). Umgesetzt im
  Todo `todos/2026-11-19-analyse-ops-sperre-umsetzen.md` (siehe
  „Umsetzung“ unten); baut auf der Ordner-Sperre `sampleLibraries
  {noAnalysis}` aus Decision `2026-11-18-klang-analyse-je-hersteller.md`
  auf.
- **Datum:** 2026-11-18, Entwurf Claude Code
- **Keine Rechtsberatung:** gelesen sind nur die öffentlichen Lizenztexte
  (the-sublet `licenses/`, abgerufen 2026-11-18, mit Hash) und der
  Wortlaut von § 44b UrhG, wie ihn die Decision zur Klang-Analyse zitiert.

## Kontext

Die Sperre `noAnalysis` gilt nur für den Klang-Index der Bibliothek
(`findSamples {text | like}`). Andere Ops lesen beliebiges Audio — eine
Datei (`file`) oder den eigenen Mix bzw. eine Spur (`track`, `section`,
`start/end`) — und messen daran Muster, zum Teil mit ML-Modellen.

Die zwei Klauseln (Wortlaut: Decision zur Klang-Analyse):

- **NI-EULA 3.1:** kein „reproduce and/or otherwise use … audio and visual
  content contained therein in any manner for purposes of data mining
  (Sec. 44b German Copyright Act) and/or training artificial intelligence
  technologies“.
- **LANDR Samples Licence (iv):** kein „use any Sample … for any form of
  Artificial Intelligence (AI) development, including but not limited to
  the generation, synthesis, or manipulation of audio, MIDI or any other
  related content. This restriction applies to all forms of AI-based
  technologies, algorithms, or systems that involve data processing,
  learning, or content creation“.
- § 44b Abs. 1: Data Mining ist „die automatisierte Analyse von einzelnen
  oder mehreren digitalen oder digitalisierten Werken, um daraus
  Informationen insbesondere über Muster, Trends und Korrelationen zu
  gewinnen“.

Erlaubt bleibt, was beide Lizenzen als Zweck nennen: das Sample im eigenen
Song spielen und bearbeiten (NI 3.7 „music and audio productions“, LANDR
„incorporation into your Recordings“, „with or without modifications“).

## Die Ops (Stand main, 2026-11-18)

| Gruppe | Ops | Was sie tun | Eingabe |
|---|---|---|---|
| **A — ML-Modell auf dem Audio** | `audioToMidi`, `addMidiFromAudio`, `describeVoice`, `describeTimbre`, `compareVoice`, `compareStems`, `describeLayers`, `splitLayer`, `compareSeparated` (basic-pitch, `ops/Voice.cpp`, `ops/Analysis.cpp`); `separateStems` (HT-Demucs, `ops/Stems.cpp`); `compareClasses` (53-Klassen-Modell); `matchSound`, `tuneSound` (Referenz gegen gerenderte Presets, `ops/SoundMatch.cpp`); `compareReference` mit Artikulation (basic-pitch); `analyze {embed: true}` (CLAP-Vektor einer Datei, `ops/Processing.cpp`) | Modell gewinnt Noten, Stems, Klassen oder Klangvektoren aus dem Audio; `separateStems` und `addMidiFromAudio` erzeugen neues Material daraus | Datei, Spur oder Mix |
| **B — Messung ohne Modell** | `detectTempo`, `detectKey`, `extractGroove`, `analyzeStructure`, `findRepeats`, `describeDrums`, `compareDrums`, `compareReference` (Lautheit, Klangfarbe), `detectSongBoundaries` | Signalverarbeitung: Onsets, Chroma, Spektrum, Hüllkurve → Tempo, Tonart, Muster | Datei, Spur oder Mix |
| **C — Pegel** | `analyze` (ohne `embed`), `levels`, `peak`, `clipPeaks` | LUFS, Spitze, Wellenform | Spur, Mix, Clip |

Die meisten Ops der Gruppen A und B laden ihr Audio über eine Stelle:
`loadAnalysisSource` (`dawd/source/ops/AnalysisSource.*`). `separateStems`,
`compareClasses`, `matchSound`/`tuneSound` und `analyze {embed}` lesen die
Datei selbst.

Lücke heute: `analyze {file, embed: true}` rechnet denselben CLAP-Vektor
wie die Sample-Suche — auch für eine Datei aus einem gesperrten Ordner.

## Bewertung

- **Gruppe A, Datei aus NI oder LANDR:** fällt unter beide Klauseln.
  Automatische Analyse eines einzelnen Werks, die Informationen über Muster
  gewinnt (§ 44b Abs. 1, NI 3.1). Ein KI-System verarbeitet das Sample und
  erzeugt MIDI oder Audio daraus (LANDR (iv) nennt „generation … or
  manipulation of audio, MIDI“ und „data processing“).
- **Gruppe B, Datei aus NI:** der Wortlaut von § 44b Abs. 1 trifft auch
  Tempo- und Tonart-Erkennung einer einzelnen Datei („einzelnen …
  Werken“, „Muster“). Ob normale DAW-Funktionen gemeint sind, beantwortet
  keine Quelle hier. **Gruppe B, Datei aus LANDR:** kein KI-System, (iv)
  greift dem Wortlaut nach nicht.
- **Gruppe C:** Pegel messen gehört zum Mischen des eigenen Songs; kein
  Muster im Sinn der Klauseln. Bleibt frei.
- **Eigener Mix oder Spur mit eingebautem Sample:** offen. Der Mix ist das
  erlaubte Ziel („music and audio productions“, „Recordings“), aber die
  Analyse liest das Sample mit. LANDR (iv) gilt ausdrücklich für jede
  Nutzung des Samples, auch eingebaut. Eine Spur, die fast nur aus dem
  Sample besteht (ein Loop), kommt der Datei gleich.

## Reicht die Herkunft im Projekt?

Nein, nicht ohne Änderung:

- `addClip` mit WAV verweist auf die Originaldatei (`insertAudioClip`,
  `EngineHelpers.cpp`) — Herkunft bekannt. mp3/ogg werden nach `media/`
  dekodiert — Herkunft weg.
- `setSamples` kopiert nach `media/samples/` (`ops/Processing.cpp`) und
  merkt sich die Quelle nicht.
- Abgeleitetes Audio vergisst die Quelle: Clip-FX (`processClip` hält nur
  die Projektdatei als `source`), Freeze, Bounce, `renderStems`, Stems aus
  `separateStems`.
- Eine Datei, die von Hand kopiert wurde, erkennt nur ein Vergleich des
  Inhalts.

Was nötig wäre: die Quelle beim Kopieren notieren (Sampler-Sound, dekodierte
Datei) und bei abgeleitetem Audio die Quellen mitgeben; dann kann dawd für
jede Spur und jeden Bereich sagen, ob Material aus einem gesperrten Ordner
klingt.

## Entscheidung

1. **Datei aus gesperrtem Ordner** (`file`, Referenz, `analyze {embed}`):
   Gruppe A **lehnt ab** — gleiche Meldung wie `findSamples {like}` („no
   sound analysis for files in … (blocked for its licence)“). Gruppe B
   **lehnt ab** bei Ordnern, die die Sperre trägt — eine Regel für alle
   gesperrten Ordner statt je Hersteller und Op. Gruppe C frei.
2. **Spur oder Mix mit Material aus gesperrtem Ordner:** Gruppe A
   **lehnt ab**, wenn im Bereich Material aus gesperrtem Ordner klingt;
   die Meldung nennt Spur und Datei, damit der Agent die Spur stumm misst
   (`track` auf eine andere Spur) oder den Bereich wählt. Gruppe B **warnt**
   (`note` mit Spur und Datei) und misst — Tempo und Tonart des eigenen
   Songs braucht jede Arbeit am Song.
3. **Herkunft:** dawd notiert die Quelle beim Kopieren (`setSamples`,
   dekodierte Dateien) und gibt sie an abgeleitetes Audio weiter
   (Clip-FX, Freeze, Bounce, Stems). Geprüft wird bei jeder Analyse gegen
   die Liste der gesperrten Ordner, nicht beim Einfügen — wird ein Ordner
   freigegeben, gilt das sofort.
4. Prüfstelle: `loadAnalysisSource` plus die vier Ops, die selbst laden.

## Alternativen

- **Nur warnen, nie ablehnen:** einfacher; der Agent würde aber weiter
  NI- und LANDR-Material durch die Modelle schicken. Verworfen für
  Gruppe A.
- **Auch Gruppe B auf dem Mix ablehnen:** am sichersten; dann misst kein
  Agent mehr Tempo und Tonart eines Songs mit NI-Drums. Verworfen, weil
  der Mix das erlaubte Ziel ist.
- **Nur Dateien prüfen, Mix nicht (keine Herkunft bauen):** wenig Code,
  lässt aber `audioToMidi` auf einer Spur mit einem LANDR-Loop durch.

## Folgen

- Umsetzung als eigenes Todo, erst nach dem Ja und nach dem Merge der
  Ordner-Sperre.
- Tests: Datei aus gesperrtem Ordner je Gruppe; Spur mit Sampler aus
  gesperrtem Ordner (A abgelehnt, B mit note); Freeze/Clip-FX behalten die
  Herkunft; Freigabe wirkt sofort.
- UI: die Fehler- und Hinweistexte erscheinen in den Analyse-Panels wie
  andere Fehler; kein neues Control.
- Die Ops, die der Agent für The Sublet nutzt, bleiben auf eigenem
  Material frei (Synth von dawd, eigene Aufnahmen).

## Umsetzung (2026-11-18)

- Herkunft: `dawd/source/Origins.*`, Seitendatei `.dawd-origins.json` im
  Ordner der abgeleiteten Datei (Dateiname -> Quellen). Notiert bei
  `setSamples` (Kopie nach `media/samples`), dekodierter WAV, `processClip`,
  Trim-Kopie (`-pad`), `freezeTrack`, `bounceNode` und `separateStems`.
- Prüfung: `blockedFileError` und `checkMixLicence`
  (`dawd/source/ops/AnalysisSource.*`). `loadAnalysisSource` prüft für alle
  Ops, die darüber laden (Flag `ml` für Gruppe A); `separateStems`,
  `compareClasses`, `analyze {embed}`, `compareReference`,
  `detectSongBoundaries` und `findSamples {like}` auf dem Mix prüfen
  selbst.
- Der Hinweis der Gruppe B heißt `licenceNote` statt `note`: `note` ist in
  mehreren Ops schon belegt. `executeOp` setzt ihn je Befehl (auch im
  `batch`), das Flow-UI zeigt ihn rot in der Statuszeile.
- `compareReference` misst mit `licenceNote`; nur die Artikulation
  (basic-pitch) fällt weg, mit Hinweis in `hints`.
- `audioToMidi`/`addMidiFromAudio` zählen ganz zur Gruppe A, auch
  `mode: drums` (ohne Modell, erzeugt aber MIDI aus dem Audio).
- Nicht verfolgt: Exporte (`render`, `renderStems`) und die Teile von
  `splitLayer` — ihre Eingabe ist schon geprüft; eine Datei, die von Hand
  kopiert wurde (wie oben unter „Reicht die Herkunft im Projekt?“).
- Test: `analysis_licence_block_flow` in `tests/run_tests.py`.
