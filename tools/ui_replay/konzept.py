#!/usr/bin/env python3
"""Auswertung des UI-Nachstell-Laufs für das Konzept Dock/Referenz.

Liest die records.jsonl je Lauf und Fenstergröße und rechnet:
- Bereiche (Kopfleiste, Baum, Timeline, Popover, Dock: Noten/Routing/
  Rezepte/Referenz) und Element-Gruppen je Bedienelement (Feld `desc`);
- Rasten je Bereich (Op rein in einem Bereich, sonst „gemischt“);
- welche Gruppen in einem Arbeitsschritt (Node) und in aufeinander
  folgenden Ops zusammen gebraucht werden (Cluster);
- mit `--model <mess.json>`: Aufwand der Skizzen (siehe `skizze_mess.js`)
  über dieselbe Op-Folge — Rasten aus der Geometrie, Reiter-Wechsel,
  verdeckte Ziele, Ergebnisse außerhalb des Bildes.

Aufruf: python3 tools/ui_replay/konzept.py <RESULTS> [--model mess.json] [--json out.json]
"""
import json
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

RUNS = ['nachbau-14', 'remix-1', 'stil-1']
SIZES = ['1920x1080', '1366x768']

# Element-Gruppe je desc (erste passende Regel gewinnt). Gruppe → Bereich
# steht im Präfix (top/tree/tl/pop/notes/route/recipe/ref).
RULES = [
    (r'^(mode |Play/Stop|Undo|Insert file|LUFS display)', 'top.bar'),
    (r'^Export \(for Analyze\)|^Analyze$|^track meter \(analyze\)|^LUFS', 'pop.analyze'),
    (r'^Export$|^popover (Range|Tail|Format|Batch)|^stems checkbox|^Batch$', 'pop.export'),
    (r'node|add note|fork|terminate|favorite', 'tree.node'),
    (r'^(folder|open project folder|\+ New folder|Create|Start a song here|project folder|recent song)', 'pop.project'),
    (r'^(clip|duplicate ⧉|delete clip|MIDI clip|popover (Fade|Gain|Volume|Transpose|Time|BPM|Bars))', 'tl.clip'),
    (r'^(section|delete section|duplicate section|hover target of (section|delete section)|custom colour|tempo tag)', 'tl.section'),
    (r'^(zoom|ruler)', 'tl.nav'),
    (r'^(track header|track name|lane header|solo button|mute button|\+ Audio|\+ MIDI|no instrument|pick an instrument|instrument)', 'tl.track'),
    (r'^(automation|A \(automation|∿ LFO|LFO wave|modifier|rate$|steps$)', 'tl.auto'),
    (r'^(piano roll|Add notes as clip|Draw a MIDI clip|Done$)', 'notes.roll'),
    (r'^(fader|pan slider|routing Mixer)', 'route.mixer'),
    (r'^(parameter slider|setting |wet slider|preset|Load preset|sample|plugin name \(card\))', 'route.card'),
    (r'^(plugin name|routing Simple|\+ |remove plugin)', 'route.chain'),
    (r'^(recipe|Save recipe)', 'recipe'),
    (r'^Load reference', 'ref.load'),
    (r'^(Compare|Tempo|Key|Structure)$', 'ref.song'),
    (r'^(Voice|Drums|Match sound|Tune)$', 'ref.track'),
    (r'^(Repeats|Layers|Classes|Cross-check|Split)$', 'ref.part'),
    (r'^(To MIDI|Stems)', 'ref.make'),
]
GROUP_NAMES = {
    'top.bar': 'Kopfleiste (Modus, Play, Undo, Einfügen)',
    'pop.analyze': 'Analyse (Spur-Meter, Export-Popover → Analyze)',
    'pop.export': 'Export-Popover',
    'tree.node': 'Baum (Node, Notiz, Fork, Terminate)',
    'pop.project': 'Projekt-Dialog',
    'tl.clip': 'Timeline: Clips und Clip-Popover',
    'tl.section': 'Timeline: Sections',
    'tl.nav': 'Timeline: Zoom, Lineal',
    'tl.track': 'Timeline: Spurkopf, neue Spur',
    'tl.auto': 'Timeline: Automation, LFO',
    'notes.roll': 'Dock: Piano-Roll',
    'route.mixer': 'Dock: Mixer',
    'route.card': 'Dock: Plugin-Karte (Parameter, Presets, Samples)',
    'route.chain': 'Dock: Routing-Kette (Plugin wählen, + Plugin)',
    'recipe': 'Dock: Rezepte',
    'ref.load': 'Dock: Referenz laden',
    'ref.song': 'Dock: Referenz Song-Knöpfe (Compare, Tempo, Key, Structure)',
    'ref.track': 'Dock: Referenz Spur-Knöpfe (Voice, Drums, Match, Tune)',
    'ref.part': 'Dock: Referenz Teil-Knöpfe (Repeats, Layers, Classes …)',
    'ref.make': 'Dock: Referenz erzeugt (To MIDI, Stems)',
}


