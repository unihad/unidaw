#!/usr/bin/env python3
"""Plugin-Sandbox Phase 1 mit echten Plugins messen: dawd ohne und mit
UNIDAW_PLUGIN_SANDBOX=1, dasselbe Projekt.

Projekt: N Audio-Spuren (Rauschen) mit Dragonfly Room Reverb (VST3) und N
MIDI-Spuren mit Surge XT (VST3, Init-Patch, Akkorde), Song 30 s. Am Master
TestGain-LV2 mit Gain 0 (bleibt auch mit Schalter im Prozess) — live hoert
man nichts, gerechnet wird alles.

Gemessen je Modus:
- load: treeOpen des fertigen Projekts (Plugins instanziieren, mit Sandbox
  je VST3 ein Host-Prozess)
- render: Wandzeit und CPU-Zeit (dawd + Host-Prozesse, utime+stime aus
  /proc) je Render, Median ueber --runs
- live: --live Sekunden abspielen ueber das Standard-Geraet, CPU-Anteil
  (dawd + Hosts) und verpasste Bloecke (routing.plugins[].sandbox)
- pss: Speicher (PSS, smaps_rollup) von dawd + Hosts nach dem Laden

Aufruf (aus dem Repo, Build vorhanden):
  python3 bench/sandbox/dawd_plugins.py --port 8022 [--tracks 4] [--runs 3] [--live 10]
Braucht Dragonfly Room Reverb und Surge XT als VST3 sowie TestGain-LV2
(LV2_PATH oder ~/.lv2).
"""
import argparse
import json
import os
import random
import shutil
import signal
import secrets
import statistics
import struct
import subprocess
import tempfile
import time
import urllib.request
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "..", "dawd", "build", "unidaw_artefacts", "Release", "unidaw")
TICK = os.sysconf("SC_CLK_TCK")


# Token der localhost-API: die gestarteten Instanzen erben ihn
TOKEN = os.environ.setdefault("UNIDAW_TOKEN", secrets.token_hex(32))


