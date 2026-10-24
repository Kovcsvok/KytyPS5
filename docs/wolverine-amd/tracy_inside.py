"""Zones inside the long events of given parent zones on one thread, per A/B window.

Usage: python inside.py <thread csv> <parents csv> <min_parent_us> <A t0 t1 frames> <B t0 t1 frames>
"""
import bisect
import collections
import sys

csv_all, csv_parents = sys.argv[1], sys.argv[2]
min_ns = int(float(sys.argv[3]) * 1000)
wins = {"A": tuple(map(float, sys.argv[4:7])), "B": tuple(map(float, sys.argv[7:10]))}


def window(ts):
    for k, (t0, t1, _) in wins.items():
        if t0 * 1e9 <= ts < t1 * 1e9:
            return k
    return None


parents = []  # (start, end, name, win)
with open(csv_parents, encoding="utf-8", errors="replace") as f:
    for line in f:
        p = line.rstrip("\n").split(",")
        if len(p) < 7 or not p[-4].isdigit():
            continue
        ts, dur = int(p[-4]), int(p[-3])
        w = window(ts)
        if w and dur >= min_ns:
            parents.append((ts, ts + dur, p[0], w))
parents.sort()
starts = [p[0] for p in parents]
pname = {p[2] for p in parents}
print(f"{len(parents)} long parent events")

inside = collections.defaultdict(lambda: [0, 0])  # (win, parent, child) -> [ns, n]
ptot = collections.defaultdict(lambda: [0, 0])
for s, e, n, w in parents:
    ptot[(w, n)][0] += e - s
    ptot[(w, n)][1] += 1
with open(csv_all, encoding="utf-8", errors="replace") as f:
    next(f)
    for line in f:
        p = line.rstrip("\n").split(",")
        if len(p) < 7:
            continue
        name = p[0]
        if name in pname:
            continue
        ts = int(p[-4])
        i = bisect.bisect_right(starts, ts) - 1
        if i < 0:
            continue
        s, e, pn, w = parents[i]
        if ts < e:
            acc = inside[(w, pn, name)]
            acc[0] += int(p[-3])
            acc[1] += 1

for (w, pn), (ns, n) in sorted(ptot.items()):
    frames = wins[w][2]
    print(f"\n[{w}] {pn}: {n / frames:.1f} long events/frame, {ns / 1e6 / frames:.2f} ms/frame, {ns / n / 1e3:.0f} us each")
    rows = sorted(((v[0], k[2], v[1]) for k, v in inside.items() if k[0] == w and k[1] == pn), reverse=True)
    for cns, cname, cn in rows[:12]:
        print(f"   {cname[-60:]:60s} {cns / 1e6 / frames:6.2f} ms/fr {cn / frames:8.1f}/fr {cns / cn / 1e3:7.1f} us")
