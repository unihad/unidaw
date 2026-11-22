#!/usr/bin/env python3
"""Preset-Katalog eines Instruments: jedes Preset mit derselben Phrase rendern,
Merkmale messen, daraus Klasse und Merkmalsworte ableiten.

    tools/profile_presets.py "MDA JX10" --format LV2 [--limit 20] [--port 7720]
    tools/profile_presets.py --core          # alle Kernset-Instrumente mit Presets
    tools/profile_presets.py --reclassify    # Klassen/Worte neu aus den gespeicherten Merkmalen
    tools/profile_presets.py "Helm" --format LV2 --sounds   # erkundete Klänge (-sounds.json) neu messen

Startet eine eigene dawd-Instanz wie tools/profile_plugin.py (--no-audio unter
bwrap, belegt kein Gerät) oder nutzt --port einer laufenden.

Je Preset: loadPreset, Render der Testphrase (profile_plugin.PHRASE, Version
= profile_plugin.VERSION: Legato-Linie, langer Ton, Staccato in zwei
Velocity-Stufen, Akkord mit Bass),
Messung mit analyze {file}, der Hüllkurve und describeTimbre {voice}:
- brightness (spektraler Schwerpunkt, Oktaven über/unter 1 kHz), lufs, width,
  attack (ms), sustain (Pegel im gehaltenen Akkord gegen die Spitze, dB),
  tail (Ausklingen nach dem Loslassen, s)
- class: aus Kategorie/Name (Basses, "1 BASS", "Pad" …), sonst aus dem Klang
  (Pluck, Pad, Lead/Synth); words: bright/dark, hard/soft attack, sustained/
  decaying, long tail/short release, wide/mono, silent
- clap: CLAP-Vektor des Renders (analyze {embed}) und sound (Merkmale wie
  in der Sample-Suche) — dafür findet findPresets {text | like} das Preset;
  ohne Vektor gilt ein Preset beim nächsten Lauf als offen (--no-embed: ohne)
- ab Version 2 (phrase: VERSION je Eintrag; andere gelten als offen): dazu
  sweep, glide, vibrato, beating, detune, correlation, velocity (profile_plugin)
  und die Worte legato/glide, vibrato, detuned, filter sweep, wide anti-phase

Ergebnis: dawd/plugin-profiles/<format>-<uid>-presets.json — findPresets
sucht darin (Klasse, Worte, Name, Kategorie). Wird alle 25 Presets
geschrieben; ein zweiter Lauf misst nur, was noch fehlt.
"""
import argparse
import datetime
import json
import math
import os
import re
import signal
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_plugin as pp  # noqa: E402

# Kategorie-/Namensworte -> Klasse (erste passende Regel gewinnt; Diva-Ordner
# "1 BASS" … "7 EFFECTS", Surge-Kategorien, übliche Präfixe wie "BA ", "PD ").
# Klangklassen vor fx: "5th Sweep Pad" ist ein Pad, kein Effekt.
# Neue Klassen/Worte auch in presetSynonyms() (dawd/source/ops/Presets.cpp)
# eintragen: Suchworte von findPresets query/text.
CLASS_HINTS = [
    ("drum", r"\b(bass ?drums?|kick ?drums?|808 kick)\b"),   # vor bass: "Bass Drum" ist eine Trommel
    ("pad", r"\b(pads?|dream synth|pd|choir|swell)\b"),
    ("bass", r"\b(bass(es)?|sub|ba)\b"),
    ("lead", r"\b(leads?|ld|solo)\b"),
    ("drum", r"\b(drums?|kick|snare|hats?|perc|percussion|toms?)\b"),
    ("pluck", r"\b(plucks?|percussive|pl|pizz\w*|mallets?|marimba|harp)\b"),
    ("bell", r"\b(bells?|chimes?|glock\w*|vibes?)\b"),
    ("keys", r"\b(keys|piano|pianet|e\.?-?piano|ep|rhodes|wurli\w*|clav\w*|harpsichord)\b"),
    ("organ", r"\b(organs?|org)\b"),
    ("strings", r"\b(strings?|violin|cello|orchestra\w*)\b"),
    ("brass", r"\b(brass|horns?|trumpet|sax\w*)\b"),
    ("seq", r"\b(seq\w*|arp\w*|rhythmic|rhythms?|gated)\b"),
    ("poly", r"\b(poly ?synths?|polys?|chords?|stabs?)\b"),
    ("fx", r"\b(fx|sfx|effects?|noise|riser|sweep|atmos\w*|soundscape|texture)\b"),
]

