#!/usr/bin/env bash
# Lift the REAL SPURS library (libsre) from YOUR OWN decrypted firmware module so
# the game's cellSpurs calls run Sony's code instead of ps3recomp's partial HLE
# (see external/ps3recomp/docs/FIRMWARE_LLE.md).
#
#   tools/lift_libsre.sh path/to/libsre.prx
#   tools/lift_libsre.sh --ppu-only
#
# --ppu-only re-runs just the PPU lift (step 3) from the intermediates a full
# run left in generated/prx/lift -- no libsre.prx needed, and nothing else
# (relocated image, exports, SPU lifts) is touched. For lifter-only changes:
# the save-state return-address stores, or continuations.
#
# Save-state continuations (M3, docs/NOTES.md 2026-10-15 / 2026-10-20): the
# lift takes config/ss_cont_list.txt and config/ss_cont_list_libsre.txt
# together (the lifter keeps the libsre return sites of the lists and ignores
# the rest) and writes generated/prx/libsre_ns/ppu_recomp_cont.{cpp,txt}; the
# table registers itself with runtime/ppu/ppu_resume.cpp as image "libsre".
# PPU_LIFT_CONT_LIST=<file> lifts with that list instead, PPU_LIFT_CONT_LIST=-
# with none.
#
# Get libsre.prx: install the PS3 firmware in RPCS3, then Utilities -> Decrypt
# PS3 Binaries on <rpcs3>/dev_flash/sys/external/libsre.sprx.
#
# Output (git-ignored -- firmware-derived, never commit or distribute):
#   generated/prx/libsre.linked.bin        relocated image, loaded at runtime
#   generated/prx/libsre_ns/               lifted PPU code + export table
#   generated/prx/spu/                     SPU ELFs embedded in libsre
set -euo pipefail
# SPU lifts group basic blocks into call-graph regions (in-region branches are
# gotos instead of dispatcher hops); SPU_LIFT_REGIONS=0 restores the old lift.
export SPU_LIFT_REGIONS="${SPU_LIFT_REGIONS:-1}"
# spu_lifter.py --reg-cache (SPU_LIFT_RCACHE) is verified for tools/lift_jobs.sh's
# spu_jobs and tools/lift_jobchain.py's spu_jobchain only: never for these lifts.
export SPU_LIFT_RCACHE=0
cd "$(dirname "$0")/.."
if [ -z "${PYTHON:-}" ]; then
    if python3 -c "" >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
PY="$PYTHON"
SDK="${PS3RECOMP:-external/ps3recomp}"
PPU_ONLY=0
if [ "${1:-}" = "--ppu-only" ]; then
    PPU_ONLY=1
else
    PRX="${1:?usage: tools/lift_libsre.sh path/to/libsre.prx | --ppu-only}"
fi
BASE=0x30000000          # must match LIBSRE_BASE in src/stubs.cpp
OUT=generated/prx

if [ "$PPU_ONLY" = 1 ]; then
    for f in summary.txt libsre_image.bin libsre_functions.json libsre_imports.list.json; do
        [ -f "$OUT/lift/$f" ] || { echo "--ppu-only: no $OUT/lift/$f -- run a full tools/lift_libsre.sh first"; exit 1; }
    done
    TOC=$(sed -n 's/.*TOC \(0x[0-9A-Fa-f]*\).*/\1/p' "$OUT/lift/summary.txt" | head -n1)
else
rm -rf "$OUT" && mkdir -p "$OUT/lift" "$OUT/libsre_ns"

# 1. Relocate (linked image for the runtime) + metadata (seeds, exports, imports).
"$PY" "$SDK/tools/prx_relocate.py" "$PRX" --base "$BASE" --out "$OUT/libsre.linked.bin"
"$PY" "$SDK/tools/lift_prx.py" "$PRX" --base "$BASE" -o "$OUT/lift" | tee "$OUT/lift/summary.txt"
TOC=$(sed -n 's/.*TOC \(0x[0-9A-Fa-f]*\).*/\1/p' "$OUT/lift/summary.txt" | head -n1)

# 2. libsre's own firmware imports become ps3_hle_call(nid), like the game's.
"$PY" - "$OUT/lift" <<'EOF'
import json, sys
d = sys.argv[1]
imp = json.load(open(f"{d}/libsre_imports.json"))
out = [{"library": lib, "nid": nid.lower(), "stub": stub.lower()}
       for lib, m in imp.items() for nid, stub in m.items()]