def api(port, op, **kw):
    req = urllib.request.Request(f"http://127.0.0.1:{port}/command", data=json.dumps({"op": op, **kw}).encode(),
                                 headers={"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)


def start(port, sandbox, run_dir):
    env = dict(os.environ, DISPLAY="")
    env.pop("WAYLAND_DISPLAY", None)
    # Standard ist an (seit Phase 3): "im Prozess" braucht ausdruecklich 0
    env["UNIDAW_PLUGIN_SANDBOX"] = "1" if sandbox else "0"
    err = open(os.path.join(run_dir, "unidaw.err"), "w")
    p = subprocess.Popen([os.path.abspath(BIN), "--port", str(port), "--log", "events.jsonl", "--config", "config",
                          "--interface-sim", "--window-sim"],
                         cwd=run_dir, env=env, stdout=subprocess.DEVNULL, stderr=err, start_new_session=True)
    for _ in range(100):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2)
            return p
        except Exception:
            time.sleep(0.2)
    raise SystemExit("dawd did not start")


def pids(proc):
    out = subprocess.run(["ps", "-o", "pid=", "--ppid", str(proc.pid)], capture_output=True, text=True).stdout.split()
    return [proc.pid] + [int(x) for x in out]


def cpu_s(pid_list):
    total = 0
    for pid in pid_list:
        try:
            with open(f"/proc/{pid}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            total += int(fields[11]) + int(fields[12])   # utime, stime
        except OSError:
            pass
    return total / TICK


def pss_mb(pid_list):
    total = 0
    for pid in pid_list:
        try:
            with open(f"/proc/{pid}/smaps_rollup") as f:
                for line in f:
                    if line.startswith("Pss:"):
                        total += int(line.split()[1])
        except OSError:
            pass
    return total / 1024


def noise_wav(path, seconds):
    rnd = random.Random(1)
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"".join(struct.pack("<h", int(rnd.uniform(-6000, 6000))) for _ in range(int(seconds * 44100))))


def build(port, d, tracks, seconds):
    api(port, "treeSeed", dir=d, label="bench")
    api(port, "treeSettings", autoBranch=False)
    api(port, "updateSection", name="Section 1", length=seconds)
    w = os.path.join(d, "noise.wav")
    noise_wav(w, seconds)
    for i in range(tracks):
        t = 0 if i == 0 else api(port, "createTrack")["index"]
        api(port, "addClip", track=t, file=w, section="Section 1")
        r = api(port, "addPlugin", name="Dragonfly Room Reverb", format="VST3", track=t, at="end")
        assert r.get("ok"), r
    beats = int(seconds * 2)   # 120 BPM
    for i in range(tracks):
        m = api(port, "createTrack", type="midi", instrument="Surge XT", format="VST3")
        assert m.get("ok"), m
        notes = [{"pitch": p + 12 * (i % 2), "beat": b, "beats": 1.5} for b in range(0, beats, 2) for p in (48, 52, 55)]
        api(port, "addMidiClip", track=m["index"], section="Section 1", notes=notes)
    r = api(port, "addPlugin", name="testgain", format="LV2", at="master")
    assert r.get("ok"), r
    api(port, "setParameter", plugin=r["id"], name="Gain", value=0.0)
    api(port, "treeClose")


def measure(port, sandbox, d, runs, live):
    run_dir = tempfile.mkdtemp(prefix="dawd-bench-run-")
    proc = start(port, sandbox, run_dir)
    res = {"mode": "sandbox" if sandbox else "in-process"}
    try:
        api(port, "listPlugins")   # Scan, sonst kennt treeOpen die Plugins nicht
        t0 = time.time()
        assert api(port, "treeOpen", dir=d).get("ok")
        res["load_s"] = time.time() - t0
        plugs = api(port, "routing")["plugins"]
        res["hosts"] = sum(1 for p in plugs if "sandbox" in p)
        res["pss_mb"] = pss_mb(pids(proc))
        walls, cpus = [], []
        for i in range(runs):
            ps = pids(proc)
            c0, t0 = cpu_s(ps), time.time()
            r = api(port, "render", file=os.path.join(d, f"out-{res['mode']}-{i}.wav"))
            assert r.get("ok"), r
            walls.append(time.time() - t0)
            cpus.append(cpu_s(ps) - c0)
        res["render_wall_s"] = statistics.median(walls)
        res["render_cpu_s"] = statistics.median(cpus)
        res["render_runs"] = [round(x, 2) for x in walls]
        res["block"] = api(port, "audioSettings").get("bufferSize")
        if live > 0:
            ps = pids(proc)
            c0, d0, t0 = cpu_s(ps), cpu_s(ps[:1]), time.time()
            api(port, "play", on=True, **{"from": 0})
            time.sleep(live)
            lv = api(port, "levels")
            api(port, "play", on=False)
            dt = time.time() - t0
            res["live_playing"] = lv.get("playing")
            res["live_cpu_pct"] = 100 * (cpu_s(ps) - c0) / dt
            res["live_cpu_dawd_pct"] = 100 * (cpu_s(ps[:1]) - d0) / dt
            plugs = api(port, "routing")["plugins"]
            res["live_missed_blocks"] = sum(p["sandbox"]["missedBlocks"] for p in plugs if "sandbox" in p)
        api(port, "quit")
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        shutil.rmtree(run_dir, ignore_errors=True)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8022)
    ap.add_argument("--tracks", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--live", type=float, default=10)
    a = ap.parse_args()
    d = tempfile.mkdtemp(prefix="dawd-bench-proj-")
    run_dir = tempfile.mkdtemp(prefix="dawd-bench-run-")
    proc = start(a.port, False, run_dir)
    try:
        api(a.port, "listPlugins")
        build(a.port, d, a.tracks, a.seconds)
        api(a.port, "quit")
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        shutil.rmtree(run_dir, ignore_errors=True)
    with open("/proc/loadavg") as f:
        load = " ".join(f.read().split()[:3])
    print(f"# {time.strftime('%Y-%m-%dT%H:%M:%S')} tracks={a.tracks}x Dragonfly Room + {a.tracks}x Surge XT, "
          f"song {a.seconds} s, loadavg {load}")
    try:
        for sandbox in (False, True):
            print(json.dumps(measure(a.port, sandbox, d, a.runs, a.live)))
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    main()
