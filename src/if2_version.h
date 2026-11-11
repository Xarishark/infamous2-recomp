/*
 * if2_version.h -- the EBOOT this build lifted, and the per-version values of the title hooks.
 *
 * The hooks are written against BCUS98125's disc EBOOT (PARAM.SFO VERSION 02.00, APP_VER 01.00): the
 * addresses in every comment are that build's, and so is the first argument of each IF2_EA / IF2_V /
 * IF2_FN below. The title update 1.04 moves almost everything (code is inserted, the data segment
 * shifts, the TOC moves to 0x9DB730), so each value the hook code uses also carries its 1.04 one.
 * CMake defines IF2_EBOOT_V104=1 when analysis/inputs.json says the lift is of the 1.04 update
 * (update_app_ver "01.04"); the updates 1.01..1.03 have no table, so CMake refuses those lifts.
 *
 * Where the 1.04 values come from: tools/eboot_map.py maps 1.00 -> 1.04 from the two ELFs (functions
 * by a relink-proof instruction signature, data by votes at corresponding instructions), and every
 * value that sits in code or data the update changed was checked against the instruction that reads
 * it in 1.04 (docs/NOTES.md, 2026-10-21).
 */
#pragma once

#if defined(IF2_EBOOT_V104) && IF2_EBOOT_V104
#define IF2_EBOOT_NAME    "BCUS98125 update 1.04"
#define IF2_V(v100, v104) (v104)
#define IF2_FN(f100, f104) f104
#else
#define IF2_EBOOT_NAME    "BCUS98125 disc (v02.00)"
#define IF2_V(v100, v104) (v100)
#define IF2_FN(f100, f104) f100
#endif

/* a guest address (IF2_V is the same for other per-version values: code words, TOC offsets) */
#define IF2_EA(v100, v104) IF2_V(v100, v104)
