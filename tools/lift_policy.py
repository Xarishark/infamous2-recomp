#!/usr/bin/env python3
"""Lift SPURS policy modules captured at runtime and register them as overlays.

The SPURS kernel (libsre SPU image, lifted as image 41) DMAs the selected
workload's policy module into local store at 0xA00 and enters it with
`bisl $r0, 0xA00`. Policy modules (system service, taskset, the game's Wws_Job
job manager, ...) are raw SPU code, not ELFs, so nothing lifts them statically.

Capture them with the runtime's overlay dumper:
    SPU_DUMP_OVL=<dir>,0xA00   ->  <dir>/ovl_<source EA>.bin
(tools/run.ps1 -DumpSpu sets it). Then:
    python tools/lift_policy.py --dumps analysis/pm --out generated/spu_pm

Each module is wrapped as an SPU ELF at base 0xA00, lifted with its own symbol
prefix, and registered under a distinct image id with its first 16 bytes as the
content signature, so the runtime switches to it whenever those bytes are
DMA'd into LS (same mechanism as tools/lift_jobmods.py, different base).

Source EAs belong to the EBOOT the dumps were taken from. A module that lives in
the EBOOT (the game's Wws_Job PM) and the task ELFs move in another build of it
(title update 1.04: 0x887080 -> 0x8E5B00, 0x7AB900 -> 0x80A100), so both are
located by CONTENT in the EBOOT being lifted (analysis/inputs.json): a dump
taken on one build registers correctly on the other. libsre's modules sit at
its fixed load base and are kept as they are.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBSRE_LO = 0x30000000


def eboot_find(elf, blob):
    """Every guest EA where blob lies in the ELF's file-backed segments."""
    out = []
    for va, _end, off, fsz, _fl in elf.segs:
        seg = elf.d[off:off + fsz]
        i = seg.find(blob)
        while i >= 0:
            out.append(va + i)
            i = seg.find(blob, i + 1)
    return out


