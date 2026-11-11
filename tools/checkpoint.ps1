<#
inFamous 2 checkpoints: saves to start test runs from, instead of booting the
game from the beginning (tools\run.ps1 -Checkpoint <name>; src\if2_checkpoint.cpp).

  powershell -ExecutionPolicy Bypass -File tools\checkpoint.ps1 <command> ...

  list                          every checkpoint: name, when, where in the game
  show <name> [-Open]           its details and files (-Open: the screenshot)
  save <name> ["description"] [-From game|lastrun|<checkpoint>|<folder>] [-Force]
                                keep a save as a checkpoint, with a screenshot of
                                the game window when it is running. -From:
                                  game     the player's save data (default)
                                  lastrun  the save data of the newest test run
                                  <name>   another checkpoint (e.g. auto-0012)
                                  <folder> a save directory -- e.g. RPCS3's
                                           dev_hdd0\home\00000001\savedata\BCUS98125_INFAMOUS2_0
                                           -- or a savedata folder holding one
  load <name> [-Force]          put the checkpoint's save into the player's save
                                data, so a plain run continues from it; the save
                                it replaces is kept first as backup-<time>.
                                Refused while the game runs (-Force overrides)
  rename <name> <new name>
  describe <name> "text"
  delete <name>                 to the Recycle Bin (the exact name, no prefix)
  prune [-Keep 50]              auto-NNNN entries beyond the newest -Keep, to the Recycle Bin
  wait [-ReadyFile <f>] [-Timeout 900]
                                wait for a checkpoint run to say READY (default: the
                                newest logs\run-*.ready -- an earlier, finished run's
                                only if no new run shows up within 20 s); prints
                                it, exit 0 = ready. -ReadyFile (run.ps1 prints it)
                                is exact.
  path                          the library folder
Names are entry names, never paths (letters, digits, '_', '-', '.'); show / load /
rename / describe also take a unique prefix ("auto-0012", "03").

Library: $env:CHECKPOINT_DIR, else <project>\checkpoints (in .gitignore: these are
copies of save data -- user data, never committed). An entry:
  <name>\meta.json   <name>\screen.png   <name>\savedata\<SAVE DIR>\{PARAM.SFO, STATE, ...}
A "flat" entry (a save directory's files directly in <name>\) works too.
With tools\run.ps1 -AutoSnap (CHECKPOINT_AUTOSNAP=1) every save the game writes
is kept as auto-NNNN by the game itself, so play sessions build the library.
Prologue points have no save (the game first saves in New Marais): see
tools\run.ps1 -Checkpoint @new -Step <n> (docs\NOTES.md, "checkpoints").
#>
param(
    [Parameter(Position = 0)][string]$Command = "list",
    [Parameter(Position = 1)][string]$Name,
    [Parameter(Position = 2)][string]$Text,
    [string]$From = "game",
    [string]$Library,
    [string]$SaveRoot,
    [string]$ReadyFile,
    [int]$Timeout = 900,
    [int]$Keep = 50,
    [switch]$Force,
    [switch]$Open
)
$ErrorActionPreference = "Stop"
$Project = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Library) { $Library = $env:CHECKPOINT_DIR }
if (-not $Library) { $Library = Join-Path $Project "checkpoints" }
if (-not $SaveRoot) { $SaveRoot = $env:PS3_SAVEDATA_DIR }
if (-not $SaveRoot) { $SaveRoot = Join-Path $Project "gamedata\dev_hdd0\home\00000001\savedata" }
$Utf8 = New-Object System.Text.UTF8Encoding $false

function Fail([string]$msg) { Write-Host "checkpoint: $msg" -ForegroundColor Red; exit 1 }

function Test-Name([string]$n) {
    if (-not $n) { Fail "a checkpoint name is needed" }
    if ($n -notmatch '^[A-Za-z0-9][A-Za-z0-9_.\-]*$') { Fail "'$n': use letters, digits, '_', '-', '.'" }
}

