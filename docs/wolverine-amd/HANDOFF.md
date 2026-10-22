# Wolverine on KytyPS5 – AMD (RX 9070 XT) munka: átadó

Gép: Windows 11, i5-14600KF, RX 9070 XT 16 GB, Adrenalin 26.9.2. Játék: Marvel's Wolverine PPSA03671 01.001.005.
Ág: `wolverine-v2-amd-hang-markers` a `Kovcsvok/KytyPS5` forkban. Előtte a Codex AMD-ágai (mesh/pixel wave, depth feedback).
Napló: `JOURNAL.md` (mindenki ide ír, röviden). Feladatok: `TASKS.md`. Promptok: `PROMPTS.md`.

## Állapot (2026-10-21 este – T1, T2, T3, T5 kész; T4 előkészítve Opusnak; 8478aef CI fut)
- **Crash (combat, ráugrás/ütés/átvezető):** megkerülve (`KYTY_SKIP_POPS_SHADERS=1`). Az új játék (Trask Outpost) nyitó átvezetője és harca, valamint a lopakodós rész is 0 crash-sel ment végig. Összesen **22 egyedi POPS pixel shader hash** lett kigyűjtve. A **T4**-hez az `RX 9070 XT`-n a `VK_EXT_fragment_shader_interlock` támogatott (`pixel=true, sample=true`), és bekerültek a `KYTY_POPS_ALLOW=<hash,...|all>`, `KYTY_SHADER_POPS_ZERO_LOAD=1`, `KYTY_SHADER_POPS_NO_STORE=1` kapcsolók (`570283e`).
- **Menü-FPS (T1 kész):** a `0da13e0` (`S_MEMREALTIME` shift 0 a `timestampPeriod = 10.0 ns`-ból) watchdog nélkül **3–6 FPS-ről 24 FPS-re** javította a menüt, de a watchdogos (`KYTY_SHADER_LOOP_WATCHDOG=1`) **36–38 FPS**-től elmarad. Ok (`0x7c5c0785877c0999` RDNA/IR dump): a shader a játék GPU frame-pacer várakozója (`min(t0 + s6, max(s[4:5], prev_t0 + s7))`), és mivel a CPU óra-epoch (`s[4:5]`) eltér a Vulkan `OpReadClockKHR` epochtól (`t0`), minden képkockánál kifutja az `s6` (~16 ms @ 100 MHz) korlátot (`26 ms render + 16 ms spin = 42 ms = 24 FPS`). A `KYTY_SHADER_LOOP_WATCHDOG=1` 4095 iterációnál (~0,2 ms) elvágja a ciklust és megőrzi a **38 FPS**-t; a naplókban kizárólag ez az egy shader akad fenn a watchdogon.
- **Játékmenet (T2 + T3 + T5):**
  - **T2 (`8226cf3` + `4d3987b`):** az összes alkalmazható upstream perf-commit átültetve (`70668fb` -> `95193a8`, `4bb4be5` -> `f879c99`, `75d7a43` -> `0871a86`, `8cc5f53` -> `b9584a8`, `3c4124d` -> `e4a9f11`; `f6f1ac8` nem kell, mert nincs `FoldWorklist`). A lopakodós rész FPS-e állva **18-ról 22–27 FPS-re** nőtt (Tracy átlag: 23,8 FPS), forgolódva/lépkedve **9–18 FPS**.
  - **T3 (`570283e` + `8478aef`):** `KYTY_HOST_ARCH=x86-64-v3` (`/clang:-march=x86-64-v3` a `3rdparty` után) + arányos VRAM GC küszöbök (`40% / 70% / 88%` = `5758 / 10076 / 12668 MiB`, `KYTY_VRAM_GC_PROPORTIONAL=0` a régi). **Mérés (`8478aef`, `C:\Games\Wolverine-AMD-T3b`):** főmenü **40 FPS**, lopakodós rész állva **23–25 FPS** (`6544 cmdbuf / 5 s`, +20% a `T2a`-hoz és +90% a `Guard2`-höz képest!), mozgás/ráugrás közben időszakos 1–6 FPS-es megakadásokkal (4 ok azonosítva a `JOURNAL.md`-ben és a `TASKS.md`-ben).

