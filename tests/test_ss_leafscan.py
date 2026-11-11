#!/usr/bin/env python3
"""tools/ss_leafscan.py on a synthetic raw image: which callers' return sites it names.

  leaf A  mflr r0; std r0,16(r1); li r11,141; beq +8; nop; sc; ld r0,16(r1); mtlr r0; blr
          (frameless, LR in the caller's slot, a conditional branch on the way)   -> its callers listed
  leaf B  stdu r1,-0x70(r1); ...; sc          (makes a frame: the walker sees it) -> not listed
  leaf C  mflr r0; bl D; ...                  (calls something else first)        -> not listed
  leaf E  mflr r0; std r0,16(r1); bl <stub>   (HLE park in a frameless wrapper)   -> listed with --stubs
  callers: 2 of A (one already in the --cont report), 1 of B, 1 of C, 1 of E.
Run: py -3 tests/test_ss_leafscan.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "ss_leafscan.py")
BASE = 0x00100000
SC, BLR, NOP, MFLR, STD_LR, LD_LR, MTLR = 0x44000002, 0x4E800020, 0x60000000, 0x7C0802A6, 0xF8010010, 0xE8010010, 0x7C0803A6


def bl(at, to):
    return 0x48000001 | ((to - at) & 0x03FFFFFC)


def main():
    w = {}
    A, B, C, D, E, STUB = BASE + 0x100, BASE + 0x200, BASE + 0x300, BASE + 0x380, BASE + 0x400, BASE + 0x800
    for k, v in enumerate([MFLR, STD_LR, 0x3960008D, 0x41820008, NOP, SC, LD_LR, MTLR, BLR]):
        w[A + 4 * k] = v
    for k, v in enumerate([0xF821FF91, MFLR, 0x3960008D, SC, BLR]):        # stdu r1,-0x70(r1)
        w[B + 4 * k] = v
    w[C], w[C + 4], w[C + 8] = MFLR, bl(C + 4, D), SC
    w[D] = BLR
    w[E], w[E + 4], w[E + 8], w[E + 12], w[E + 16] = MFLR, STD_LR, bl(E + 8, STUB), LD_LR, BLR
    w[STUB] = 0x4E800420                                                   # bctr
    callers = {BASE + 0x1000: A, BASE + 0x1010: A, BASE + 0x1020: B, BASE + 0x1030: C, BASE + 0x1040: E}
    for at, to in callers.items():
        w[at] = bl(at, to)
    img = bytearray(0x2000)
    for a, v in w.items():
        struct.pack_into(">I", img, a - BASE, v)
    fails = []
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "img.bin")
        open(raw, "wb").write(img)
        rep = os.path.join(d, "cont.txt")
        open(rep, "w").write(f"0x{BASE + 0x1014:08X}  func_x+0x14  after bl 0x{A:08X}  returns\n")
        stubs = os.path.join(d, "stubs.json")
        json.dump([{"library": "x", "nid": "0x1", "stub": hex(STUB)}], open(stubs, "w"))

        def run(*extra):
            r = subprocess.run([sys.executable, "-B", TOOL, "--image", f"{raw}@0x{BASE:08X}",
                                "--code-end", hex(BASE + len(img)), "--cont", rep] + list(extra),
                               capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

        rc, out = run("--list")
        print(out)
        got = {int(t, 16) for line in out.splitlines() for t in line.split("#")[0].split()}
        if rc != 0 or got != {BASE + 0x1004}:
            fails.append(f"--list without stubs: rc={rc} got {sorted(hex(x) for x in got)}")
        rc, out = run("--list", "--stubs", stubs)
        got = {int(t, 16) for line in out.splitlines() for t in line.split("#")[0].split()}
        if rc != 0 or got != {BASE + 0x1004, BASE + 0x1044}:
            fails.append(f"--list with stubs: rc={rc} got {sorted(hex(x) for x in got)}")
        rc, out = run("--stubs", stubs)
        print(out)
        if "2 frameless park leaves, 3 direct call sites, 2 with no continuation" not in out:
            fails.append("summary line wrong")
    for f in fails:
        print("FAIL:", f)
    print("PASS" if not fails else f"FAIL ({len(fails)})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
