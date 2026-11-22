#!/usr/bin/env bash
# Mess-Prototyp Plugin-Sandboxing: baut shmbench (g++, ein Prozess) und faehrt
# die Szenarien aus README.md. Ausgabe: CSV auf stdout. Kein Audio-Geraet,
# keine Engine. Laufzeit ~4 min (die Bloecke laufen im Echtzeit-Takt).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="${SHMBENCH_BIN:-$here/build/shmbench}"
mkdir -p "$(dirname "$bin")"
g++ -O2 -std=c++20 -pthread "$here/shmbench.cpp" -o "$bin"

echo "# $(date -Iseconds) $(uname -r) $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs) loadavg: $(cut -d' ' -f1-3 /proc/loadavg)"
echo "name,workers,parallel,frames,rate,paced,spinUs,workUs,fifo,blocks,medianUs,p99Us,p999Us,maxUs,blockMedianUs,blockP999Us,blockMaxUs,budgetUs,missed,crashDetectMs,blocksAfterCrash"

r() { "$bin" "$@"; }

# 512 Frames bei 44,1 kHz (dawd-Standard: ALSA 512, Budget 11,6 ms)
r --name inproc-512       --workers 0 --blocks 2000 --fifo 1
r --name sync-512         --workers 1 --blocks 2000 --fifo 1
r --name sync-512-spin50  --workers 1 --blocks 2000 --fifo 1 --spin-us 50
r --name sync-512-nofifo  --workers 1 --blocks 2000 --fifo 0
r --name sync-512-dsp2ms  --workers 1 --blocks 2000 --fifo 1 --work-us 2000
r --name chain8-512       --workers 8 --blocks 2000 --fifo 1
r --name par8-512         --workers 8 --blocks 2000 --fifo 1 --parallel 1

# 64 Frames bei 48 kHz (live ueber JACK, docs/live-audio.md; Budget 1,33 ms)
r --name sync-64          --workers 1 --blocks 5000 --frames 64 --rate 48000 --fifo 1
r --name chain8-64        --workers 8 --blocks 5000 --frames 64 --rate 48000 --fifo 1
r --name chain8-64-spin50 --workers 8 --blocks 5000 --frames 64 --rate 48000 --fifo 1 --spin-us 50

# Offline-Render: Bloecke ohne Takt, so schnell es geht
r --name render-512       --workers 1 --blocks 20000 --paced 0 --fifo 0

# Absturz: Worker 0 von 4 stirbt nach Block 200 (SIGKILL), der Rest laeuft weiter
r --name crash-512        --workers 4 --blocks 500 --fifo 1 --kill-after 200
