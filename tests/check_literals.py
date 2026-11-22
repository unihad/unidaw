#!/usr/bin/env python3
"""Findet C++-Stringliterale mit Nicht-ASCII-Zeichen (z. B. "—", Umlaute),
die nicht in juce::String::fromUTF8 (...) stehen. JUCE liest const char* nicht
als UTF-8 — solche Texte kommen in den API-Antworten als Zeichensalat an
("â\\x80\\x94"). Laeuft in tests/run.sh vor dem Bauen; Exit 1 bei Treffern."""
import pathlib
import re
import sys

fix = "--fix" in sys.argv   # Treffer gleich in fromUTF8 (...) einpacken

root = pathlib.Path(__file__).resolve().parent.parent / "dawd" / "source"
literal = re.compile(r'"(?:[^"\\]|\\.)*"')
bad = []
for path in sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.h")):
    if fix:
        lines = path.read_text(encoding="utf-8").split("\n")
        wrap = re.compile(r'(?<!fromUTF8 \()("(?:[^"\\]|\\.)*[^\x00-\x7f](?:[^"\\]|\\.)*")')
        new = [l if l.lstrip().startswith("//") else wrap.sub(r"juce::String::fromUTF8 (\1)", l) for l in lines]
        if new != lines:
            path.write_text("\n".join(new), encoding="utf-8")
            print("fixed", path.relative_to(root.parent.parent))
    for no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        code = line.split("//")[0] if not line.lstrip().startswith("//") else ""
        for m in literal.finditer(code):
            if any(ord(ch) > 127 for ch in m.group(0)):
                before = code[:m.start()].rstrip()
                if not before.endswith("fromUTF8 ("):
                    bad.append(f"{path.relative_to(root.parent.parent)}:{no}: {m.group(0)[:70]}")
if bad:
    print("Nicht-ASCII-Literale ohne juce::String::fromUTF8 (kommen als Zeichensalat an):")
    print("\n".join("  " + b for b in bad))
    sys.exit(1)
