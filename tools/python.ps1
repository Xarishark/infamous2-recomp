# Dot-sourced by tools\setup.ps1 and tools\build.ps1: find a Python 3.10+ that really runs.
#
# Windows ships "python.exe" / "python3.exe" stubs (App Execution Aliases) that only open the
# Microsoft Store, so "python" on PATH is not enough. Order: $env:PYTHON if it runs, the py
# launcher (py -3), then python / python3 on PATH. The result is exported as $env:PYTHON (forward
# slashes, so tools\*.sh can run it from Git Bash) and its folder is put first on PATH.
function Resolve-Python {
    $cands = @()
    if ($env:PYTHON) { $cands += , @($env:PYTHON) }
    $cands += , @("py", "-3")
    $cands += , @("python")
    $cands += , @("python3")
    foreach ($c in $cands) {
        $exe = $c[0]; $pre = @($c | Select-Object -Skip 1)
        try {
            $out = & $exe @pre -c "import sys; print(sys.executable); print('%d.%d' % sys.version_info[:2])" 2>$null
        } catch { continue }
        if ($LASTEXITCODE -ne 0 -or -not $out -or @($out).Count -lt 2) { continue }
        $path = @($out)[0].Trim(); $ver = [version]@($out)[1].Trim()
        if ($ver -lt [version]"3.10") { Write-Host "skipping Python $ver at $path (3.10+ needed)"; continue }
        $env:PYTHON = $path -replace '\\', '/'
        $dir = Split-Path $path
        if (-not (($env:PATH -split ';') -contains $dir)) { $env:PATH = "$dir;$dir\Scripts;$env:PATH" }
        return $path
    }
    throw "Python 3.10+ not found -- install it from python.org (or: winget install --id Python.Python.3.12 --exact)"
}
