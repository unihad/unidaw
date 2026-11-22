# Produktname unidaw

Status: gilt (Matthias, 2026-11-19)

## Kontext

Das Produkt hieß bisher nach seinem Daemon „dawd“. Vor dem ersten
öffentlichen Release wurde der Name markenrechtlich geprüft:
Paketquellen und Markt frei, Handprüfungen bei DPMA und EUIPO erledigt.
Das öffentliche Repo liegt unter der Launch-Org `unihad`.

## Entscheidung

- Das Produkt heißt **unidaw**.
- Öffentliches Repo: https://github.com/unihad/unidaw (kein eigener
  Org-Name „unidaw“).

## Folgen

- Alles, was Nutzer sehen, heißt unidaw: README und Projektdateien, Flow-UI,
  Op `about`, Binary `unidaw`, Starter `unidaw-start` und
  `unidaw-fetch-models`, AppImage `unidaw-<version>-x86_64.AppImage`,
  MCP-Server `mcp/unidaw_mcp.py` mit den Tools `unidaw_start` und
  `unidaw_command`, Claude-Code-Plugin `unidaw@unihad`, MCP-Bundle
  `unidaw-mcp-<version>.mcpb`.
- Umgebungsvariablen heißen `UNIDAW_*` (vorher `DAWD_*`), OSC-Adressen
  `/unidaw/…` (vorher `/dawd/…`).
- Ordner im Nutzerkonto: `~/.config/unidaw`, `~/.local/state/unidaw`,
  `~/.local/share/unidaw`. Gibt es nur `~/.config/dawd`, nimmt unidaw
  weiter diesen Ordner (Bibliotheken, Referenzen, Token bleiben gültig).
- Intern bleibt „dawd“, wo eine Änderung Daten oder Verhalten bräche oder
  nur Code umstellt: Quellordner `dawd/`, C++-Namespace und Klassen,
  Dateiformate (`.dawdpreset`, `.dawd-origins.json`, Spurnamen der Matrix
  in Projekten), der Wert `dawd` von `pluginHosting` (Plugin im Prozess),
  Schlüssel im Browser-Speicher der UI.
