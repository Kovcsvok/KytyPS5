# Upstream stability trial

Based on 7c09ae7, whose character/texture improvements the user confirmed;
combat still loses the AMD device at the same lunge. All wave and depth fixes remain.

Backports upstream 42bb0da (#1571), preserving CPU flags across TLS stubs.
The upstream executable regression test is included and CI runs it with
--tls-stubs-only. This is a confirmed code defect, not a confirmed Wolverine fix.

KYTY_HOST_BREADCRUMBS=1 records 256 recent host operations in bounded memory,
including draw pixel shader hashes and compute program addresses where existing
markers provide them. A synchronized snapshot is printed on device loss, including
on AMD without NV checkpoints. These are CPU observations, not GPU completion.
No per-operation disk I/O. Disabled by default outside the diagnostic launcher.

Test Combat Lifetime.bat copies saves once into separate userdata-combat-lifetime,
disables bindless eviction and slot reuse, and enables breadcrumbs. It tests the
resource lifetime hypothesis; increased VRAM or descriptor use is a tradeoff.
Play Wolverine.bat remains the normal comparison profile.

Upstream review (2026-10-17): latest observed main 42bb0da; latest published
release d7974dc. The sparse-mapping change d7974dc was reverted by d01633e;
do not backport it. Performance commit d58aefd changes allocation/synchronization
paths which differ substantially here. This fork already pools small BDA buffers,
limits synchronization to dirty epochs/submissions and has compatible driver
cache support. Broad replacement would discard Wolverine-specific work. No new
FPS improvement claimed in this stability trial.

Before EVERY future build check upstream releases and main commits, including
reverts. Retain known-good Wolverine changes and review selected patches/tests.

Sources:
https://github.com/KytyPS5/KytyPS5/commit/42bb0da520992e35f062f44e6762484c356b9ded
https://github.com/KytyPS5/KytyPS5/commit/d01633e18edfdc27f90e7d42a6f646c38934b979
https://github.com/KytyPS5/KytyPS5/commit/d58aefde490d192950a24eb7e6cce008adc6afef
