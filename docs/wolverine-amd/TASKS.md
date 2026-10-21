# Feladatok (Sonnet / olcsóbb modell)

Minden feladat előtt olvasd el: `HANDOFF.md`, a `JOURNAL.md` utolsó 20 sorát és csak a saját feladatodat. A feladat végén írj 3–6 sort a `JOURNAL.md`-be.
**Opust hívd** (szólj a felhasználónak), ha: egy hipotézis kétszer megbukott; a shader-fordító szemantikáját kellene megváltoztatni (Translate/emitter); új crash jön, és a GPU-marker/RGD kimenetét nem érted; vagy merge előtt a fő ágba.

## T1 – Óra-javítás ellenőrzése (gyors, nincs kódolás)
Build: a `0da13e0` vagy újabb commit artifactja. Indítsd az új csomagot `KYTY_SHADER_LOOP_WATCHDOG=0`-val (bat: `set KYTY_SHADER_LOOP_WATCHDOG=0` + `call "Play Wolverine.bat"`).
A naplóban kell lennie egy `Shader clock: timestamp period ... shift 0` sornak. Kérd meg a felhasználót, hogy álljon a főmenüben 30 mp-ig, és olvasd ki az FPS-t a címsorból.
- 30–40 FPS → a `Play Wolverine.bat`-ban a watchdog legyen alapból 0 (a kapcsoló maradjon). Ezután egy watchdogos futásból gyűjtsd ki a `Loop watchdog:` sorokat (melyik shader túlfut még).
- 3–6 FPS → próbáld a `KYTY_SHADER_CLOCK_SHIFT=1`, majd `2` értéket (saját cache, első betöltés lassú), és írd be az eredményt. Ha egyik sem segít, szólj az Opusnak.

## T2 – Ütköző upstream perf-commitok kézi portolása (sok kódolás, próbálgatás)
Upstream (`git fetch upstream main`) commitok, amelyek a mi águnkon ütköztek: `70668fb` (kevesebb globális barrier PM4 flush/release-nél – **legnagyobb várható nyereség**), `8cc5f53`, `3c4124d`, `75d7a43`, `f6f1ac8`, `4bb4be5`.
Egyenként: `git cherry-pick -x <c>` → oldd fel az ütközést úgy, hogy a mi Wolverine/AMD-változásaink megmaradjanak. A `tests/ShaderRecompilerComputeTests.cpp` ütközéseinél tartsd meg a mi oldalunkat (`--ours`), mert a CI nem fordítja.
Ahol lehet, tedd env-kapcsoló mögé. Egy buildbe legfeljebb 2–3 commit menjen. Teszt: menü-FPS, a lopakodós rész FPS-e állva és forogva, crash ne legyen (`Test POPS Skip` beállítás). Az eredményt commitonként írd a naplóba.

## T3 – liberda-soft/personalstation5 „perf/foundation” átnézése (próbálgatás)
`https://github.com/liberda-soft/personalstation5` (9ac5b94 merge, eee3829: benchmark log, arányos VRAM GC szintek, **x86-64-v3 build**).
Első lépés: CPU-optimalizált build (clang-cl `/arch:AVX2` vagy `-march=x86-64-v3`) a workflow `Configure` lépésében, CMake-kapcsolóval. Mérd: menü és lopakodós rész FPS-e. Utána nézd meg a VRAM GC-t.

## T4 – POPS: ne kelljen kihagyni (kutatás, próbálgatás; az eredményt az Opus értékeli)
Tények: egy POPS-os pixel shaderrel (pl. `0x2464e975d88b7f15`, az NGG mesh `0xfc94abf5b9e0e8e4` rajzolásában) elakad a GPU. Az RGD szerint nincs page fault, és „no information about in-flight shaders”. A shader a kritikus szakaszban `IMAGE_LOAD`/`IMAGE_STORE`-ral ugyanazt a storage image-et írja/olvassa. Dumpok: `C:\Games\Wolverine-AMD-HangMarkers\userdata-dump\shader-dump\2464e975d88b7f15.{rdna,}.txt`.
Kísérletek (egyenként, env-kapcsolóval):
1. `vulkaninfo`-val nézd meg, van-e `VK_EXT_fragment_shader_interlock` az AMD drivernél.
2. Készíts egy `KYTY_POPS_ALLOW=<hash,...>` listát (a `KYTY_SKIP_POPS_SHADERS` mellé), amivel egy-egy POPS-shader mégis kirajzolódik. Ezzel derítsd ki, melyik okozza a crasht, és melyik a piros képet az Omega Red-jelenetnél.
3. A crashelő shaderben próbáld ki env-kapcsolóval: (a) a kritikus szakasz `IMAGE_STORE`-ja helyett no-op, (b) az `IMAGE_LOAD` helyett 0 (SPIR-V emitter, `spirvEmitterImage.cpp` ImageRead/ImageWrite). Az lesz a hiba helye, amelyiktől elmúlik a crash.
4. RDP „Driver Experiments” (Vulkan): DCC tiltása stb., POPS-shaderek bekapcsolva.

## T5 – Játékmenet-profil (próbálgatás)
Indítsd `--profile`-lal (`call "Play Wolverine.bat" --profile`). Amikor a felhasználó a lopakodós részen van:
`tracy-capture -a 127.0.0.1 -p 8086 -o C:\KytyDiagnostics\tracy\stealth.tracy -s 15 -f`, majd `tracy-csvexport -e` → rendezés `total_ns` szerint, top 30.
Menüben (rossz FPS-nél) a parancsfeldolgozó szál ~80%-ban `GuestGpu::WaitForWork`-ben állt (GPU-limit), és volt 600 ezer `MemoryTracker`-fault / 15 mp. Írd le a top hotspotokat, és javasolj 1–2 célzott változtatást.

## T6 – Átvezetők 1 FPS (kutatás)
Ismert: a CPU a GPU label-jére vár. Naplósorok: `Label waits (5 s): suspended/forwarded`. Profilozz egy átvezetőt (mint a T5-ben), és nézd meg, melyik várakozás dominál (`CommandScheduler::Wait`, `WaitRegMem`, `Finish`).
