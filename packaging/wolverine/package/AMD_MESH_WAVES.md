# AMD mesh wave separation trial

Parent: ea6072052aa78f6d616fd8f307ca5be5a1fa666f. The user reports this parent's vertex lane correction removed the exploding geometry, but incomplete characters and a combat-time AMD driver timeout remain. Native static feedback and boot logo skipping are retained. This branch does not include the separately built pixel subgroup trial.

Port the targeted mesh-wave pass selection from Revivedx/KytyPS5 9c3d0e6144c4287b49da4e2c03e990d75172bc52: when several guest wave32 waves would share a wider host subgroup, execute one guest wave per existing mesh pass. The donor identifies cross-wave ballot/EXEC/VCC masks as a cause of missing skin primitives. The existing multi-pass emitter is reused; no geometry is suppressed to hide the symptom.

The first 24 mesh program preparations log guest wave size, host subgroup size, wave count and selected/previous pass count. KYTY_MESH_WAVE_PASSES=0 restores the parent selection. Use separate fresh program-cache directories when comparing translation-affecting environment options, preserving saves and previous builds.

Windows full launcher compilation, existing CPU/cache tests and CLI smoke check run in the workflow. GPU correctness, remaining missing character parts, the pink artifact, combat stability and performance require hardware tests. This is not a confirmed driver-timeout fix. The inherited incompatible ShaderRecompilerComputeTests target is not run.

Source: https://github.com/Revivedx/KytyPS5/commit/9c3d0e6144c4287b49da4e2c03e990d75172bc52
