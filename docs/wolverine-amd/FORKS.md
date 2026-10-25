# Forkok átnézése – mit talált ki már más (2026-10-25)

Cél: amit egy másik fork már megcsinált és megmért, azt ne találjuk ki újra. Ez a fájl a módszer és a jelöltek listája; a döntések a `JOURNAL.md`-be kerülnek.

## Módszer (ismételhető, `docs/wolverine-amd/forkscan/`)

1. `list_forks.py` – a `KytyPS5/KytyPS5` összes forkja, **a forkok forkjai is** (rekurzívan), a 2026-09-20 óta pusholtak minden águval és ágfejével. A hibák a kimenetben maradnak.
2. Egy blobless klón (`D:\KytyAnalysis\forkscan\net.git`, `git clone --bare --filter=blob:none`), a hiányzó ágfejek SHA szerint letöltve az upstream URL-ről (a GitHub fork-hálózata közös), `GIT_NO_LAZY_FETCH=1`-gyel ellenőrizve.
3. `own_commits.py` – ágankként az upstream `main`-ben nem szereplő commitok, mindegyik egyszer, az első tulajdonosához rendelve (szülő a forkja előtt, utána csillagok szerint).
4. `classify.py` – az upstream commitjainak másolatai (azonos tárgysor) és a forkok közötti másolatok nélkül, témák szerint (Materialize/SRT, GPU-szál, visszaolvasás, laphiba, tracker/upload, submit/flush, kötések, zárak, Wolverine, általános perf).

**Miért maradt ki korábban a Senaxx:** a régi szkript a GitHub compare API-val hasonlított, és a hibát adó (túl nagy eltérésű) ágakat csendben átugrotta; a compare legfeljebb 250 commitot ad. A Senaxx 1355 commit előnnyel pont ilyen volt.

Eredmény (2026-10-25): 1244 fork, 1011 aktív, ~3000 ág, 25 395 upstreamben nem szereplő commit, 18 178 egyedi tárgysor 371 forkban.

**Családfa:** a mi forkunk (`Kovcsvok/KytyPS5`) szülője az `IDXTRI/KytyPS5`, annak a `Senaxx/KytyPS5`; a kódunk közös pontja a Senaxx-szel viszont csak az upstream `f53e5d22` (2026-10-03), azóta erősen eltér (pl. `pipelineCache.cpp` +1750/−1219 sor). Az átvétel tehát kézi portolás, nem cherry-pick.

## Jelöltek a mi szűk keresztmetszeteinkre

A mi mostani képünk (harc, ~15 fps): a GPU-szál a plafon (~55 ms / képkocka, ebből `MaterializeResources` ~18 ms, submit ~9 ms, saját visszaolvasások ~8 ms); a vendégszálak ~130 GPU-írt olvasása / képkocka ~1,25 ms-mal.

### 1. Draw prep: a descriptor-walk a GPU-szál előtt, külön szálon (Materialize ~18 ms)
- **Senaxx `6632a241`** (2026-10-09, +1407 sor, `drawPrepScanner.cpp`, `drawPrep.cpp`): egy scanner szál a sorba állított submissionöket saját command processorral követi, és a már ismert shaderpárok SRT-walkjait előre lefuttatja egy csak GPU-tiszta bájtokat olvasó olvasóval; a GPU-szál a kész eredményt veszi át, ha a program, a user data, a shader base és a dispatch-méret azonos, és minden olvasott tartomány még tiszta és változatlan, különben walkol. `KYTY_DRAW_PREP` (alap 1), `KYTY_DRAW_PREP_VERIFY`. 86% walk kiszolgálva, 0 eltérés.
- **Revi (`Revivedx`) `73ffc2ee56`** – ugyanennek a portja a Wolverine-fork resource memójára: **Wolverine dzsungel, 5800X3D + 7900 XT: 22,98 → 25,03 fps, command processor 37,8 → 33,8 ms**, 98% találat; „heavy fire combat stays bound elsewhere”.
- Javítások hozzá: `usa-ratt/KytyPS5-GT7` Round 70–112 (szemét-regiszterek, COND_EXEC, minden SH-regiszter ellenőrzése, walk worker szálakon), `Cozmaaa` `b86d39e2`, `7f14d909`, `77aa7474`, `mn3ema/KytyPS5-Frank` `ed26428d`.
- Korábbi, nagyobb vonal: `Xx-LiDAF-xX` draw-prep S1–S6 (2026-09-26–10-12: párhuzamos előkészítés, tanúsítványok, `KYTY_DRAW_PREP_*`).
- Rokon: Revi `4615c71626` (−12% CP: walk plan, trace pass aliasing, **parallel materialize +4,8%**, mesh-indirekt előkonverzió), Revi `5f533fcbaf` (a forrásokat nem befolyásoló walk-feltételek kihagyása), Senaxx `816198d2` (vertex és pixel walk párhuzamosan), tarek369 `KYTY_GPU_FAN` (`453295df`), NeySlim SRT-sorozat (`3c0abccd`, `ae525bb4`, `d165c5a9` – SRT-szavak olvasása GPU-írás mellett flush nélkül, ez a mi ~2,8 ms „slow reads”-ünk).