# Merkmalsworte: Schwellen in den Einheiten der Messung
WORDS = [
    # Schwerpunkt der Phrase (C2…C5, Oktaven um 1 kHz): Median bei MDA JX10 -1,9
    ("bright", lambda f: f.get("brightness") is not None and f["brightness"] > -1.0),
    ("dark", lambda f: f.get("brightness") is not None and f["brightness"] < -3.0),
    ("hard attack", lambda f: f.get("attack") is not None and f["attack"] <= 10),
    ("soft attack", lambda f: f.get("attack") is not None and f["attack"] >= 120),
    ("sustained", lambda f: f.get("sustain") is not None and f["sustain"] > -6),
    ("decaying", lambda f: f.get("sustain") is not None and f["sustain"] < -18),
    # Ausklingen/Breite an Surge XT (2400 Presets) kalibriert: Median 1,2 s / -11 dB
    ("long tail", lambda f: f.get("tail") is not None and f["tail"] >= 2.0),
    ("short release", lambda f: f.get("tail") is not None and f["tail"] <= 0.15),
    ("wide", lambda f: f.get("width") is not None and f["width"] > -6),
    ("mono", lambda f: f.get("width") is not None and f["width"] <= -38),
    # Version 2 (Todo 2026-10-18 P2), Schwellen gewählt — nach dem ersten
    # vollen Lauf an den Verteilungen prüfen (--reclassify)
    # glide = Anteil der vier Legato-Übergänge mit Portamento, detune = Breite
    # der Teiltöne im langen Ton (Cent); Schwellen an Stichproben gewählt (Todo
    # 2026-10-27-detune-glide-messung: Glide aus/an je Preset, benannte Presets)
    ("legato/glide", lambda f: (f.get("glide") or 0) >= 0.5),
    ("vibrato", lambda f: (f.get("vibrato") or 0) >= 20),
    ("detuned", lambda f: (f.get("detune") or 0) >= 8),
    ("filter sweep", lambda f: abs(f.get("sweep") or 0) >= 0.3),
    ("wide anti-phase", lambda f: f.get("correlation") is not None and f["correlation"] < 0),
]
V2_FEATURES = ("sweep", "glide", "vibrato", "beating", "detune", "correlation", "velocity", "glideMs", "vibratoHz")

# Phrase wie im Steckbrief der Instrumente (profile_plugin, Version 2)
NOTES = pp.PHRASE
STIM = pp.PHRASE_STIM
SIGNAL = pp.PHRASE_SIGNAL
PHRASE = pp.VERSION


def sustain_db(path):
    """Pegel im langen Ton A3 (1,6–1,95 s, bis kurz vor dem Loslassen bei
    2,0 s; Version 2: 1,8–2,2 s, Loslassen bei 2,25 s) gegen seine Spitze in den ersten
    0,3 s nach dem Einsatz (1,0 s)."""
    x, rate = pp.read_wav(path)

    def rms(a, b):
        seg = x[int(a * rate):int(b * rate)]
        return math.sqrt(sum(v * v for v in seg) / max(1, len(seg))) + 1e-12

    head = max(rms(1.0 + t / 100, 1.0 + t / 100 + 0.02) for t in range(0, 30, 2))
    # -60..+30: sehr langsame Pads haben am Anfang fast nichts (sonst +200 dB)
    return round(min(30.0, max(-60.0, 20 * math.log10(rms(1.6, 1.95) / head))), 1)


