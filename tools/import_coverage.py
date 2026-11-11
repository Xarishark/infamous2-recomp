#!/usr/bin/env python3
"""How much of inFamous 2's OS surface does the pinned ps3recomp already answer?

Static-recomp cost is set by the title's firmware import table far more than by
binary size (see the VF5 port's README). This reads the import list that
ppu_loader.py wrote (analysis/EBOOT.imports.json), resolves every NID to a name,
and checks it against what the toolkit implements: the HLE library functions
gen_hle_nids.py registers, plus the ctx handlers the PPU boot scaffold registers
directly (runtime/ppu/*.cpp). Anything left over is an unimplemented import --
on first call it returns a faked CELL_OK, which is how ports silently diverge.

Writes analysis/import_coverage.md (and prints a summary).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Modules that historically gate PS3 ports: flag them loudly when imported.
HOT_MODULES = {
    "cellSpurs": "SPURS job/task pipeline -- the long pole on most ports",
    "cellSpursJq": "SPURS job queues",
    "cellSail": "movie player -- partial in ps3recomp (no real decode yet)",
    "cellVdec": "video decode -- callbacks only, needs FFmpeg",
    "cellAdec": "audio decode -- callbacks only",
    "cellGem": "PS Move -- stubbed with no device; the game must take its pad path",
    "sceNp": "PSN -- offline paths must be taken cleanly",
    "sceNp2": "PSN matchmaking",
    "sys_net": "networking (UGC level sharing in inFamous 2)",
}


def implemented_nids(sdk: Path) -> set[int]:
    sys.path.insert(0, str(sdk / "tools"))
    import gen_hle_nids  # noqa: E402  (from the pinned toolkit)
    from nid_database import compute_nid  # noqa: E402

    nids: set[int] = set()
    for mod in gen_hle_nids.discover_all():
        for sub in ("system", "filesystem", "input", "audio", "video", "network",
                    "spurs", "sync", "codec", "font", "misc", "hardware"):
            p = sdk / "libs" / sub / f"{mod}.c"
            if p.exists():
                nids.update(n for n, _ in gen_hle_nids.scan(str(p)))
    reg = re.compile(r'ps3_hle_register_ctx\(\s*(?:ps3_compute_nid\("(\w+)"\)|(0x[0-9A-Fa-f]+)u?)')
    for p in (sdk / "runtime").rglob("*.cpp"):
        for name, hexnid in reg.findall(p.read_text(encoding="latin1")):
            nids.add(compute_nid(name) if name else int(hexnid, 16))
    return nids


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--imports", type=Path, default=ROOT / "analysis" / "EBOOT.imports.json")
    ap.add_argument("--sdk", type=Path, default=ROOT / "external" / "ps3recomp")
    ap.add_argument("--out", type=Path, default=ROOT / "analysis" / "import_coverage.md")
    args = ap.parse_args()

    if not args.imports.exists():
        print(f"[x] {args.imports} missing -- run tools/analyze.sh first")
        return 1
    imports = json.loads(args.imports.read_text())

    sys.path.insert(0, str(args.sdk / "tools"))
    from nid_database import get_default_db  # noqa: E402
    db = get_default_db()
    db.load_implemented(str(args.sdk))
    have = implemented_nids(args.sdk)

    by_lib: dict[str, list[tuple[int, str, bool]]] = defaultdict(list)
    for imp in imports:
        nid = imp["nid"] if isinstance(imp["nid"], int) else int(str(imp["nid"]), 16)
        hit = db.lookup_nid(nid)
        name = hit[1] if hit else f"<unknown 0x{nid:08X}>"
        by_lib[imp.get("library", "?")].append((nid, name, nid in have))

    total = sum(len(v) for v in by_lib.values())
    done = sum(1 for v in by_lib.values() for _, _, ok in v if ok)
    lines = ["# inFamous 2 -- firmware import coverage", "",
             f"**{done} / {total}** imported functions ({100 * done // max(total, 1)}%) "
             f"across **{len(by_lib)}** libraries have a ps3recomp handler.", ""]
    hot = [lib for lib in by_lib if lib in HOT_MODULES]
    if hot:
        lines += ["## Libraries that usually gate a port", ""]
        lines += [f"- `{lib}` ({len(by_lib[lib])} imports) -- {HOT_MODULES[lib]}" for lib in hot]
        lines.append("")
    lines += ["## By library", "", "| Library | Imports | Implemented | Missing |",
              "|---|---:|---:|---|"]
    for lib in sorted(by_lib, key=lambda k: -len(by_lib[k])):
        rows = by_lib[lib]
        missing = [n for _, n, ok in rows if not ok]
        miss = ", ".join(f"`{m}`" for m in missing[:12]) + (" ..." if len(missing) > 12 else "")
        lines.append(f"| {lib} | {len(rows)} | {len(rows) - len(missing)} | {miss or '-'} |")

    args.out.parent.mkdir(exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")
    print(f"[i] {done}/{total} imports covered across {len(by_lib)} libraries")
    for lib in hot:
        print(f"[!] imports {lib}: {HOT_MODULES[lib]}")
    print(f"[ok] wrote {args.out.relative_to(ROOT) if args.out.is_relative_to(ROOT) else args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
