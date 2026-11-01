#!/usr/bin/env python3
"""compare.py <guest console.log> <host console.log>: per-function results of one conformance exe (vnbox_guard.h
markers) on the guest wined3d build vs the host wined3d build. Prints the totals of each side and every function
that is worse on the host (crash, hang, not reached, more failures, failure lines the guest doesn't have).
compare.py <console.log>: the totals and per-function table of one run."""
import re, sys
from collections import defaultdict

ESC = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
STAMP = re.compile(r'^\[[0-9.]+\] ')
# the tail can be cut by another console line written into the middle of it (Chromium): missing counts are 0
END = re.compile(r'^### END (\d+) (\S+) ok=(-?\d+) fail=(-?\d+) todofail=(-?\d+) todo=(-?\d+)(?: skip=(-?\d+))?')
FAILED = re.compile(r'^(\w+\.c):(\d+): (Test failed|Test succeeded inside todo block)')


def parse(path):
    funcs, order, cur = {}, [], None
    exited = False
    for raw in open(path, encoding='utf-8', errors='replace'):
        line = ESC.sub('', STAMP.sub('', raw.rstrip('\n'))).strip('\r')
        m = re.match(r'^### BEGIN (\d+) (\S+)', line)
        if m:
            cur = (int(m[1]), m[2])
            funcs[cur] = {'state': 'running', 'lines': set(), 'ok': 0, 'fail': 0, 'todofail': 0, 'todo': 0, 'skip': 0}
            order.append(cur)
            continue
        m = END.match(line)
        if m:
            k = (int(m[1]), m[2])
            f = funcs.setdefault(k, {'lines': set()})
            if f.get('state') != 'crash':
                f['state'] = 'done'
            for i, n in enumerate(('ok', 'fail', 'todofail', 'todo', 'skip')):
                f[n] = int(m[3 + i] or 0)
            cur = None
            continue
        m = re.match(r'^### (CRASH|HANG) (\d+) (\S+) ?(.*)', line)
        if m:
            f = funcs.setdefault((int(m[2]), m[3]), {'lines': set()})
            f['state'] = m[1].lower()
            f['why'] = m[4]
            continue
        if line.startswith('### EXIT'):
            exited = True
        m = FAILED.match(line)
        if m and cur:
            funcs[cur]['lines'].add(f'{m[1]}:{m[2]}' + (' (todo ok)' if m[3] != 'Test failed' else ''))
    return funcs, order, exited


def totals(funcs, exited):
    t = defaultdict(int)
    for f in funcs.values():
        t['functions'] += 1
        t[f.get('state', '?')] += 1
        for n in ('ok', 'fail', 'todofail', 'todo', 'skip'):
            t[n] += f.get(n, 0)
        if f.get('state') == 'done' and not f.get('fail') and not f.get('todofail'):
            t['clean'] += 1
    t['exited'] = int(exited)
    return dict(t)


def fmt(t):
    return ('functions={functions} clean={clean} crash={crash} hang={hang} running(at end)={running} | checks: '
            'passed={ok} failed={fail} todo-passed={todofail} todo={todo} skipped={skip} | exit={exited}').format(
        **{k: t.get(k, 0) for k in ("functions", "clean", "crash", "hang", "running", "ok", "fail", "todofail", "todo", "skip", "exited")})


if len(sys.argv) == 2:
    funcs, order, ex = parse(sys.argv[1])
    print(fmt(totals(funcs, ex)))
    for k in order:
        f = funcs[k]
        print(f"{k[0]:4} {k[1]:60} {f.get('state'):7} ok={f.get('ok', 0)} fail={f.get('fail', 0)} "
              f"todofail={f.get('todofail', 0)} skip={f.get('skip', 0)} {f.get('why', '')}")
    sys.exit()

g, gorder, gex = parse(sys.argv[1])
h, horder, hex_ = parse(sys.argv[2])
print('guest:', fmt(totals(g, gex)))
print('host: ', fmt(totals(h, hex_)))
worse = []
for k in gorder:
    gf, hf = g[k], h.get(k)
    if hf is None:
        worse.append((k, 'not reached on host', ''))
        continue
    if hf.get('state') != 'done' and gf.get('state') == 'done':
        worse.append((k, hf.get('state'), hf.get('why', '')))
        continue
    new = sorted(hf['lines'] - gf['lines'])
    if hf.get('fail', 0) > gf.get('fail', 0) or new:
        worse.append((k, f"fail {gf.get('fail', 0)} -> {hf.get('fail', 0)}, ok {gf.get('ok', 0)} -> {hf.get('ok', 0)}",
                      ' '.join(new[:12]) + (' ...' if len(new) > 12 else '')))
better = [k for k in gorder if k in h and (h[k].get('fail', 0) < g[k].get('fail', 0)
                                           or (g[k].get('state') != 'done' and h[k].get('state') == 'done'))]
print(f'\nworse on host: {len(worse)} functions')
for k, what, extra in worse:
    print(f'  {k[0]:4} {k[1]}: {what} {extra}')
print(f'better on host: {len(better)} functions: ' + ' '.join(k[1] for k in better))
