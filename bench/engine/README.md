# Engine-Benchmark

Misst die Tracktion Engine mit dawds Routing-Code (`Routing.cpp`, `Feedback.cpp`):
16 Nutzer-Spuren mit je einem 30-s-Audio-Clip, 0/1/2/4/8 Module (Built-in-Effekte)
als Kette je Spur bis Main. Modi: `tebench <dir> [render|blocks|helm|live]`.

## render (Default)

- `buildMedianMs` — Graph-Neuaufbau (`createNodeForEdit` + `prepareToPlay`), Median aus 7
- `edgeOpUs` — eine Kante im Modell (`disconnectNodes` + `connectNodes`, je Op)
- `render1ThreadMs` / `renderAllMs` — Offline-Render 30 s, Block 512, 44,1 kHz
- `rtf1` / `rtfAll` — Echtzeitfaktor (30 s / Renderzeit)

## blocks / helm — Aussetzer-Indikator

Jeder 512er-Block einzeln gestoppt (Renderer-Weg ohne Datei): Mittel, p99,
Maximum und Blöcke über dem Echtzeit-Budget (11,6 ms bei 44,1 kHz), mit 1 Thread
und allen Kernen. `helm`: N MIDI-Spuren mit je einer Helm-Instanz (LV2, aus
`~/.lv2`) und Vierklängen; `peak` prüft, dass es klingt.

## live

Spielt über das echte Gerät (ALSA → PipeWire, Master auf −100 dB, also stumm)
je 15 s: XRuns des Geräts (ALSA-Underruns) und CPU-Last der Engine
(`DeviceManager::getCpuUsage`, Mittel/Max). Achtung: Tracktion schaltet den
Ausgang stumm, wenn die CPU-Last 98 % übersteigt (`setCpuLimitBeforeMuting`).

```
cmake -S bench/engine -B bench/engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build bench/engine/build -j$(nproc)
bench/engine/build/tebench_artefacts/Release/tebench /tmp/tebench > result.csv
bench/engine/build/tebench_artefacts/Release/tebench /tmp/tebench blocks > blocks.csv
bench/engine/build/tebench_artefacts/Release/tebench /tmp/tebench helm > helm.csv
bench/engine/build/tebench_artefacts/Release/tebench /tmp/tebench live > live.csv
```

Ergebnisse vom 2026-10-14 (AMD Ryzen 7 4800H, 16 Threads) liegen als CSV daneben.

Nicht gemessen: VST3-Fremd-Plugins (keine installiert), Effekt-Plugins von
Drittherstellern, Last durch andere Programme auf dem Desktop.
