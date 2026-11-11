#!/usr/bin/env python3
"""Extract WWS SPU job modules embedded (uncompressed) in a decrypted EBOOT.

A module is `4 ila | entryOffset | imageSize | 0 | 0 | 0xC0DEC0DE | 0 | 0 | n`
(the same header ps3recomp's tools/extract_jobmods.py looks for inside SDATA
archives). inFamous 2 links its job modules straight into the EBOOT, so they
can be cut out directly -- no runtime capture needed.

Output: <out>/jobmod_<sha1[:12]>_e<entryOffset>.bin, the name format
tools/lift_jobmods.py expects.
    python tools/extract_eboot_jobs.py analysis/inputs.json --out analysis/jobmods
"""
import argparse, glob, hashlib, json, os, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument("eboot", help="EBOOT.elf, or analysis/inputs.json")
ap.add_argument("--out", default="analysis/jobmods")
a = ap.parse_args()
path = a.eboot
if path.endswith(".json"):
    path = json.load(open(path))["eboot"]
d = open(path, "rb").read()
os.makedirs(a.out, exist_ok=True)
# Only this EBOOT's modules: another build's (title update 1.04 rebuilt 16 of the 27) would be lifted and
# registered beside them.
for old in glob.glob(os.path.join(a.out, "jobmod_*.bin")):
    os.remove(old)
n = i = 0
while (i := d.find(b"\xC0\xDE\xC0\xDE", i)) >= 0:
    st = i - 0x20
    i += 4
    if st < 0 or st % 4:
        continue
    # the first four words must be `ila` (opcode 0x21, top 7 bits = 0b0100001)
    if any((struct.unpack_from(">I", d, st + k)[0] >> 25) != 0x21 for k in range(0, 16, 4)):
        continue
    entry, size = struct.unpack_from(">II", d, st + 0x10)
    if not (0x30 <= size <= 0x40000 and entry < size):
        continue
    raw = d[st:st + size]
    name = f"jobmod_{hashlib.sha1(raw).hexdigest()[:12]}_e{entry:x}.bin"
    open(os.path.join(a.out, name), "wb").write(raw)
    n += 1
    print(f"  0x{st:07X}  size={size:6}  entry=+0x{entry:X}  -> {name}")
print(f"{n} job module(s) -> {a.out}")
