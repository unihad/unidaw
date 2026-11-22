# Warp-Marker wirken in Echtzeit

- **Status:** gilt — umgesetzt im Branch `warp-echtzeit` (Todo
  `todos/2026-10-31-warp-echtzeit.md`)
- **Datum:** 2026-10-31, Matthias (Handprüfung: „gleich Echtzeit-Warp
  bauen, nicht erst nur den Bereich verkleinern“)

## Kontext

`warpClip` renderte bei jeder Änderung die ganze Quelldatei offline
(Rubberband mit Keyframe-Map) nach `media/<name>-warp<N>.wav`. Bei einem
Stem von ~220 s dauerte das 5,1–5,6 s, der Message-Thread stand so lange,
jede Änderung schrieb 77 MB. In Ableton Live wirkt ein verschobener Marker
sofort. Das Echtzeit-Warp der Engine traf die Beats nur auf ±20 ms bzw.
~90 ms und war darum aus.

## Entscheidung

- Warp-Marker rechnet die Engine beim Abspielen und im Render in Echtzeit:
  ein Rubberband-Stretcher, sein Verhältnis je eingespeistem Stück aus der
  Marker-Map (Patch `dawd/patches/tracktion-warp-realtime.patch`).
- Der Clip spielt die Originaldatei; die Marker bleiben `DAWDWARP` am Clip
  (`realtime`), dawd gibt sie über `EngineBehaviour::getWarpPointsForClip`
  an die Engine.
- Kein Render, keine Datei je Änderung, keine Ladeanzeige in der Flow-UI.
- Genauigkeit wie vorher: Einsätze auf den Markern ±5 ms.

## Folgen

- `warpClip` antwortet in 0,01 s statt 1,7 s (200 s Quelle, eigene Instanz)
  bzw. 5,6 s (Live-Instanz).
- Projekte von vorher: `playWarpSources` stellt gewarpte Clips beim Laden
  auf die Originaldatei um; `treeCompact` räumt die alten `-warp`-Dateien,
  sobald nichts mehr auf sie zeigt.
- Wege b (Hybrid mit Hintergrund-Render) und c (nur den genutzten Bereich
  rendern) sind nicht nötig; c bleibt im Todo als Messpunkt (0,22 s).
