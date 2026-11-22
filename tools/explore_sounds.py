#!/usr/bin/env python3
"""Klänge eines Instruments erkunden: Parameter der Presets gezielt
kombinieren, messen und neue Klänge katalogisieren (Quality-Diversity,
MAP-Elites-artig).

    tools/explore_sounds.py "MDA JX10" --format LV2 [--budget 3000] [--workers 4] [--seeds 40]

Ablauf:
- Startpunkte sind Presets aus dem Preset-Katalog (<format>-<uid>-presets.json,
  tools/profile_presets.py), über die Klassen verteilt.
- Ein Schritt ändert 2–5 Parameter eines Katalog-Eintrags grob (±35 % des
  Regelwegs oder ein Zufallswert). Welche Parameter, steht im Steckbrief
  (tools/profile_plugin.py): starke öfter, reine Lautstärke- und
  Tonhöhen-Regler nie, Abhängigkeiten (needs) werden mitgesetzt.
- Der Lauf lernt mit: Parameter, deren Änderung den Klang selten zu einem
  neuen macht (CLAP-Ähnlichkeit zum Eltern-Klang bleibt über --similar —
  Regler einer unbenutzten Szene oder LFO, leerer Effekt-Slot), werden nach
  fünf Versuchen seltener gezogen. Die Zählung steht im Katalog
  (explore.learned, Übersicht explore.effective), ein neuer Lauf lernt dort
  weiter. Dieselbe Phrase zweimal gerendert ist sich nur zu 0.96–0.99
  ähnlich (Surge, Helm) — eine kleinere Schwelle misst Rauschen.
- Jeder Kandidat spielt dieselbe Phrase wie der Preset-Katalog und wird
  gemessen (Lautheit, Helligkeit, Attack, Sustain, Ausklang, Breite) und
  eingebettet (CLAP, analyze {embed}).
- Die Merkmale teilen den Klangraum in Fächer (hell/dunkel × Attack ×
  Sustain × Ausklang × Breite). Ein Fach hält bis zu --per-cell Klänge, die
  sich hörbar unterscheiden (CLAP-Ähnlichkeit unter --similar). Neu ist ein
  Klang, der ein leeres Fach füllt oder in ein nicht volles Fach passt; in
  einem vollen Fach verdrängt er den ähnlichsten Nachbarn (zählt nicht als
  neu). Kalibrierung: Werkspresets von MDA JX10 sind ihrem nächsten Nachbarn
  im Median zu 0.90 ähnlich.
- Schluss, wenn von den letzten --window Kandidaten weniger als --stop-rate
  neu waren (der Klangraum ist ausgeschöpft) oder das Budget aufgebraucht ist.

Ergebnis: dawd/plugin-profiles/<format>-<uid>-sounds.json — findPresets
findet die Klänge (kind "sound", auch mit text/like), loadPreset {plugin,
sound: id} lädt sie. Ein neuer Lauf setzt das Archiv fort.

Mehrere dawd-Instanzen (--workers) laufen parallel ohne Audiogerät (unter
bwrap wie der Profiler, je eigener Port und Config-Ordner).
"""
import argparse
import base64
import datetime
import json
import math
import os
import random
import re
import signal
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_plugin as pp  # noqa: E402
import profile_presets as ppr  # noqa: E402

WEIGHT = {"strong": 1.0, "medium": 0.7, "weak": 0.5, "none": 0.25}
# Tonhöhe verschiebt nur die Phrase, Lautstärke nur den Pegel — beides ist kein neuer Klang
NOT_TIMBRE = re.compile(r"\b(tune|tuning|octave|transpose|pitch|fine|coarse|semi\w*|master|volume|output|out gain|level)\b", re.I)

# Fächer des Klangraums: Grenzen je Merkmal (Einheiten wie im Preset-Katalog)
BINS = {
    "brightness": [-4.0, -3.0, -2.0, -1.0, 0.0],   # Oktaven um 1 kHz
    "attack": [10, 40, 120],                      # ms
    "sustain": [-18, -6],                         # dB
    "tail": [0.15, 0.5, 2.0],                     # s
    "width": [-38, -12, -6],                      # dB
}


def cell_of(f):
    if f.get("silent"):
        return None
    key = []
    for k, edges in BINS.items():
        v = f.get(k)
        if v is None:
            return None
        key.append(sum(1 for e in edges if v > e))
    return tuple(key)