# PARAM.SFO: the binary PSF the PS3 / RPCS3 write, or older KEY=value text.
function Get-Sfo([string]$path) {
    $r = @{}
    if (-not (Test-Path -LiteralPath $path)) { return $r }
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -ge 20 -and $b[0] -eq 0 -and $b[1] -eq 0x50 -and $b[2] -eq 0x53 -and $b[3] -eq 0x46) {
        $kt = [BitConverter]::ToUInt32($b, 8); $dt = [BitConverter]::ToUInt32($b, 12); $n = [BitConverter]::ToUInt32($b, 16)
        for ($i = 0; $i -lt $n -and (20 + 16 * $i + 16) -le $b.Length; $i++) {
            $e = 20 + 16 * $i
            $ko = $kt + [BitConverter]::ToUInt16($b, $e); $fmt = [BitConverter]::ToUInt16($b, $e + 2)
            $len = [BitConverter]::ToUInt32($b, $e + 4); $off = $dt + [BitConverter]::ToUInt32($b, $e + 12)
            $ke = $ko; while ($ke -lt $b.Length -and $b[$ke] -ne 0) { $ke++ }
            $key = [Text.Encoding]::ASCII.GetString($b, $ko, $ke - $ko)
            if ($off + $len -gt $b.Length) { continue }
            if ($fmt -eq 0x0404) { $r[$key] = [BitConverter]::ToUInt32($b, $off) }
            else { $r[$key] = [Text.Encoding]::UTF8.GetString($b, $off, $len).Split([char]0)[0] }
        }
    } else {
        foreach ($l in [Text.Encoding]::UTF8.GetString($b) -split "`n") {
            $q = $l.IndexOf('='); if ($q -gt 0) { $r[$l.Substring(0, $q)] = $l.Substring($q + 1).TrimEnd("`r") }
        }
    }
    return $r
}

# The save directories (holding a PARAM.SFO) in an entry, a savedata folder or a save folder.
function Get-SaveDirs([string]$path) {
    if (Test-Path -LiteralPath (Join-Path $path "PARAM.SFO")) { return @(Get-Item -LiteralPath $path) }
    $sd = Join-Path $path "savedata"
    if (Test-Path -LiteralPath $sd) { $path = $sd }
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    # not cellSaveData's ".<dir>.tmp" / ".<dir>.old" work directories: an
    # uncommitted or superseded copy is not "the player's save"
    return @(Get-ChildItem -LiteralPath $path -Directory | Where-Object { $_.Name -notlike ".*" -and (Test-Path -LiteralPath (Join-Path $_.FullName "PARAM.SFO")) } |
             Sort-Object { (Get-ChildItem -LiteralPath $_.FullName -File | Measure-Object LastWriteTime -Maximum).Maximum } -Descending)
}

function Get-Meta([string]$entry) {
    $m = Join-Path $entry "meta.json"
    # UTF-8 (the game and Write-Meta write it without a BOM; Windows PowerShell
    # would read it in the ANSI code page)
    if (Test-Path -LiteralPath $m) { try { return Get-Content -LiteralPath $m -Raw -Encoding UTF8 | ConvertFrom-Json } catch { } }
    return $null
}
function Write-Meta([string]$entry, $meta) {
    [IO.File]::WriteAllText((Join-Path $entry "meta.json"), ($meta | ConvertTo-Json -Depth 4), $Utf8)
}

function Get-Entry([string]$n, [switch]$Exact) {
    # a name, never a path: "delete .." or "delete ..\gamedata" must not reach
    # outside the library (this also keeps wildcards out of the prefix match)
    Test-Name $n
    $p = Join-Path $Library $n
    if (-not (Test-Path -LiteralPath $p -PathType Container)) {
        if ($Exact) { Fail "no checkpoint named exactly '$n' in $Library (checkpoint.ps1 list)" }
        # a unique prefix will do ("auto-0012", "03")
        $hits = @(Get-ChildItem -LiteralPath $Library -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name.StartsWith($n, [StringComparison]::OrdinalIgnoreCase) -and $_.Name -notlike "_*" })
        if ($hits.Count -eq 1) { return $hits[0].FullName }
        if ($hits.Count -gt 1) { Fail "'$n' matches $($hits.Count) checkpoints: $(($hits | ForEach-Object Name) -join ', ')" }
        Fail "no checkpoint '$n' in $Library (checkpoint.ps1 list)"
    }
    return (Resolve-Path -LiteralPath $p).Path
}

