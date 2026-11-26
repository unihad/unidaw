# Shorts-Reihe statt Demo-Video, Agents nehmen auf

- **Status:** gilt — Matthias hat am 2026-11-23 entschieden
  (Ziel-Evaluation `agent-daw-bereit-fuer-den-vertrieb-machen`, Todo
  `todos/2026-11-23-shorts-reihe-planen.md`). Ersetzt das Drehbuch
  [`docs/demo-video.md`](../demo-video.md) als Plan fürs Video.
- **Datum:** 2026-11-23, Entwurf Claude Code

## Kontext

Für README und Release-Ankündigung war ein Demo-Video von 90–120 s
geplant: ein Agent arrangiert, Matthias nimmt eine Spur auf
(`docs/demo-video.md`, Todo `2026-11-18-demo-video-aufnehmen`). Dafür
hätte Matthias selbst aufnehmen und schneiden müssen. Das Todo ist
abgebrochen (cancelled).

## Entscheidung

- Statt eines langen Videos gibt es eine **Reihe von Shorts**: je ein
  Short pro Feature von unidaw, Hochformat 9:16, etwa 20–45 s.
- **Agents nehmen die Shorts automatisch auf.** Matthias nimmt nichts
  selbst auf und spielt nichts ein. Material, das sonst ein Mensch
  einspielt (eine Melodie), erzeugt die Kette selbst.
- **Der erste Short trägt die Release-Ankündigung** und zeigt unidaw als
  Ganzes: ein Agent arbeitet über MCP mit der DAW. Die übrigen Shorts
  erscheinen danach als Reihe.
- Drehbücher, Reihenfolge und gemeinsame Regeln: [`docs/shorts.md`](../shorts.md).

## Alternativen

- **Demo-Video wie geplant:** verworfen — braucht Aufnahme und Schnitt von
  Matthias, und ein 2-Minuten-Video passt nicht in die Kanäle für kurze
  Clips.
- **Ein Short für alles:** verworfen — 45 s reichen nicht für mehr als ein
  Feature; eine Reihe gibt den Kanälen mehrere Termine.

## Folgen

- `docs/demo-video.md` ist abgelöst; der Trockenlauf darin bleibt als
  Beleg, dass die Prompts laufen.
- Die automatische Aufnahme baut Todo
  `2026-11-23-ersten-short-automatisch-aufnehmen` (Skript unter `tools/`,
  erster Short), die übrigen Shorts Todo
  `2026-11-23-restliche-shorts-aufnehmen`.
- Weil kein Mensch einspielt, zeigen die Shorts keine Live-Aufnahme. Der
  Ton jedes Shorts ist der Render des Songs aus dem Short.
