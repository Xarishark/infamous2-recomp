# Roadmap

Milestones follow the order every ps3recomp port has actually gone through.
Each one has an exit test you can check from a log or a screenshot, not a feeling.

Honest framing: the ps3recomp porting guide puts a "complex AAA title" at
**months to years**. inFamous 2 is a first-party, SPU-heavy, streaming open-world
game — much harder than anything currently running on the toolkit (the most
advanced 3D port, Tokyo Jungle, reaches its menus). Expect M4 and M7 to be
where most of the time goes.

| | Milestone | Exit test |
|---|---|---|
| **M0** | Inputs | `tools/check_inputs.py` passes on a decrypted **update** EBOOT. |
| **M1** | Analysis | `analysis/import_coverage.md` exists; function count, SPU image count and code-end recorded in `docs/NOTES.md`. |
| **M2** | Lift & link | `tools/relift.sh` + `cmake --build` produce `infamous2.exe`. |
| **M3** | Boot to main loop | CRT, TLS, static constructors run; `cellSysutilCheckCallback` polled every frame; `cellGcmInit` succeeds; window opens and flips. |
| **M4** | SPU / SPURS | Every SPU image the title hands SPURS is lifted and registered — no `[spu] unrecognised image` lines in a 10-minute run. |
| **M5** | First pixels | Sony / Sucker Punch logos and the title screen drawn by the game's own shaders (`--live-draw` and default backend both tried). |
| **M6** | Menus, input, audio, saves | Main menu navigable on a pad; music plays; a save is created and reloaded. |
| **M7** | In game | Load into New Marais; Cole moves; HUD draws. Then: first mission completes. |
| **M8** | Playable | Story start to end on one save, no crashes. |
| **M9** | Native extras | Resolution scaling, uncapped/60 fps (needs game-logic timing review), KB+M, widescreen FOV. |

## M1 — what to look at first

- **Import count and libraries.** VF5 (107 imports, no `cellSpurs`) linked on
  the first try; flOw (PhyreEngine) and Yakuza needed months of runtime work.
  Expect inFamous 2 to import `cellSpurs`, `cellSail` (FMVs), `sceNp*`, `cellGem`.
- **Function count.** Expect tens of thousands; anything > 150k after
  `find_functions` means `--code-end` is wrong or data is being lifted.
- **Static SPU images** vs. SPURS jobs loaded from data files (see M4).
- **Title update.** Lift the newest EBOOT you have; record its APP_VER and SHA-1
  (from `analysis/inputs.json`) in `docs/NOTES.md` so lifts are reproducible.

## M3 — known traps (from other ports)

- `unresolved indirect call -> 0x39800000` → lifted without `--hle-stubs`, or
  the HLE NID table is missing. Both are handled by `relift.sh` + CMake; if you
  see it, something drifted.
- Guest pointers used as host pointers in an HLE call → crash at a low address.
  The fix belongs upstream in ps3recomp.
- A guest thread spinning forever on a sync word: check endianness of the
  primitive first (`CellSyncMutex` was this exact bug for YDKJ).
- `sys_ppu_thread_once` style "returns OK but did nothing" HLE stubs — check
  the import coverage report before chasing a lifter bug.
- Cold-cache timing races: confirm a regression with a second run.

## M4 — SPU strategy

1. Lift the static images (`analyze.sh` extracts them, `relift.sh` lifts them).
2. Run with `tools/run.ps1 -DumpSpu` (`--dump-spu` on bash). The runtime writes
   every SPURS job binary it cannot match to `analysis/spu_runtime/`. Those
   are raw code+data blobs, not ELFs: wrap them with
   `external/ps3recomp/tools/wrap_spu_elf.py`, drop them in
   `analysis/spu_images/`, re-run `relift.sh`.
3. For a job whose purpose is identifiable (audio mix, decompression, skinning),
   a host-side replacement through the SPU fallback registry
   (`external/ps3recomp/docs/SPU_FALLBACK.md`) can be faster and simpler than
   lifting it.

## Rules of the road

- **Fix the toolkit, not the generated code.** If the lift is wrong, the fix
  goes into ps3recomp (and upstream as a PR). `generated/` is disposable.
- Title-specific behaviour goes in `src/infamous2_hooks.cpp`, each with a
  comment saying what needed it and how it was confirmed.
- Every bring-up session ends with a dated entry in `docs/NOTES.md`: what you
  tried, the log line it ended on, the next suspect.
