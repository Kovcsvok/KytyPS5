"""Self time per zone on one Tracy thread within a time window.

Usage: tracy-csvexport -u <trace> | python tracy_self_thread.py <thread> <t0_s> <t1_s> <frames>
Zones whose events are all shorter than MIN_NS are dropped (their time counts as the parent's).
"""
import collections
import sys

thread = sys.argv[1]
t0 = int(float(sys.argv[2]) * 1e9)
t1 = int(float(sys.argv[3]) * 1e9)
frames = int(sys.argv[4])
MIN_NS = 1500

events = []
counts_all = collections.Counter()
next(sys.stdin)
for line in sys.stdin:
    f = line.rstrip("\n").split(",")
    if len(f) < 7 or f[-2] != thread:
        continue
    ts = int(f[-4])
    if ts < t0 or ts >= t1:
        continue
    dur = int(f[-3])
    name = f[0]
    counts_all[name] += 1
    if dur >= MIN_NS:
        events.append((ts, ts + dur, name))

events.sort(key=lambda e: (e[0], -e[1]))
self_ns = collections.Counter()
total_ns = collections.Counter()
count = collections.Counter()
stack = []  # [end, name, child_ns]


def pop_until(ts):
    while stack and stack[-1][0] <= ts:
        end, name, child, start = stack.pop()
        self_ns[name] += (end - start) - child
        if stack:
            stack[-1][2] += end - start


for start, end, name in events:
    pop_until(start)
    total_ns[name] += end - start
    count[name] += 1
    stack.append([end, name, 0, start])
pop_until(1 << 62)

window_s = (t1 - t0) / 1e9
print(f"thread {thread}, {window_s:.1f} s, {frames} frames ({frames / window_s:.1f} fps); ms per frame")
print(f"{'zone':58s} {'self':>6s} {'total':>6s} {'n/frame':>8s} {'us/ev':>7s}")
for name, s in self_ns.most_common(40):
    n = count[name]
    print(f"{name[-58:]:58s} {s / 1e6 / frames:6.2f} {total_ns[name] / 1e6 / frames:6.2f} "
          f"{counts_all[name] / frames:8.1f} {total_ns[name] / n / 1e3:7.1f}")
