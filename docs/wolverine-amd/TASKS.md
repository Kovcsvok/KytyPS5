# Feladatok (Sonnet / olcsóbb modell)

Minden feladat előtt olvasd el: `HANDOFF.md`, a `JOURNAL.md` utolsó 20 sorát és csak a saját feladatodat. A feladat végén írj 3–6 sort a `JOURNAL.md`-be.
**Opust hívd** (szólj a felhasználónak), ha: egy hipotézis kétszer megbukott; a shader-fordító szemantikáját kellene megváltoztatni (Translate/emitter); új crash jön, és a GPU-marker/RGD kimenetét nem érted; vagy merge előtt a fő ágba.

## T1 – Óra-javítás ellenőrzése [KÉSZ]
- **Eredmény (`e5fbd3f`, `KYTY_SHADER_LOOP_WATCHDOG=0`):** a naplóban `Shader clock: timestamp period 10.000 ns, S_MEMREALTIME shift 0`. A főmenü FPS-e **3–6 FPS-ről 24 FPS-re** nőtt, míg `KYTY_SHADER_LOOP_WATCHDOG=1`-gyel **38 FPS**.
- **Ok (`0x7c5c0785877c0999` RDNA/IR dump elemzése):** a shader a játék GPU frame-pacer várakozója (`min(t0 + s6, max(s[4:5], prev_t0 + s7))`). Mivel a CPU óra-epoch (`s[4:5]`) nem egyezik a Vulkan `OpReadClockKHR(Device)` epochjával (`t0`), mindig kifutja az `s6` (~16 ms @ 100 MHz) korlátot (`26 ms render + 16 ms spin = 42 ms = 24 FPS`). A `KYTY_SHADER_LOOP_WATCHDOG=1` 4095 iterációnál (~0,2 ms) elvágja a ciklust (de kiírja az időbélyegeket), így megmarad a **38 FPS**; az összes naplóban kizárólag a `0x7c5c0785877c0999` akad fenn a watchdogon.

## T2 – Ütköző upstream perf-commitok kézi portolása [KÉSZ]
- **1. csomag (`8226cf3`):** `95193a8` (upstream `70668fb` – PM4 globális barrierek elhagyása, `KYTY_LEGACY_GLOBAL_BARRIERS=1` visszaállítja), `f879c99` (`4bb4be5` – agc log_count eltávolítása), `0871a86` (`75d7a43` – ismételt SSA bitcastok megosztása).
  - **Mérés (`C:\Games\Wolverine-AMD-T2a`):** menü **36–38 FPS**, lopakodós rész állva **18 → 22–27 FPS** (Tracy: 23,8 FPS átlag, 5400 cmdbuf / 5 s), forgolódva/lépkedve **9–18 FPS**, 0 crash.
- **2. csomag (`4d3987b`):** `b9584a8` (`8cc5f53` – wave64 SMEM megosztás, `KYTY_SHADER_WAVE64_SHARE_SMEM=0` kikapcsolja), `e4a9f11` (`3c4124d` – `EmitFPSaturate32` `GLSLstd450NClamp`; a `NativeFloatMinMax` és az 5-op `EmitFPMedTri32` már bent volt; az `f6f1ac8` nem alkalmazható, mert ezen az ágon nincs `FoldWorklist`).

## T3 – liberda-soft/personalstation5 „perf/foundation” [KÉSZ (`570283e` + `8478aef`)]
- Beépítve (`570283e` + `8478aef`):
  1. `CMakeLists.txt` + `.github/workflows/wolverine-hang-markers.yml`: `KYTY_HOST_ARCH=x86-64-v3` (`/clang:-march=x86-64-v3` a `3rdparty` után, így az `sccache` megmarad).
  2. `textureCache.cpp`: arányos VRAM GC küszöbök (`40% / 70% / 88%`, RX 9070 XT-n `5758 / 10076 / 12668 MiB`, `KYTY_VRAM_GC_PROPORTIONAL=0` visszaállítja a régit).
- **Mérés (`8478aef`, `C:\Games\Wolverine-AMD-T3b`):** főmenü **40 FPS**, lopakodós rész állva **23–25 FPS** (`6544 cmdbuf / 5 s`), ráugrásnál 1–6 FPS-es rövid megakadás (okait lásd a T5 alatt).

