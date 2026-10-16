# AMD combined wave trial

Combines the mesh-wave change from 8177016 with the pixel subgroup change
from 0e243ba, retaining the tested ea60720 vertex lane correction, native static
depth feedback, boot-logo skip and existing pipeline tuning.

The pixel request remains gated by Vulkan device capabilities and its pNext
storage belongs to the asynchronous graphics pipeline build. Mesh passes remain
gated by the existing emitter support, wave width and wave count checks.

Also fixes program-cache signature classification for KYTY_VS_LANE_WRAP32.
Previously the generic translation-switch filter missed this name, so toggling
it could reuse SPIR-V compiled with the opposite setting. A regression check is
included in ProgramDiskCacheTests. Mesh-wave switches already match that filter.

Use separate userdata and a copy of saves for each comparison. No combat-crash
fix or FPS improvement is claimed. Hardware testing must compare the same scene,
character close-up, pink effect and attack, preferably again after caches warm.

Relevant Infamous-fork review:
- d15e462 fixes SDWA comparison destinations in shadPS4. Kyty's decoder already
  selects inst.dst for the explicit scalar destination, and its translator writes
  that destination; the shadPS4 dst[0]/dst[1] patch does not apply here.
- 73539f3 illustrates that cached shader paths must honor translation changes.
  Kyty's concrete issue addressed here is its missing vertex-switch cache key.
- 69f3dde and 9443ed8 optimize shadPS4-specific buffer binding/recording paths.
  Their measured gains cannot be transferred to this renderer without profiling.

References: https://github.com/rayhlancaner-png/shadPS4-Infamous-FixBuild
and https://github.com/Revivedx/KytyPS5/commit/9c3d0e6144c4287b49da4e2c03e990d75172bc52