# The game window's client area into a PNG (as tools\kbm_inject.ps1 "shot").
function Save-Screenshot([string]$file) {
    if (-not ("CkWin" -as [type])) {
        Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class CkWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] static extern IntPtr FindWindowExA(IntPtr p, IntPtr a, string cls, string title);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] static extern uint RegisterWindowMessageA(string name);
  [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutA(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint ms, out IntPtr res);
  public static bool Shot(string file, int maxw) {
    IntPtr top = FindWindowExA(IntPtr.Zero, IntPtr.Zero, "ps3recomp_d3d12", null);
    if (top == IntPtr.Zero || !IsWindowVisible(top) || IsIconic(top)) return false;
    IntPtr v = FindWindowExA(top, IntPtr.Zero, "ps3recomp_d3d12_view", null); if (v == IntPtr.Zero) v = top;
    RECT r; GetClientRect(v, out r); POINT o = new POINT(); ClientToScreen(v, ref o);
    int w = r.R - r.L, h = r.B - r.T; if (w < 16 || h < 16) return false;
    // the game's options overlay (menu, FPS counter) is composed by the desktop: hidden for the grab
    uint ovl = RegisterWindowMessageA("ps3recomp_overlay_capture"); IntPtr res;
    if (ovl != 0) { SendMessageTimeoutA(top, ovl, (IntPtr)1, IntPtr.Zero, 2, 3000, out res); System.Threading.Thread.Sleep(30); }
    using (var full = new Bitmap(w, h)) {
      try { using (var g = Graphics.FromImage(full)) g.CopyFromScreen(o.X, o.Y, 0, 0, new Size(w, h)); }
      finally { if (ovl != 0) SendMessageTimeoutA(top, ovl, IntPtr.Zero, IntPtr.Zero, 2, 3000, out res); }
      int dw = Math.Min(w, maxw), dh = Math.Max(1, h * dw / w);
      using (var s = new Bitmap(full, new Size(dw, dh))) s.Save(file, ImageFormat.Png);
    }
    return true;
  }
}
"@
    }
    return [CkWin]::Shot($file, 960)
}

function Show-List {
    $dirs = @(Get-ChildItem -LiteralPath $Library -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike "_*" } | Sort-Object Name)
    if (-not $dirs.Count) { Write-Host "no checkpoints yet ($Library)"; return }
    $w = [Math]::Max(12, ($dirs | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum)
    Write-Host ("{0} {1,-16} {2,-14} {3}" -f "NAME".PadRight($w), "CREATED", "SOURCE", "WHERE IN THE GAME  [note]")
    foreach ($d in $dirs) {
        $m = Get-Meta $d.FullName
        $sd = @(Get-SaveDirs $d.FullName)
        $sfo = if ($sd.Count) { Get-Sfo (Join-Path $sd[0].FullName "PARAM.SFO") } else { @{} }
        $where = if ($m -and $m.subtitle) { $m.subtitle } else { $sfo["SUB_TITLE"] }
        $detail = if ($m -and $m.detail) { $m.detail } else { $sfo["DETAIL"] }
        if ($detail) { $where = "$where | " + ($detail -split "`n")[0] }
        if (-not $sd.Count) { $where = "NO SAVE -- $where" }
        $created = if ($m -and $m.created) { ("" + $m.created).Replace("T", " ") } else { $d.CreationTime.ToString("yyyy-MM-dd HH:mm:ss") }
        if ($created.Length -gt 16) { $created = $created.Substring(0, 16) }
        $src = if ($m -and $m.source) { "" + $m.source } elseif ($sd.Count) { "import" } else { "?" }
        if ($src.Length -gt 14) { $src = $src.Substring(0, 12) + ".." }
        $note = if ($m -and $m.description -and $m.description -ne $m.subtitle -and -not ("" + $m.description).StartsWith("" + $m.subtitle)) { "  [" + $m.description + "]" } else { "" }
        Write-Host ("{0} {1,-16} {2,-14} {3}{4}" -f $d.Name.PadRight($w), $created, $src, $where, $note)
    }
    $runs = Join-Path $Library "_runs"
    if (Test-Path -LiteralPath $runs) { Write-Host ("test-run sandboxes: {0} in {1}" -f @(Get-ChildItem -LiteralPath $runs -Directory).Count, $runs) }
}

