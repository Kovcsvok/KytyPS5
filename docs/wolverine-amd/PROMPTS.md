# Promptok

## Sonnet / olcsóbb modell – munkamenet indítása
```
KytyPS5 Wolverine AMD-munkát folytatsz. Válaszolj magyarul, röviden.
Repo: C:\Users\Ádám Kovács\Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\work\KytyPS5, ág: wolverine-v2-amd-hang-markers.
Előbb olvasd el: docs\wolverine-amd\HANDOFF.md, a docs\wolverine-amd\JOURNAL.md utolsó 20 sorát,
és a docs\wolverine-amd\TASKS.md-ből CSAK ezt: <T#>. Utána csináld meg a feladatot.
Tokenspórolás:
- Nagy fájlt/naplót ne olvass egészben: grep -n, majd sed -n 'a,bp'. A naplóból csak a kulcssorok kellenek
  (Shader clock, Loop watchdog, AMD GPU markers, GPU hang suspect, Indirect draw, POPS:, Device fault).
- Ne fedezd fel újra, ami a HANDOFF-ban van. Ne listázz könyvtárfákat feleslegesen.
- CI-buildből kevesebb legyen: több kis, env-kapcsolós változtatást tegyél egy buildbe. A CI-ra SOHA ne várj
  háttér-pollinggal vagy timerrel: kérd meg a felhasználót, hogy szóljon, ha kész. Futó CI alatt ne pusholj.
- Kérdezz a felhasználótól, ha játék közbeni megfigyelés kell (FPS, crash helye). Gombokkal, röviden.
A végén írj 3–6 sort a JOURNAL.md-be (dátum, mit csináltál, eredmény, commit, következő lépés), majd commitolj és pusholj.
Szólj a felhasználónak, hogy hívja az Opust, ha: két hipotézis megbukott, shader-fordító szemantikát kell változtatni,
új crash jött, és nem érted a marker/RGD kimenetet, vagy merge előtt vagy.
```

## Opus – új chat indítása (2026-10-23, kész, bemásolható)
```
KytyPS5 Wolverine AMD folytatás (Opus). Magyarul, tömören, tokentakarékosan.
Repo: C:\Users\Ádám Kovács\Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\work\KytyPS5, ág wolverine-v2-amd-hang-markers.
Olvasd: docs\wolverine-amd\HANDOFF.md „Állapot (2026-10-23)” + JOURNAL.md utolsó ~60 sora. Ne fedezd fel újra, ami ott van.
Teendők: 1) A 7ce53a1 CI artifactját (ha kész) tedd a C:\Games\Wolverine-AMD-Async\emulator helyére, a felhasználó teszteli
harcban: Play Wolverine AMD DGC.bat + DGC Draws 0/1.bat; elemezd a naplót. 2) Utána T12c: visszaolvasási késleltetés
(tartományonkénti írási tick + közvetlen BAR / copy-queue olvasás GPU-várás nélkül). Cél: stabil 30 FPS a Trask-harcban.
Szabályok: CI-re soha ne pollingolj, kérd meg a felhasználót, hogy szóljon; futó CI alatt ne pusholj kódot; több kódváltozást
egy pushba; minden változás env/élő kapcsolóval, JOURNAL-bejegyzéssel; KYTY_PROGRAM_CACHE_GIT_REVISION csak shader-fordítást
nem érintő buildnél; törlés csak lomtárba; játékmappához ne nyúlj.
```

## Opus – új chat indítása (2026-10-22, korábbi)
```
KytyPS5 Wolverine AMD-munka folytatása (Opusként: döntés, ellenőrzés, nehéz hibakeresés). Magyarul, tömören, tokentakarékosan.
Gép: Win11, i5-14600KF, RX 9070 XT (Adrenalin 26.9.2). Nincs helyi fordító: push → GitHub Actions → artifact letöltése API-val.
Repo: C:\Users\Ádám Kovács\Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\work\KytyPS5, ág: wolverine-v2-amd-hang-markers.
Olvasd el: docs\wolverine-amd\HANDOFF.md (az elején az „Állapot (2026-10-22)” rész), a TASKS.md T7 és T8 részét,
és a JOURNAL.md utolsó ~40 sorát. Ne fedezd fel újra, ami ott le van írva.
Jelenleg: C:\Games\Wolverine-AMD-Play\Play Wolverine AMD.bat (95572c5) játszható, nincs crash, mozgás 19–26 FPS.
A POPS vér-shaderek bindless képírása el van dobva (KYTY_SHADER_BINDLESS_STORAGE_WRITES=0, az alapértelmezés).
Nyitott feladatok, ebben a sorrendben:
1) T8 árnyék-villogás (az árnyékok gyorsan eltűnnek/visszajönnek; KYTY_KEEP_GPU_IMAGES=1 nem segít, fekete foltot okoz).
2) Teljesítmény: a GPU-szál CPU-limitált (Tracy: C:\KytyDiagnostics\tracy\lunge_t3b.tracy; szinkron visszaolvasások
   a BufferCache::ReadMemory-ban, FindBuffers-csúcs ráugráskor, első használatú programbetöltés).
3) T7 teljes vér: bármely valódi bindless kép írása fagy AMD-n (storage tömbökkel és GENERAL layouttal is),
   az 1x1 placeholderbe nem. Következő lépés: RGD Hardware Crash Analysis a KYTY_SHADER_BINDLESS_STORAGE_WRITES=2 beállítással.
Szabályok: SOHA ne használj háttér-pollingot vagy timert a CI-re várva, kérd meg a felhasználót, hogy szóljon.
Futó CI alatt ne pusholj. Minden változás env-kapcsolóval A/B-zhető legyen, és írd be a JOURNAL.md-be.
KYTY_PROGRAM_CACHE_GIT_REVISION-t csak akkor használj, ha a build nem változtat a shader-fordításon.
```
