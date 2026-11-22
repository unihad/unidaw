# Spurnamen sind die stabile Adresse, Indizes gelten nur für den Moment

- **Status:** gilt
- **Datum:** 2026-10-24, Matthias

## Kontext

Lanes (Takes, Inserts) sind eigene Engine-Spuren und hängen hinten an
`userTracks`; dadurch springen die Spurindizes nach einer Aufnahme (1k P9).
Ein Indexraum gilt für `state.trackList`, Routing-Knoten `t<i>`,
Node-Aktionen, Automation-Ids und Rezepte. Stabile Indizes wären ein großer
Umbau mit API-Bruch (Fakten: Agent `kleinkram`, 2026-10-24).

## Entscheidung

Die Indizes bleiben, wie sie sind. Spurnamen sind die stabile Adresse (sie
werden überall als `track` angenommen, `trackIndexFor` in
`OpRegistry.cpp`); ein Index gilt nur bis zur nächsten Änderung der
Spurliste.

## Folgen

- MCP-Doku und AGENTS.md empfehlen Spurnamen und sagen, dass Lanes hinten
  Indizes belegen.
- Optional `laneOf` in `state.trackList`.
