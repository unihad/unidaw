#!/usr/bin/env python3
"""Plugin-Sandbox Phase 4 mit echten Plugins messen (VST3 und LV2): dawd im
Prozess (UNIDAW_PLUGIN_SANDBOX=0) gegen Sandbox mit Gruppierung "je Plugin"
(Standard) und "je Modul" (pluginHosting single), dasselbe Projekt.

Szenarien:
- mix: N-mal drei MIDI-Spuren, Song --seconds:
    Surge XT (VST3) -> x42-eq (LV2)
    Dexed (VST3) -> Dragonfly Room Reverb (LV2)
    MDA JX10 (LV2) -> Dragonfly Room Reverb (VST3)
  Mit Sandbox "je Plugin" sind das 6 Host-Prozesse, egal wie gross N ist.
- chain: eine MIDI-Spur, 5 Plugins hintereinander (Surge XT -> x42-eq ->
  Dragonfly Room LV2 -> Dragonfly Room VST3 -> x42-eq). Eine Kette rechnet
  der Graph nacheinander: (Wand mit Sandbox - Wand im Prozess) / Bloecke /
  Plugins schaetzt den Aufschlag je Plugin und Block (Rundlauf durch den
  Host) mit echten Plugins.
- add: frisches Projekt, je Plugin einmal addPlugin (mit Sandbox startet das
  den Host-Prozess; LV2 laedt dort einmal alle LV2-Klassen).

Gemessen je Modus wie dawd_plugins.py: load (treeOpen), Render Wand- und
CPU-Zeit (dawd + Hosts, Median ueber --runs), PSS nach dem Laden (dawd +
Hosts, dazu dawd allein), Hosts; die Plugin-Liste kommt aus einem Scan vorab
(Kopie der Config), damit kein Scan im Prozess das PSS verfaelscht;
live (--live Sekunden ueber das Standard-Audio-Geraet, CPU und verpasste
Bloecke) nur auf Wunsch — Standard 0, das Geraet spielt dann mit (am Master
haengt dafuer testgain LV2 mit Gain 0: still, aber gerechnet).

Aufruf (aus dem Repo, Build vorhanden; ohne Geraet z. B. unter
bwrap --dev-bind / / --tmpfs /dev/snd mit PIPEWIRE_REMOTE=/nonexistent):
  python3 bench/sandbox/dawd_plugins_phase4.py --port 8031 [--n 2] [--runs 3] [--seconds 30] [--live 0]
Ausgabe: Kopfzeile (# ...) und je Messung eine JSON-Zeile auf stdout.
"""
import argparse
import json
import os
import shutil
import signal
import statistics
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dawd_plugins import api, cpu_s, pids, pss_mb, start   # noqa: E402

EQ = ("x42-eq - Parametric Equalizer Stereo", "LV2")
ROOM_LV2 = ("Dragonfly Room Reverb", "LV2")
ROOM_VST3 = ("Dragonfly Room Reverb", "VST3")
SYNTHS = [(("Surge XT", "VST3"), EQ), (("Dexed", "VST3"), ROOM_LV2), (("MDA JX10", "LV2"), ROOM_VST3)]
CHAIN_SYNTH = ("Surge XT", "VST3")
CHAIN = [EQ, ROOM_LV2, ROOM_VST3, EQ]


def notes(seconds, shift):
    beats = int(seconds * 2)   # 120 BPM
    return [{"pitch": p + shift, "beat": b, "beats": 1.5} for b in range(0, beats, 2) for p in (48, 52, 55)]


def add_track(port, synth, effects, seconds, shift):
    m = api(port, "createTrack", type="midi", instrument=synth[0], format=synth[1])
    assert m.get("ok"), m
    api(port, "addMidiClip", track=m["index"], section="Section 1", notes=notes(seconds, shift))
    for name, fmt in effects:
        r = api(port, "addPlugin", name=name, format=fmt, track=m["index"], at="end")
        assert r.get("ok"), r


def build(port, d, scenario, n, seconds, mute):
    api(port, "treeSeed", dir=d, label="bench")
    api(port, "treeSettings", autoBranch=False)
    api(port, "updateSection", name="Section 1", length=seconds)
    if scenario == "mix":
        for i in range(n):
            for k, (synth, fx) in enumerate(SYNTHS):
                add_track(port, synth, [fx], seconds, 12 * ((i + k) % 2))
    else:
        add_track(port, CHAIN_SYNTH, CHAIN, seconds, 0)
    if mute:
        # live: am Master testgain LV2 mit Gain 0 — gerechnet wird alles, hoeren tut man nichts
        r = api(port, "addPlugin", name="testgain", format="LV2", at="master")
        assert r.get("ok"), r
        api(port, "setParameter", plugin=r["id"], name="Gain", value=0.0)
    api(port, "treeClose")


def fresh_run_dir(config):
    """Lauf-Ordner mit der Plugin-Liste aus config (Settings.xml): kein Scan
    in der gemessenen Instanz — ein Scan im Prozess liesse alle gescannten
    Bibliotheken in dawd geladen (PSS)."""
    run_dir = tempfile.mkdtemp(prefix="dawd-bench4-run-")
    shutil.copytree(config, os.path.join(run_dir, "config"))
    return run_dir


