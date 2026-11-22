# Suite parallel auf zwei dawd-Instanzen

- **Status:** gilt
- **Datum:** 2026-10-25, Matthias

## Kontext

Die Suite lief auf einer dawd-Instanz, alle Tests nacheinander: 15,5 min
mit ML-Trenn-Tests (182 Tests, Messung vom 2026-10-25), davon 5,4 min
ML-Tests. Ohne ML-Tests (pre-commit ohne ML-Code) gemessen 11 min 18 s
(179 Tests, Branch `suite-parallel`, Port 8031). Die meisten Tests legen
einen eigenen Song-Baum an und brauchen nichts von früheren Tests. Nur die
ersten Tests bauen aufeinander auf (Seed in `treeSeed_fields`, dann
Branch, Reset, Delete …); sie brauchen zusammen knapp 1 min (53 s).

## Entscheidung

- `tests/run.sh` startet die Suite in zwei Gruppen auf zwei Instanzen
  parallel: Gruppe A auf `DAWD_TEST_PORT`, Gruppe B auf `DAWD_TEST_PORT` + 2
  (+ 1 ist die Session-Instanz in session_restore_flow, + 20 audio_api_flow).
- Gruppe B steht als Liste `GROUP_B` in `tests/run_tests.py`: nur Tests,
  die allein laufen. Geprüft: Gruppe B allein vorwärts und rückwärts grün.
- Alles andere ist Gruppe A: die Kette in ihrer Reihenfolge, die
  ML-Trenn-Tests (~10 GB RAM — nie in beiden Instanzen zugleich), die
  Tests am MIDI-Through-Loopback (Aufnahme und Arm; der Loopback ist
  systemweit) und jeder neue Test, der nicht in `GROUP_B` steht.
- Ausgewogen nach gemessener Dauer: die langen Klang-Analysen (voice_*,
  timbre_punch_flow, reverb_echo_flow, drums_tempo_flow, find_repeats_flow)
  stehen in A.
- Ausgabe live mit Präfix `[A]`/`[B]`, am Ende beide Zusammenfassungen und
  alle Failures; Exit ≠ 0, wenn eine Gruppe rot ist. Ctrl-C/SIGTERM
  beendet beide Läufe samt Instanzen (SIGTERM an run_tests.py, das seine
  Instanz per Prozessgruppe beendet).
- Eine Instanz wie bisher: `DAWD_TESTS=…` (Teilmenge, auch der
  pre-push-Hook) oder `DAWD_SERIAL=1` (Notausgang). Die Hooks bleiben
  unverändert.

## Folgen

- Gemessen (Branch `suite-parallel`, beim Start keine fremde Suite):

  | Lauf | seriell | parallel |
  |---|---|---|
  | ohne ML (`DAWD_SKIP_ML=1`) | 11 min 18 s | 5 min 24 s |
  | mit ML | ~15 min 30 s (main, 182 Tests) | 10 min 34 s |

  Gruppe A / B ohne ML 322 s / 295 s, mit ML 633 s / 359 s.
  RAM-Spitze (RSS aller Test-Instanzen): ohne ML 2,8 GB, mit ML 11,9 GB.
- Mit ML-Tests ist A länger als B (die ML-Tests laufen in A obendrauf).
  Optimiert ist der häufige Fall, der Commit ohne ML-Code.
- Zwei versteckte Abhängigkeiten gefunden (Lauf rückwärts) und behoben:
  batch_flow las das ganze Event-Log der Instanz (ein Schritt `batch` aus
  recipe_flow stand darin) — jetzt nur das Log ab Testbeginn; recipe_flow
  zählte bei der Suche auch Rezepte späterer Tests — jetzt nur die eigenen.
- Ein neuer Test landet in A. In `GROUP_B` kommt er erst nach dem Lauf
  allein; Gruppe B danach vorwärts und rückwärts prüfen.
- Eine Suite belegt die Ports P bis P + 3, P + 20 und P + 22
  (P = `DAWD_TEST_PORT`), dazu UDP P + 1000 bis P + 1003 (OSC).
  Nachtrag 2026-11-09: ohne `DAWD_TEST_PORT` wählt `tests/run.sh` P selbst
  — den ersten freien aus 7411, 7441, 7471 … — und sperrt ihn für den Lauf
  (`flock`). Vorher nahmen alle Suiten 7411; liefen zwei gleichzeitig
  (Worktree und pre-push-Hook), benutzte die eine die Instanz der anderen
  und beendete sie am Ende mit `quit` (Todo
  `2026-11-09-pre-push-hook-rot-test-instanz-stirbt`).

Entscheidung Matthias 2026-10-25: Mit ML-Tests ist Gruppe A länger als B (633 s gegen 359 s) — reicht so; die Gruppen bleiben fest.
