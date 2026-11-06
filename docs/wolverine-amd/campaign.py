"""Runs several live-switch experiments in one fight, one after another, so one play session answers
several questions (experiment.py runs one A/B).

  python campaign.py plans/2026-11-06.json

A plan is a list of phases:
  {"name": "...", "minutes": 1.5, "hold": {"KYTY_X": 1}}            hold values (no A/B)
  {"name": "...", "minutes": 4, "ab": {"KYTY_X": [0, 2]}, "needs": "phase name"}
"ab" runs experiment.py with A = the first values, B = the second, both on top of the values the
campaign carries. After an A/B: B wins when its paired fps change minus its error is above zero (its
values are then carried into later phases and written to the live file at the end), loses when
plus its error is below zero. A phase whose "needs" lost is skipped. "gate" (optional, on a hold
phase): {"min_sum": {"srt_late_checked": 1}, "max_sum": {"srt_late_mismatches": 0}} stops phases
needing it when a telemetry column's sum over the phase is below / above the value.
An A/B during which the emulator dies or logs a fatal line is "crashed": the setup active at the
crash is named and nothing is carried.
Every phase checks first that the emulator still runs and its log shows no fatal line; otherwise the
campaign stops. The live file gets the winners at the end, nothing else changes. Summary:
D:\\KytyAnalysis\\campaigns\\<time>\\summary.txt (the experiment schedules stay in userdata\\logs).
"""
import csv
import glob
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGE = r"C:\Games\Wolverine-AMD-Async"
LIVE = os.path.join(PACKAGE, "userdata", "kyty_live.txt")
LOGS = os.path.join(PACKAGE, "userdata", "logs")
OUT_ROOT = r"D:\KytyAnalysis\campaigns"
# Child scripts print UTF-8 (PYTHONUTF8), read leniently.
TEXT = {"text": True, "encoding": "utf-8", "errors": "replace", "env": {**os.environ, "PYTHONUTF8": "1"}}
FATAL = re.compile(r"device.?lost|VK_ERROR_DEVICE_LOST|EXIT |unhandled exception|access violation", re.I)

sys.path.insert(0, HERE)
import experiment  # noqa: E402  (read_lines, write_atomic, with_values)


def emulator_running():
    # The console code page (Hungarian Windows: a 0xff thousands separator) is not UTF-8.
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq kyty_emulator.exe", "/NH"],
                         capture_output=True).stdout.decode("ascii", "replace")
    return "kyty_emulator.exe" in out


def newest(pattern):
    found = sorted(glob.glob(os.path.join(LOGS, pattern)), key=os.path.getmtime)
    return found[-1] if found else None


def fatal_lines(log, offset):
    if log is None:
        return []
    with open(log, "rb") as f:
        f.seek(offset)
        text = f.read().decode("utf-8", "replace")
    return [line for line in text.splitlines() if FATAL.search(line)][:5]