def group_of(desc):
    d = re.sub(r'^hover target of ', '', desc)
    for pat, g in RULES:
        if re.search(pat, d):
            return g
    return 'other'


def region(g):
    if g.startswith(('notes', 'route', 'recipe', 'ref')):
        return 'dock'
    return g.split('.')[0]


def load(results, run, size):
    p = Path(results) / f'{run}-{size}' / 'records.jsonl'
    return [json.loads(l) for l in p.open()]


def ui_ops(recs):
    return [r for r in recs if r.get('mode') in ('ui', 'uiPartial', 'uiFailed', 'recipeStep') and r['ui']['elems']]


def analyse(results):
    out = {}
    for size in SIZES:
        reg_wheel = Counter()
        reg_ops = Counter()
        grp_wheel = Counter()
        grp_ops = Counter()
        op_wheel = defaultdict(Counter)   # op → {region: wheel}
        pair_step = Counter()
        grp_step = Counter()
        pair_next = Counter()
        steps_total = 0
        unknown = Counter()
        obscured = Counter()
        feedback_off = Counter()
        prompts = Counter()
        for run in RUNS:
            recs = load(results, run, size)
            steps = defaultdict(set)
            prev = None
            for r in recs:
                u = r['ui']
                gs = [group_of(e['desc']) for e in u['elems']]
                for e, g in zip(u['elems'], gs):
                    if g == 'other':
                        unknown[e['desc']] += 1
                for o in u['obscured']:
                    obscured[(r['op'], o, group_of(o))] += 1
                fb = r.get('feedback')
                if fb and not fb.get('visible'):
                    feedback_off[(r['op'], fb.get('where', ''))] += 1
                if u['prompts']:
                    prompts[r['op']] += u['prompts']
                if not gs:
                    continue
                regs = {region(g) for g in gs if g != 'other'}
                key = regs.pop() if len(regs) == 1 else 'gemischt'
                reg_wheel[key] += u['wheel']
                reg_ops[key] += 1
                op_wheel[r['op']][key] += u['wheel']
                ugs = set(gs) - {'other'}
                for g in ugs:
                    grp_ops[g] += 1
                    grp_wheel[g] += u['wheel'] / len(ugs)
                steps[(run, r['step'])] |= ugs
                if prev is not None and prev[0] == r['step']:
                    for a in prev[1]:
                        for b in ugs:
                            if a != b:
                                pair_next[tuple(sorted((a, b)))] += 1
                prev = (r['step'], ugs)
            for s, gset in steps.items():
                steps_total += 1
                for g in gset:
                    grp_step[g] += 1
                for a in gset:
                    for b in gset:
                        if a < b:
                            pair_step[(a, b)] += 1
        out[size] = dict(reg_wheel=reg_wheel, reg_ops=reg_ops, grp_wheel=grp_wheel, grp_ops=grp_ops,
                         op_wheel=op_wheel, pair_step=pair_step, grp_step=grp_step, pair_next=pair_next,
                         steps_total=steps_total, unknown=unknown, obscured=obscured,
                         feedback_off=feedback_off, prompts=prompts)
    return out


