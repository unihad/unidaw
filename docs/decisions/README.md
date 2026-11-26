# Decisions zu agent-daw

Alle Entscheidungen zu agent-daw (dawd, Flow-UI, MCP, Vertrieb des Produkts)
stehen hier im Repo — eine Datei je Decision (Matthias, 2026-10-24).

- Dateiname: `JJJJ-MM-TT-kurzer-titel.md`.
- Inhalt: Status (gilt | ersetzt durch …), Kontext, Entscheidung, Folgen,
  ersetzt (falls eine ältere Decision abgelöst wird).
- Wer entscheidet: Matthias. Agents schreiben die Datei, sobald er
  entschieden hat, und verweisen aus Todos, `ARCHITECTURE.md` und Code-
  Kommentaren hierher.
- Kleine API-Entscheidungen zu einzelnen Befunden bleiben im Agent Log des
  Todos („Entscheidung Matthias …“); hier landet, was Architektur, Verhalten
  für den Nutzer oder das Produkt grundsätzlich festlegt.

## Ältere Decisions (bis 2026-10-23 außerhalb des Repos)

Vor dieser Regel lagen die Decisions in einer internen Wissensablage; ihr
Text ist nicht im Repo. Sie gelten weiter, bis eine Decision hier sie
ersetzt:

- `decision.2026-10-09-agent-first-daw-eigenbau-frontend-auf-tracktion-engine-mit-befehls-api-und-event-log`
- `decision.2026-10-11-song-baum-fuer-dawd-seed-pfad-favoriten-teilbaum-terminierung`
- `decision.2026-10-11-dawd-ist-eigenstaendige-daw-flow-tree-als-strukturschicht-linearer-track-view-geplant`
- `decision.2026-10-12-*` (Aufnahme/Insert-Konventionen) — Begrenzung der
  Aufnahme auf die Section ersetzt durch
  [2026-10-24-sections-als-zeitbereiche](2026-10-24-sections-als-zeitbereiche.md),
  „jeder Insert eine eigene Lane“ (addClip) ersetzt durch
  [2026-10-29-ueberlappung-fehler](2026-10-29-ueberlappung-fehler.md)
- `decision.2026-10-13-dawd-op-registry-und-node-aktionen-als-fundament-fur-die-vier-aktions-typen`

## Decisions

- [2026-10-24 Decisions im Repo](2026-10-24-decisions-im-repo.md)
- [2026-10-24 Sections als Zeitbereiche](2026-10-24-sections-als-zeitbereiche.md)
- [2026-10-24 Vertrieb ruht bis zum ausgereiften Produkt](2026-10-24-vertrieb-ruht.md) — ersetzt durch 2026-11-18 Open Source; intern, nicht im öffentlichen Stand
- [2026-10-24 Sends hinter der Kette sind Post-Fader-Sends](2026-10-24-post-fader-sends.md)
- [2026-10-24 Änderungen im Plugin-Fenster sind eine Node-Aktion](2026-10-24-plugin-fenster-als-aktion.md)
- [2026-10-24 Gruppen sind Knoten der Routing-Matrix](2026-10-24-gruppen-als-matrix-knoten.md)
- [2026-10-24 Einheiten: `real` ist die angezeigte Zahl](2026-10-24-einheiten-real.md)
- [2026-10-24 Spurnamen sind die stabile Adresse](2026-10-24-spurnamen-als-adresse.md)
- [2026-10-25 Commits nur in todos/ und docs/ ohne Test-Suite](2026-10-25-doku-commits-ohne-suite.md)
- [2026-10-25 ML-Trenn-Tests vor dem Push statt bei jedem Commit](2026-10-25-ml-tests-vor-dem-push.md)
- [2026-10-26 Master-Lautstärke 0 dB: die Wiedergabe ist die Datei](2026-10-26-master-0db.md)
- [2026-10-25 Suite parallel auf zwei dawd-Instanzen](2026-10-25-suite-parallel.md)
- [2026-10-25 Plugin-Sandboxing: Fremd-Plugins in eigenen Prozessen](2026-10-25-plugin-sandboxing.md)
- [2026-10-25 Crossfades standardmäßig an, auch in bestehenden Projekten](2026-10-25-crossfade-standard.md)
- [2026-10-26 OSC-Fernsteuerung: feste Adressen, standardmäßig nur dieser Rechner](2026-10-26-osc-fernsteuerung.md)
- [2026-10-29 Überlappender Clip ist ein Fehler, neue Lane nur auf Wunsch](2026-10-29-ueberlappung-fehler.md)
- [2026-10-29 Flow-UI: Seitenleiste für Referenz und Analyse, Dock mit Reitern](2026-10-29-ui-seitenleiste-referenz.md)
- [2026-10-30 Render mit Zeitfenster bekommt einen festen Vorlauf](2026-10-30-analyse-vorlauf.md)
- [2026-10-30 Flow-UI ohne Modus-Umschalter Collect/Select/Process](2026-10-30-modus-umschalter-weg.md)
- [2026-10-31 Clip-Leiste nur am gewählten Clip, Klick verzögert](2026-10-31-clip-leiste-nur-gewaehlt.md)
- [2026-10-31 Warp-Marker wirken in Echtzeit](2026-10-31-warp-echtzeit.md)
- [2026-10-31 Flow-UI zeigt Änderungen von außen sofort](2026-10-31-aenderungen-von-aussen-live.md)
- [2026-11-18 Klang-Analyse der Sample-Bibliothek je Hersteller](2026-11-18-klang-analyse-je-hersteller.md)
- [2026-11-18 Analyse-Ops auf Samples gesperrter Hersteller](2026-11-18-analyse-ops-auf-gesperrten-samples.md)
- [2026-11-18 Öffentlicher Stand ohne Git-Historie, ohne Diva-Katalog](2026-11-18-oeffentlicher-stand-ohne-historie.md)
- [2026-11-19 MP3-Decoder von JUCE bleibt im öffentlichen Release](2026-11-19-mp3-decoder-bleibt.md)
- [2026-11-18 dawd wird Open Source (AGPLv3), Einnahmen nur über Spenden](2026-11-18-open-source-agpl-spenden.md)
- [2026-11-19 Produktname unidaw](2026-11-19-produktname-unidaw.md)
- [2026-11-23 Shorts-Reihe statt Demo-Video, Agents nehmen auf](2026-11-23-shorts-statt-demo-video.md)

## Vorschläge (warten auf Matthias, gelten noch nicht)

- keine