function Get-SourceDir([string]$from) {
    switch -regex ($from) {
        '^game$' {
            $sd = @(Get-SaveDirs $SaveRoot)
            if (-not $sd.Count) { Fail "no save in $SaveRoot (has the game saved yet?)" }
            return $sd[0].FullName
        }
        '^lastrun$' {
            $runs = Join-Path $Library "_runs"
            $sd = @(Get-ChildItem -LiteralPath $runs -Directory -ErrorAction SilentlyContinue | Sort-Object CreationTime -Descending |
                    ForEach-Object { Get-SaveDirs $_.FullName } | Select-Object -First 1)
            if (-not $sd.Count) { Fail "no test run with a save in $runs" }
            return $sd[0].FullName
        }
        default {
            if (Test-Path -LiteralPath $from) { $p = (Resolve-Path -LiteralPath $from).Path }
            else { $p = Get-Entry $from }
            $sd = @(Get-SaveDirs $p)
            if (-not $sd.Count) { Fail "no save directory (with a PARAM.SFO) in $p" }
            return $sd[0].FullName
        }
    }
}

function Copy-SaveDir([string]$src, [string]$dstRoot, [string]$dirName) {
    $dst = Join-Path $dstRoot $dirName
    New-Item -ItemType Directory -Force $dst | Out-Null
    Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Name -ne "meta.json" -and $_.Name -notlike "screen.*" } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
            # a read-only copy (from a checkpoint marked read-only) would make
            # every later save of the game into this directory fail
            $c = Get-Item -LiteralPath (Join-Path $dst $_.Name)
            if ($c.IsReadOnly) { $c.IsReadOnly = $false }
        }
    return $dst
}

function Save-Entry([string]$n, [string]$desc, [string]$from, [string]$source) {
    Test-Name $n
    $entry = Join-Path $Library $n
    $src = Get-SourceDir $from
    if (Test-Path -LiteralPath $entry) {
        if (-not $Force) { Fail "'$n' exists (-Force replaces it; the old one goes to the Recycle Bin)" }
        if ($src.StartsWith((Resolve-Path -LiteralPath $entry).Path, [StringComparison]::OrdinalIgnoreCase)) { Fail "'$n' cannot replace itself" }
        Remove-ToRecycleBin $entry
    }
    $sfo = Get-Sfo (Join-Path $src "PARAM.SFO")
    $dirName = if ($sfo["SAVEDATA_DIRECTORY"]) { $sfo["SAVEDATA_DIRECTORY"] } else { Split-Path $src -Leaf }
    if ($dirName -eq "savedata" -or $dirName -eq $n) { $dirName = "BCUS98125_INFAMOUS2_0" }
    New-Item -ItemType Directory -Force $entry | Out-Null
    Copy-SaveDir $src (Join-Path $entry "savedata") $dirName | Out-Null
    # the source checkpoint's own screenshot, else the game window now
    $screen = ""
    $olds = @((Join-Path $src "screen.png"), (Join-Path $src "..\..\screen.png")) | Where-Object { Test-Path -LiteralPath $_ }
    if ($from -notmatch '^(game|lastrun)$' -and $olds) { Copy-Item -LiteralPath @($olds)[0] (Join-Path $entry "screen.png"); $screen = "screen.png" }
    elseif (Save-Screenshot (Join-Path $entry "screen.png")) { $screen = "screen.png" }
    if (-not $desc) { $desc = $sfo["SUB_TITLE"] }
    Write-Meta $entry ([ordered]@{
        name = $n; description = $desc; created = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
        source = $source; from = $src; dir = $dirName
        title = $sfo["TITLE"]; subtitle = $sfo["SUB_TITLE"]; detail = $sfo["DETAIL"]; screen = $screen })
    Write-Host ("saved '{0}': {1} | {2}  <- {3}{4}" -f $n, $sfo["SUB_TITLE"], (("" + $sfo["DETAIL"]) -split "`n")[0], $src,
                $(if ($screen) { "  (+ screenshot)" } else { "" }))
}