### 2. Vendégolvasások visszaolvasás nélkül (vendégszálak ~160 ms szálidő / képkocka)
- **mionstyle7 `a507e89f9d`** (+165 sor): a játékmenet 15–50 ezer vendég-visszaolvasásának **~78%-a olyan bájt volt, amit a GPU sosem írt** (írási bitmap szerint); ezeket a hibázó szál maga szolgálja ki a vendégmemóriából, a GPU-szál nélkül (`KYTY_BITMAP_CLEAN_READ`). Ráépül: `6e62f48b`, `5f4d93d5`, `c837d857`, `3524eff9`. A mi `READBACK_STATS` mérésünk ugyanezt mutatta (8 GB letöltés / 5 s-ból ~2 MB változás).
- Írási bitmap: Senaxx `df053d75` (`KYTY_WRITE_BITMAP=1`, +361), mionstyle7 `0b55d9b1`, `270bba48`, Frank `a2d4a97d`. **Shaderkódot érint** (SPIR-V írásjelölés) → program-cache újraépül.
- **mionstyle7 `09735d84ba`**: a képkockánként újraolvasott ablakok előre letöltése a submission végén (`KYTY_READBACK_PREDICT`), +181 sor.
- **mionstyle7 `b1d32f5f88`**: a vendégszál maga olvassa vissza a GPU-írt bájtokat a mellék-sorról, GPU-szál nélkül (`KYTY_DIRECT_READBACK`), +305 sor. (Nálunk a readback-queue hasonló, de két GPU-szál-kört tesz.)
- mionstyle7 `3ffda604`, `62c59d59`: írás GPU-tiszta bájtokra letöltés nélkül; RMW-tárolások emulálása.
- Revi `31cf25b7` és AndrewsK30 `00932f1e`: vendégmemóriára importált pufferek (`VK_EXT_external_memory_host`, `KYTY_HOST_IMPORT`), Revi szerint üresjáratban +8%, kevesebb írási laphiba.

