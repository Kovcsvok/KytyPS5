# AMD wave32 vertex lane trial

Based on 5e6d034e4cdb3e68c8f5a105b40ea6e7f28b3bad: retains native static depth feedback and boot logo skipping.

Backports only the V_MBCNT wave32 vertex lane correction from Revivedx/KytyPS5 commit 9c3d0e6144c4287b49da4e2c03e990d75172bc52, originally credited there to Senaxx d7873b2a. A guest wave32 vertex program masks the host subgroup lane index to 0..31 before evaluating both low and high MBCNT operations. Wave64 programs and all other shader stages are unchanged. No global wave64 forcing or position filtering is added.

The change defaults on. KYTY_VS_LANE_WRAP32=0 restores the previous translation, matching the donor's diagnostic switch. This environment switch is read when translating shaders, so a comparison must use separate fresh _PipelineCache directories; cached shader programs are not a valid A/B comparison across this switch. Preserve saves, and do not delete the previous build or its cache.

Full Windows launcher compilation and existing CPU tests run in the workflow. Rendering correctness and frame rate require hardware testing; no successful GPU result is claimed yet. The old incompatible ShaderRecompilerComputeTests target is not included.

Reference: https://github.com/Revivedx/KytyPS5/commit/9c3d0e6144c4287b49da4e2c03e990d75172bc52
