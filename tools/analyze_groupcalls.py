#!/usr/bin/env python3
"""
PDW groepsoproep diagnose — leest no-si-groupcalls.txt (betrouwbaar) en dag.log.

no-si-groupcalls.txt wordt door PDW geschreven elke keer dat een groepsbericht
binnenkomt zonder overeenkomende Short Instruction (Y++ event in F12).
Dit is de enige betrouwbare bron: dag.log mist Y++ events die door de
FLEXGROUPMODE_COMBINE-filter zijn onderdrukt.

Gebruik:
    python analyze_groupcalls.py <PDW-map>
    python analyze_groupcalls.py "C:\\PDW"          # map met dag.log + no-si-groupcalls.txt
    python analyze_groupcalls.py .                  # huidige map
"""

import re
import sys
import os
from collections import defaultdict

GROUP_CAPCODE_MIN = 2029568
GROUP_CAPCODE_MAX = 2029583

# no-si-groupcalls.txt regelformaat:
#  08-05-26 12:34:56  capcode=2029574  GroupFrame=-1  iCurrentFrame=123  bericht
NOSI_RE = re.compile(
    r'^\s*(\d{2}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2})'
    r'\s+capcode=(\d+)'
    r'\s+GroupFrame=(-?\d+)'
    r'\s+iCurrentFrame=(\d+)'
    r'\s*(.*?)\s*$'
)

# dag.log header-regel: "00:00:08 08-05-26  GROUP-6  P 2 BMD-04 ..."
HEADER_RE = re.compile(
    r'^(\d{2}:\d{2}:\d{2})\s+(\d{2}-\d{2}-\d{2})\s+(\S+)\s*(.*?)\s*$'
)
CAPCODE_RE = re.compile(r'^\s{5,}(\d{5,9})\s*(.*?)\s*$')


def is_group_cap(cap):
    return GROUP_CAPCODE_MIN <= int(cap) <= GROUP_CAPCODE_MAX


def groupnum(cap):
    return int(cap) - GROUP_CAPCODE_MIN + 1


def grouplabel(n):
    return f'GROUP{"-" if n < 10 else ""}{n}'


def type_to_groupnum(msg_type):
    t = re.sub(r'(?i)group', '', msg_type).replace('-', '').strip()
    try:
        return int(t)
    except ValueError:
        return None


