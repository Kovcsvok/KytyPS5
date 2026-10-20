# Wolverine / AMD review notes

These notes summarize the most promising leads found while comparing
`Kovcsvok/KytyPS5`, upstream KytyPS5, and related forks. They are suggestions
and evidence, not an implementation specification. The local Opus review
should decide what is technically appropriate after inspecting the current
base.

## Context

- Target: Marvel's Wolverine on Windows, RX 9070 XT + i5-14600KF.
- The checked-out historical base was `b3e419ff` (2026-10-01).
- Upstream was `1a053f9e` (2026-10-20), about 499 commits newer at audit time.
- Local Wolverine/AMD refs already exist, including `wolverine-v2`,
  `wolverine-v2-amd-tuning`, `wolverine-v2-amd-depth-feedback`,
  `wolverine-v2-amd-hang-markers`, and the wave/subgroup branches.
- The existing project-specific tuning work (`161b9962`) explicitly targets
  i5-14600KF + RX 9070 XT, but its documentation says runtime validation was
  still pending.

## Most interesting things to try

### AMD hang/watchdog path — strongest first lead

Commit `9c7c24dd` adds AMD GPU hang markers, indirect-draw record capture, and
loop-watchdog reporting. Its author reported that the RX 9070 XT Wolverine
menu changed from roughly 3–6 FPS without the watchdog to 30–40 FPS with it.
That may indicate a shader loop or GPU-progress problem rather than ordinary
rendering cost.

Worth checking:

- whether the watchdog is already present in the selected upstream base;
- whether the timeout is safe for all games or should stay a Wolverine-only
  option;
- whether the captured indirect-draw record identifies the first bad draw;
- whether the watchdog changes only a hang into a skipped/partial result.

The diagnostic parts are likely useful even if the default watchdog policy is
not accepted.

### AMD host-fault history and lifetime evidence

`9fc81580` adds host-fault history and an isolated combat-lifetime trial.
This is useful for distinguishing:

- a stale image/read-after-free problem;
- a bad indirect draw or runaway shader;
- ordinary CPU/GPU synchronization pressure.

The Wolverine notes already describe rare device loss after image memory was
released while the GPU could still read it. This makes lifetime diagnostics
more valuable than speculative memory-pool tuning.

### Depth feedback

`5a6cbf7f` backports AMD depth-feedback copies into the Wolverine branch. It
touches `textureCache.cpp/.h` and `renderDraw.cpp`, and the commit explicitly
says RX 9070 XT validation was still pending.

This is a plausible fix for AMD depth/render feedback behavior, but it changes
image synchronization and lifetime. It should be considered only after checking
the current texture-cache APIs and should remain independently switchable.
Look specifically for:

- unnecessary full-image copies;
- ordering between threaded command recording and deferred image lifetime;
- whether the copy fixes missing depth data or only masks a race;
- cache invalidation and device-loss behavior after repeated combat scenes.

### Pipeline tuning

`161b9962` adds reversible pipeline tuning profiles with
`KYTY_PIPELINE_THREADS` and `KYTY_PIPELINE_WAIT_MS`. The associated notes say
the default on a 14600KF was four pipeline threads and that a 5 ms wait was an
experiment, not a proven universal improvement.

Useful questions:

- does more pipeline parallelism steal time from the game's CPU threads?
- does a shorter wait improve responsiveness but increase stutter?
- are the profiles truly reversible and included in pipeline/cache identity?

Do not hard-code CPU affinity or disable E-cores based only on the CPU model.

### Wave and subgroup changes

The following are technically relevant to RDNA2/AMD but need isolated testing:

- `0e243baa`: guest pixel subgroup sizing;
- `8177016a`: mesh waves on wider host subgroups;
- `ea607205`: wave32 `V_MBCNT` lane handling;
- `7c09ae78`: combined wave corrections and program-cache identity.

Potential benefit is correct guest-wave behavior and fewer shader stalls.
Potential failure modes are wrong lane results, corrupted geometry, shader
cache reuse with incompatible generated code, or driver-specific regressions.
Any change that alters generated shader behavior should be checked for cache
identity/version handling.

## Things the evidence argues against enabling by default

- `KYTY_BINDLESS_EVICT=1`: Wolverine notes report stale-image reads and device
  loss after eviction.
- `KYTY_FUNCTION_ARRAY_SHRINK=1`: reported extra stutter without a VRAM gain.
- `KYTY_VMA_BLOCK_MB=64`: associated with reproducing a stale-image/device-loss
  path.
- `KYTY_SKIP_BVH_DISPATCHES=0`: ray-tracing programs were not stable in the
  tested Wolverine path.
- game-specific shader skip lists as general fixes;
- fixed CPU-affinity masks or global E-core disabling.

## Useful external ideas

- **Jetsku/KytyPS5:** PGO based on real play sessions, explicit upstream
  synchronization, small documented cherry-picks, and avoiding large test
  fixtures on the stack. The method is reusable; the Astro Bot profile is not
  evidence for Wolverine.
- **Supermedo/KytyPS5-SuperMedo:** shader seed packing/precompile and
  reproducible per-game launch settings may reduce first-run stutter. NVIDIA,
  DLSS, and Demon's Souls-specific changes are not relevant here.
- **Almo7aya/KytyPS5-Shaders-Lab:** isolated RDNA2 execution, Vulkan replay,
  and semantic validation are valuable for proving shader changes without
  relying only on FPS.
- **Ghost of Yotei AMD test:** its reports point to compute cost,
  CPU/GPU synchronization, and high VRAM use as useful diagnostic dimensions,
  but its roughly 5 FPS experimental state is not a safe patch source.

## Practical comparison data

For each candidate, compare a clean baseline and one change at a time:

- cold boot versus warm shader cache;
- menu, traversal, combat, and a known hang/crash location;
- average FPS and frame-time spikes;
- VRAM/shared memory, GPU utilization, and CPU-thread utilization;
- shader compile time and stutter;
- device losses, watchdog trips, skipped draws, and visible corruption.

The most informative order is diagnostics/watchdog first, then pipeline tuning,
then depth feedback, then each wave/subgroup change separately. The goal is not
to maximize a single FPS number: a candidate is useful only if it improves
progress or stability without introducing device loss, persistent corruption,
cache instability, or unexplained stutter.
