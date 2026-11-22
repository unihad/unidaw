#!/usr/bin/env python3
"""Erzeugt das mitgelieferte Drum-Kit "basic" (dawd/kits/basic): synthetisierte
One-Shots in GM-Belegung, 44,1 kHz, 16 Bit mono, dazu kit.json. Ohne
Abhaengigkeiten und ohne fremde Samples — keine Lizenzfrage. Deterministisch
(fester Zufalls-Seed): erneutes Ausfuehren ergibt dieselben Dateien.

    python3 dawd/kits/make_basic_kit.py
"""
import json
import math
import os
import random
import struct
import wave

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "basic")
rnd = random.Random(7)


def noise(n):
    return [rnd.random() * 2 - 1 for _ in range(n)]


def biquad(x, kind, f, q=0.707):
    """RBJ-Biquad: lp, hp oder bp."""
    w = 2 * math.pi * f / RATE
    c, s = math.cos(w), math.sin(w)
    a = s / (2 * q)
    if kind == "lp":
        b0, b1, b2 = (1 - c) / 2, 1 - c, (1 - c) / 2
    elif kind == "hp":
        b0, b1, b2 = (1 + c) / 2, -(1 + c), (1 + c) / 2
    else:
        b0, b1, b2 = a, 0.0, -a
    a0, a1, a2 = 1 + a, -2 * c, 1 - a
    y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for v in x:
        o = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, v, y1, o
        y.append(o)
    return y


def env(n, decay, attack=0.001):
    a = max(1, int(attack * RATE))
    return [min(1.0, i / a) * math.exp(-i / RATE / decay) for i in range(n)]


def sweep(n, f0, f1, rate_s, decay):
    out, ph = [], 0.0
    for i in range(n):
        t = i / RATE
        ph += 2 * math.pi * (f1 + (f0 - f1) * math.exp(-t / rate_s)) / RATE
        out.append(math.sin(ph) * math.exp(-t / decay))
    return out


def mix(*parts):
    n = max(len(p) for p in parts)
    return [sum(p[i] for p in parts if i < len(p)) for i in range(n)]


def scale(x, g):
    return [v * g for v in x]


def mul(a, b):
    return [u * v for u, v in zip(a, b)]


def kick(f0=150, f1=48, decay=0.45):
    n = int(decay * 4 * RATE)
    click = mul(biquad(noise(int(0.004 * RATE)), "hp", 3000), env(int(0.004 * RATE), 0.001))
    return mix(scale(sweep(n, f0, f1, 0.03, decay), 0.95), scale(click, 0.3))


def snare(tone=185, bright=1500, decay=0.18):
    n = int(0.5 * RATE)
    body = mix(scale(sweep(int(0.25 * RATE), tone * 1.4, tone, 0.01, 0.09), 0.5),
               scale(sweep(int(0.25 * RATE), tone * 2.4, tone * 1.8, 0.01, 0.06), 0.25))
    rattle = mul(biquad(biquad(noise(n), "hp", bright), "lp", 9000), env(n, decay))
    return mix(body, scale(rattle, 0.7))


def clap():
    n = int(0.4 * RATE)
    src = biquad(noise(n), "bp", 1200, 1.2)
    e = [0.0] * n
    for k, d in enumerate((0.0, 0.011, 0.022)):
        s0 = int(d * RATE)
        for i in range(s0, n):
            e[i] = max(e[i], math.exp(-(i - s0) / RATE / (0.008 if k < 2 else 0.14)))
    return scale(mul(src, e), 2.2)


def hat(decay, hp=7000):
    n = int(max(0.12, decay * 5) * RATE)
    metal = mix(*[[math.copysign(1, math.sin(2 * math.pi * f * i / RATE)) for i in range(n)]
                  for f in (205, 304, 369, 522, 540, 800)])
    src = mix(scale(noise(n), 1.0), scale(metal, 0.15))
    return scale(mul(biquad(biquad(src, "hp", hp), "hp", hp), env(n, decay)), 0.8)


def tom(f):
    return scale(mix(sweep(int(0.9 * RATE), f * 1.6, f, 0.04, 0.28),
                     scale(mul(biquad(noise(int(0.1 * RATE)), "lp", 3000), env(int(0.1 * RATE), 0.02)), 0.15)), 0.9)