def report(a):
    w = print
    for size in SIZES:
        d = a[size]
        tot = sum(d['reg_wheel'].values())
        w(f'\n## {size}: Rasten je Bereich (UI-Ops mit Elementen)\n')
        w('| Bereich | Ops | Rasten | Anteil | ⌀ je Op |')
        w('|---|---|---|---|---|')
        for k, v in d['reg_wheel'].most_common():
            n = d['reg_ops'][k]
            w(f'| {k} | {n} | {v} | {100 * v / tot:.0f} % | {v / n:.0f} |')
        w(f'| Summe | {sum(d["reg_ops"].values())} | {tot} | | |')
        w(f'\n### {size}: Gruppen (Rasten anteilig, wenn eine Op mehrere Gruppen hat)\n')
        w('| Gruppe | Ops | Rasten | Schritte mit Gruppe |')
        w('|---|---|---|---|')
        for g, v in sorted(d['grp_wheel'].items(), key=lambda x: -x[1]):
            w(f'| {GROUP_NAMES.get(g, g)} | {d["grp_ops"][g]} | {v:.0f} | {d["grp_step"][g]} von {d["steps_total"]} |')
        w(f'\n### {size}: Paare im selben Arbeitsschritt (Node) — Top 25\n')
        w('| Gruppe A | Gruppe B | Schritte | Übergänge Op→Op |')
        w('|---|---|---|---|')
        for (x, y), n in d['pair_step'].most_common(25):
            w(f'| {GROUP_NAMES.get(x, x)} | {GROUP_NAMES.get(y, y)} | {n} | {d["pair_next"][(x, y)]} |')
        w(f'\n### {size}: Verdeckte Ziele nach Gruppe\n')
        oc = Counter()
        for (op, o, g), n in d['obscured'].items():
            oc[g] += n
        w(', '.join(f'{GROUP_NAMES.get(g, g)}: {n}' for g, n in oc.most_common()))
        w(f'\n### {size}: Ergebnis außerhalb des Bildes / nur Toast\n')
        for (op, wh), n in sorted(d['feedback_off'].items(), key=lambda x: -x[1])[:15]:
            w(f'- {op} — {wh}: {n}')
        w(f'\nPrompts je Op: ' + ', '.join(f'{k} {v}' for k, v in d['prompts'].most_common()))
        if d['unknown']:
            w('\nOhne Gruppe: ' + ', '.join(f'{k} ({v})' for k, v in d['unknown'].most_common()))


# ── Modell: dieselbe Op-Folge gegen die Geometrie einer Skizze ─────────
# Bedienelement (desc) → Schlüssel data-ctl in der Skizze. None = entfällt
# (Ansicht ist ein Reiter bzw. immer da), 'tab:X' = Reiter-Knopf.
REF_BTNS = {'Compare', 'Tempo', 'Key', 'Structure', 'Voice', 'Drums', 'Match sound', 'Tune', 'Repeats',
            'Layers', 'Classes', 'Cross-check', 'Split', 'To MIDI', 'Stems'}
SKETCHES = ['v1-reiter', 'v2-seitenleiste', 'v3-referenzspur']


