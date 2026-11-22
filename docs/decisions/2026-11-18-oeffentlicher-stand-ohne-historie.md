# Öffentlicher Stand ohne Git-Historie, ohne Diva-Katalog

- **Status:** gilt — Matthias hat am 2026-11-18 beide Vorschläge
  angenommen (Todo `todos/2026-11-18-repo-fuer-veroeffentlichung-pruefen.md`,
  Frage „weg“: frischer-stand, Frage „diva“: yes).
- **Datum:** 2026-11-18, Entwurf Claude Code

## Kontext

agent-daw soll öffentlich werden (Ziel
`agent-daw-bereit-fuer-den-vertrieb-machen`). Die Prüfung von Stand und
Historie (859 Commits, Agent Log des Todos oben) fand keine Zugangsdaten
und keine großen Dateien. Sie fand aber Material, das nicht nach außen
soll und in fast jedem Commit steckt:

- `todos/` mit internen Agent-Logs, Session-IDs und privaten Pfaden. Die
  interne Planung committet die Todos laufend nach main.
- Alte Closed-Source-Planung: `docs/vertrieb-und-verkauf.md`,
  `docs/decisions/2026-10-24-vertrieb-ruht.md`, `APP.md`.
- Der Preset-Katalog von Diva (`dawd/plugin-profiles/vst3-357abb5a-presets.json`):
  u-he, kommerziell, 1.432 Presets, davon 871 aus Dritt-Bänken, mit Namen
  und Klang-Vektoren. Die Lizenz von u-he ist nicht geprüft.

Die Historie davon zu befreien hieße, sie über alle Commits neu zu
schreiben (`git filter-repo`): main auf Forgejo, alle Worktrees, offenen
Branches und PRs wären neu aufzusetzen. AGPLv3 verlangt den Quellcode,
nicht die Historie.

## Entscheidung

1. **Frischer Stand statt ganzer Historie.** Forgejo bleibt intern, mit
   voller Historie und den Todos. Öffentlich erscheint ein Export des
   Baums von main ohne die internen Pfade; erster Commit = erste
   Version, danach je Release ein Export-Commit. Kein Spiegel, der die
   Historie von Forgejo eins zu eins überträgt.
2. **Der Diva-Preset-Katalog bleibt draußen.** Die Datei
   `vst3-357abb5a-presets.json` kommt nicht in den öffentlichen Stand.
   Die Plugins Surge, Dexed, Helm, MDA und sfizz sind frei; ihre Kataloge
   bleiben. Der Steckbrief `vst3-357abb5a.json` (gemessene Parameter, keine
   Preset-Namen) ist von der Frage nicht erfasst.

## Alternativen

- **Ganze Historie veröffentlichen:** verworfen — Todos, Vertriebsplanung
  und der Diva-Katalog stünden in jedem alten Commit, und neue Todos
  kämen über den Spiegel laufend dazu.
- **Historie umschreiben:** verworfen — bricht Forgejo-main, Worktrees
  und offene PRs, ohne dass die Lizenz etwas davon verlangt.

## Folgen

- Umsetzung (Export-Skript, Ausschlussliste, relative Pfade in
  `dawd/plugin-profiles/`, `play-src.wav` weg, Verweise auf interne
  Systeme entschärfen): Todo
  `todos/2026-11-18-oeffentlichen-stand-bereinigen.md`. Das Skript ist
  `tools/export_public.sh`: Baum von main ohne die Pfade in `EXCLUDE`
  als Commit auf den lokalen Branch `public`, erster Export ohne
  Vorgänger; es pusht nicht. Kataloge tragen Pfade relativ zum
  Preset-Ordner des Plugins.
- Der geplante Schritt „Spiegel Forgejo → öffentlich einrichten“ im Ziel
  wird zum Export-Skript.
- Der Diva-Katalog bleibt intern nutzbar. Im öffentlichen Stand fehlt er;
  wer Diva besitzt, kann den Katalog selbst erzeugen
  (`tools/profile_presets.py`).
