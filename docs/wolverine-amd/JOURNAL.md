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

## 2026-10-21 — Gemini — T2b tapasztalatok + T3 (x86-64-v3 + VRAM GC) + T4 (POPS kapcsolók)
T2b (4d3987b) New Game intro futás: a KYTY_RESOURCE_MEMO=1 87%-os miss-arányt mutatott (7323 hit / 49880 miss), ezért az alapértelmezést visszaállítottuk 0-ra. Az új játék indításakor 2157 shader-rekord (924 PS, 290 CS, 58 VS, 44 GS) fordult le nulláról (497 MB program cache + 194 MB driver pipeline cache mentve), és az intro+harc crash nélkül lement; közben 22 egyedi POPS pixel shader hash-t gyűjtöttünk ki.
T3 (liberda-soft eee3829): CMake KYTY_HOST_ARCH=x86-64-v3 (/clang:-march=x86-64-v3 a 3rdparty után, így az sccache megmarad) bekapcsolva a CI workflow-ban; TextureCache arányos VRAM GC küszöbök (40% / 70% / 88%, 15 GB-nál ~6,0 / 10,5 / 13,2 GB, KYTY_VRAM_GC_PROPORTIONAL=0 visszaállítja a régit).
T4: vulkaninfo alapján az RX 9070 XT támogatja a VK_EXT_fragment_shader_interlock-ot (pixel+sample=true). Beépítve: KYTY_POPS_ALLOW=<hash,...|all> (egyenkénti POPS-engedélyezés KYTY_SKIP_POPS_SHADERS=1 mellett), valamint KYTY_SHADER_POPS_ZERO_LOAD=1 és KYTY_SHADER_POPS_NO_STORE=1 a spirvEmitterImage.cpp-ben.

## 2026-10-21 — Gemini — T3 Tracy profil (stealth_t3.tracy) + BuildKey regresszió javítása
T3 (570283e) mérés: a lopakodós részen ~8–10 FPS volt (kevesebb/rövidebb droppal). A 6,16 s-os Tracy profil (stealth_t3.tracy, port 8087) azonnal kimutatta az okot: a ProgramCache::BuildKey önideje 179 ms-ról (2,6 us/hívás) 2967,3 ms-ra (145,2 us/hívás, 55x lassulás!) ugrott.
Ok: a 88e2835-ben a ShaderFunctions.cpp-ben a ResolveCalls a recording olvasót kapta meg a sima read helyett, így a változó SRT/call-table memóriaterületek bekerültek az expansion.reads-be, emiatt a StillReads minden compute dispatchnél cache miss-t dobott és újrafordította a hívásfa-kibontást. A ShaderFunctions.cpp visszaállítva az eredetire (a pipelineCache.cpp-ben a ShaderReadChunks megmaradt).

## 2026-10-21 — Gemini — T3b (8478aef) mérés + ráugrás-akadás (1–6 FPS spike) elemzése
T3b (8478aef = x86-64-v3 + arányos VRAM GC + T2a/T2b + ShaderReadChunks): a főmenü elérte a 40 FPS-t, a lopakodós rész állva 23–25 FPS (Record thread: 6544 cmdbuf / 5 s, +90% a Guard2-höz és +20% a T2a-hoz képest).
A peremről való ráugrásnál tapasztalt 1–6 FPS-es megakadás 4 oka a napló (run_2026-10-21_200636.log) alapján: 1) első ráugráskor 33 új shader-permutáció (2660->2693) + 471 textúra-allokáció futott le egyszerre (ezek most bekerültek a 240 MB-os PPSA03671.bin + 2693 rekordos program cache-be); 2) két nem támogatott compute shader (0x29be8047c4e86afe BVH és 0xfc0537b7278db60c) minden dispatchnél végigfuttatja az ExpandShaderFunctions-t az unsupported-ellenőrzés előtt (skip_live.txt / --skip-shaders megkerüli); 3) a 180 mp-enkénti 240 MB-os Vulkan pipeline cache checkpoint-mentés (KYTY_PIPELINE_CACHE_CHECKPOINT_S=0 kikapcsolja); 4) szinkron GPU readbackek animáció közben (KYTY_ASYNC_READ_SNAPSHOT=1, KYTY_COPY_QUEUE_INFLIGHT=1, KYTY_DIRECT_READBACK=1).
