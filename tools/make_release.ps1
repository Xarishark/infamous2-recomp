# Make the ready-to-run release zip, inFamous2Recomp-<tag>-windows.zip, from a commit. Publishes nothing.
#   powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Tag v0.1.2 [-Ref HEAD] [-OutDir out\release\v0.1.2] [-AllowDirty]
# The zip is `git archive <Ref>` under the folder inFamous2Recomp\, plus inFamous2Launcher.exe built from that
# same tree, plus release-files.txt: the list of every file in the zip. The launcher's updater writes only these
# files and deletes only files the installed release's list has and the new one does not, so player data
# (game\, build\, saves, settings, ...) is never touched. The launcher's version (AppInfo.Version) must equal the tag.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Ref = "HEAD",
    [string]$OutDir,
    [switch]$AllowDirty
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

if ($Tag -notmatch '^v(\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?)$') { throw "tag '$Tag' is not v<major>.<minor>.<patch>" }
$version = $Matches[1]
if (-not $OutDir) { $OutDir = Join-Path $root "out\release\$Tag" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# git writes progress to stderr: check exit codes instead of letting "Stop" turn it into errors
$ErrorActionPreference = "Continue"
$commit = (git -C $root rev-parse --verify "$Ref^{commit}" 2>&1)
if ($LASTEXITCODE -ne 0) { throw "git: '$Ref' is not a commit: $commit" }
if ($Ref -eq "HEAD" -and (git -C $root status --porcelain --untracked-files=no)) {
    if (-not $AllowDirty) { throw "the working tree has uncommitted changes, which the zip would NOT contain (it is made from $Ref). Commit them, or pass -AllowDirty." }
    Write-Warning "uncommitted changes are not in the zip (it is made from $Ref)"
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("inf2-release-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
try {
    $base = Join-Path $work "base.zip"
    git -C $root archive --format=zip --prefix=inFamous2Recomp/ -o $base $commit
    if ($LASTEXITCODE -ne 0) { throw "git archive failed (exit $LASTEXITCODE)" }
    [IO.Compression.ZipFile]::ExtractToDirectory($base, $work)
    $tree = Join-Path $work "inFamous2Recomp"

    # the launcher in the zip must know it is this release, or it would offer this release as an update forever
    $cs = Get-Content (Join-Path $tree "launcher\inFamous2Launcher.cs") -Raw
    if ($cs -notmatch 'public const string Version = "([^"]+)";') { throw "no AppInfo.Version in launcher\inFamous2Launcher.cs" }
    if ($Matches[1] -ne $version) { throw "the launcher's AppInfo.Version is $($Matches[1]), the tag is ${Tag}: bump launcher\inFamous2Launcher.cs and commit" }

    cmd /c "`"$tree\tools\build_launcher.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "the launcher build failed" }
    $exe = Join-Path $tree "inFamous2Launcher.exe"
    $pv = (Get-Item $exe).VersionInfo.ProductVersion
    if ($pv -ne $version) { throw "the built launcher says $pv, expected $version" }
    $ErrorActionPreference = "Stop"

    # release-files.txt: every file of the release, itself included
    $manifest = "release-files.txt"
    $files = @(Get-ChildItem $tree -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($tree.Length + 1).Replace('\', '/') })
    if ($files -contains $manifest) { throw "the repository itself has a $manifest" }
    $files = @($files + $manifest | Sort-Object { $_.ToLowerInvariant() })
    $lines = @("# inFamous2Recomp $Tag ($($commit.Substring(0, 12))): the files this release ships.",
               "# The launcher's updater replaces these and deletes only files an older list has and this one does not.") + $files
    [IO.File]::WriteAllText((Join-Path $tree $manifest), ($lines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding($false)))

    $zipPath = Join-Path $OutDir "inFamous2Recomp-$Tag-windows.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    $fs = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew)
    $zip = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $files) {
            $src = Join-Path $tree $f.Replace('/', '\')
            $e = $zip.CreateEntry("inFamous2Recomp/$f", [IO.Compression.CompressionLevel]::Optimal)
            $e.LastWriteTime = (Get-Item $src).LastWriteTime
            $in = [IO.File]::OpenRead($src); $out = $e.Open()
            try { $in.CopyTo($out) } finally { $out.Dispose(); $in.Dispose() }
        }
    } finally { $zip.Dispose(); $fs.Dispose() }
    Copy-Item $exe (Join-Path $OutDir "inFamous2Launcher.exe") -Force

    "release $Tag from $($commit.Substring(0, 12)): $($files.Count) files"
    foreach ($p in @($zipPath, (Join-Path $OutDir "inFamous2Launcher.exe"))) {
        "{0}  {1}  ({2} bytes)" -f (Get-FileHash $p -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path $p -Leaf), (Get-Item $p).Length
    }
    "nothing was published: upload these two files to the GitHub release $Tag yourself"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
