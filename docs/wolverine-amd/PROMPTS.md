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
- CI-buildből kevesebb legyen: több kis, env-kapcsolós változtatást tegyél egy buildbe. A CI-ra háttérben várj.
- Kérdezz a felhasználótól, ha játék közbeni megfigyelés kell (FPS, crash helye). Gombokkal, röviden.
A végén írj 3–6 sort a JOURNAL.md-be (dátum, mit csináltál, eredmény, commit, következő lépés), majd commitolj és pusholj.
Szólj a felhasználónak, hogy hívja az Opust, ha: két hipotézis megbukott, shader-fordító szemantikát kell változtatni,
új crash jött, és nem érted a marker/RGD kimenetet, vagy merge előtt vagy.
```

## Opus – új chat indítása
```
KytyPS5 Wolverine AMD-munka folytatása (Opusként: döntés, ellenőrzés, nehéz hibakeresés). Magyarul, tömören.
Repo: C:\Users\Ádám Kovács\Documents\Codex\2026-10-16\el-z-gpt-le-r-sa-2\work\KytyPS5, ág: wolverine-v2-amd-hang-markers.
Olvasd el: docs\wolverine-amd\HANDOFF.md és a docs\wolverine-amd\JOURNAL.md bejegyzéseit a legutóbbi Opus-bejegyzés óta.
Az alábbit kell ellenőrizned/eldöntened: <ide írd, mit kért a Sonnet, vagy mi a gond>.
Ne olvass végig nagy fájlokat, és ne fedezd fel újra a HANDOFF tartalmát. Az eredményt írd a JOURNAL.md-be.
```
