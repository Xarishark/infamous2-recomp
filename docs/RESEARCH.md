# State of PS3 static recompilation (October 2026)

Snapshot of what was found when this project was started, so the choice of
toolchain is documented.

## The toolkit: ps3recomp

[sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp) (MIT) is the only
active PS3 static-recompilation toolkit. It lifts PPU (PowerPC 64) and SPU code
ahead of time to C/C++, links it against clean-room HLE replacements for the PS3
OS libraries (~97 modules), and translates RSX command streams to Direct3D 12
(Metal in progress, Vulkan planned). Same philosophy as N64Recomp,
XenonRecomp/UnleashedRecomp and ReXGlue on the Xbox 360 side.

Platform reality (from its README, Sept 2026): **only Windows runs a recompiled
game with real rendering**. macOS renders through Metal but cannot boot a title
yet; Linux has a headless CI backend only. (This repo's verification build ran
the toolkit's smoke ELF through the boot scaffold on Linux, headless.)

## Ports on it, and how far they got

| Title | Furthest point |
|---|---|
| The Simpsons Arcade Game | Playable — stage 1 plays through |
| You Don't Know Jack | Gameplay — questions on screen with scoring |
| Tokyo Jungle | Menus at ~30 fps — first 3D title attempted |
| Virtua Fighter 5 | Renders loading/CRIWARE screens at 58 fps |
| flOw | Assets load, app loop runs, ~2.8k draws |
| Twisted Metal | Renderer works end to end; boots real game code |
| Yakuza: Dead Souls, Demon's Souls, LittleBigPlanet | Community forks driving toolkit fixes |

The lesson every port repeats: cost is driven by the **import table**
(how much of the PS3 OS the game touches) and by **SPU/SPURS usage**, not by
binary size. A title with no `cellSpurs` (VF5) linked first try; titles built
on SPURS jobs spend most of their time there.

## Where inFamous 2 sits

- Sucker Punch's own engine, 2011, streaming open world, PS Move support,
  user-generated-content mode (its online service is gone).
- First-party late-generation PS3 titles lean hard on SPUs. Expect `cellSpurs`
  job chains for animation, physics, culling and audio, many of them loaded
  from data files rather than embedded in the EBOOT.
- In RPCS3 it has historically been "ingame" with heavy SPU cost and a
  reported crash in a late mission (forum reports 2017–2019). Check the current
  RPCS3 compatibility entry before relying on it as an oracle for a given scene.
- No public recomp/decomp of any inFamous game existed when this was started.

That puts it in the guide's hardest bracket. Nothing about it is impossible
with this toolkit; it is long.

## Other tools considered

- **RPCS3** — not a recompiler, but required: decryption and behavioural oracle.
- **Ghidra + MCP bridges** (bethington/ghidra-mcp, LaurieWired/GhidraMCP) — for
  function-boundary seeding and assistant-driven RE. See `MCP_SETUP.md`.
- **XenonRecomp / ReXGlue** — Xbox 360 only; inFamous 2 was PS3-exclusive, so
  there is no 360 build to recompile instead.
