# Flow-UI ohne Modus-Umschalter Collect/Select/Process

- **Status:** umgesetzt (2026-10-30, Todo
  `todos/2026-10-30-ui-modus-umschalter-weg.md`)
- **Datum:** 2026-10-30, Matthias

## Kontext

Der Umschalter `#modeSeg` (Collect · Select · Process) steuert seit
Variante B ([2026-10-29-ui-seitenleiste-referenz](2026-10-29-ui-seitenleiste-referenz.md))
nur noch Sichtbarkeiten in der Timeline (`nodeModes()` in
`dawd/ui/flow.html`); die Dock-Reiter hängen nicht mehr daran:

- Collect: Record/Monitor/Insert oben, Arm-Knopf je Spur.
- Select: Stern (Favorit) an Clips und Sections — nur hier.
- Process: Automation-Knopf, Freeze, „+“ an der Kette einer Gruppe.

Die Sorte eines Nodes (collect/select/process) kommt aus seinen Aktionen;
den neuen Node legt Auto-Branch an, nicht der Umschalter.

## Entscheidung (Option A)

- Der Umschalter fällt weg; alle Bedienelemente sind immer sichtbar
  (Record/Monitor/Insert, Arm, Automation, Freeze, „+“ der Gruppen-Kette).
- Favorit: Stern im Clip-Popover und an der Section.
- Unverändert: Node-Aktionen mit ihrer Sorte, Auto-Branch und sein
  Schalter, alle Ops.
- Verworfen: Umschalter behalten; erst nur in die Longlist.

## Folgen

- Die Timeline zeigt mehr Knöpfe zugleich — betrifft
  `todos/2026-10-30-ui-knoepfe-sichtbar.md` ([I]).
- Gewählte Sorte je Node im Browser (`ACTION_KEY`) und `nodeModes()`
  entfallen.
- `docs/ui-checklist.md`, die `ui_html`-Marker und die Treiber in
  `tools/ui_replay/` ändern sich mit.
