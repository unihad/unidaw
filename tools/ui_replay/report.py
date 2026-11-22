#!/usr/bin/env python3
"""Auswertung der Nachstell-Läufe: Tabellen je Lauf × Fenstergröße.

Aufruf:
  python3 tools/ui_replay/report.py <ergebnisordner> > tabellen.md

Liest <ergebnisordner>/<lauf>-<BxH>/records.jsonl (von replay.js) und
schreibt Markdown: Übersicht, Kriterien 1–9, Op-Typen, Arbeitsschritte,
nicht erreichbare Ops, Hover-only, kleine Ziele, Merklast.
"""
import json
import os
import sys
from collections import Counter, defaultdict

ROOT = sys.argv[1]
READ = {'analyze', 'detectTempo', 'detectKey', 'analyzeStructure', 'findRepeats', 'compareReference',
        'compareVoice', 'compareDrums', 'compareStems', 'compareClasses', 'compareSeparated', 'describeVoice', 'describeTimbre',
        'describeDrums', 'describeLayers', 'audioToMidi', 'findRecipes', 'recipeInfo', 'findPresets', 'findSamples',
        'listParameters', 'listAutomatable', 'listSamples', 'pluginInfo', 'clipNotes', 'matchSound', 'detectSongBoundaries',
        'render', 'renderStems', 'renderBatch', 'exportMidi', 'separateStems', 'saveRecipe', 'rateRecipe'}

def load(d):
    recs = []
    with open(os.path.join(d, 'records.jsonl')) as f:
        for line in f:
            recs.append(json.loads(line))
    return recs

def norm(r):
    # Das UI lädt Dateien hoch (Kopie unter media/): ein anderer Pfad ist kein Unterschied
    gap = [g for g in (r.get('paramGap') or []) if g != 'file=']
    if r.get('check') == 'partial request' and not gap:
        r['check'] = 'same request'
    if r.get('check') == 'partial request' and r['op'] not in READ:
        r['check'] = 'differs'
    r['paramGap'] = gap
    # Merklast: nur Messwerte (≥ 1 mit Nachkommastellen, oder ≥ 1000) — kleine
    # Zahlen wie Fades 0.02 tauchen zufällig in Antworten auf
    def measured(v):
        try:
            f = abs(float(v))
        except ValueError:
            return False
        return (f >= 1 and '.' in v) or f >= 1000
    r['memo'] = [v for v in (r.get('memo') or []) if measured(v)]
    return r

def counted(r):
    return r.get('mode') not in ('agent', 'agentError', 'recipeStep')

def ui(r, k):
    return (r.get('ui') or {}).get(k, 0) or 0

runs = {}
for name in sorted(os.listdir(ROOT)):
    p = os.path.join(ROOT, name, 'records.jsonl')
    if os.path.exists(p):
        runs[name] = [norm(r) for r in load(os.path.join(ROOT, name))]


def acts(r):
    """Bedienhandlungen ohne Tippen: Klick, Doppelklick, Ziehen, Rad-Rasten, Dateidialog."""
    return ui(r, 'clicks') + ui(r, 'dblclicks') + ui(r, 'drags') + ui(r, 'wheel') + ui(r, 'fileDialogs')

def reveal(r):
    """Aufdecken: Scrollen, Ansichtswechsel, Hover-only."""
    return ui(r, 'scrollActs') + ui(r, 'views') + len((r.get('ui') or {}).get('hoverOnly', []))

def fmt(n):
    return f'{n:,}'.replace(',', '.')

out = []
w = out.append