def classify(category, name, f):
    # DX7-Namen ohne Leerzeichen: "dtunPiano", "PIANOdrops", "T-E.Piano2"
    name = re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z]{3})(?=[a-z]{3})|(?<=[A-Za-z])(?=\d)", " ", name)
    hay = f"{category} {name}".lower().replace("_", " ")
    for cls, rx in CLASS_HINTS:
        if re.search(rx, hay):
            return cls, "name"
    if f.get("silent"):
        return "fx", "sound"
    if f.get("sustain") is not None and f["sustain"] < -18 and (f.get("attack") or 99) <= 20:
        return "pluck", "sound"
    if (f.get("attack") or 0) >= 120 and (f.get("sustain") or -99) > -10:
        return "pad", "sound"
    return "synth", "sound"


def words_for(f):
    if f.get("silent"):
        return ["silent"]
    return [w for w, test in WORDS if test(f)]


def catalog(api, name, fmt, workdir, limit=None, log=print, embed=True):
    proj = tempfile.mkdtemp(prefix="presets-", dir=workdir)
    api("treeClose")
    if not api("treeSeed", dir=proj, label="presets").get("ok"):
        raise RuntimeError("treeSeed failed")
    api("treeSettings", autoBranch=False)
    api("updateSection", name="Section 1", length=4)
    info = api("pluginInfo", name=name, **({"format": fmt} if fmt else {}), limit=1)
    if not info.get("ok"):
        raise RuntimeError(info.get("error"))
    path = os.path.join(pp.OUT, info["profileId"] + "-presets.json")
    old = json.load(open(path)) if os.path.exists(path) else {}
    # ohne Vektor oder mit alter Phrase gilt ein Preset als offen (stumme
    # brauchen keinen Vektor)
    done = {(p["category"], p["name"]): p for p in old.get("presets", [])
            if p.get("phrase") == PHRASE and ("clap" in p or p["features"].get("silent") or not embed)}
    keep = {(p["category"], p["name"]): p for p in old.get("presets", [])}

    t = api("createTrack", type="midi", instrument=info["name"], format=info["format"])
    if not t.get("ok"):
        raise RuntimeError(t.get("error"))
    pid = t["id"]
    r = api("addMidiClip", track=t["index"], section="Section 1", startBeat=0, lengthBeats=8, notes=NOTES)
    if not r.get("ok"):
        raise RuntimeError(r.get("error"))

    presets, offset = [], 0
    while True:
        page = api("listPresets", plugin=pid, limit=500, offset=offset)
        presets += [p for p in page.get("presets", []) if p["source"] != "dawd"]
        offset += 500
        if offset >= page.get("total", 0):
            break
    if limit:
        presets = presets[:limit]
    out = os.path.join(proj, "p.wav")

    def render():
        r = api("render", file=out, section="Section 1", tail=2, force=True)
        if not r.get("ok"):
            raise RuntimeError(r.get("error"))
        m = pp.metrics(api, out, STIM, embed=embed, voice=True)
        if not m["silent"]:
            m["sustain"] = sustain_db(out)
        return m

    render()   # einmal rendern: LV2-Plugins übernehmen Werte erst danach (--no-audio)
    log(f"{info['name']} ({info['format']}): {len(presets)} presets, {len(done)} already measured")
    entries, t0, fresh = [], time.time(), 0

    def save():
        result = {
            "plugin": {k: info.get(k) for k in ("name", "format", "uid", "vendor", "version")},
            "generated": datetime.datetime.now().isoformat(timespec="seconds"),
            "signal": SIGNAL,
            "phrase": PHRASE,
            # Einträge der alten Phrase bleiben stehen, bis sie neu gemessen
            # sind — ein abgebrochener Lauf lässt den Katalog vollständig
            "presets": sorted(entries + [p for k, p in keep.items() if k not in {(e["category"], e["name"]) for e in entries}],
                              key=lambda p: (p["category"].lower(), p["name"].lower())),
        }
        with open(path + ".tmp", "w") as fh:
            json.dump(result, fh, indent=1, ensure_ascii=False)
        os.replace(path + ".tmp", path)

    def where(p):
        # Programm, Datei oder Datei + Stimme (DX7-Cartridge in Dexed)
        if p["source"] == "program":
            return {"program": p["program"]}
        return {"path": p["path"], **({"program": p["program"]} if "program" in p else {})}

    for i, p in enumerate(presets):
        key = (p["category"], p["name"])
        if key in done:
            continue
        r = api("loadPreset", plugin=pid, **where(p))
        if not r.get("ok"):
            log(f"  skip {p['category']}/{p['name']}: {r.get('error')}")
            continue
        try:
            m = render()
        except RuntimeError as e:
            log(f"  skip {p['category']}/{p['name']}: {e}")
            continue
        feats = features_of(m)
        cls, by = classify(p["category"], p["name"], feats)
        e = {"name": p["name"], "category": p["category"], "source": p["source"], "class": cls, "classFrom": by,
             "words": words_for(feats), "features": feats, "phrase": PHRASE}
        if m.get("clap"):
            e["clap"] = m["clap"]
            if m.get("sound"):
                e["sound"] = m["sound"]
        e.update(where(p))
        if "path" in e:   # relativ, ohne Benutzername (profile_plugin.catalog_path)
            e["path"] = pp.catalog_path(e["path"])
        entries.append(e)
        fresh += 1
        if fresh % 25 == 0:
            save()
        eta = (time.time() - t0) / fresh * (len(presets) - i - 1)
        log(f"  [{i + 1}/{len(presets)}] {p['category']}/{p['name']}: {cls} ({by}) {', '.join(e['words'])}  ~{eta / 60:.1f} min left")
    save()
    api("treeClose")
    log(f"wrote {path} ({fresh} measured, {(time.time() - t0) / 60:.1f} min)")
    return path


