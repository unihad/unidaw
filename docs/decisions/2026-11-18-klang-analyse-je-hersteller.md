# Klang-Analyse der Sample-Bibliothek je Hersteller

- **Status:** gilt — Matthias hat den Vorschlag am 2026-11-18 so
  angenommen (Todo
  `todos/2026-11-18-klang-analyse-gegen-sample-lizenzen-pruefen.md`,
  Frage „vorschlag-ok“: yes).
- **Datum:** 2026-11-18, Entwurf Claude Code
- **Keine Rechtsberatung:** gelesen sind nur die öffentlichen Lizenztexte
  und der Gesetzestext.

## Kontext

dawd rechnet für jede Datei der Sample-Bibliothek eine Klang-Analyse
(`sampleLibraries {analyze}`, `docs/sample-bibliothek.md`): die Datei wird
dekodiert, ein vortrainiertes Modell (CLAP) macht daraus Vektoren, dazu
kommen gemessene Merkmale (Tonhöhe, Vibrato, Punch, Tempo). Das Ergebnis
liegt in `<config>/index/<hash>.sound` und dient nur der Suche
(`findSamples {text | like}`). dawd trainiert kein Modell. Auf Matthias'
Rechner liegt dieser Index seit 2026-10-17 für die ganze Platte vor, also
auch für NI und LANDR.

Quellen (Lizenztexte mit Hash, abgerufen 2026-11-18, im Repo the-sublet
unter `licenses/`):

| Hersteller | Stelle | Wortlaut (Auszug) |
|---|---|---|
| Native Instruments (EULA, Stand 2026-07-01) | 3.1 | Licensee shall not have the right to reproduce and/or otherwise use the Product … audio and visual content contained therein in any manner for purposes of data mining (Sec. 44b German Copyright Act) and/or training artificial intelligence technologies. |
| LANDR (Terms of Service, Stand 2026-11-09) | Samples Licence (iv) | use any Sample for the purpose of training machine learning models, or for any form of Artificial Intelligence (AI) development, including … the generation, synthesis, or manipulation of audio … This restriction applies to all forms of AI-based technologies, algorithms, or systems that involve data processing, learning, or content creation |
| Ableton (EULA, ohne Datum) | 5 (3) a | These materials are provided for Your use only with the Ableton Product … Keine Klausel zu Data Mining oder KI. |
| Arturia (Lizenz im Handbuch Pigments, 2025) | 2 | Arturia reserves all rights not expressly granted. Keine Klausel zu Data Mining oder KI. |

§ 44b UrhG (gesetze-im-internet.de, abgerufen 2026-11-18): Abs. 1 —
Text und Data Mining ist „die automatisierte Analyse von einzelnen oder
mehreren digitalen oder digitalisierten Werken, um daraus Informationen
insbesondere über Muster, Trends und Korrelationen zu gewinnen“. Abs. 2
erlaubt die Kopien dafür, Abs. 3 nur, wenn der Rechteinhaber es sich nicht
vorbehalten hat.

## Bewertung

- **Die Klang-Analyse ist Data Mining im Sinne von § 44b Abs. 1.** Sie
  analysiert jede Datei automatisch und gewinnt daraus Informationen über
  Muster (Klangvektoren, Merkmale). Dass kein Modell trainiert wird, ändert
  daran nichts: NI verbietet Data Mining *und/oder* KI-Training.
- **NI:** 3.1 verweist ausdrücklich auf § 44b. Die Analyse fällt darunter.
- **LANDR:** (iv) nennt jedes KI-basierte System, das Daten verarbeitet.
  CLAP ist ein solches System. Die Analyse fällt darunter. Dazu erlaubt die
  Samples Licence die Samples nur in „Recordings“ über LANDRs Release
  Service (Decision the-sublet `2026-11-18-samples-nur-im-fertigen-mix`).
- **Ableton Core Library:** keine Klausel zu Data Mining oder KI, also
  kein Vorbehalt nach § 44b Abs. 3. Offen ist 5 (3) a („use only with the
  Ableton Product“) — das betrifft jede Nutzung in dawd, nicht nur die
  Analyse (eigenes Todo, siehe Folgen).
