# Ground truth for the inFamous 2 port

Written 2026-10-15 from four research passes (RPCS3's known inFamous 2 gaps, an NV40/RSX spec audit of our decompilers, real-PS3 visual targets, Cell/LV2 semantics) and a completeness critique. Every claim that drives a recommendation was re-checked against its source; see "Re-verification log" (section 12).

Title: inFamous 2, BCUS98125 disc v02.00. Native output is 1280x720 with no anti-aliasing.

RPCS3 is not the target. Its wiki calls the game "Playable" only with workarounds (Strict Rendering Mode, relaxed ZCULL, a resolution-scale threshold); its compatibility DB entry ("Ingame") dates from 2018 and says nothing about current RPCS3 either way. The port must look and behave like a real PS3.

---

## 1. Ground-truth policy

### 1.1 Order of authority (highest first)

| Tier | Source | Typical use |
|---|---|---|
| **T1a: hardware measurements** | Outputs recorded on a real PS3: the ps3autotests `.expected` files, and the result dumps attached to hardware-test PRs (RPCS3 #19783, #19795 VMX estimates; the #19410 ROP suite of about 700 tests). The measured outputs count as facts, not the emulator code around them. | CPU float semantics, ROP rules |
| **T1b: hardware footage** | Real-PS3 captures of the same scene, measured numerically (`tools/ground_truth/targets.json`). | How a frame must look: levels, colour ratios, halo size, banding |
| **T2: published specs** | SPU ISA v1.2, CBEA, Cell Handbook v1.11, VMX PEM 2.07c, NV_fragment_program(2), NV_vertex_program(1_1), ARB_shadow, D3D9 gamma rules, libgcm semantics from the PSL1GHT headers. | Instruction semantics, CC rules, clamps, packing |
| **T3: the game itself** | The game's code and data: an instruction whose meaning forces a rule (for example `R1 - sat(R1)`), constants the game computes. The primary source is OUR OWN guest memory (our SPU float path is bit-exact). RPCS3's CPU-side memory (`tools/rpcs3_probe/rpcs3_mem.py`) is safe only for data loaded from disc or produced by integer code: its defaults are approximate for floats (relaxed XFloat, NJ, unfused VMX, 80 MHz timebase). | Disambiguating RSX-only encodings; checking constants |
| **T4: hardware-tested reverse engineering** | RPCS3 source comments that say "hw test"/"realhw", envytools/nouveau. | RSX-only fields with no public spec |
| **T5: RPCS3 behaviour or output** | What RPCS3 renders or computes. | A sanity cross-check only. It never decides a pass or a fail. |

### 1.2 Rules for every fix and every verify step

1. **Cite the tier.** A change or NOTES entry names its evidence: a URL plus section, a file:line, or a measurement id. "RPCS3 does it this way" counts as T5, which is not evidence.
2. **Two-evidence rule for RSX-only semantics.** Some fields are RSX-only: source precision modifiers, `SRC2.perspective_corr`, the texture crossbar and per-format presentation, `SHADER_PACKER`, signed blend modes and XRGB quirks. Implementing one needs a T4 hardware-test note and a T3 title instruction or data whose meaning requires it. If both are not available, put it behind an env toggle, default it off and record it as an open question.
3. **Two layers of verification.**
   - **(a) Mechanism.** A unit test encodes the T1/T2/T4 rule (`libs/video/tests`, `spec_audit_test.c`). Per-draw logs or captures must show the mechanism changed, for example "unit 5 now binds the 1024x8 X16 lookup".
   - **(b) Picture.** The scene, measured with `tools/ground_truth/pngstat.py`, `tools/ground_truth/halo.py` and `tools/ground_truth/bandstep.py`, must be within tolerance of the T1b targets.
   - Both layers must pass. A picture-only pass can hide a compensating error, for example halving the game's 2.0 glow gain (`VPC_NORM`).
4. **Precedence when tiers conflict.** Footage wins on the look; specs and tests win on the mechanism. If a spec-correct change makes the picture worse, there is a second bug. Find it; do not revert the spec fix.
5. **A/B toggles.** Every semantic change gets an env switch (as with `FP_TC89`, `FP_BX2`, `IMM_FIX`). The verify step records the per-toggle delta on the standard scenes.
6. **No silent fallbacks.**
   - Unknown formats, opcodes, methods or remaps must be logged. Debug builds should abort.
   - Never keep the previous texture, and never sample a NULL SRV quietly.
   - The stale-texture bug below came from exactly such a fallback.
7. **Use RPCS3 only as a labelled side-by-side.** Record where RPCS3 is wrong (section 10). Never write "RPCS3 renders this correctly" in a prompt or a pass criterion.
8. **Licence.** RPCS3 is GPL-2.0 and this project is MIT. Use hardware outputs and documented facts. Re-derive tables from dumps; do not copy GPL code.

### 1.3 Standard verification scenes

Each scene is listed with its hardware reference in `targets.json`.

| Id | How to reach it | Hardware reference |
|---|---|---|
| PIER | `tools\run.ps1 -Step 11 -Script "30-520:128,0,128,128"`: the post-Beast walk under an orange sky, with the ship on the right. | LACRY 3:39, RELLINGS 4:05, FRW 3:20 |
| LAMPS | Same walk, lamps against the sky. | FRW 3:44 radial profiles |
| BOSS | Beast fight, the legitimate bloom maximum. | LACRY 5:45, 6:40, 7:00 |
| SUNSET | Pre-Beast cutscene with a magenta sky, used for sky banding and backlit skin. | LACRY 2:30 to 3:02 |
| BOAT | New Marais arrival cutscene (Cole, Kuo, Zeke). The run `run-20261015-045003` reached a cutscene with Zeke and Kuo from checkpoint `03_forward_momentum_rpcs3`. | LACRY 10:33 (t=633), FRW t=603.5 |
| BLUESKY | Cole under a blue evening sky. | LACRY 30:00 |
| DAY | New Marais facade and rooftops. | LACRY 25:40, 26:30 |

Measure on full-resolution 1280x720 frame dumps (`CELLMARK_DUMP`). The READY screenshots are 960x540 and average banding away. Pick regions by content in our camera framing, because the camera position differs from the captures. Trust ratios and percentiles more than absolute levels; the capture chains differ by about 10%.

---

## 2. Spec and source index

**Specs (T2)**
- NV_fragment_program: https://registry.khronos.org/OpenGL/extensions/NV/NV_fragment_program.txt. Relevant sections: 3.11.1.1 (f[WPOS], reduced-precision COL0/COL1); 3.11.4.4 (CC update: "the corresponding condition code component is unchanged" where the mask fails); 3.11.5.22 PK4B and 3.11.5.43 UP4B (excess-128); the Issues section on fx12 range [-2,+2), NaN to 0.0 for fx12, and fp16 overflow to ±INF.
- NV_fragment_program2: https://registry.khronos.org/OpenGL/extensions/NV/NV_fragment_program2.txt (NRM, DIV, DIVSQ, LOOP/REP, indexed inputs [A0+n]).
- NV_vertex_program: https://registry.khronos.org/OpenGL/extensions/NV/NV_vertex_program.txt. 2.14.1.5: "The selected primary and secondary colors for each primitive are clamped to the range [0,1]." 2.14.1.9: out-of-range relative reads return (0,0,0,0). 2.14.1.10.x covers RSQ, EXP and LOG. 2.14.1.11 is the 0*x=0 rule.
- NV_vertex_program1_1: https://registry.khronos.org/OpenGL/extensions/NV/NV_vertex_program1_1.txt (RCC: sign kept, |r| in [5.42101e-20, 1.84467e19]).
- ARB_shadow: https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shadow.txt (depth compare; NV hardware filters the compare result, i.e. PCF).
- D3D9 gamma: https://learn.microsoft.com/en-us/windows/win32/direct3d9/gamma (sRGB applies to RGB only; DX9-class hardware may linearise after filtering).
- SPU ISA v1.2: https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/SPU_ISA_v1.2_27Jan2007_pub.pdf. §9.1 single precision (truncation, no denormals, no Inf/NaN); §9.2 double precision (IEEE, round mode taken from the FPSCR, default nearest).
- Cell BE Handbook v1.11 (§20.2.1, the 128-byte reservation granule), CBEA 1.01 and VMX PEM 2.07c (§3.2.1.2 non-Java mode).
- PSL1GHT headers (libgcm, lv2 fs, audio, sysutil).
- 64-bit PowerPC ELF ABI Supplement 1.9: https://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi-1.9.html. Section 3.2.3: floating-point arguments go in f1-f13 and still take their parameter doubleword, so the GPR of that slot is skipped (Figure 3-18: c in r3, ff in f1, d in r5); the parameter save area is at SP+48, the ninth doubleword at SP+0x70. Section 3.2.4: float and double results are returned in f1.

**Hardware data (T1a)**
- ps3autotests: https://github.com/RPCS3/ps3autotests/tree/master/tests/cpu. The `spu_fpu`, `ppu_fpu` and `ppu_vpu` `.expected` files are real-PS3 output. There are no public shader or texture vectors; the GPU tests there cover only PFIFO, RAMHT and MMIO.
- RPCS3 PR #19410 (https://github.com/RPCS3/rpcs3/pull/19410), "verified across around 700 unique tests". Commit fbf1f96 says the order is "sRGB -> ROUND -> ALPHA_TEST -> BLEND" and that blending does not run in linear space. Commit 5bc2c0d says DST_ALPHA reads 1 on every XRGB8 format. Other commits cover the RGB565 byte order, 16-bit blend constants and signed blend modes.
- RPCS3 #19783 and #19795 (VMX estimate tables and the VSCR dump), #7135, #7874 and #7798 (PPU FMA, fres and frsqrte), #11935 (sceNpTrophy read-only contexts).

**Footage (T1b)**: `tools/ground_truth/targets.json`.
- LACRY: https://www.youtube.com/watch?v=7J0JZJeTRoQ (Elgato).
- RELLINGS: https://www.youtube.com/watch?v=pUNWGwCDx5o
- FRW: https://www.youtube.com/watch?v=V3d9r2KKsL4 (HD PVR 2).
- Backup 2011 launch-week captures are listed in that file.

**Reverse engineering of the title (T3/T4)**: the RPCS3 fork https://github.com/ttmx/rpcs3-infamous2 (EU disc v1.04, so addresses differ from ours; the structure should match).

**RPCS3 sources (T4 notes only)**: RSXFragmentProgram.h, FragmentProgramDecompiler.cpp, RSXThread.cpp and RSXZCULL.cpp, plus RSXTexture.cpp, GLTexture.cpp, rsx_decode.h and GLVertexProgram.cpp on GitHub master.

**envytools rnndb** (nouveau, desktop NV40, T4): https://github.com/envytools/envytools/blob/master/rnndb/graph/nv30-40_3d.xml. It names 0x1d88 COORD_CONVENTIONS (height 0-11, origin bit 12, centre bit 16), 0x17cc QUERY_ENABLE, 0x1800 QUERY_GET (offset 0-23) and 0x1a10 TEX_SWIZZLE. It does not describe 0x1fec, which is RSX-specific.

---

## 3. Fragment program (NV40/RSX) semantics

### 3.1 Encoding fields our decoder must honour

Bit positions are given per word. The field order was checked against RPCS3's `RSXFragmentProgram.h` and matches `rsx_fp_decompiler.c`.

| Field | Where | Meaning | Tier | Ours (`rsx_fp_decompiler.c`) |
|---|---|---|---|---|
| Destination saturate | OPDEST bit 31 | Saturate the result after the scale. | T2 | OK (line 1016) |
| Result scale | SRC1 bits 28-30 | 1/2/3 = x2/x4/x8; 5/6/7 = /2, /4, /8; applied before saturate. | T4 | OK (lines 1024-1026) |
| Destination precision | OPDEST bits 22-23 | 0 = fp32, 1 = fp16, 2 = fx12, 3 = fx9. RPCS3 exempts NRM, MIN, MAX, COS, SIN, REFL, FRC, LIT, LIF and LG2, and a MOV from an fp16 temp to fp16. | T2 (NV_fp conversions; NV_fp_option: X overflows to -2048/1024 or +2047/1024) + T4 | **Done** for fx12 (`FP_DSTPREC`, default 0x04): `rsx_fx12` = NaN to 0, clamp to [-2048/1024, 2047/1024]; set-on results (0/1) and packs exempt. fx9 is T4 only and unused by the title: `FP_DSTPREC=0x0C`. fp16 precision into an R register stays fp32 (NV_fp_option: "no less than" fp16). |
| H-register storage | OPDEST bit 7 | An H destination is stored as fp16 (10-bit mantissa, overflow to ±INF). | T2 | **Done** (`FP_HROUND`, default 1): every H write stores `f16tof32(f32tof16(x))`, packs exempt. WARP's f32tof16 rounds toward zero and saturates a finite overflow at 65504 (measured; the GPU's own rounding was not measured); `FP_HROUND=2` adds the overflow to ±INF. A C-suffixed H write sets the CC from the stored fp16 value (3.4). Cost over the title's 322 programs: +41% FXC instruction slots (=2: +104%). |
| exp_tex (_bx2) | OPDEST bit 21 | Texel expanded as 2c-1, sRGB decode suppressed. | T4 | OK (`FP_BX2`) |
| **Source precision modifier** | SRC1 bits 19-21 (src0), 22-24 (src1), 25-27 (src2) | 0 = none, 1 = fp16, 2 = fx12 clamp [-2,2], 3 = fx9 clamp [-1,1], 4 = saturate [0,1], 5 = no-op on hardware. Order: saturate is applied **before** abs; fp16/fx12/fx9 are applied **after** abs; negate is applied last. Mode 1 on an fp16 temp is a no-op. COL0/COL1 and SSA ignore the modifier. | T4 (RPCS3 `GetSRC`/`ClampValue`: mode 5 is a no-op "from hw tests"; the order note says "tested with Naruto UNS", a game test, not a hardware test; the COL0/COL1/SSA exemption is RPCS3 code only) + T3 (god-ray `ADD R2.zw, R1, -R1 [src1 sat]`) | **Done** for saturate (`FP_SRCPREC`, default 0x10), which has both evidences (T3 also: C2EBEA7F `SNE H0, H0, H0[sat]`). No title instruction combines a saturated source with abs, so the order does not reach the title. COL0/COL1/SSA reads take no modifier (no title instruction has one); a branch instruction's DWORD2 is not read as modifiers. fx9 (3 instructions, all `-|cos|[fx9]` with |cos| <= 1), fx12 and fp16 (unused) have no title instruction that needs them: off, `FP_SRCPREC=0x1E` enables all. Modes 6/7 are logged. |
| Source abs | SRC0 word bit 29 / SRC1 word bit 18 / SRC2 word bit 18 | abs | T4 | OK |
| **perspective_corr** | SRC2 bit 31 | The input TEXn is multiplied by 1/w_clip (GL `gl_FragCoord.w`). | T4 (RPCS3 PR #10764: "multiplies TEX0-TEX9 by 1/w", found by fuzzing SRC2 bits; that it fixed RPCS3's inFamous 2 demo colours, #7721, is T5 context, not evidence) + T3 (39 programs use it straight as a lookup coordinate; the VPs pre-multiply those coordinates by clip w, e.g. 12C68310 `R6.xy = ndc * c466.xy + c466.zw; MUL o8.xyz, R0.wwww, R6`, 125CC02B o14) | **Done** (`FP_PERSP`, default on): `input.tcN * rsx_iw`, `rsx_iw = 1.0 / input.position.w`. The TEX_COORD_CONTROL 2D override is not decoded. |
| use_index_reg / addr_reg | SRC2 bit 30 / bits 19-29 | Input TEXn indexed by the loop counter [A0+n] (NV_fp2). | T2 | Missing; the title does not use it |
| Condition code | SRC0 bits 18-31, OPDEST bit 8 | exec_if LT/EQ/GR, a swizzle, the CC0/CC1 select. | T2 | OK; the CC update rule is fixed (3.4, `FP_CCRULE`) |

**Implementation note for perspective_corr on D3D12.** In a D3D pixel shader, `SV_Position.w` holds the clip-space **w**, not 1/w. Dawn/Tint inserts `1.0f / position.w` on HLSL targets for this reason: https://dawn.googlesource.com/dawn/+/34eee4e680119d3e3c8950bf8c81423813bd550d%5E%21.
- RPCS3's `reg * gl_FragCoord.w` therefore becomes `input.tcN * rcp(input.position.w)` in our HLSL, or a 1/w varying passed from the VS.
- Writing `* input.position.w` would square the error instead of removing it.
- With the TEX_COORD_CONTROL 2D bit set (method 0x0B40+4n), RPCS3 gives `(xy/w, 0, 1)` (T4). Whether the game sets it is unproven: 9 of the 10 EBOOT immediates 0x0B40 are `addi` struct offsets and one `ori` sits in libgcm's texture writer (critique, `methsites.py`). See section 11, question 9.

### 3.2 Input registers

| Input | Hardware value | Tier | Ours |
|---|---|---|---|
| 0 WPOS | Window position, w = 1/w. Origin and centre come from SHADER_WINDOW 0x1D88. | T2/T4 | SV_Position by default; no title FP reads it (0 of 322). `FP_WPOS=1`: x, y in SHADER_WINDOW's origin and centre (b1 `fp_window`), w = 1/w (5.2). |
| 1/2 COL0/COL1 | **Clamped to [0,1].** The spec clamps per vertex at the VS output (NV_vp 2.14.1.5); RPCS3 approximates this with a saturate at the FP read. | T2 | **Done** (`VP_COLSAT`, default 1): `saturate` in the VS epilogue, before interpolation. `VP_COLSAT=2` clamps at the FP read instead (RPCS3's placement), 0 = off. |
| 3 FOGC | Raw | T4 | OK |
| 4-11 TEX0-7 | Raw (2D override: z=0, w=hpos.w; point-sprite override) | T4 | Raw; the 2D override is missing |
| 12/13 TEX8/TEX9 | TEX8 comes from VP o15: Mesa nouveau NV40 `NV40_VP_INST_DEST_TC(n)` = 7 + n with ten texcoords on nv4x (`nv40_vertprog.h`, `nvfx_vertprog.c`), enabled by VP_RESULT_EN bit 12 = `0x1000 << (n - 8)` (`nvfx_fragprog.c`), the bit PSL1GHT names GCM_ATTRIB_OUTPUT_MASK_TEX8; RPCS3's reg_table agrees. TEX9: RPCS3 reads VP o6 (point size, user clip 3-5 in .yzw), nouveau output 16 -- the two T4 sources disagree. Both are gated by VERTEX_ATTRIB_OUTPUT_MASK (0x1FF4) bits 12/13. | T4 + T3 (TEX8: the tint VPs write c260/c261/c262 to o13/o14/o15 for FPs that blend t.x*TEX6 + t.y*TEX7 + t.z*TEX8; the two-colour VPs write o13 + o15 only, for FPs that read TEX6 + TEX8 only; 20 of 250 VPs write o15, none writes o6 or o16; 30 of 322 FPs read TEX8, none reads TEX9) + T1b (gate lanterns cream-white, LACRY t=219-226; yellow with TEX8 = 0) | **Done** for TEX8 (`FP_TC89`, default 1 since 2026-10-15; 0 = off). TEX9 stays opt-in (`FP_TC89=2` reads o6; with the optional `VP_TEX9=16`, o16): no title use, open question 13. The mask gates both in the VS (`VP_OUTMASK`, default on, via `vp_posoffset.w`) |
| 14 SSA | +1 front face, -1 back face | T4 | OK |

### 3.3 Opcodes

- **Correct:** MOV, MUL, ADD, MAD, DP3, DP4, DP2, MIN, MAX, FRC, FLR, RCP, RSQ (of |x|), EX2, LG2, SIN, COS, POW, DIV, DIVSQ, LRP, LIF, SLT/SGE/SLE/SGT/SNE/SEQ, KIL (any() of the swizzled CC), DDX, DDY, TEX, TXP (2D), TXB, TXL, PK4UB/UP4UB (x in bits 0-7), PK2H/UP2H, PK2US/UP2US, PKB/UPB, PKG/UPG, scale, _SAT.
- **Wrong or missing, but unused by the title:**
  - PK4B/UP4B. The spec uses excess-128, `ub = round(127*clamp(x,-128/127,127/127)+128)`; ours uses two's complement.
  - DST = (1, a.y*b.y, a.z, b.w).
  - LIT = (1, max(x,0), x>0 ? pow(max(y,0), w) : 0, 1).
  - RFL, STR = 1, SFL = 0, DP2A = dp2 + c.x, TXD, and the BEM family.
  - These are in the census and in `spec_audit_test.c` FP-7.
- **TXP with a depth-format texture and a zfunc set** is a hardware shadow compare (3.6 and section 4). 28 programs compute the reference z just before a `TXP Hn.x`.
- **NRM** writes .w = 1 in ours, while NV_fp2 leaves it undefined. Keep it, but guard against the zero vector.

### 3.4 Condition-code update

NV_fp 3.11.4.4: a CC component is updated only where the write mask and the conditional-write test both pass. The other components keep their **old CC**.
- Ours (lines 1067-1077) copies the post-select destination, so a failing component receives the old *destination* value.
- Fix: a per-component select, `ccN.m = _p.m ? _v.m : ccN.m`, and the same for the destination (lerp turns an INF or NaN on either side into NaN). "FL" writes neither.
- The CC component is generated from the value the destination component now holds: "each component of the destination register enabled for writes is compared to zero", and the pseudo-code types it `GenerateCC(DstT value)`. So it follows the fx12 conversion and, for an H destination, the fp16 storage. Without a destination (RC) it is the converted result.
- **Done** (`FP_CCRULE`, default on; 0 = the old lerp and destination copy).

### 3.5 Exports

- Colour comes from h0 by default and from r0 under 32-bit exports (SHADER_CONTROL). Depth comes from r1.z, saturated.
- The NaN guard `(_o == _o) ? _o : 0` is **per component** under FXC/SM5, because the HLSL ternary is component-wise. That matches the D3D rule that NaN becomes 0 per channel. (The specaudit pass reported the opposite, that a NaN zeroed the whole target. That is incorrect.)
- What remains is bit-exact transport. A lane last written through the other register file (31 programs, e.g. `PKG` into h4) goes through `f16tof32`/`f32tof16`. NaN/Inf bit patterns are therefore lost on fp16 targets.

### 3.6 Texture instructions: what the shader sees

The value a TEX returns is format presentation, then the crossbar remap, then optional sign/gamma/bx2 (per channel), then a compare if the format is depth and zfunc is set. See section 4.

---

## 4. Textures

### 4.1 Format codes and presentation

Format codes follow `gcm_enums.h`; the LN (0x20) and UN (0x40) flags are stripped.

"Presented ARGB" is what the crossbar indexes (source 0 = A, 1 = R, 2 = G, 3 = B). It comes from RPCS3's `GLTexture.cpp` `get_swizzle_remap`, which is written in ARGB order and is T4. "X" and "Y" are the format's channels.

| Code | Format | Bytes/texel | Presented ARGB | Remap rule | Ours today |
|---|---|---|---|---|---|
| 0x81 | B8 | 1 | (1, B, B, B) | normal | Bound. The prepass forces mapping 0x1000 = (B,B,B,B), so A is wrong and the remap is ignored (backend 6334, 8575). |
| 0x82 / 0x83 / 0x84 | A1R5G5B5 / A4R4G4B4 / R5G6B5 | 2 | (A,R,G,B) / (1,R,G,B) | normal | **Dropped** at bind |
| 0x85 | A8R8G8B8 | 4 | (A,R,G,B) | normal | OK |
| 0x86-0x88 | DXT1 / DXT23 / DXT45 | block | (A,R,G,B) | normal | OK |
| 0x8B | G8B8 | 2 | (G,B,G,B) | normal | OK (`lanes_g8b8`) |
| 0x8D / 0x8E | COMPRESSED_B8R8_G8R8 / R8B8_R8G8 | 2 texels per 32 bits | 4:2:2-style | normal | Wrong layout (TX-1) |
| 0x8F | R6G5B5 | 2 | (1,R,G,B) | normal | Wrong layout |
| 0x90 / 0x91 | DEPTH24_D8 / _FLOAT | 4 | (d,d,d,d), or the compare result when zfunc is set | n/a | **Dropped**; no compare |
| 0x92 / 0x93 | DEPTH16 / _FLOAT | 2 | as above | n/a | **Dropped** |
| 0x94 | **X16** (unorm, big-endian u16) | 2 | (X, 1, X, 1) | **16-bit-block rewrite** | **Dropped**. With the whitelist removed, our crossbar would read constant 0 (TX-2). |
| 0x95 | **Y16_X16** (unorm) | 4 | two channels, (G,R,G,R) in RPCS3's upload naming | 16-bit-block rewrite | **Dropped**; A=1, B=0 (TX-4) |
| 0x98 / 0x99 | HILO8 / HILO_S8 | 2 | (R,G,R,G) | 16-bit-block rewrite | Wrong layout |
| 0x9A | W16_Z16_Y16_X16_FLOAT | 8 | (A,R,G,B) | **crossbar low byte forced to 0xE4** | Bound; crossbar applied (TX-5) |
| 0x9B | W32_Z32_Y32_X32_FLOAT | 16 | (A,R,G,B) | forced 0xE4 | **Dropped** |
| 0x9C | X32_FLOAT | 4 | (X,X,X,X) | forced 0xE4 | **Dropped**: the SSAO 640x360 half-res depth goes stale |
| 0x9D / 0x9E | D1R5G5B5 / D8R8G8B8 | 2 / 4 | (1,R,G,B) | normal | 0x9E decodes; dropped at bind |
| 0x9F | Y16_X16_FLOAT | 4 | (R,G,R,G) | low byte forced to 0x66, or 0x56 with an override | **Dropped** |

**16-bit-block remap rewrite** (RPCS3 `RSXTexture.cpp` `decoded_remap`, T4). For X16, Y16_X16, HILO8 and HILO_S8, the low byte of CONTROL1 is rewritten before decoding:

| Low byte | Override (bits 16-31) = 0 | Override ≠ 0 |
|---|---|---|
| 0xE4 | 0x66 | 0x56 |
| 0x4E | 0x99 | 0xA9 |
| 0xEE | 0xAA | 0xAA |
| 0x44 | 0x55 | 0x55 |
| any other | unchanged | unchanged |

So inFamous 2's X16 lookup with remap 0xAAEE presents **X in all four lanes**.

The title intent (T3) confirms this. The 1024x8 X16 is a specular-power lookup:
- 8B314D2A op 38 reads `TEX4 .x` at (sat(N·H), gloss*0.5+0.5).
- The skin program 632599C5 reads `TEX5 .x` at (sat(N·H), 0.281).
- A lookup that read 0 would remove all specular and glow.

**Crossbar (CONTROL1 low 16 bits).**
- Bits 0-7 are four 2-bit source selects for outputs A, R, G, B (LSB first).
- Bits 8-15 are four 2-bit ops: 0 = force 0, 1 = force 1, 2/3 = take the source.
- Ours (`rsx_texture_layout.c` 470-492) implements this for ARGB and G8B8 only.

### 4.2 Sampler state

| Field | Hardware | Ours |
|---|---|---|
| TEXTURE_ADDRESS bits 28-31 | zfunc. With a depth format, this enables the compare with bilinear PCF of the result. | Ignored; `ComparisonFunc` is NEVER (backend 7794) |
| TEXTURE_FILTER bits 28-31 | Per channel (A,R,G,B), two's-complement signed reinterpretation to [-1,1]. | Logged only (backend 4160) |
| CONTROL0 bit 2 | Texture alpha kill | Missing |
| TEXTURE_BORDER_COLOR 0x1A1C+0x20n | Border colour for BORDER modes (the game sets it at 4 sites) | Zero border colour |
| CLAMP, MIRROR_ONCE_* | GL CLAMP and mirror-once modes | Become edge clamp |
| Format bits 4-7, CONTROL3 bits 20-31 | 3D texture dimension and depth | Ignored |
| LOD bias, min/max LOD, anisotropy | Signed 13-bit /256, 4.8 fixed point | OK |
| Gamma bits (FORMAT) | sRGB decode per channel; post-filter is acceptable on DX9-class parts | OK |

### 4.3 Textures resolved to a render target

Hardware reads the bytes in memory using the *texture's* format and remap. Ours binds the RT resource with the default mapping (backend 8543-8548; zeta alias 8440-8456), so format reinterpretation and remap are lost.

The game's known reinterpretations, from the ttmx RE:
- G-buffer A as A8R8G8B8: gloss byte plus normal.
- G-buffer B (D24S8 bytes: depth high to low, then stencil).
- The SSAO C1 640x360 X32_FLOAT.
- The AO 1280x720 8-bit image.
- The 320x180 DOF/bloom image aliasing the normals address.
- 0xCF770000, where a 512x288 particle depth shares its address with a 256x256 shadow map.

---

## 5. Surfaces, ROP and other RSX state

### 5.1 ROP order and blending (T1a: the PR #19410 hardware suite)

1. If **SHADER_PACKER** (method **0x1FEC**) is nonzero, colour exports are converted linear to sRGB, **RGB only**.
   - Only on integer (8-bit) surfaces, and not under 32-bit exports.
   - RPCS3 applies it to every enabled MRT.
   - The game toggles it: `li r4,0x1FEC` at 10 sites through the set-method helper 0x5041C0. The value is 1 at 0xD887C, 0xD8FE8 and 0x2E9AE8, and 0 elsewhere. 0x2E9910 is a generic material draw keyed on a material flag; 0xD7FDC holds full-screen passes with depth test off and stencil EQUAL 128. Our command decoder has no case for it.
2. Round the output to 8 bits.
3. Alpha test. RPCS3 notes this placement was inferred from game observations.
4. Blend **on the stored bytes**, never in linear space. An `_SRGB` RTV would blend in linear space, so it is wrong here.
5. Output remap (B8, G8B8, XRGB) and the colour mask.

Blend equations are packed with RGB in the low 16 bits and alpha in the high 16.

| Equation | Hardware |
|---|---|
| 0x8006 FUNC_ADD, 0x800A SUBTRACT, 0x800B REV_SUBTRACT | Standard; the source is saturated first |
| 0x8007 MIN / 0x8008 MAX | Factors ignored; `min/max(sat(src), dst)` |
| **0xF005 REVERSE_SUBTRACT_SIGNED** | `dst - s8(src)` |
| **0xF006 ADD_SIGNED** | `s8(src) + dst` |
| **0xF007 REVERSE_ADD_SIGNED** | `sat(src) + s8(dst)` |

- Here `s8(v)` means: take `u8 = round(sat(v)*255)`, reinterpret it as a signed byte, then divide by 255.
- The signed equations ignore the blend factors. On fp16 targets they are the plain add/subtract (T4 only).
- The result is rounded to 8 bits. Wrap or clamp is open: "integer wrap" is issue #15061's reading of Vanillaware games (a game observation, not a test), and RPCS3's #19410 code clamps (`_blend_saturate_signed`; its code is T5, only the suite's recorded outputs are T1a). Ours clamps (D3D12 fixed function cannot wrap); an offset buffer around 128 reaches neither.
- inFamous refraction depends on these modes (RPCS3 #11149). **T3, the title:** its SetBlendEquation inline (func_00501680: 0x320 = eq << 16 | eq) is called with 0xF006 at 6 sites (0x0E0E7C, 0x2AEFE4, 0x2B6650, 0x2EC8F4, 0x362B54, 0x4274B0), each followed by a reset to 0x8006; five of them (all but 0x2AEFE4, whose depth state is set by its caller) turn DEPTH_TEST_ENABLE (0xA74) and the depth mask (0xA70) off around the draw; four pass ONE/ONE, two SRC_ALPHA/ONE_MINUS_SRC_ALPHA (which the mode ignores). No 0xF005/0xF007 site. Census: `py -I tools/ground_truth/blendsites.py generated/ppu --summary` (17 equation calls: 10 x 0x8006, 1 x 0x8008, 6 x 0xF006; 5 BLEND_FUNC_SEPARATE sites take the alpha as `(1 - DST_ALPHA) * src.a + dst.a`, which reads the XRGB rule below).
- **Done** (`BLEND_SIGNED`, default 1; `rsx_rop.h`): two draws per signed op. Pass P exports `max(s8, 0)` with ADD (0xF006) or REV_SUBTRACT (0xF005), pass N `max(-s8, 0)` with the other op, writing only the signed channels of the blended targets; per fragment that is `clamp(dst +- s8(src))` exactly (WARP, all 65536 pairs). Depth and stencil (`rsx_rop_fill_ds`): pass P keeps the guest's depth test and depth writes but writes no stencil; pass N writes no depth (LESS/GREATER relaxed to LESS_EQUAL/GREATER_EQUAL, NOT_EQUAL to ALWAYS, only where P wrote depth) and applies the guest's stencil ops against the stencil P saw, so both passes cover the same fragments and each buffer is updated once (WARP W10-W13: stencil INCR, LESS without writes at equal depth, LESS and NOT_EQUAL with writes). The negative pass is drawn outside any per-draw occlusion query. Overlapping primitives of one draw land all positive parts first, so they differ from primitive order only where an intermediate sum saturates (or, with depth/stencil writes, where one primitive hides or re-stencils another). 0xF007 is drawn as ADD and logged (`[BLENDSGN]`).

Other ROP facts:
- **SRC_ALPHA_SATURATE** = min(src.a, 1 - dst.a). D3D's SRC_ALPHA_SAT is the same.
- **XRGB8 surfaces** (X8R8G8B8_Z8 = 4, X8R8G8B8_O8 = 5; by inference also X1R5G5B5 1/2 and X8B8G8R8 14/15): DST_ALPHA reads 1 whatever the Z/O tag. The stored X byte is 0x00 (Z) or 0xFF (O). Opt-in (`ROP_XRGB=1`: no title use proven; `[ROPFMT]` logs the first draw into each colour format): DST_ALPHA factors become ONE/ZERO and the exported or blended alpha is forced to the X value; an O target blended with source-alpha colour factors keeps its stored alpha (logged).
- **16-bit blend constants** are fp16, per the #19410 commit "Fix decoding of 16-bit blend constants". Mesa nouveau (T4) writes R | G << 16 to BLEND_COLOR and B | A << 16 to BLEND_COLOR2 (0x037C) for fp16 targets, then rewrites BLEND_COLOR as ARGB8, so the layout is open. Opt-in `BLEND_C16=1`; `[BLENDC16]` logs every constant-factor draw on an fp16 target. The title sets BLEND_COLOR at one site (0x41EA50) and never BLEND_COLOR2.
- **CONSTANT_ALPHA in a colour factor** needs D3D12_BLEND_ALPHA_FACTOR when the draw also uses CONSTANT_COLOR (the title's 0x41B17C: CONSTANT_COLOR / ONE_MINUS_CONSTANT_ALPHA). **Done** (`BLEND_KALPHA`, used when the device reports `AlphaBlendFactorSupported`; WARP does).
- **Per-MRT blend enable**: SET_BLEND_ENABLE_MRT is **0x036C** (not 0x012C), bits 1-3 for MRT1-3. T2 PSL1GHT `rsxSetBlendEnableMrt`; T4 Mesa nouveau `NV40_3D_MRT_BLEND_ENABLE`; T3 the title's func_005014F0 writes 0x310 = m & 1 and 0x36C = m & 0xE, with m = 0x3, 0x9, 0x8, 0x1, 0x0 (0x8: only MRT 3 blends). **Done** (`BLEND_MRT`, default 1; 0 = every MRT mirrors target 0).
- **No blending on fp32 surfaces.** **Done** (`BLEND_F32`, default 1, `[ROPF32]`).
- **Source rounded to 8 bits before the blend** (step 2): **Done** for blended 8-bit targets (`ROP_ROUND8`, default 1). WARP already rounds the source to the target format, so there it changes nothing.
- **B8 (9) and G8B8 (10) targets** store only B, or G and B. RGB565 and X1R5G5B5 have their own byte order and quantise to 5/6 bits. Not done, and nobody is working on it yet: all of them are RGBA8 resources (a B8 target is seeded and read back at 4 bytes per pixel). `[ROPFMT]` shows whether the title draws into them; if it does, native R8/R8G8/B5G6R5/B5G5R5A1 targets need the seed, readback and RT-view paths changed together.
- **Glow mask**: `[ROPLOG]` prints the full blend state (both factor pairs, equation, MRT enables, colour mask, alpha test, packer) of the first 4 draws of 8B314D2A and 1572A3B3 (`ROP_LOG=<n>`, `ROP_LOG_ID=<ids>`).
- **Dithering**: the game writes DITHER_ENABLE (0x0300) = 0 through the helper at 0x133284, so missing dithering is not the cause of the banding.

### 5.2 Viewport, scissor, window

The rules live in `libs/video/rsx_raster.h`; the mechanism test is `libs/video/tests/test_raster_warp.c` (D3D12 WARP).

- **Transform** (`RASTER_XFORM`, default on). VIEWPORT_SCALE/OFFSET x and y (0x0A30/0x0A20) are the window transform: window = ndc * scale + offset, fractions included, rows counted down from row 0.
  - T2: PSL1GHT `rsxSetViewport` writes them as given. T4: Mesa nouveau `nv30_validate_viewport` writes them unrounded and derives the integer VIEWPORT_HORIZ/VERT from them.
  - T3: the title's viewport writer (EBOOT 0x501C90, called from 0x20B28C and 0x2E7C68) builds offset (x + w/2, y + h/2) and scale (w/2, -h/2) from an unclamped integer rect (y flipped from its GL-style rect: y = H - y1). Its scissor (0x501DF0, via 0x20B16C) is the same rect clamped to the surface and flipped the same way (y = H - min(H, y1)), and zero-sized (x 0, y H, w 0, h 0) when the rect is off it; the other scissor writers (0x2D93A4, 0x2E7C8C) are top-down too. A negative x lands in the low 16 bits of VIEWPORT_HORIZONTAL (0xFFF0 for -16): only the floats carry such a viewport.
  - **Done** (2026-10-15). Before, the D3D viewport was the integer rect, or the full target when the rect left it. Now (`rsx_raster_host_map`) a window rect [o - |s|, o + |s|] with integer edges -- every libgcm transform, so every draw of the title -- is the D3D viewport itself, a negative origin or one past the target included, and the VS epilogue only mirrors an axis whose scale has the other sign. For a rect inside the target that is the old D3D state bit for bit (test "libgcm_equals_old_path", "host_map_libgcm_is_old_viewport"), so the title's ordinary draws do not change. A fractional edge, a rect past D3D12's viewport range, or `RASTER_VPCLIP` 0/2 takes the frame as the viewport and the whole map in the epilogue (route 2): a fractional D3D viewport is not the pixel-centre footprint on WARP (it drops the row between a fractional bottom edge's integer part and the edge), and route 2 is not bit-identical to the old viewport for an integer rect (WARP, 1280x720: 76 edge pixels in 192 triangles, interpolants off by up to 3e-4), which is why it is kept for the cases that need it.
  - The transform's own pixel clip is the image of the NDC square (`RASTER_VPCLIP=1`, the GL/D3D clip volume, T2). Exact clipping or guard band plus the integer rect is open (question 14): `RASTER_VPCLIP=2` uses the rect, `0` neither.
- **Scissor** (`RASTER_SCISSOR`, default on). SET_SCISSOR (0x08C0/0x08C4) and the surface clip (0x0200/0x0204) clip every draw; a zero-sized scissor draws nothing. Before, the guest scissor was never read. T2: GL 2.1 4.1.2. T4: nouveau `nv30_validate_scissor` (0x10000000 = off). T3: the title's per-target scissor above.
- **Clears** (`RASTER_CLEAR`, default on). CLEAR_SURFACE writes scissor & surface clip only; the viewport does not limit it. Before, the whole surface. T2: GL 2.1 4.2.3 (Clear applies the scissor test). T4: nouveau clears a sub-rectangle by programming SCISSOR_HORIZ/VERT before CLEAR_BUFFERS (`nv30_clear_render_target`, `nv30_clear_depth_stencil`), validates the scissor before a full clear (`nv30_clear`), and leaves VIEWPORT_HORIZ alone. The title's clear helpers (0x20B110, 0x20CBE0) set no scissor of their own; the only scissor writes are the target helper's (0x501DF0 is called from 0x20B2B0, 0x2D93A4 and 0x2E7C8C). A display colour clear that only seeds the frame-start backbuffer clear is not clipped (logged once per rect).
- **SHADER_WINDOW 0x1D88** (envytools COORD_CONVENTIONS: height bits 0-11, origin bit 12, pixel centre bit 16) sets what f[WPOS] reads, not where pixels land. T4: nouveau sets it from the program's TGSI coordinate properties and never adjusts the viewport for it. PSL1GHT `rsxSetSurface` writes origin bottom, and a libgcm transform such as the title's (scale.y = -h/2) puts NDC +1 on row 0, the top of the displayed image. Decoded and logged per value (`[RASTER] SHADER_WINDOW`; the title's own value is not known statically: its SetSurface copies a prebuilt block); f[WPOS] uses it under `FP_WPOS=1` (default off, rule 2). Its effect on DDY is open (question 15). WINDOW_OFFSET 0x02B8 is decoded and logged when nonzero, not applied.
- Logs: `RASTER_LOG=1` (default) prints each distinct draw state whose transform or pixels differ from the old path (`[RASTER] draw ... old: viewport ...`) and each clipped clear; `=2` adds counts every 20000 draws; `=0` is silent.

### 5.3 Occlusion reports

- **Enable and counting.** ZPASS_PIXEL_COUNT_ENABLE (0x17CC) enables counting. CLEAR_REPORT_VALUE (0x17C8) resets it.
- **GET_REPORT (0x1800)** carries `(type << 24) | offset`. Type 1 is the ZPASS count. It writes the big-endian `CellGcmReportData {u64 timestamp; u32 value; u32 pad}` to the report sink.
- **Report sink.** SET_CONTEXT_DMA_REPORT (0x1A8) selects where reports go:
  - REPORT_LOCATION_LOCAL (0x66626660) goes to the RSX report area: 2048 records.
  - REPORT_LOCATION_MAIN (0xBAD68000) goes to the IO-mapped address `iomap(0x0E000000 + offset)`.
  - The memory DMAs work too: HOST_BUFFER (0xFEED0001) = `iomap(offset)`, FRAME_BUFFER (0xFEED0000) = local memory + offset.
  - Source: RPCS3 `RSXThread.cpp` 190-212 (T4/T5, addresses only); the counter rule (cleared by CLEAR_REPORT_VALUE, counting while enabled, kept across GET_REPORTs) is Mesa nouveau `nv30_query.c` (MIT, T4) and the PSL1GHT inline commands (T2).
- **Hardware value.** Hardware writes the real sample count. RPCS3 writes 0xFFFF for any non-zero count unless "precise" is set, and its wiki tells players to disable the queries.
- **RENDER_ENABLE (0x1E98)** is conditional rendering on a report: 0x01000000 = always, 0x02000000 | offset = only while that report's value is non-zero (T2 PSL1GHT SetRenderEnable, T4 nouveau `nv40_query_render_condition`). The EBOOT holds no 0x1E98 immediate; it is implemented for completeness.
- **The title** (T3, EBOOT).
  - SET_CONTEXT_DMA_REPORT is 0xFEED0001 (HOST_BUFFER), set at 0x4EF5DC (init) and 0x1326B0 (state reset). Offsets 0x14080/0x14090 ("indices" 5128/5129) are therefore plain IO offsets, inside cellGcmInit's IO map (EA 0x40414080; our runs log `_cellGcmInitBody(... ioAddr=0x40400000)` and `[GET_REPORT] type=1 idx=5128`): not REPORT_LOCATION_MAIN, and no 0x0E000000 mapping is needed.
  - func_00131924 allocates the 0x80-byte record buffer (pointer at 0xA00990) and builds a texture over it: format 0x0001F52A (main memory, Y16_X16 | LN | UN), 4x2, pitch 16, point sampling.
  - func_000D066C calls func_00131A80 twice: CLEAR_REPORT_VALUE(1), colour and depth writes off, ZPASS enable, the 36-vertex box, GET_REPORT, ZPASS disable; the first depth-tested, the second with the depth test off (0xA74 = 0).
  - The game imports none of the getters and the GET_REPORT / CLEAR writers (0x501300, 0x501380) have one caller each (func_00131A80): this is the only query pair. No PPU read of the records is known; delivery before every label / semaphore / reference write covers one synchronised that way. func_000E43AC binds the texture and draws with fragment program 97197763A2494DB3: count = texel(2,row).x * 65535*65536 + .w * 65535 (the 32-bit value word as two 16-bit halves), glow = visible/total / (1 + 15833.8 r^2) * (0.169, 0.149, 0.135) at the sun's screen position (0.652, 0.825) in the captured instance, alpha 0: a small additive sun glow, at most (43,38,34)/255 at its centre, half at about 6 px.
  - Before this was implemented the records were never written: the title read whatever its allocator left in the buffer (not measured; the first `REPORT_LOG` lines print it). Zeros give total = 0, the CC test fails and the glow is 0 in every frame; exact counts add the glow wherever the sun is visible (brighter, around the sun only).
- **Ours.** Done (`rsx_report.c`, `rsx_d3d12_report.c`): a D3D12 occlusion query per counted draw (exact samples, one per pixel), segments summed per report, the big-endian record written into guest memory before anything can read it (the first draw sampling memory under it, a RENDER_ENABLE test, a label / semaphore / reference write). Toggles: `REPORT_EXACT=0` (old host array), `REPORT_SYNC=0` (measure at GET_REPORT), `RENDER_COND=0`, `REPORT_LOG=<n>` (each line prints what the memory held before, i.e. what the title read until now). Cost: one mid-frame flush and GPU wait per frame with a counted report, at the first point that can read it (the next label / semaphore / reference write or the consumer draw); CPU and GPU no longer overlap for that part of the frame. Not measured yet: compare fps against `REPORT_EXACT=0` (`RF_DBG=1` shows `reason=zpass-report`).

### 5.4 Sampling a bound render target

- The RSX reads textures through a read-only texture cache the title invalidates (INVALIDATE_TEXTURE_CACHE 0x1FD8: inline at 0x3327EC; libgcm's writer at 0x503880, called from 18 sites). How much of a surface that cache can still hold at a later fetch is neither documented nor measured; it is a few KB, a render target is MB.
- Ours used to give such a unit an SRV on the live render target with no transition: a D3D12 read/write hazard with undefined results (RPCS3 non-strict mode, its #18628 floor flicker).
- Done (`RT_SELFSAMPLE`, default 2): the unit samples a copy of the target taken right before the draw, i.e. memory as the draw starts. Every other texture here follows that rule too (an offscreen target sampled while unbound reads its latest contents, whatever the texture-cache epoch). `=1` keeps one copy per texture-cache epoch and frame: the cache hypothesis, RSX-only semantics with no T4 note, so opt-in by 1.2 rule 2. `=0` is the old live view, for A/B only. `[SELFSAMPLE]` lines name the draws and count the copies per frame (each is a full-target copy). Depth read as colour already went through a copy (ztex_convert).

---

## 6. Vertex program

- **COL0/COL1 (and BFC0/BFC1)**: saturate in the VS epilogue (NV_vp 2.14.1.5). This is spec-exact per vertex and better than RPCS3's FP-side clamp. **Done** for COL0/COL1 (`VP_COLSAT`); BFC0/BFC1 are not routed (no two-sided colour).
- **VERTEX_ATTRIB_OUTPUT_MASK (0x1FF4)** gates each output; RPCS3 tests it per output. Ours applies it to TEX8/TEX9 only (`VP_OUTMASK`, bits 12/13 as `vp_posoffset.w`); the other outputs are always forwarded.
- **Spec special cases** (**Done**, `VP_SPEC`, default on). Of these the title's 250 VPs use only RSQ (93 instructions in 74 programs):
  - RCC keeps the sign, with |r| in [2^-64, 2^64] (NV_vp1_1, NV_vp2 2.14.3.25).
  - RSQ(0) = +Inf (NV_vp 2.14.1.10.7, NV_vp2 rule 4); was rsqrt(max(|x|, 1e-10)). Negative inputs keep the abs: NV_vp says |x|, NV_vp2 says NaN (open, section 11).
  - EXP/LOG return their vector forms.
  - LG2(0) = -Inf and LG2(x < 0) = NaN (NV_vp2 2.14.3.17).
  - Out-of-range relative constant reads return 0 (NV_vp 2.14.1.9); the range is our 512-entry file.
- **Correct:** the 0*x = 0 rule, ARL floor, dual-issue ownership (vpaudit found no hazards in the 250 VPs), and CC on the vector unit.

---

## 7. Cell and LV2 semantics (non-graphics)

All of these are T1a or T2. Harnesses that run our code against the real-PS3 `.expected` files were built during the research (local work files, not yet in the repo).

| Area | Hardware | Ours | Score vs hardware |
|---|---|---|---|
| SPU single precision | Truncation, denormals read and written as 0, exponent 255 is an ordinary number, frest/frsqest tables | OK in the RZ+DAZ build | 100% (misses are only "????????" lines) |
| SPU double precision | IEEE with the FPSCR round mode (default nearest); dfma fused; dfnms = -(a*b - c); default NaN positive | Round toward zero (`SPU__DP_ON` clears only DAZ/FTZ, `spu_helpers.h` 1543); dfma not fused (1596-1608) | dfa 431/484, dfm 371/484, frds 19/22 |
| PPU VMX | VSCR = 0x00010000 (NJ on, never changed by the EBOOT); round to nearest; vmaddfp fused; vnmsubfp = -(a*c - b); hardware estimate tables; default NaN 0x7FC00000 | No NJ flush, not fused, exact maths for the estimates | vmaddfp 9264/10648, vnmsubfp 3660/10648, vrefp 5/22, vrsqrtefp 4/22 |
| PPU FPU | fmadds rounded once; hardware fres/frsqrte; PowerPC NaN | Double rounding (`ppu_lifter.py` 2109-2121), exact estimates | fmadds 10646/10648 |
| Timebase / SPU decrementer | 79.8 MHz | OK (RPCS3 uses 80 MHz) | |
| Vblank | 59.94 Hz, evenly spaced; handler argument = head; delivered on libgcm's interrupt thread | 62.5 Hz in GetTickCount bursts (`src/main.cpp` 337, 405); passes the count; runs on the next HLE caller (`ppu_hle.cpp` 228) | |
| lwmutex | trylock on a busy mutex returns EBUSY 0x8001000A; recursion attribute; unlock by a non-owner returns EPERM | 0x8001000B (`ppu_sysprx.cpp` 283); recursion ignored | |
| lwcond | Infinite wait blocks | Returns after 500 ms (`PS3_LWCOND_SAFETY_MS`) | |
| cellFs flags | CREAT 0100, EXCL 0200, TRUNC 01000, APPEND 02000 (octal) | `ppu_fs.cpp` 62-64: CREAT 0x200, TRUNC 0x400, APPEND 0x100 (wrong). `sys_fs.h` 73-76 also wrong. `cellFs.h` is correct. | |
| sys_process_is_stack | High nibble 0xD | Range 0x0E000000-0x10000000 (`ppu_sysprx.cpp` 86), which is our TLS area | |
| Reservations | 128-byte granule; any store or dcbz in the line breaks it | Word-sized value compare | |
| HLE float arguments (T2, ELFv1 3.2.3/3.2.4; T3, the title's own calls) | f1-f13 in order, interleaved slot numbering with the GPRs; arguments 9+ at SP+0x70; float results in f1. T3, from the lifted EBOOT: of the 168 functions whose entry block reads f1 and r5 as arguments, 77 never read r4 there (e.g. func_0011DE1C keeps f1 and r5 and overwrites r4); the varargs call at 0x1B54C copies f3 into r6 and f4 into r9 (each float's slot GPR) and its 9th argument to SP+0x70; cellGcmBindZcull (0x4EFC74) gets arguments 9-12 at SP+0x70..0x88 | Implemented 2026-10-15 (P0-3), in-game check pending: `gen_hle_nids.py` emits an ELFv1 thunk for the 12 float/double handlers and the 3 with more than 8 parameters (`include/ps3emu/hle_abi.h`, `PS3_HLE_ABI=0` = old integer adapter). Before, `cellVideoOutSetGamma` logged -0.00. | |
| HLE prototypes the title calls with more arguments | cellGcmBindZcull takes 12 (index ... sMask; T2, PSL1GHT `rsx/gcm_sys.h` gcmBindZcull); the title passes all 12 (T3, 0x4EFC74) | **Done:** `cellGcmBindZcull` takes the 12 parameters (ELFv1 thunk for arguments 9-12; regenerate `ppu_hle_nids.cpp`), stores and logs the binding and flags values outside libgcm's rules (`rsx_zcull_bind`). Zcull itself is not modelled: as a conservative early reject of tiles the depth test would fail it should change neither the picture nor a ZPASS count (an inference from T2/T4 descriptions, no hardware measurement); ZCULL_STATS reports (types 2-5) write 0 and are logged. cellAudioSetPortLevel (r3 port, f1 level from the global at r2-0x6A78, 0x57E00C) has no handler: the level is faked away. | |

---

## 8. The title's own pipeline (T3, from the ttmx RE and our notes)

- **G-buffer A**, 1280x720, memory order A,R,G,B. A holds the gloss/material byte, `gloss = exp2(m/256*8.0314 - 0.889)`. RGB holds the world normal, `n = byte*2.00784/256 - 1`.
- **G-buffer B** holds the D24S8 bytes (depth high to low, then stencil, which is the "character mask").
- **Copies to main memory.** Both G-buffers are copied by NV3089 blits of 1024 + 256 columns, at main 0x37400B80 and 0x37784B80 on the EU disc.
- **SPU SSAO** runs in 5 stages:
  - Half-resolution depth, 640x360 X32_FLOAT, with `1/z = 0.100000347 - 5.96042646e-9*d24`.
  - A 12-sample hemisphere.
  - 1-6-10-6-1 blurs.
  - A bilateral upsample to a 1280x720 8-bit AO image.
- **SPU tiled lighting** uses 32x40 tiles and up to 256 lights.
  - Diffuse is stored as `sqrt(rgb)` with a scale in A (range 0-8); an active but unlit tile is A=90.
  - Specular is monochrome, `((1-N·H)*gloss+1)^-8`.
- **Particles** are drawn into 512x288 colour plus depth.
- **DOF** works on a 320x180 image.
- **Bloom** goes from 160x90 down to 20x11.
- **Final composite**: `scene*(1-bloom.a) + 2*bloom + glow`. The glow mask is in scene alpha; the vertical blur gain of 2.0 per level is the game's own (checked against RPCS3 CPU memory, T3).
- **Glow mask source** (8B314D2A, ops 37-45):
  - Specular power is `sat(N·V)*129` times the X16 lookup `.x`.
  - The mask is `sat(spec*lum - 0.1)*0.3`, so the alpha it writes is at most 0.3.
- **Sun shafts** (ADD0CB4D): 16 taps toward the sun at (0.745, 0.010).
  - Each tap's off-screen fade is `R1 - sat(R1)`, then `sat(1 - 1000*|.|²)`.
  - Without the source saturate, every off-screen tap counts fully.

---

## 9. Hardware visual targets (T1b)

Measured on real-PS3 captures; full data in `tools/ground_truth/targets.json`. Our values come from `logs\run-20261014-230902-ready.png` (PIER) and `logs\run-20261015-045003-ready.png` (BOAT-type cutscene), measured with `tools/ground_truth/pngstat.py`.

| Check | Hardware | Ours on 2026-10-14/15 |
|---|---|---|
| PIER whole frame, luma ≥ 235 | 0.05% / 0.19% / 0.67% (three captures) | **5.48%** |
| PIER whole frame, clipped (RGB ≥ 250) | 0 to 0.02% | **0.67%** |
| PIER floor mean | (55-67, 41-55, 31-48) | **(105-132, 79-97, 69-82)** |
| PIER floor luma p95 | 75-85 | **189-252** |
| PIER sky top, luma ≥ 235 | 0% (LACRY), 0.01% (RELLINGS) | **17%** |
| Harbour water luma p95 | about 51-54, never white | (measure on the ship view of the -Step 11 script) |
| Lamp halo (720p px) | Half height at about 16-20, background by about 40-50; white core a few px | (measure with `tools/ground_truth/halo.py`) |
| BOSS blast, luma ≥ 235 | about 4.2% at the peak; clipped 0.01% | |
| Sky banding (720p) | Row steps of 1-2 levels at most; median well under 1 | (measure on 1280x720 dumps with `tools/ground_truth/bandstep.py`) |
| BOAT skin | R>G>B; R/G 1.45-1.76; G/B 1.19-1.57; hue 15-22°; neck stays red-brown | **Zeke's face (31,49,53), R/G 0.64, hue 191°**. Kuo's cheek in shadow (18,22,26), hue 207°. |
| BLUESKY skin | Lit (161,106,76), shadow (126,81,64): warm under a blue sky | |
| Character rim highlights | No clipped rims | Zeke's arm region: 5.2% clipped |
| DAY whole frame, luma ≥ 235 | 0.07-0.21%; clipped 0 | |

Footage note: the pier walk to the gate comes *after* the Beast appears, under an orange smoke sky. Our -Step 11 frame shows the same state. A magenta sky in that walk would mean the warp set the wrong atmosphere.

---

## 10. Where RPCS3 is wrong for this title (do not copy)

- **#18628** (open): floor flicker at a New Marais rooftop. It is fixed only with Strict Rendering Mode, because RPCS3 samples bound render targets live (a feedback loop).
- **#17370** (open): black artifacting from its GPU detiling.
- **#11149**: inFamous refraction was broken until #19410 (2026-09). The fix is Vulkan only; OpenGL is still wrong.
- **ZCULL**: 0xFFFF counts. The wiki recommends Relaxed ZCULL and disabling the queries.
- **SPU XFloat**: the default is approximate. Relaxed gives "blue visuals" on AVX-512 CPUs and crashes in missions 10 and 18. Accurate XFloat was needed in inFamous 1 (#11719).
- **PPU**: approximate NJ and exact estimates are the defaults. The 128-byte reservation is off by default.
- **Timebase**: 80 MHz instead of 79.8 MHz.
- **Resolution scaling**: a 320x320 threshold hack; disabling mesh trimming gives black lines (#19508).

---

## 11. Open RE questions and how to settle them

1. **X16 and Y16_X16 presentation on RSX.**
   - Evidence so far: T4 (RPCS3), T3 (the lookup must be non-zero), and envytools' desktop NV40 names A16 (0x14) / A16L16 (0x15).
   - To settle it: decode the 1024x8 lookup from guest memory on the CPU, then check that the sampled `.x` equals it. Check against the RPCS3 oracle only as a side note.
2. **Whether the alpha test runs on the quantised output.** RPCS3 marks this unverified. Keep it toggleable.
3. **SHADER_PACKER with MRT.** RPCS3 converts every enabled MRT, while the D3D9 behaviour is unclear. The game's two 0xD7FDC passes are single-target, so either reading works for now.
4. **VERTEX_ATTRIB_OUTPUT_MASK.** What the FP reads for a masked-off output: RPCS3 leaves the value unwritten (0).
5. **Report IO mapping.** Settled (T3): the title reports through HOST_BUFFER (0xFEED0001), so 0x14080 is an IO offset inside cellGcmInit's map; it never uses REPORT_LOCATION_MAIN and has no direct call to cellGcmMapEaIoAddressWithFlags or cellGcmMapMainMemory in the EBOOT.
6. **Source fx9/fx12/fp16 modifiers and fx9 results** (`FP_SRCPREC` 0x02/0x04/0x08, `FP_DSTPREC` 0x08). Only T4 (RPCS3) gives their meaning; no title instruction needs them. Settle with a hardware micro-test (critique P2-14).
7. **fp16 rounding.** NV_fp allows either nearest value; D3D's f32tof16 rounds toward zero. Whether RSX rounds to nearest is unknown; it changes H values by at most one fp16 ulp.
8. **VP RSQ of a negative input**: |x| (NV_vp) or NaN (NV_vp2). Also the RSX's own constant-file size for relative reads (our 512, RPCS3 468).
9. **TEX_COORD_CONTROL 2D override** with perspective_corr: (xy/w, 0, 1) per RPCS3. Not decoded; first log whether the game sets 0x0B40 at run time.
10. **Source modifier 5.** We read it as a no-op (RPCS3 "from hw tests", T4). In the title its 687 uses are exactly every NRM_H source (453) and both sources of 117 of the 119 X-precision compares (SLTX, SGTX, SLEX, SEQX), never an fp32 instruction. That fits "convert the source to the instruction's precision" (NV_fp 3.11.4.2 converts sources to the instruction precision), which would clamp the compare operands to fx12 and send NaN to 0. It changes a result only for operands outside [-2, 2) or NaN. Settle with a hardware micro-test (SLTX of 2.5 against 3.0 with modifier 5).
11. **perspective_corr on the Metal path.** glslang's HLSL front end gives `SV_Position.w` as 1/w (Vulkan) unless its DX position-w option is set, and `rsx_shader_msl.cpp` does not set it. There `rsx_iw` is w, so `FP_PERSP` multiplies by w instead of 1/w. Only the D3D12 path was verified (WARP).

---
12. **Video-out gamma (`cellVideoOutSetGamma`).**
    - T3: func_0013287C passes r3 = 0 and, in f1, the saved option at settings +0x6A4 (written by the options setter func_0004C12C, read back by func_0004AE54; by its use, the brightness option). It calls only when the value differs from the one it cached at this+8. The value is logged now (`[cellAvconfExt] VideoOutSetGamma(... gamma=... [bits])`); record it with every measurement.
    - Range: [0.8, 1.2] is T5 only (RPCS3). CryEngine 3's r_Gamma page (https://www.cryengine.com/docs/static/engines/cryengine-3/categories/9895942/pages/9218939) gives the PS3 range as 0.8 to 1.25 and calls it a "graphics card gamma correction" that "affects also HUD and desktop", i.e. an output-stage transfer. A rejected value is logged with its bits.
    - Curve: we apply `out = in^(1/gamma)` to RGB in the present pass, after every copy the game can read back and before the dump (`libs/video/rsx_videoout_gamma.c`). The direction is the usual display-gamma convention (SDL 1.2 `SDL_gamma.c`: `pow(i/256, 1/gamma)`), not a Sony document. `VIDEOOUT_GAMMA=inv` gives the other direction, `=0` turns it off, `VIDEOOUT_GAMMA_FORCE=<g>` overrides the value. At 1.0 all of them are the identity.
    - Default on, by the P0-3 task, although rule 2 would keep a rule with no T4 note off: not applying the title's request is certainly wrong for any value other than 1.0, and 1.0 (the boot value) is a no-op. If the logged value at default settings is not 1.0, decide the default with the T1b A/B (default, `inv`, `0`) and record the result here.
    - To settle it: set the in-game brightness to its minimum and maximum, log the gamma each passes, and compare a dark scene with footage taken at a known brightness setting.

13. **TEX9's vertex output.** RPCS3 reads o6, Mesa nouveau's NV40 output 16 (both T4). No title VP writes o6 or o16 and no title FP reads TEX9, so it changes no pixel of inFamous 2; `FP_TC89=2` (o6) and `VP_TEX9=16` (o16) keep both readings available.

14. **The viewport's pixel clip.** NV40 either clips x/y to the NDC square exactly (`RASTER_VPCLIP=1`, the default) or uses a guard band plus the integer VIEWPORT_HORIZONTAL/VERTICAL rect (`=2`). For libgcm's writer the two differ only for a negative origin, which wraps in the 16-bit rect, so under `=2` such a draw would vanish. Settle on hardware: a full-screen quad under a viewport at x = -16 (w 1280) with a full scissor draws 1264 columns (=1) or none (=2).

15. **SHADER_WINDOW's origin and DDY.** Whether origin bottom also flips the sign of DDY is undocumented. Not modelled; the title uses DDY in 2 programs. Settle with a hardware micro-test: DDY of f[WPOS].y under both origins.

## 12. Re-verification log (2026-10-15)

| Claim | Checked against | Result |
|---|---|---|
| SRC1 bits 19-27 are the precision modifiers; SRC2 bit 31 is perspective_corr | RPCS3 `RSXFragmentProgram.h` 94-153 | Confirmed |
| Modifier order: sat before abs, negate last; COL0/1 saturated | RPCS3 `FragmentProgramDecompiler.cpp` 548-801 | Confirmed |
| Ours ignores both and does not clamp COL | `rsx_fp_decompiler.c` 398-477; `rsx_vp_decompiler.c` 692; spec_audit_test FP-1, 2, 4, 5 and VP-1 FAIL | Confirmed |
| God-ray `R1 - sat(R1)` | Census disassembly of ADD0CB4D, op 8 (`[sprec=0,4,0]`) | Confirmed |
| D3D `SV_Position.w` = w, not 1/w | Dawn commit 34eee4e6 | Confirmed (an implementation detail for the persp fix) |
| Stale texture on unsupported formats | `rsx_d3d12_backend.c` 12301-12372 (`cur_texs[unit]` untouched); prepass 8551-8593 gives a NULL SRV outside its list | Confirmed. Both lists must widen. |
| X16 0xAAEE crossbar reads constant 0 in ours | `rsx_texture_layout.c` 470-492; TX-2 FAIL | Confirmed |
| 16-bit-block remap table; float formats forced to 0xE4 | RPCS3 `RSXTexture.cpp` | Confirmed (values in 4.1) |
| SHADER_PACKER = sRGB output, toggled by the game | RPCS3 `rsx_decode.h` (`!!value`), `RSXThread.cpp` 2455-2476; EBOOT scan: 10 sites, 1 at D887C, D8FE8, 2E9AE8 | Confirmed; rsx_commands has no case |
| ROP order and blending on stored bytes; XRGB DST_ALPHA = 1 | Commits fbf1f96 and 5bc2c0d of PR #19410 | Confirmed (the alpha-test placement is "inferred" there) |
| Signed blend semantics | PR #19410 patch (`_blend_saturate_signed`) | Confirmed; ours maps them to ADD (backend 3192-3201) |
| Reports never reach guest memory; the game does not import the getters | `cellGcmSys.c` 1837-1845; NID resolution of the cellGcmSys imports | Confirmed |
| NV_vp colour clamp, relative-address read of 0 | NV_vertex_program 2.14.1.5 and 2.14.1.9 | Confirmed |
| NV_fp CC rule; PK4B excess-128 | NV_fragment_program 3.11.4.4 and 3.11.5.22/43 | Confirmed |
| "The NaN guard zeroes the whole target" (specaudit A6) | HLSL ternary under FXC SM5 is per component | **Refuted**: the guard is per channel |
| "Blue skin fixed by FP_TC89" (rpcs3gaps item 23) | `logs\run-20261015-045003-ready.png` (exe built 04:49, sources with TC89 on): Zeke's face (31,49,53), hue 191° | **Refuted**: still blue. The most likely remaining cause is the stale X16 lookup on unit 5 of 632599C5. That program computes specular = tc6 (light colour) x TEX2 mask x 0.066 x 129·sat(N·L) x TEX5.x(N·H, 0.281); see HLSL lines 63-88 in `rpcs3gaps\skin_632599C5.hlsl.txt`. With a stale texel near 1, the tc6 light colour saturates the skin. |
| The game disables dithering | EBOOT: DITHER_ENABLE (0x0300) = 0 at 0x133284 through the helper | Confirmed (one site) |
| cellFs flags wrong; lwmutex trylock code | `ppu_fs.cpp` 62-64, `sys_fs.h` 73-76, `ppu_sysprx.cpp` 283 | Confirmed |
| SPU double precision under round toward zero | `spu_helpers.h` 1542-1544 (clears only 0x8040) | Confirmed |
| ELFv1 interleaved slot numbering, save area at SP+48 (P0-3) | ELFv1 1.9 Figure 3-18 (d in r5, hh in f5) and 3.2.4; lifted EBOOT scan (77 of 168 functions taking f1 and r5 never read r4; the 0x1B54C varargs duplicates; the 0x4EFC74 12-argument call) | Confirmed: the PS3 toolchain follows it |
| Every float/double or >8-parameter HLE handler goes through a thunk (P0-3) | clang AST of all 1120 registered handlers (canonical types) against the generated table; generator parse = AST on all 1120 | Confirmed: 15 thunks (12 float, 3 with >8 parameters), no float result, no aggregate by value; the ctx handlers take no float |
| D3D `SV_Position.w` is clip w (for perspective_corr) | D3D11 WARP, FXC ps_5_0: clip w 4 reads back 4; `tc * (1/SV_Position.w)` = tc/4 (P0-4 probe, `test_fp_warp` "persp") | Confirmed |
| The title pre-multiplies screen coordinates by clip w | VP disassembly: 12C6831033A5005C ops 46-59, 125CC02B9D3DD8BC ops 18-23 | Confirmed (T3 for perspective_corr) |
| f32tof16 rounding and overflow | WARP: 0.95 -> 0x3B99 (toward zero), 70000 -> 0x7BFF (65504), INF and NaN kept, denormals kept | Measured; `FP_HROUND=2` restores NV_fp's ±INF |
| HLSL `clamp(NaN)` is not 0; `isnan` survives FXC | WARP: clamp(NaN, -2, 2) = -2; isnan compiles to `ne x, x` | Hence `rsx_fx12`'s explicit NaN test |
| "TEX_COORD_CONTROL 0x0B40 is set at 10 sites" | critique `methsites.py` over the EBOOT | **Refuted**: 9 are `addi` struct offsets; use by the game unproven |
| The title's viewport transform comes from integer rects; its scissor is the rect clamped (P0-6) | EBOOT disassembly: 0x501C90 writes VIEWPORT_HORIZ/VERT, CLIP_MIN/MAX and offset/scale from r4-r7 and f1/f2 (0.5 at TOC-0x77EC); its callers 0x20B28C (an unclamped rect) and 0x2E7C68 (0, 0, w, h); the scissor writer 0x501DF0 via 0x20B16C clamps to the surface and zeroes an off-surface rect. No other VIEWPORT_OFFSET writer in the PPU code | Confirmed: no fractional offsets from the PPU; a negative origin wraps in the integer rect |
| CLEAR_SURFACE obeys the scissor, not the viewport | Mesa nouveau `nv30_clear.c`, `nv30_state_validate.c` (`nv30_validate_scissor`); GL 2.1 4.2.3 | Confirmed (T4 + T2) |
| SHADER_WINDOW's origin does not move pixels | nouveau `nvfx_fragprog.c` (TGSI FS_COORD_ORIGIN/PIXEL_CENTER only set COORD_CONVENTIONS), `nv30_validate_viewport`; PSL1GHT `rsxSetSurface` (origin 1) with the title's scale.y < 0 | Confirmed (T4 + T2/T3) |
| D3D12: both routes give window = ndc * s + o at pixel centres (route 1: integer rects, a negative origin, mirrored axes; route 2: fractions, clip w != 1, past the target); an empty scissor draws nothing; clear rects clear only themselves | WARP, `test_raster_warp` (70 checks) | Confirmed |
| "The clip-space epilogue is bit-identical to the integer-rect viewport for libgcm's transforms" (raster P0-6 first cut) | WARP, raster-verify `exp_nondyadic.c`: 8 rects x 24 random triangles on 1280x720 | **Refuted** except for the full frame (the earlier test only tried a 64x48 frame, where every w / 64 is dyadic); hence route 1 |
| The CC takes the value the destination holds | NV_fragment_program 3.11.4.4 prose and `GenerateCC(DstT value)` | Confirmed; P0-4 verify moved the H-destination CC to the stored fp16 value |
| Source modifier 5 sits only on NRM_H and X-precision compares | census `fpquery.py sprec`/`census_fp.txt` (687 = 453 + 2 x 117) | Confirmed; open question 10 |
| "The green gate pillar with TEX8/9 on is a TEX8/TEX9 error" (2026-10-15) | OPSNAP of the lit target 0x011C0000 (tc89cap frame_00): the pillar face stays (6, 8, 10) until op979, the deferred sun pass 6B8AE6FE, which reads no TEX8/TEX9, writes (34, 61, 10). Its census ucode 6BFFD9CDF07FC47D: op 40 `MAD_H H7.w, H1, H0, H5.z [src2 sat]`, op 43 `MUL_H H1.w, H5.z [src0 sat], 129`, H5.z = N.(-sun); our HLSL had no saturate. Frame-wide, 834 of 843 green-cast pixels of that pass are sun-visible faces turned away from the sun, none in any sun-facing bin | **Refuted**: the missing source saturate (now `FP_SRCPREC`). Pillar N.L = -0.85 times the sun colour (0.89, 0.62, 0.54) drove R and B below zero. With TEX8 = 0 the face was dark olive (window shot: 1.64% green-cast in the gate box vs 0.84% with TEX8 on), which hid it |
| TEX8 = VP o15 | Mesa nouveau `nv40_vertprog.h` (DEST_TC(n) = 7 + n), `nvfx_fragprog.c` (ten texcoords on nv4x, VP_RESULT_EN `0x1000 << (n - 8)`); the title's VPs (o15 = c258 in 11, c262 in 5, v3.x * c464 in 4) and FPs (30 read TEX8, all as a tint or fill colour) | Confirmed (two evidences): `FP_TC89` default 1 |
| "inFamous 2's skin shader takes its warm lighting term from TEX9" (test_fp_decompiler.c) | census of input_src 0xD over the 322 FPs; `rpcs3gaps\skin_632599C5.hlsl.txt` line 94 | **Refuted**: no program reads TEX9; the skin program 632599C5 reads TEX8 (`h2 = tc8`, times a mask, added into the term it exports from h8), so `FP_TC89`'s default does reach the faces |
| "SET_BLEND_ENABLE_MRT is 0x012C" (5.1, from RPCS3 `rsx_decode.h`) | Lifted EBOOT: the libgcm inline func_005014F0 writes header 0x4036C with `m & 0xE` next to 0x40310 with `m & 1`; Mesa nouveau `NV40_3D_MRT_BLEND_ENABLE` 0x36c | **Refuted**: it is 0x036C (T3 + T4) |
| The title uses the signed blend equations | `tools/ground_truth/blendsites.py` over generated/ppu: SetBlendEquation(0xF006) at 6 sites, depth test and mask off around 5 of them; no 0xF005/0xF007 | Confirmed (T3; with the #19410 note, T4, the two evidences for `BLEND_SIGNED`) |
| Two passes reproduce `clamp(dst + s8(src))` and `clamp(dst - s8(src))` under depth/stencil state | WARP, `test_rop_warp` W1-W3 (all 65536 pairs), W10-W13 | Confirmed (mechanism; the overlap caveat in 5.1 stands) |

---

## 13. Measurement tools (`tools/ground_truth`)

All three are stdlib-only and read-only.
- `tools/ground_truth/pngstat.py IMG [name=x0,y0,x1,y1 ...] [--scale720]`: mean RGB, luma p50/p95/max, hiPct, clipPct, R/G, G/B and hue, the same measures as `targets.json`.
- `tools/ground_truth/halo.py IMG cx,cy [--scale720]`: radial lamp profile, half-height radius and background radius.
- `tools/ground_truth/bandstep.py IMG x0,y0,x1,y1 [--scale720]`: sky row-step statistics and flat-run lengths.
- `tools/ground_truth/measure_yt.js`: the same measures on real-PS3 YouTube frames in a browser; nothing is downloaded.
