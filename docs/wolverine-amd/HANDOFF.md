# Wolverine on KytyPS5 – AMD (RX 9070 XT) munka: átadó

Gép: Windows 11, i5-14600KF, RX 9070 XT 16 GB, Adrenalin 26.9.2. Játék: Marvel's Wolverine PPSA03671 01.001.005.
Ág: `wolverine-v2-amd-hang-markers` a `Kovcsvok/KytyPS5` forkban. Előtte a Codex AMD-ágai (mesh/pixel wave, depth feedback).
Napló: `JOURNAL.md` (mindenki ide ír, röviden). Feladatok: `TASKS.md`. Promptok: `PROMPTS.md`.

## Állapot (2026-10-21)
- **Crash (combat, ráugrás/ütés/átvezető):** megkerülve. `KYTY_SKIP_POPS_SHADERS=1` (az AMD indító alapból bekapcsolja) kihagyja a POPS-ot használó pixel shadereket (azokat, amelyek `pops_exiting_wave_id`-t olvasnak). Így a játék a lopakodós részig crash nélkül ment. Ára: a vér, egyes ütéseffektek és az Omega Red-jelenet egyik effektje hiányzik (piros kép). A POPS-lefagyás pontos oka még ismeretlen.
- **Menü 3–6 FPS → 30–40:** az ok az `S_MEMREALTIME` óra rossz skálázása AMD-n (fix `>>3`, NVIDIA-ra hangolva). Emiatt a játék GPU-s várakozó shadere (`0x7c5c0785877c0999`) 8× tovább várt. Javítás: `0da13e0` (a shift a `timestampPeriod`-ból jön, AMD-n 0). **Hardveren még nincs ellenőrizve.** Addig a loop watchdog (`KYTY_SHADER_LOOP_WATCHDOG=1`) is be van kapcsolva; ez a 4096. iterációnál vágja a ciklusokat, és ez is 30–40 FPS-t ad.
- **Játékmenet:** állva 18, forgolódva 10–11 FPS (lopakodós rész). Átvezetők 1 FPS (régi ismert hiba: a CPU a GPU-ra vár).

## Mit csináltunk (commitok, újabb felül)
| Commit | Mi | Hatás |
|---|---|---|
| 0da13e0 | S_MEMREALTIME shift a host órából; AMD indító: POPS-skip be | menü-FPS fix (ellenőrizendő) |
| 2952489, 6a8a35a | upstream e87fb8c (mutex FIFO), be9e139 (LOGF out-of-line) | kis CPU-javítás, nem mérve |
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
  - `C:\Games\Wolverine-AMD-Guard2` (9c7c24d). **`Test POPS Skip.bat` = a működő beállítás**, a legfrissebb mentés a `userdata-pops\_SaveData`-ban van (lopakodós rész).
  - `C:\Games\Wolverine-AMD-Guard` (9da53e3), `C:\Games\Wolverine-AMD-HangMarkers` (de2b5ba; shader-dumpok: `userdata-dump\shader-dump`, `userdata-pops\shader-dump`).
  - Codex régi csomagjai: `Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\outputs\*`, `...\new-chat-6\Wolverine-AMD-*`.
- Eszközök: Tracy 0.14.1 `C:\Games\tracy-0.14.1` (`tracy-capture`, `tracy-csvexport`; az emulátor `--profile` kapcsolóval, port 8086). Felvételek: `C:\KytyDiagnostics\tracy`.
  RGD/RDP: `C:\KytyDiagnostics\RDTS\RadeonDeveloperToolSuite-2026-10-02-1872`. A dumpok a `Documents\rgd_dumps` mappába kerülnek; az `rgd.exe` nem kezel ékezetes utat, ezért előbb másold a `C:\KytyDiagnostics\rgd` mappába. Az RDP Crash Analysis módja lassít.
  Vulkan SDK (glslangValidator, spirv-dis): `C:\KytyDiagnostics\VulkanSDK-1.4.363.0\Bin`.
- Naplók: `<csomag>\<userdata>\logs\run_*.log`. A release-ben a `LOGF` néma, csak a `printf` látszik.

## Build és teszt menete
1. Nincs helyi fordító. Push a `wolverine-v2-amd-hang-markers` ágra → a GitHub Actions workflow `.github/workflows/wolverine-hang-markers.yml` (~30–35 perc). Új ághoz kell egy saját workflow-másolat, és az első build cache nélkül ~1 óra.
   Commit: `git -c user.name=Claude -c user.email=noreply@anthropic.com commit ...` (nincs globális identitás).
2. Állapot: `curl -s https://api.github.com/repos/Kovcsvok/KytyPS5/actions/runs?branch=<ág>&per_page=1`. Artifact: `.../runs/<id>/artifacts` (név, méret, `digest`).
3. Letöltés: a beépített böngészőpanelben (a felhasználó be van lépve a GitHubra) navigálj ide: `https://github.com/Kovcsvok/KytyPS5/actions/runs/<run>/artifacts/<id>`. A fájl a `Downloads\<guid>.tmp` alatt jelenik meg; az SHA256-ot vesd össze a `digest`-tel.
4. Csomag: tömörítsd ki egy új `C:\Games\Wolverine-AMD-<név>` mappába, másold át a `game_path.txt`-t, a `userdata\_SaveData`-t és a `_PipelineCache\PPSA03671.bin`-t. A `*.programs.*.bin`-t ne másold: build-függő.
5. Indítás: `Start-Process cmd.exe '/c "<csomag>\<bat>"'`. FPS: `(Get-Process kyty_emulator).MainWindowTitle` (a címsor végén `fps: N`). A játékot a felhasználó irányítja.
6. Hasznos env-ek: `KYTY_GPU_MARKERS=1` (lefagyott rajzolás + shader hash), `KYTY_HOST_BREADCRUMBS=1`, `KYTY_LIVE_SKIP=1` + `<userdata>\skip_live.txt` (shaderek kihagyása hash szerint, újrafordítás nélkül), `KYTY_IR_DUMP_DIR` + `KYTY_DECODED_DUMP_DIR` + `KYTY_IR_DUMP_HASHES` (a dump csak friss program-cache-sel készül, és előre létre kell hozni egy üres `<hash>.txt`-t).
   A shadert érintő env-nevek (SHADER/MESH/…) a program-cache aláírásába kerülnek, ezért saját cache-t kapnak (az első betöltés lassú).

## Szabályok
- A játékfájlokat, a mentéseket és a régi csomagokat ne töröld. Új teszthez új mappát vagy userdata-másolatot használj. A TDR-t (registry) ne emeld.
- Egy CI-buildbe több kis változtatás mehet, de mindegyik legyen kapcsolóval A/B-zhető (env), és ezt írd be a `JOURNAL.md`-be.