json.dump(out, open(f"{d}/libsre_imports.list.json", "w"), indent=1)
# PPU code ends at the import stub table; after it is data (module info,
# export/import tables, embedded SPU programs). Lifting past it decodes data.

EOF
fi
CODE_END=$("$PY" -c "import json;d=json.load(open('$OUT/lift/libsre_imports.list.json'));print(hex(max(int(e['stub'],16) for e in d)+0x20))")

# 3. Lift to C under the libsre_ prefix so it links beside the game.
#    --cont-list: the save-state continuation lists, merged (see the top).
CONT=()
if [ -z "${PPU_LIFT_CONT_LIST:-}" ]; then
    : > "$OUT/lift/cont_list.txt"
    for f in config/ss_cont_list.txt config/ss_cont_list_libsre.txt; do
        [ -f "$f" ] && { cat "$f"; echo; } >> "$OUT/lift/cont_list.txt"
    done
    [ -s "$OUT/lift/cont_list.txt" ] && CONT=(--cont-list "$OUT/lift/cont_list.txt")
elif [ "$PPU_LIFT_CONT_LIST" != "-" ]; then
    CONT=(--cont-list "$PPU_LIFT_CONT_LIST")
fi
PPU_LIFT_CONT_LIST= "$PY" "$SDK/tools/ppu_lifter.py" "$OUT/lift/libsre_image.bin" --raw --base "$BASE" \
    --toc "$TOC" --symbol-prefix libsre_ \
    --functions "$OUT/lift/libsre_functions.json" \
    --hle-stubs "$OUT/lift/libsre_imports.list.json" \
    --code-end "$CODE_END" \
    "${CONT[@]}" \
    -o "$OUT/libsre_ns"
if [ "$PPU_ONLY" = 1 ]; then
    echo
    echo "libsre PPU code relifted (TOC $TOC, code end $CODE_END). Rebuild:"
    echo "  powershell -ExecutionPolicy Bypass -File tools\\build.ps1 -BuildOnly"
    exit 0
fi

# 4. NID -> export table for the runtime PRX loader.
"$PY" "$SDK/tools/prx_exports.py" "$PRX" --symbol libsre --out "$OUT/libsre_ns/libsre_exports.c"

# 5. Embedded SPU ELFs (the rest of the SPURS kernel's SPU code is captured at
#    runtime with run.ps1 -DumpSpu and lifted with relift.sh).
"$PY" "$SDK/tools/extract_spu_images.py" "$OUT/lift/libsre_image.bin" --output "$OUT/spu"
# ...and lift them (SPURS kernel etc.), registered by content fingerprint like
# the game's own SPU images.
# 0x808 is where a policy module exits to the kernel (stack reset); it must be
# a lifted entry so src/stubs.cpp can register it as a one-way stack reset.
"$PY" "$SDK/tools/build_spu_workloads.py" \
    --images "$OUT/spu" --lifted "$OUT/spu_lifted" --relift \
    --extra-funcs spu_0000_at_00020380=0x808 \
    --out "$OUT/spu_lifted/spu_workloads.c" \
    --register-fn infamous2_libsre_spu_register_all --constructor --title libsre
# build_spu_workloads numbers images from 1, as relift.sh does for the game's
# own SPU images -- the same ids would share one per-image dispatch table.
# Move libsre's images to 41+ (ids are 8-bit in the dispatch key).
"$PY" - "$OUT/spu_lifted/spu_workloads.c" <<'EOF'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"spu_begin_image\((\d+)\)",
           lambda m: f"spu_begin_image({int(m.group(1)) + 40 if int(m.group(1)) else 0})", s)
s = re.sub(r"(spu_workload_register_img\([^,]+,[^,]+, )(\d+)(,)",
           lambda m: f"{m.group(1)}{int(m.group(2)) + 40}{m.group(3)}", s)
open(p, "w").write(s)
EOF

echo
echo "libsre lifted (TOC $TOC, code end $CODE_END). Rebuild:"
echo "  powershell -ExecutionPolicy Bypass -File tools\\build.ps1 -BuildOnly"
