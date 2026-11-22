# unidaw — Vorbilder für die Agent-UX

Vergleich mit den MCP-Interfaces anderer DAWs: was unidaw übernehmen kann und
was nicht. Stand 2026-10-13. Diese Datei ist Lesereferenz für Agenten, die
Features an unidaw bauen — die Vorbilder werden **nicht installiert**.

## Die Vorbilder

- **reaper-mcp** — github.com/bonfire-systems/reaper-mcp — MIT, 60 Tools in
  9 Kategorien (Project, Tracks, Track Templates, MIDI, FX, Audio, Mixing,
  Rendering, Mastering, Analysis). Braucht REAPER 7+.
- **ableton-mcp** — github.com/ahujasid/ableton-mcp — MIT, 3.2k Sterne.
  Braucht laufendes Ableton Live mit Remote Script.
- **LivePilot** — github.com/dreamrec/LivePilot — BSL-1.1 (source-available,
  nicht Open Source), 474 Tools, macOS + Windows.

**Warum keine Installation:** Alle drei brauchen ihre Host-DAW (REAPER 7+,
laufendes Ableton; LivePilot nur macOS/Windows) — auf Linux tot
angeschlossen. Der Wert liegt in den Tool-Listen, und die stehen in den
Repos: klonen, lesen, Operationen vergleichen. Das reicht als Referenz.

## Lessons für unidaw

1. **Kontext-Budget klein halten.** LivePilot wirbt in normalen Sessions nur
   28 Entry-Points statt aller 474 Tool-Schemas — der Rest bleibt suchbar und
   aufrufbar. dawds Zwei-Tools-Design (`unidaw_start` + `unidaw_command` mit
   `listOps` als Op-Tabelle) ist richtiger als ein Tool pro Op; die
   Op-Referenz wächst in `listOps` und hier in den Docs, nicht in immer
   größeren Tool-Beschreibungen.
2. **Analyse-Ops sind der nächste Hebel.** reaper-mcp hat sechs
   Analyse-Tools (`analyze_loudness`, `analyze_dynamics`,
   `analyze_frequency_spectrum`, `analyze_stereo_field`,
   `analyze_transients`, `detect_clipping`); unidaw hat nur `peak`. Agenten,
   die Mix- oder Abgabe-Urteile fällen, brauchen messbare Zahlen statt
   Klangbeschreibungen. Als Ops in `Processing.cpp` nachrüsten — Verarbeitung
   läuft schon headless (renderToFile-Muster). Deckt sich mit der Longlist
   (LUFS als M).
3. **Ergebnisse mit Label.** LivePilot trennt Analyse-Berichte in gemessenes
   Audio, Heuristiken und künstlerisches Urteil. Für dawds Analyse-Ops heißt
   das: messbare Felder in der Antwort, keine Prosa — Deutungen heißen so.
4. **Keine dekorativen Parameter.** LivePilot v1.30 hat dekorative Parameter
   entfernt („options that mean something"). Regel für die Op-Registry: jeder
   Parameter muss das Ergebnis ändern, sonst fliegt er.
5. **Batch-/Makro-Op bestätigt.** reaper-mcp-Agenten rufen Set-Operationen in
   Ketten auf — dawds Batch-Op spart genau diese Roundtrips. Umgesetzt
   2026-10-14: `batch {commands, stopOnError?}`.
6. **Song-Baum statt Track-Templates.** reaper-mcp nutzt Track-Template-Dateien
   (FX-Zustand, Routing, Folder-Struktur). dawds Äquivalent ist der
   Song-Baum mit Favoriten-Pfad (`treeFavorite`) — schon da, nicht doppelt
   bauen.
