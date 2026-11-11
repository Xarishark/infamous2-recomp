<#
.SYNOPSIS
  Drive the running inFamous 2 (ps3recomp) window with synthetic keyboard and
  mouse input (SendInput), to test the keyboard/mouse controls in game.

.DESCRIPTION
  Runs a list of steps against the game window. Input only goes out while the
  game window is the foreground window: the script brings it forward first
  ("focus"), and stops (releasing everything it holds) as soon as anything
  else takes the foreground, so it never types into another application.

  SendInput relative mouse motion arrives in the game as raw input (WM_INPUT),
  exactly like a real mouse. The game only reads mouse motion and mouse
  buttons while it has captured the mouse, so "capture" (a click in the
  window, which the game swallows) must come before "move"/"click"/"hold RMB".

  Steps (separated by ';', '|' or new lines; key names as in config\input.ini):
    focus                      bring the game window to the foreground
    capture [x y]              left-click the client area (centre by default):
                               the game captures the mouse; the click is not a game input
    release                    tap Alt: the game releases the mouse
    wait <sec>
    press <keys> [ms]          tap (default 120 ms); <keys> may combine with '+': RMB+LMB
    hold <keys> <sec>          hold, e.g.  hold W 2   hold W+LShift 1.5   hold RMB 3
    down <keys> / up <keys>    press / release and leave it there
    click [left|right|middle|x1|x2] [ms]
    move <dx> <dy> <sec>       relative mouse motion (counts), spread evenly over <sec>
    wheel <notches>            + = up (away from you), - = down
    shot <file.png>            screenshot of the game's client area; the options
                               overlay (menu, FPS counter, notices) is hidden for
                               it unless -WithOverlay is given
    point <x> <y>              put the cursor at client (x, y), no click (menus)
    log <text>                 print a marker line

  Keys: A-Z 0-9 F1-F24 Space Enter Esc Tab Backspace CapsLock Shift LShift
  RShift Ctrl LCtrl RCtrl Alt LAlt RAlt Up Down Left Right Insert Delete Home
  End PageUp PageDown Num0-Num9 NumAdd NumSub NumMul NumDiv NumDecimal
  Semicolon Equals Comma Minus Period Slash Grave LBracket Backslash RBracket
  Apostrophe, mouse buttons LMB RMB MMB Mouse4 Mouse5, WheelUp WheelDown, 0xNN.

.EXAMPLE
  # walk forward 2 s, turn the camera right, aim + fire, jump, pause and unpause
  powershell -ExecutionPolicy Bypass -File tools\kbm_inject.ps1 -Steps "focus; capture; wait 0.5; shot screenshots\kbm_0.png; hold W 2; shot screenshots\kbm_1.png; move 800 0 1.0; wait 0.3; shot screenshots\kbm_2.png; hold RMB 1; press RMB+LMB 300; press Space; wait 1; press Esc; wait 1.5; shot screenshots\kbm_3.png; press Esc; release"

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\kbm_inject.ps1 -List
  powershell -ExecutionPolicy Bypass -File tools\kbm_inject.ps1 -DryRun -Steps "hold W 2; move 600 0 1"
