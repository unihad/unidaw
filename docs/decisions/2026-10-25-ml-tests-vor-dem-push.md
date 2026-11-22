# ML-Trenn-Tests vor dem Push statt bei jedem Commit

- **Status:** gilt
- **Datum:** 2026-10-25, Matthias

## Kontext

Erste Messung der Dauer je Test (Suite 15,5 min, 182 Tests, Commit
5549a37): `classes_flow` 142 s, `stems_flow` 142 s, `befunde_1e_flow` 35 s —
die Stem-Trennung mit großen ML-Modellen ist gut ein Drittel der Suite und
braucht ~10 GB RAM je Instanz. Die meisten Commits ändern keinen ML-Code.

## Entscheidung

- `tests/run_tests.py`: `ML_TESTS` = stems_flow, classes_flow,
  befunde_1e_flow, split_layer_flow. `DAWD_SKIP_ML=1` lässt sie weg,
  `DAWD_TESTS=…,ml` wählt sie aus.
- pre-commit-Hook: Ändert der Commit keine ML-Datei
  (`dawd/source/Separation.*`, `dawd/source/ops/Stems.cpp`,
  `dawd/source/ops/Voice.cpp`, `tools/fetch_ml.sh`, `dawd/CMakeLists.txt`),
  läuft die Suite ohne die ML-Tests.
- pre-push-Hook (neu): fährt vor jedem Push die ML-Tests; rot blockt den
  Push.
- Ausnahme (2026-11-09): ändern alle gepushten Commits nur `todos/` und
  `docs/`, läuft keine Suite — wie beim pre-commit-Hook
  ([Decision](2026-10-25-doku-commits-ohne-suite.md)). Der Hook liest dazu
  je Ref die Commits zwischen Remote- und lokalem Stand (stdin des
  pre-push; neuer Ref: was kein Remote kennt). Ist der Remote-Stand lokal
  unbekannt, läuft die Suite.

## Folgen

- Ein Commit ohne ML-Code spart ~5 min; ein Push dauert ~5 min länger.
- Ändert ein Commit nur die ML-Tests selbst (in `tests/run_tests.py`),
  laufen sie erst vor dem Push.
- Die Todo-Commits der internen Planung (`git push origin main`) hängen nicht
  mehr an einer roten Suite.
- Die Hooks sind nicht im Repo (`.git/hooks`); Inhalt in `AGENTS.md`.