def parse_nosi(filename):
    events = []
    if not os.path.exists(filename):
        return events
    with open(filename, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            m = NOSI_RE.match(line)
            if m:
                cap = int(m.group(3))
                if is_group_cap(cap):
                    events.append({
                        'date':          m.group(1),
                        'time':          m.group(2),
                        'capcode':       cap,
                        'group_frame':   int(m.group(4)),
                        'current_frame': int(m.group(5)),
                        'text':          m.group(6),
                    })
    return events


def parse_daglog(filename):
    """Leest dag.log; retourneert alleen succesvolle GROUP-x berichten."""
    group_ok = []
    if not os.path.exists(filename):
        return group_ok
    current = None
    with open(filename, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line.strip():
                continue
            m = HEADER_RE.match(line)
            if m and not m.group(3).isdigit():
                if current and current['type'].upper().startswith('GROUP'):
                    group_ok.append(current)
                current = {
                    'time': m.group(1),
                    'date': m.group(2),
                    'type': m.group(3).strip(),
                    'text': m.group(4).strip(),
                }
            # capcode-regels interesseren ons niet voor GROUP-ok telling
    if current and current['type'].upper().startswith('GROUP'):
        group_ok.append(current)
    return group_ok


def analyze_files(nosi_file, dag_file, label):
    hr = '─' * 72

    nosi_events = parse_nosi(nosi_file)
    group_ok    = parse_daglog(dag_file) if dag_file else []

    # Per groepsnummer
    ok_by_n    = defaultdict(list)
    nosi_by_n  = defaultdict(list)

    for m in group_ok:
        n = type_to_groupnum(m['type'])
        if n is not None:
            ok_by_n[n].append(m)

    for e in nosi_events:
        nosi_by_n[groupnum(e['capcode'])].append(e)

    # ── Bestanden ─────────────────────────────────────────────────────────────
    print(hr)
    print('  PDW Groepsoproep Analyse')
    print(hr)
    print(f'  Bronmap                       : {label}')
    nosi_status = 'aanwezig' if os.path.exists(nosi_file) else 'NIET aanwezig (PDW-versie zonder Y++ logging?)'
    dag_status  = 'aanwezig' if dag_file and os.path.exists(dag_file) else ('niet opgegeven' if not dag_file else 'NIET aanwezig')
    print(f'  no-si-groupcalls.txt          : {nosi_status}')
    print(f'  dag.log                       : {dag_status}')
    print(hr)
    print(f'  Succesvolle GROUP-berichten   : {len(group_ok)}'
          '  (uit dag.log)')
    print(f'  Y++ events zonder SI          : {len(nosi_events)}'
          '  (uit no-si-groupcalls.txt — gegarandeerd volledig)')
    print(hr)

    # ── Y++ detail ────────────────────────────────────────────────────────────
    if nosi_events:
        print('  Y++ EVENTS (groepsbericht ontvangen zonder SI-match):')
        print(f'  {"Datum":<10} {"Tijd":<10} {"Capcode":<10} {"GrpFrame":>9} {"CurFrame":>9}  {"Tekst"}')
        print(f'  {"─"*10} {"─"*10} {"─"*10} {"─"*9} {"─"*9}  {"─"*40}')
        for e in nosi_events:
            gf  = str(e['group_frame']) if e['group_frame'] != -1 else '-1 (geen SI)'
            print(f'  {e["date"]:<10} {e["time"]:<10} {e["capcode"]:<10} {gf:>9} {e["current_frame"]:>9}  {e["text"][:50]}')
        print()

        # Diagnose GroupFrame
        no_si   = [e for e in nosi_events if e['group_frame'] == -1]
        wrong_f = [e for e in nosi_events if e['group_frame'] != -1]
        if no_si:
            print(f'  → {len(no_si)}× GroupFrame=-1: geen SI ontvangen vóór het groepsbericht.')
            print('    Oorzaak: netwerk stuurt direct broadcast, of SI-frame gemist.')
        if wrong_f:
            print(f'  → {len(wrong_f)}× GroupFrame≠iCurrentFrame: SI wél ontvangen, maar verkeerd frame.')
            print('    Oorzaak: timing/frame-fout of meerdere SI-frames voor zelfde groep.')
        print()
    else:
        if os.path.exists(nosi_file):
            print('  Geen Y++ events in no-si-groupcalls.txt — groepsoproepen verlopen correct.')
        else:
            print('  no-si-groupcalls.txt ontbreekt — draai een nieuwere PDW-versie met Y++ logging.')
        print()

    # ── Per groepsadres ────────────────────────────────────────────────────────
    all_n = sorted(set(list(ok_by_n) + list(nosi_by_n)))
    if all_n:
        print('  PER GROEPSADRES:')
        print(f'  {"Adres":<10} {"Capcode":<10} {"Succesvol":>10} {"Y++":>6}  Oordeel')
        print(f'  {"─"*10} {"─"*10} {"─"*10} {"─"*6}  {"─"*38}')
        for n in all_n:
            cap = GROUP_CAPCODE_MIN + n - 1
            s   = len(ok_by_n.get(n, []))
            y   = len(nosi_by_n.get(n, []))
            lbl = grouplabel(n)
            if   s > 0 and y == 0: oordeel = 'Altijd succesvol'
            elif s == 0 and y > 0: oordeel = '*** ALTIJD Y++ — netwerk stuurt zonder SI?'
            else:                  oordeel = f'Gemengd ({y}/{s+y} Y++) — soms SI gemist?'
            print(f'  {lbl:<10} {cap:<10} {s:>10} {y:>6}  {oordeel}')
        print()

    # ── Tijdlijn ──────────────────────────────────────────────────────────────
    print('  TIJDLIJN (succesvol en Y++ door elkaar):')
    timeline = []
    for m in group_ok:
        n = type_to_groupnum(m['type'])
        timeline.append((m['date'], m['time'], 'OK   ', m['type'], m['text'][:50]))
    for e in nosi_events:
        lbl = grouplabel(groupnum(e['capcode']))
        timeline.append((e['date'], e['time'], 'Y++  ', lbl, e['text'][:50]))
    timeline.sort()
    MAX_TL = 80
    for date, time, status, typ, text in timeline[:MAX_TL]:
        marker = '>>> ' if 'Y++' in status else '    '
        print(f'  {marker}{time} {date}  {status}  {typ:<12}  {text}')
    if len(timeline) > MAX_TL:
        print(f'  ... ({len(timeline) - MAX_TL} regels weggelaten)')
    print()

    # ── Conclusie ─────────────────────────────────────────────────────────────
    print(hr)
    print('  CONCLUSIE:')
    if not nosi_events:
        if os.path.exists(nosi_file):
            print('  Geen Y++ situaties gevonden. Groepsoproepen verlopen correct.')
        else:
            print('  Kan niet concluderen: no-si-groupcalls.txt ontbreekt.')
            print('  Update PDW zodat Y++ events worden gelogd.')
    else:
        total = len(group_ok) + len(nosi_events)
        pct   = 100 * len(nosi_events) / max(1, total)
        print(f'  {len(nosi_events)} Y++ event(s) ({pct:.0f}% van alle groepsberichten).')

        always_y = [n for n in all_n if not ok_by_n.get(n) and nosi_by_n.get(n)]
        mixed    = [n for n in all_n if ok_by_n.get(n) and nosi_by_n.get(n)]

        no_si_count   = sum(1 for e in nosi_events if e['group_frame'] == -1)
        wrong_f_count = len(nosi_events) - no_si_count

        if no_si_count:
            print(f'\n  {no_si_count}× geen SI ontvangen (GroupFrame=-1):')
            if always_y:
                names = ', '.join(grouplabel(n) for n in always_y)
                print(f'  Altijd Y++: {names}')
                print('  → Het netwerk stuurt deze groepen ZONDER SHORT INSTRUCTION.')
                print('    PDW-gedrag is correct; Y++ is verwacht en onvermijdbaar.')
            if mixed:
                names = ', '.join(grouplabel(n) for n in mixed)
                print(f'  Gemengd: {names}')
                print('  → SI soms ontvangen, soms niet.')
                print('    Controleer signaalonderbrekingen rond SI-frames (~1,875 s).')

        if wrong_f_count:
            print(f'\n  {wrong_f_count}× SI ontvangen maar verkeerd framenummer:')
            print('  → SI-frame en groepsbericht-frame komen niet overeen.')
            print('    Mogelijke oorzaak: timing-discrepantie of dubbele SI.')

    print(hr)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Gebruik:')
        print('  python analyze_groupcalls.py <PDW-map>')
        print('  python analyze_groupcalls.py no-si-groupcalls.txt [dag.log]')
        sys.exit(1)

    arg1 = sys.argv[1]

    if os.path.isdir(arg1):
        nosi_file = os.path.join(arg1, 'no-si-groupcalls.txt')
        dag_file  = os.path.join(arg1, 'dag.log')
        label     = arg1
    elif os.path.isfile(arg1):
        nosi_file = arg1
        dag_file  = sys.argv[2] if len(sys.argv) >= 3 else None
        label     = os.path.dirname(os.path.abspath(arg1)) or '.'
    else:
        print(f'Niet gevonden: {arg1}')
        sys.exit(1)

    try:
        analyze_files(nosi_file, dag_file, label)
    except Exception as e:
        print(f'Fout: {e}')
        raise