def cymbal(decay, hp, ping=0.0):
    n = int(decay * 3 * RATE)
    metal = mix(*[[math.copysign(1, math.sin(2 * math.pi * f * i / RATE)) for i in range(n)]
                  for f in (310, 457, 563, 711, 893, 1077)])
    src = mix(noise(n), scale(metal, 0.4))
    out = scale(mul(biquad(src, "hp", hp), env(n, decay, 0.002)), 0.55)
    if ping:
        out = mix(out, scale(mul([math.sin(2 * math.pi * 3150 * i / RATE) for i in range(n)], env(n, 0.5)), ping))
    return out


def cowbell():
    n = int(0.5 * RATE)
    sq = mix(*[[0.5 * math.copysign(1, math.sin(2 * math.pi * f * i / RATE)) for i in range(n)] for f in (545, 815)])
    return scale(mul(biquad(sq, "bp", 700, 1.5), env(n, 0.12)), 1.6)


def shaker(decay=0.05, attack=0.015):
    n = int(0.25 * RATE)
    return scale(mul(biquad(noise(n), "hp", 5000), env(n, decay, attack)), 0.7)


def tambourine():
    n = int(0.4 * RATE)
    jingle = [1 + 0.6 * math.sin(2 * math.pi * 23 * i / RATE) for i in range(n)]
    return scale(mul(mul(biquad(noise(n), "hp", 6000), env(n, 0.12)), jingle), 0.6)


def rim():
    n = int(0.12 * RATE)
    return mix(scale(mul([math.sin(2 * math.pi * 1700 * i / RATE) for i in range(n)], env(n, 0.012)), 0.6),
               scale(mul(biquad(noise(n), "bp", 2500, 2), env(n, 0.008)), 0.6))


# GM-Belegung (General MIDI Percussion Key Map)
SOUNDS = [
    (35, "kick deep", lambda: kick(120, 40, 0.6)),
    (36, "kick", kick),
    (37, "side stick", rim),
    (38, "snare", snare),
    (39, "clap", clap),
    (40, "snare bright", lambda: snare(210, 2500, 0.14)),
    (41, "tom floor low", lambda: tom(78)),
    (42, "hihat closed", lambda: hat(0.045)),
    (43, "tom floor high", lambda: tom(92)),
    (44, "hihat pedal", lambda: hat(0.07, 6000)),
    (45, "tom low", lambda: tom(110)),
    (46, "hihat open", lambda: hat(0.35)),
    (47, "tom low mid", lambda: tom(130)),
    (48, "tom high mid", lambda: tom(155)),
    (49, "crash", lambda: cymbal(1.4, 4000)),
    (50, "tom high", lambda: tom(185)),
    (51, "ride", lambda: cymbal(1.2, 6000, ping=0.15)),
    (54, "tambourine", tambourine),
    (56, "cowbell", cowbell),
    (70, "shaker", shaker),
]


def write(path, x):
    peak = max(abs(v) for v in x) or 1.0
    g = 0.89 / peak   # -1 dBFS
    # Ende: bis -70 dB kuerzen, 5 ms Ausblenden
    end = len(x)
    while end > 1 and abs(x[end - 1]) * g < 3e-4:
        end -= 1
    x = x[:end + int(0.005 * RATE)]
    fade = int(0.005 * RATE)
    for i in range(min(fade, len(x))):
        x[len(x) - 1 - i] *= i / fade
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v * g)) * 32767)) for v in x))


HATS = (42, 44, 46)


def main():
    os.makedirs(OUT, exist_ok=True)
    kit = []
    for note, name, fn in SOUNDS:
        file = f"{note:03d} {name}.wav"
        write(os.path.join(OUT, file), fn())
        sound = {"note": note, "name": name, "file": file}
        if note in HATS:
            sound["choke"] = 1   # geschlossene/getretene Hat stoppt die offene
        kit.append(sound)
    with open(os.path.join(OUT, "kit.json"), "w") as f:
        json.dump({"name": "basic", "description": "dawd basic kit — synthesized one-shots, GM layout",
                   "sounds": kit}, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
