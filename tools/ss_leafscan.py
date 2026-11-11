#!/usr/bin/env python3
"""Save-state continuations the census cannot see: callers of frameless syscall leaves.

A leaf that makes a syscall without a stack frame of its own (`mflr r0; std r0,16(r1); ... sc`, e.g. the
usleep wrapper func_001320AC or the semaphore wait func_006E0830) keeps its caller's return address in LR
or in the CALLER's LR slot [r1+16]. A thread parked in that syscall has the return address on no frame of
the back chain, so the census walker (SS_CHAINS "frame" lines) and its seed (ss_census.py --seed) never
name it -- only the strict walk does, one census at a time ("  strict ... missing="). This scan names
them all at once: every direct `bl T` whose target reaches an `sc` (fall-through and conditional
branches, <= 64 instructions) with no frame made and no other call on the way, or that reaches a `bl` to
an import stub the same way (an HLE park).

  py -3 tools/ss_leafscan.py [--image <EBOOT.elf | raw.bin@0xBASE>] [--code-end 0x...]
                             [--stubs imports.json] [--cont report.txt ...] [--list]

Defaults: the EBOOT of analysis/inputs.json, analysis/code_end.txt, analysis/EBOOT.imports.json, and
generated/ppu/ppu_recomp_cont.txt. Prints each leaf with its callers' return addresses and which of them
have no continuation in the reports; --list prints only the uncovered ones in --cont-list form (for
config/ss_cont_list.txt, section "frameless syscall leaves"). libsre: --image
generated/prx/lift/libsre_image.bin@0x30000000 --code-end 0x3001db18
--stubs generated/prx/lift/libsre_imports.list.json --cont generated/prx/libsre_ns/ppu_recomp_cont.txt.
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_image(spec):
    """-> [(vaddr, bytes)] executable segments."""
    if "@" in spec:
        path, base = spec.rsplit("@", 1)
        return [(int(base, 16), open(path, "rb").read())]
    elf = open(spec, "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 2:
        sys.exit(f"{spec}: not a big-endian ELF64 (decrypted EBOOT.elf expected)")
    phoff, = struct.unpack_from(">Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x36)
    segs = []
    for i in range(phnum):
        ptype, flags, off, vaddr, _pa, filesz, _ms, _al = struct.unpack_from(">IIQQQQQQ", elf, phoff + i * phentsize)
        if ptype == 1 and (flags & 1):
            segs.append((vaddr, elf[off:off + filesz]))
    return segs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--image")
    ap.add_argument("--code-end")
    ap.add_argument("--stubs")
    ap.add_argument("--cont", action="append")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    R = lambda rel: os.path.join(ROOT, rel)
    image = a.image or json.load(open(R("analysis/inputs.json")))["eboot"]
    code_end = int(a.code_end or open(R("analysis/code_end.txt")).read().split()[-1], 16)
    stubs_path = a.stubs or (R("analysis/EBOOT.imports.json") if not a.image else None)
    conts = a.cont or ([R("generated/ppu/ppu_recomp_cont.txt")] if not a.image else [])
    segs = load_image(image)
    stubs = {int(e["stub"], 16) for e in json.load(open(stubs_path))} if stubs_path else set()

    def word(x):
        for v, b in segs:
            if v <= x < v + len(b) - 3:
                return struct.unpack_from(">I", b, x - v)[0]
        return None

    def bl_target(x, w):
        d = w & 0x03FFFFFC
        if d & 0x02000000:
            d -= 0x04000000
        return (x + d) & 0xFFFFFFFF

    def park_in(t):
        seen, todo, n = set(), [t], 0
        while todo and n < 64:
            x = todo.pop()
            while x not in seen and n < 64:
                seen.add(x)
                n += 1
                w = word(x)
                if w is None:
                    break
                op = w >> 26
                if w == 0x44000002:                                  # sc
                    return x
                if op == 18 and (w & 3) == 1 and bl_target(x, w) in stubs:
                    return x                                         # bl import stub (HLE)
                if op in (18, 19):                                   # b / bl / blr / bctr...
                    break
                if (w & 0xFFFF0000) == 0x94210000 or (w & 0xFFFF0003) == 0xF8210001:
                    break                                            # stwu / stdu r1: a real frame
                if op == 16:                                         # bc
                    if w & 1:
                        break
                    d = w & 0xFFFC
                    if d & 0x8000:
                        d -= 0x10000
                    if not (w & 2):
                        todo.append((x + d) & 0xFFFFFFFF)
                x += 4
        return None

    cover = set()
    for p in conts:
        for line in open(p, encoding="utf-8", errors="replace"):
            m = re.match(r"0x([0-9A-Fa-f]{8})\s", line)
            if m:
                cover.add(int(m.group(1), 16))
    memo, callers = {}, collections.defaultdict(list)
    for v, b in segs:
        for x in range(v, min(v + len(b), code_end) - 3, 4):
            w = struct.unpack_from(">I", b, x - v)[0]
            if (w >> 26) == 18 and (w & 3) == 1:
                t = bl_target(x, w)
                if t not in memo:
                    memo[t] = park_in(t)
                if memo[t] is not None:
                    callers[t].append(x + 4)
    total = sum(len(c) for c in callers.values())
    unc = sorted(r for c in callers.values() for r in c if r not in cover)
    if a.list:
        for t in sorted(callers):
            u = [r for r in callers[t] if r not in cover]
            if u:
                print(" ".join(f"0x{r:08X}" for r in sorted(u)) + f"   # leaf 0x{t:08X} (park at 0x{memo[t]:08X})")
        return
    print(f"{len(callers)} frameless park leaves, {total} direct call sites, {len(unc)} with no continuation"
          + (f" in {', '.join(conts)}" if conts else ""))
    for t in sorted(callers, key=lambda t: (-len(callers[t]), t)):
        u = [r for r in callers[t] if r not in cover]
        print(f"  leaf 0x{t:08X} park at 0x{memo[t]:08X}: {len(callers[t])} caller(s), {len(u)} uncovered"
              + (": " + " ".join(f"0x{r:08X}" for r in sorted(u)) if u else ""))


if __name__ == "__main__":
    main()
