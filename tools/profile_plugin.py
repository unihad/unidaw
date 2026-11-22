#!/usr/bin/env python3
"""Steckbrief eines Plugins messen: was jeder Parameter hörbar tut.

    tools/profile_plugin.py "Surge XT" --format VST3 [--port 7461] [--query cutoff] [--max 400]
    tools/profile_plugin.py --core            # das ganze Kernset (dawd/plugin-profiles/coreset.json)

Startet eine eigene dawd-Instanz (--interface-sim --no-audio, unter bwrap ohne
/dev/snd: fasst weder das Fireface noch /dev/snd/seq an; eigener Config-Ordner) oder
nutzt --port einer laufenden. Je Plugin:

- Testsignal: Instrumente spielen die Testphrase (Version 3, 2026-10-27):
  Legato-Linie mit Überlappung (Glide bei Mono/Portamento), langer Ton
  (Filter-Hüllkurve, Vibrato, LFO, Schwebung), Staccato in zwei
  Velocity-Stufen, Akkord mit Bass; Effekte bekommen Rauschen, Klicks,
  Rausch-Bursts mit Pegelsprüngen und einen Sinus-Sweep (danach Stille,
  damit Hall/Delay-Fahnen messbar sind).
- Effekte: zusätzlich aus den Bursts burstOnset (Einsatz gegen den Rest des
  lauten Teils), burstEarly/burstLate (leiser Teil gleich nach dem Abfall
  bzw. kurz vor dem nächsten Burst, gegen den lauten) — Attack/Release von
  Kompressor und Limiter, Hold/Hysterese eines Gates.
- Instrumente: zusätzlich describeTimbre {voice} (dieselbe Messung wie
  matchSound/tuneSound): sustain, sweep (Filterfahrt im Ton), vibrato,
  beating (Pegelschwankung im Ton), correlation; aus dem Render: velocity
  (Pegel der lauten gegen die leisen Staccato-Töne), glide (Anteil der
  Legato-Übergänge mit Portamento) und detune (Breite der Teiltöne im
  langen Ton, Cent) — Todo 2026-10-27-detune-glide-messung.
- Je Parameter fünf Werte (0, .25, .5, .75, 1), jeweils rendern und mit dawd
  messen (analyze {file}: LUFS, Spektrum, Stereo); Hüllkurve (Attack,
  Ausklingen) aus dem Render. Danach zurück auf den Ausgangswert.
- Gleichartige Parameter (Ziffern, Szene A/B) werden einmal gemessen, die
  anderen verweisen darauf (sameAs).
- Parameter ohne messbare Wirkung werden erneut gemessen, nachdem passende
  Schalter (On/Enable/Active, bei 4OSC die Einstellungen) eingeschaltet sind:
  needs = was dafür an sein muss. Ebenso Mengen-Regler in Neutralstellung
  (aufdrehen) und Schwellen auf der falschen Seite des Testsignals (über
  0.2 tief stellen: Ratio, Attack, Release eines Kompressors wirken sonst
  nicht; darunter hoch stellen: ein Gate schließt sonst nie).

Ergebnis: dawd/plugin-profiles/<format>-<uid>.json — pluginInfo zeigt es
unter "measured" und je Parameter als "does"/"needs". measured.version =
VERSION bei Instrumenten (2: neue Phrase und Schalter-Erkennung, 3: Pause vor
dem Staccato), EFFECT_VERSION bei Effekten (4: Bursts); --skip-current lässt
Steckbriefe mit ihrer aktuellen Version aus. Ein
Lauf schreibt alle 10 Parameter <slug>.partial.json und setzt dort fort.
"""
import argparse
import datetime
import json
import math
import os
import random
import re
import secrets
import signal
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import wave

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
UNIDAW_BIN = os.path.join(ROOT, "dawd", "build", "unidaw_artefacts", "Release", "unidaw")
PROFILES = os.path.join(ROOT, "dawd", "plugin-profiles")
# Wohin die Ergebnisse gehen (Probeläufe: UNIDAW_PROFILE_OUT=<Ordner>, sonst
# die Steckbriefe des Repos, aus dem das Werkzeug läuft)
OUT = os.environ.get("UNIDAW_PROFILE_OUT") or PROFILES
POINTS = [0.0, 0.25, 0.5, 0.75, 1.0]

# Schwellen: ab hier gilt eine Änderung als hörbar (Einheit je Messgröße)
THRESH = {"lufs": 1.0, "brightness": 0.25, "low": 2.0, "high": 2.0, "width": 3.0,
          "tail": 0.15, "attack": 25.0, "pitch": 0.5,
          # Version 2 (Instrumente, aus describeTimbre; Schwellen gewählt):
          "sustain": 3.0, "sweep": 0.15, "glide": 0.5, "vibrato": 15.0, "beating": 2.0,
          "correlation": 0.2, "velocity": 2.0,
          # glide: zwei der vier Legato-Übergänge; detune: Renders desselben
          # Presets streuen ~1 ct (Todo 2026-10-27)
          "detune": 5.0,
          # Effekte, Bursts (Entscheidung Matthias 2026-10-27; wie lufs 1 dB)
          "burstOnset": 1.0, "burstEarly": 1.0, "burstLate": 1.0}
VERSION = 3
# Effekt-Steckbriefe: 4 = Testsignal mit Bursts (2026-10-27). Ältere
# Effekt-Steckbriefe tragen keine oder Version 3
EFFECT_VERSION = 4
FLOOR = -40.0   # Bänder-/Breitenanteile darunter zählen als "nichts" (sonst rauscht fast leere Energie in dB)
UNITS = {"lufs": "LU", "brightness": "oct", "low": "dB", "high": "dB", "width": "dB",
         "tail": "s", "attack": "ms", "pitch": "st",
         "sustain": "dB", "sweep": "oct", "glide": "share", "vibrato": "ct", "beating": "dB",
         "correlation": "", "velocity": "dB", "detune": "ct",
         "burstOnset": "dB", "burstEarly": "dB", "burstLate": "dB"}

# Testphrase der Instrumente, Version 3 (120 BPM, Beat = 0,5 s). Todo
# 2026-10-18 P2: die alte Phrase (Akkord, kurzer und langer Ton) regte
# Portamento, Unison, LFOs und Filter-Hüllkurven kaum an.
#   0–2   Legato-Linie C4 E4 G4 E4, je 0,55 Beat (25 ms Überlappung)
#   2–4   langer Ton A3 (1 s), dann 0,25 s Pause
#   4,5–5,5 Staccato C5, Velocity 50 50 120 120
#   5,5–7,5 Akkord C3 E3 G3 mit Bass C2
# Version 3 (Entscheidung Matthias 2026-10-27): in Version 2 endete A3 (1,25 s)
# genau beim ersten Staccato, und laut folgte auf leise — die Fahnen lagen in
# den leisen Fenstern, velocity maß 4OSC mit 12,6 statt 22,6 dB
PHRASE = ([{"pitch": p, "beat": i * 0.5, "beats": 0.55, "velocity": 100} for i, p in enumerate((60, 64, 67, 64))]
          + [{"pitch": 57, "beat": 2.0, "beats": 2.0, "velocity": 100}]
          + [{"pitch": 72, "beat": 4.5 + i * 0.25, "beats": 0.15, "velocity": v} for i, v in enumerate((50, 50, 120, 120))]
          + [{"pitch": p, "beat": 5.5, "beats": 2.0, "velocity": 90} for p in (36, 48, 52, 55)])