### 3. Feltöltés / tracker
- **Senaxx `6625a63e`** (+114 sor, csak `memoryTracker.h`): a csak olvasó szinkron csak a CPU-piszkos régiókat járja be (a játék 2,7 GB-os V#-eket köt rajzolásonként többször; 1,22 M szinkron/s). `KYTY_TRACKER_DIRTY_WALK`. Nálunk `MemoryTracker::Upload` ~30 ezer hívás / képkocka.
- mionstyle7 `70ba4756` (CPU-írt lap slice-onként egyszer feltöltve), Revi `c3bf5829` (feltöltés worker szálon).

### 4. Submit / flush (~9 ms / képkocka)
- ClGratton `5f944f84`: `vkQueueSubmit` és a feltöltési fence saját szálon (a GPU-szál 5%-a ment a fence-re).
- mionstyle7 `eae13930`: a RELEASE_MEM megszakítások 2 ms-on belül egy submitba kötegelve; upstream `dc40478e`: megszakításonként nincs külön flush. Nálunk `CpOpReleaseMem` minden megszakításnál `BufferFlush()`; a címkéknél már van kötegelés (`KYTY_LABEL_FLUSH_US=500`).
- AndrewsK30 `163d6905` (`KYTY_ASYNC_SUBMIT`), `1b632c25`.

### 5. Wolverine-forkok, amelyeket követni kell
- **`todojuegos67-bot/KytyPS5`** („the Wolverine fork”, 1627 saját commit, napi munka): Wolverine TURBO (copy-queue aszinkron visszaolvasás `KYTY_ASYNC_COPY_QUEUE`, `KYTY_READBACK_HISTORY` kötegelés, FSR 3 képkocka-generálás `KYTY_FSR3_FG`, `KYTY_OP_GPU_TIME`).
- **`Revivedx/KytyPS5`** (Revi, Linux + AMD/RADV): a fenti draw prep, parallel materialize, host import, harci visszaolvasás compute-soron (`2194426f`).
- `mionstyle7/KytyPS5`, `mn3ema/KytyPS5-Frank`, `AndrewsK30/KytyPS5`, `TheCruZ/KytyPS5-GTA`, `usa-ratt/KytyPS5-GT7`, `chenxiao07/KytyPS5` (283 csillag, főleg memória/RAM és DLSS).

## Javasolt sorrend
1. **Draw prep scanner** (Senaxx `6632a241` + usa-ratt/Cozmaaa javítások), `KYTY_DRAW_PREP` élő kapcsolóval, `KYTY_DRAW_PREP_VERIFY`-jal ellenőrizve. A GPU-szál most a plafon, és ez a legnagyobb tétele; Wolverine-mérés van rá.
2. **Írási bitmap + bitmapos tiszta olvasás** (Senaxx `df053d75` / mionstyle7 `a507e89f`): a vendégszálak várakozására; shaderkódot érint.
3. Kicsi és független: **Senaxx `6625a63e`** tracker dirty walk.
4. Utána mérés szerint: parallel materialize (Revi), readback-előrejelzés (mionstyle7), submit szál (ClGratton), RELEASE_MEM-kötegelés.

## Átvételi terv: írási bitmap + bitmapos tiszta olvasás (2026-10-25)

**Mit csinál a forrás.** Senaxx `df053d75` (+361 sor, `KYTY_WRITE_BITMAP=1`): ha egy shader olyan kötésen ír, amelynek írt tartományát a gazdagép nem tudja behatárolni (és a kötés ≥ `KYTY_WRITE_BITMAP_MIN_KB`, alap 1 MiB), a shader minden tárolása/atomikus művelete beállítja a 4 KiB-os lapja bitjét egy 32 MiB-os bitmapben (host-koherens eszközmemória, 1 TiB alatti címek). Visszaolvasáskor (`RefineWithWriteBitmap`) a bit nélküli lapok letöltés nélkül kikerülnek a GPU-írt halmazból. A mionstyle7 erre épülő javításai kötelezők: `270bba48` (a bitmap BAR-on nem gyorsítótárazott, szavanként egyszer olvasandó – nélküle a GPU-szál 41%-a ment rá), `0b55d9b1` (tárolás előtt bitteszt, nélküle egy compute shader 78 ms lett az atomikus ütközéstől), `3524eff9` (a futó írásokra nem várni). A vendégszálas kiszolgálás: `a507e89f` + `6e62f48b` + `5f4d93d5` + `c837d857` (`IsBitmapCleanForConcurrentRead` a meglévő tiszta-olvasási útban, a lap feloldása a vendégszálon, epoch-védelemmel).

**Mi van már nálunk.** Korlátos írás (`KYTY_BOUNDED_BUFFER_WRITES=1`, `ComputeBufferWriteExtents`, `ObtainBufferWritten`): ahol az írt tartomány kiszámítható, csak az lesz GPU-írt. A bitmap csak a maradék, korlát nélküli írásokra hat. Vendégszálas tiszta olvasás: `RenderContext::CanServeCleanRead` → `BufferCache::IsCleanForConcurrentRead`; ide köthető be a bitmapos feltétel.

**Lépések.**
0. **Mérés, build nélkül:** 20–30 s `KYTY_READBACK_STATS=1` harcban (drága, csak rövid ablakra): pufferenként írók (shader-hash), legnagyobb kötés, letöltött vs. ténylegesen változott bájtok. Döntési szabály: ha a visszaolvasási idő zöme olyan pufferekre esik, amelyek írója korlát nélküli nagy kötés, és a letöltések nagy része nem változtat bájtot, jöhet az 1. lépés; ha a korlátos írások dominálnak, a bitmap keveset hoz.
1. **Build (shaderkódot érint):** Senaxx `df053d75` + mionstyle7 `270bba48`, `0b55d9b1`, `3524eff9`, a mi `NativeStorageBuffer` / `MarkGpuWritten` / `ReadMemoryOnGpu` / readback-queue utunkra igazítva; `KYTY_WRITE_BITMAP` élő kapcsolóval (bekapcsoláskor új permutációk fordulnak). A `ResourceSpecialization` és a `BindingLayout` új mezői miatt a program-lemezcache formátumát emelni kell → egyszeri teljes shader-újrafordítás (első indítás lassú); ennél a buildnél a `KYTY_PROGRAM_CACHE_GIT_REVISION` rögzítése nem marad.
2. **Build:** bitmapos tiszta olvasás a vendégszálon (`a507e89f` + 3 javítás) a `CanServeCleanRead`-be, `KYTY_BITMAP_CLEAN_READ` élő kapcsolóval.
Mindkét buildnél automatikus harci A/B.