def telemetry_sums(start_ms, end_ms, names):
    """Sums of telemetry columns over rows in [start_ms, end_ms] of the newest telemetry file."""
    path = newest("telemetry_*.csv")
    sums = {n: 0.0 for n in names}
    if path is None:
        return sums
    with open(path, encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            try:
                t = float(row.get("unix_ms") or 0)
            except ValueError:
                continue
            if start_ms <= t <= end_ms:
                for n in names:
                    try:
                        sums[n] += float(row.get(n) or 0)
                    except ValueError:
                        pass
    return sums


def setup_at(schedule_path, when_ms):
    """The name of the experiment setup whose block covers when_ms (or the last block's)."""
    try:
        with open(schedule_path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return "unknown"
    setups = {str(k): v.get("name", k) for k, v in data.get("setups", {}).items()}
    blocks = data.get("blocks", [])
    for start, end, setup in blocks:
        if start <= when_ms <= end + 2000:
            return setups.get(str(setup), str(setup))
    return setups.get(str(blocks[-1][2]), "?") + " (last block)" if blocks else "unknown"


def paired_b(report):
    """(mean %, error %) of the second setup's paired change, or None."""
    rows = [l for l in report.splitlines() if re.search(r"[+-][\d.]+% \(\+/-[\d.]+, n=\d+\)", l)]
    if not rows:
        return None
    m = re.search(r"([+-][\d.]+)% \(\+/-([\d.]+), n=(\d+)\)", rows[-1])
    return float(m.group(1)), float(m.group(2))


def main():
    plan_path = sys.argv[1]
    with open(plan_path, encoding="utf-8") as f:
        plan = json.load(f)
    stamp = time.strftime("%Y-%m-%d_%H%M%S")
    out_dir = os.path.join(OUT_ROOT, stamp)
    os.makedirs(out_dir, exist_ok=True)
    summary = []

    def say(text):
        print(text, flush=True)
        summary.append(text)
        with open(os.path.join(out_dir, "summary.txt"), "w", encoding="utf-8") as out:
            out.write("\n".join(summary) + "\n")

    sha_file = os.path.join(PACKAGE, "emulator", "BUILD_SHA.txt")
    build = open(sha_file).read().strip() if os.path.exists(sha_file) else "unknown"
    say(f"campaign {os.path.basename(plan_path)} on build {build}, {stamp}")
    if not emulator_running():
        say("the emulator is not running: nothing done")
        return 1

    original = experiment.read_lines(LIVE)
    carried = {}
    outcome = {}
    log = newest("run_*.log")
    log_offset = os.path.getsize(log) if log else 0
    try:
        for phase in plan:
            name = phase["name"]
            need = phase.get("needs")
            if need and outcome.get(need) in ("lost", "skipped", "stopped", "gated"):
                outcome[name] = "skipped"
                say(f"\n== {name}: skipped ({need} {outcome[need]})")
                continue
            if not emulator_running():
                say(f"\n== {name}: the emulator stopped; campaign ends")
                outcome[name] = "stopped"
                break
            bad = fatal_lines(log, log_offset)
            if bad:
                say(f"\n== {name}: fatal log lines; campaign ends\n" + "\n".join(bad))
                break
            minutes = float(phase["minutes"])
            if "hold" in phase:
                values = {**carried, **phase["hold"], "KYTY_TELEMETRY_MS": 500, "KYTY_EXPERIMENT": 0}
                experiment.write_atomic(LIVE, experiment.with_values(original, values))
                start = int(time.time() * 1000)
                say(f"\n== {name}: hold {phase['hold']} for {minutes} min")
                deadline = time.time() + minutes * 60
                while time.time() < deadline and emulator_running():
                    time.sleep(1)
                end = int(time.time() * 1000)
                outcome[name] = "held"
                gate = phase.get("gate")
                if gate:
                    # Telemetry sums over the phase: {"min_sum": {"column": value}} fails when a
                    # sum is below its value, {"max_sum": {...}} when one is above.
                    low, high = gate.get("min_sum", {}), gate.get("max_sum", {})
                    sums = telemetry_sums(start, end, list(low) + list(high))
                    say(f"   telemetry: {sums}")
                    if any(sums[c] < v for c, v in low.items()) or any(sums[c] > v for c, v in high.items()):
                        outcome[name] = "gated"
                        say("   gate failed: phases needing it are skipped")
                continue
            key_values = phase["ab"]
            a = {k: v[0] for k, v in key_values.items()}
            b = {k: v[1] for k, v in key_values.items()}
            fmt = lambda tag, vals: tag + ":" + ",".join(f"{k}={v}" for k, v in {**carried, **vals}.items())
            # experiment.py reads and restores the live file itself: start it from the carried state.
            experiment.write_atomic(LIVE, experiment.with_values(original, carried))
            say(f"\n== {name}: A/B {a} vs {b}, {minutes} min (carried {carried})")
            run = subprocess.run([sys.executable, os.path.join(HERE, "experiment.py"), "--minutes", str(minutes),
                                  fmt("A", a), fmt("B", b)], capture_output=True, **TEXT)
            schedule = re.search(r"schedule: (.+)", run.stdout)
            if schedule is None:
                say("   experiment.py failed:\n" + run.stdout[-800:] + run.stderr[-800:])
                outcome[name] = "stopped"
                break
            # An emulator that died during the A/B: say in which setup, and carry nothing.
            crashed = not emulator_running() or fatal_lines(log, log_offset)
            if crashed:
                crash_ms = os.path.getmtime(log) * 1000 if log else time.time() * 1000
                say(f"   the emulator died or logged a fatal line during this phase (last log write "
                    f"{time.strftime('%H:%M:%S', time.localtime(crash_ms / 1000))}, setup "
                    f"{setup_at(schedule.group(1).strip(), crash_ms)}); nothing is carried")
            report = subprocess.run([sys.executable, os.path.join(HERE, "experiment_report.py"),
                                     schedule.group(1).strip()], capture_output=True, **TEXT).stdout
            with open(os.path.join(out_dir, f"{len(outcome) + 1:02d}_{name}.txt"), "w", encoding="utf-8") as out:
                out.write(report)
            result = paired_b(report)
            if result is None:
                outcome[name] = "crashed" if crashed else "unclear"
                say("   no paired result")
                if crashed:
                    break
                continue
            mean, err = result
            verdict = "won" if mean - err > 0 else "lost" if mean + err < 0 else "unclear"
            if crashed:
                say(f"   B {mean:+.1f}% (+/-{err:.1f}) before the crash")
                outcome[name] = "crashed"
                break
            outcome[name] = verdict
            say(f"   B {mean:+.1f}% (+/-{err:.1f}): {verdict}")
            if verdict == "won":
                carried.update(b)
    finally:
        experiment.write_atomic(LIVE, experiment.with_values(original, carried))
        say(f"\nlive file: original values plus the winners {carried}")
        for name, verdict in outcome.items():
            say(f"   {name}: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