def ctl_of(desc, g):
    d = re.sub(r' \(correct\)$', '', desc)
    if g == 'ref.load':
        return 'ref.load'
    if g.startswith('ref.'):
        d = 'To MIDI' if d.startswith('To MIDI') else d
        return 'ref:' + d if d in REF_BTNS else None
    if d in ('routing Simple', 'routing Mixer'):
        return None
    if g == 'route.mixer':
        return 'route.mixer'
    if g == 'route.card':
        if d.startswith(('preset', 'Load preset')):
            return 'route.preset'
        if d.startswith(('parameter slider', 'setting', 'wet slider')):
            return 'param:' + d
        if d == 'plugin name (card)':
            return 'route.chain'
        return 'route.card'
    if g == 'route.chain':
        return 'route.add' if d.startswith('+') else 'route.chain'
    if g == 'recipe':
        return 'recipe.search' if d == 'recipe search' else 'recipe.apply'
    if g == 'notes.roll':
        return 'notes.roll'
    if g == 'pop.analyze' and d in ('Export (for Analyze)', 'Analyze'):
        return 'analysis.analyze' if d == 'Analyze' else None
    return None


def ref_kind(path):
    p = str(path)
    if '/stems/' in p or re.search(r'/(vocals|bass|drums|other)\.wav$', p):
        return 'stem:' + re.sub(r'\.wav$', '', p.rsplit('/', 1)[-1])
    if '/references/' in p or '/.config/unidaw/' in p:
        return 'mix'
    return 'file'


def ref_range(r):
    p = r['params']
    ref = p.get('reference') if isinstance(p.get('reference'), dict) else None
    if ref and (ref.get('start') is not None):
        return (ref.get('start'), ref.get('end'))
    if p.get('file') and p.get('start') is not None and r['op'] not in ('analyze', 'render', 'addClip', 'duplicateClip', 'trimClip', 'moveClip'):
        return (p.get('start'), p.get('end'))
    if p.get('referenceStart') is not None:
        return (p.get('referenceStart'), p.get('referenceEnd'))
    return None


def ref_file(r):
    p = r['params']
    ref = p.get('reference') if isinstance(p.get('reference'), dict) else None
    return (ref or {}).get('file') or p.get('file') or ''


def tl_estimate(results, size):
    """⌀ Rasten einer reinen Timeline-Op je Gruppe (zum Abziehen bei gemischten Ops)."""
    s, n = Counter(), Counter()
    for run in RUNS:
        for r in load(results, run, size):
            gs = {group_of(e['desc']) for e in r['ui']['elems']} - {'other'}
            if len(gs) == 1 and next(iter(gs)).startswith('tl.'):
                g = next(iter(gs)); s[g] += r['ui']['wheel']; n[g] += 1
    return {g: s[g] / n[g] for g in n}


