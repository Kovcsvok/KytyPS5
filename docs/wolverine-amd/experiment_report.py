"""Compares the setups of an experiment.py run from the emulator's telemetry.

  python experiment_report.py [logs dir or experiment_*.json]
Default: the newest experiment_*.json in C:/Games/Wolverine-AMD-Async/userdata/logs. Uses the
telemetry_*.csv / frames_*.csv rows inside the schedule, dropping the first settle_s of every
block and rows whose tag is not the block's. The paired column compares each block with the
nearest blocks of the first setup before and after it, which cancels most of the scene's drift.
"""
import csv
import glob
import json
import math
import os
import statistics
import sys

DEFAULT_LOGS = r"C:\Games\Wolverine-AMD-Async\userdata\logs"


def pick_schedule(arg):
    if arg and arg.endswith(".json"):
        return arg
    found = sorted(glob.glob(os.path.join(arg or DEFAULT_LOGS, "experiment_*.json")), key=os.path.getmtime)
    if not found:
        raise SystemExit("no experiment_*.json")
    return found[-1]


def load_rows(logs, prefix, t0, t1):
    rows = []
    for path in glob.glob(os.path.join(logs, f"{prefix}_*.csv")):
        with open(path, encoding="utf-8", errors="replace") as f:
            for r in csv.DictReader(f):
                try:
                    t = int(r["unix_ms"])
                except (KeyError, ValueError):
                    continue
                if t0 <= t <= t1:
                    rows.append(r)
    rows.sort(key=lambda r: int(r["unix_ms"]))
    return rows


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(math.ceil(p / 100 * len(s))) - 1)]


