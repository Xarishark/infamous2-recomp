# texpack: texture dumps and replacement packs

Everything here works on files made from **your own copy of the game**. Dumps and packs are derived from the
disc's data: keep them under `gamedata\` (git-ignored) and do not share or commit them.

## 1. Dump

    set TEXDUMP=1
    powershell -ExecutionPolicy Bypass -File tools\run.ps1 -Checkpoint @new -Step 11 -ExitAfter 1800 -Script "30-520:128,0,128,128"

Every static texture the game samples is written once (ever) to `gamedata\textures\dump\BCUS98125\<FMT>\`
as `<hash>_<W>x<H>_<FMT>[_ln][_m<levels>][_cube].png` (DDS for cubes and floats), and listed in `index.jsonl`
with where it was first seen and how the game samples it. `TEXDUMP_RAW=1` writes DDS files holding exactly
what the game uploads (an identity pack). Render targets, depth, texel-addressed, RSX-written, one-colour and
dynamic textures are never dumped; all but the dynamic ones are never replaced either (a texture whose
content keeps changing at one address is still looked up, so a pack can replace a streamed texture).

## 2. Make a pack

Build the tool once: `tools\texpack\build.cmd` (writes `build\texpack.exe`; needs LLVM clang-cl and the VS C++
tools, links only Windows' WIC).

    build\texpack.exe list     gamedata\textures\dump\BCUS98125 --min 512            (what, where seen, class)
    build\texpack.exe upscale  gamedata\textures\dump\BCUS98125 gamedata\textures\packs\BCUS98125\MyPack --scale 2
    build\texpack.exe compress gamedata\textures\packs\BCUS98125\MyPack               (PNG -> BC1 / BC3 DDS + mips)
    build\texpack.exe selftest

- `upscale` keeps the file names (the name is the texture's identity). Built-in method: DXT seam deblocking,
  Lanczos-3 in linear light with anti-ringing, an edge-aware unsharp mask (`--sharpen 0..1`, default 0.6);
  wrap / mirror / clamp edges as the game samples them (tiling stays seamless); alpha-tested textures keep
  their coverage; normal maps are renormalised and never sharpened; data textures (LUTs, NEAREST, <= 8 px,
  16-bit, float) are skipped. Filters: `--only <hash,...>`, `--fmt DXT1`, `--min 512`, `--class color`,
  `--limit N`. `--scale 1` copies (an identity pack); `--tint r,g,b` multiplies colour (a visible test).
- `--method external --exe <path>\realesrgan-ncnn-vulkan.exe [--model realesrgan-x4plus]` sends the colour of
  colour textures through Real-ESRGAN (a separate download, BSD-3 / MIT; not shipped here) on a wrap-aware
  bordered copy; alpha always goes through the built-in resampler, normal maps and data never go to it.
  Sharper on clean hard surfaces (wood, facades); it smooths fine grain and fabric into a painted look and
  invents lines on UI / text art, so check its output per texture.
- Or edit the dumped PNGs by hand: same name, any power-of-two multiple of the size (1/4 .. 16x).
- Play packs as DDS (BC1/BC3/BC7 with mips) where you can: they load in a few ms; a big PNG takes 50-100 ms of
  the loader thread and 4x the video memory.

## 3. Play

    set TEXPACK=1

Every folder under `gamedata\textures\packs\BCUS98125\` is a pack (`_name` folders are off; `TEXPACK_ORDER`
sets which wins). Replacements load in the background; the original is shown until a replacement is ready.
`TEXPACK_HOTRELOAD=1` picks up edited files while playing. All switches: `settings_manifest.txt`.
