#!/usr/bin/env python3
"""prof-symbols.py ROOTFS.zip CONSOLE.log [N]: the prof=1 sampler's cumulative top blocks with function names.

Joins the last report's [prof-cum] lines (process, module, 16-byte offset, ms, instructions, entries), finds each
module in the root fs zip (any entry with that file name: the Wine .dll.so files, libc, ...) and names an offset
after the dynamic symbol that contains it (nm -D -S). The root fs Wine is stripped, so only exported functions get
a name; static functions show as "?". Windows exes and PE dlls from the game folder have no symbols here.
Prints N rows (default 40) and a per-symbol total.
"""
import json
import re
import subprocess
import sys
import tempfile
import zipfile
from bisect import bisect_right
from collections import defaultdict


def last_cum(path):
    """Items of the last [prof-cum] report (a report's lines come together, after its [prof] lines)."""
    items, cur = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"\[prof-cum\] (.*)$", line)
        if m:
            cur.append(m.group(1).strip())
        elif cur and "[prof]" in line:
            items, cur = cur, []
    lines = cur or items
    return json.loads("[" + ",".join(lines) + "]") if lines else []


def symbols(zf, names, tmp):
    """{module: sorted [(start, end, name)]} from nm -D -S of each module found in the zip."""
    by_name = defaultdict(list)
    for e in zf.namelist():
        by_name[e.rsplit("/", 1)[-1]].append(e)
    out = {}
    for mod in names:
        entries = by_name.get(mod)
        if not entries:
            continue
        f = zf.extract(entries[0], tmp)
        r = subprocess.run(["nm", "-D", "-S", "--defined-only", f], capture_output=True, text=True)
        syms = []
        for line in r.stdout.splitlines():
            p = line.split()
            if len(p) == 4 and p[2] in "TtWw":
                start = int(p[0], 16)
                syms.append((start, start + int(p[1], 16), p[3].split("@")[0]))
        out[mod] = sorted(syms)
    return out


def name(syms, off):
    if not syms:
        return "?"
    i = bisect_right(syms, (off, 1 << 62)) - 1
    # 16-byte buckets: a block in the bucket may start just past the symbol's start
    for s, e, n in (syms[i:i + 2] if i >= 0 else syms[:1]):
        if s <= off + 15 and off < e:
            return "%s+0x%x" % (n, max(0, off - s))
    return "?"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rows = last_cum(sys.argv[2])
    if not rows:
        sys.exit("no [prof-cum] lines (run with prof=1 for at least 5 s)")
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    total = sum(r[3] for r in rows)
    with zipfile.ZipFile(sys.argv[1]) as zf, tempfile.TemporaryDirectory() as tmp:
        syms = symbols(zf, {r[1] for r in rows}, tmp)
    per = defaultdict(float)
    print("%6s %8s %8s  %s" % ("share", "ms", "instr/e", "process:module+offset  symbol"))
    for i, (proc, mod, off, ms, instr, entries) in enumerate(rows):
        sym = name(syms.get(mod), off)
        per["%s:%s %s" % (proc, mod, sym.split("+")[0])] += ms
        if i < n:
            print("%5.1f%% %8.0f %8.1f  %s:%s+0x%x  %s" % (100 * ms / total, ms, instr / max(entries, 1), proc, mod, off, sym))
    print("\nby symbol (all %d rows, %.0f ms):" % (len(rows), total))
    for k, v in sorted(per.items(), key=lambda kv: -kv[1])[:n]:
        print("%5.1f%% %8.0f  %s" % (100 * v / total, v, k))


if __name__ == "__main__":
    main()
