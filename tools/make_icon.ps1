# make_icon.ps1 -In <png> -Out <ico>: build a Windows icon (256, 64, 48, 32, 24, 16 px, PNG-compressed entries) from a
# picture, fitted into a square on a transparent background with its aspect kept.
#
# The build runs this on the disc's own PS3_GAME\ICON0.PNG (320x176) to give infamous2.exe the game's icon. The
# result is derived from your copy of the game, so it is written to the build folder and never committed.
param([Parameter(Mandatory)][string]$In, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile((Resolve-Path $In).Path)
$sizes = 256, 64, 48, 32, 24, 16
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $k = [math]::Min($s / $src.Width, $s / $src.Height)
    $w = [int][math]::Round($src.Width * $k); $h = [int][math]::Round($src.Height * $k)
    $g.DrawImage($src, [int](($s - $w) / 2), [int](($s - $h) / 2), $w, $h)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose(); $ms.Dispose()
}
$src.Dispose()
# ICONDIR + ICONDIRENTRY[n] + the PNG payloads (Vista+ reads PNG entries of any size)
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$off)
    $off += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, $ms.ToArray())
$bw.Dispose()
Write-Host "icon: $Out ($($sizes -join ', ') px)"