## Állapot (2026-10-22 délután – friss, ezt olvasd először)
- **Ajánlott csomag:** `C:\Games\Wolverine-AMD-Async\Play Wolverine AMD Async.bat` (`c5cf56c`, a 95572c5 program-cache-ével `KYTY_PROGRAM_CACHE_GIT_REVISION`-nel; `KYTY_ASYNC_SHADERS=2` + 4 worker). Ugyanaz a játszhatóság, mint a `Wolverine-AMD-Play`, + háttér-transzláció, + 5 s-os FPS-sor a naplóban (`Frames (5 s)`). Profilozás: `Profile Wolverine AMD.bat` + `Tracy 30s.bat`.
- **Teljesítmény-kép (mérve):** menü 45, nyugodt rész 25–32, új részek felé haladva és harcban **5–10 FPS** – a régi 95572c5-tel is ugyanennyi (nem regresszió). A shaderfordítás ebben már elhanyagolható (meleg cache). Fő költség (`C:\KytyDiagnostics\tracy\forward_2026-10-22_100601.tracy`): a GPU-szál draw-onkénti CPU-munkája (~11 ezer draw + ~11 ezer dispatch / s, 60–90 µs/draw) és a vendégszálak GPU→CPU visszaolvasásai. Kipróbálva, **nem segített**: `KYTY_STALE_INFLIGHT_READS=1`, `KYTY_GPU_THREAD_PRIORITY=2`, `KYTY_VIDEO_OUT_1080P=1` (a játék figyelmen kívül hagyja), `KYTY_READBACK_TILED_IMAGES=1` (T8-ra sem). A játék occlusion query-ket a CPU-n/compute-tal olvas, predikáció nincs (T11). Következő nagy feladat: **T12**.
- **Vér (T7)** és **árnyék-villogás (T8)**: változatlanul nyitott (lásd lent).

## Állapot (2026-10-22 reggel)
- **Játszható (tesztelve: nincs crash, mozgás 19–26 FPS, néha 28–32; a vérfolt-hiba egy idő után eltűnik):** `C:\Games\Wolverine-AMD-Play\Play Wolverine AMD.bat` (`95572c5`): vér/POPS be, a POPS shaderek bindless képírása eldobva (nincs crash; Wolverine vérfoltja villoghat), laphiba-szélesítés 64 KiB (mozgás 10–14 → 15–19 FPS), natív float min/max (upstream 2864818).
- **Vér-crash oka (T7, nyitott):** a POPS shaderek (0x2464…, 0x5047…) bindless storage képekbe írnak (HDR render target, 1664x936 RGBA16F pufferek). AMD-n bármely valódi kép írása fagy (helyes storage tömbökkel és GENERAL layouttal is), az 1x1 placeholderbe írás nem. Kapcsolók: `KYTY_SHADER_BINDLESS_STORAGE_WRITES=2`, `KYTY_BINDLESS_STORAGE_PLACEHOLDER=1|2` (élő), `KYTY_SHADER_BALLOT_NO_HELPERS=1`. Következő: RGD Hardware Crash Analysis `=2`-vel.
- **Árnyék-villogás (T8, nyitott)**, `KYTY_KEEP_GPU_IMAGES=1` nem segít.