# ── Übersicht ─────────────────────────────────────────────────────────
w('## Übersicht je Lauf und Fenstergröße\n')
w('| Lauf | Ops (ohne Agent-Meta) | UI ganz | UI teilweise | UI scheitert | ohne UI-Weg | Klicks | Doppelkl. | Ziehen | Tasten (davon Freitext) | Rad-Rasten | Mausweg (px) | Ansichtswechsel | Hover-only | Dateidialoge | Prompts |')
w('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for name, recs in runs.items():
    c = [r for r in recs if counted(r)]
    m = Counter(r['mode'] for r in c)
    w('| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} ({}) | {} | {} | {} | {} | {} | {} |'.format(
        name, len(c), m['ui'], m['uiPartial'], m['uiFailed'], m['noUi'],
        fmt(sum(ui(r, 'clicks') for r in c)), fmt(sum(ui(r, 'dblclicks') for r in c)), fmt(sum(ui(r, 'drags') for r in c)),
        fmt(sum(ui(r, 'keys') for r in c)), fmt(sum(ui(r, 'keysFree') for r in c)),
        fmt(sum(ui(r, 'wheel') for r in c)), fmt(int(sum(ui(r, 'mousePx') for r in c))),
        fmt(sum(ui(r, 'views') for r in c)), fmt(sum(len(r['ui'].get('hoverOnly', [])) for r in c)),
        fmt(sum(ui(r, 'fileDialogs') for r in c)), fmt(sum(ui(r, 'prompts') for r in c))))
w('')
w('Agent-Meta (ping, state, treeState, listOps …) und Fehlversuche des Agenten zählen nicht.')
w('')

# ── Kriterien 4, 6, 7 ─────────────────────────────────────────────────
w('## Ausführbarkeit, Rückmeldung, Merklast\n')
w('| Lauf | Ändernde Ops: gleicher Befehl | gleicher Zustand | Zustand weicht ab | Lese-/Datei-Ops: gleiche Anfrage | Anfrage mit Lücken | keine Anfrage | Ergebnis ohne Extra-Klick im Bild | nur als Toast (3,2 s) | Ergebnis außerhalb des Bildes | Warten > 2 s ohne Fortschritt (Ops) | Merklast (Werte) |')
w('|---|---|---|---|---|---|---|---|---|---|---|---|')
for name, recs in runs.items():
    c = [r for r in recs if counted(r) and r['mode'] != 'noUi']
    ch = Counter(r.get('check') for r in c)
    fb = [r.get('feedback') for r in c if r.get('feedback')]
    vis = sum(1 for f in fb if f.get('visible') and not f.get('transient'))
    toast = sum(1 for f in fb if f.get('visible') and f.get('transient'))
    off = sum(1 for f in fb if f.get('shownOffscreen') or (not f.get('visible') and not f.get('error')))
    waits = [r for r in c if (r.get('wait') or {}).get('ms', 0) > 2000 and not (r.get('wait') or {}).get('progress') and not (r.get('wait') or {}).get('toastPct')]
    memo = sum(len(r.get('memo') or []) for r in c)
    mut = Counter(r.get('check') for r in c if r['op'] not in READ)
    rd = Counter(r.get('check') for r in c if r['op'] in READ)
    w(f"| {name} | {mut['same request']} | {mut['same state']} | {mut['differs']} | {rd['same request']} | {rd['partial request']} | {rd['no request']} | {vis} | {toast} | {off} | {len(waits)} | {memo} |")
w('')

# ── Arbeitsschritte (Kriterium 1, 2, 5) ───────────────────────────────
w('## Arbeitsschritte (Node im Baum) — Kriterium 1, 2, 5\n')
w('Ein Schritt ist „alles im Blick“, wenn kein Scrollen, kein Ansichtswechsel (Modus, Popover, Karte) und kein Hover-Aufdecken nötig war.\n')
for name, recs in runs.items():
    steps = defaultdict(list)
    labels = {}
    for r in recs:
        if counted(r):
            steps[r['step']].append(r)
            labels[r['step']] = r.get('stepLabel', '')
    total = len(steps)
    clear = sum(1 for s in steps.values() if all(reveal(r) == 0 for r in s))
    w(f'### {name}: {clear} von {total} Schritten ohne Aufdecken\n')
    w('| Schritt | Ops | Klicks+Ziehen+Rad | Tasten | Rad-Rasten | Ansichtswechsel | Hover-only | Mausweg (px) | ohne UI-Weg |')
    w('|---|---|---|---|---|---|---|---|---|')
    ranked = sorted(steps.items(), key=lambda kv: -sum(acts(r) for r in kv[1]))
    for sid, s in ranked[:15]:
        w('| {} | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
            labels[sid][:48], len(s), sum(acts(r) for r in s), sum(ui(r, 'keys') for r in s),
            sum(ui(r, 'wheel') for r in s), sum(ui(r, 'views') for r in s),
            sum(len(r['ui'].get('hoverOnly', [])) for r in s), fmt(int(sum(ui(r, 'mousePx') for r in s))),
            sum(1 for r in s if r['mode'] == 'noUi')))
    w('')

# ── Op-Typen ─────────────────────────────────────────────────────────
w('## Op-Typen: Aufwand je Op (Mittelwert) und Erreichbarkeit\n')
ops = sorted({r['op'] for recs in runs.values() for r in recs if counted(r)})
names = list(runs)
w('| Op | ' + ' | '.join(f'{n}: Anzahl / ohne UI / Aktionen ⌀ / Rad ⌀' for n in names) + ' |')
w('|---|' + '---|' * len(names))
for op in ops:
    cells = []
    for n in names:
        c = [r for r in runs[n] if counted(r) and r['op'] == op]
        if not c:
            cells.append('–')
            continue
        u = [r for r in c if r['mode'] != 'noUi']
        cells.append('{} / {} / {} / {}'.format(len(c), len(c) - len(u),
                     round(sum(acts(r) for r in u) / len(u), 1) if u else '–',
                     round(sum(ui(r, 'wheel') for r in u) / len(u), 1) if u else '–'))
    w(f'| {op} | ' + ' | '.join(cells) + ' |')