def relocate(elf, ea, blob, what):
    """ea if the lifted EBOOT holds blob there, else the one place it does (or ea, with a warning)."""
    if elf is None or ea >= LIBSRE_LO:
        return ea
    s = elf.seg_of(ea)
    if s and elf.d[s[2] + (ea - s[0]):s[2] + (ea - s[0]) + len(blob)] == blob:
        return ea
    hits = eboot_find(elf, blob)
    if len(hits) == 1:
        print(f"  {what}: 0x{ea:08X} in the dump's EBOOT is 0x{hits[0]:08X} in this one (same bytes)")
        return hits[0]
    print(f"  WARNING {what}: 0x{ea:08X} not found in this EBOOT ({len(hits)} matches) -- registered as is")
    return ea


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumps", default=os.path.join(ROOT, "analysis", "pm"))
    ap.add_argument("--out", default=os.path.join(ROOT, "generated", "spu_pm"))
    ap.add_argument("--sdk", default=os.path.join(ROOT, "external", "ps3recomp"))
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0xA00)
    # Ids are 8-bit in the dispatch key. Game images 1.., libsre 41.., jobmods
    # 200.. (lift_jobmods.py) -> policy modules 120..
    ap.add_argument("--image-base", type=int, default=120)
    # Source EA of the game's Wws_Job policy module (cellSpurs AddWorkload pm=);
    # 0x887080 for BCUS98125 1.00. Marks it for the runtime's WWS job dispatch.
    ap.add_argument("--wws-ea", type=lambda s: int(s, 0), default=0x887080)
    # libsre's taskset PM (source EA at the 0x30000000 libsre base), and the
    # SPURS task ELFs it loads: EA of the task ELF in the EBOOT = lifted image id
    # (tools/relift.sh numbers the EBOOT's SPU images 1.. by file offset).
    # 0x7AB900 (file 0x79B900, spu_0000) is the psarc/zlib decompressor task the
    # FIOS media threads queue (cellSpursCreateTask entry=0x7AB900).
    ap.add_argument("--taskset-ea", type=lambda s: int(s, 0), default=0x30023680)
    ap.add_argument("--task", action="append", default=None, metavar="EA=IMAGE",
                    help="task ELF EA and its lifted image id (repeatable; default: the "
                         "EBOOT's first SPU image, image 1, found by content)")
    ap.add_argument("--eboot", default=os.path.join(ROOT, "analysis", "inputs.json"),
                    help="the EBOOT being lifted (or analysis/inputs.json): EBOOT-resident "
                         "modules and task ELFs are located in it by content")
    ap.add_argument("--images", default=os.path.join(ROOT, "analysis", "spu_images"))
    args = ap.parse_args()

    sys.path.insert(0, os.path.join(args.sdk, "tools"))
    from wrap_spu_elf import wrap  # noqa: E402
    from build_spu_workloads import debase_includes  # noqa: E402
    from eboot_map import Elf  # noqa: E402
    lifter = os.path.join(args.sdk, "tools", "spu_lifter.py")
    eboot = args.eboot
    if eboot.endswith(".json") and os.path.exists(eboot):
        eboot = json.load(open(eboot)).get("eboot", "")
    elf = Elf(eboot) if eboot and os.path.exists(eboot) else None
    if elf is None:
        print(f"  (no EBOOT at {eboot!r}: source EAs are registered as dumped)")

    dumps = sorted(glob.glob(os.path.join(args.dumps, "*.bin")))
    if not dumps:
        print(f"no *.bin in {args.dumps} -- run with SPU_DUMP_OVL=<dir>,0xA00 first")
        return 1
    os.makedirs(args.out, exist_ok=True)
    # The lifter takes --reg-cache's default from SPU_LIFT_RCACHE. Register
    # caching is verified for spu_jobs/spu_jobchain only, never the policy
    # modules: force it off here, whatever the caller's shell exports (relift.sh
    # and lift_libsre.sh do the same). spu_pm is lifted with regions, as
    # tools/lift_jobs.sh does; SPU_LIFT_REGIONS=0 still opts out.
    env = dict(os.environ)
    env["SPU_LIFT_RCACHE"] = "0"
    env.setdefault("SPU_LIFT_REGIONS", "1")
    tmp = os.path.join(args.out, "_tmp.elf")
    mods = []
    for i, path in enumerate(dumps):
        raw = open(path, "rb").read()
        dump_ea = int(os.path.splitext(os.path.basename(path))[0].replace("ovl_", ""), 16)
        ea = relocate(elf, dump_ea, raw, os.path.basename(path))
        name = "pm_%08X" % ea
        prefix = name + "_"
        with open(tmp, "wb") as f:
            f.write(wrap(raw, base=args.base, entry=args.base))
        cmd = [sys.executable, lifter, "--auto-functions", tmp,
               "--base", hex(args.base), "--symbol-prefix", prefix,
               "-o", os.path.join(args.out, name),
               "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h"]
        # The WWS job manager's SPU interrupt vector: its init plants `bra 0xA2C`
        # at LS 0 (built with ilhu/iohl + shli, so no scan finds it). Without a
        # lifted entry every stall-and-notify interrupt started in the SMC
        # microstepper and then the interpreter.
        if args.wws_ea and dump_ea == args.wws_ea:
            cmd += ["--extra-funcs", "0xA2C"]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            print(f"  FAIL {name}\n{r.stderr[-2000:]}")
            continue
        for fn in ("spu_recomp.c", "spu_recomp.h"):   # "../../runtime/spu/x.h" -> "x.h"
            debase_includes(os.path.join(args.out, name, fn))
        funcs = unsup = 0
        for line in r.stdout.splitlines():
            s = line.strip()
            if s.endswith("function(s) lifted"):
                funcs = int(s.split()[0])
            if "unsupported instruction(s)" in s:
                unsup = int(s.split()[0])
        img = args.image_base + i
        mods.append((name, prefix, ea, img, len(raw), dump_ea))
        print(f"  {name:22} size={len(raw):6} funcs={funcs:4} unsup={unsup:3} image={img}")
    if os.path.exists(tmp):
        os.remove(tmp)

    L = ["/* spu_pm_register.c -- GENERATED by tools/lift_policy.py.",
         " * SPURS policy modules, entered at LS 0x%X by the lifted kernel." % args.base,
         " * Each is a bounded co-resident code REGION keyed by its source EA (libsre",
         " * and the EBOOT sit at fixed addresses): the runtime gives it its own",
         " * resident-code slot when it is DMA'd in, so the job manager stays live",
         " * while a job module is the resident overlay at 0x4000. */",
         "#include <stdint.h>",
         "extern void spu_begin_image(int image_id);",
         "extern void spu_overlay_register_region_sticky(uint32_t content_ea, uint32_t span, int image_id);",
         "extern int g_spu_wws_pm_image;"]
    for name, prefix, *_ in mods:
        L.append(f"extern void {prefix}spu_recomp_register(void);")
    L.append("void infamous2_pm_register_all(void)\n{")
    for name, prefix, ea, img, size, dump_ea in mods:
        L.append(f"    spu_begin_image({img}); {prefix}spu_recomp_register();")
        L.append(f"    spu_overlay_register_region_sticky(0x{ea:08X}u, 0x{size:X}u, {img});   /* {name} */")
    wws = [img for name, _, ea, img, size, dump_ea in mods if args.wws_ea and dump_ea == args.wws_ea]
    if wws:
        L.append(f"    g_spu_wws_pm_image = {wws[0]};   /* the WWS job manager (Wws_Job PM) */")
    tset = [img for name, _, ea, img, size, dump_ea in mods if ea == args.taskset_ea]
    tasks = args.task
    if not tasks:
        # the psarc/zlib decompressor: the EBOOT's first SPU image (lifted as image 1),
        # wherever this build of the EBOOT keeps it (1.00 0x7AB900, 1.04 0x80A100)
        imgs = sorted(glob.glob(os.path.join(args.images, "*.elf")))
        tea = None
        if imgs and elf is not None:
            hits = eboot_find(elf, open(imgs[0], "rb").read()[:4096])
            tea = hits[0] if len(hits) == 1 else None
        if tea is None:
            tea = 0x7AB900
            print(f"  WARNING task ELF of {imgs[0] if imgs else 'image 1'} not located: 0x7AB900 (disc EBOOT)")
        tasks = ["0x%X=1" % tea]
    if tset:
        L.insert(L.index("extern int g_spu_wws_pm_image;") + 1,
                 "extern void spu_taskset_register_task_elf(uint32_t elf_ea, int image_id, int policy_image_id);")
        for t in tasks:
            tea, timg = t.split("=")
            L.append(f"    spu_taskset_register_task_elf({tea}u, {int(timg)}, {tset[0]});"
                     f"   /* task ELF -> lifted image, under the taskset PM */")
    L.append("    spu_begin_image(0);\n}")
    L.append("__attribute__((constructor)) static void infamous2_pm_register_ctor(void)\n"
             "{\n    infamous2_pm_register_all();\n}")
    with open(os.path.join(args.out, "spu_pm_register.c"), "w") as f:
        f.write("\n".join(L) + "\n")
    print(f"wrote {os.path.join(args.out, 'spu_pm_register.c')} ({len(mods)} module(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