def simulate(results, size, geo, popover=False):
    """geo: Messung einer Skizze in einer Fenstergröße (skizze_mess.js). Gibt Summen
    und je Op die Modell-Rasten zurück."""
    tle = tl_estimate(results, size)
    row0, row12 = geo.get('route.card0'), geo.get('route.card')
    step = (row12['y0'] - row0['y0']) / 11 if row0 and row12 and 'y0' in row0 and 'y0' in row12 else 26
    tot = Counter()
    steps_clean = {'heute': 0, 'skizze': 0, 'n': 0}
    for run in RUNS:
        recs = load(results, run, size)
        active = 'routing'
        scroll = defaultdict(float)
        source = 'mix'
        rng = None
        step_state = {}
        card_open = False
        for r in recs:
            u = r['ui']
            gs = [group_of(e['desc']) for e in u['elems']]
            has_dock = any(region(g) == 'dock' for g in gs)
            if r['op'] in REF_OPS:
                rr = ref_range(r)
                if rr and rr != rng:
                    tot['range_sets'] += 1; rng = rr
                if rr:
                    tot['range_ops'] += 1
            if not gs:
                continue
            # heutiger Dock-Anteil der Rasten
            tl_part = min(u['wheel'], sum(tle.get(g, 0) for g in set(gs) if g.startswith('tl.'))) if has_dock else u['wheel']
            dock_today = u['wheel'] - tl_part if has_dock else 0
            wheel = 0; clicks = 0; switches = 0; saved_views = 0
            for e, g in zip(u['elems'], gs):
                d = e['desc']
                if d == 'mode Process' and has_dock:
                    tot['saved_mode'] += 1; saved_views += 1; continue
                if d in ('routing Simple', 'routing Mixer'):
                    tot['saved_routing_toggle'] += 1; saved_views += 1; continue
                if d == 'Export (for Analyze)':
                    tot['saved_export_popover'] += 1; saved_views += 1; continue
                if region(g) != 'dock' and g != 'pop.analyze':
                    continue
                c = ctl_of(d, g)
                if c is None:
                    continue
                if c == 'ref.load':
                    k = ref_kind(ref_file(r))
                    if k == 'file':
                        tot['file_dialogs_left'] += 1; c = 'ref.file'
                    else:
                        tot['file_dialogs_saved'] += 1
                        if k != source:
                            clicks += 1; tot['source_clicks'] += 1; source = k
                        c = 'ref.source'
                param = None
                if c.startswith('param:'):
                    param = c[6:]; c = 'route.card'
                m = geo.get(c)
                if m is None:
                    tot['missing:' + c] += 1
                    continue
                if m.get('menu'):
                    clicks += 1; tot['menu_clicks'] += 1
                    m = geo.get(m['menu'], {})
                tab = m.get('tab')
                if tab and tab != active:
                    switches += 1; active = tab
                if m.get('obscured'):
                    tot['obscured_model'] += 1
                if 'y0' in m:
                    y0, y1 = m['y0'], m['y1']
                    if param is not None:
                        idx = sum(map(ord, param)) % 40
                        y0 = row0['y0'] + idx * step if row0 and 'y0' in row0 else y0
                        y1 = y0 + (m['y1'] - m['y0'])
                    key = (m.get('scroller'), tab)
                    H = m['ch']; mx = max(0, m['sh'] - H)
                    sc = scroll[key]; cy = (y0 + y1) / 2 - sc
                    if cy < 4 or cy > H - 4:
                        want = (y0 + y1) / 2 - (40 if cy < 4 else H - 40)
                        want = min(mx, max(0, want))
                        nw = math.ceil(abs(want - sc) / 100); wheel += nw; scroll[key] = want
                        tot['wheel_by:' + c] += nw
                        if param is not None:
                            tot['param_scrolls'] += 1
                elif not m.get('inView', True):
                    tot['not_in_view'] += 1
            tot['ops'] += 1
            tot['wheel_today'] += u['wheel']
            tot['dock_today'] += dock_today
            tot['dock_model'] += wheel
            tot['wheel_model'] += tl_part + wheel
            tot['tab_switches'] += switches
            tot['clicks_added'] += clicks + switches
            if any(g.startswith('tl.') for g in gs) and card_open and popover:
                tot['popover_in_way'] += 1; tot['clicks_added'] += 1
            if any(ctl_of(e['desc'], g) in ('route.card', 'route.preset') or (ctl_of(e['desc'], g) or '').startswith('param:') for e, g in zip(u['elems'], gs)):
                card_open = True
            elif any(region(g) == 'dock' for g in gs) and active != 'routing':
                card_open = False
            fb = r.get('feedback')
            if fb and str(fb.get('where', '')).startswith('toast'):
                tot['toast_only_today'] += 1
            if fb and not fb.get('visible'):
                tot['feedback_off_today'] += 1
                res = geo.get('analysis.result', {})
                vis = res.get('inView') and (res.get('tab') in (None, active))
                if not vis:
                    tot['feedback_off_model'] += 1
            ob = sum(1 for o in u['obscured'] if region(group_of(o)) == 'dock')
            tot['obscured_dock_today'] += ob
            # Arbeitsschritt ohne Aufdecken (Scrollen, Ansichtswechsel, Hover)
            sk = (run, r['step'])
            st = step_state.setdefault(sk, [True, True, True])
            views_model = max(0, u['views'] - saved_views) + switches
            if u['wheel'] or u['views'] or u['hoverOnly']:
                st[0] = False
            if (tl_part + wheel) or views_model or u['hoverOnly']:
                st[1] = False
            # dazu ✎ (Notiz) am aktuellen Node immer sichtbar (Bericht, Vorschlag 9)
            if (tl_part + wheel) or views_model or [x for x in u['hoverOnly'] if 'add note' not in x]:
                st[2] = False
        for st in step_state.values():
            steps_clean['n'] += 1
            steps_clean['heute'] += st[0]
            steps_clean['skizze'] += st[1]
            steps_clean['note'] = steps_clean.get('note', 0) + st[2]
    tot['steps_n'] = steps_clean['n']; tot['steps_clean_today'] = steps_clean['heute']; tot['steps_clean_model'] = steps_clean['skizze']; tot['steps_clean_note'] = steps_clean.get('note', 0)
    return tot


