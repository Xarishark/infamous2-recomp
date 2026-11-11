#!/usr/bin/env bash
# Fetch the pinned ps3recomp toolkit into external/ and install its Python deps.
# Run from the repo root. Re-run after bumping ps3recomp.pin.
set -euo pipefail
cd "$(dirname "$0")/.."

PIN="$(tr -d '[:space:]' < ps3recomp.pin)"
DEST=external/ps3recomp

if [ ! -d "$DEST/.git" ]; then
    mkdir -p external
    git clone https://github.com/sp00nznet/ps3recomp.git "$DEST"
fi
git -C "$DEST" fetch --quiet origin
git -C "$DEST" checkout --quiet "$PIN"
echo "ps3recomp pinned at $(git -C "$DEST" log -1 --format='%h %cd' --date=short)"

# Local fixes to the toolkit, kept here until they land upstream. checkout above
# resets tracked files, so re-applying on every run is safe.
git -C "$DEST" checkout --quiet -- .
git -C "$DEST" clean --quiet -fd   # files the patch adds survive a checkout; apply would refuse them
for pt in patches/ps3recomp/*.patch; do
    [ -e "$pt" ] || continue
    git -C "$DEST" apply --ignore-whitespace --whitespace=nowarn "$PWD/$pt" && echo "applied $(basename "$pt")"
done

# Windows installs "python" (and a Microsoft Store stub named python3); prefer a python3 that really runs.
if [ -z "${PYTHON:-}" ]; then
    if python3 -c "" >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
PY="$PYTHON"
"$PY" -m pip install -r "$DEST/requirements.txt" ${PIP_EXTRA:-}
echo
echo "Next: put your decrypted EBOOT + disc files under game/ (see game/README.md),"
echo "then run:  $PY tools/check_inputs.py"
