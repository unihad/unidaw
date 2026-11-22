# Render mit Zeitfenster bekommt einen festen Vorlauf

- **Status:** gilt — umgesetzt im Branch `analyse-vorlauf` (Todo
  `todos/2026-10-30-analyse-vorlauf.md`)
- **Datum:** 2026-10-30, Matthias

## Kontext

`analyze {track, start, end}` renderte nur das Fenster `[start, end]`. Ein
frisch geladenes Instrument begann dort aus der Stille, spätere Messungen
erbten den Zustand (Hall, Hüllkurve) des vorigen Renders. Surge XT „Cyber
Pad“, Fenster 2,00–2,06 s: erste Messung −44,0 dB, zweite −32,3 dB, voller
Render 0–3 s −30,6 dB (Agent Log von `todos/2026-10-30-lfo-wert-im-fader.md`).
Daher kamen die −76 dB aus der Longlist ([K]).

## Entscheidung (Option B)

- Ein Render mit Zeitfenster (`renderMixRange`) rendert einen festen
  Vorlauf von 2 s vor `start` mit; liegt `start` unter 2 s, ab 0 s.
- Gemessen wird nur das Fenster. Eine Datei, die der Nutzer bekommt
  (Rezept-Hörprobe, Stems aus `separateStems`), enthält den Vorlauf nicht.
- Verworfen: ab Song- oder Section-Anfang rendern; nur warnen.

## Folgen

- Gilt für alle Aufrufer von `renderMixRange`: `analyze`,
  `compareReference`, die Analyse-Ops über `loadAnalysisSource`
  (`describeVoice`, `detectTempo` …, auch der zweite Render für
  `renderSpread`), `separateStems`, `compareClasses`, `findSamples {like}`,
  die Rezept-Hörprobe.
- Limiter messen ihre Gain-Reduction nur im Fenster (`resetStats (from)`).
- Ein Render mit Fenster kostet bis zu 2 s Render mehr.
- Bleibt offen: ein Fenster, das vor 2 s beginnt, erbt weiter den Rest des
  vorigen Renders; Hüllkurven länger als 2 s setzen am Vorlauf-Beginn neu
  ein.
- Details: `ARCHITECTURE.md` (Analyse), `docs/engine-bewertung.md`, MCP-Doku
  zu `analyze`.