## Mit csináltunk (commitok, újabb felül)
| Commit | Mi | Hatás |
|---|---|---|
| 8478aef | `ShaderFunctions.cpp` `ResolveCalls` `recording` olvasó visszaállítása `read`-re | javítja a `88e2835` 2,97 s / 6 s `BuildKey` cache-miss regresszióját (tesztelendő!) |
| 570283e | T3 (`KYTY_HOST_ARCH=x86-64-v3`, arányos VRAM GC `KYTY_VRAM_GC_PROPORTIONAL`) + T4 (`KYTY_POPS_ALLOW`, `KYTY_SHADER_POPS_ZERO_LOAD`, `KYTY_SHADER_POPS_NO_STORE`) + `KYTY_RESOURCE_MEMO` alapból 0 | kevesebb/rövidebb VRAM GC drop; POPS kísérleti kapcsolók készen |
| 4d3987b / 88e2835 | T5 `ExpandShaderFunctions` `ShaderReadChunks` átadás (`KYTY_RESOURCE_MEMO=1` 87% miss miatt visszavonva) | a `ShaderFunctions.cpp` része `8478aef`-ben visszavonva |
| e4a9f11, b9584a8 | T2 2. csomag: upstream `3c4124d` (saturate `NClamp`), `8cc5f53` (wave64 SMEM megosztás, `KYTY_SHADER_WAVE64_SHARE_SMEM`) | shader-kódminőség |
| 8226cf3 (95193a8, f879c99, 0871a86) | T2 1. csomag: upstream `70668fb` (`KYTY_LEGACY_GLOBAL_BARRIERS`), `4bb4be5` (agc log), `75d7a43` (SSA bitcast) | **lopakodós rész állva 18 → 22–27 FPS** (5400 cmdbuf / 5 s) |
| e5fbd3f | `spirvEmitterFlow.cpp` hiányzó `SpirvEmitter.h` include pótlása | `0da13e0` CI-fordítás javítása |
| 0da13e0 | S_MEMREALTIME shift a host órából; AMD indító: POPS-skip be | menü-FPS watchdog nélkül 3–6 → 24 FPS (watchdog=1-gyel 38 FPS) |
| 2952489, 6a8a35a | upstream e87fb8c (mutex FIFO), be9e139 (LOGF out-of-line) | kis CPU-javítás |
| 9c7c24d | indirekt rajzolás-rekordok, watchdog-jelentések a naplóban; watchdog alapból be | diagnosztika, menü-FPS |
| 9da53e3 | IndirectDrawGuard: lehetetlen indirekt számú rajzolások eldobása | védelem; a crasht NEM ez okozta |
| 97475cc… | upstream shader-javítások (inline konstansok, clamp NaN, f16 minmax, 64-bit atomic add) | pontosság |
| d413466 | `KYTY_SKIP_POPS_SHADERS` kapcsoló | **crash megkerülve** |
| de2b5ba | subgroup-fence LDS-hez `s_waitcnt`-nél (`KYTY_SHADER_LDS_WAVE_FENCE=0` kikapcsolja) | pontosság, crashre nincs hatás |
| fd82fab | AMD GPU-markerek (`KYTY_GPU_MARKERS=1`), `KYTY_SHADER_LOOP_WATCHDOG` | diagnosztika: „GPU hang suspect” sor |

Ami nem segített a crashen: eviction/slot reuse ki (Codex), LDS fence, indirekt védő, egyes shaderek egyenkénti kihagyása (mindig újabb jött).
Ami segített: az összes POPS-shader kihagyása (crash), watchdog vagy óra-javítás (menü-FPS).

