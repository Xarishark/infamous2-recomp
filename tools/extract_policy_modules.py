#!/usr/bin/env python3
"""Extract inFamous 2's four SPURS policy modules from YOUR files, without running the game.

The SPURS kernel DMAs a workload's policy module into SPU local store at 0xA00 and runs it
(tools/lift_policy.py lifts them). They were first captured at run time (tools/run.ps1 -DumpSpu,
SPU_DUMP_OVL=<dir>,0xA00), but every one of them is a plain copy of bytes that sit in a file you
already have:

    the game's Wws_Job module   in the EBOOT being lifted   (disc 0x887080, update 1.04 0x8E5B00)
    three libsre modules        in libsre at its load base  (generated/prx/libsre.linked.bin,
                                                              written by tools/lift_libsre.sh)

so this writes the same analysis/pm/ovl_<EA>.bin files a -DumpSpu run would, from the EBOOT named
in analysis/inputs.json and the relocated libsre. The EBOOT's module is named after its disc EBOOT
address (ovl_00887080.bin, as captured on the disc build) whichever EBOOT it is read from:
tools/lift_policy.py marks the module with that name as the WWS job manager (--wws-ea) and finds
its bytes in the EBOOT being lifted itself. Each module is checked against the SHA-1 of the
known-good capture; a mismatch is reported and that module is left out (the runtime then
interprets it, and a -DumpSpu run can still capture it).

    python tools/extract_policy_modules.py [--out analysis/pm]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIBSRE_BASE = 0x30000000      # tools/lift_libsre.sh BASE (src/stubs.cpp LIBSRE_BASE)

# (EA by EBOOT build, size, SHA-1 of the module's bytes)
EBOOT_MODULES = [
    ({"": 0x00887080, "01.04": 0x008E5B00}, 0x2E80, "50dc1c2cb3f84e03ee2f62f56c203f34b5bbdad4"),
]
LIBSRE_MODULES = [
    (0x30021480, 0x2200, "f41f5ec45be04d84f9d279af5fcfd478542b0240"),
    (0x30023680, 0x1E40, "dd993498889ae02aa471e3bfaa0623d104b67ff4"),
    (0x3002A180, 0x3E80, "0b30c7042a95059084c3e460817c7f0a5e68306b"),
]


def elf_read(path: Path, ea: int, size: int) -> bytes | None:
    """size bytes at guest address ea from the file-backed part of a PT_LOAD segment."""
    d = path.read_bytes()
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phentsize, phnum = struct.unpack_from(">HH", d, 0x36)
    for i in range(phnum):
        p_type, _fl, off, va, _pa, fsz, _msz, _al = struct.unpack_from(">IIQQQQQQ", d, phoff + i * phentsize)
        if p_type == 1 and va <= ea and ea + size <= va + fsz:
            return d[off + (ea - va):off + (ea - va) + size]
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--inputs", type=Path, default=ROOT / "analysis" / "inputs.json")
    ap.add_argument("--libsre", type=Path, default=ROOT / "generated" / "prx" / "libsre.linked.bin")
    ap.add_argument("--out", type=Path, default=ROOT / "analysis" / "pm")
    args = ap.parse_args()

    inputs = json.loads(args.inputs.read_text())
    eboot = Path(inputs["eboot"])
    if not eboot.is_absolute():
        eboot = ROOT / eboot
    ver = inputs.get("update_app_ver") or ""
    args.out.mkdir(parents=True, exist_ok=True)
    ok = bad = 0

    def put(ea: int, blob: bytes | None, sha: str, what: str, name_ea: int | None = None) -> None:
        nonlocal ok, bad
        if blob is None:
            print(f"[x] {what}: 0x{ea:08X} is outside the file -- left out")
            bad += 1
            return
        if hashlib.sha1(blob).hexdigest() != sha:
            print(f"[x] {what}: the bytes at 0x{ea:08X} are not the known module -- left out")
            bad += 1
            return
        dst = args.out / f"ovl_{(ea if name_ea is None else name_ea):08X}.bin"
        dst.write_bytes(blob)
        print(f"[ok] {what}: {dst.relative_to(ROOT) if dst.is_relative_to(ROOT) else dst} ({len(blob)} bytes)")
        ok += 1

    for eas, size, sha in EBOOT_MODULES:
        ea = eas.get(ver)
        if ea is None:
            print(f"[x] EBOOT APP_VER {ver or '01.00'}: no known address for its policy module -- left out")
            bad += 1
            continue
        put(ea, elf_read(eboot, ea, size), sha, "EBOOT Wws_Job module", name_ea=eas[""])

    if args.libsre.exists():
        lib = args.libsre.read_bytes()
        for ea, size, sha in LIBSRE_MODULES:
            o = ea - LIBSRE_BASE
            put(ea, lib[o:o + size] if o + size <= len(lib) else None, sha, "libsre module")
    else:
        print(f"[x] {args.libsre} not found -- run tools/lift_libsre.sh first; libsre's modules left out")
        bad += len(LIBSRE_MODULES)

    print(f"{ok} policy module(s) written, {bad} left out")
    if bad:
        sha = inputs.get("eboot_sha1", "?")
        print(f"[x] the policy modules are taken from known places in the BCUS98125 EBOOTs (disc 1.00 and update "
              f"1.04); this EBOOT (title {inputs.get('title_id', '?')}, SHA-1 {sha}) does not hold them there, so "
              f"it is not one of those builds. This port supports only the US edition, BCUS98125, with update "
              f"1.04 -- tools/check_inputs.py says so unless IF2_ALLOW_UNKNOWN_EBOOT=1 was set. For the European "
              f"languages on a US copy: PC Settings > System > Language edition: Europe.")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
