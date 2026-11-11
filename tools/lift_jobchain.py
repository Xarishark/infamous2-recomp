#!/usr/bin/env python3
"""Lift the SPURS JobChain job binaries linked into the EBOOT, position-independent.

inFamous 2's post-processing (and other per-frame SPU work) runs as SPURS job
chains: libsre's JobChain policy module (image 123, tools/lift_policy.py) DMAs
each job binary from the EBOOT into a local-store buffer it allocates and calls
it. The buffer moves -- the menu alone loads binaries at 20+ LS addresses -- so
a lift at any fixed address misses, and every job ran in the SPU interpreter
(about one host core's worth of interpreter time per second of menu).

Every binary starts with the same header (4 x `ila`, then `xor $r?; br +8;
xor; br <code>`), which is what this scans for. Each is wrapped as an SPU ELF at
--base (0x20000: above every absolute target the policy module's API uses, so
absolute and image-relative branch targets cannot be confused), lifted with
spu_lifter.py --pic, and registered with spu_overlay_register_region_pic(), so
the runtime gives it a resident slot wherever it lands and runs it with that
slot's load bias.

    python tools/lift_jobchain.py analysis/inputs.json --out generated/spu_jobchain
"""
import argparse, json, os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR_TAIL = bytes.fromhex("4401285032000080")   # +0x10: xor ; br +8


def is_ila(w: int) -> bool:
    return (w >> 25) == 0x21


def find_jobs(d: bytes, seg_off: int, seg_va: int, seg_size: int):
    starts = []
    i = d.find(HDR_TAIL, seg_off)
    while 0 <= i < seg_off + seg_size:
        st = i - 0x10
        if st >= seg_off and st % 16 == 0 and \
                all(is_ila(struct.unpack_from(">I", d, st + k)[0]) for k in range(0, 16, 4)) and \
                d[st + 0x18:st + 0x1C] == HDR_TAIL[:4] and d[st + 0x1C] == 0x32:
            starts.append(st)
        i = d.find(HDR_TAIL, i + 1)
    jobs = []
    for n, st in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else min(st + 0x40000, seg_off + seg_size)
        if n + 1 == len(starts):            # last one: stop at a long zero run
            e = st + 0x40
            while e < end and d[e:e + 0x80] != b"\0" * 0x80:
                e += 0x10
            end = e
        while end > st + 0x40 and d[end - 4:end] == b"\0\0\0\0":
            end -= 4
        end = st + ((end - st + 15) & ~15)
        jobs.append((seg_va + st - seg_off, d[st:end]))
    return jobs


PLUGIN_MAGIC = bytes.fromhex("DA7ABA5E")