def features_of(m):
    feats = {k: m.get(k) for k in ("lufs", "brightness", "width", "attack", "sustain", "tail")}
    feats["silent"] = m["silent"]
    for k in V2_FEATURES:
        if m.get(k) is not None:
            feats[k] = m[k]
    return feats


def sounds(api, name, fmt, workdir, limit=None, log=print):
    """Erkundete Klänge (<slug>-sounds.json, tools/explore_sounds.py) mit der
    Phrase neu messen: Merkmale, Worte, Name, CLAP-Vektor. Id, Basis,
    Parameter und Klasse bleiben (loadPreset {sound: id} lädt weiter
    dasselbe). Alle 25 Klänge geschrieben; fertig ist, was phrase = VERSION trägt."""
    proj = tempfile.mkdtemp(prefix="sounds-", dir=workdir)
    api("treeClose")
    if not api("treeSeed", dir=proj, label="sounds").get("ok"):
        raise RuntimeError("treeSeed failed")
    api("treeSettings", autoBranch=False)
    api("updateSection", name="Section 1", length=4)
    info = api("pluginInfo", name=name, **({"format": fmt} if fmt else {}), limit=1)
    if not info.get("ok"):
        raise RuntimeError(info.get("error"))
    path = os.path.join(pp.OUT, info["profileId"] + "-sounds.json")
    src = path if os.path.exists(path) else os.path.join(pp.PROFILES, info["profileId"] + "-sounds.json")
    if not os.path.exists(src):
        log(f"{info['name']}: no sounds catalogue")
        return None
    data = json.load(open(src))
    t = api("createTrack", type="midi", instrument=info["name"], format=info["format"])
    if not t.get("ok"):
        raise RuntimeError(t.get("error"))
    pid = t["id"]
    r = api("addMidiClip", track=t["index"], section="Section 1", startBeat=0, lengthBeats=8, notes=NOTES)
    if not r.get("ok"):
        raise RuntimeError(r.get("error"))
    out = os.path.join(proj, "s.wav")
    api("render", file=out, section="Section 1", tail=2, force=True)
    todo = [x for x in data["sounds"] if x.get("phrase") != PHRASE][:limit]
    log(f"{info['name']} ({info['format']}): {len(todo)} of {len(data['sounds'])} sounds to measure")

    def save():
        data["generated"] = datetime.datetime.now().isoformat(timespec="seconds")
        data["signal"], data["phrase"] = SIGNAL, PHRASE
        with open(path + ".tmp", "w") as fh:
            json.dump(data, fh, indent=1, ensure_ascii=False)
        os.replace(path + ".tmp", path)

    t0, fresh = time.time(), 0
    for i, x in enumerate(todo):
        lp = api("loadPreset", plugin=pid, sound=x["id"])
        if not lp.get("ok"):
            log(f"  skip {x['id']}: {lp.get('error')}")
            continue
        r = api("render", file=out, section="Section 1", tail=2, force=True)
        if not r.get("ok"):
            log(f"  skip {x['id']}: {r.get('error')}")
            continue
        m = pp.metrics(api, out, STIM, embed=True, voice=True)
        if not m["silent"]:
            m["sustain"] = sustain_db(out)
        f = features_of(m)
        words = words_for(f)
        x.update({"features": f, "words": words, "phrase": PHRASE,
                  "name": f"{' '.join(words[:2]) or 'plain'} {x.get('class', 'synth')} · {x['base']['name']}"})
        if m.get("clap"):
            x["clap"] = m["clap"]
            if m.get("sound"):
                x["sound"] = m["sound"]
        fresh += 1
        if fresh % 25 == 0:
            save()
        eta = (time.time() - t0) / fresh * (len(todo) - i - 1)
        log(f"  [{i + 1}/{len(todo)}] {x['id']}: {', '.join(words)}  ~{eta / 60:.1f} min left")
    save()
    api("treeClose")
    log(f"wrote {path} ({fresh} measured, {(time.time() - t0) / 60:.1f} min)")
    return path


