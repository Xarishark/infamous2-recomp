# Fetch the pinned ps3recomp toolkit into external\ and install its Python deps.
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools\setup.ps1
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "python.ps1")
# Native tools write progress to stderr; with this output redirected, Windows PowerShell 5 would make
# each such line a terminating error under "Stop". Exit codes are checked instead.
$ErrorActionPreference = "Continue"

$pin  = (Get-Content ps3recomp.pin -Raw).Trim()
$dest = "external\ps3recomp"
function Git-Ok($what) { if ($LASTEXITCODE -ne 0) { throw "git: $what failed (exit $LASTEXITCODE)" } }

if (-not (Test-Path "$dest\.git")) {
    New-Item -ItemType Directory -Force external | Out-Null
    git clone https://github.com/sp00nznet/ps3recomp.git $dest 2>&1 | ForEach-Object { "$_" }
    Git-Ok "clone of github.com/sp00nznet/ps3recomp"
}
# Fetch only when the pinned commit is not here yet (re-runs work offline).
git -C $dest cat-file -e "$pin^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    git -C $dest fetch --quiet origin 2>&1 | ForEach-Object { "$_" }
    Git-Ok "fetch"
}
git -C $dest checkout --quiet $pin
Git-Ok "checkout $pin"
git -C $dest log -1 --format="ps3recomp pinned at %h %cd" --date=short

# Local fixes to the toolkit, kept here until they land upstream. Reset tracked
# files and remove untracked ones (files the patch itself adds are left over by
# an earlier apply, and `git apply` refuses to create a file that exists) so
# re-running setup re-applies cleanly. Ignored files are kept.
git -C $dest checkout --quiet -- .
git -C $dest clean --quiet -fd
foreach ($p in @(Get-ChildItem patches\ps3recomp\*.patch -ErrorAction SilentlyContinue)) {
    git -C $dest apply --ignore-whitespace --whitespace=nowarn $p.FullName 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $($p.Name)" }
    Write-Host "applied $($p.Name)"
}

$py = Resolve-Python
Write-Host "Python: $py"
& $py -m pip install --disable-pip-version-check -r "$dest\requirements.txt" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "pip install -r $dest\requirements.txt failed" }
Write-Host ""
Write-Host "Next: put your decrypted EBOOT + disc files under game\ (see game\README.md),"
Write-Host "then run:  py tools\check_inputs.py"
