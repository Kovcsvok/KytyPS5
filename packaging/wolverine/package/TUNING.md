# Wolverine v2 - i5-14600KF / RX 9070 XT tesztprofilok

Ez a tesztverzio tartalmazza az AMD depth-feedback javitast. A profilok teljesitmenye es
jatekbeli stabilitasa meg nincs igazolva. Inditas: `Play Wolverine - Tuning.bat`.
Az eredeti `Play Wolverine.bat` az alapbeallitasokkal tovabbra is hasznalhato.

## Meresi sorrend

1. Eloszor az 1-es profillal ellenorizd, hogy tovabbjutsz-e a korabbi osszeomlas helyen.
2. Minden profilhoz azonos mentes es 60-90 masodperces utvonal kell, azonos ablakmerettel.
3. Elso kor: cache-epites. Utana ugyanazzal a profillal harom meresi kor kovetkezzen.
4. Hasonlitsd ossze az 1-es es 2-es profilt, majd az 1-est a 3-assal es 4-essel.
5. Az 5-os profil kulon kiserlet: csak az aszinkron shaderforditast kapcsolja be.
6. A 6-os profil diagnosztikara valo. Az extra meres es naplozas befolyasolhatja az FPS-t;
   a vegleges teljesitmenyeredmenyekhez a diagnosztika legyen kikapcsolva.

Figyeld az atlag-FPS mellett az akadasokat, a kesve megjeleno objektumokat es a hibakat is.
A shadercache-eket ne torold a meresek kozott. A beallitasoktol fuggo cache-alairas miatt
egy uj profil elso kore ismet forditassal jarhat. Mas GPU vagy driver cache-et ne masold be.
A menteseid maradjanak biztonsagi masolatban; a tesztcsomag kulon konyvtarban hasznalhato.

## Implementalt kapcsolok

- `KYTY_PIPELINE_THREADS`: 0 vagy hiany = eredeti automatikus beallitas (14600KF-en 4),
  explicit 1-8 szal. A noveles nem feltetlenul gyorsit; a jatek CPU-szalaitol is elvehet idot.
- `KYTY_PIPELINE_WAIT_MS`: hiany = 20 ms, 0-100 ms megengedett. Az 5 ms-os profil
  hamarabb elengedi azokat a rajzolasokat, amelyeket a meglev� kod biztonsagosan halaszthat.
  A memoriairasos es egyeb kotelezo muveletek szinkron utvonala valtozatlan.
- A lemezes shadercache es a rendereloszal megmarad. A fordito zarja bekapcsolva marad.

## Kovetkezo fejlesztes: betolteskori elokeszites

A teljes pipeline-elokeszites meg nincs implementalva ebben a valtozatban.
A GTA-fork program- es pipeline-tarolojanak atultetese kulon fejlesztes: futasokon at
ervenyes azonositas, verzio/GPU/driver/beallitas ellenorzes, hatterforditas es leallas kell.
Nem eleg a Vulkan cache-fajlt betolteni, mert a pipeline-leirokat es shader-valtozatokat is
ujra elo kell allitani. A mostani beallitasmeres es crash-teszt adja ennek az alapjat.

A P/E magokra iranyitas szinten kesobbi, kulon A/B teszt: valos CPU-topologia alapjan,
nem rogzitett processzor-affinitasi maszkkal. Az E-magokat globalisan nem tiltjuk le.