def reclassify():
    """Klassen und Worte neu aus den gespeicherten Merkmalen (nach Änderungen an
    CLASS_HINTS/WORDS — ohne neu zu rendern)."""
    import glob
    for path in sorted(glob.glob(os.path.join(pp.OUT, "*-presets.json"))):
        data = json.load(open(path))
        for p in data["presets"]:
            if "sustain" in p["features"] and p["features"]["sustain"] is not None:
                p["features"]["sustain"] = min(30.0, max(-60.0, p["features"]["sustain"]))
            p["class"], p["classFrom"] = classify(p["category"], p["name"], p["features"])
            p["words"] = words_for(p["features"])
        with open(path, "w") as fh:
            json.dump(data, fh, indent=1, ensure_ascii=False)
        print(f"reclassified {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", nargs="?")
    ap.add_argument("--format")
    ap.add_argument("--port", type=int)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--core", action="store_true")
    ap.add_argument("--reclassify", action="store_true")
    ap.add_argument("--no-embed", action="store_true", help="ohne CLAP-Vektoren (findPresets text/like findet die Presets dann nicht)")
    ap.add_argument("--sounds", action="store_true", help="die erkundeten Klänge (-sounds.json) neu messen statt der Presets")
    ap.add_argument("--start-port", type=int, default=7720, help="first port to try for the own dawd")
    ap.add_argument("--workdir", help="folder for the own dawd (config, log, renders); default a temp folder")
    args = ap.parse_args()
    # SIGTERM (tools/remeasure_all.sh beim Abbruch) wie Ctrl-C: finally
    # beendet die eigene dawd per quit
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    if args.reclassify:
        return reclassify()

    workdir = args.workdir or tempfile.mkdtemp(prefix="dawd-presets-")
    os.makedirs(workdir, exist_ok=True)
    proc = None
    port = args.port
    if not port:
        port = pp.free_port(args.start_port)
        proc = pp.start_dawd(port, workdir)
    api = pp.Dawd(port)
    try:
        if args.core:
            core = json.load(open(os.path.join(pp.PROFILES, "coreset.json")))["plugins"]
            plugins = {p["name"]: p for p in api("listPlugins", core=True).get("plugins", [])}
            targets = [(c["name"], c["format"] if c["format"] != "builtin" else None) for c in core
                       if plugins.get(c["name"], {}).get("class") == "instrument" and c["format"] != "builtin"]
        elif args.name:
            targets = [(args.name, args.format)]
        else:
            ap.error("name or --core")
        for name, fmt in targets:
            try:
                if args.sounds:
                    sounds(api, name, fmt, workdir, args.limit)
                else:
                    catalog(api, name, fmt, workdir, args.limit, embed=not args.no_embed)
            except RuntimeError as e:
                print(f"skip {name}: {e}", flush=True)
    finally:
        if proc:
            try:
                api("quit")
            except Exception:
                pass
            proc.wait(timeout=10)


if __name__ == "__main__":
    main()
