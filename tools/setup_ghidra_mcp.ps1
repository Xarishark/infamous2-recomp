<#
.SYNOPSIS
  One-shot install of Ghidra + the GhidraMCP plugin + its MCP bridge on Windows,
  wired into Claude Desktop and this repo's Claude Code config.

.DESCRIPTION
  1. Downloads the latest bethington/ghidra-mcp release (plugin zip + bridge wheel).
  2. Reads which Ghidra version that plugin was built for, and downloads exactly
     that Ghidra release (Ghidra refuses extensions built for another version).
  3. Downloads a Temurin JDK 21 unless a JDK 21+ is already on PATH, and points
     Ghidra at it (no first-run "where is Java?" prompt).
  4. Installs the plugin into Ghidra's per-user Extensions directory.
  5. Creates a private Python venv and installs the bridge into it.
  6. Writes the "ghidra" MCP server into Claude Desktop's config (backing up the
     old file) and into <repo>\.mcp.json for Claude Code.
  Everything goes under -InstallDir; re-running is safe and skips finished steps.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\setup_ghidra_mcp.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "re-tools"),
    [switch]$SkipClaudeDesktop
)
$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"   # Invoke-WebRequest is 10x slower with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$Downloads  = Join-Path $InstallDir "downloads"
$PluginPort = 8089     # GhidraMCP v6 plugin's default HTTP port
$Headers    = @{ "User-Agent" = "infamous2-recomp-setup"; "Accept" = "application/vnd.github+json" }
New-Item -ItemType Directory -Force $InstallDir, $Downloads | Out-Null

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

function Get-File([string]$Url, [string]$Dest, [string]$Sha256) {
    if (-not (Test-Path $Dest)) {
        Write-Host "   downloading $(Split-Path -Leaf $Dest)"
        Invoke-WebRequest -Uri $Url -OutFile "$Dest.part" -Headers @{ "User-Agent" = "infamous2-recomp-setup" }
        Move-Item -Force "$Dest.part" $Dest
    }
    if ($Sha256) {
        $got = (Get-FileHash -Algorithm SHA256 $Dest).Hash
        if ($got -ne $Sha256.ToUpper()) {
            Remove-Item -Force $Dest
            throw "SHA-256 mismatch for $Dest (expected $Sha256, got $got) -- deleted, re-run to retry"
        }
        Write-Host "   sha256 ok"
    }
}

function Expand-Zip([string]$Zip, [string]$Dest) {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    # tar.exe ships with Windows 10 1803+ and is far faster than Expand-Archive on Ghidra's 20k files
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (Test-Path $tar) { & $tar -xf $Zip -C $Dest; if ($LASTEXITCODE -ne 0) { throw "tar failed on $Zip" } }
    else { Expand-Archive -Force -Path $Zip -DestinationPath $Dest }
}

function Get-AssetDigest($asset) {
    # GitHub exposes "digest": "sha256:<hex>" on release assets
    if ($asset.PSObject.Properties.Name -contains "digest" -and $asset.digest -like "sha256:*") {
        return $asset.digest.Substring(7)
    }
    return $null
}

# ---------------------------------------------------------------------------
Step "1/6  GhidraMCP release"
$mcpRel   = Invoke-RestMethod -Headers $Headers "https://api.github.com/repos/bethington/ghidra-mcp/releases/latest"
$pluginA  = $mcpRel.assets | Where-Object { $_.name -like "GhidraMCP-*.zip" } | Select-Object -First 1
$wheelA   = $mcpRel.assets | Where-Object { $_.name -like "*.whl" }           | Select-Object -First 1
if (-not $pluginA -or -not $wheelA) { throw "release $($mcpRel.tag_name) is missing the plugin zip or bridge wheel" }
Write-Host "   $($mcpRel.tag_name): $($pluginA.name), $($wheelA.name)"
$pluginZip = Join-Path $Downloads $pluginA.name
$wheel     = Join-Path $Downloads $wheelA.name
Get-File $pluginA.browser_download_url $pluginZip (Get-AssetDigest $pluginA)
Get-File $wheelA.browser_download_url  $wheel     (Get-AssetDigest $wheelA)

