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

## Where the time goes now (combat, ~17-20 fps, per frame; campaign 2026-11-06)

The GPU thread (the emulated command processor) is the ceiling: ~82% busy, ~42-48 ms of work a
frame; the emulator uses ~3.6 of 20 CPU cores. Per frame on the GPU thread: resource
materialization (SRT walks) 9-10.5 ms, its own synchronous readbacks 4.8-6.1 ms, command buffer
submission 4.3-5.5 ms, guest page-protection changes 4.2-5.2 ms, its page faults 4.5-6.2 ms, copy
queue waits 4.1-5.4 ms. Guest threads spend ~44 ms a frame (summed over threads) in page faults,
~16-24 ms of it in read faults on GPU-written pages. Page-protection syscalls run ~1,940 a frame
and track those guest faults, not the GPU thread's upload re-protections (~5.5% of them).

`KYTY_MATERIALIZE_STATS=1` (GPU-thread walks only; the draw-prep scanner thread walks ~80% of the
draws ahead): ~16k walks/s, ~200 ms/s of walk time, almost all in the flat SRT pass. **~800 SRT
reads/s hit bytes the GPU still owns and take ~190 us each (~150 ms/s, roughly 8 ms a frame).**
Buffer, image and sampler descriptor work together is only ~22 ms/s (~1.3 ms a frame). So the
SRT words the GPU writes are the target (joshweible `ac8ee370` + `fcbb019b`), not a descriptor
cache.

## What helped (combat ABBA, paired fps change)

| Switch (live unless noted) | Change | Why it works |
|---|---|---|
| `KYTY_SRT_LATE_WORDS=1` (port of joshweible `ac8ee370` + `fcbb019b`) | **+6.7%** (+/-1.9, 22.6 -> 24.1 fps) | A flat SRT slot that is itself a raw read of a word a GPU buffer write owns stays zero and a GPU copy fills it just before the draw or dispatch, instead of the GPU thread faulting and waiting for the GPU. Operands, descriptors, conditions and the strict reader still read true values. Mode 2 checked 17,884 copies against the true values: 0 mismatches. Materialize 9.2 -> 7.1 ms, GPU-thread faults 4.8 -> 2.8 ms a frame. Measuring first mattered: of ~911 slow SRT reads a second only ~124 really waited for the GPU, all in four shaders. |
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
| `KYTY_WRITE_PREDICT=2` (leave pages readable that a tracked write is predicted not to write; check the write bitmap at completion) | +2.1% (+/-5.0), n.s. | Predictions were almost always right (209 misses in 2.4M checked pages), but protection time did not fall (4.2 -> 4.5 ms): to be correct, the GPU thread's direct guest writes (WRITE_DATA etc., which can reach the guest before the completion check) had to close every still-open page first, ~10M pages per 5 s. One 14 s stall with an 11 s region-lock wait happened in mode 2 only. Removed. |
| `KYTY_WRITE_PREDICT_BLOCK_KB` 16 vs 64 | -2.7% (+/-8.1), n.s. | Removed with the above. |
| `KYTY_SEMA_WAKE_ONE` (upstream `a0268f1d`: a semaphore signal wakes only the waiters it serves) | -5.9% (+/-7.7), n.s. | Removed. |
| `KYTY_REPROTECT_BATCH` (carbonimax `9ef01206` + `3a154025`: read-only upload re-protections recorded and applied in merged runs before submissions and guest sync points) | **-7.4%** (+/-3.0) | Protection syscalls did not fall (1945 -> 1940 a frame): the deferrable re-protections are only ~5.5% of them; the rest follow the ~850-900 guest faults a frame. Sync packets flush ~550 times a second, ~9 copies each, so little merges; spinning rose 3.2 -> 8.3 ms and guest read faults 28 -> 32 ms. Removed. |
| `KYTY_SRT_LATE_WIDE` (late words also in programs that capture specialization reads, and for uniform buffer reads) | +9.0% (+/-4.4) "won", rejected | Late words 15.8 vs 16.1 a frame, fault reads 2.98 vs 3.00: it added no late word in this scene, so the gain is scene noise. Verified correct (9,826 checks, 0 mismatches). Removed. |
| `KYTY_DRAW_PREP` + `KYTY_SRT_FAST_READ` | +20% in one run, +3.5% n.s. in the confirmation | Kept on: the mechanism is real (Materialize 21.6 -> 8.8 ms) but the fps gain was scene noise. |

