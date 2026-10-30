# Marvel's Wolverine on KytyPS5, AMD RX 9070 XT: performance findings

Fork `Kovcsvok/KytyPS5`, branch `wolverine-v2-amd-hang-markers`. This file is the English summary
of what was measured, what helped and what did not, so another person (or an AI agent scanning the
KytyPS5 fork network) can reuse it without reading the Hungarian working log `JOURNAL.md`.

Keywords: KytyPS5, PS5 emulator, Marvel's Wolverine, PPSA03671, AMD, RDNA4, RX 9070 XT, Vulkan,
GPU thread, page protection, write bitmap, readback, device-generated commands, live switches.

## Setup and method

- Windows 11, Intel i5-14600KF (6P+8E), AMD RX 9070 XT 16 GB, Adrenalin 26.9.2. Game version
  01.001.005. Reference scene: the Trask Outpost combat (the heaviest scene we found).
- Every change sits behind an environment or live switch (`userdata\kyty_live.txt`, re-read while
  the game runs, see `src/common/liveSwitches.h`), so it can be A/B tested in one session.
- `docs/wolverine-amd/experiment.py` rewrites the live file every 10 s in ABBA order while the
  player keeps fighting; `experiment_report.py` compares the setups from the emulator's telemetry
  (`KYTY_TELEMETRY_MS`, `logs/telemetry_*.csv`, `logs/frames_*.csv`) with a paired estimate that
  cancels scene drift. A result counts only when the paired interval excludes zero.
- Tracy (`--profile`) shows where time goes, but it inflates short, hot zones (its own overhead
  lands in the parent's self time): cross-check with the telemetry counters before acting on it.
- Builds come from GitHub Actions (`.github/workflows/wolverine-hang-markers.yml`, Windows x64,
  `KYTY_HOST_ARCH=x86-64-v3`); artifacts are kept 3 days.

## Where the time goes now (combat, ~18-21 fps, per frame)

The GPU thread (the emulated command processor) is the ceiling: ~75% busy, ~41 ms of work a frame.
Biggest parts: resource materialization (SRT walks) ~9.5 ms, synchronous readbacks of its own page
faults ~5 ms, guest page-protection changes ~4.4-4.9 ms (640 VirtualProtect calls a frame, ~7.5 us
each because of the TLB flush), command buffer submission ~4.5 ms, write-bitmap refinements
~2.2 ms. Guest threads spend ~50 ms a frame (summed over threads) in page faults.

## What helped (combat ABBA, paired fps change)

| Switch (live unless noted) | Change | Why it works |
|---|---|---|
| `KYTY_WRITE_BITMAP=1` + `KYTY_WRITE_BITMAP_UNPROTECT=1` | **+34.3%** (17.1 -> 22.3 fps) | Shaders writing through unbounded bindings set one bit per 4 KiB page; a guest read fault downloads only pages the GPU really wrote (from Senaxx `df053d75`, mionstyle7 `270bba48`/`0b55d9b1`/`3524eff9`). Without UNPROTECT it was -10.9% (dropped pages kept faulting). |
| `KYTY_RANGE_EDIT=1` | **+34.9%** | Edits the virtual range table in place instead of rebuilding it. |
| `KYTY_SRT_PAGE_CACHE=1` | **+19.6%** | Per-page backing lookups for the GPU thread's SRT reads without two hot locks. |
| `KYTY_READBACK_QUEUE_ASYNC=1` + `KYTY_BACKING_LOCKS=2` (+ `RANGE_TICK=1`, `FLUSH_US=500`) | **+11.8%** | Guest readbacks wait for the last writer's tick on a copy queue; lock-free backing copies. |
| `KYTY_ASYNC_WRITE_READBACK=1` on top | **+9.8%** | Guest writes to GPU-written pages no longer drain the GPU on the GPU thread. |
| `KYTY_DEVICE_GENERATED_COMMANDS=1` (start-up) + `KYTY_DGC_DRAWS=1` | **+7.8%** | The GPU generates mesh-indirect draws; no CPU read of GPU-written draw arguments. |
| `KYTY_READBACK_SPARSE_KB=512` (vs 4096) | +5.4%, p99 200 -> 151 ms | Smaller sparse readback windows. |
| `KYTY_REGION_BITMAP=1` | ~+5% (frames per 15 s) | Read syncs of a 2.7 GiB heap skip clean 4 MiB regions through a summary bitmap. |
| `KYTY_FAULT_WIDEN_KB=64` | movement 10-14 -> 15-19 fps | A CPU write fault marks the whole 64 KiB block CPU-dirty. |
| Upstream perf ports (`70668fb`, `75d7a43`, `8cc5f53`, `3c4124d`) | stealth area 18 -> 22-27 fps | See `HANDOFF.md`. |
| `KYTY_SKIP_POPS_SHADERS` / `KYTY_LIVE_SKIP` with `skip_live.txt` | combat 5-10 -> 13-15 fps, no crash | Skips the POPS (ordered pixel) shaders that hung AMD GPUs. |

## What did not help (removed or left off)

| Tried | Result | Lesson |
|---|---|---|
| `KYTY_WRITE_BITMAP_DEFER` (unprotect unwritten pages at write completion) | **-14.2%** | Read faults fell 80%, but the next binding re-protected everything released (protection 4.9 -> 7.9 ms) and labels arrived later. Removed. |
| Eager write-back of GPU-written pages | +15% alone, nothing on top of the write bitmap | Superseded; removed. |
| Tracker present bits, bulk bitmap reads, expander hash trust | ~0 | Tracy had overstated those zones; removed. |
| Pinning the GPU thread to P-cores (`KYTY_GPU_THREAD_CPUS`) | -7.7% (n.s.), more spinning | Removed. |
| `KYTY_DIRECT_READBACK`, `KYTY_READBACK_STALE_ALL`, `KYTY_DISPATCH_BARRIERS=0` | slower or glitches | Left off. |
| `KYTY_DRAW_PREP` + `KYTY_SRT_FAST_READ` | +20% in one run, +3.5% n.s. in the confirmation | Kept on: the mechanism is real (Materialize 21.6 -> 8.8 ms) but the fps gain was scene noise. |

## In progress

`KYTY_WRITE_PREDICT` (live, default 0; 1 measures only, 2 applies after 30 s of learning): a
tracked write protects only the pages it is predicted to write (64 KiB blocks an earlier tracked
write wrote or the GPU thread read, `KYTY_WRITE_PREDICT_BLOCK_KB`). The completion thread checks
the bitmap before the labels of that tick are published and protects pages written after all.
Target: most of the ~4.4 ms of protection changes and ~2.2 ms of refinements on the GPU thread.
Results will be added here.

## Files

- `docs/wolverine-amd/JOURNAL.md`: every experiment with its numbers (Hungarian).
- `docs/wolverine-amd/HANDOFF.md`: current state and machine set-up (Hungarian).
- `docs/wolverine-amd/FORKS.md` and `forkscan/`: review of the whole KytyPS5 fork network (which
  fork has what), with the scripts to repeat it.
- `docs/wolverine-amd/experiment.py`, `experiment_report.py`: the live-switch ABBA tools.
- `docs/wolverine-amd/tracy_parents.py`: per-zone self time and parent/child breakdown from a
  `tracy-csvexport -u` export.
