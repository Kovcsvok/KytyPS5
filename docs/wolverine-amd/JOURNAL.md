# Napló (legújabb alul; bejegyzésenként 3–6 sor)

Formátum: `## ÉÉÉÉ-HH-NN – modell – feladat` + mit csináltál, eredmény (mérés), commit, következő lépés.

## 2026-10-16/17 – Codex (GPT) – AMD alapok
mesh/pixel wave-javítások, depth feedback, boot logo skip, TLS-flag fix. A karakterek képe jobb lett, a combat-crash megmaradt. Csomagok: `Documents\Codex\...\outputs`.

## 2026-10-17 – Opus – crash
AMD GPU-markerek (fd82fab) → a lefagyó rajzolás pontosan azonosítható. RGD: nincs page fault. Minden POPS-os pixel shader kihagyásával (`KYTY_SKIP_POPS_SHADERS=1`) a ráugrás, az ütés és az átvezető is átment. Az IndirectDrawGuard (9da53e3) nem ezt javította, de bent marad.

## 2026-10-17 – Opus – menü-FPS
A watchdog nélkül 3–6 FPS, vele 30–40. Ok: a `0x7c5c0785877c0999` S_MEMREALTIME-on pörög, és az óra AMD-n 8× lassú volt (fix `>>3`). Javítás: 0da13e0 (shift a timestampPeriod-ból). Hardveren nincs ellenőrizve → T1.

## 2026-10-21 – Opus – upstream/forkok, átadás
Átvéve: e87fb8c, be9e139. Ütközik (→ T2): 70668fb, 8cc5f53, 3c4124d, 75d7a43, f6f1ac8, 4bb4be5. Forkokban nincs POPS/óra-megoldás. Ígéretes: liberda-soft perf/foundation (x86-64-v3) → T3. Következő: T1, majd T2/T3/T4.
Push: d090e54 → CI fut (nem ellenőriztem). T1 első lépése: CI-állapot. Ha fordítási hiba van, az valószínűleg a 0da13e0/6a8a35a/2952489-ben van; javítsd, vagy reverteld a 6a8a35a/2952489-et.

## 2026-10-21 — Sonnet — T1 (CI-javítás)
A 59889a6 build elbukott: spirvEmitterFlow.cpp(1000): 'GetRealtimeClockShift' nincs deklarálva (0da13e0; a fájl nem includolta a SpirvEmitter.h-t). Javítva az include hozzáadásával, ujra-push. Más fordítási hiba még lehet (ninja az elsőnél megállt).
T2 elő: 70668fb átültetve a helyi wolverine-v3-t2-perf ágon (d806b6e), kapcsoló: KYTY_LEGACY_GLOBAL_BARRIERS=1 a régi viselkedéshez. A T1 build után kerül a fő ágra.

## 2026-10-21 — Gemini — T1 eredmény + T2 1. csomag
T1 (e5fbd3f, shift 0, KYTY_SHADER_LOOP_WATCHDOG=0): a menü 3–6 FPS-ről 24 FPS-re nőtt, de elmarad a watchdogos 38 FPS-től. Ok (0x7c5c0785877c0999 RDNA dump): a shader a CPU-tól kapott s[4:5] cél-időbélyegre vár max s6 tickig (~16 ms @ 100 MHz); eltérő CPU/GPU óra-epoch miatt mindig kifutja az s6 korlátot (shift 3-mal 8x16=133 ms -> 5 FPS, shift 0-val 1x16 ms -> 24 FPS, watchdog=1-gyel 4095 iteráció -> 38 FPS). A Play Wolverine.bat-ban a KYTY_SHADER_LOOP_WATCHDOG=1 alapértelmezés marad; a logokban kizárólag a 0x7c5c0785877c0999 akadt fenn a watchdogon.
T2 1. csomag átültetve: 95193a8 (upstream 70668fb: PM4 globális barrierek elhagyása, KYTY_LEGACY_GLOBAL_BARRIERS=1 visszakapcsolja), f879c99 (upstream 4bb4be5: agc atomi log_count eltávolítása), 0871a86 (upstream 75d7a43: ismételt SSA bitcastok megosztása).

## 2026-10-21 — Gemini — T2 1. csomag mérés + T5 profil + T2 2. csomag
T2 1. csomag (8226cf3: 70668fb + 4bb4be5 + 75d7a43): menü ~36–38 FPS, lopakodós rész állva 18 FPS-ről 22–27 FPS-re nőtt (Tracy átlag: 23,8 FPS), forgolódva/lépkedve 9–18 FPS (dropokkal), crash nem volt.
T5 profil (stealth_stand.tracy, 6,14 s / 146 frame): GPU record szál 91,7%-ban CPU-limitált (WaitForWork csak 513 ms). Top hotspotok: 1) BufferCache::ReadMemory 4460 ms (CommandScheduler::Wait 1381 ms, CopyQueueReadback 812 ms), 2) 689e page fault / 6 s (FaultLockWait 1329 ms + PageManager::Protect 1308 ms), 3) ProgramCache::MaterializeResources 846 ms + BuildKey 738 ms (ShaderGuestMemoryRead 6,9M hívás / 357 ms), 4) Rebind/Prepare/CommitBindings 574 ms.
T2 2. csomag + T5 javítások: b9584a8 (8cc5f53: wave64 SMEM megosztás, KYTY_SHADER_WAVE64_SHARE_SMEM), e4a9f11 (3c4124d: saturate NClamp; f6f1ac8 nem kell, nincs FoldWorklist), plusz T5 célzott fix: ShaderFunctionExpander gyorsítótár-találat ResolveCalls előtt + ShaderReadChunks használata (KYTY_FAST_FUNC_EXPAND=0 kikapcsolja) és KYTY_RESOURCE_MEMO=1 alapból bekapcsolva.