w('')

# ── Kostentreiber: Summen je Op-Typ und Ansichtswechsel nach Art ──────
w('## Kostentreiber: Summen je Op-Typ (alle Läufe, je Fenstergröße)\n')
for size in sorted({n.rsplit('-', 1)[1] for n in names}):
    tot = defaultdict(lambda: Counter())
    for n in names:
        if not n.endswith(size):
            continue
        for r in runs[n]:
            if counted(r) and r['mode'] != 'noUi':
                t = tot[r['op']]
                t['n'] += 1; t['acts'] += acts(r); t['wheel'] += ui(r, 'wheel'); t['keys'] += ui(r, 'keys') - ui(r, 'keysFree')
                t['views'] += ui(r, 'views'); t['drags'] += ui(r, 'drags')
    w(f'### {size}\n')
    w('| Op | Ops | Aktionen gesamt | davon Rad-Rasten | Ziehen | Tasten (ohne Freitext) | Ansichtswechsel |')
    w('|---|---|---|---|---|---|---|')
    for op, t in sorted(tot.items(), key=lambda kv: -kv[1]['acts'])[:20]:
        w(f"| {op} | {t['n']} | {t['acts']} | {t['wheel']} | {t['drags']} | {t['keys']} | {t['views']} |")
    w('')
w('## Ansichtswechsel nach Art\n')
vc = Counter()
for n, recs in runs.items():
    for r in recs:
        if counted(r):
            for v in r['ui'].get('viewList', []):
                vc[v] += 1
w('| Wechsel | Anzahl (alle) |')
w('|---|---|')
for v, k in vc.most_common(20):
    w(f'| {v} | {k} |')
w('')

# ── Nicht erreichbar, Lücken ─────────────────────────────────────────
w('## Ohne UI-Weg (Kriterium 3)\n')
reasons = Counter()
for n, recs in runs.items():
    for r in recs:
        if counted(r) and r['mode'] == 'noUi':
            reasons[(r['op'], r.get('reason', ''))] += 1
w('| Op | Grund | Anzahl (alle Läufe/Größen) |')
w('|---|---|---|')
for (op, why), k in reasons.most_common():
    w(f'| {op} | {why} | {k} |')
w('')
w('## UI-Weg mit Lücken (Parameter nicht im UI)\n')
gaps = Counter()
for n, recs in runs.items():
    for r in recs:
        if counted(r):
            for g in r.get('partial') or []:
                gaps[(r['op'], g)] += 1
w('| Op | Lücke | Anzahl |')
w('|---|---|---|')
for (op, g), k in gaps.most_common(60):
    w(f'| {op} | {g} | {k} |')
w('')
w('## UI-Weg scheitert\n')
fails = Counter()
for n, recs in runs.items():
    for r in recs:
        if counted(r) and r['mode'] == 'uiFailed':
            fails[(r['op'], (r.get('error') or '')[:90])] += 1
w('| Op | Fehler | Anzahl |')
w('|---|---|---|')
for (op, e), k in fails.most_common(40):
    w(f'| {op} | {e} | {k} |')
w('')

# ── Hover-only, kleine Ziele (Kriterium 8) ───────────────────────────
w('## Nur bei Hover sichtbar (Kriterium 8)\n')
hov = Counter()
for n, recs in runs.items():
    for r in recs:
        if counted(r):
            for d in r['ui'].get('hoverOnly', []):
                hov[d] += 1
w('| Element | Benutzungen |')
w('|---|---|')
for d, k in hov.most_common():
    w(f'| {d} | {k} |')
w('')
w('## Klickziele kleiner als 24 px (Kriterium 8)\n')
small = {}
for n, recs in runs.items():
    for r in recs:
        if counted(r):
            for s in r['ui'].get('small', []):
                key = s['desc']
                e = small.setdefault(key, {'w': s['w'], 'h': s['h'], 'n': 0})
                e['n'] += 1
w('| Element | Größe (px) | Benutzungen |')
w('|---|---|---|')
for d, e in sorted(small.items(), key=lambda kv: -kv[1]['n']):
    w(f"| {d} | {e['w']} × {e['h']} | {e['n']} |")
w('')

# ── Merklast (Kriterium 7) ───────────────────────────────────────────
w('## Merklast: abgelesen und eingetippt (Kriterium 7)\n')
w('| Lauf | Op | Werte |')
w('|---|---|---|')
for n, recs in runs.items():
    for r in recs:
        if counted(r) and r.get('memo'):
            w(f"| {n} | {r['op']} (#{r['seq']}) | {', '.join(r['memo'][:8])} |")
w('')

print('\n'.join(out))