#>
param(
    [string]$Steps = "",
    [string]$StepsFile = "",            # read the steps from a file instead
    [string]$Title = "",                # window title substring (default: the ps3recomp D3D12 window class)
    [int]$StepDelayMs = 100,            # pause after every step
    [switch]$WithOverlay,               # "shot" keeps the options overlay (default: hidden for the grab)
    [switch]$List,                      # list candidate windows and exit
    [switch]$DryRun                     # parse and print the steps; send nothing
)
$ErrorActionPreference = "Stop"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class KbmInject
{
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL; public ushort wParamH; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public HARDWAREINPUT hi; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion u; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);

    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint ms);
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint ms, out IntPtr result);

    public static IntPtr Target = IntPtr.Zero;

    public static string TitleOf(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
    public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }

    public static List<IntPtr> Windows(string titleSub)
    {
        var list = new List<IntPtr>();
        EnumWindows(delegate (IntPtr h, IntPtr lp) {
            if (!IsWindowVisible(h)) return true;
            string t = TitleOf(h), c = ClassOf(h);
            bool hit = String.IsNullOrEmpty(titleSub) ? c == "ps3recomp_d3d12"
                     : t.IndexOf(titleSub, StringComparison.OrdinalIgnoreCase) >= 0;
            if (hit) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static bool InFront() { return Target != IntPtr.Zero && GetForegroundWindow() == Target; }

    public static bool Focus()
    {
        if (IsIconic(Target)) ShowWindow(Target, 9 /* SW_RESTORE */);
        SetForegroundWindow(Target);
        Thread.Sleep(100);
        if (InFront()) return true;
        // Foreground lock: borrow the foreground thread's input state.
        uint pid;
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        uint me = GetCurrentThreadId();
        if (fg != 0 && fg != me) AttachThreadInput(me, fg, true);
        BringWindowToTop(Target);
        SetForegroundWindow(Target);
        if (fg != 0 && fg != me) AttachThreadInput(me, fg, false);
        Thread.Sleep(100);
        return InFront();
    }

    static void Send(INPUT i)
    {
        if (SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT))) != 1)
            throw new Exception("SendInput failed (error " + Marshal.GetLastWin32Error() + ")");
    }

    static bool IsExtended(ushort vk)
    {
        return (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E || vk == 0x2C ||
               (vk >= 0x5B && vk <= 0x5D) || vk == 0x6F || vk == 0x90 || vk == 0xA3 || vk == 0xA5;
    }

    public static void Key(ushort vk, bool up)
    {
        var i = new INPUT();
        i.type = 1;                                   // INPUT_KEYBOARD
        i.u.ki.wVk = vk;
        i.u.ki.wScan = (ushort)MapVirtualKey(vk, 0);  // MAPVK_VK_TO_VSC
        i.u.ki.dwFlags = (up ? 2u : 0u) | (IsExtended(vk) ? 1u : 0u);
        Send(i);
    }

    public static void Mouse(uint flags, uint data, int dx, int dy)
    {
        var i = new INPUT();
        i.type = 0;                                   // INPUT_MOUSE
        i.u.mi.dx = dx; i.u.mi.dy = dy; i.u.mi.mouseData = data; i.u.mi.dwFlags = flags;
        Send(i);
    }

    // Relative motion spread evenly over ms, on wall-clock time (not a count
    // of sleeps, which the 15.6 ms timer would stretch). False if the game
    // lost the foreground on the way.
    public static bool MoveOver(int dx, int dy, int ms)
    {
        var sw = Stopwatch.StartNew();
        int sentX = 0, sentY = 0;
        if (ms < 1) ms = 1;
        while (true) {
            if (!InFront()) return false;
            double f = Math.Min(1.0, sw.Elapsed.TotalMilliseconds / ms);
            int wantX = (int)Math.Round(dx * f), wantY = (int)Math.Round(dy * f);
            if (wantX != sentX || wantY != sentY) {
                Mouse(0x0001 /* MOVE */, 0, wantX - sentX, wantY - sentY);
                sentX = wantX; sentY = wantY;
            }
            if (f >= 1.0) return true;
            Thread.Sleep(4);
        }
    }

    public static bool Wait(int ms)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) {
            if (!InFront()) return false;
            Thread.Sleep(Math.Max(1, Math.Min(20, ms - (int)sw.ElapsedMilliseconds)));
        }
        return true;
    }

    // The game's options overlay is composed by the desktop, so a grab of the
    // screen would hold it: the game hides it on this registered message
    // (wParam 1, answering once it is gone; 0 shows it again).
    public static void Shot(string path, bool withOverlay)
    {
        IntPtr res = IntPtr.Zero;
        uint msg = withOverlay ? 0 : RegisterWindowMessage("ps3recomp_overlay_capture");
        if (msg != 0) { SendMessageTimeout(Target, msg, (IntPtr)1, IntPtr.Zero, 2 /* SMTO_ABORTIFHUNG */, 3000, out res); Thread.Sleep(30); }
        try { ShotNow(path); }
        finally { if (msg != 0) SendMessageTimeout(Target, msg, IntPtr.Zero, IntPtr.Zero, 2, 3000, out res); }
    }

    static void ShotNow(string path)
    {
        RECT r; GetClientRect(Target, out r);
        POINT p = new POINT(); ClientToScreen(Target, ref p);
        int w = r.Right - r.Left, h = r.Bottom - r.Top;
        using (var bmp = new System.Drawing.Bitmap(w, h))
        using (var g = System.Drawing.Graphics.FromImage(bmp)) {
            g.CopyFromScreen(p.X, p.Y, 0, 0, new System.Drawing.Size(w, h));
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
"@

# --- key names (the same names config\input.ini uses) ----------------------
$Named = @{
    "space"=0x20; "enter"=0x0D; "return"=0x0D; "esc"=0x1B; "escape"=0x1B; "tab"=0x09;
    "backspace"=0x08; "back"=0x08; "capslock"=0x14; "caps"=0x14;
    "shift"=0x10; "lshift"=0xA0; "rshift"=0xA1; "ctrl"=0x11; "control"=0x11; "lctrl"=0xA2; "rctrl"=0xA3;
    "alt"=0x12; "lalt"=0xA4; "ralt"=0xA5;
    "up"=0x26; "down"=0x28; "left"=0x25; "right"=0x27;
    "insert"=0x2D; "ins"=0x2D; "delete"=0x2E; "del"=0x2E; "home"=0x24; "end"=0x23;
    "pageup"=0x21; "pgup"=0x21; "pagedown"=0x22; "pgdn"=0x22; "pause"=0x13;
    "nummul"=0x6A; "numadd"=0x6B; "numsub"=0x6D; "numdecimal"=0x6E; "numdiv"=0x6F;
    "semicolon"=0xBA; "equals"=0xBB; "comma"=0xBC; "minus"=0xBD; "period"=0xBE; "slash"=0xBF;
    "grave"=0xC0; "lbracket"=0xDB; "backslash"=0xDC; "rbracket"=0xDD; "apostrophe"=0xDE;
}
# mouse "keys": button flags (down, up, data)
$MouseKeys = @{
    "lmb"=@(0x0002,0x0004,0); "mouse1"=@(0x0002,0x0004,0); "left"=@(0x0002,0x0004,0);
    "rmb"=@(0x0008,0x0010,0); "mouse2"=@(0x0008,0x0010,0); "right"=@(0x0008,0x0010,0);
    "mmb"=@(0x0020,0x0040,0); "mouse3"=@(0x0020,0x0040,0); "middle"=@(0x0020,0x0040,0);
    "mouse4"=@(0x0080,0x0100,1); "x1"=@(0x0080,0x0100,1);
    "mouse5"=@(0x0080,0x0100,2); "x2"=@(0x0080,0x0100,2);
}

function Resolve-Key([string]$name, [switch]$ClickNames) {
    $n = ($name -replace '[\s_\-]', '').ToLowerInvariant()
    if ($n -eq "wheelup")   { return @{ kind = "wheel"; delta = 120 } }
    if ($n -eq "wheeldown") { return @{ kind = "wheel"; delta = -120 } }
    if ($MouseKeys.ContainsKey($n) -and ($ClickNames -or @("left","right","middle","x1","x2") -notcontains $n)) {
        $m = $MouseKeys[$n]; return @{ kind = "mouse"; down = $m[0]; up = $m[1]; data = $m[2]; name = $name }
    }
    if ($n.Length -eq 1 -and $n -match '[a-z0-9]') { return @{ kind = "key"; vk = [int][char]$n.ToUpperInvariant(); name = $name } }
    if ($n -match '^f(\d{1,2})$' -and [int]$matches[1] -ge 1 -and [int]$matches[1] -le 24) { return @{ kind = "key"; vk = 0x6F + [int]$matches[1]; name = $name } }
    if ($n -match '^num(pad)?(\d)$') { return @{ kind = "key"; vk = 0x60 + [int]$matches[2]; name = $name } }
    if ($n -match '^0x([0-9a-f]{1,2})$') { return @{ kind = "key"; vk = [Convert]::ToInt32($matches[1], 16); name = $name } }
    if ($Named.ContainsKey($n)) { return @{ kind = "key"; vk = $Named[$n]; name = $name } }
    throw "unknown key '$name'"
}

$script:Held = New-Object System.Collections.ArrayList

function Log([string]$msg) { Write-Host ("[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $msg) }

function Assert-Front {
    if (-not [KbmInject]::InFront()) { throw "the game window is not in the foreground -- stopping (nothing more is sent)" }
}

function Key-Down($k) {
    Assert-Front
    switch ($k.kind) {
        "key"   { [KbmInject]::Key([uint16]$k.vk, $false) }
        "mouse" { [KbmInject]::Mouse([uint32]$k.down, [uint32]$k.data, 0, 0) }
        "wheel" {
            $d = [int64]$k.delta; if ($d -lt 0) { $d += 4294967296 }   # mouseData is a DWORD
            [KbmInject]::Mouse(0x0800, [uint32]$d, 0, 0); return
        }
    }
    [void]$script:Held.Add($k)
}

function Key-Up($k) {
    switch ($k.kind) {
        "key"   { [KbmInject]::Key([uint16]$k.vk, $true) }
        "mouse" { [KbmInject]::Mouse([uint32]$k.up, [uint32]$k.data, 0, 0) }
        "wheel" { return }
    }
    for ($i = $script:Held.Count - 1; $i -ge 0; $i--) { if ($script:Held[$i] -eq $k) { $script:Held.RemoveAt($i) } }
}

function Release-All {
    for ($i = $script:Held.Count - 1; $i -ge 0; $i--) {
        $k = $script:Held[$i]
        try {
            if ($k.kind -eq "key") { [KbmInject]::Key([uint16]$k.vk, $true) }
            elseif ($k.kind -eq "mouse") { [KbmInject]::Mouse([uint32]$k.up, [uint32]$k.data, 0, 0) }
        } catch { }
    }
    $script:Held.Clear()
}

function Parse-Keys([string]$spec) { return @($spec -split '\+' | Where-Object { $_ } | ForEach-Object { Resolve-Key $_ }) }

function Wait-Front([double]$sec) {
    if (-not [KbmInject]::Wait([int]([math]::Round($sec * 1000)))) { throw "the game window lost the foreground -- stopping" }
}

function Run-Step([string]$step) {
    $t = @($step.Trim() -split '\s+' | Where-Object { $_ })
    if ($t.Count -eq 0) { return }
    $cmd = $t[0].ToLowerInvariant()
    $a = @($t | Select-Object -Skip 1)
    $inv = [Globalization.CultureInfo]::InvariantCulture
    switch ($cmd) {
        "focus" {
            if (-not $DryRun) { if (-not [KbmInject]::Focus()) { throw "could not bring the game window to the foreground (click it once, then re-run)" } }
            Log "focus"
        }
        "capture" {
            if ($DryRun) { Log "capture"; break }
            Assert-Front
            $r = New-Object KbmInject+RECT; [void][KbmInject]::GetClientRect([KbmInject]::Target, [ref]$r)
            $p = New-Object KbmInject+POINT
            if ($a.Count -ge 2) { $p.X = [int]$a[0]; $p.Y = [int]$a[1] } else { $p.X = [int](($r.Right - $r.Left) / 2); $p.Y = [int](($r.Bottom - $r.Top) / 2) }
            [void][KbmInject]::ClientToScreen([KbmInject]::Target, [ref]$p)
            [void][KbmInject]::SetCursorPos($p.X, $p.Y)
            Start-Sleep -Milliseconds 30
            [KbmInject]::Mouse(0x0002, 0, 0, 0); Start-Sleep -Milliseconds 60; [KbmInject]::Mouse(0x0004, 0, 0, 0)
            Log ("capture: clicked client ({0},{1})" -f $p.X, $p.Y)
        }
        "release" {
            if (-not $DryRun) { Assert-Front; [KbmInject]::Key(0x12, $false); Start-Sleep -Milliseconds 50; [KbmInject]::Key(0x12, $true) }
            Log "release (Alt)"
        }
        "wait" {
            $s = [double]::Parse($a[0], $inv)
            Log "wait $s s"
            if (-not $DryRun) { Wait-Front $s }
        }
        { $_ -eq "press" -or $_ -eq "hold" } {
            $keys = Parse-Keys $a[0]
            if ($cmd -eq "press") { $ms = 120; if ($a.Count -ge 2) { $ms = [int]$a[1] } }
            else { $ms = [int]([math]::Round([double]::Parse($a[1], $inv) * 1000)) }
            Log ("{0} {1} for {2} ms" -f $cmd, $a[0], $ms)
            if ($DryRun) { break }
            $pressed = New-Object System.Collections.ArrayList
            try {
                foreach ($k in $keys) { Key-Down $k; [void]$pressed.Add($k) }
                Wait-Front ($ms / 1000.0)
            } finally {
                for ($i = $pressed.Count - 1; $i -ge 0; $i--) { Key-Up $pressed[$i] }
            }
        }
        "down" { $keys = Parse-Keys $a[0]; Log "down $($a[0])"; if (-not $DryRun) { foreach ($k in $keys) { Key-Down $k } } }
        "up"   { $keys = Parse-Keys $a[0]; Log "up $($a[0])";   if (-not $DryRun) { foreach ($k in $keys) { Key-Up $k } } }
        "click" {
            $b = "left"; if ($a.Count -ge 1) { $b = $a[0] }
            $ms = 80; if ($a.Count -ge 2) { $ms = [int]$a[1] }
            $k = Resolve-Key $b -ClickNames
            if ($k.kind -ne "mouse") { throw "click: '$b' is not a mouse button" }
            Log "click $b ($ms ms)"
            if ($DryRun) { break }
            Key-Down $k
            try { Wait-Front ($ms / 1000.0) } finally { Key-Up $k }
        }
        "move" {
            $dx = [int]$a[0]; $dy = [int]$a[1]; $s = [double]::Parse($a[2], $inv)
            Log "move ($dx,$dy) over $s s"
            if (-not $DryRun) {
                Assert-Front
                if (-not [KbmInject]::MoveOver($dx, $dy, [int]([math]::Round($s * 1000)))) { throw "the game window lost the foreground during a move -- stopping" }
            }
        }
        "wheel" {
            $n = [int]$a[0]
            Log "wheel $n"
            if (-not $DryRun) {
                $k = @{ kind = "wheel"; delta = [math]::Sign($n) * 120 }
                for ($i = 0; $i -lt [math]::Abs($n); $i++) { Key-Down $k; Wait-Front 0.25 }
            }
        }
        "shot" {
            $path = $a -join ' '
            if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path (Get-Location).Path $path }
            $dir = Split-Path -Parent $path
            if (-not $DryRun) {
                if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
                [KbmInject]::Shot($path, [bool]$WithOverlay)
            }
            Log "shot $path$(if ($WithOverlay) { ' (with the overlay)' })"
        }
        "point" {
            if ($DryRun) { Log "point $($a[0]) $($a[1])"; break }
            Assert-Front
            $p = New-Object KbmInject+POINT
            $p.X = [int]$a[0]; $p.Y = [int]$a[1]
            [void][KbmInject]::ClientToScreen([KbmInject]::Target, [ref]$p)
            [void][KbmInject]::SetCursorPos($p.X, $p.Y)
            Log ("point: client ({0},{1})" -f $a[0], $a[1])
        }
        "log" { Log ("--- " + ($a -join ' ')) }
        default { throw "unknown step '$step'" }
    }
}

# --- main ------------------------------------------------------------------
[void][KbmInject]::SetProcessDPIAware()      # client rects and cursor positions in real pixels

if ($List) {
    $all = [KbmInject]::Windows($Title)
    if ($all.Count -eq 0) { Write-Host "no matching window (is the game running?)"; exit 1 }
    foreach ($h in $all) { Write-Host ("0x{0:X}  [{1}]  {2}" -f $h.ToInt64(), [KbmInject]::ClassOf($h), [KbmInject]::TitleOf($h)) }
    exit 0
}

if ($StepsFile) { $Steps = Get-Content -Raw $StepsFile }
$stepList = @($Steps -split '[;|\r\n]+' | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
if ($stepList.Count -eq 0) { throw "no steps (-Steps `"focus; capture; hold W 2; ...`")" }

# parse everything up front, so a typo fails before any input is sent
foreach ($s in $stepList) {
    $t = @($s -split '\s+')
    switch ($t[0].ToLowerInvariant()) {
        { @("press","hold","down","up") -contains $_ } { [void](Parse-Keys $t[1]) }
        "click" { if ($t.Count -ge 2) { [void](Resolve-Key $t[1] -ClickNames) } }
    }
}

if (-not $DryRun) {
    $wins = [KbmInject]::Windows($Title)
    if ($wins.Count -eq 0) { throw "game window not found (class ps3recomp_d3d12$(if ($Title) { " / title *$Title*" })) -- is the game running?" }
    [KbmInject]::Target = $wins[0]
    Log ("target window 0x{0:X}: {1}" -f [KbmInject]::Target.ToInt64(), [KbmInject]::TitleOf([KbmInject]::Target))
    [void][KbmInject]::timeBeginPeriod(1)
}

$rc = 0
try {
    foreach ($s in $stepList) {
        Run-Step $s
        if (-not $DryRun -and $StepDelayMs -gt 0) { Start-Sleep -Milliseconds $StepDelayMs }
    }
    Log "done"
} catch {
    Log ("ERROR: " + $_.Exception.Message)
    $rc = 1
} finally {
    if (-not $DryRun) { Release-All; [void][KbmInject]::timeEndPeriod(1) }
}
exit $rc