## In progress

The remaining GPU wait in Materialize is shader `0xfb5ee753d43f3f0b`: 16-24 strict flat reads a
second in combat, ~2.5-3.5 ms each (37-70 ms/s), almost all with the writing GPU work still
running. The plan marks a flat slot clean (exact) when it feeds a written buffer's or image's
descriptor (the alias proof needs the exact address), an indirect image's material or table
source, or a bindless sampler. Next: log which of these it is and whether the value changes
between reads.

## Next

1. PGO with an own Wolverine combat profile (pengui07 `KytyPS5-nhl` `KYTY_PGO=generate|use`).
2. Later: an upload thread (inFAMOUS shadPS4 fork pattern); a descriptor cache only if the
   descriptor share grows.

## Related work in other forks (scan 2026-11-06)

From the whole KytyPS5 fork network (`forkscan/`, 1136 active forks); candidates for the same
bottlenecks, not yet ported unless noted:

- joshweible `yotei-front-thread` (Ghost of Yotei): `ac8ee370` fills flat SRT words the GPU is
  still writing with a GPU copy instead of a CPU wait (their GPU-thread read waits 25 -> 7 ms);
  `99082835` parses PM4 packets on a front thread; `7eb33b86`/`1609f5e6` read clean SRT bytes
  without the fault (we already do this through `TryReadGpuCleanBacking`).
- carbonimax `3a154025` `KYTY_REPROTECT_BATCH` (+8.5% in one game, +1.4% in another): batches the
  re-protections after uploads until guest sync points; `fd13335327` records one copy per
  destination between shared barriers. Ported as `KYTY_REPROTECT_BATCH`: -7.4% here (see above),
  removed.
- Upstream `a0268f1d` (a semaphore signal wakes only the waiters it hands counts to): ported as
  `KYTY_SEMA_WAKE_ONE`, no gain here (see above), removed.
- PGO: pengui07 `KytyPS5-nhl` (clang-cl, `KYTY_PGO`, 45.6 -> 49.8 fps in NHL), also Xx-LiDAF-xX,
  chenxiao07, ArtyomZabroda (`__llvm_profile_write_file` before `quick_exit`), duy64.
- Xx-LiDAF-xX: ReleaseMem/submit batching and write-invalidation clustering (we have
  `KYTY_SLICE_FLUSH_US` and `KYTY_FAULT_WIDEN_KB`); MrBigBritches: batched hot readbacks, submit
  cadence; JOmega321: `KYTY_SERVE_IN_WAITS` (needs an async upload worker).

## Measuring several things in one session

`docs/wolverine-amd/campaign.py <plan.json>` runs the phases of a plan (`plans/`) one after
another during one fight: hold phases (with a telemetry gate) and ABBA A/B phases; a B that wins
(paired change minus its error above zero) is carried into later phases and kept in the live file,
a phase whose prerequisite lost is skipped, and the campaign stops if the emulator dies or logs a
fatal line. The package's `Update Emulator.ps1` installs the newest CI build when the game starts.

## Files

- `docs/wolverine-amd/JOURNAL.md`: every experiment with its numbers (Hungarian).
- `docs/wolverine-amd/HANDOFF.md`: current state and machine set-up (Hungarian).
- `docs/wolverine-amd/FORKS.md` and `forkscan/`: review of the whole KytyPS5 fork network (which
  fork has what), with the scripts to repeat it.
- `docs/wolverine-amd/experiment.py`, `experiment_report.py`: the live-switch ABBA tools.
- `docs/wolverine-amd/tracy_parents.py`: per-zone self time and parent/child breakdown from a
  `tracy-csvexport -u` export.