def measure(port, mode, d, runs, live, seconds, scenario, config):
    run_dir = fresh_run_dir(config)
    proc = start(port, mode != "in-process", run_dir)
    res = {"scenario": scenario, "mode": mode}
    try:
        if mode == "single":
            assert api(port, "treeOpen", dir=d).get("ok")
            done = set()
            for p in api(port, "routing")["plugins"]:
                if p.get("hosting") and (p["name"], p["format"]) not in done:
                    api(port, "pluginHosting", plugin=p["id"], mode="single")
                    done.add((p["name"], p["format"]))
            api(port, "treeClose")
        t0 = time.time()
        assert api(port, "treeOpen", dir=d).get("ok")
        res["load_s"] = round(time.time() - t0, 3)
        plugs = api(port, "routing")["plugins"]
        res["plugins"] = sum(1 for p in plugs if p["format"] in ("VST3", "LV2"))
        res["hosts"] = len({p["sandbox"]["pid"] for p in plugs if "sandbox" in p})
        ps = pids(proc)
        res["pss_mb"] = round(pss_mb(ps), 1)
        res["pss_dawd_mb"] = round(pss_mb(ps[:1]), 1)
        walls, cpus = [], []
        for i in range(runs):
            ps = pids(proc)
            c0, t0 = cpu_s(ps), time.time()
            r = api(port, "render", file=os.path.join(d, f"out-{mode}-{i}.wav"))
            assert r.get("ok"), r
            walls.append(time.time() - t0)
            cpus.append(cpu_s(ps) - c0)
        res["render_wall_s"] = round(statistics.median(walls), 3)
        res["render_cpu_s"] = round(statistics.median(cpus), 2)
        res["render_runs"] = [round(x, 2) for x in walls]
        # render rechnet in Bloecken zu 512 (ops/Processing.cpp, blockSizeForAudio)
        res["render_blocks"] = int(seconds * 44100 / 512)
        if live > 0:
            ps = pids(proc)
            c0, t0 = cpu_s(ps), time.time()
            api(port, "play", on=True, **{"from": 0})
            time.sleep(live)
            lv = api(port, "levels")
            api(port, "play", on=False)
            dt = time.time() - t0
            res["live_playing"] = lv.get("playing")
            res["live_cpu_pct"] = round(100 * (cpu_s(ps) - c0) / dt, 1)
            plugs = api(port, "routing")["plugins"]
            res["live_missed_blocks"] = sum(p["sandbox"]["missedBlocks"] for p in plugs if "sandbox" in p)
        api(port, "quit")
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        shutil.rmtree(run_dir, ignore_errors=True)
    return res


def measure_add(port, mode, config):
    """addPlugin je Plugin in einem frischen Projekt (mit Sandbox: Host-Start + Laden)."""
    run_dir = fresh_run_dir(config)
    d = tempfile.mkdtemp(prefix="dawd-bench4-add-")
    proc = start(port, mode != "in-process", run_dir)
    res = {"scenario": "add", "mode": mode}
    try:
        api(port, "treeSeed", dir=d, label="add")
        api(port, "treeSettings", autoBranch=False)
        for name, fmt in [CHAIN_SYNTH, ("Dexed", "VST3"), ("MDA JX10", "LV2"), EQ, ROOM_LV2, ROOM_VST3]:
            t0 = time.time()
            r = api(port, "addPlugin", name=name, format=fmt)
            assert r.get("ok"), r
            res[f"{name} {fmt}"] = round(time.time() - t0, 3)
        api(port, "quit")
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        shutil.rmtree(run_dir, ignore_errors=True)
        shutil.rmtree(d, ignore_errors=True)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8031)
    ap.add_argument("--n", type=int, default=2)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--live", type=float, default=0)
    ap.add_argument("--scenarios", default="add,chain,mix")
    a = ap.parse_args()
    with open("/proc/loadavg") as f:
        load = " ".join(f.read().split()[:3])
    print(f"# {time.strftime('%Y-%m-%dT%H:%M:%S')} n={a.n} song {a.seconds} s runs={a.runs} live={a.live} s, "
          f"loadavg {load}", flush=True)
    # Einmal scannen; jede gemessene Instanz bekommt eine Kopie der Config
    scan_dir = tempfile.mkdtemp(prefix="dawd-bench4-scan-")
    proc = start(a.port, False, scan_dir)
    try:
        api(a.port, "listPlugins")
        api(a.port, "quit")
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
    config = os.path.join(scan_dir, "config")
    try:
        for scenario in a.scenarios.split(","):
            if scenario == "add":
                for mode in ("in-process", "plugin"):
                    print(json.dumps(measure_add(a.port, mode, config)), flush=True)
                continue
            d = tempfile.mkdtemp(prefix=f"dawd-bench4-{scenario}-")
            run_dir = fresh_run_dir(config)
            proc = start(a.port, False, run_dir)
            try:
                build(a.port, d, scenario, a.n, a.seconds, a.live > 0)
                api(a.port, "quit")
                proc.wait(timeout=30)
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                shutil.rmtree(run_dir, ignore_errors=True)
            try:
                for mode in ("in-process", "plugin", "single"):
                    print(json.dumps(measure(a.port, mode, d, a.runs, a.live, a.seconds, scenario, config)), flush=True)
            finally:
                shutil.rmtree(d, ignore_errors=True)
    finally:
        shutil.rmtree(scan_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