# Which Ghidra was the plugin built for? extension.properties says, as version=X.Y.Z
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($pluginZip)
try {
    $propEntry = $z.Entries | Where-Object { $_.FullName -like "*extension.properties" } | Select-Object -First 1
    if (-not $propEntry) { throw "no extension.properties in $pluginZip" }
    $reader = New-Object IO.StreamReader($propEntry.Open())
    $props  = $reader.ReadToEnd(); $reader.Close()
    $pluginDirName = $propEntry.FullName.Split("/")[0]
} finally { $z.Dispose() }
$GhidraVer = ([regex]::Match($props, "(?m)^\s*version\s*=\s*([0-9][0-9.]*)")).Groups[1].Value
if (-not $GhidraVer) { throw "could not read the target Ghidra version from extension.properties" }
Write-Host "   plugin targets Ghidra $GhidraVer"

# ---------------------------------------------------------------------------
Step "2/6  Ghidra $GhidraVer"
$GhidraHome = Join-Path $InstallDir "ghidra_$($GhidraVer)_PUBLIC"
if (-not (Test-Path (Join-Path $GhidraHome "ghidraRun.bat"))) {
    $gRel = Invoke-RestMethod -Headers $Headers "https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/tags/Ghidra_$($GhidraVer)_build"
    $gA   = $gRel.assets | Where-Object { $_.name -like "ghidra_$($GhidraVer)_PUBLIC_*.zip" } | Select-Object -First 1
    if (-not $gA) { throw "no ghidra_$($GhidraVer)_PUBLIC zip on release Ghidra_$($GhidraVer)_build" }
    $gZip = Join-Path $Downloads $gA.name
    Get-File $gA.browser_download_url $gZip (Get-AssetDigest $gA)
    Write-Host "   extracting (a minute or two)"
    Expand-Zip $gZip $InstallDir
}
Write-Host "   $GhidraHome"

# ---------------------------------------------------------------------------
Step "3/6  JDK 21"
function Get-JavaMajor([string]$javaExe) {
    try {
        $out = & $javaExe -version 2>&1 | Out-String
        $m = [regex]::Match($out, 'version "(\d+)')
        if ($m.Success) { return [int]$m.Groups[1].Value }
    } catch {}
    return 0
}
$JdkHome = $null
$sysJava = Get-Command java -ErrorAction SilentlyContinue
if ($sysJava -and (Get-JavaMajor $sysJava.Source) -ge 21) {
    $JdkHome = Split-Path -Parent (Split-Path -Parent $sysJava.Source)
    Write-Host "   using JDK on PATH: $JdkHome"
} else {
    $existing = Get-ChildItem $InstallDir -Directory -Filter "jdk-21*" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) { $JdkHome = $existing.FullName }
    else {
        $jdk  = (Invoke-RestMethod "https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jdk&os=windows&vendor=eclipse")[0].binary.package
        $jZip = Join-Path $Downloads $jdk.name
        Get-File $jdk.link $jZip $jdk.checksum
        Expand-Zip $jZip $InstallDir
        $JdkHome = (Get-ChildItem $InstallDir -Directory -Filter "jdk-21*" | Select-Object -First 1).FullName
    }
    Write-Host "   Temurin JDK: $JdkHome"
}
# Point Ghidra at it so it never prompts
$launchProps = Join-Path $GhidraHome "support\launch.properties"
$lp = Get-Content $launchProps
if (-not ($lp -match "^JAVA_HOME_OVERRIDE=.+")) {
    $lp = $lp -replace "^JAVA_HOME_OVERRIDE=.*$", "JAVA_HOME_OVERRIDE=$JdkHome"
    if (-not ($lp -match "^JAVA_HOME_OVERRIDE=")) { $lp += "JAVA_HOME_OVERRIDE=$JdkHome" }
    Set-Content -Path $launchProps -Value $lp -Encoding ASCII
}

# ---------------------------------------------------------------------------
Step "4/6  GhidraMCP plugin -> Ghidra user Extensions"
$extRoot = Join-Path $env:APPDATA "ghidra\ghidra_$($GhidraVer)_PUBLIC\Extensions"
$extDest = Join-Path $extRoot $pluginDirName
if (Test-Path $extDest) { Remove-Item -Recurse -Force $extDest }   # re-run = upgrade
Expand-Zip $pluginZip $extRoot
Write-Host "   $extDest"