def decode(vec):
    raw = base64.b64decode(vec)
    v = [b - 256 if b > 127 else b for b in raw]
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def cos(a, b):
    return sum(x * y for x, y in zip(a, b))


class Archive:
    """Fächer mit je bis zu per_cell unterschiedlichen Klängen."""

    def __init__(self, per_cell, similar):
        self.per_cell, self.similar = per_cell, similar
        self.cells = {}
        self.entries = []
        self.lock = threading.Lock()

    def offer(self, entry, replace=True):
        """"added" (neues Fach oder Platz im Fach), "replaced" (verdrängt im
        vollen Fach den ähnlichsten Nachbarn — verfeinert nur) oder der Grund
        der Ablehnung: "no cell" (Merkmal fehlt), "similar" (zu nah an einem
        Klang im Fach), "full" (Fach voll, nicht verschiedener als dessen
        ähnlichstes Paar)."""
        cell = cell_of(entry["features"])
        if cell is None or "clap" not in entry:
            return "no cell"
        vec = decode(entry["clap"])
        with self.lock:
            members = self.cells.setdefault(cell, [])
            sims = [cos(vec, m["_vec"]) for m in members]
            if sims and max(sims) >= self.similar:
                return "similar"
            result = "added"
            if len(members) >= self.per_cell and not replace:
                return "full"
            if len(members) >= self.per_cell:
                # voll: nur aufnehmen, wenn er sich mehr unterscheidet als das ähnlichste Paar im Fach
                pair = max(((i, j, cos(members[i]["_vec"], members[j]["_vec"]))
                            for i in range(len(members)) for j in range(i + 1, len(members))), key=lambda x: x[2])
                if max(sims) >= pair[2]:
                    return "full"
                out = members.pop(pair[1])
                self.entries.remove(out)
                result = "replaced"
            entry["_vec"], entry["_cell"] = vec, cell
            members.append(entry)
            self.entries.append(entry)
            return result

    def load(self, entry):
        """Gespeicherten Klang ohne Prüfung übernehmen: nach einer Neumessung
        (andere Phrase, neue Vektoren) liegen alte Klänge oft im selben Fach
        oder über --similar — offer() verwarf sie (Surge 337 -> 167). Ein
        Fortsetzen darf den Katalog nicht schrumpfen."""
        vec = decode(entry["clap"])
        cell = cell_of(entry["features"]) or ()
        with self.lock:
            entry["_vec"], entry["_cell"] = vec, cell
            if cell:
                self.cells.setdefault(cell, []).append(entry)
            self.entries.append(entry)

    def pick(self, rng):
        with self.lock:
            # jüngere Einträge etwas öfter: dort ist der Klangraum noch unerforscht
            n = len(self.entries)
            i = min(n - 1, int(n * (1 - rng.random() ** 1.5)))
            return self.entries[i]


def parse_needs(needs):
    """["Gain 1 = 14.4", "Filter type 0 = Bell", "Highpass"] -> {name: text | None}."""
    out = {}
    for n in needs or []:
        if n.startswith("other ") or n.endswith(" on (waves, effects)"):
            continue
        if " > 0" in n:
            continue
        name, _, text = n.partition(" = ")
        out[name.strip()] = text.strip() or None
    return out


def candidates(profile):
    """Parameter, die den Klang verändern: [(name, weight, needs)]."""
    params = profile["measured"]["parameters"]
    out = []
    for name, p in params.items():
        src = params.get(p["sameAs"], {}) if "sameAs" in p else p
        if NOT_TIMBRE.search(name):
            continue
        does = src.get("does") or []
        if does and all(d["metric"] == "lufs" for d in does):
            continue   # nur Pegel
        w = WEIGHT.get(src.get("strength") or "none", 0.25)
        out.append((name, w, parse_needs(src.get("needs"))))
    return out


def seeds_from(catalog, n, rng):
    """Bis zu n Presets, reihum über die Klassen (stumme nie)."""
    by_class = {}
    for p in catalog["presets"]:
        if p["features"].get("silent"):
            continue
        by_class.setdefault(p["class"], []).append(p)
    for v in by_class.values():
        rng.shuffle(v)
    out = []
    while len(out) < n and any(by_class.values()):
        for c in sorted(by_class):
            if by_class[c] and len(out) < n:
                out.append(by_class[c].pop())
    return out


def where(p):
    if p["source"] == "program":
        return {"program": p["program"]}
    return {"path": p["path"], **({"program": p["program"]} if "program" in p else {})}


