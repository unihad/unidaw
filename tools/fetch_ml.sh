#!/usr/bin/env bash
# Laedt die externen Bausteine fuer dawd (nicht im Repo): ONNX Runtime (MIT)
# und Rubberband (GPL) nach dawd/third_party/, die Modelle nach dawd/models/.
# Danach dawd neu bauen (bash tests/run.sh) — CMake findet sie und schaltet
# die ML-Ops (audioToMidi, separateStems, findSamples nach Klang) und den
# Timestretcher ein.
# Idempotent.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ORT_VERSION=1.31.0
ORT_DIR="$ROOT/dawd/third_party/onnxruntime-linux-x64-$ORT_VERSION"
MODELS="$ROOT/dawd/models"
mkdir -p "$ROOT/dawd/third_party" "$MODELS"

if [ ! -f "$ORT_DIR/lib/libonnxruntime.so" ]; then
    echo "ONNX Runtime $ORT_VERSION …"
    curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-linux-x64-$ORT_VERSION.tgz" \
        | tar -xz -C "$ROOT/dawd/third_party"
fi

# Modelle (basic-pitch, HT-Demucs 4/6 Stems, CLAP): dasselbe Skript, das
# Nutzer des AppImage aufrufen — Quellen, Lizenzen und Pruefsummen stehen dort
bash "$ROOT/dawd/launcher/unidaw-fetch-models" "$MODELS"

# MVSep Mega 53 (ZFTurbo, BS-RoFormer, 53 Instrumentenklassen) fuer
# separateStems {stems: 53} und compareClasses — nur mit MEGA53=1. Lizenz:
# Gewichte MIT laut Autor (Issue #245, 2026-09-25), ohne Gewaehr fuer die
# Trainingsdaten (unbekannt) — vor einem Vertrieb klaeren. Es gibt keinen
# fertigen ONNX-Export: Checkpoint (~1,4 GB), Config und MSST-Code (MIT,
# Commit 6fdad13) laden, dann tools/export_mega53.py — braucht Python mit
# torch, einops und onnx (PYTHON=...), ~8 GB RAM; Ergebnis mega53.onnx +
# mega53.onnx.data (~2,8 GB)
if [ "${MEGA53:-0}" = 1 ] && [ ! -s "$MODELS/mega53.onnx" ]; then
    echo "mega53 (Export) …"
    M53="$ROOT/dawd/third_party/mega53"
    REL="https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/download/v1.0.21"
    mkdir -p "$M53"
    [ -s "$M53/mega53.ckpt" ] || { curl -fsSL -o "$M53/mega53.ckpt.part" "$REL/mvsep_mega_model_bs_roformer_53_stems_v1.ckpt" && mv "$M53/mega53.ckpt.part" "$M53/mega53.ckpt"; }
    echo "c62820893bbf86d4e734f966bd142d9157cfc8bb8e79e9d8f9ea553f3ff3519f  $M53/mega53.ckpt" | sha256sum -c --quiet
    [ -s "$M53/mega53.yaml" ] || curl -fsSL -o "$M53/mega53.yaml" "$REL/mvsep_mega_model_bs_roformer_53_stems.yaml"
    if [ ! -d "$M53/msst" ]; then
        git clone -q https://github.com/ZFTurbo/Music-Source-Separation-Training.git "$M53/msst"
        git -C "$M53/msst" checkout -q 6fdad13686bb7e84d029ec66f9dbe76e65c93ebf
    fi
    MSST_DIR="$M53/msst" "${PYTHON:-python3}" -I "$ROOT/tools/export_mega53.py" "$M53/mega53.ckpt" "$M53/mega53.yaml" "$MODELS/mega53.onnx"
fi

# Rubberband (Breakfast Quay, GPL — bei Closed-Source-Vertrieb kommerzielle
# Lizenz noetig): Timestretch/Pitch fuer Audio-Clips; die Engine baut die
# Single-File-Variante mit, wenn die Quellen hier liegen
RB_VERSION=v4.0.0
if [ ! -f "$ROOT/dawd/third_party/rubberband/single/RubberBandSingle.cpp" ]; then
    echo "Rubberband $RB_VERSION …"
    rm -rf "$ROOT/dawd/third_party/rubberband"
    git clone -q --depth 1 --branch "$RB_VERSION" https://github.com/breakfastquay/rubberband.git "$ROOT/dawd/third_party/rubberband"
fi

echo "ok: $ORT_DIR, $MODELS, rubberband"