## T4 – POPS: ne kelljen kihagyni [ESZKÖZÖK BEÉPÍTVE (`570283e`), OPUS ÉRTÉKELÉSRE / KÍSÉRLETEKRE KÉSZ]
Tények: egy POPS-os pixel shaderrel (pl. `0x2464e975d88b7f15`, az NGG mesh `0xfc94abf5b9e0e8e4` rajzolásában) elakad a GPU. Az RGD szerint nincs page fault, és „no information about in-flight shaders”. A shader a kritikus szakaszban `IMAGE_LOAD`/`IMAGE_STORE`-ral ugyanazt a storage image-et írja/olvassa. Dumpok: `C:\Games\Wolverine-AMD-HangMarkers\userdata-dump\shader-dump\2464e975d88b7f15.{rdna,}.txt`.
Elvégzett előkészületek:
1. **`vulkaninfo` ellenőrzés [KÉSZ]:** az AMD RX 9070 XT (Adrenalin 26.9.2) **támogatja a `VK_EXT_fragment_shader_interlock` kiterjesztést** (`fragmentShaderSampleInterlock = true`, `fragmentShaderPixelInterlock = true`, `fragmentShadingRateWithFragmentShaderInterlock = false`).
2. **`KYTY_POPS_ALLOW=<hash,...|all>` [KÉSZ a `570283e`-ben]:** a `ShaderRecompiler.cpp`-ben a `KYTY_SKIP_POPS_SHADERS=1` mellett egyenként engedélyezhetők a POPS pixel shaderek (és a `programDiskCache.cpp` `Known[]`-ba is bekerült).
   - **Lopakodós részen előforduló 3 POPS shader:** `0x2464e975d88b7f15`, `0x2dd1f53a920d4efa`, `0x557ffa4a21f87ef4`.
   - **Trask Outpost (New Game intro + harc) során előforduló összes (22 db) POPS shader:** `0xe4ef0b2529951a0f`, `0xaa760bdf4eddec15`, `0x4fa6faa57bc9a97a`, `0x2209aa9404e8606a`, `0xba796dac83b0658f`, `0xfcfb268bca438b2d`, `0x1574cecd82c28df4`, `0xa3e62c189ae4564b`, `0x690ec7936920d329`, `0xb344cc7a6646c514`, `0x8b8e92dd69f19158`, `0x9a7be76258fefb25`, `0xe940d4740563d001`, `0x557ffa4a21f87ef4`, `0x70b3f1c0079d12e0`, `0x4c5c243a3d0c358d`, `0xcd76a2365dc7c9e6`, `0xbfacdd28a2288ac4`, `0x2464e975d88b7f15`, `0x2dd1f53a920d4efa`, `0x9199454ef844a809`, `0x618eca57f179de7d`.
3. **SPIR-V emitter kapcsolók [KÉSZ a `570283e`-ben]:** `spirvEmitterImage.cpp`-ben `KYTY_SHADER_POPS_ZERO_LOAD=1` (pixel shader storage `ImageRead` -> 0) és `KYTY_SHADER_POPS_NO_STORE=1` (pixel shader `ImageWrite` -> no-op).
4. **Opus-elemzés (2026-10-21, lásd `JOURNAL.md`):** a POPS-szinkron a fordításban kiesik, nem az fagy. Tesztek: `Wolverine-AMD-T3b\T4a POPS be.bat` → (ha fagy) `T4b POPS be kep nelkul.bat`.
5. **Régi terv:** a `0x2464e975d88b7f15`-tel (`KYTY_POPS_ALLOW=0x2464e975d88b7f15`) kipróbálni a `KYTY_SHADER_POPS_NO_STORE=1` és `KYTY_SHADER_POPS_ZERO_LOAD=1` kapcsolókat, majd ha a párhuzamos RMW ütközés okozza a GPU-hangot, a POPS kritikus szakaszát `OpBeginInvocationInterlockEXT` / `OpEndInvocationInterlockEXT` (`SPV_EXT_fragment_shader_interlock`) utasításokba csomagolni!