def stop_dawd(api, proc):
    """quit, dann warten: terminate allein beendet nur bwrap, die dawd darunter
    liefe weiter (eigene Session) und hielte ihren Port."""
    try:
        api("treeClose")
        api("quit")
        proc.wait(timeout=20)
    except Exception:   # noqa: BLE001
        proc.terminate()


class Worker:
    """Eine dawd-Instanz mit Projekt, Instrument und Phrase."""

    def __init__(self, workdir):
        self.port = pp.free_port(7740)
        self.proc = pp.start_dawd(self.port, workdir)
        self.api = pp.Dawd(self.port)
        self.out = os.path.join(workdir, "x.wav")
        proj = tempfile.mkdtemp(prefix="explore-", dir=workdir)
        api = self.api
        api("treeClose")
        if not api("treeSeed", dir=proj, label="explore").get("ok"):
            raise RuntimeError("treeSeed failed")
        api("treeSettings", autoBranch=False)
        api("updateSection", name="Section 1", length=4)

    def setup(self, info):
        api = self.api
        t = api("createTrack", type="midi", instrument=info["name"], format=info["format"])
        if not t.get("ok"):
            raise RuntimeError(t.get("error"))
        self.pid = t["id"]
        r = api("addMidiClip", track=t["index"], section="Section 1", startBeat=0, lengthBeats=8, notes=ppr.NOTES)
        if not r.get("ok"):
            raise RuntimeError(r.get("error"))
        self.base, self.dirty = None, set()
        self.render()   # LV2 übernimmt Werte erst nach dem ersten Render (--no-audio)
        return self

    def render(self):
        r = self.api("render", file=self.out, section="Section 1", tail=2, force=True)
        if not r.get("ok"):
            raise RuntimeError(r.get("error"))
        m = pp.metrics(self.api, self.out, ppr.STIM, embed=True)
        if not m["silent"]:
            m["sustain"] = ppr.sustain_db(self.out)
        return m, m.pop("clap", None), m.pop("sound", None)

    def voice(self, m):
        """Die Messgrößen der Version 2+ (describeTimbre, velocity) am letzten
        Render nachholen — nur für aufgenommene Klänge, die Fächer brauchen
        sie nicht. Danach sind die Merkmale dieselben wie in
        profile_presets --sounds (gleiche Phrase, phrase = VERSION)."""
        m = dict(m, **pp.timbre_metrics(self.api, self.out))
        m["velocity"] = pp.velocity_db(self.out)
        return ppr.features_of(m)

    def load_base(self, seed):
        """Basis-Preset laden (nur bei Wechsel) und seine Parameterwerte."""
        key = json.dumps(where(seed), sort_keys=True)
        if self.base != key:
            r = self.api("loadPreset", plugin=self.pid, **where(seed))
            if not r.get("ok"):
                raise RuntimeError(r.get("error"))
            self.base, self.dirty = key, set()
            self.values = {p["name"]: p["value"] for p in self.api("listParameters", plugin=self.pid, limit=1000)["parameters"]}
        return self.values

    def evaluate(self, seed, params, needs_texts):
        values = self.load_base(seed)
        # Namen hängen bei manchen Synths am Preset (Surge: Oszillator-Regler
        # heißen je nach Typ "Width 1" oder "Param 1") — was das Basis-Preset
        # nicht hat, entfällt (und fehlt dann auch im gespeicherten Klang)
        for n in [n for n in params if n not in values]:
            del params[n]
        for n in [n for n in needs_texts if n not in values]:
            del needs_texts[n]
        # was der vorige Kandidat geändert hat, zurück auf die Basis
        reset = {n: values[n] for n in self.dirty - set(params) if n in values}
        if reset:
            self.api("setParameter", plugin=self.pid, values=reset)
        self.dirty = set(params) | set(needs_texts)
        if params:
            r = self.api("setParameter", plugin=self.pid, values=params)
            if not r.get("ok"):
                raise RuntimeError(r.get("error"))
        if needs_texts:
            self.api("setParameter", plugin=self.pid, texts=needs_texts)
        return self.render()

    def close(self):
        stop_dawd(self.api, self.proc)


def log_now(*a):
    print(*a, flush=True)   # in eine Datei umgeleitet sonst erst am Ende sichtbar


