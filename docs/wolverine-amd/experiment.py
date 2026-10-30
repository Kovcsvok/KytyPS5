"""Alternates setups of live switches while the game runs, for experiment_report.py.

The player stays in one scene (for example a fight) and plays; this script rewrites the live
file every --dwell seconds, cycling the setups in A B B A order (so a steady drift of the scene
cancels out), with KYTY_EXPERIMENT=<tag> and KYTY_TELEMETRY_MS on, so the emulator's telemetry
rows (logs/telemetry_*.csv, logs/frames_*.csv) say which setup ran. At the end, or with
--restore, the live file gets its old contents back (switches it did not name keep the first
setup's value, telemetry and the tag go to 0). The schedule goes to logs/experiment_<time>.json.

  python experiment.py --minutes 3 "A:KYTY_READBACK_QUEUE_ASYNC=0" "B:KYTY_READBACK_QUEUE_ASYNC=1"
  python experiment.py --restore
Options: --live FILE (default C:/Games/Wolverine-AMD-Async/userdata/kyty_live.txt),
--dwell 10, --settle 2 (seconds the report drops after each switch), --telemetry-ms 500,
--tracy SECONDS (also records Tracy from the start; needs "Profile Wolverine AMD.bat").
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time

DEFAULT_LIVE = r"C:\Games\Wolverine-AMD-Async\userdata\kyty_live.txt"
TRACY_CAPTURE = r"C:\Games\tracy-0.14.1\tracy-capture.exe"
TRACY_DIR = r"D:\KytyAnalysis\tracy"


def read_lines(path):
    with open(path, encoding="utf-8") as f:
        return f.read().splitlines()


def write_atomic(path, lines):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    for _ in range(40):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:  # the emulator is reading it
            time.sleep(0.05)
    raise SystemExit(f"could not replace {path}")


def with_values(lines, values):
    """lines with each NAME=... set to values[NAME]; names not present are appended."""
    out, seen = [], set()
    for line in lines:
        name = line.split("=", 1)[0].strip() if "=" in line and not line.lstrip().startswith("#") else None
        if name in values:
            out.append(f"{name}={values[name]}")
            seen.add(name)
        else:
            out.append(line)
    out += [f"{k}={v}" for k, v in values.items() if k not in seen]
    return out


def parse_setup(text):
    name, _, body = text.partition(":")
    values = {}
    for item in filter(None, body.split(",")):
        key, _, value = item.partition("=")
        values[key.strip()] = value.strip()
    if not name or not values:
        raise SystemExit(f"bad setup {text!r}, expected NAME:KEY=VALUE,KEY=VALUE")
    return name, values


def backup_path(live):
    return live + ".experiment-backup"


def restore(live):
    backup = backup_path(live)
    if not os.path.exists(backup):
        print("nothing to restore")
        return
    with open(backup, encoding="utf-8") as f:
        saved = json.load(f)
    values = dict(saved["first_setup"])
    original = {}
    for line in saved["lines"]:
        if "=" in line and not line.lstrip().startswith("#"):
            key, _, value = line.partition("=")
            original[key.strip()] = value.strip()
    values = {k: v for k, v in values.items() if k not in original}
    values.update({"KYTY_TELEMETRY_MS": original.get("KYTY_TELEMETRY_MS", "0"),
                   "KYTY_EXPERIMENT": original.get("KYTY_EXPERIMENT", "0")})
    write_atomic(live, with_values(saved["lines"], values))
    os.replace(backup, backup + ".done")
    print(f"restored {live}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("setups", nargs="*")
    ap.add_argument("--live", default=DEFAULT_LIVE)
    ap.add_argument("--dwell", type=float, default=10)
    ap.add_argument("--settle", type=float, default=2)
    ap.add_argument("--minutes", type=float, default=3)
    ap.add_argument("--telemetry-ms", type=int, default=500)
    ap.add_argument("--tracy", type=int, default=0)
    ap.add_argument("--restore", action="store_true")
    args = ap.parse_args()
    if args.restore:
        restore(args.live)
        return
    setups = [parse_setup(s) for s in args.setups]
    if len(setups) < 2:
        raise SystemExit("give at least two setups")
    if os.path.exists(backup_path(args.live)):
        raise SystemExit("a previous experiment was not restored: run with --restore first")

    lines = read_lines(args.live)
    with open(backup_path(args.live), "w", encoding="utf-8") as f:
        json.dump({"lines": lines, "first_setup": setups[0][1]}, f)
    logs = os.path.join(os.path.dirname(args.live), "logs")
    stamp = time.strftime("%Y-%m-%d_%H%M%S")
    record = {
        "setups": {str(i + 1): {"name": n, "values": v} for i, (n, v) in enumerate(setups)},
        "dwell_s": args.dwell, "settle_s": args.settle, "telemetry_ms": args.telemetry_ms,
        "start_ms": int(time.time() * 1000), "blocks": [],
    }
    out_json = os.path.join(logs, f"experiment_{stamp}.json")

    tracy = None
    if args.tracy > 0:
        os.makedirs(TRACY_DIR, exist_ok=True)
        record["tracy"] = os.path.join(TRACY_DIR, f"experiment_{stamp}.tracy")
        tracy = subprocess.Popen([TRACY_CAPTURE, "-o", record["tracy"], "-a", "127.0.0.1",
                                  "-s", str(args.tracy), "-f"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    stop = []
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    order = list(range(len(setups)))
    end_time = time.time() + args.minutes * 60
    round_index = 0
    try:
        while time.time() < end_time and not stop:
            for i in (order if round_index % 2 == 0 else order[::-1]):
                if time.time() >= end_time or stop:
                    break
                name, values = setups[i]
                tag = i + 1
                write_atomic(args.live, with_values(lines, {**values, "KYTY_TELEMETRY_MS": args.telemetry_ms,
                                                            "KYTY_EXPERIMENT": tag}))
                start = int(time.time() * 1000)
                print(f"{time.strftime('%H:%M:%S')} {name} (tag {tag})", flush=True)
                deadline = time.time() + args.dwell
                while time.time() < deadline and not stop:
                    time.sleep(0.1)
                record["blocks"].append([start, int(time.time() * 1000), tag])
                with open(out_json, "w", encoding="utf-8") as f:
                    json.dump(record, f, indent=1)
            round_index += 1
    finally:
        record["end_ms"] = int(time.time() * 1000)
        with open(out_json, "w", encoding="utf-8") as f:
            json.dump(record, f, indent=1)
        restore(args.live)
        if tracy is not None:
            tracy.wait(timeout=args.tracy + 60)
        print(f"schedule: {out_json}")


if __name__ == "__main__":
    sys.exit(main())