PHRASE_STIM = (0.0, 3.75)
# Attack nur am ersten Ton (bis zum Einsatz des zweiten, 0,25 s): danach
# summieren sich die überlappenden Legato-Töne — mit langem Release stieg
# die Spitze bis zum vierten Ton, "attack" maß dann 500 ms (Amp Release)
PHRASE_ATTACK = 0.25
PHRASE_SIGNAL = ("MIDI v3: legato line C4 E4 G4 E4 (overlapping), long A3 1 s, rest 0.25 s, staccato C5 velocity 50 50 120 120, "
                 "chord C2+C3 E3 G3 1 s (120 BPM)")
STACCATO = [(2.25 + i * 0.125, v) for i, v in enumerate((50, 50, 120, 120))]   # Sekunden, Velocity
SKIP = re.compile(r"^(dry level|wet level|bypass|m\d+:|midi cc|program|pitch ?bend|mod ?wheel|aftertouch)"
                  # nur Anzeige (LSP: Overlays, Graphen, Pegel-Sichtbarkeit; x42: Peak-Hold)
                  r"|^(show |pause graph|clear graph|reset peak|graph zoom|fft reactivity|filter select|inspect"
                  r"|automatically inspect|quantize note|hue \d)|visibility (left|right)$|graph enable (left|right)$"
                  # Surge XT Effects: Bedeutung hängt von FX Type ab — Sweep sagt nichts
                  r"|^fx parameter \d+$", re.I)
SWITCH = re.compile(r"\b(on|enable|enabled|active|power)\b", re.I)


def switch_name(name):
    """Schalter-Name auch mit "_" (Helm: filter_on, reverb_on)."""
    return bool(SWITCH.search(name.replace("_", " ")))


# Preset-Ordner der Plugins wie in dawd/source/Presets.cpp (presetsFor). Kataloge
# speichern Pfade relativ dazu: kein Benutzername im Repo, und der Katalog passt
# auf jedem Rechner (dawd sucht die Datei beim Laden: matchesCatalogPath).
PRESET_ROOTS = [os.path.expanduser(p) for p in (
    "~/.local/share/surge-xt", "/usr/share/surge-xt", "/usr/local/share/surge-xt", "~/Documents/Surge XT",
    "~/.local/share/DigitalSuburban/Dexed", "~/Documents/DigitalSuburban/Dexed",
    "~/.u-he", "~/.vst3/presets", "/usr/share/vst3/presets", "/usr/local/share/vst3/presets",
    "~/sfz", "~/.local/share/sfz", "~/Documents/SFZ")]


def catalog_path(path):
    """Preset-Datei für den Katalog: relativ zum Preset-Ordner des Plugins
    (patches_factory/..., Cartridges/..., <Ordner>/x.sfz). Außerhalb der
    bekannten Ordner: Elternordner + Dateiname."""
    for root in PRESET_ROOTS:
        if path.startswith(root + os.sep):
            return os.path.relpath(path, root)
    if not os.path.isabs(path):
        return path
    return os.path.join(os.path.basename(os.path.dirname(path)), os.path.basename(path))


class Dawd:
    def __init__(self, port):
        self.base = f"http://127.0.0.1:{port}"
        self.port = port

    def token(self):
        """Token der localhost-API: UNIDAW_TOKEN (eigene Instanz, start_dawd),
        sonst die Datei einer laufenden dawd mit Standard-Config."""
        if os.environ.get("UNIDAW_TOKEN"):
            return os.environ["UNIDAW_TOKEN"]
        try:
            with open(os.path.expanduser(f"~/.config/unidaw/api-token-{self.port}")) as f:
                return f.read().strip()
        except OSError:
            return ""

    def __call__(self, op, **kw):
        req = urllib.request.Request(self.base + "/command", data=json.dumps({"op": op, **kw}).encode(),
                                     headers={"Content-Type": "application/json",
                                              "Authorization": "Bearer " + self.token()})
        with urllib.request.urlopen(req, timeout=900) as r:
            return json.load(r)


def free_port(start):
    """Erster freier Port ab start: zwei Läufe dürfen sich keine dawd teilen
    (sonst drehen beide an denselben Parametern). Messwerkzeuge starten bei
    7700 — 74xx gehört den Suiten (UNIDAW_TEST_PORT 7411/7451 und +1/+20)."""
    for port in range(start, start + 40):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError(f"no free port from {start}")


def start_dawd(port, workdir):
    # eigene Instanz: Token vorgeben (erbt sie, Dawd.token liest ihn)
    os.environ.setdefault("UNIDAW_TOKEN", secrets.token_hex(32))
    cfg = os.path.join(workdir, "config")
    os.makedirs(cfg, exist_ok=True)
    cmd = [UNIDAW_BIN, "--port", str(port), "--config", cfg, "--interface-sim", "--no-audio",
           "--log", os.path.join(workdir, "events.jsonl")]
    # --no-audio öffnet kein Gerät, die Engine zählt beim Start aber trotzdem
    # MIDI (/dev/snd/seq) und Karten auf — ein offenes seq blockiert das
    # Neuladen des Fireface-Treibers. bwrap blendet /dev/snd ganz aus.
    if shutil.which("bwrap"):
        cmd = ["bwrap", "--dev-bind", "/", "/", "--tmpfs", "/dev/snd", "--die-with-parent"] + cmd
    proc = subprocess.Popen(cmd,
                            cwd=workdir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(100):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=1)
            return proc
        except Exception:
            time.sleep(0.2)
    proc.kill()
    sys.exit("dawd did not come up")


# Bursts im Effekt-Testsignal (Version 4, Entscheidung Matthias 2026-10-27):
# ab BURST_START je BURST_CYCLE s ein lauter Teil (BURST_LOUD s, Pegel wie das
# Rauschen am Anfang) und ein leiser Teil BURST_QUIET_DB darunter. Ohne
# Pegelsprünge zeigten Limiter-Release und Gate-Hold/-Hysterese keine
# Wirkung: stetiges Rauschen regelt der Limiter gleichmäßig, das Gate bleibt
# offen. Die Bursts liegen vor dem Sweep: das Signal endet wie in Version 3
# mit dem Sweep bis 8 kHz (Dragonfly High Cross/High Mult wirken auf die
# Fahne der Höhen; mit den Bursts am Ende verloren sie ihre Wirkung)
EFFECT_LENGTH = 5.0
BURST_START, BURST_CYCLE, BURST_LOUD, BURSTS, BURST_QUIET_DB = 1.5, 0.25, 0.1, 4, -40.0
SWEEP_START = BURST_START + BURSTS * BURST_CYCLE
EFFECT_STIM = (0.0, SWEEP_START + 1.0)
EFFECT_SIGNAL = ("audio v4: pink noise 1 s, clicks, 4 noise bursts (100 ms loud, 150 ms 40 dB quieter), "
                 "sine sweep 80 Hz–8 kHz, then silence")