REF_OPS = {'compareVoice', 'compareDrums', 'matchSound', 'tuneSound', 'compareStems', 'compareReference', 'detectTempo',
           'detectKey', 'analyzeStructure', 'findRepeats', 'describeLayers', 'compareClasses', 'describeVoice',
           'describeTimbre', 'describeDrums', 'addMidiFromAudio', 'separateStems', 'audioToMidi', 'compareSeparated'}


def model_report(results, mess):
    w = print
    rows = []
    for size in SIZES:
        for sk in SKETCHES:
            rows.append((size, sk, simulate(results, size, mess[sk][size], popover=sk == 'v1-reiter')))
    w('\n## Eichung: Modell gegen das nachgebaute heutige Dock (v0-heute, gleicher Inhalt wie die Skizzen)\n')
    w('| Größe | Dock-Rasten gemessen (echtes UI) | Modell gegen v0-heute | Rest nach Element |')
    w('|---|---|---|---|')
    for size in SIZES:
        t = simulate(results, size, mess['v0-heute'][size])
        by = ', '.join(f"{k[9:]} {v}" for k, v in sorted(t.items(), key=lambda x: -x[1]) if k.startswith('wheel_by:'))
        w(f"| {size} | {t['dock_today']:.0f} | {t['dock_model']} | {by} |")
    w('\n## Modell: dieselbe Op-Folge gegen die Skizzen (alle drei Läufe)\n')
    w('| Größe | Skizze | Rasten gesamt heute → Skizze | davon Dock heute → Skizze | Reiter-Wechsel | Menü-Klicks | Quellen-Klicks | gesparte Modus-/Routing-/Export-Klicks | Dateidialoge gespart | verdeckte Dock-Ziele heute → Skizze | Ergebnis nicht im Bild heute → Skizze | Schritte ohne Aufdecken heute → Skizze | Bereich setzen (Ops mit Bereich) |')
    w('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for size, sk, t in rows:
        saved = t['saved_mode'] + t['saved_routing_toggle'] + t['saved_export_popover']
        w(f"| {size} | {sk} | {t['wheel_today']} → {t['wheel_model']:.0f} | {t['dock_today']:.0f} → {t['dock_model']} | {t['tab_switches']} | {t['menu_clicks']} | {t['source_clicks']} | {saved} ({t['saved_mode']}/{t['saved_routing_toggle']}/{t['saved_export_popover']}) | {t['file_dialogs_saved']} (bleiben {t['file_dialogs_left']}) | {t['obscured_dock_today']} → {t['obscured_model']} | {t['feedback_off_today']} → {t['feedback_off_model']} | {t['steps_clean_today']} → {t['steps_clean_model']} von {t['steps_n']} | {t['range_sets']} ({t['range_ops']}) |")
    w('\n| Größe | Skizze | Ergebnis nur als Toast heute | Karte (Popover) über der Timeline | Schritte ohne Aufdecken, dazu ✎ immer sichtbar | Parameter außerhalb der Karte (Suche statt Scrollen) | Rest-Rasten im Dock nach Element |')
    w('|---|---|---|---|---|---|---|')
    for size, sk, t in rows:
        by = ', '.join(f"{k[9:]} {v}" for k, v in sorted(t.items(), key=lambda x: -x[1]) if k.startswith('wheel_by:'))
        w(f"| {size} | {sk} | {t['toast_only_today']} | {t['popover_in_way']} | {t['steps_clean_note']} von {t['steps_n']} | {t['param_scrolls']} | {by} |")
    miss = {k: v for _, _, t in rows for k, v in t.items() if k.startswith('missing:') or k == 'not_in_view'}
    if miss:
        w('\nNicht gefunden/nicht im Bild: ' + ', '.join(f'{k} {v}' for k, v in miss.items()))
    return rows


def step_kind(label):
    """Art des Arbeitsschritts aus dem Node-Label der Agenten."""
    if re.search(r'^(Start|#\d+ )', label) or '(#1)' in label:
        return 'Referenz-Analyse (Start)'
    if 'Noten' in label or ': Notes' in label:
        return 'Noten'
    if ': Mix' in label:
        return 'Spur-Mix'
    if 'Arrangement' in label:
        return 'Arrangement'
    if label.startswith(('Song: Klang', 'Song: Sound', 'Endstand')) or 'Mix-Ziel' in label:
        return 'Summe / Song-Klang'
    if label.startswith(('Recipe', 'Rezept')):
        return 'Rezept anwenden'
    return 'Spur-Klang'


def kinds_report(results, size='1920x1080'):
    w = print
    nst, ops, wheel, per = Counter(), Counter(), defaultdict(Counter), defaultdict(Counter)
    for run in RUNS:
        seen = set()
        for r in load(results, run, size):
            k = step_kind(r['stepLabel'])
            sk = (run, r['step'])
            if sk not in seen:
                seen.add(sk); nst[k] += 1
            gs = {group_of(e['desc']) for e in r['ui']['elems']} - {'other'}
            ops[k] += 1
            for g in gs:
                wheel[k][g] += r['ui']['wheel'] / len(gs)
                per[k][(sk, g)] = 1
    w(f'\n## Arbeitsschritt-Arten ({size}): welche Gruppen zusammen gebraucht werden\n')
    w('Gruppen in mindestens 15 % der Schritte dieser Art (ohne Baum und Kopfleiste); in Klammern Rasten.\n')
    w('| Art | Schritte | Ops | Rasten | Gruppen (Anteil der Schritte, Rasten) |')
    w('|---|---|---|---|---|')
    for k, n in nst.most_common():
        cnt = Counter(g for (_, g) in per[k])
        gl = [f'{GROUP_NAMES.get(g, g)} {100 * c / n:.0f} % ({wheel[k][g]:.0f})' for g, c in cnt.most_common()
              if g not in ('tree.node', 'top.bar') and c / n >= 0.15]
        w(f'| {k} | {n} | {ops[k]} | {sum(wheel[k].values()):.0f} | {"; ".join(gl)} |')


def range_report(results, size='1920x1080'):
    w = print
    w(f'\n## Referenz-Bereiche je Lauf ({size}): wie oft derselbe Bereich\n')
    w('| Lauf | Referenz-Ops | ohne Bereich (ganze Datei) | häufigste Bereiche (s) |')
    w('|---|---|---|---|')
    for run in RUNS:
        c = Counter(); n = 0
        for r in load(results, run, size):
            if r['op'] not in REF_OPS:
                continue
            n += 1
            c[ref_range(r)] += 1
        top = ', '.join(f'{a}–{b} ×{v}' for (a, b), v in [(k, v) for k, v in c.most_common() if k][:3])
        w(f'| {run} | {n} | {c[None]} | {top} |')


if __name__ == '__main__':
    args = sys.argv[1:]
    res = args[0]
    a = analyse(res)
    report(a)
    kinds_report(res)
    range_report(res)
    if '--model' in args:
        mess = json.load(open(args[args.index('--model') + 1]))
        model_report(res, mess)
