# Reference backports for Wolverine v2

This branch starts from the AMD tuning branch. Changes here are candidates until a full
Windows emulator build and an RX 9070 XT gameplay comparison pass.

## Implemented candidate: avoid redundant image-LRU touches

Adapted from TheCruZ/KytyPS5-GTA commit
https://github.com/TheCruZ/KytyPS5-GTA/commit/fba8bc273317ba276a50b246745ad96ccb7047a6

The original uses a collector tick; Wolverine uses LruClock() (presentation progress plus
collector progress). Preserve Wolverine's clock, initialize the per-image timestamp on every
registration, and preserve the LRU list's monotonic timestamp behavior. No eviction limits,
image lifetimes or memory budgets are changed.

KYTY_IMAGE_LRU_MEMO=0 restores repeated touches for an A/B comparison. The default here is 1.
The expected benefit is less CPU cache traffic for repeated texture bindings, not guaranteed FPS.

CI here runs the existing LRU invariant tests with ASan and UBSan. It does not build the whole
emulator or test GPU rendering. The other branches continue building their original artifacts.

## Reviewed upstream references

- Core synchronization/allocation changes:
  https://github.com/KytyPS5/KytyPS5/commit/d58aefde490d192950a24eb7e6cce008adc6afef
  Changed-range BDA synchronization and presentation locking are coupled to surrounding code.
  Wolverine already has revision-tolerant driver-cache loading and partial small-buffer pooling;
  copying those settings again would not establish a new optimization.
- Core depth-feedback handling:
  https://github.com/KytyPS5/KytyPS5/commit/b89971790094405979d7dba65dbe26f970a8d699
  This changes a fatal exit into a one-time message. Keep our depth-copy fallback instead.
- AMD descriptor-pool accounting:
  https://github.com/TheCruZ/KytyPS5-GTA/commit/a309ee6e9a3af16938e03cde3fbf25a80ec47abf
  Relevant candidate if descriptor allocation fails; it requires layout registration and pool
  accounting together. Not imported yet, and no such failure was present in the user's logs.
- Program/pipeline precreation:
  https://github.com/TheCruZ/KytyPS5-GTA/commit/864539ff5f6be79c90217cba68a2ef6bc274cb30
  A larger migration with persistent program identities and input-layout compatibility checks.
  Not implemented in this branch. The existing cache does not by itself precreate all pipelines.

## Build status caveat

The initial AMD build linked kyty_emulator.exe, then failed compiling the inherited
ShaderRecompilerComputeTests.cpp against obsolete APIs (SwapBgra16, sampler snapshot_index,
and an old CopyImageWithBuffer signature). The baseline workflow now builds its supported
targets and retains its previously executed CPU checks. That GPU test harness still needs
adaptation before it can be used to validate rendering changes.