def burst_samples(rate):
    """Die Bursts ab BURST_START: eigenes Rauschen (Seed 2), damit Rauschen
    und Klicks wie in Version 3 bleiben; burst_db sucht damit den Versatz im
    Render."""
    rnd = random.Random(2)
    b = [0.0] * 3
    quiet = 10 ** (BURST_QUIET_DB / 20)
    out = []
    for i in range(int(BURSTS * BURST_CYCLE * rate)):
        w = rnd.uniform(-1, 1)
        b[0] = 0.997 * b[0] + 0.029591 * w
        b[1] = 0.985 * b[1] + 0.032534 * w
        b[2] = 0.950 * b[2] + 0.048056 * w
        loud = (i / rate) % BURST_CYCLE < BURST_LOUD
        out.append(0.9 * (b[0] + b[1] + b[2] + 0.2 * w) * (1.0 if loud else quiet))
    return out


def write_effect_signal(path, rate=44100):
    """0–1 s rosa Rauschen, 1–1,5 s Klicks, 1,5–2,5 s Bursts (BURST_*),
    2,5–3,5 s Sinus-Sweep 80 Hz–8 kHz, dann Stille. Rauschen, Klicks und
    Sweep sind dieselben wie in Version 3, der Sweep 1 s später."""
    random.seed(1)
    n = int(EFFECT_LENGTH * rate)
    out = [0.0] * n
    b = [0.0] * 3
    for i in range(int(1.0 * rate)):
        w = random.uniform(-1, 1)
        b[0] = 0.997 * b[0] + 0.029591 * w
        b[1] = 0.985 * b[1] + 0.032534 * w
        b[2] = 0.950 * b[2] + 0.048056 * w
        out[i] = 0.9 * (b[0] + b[1] + b[2] + 0.2 * w)
    for k in range(4):
        s0 = int((1.0 + k * 0.125) * rate)
        for i in range(int(0.004 * rate)):
            out[s0 + i] += 0.8 * math.exp(-i / (0.001 * rate)) * (1 if i % 2 else -1)
    s0, length = int(SWEEP_START * rate), int(1.0 * rate)
    phase = 0.0
    for i in range(length):
        f = 80 * (8000 / 80) ** (i / length)
        phase += 2 * math.pi * f / rate
        out[s0 + i] += 0.4 * math.sin(phase)
    s0 = int(BURST_START * rate)
    for i, v in enumerate(burst_samples(rate)):
        out[s0 + i] = v
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in out))


