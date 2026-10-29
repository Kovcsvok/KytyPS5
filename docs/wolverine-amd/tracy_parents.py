"""Self time per zone on one Tracy thread, and where the time of chosen zones goes and comes from.

Usage: tracy-csvexport -u <trace> > all.csv
       python tracy_parents.py all.csv <thread> <frames> [min_ns] [zone ...]
Reads the export file directly (a pipe through Git Bash is ~20x slower). Zones shorter than
min_ns (default 400) are dropped: their time counts as their parent's self time. For each named
zone (substring match) it prints its parents and its direct children, in ms per frame.
"""
import collections
import sys

path, thread, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
min_ns = int(sys.argv[4]) if len(sys.argv) > 4 else 400
wanted = sys.argv[5:]

names = {}
events = []
counts_all = collections.Counter()
with open(path, encoding="utf-8", errors="replace") as f:
    next(f)
    for line in f:
        # name may contain commas; the last six fields are fixed
        p = line.rstrip("\n").rsplit(",", 6)
        if len(p) < 7 or p[-2] != thread:
            continue
        name = p[0]
        counts_all[name] += 1
        dur = int(p[-3])
        if dur < min_ns:
            continue
        nid = names.setdefault(name, len(names))
        ts = int(p[-4])
        events.append((ts, -dur, nid))
id_name = {v: k for k, v in names.items()}
events.sort()

self_ns = collections.Counter()
total_ns = collections.Counter()
count = collections.Counter()
parent_of = collections.defaultdict(collections.Counter)  # zone -> parent -> ns
child_of = collections.defaultdict(collections.Counter)  # zone -> child -> ns
stack = []  # [end, nid, child_ns, start]


def pop_until(ts):
    while stack and stack[-1][0] <= ts:
        end, nid, child, start = stack.pop()
        self_ns[nid] += (end - start) - child
        if stack:
            stack[-1][2] += end - start


for start, neg, nid in events:
    end = start - neg
    pop_until(start)
    total_ns[nid] += end - start
    count[nid] += 1
    if stack:
        parent = stack[-1][1]
        parent_of[nid][parent] += end - start
        child_of[parent][nid] += end - start
    else:
        parent_of[nid][-1] += end - start
    stack.append([end, nid, 0, start])
pop_until(1 << 62)

ms = lambda ns: ns / 1e6 / frames
print(f"thread {thread}, {frames} frames, zones >= {min_ns} ns; ms per frame")
print(f"{'zone':60s} {'self':>6s} {'total':>6s} {'n/frame':>8s} {'us/ev':>7s}")
for nid, s in self_ns.most_common(45):
    name = id_name[nid]
    print(f"{name[-60:]:60s} {ms(s):6.2f} {ms(total_ns[nid]):6.2f} "
          f"{counts_all[name] / frames:8.1f} {total_ns[nid] / count[nid] / 1e3:7.1f}")
for want in wanted:
    for nid, name in id_name.items():
        if want not in name:
            continue
        print(f"\n== {name}: total {ms(total_ns[nid]):.2f}, self {ms(self_ns[nid]):.2f}")
        print("  parents:")
        for p, ns in parent_of[nid].most_common(8):
            print(f"    {ms(ns):6.2f}  {id_name.get(p, '(top)')[-70:]}")
        print("  children:")
        for c, ns in child_of[nid].most_common(10):
            print(f"    {ms(ns):6.2f}  {id_name[c][-70:]}")
