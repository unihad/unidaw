#!/usr/bin/env python3
"""Op-Folgen der Agent-Läufe aus events.jsonl ziehen (streamend, nur Zeitfenster).

Aufruf:
  python3 tools/ui_replay/extract.py ~/dev/agent-daw/events.jsonl OUTDIR

Schreibt je Lauf OUTDIR/<lauf>.ops.jsonl (seq, ts, op, params, result) und
druckt eine Op-Statistik. Die Datei ist am Anfang mehrzeilig formatiert;
gelesen werden nur einzeilige Einträge ({"seq" am Zeilenanfang).
"""
import json
import sys
from datetime import datetime

# Zeitfenster laut Todo 2026-10-29-ui-nachstell-lauf.md (Ortszeit, mit Puffer)
LAEUFE = {
    "nachbau-14": ("2026-10-22T20:55:00+02:00", "2026-10-22T21:40:00+02:00"),
    "remix-1": ("2026-10-29T11:57:00+01:00", "2026-10-29T12:50:00+01:00"),
    "stil-1": ("2026-10-29T13:07:00+01:00", "2026-10-29T14:00:00+01:00"),
}


def main():
    src, out = sys.argv[1], sys.argv[2]
    fenster = {k: (datetime.fromisoformat(a), datetime.fromisoformat(b))
               for k, (a, b) in LAEUFE.items()}
    dateien = {k: open(f"{out}/{k}.ops.jsonl", "w") for k in LAEUFE}
    zahl = {k: {} for k in LAEUFE}
    with open(src, "rb") as f:
        for raw in f:
            if not raw.startswith(b'{"seq"'):
                continue
            # schneller Vorfilter über das Datum im Zeitstempel
            kopf = raw[:80]
            if b"2026-10-22T2" not in kopf and b"2026-10-29T1" not in kopf:
                continue
            e = json.loads(raw)
            ts = datetime.fromisoformat(e["ts"])
            for k, (a, b) in fenster.items():
                if a <= ts <= b:
                    dateien[k].write(json.dumps(e) + "\n")
                    zahl[k][e["op"]] = zahl[k].get(e["op"], 0) + 1
    for k, z in zahl.items():
        dateien[k].close()
        print(k, sum(z.values()), "ops")
        for op, n in sorted(z.items(), key=lambda x: -x[1]):
            print(f"  {op:24s} {n}")


if __name__ == "__main__":
    main()
