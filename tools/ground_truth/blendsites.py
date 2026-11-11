"""Census of the title's ROP/blend state writes in the lifted EBOOT (T3 evidence).

Stdlib only, read-only. Reads generated/ppu/ppu_recomp_*.cpp (the lifter's
output) and, for every call to the libgcm inline helpers that write blend
methods, prints the enclosing function, the call site and the immediates
loaded into r4..r7 just before it (straight-line tracking of li/lis/ori;
'?' = not an immediate there).

Helpers, identified from their bodies (ppu_recomp_002.cpp):
  func_005014F0  SET_BLEND_ENABLE 0x310 = m & 1, SET_BLEND_ENABLE_MRT 0x36C = m & 0xE
  func_00501570  SET_BLEND_FUNC_SFACTOR/DFACTOR 0x314/0x318 = s<<16|s, d<<16|d
  func_005015F0  the same with separate alpha factors (r6, r7)
  func_00501680  SET_BLEND_EQUATION 0x320 = eq<<16 | eq
  func_00501700  SET_BLEND_COLOR 0x31C = r4
  func_005041C0  the generic set-method (r4 = method, r5 = value)

Usage: py -I tools/ground_truth/blendsites.py generated/ppu [--summary]
"""
import collections
import os
import re
import sys

HELPERS = {
    "func_005014F0": "BLEND_ENABLE(0x310)=m&1 + BLEND_ENABLE_MRT(0x36C)=m&0xE",
    "func_00501570": "BLEND_FUNC(0x314/0x318)",
    "func_005015F0": "BLEND_FUNC_SEPARATE(0x314/0x318)",
    "func_00501680": "BLEND_EQUATION(0x320)",
    "func_00501700": "BLEND_COLOR(0x31C)",
}
SETMETHOD = "func_005041C0"
METHODS = {0x304: "ALPHA_TEST_ENABLE", 0x308: "ALPHA_FUNC", 0x30C: "ALPHA_REF",
           0x310: "BLEND_ENABLE", 0x314: "BLEND_SFACTOR", 0x318: "BLEND_DFACTOR",
           0x31C: "BLEND_COLOR", 0x320: "BLEND_EQUATION", 0x324: "COLOR_MASK",
           0x36C: "BLEND_ENABLE_MRT", 0x370: "COLOR_MASK_MRT", 0x37C: "BLEND_COLOR2",
           0x1FEC: "SHADER_PACKER", 0x300: "DITHER_ENABLE"}

RE_FUNC = re.compile(r"^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{")
RE_LI = re.compile(r"ctx->gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\((-?(?:0x[0-9A-Fa-f]+|\d+))\);")
RE_LIS = re.compile(r"ctx->gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\(\(uint32_t\)(0x[0-9A-Fa-f]+|\d+) << 16\);")
RE_ORI = re.compile(r"ctx->gpr\[(\d+)\] = ctx->gpr\[(\d+)\] \| (0x[0-9A-Fa-f]+|\d+);")
RE_CALL = re.compile(r"ctx->lr = (0x[0-9A-F]+); (func_[0-9A-F]{8})\(ctx\);")
RE_WRITE = re.compile(r"ctx->gpr\[(\d+)\] =")


def imm(s):
    return int(s, 0) & 0xFFFFFFFF


def hx(v):
    return "?" if v is None else "0x%X" % v


def scan(path):
    rows = []
    cur, regs = None, {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = RE_FUNC.match(ln)
            if m:
                cur, regs = m.group(1), {}
                continue
            if ln.startswith("loc_"):          # a branch target: drop what we knew
                regs = {}
                continue
            m = RE_LI.search(ln)
            if m:
                regs[int(m.group(1))] = imm(m.group(2))
                continue
            m = RE_LIS.search(ln)
            if m:
                regs[int(m.group(1))] = (imm(m.group(2)) << 16) & 0xFFFFFFFF
                continue
            m = RE_ORI.search(ln)
            if m:
                d, s = int(m.group(1)), int(m.group(2))
                if s in regs:
                    regs[d] = regs[s] | imm(m.group(3))
                else:
                    regs.pop(d, None)
                continue
            m = RE_CALL.search(ln)
            if m:
                site, fn = int(m.group(1), 16) - 4, m.group(2)
                r = [regs.get(i) for i in (4, 5, 6, 7)]
                if fn in HELPERS:
                    if fn == "func_00501680":
                        val = "eq=" + hx(None if r[0] is None else ((r[0] & 0xFFFF) << 16) | (r[0] & 0xFFFF))
                    elif fn == "func_00501570":
                        val = "s=%s d=%s" % (hx(r[0]), hx(r[1]))
                    elif fn == "func_005015F0":
                        val = "s=%s d=%s sA=%s dA=%s" % tuple(hx(v) for v in r)
                    else:
                        val = "r4=" + hx(r[0])
                    rows.append((cur, site, HELPERS[fn], val))
                elif fn == SETMETHOD and r[0] in METHODS:
                    rows.append((cur, site, "SETMETHOD " + METHODS[r[0]], "value=" + hx(r[1])))
                regs = {}
                continue
            m = RE_WRITE.search(ln)
            if m:
                regs.pop(int(m.group(1)), None)
    return rows


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    rows = []
    for fn in sorted(os.listdir(d)):
        if fn.startswith("ppu_recomp_") and fn.endswith(".cpp") and "cont" not in fn:
            rows += scan(os.path.join(d, fn))
    if "--summary" in sys.argv:
        c = collections.Counter((what, val) for _, _, what, val in rows)
        for (what, val), n in sorted(c.items()):
            print("%4d  %-58s %s" % (n, what, val))
    else:
        for func, site, what, val in rows:
            print("%s  call@0x%06X  %-58s %s" % (func, site, what, val))
    return 0


if __name__ == "__main__":
    sys.exit(main())
