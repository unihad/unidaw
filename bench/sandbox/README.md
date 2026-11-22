# Mess-Prototyp Plugin-Sandboxing

Kernfrage für `docs/plugin-sandboxing.md`: Passt der Weg eines Audio-Blocks
in einen anderen Prozess und zurück **im selben Block** ins Budget — also
Sandboxing ohne zusätzlichen Block Latenz?

`shmbench` startet N Worker-Prozesse (fork). Je Worker ein Slot im Shared
Memory (mmap): Steuerwörter, Eingang und Ausgang (Stereo, float). Der Host
schreibt den Block, weckt den Worker per Futex, wartet mit Frist (= Budget
des Blocks) auf „fertig“. Der Worker rechnet ein Gain plus optional eine
feste Last (`--work-us`) und meldet seine Rechenzeit. Gemessen wird der
**Aufschlag**: Rundlauf minus Rechenzeit im Worker. Verpasst ein Worker die
Frist, gibt der Host Stille aus (`missed`) und prüft, ob der Worker noch
lebt.

Kein Audio-Gerät, keine Engine, kein unidaw-Build. Ein Prozess `g++`.

```
bash bench/sandbox/run.sh > bench/sandbox/results-<datum>-<rechner>.csv
```

## Szenarien (`run.sh`)

| Name | Was |
| --- | --- |
| `inproc-512` | Vergleich: dasselbe „Plugin“ im eigenen Prozess |
| `sync-512` | 1 Worker, 512 Frames bei 44,1 kHz (Budget 11,6 ms), im Echtzeit-Takt, SCHED_FIFO |
| `sync-512-spin50` | dazu 50 µs aktives Warten vor dem Futex |
| `sync-512-nofifo` | ohne Echtzeit-Priorität |
| `sync-512-dsp2ms` | Worker rechnet 2 ms je Block |
| `chain8-512` | 8 Worker hintereinander in einem Host-Thread (Kette aus 8 Plugins) |
| `par8-512` | 8 Worker, je einer pro Host-Thread (Graph mit Thread-Pool) |
| `sync-64`, `chain8-64`, `chain8-64-spin50` | 64 Frames bei 48 kHz (live über JACK, Budget 1,33 ms) |
| `render-512` | ohne Takt, so schnell es geht (Offline-Render) |
| `crash-512` | 4 Worker, Worker 0 stirbt nach Block 200 (SIGKILL) |

## Spalten

- `medianUs`, `p99Us`, `p999Us`, `maxUs` — Aufschlag je Rundlauf (ein Plugin, ein Block).
- `blockMedianUs`, `blockP999Us`, `blockMaxUs` — Summe je Block und Host-Thread
  (bei `chain8` die ganze Kette).
- `missed` — Frist verpasst; `crashDetectMs` — Zeit vom SIGKILL bis der Host
  den toten Worker erkennt; `blocksAfterCrash` — Blöcke, die danach ohne ihn
  weiterliefen.

## Ergebnis 2026-10-25 (Ryzen 7 4800H, `results-2026-10-25-ryzen7-4800h.csv`)

Zwei andere Agents bauten parallel (loadavg 2–4); die Werte sind darum eher
pessimistisch.

- Ein Plugin, 512 Frames: Aufschlag Median 55 µs, p99,9 0,10 ms, Maximum
  0,11 ms — unter 1 % des Budgets. Ohne SCHED_FIFO bis 1 ms Maximum (8 %).
- Kette aus 8 Plugins, 512 Frames: Summe Median 0,43 ms, Maximum 1,0 ms
  (9 % des Budgets).
- 64 Frames bei 48 kHz: ein Plugin Median 39 µs, Max 0,10 ms (8 %); Kette aus
  8: Summe Median 0,13 ms, Max 0,30 ms (22 % des Budgets).
- Aktives Warten (50 µs) half nicht verlässlich (Kette 64: schlechter).
- Absturz: der Host erkennt den toten Worker nach einer Frist (11,7 ms = ein
  Block), die anderen drei laufen weiter (301 Blöcke ohne Ausfall).
- Ohne Frist keine verpassten Blöcke. Erster Lauf hatte Ausreißer genau einer
  Frist — Fehler im Prototyp (Futex wartete auf den schon gesetzten Wert),
  behoben vor diesem Lauf.

Nicht gemessen: echte Plugins (Diva, Surge), Übergabe von MIDI, Parametern
und Playhead, Last durch viele Spuren, Wayland/X11-Fenster.

## Mit unidaw und echten Plugins

- `dawd_plugins.py` (Phase 1): N × Dragonfly Room + N × Surge XT (VST3), im
  Prozess gegen Sandbox; Rohdaten `results-2026-10-25-dawd-plugins.jsonl`.
- `dawd_plugins_phase4.py` (Phase 4): VST3 und LV2 (Surge XT, Dexed, MDA
  JX10, x42-eq, Dragonfly Room), im Prozess gegen Gruppierung „je Plugin“
  und „je Modul“; Kette für den Aufschlag je Plugin und Block, `addPlugin`
  für den Host-Start; live nur mit `--live` (spielt über das Standard-Gerät).
  Rohdaten `results-2026-10-26-phase4.jsonl`, Tabelle in
  `docs/plugin-sandboxing.md`, Abschnitt 0.
