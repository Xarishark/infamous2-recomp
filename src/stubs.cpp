/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

#ifdef IF2_HAVE_LIBSRE
#include <cstdlib>
#include "prx_loader.h"

/* libsre's namespaced lifted tables (generated/prx/libsre_ns/). */
extern "C" const prx_func_entry libsre_function_table[];
extern "C" const uint64_t        libsre_function_table_count;
extern "C" const prx_export      libsre_exports[];
extern "C" const uint32_t        libsre_export_count;

struct ppu_context;
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void spu_register_stack_reset_entry(uint32_t entry, int image_id);
static void if2_register(uint32_t guest, void (*host)(void*))
{
    ppu_register_function(guest, (void (*)(ppu_context*))host);
}

/* Must match tools/lift_libsre.sh. 0x30000000 is clear of this title's map:
 * EBOOT 0x10000-0xB2E000, stacks/TLS 0x0E000000-0x11000000, sys_memory
 * 0x40000000-0x50000000. */
static const uint32_t LIBSRE_BASE = 0x30000000u;
#endif

extern "C" void ps3_load_prx_modules(void)
{
#ifdef IF2_HAVE_LIBSRE
    if (const char* off = getenv("IF2_HLE_SPURS"); off && *off == '1') {
        fprintf(stderr, "[infamous2] IF2_HLE_SPURS=1 -- not loading libsre\n");
        return;
    }
    const char* dir = getenv("IF2_PRX_DIR");
    if (!dir || !*dir) dir = IF2_PRX_DIR;
    char path[1024];
    snprintf(path, sizeof path, "%s/libsre.linked.bin", dir);

    uint32_t sz = 0;
    uint8_t* img = prx_image_load_file(path, &sz);
    if (!img) {
        fprintf(stderr, "[infamous2] %s not found -- cellSpurs stays HLE\n", path);
        return;
    }
    prx_module m;
    memset(&m, 0, sizeof m);
    m.name         = "libsre";
    m.base         = LIBSRE_BASE;
    m.image        = img;
    m.image_size   = sz;
    m.funcs        = libsre_function_table;
    m.func_count   = libsre_function_table_count;
    m.exports      = libsre_exports;
    m.export_count = libsre_export_count;
    prx_load_result r = prx_load_module(&m, if2_register);
    free(img);
    fprintf(stderr, "[infamous2] libsre %s: %u functions at 0x%08X\n",
            r.ok ? "loaded" : "load FAILED", r.funcs_registered, r.base);
    /* The SPURS kernel (SPU image 41) calls a policy module with
     * `bisl $r0,$r11` at LS 0x800, but a PM leaves by BRANCHING to 0x808
     * (`ila $r1,0x3FFD0` -- the kernel's stack reset), never returning to the
     * link. Without this the drain waiting for 0x804 resumed the kernel inside
     * itself and every workload switch nested one more host frame, until the
     * depth guard halted the kernel. Needs the 0x808 entry lift_libsre.sh adds. */
    if (r.ok) spu_register_stack_reset_entry(0x808u, 41);
#endif
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
