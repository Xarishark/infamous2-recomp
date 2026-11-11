#!/usr/bin/env bash
# Stage A -- understand the binary before lifting it. Run from anywhere.
#
#   tools/check_inputs.py   must have passed (it writes analysis/inputs.json)
#   GHIDRA_DIR=/opt/ghidra  optional: also run a headless Ghidra pass whose
#                           function starts seed the lift (better recall on a
#                           stripped retail binary than .opd + prologue scanning)
#
# Produces, under analysis/:
#   EBOOT.functions.json EBOOT.imports.json EBOOT.image.json EBOOT.loader.json
#   import_coverage.md   code_end.txt   spu_images/   ghidra/ (optional)
set -euo pipefail
cd "$(dirname "$0")/.."
# Windows installs "python" (and a Microsoft Store stub named python3); prefer a python3 that really runs.
if [ -z "${PYTHON:-}" ]; then
    if python3 -c "" >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
PY="$PYTHON"
SDK="${PS3RECOMP:-external/ps3recomp}"

[ -f analysis/inputs.json ] || { echo "run tools/check_inputs.py first"; exit 1; }
EBOOT="$("$PY" -c 'import json;print(json.load(open("analysis/inputs.json"))["eboot"])')"
echo "== EBOOT: $EBOOT"

echo "== stage 1: image, OPD function table, TOC, firmware imports"
"$PY" "$SDK/tools/ppu_loader.py" "$EBOOT" -o analysis/
# ppu_loader names its outputs after the input file's stem
for f in analysis/*.functions.json analysis/*.imports.json analysis/*.image.json analysis/*.loader.json; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"; ext="${base#*.}"
    [ "$base" = "EBOOT.$ext" ] || mv -f "$f" "analysis/EBOOT.$ext"
done

echo "== import coverage against the pinned ps3recomp"
"$PY" tools/import_coverage.py --sdk "$SDK"

echo "== code-end bound for the lifter"
"$PY" tools/code_end.py "$EBOOT" | tee analysis/code_end.txt

echo "== embedded SPU programs"
# (a fresh folder: images of a previously analysed EBOOT build would be lifted and registered too)
rm -rf analysis/spu_images
"$PY" "$SDK/tools/extract_spu_images.py" "$EBOOT" --output analysis/spu_images
n=$(find analysis/spu_images -maxdepth 1 -type f 2>/dev/null | wc -l)
echo "   $n SPU image(s) found statically. SPURS jobs loaded from data files at"
echo "   runtime will NOT be here -- capture those during bring-up (docs/ROADMAP.md, M4)."

if [ -n "${GHIDRA_DIR:-}" ]; then
    echo "== headless Ghidra pass (slow on a binary this size -- expect a long run)"
    "$PY" "$SDK/tools/ghidra_analyze.py" "$EBOOT" -o analysis/ghidra --ghidra-dir "$GHIDRA_DIR"
fi

echo
echo "Read analysis/import_coverage.md, then run tools/relift.sh"