def explore(name, fmt, args, log=log_now):
    rng = random.Random(args.seed)
    workdir = tempfile.mkdtemp(prefix="dawd-explore-")
    first = Worker(tempfile.mkdtemp(prefix="w-", dir=workdir))
    info = first.api("pluginInfo", name=name, **({"format": fmt} if fmt else {}), limit=1)
    if not info.get("ok"):
        first.close()
        raise RuntimeError(info.get("error"))
    slug = info["profileId"]
    prof_path = os.path.join(pp.PROFILES, slug + ".json")
    cat_path = os.path.join(pp.PROFILES, slug + "-presets.json")
    # Probeläufe: UNIDAW_PROFILE_OUT=<Ordner> (setzt dort fort, sonst beim Archiv des Repos)
    out_path = os.path.join(pp.OUT, slug + "-sounds.json")
    src_path = out_path if os.path.exists(out_path) else os.path.join(pp.PROFILES, slug + "-sounds.json")
    if not os.path.exists(prof_path) or not os.path.exists(cat_path):
        first.close()
        raise RuntimeError(f"need the profile and the preset catalog first: {prof_path}, {cat_path}")
    profile = json.load(open(prof_path))
    catalog = json.load(open(cat_path))
    # nur Parameter, die das Plugin wirklich hat (Steckbrief einer anderen
    # Version, Surge: "A Osc 1 Sub Mix")
    first.setup(info)
    present = {p["name"] for p in first.api("listParameters", plugin=first.pid, limit=1000)["parameters"]}
    cands = [(n, w, {d: t for d, t in nd.items() if d in present}) for n, w, nd in candidates(profile) if n in present]
    if not cands:
        first.close()
        raise RuntimeError("no parameters to vary")

    # Presets mit Klasse aus dem Namen: Vorbilder für die Klasse erkundeter Klänge
    named = [(decode(p["clap"]), p["class"]) for p in catalog["presets"] if p.get("classFrom") == "name" and p.get("clap")]
    archive = Archive(args.per_cell, args.similar)
    seeds = {}
    learned = {}
    if os.path.exists(src_path):
        old = json.load(open(src_path))
        learned = old.get("explore", {}).get("learned", {})
        for s in old.get("sounds", []):
            archive.load(dict(s))
            seeds[json.dumps(s["base"], sort_keys=True)] = s["base"]
    for p in seeds_from(catalog, args.seeds, rng):
        base = {"name": p["name"], "category": p["category"], "source": p["source"], **where(p)}
        seeds[json.dumps(base, sort_keys=True)] = base
        # das Preset selbst ist der erste Eintrag seines Fachs (wird nicht gespeichert)
        if "clap" in p:
            archive.offer({"base": base, "params": {}, "features": p["features"], "clap": p["clap"],
                           "class": p["class"], "classFrom": p.get("classFrom"), "preset": True}, replace=False)
    log(f"{info['name']} ({info['format']}): {len(cands)} parameters to vary "
        f"({len(candidates(profile)) - len(cands)} from the profile missing in the plugin), {len(seeds)} seeds, "
        f"{sum(1 for e in archive.entries if not e.get('preset'))} sounds so far, {args.workers} workers")

    workers = [first] + [Worker(tempfile.mkdtemp(prefix="w-", dir=workdir)).setup(info)
                         for _ in range(args.workers - 1)]
    stats = {"evaluated": 0, "added": 0, "replaced": 0, "rejected": 0, "failed": 0, "failedInRow": 0, "recent": []}
    # Ablehnungsgründe (silent, quiet, no cell, similar, full) und Nebenzahlen:
    # "as parent" = CLAP-Ähnlichkeit zum Eltern-Klang über --similar,
    # "unchanged" = keine Änderung blieb übrig (Parameter fehlt im Basis-Preset)
    reasons = {}
    stop = threading.Event()
    reason = ["budget"]
    t0 = time.time()
    names = [c[0] for c in cands]
    weights = [c[1] for c in cands]
    needs = {c[0]: c[2] for c in cands}

    # Mitlernen: je Parameter, wie oft eine Änderung den Klang hörbar bewegt
    # hat (CLAP-Abstand zum Eltern-Klang). Was nie etwas bewegt (Regler einer
    # unbenutzten Szene, leerer Effekt-Slot), wird seltener gezogen.
    tried, moved = {}, {}
    for n, (t, mv) in learned.items():
        tried[n], moved[n] = t, mv

    def weight(i):
        n = names[i]
        t = tried.get(n, 0)
        if t < 5:
            return weights[i]
        return weights[i] * (0.1 + moved.get(n, 0) / t)

    def child():
        """Neuer Kandidat: Eltern-Eintrag + 2–5 grobe Änderungen."""
        parent = None
        if archive.entries and rng.random() < 0.85:
            parent = archive.pick(rng)
            base, params = parent["base"], dict(parent["params"])
        else:
            base, params = rng.choice(list(seeds.values())), {}
        k = rng.randint(2, 5)
        texts = {}
        with archive.lock:
            w_now = [weight(i) for i in range(len(names))]
        changed = set(rng.choices(names, weights=w_now, k=k))
        for n in changed:
            cur = params.get(n)
            if cur is None or rng.random() < 0.3:
                params[n] = round(rng.random(), 3)
            else:
                params[n] = round(min(1.0, max(0.0, cur + rng.gauss(0, 0.35))), 3)
            for dep, text in needs[n].items():
                if text:
                    texts[dep] = text
        return base, params, texts, parent, changed

    def run(w):
        while not stop.is_set():
            with archive.lock:
                if stats["evaluated"] >= args.budget:
                    stop.set()
                    break
            base, params, texts, parent, changed = child()
            try:
                m, vec, sound = w.evaluate(base, params, texts)
            except Exception as e:   # noqa: BLE001 — ein kaputter Kandidat hält den Lauf nicht an
                with archive.lock:
                    stats["failed"] += 1
                    stats["failedInRow"] += 1
                    if stats["failedInRow"] >= 50:
                        reason[0] = f"50 failures in a row (last: {e})"
                        stop.set()
                log(f"  failed: {e}")
                w.base = None
                continue
            feats = {k: m.get(k) for k in ("lufs", "brightness", "width", "attack", "sustain", "tail")}
            feats["silent"] = m["silent"]
            entry = {"base": base, "params": params, "features": feats}
            if texts:
                entry["texts"] = texts
            got = None
            if m["silent"]:
                why = "silent"
            elif (m.get("lufs") or -70) <= -45:
                why = "quiet"
            elif not vec:
                why = "no clap"
            else:
                entry["clap"] = vec
                if sound:
                    entry["sound"] = sound
                why = archive.offer(entry)
                if why in ("added", "replaced"):
                    got, why = why, None
                    try:
                        full = w.voice(m)
                    except Exception as e:   # noqa: BLE001
                        log(f"  voice metrics failed: {e}")
                        full = None
                    with archive.lock:
                        if full:
                            entry["features"], entry["phrase"] = full, ppr.PHRASE
                        elif entry in archive.entries:
                            # ohne volle Merkmale nicht in den Katalog (sonst gälte er als alt gemessen)
                            archive.entries.remove(entry)
                            archive.cells[entry["_cell"]].remove(entry)
            with archive.lock:
                if why:
                    reasons[why] = reasons.get(why, 0) + 1
                if parent is not None and params == parent["params"]:
                    reasons["unchanged"] = reasons.get("unchanged", 0) + 1
                if parent is not None and parent.get("_vec") and vec:
                    new = cos(decode(vec), parent["_vec"]) < args.similar
                    if not new:
                        reasons["as parent"] = reasons.get("as parent", 0) + 1
                    for n in changed & set(params):
                        tried[n] = tried.get(n, 0) + 1
                        moved[n] = moved.get(n, 0) + new

                stats["evaluated"] += 1
                stats["failedInRow"] = 0
                stats[got or "rejected"] += 1
                # nur Zuwachs zählt für den Abbruch — Ersetzen verfeinert bloß
                stats["recent"] = (stats["recent"] + [got == "added"])[-args.window:]
                n, rate = stats["evaluated"], sum(stats["recent"]) / max(1, len(stats["recent"]))
                if n >= args.min_evals and len(stats["recent"]) >= args.window and rate < args.stop_rate:
                    reason[0] = f"saturated ({rate:.1%} new in the last {args.window})"
                    stop.set()
            if n % 50 == 0:
                el = time.time() - t0
                log(f"  {n} evaluated, {len(archive.entries)} in archive, {len(archive.cells)} cells, "
                    f"{rate:.0%} new recently ({stats['replaced']} replaced), {n / el * 60:.0f}/min, "
                    f"rejected: {json.dumps(reasons, sort_keys=True)}")
            if n % 200 == 0:
                save()

    def save():
        with archive.lock:
            sounds = [e for e in archive.entries if not e.get("preset")]
            # Ids bleiben beim Fortsetzen (loadPreset {sound}), neue Klänge zählen weiter
            top = max((int(e["id"][1:]) for e in sounds if e.get("id")), default=0)
            for e in sounds:
                if not e.get("id"):
                    top += 1
                    e["id"] = f"s{top:04d}"
        out = []
        for e in sorted(sounds, key=lambda e: e["id"]):
            f = e["features"]
            cls, by = ppr.classify("", "", f)
            # Klasse der ähnlichsten benannten Presets (CLAP, Mehrheit der drei
            # nächsten, wenn sie nah genug sind) — der Klang allein trennt nur
            # Pad/Pluck/Synth
            near = sorted(((cos(e["_vec"], v), c) for v, c in named), reverse=True)[:3]
            votes = {}
            for sim, c in near:
                if sim >= args.class_similar:
                    votes[c] = votes.get(c, 0) + sim
            if votes:
                cls, by = max(votes, key=votes.get), "nearest presets"
            words = ppr.words_for(f)
            base = dict(e["base"])
            if "path" in base:   # relativ, ohne Benutzername (profile_plugin.catalog_path)
                base["path"] = pp.catalog_path(base["path"])
            s = {"id": e["id"], "name": f"{' '.join(words[:2]) or 'plain'} {cls} · {base['name']}",
                 "base": base, "params": e["params"], "class": cls, "classFrom": by, "words": words,
                 "features": f, "clap": e["clap"]}
            for k in ("texts", "sound", "phrase"):
                if k in e:
                    s[k] = e[k]
            out.append(s)
        result = {
            "plugin": {k: info.get(k) for k in ("name", "format", "uid", "vendor", "version")},
            "generated": datetime.datetime.now().isoformat(timespec="seconds"),
            "signal": ppr.SIGNAL,
            "phrase": ppr.PHRASE,
            "explore": {"parameters": names,
                        "effective": sorted(((n, round(moved.get(n, 0) / tried[n], 2)) for n in tried if tried[n] >= 5),
                                            key=lambda x: -x[1])[:40],
                        "learned": {n: [tried[n], moved.get(n, 0)] for n in sorted(tried)}, "evaluated": stats["evaluated"], "added": stats["added"], "replaced": stats["replaced"],
                        "failed": stats["failed"], "rejected": dict(sorted(reasons.items())),
                        "cells": len(archive.cells), "stopped": reason[0],
                        "perCell": args.per_cell, "similar": args.similar},
            "sounds": out,
        }
        tmp = out_path + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(result, fh, indent=1, ensure_ascii=False)
        os.replace(tmp, out_path)

    # SIGTERM/SIGINT: fertig rechnen, speichern, dawds per quit beenden (im
    # Hintergrund gestartet ignoriert die Shell SIGINT — deshalb beide)
    def on_signal(signum, _frame):
        reason[0] = f"signal {signal.Signals(signum).name}"
        stop.set()

    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, on_signal)
    threads = [threading.Thread(target=run, args=(w,), daemon=True) for w in workers]
    try:
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        reason[0] = "interrupted"
        stop.set()
        for t in threads:
            t.join()
    finally:
        save()
        for w in workers:
            w.close()
    log(f"wrote {out_path}: {sum(1 for e in archive.entries if not e.get('preset'))} sounds in {len(archive.cells)} cells, "
        f"{stats['evaluated']} evaluated, stopped: {reason[0]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("--format")
    ap.add_argument("--budget", type=int, default=3000, help="höchstens so viele Kandidaten")
    ap.add_argument("--workers", type=int, default=4, help="parallele dawd-Instanzen")
    ap.add_argument("--seeds", type=int, default=40, help="Presets als Startpunkte")
    ap.add_argument("--per-cell", type=int, default=4, help="Klänge je Fach")
    ap.add_argument("--similar", type=float, default=0.92, help="CLAP-Ähnlichkeit, ab der zwei Klänge gleich gelten")
    ap.add_argument("--class-similar", type=float, default=0.85, help="Ähnlichkeit, ab der ein Preset seine Klasse vererbt")
    ap.add_argument("--window", type=int, default=300, help="Fenster für die Abbruch-Rate")
    ap.add_argument("--stop-rate", type=float, default=0.02, help="Schluss unter diesem Anteil neuer Klänge")
    ap.add_argument("--min-evals", type=int, default=500)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    explore(args.name, args.format, args)


if __name__ == "__main__":
    main()
