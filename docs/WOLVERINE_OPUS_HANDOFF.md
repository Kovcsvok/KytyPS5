# Wolverine AMD handoff for local review

This document is the implementation handoff for a later local review on an
RX 9070 XT and i5-14600KF. It deliberately separates safe, mechanical work
from changes that need runtime validation. Do not merge the latter blindly.

## Current baseline

- Current branch baseline: `b3e419ff` (`2026-10-01`)
- Upstream `KytyPS5/KytyPS5` at audit time: `1a053f9e` (`2026-10-20`)
- Divergence: the current branch is 499 commits behind upstream main.
- Existing local Wolverine/AMD refs include:
  - `origin/wolverine-v2`
  - `origin/wolverine-v2-amd-tuning`
  - `origin/wolverine-v2-amd-depth-feedback`
  - `origin/wolverine-v2-amd-hang-markers`
  - `origin/wolverine-v2-amd-upstream-stability`
  - `origin/wolverine-v2-amd-combined-waves`
  - `origin/wolverine-v2-amd-mesh-waves`
  - `origin/wolverine-v2-amd-pixel-wave`
  - `origin/wolverine-v2-amd-wave32-lanes`

The current working tree was not runtime-tested and should not be treated as
an already validated Wolverine build.

## Recommended implementation order

### 1. Rebase/synchronize first

Do not cherry-pick individual old upstream fixes into the current baseline
until the intended base is decided. Prefer creating a fresh branch from the
current upstream main and porting only the Wolverine delta. If preserving this
branch is required, use a temporary integration branch and compare the
resulting behavior before replacing it.

The upstream delta contains shader, GPU resource lifetime, networking, kernel,
and test changes. A partial cherry-pick can compile while silently mixing
incompatible cache keys, renderer assumptions, or test fixtures.

### 2. Safe first pass: diagnostics and reproducibility

These are the best first candidates because they improve failure visibility and
can be disabled without changing normal rendering semantics:

| Commit | Change | Recommendation |
|---|---|---|
| `9c7c24dd` | AMD GPU hang markers, indirect-draw record capture, loop watchdog reporting | Port after the upstream base is aligned; enable watchdog only in the Wolverine launcher preset |
| `9fc81580` | AMD host-fault history and isolated combat lifetime test | Port with the diagnostics only; keep the lifetime test as an opt-in script |
| `161b9962` | Reversible pipeline tuning profiles and tests | Port the profile mechanism, not a hard-coded performance setting |

The watchdog is especially important for this target. The source commit reports
RX 9070 XT menu behavior changing from roughly 3–6 FPS to 30–40 FPS when a
shader loop watchdog was enabled, but this is a report from the author, not a
local reproduction. Keep the watchdog timeout and launcher switch visible and
reversible.

### 3. AMD rendering candidates: validate separately

These changes can help Wolverine but affect synchronization, shader subgroup
selection, or image lifetime. They must be tested independently and should
not be combined into one first build:

| Commit/branch | Risk |
|---|---|
| `5a6cbf7f` / `wolverine-v2-amd-depth-feedback` | Depth-feedback copies change texture/render synchronization; the original commit explicitly says RX 9070 XT validation was pending |
| `0e243baa` / `wolverine-v2-amd-pixel-wave` | Requests guest pixel subgroup sizing; driver-dependent |
| `8177016a` / `wolverine-v2-amd-mesh-waves` | Changes mesh-wave handling on wider host subgroups |
| `ea607205` / `wolverine-v2-amd-wave32-lanes` | Changes RDNA2 wave32 lane arithmetic |
| `7c09ae78` / `wolverine-v2-amd-combined-waves` | Combines subgroup corrections and changes program cache identity |

For every one of these, clear or version the shader/program cache when the
cache key changes. Compare boot, menu, one combat scene, one traversal scene,
VRAM use, device-loss count, and visible corruption separately.

### 4. Changes to keep disabled unless evidence supports them

- `KYTY_BINDLESS_EVICT=1`: the Wolverine notes report stale-image reads and
  device loss after eviction. Do not enable by default.
- `KYTY_FUNCTION_ARRAY_SHRINK=1`: reported to add stutter without reducing VRAM
  use in Wolverine.
- `KYTY_VMA_BLOCK_MB=64`: reported to reproduce the stale-image/device-loss
  path; do not use as a default.
- `KYTY_SKIP_BVH_DISPATCHES=0`: ray-tracing programs were not stable in the
  tested Wolverine path.
- Hard-coded CPU affinity or globally disabling E-cores: do not add without
  topology-aware A/B evidence.
- Game-specific shader skip lists: keep them data-driven and opt-in; never
  present them as general emulator fixes.

## Useful ideas from other forks

### Jetsku/KytyPS5

Use as a source of method and validation discipline rather than a blind merge:

- PGO profiles based on real play sessions and boot paths.
- Explicit upstream synchronization and conflict explanations.
- Heap-based large test fixtures where stack allocation can overflow.
- Small, documented cherry-picks with a test or measured reason.

The PGO profile is not portable evidence for Wolverine. It should only be
recreated from Wolverine traces on the target machine.

### Supermedo/KytyPS5-SuperMedo

Potentially useful later:

- shader seed packing and precompile workflow;
- per-game launcher settings and reproducible launch presets;
- bounded audio queue and gamepad disconnect handling.

Do not import the NVIDIA/DLSS or Demon's Souls-specific changes into the AMD
Wolverine path.

### Almo7aya/KytyPS5-Shaders-Lab

The most useful part is the test/replay approach: isolated RDNA2 execution,
semantic validation, Vulkan replay fixtures, and explicit host-profile
assessments. Treat it as validation infrastructure, not an immediate runtime
optimization.

### AMD Ghost of Yotei test fork

Its reports are useful for diagnosis: compute shaders and CPU/GPU synchronization
were the dominant costs, with high VRAM use and missing effects. The reported
~5 FPS state is not a safe code source for Wolverine.

### john-rm/KytyPS5-optimized and KytyPlus

No directly transferable, measured RX 9070 XT/Wolverine runtime optimization
was found in the inspected history. KytyPlus contains launcher and compatibility
integration with a high conflict surface; defer it unless a specific missing
API or launcher feature is required.

## Local validation checklist

When the hardware is available, use a clean game/cache directory and record:

1. exact emulator commit, driver version, OS build, and game build;
2. cold boot and warm-cache results separately;
3. menu, traversal, combat, and a known crash/hang location;
4. average and 1% low FPS, frame time, shader compile time, VRAM and shared
   memory, GPU utilization, and CPU thread utilization;
5. device-loss count, watchdog trips, skipped draws, and visible corruption;
6. whether disabling the candidate restores the previous behavior.

The first comparison should be baseline versus watchdog/diagnostics only.
Depth feedback and subgroup changes should then be tested one at a time.

## What the local Opus review should do

1. Select the upstream base and produce a clean integration branch.
2. Port `9c7c24dd`, `9fc81580`, and `161b9962` in separate commits.
3. Inspect and adapt `5a6cbf7f` only after checking renderer and cache API
   compatibility with the selected upstream base.
4. Port the wave/subgroup commits individually, versioning cache identity where
   required.
5. Run the validation checklist and keep only changes with a measurable
   stability or performance benefit.
6. Reject any patch that improves one scene by introducing device loss,
   persistent corruption, cache instability, or unexplained stutter.
