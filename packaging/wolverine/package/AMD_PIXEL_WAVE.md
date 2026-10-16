# AMD pixel shader wave size trial

Builds on ea6072052aa78f6d616fd8f307ca5be5a1fa666f (the separate wave32 vertex lane trial). Boot logo skipping and native static depth feedback remain enabled by the portable launcher.

Ports only the pixel-stage subgroup selection from Revivedx/KytyPS5 9c3d0e6144c4287b49da4e2c03e990d75172bc52. Each pixel shader requests its guest wave size when the enabled Vulkan features, requiredSubgroupSizeStages fragment bit, and min/max range permit it. Unsupported hosts retain the previous driver-selected size. No REQUIRE_FULL_SUBGROUPS flag is used for fragment shaders. The pNext structure lives in GraphicsPipelineBuild, alongside all other inputs owned until the asynchronous driver build completes.

Release logs now print default/min/max subgroup sizes and the required-stage mask, plus the selected pixel subgroup path once per guest-wave-size/path combination. This verifies whether the change can actually apply on the host; capability alone is not proof of a rendering fix.

KYTY_PS_SUBGROUP=0 disables the pixel request. Compare against the parent wave32-lane build with its own cache, or use separate fresh _PipelineCache directories when toggling environment options. Environment options are not a supported cache invalidation mechanism. Preserve saves and all previous builds.

This targets pixel subgroup correctness (the donor reports incorrect particles on RADV), not a proven fix for the remaining geometry on Windows AMD. The Windows workflow compiles the full launcher and runs the existing CPU/cache tests and CLI smoke test. Hardware correctness, stability and performance remain unverified. Test the parent wave32-lane build first to isolate its result.

Source: https://github.com/Revivedx/KytyPS5/commit/9c3d0e6144c4287b49da4e2c03e990d75172bc52