## Hol mi van a gépen
- Repo (klón): `C:\Users\Ádám Kovács\Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\work\KytyPS5` (`origin` = fork, `upstream` = KytyPS5/KytyPS5). Donor: `...\work\Infamous` (shadPS4-alapú, nem közvetlenül portolható).
- Játék: `C:\Games\Ps5 games\Marvels.Wolverine-PPSA03671-USA-Game` – **soha ne módosítsd.**
- Tesztcsomagok (mindegyikben `emulator\`, `Play Wolverine.bat`, `userdata*\`):
  - `C:\Games\Wolverine-AMD-T3` (570283e): a legfrissebb 236 MB-os `_PipelineCache\PPSA03671.bin` és a 497 MB-os `PPSA03671.programs.0101992f197902bf.bin` (`2623` rekord!) a `userdata-pops\_PipelineCache` alatt van, a lopakodós mentés pedig a `userdata-pops\_SaveData` alatt.
  - `C:\Games\Wolverine-AMD-T2a` (8226cf3), `C:\Games\Wolverine-AMD-T2b` (4d3987b), `C:\Games\Wolverine-AMD-T1` (e5fbd3f).
  - `C:\Games\Wolverine-AMD-Guard2` (9c7c24d). **`Test POPS Skip.bat` = a működő beállítás**, a legfrissebb mentés a `userdata-pops\_SaveData`-ban van (lopakodós rész).
  - `C:\Games\Wolverine-AMD-Guard` (9da53e3), `C:\Games\Wolverine-AMD-HangMarkers` (de2b5ba; shader-dumpok: `userdata-dump\shader-dump`, `userdata-pops\shader-dump`).
  - Codex régi csomagjai: `Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\outputs\*`, `...\new-chat-6\Wolverine-AMD-*`.
- Eszközök: Tracy 0.14.1 `C:\Games\tracy-0.14.1` (`tracy-capture`, `tracy-csvexport`; az emulátor `--profile` kapcsolóval, port 8086). Felvételek: `C:\KytyDiagnostics\tracy` (`stealth_stand.tracy` + `stealth_t3.tracy` + CSV exportok).
  RGD/RDP: `C:\KytyDiagnostics\RDTS\RadeonDeveloperToolSuite-2026-10-02-1872`. A dumpok a `Documents\rgd_dumps` mappába kerülnek; az `rgd.exe` nem kezel ékezetes utat, ezért előbb másold a `C:\KytyDiagnostics\rgd` mappába. Az RDP Crash Analysis módja lassít.
  Vulkan SDK (glslangValidator, spirv-dis): `C:\KytyDiagnostics\VulkanSDK-1.4.363.0\Bin`.
- Naplók: `<csomag>\<userdata>\logs\run_*.log`. A release-ben a `LOGF` néma, csak a `printf` látszik.

## Build és teszt menete
1. Nincs helyi fordító. Push a `wolverine-v2-amd-hang-markers` ágra → a GitHub Actions workflow `.github/workflows/wolverine-hang-markers.yml` (~6–15 perc `sccache`-sel). **FONTOS:** a workflow-ban `cancel-in-progress: true` van (futó CI közben NE pusholj dokumentációt sem), és **SOHA ne indíts háttér-pollingot vagy `schedule` timert a CI-re várva** (kreditet éget; kérd meg a felhasználót, hogy szóljon, ha kész).
   Commit: `git -c user.name=Claude -c user.email=noreply@anthropic.com commit ...` (nincs globális identitás).
2. Állapot és közvetlen API-letöltés (böngésző nélkül!): a `git credential fill`-ből kinyert tokennel (`Authorization = "Bearer $tok"`) közvetlenül letölthető a `https://api.github.com/repos/Kovcsvok/KytyPS5/actions/artifacts/<artifact_id>/zip`.
3. Csomag: tömörítsd ki egy új `C:\Games\Wolverine-AMD-<név>` mappába, másold át a `game_path.txt`-t, a `C:\Games\Wolverine-AMD-T3\userdata-pops\_SaveData`-t és a `C:\Games\Wolverine-AMD-T3\userdata-pops\_PipelineCache\*` fájlokat. Ha az új build nem változtatott a shader-fordításon, a `.bat`-ba írd bele: `set KYTY_PROGRAM_CACHE_GIT_REVISION=4d3987b4baee235d60038112e4e66b17a15eed90`, így a 497 MB-os `PPSA03671.programs.0101992f197902bf.bin` (`2623` shader-rekord!) újrafordítás nélkül betölt!
4. Indítás a felhasználói asztali munkamenetben (Session 3): a sima `Start-Process` a tool-subshell kilépésekor bezárja az ablakot! Mindig Windows Task Schedulerrel indíts szóköztelen `.bat`-on keresztül:
   `schtasks /Create /TN "KytyT1" /TR "C:\Games\Wolverine-AMD-<név>\Run.bat --profile" /SC ONCE /ST 23:59 /RU "$env:USERDOMAIN\$env:USERNAME" /IT /F` majd `schtasks /End /TN "KytyT1"` + `schtasks /Run /TN "KytyT1"`.
6. Hasznos env-ek: `KYTY_GPU_MARKERS=1` (lefagyott rajzolás + shader hash), `KYTY_HOST_BREADCRUMBS=1`, `KYTY_LIVE_SKIP=1` + `<userdata>\skip_live.txt` (shaderek kihagyása hash szerint, újrafordítás nélkül), `KYTY_IR_DUMP_DIR` + `KYTY_DECODED_DUMP_DIR` + `KYTY_IR_DUMP_HASHES` (a dump csak friss program-cache-sel készül, és előre létre kell hozni egy üres `<hash>.txt`-t).
   A shadert érintő env-nevek (SHADER/MESH/…) a program-cache aláírásába kerülnek, ezért saját cache-t kapnak (az első betöltés lassú).

## Szabályok
- A játékfájlokat, a mentéseket és a régi csomagokat ne töröld. Új teszthez új mappát vagy userdata-másolatot használj. A TDR-t (registry) ne emeld.
- Egy CI-buildbe több kis változtatás mehet, de mindegyik legyen kapcsolóval A/B-zhető (env), és ezt írd be a `JOURNAL.md`-be.