def main():
    schedule_path = pick_schedule(sys.argv[1] if len(sys.argv) > 1 else None)
    with open(schedule_path, encoding="utf-8") as f:
        sched = json.load(f)
    logs = os.path.dirname(schedule_path)
    blocks = sched["blocks"]
    settle = sched["settle_s"] * 1000
    names = {int(k): v["name"] for k, v in sched["setups"].items()}
    t0, t1 = blocks[0][0], blocks[-1][1] + 2000
    frames = load_rows(logs, "frames", t0, t1)
    rows = load_rows(logs, "telemetry", t0, t1)
    if not frames:
        raise SystemExit("no frame rows in the schedule (was the build's telemetry on?)")

    per_block = []  # (tag, fps, frame_ms list, telemetry rows)
    for start, end, tag in blocks:
        fr = [float(r["frame_ms"]) for r in frames
              if start + settle <= int(r["unix_ms"]) < end and int(r["tag"]) == tag]
        tr = [r for r in rows if int(r["tag"]) == tag and
              int(r["unix_ms"]) - float(r["interval_ms"]) >= start + settle and int(r["unix_ms"]) <= end + 300]
        fps = len(fr) / (sum(fr) / 1000) if fr and sum(fr) > 0 else float("nan")
        per_block.append((tag, fps, fr, tr))

    out = [f"{os.path.basename(schedule_path)}: {len(blocks)} blocks of {sched['dwell_s']:.0f} s "
           f"(first {sched['settle_s']:.0f} s dropped)"]
    out.append("\nBlocks (fps): " + "  ".join(f"{names[t]}={f:.1f}" for t, f, _, _ in per_block))

    base = int(min(names))
    out.append(f"\n{'setup':10s} {'blocks':>6s} {'frames':>6s} {'fps':>6s} {'p50 ms':>7s} {'p99 ms':>7s} "
               f"{'1% low':>7s} {'max ms':>7s}  paired vs {names[base]}")
    for tag in sorted(names):
        bl = [b for b in per_block if b[0] == tag and b[2]]
        fr = [x for b in bl for x in b[2]]
        if not fr:
            out.append(f"{names[tag]:10s} no frames")
            continue
        paired = ""
        if tag != base:
            ratios = []
            for i, b in enumerate(per_block):
                if b[0] != tag or not b[2]:
                    continue
                prev = next((p for p in reversed(per_block[:i]) if p[0] == base and p[2]), None)
                nxt = next((p for p in per_block[i + 1:] if p[0] == base and p[2]), None)
                ref = [p[1] for p in (prev, nxt) if p is not None]
                if ref:
                    ratios.append(b[1] / statistics.mean(ref))
            if ratios:
                mean = statistics.mean(ratios)
                sd = statistics.stdev(ratios) if len(ratios) > 1 else 0.0
                paired = f"{(mean - 1) * 100:+.1f}% (+/-{sd / math.sqrt(len(ratios)) * 100:.1f}, n={len(ratios)})"
        fps = len(fr) / (sum(fr) / 1000)
        p99 = pct(fr, 99)
        out.append(f"{names[tag]:10s} {len(bl):6d} {len(fr):6d} {fps:6.1f} {pct(fr, 50):7.1f} {p99:7.1f} "
                   f"{1000 / p99:7.1f} {max(fr):7.0f}  {paired}")

    if rows:
        keys = [k for k in rows[0].keys() if k.endswith("_ms") and k not in ("unix_ms", "interval_ms", "frame_max_ms", "process_cpu_ms")]
        counts = [k for k in ("guest_faults", "readback_waits", "buffer_locks", "memory_locks", "ranges_locks", "spin_locks", "mutexes",
                              "gpu_locks", "ranges_changes", "ranges_edits", "clamp_locked", "clamp_ring_saved",
                              "cmdbufs") if k in rows[0]]
        out.append("\nPer frame (ms or count), GPU thread busy/idle share, emulator CPU cores, GPU thread GHz running:")
        header = f"{'':22s}" + "".join(f"{names[t]:>10s}" for t in sorted(names))
        out.append(header)
        table = {}
        for tag in sorted(names):
            tr = [r for b in per_block if b[0] == tag for r in b[3]]
            nf = sum(int(r["frames"]) for r in tr) or 1
            iv = sum(float(r["interval_ms"]) for r in tr) or 1
            col = {k: sum(float(r[k]) for r in tr) / nf for k in keys + counts}
            col["gpu busy %"] = sum(float(r["gpu_process_ms"]) for r in tr) / iv * 100
            col["gpu idle %"] = sum(float(r["gpu_idle_ms"]) for r in tr) / iv * 100
            col["cpu cores"] = sum(float(r["process_cpu_ms"]) for r in tr) / iv
            col["gpu thread GHz"] = sum(float(r["gpu_thread_mcycles"]) for r in tr) / max(
                sum(float(r["gpu_process_ms"]) for r in tr), 1)
            derived = []
            if "gpu_thread_run_ms" in rows[0]:
                s_ = lambda k: sum(float(r[k]) for r in tr) / nf
                # Time the GPU thread is neither running nor idle-waiting: blocked inside its work.
                col["gpu off-cpu ms"] = iv / nf - s_("gpu_thread_run_ms") - s_("gpu_idle_ms") - s_("gpu_blocked_ms")
                col["gpu known waits ms"] = (s_("gpu_lock_ms") + s_("gpu_fence_ms") + s_("gpu_operation_ms") +
                                             s_("gpu_record_ms") +
                                             max(0.0, s_("gpu_cond_ms") - s_("gpu_idle_ms") - s_("gpu_blocked_ms")))
                col["gpu unexplained ms"] = col["gpu off-cpu ms"] - col["gpu known waits ms"]
                cores = sum(int(r["gpu_on_pcore"]) + int(r["gpu_on_ecore"]) for r in tr) or 1
                col["gpu on e-core %"] = sum(int(r["gpu_on_ecore"]) for r in tr) / cores * 100
                derived = ["gpu off-cpu ms", "gpu known waits ms", "gpu unexplained ms", "gpu on e-core %"]
            if "ranges_edits" in rows[0]:
                tot = lambda k: sum(float(r[k]) for r in tr)
                col["ranges hold/change ms"] = tot("ranges_hold_ms") / max(tot("ranges_changes"), 1)
                col["ranges edit/each ms"] = tot("ranges_edit_ms") / max(tot("ranges_edits"), 1)
                col["ranges table size"] = tot("ranges_size") / max(tot("ranges_changes"), 1)
                derived += ["ranges hold/change ms", "ranges edit/each ms", "ranges table size"]
            table[tag] = col
        for k in ["gpu busy %", "gpu idle %", "cpu cores", "gpu thread GHz"] + derived + keys + counts:
            out.append(f"{k:22s}" + "".join(f"{table[t][k]:10.2f}" for t in sorted(names)))
    text = "\n".join(out)
    print(text)
    with open(schedule_path[:-5] + "_report.txt", "w", encoding="utf-8") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    main()
