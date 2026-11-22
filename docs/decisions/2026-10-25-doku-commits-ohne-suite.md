# Commits nur in `todos/` und `docs/` ohne Test-Suite

- **Status:** gilt
- **Datum:** 2026-10-25, Matthias

## Kontext

Der pre-commit-Hook (`.git/hooks/pre-commit`, nur lokal) fährt bei jedem
Commit `tests/run.sh`: Bauen und die ganze Suite, rund 15 Minuten (181
Tests, eine dawd-Instanz, nacheinander). Seit 2026-10-20 haben 53 von 156
Commits in main keinen Code geändert, nur Todos oder Doku.

## Entscheidung

Ändert ein Commit nur Dateien unter `todos/` und `docs/`, überspringt der
Hook die Suite. Jeder andere Commit fährt weiter die ganze Suite.

## Folgen

- Todo- und Decision-Commits sind sofort fertig.
- Der Hook ist nicht im Repo; ein neuer Klon braucht ihn von Hand (Inhalt
  siehe `AGENTS.md`, Abschnitt Tests).
