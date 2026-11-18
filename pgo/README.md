# Profile-guided optimization

The Windows build can be optimized with a clang profile of Marvel's Wolverine (PPSA03671) combat:
the compiler then inlines and lays out the code the emulator really runs hot (mostly the GPU
thread). It changes no behaviour. CMake: `KYTY_PGO_GENERATE` / `KYTY_PGO_USE` (top-level
`CMakeLists.txt`, after Senaxx `f617aa53`, which follows Jetsku's fork).

## Training round

1. `pgo/GENERATE` exists: CI builds the instrumented emulator (slower; the package installs as
   usual). `Play Wolverine AMD Async.bat` sets `KYTY_PGO_FILE=D:\KytyAnalysis\pgo\wolverine-<n>.profraw`;
   `src/main.cpp` writes the counters there every 20 s (`KYTY_PGO_DUMP_SECONDS`).
2. Play combat (Trask) and some traversal for 5-10 minutes.
3. Delete `pgo/GENERATE`, put the `.profraw` files (zipped) under `pgo/train/`, push. CI merges them
   with the `llvm-profdata` of its own clang (raw profiles need the same LLVM version), builds with
   the result and uploads it as `zz-pgo-profdata`.
4. Commit that `PPSA03671.profdata` here and delete `pgo/train/`: later builds use it.

## When it goes stale

Functions whose code changed lose their profile (clang warns, the build still works); the rest
keep it. Retrain after large changes to the GPU thread's hot paths. The profile is tied to the CI
source path (`D:\a\KytyPS5\KytyPS5`): clang-cl names functions in anonymous namespaces after a
hash of the source file's path.
