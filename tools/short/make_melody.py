#!/usr/bin/env python3
"""Melodie fuer die Shorts erzeugen (docs/shorts.md, Shorts 3 und 6):
8 Takte in a-Moll bei 96 BPM, gezupfter Ton (additiv: sechs Teiltoene,
die hohen klingen schneller ab), Mono-WAV 44,1 kHz 16 Bit. Ohne Zufall:
gleicher Aufruf, gleiche Datei.

    python3 tools/short/make_melody.py <ziel.wav>
"""
import math
import struct
import sys
import wave

RATE = 44100
BPM = 96
BAR_BEATS = 4
BARS = 8

# (MIDI-Note, Schlaege) je Takt — a-Moll mit Leitton Gis, Schluss auf A.
# Viel D und F, Gis vor dem A: eine Melodie aus A-C-E-G-B allein hoerte
# detectKey als E-Moll (Lauf listen 2026-11-23, Karplus-Strong-Ton; Todo
# 2026-11-23-detectkey-gezupfte-melodie)
MELODY = [
    [(69, 1), (72, 1), (76, 1), (72, 1)],
    [(74, 1), (72, 1), (69, 2)],
    [(65, 1), (69, 1), (72, 1.5), (71, 0.5)],
    [(69, 2), (64, 1), (68, 1)],
    [(69, 1), (72, 1), (76, 1.5), (74, 0.5)],
    [(77, 1), (76, 1), (74, 1), (72, 1)],
    [(71, 1), (69, 1), (68, 1), (71, 1)],
    [(69, 3), (None, 1)],
]


def pluck(freq, seconds):
    """Zupfton aus sechs Teiltoenen (Amplitude 1/h^1,5, Abklingen je h
    schneller), 4 ms Anstieg, 20 ms Ausklang. Karplus-Strong (bis
    2026-11-23) gab audioToMidi Geister am 7. Teilton (A4 -> G7; Todo
    2026-11-23-audiotomidi-obertoene-gezupft)."""
    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        v = sum(math.sin(2 * math.pi * freq * h * t) / h ** 1.5 * math.exp(-1.5 * h * t) for h in range(1, 7))
        out[i] = v * min(1.0, t / 0.004) * min(1.0, (n - i) / (0.02 * RATE))
    return out


def main(path):
    beat = 60.0 / BPM
    total = int(RATE * BARS * BAR_BEATS * beat)
    mix = [0.0] * total
    pos = 0.0
    for bar in MELODY:
        assert abs(sum(b for _, b in bar) - BAR_BEATS) < 1e-9, bar
        for note, beats in bar:
            if note is not None:
                freq = 440.0 * 2 ** ((note - 69) / 12)
                # klingt etwas ueber die Note hinaus aus, wie eine Saite
                tone = pluck(freq, beats * beat + 0.1)
                start = int(pos * beat * RATE)
                for i, v in enumerate(tone):
                    if start + i >= total:
                        break
                    mix[start + i] += v
            pos += beats
    peak = max(abs(v) for v in mix) or 1.0
    gain = 10 ** (-3 / 20) / peak   # Spitze bei -3 dBFS
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v * gain)) * 32767)) for v in mix))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