def find_plugins(d: bytes, seg_off: int, seg_va: int, seg_size: int):
    """The "DA7ABA5E" code plugins: 4 x `ila`, the magic, the code offset (0x30)
    and the total size. A WWS job module (image 225) DMAs one into its own LS
    (0x6C00) and `bisl`s to +0x30; unlifted, that ran in the interpreter --
    about 3.5 host cores' worth once the game is past the intro."""
    out = []
    i = d.find(PLUGIN_MAGIC, seg_off)
    while 0 <= i < seg_off + seg_size:
        st = i - 0x10
        if st >= seg_off and st % 16 == 0 and \
                all(is_ila(struct.unpack_from(">I", d, st + k)[0]) for k in range(0, 16, 4)):
            code, size = struct.unpack_from(">II", d, i + 4)
            if code == 0x30 and 0x30 < size <= 0x40000 and st + size <= seg_off + seg_size:
                out.append((seg_va + st - seg_off, d[st:st + ((size + 15) & ~15)]))
        i = d.find(PLUGIN_MAGIC, i + 1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("eboot", help="EBOOT.elf, or analysis/inputs.json")
    ap.add_argument("--out", default=os.path.join(ROOT, "generated", "spu_jobchain"))
    ap.add_argument("--sdk", default=os.path.join(ROOT, "external", "ps3recomp"))
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x20000)
    # Ids are 8-bit: game images 1.., libsre 41/42, PMs 120..123, WWS job
    # modules 200..; JobChain jobs 130..
    ap.add_argument("--image-base", type=int, default=130)
    # DA7ABA5E plugins: 160..
    ap.add_argument("--plugin-image-base", type=int, default=160)
    a = ap.parse_args()
    path = a.eboot
    if path.endswith(".json"):
        path = json.load(open(path))["eboot"]
    d = open(path, "rb").read()

    # executable PT_LOAD segment holding the binaries
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phnum = struct.unpack_from(">H", d, 0x38)[0]
    jobs = []
    plugins = []
    for k in range(phnum):
        t, fl, off, va, pa, fs, ms, al = struct.unpack_from(">IIQQQQQQ", d, phoff + k * 56)
        if t == 1 and fs:
            if fl & 1:
                jobs += find_jobs(d, off, va, fs)
            plugins += find_plugins(d, off, va, fs)
    if not jobs:
        print("no JobChain job binaries found")
        return 1
    if a.image_base + len(jobs) > a.plugin_image_base:
        print(f"{len(jobs)} jobs do not fit image ids {a.image_base}..{a.plugin_image_base - 1}")
        return 1
    if a.plugin_image_base + len(plugins) > 200:
        print(f"{len(plugins)} plugins do not fit image ids {a.plugin_image_base}..199")
        return 1
    # (ea, raw, name, image id, extra entries relative to the image start)
    items = [(ea, raw, f"jc_{ea:08x}", a.image_base + n, (0x18, 0x30))
             for n, (ea, raw) in enumerate(jobs)]
    items += [(ea, raw, f"plug_{ea:08x}", a.plugin_image_base + n, (0x30,))
              for n, (ea, raw) in enumerate(plugins)]

    sys.path.insert(0, os.path.join(a.sdk, "tools"))
    from wrap_spu_elf import wrap                     # noqa: E402
    from build_spu_workloads import debase_includes   # noqa: E402
    # The WWS job modules' position-independent seed scans apply unchanged:
    # these binaries are built the same way (r126/r79 = load base from an
    # `ila; brsl .+4; sf` probe, every address = link-time immediate + base).
    from lift_jobmods import switch_table_targets, manual_link_targets  # noqa: E402
    lifter = os.path.join(a.sdk, "tools", "spu_lifter.py")
    os.makedirs(a.out, exist_ok=True)
    tmp = os.path.join(a.out, "_tmp.elf")
    env = dict(os.environ)
    env.setdefault("SPU_LIFT_REGIONS", "1")
    # SPU registers in C locals (spu_lifter.py --reg-cache): the DA7ABA5E
    # plugins hold the hottest lifted SPU loops (plug_00821000 ~19% of SPU
    # host time); SPU_LIFT_RCACHE=0 restores the ctx->gpr lift.
    env.setdefault("SPU_LIFT_RCACHE", "1")
    done = []
    sigs = {}
    for ea, raw, name, img, entries in items:
        sigs[name] = raw[:16]      # the 4-ila header: matched when a job streams a copy
        with open(tmp, "wb") as f:
            f.write(wrap(raw, base=a.base, entry=a.base))
        # Jobs: +0x18 / +0x30 are the header's branch chain; the policy also
        # enters there (seen as dispatcher misses at buffer+0x18 / +0x30).
        # Plugins: +0x30 is the code entry the job module calls.
        #
        # Plus every case of a position-independent `switch`: the table sits
        # right after the dispatching `bi`, its entries relative to the table,
        # and the only immediate naming it is the table's link-time offset
        # (`ila $rT, tbl; a $rT, $rT, $r126`), which is below --base -- so
        # find_spu_functions' jump-table scan never tried it and no case was
        # lifted. jc_008a0900's +0x9F0 (called from +0x1254; cases +0xED4,
        # +0xEE0, +0xF64, +0x1050) logged "branched into unlifted LS
        # <base>+0xED4/+0xEE0 (lr=<base>+0x1258)" in most runs since
        # 2026-10-05 and ran in the interpreter (SPU_INTERP_UNLIFTED);
        # jc_008c3300, jc_008c4f80 and jc_008c6f00 carry the same construct.
        # Both scans return offsets here (base 0).
        found = set(switch_table_targets(raw, 0)) | set(manual_link_targets(raw, 0))
        if found - set(entries):
            print(f"  {name}: switch-case / manual-link entries "
                  f"{', '.join(f'+0x{e:X}' for e in sorted(found - set(entries)))}")
        entries = tuple(sorted(set(entries) | found))
        cmd = [sys.executable, lifter, "--auto-functions", tmp, "--base", hex(a.base),
               "--pic", "--symbol-prefix", name + "_",
               "--extra-funcs", ",".join(f"{a.base + e:#x}" for e in entries),
               "-o", os.path.join(a.out, name),
               "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h"]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            print(f"  FAIL {name}\n{r.stderr[-2000:]}")
            continue
        for fn in ("spu_recomp.c", "spu_recomp.h"):
            debase_includes(os.path.join(a.out, name, fn))
        funcs = unsup = 0
        for line in r.stdout.splitlines():
            s = line.strip()
            if s.endswith("function(s) lifted"):
                funcs = int(s.split()[0])
            if "unsupported instruction(s)" in s:
                unsup = int(s.split()[0])
        done.append((name, ea, img, len(raw)))
        print(f"  {name}  size=0x{len(raw):05X}  funcs={funcs:4} unsup={unsup:3} image={img}")
    if os.path.exists(tmp):
        os.remove(tmp)

    L = ["/* jobchain_register.c -- GENERATED by tools/lift_jobchain.py.",
         " * SPURS JobChain job binaries from the EBOOT, lifted position-independent",
         f" * at LS 0x{a.base:X}: each runs wherever the JobChain policy module loads it,",
         " * with that resident slot's load bias (spu_lifter.py --pic). */",
         "#include <stdint.h>",
         "extern void spu_begin_image(int image_id);",
         "extern void spu_overlay_register_region_pic(uint32_t content_ea, uint32_t span,",
         "                                            int image_id, uint32_t pic_base,",
         "                                            const uint8_t* sig);"]
    for name, *_ in done:
        L.append(f"extern void {name}_spu_recomp_register(void);")
    L.append("#include <stdlib.h>")
    L.append("void infamous2_jobchain_register_all(void)\n{")
    # The DA7ABA5E plugins are lifted by default; SPU_LIFTED_PLUGINS=0 runs
    # them in the interpreter instead (each call checks out against it,
    # SPU_PIC_VERIFY). They were opt-in while runs with them froze after the
    # intro: that was the RSX FIFO walker's park-grace timer (cellGcmSys.c),
    # which faster SPU code made fire -- see docs/NOTES.md, 2026-10-07.
    L.append('    const char* pe = getenv("SPU_LIFTED_PLUGINS");')
    L.append("    const int plugins = !(pe && *pe == '0');")
    for name, ea, img, size in done:
        sig = ", ".join(f"0x{b:02X}" for b in sigs[name])
        guard = "if (plugins) " if name.startswith("plug_") else ""
        L.append(f"    {guard}{{ static const uint8_t sig[16] = {{ {sig} }};")
        L.append(f"      spu_begin_image({img}); {name}_spu_recomp_register();")
        L.append(f"      spu_overlay_register_region_pic(0x{ea:08X}u, 0x{size:X}u, {img}, 0x{a.base:X}u, sig); }}")
    L.append("    spu_begin_image(0);\n}")
    L.append("__attribute__((constructor)) static void infamous2_jobchain_register_ctor(void)\n"
             "{\n    infamous2_jobchain_register_all();\n}")
    with open(os.path.join(a.out, "jobchain_register.c"), "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print(f"wrote {os.path.join(a.out, 'jobchain_register.c')} ({len(done)} job(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
