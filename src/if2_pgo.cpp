/*
 * inFamous 2 -- the PGO training run's profile writer.
 *
 * Built only into IF2_PGO=instrument builds (CMakeLists.txt adds this file and
 * defines IF2_PGO_INSTRUMENT for it); see tools\build.ps1 -Pgo.
 *
 * clang's profile runtime writes its .profraw from an atexit handler, and this
 * port never runs one: every way out -- the window's close box, the checkpoint
 * driver's quit() (CHECKPOINT_EXIT_AFTER, -ExitAfter), a failed boot -- is
 * ExitProcess, which skips the executable's atexit table. A training run would
 * end with no profile. So, in an instrumented build only:
 *   - a background thread writes the cumulative counters every
 *     IF2_PGO_DUMP_S seconds (default 15; 0 = never), so even a run that is
 *     killed keeps a profile at most that old;
 *   - this module's import of ExitProcess is pointed at a wrapper that writes
 *     the profile first, while every thread is still alive (nothing is
 *     orphaned: no heap or stdio lock held by a terminated thread);
 *   - a normal return from main writes it too (atexit).
 * Each write goes to <file>.tmp and is then renamed over <file>, so a run that
 * dies mid-write leaves the previous complete profile, never a torn one.
 *
 * <file> = IF2_PGO_FILE, else <IF2_PGO_DIR, or "pgo-raw" beside the exe>\
 * if2-<pid>.profraw: one file per run, always the run's whole history, so
 * llvm-profdata merge of the directory counts every run exactly once.
 *
 * Training only: the counters are not atomic, so threads running the same code
 * lose a few increments -- harmless for a profile -- and none of this exists in
 * the build that ships. Game behaviour is untouched: the writer only reads the
 * counters, and the ExitProcess wrapper ends in the real ExitProcess.
 */
#if defined(_WIN32) && defined(IF2_PGO_INSTRUMENT)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern "C" int  __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char* name);
/* Defining this keeps the runtime's own registration object out of the link:
 * no LLVM_PROFILE_FILE / default.profraw handling and no exit-time writer of
 * its own (on a normal exit() it would append a second copy of the counters,
 * and merge would count that run twice). This file is the only writer. */
extern "C" int __llvm_profile_runtime = 0;

static char s_out[MAX_PATH], s_tmp[MAX_PATH + 8];
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile LONG s_writes = 0;
static volatile LONG s_final = 0;       /* the exit-time write has been made */

static void if2_pgo_write(const char* why)
{
    AcquireSRWLockExclusive(&s_lock);
    DeleteFileA(s_tmp);                     /* a torn leftover is never appended to */
    __llvm_profile_set_filename(s_tmp);
    const int rc = __llvm_profile_write_file();
    const BOOL mv = rc == 0 && MoveFileExA(s_tmp, s_out, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const LONG n = InterlockedIncrement(&s_writes);
    ReleaseSRWLockExclusive(&s_lock);
    if (n == 1 || strcmp(why, "periodic") != 0 || !mv)
        fprintf(stderr, "[pgo] profile %s (%s) -> %s\n", mv ? "written" : "NOT written", why, s_out);
}

/* The last write of the run (ExitProcess or exit(), whichever comes first). */
static void if2_pgo_final(const char* why)
{
    if (InterlockedExchange(&s_final, 1)) return;
    if2_pgo_write(why);
    fflush(stderr);
}

static DWORD WINAPI if2_pgo_dumper(LPVOID p)
{
    const DWORD ms = (DWORD)(uintptr_t)p;
    for (;;) {
        Sleep(ms);
        if (s_final) return 0;
        if2_pgo_write("periodic");
    }
}

typedef VOID (WINAPI* exitprocess_fn)(UINT);
static exitprocess_fn s_real_exit;
static VOID WINAPI if2_pgo_exit(UINT code)
{
    if2_pgo_final("ExitProcess");
    s_real_exit(code);
}

/* Point this module's import of ExitProcess at if2_pgo_exit. */
static int if2_pgo_hook_exit(void)
{
    uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int hooked = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
         dir.VirtualAddress && d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat   = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const IMAGE_IMPORT_BY_NAME* ibn = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, "ExitProcess") != 0) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof iat->u1.Function, PAGE_READWRITE, &old)) continue;
            if (!s_real_exit) s_real_exit = (exitprocess_fn)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)&if2_pgo_exit;
            VirtualProtect(&iat->u1.Function, sizeof iat->u1.Function, old, &old);
            hooked++;
        }
    }
    return hooked;
}

static void if2_pgo_atexit(void) { if2_pgo_final("exit"); }

static struct if2_pgo_init {
    if2_pgo_init()
    {
        const char* f = getenv("IF2_PGO_FILE");
        if (f && *f) {
            snprintf(s_out, sizeof s_out, "%s", f);
        } else {
            char dir[MAX_PATH];
            const char* d = getenv("IF2_PGO_DIR");
            if (d && *d) snprintf(dir, sizeof dir, "%s", d);
            else {
                GetModuleFileNameA(NULL, dir, sizeof dir);
                char* s = strrchr(dir, '\\'); if (s) *s = 0;
                strncat(dir, "\\pgo-raw", sizeof dir - strlen(dir) - 1);
            }
            CreateDirectoryA(dir, NULL);
            snprintf(s_out, sizeof s_out, "%s\\if2-%lu.profraw", dir, GetCurrentProcessId());
        }
        snprintf(s_tmp, sizeof s_tmp, "%s.tmp", s_out);
        __llvm_profile_set_filename(s_tmp);
        const int hooked = if2_pgo_hook_exit();
        atexit(if2_pgo_atexit);
        DWORD sec = 15;
        if (const char* e = getenv("IF2_PGO_DUMP_S")) sec = (DWORD)strtoul(e, 0, 10);
        if (sec) CloseHandle(CreateThread(NULL, 0, if2_pgo_dumper, (LPVOID)(uintptr_t)(sec * 1000u), 0, NULL));
        fprintf(stderr, "[pgo] instrumented build: profile -> %s (every %lus%s)\n",
                s_out, (unsigned long)sec, hooked ? ", and at ExitProcess" : "; ExitProcess NOT hooked");
    }
} s_if2_pgo_init;
#endif