- **Ableton Factory Packs:** Packs von Dritten haben eigene Bedingungen
  (EULA 14), nicht geprüft.
- **Arturia:** keine Klausel zu Data Mining oder KI; nur „all rights not
  expressly granted“ vorbehalten. Ob das ein Vorbehalt nach § 44b Abs. 3
  ist, belegt keine Quelle hier. Für The Sublet ist Arturia schon gesperrt
  (unklar, Arturia wird nicht gefragt).

## Entscheidung

| Ordner unter `Samples/` | Klang-Analyse | Grund |
|---|---|---|
| `NI/` | **aus** | NI-EULA 3.1 |
| `Landr/` | **aus** | LANDR Samples Licence (iv) |
| `Ableton/Factory Packs/` | **aus** | Bedingungen der Dritt-Packs nicht geprüft |
| `Arturia/` | **aus** | unklar, wie in the-sublet |
| `Ableton/Core Library/` | an | keine Klausel; 5 (3) a eigenes Todo |
| `Ableton/User Library/` | an | eigene Dateien |
| dawds Kits (`dawd/kits/`) | an | eigenes Material |

- dawd sperrt die Analyse je Ordner: `sampleLibraries {noAnalysis:
  "<ordner>"}`, aufheben mit `{allowAnalysis: "<ordner>"}`. Gespeichert in
  `<config>/libraries.json` (je Rechner, weil der Pfad je Rechner anders
  ist). In der Flow-UI: Sampler-Karte, Suche „Sound“, Zeile der Analyse
  („No analysis…“, Chip je Ordner mit ×).
- Sperren löscht die schon gerechneten Klang-Daten des Ordners aus dem
  Index (auch auf der Platte). Die Analyse überspringt ihn,
  `findSamples {like}` lehnt eine Datei daraus als Referenz ab. Die Suche
  nach Namen und Ordner bleibt, ebenso `setSamples` (Samples im Song
  spielen ist keine Analyse).
- Wird ein Ordner freigegeben, holt die nächste Analyse ihn nach.

## Alternativen

- **Nichts sperren, weil nur ein Suchindex entsteht:** verworfen —
  § 44b Abs. 1 fragt nicht nach dem Zweck, und NI nennt § 44b ausdrücklich.
- **Nur NI und LANDR sperren:** nicht gewählt — Arturia und die Factory
  Packs bleiben gesperrt, bis ihre Bedingungen geprüft sind. Eine spätere
  Freigabe ist ein Befehl (`allowAnalysis`).
- **Sperre fest im Code (Ordnernamen):** verworfen — die Pfade hängen am
  Rechner, und Matthias soll ohne neuen Build umschalten können.

## Folgen

- Rollout auf Matthias' Rechner: die vier Ordner mit `noAnalysis` sperren;
  das löscht die Vektoren von NI, LANDR, Factory Packs und Arturia aus dem
  Index von 2026-10-17.
- Die Suche nach Klang findet danach nur noch Ableton Core Library, User
  Library und dawds Kits. Für The Sublet galt das schon (Regel 5 der
  Decision dort: Samples nur nach Namen).
- Nicht abgedeckt: andere Analyse-Ops (`describeVoice`, `separateStems`,
  `audioToMidi`, `matchSound` …) auf einer Datei oder einem Mix mit
  Samples gesperrter Hersteller. LANDR (iv) nennt auch „manipulation of
  audio“. Prüfung im Folge-Todo
  `todos/2026-11-18-analyse-ops-auf-gesperrten-samples.md`.
- Ableton 5 (3) a („use only with the Ableton Product“): geklärt in
  the-sublet, Decision `2026-11-18-ableton-core-library-auch-in-dawd`
  (Core-Library-Samples bleiben auch in dawd erlaubt).
- Lizenztexte ändern sich: vor einer Freigabe den Text neu abrufen und mit
  dem Hash in the-sublet `licenses/` vergleichen.