function Remove-ToRecycleBin([string]$path) {
    Add-Type -AssemblyName Microsoft.VisualBasic
    [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($path,
        [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
        [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
}

switch ($Command.ToLower()) {
    "list" { Show-List }
    "path" { Write-Output $Library }
    "show" {
        $e = Get-Entry $Name
        $m = Get-Meta $e
        Write-Host "checkpoint $(Split-Path $e -Leaf)  ($e)"
        if ($m) { $m | Format-List | Out-String | Write-Host }
        foreach ($sd in Get-SaveDirs $e) {
            $s = Get-Sfo (Join-Path $sd.FullName "PARAM.SFO")
            Write-Host ("save {0}: {1} -- {2}" -f $sd.Name, $s["SUB_TITLE"], (("" + $s["DETAIL"]) -replace "`n", "; "))
            Get-ChildItem -LiteralPath $sd.FullName -File | ForEach-Object { Write-Host ("   {0,-12} {1,8} bytes  {2}" -f $_.Name, $_.Length, $_.LastWriteTime) }
        }
        $shot = @(Get-ChildItem -LiteralPath $e -File -Filter "screen.*" | Select-Object -First 1)
        if ($shot.Count) { Write-Host "screenshot: $($shot[0].FullName)"; if ($Open) { Invoke-Item $shot[0].FullName } }
    }
    "save" {
        $label = if ($From -eq "game") { "manual" } elseif ($From -eq "lastrun") { "lastrun" }
                 elseif (Test-Path -LiteralPath $From) { "import" } else { "copy:" + (Split-Path (Get-Entry $From) -Leaf) }
        Save-Entry $Name $Text $From $label
    }
    "load" {
        $e = Get-Entry $Name
        $sd = @(Get-SaveDirs $e)
        if (-not $sd.Count) { Fail "'$Name' holds no save directory" }
        # A running game autosaves over what is loaded here (and its Continue
        # already happened): close it first.
        if (-not $Force -and (Get-Process -Name "infamous2" -ErrorAction SilentlyContinue)) {
            Fail "the game is running: it would save over the loaded checkpoint. Close it first (or -Force)"
        }
        $m = Get-Meta $e
        New-Item -ItemType Directory -Force $SaveRoot | Out-Null
        foreach ($s in $sd) {
            $sfo = Get-Sfo (Join-Path $s.FullName "PARAM.SFO")
            $dirName = if ($s.FullName -eq $e) { if ($m -and $m.dir) { $m.dir } elseif ($sfo["SAVEDATA_DIRECTORY"]) { $sfo["SAVEDATA_DIRECTORY"] } else { "BCUS98125_INFAMOUS2_0" } } else { $s.Name }
            $target = Join-Path $SaveRoot $dirName
            # A save interrupted between cellSaveData's two renames is left only as
            # ".<dir>.old" -- the player's latest save, which the runtime restores
            # when <dir> is missing. With <dir> about to exist again it would be
            # deleted by the next save instead: back it up as the replaced save.
            $stranded = Join-Path $SaveRoot ".$dirName.old"
            if (-not (Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $stranded)) {
                Move-Item -LiteralPath $stranded -Destination $target
                Write-Host "found the save '$dirName' left by an interrupted save ('.$dirName.old'): backing it up"
            }
            if (Test-Path -LiteralPath $target) {
                $bk = "backup-" + (Get-Date).ToString("yyyyMMdd-HHmmss")
                $bkd = Join-Path $Library "$bk\savedata"
                New-Item -ItemType Directory -Force $bkd | Out-Null
                Move-Item -LiteralPath $target -Destination $bkd
                $old = Get-Sfo (Join-Path $bkd "$dirName\PARAM.SFO")
                Write-Meta (Join-Path $Library $bk) ([ordered]@{ name = $bk; description = "the player's save before 'load $Name'"
                    created = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss"); source = "backup"; dir = $dirName
                    subtitle = $old["SUB_TITLE"]; detail = $old["DETAIL"]; screen = "" })
                Write-Host "the save it replaces is kept as '$bk'"
            }
            $dst = Copy-SaveDir $s.FullName $SaveRoot $dirName
            # Continue loads the save whose PARAM.SFO is newest (cellSaveData
            # sorts by it, as the title asks); the copy keeps the checkpoint's
            # old time, so a newer save in another slot would win. Make it now.
            $psfo = Join-Path $dst "PARAM.SFO"
            if (Test-Path -LiteralPath $psfo) { (Get-Item -LiteralPath $psfo).LastWriteTime = Get-Date }
            Write-Host ("loaded '{0}' into {1}: {2}" -f (Split-Path $e -Leaf), $target, $sfo["SUB_TITLE"])
        }
        Write-Host "a plain run (tools\run.ps1) now continues from it"
    }
    "rename" {
        $e = Get-Entry $Name; Test-Name $Text
        $dst = Join-Path $Library $Text
        if (Test-Path -LiteralPath $dst) { Fail "'$Text' exists" }
        Rename-Item -LiteralPath $e -NewName $Text
        $m = Get-Meta $dst
        if ($m) { $m.name = $Text; Write-Meta $dst $m }
        Write-Host "renamed '$(Split-Path $e -Leaf)' to '$Text'"
    }
    "describe" {
        $e = Get-Entry $Name
        $m = Get-Meta $e
        if (-not $m) { $m = [pscustomobject]@{ name = (Split-Path $e -Leaf); description = "" } }
        if ($m.PSObject.Properties["description"]) { $m.description = $Text } else { $m | Add-Member description $Text }
        Write-Meta $e $m
        Write-Host "described '$(Split-Path $e -Leaf)': $Text"
    }
    "delete" {
        $e = Get-Entry $Name -Exact      # never by prefix: "delete 03" must not pick an entry for you
        Remove-ToRecycleBin $e
        Write-Host "'$(Split-Path $e -Leaf)' moved to the Recycle Bin"
    }
    "prune" {
        $auto = @(Get-ChildItem -LiteralPath $Library -Directory -Filter "auto-*" |
                  Sort-Object { $n = 0; [void][int]::TryParse($_.Name.Substring(5), [ref]$n); $n } -Descending)
        $old = @($auto | Select-Object -Skip $Keep)
        foreach ($d in $old) { Remove-ToRecycleBin $d.FullName }
        Write-Host ("{0} auto checkpoint(s), {1} moved to the Recycle Bin (kept the newest {2})" -f $auto.Count, $old.Count, $Keep)
    }
    "wait" {
        # Without -ReadyFile: the newest run's logs\run-<time>.ready. An earlier
        # run's finished outcome must not answer for a run that is only starting
        # (run.ps1 started in the background, wait called at once), so a file
        # whose game has exited and that was finished before this wait began is
        # only taken when no newer run turns up within $grace seconds.
        $grace = [Math]::Min(20, $Timeout)
        $t0 = Get-Date
        $logs = Join-Path $Project "logs"
        function Read-Kv([string]$f) {
            $kv = @{}
            foreach ($l in Get-Content -LiteralPath $f -ErrorAction SilentlyContinue) { $q = $l.IndexOf('='); if ($q -gt 0) { $kv[$l.Substring(0, $q)] = $l.Substring($q + 1) } }
            return $kv
        }
        function Test-Alive($kv) { return $kv["pid"] -and (Get-Process -Id ([int]$kv["pid"]) -ErrorAction SilentlyContinue) }
        while ($true) {
            $waited = ((Get-Date) - $t0).TotalSeconds
            $f = $ReadyFile
            if (-not $f) {
                $n = Get-ChildItem -LiteralPath $logs -Filter "run-*.ready" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
                if ($n) { $f = $n.FullName }
            }
            $kv = if ($f -and (Test-Path -LiteralPath $f)) { Read-Kv $f } else { @{} }
            $st = $kv["status"]
            if ($st -and $st -ne "booting") {
                $stale = -not $ReadyFile -and -not (Test-Alive $kv) -and (Get-Item -LiteralPath $f).LastWriteTime -lt $t0
                if (-not $stale -or $waited -gt $grace) { break }
            } elseif ($st -eq "booting" -and $kv["pid"] -and -not (Test-Alive $kv) -and $waited -gt $grace) {
                Fail "the game (pid $($kv["pid"])) exited without a checkpoint outcome ($f)"
            }
            if ($waited -gt $Timeout) { Fail $(if ($f) { "no outcome in $f after $Timeout s" } else { "no logs\run-*.ready after $Timeout s -- start a run with tools\run.ps1 -Checkpoint <name>" }) }
            Start-Sleep -Milliseconds 500
        }
        Write-Output "ready file: $f"
        Get-Content -LiteralPath $f | Write-Output
        if ($kv["status"] -eq "ready") { exit 0 } else { exit 1 }
    }
    default { Fail "unknown command '$Command' (list, show, save, load, rename, describe, delete, prune, wait, path)" }
}
