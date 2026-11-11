#!/usr/bin/env python3
"""Print the --code-end bound for ppu_lifter.py: the end of the last executable
section of a PS3 PPU ELF (hex).

Why it matters: the R-X segment of a PS3 EBOOT also carries .rodata after the
code. Without a bound, the lifter's branch-target pass promotes bytes in that
data to "functions", and lifts explode to multi-GB (this is what cost the flOw
and You Don't Know Jack ports days). The .lib.stub import trampolines sit at the
end of the code and MUST stay inside the bound, or --hle-stubs has nothing to
rewrite -- this script includes them by construction.

If section headers are stripped, falls back to the end of the executable
PT_LOAD and says so on stderr; review that value by hand in Ghidra.
"""
import struct
import sys

SHF_EXECINSTR = 0x4
PF_X = 0x1


def main(path: str) -> int:
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
        print("not a 64-bit big-endian ELF", file=sys.stderr)
        return 1
    phoff, shoff = struct.unpack_from(">QQ", d, 0x20)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from(">HHHHH", d, 0x36)

    ends = []
    for i in range(shnum if shoff else 0):
        o = shoff + i * shentsize
        _name, _type, flags, addr, _off, size = struct.unpack_from(">IIQQQQ", d, o)
        if flags & SHF_EXECINSTR and size:
            ends.append(addr + size)
    if ends:
        print(hex(max(ends)))
        return 0

    for i in range(phnum):
        o = phoff + i * phentsize
        p_type, p_flags, _off, vaddr, _pa, _filesz, memsz = struct.unpack_from(">IIQQQQQ", d, o)
        if p_type == 1 and p_flags & PF_X:
            ends.append(vaddr + memsz)
    if not ends:
        print("no executable section or segment found", file=sys.stderr)
        return 1
    print("warning: no section headers; using end of executable PT_LOAD -- this "
          "includes .rodata, tighten it by hand", file=sys.stderr)
    print(hex(max(ends)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
