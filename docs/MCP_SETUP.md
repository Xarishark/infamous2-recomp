# Tooling & MCP setup

Static recompilation is mostly *reading machine code and figuring out why the
lifted version diverged*. These are the tools worth installing, roughly in the
order you will need them.

## Required

| Tool | Why |
|---|---|
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) | The toolkit: loader, PPU/SPU lifters, HLE runtime, RSX → D3D12. Fetched by `tools/setup.*`, pinned in `ps3recomp.pin`. |
| [RPCS3](https://rpcs3.net) | Decrypts the EBOOT, and is the **behavioural oracle**: when the recomp does something different, run the same moment in RPCS3 with logging and compare. |
| Python 3.10+, CMake 3.20+, Ninja | Pipeline + build. |
| Windows: VS 2022 Build Tools + LLVM (`clang-cl`) | `clang-cl` is what the existing ports build with. Configure from an *x64 Native Tools* prompt. |
| Git Bash (Windows) | Runs `tools/*.sh`. |

## Strongly recommended: Ghidra + an MCP bridge

[Ghidra](https://ghidra-sre.org) handles the PPU as language
`PowerPC:BE:64:64-32addr` (64-bit, big-endian, 32-bit addressing — the id
ps3recomp's `ghidra_analyze.py` uses; pick the same when importing by hand).
Two uses here:

1. **Better function boundaries.** `GHIDRA_DIR=/path/to/ghidra tools/analyze.sh`
   runs ps3recomp's headless export; `relift.sh` then seeds the lift with
   Ghidra's call-graph-recovered starts and attaches its names as comments.
2. **Interactive RE through Claude.** An MCP bridge lets an assistant decompile,
   rename and cross-reference inside your Ghidra project directly, which is
   most of the bring-up loop ("why does this function spin on `0x00A31C40`?").

Two maintained bridges:

- **[bethington/ghidra-mcp](https://github.com/bethington/ghidra-mcp)** — large
  tool surface (scripts, projects, batch renames), Ghidra 12.x, frequent releases.
  **This is the one the setup script installs.**
- **[LaurieWired/GhidraMCP](https://github.com/LaurieWired/GhidraMCP)** — the
  original; smaller and simpler.

### Automated setup (Windows)

```bat
powershell -ExecutionPolicy Bypass -File tools\setup_ghidra_mcp.ps1
```

It installs, under `%LOCALAPPDATA%\re-tools\`: the latest bethington/ghidra-mcp
release, **the exact Ghidra version that plugin was built for** (Ghidra rejects
extensions built for any other version), a Temurin JDK 21 if you lack one, and
the bridge in a private Python venv. Downloads are SHA-256 checked. It then adds
a `ghidra` server to Claude Desktop's config (old file backed up) and to this
repo's `.mcp.json` for Claude Code. Needs Python 3.10+ on PATH. Safe to re-run;
re-running also upgrades.

Then, once, inside Ghidra:

1. Launch `%LOCALAPPDATA%\re-tools\Ghidra.cmd`, create a project, import a binary.
2. In CodeBrowser: *File → Configure → Configure All Plugins*, tick **GhidraMCP**.
3. *Tools → GhidraMCP → Start MCP Server* (port 8089).
4. Fully quit and reopen Claude Desktop. The ghidra tools only answer while
   Ghidra and its server are running.

The bridge runs on *your* PC, so it works in Claude Desktop and Claude Code on
that machine — not in a claude.ai cloud session, which cannot reach your localhost.

### Manual setup

1. Install JDK 21 and the Ghidra version named in the plugin release notes.
2. Ghidra → *File → Install Extensions → +* → the release's `GhidraMCP-*.zip` → restart.
3. `pip install` the release's `ghidra_mcp_bridge-*.whl` (into a venv).
4. Copy `.mcp.json.example` to `.mcp.json` and fix the path to `bridge-mcp-ghidra.exe`.

Never commit the Ghidra project, exported JSON or decompiled C: they are
derived from Sony/Sucker Punch code. `analysis/` is git-ignored for that reason.

## Useful references

- RPCS3 source (`rpcs3/Emu/Cell/Modules/*`) — the ground truth for what each
  firmware function does; ps3recomp's HLE libraries are modelled on it.
- [psdevwiki](https://www.psdevwiki.com/ps3/) — SELF/SFO/PKG formats, lv2 syscalls.
- The other ps3recomp ports' `docs/` folders, especially
  [tokyojungle](https://github.com/sp00nznet/tokyojungle) (first 3D title to reach
  menus) and [vf5](https://github.com/sp00nznet/vf5) (cleanest pipeline script).