## T5 – Játékmenet-profil [KÉSZ]
- Mentett profilok: `C:\KytyDiagnostics\tracy\stealth_stand.tracy` (`T2a`, 6,14 s, 146 frame = 23,8 FPS állva) és `C:\KytyDiagnostics\tracy\stealth_t3.tracy` (`T3`, 6,16 s, 44 frame mozgás közben), CSV exportokkal (`*_total.csv`, `*_self.csv`).
- **Fő megállapítások:** a GPU record szál **91,7%-ban CPU-limitált** (`GuestGpu::WaitForWork` csak 438–513 ms / 6,1 s).
  1. **`BufferCache::ReadMemory` (`bufferCache.cpp:774`):** állva **4460,6 ms**, mozgás közben **23 278,8 ms** (CPU szálakon összesen; `WaitWriteReadback` 5975 ms, `CommandScheduler::Wait` 1381–4295 ms, `CopyQueueReadback` 356–812 ms, `ReadMemory(GPU thread)` 667 ms).
     - **Javasolt élő A/B teszt (`kyty_live.txt`-ben azonnal kipróbálható, kódmódosítás nélkül!):** `KYTY_ASYNC_READ_SNAPSHOT=1`, `KYTY_COPY_QUEUE_INFLIGHT=1`, `KYTY_DIRECT_READBACK=1` (lásd `bufferCache.cpp:287-312, 946-955`).
  2. **Page-fault vihar (217e – 689e fault / 6 s):** `MemoryTracker::FaultLockWait` (402–1329 ms) + `PageManager::Protect` (440–1308 ms) + `BufferCache::InvalidateMemory` (341–1531 ms).
  3. **`ProgramCache::MaterializeResources` + `BuildKey`:** állva `846 ms + 738 ms` (`ShaderGuestMemoryRead` 7,73M hívás = 574 ms). A `KYTY_RESOURCE_MEMO=1` 87%-os miss-arányt adott (`7323 hit / 49880 miss`), ezért kikapcsolva maradt. Az `ExpandShaderFunctions` most már átadja a `ShaderReadChunks`-ot (`88e2835` / `8478aef`).
  4. **Descriptor kötés:** `FindBuffers` (262 ms self) + `RebindBuffers` (97–322 ms) + `PrepareBindings` + `CommitBindings`.

## T6 – Átvezetők 1 FPS (kutatás) [NYITOTT]
Ismert: a CPU a GPU label-jére vár. A New Game (Trask Outpost) átvezető alatt a naplóban: `Label waits (5 s): suspended 220–349, forwardable 0, forwarded 236–302`. Profilozz egy átvezetőt (`tracy-capture`), és nézd meg, melyik várakozás dominál (`CommandScheduler::Wait`, `WaitRegMem`, `Finish`).

## T7 – Valódi bindless storage image támogatás (Opus) [KÓD KÉSZ, TESZTELENDŐ]
Ok: a `DefineBindlessImages` a bindless storage image-hez `Sampled=2` tömböt deklarál a set 1 sampled bindingjére; a `BindlessTable`-ben nincs storage tömb, a képek `SHADER_READ_ONLY_OPTIMAL`-ban vannak. `3ddfd07` óta a pixel shader írásai kimaradnak (crash megszűnt, de a vérfolt villog/rossz).
Terv: 1) `BindlessTable`: `eStorageImage` tömbök (2D, 2DArray) ugyanazzal a slot-indexszel; storage-képes képekhez storage nézet (sRGB → UNORM nézet, `MUTABLE_FORMAT`), egyébként placeholder. 2) Az írt bindless képek layoutja `GENERAL` (a sampled descriptor is `GENERAL`-lal), STORAGE usage. 3) Emitter: storage tömb a saját bindingre. 4) Írás után a kép GPU-módosítottnak jelölése (texture cache). Kapcsoló: `KYTY_SHADER_BINDLESS_STORAGE_WRITES`.

## T8 – Árnyék-villogás [NYITOTT]
Az árnyékok gyorsan váltakozva eltűnnek/visszajönnek (POPS-tól és a laphiba-szélesítéstől független, a régebbi buildekben is volt). `KYTY_KEEP_GPU_IMAGES=1` nem segít, fekete foltot okoz. Nyom: `textureCache.cpp` ~1580 komment (1664x936 GPU-írt képek minden második frame-ben újrafeltöltve a buffer cache-ből). Következő: `KYTY_IMAGE_STATS=1`/feltöltés-statisztika egy árnyékos jelenetben, F9 frame-dump.

## T10 – Engine-beállítások `commandline.txt`-ből (kutatás) [NYITOTT]
A játék `commandline.txt`-je `{ Szekció.Mező=érték }` sorokat fogad (pl. `{ EngineDebug.FrameTimeGraphEnable=false }`), felülírható a `userdata\overrides\commandline.txt`-vel (a játékot ne módosítsd!). Mezőjelöltek és szekciójelöltek: lásd `JOURNAL.md` 2026-10-22 „FSR/frame gen…”. Első lépés: kideríteni, hogyan párosul a szekció a mezővel (eboot reflexiós táblák: a mezőnév-string címére mutató pointerek környezete), és látszik-e a játék hibakimenete (a `-nodbgmsg` nélkül). Cél: felbontás/LOD/mip bias/árnyék-cache, mérés `KYTY_SHADER_STATS` + FPS.

## T11 – Occlusion query-k (kutatás) [NYITOTT]
A napló szerint a játék occlusion query-ket használ, amelyeket az emulátor „mindig látható”-nak vesz → minden takart objektum is kirajzolódik. Felmérni: hogyan használja (GPU-predikáció vagy CPU-olvasás), mennyi draw-t takarítana meg, és mekkora munka a valódi (Vulkan occlusion query + eredmény a vendégmemóriába) megvalósítás.
