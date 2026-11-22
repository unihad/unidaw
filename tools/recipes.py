#!/usr/bin/env python3
"""Rezept-Katalog pflegen (dawd: saveRecipe, findRecipes, applyRecipe, rateRecipe).

    tools/recipes.py index                # recipes/README.md + je Rezept recipe.md
    tools/recipes.py index --drafts       # Entwürfe aus ~/.config/unidaw/recipes dazu (nur Ausgabe)
    tools/recipes.py duplicates           # Rezepte mit gleichen Zutaten
    tools/recipes.py freeze <lauf-ordner> # Katalog für einen Praxistest einfrieren

Ablage: geprüfte Rezepte im Repo unter recipes/<kind>/<id>/ (recipe.json,
state/*.dawdpreset), Entwürfe und verworfene in <config>/recipes/. Das Markdown
ist zum Lesen für Menschen und wird erzeugt — nicht von Hand ändern.

freeze schreibt <lauf-ordner>/recipes-version.json: Commit, Stand des Ordners
(uncommittete Änderungen zählen mit) und je geprüftem Rezept ein Hash. Damit
steht im Testordner, mit welchem Katalog der Agent gearbeitet hat.
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKED = os.path.join(REPO, "recipes")
DRAFTS = os.path.expanduser("~/.config/unidaw/recipes")
KINDS = {"sound": "Klang", "chain": "Kette", "workflow": "Arbeitsablauf", "mixTarget": "Mix-Ziel"}


def load(root):
    out = []
    if not os.path.isdir(root):
        return out
    for base, _, files in os.walk(root):
        if "recipe.json" in files:
            path = os.path.join(base, "recipe.json")
            try:
                with open(path) as fh:
                    r = json.load(fh)
            except (OSError, ValueError) as e:
                print(f"kaputt: {path}: {e}", file=sys.stderr)
                continue
            if r.get("id"):
                out.append((base, r))
    return sorted(out, key=lambda x: (x[1].get("kind", ""), x[1].get("title", "").lower()))


def stars(r):
    n = int((r.get("rating") or {}).get("stars") or 0)
    return "★" * n + "☆" * (5 - n) if n else "–"


def use(r):
    s = r.get("stats") or {}
    return f"{s.get('ok', 0)}/{s.get('used', 0)}" if s.get("used") else "–"


def step_line(s):
    if s.get("op"):
        params = json.dumps(s.get("params", {}), ensure_ascii=False)
        bind = f" → {s['bind']}" if s.get("bind") else ""
        opt = " (optional)" if s.get("optional") else ""
        text = f" — {s['text']}" if s.get("text") else ""
        return f"`{s['op']} {params}`{bind}{opt}{text}"
    return s.get("text", "")


def markdown(r):
    lines = [f"# {r['title']}", "",
             f"*{KINDS.get(r.get('kind'), r.get('kind'))}* · Status: {r.get('status', 'draft')} · "
             f"Bewertung: {stars(r)} · erfolgreich/genutzt: {use(r)}", "",
             r.get("goal", ""), ""]
    if r.get("tags"):
        lines += ["Tags: " + ", ".join(r["tags"]), ""]
    if r.get("tracks"):
        lines += ["## Spuren", ""] + [f"- `{t['ref']}`: {t.get('type', 'audio')}" for t in r["tracks"]] + [""]
    if r.get("modules"):
        lines += ["## Zutaten", ""]
        for m in r["modules"]:
            head = f"- `{m['ref']}` **{m['plugin']}** ({m.get('format', 'builtin')}, {m.get('class', '')})"
            if m.get("wet") is not None and m.get("wet") < 0.999:
                head += f", wet {m['wet']}"
            if m.get("master"):
                head += ", Master-Kette"
            lines.append(head)
            for p in m.get("parameters", []):
                unit = f" {p['unit']}" if p.get("unit") and p["unit"] not in p.get("text", "") else ""
                lines.append(f"  - {p['name']}: {p.get('text', '')}{unit}")
            more = m.get("changedParameters", 0) - len([p for p in m.get("parameters", []) if "real" in p])
            if more > 0:
                lines.append(f"  - … und {more} weitere (im Zustand)")
        lines.append("")
    if r.get("steps"):
        lines += ["## Schritte", ""] + [f"{i}. {step_line(s)}" for i, s in enumerate(r["steps"], 1)] + [""]
    if r.get("notes"):
        lines += ["## Notizen", "", r["notes"], ""]
    if r.get("warnings"):
        lines += ["## Nicht enthalten", ""] + [f"- {w}" for w in r["warnings"]] + [""]
    o = r.get("origin") or {}
    if o:
        lines += ["## Herkunft", "",
                  f"Projekt `{o.get('project', '')}`, Node {o.get('node', '')} ({o.get('label', '')}), "
                  f"{o.get('created', '')[:10]}, {o.get('author', '')}", ""]
        if o.get("note"):
            lines += ["> " + o["note"].replace("\n", "\n> "), ""]
    c = (r.get("rating") or {}).get("comment")
    if c:
        lines += ["## Kommentar", "", c, ""]
    fb = r.get("feedback") or []
    if fb:
        lines += ["## Rückmeldungen", ""]
        for f in fb[-10:]:
            ok = {True: "ok", False: "nicht ok"}.get(f.get("ok"), "–")
            lines.append(f"- {f.get('date', '')[:10]} ({ok}): {f.get('note', '')}")
        lines.append("")
    if r.get("variants"):
        lines += ["Varianten: " + ", ".join(f"`{v}`" for v in r["variants"]), ""]
    lines.append(f"<!-- erzeugt von tools/recipes.py aus recipe.json ({r['id']}) -->")
    return "\n".join(lines) + "\n"


def cmd_index(args):
    found = load(CHECKED)
    for base, r in found:
        with open(os.path.join(base, "recipe.md"), "w") as fh:
            fh.write(markdown(r))
    rows = ["# Rezept-Katalog", "",
            "Geprüfte Rezepte für dawd — erzeugt von `tools/recipes.py index`, nicht von Hand ändern. "
            "Agenten finden sie über `findRecipes`, wenden sie mit `applyRecipe` an.", ""]
    if not found:
        rows.append("Noch keine geprüften Rezepte.")
    for kind, label in KINDS.items():
        part = [(b, r) for b, r in found if r.get("kind") == kind]
        if not part:
            continue
        rows += [f"## {label}", "", "| Rezept | Ziel | Plugins | Bewertung | erfolgreich/genutzt |", "|---|---|---|---|---|"]
        for b, r in part:
            rel = os.path.relpath(os.path.join(b, "recipe.md"), CHECKED)
            goal = r.get("goal", "").replace("|", "/")
            rows.append(f"| [{r['title']}]({rel}) | {goal} | {', '.join(r.get('plugins', []))} | {stars(r)} | {use(r)} |")
        rows.append("")
    os.makedirs(CHECKED, exist_ok=True)
    with open(os.path.join(CHECKED, "README.md"), "w") as fh:
        fh.write("\n".join(rows).rstrip() + "\n")
    print(f"{len(found)} geprüfte Rezepte -> {os.path.join(CHECKED, 'README.md')}")
    if args.drafts:
        drafts = load(DRAFTS)
        print(f"\nEntwürfe/verworfene in {DRAFTS}: {len(drafts)}")
        for _, r in drafts:
            print(f"  [{r.get('status', 'draft')}] {r['id']}: {r['title']} — {r.get('goal', '')}")
    return 0


def fingerprint(r):
    mods = sorted((m.get("plugin", ""), m.get("format", "")) for m in r.get("modules", []))
    ops = [s.get("op") or "text" for s in r.get("steps", [])]
    return (r.get("kind"), tuple(mods), tuple(ops) if not mods else ())


def cmd_duplicates(args):
    groups = {}
    for base, r in load(CHECKED) + load(DRAFTS):
        groups.setdefault(fingerprint(r), []).append((base, r))
    dup = [g for g in groups.values() if len(g) > 1]
    for g in dup:
        mods = ", ".join(m.get("plugin", "") for m in g[0][1].get("modules", [])) or "nur Schritte"
        print(f"{g[0][1].get('kind')}: {mods}")
        for base, r in g:
            print(f"  [{r.get('status', 'draft')}] {r['id']}: {r['title']}  ({base})")
    if not dup:
        print("keine Rezepte mit gleichen Zutaten")
    return 0


def git(*a):
    try:
        return subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def cmd_freeze(args):
    found = [(b, r) for b, r in load(CHECKED) if r.get("status") == "checked"]
    recipes = {}
    for base, r in found:
        h = hashlib.sha256()
        for sub, _, files in sorted(os.walk(base)):
            for f in sorted(files):
                if f.endswith(".md"):
                    continue
                with open(os.path.join(sub, f), "rb") as fh:
                    h.update(f.encode() + b"\0" + fh.read())
        recipes[r["id"]] = {"title": r["title"], "kind": r.get("kind"), "sha256": h.hexdigest()[:16]}
    version = {
        "frozen": datetime.datetime.now().isoformat(timespec="seconds"),
        "commit": git("log", "-1", "--format=%H", "--", "recipes") or None,
        "uncommitted": bool(git("status", "--porcelain", "--", "recipes")),
        "count": len(recipes),
        "recipes": recipes,
    }
    os.makedirs(args.dir, exist_ok=True)
    out = os.path.join(args.dir, "recipes-version.json")
    with open(out, "w") as fh:
        json.dump(version, fh, indent=1, ensure_ascii=False)
    note = " (mit uncommitteten Änderungen!)" if version["uncommitted"] else ""
    print(f"{len(recipes)} geprüfte Rezepte, Commit {(version['commit'] or '–')[:10]}{note} -> {out}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("index")
    p.add_argument("--drafts", action="store_true")
    sub.add_parser("duplicates")
    p = sub.add_parser("freeze")
    p.add_argument("dir")
    args = ap.parse_args()
    return {"index": cmd_index, "duplicates": cmd_duplicates, "freeze": cmd_freeze}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
