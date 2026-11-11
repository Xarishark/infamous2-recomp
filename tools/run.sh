#!/usr/bin/env bash
# Run the recompiled build. NOTE: as of ps3recomp 2026-09 only Windows has a
# renderer that draws a real game (D3D12). On Linux the runner boots a lifted
# image but renders through the headless CI backend; macOS has Metal but no
# verified title boot yet (see ps3recomp README, "Platform support").
#   tools/run.sh [--live-draw] [--verbose] [--dump-spu]
set -euo pipefail
cd "$(dirname "$0")/.."
# Windows installs "python" (and a Microsoft Store stub named python3); prefer a python3 that really runs.
if [ -z "${PYTHON:-}" ]; then
    if python3 -c "" >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
PY="$PYTHON"
EBOOT="$("$PY" -c 'import json;print(json.load(open("analysis/inputs.json"))["eboot"])')"

for a in "$@"; do
    case "$a" in
        --live-draw) export RSX_LIVE_DRAW=1 ;;
        --verbose)   export PS3_VERBOSE=1 IF2_VERBOSE=1 ;;
        # SPURS job binaries load from data files, not the EBOOT; this is the
        # only way to capture them for lifting (docs/ROADMAP.md, M4).
        --dump-spu)  mkdir -p analysis/spu_runtime analysis/pm
                     export SPU_DUMP_MISS="$PWD/analysis/spu_runtime"
                     export SPU_DUMP_OVL="$PWD/analysis/pm,0xA00" ;;
    esac
done
# Pin the VFS root: the runner's derive-from-EBOOT-path guess is wrong when
# lifting the title-update EBOOT, which lives under hdd0/.
export PS3_VFS_ROOT="$PWD/game/disc"
# game/hdd0 holds the title update: only a build lifted from it runs with it (tools/build.ps1 -Disc)
if [ -d game/hdd0 ] && grep -q '"update_app_ver": "' analysis/inputs.json 2>/dev/null; then
    export PS3_HDD0_ROOT="$PWD/game/hdd0"
fi
export PS3_TITLE="inFamous 2 (ps3recomp)"

mkdir -p logs
log="logs/run-$(date +%Y%m%d-%H%M%S).log"
./build/infamous2 "$EBOOT" 2>&1 | tee "$log"
echo "log: $log"
