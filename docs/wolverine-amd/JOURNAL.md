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