# ---------------------------------------------------------------------------
Step "5/6  MCP bridge (private Python venv)"
$pyExe = $null; $pyArgs = @()
foreach ($cand in @("py|-3", "python")) {
    $parts = $cand.Split("|")
    $exe = Get-Command $parts[0] -ErrorAction SilentlyContinue
    if (-not $exe) { continue }
    $extra = @(); if ($parts.Length -gt 1) { $extra = @($parts[1]) }
    $ver = & $exe.Source @extra -c "import sys;print('%d.%d'%sys.version_info[:2])" 2>$null
    if ($ver -and [version]"$ver" -ge [version]"3.10") { $pyExe = $exe.Source; $pyArgs = $extra; break }
}
if (-not $pyExe) { throw "Python 3.10+ not found. Install it from python.org (tick 'Add to PATH') and re-run." }
$venv = Join-Path $InstallDir "ghidra-mcp-venv"
if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    & $pyExe @pyArgs -m venv $venv
}
& (Join-Path $venv "Scripts\python.exe") -m pip install --quiet --upgrade pip
& (Join-Path $venv "Scripts\python.exe") -m pip install --quiet --upgrade $wheel
$Bridge = Join-Path $venv "Scripts\bridge-mcp-ghidra.exe"
if (-not (Test-Path $Bridge)) { throw "bridge installed but $Bridge is missing -- check the wheel's entry point name" }
Write-Host "   $Bridge"

# ---------------------------------------------------------------------------
Step "6/6  MCP client config"
$server = [ordered]@{
    command = $Bridge
    args    = @("--transport", "stdio")
    env     = [ordered]@{ GHIDRA_MCP_URL = "http://127.0.0.1:$PluginPort" }
}
$utf8 = New-Object Text.UTF8Encoding($false)   # no BOM: some JSON parsers reject it

function Merge-McpConfig([string]$path) {
    $cfg = [ordered]@{}
    if (Test-Path $path) {
        Copy-Item $path "$path.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        $raw = [IO.File]::ReadAllText($path)
        if ($raw.Trim()) {
            $obj = $raw | ConvertFrom-Json
            foreach ($p in $obj.PSObject.Properties) { $cfg[$p.Name] = $p.Value }
        }
    }
    $servers = [ordered]@{}
    if ($cfg.Contains("mcpServers") -and $cfg["mcpServers"]) {
        foreach ($p in $cfg["mcpServers"].PSObject.Properties) { $servers[$p.Name] = $p.Value }
    }
    $servers["ghidra"] = $server
    $cfg["mcpServers"] = $servers
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, ($cfg | ConvertTo-Json -Depth 20), $utf8)
    Write-Host "   wrote $path"
}

if (Test-Path (Join-Path $RepoRoot "CMakeLists.txt")) { Merge-McpConfig (Join-Path $RepoRoot ".mcp.json") }
if (-not $SkipClaudeDesktop) { Merge-McpConfig (Join-Path $env:APPDATA "Claude\claude_desktop_config.json") }

# Convenience launcher
$launcher = Join-Path $InstallDir "Ghidra.cmd"
[IO.File]::WriteAllText($launcher, "@echo off`r`nstart `"`" `"$GhidraHome\ghidraRun.bat`"`r`n", $utf8)

Write-Host @"

Done. Three one-time clicks left, in Ghidra:
  1. Start it:  $launcher
  2. Open any program in CodeBrowser (File > Import File... -- a decrypted
     EBOOT.elf, or any binary to test with), then File > Configure >
     Configure All Plugins, tick "GhidraMCP", OK.
     (If GhidraMCP is not listed: File > Install Extensions > + and pick
      $pluginZip, then restart Ghidra.)
  3. Tools > GhidraMCP > Start MCP Server   (listens on 127.0.0.1:$PluginPort)

Then fully quit and reopen Claude Desktop (or start Claude Code in this repo);
the "ghidra" tools appear once Ghidra's server is running.
For PS3 PPU code import with language PowerPC:BE:64:64-32addr.
"@ -ForegroundColor Green