def read_wav(path):
    try:
        import numpy as np
    except ImportError:
        np = None
    if np is not None:
        with wave.open(path) as w:
            ch, rate, n, sw = w.getnchannels(), w.getframerate(), w.getnframes(), w.getsampwidth()
            raw = w.readframes(n)
        if sw == 3:
            b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
            v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
            v = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / 8388608.0
        elif sw == 2:
            v = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
        else:
            v = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
        return v.reshape(-1, ch).mean(axis=1).tolist(), rate
    with wave.open(path) as w:
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        sw = w.getsampwidth()
        raw = w.readframes(n)
    if sw == 2:
        vals = struct.unpack("<%dh" % (len(raw) // 2), raw)
        scale = 32768.0
    elif sw == 3:
        vals = [int.from_bytes(raw[i:i + 3], "little", signed=True) for i in range(0, len(raw), 3)]
        scale = 8388608.0
    else:
        vals = struct.unpack("<%di" % (len(raw) // 4), raw)
        scale = 2147483648.0
    mono = [sum(vals[i + c] for c in range(ch)) / (ch * scale) for i in range(0, len(vals), ch)]
    return mono, rate


def envelope_stats(path, stim_start, stim_end, attack_window=1.0):
    """Attack (ms bis -3 dB unter dem Spitzenwert ab Einsatz, innerhalb von
    attack_window s) und Ausklingen (s ab Ende der Anregung bis 50 dB unter
    der Spitze)."""
    x, rate = read_wav(path)
    hop = int(0.005 * rate)
    env = []
    for i in range(0, len(x) - hop, hop):
        seg = x[i:i + hop]
        env.append(math.sqrt(sum(v * v for v in seg) / hop) + 1e-12)
    db = [20 * math.log10(e) for e in env]
    if not db or max(db) < -90:
        return {"attack": None, "tail": None, "peak": -100.0}
    peak = max(db)
    i0 = int(stim_start / 0.005)
    onset = next((i for i in range(i0, len(db)) if db[i] > peak - 40), i0)
    # Attack nur im ersten Einsatz (attack_window ab dem Onset, eigene Spitze) — sonst
    # zählte ein späterer, lauterer Ton als "Attack"
    n = int(round(attack_window / 0.005))
    win = db[onset:onset + n]
    local = max(win) if win else peak
    top = next((i for i in range(onset, min(len(db), onset + n)) if db[i] > local - 3), onset)
    end = int(stim_end / 0.005)
    last = end
    for i in range(end, len(db)):
        if db[i] > peak - 50:
            last = i
    return {"attack": round((top - onset) * 5.0, 1), "tail": round((last - end) * 0.005, 2), "peak": round(peak, 1)}


def burst_lag(x, rate, lo=-0.2, hi=0.25):
    """Versatz der Bursts im Render (s): Kreuzkorrelation der Wellenform mit
    den Bursts der Quelle (burst_samples), lo … hi. Ein Versatz um eine ganze
    Periode passt nur auf drei der vier Bursts und verliert. Ohne klare
    Spitze (über 8x dem Median, z. B. Hall 100 % nass) oder ohne numpy: 0."""
    try:
        import numpy as np
    except ImportError:
        return 0.0
    src = np.asarray(burst_samples(rate))
    a = int((BURST_START + lo) * rate)
    seg = np.asarray(x[max(0, a):a + len(src) + int((hi - lo) * rate)], dtype=float)
    if a < 0 or len(seg) < len(src) + int((hi - lo) * rate) or not seg.any():
        return 0.0
    n = 1 << int(math.ceil(math.log2(len(seg) + len(src))))
    corr = np.abs(np.fft.irfft(np.fft.rfft(seg, n) * np.conj(np.fft.rfft(src, n)), n)[:len(seg) - len(src) + 1])
    i = int(np.argmax(corr))
    if corr[i] <= 8 * float(np.median(corr)):
        return 0.0
    return (i + a) / rate - BURST_START


def burst_db(path):
    """Bursts des Effekt-Testsignals, je Burst gegen den lauten Teil
    (50–100 ms nach dem Einsatz), gemittelt, dB: burstOnset = erste 5 ms
    (Attack: ein langsamer Kompressor lässt den Einsatz durch), burstEarly =
    5–60 ms nach dem Abfall (Release: die Regelung erholt sich noch; Gate:
    Release), burstLate = 90–150 ms nach dem Abfall (Gate zu oder per Hold/
    Hysterese noch offen; Hall/Delay füllen die Lücke).
    Die Fenster folgen dem Versatz der Bursts im Render (burstLag, ms):
    Latenz-Ausgleich hängt beim Render einen Schritt nach — nach einer
    Latenz-Änderung (LSP Limiter Lookahead, LSP-EQ Equalizer mode zurück auf
    IIR) lag der Render um -93 … +10 ms daneben, die Fenster maßen sonst
    Unsinn (burst_lag)."""
    x, rate = read_wav(path)
    pre = [0.0]
    for v in x:
        pre.append(pre[-1] + v * v)

    def energy(a, b):
        i, j = max(0, int(a * rate)), max(0, min(len(x), int(b * rate)))
        return pre[j] - pre[i] if j > i else 0.0

    def db(a, b):
        return 10 * math.log10(energy(a, b) / max(1, int(b * rate) - int(a * rate)) + 1e-18)

    starts = [BURST_START + k * BURST_CYCLE for k in range(BURSTS)]
    lag = burst_lag(x, rate)
    out = {"burstOnset": [], "burstEarly": [], "burstLate": []}
    for c0 in starts:
        c = c0 + lag
        loud = db(c + 0.05, c + BURST_LOUD)
        out["burstOnset"].append(db(c, c + 0.005) - loud)
        out["burstEarly"].append(db(c + BURST_LOUD + 0.005, c + BURST_LOUD + 0.06) - loud)
        out["burstLate"].append(db(c + BURST_LOUD + 0.09, c + BURST_CYCLE) - loud)
    # ganz stumm (Gate zu, Reduction -inf) zählt als -90 dB, nicht -180
    res = {k: round(sum(max(-90.0, d) for d in v) / len(v), 2) for k, v in out.items()}
    res["burstLag"] = round(lag * 1000)
    return res


def velocity_db(path):
    """Pegel der Staccato-Töne mit Velocity 120 gegen die mit 50 (je die
    ersten 60 ms, dB) — wie stark das Instrument auf Velocity reagiert."""
    x, rate = read_wav(path)

    def rms(t):
        seg = x[int(t * rate):int((t + 0.06) * rate)]
        return math.sqrt(sum(v * v for v in seg) / max(1, len(seg))) + 1e-9

    hi = [rms(t) for t, v in STACCATO if v > 100]
    lo = [rms(t) for t, v in STACCATO if v <= 100]
    return round(20 * math.log10((sum(hi) / len(hi)) / (sum(lo) / len(lo))), 2)


# Langer Ton A3 (Einsatz 1,0 s) und die Legato-Übergänge der Phrase (alte
# Note, neue Note, Einsatz, nächster Einsatz in s) — für detune und glide
# (Todo 2026-10-27-detune-glide-messung)
# Messfenster im langen Ton: 0,25 s nach dem Einsatz (ein langsames
# Portamento ist dann angekommen) bis 0,05 s vor dem Loslassen
LONG_NOTE = next((n["beat"] * 0.5 + 0.25, (n["beat"] + n["beats"]) * 0.5 - 0.05) for n in PHRASE if n["pitch"] == 57)
LEGATO = [(60, 64, 0.25, 0.5), (64, 67, 0.5, 0.75), (67, 64, 0.75, 1.0), (64, 57, 1.0, 1.4)]


def detune_cents(x, rate):
    """Verstimmung (Unison-Detune, Chorus, verstimmte Oszillatoren) im langen
    Ton A3 (LONG_NOTE):
    Spektrum auf ein 1-Cent-Raster von 600 Hz bis 2 kHz (dort liegen die
    Teiltöne weit genug auseinander), Gipfel bis 30 dB unter dem stärksten
    und mindestens 15 dB über dem Rauschboden; je Gipfel die halbe Breite
    (Cent) der mittleren 80 % der Energie in ±50 ct, Median. Verstimmung
    verbreitert jeden Teilton um gleich viele Cent, Pegelbewegung (Tremolo,
    Abklingen) nur um wenige Hz. Ein Oszillator ~1 ct, Surge Unison Detune
    5/15 ct → ~5/15. Vibrato zählt mit. None ohne zwei klare Gipfel
    (Rauschen, Schlagzeug, verklungen) oder ohne numpy. Die Pegel-Schwebung
    beatingDb (describeTimbre) trennt verstimmte Klänge nicht: sie misst vor
    allem das Ausklingen am Notenende. Gegenstück in dawd: partialWidths
    (VoiceAnalysis.cpp) → describeTimbre ensemble.detuneCents, dort mit
    Fenstern aus den erkannten Noten (4OSC-Phrase: 1,5/6/13 gegen hier
    1/6/13). Eine Änderung der Methode gehört in beide."""
    try:
        import numpy as np
    except ImportError:
        return None
    seg = np.asarray(x[int(LONG_NOTE[0] * rate):int(LONG_NOTE[1] * rate)], dtype=float)
    if len(seg) < rate // 2:
        return None
    nfft = 1 << int(math.ceil(math.log2(len(seg) * 4)))
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft)) ** 2
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    lo, half = 600.0, 50
    m = (freqs >= lo * 2 ** (-half / 1200)) & (freqs < 2000.0 * 2 ** (half / 1200))
    cents = np.round(1200 * np.log2(freqs[m] / lo)).astype(int) + half
    raw = np.bincount(cents, weights=spec[m])
    grid = np.maximum(0.0, raw - 2.0 * np.percentile(raw, 25))
    if grid.sum() <= 0:
        return None
    box = np.ones(41)
    sm, rsm = np.convolve(grid, box, "same"), np.convolve(raw, box, "same")
    floor = np.percentile(rsm, 25)
    widths = []
    for i in range(half, len(sm) - half):
        if sm[i] >= sm.max() * 1e-3 and sm[i] == sm[i - half:i + half + 1].max() and rsm[i] >= floor * 10 ** 1.5:
            cum = np.cumsum(grid[i - half:i + half + 1])
            cum /= cum[-1]
            widths.append((np.searchsorted(cum, 0.9) - np.searchsorted(cum, 0.1)) / 2)
    return round(float(np.median(widths)), 1) if len(widths) >= 2 else None


def legato_glide(x, rate):
    """Glide in den vier Legato-Übergängen der Phrase: 5-ms-Schritte vom
    Einsatz bis zum nächsten; Tonhöhe je Schritt = Maximum der Summe über 8
    Harmonische (Fenster 23 ms) auf einem 10-Cent-Raster von der alten bis
    zur neuen Note. Ein Übergang gleitet, wenn mindestens 3 hörbare Schritte
    (15 ms) am Stück mindestens 80 ct von beiden Noten entfernt dazwischen
    liegen und dabei stetig zur neuen Note laufen (je Schritt 0–150 ct,
    zusammen ≥ 50 ct) — ein Tonschritt springt, ein Gemisch aus alter und
    neuer Note steht oder springt, Portamento läuft durch. Ergebnis: (Anteil
    gleitender Übergänge, Median der Gleitzeit in ms) oder (None, None) ohne
    numpy. Gegenstück in dawd: legatoGlideMs (VoiceAnalysis.cpp) →
    describeVoice glideShare (Anteil der erkannten Legato-Übergänge); hier
    die Übergänge der Phrase (LEGATO). Eine Änderung der Methode gehört in
    beide. Ausnahme Gleitzeit: dawd rechnet sie auf den ganzen Tonschritt
    hoch (Entscheidung Matthias 2026-10-29, glideMs = Einstellung bei
    linearem Portamento); hier bleibt sie die Zeit des gemessenen
    Mittelteils, damit die glideMs der Kataloge ohne Neumessung gültig
    bleiben (4OSC 80 ms: hier ~45 ms, dawd ~80 ms)."""
    try:
        import numpy as np
    except ImportError:
        return None, None
    x = np.asarray(x, dtype=float)
    win, nfft, hop = 1024, 4096, 0.005
    w = np.hanning(win)
    glides, times = 0, []
    for a, b, t0, t1 in LEGATO:
        target = (b - a) * 100
        sign = 1 if target > 0 else -1
        cents = np.arange(min(0, target) - 50, max(0, target) + 51, 10.0)
        f = 440.0 * 2 ** ((a - 69) / 12) * 2 ** (cents / 1200)
        steps = []
        for i in range(int((t1 - t0) / hop)):
            s = int((t0 - 0.01 + i * hop) * rate) - win // 2
            seg = x[max(0, s):s + win]
            if s < 0 or len(seg) < win:
                continue
            spec = np.abs(np.fft.rfft(seg * w, nfft))
            sal = np.zeros(len(f))
            for k in range(1, 9):
                sal += np.interp(f * k * nfft / rate, np.arange(len(spec)), spec, right=0.0) / math.sqrt(k)
            j = int(np.argmax(sal))
            steps.append((cents[j], sal[j]))
        loud = max((v for _, v in steps), default=0.0)
        best, run = (0, 0.0), []
        for c, v in steps:
            inside = v > 0.1 * loud and abs(c) >= 80 and abs(c - target) >= 80 and min(0, target) < c < max(0, target)
            if inside and run and 0 <= (c - run[-1]) * sign <= 150:
                run.append(c)
            else:
                run = [c] if inside else []
            if run and (len(run), abs(run[-1] - run[0])) > best:
                best = (len(run), abs(run[-1] - run[0]))
        if best[0] >= 3 and best[1] >= 50:
            glides += 1
            times.append(best[0] * hop * 1000)
    return glides / len(LEGATO), (round(float(np.median(times))) if times else None)


def timbre_metrics(api, path):
    """Die Messung von matchSound/tuneSound (describeTimbre mit voice) an der
    Testphrase: Raster 120 BPM ab 0, ohne CLAP-Zeitleiste."""
    t = api("describeTimbre", file=path, bpm=120, firstDownbeat=0, voice=True, timeline=False)
    if not t.get("ok"):
        return {}
    tb, v = t.get("timbre", {}), t.get("voice", {})
    env, st, flt, ens = tb.get("envelope", {}), tb.get("stereo", {}), tb.get("filter", {}), tb.get("ensemble", {})
    # glide und detune misst das Werkzeug selbst an der Phrase (Todo
    # 2026-10-27-detune-glide-messung): mit den bekannten Zeiten der Phrase
    # statt der erkannten Noten. dawd misst seit Todo
    # 2026-10-27-comparevoice-detune dasselbe (detuneCents, glideShare) —
    # die Kataloge bleiben bei diesen Werten, ohne Neumessung
    x, rate = read_wav(path)
    glide, glide_ms = legato_glide(x, rate)
    out = {"sustain": env.get("sustainDb"), "sweep": flt.get("noteSweepOct"), "glide": glide,
           "vibrato": (v.get("vibratoCents") or 0.0) if (v.get("vibratoShare") or 0) >= 0.3 else 0.0,
           "beating": ens.get("beatingDb"), "detune": detune_cents(x, rate), "correlation": st.get("correlation"),
           "glideMs": glide_ms, "vibratoHz": v.get("vibratoHz"), "beatingHz": ens.get("beatingHz")}
    return {k: v for k, v in out.items() if v is not None}


def metrics(api, path, stim, embed=False, voice=False):
    """embed: auch den CLAP-Vektor (m["clap"]) und die Klang-Merkmale
    (m["sound"]) — für Kataloge, die findPresets nach Klang durchsucht.
    voice: die Messgrößen der Version 2 (Instrumente mit PHRASE)."""
    a = api("analyze", file=path, **({"embed": True} if embed else {}))
    if not a.get("ok"):
        raise RuntimeError(a.get("error"))
    bands = [b for b in a.get("bands", []) if b.get("db") is not None]
    pw = [10 ** (b["db"] / 10) for b in bands]
    tot = sum(pw) or 1e-12
    centroid = sum(math.log2(b["hz"] / 1000.0) * p for b, p in zip(bands, pw)) / tot
    low = 10 * math.log10(sum(p for b, p in zip(bands, pw) if b["hz"] <= 125) / tot + 1e-12)
    high = 10 * math.log10(sum(p for b, p in zip(bands, pw) if b["hz"] >= 4000) / tot + 1e-12)
    env = envelope_stats(path, *stim, attack_window=PHRASE_ATTACK if tuple(stim) == PHRASE_STIM else 1.0)
    silent = bool(a.get("silent")) or a.get("lufs") is None or a["lufs"] < -60
    m = {"lufs": a.get("lufs") if a.get("lufs") is not None else -70.0, "silent": silent}
    if not silent:
        # Spektrum/Hüllkurve nur bei hörbarem Signal — Stille hat keine Klangfarbe
        m.update({"brightness": round(centroid, 3), "low": round(max(FLOOR, low), 2), "high": round(max(FLOOR, high), 2),
                  "width": max(FLOOR, (a.get("stereo") or {}).get("widthDb", -100.0)),
                  "tail": env["tail"], "attack": env["attack"]})
        if a.get("clap"):
            m["clap"], m["sound"] = a["clap"], a.get("sound")
        if voice:
            m.update(timbre_metrics(api, path))
            m["velocity"] = velocity_db(path)
        if tuple(stim) == EFFECT_STIM:
            m.update(burst_db(path))
    return m


def change(base, m):
    """Je Messgröße die Änderung gegenüber der Basis, normiert auf die Schwelle."""
    out = {}
    for k, th in THRESH.items():
        if k not in m or m.get(k) is None or base.get(k) is None:
            continue
        d = m[k] - base[k]
        if k == "width" and (m[k] <= -99 and base[k] <= -99):
            continue
        out[k] = (d, abs(d) / th)
    return out


NOISE = {}   # je Messgröße die Streuung der Basis (wiederholte Renders)


def describe(points):
    """points: [(v, text, metrics)] -> Wirkung je Messgröße (Bereich, Richtung)."""
    does = []
    for k, th0 in THRESH.items():
        th = max(th0, 2.0 * NOISE.get(k, 0.0))
        vals = [(v, t, m.get(k)) for v, t, m in points if m.get(k) is not None]
        if len(vals) < 2:
            continue
        lo = min(vals, key=lambda x: x[2])
        hi = max(vals, key=lambda x: x[2])
        span = hi[2] - lo[2]
        if span < th:
            continue
        first, last = vals[0][2], vals[-1][2]
        steps = [b[2] - a[2] for a, b in zip(vals, vals[1:])]
        mono = all(s >= -th / 2 for s in steps) or all(s <= th / 2 for s in steps)
        if not mono and span < 2 * th:
            continue   # kleine, nicht gerichtete Schwankung: Messrauschen
        does.append({"metric": k, "unit": UNITS[k], "span": round(span, 2),
                     "atMin": round(first, 2), "atMax": round(last, 2),
                     "direction": ("up" if last > first else "down") if mono else "varies",
                     "ratio": round(span / th, 1)})
    does.sort(key=lambda d: -d["ratio"])
    return does


def directed(does):
    """Nur gerichtete Wirkungen — unter eingeschalteten Effekten schwankt viel.
    Starke ungerichtete (ratio ab 5, wie strength "strong") zählen mit:
    Pulse Width ist um 50 % symmetrisch (1 % und 99 % klingen gleich, mit
    Wave = square 11 LU Unterschied zur Mitte) und galt sonst als wirkungslos."""
    return [d for d in does if d["direction"] != "varies" or d["ratio"] >= 5]


def strength(does):
    r = max((d["ratio"] for d in does), default=0)
    return "none" if r == 0 else "weak" if r < 2 else "medium" if r < 5 else "strong"


AMOUNT = re.compile(r"\b(gain|ratio|depth|amount|mix|drive|feedback)\b", re.I)
# Schwelle beim Mitdrehen: über 0.2 nach THRESHOLD_LOW (eingebauter Compressor
# -19 dB, LSP als linearer Faktor -20 dB), darunter nach THRESHOLD_HIGH (LSP
# Gate: Curve threshold 0.063 schließt in den leisen Burst-Teilen nicht, 0.3
# schon)
THRESHOLD_LOW, THRESHOLD_HIGH = 0.1, 0.3


def is_neutral(p):
    """Regler, der gerade nichts tut: ganz unten oder Anzeige 0 (Gain 0 dB)."""
    if p["value"] <= 0.05:
        return True
    m = re.match(r"\s*[+-]?(\d+(?:\.\d+)?)", str(p.get("text", "")))
    return bool(m) and float(m.group(1)) == 0.0


def group_key(name):
    k = re.sub(r"^[AB] ", "", name)
    return re.sub(r"\d+", "#", k)


def summary(does):
    names = {"lufs": "loudness", "brightness": "brightness", "low": "lows", "high": "highs",
             "width": "stereo width", "tail": "tail/decay", "attack": "attack", "pitch": "pitch",
             "sustain": "sustain", "sweep": "filter sweep", "glide": "glide", "vibrato": "vibrato",
             "beating": "level beating", "detune": "detune", "correlation": "L/R correlation", "velocity": "velocity response",
             "burstOnset": "burst onset", "burstEarly": "level right after a burst", "burstLate": "level between bursts"}
    parts = []
    for d in does[:3]:
        arrow = {"up": "↑", "down": "↓", "varies": "↕"}[d["direction"]]
        parts.append(f"{names[d['metric']]} {arrow} {d['span']} {d['unit']}")
    return ", ".join(parts)


def profile(api, name, fmt, workdir, query=None, max_params=600, log=print, partial=None):
    proj = tempfile.mkdtemp(prefix="profile-", dir=workdir)
    api("treeClose")
    if not api("treeSeed", dir=proj, label="profile").get("ok"):
        raise RuntimeError("treeSeed failed")
    api("treeSettings", autoBranch=False)
    api("updateSection", name="Section 1", length=4)   # Effekte: unten EFFECT_LENGTH

    info = api("pluginInfo", name=name, **({"format": fmt} if fmt else {}), limit=1)
    if not info.get("ok"):
        raise RuntimeError(info.get("error"))
    instrument = None
    pl = [p for p in api("listPlugins").get("plugins", []) if p["name"] == info["name"] and p["format"] == info["format"]]
    cls = pl[0]["class"] if pl else "audioFx"

    if cls == "instrument":
        t = api("createTrack", type="midi", instrument=info["name"], format=info["format"])
        pid, track = t["id"], t["index"]
        r = api("addMidiClip", track=track, section="Section 1", startBeat=0, lengthBeats=8, notes=PHRASE)
        if not r.get("ok"):
            raise RuntimeError(r.get("error"))
        stim = PHRASE_STIM
        signal = PHRASE_SIGNAL
        instrument = True
    else:
        # Name mit Version: ein wiederverwendeter --workdir hätte sonst das alte Signal
        sig = os.path.join(workdir, f"effect-signal-v{EFFECT_VERSION}.wav")
        if not os.path.exists(sig):
            write_effect_signal(sig)
        api("addClip", track=0, file=sig, section="Section 1")
        pid = api("addPlugin", name=info["name"], format=info["format"], track=0, at="start")["id"]
        api("updateSection", name="Section 1", length=EFFECT_LENGTH)
        stim = EFFECT_STIM
        signal = EFFECT_SIGNAL

    out = os.path.join(proj, "m.wav")
    version = VERSION if instrument else EFFECT_VERSION

    def render():
        r = api("render", file=out, section="Section 1", tail=2, force=True)
        if not r.get("ok"):
            raise RuntimeError(r.get("error"))
        return metrics(api, out, stim, voice=bool(instrument))

    # Basis dreimal: die Streuung hebt die Schwellen (Zufallsphasen, Unison)
    bases = [render() for _ in range(3)]
    base = bases[0]
    NOISE.clear()
    for k in THRESH:
        vals = [b[k] for b in bases if b.get(k) is not None]
        if len(vals) == 3:
            NOISE[k] = max(vals) - min(vals)
    # limit: listParameters liefert sonst nur 80 (Helm hat 139, Diva ~280 —
    # der Lauf vom 2026-10-26 sah bei Helm nur bis poly_lfo_waveform)
    params = api("listParameters", plugin=pid, samples=5, limit=1000)
    plist = [p for p in params.get("parameters", []) if not SKIP.search(p["name"])]
    allp = list(plist)   # Schalter und Mengen-Regler aus allen, auch mit --query
    settings = params.get("settings", [])
    if query:
        plist = [p for p in plist if all(w in p["name"].lower() for w in query.lower().split())]

    groups, order = {}, []
    for p in plist:
        k = group_key(p["name"])
        if k not in groups:
            groups[k] = p["name"]
            order.append(p)
    order = order[:max_params]
    # Schalter: On/Enable/… im Namen, oder eine Auswahl, die gerade auf "Off"
    # steht (LSP-EQ: "Filter type 0" = Off, das Band wirkt erst mit einem Typ)
    # oder ein reiner Off/On-Schalter, der aus ist (x42-eq: "Highpass")
    # LV2 ohne Wertetexte (Helm: filter_on, scale linear, Text "0"): ein
    # Schalter-Name, der auf 0 steht, zählt auch (Todo 2026-10-18 P2: Helm
    # cutoff "no audible effect", weil filter_on nie eingeschaltet wurde)
    def lv2_toggle(p):
        return switch_name(p["name"]) and p.get("scale") not in ("stepped", "choice") and p["value"] <= 0.001 \
            and str(p.get("text", "")).strip() in ("0", "0.0", "0.000000", "off", "Off")

    # Eine Auswahl auf "External" (LSP Sidechain Compressor: Sidechain type)
    # wartet auf ein Schlüsselsignal, das der Steckbrief nicht hat — der
    # Kompressor regelt nie; sie zählt als Schalter, "an" = erste andere Option
    switches = [p for p in allp if (p.get("scale") in ("stepped", "choice") or lv2_toggle(p))
                and (switch_name(p["name"]) or (str(p.get("text", "")).lower() == "off" and len(p.get("options") or []) > 2)
                     or ([o.lower() for o in p.get("options") or []] == ["off", "on"] and str(p.get("text", "")).lower() == "off")
                     or (str(p.get("text", "")).lower() == "external" and len(p.get("options") or []) > 1))
                and not re.search(r"\b(mute|solo|listen|bypass|freeze|invert|phase|reset|link)\b", p["name"], re.I)]
    # Mengen-Regler in Neutralstellung (Gain 0 dB, Ratio 0): solange sie da
    # stehen, wirken Frequenz/Bandbreite bzw. Threshold/Attack nicht
    amounts = [q for q in allp if q.get("scale") not in ("stepped", "choice") and AMOUNT.search(q["name"])
               and not re.search(r"\b(input|output|dry|wet|makeup)\b", q["name"], re.I) and is_neutral(q)]
    # Schwellen über dem Testsignal (eingebauter Compressor -6 dB, LSP
    # Limiter 0 dB; das Rauschen liegt bei -9 dB RMS, Spitzen 0 dB): Ratio,
    # Attack, Release wirken erst, wenn die Schwelle darunter liegt — wie ein
    # Mengen-Regler mitdrehen, aber auf die andere Seite (meist nach unten;
    # ein Gate braucht sie über dem leisen Teil der Bursts)
    thresholds = [q for q in allp if q.get("scale") not in ("stepped", "choice") and re.search(r"\bthreshold\b", q["name"], re.I)]
    levers = amounts + thresholds
    log(f"{info['name']} ({info['format']}): {len(params.get('parameters', []))} parameters, "
        f"{len(order)} to measure, {len(switches)} switches, signal: {signal}")

    def sweep(p):
        orig = p["value"]
        pts = []
        for v in POINTS:
            r = api("setParameter", plugin=pid, name=p["name"], value=v)
            text = (r.get("set") or [{}])[0].get("text", "")
            pts.append((v, text, render()))
        api("setParameter", plugin=pid, name=p["name"], value=orig)
        return pts

    def on_text(s):
        """Auswahl mit "Off" vorn: die erste Option danach (Bell statt der letzten)."""
        opts = s.get("options") or []
        if str(s.get("text", "")).lower() == "external":
            return next((o for o in opts if o.lower() != "external"), None)
        return opts[1] if len(opts) > 2 and opts[0].lower() == "off" else None

    def turn_on(switch_list, on=True):
        prev = []
        for s in switch_list:
            prev.append((s["name"], s["value"]))
            if on and on_text(s):
                api("setParameter", plugin=pid, name=s["name"], text=on_text(s))
            else:
                api("setParameter", plugin=pid, name=s["name"], value=1.0 if on else 0.0)
        return prev

    # Fortsetzen: was ein abgebrochener Lauf schon gemessen hat
    measured, t0 = {}, time.time()
    if partial and os.path.exists(partial):
        try:
            old = json.load(open(partial))
            if old.get("version") == version and old.get("signal") == signal:
                measured = old.get("parameters", {})
                log(f"  resuming: {len(measured)} parameters from {partial}")
        except ValueError:
            pass
    todo = [p for p in order if p["name"] not in measured]
    done_before = len(order) - len(todo)
    for i, p in enumerate(todo, start=done_before):
        pts = sweep(p)
        does = describe(pts)
        entry = {"scale": p.get("scale"), "points": [[v, t] for v, t, _ in pts]}
        if not does and (switches or settings or levers):
            # Abhängigkeit: erst der namensähnlichste Schalter, dann alle
            words = set(re.findall(r"[a-z]+", p["name"].lower())) - {"on", "enable", "active", "level", "mix", "amount"}
            near = [s for s in switches if s["name"] != p["name"] and words & set(re.findall(r"[a-z]+", s["name"].lower()))]
            near.sort(key=lambda s: -len(words & set(re.findall(r"[a-z]+", s["name"].lower()))))
            # sonst der Schalter mit gleicher Nummer (LSP: "Frequency 0" → "Filter type 0")
            num_p = re.findall(r"\d+$", p["name"])
            near = near or [s for s in switches if s["name"] != p["name"] and num_p and re.findall(r"\d+$", s["name"]) == num_p]
            tried = []
            if near:
                tried.append([near[0]])
            if switches:
                tried.append(switches)
            for combo in tried:
                prev = turn_on(combo)
                d2 = directed(describe(sweep(p)))
                for nm, val in prev:
                    api("setParameter", plugin=pid, name=nm, value=val)
                if d2 and len(combo) > 3:
                    # alle an wirkt: welcher allein genügt? (Helm: cutoff
                    # braucht filter_on, nicht arp_on) — nur für die Regler,
                    # die erst so wirken, einzeln nachgeprüft
                    for sw in combo:
                        prev = turn_on([sw])
                        d1 = directed(describe(sweep(p)))
                        for nm, val in prev:
                            api("setParameter", plugin=pid, name=nm, value=val)
                        if d1:
                            combo, d2 = [sw], d1
                            break
                if d2:
                    entry["needs"] = ([s["name"] + (f" = {on_text(s)}" if on_text(s) else "") for s in combo]
                                      if len(combo) <= 3 else ["other switches on"])
                    does = d2
                    break
            if not does and levers:
                # erst gleiche Nummer bzw. gleiches Wort, dann alle (wenn es
                # wenige sind); danach die Schwellen einzeln (needs nennt nur
                # die nötige; nur gleiche bzw. keine Nummer, höchstens 4 —
                # LSP Multiband hat 8 Bänder) und zuletzt alles zusammen
                num = re.findall(r"\d+$", p["name"])
                pw = set(re.findall(r"[a-z]+", p["name"].lower())) - {"frequency", "freq", "time", "level", "gain"}
                same_num = [q for q in amounts if q["name"] != p["name"] and num and re.findall(r"\d+$", q["name"]) == num]
                same_word = [q for q in amounts if q["name"] != p["name"] and pw & set(re.findall(r"[a-z]+", q["name"].lower()))]
                rest = [q for q in amounts if q["name"] != p["name"]]
                low = [q for q in thresholds if q["name"] != p["name"] and re.findall(r"\d+$", q["name"]) in ([], num)][:4]
                both = (rest if len(rest) <= 8 else []) + low
                combos = []
                for c in (same_num, same_word, rest if len(rest) <= 8 else [], *[[q] for q in low], both):
                    if c and c not in combos:
                        combos.append(c)
                # je Menge allein, dann zusammen mit dem nächsten Schalter
                # (LSP-EQ: Band braucht Filter type = Bell und Gain ≠ 0)
                tries = [(c, sw) for c in combos for sw in ([None] + near[:1])]
                for combo, sw in tries:
                    texts, prev = [], turn_on([sw]) if sw else []
                    if sw:
                        texts.append(sw["name"] + (f" = {on_text(sw)}" if on_text(sw) else ""))
                    for q in combo:
                        r = api("setParameter", plugin=pid, name=q["name"],
                                value=(THRESHOLD_LOW if q["value"] > 0.2 else THRESHOLD_HIGH) if q in thresholds
                                else 0.9 if q["value"] >= 0.3 else 0.75)
                        texts.append(f"{q['name']} = {(r.get('set') or [{}])[0].get('text', '')}")
                    d4 = directed(describe(sweep(p)))
                    for q in combo:
                        api("setParameter", plugin=pid, name=q["name"], value=q["value"])
                    for nm, val in prev:
                        api("setParameter", plugin=pid, name=nm, value=val)
                    if d4:
                        entry["needs"] = texts if len(texts) <= 3 else ["other amounts up (gain, ratio, …)"]
                        does = d4
                        break
            if not does and settings:
                # 4OSC & Co.: Einstellungen (Wellenform, Effekte an) — erst die
                # mit gleicher Nummer bzw. gleichem Wort, dann alle
                def wave_for(st):
                    opts = st.get("options", [])
                    want = "square" if "pulse" in p["name"].lower() else "sawUp"
                    return want if want in opts else next((o for o in opts if o not in ("none", "sine")), None)

                def as_setting(st):
                    if st["kind"] == "bool":
                        return True
                    if st["kind"] == "choice":
                        return wave_for(st) if "wave" in st["name"].lower() else next(
                            (o for o in st.get("options", []) if o not in ("none", str(st["value"]))), None)
                    return None

                num = re.findall(r"\d+$", p["name"])
                pw = set(re.findall(r"[a-z]+", p["name"].lower()))
                same_num = [st for st in settings if num and re.findall(r"\d+$", st["name"]) == num and "lfo" not in st["name"].lower()]
                same_word = [st for st in settings if pw & set(re.findall(r"[a-z]+", st["name"].lower())) - {"on", "1", "2", "3", "4"}]
                combos = [c for c in (same_num, same_word, settings) if c]
                # verwandte Mengen-Regler (Reverb Mix zu Reverb Width) dabei aufdrehen
                mixes = [q for q in plist if q["name"] != p["name"] and q["value"] < 0.05
                         and re.search(r"\b(mix|amount|level)\b", q["name"], re.I)
                         and pw & set(re.findall(r"[a-z]+", q["name"].lower())) - {"mix", "amount", "level"}]
                for combo in combos:
                    vals = {st["name"]: as_setting(st) for st in combo if as_setting(st) is not None}
                    if not vals:
                        continue
                    api("setParameter", plugin=pid, settings=vals)
                    for q in mixes:
                        api("setParameter", plugin=pid, name=q["name"], value=0.5)
                    d3 = directed(describe(sweep(p)))
                    api("setParameter", plugin=pid, settings={st["name"]: st["value"] for st in settings})
                    for q in mixes:
                        api("setParameter", plugin=pid, name=q["name"], value=q["value"])
                    if d3:
                        needs = [f"{k} = {v}" for k, v in vals.items()] if len(vals) <= 3 else ["settings on (waves, effects)"]
                        entry["needs"] = needs + [f"{q['name']} > 0" for q in mixes]
                        does = d3
                        break
        entry["does"] = does
        entry["strength"] = strength(does)
        entry["summary"] = summary(does) if does else "no audible effect with the test signal"
        measured[p["name"]] = entry
        if partial and (i + 1) % 10 == 0:
            with open(partial + ".tmp", "w") as f:
                json.dump({"version": version, "signal": signal, "parameters": measured}, f, ensure_ascii=False)
            os.replace(partial + ".tmp", partial)
        eta = (time.time() - t0) / (i + 1 - done_before) * (len(order) - i - 1)
        log(f"  [{i + 1}/{len(order)}] {p['name']}: {entry['summary']}" + (f" (needs {entry['needs']})" if "needs" in entry else "")
            + f"  ~{eta / 60:.1f} min left")

    for p in plist:
        k = group_key(p["name"])
        if p["name"] not in measured and groups.get(k) in measured:
            measured[p["name"]] = {"sameAs": groups[k]}

    api("treeClose")
    return {
        "plugin": {k: info.get(k) for k in ("name", "format", "uid", "vendor", "version")},
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "measured": {
            "version": version, "signal": signal, "points": POINTS, "thresholds": {k: max(v, 2.0 * NOISE.get(k, 0.0)) for k, v in THRESH.items()},
            "units": UNITS, "noise": dict(NOISE),
            "baseline": base, "parameters": measured,
            "switches": [s["name"] for s in switches],
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", nargs="?")
    ap.add_argument("--format")
    ap.add_argument("--port", type=int)
    ap.add_argument("--query")
    ap.add_argument("--max", type=int, default=600)
    ap.add_argument("--core", action="store_true")
    ap.add_argument("--skip-existing", action="store_true")
    ap.add_argument("--skip-current", action="store_true", help="skip profiles already measured with this version")
    ap.add_argument("--only", choices=("instruments", "effects"), help="with --core: only these")
    ap.add_argument("--start-port", type=int, default=7700, help="first port to try for the own dawd")
    ap.add_argument("--workdir", help="folder for the own dawd (config, log, renders); default a temp folder")
    args = ap.parse_args()
    # SIGTERM (tools/remeasure_all.sh beim Abbruch) wie Ctrl-C: finally
    # beendet die eigene dawd per quit
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))

    workdir = args.workdir or tempfile.mkdtemp(prefix="dawd-profile-")
    os.makedirs(workdir, exist_ok=True)
    proc = None
    port = args.port
    if not port:
        port = free_port(args.start_port)
        proc = start_dawd(port, workdir)
    api = Dawd(port)
    def open_info():
        # pluginInfo braucht einen offenen Edit — profile() schließt seinen am Ende
        api("treeClose")
        api("treeSeed", dir=tempfile.mkdtemp(prefix="info-", dir=workdir), label="info")

    try:
        if args.core:
            targets = [(c["name"], c["format"] if c["format"] != "builtin" else None)
                       for c in json.load(open(os.path.join(PROFILES, "coreset.json")))["plugins"]]
            if args.only:
                classes = {(p["name"], p["format"]): p.get("class") for p in api("listPlugins", all=True).get("plugins", [])}
                want = args.only == "instruments"
                targets = [(n, f) for n, f in targets
                           if (classes.get((n, f or "builtin")) == "instrument") == want]
        elif args.name:
            targets = [(args.name, args.format)]
        else:
            ap.error("name or --core")
        for name, fmt in targets:
            open_info()
            info = api("pluginInfo", name=name, **({"format": fmt} if fmt else {}), limit=1)
            if not info.get("ok"):
                print(f"skip {name}: {info.get('error')}")
                continue
            path = os.path.join(OUT, info["profileId"] + ".json")
            if args.skip_existing and os.path.exists(path):
                print(f"skip {name}: {path} exists")
                continue
            if args.skip_current and os.path.exists(path):
                try:
                    m = json.load(open(path)).get("measured", {})
                    current = VERSION if str(m.get("signal", "")).startswith("MIDI") else EFFECT_VERSION
                    if m.get("version") == current:
                        print(f"skip {name}: {path} has version {current}")
                        continue
                except ValueError:
                    pass
            t_start = time.time()
            partial = os.path.join(OUT, info["profileId"] + ".partial.json")
            result = profile(api, name, fmt, workdir, args.query, args.max, partial=partial)
            if not result["measured"]["parameters"]:
                # Sampler/Drum Kit: Klang kommt aus den Samples, nicht aus Parametern
                print(f"skip {name}: no parameters to measure")
                continue
            with open(path + ".tmp", "w") as f:
                json.dump(result, f, indent=1, ensure_ascii=False)
            os.replace(path + ".tmp", path)
            if os.path.exists(partial):
                os.remove(partial)
            print(f"wrote {path} ({(time.time() - t_start) / 60:.1f} min)", flush=True)
    finally:
        if proc:
            try:
                api("quit")
            except Exception:
                pass
            proc.wait(timeout=10)


if __name__ == "__main__":
    main()
