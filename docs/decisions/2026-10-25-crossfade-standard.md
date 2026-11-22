# Crossfades standardmäßig an, auch in bestehenden Projekten

- **Status:** gilt — umgesetzt (Todo `todos/2026-10-25-antworten-umsetzen.md`)
- **Datum:** 2026-10-25, Matthias

## Kontext

Seit dem Merge `clips-arrangement` (0e522a1) gibt es Crossfades zwischen
überlappenden Clips einer Lane (`setClip {crossfade}`, autoCrossfade der
Engine). Voreinstellung des Agents war: Schalter je Clip, aus; die Kurve
linear.

## Entscheidung

- Crossfade ist **standardmäßig an** — für neue Clips und für Clips in
  **bestehenden Projekten** (beim Laden). Überlappende Clips blenden über
  die Überlappung über, statt sich zu addieren. Abschalten je Clip bleibt
  (`setClip {crossfade: false}`, ⋈ in der Lane).
- Kurve der Crossfades standardmäßig **convex** (gleiche Leistung, kein
  Pegelloch bei verschiedenem Material).

## Folgen

- Ältere Mixe mit überlappenden Clips können anders klingen.
- Ein ausgeschalteter Crossfade muss gespeichert bleiben (ein „aus“ darf
  beim Laden nicht wieder „an“ werden) — die Umsetzung braucht einen
  Merker, der „nie gesetzt“ von „aus“ unterscheidet.
- Tests, die Überlappungen addieren, ändern sich.

## Umsetzung

- „Nie gesetzt“ = Clip ohne die Eigenschaft `autoCrossfade` im Edit:
  `refreshAutoCrossfades` (nach jeder Mutation, beim Laden, beim Render
  eines anderen Nodes) schaltet ihn an. `setClip {crossfade: false}`
  speichert `autoCrossfade="0"` — das bleibt aus.
- Kurve: die Engine kennt nur eine Kurve je Seite (Blende und Crossfade
  gemeinsam). Solange `fadeCurve` nie gesetzt ist (Merker `dawdAutoCurve`),
  setzt dawd convex auf der Seite, die überblendet, sonst linear — normale
  Blenden bleiben linear. `fadeCurve auto` gibt die Kurve an dawd zurück.
  Ältere Projekte mit gesetzter Kurve behalten sie.
- Comp-Grenzen (`compLane`, überlappende Teile) blenden damit auch convex.
