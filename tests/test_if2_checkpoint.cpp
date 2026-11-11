/* Host test for src/if2_checkpoint.cpp: includes the file, stubs the runtime
 * it calls, and checks install (opt-in), the sandbox, SFO parsing, pad
 * injection, the save-data wrappers (exact directory from
 * ps3_savedata_last_op, autosnap + dedup), the boot driver state machine
 * (continue, new game, no signals, IF2_TASKJUMP gating and its mark, timeout) and the PNG
 * writer. Child-process modes check the exits that end a run. Synthetic data
 * only; it needs no game files and no build of the game. From the project
 * folder (Windows):
 *
 *   clang-cl /nologo /EHsc /Od -std:c++20 -MD -Wno-everything
 *     -Iexternal\ps3recomp\runtime\prx -Iexternal\ps3recomp\include
 *     -Iexternal\ps3recomp\runtime\ppu tests\test_if2_checkpoint.cpp
 *     /Fe%TEMP%\test_if2_checkpoint.exe /Fo%TEMP%\ /link user32.lib gdi32.lib
 *     ole32.lib windowscodecs.lib && %TEMP%\test_if2_checkpoint.exe
 *
 * Prints "if2_checkpoint host checks passed". */
#include "../src/if2_checkpoint.cpp"

#include <cassert>
#include <map>


/* ---- runtime stubs ---- */
static std::vector<uint8_t> g_vm(0x01000000);
extern "C" uint8_t  vm_read8(uint64_t a) { return a < g_vm.size() ? g_vm[a] : 0; }
extern "C" uint16_t vm_read16(uint64_t a) { return (uint16_t)(vm_read8(a) << 8 | vm_read8(a + 1)); }
extern "C" uint32_t vm_read32(uint64_t a) { return (uint32_t)vm_read16(a) << 16 | vm_read16(a + 2); }
extern "C" void vm_write16(uint64_t a, uint16_t v) { g_vm[a] = (uint8_t)(v >> 8); g_vm[a + 1] = (uint8_t)v; }
static void w32(uint32_t a, uint32_t v) { vm_write16(a, (uint16_t)(v >> 16)); vm_write16(a + 2, (uint16_t)v); }
static void wstr(uint32_t a, const char* s) { memcpy(&g_vm[a], s, strlen(s) + 1); }
static unsigned g_frames = 0;
extern "C" unsigned ppu_boot_frames_presented(void) { return g_frames; }
extern "C" void padhost_shutdown(void) {}
extern "C" void ss_request_summary(const char*) {}
static std::map<std::string, void (*)(ppu_context*)> g_reg;
extern "C" void ps3_hle_register_ctx(uint32_t, const char* name, void (*fn)(ppu_context*)) { g_reg[name] = fn; }
extern "C" int32_t cellPadGetData(uint32_t, void* data)
{
    const uint32_t ea = (uint32_t)(uintptr_t)data;
    w32(ea, 24);
    for (int i = 0; i < 64; i++) vm_write16(ea + 4 + 2 * i, 0);
    for (int i = 4; i < 8; i++) vm_write16(ea + 4 + 2 * i, 128);
    return 0;
}

/* cellSaveData: a directory per save under the directory it was given, and
 * the last-op record the real one keeps. */
static std::string g_tk_root;                 /* "" = PS3_SAVEDATA_DIR (fixed at first use) */
static std::string g_tk_fixed;
extern "C" const char* ps3_savedata_root(void)
{
    if (!g_tk_root.empty()) return g_tk_root.c_str();
    if (g_tk_fixed.empty()) g_tk_fixed = env("PS3_SAVEDATA_DIR") ? env("PS3_SAVEDATA_DIR") : "x";
    return g_tk_fixed.c_str();
}
static uint32_t g_op_n = 0;
static std::string g_op_dir;
static int g_op_wrote = 0;
static void note_op(const std::string& d, int w) { g_op_dir = d; g_op_wrote = w; g_op_n++; }
extern "C" uint32_t ps3_savedata_last_op(char* dirName, int* wrote)
{
    if (dirName) snprintf(dirName, 32, "%s", g_op_dir.c_str());
    if (wrote) *wrote = g_op_wrote;
    return g_op_n;
}
static void put_save(const std::string& root, const std::string& dir, const std::string& state, const char* subtitle)
{
    mkdirs(join(root, dir));
    FILE* f = fopen(join(join(root, dir), "STATE").c_str(), "wb"); fwrite(state.data(), 1, state.size(), f); fclose(f);
    f = fopen(join(join(root, dir), "PARAM.SFO").c_str(), "wb");
    fprintf(f, "TITLE=inFAMOUS 2\nSUB_TITLE=%s\nDETAIL=Completion: 1%%\nPowers: 3\n", subtitle); fclose(f);
}
static int g_load_rc = 0;
static std::string g_load_dir = "BCUS98125_INFAMOUS2_0";   /* "" = nothing to load */
extern "C" void ps3_savedata_list_auto_load(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)g_load_rc;
    if (g_load_rc == 0 && !g_load_dir.empty() && is_dir(join(savedata_root(), g_load_dir))) note_op(g_load_dir, 0);
}
static std::string g_save_payload = "AAAA";
static std::string g_next_subtitle = "(Autosave) 3. Forward Momentum";
static int g_readonly = 0;
extern "C" int32_t cellSaveDataAutoSave2(uint32_t, const char* dirName, uint32_t, void*, void*, void*, uint32_t, void*)
{
    const std::string dir = guest_str((uint32_t)(uintptr_t)dirName, 64);
    if (!g_readonly) put_save(savedata_root(), dir, g_save_payload, g_next_subtitle.c_str());
    note_op(dir, !g_readonly);
    return 0;
}
extern "C" int32_t cellSaveDataListSave2(uint32_t, void*, void*, void*, void*, void*, uint32_t, void*)
{
    put_save(savedata_root(), "BCUS98125_INFAMOUS2_3", std::string(1000, 'm'), "3. Manual");
    note_op("BCUS98125_INFAMOUS2_3", 1);
    return 0;
}
extern "C" int32_t cellSaveDataListLoad2(uint32_t, void*, void*, void*, void*, void*, uint32_t, void*)
{
    note_op("BCUS98125_INFAMOUS2_3", 0);
    return 0;
}
/* src/if2_taskgraph.cpp */
static int g_tj = 0;
extern "C" int if2_taskjump_status(char* what, size_t cap)
{
    if (what && cap) snprintf(what, cap, "%s", g_tj == 2 ? "target step #219 active" : g_tj == 3 ? "gave up" : g_tj ? "waiting" : "");
    return g_tj;
}

/* the signal name table: names at the ids the driver uses, "Sphere" after 109 */
static void make_sig_table()
{
    std::map<int, const char*> named = {{0, "AmbientSpeech"}, {48, "MenuActivate"}, {49, "MenuDeactivate"},
        {58, "MissionStart"}, {60, "MovieStarted"}, {61, "MovieFinished"}, {62, "PackLoaded"},
        {88, "ResetWorldPre"}, {105, "WarpedToCheckpoint"}, {108, "XpChanged"}};
    uint32_t s = 0x00500000;
    for (int i = 0; i <= 109; i++) {
        char b[32];
        snprintf(b, sizeof b, "Sig%d", i);
        const char* n = i == 109 ? "Sphere" : named.count(i) ? named[i] : b;
        wstr(s, n);
        w32(kSigNames + 4u * (uint32_t)i, s);
        s += 32;
    }
}
static void sig(const char* name)
{
    ppu_context c{};
    c.gpr[4] = (uint64_t)sig_id(name);
    if2_signal_hook(&c);
}
static std::map<std::string, std::string> read_kv(const std::string& p)
{
    std::map<std::string, std::string> m;
    std::string s;
    if (!read_file(p, s)) return m;
    size_t a = 0;
    while (a < s.size()) {
        size_t e = s.find('\n', a); if (e == std::string::npos) e = s.size();
        const std::string l = s.substr(a, e - a);
        const size_t q = l.find('=');
        if (q != std::string::npos) m[l.substr(0, q)] = l.substr(q + 1);
        a = e + 1;
    }
    return m;
}
static void reset_driver(Boot b)
{
    g_boot = b; g_phase.store(P_BOOT); g_bootload.store(-1); g_presses = 0; g_last_press = 0;
    g_ready_frame.store(0); g_exit_code.store(-1); g_t_took = g_t_menu = -1;
    g_tj_frame = 0; g_tj_what.clear(); g_hooked = false; g_tj = 0;
    g_tj_mark_frame.store(0);
    for (int i = 0; i < kMaxSig; i++) { g_sig_count[i].store(0); g_sig_frame[i].store(0); }
    g_press_mask.store(0);
}
static void run_frames(unsigned n) { for (unsigned i = 0; i < n; i++) { g_frames++; tick(); } }
static void boot_load(int loaded)       /* what if2_ListAutoLoad publishes */
{
    g_loaded_dir = loaded ? "BCUS98125_INFAMOUS2_0" : "";
    g_bootload_frame.store(g_frames);
    g_bootload.store(loaded);
}
static std::string psf_bin(const char* dir, const char* sub)
{
    std::string keys = std::string("SAVEDATA_DIRECTORY") + '\0' + "SUB_TITLE" + '\0';
    while (keys.size() % 4) keys += '\0';
    std::string data(32 + 128, '\0');
    memcpy(&data[0], dir, strlen(dir)); memcpy(&data[32], sub, strlen(sub));
    const uint32_t kt = 20 + 32, dt = kt + (uint32_t)keys.size();
    std::string h(20, '\0');
    auto le32 = [](std::string& s, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) s[o + i] = (char)(v >> (8 * i)); };
    auto le16 = [](std::string& s, size_t o, uint32_t v) { s[o] = (char)v; s[o + 1] = (char)(v >> 8); };
    memcpy(&h[0], "\0PSF", 4); le32(h, 4, 0x101); le32(h, 8, kt); le32(h, 12, dt); le32(h, 16, 2);
    std::string idx(32, '\0');
    le16(idx, 0, 0); le16(idx, 2, 0x0204); le32(idx, 4, (uint32_t)strlen(dir) + 1); le32(idx, 8, 32); le32(idx, 12, 0);
    le16(idx, 16, 19); le16(idx, 18, 0x0204); le32(idx, 20, (uint32_t)strlen(sub) + 1); le32(idx, 24, 128); le32(idx, 28, 32);
    return h + idx + keys + data;
}
static void make_flat(const std::string& lib, const char* name)
{
    const std::string d = join(lib, name);
    mkdirs(d);
    const std::string bin = psf_bin("BCUS98125_INFAMOUS2_0", "(Autosave) 3. Forward Momentum");
    FILE* f = fopen(join(d, "PARAM.SFO").c_str(), "wb"); fwrite(bin.data(), 1, bin.size(), f); fclose(f);
    f = fopen(join(d, "STATE").c_str(), "wb"); fputs("state", f); fclose(f);
    f = fopen(join(d, "screen.png").c_str(), "wb"); fclose(f);
}

/* Child modes, run by main through CreateProcess: the real install and driver
 * thread against a scripted "title"; each must end the process itself. */
static int fake_game()
{
    make_sig_table();
    if (if2_checkpoint_install() != 5) return 98;
    ppu_context c{};
    wstr(0x200000, "BCUS98125"); w32(0x210008, 0x200000); c.gpr[5] = 0x210000;
    for (int i = 0; i < 20000; i++) {
        g_frames++;
        if (g_frames == 20) g_reg["cellSaveDataListAutoLoad"](&c);
        if (g_frames == 100) sig("PackLoaded");
        if (g_frames == 300) sig("WarpedToCheckpoint");
        if (g_frames > 300 && g_reg.count("cellPadGetData")) {          /* the title polls the pad */
            ppu_context p{}; p.gpr[4] = 0x100000; g_reg["cellPadGetData"](&p);
            if (vm_read16(0x100000 + 4 + 6) & 0x40) { static int once; if (!once++) fprintf(stderr, "[fake] CROSS seen at frame %u\n", g_frames); }
        }
        Sleep(1);
    }
    return 99;      /* the driver never ended the run */
}
static int child_mismatch()
{
    g_tk_root = "C:\\somewhere\\else";      /* a cellSaveData that ignores PS3_SAVEDATA_DIR */
    if2_checkpoint_install();
    return 99;
}
static int child_nope() { if2_checkpoint_install(); return 99; }

static DWORD run_child(const char* mode, const std::map<std::string, std::string>& envs)
{
    for (auto& kv : envs) _putenv_s(kv.first.c_str(), kv.second.c_str());
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string cmd = std::string("\"") + exe + "\" " + mode;
    STARTUPINFOA si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    assert(CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1234;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    for (auto& kv : envs) _putenv_s(kv.first.c_str(), "");
    return code;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "fake-game")) return fake_game();
    if (argc > 1 && !strcmp(argv[1], "mismatch")) return child_mismatch();
    if (argc > 1 && !strcmp(argv[1], "nope")) return child_nope();
    char tmp[MAX_PATH]; GetTempPathA(sizeof tmp, tmp);
    char base[MAX_PATH]; snprintf(base, sizeof base, "%sif2cp-test-%lu", tmp, (unsigned long)GetCurrentProcessId());
    remove_tree(base);
    const std::string lib = join(base, "lib");
    mkdirs(lib);

    /* install: nothing asked for -> nothing registered, no library touched */
    for (const char* v : {"CHECKPOINT", "CHECKPOINT_AUTOSNAP", "IF2_SIGLOG", "IF2_BOOT", "PS3_SAVEDATA_DIR"}) _putenv_s(v, "");
    _putenv_s("CHECKPOINT_DIR", lib.c_str());
    assert(if2_checkpoint_install() == 0 && g_reg.empty() && g_autosnap == 0 && g_lib.empty());
    /* autosnap asked for: the four save-data wrappers, no pad, no driver */
    _putenv_s("CHECKPOINT_AUTOSNAP", "1");
    assert(if2_checkpoint_install() == 4 && g_reg.count("cellSaveDataListAutoLoad") && g_reg.count("cellSaveDataAutoSave2") &&
           g_reg.count("cellSaveDataListSave2") && g_reg.count("cellSaveDataListLoad2") && !g_reg.count("cellPadGetData") &&
           g_autosnap == 1 && g_lib == lib);
    _putenv_s("CHECKPOINT_AUTOSNAP", "");
    g_reg.clear();

    /* SFO: binary and text */
    const std::string bin = psf_bin("BCUS98125_INFAMOUS2_0", "(Autosave) 3. Forward Momentum");
    assert(sfo_get(bin, "SAVEDATA_DIRECTORY") == "BCUS98125_INFAMOUS2_0");
    assert(sfo_get(bin, "SUB_TITLE") == "(Autosave) 3. Forward Momentum");
    assert(sfo_get(bin, "DETAIL").empty() && sfo_get(bin, "SUB").empty());
    assert(sfo_get("TITLE=a\r\nSUB_TITLE=b c\n", "SUB_TITLE") == "b c");
    assert(sfo_get(bin.substr(0, 30), "SUB_TITLE").empty());         /* truncated: no crash */

    /* script */
    assert(parse_script("0:0x4000|30-90:128,0,128,128|120:0x0008/2"));
    assert(g_script.size() == 3 && g_script[0].f1 == 6 && g_script[1].kind == 1 && g_script[1].st[1] == 0 &&
           g_script[2].mask == 8 && g_script[2].f1 == 122 && g_script_end == 122);
    g_script.clear(); g_script_end = 0;
    assert(!parse_script("x:1") && (g_script.clear(), !parse_script("5-3:1,2,3,4")) &&
           (g_script.clear(), !parse_script("1-3:1,2")));
    g_script.clear();

    /* pad: CROSS + its pressure word, sticks */
    {
        ppu_context c{}; c.gpr[3] = 0; c.gpr[4] = 0x100000;
        g_frames = 100; press(0x4000, 6, "test");
        g_frames = 101; if2_cellPadGetData(&c);
        assert(c.gpr[3] == 0 && vm_read16(0x100000 + 4 + 6) == 0x40 && vm_read16(0x100000 + 4 + 28) == 255);
        g_frames = 108; if2_cellPadGetData(&c);
        assert(vm_read16(0x100000 + 4 + 6) == 0);                     /* released */
        g_ready_frame.store(200); parse_script("10-20:1,2,3,4");
        g_frames = 215; if2_cellPadGetData(&c);
        assert(vm_read16(0x100000 + 4 + 12) == 1 && vm_read16(0x100000 + 4 + 8) == 3);
        g_script.clear(); g_ready_frame.store(0); g_press_mask.store(0);
    }

    /* sandbox: flat (RPCS3 copy) with a binary SFO naming the directory */
    {
        make_flat(lib, "03_flat");
        assert(make_sandbox("03_flat"));
        const std::string root = env("PS3_SAVEDATA_DIR");
        assert(root == g_sandbox && is_file(join(join(root, "BCUS98125_INFAMOUS2_0"), "STATE")) &&
               !is_file(join(join(root, "BCUS98125_INFAMOUS2_0"), "screen.png")));
        assert(!make_sandbox("nope"));
        assert(make_sandbox("@new") && list_dir(env("PS3_SAVEDATA_DIR"), true).empty());
        /* an entry in the savedata\<DIR> layout */
        mkdirs(join(join(join(lib, "04_tree"), "savedata"), "BCUS98125_INFAMOUS2_0"));
        put_save(join(join(lib, "04_tree"), "savedata"), "BCUS98125_INFAMOUS2_0", "s", "x");
        assert(make_sandbox("04_tree") && is_file(join(join(env("PS3_SAVEDATA_DIR"), "BCUS98125_INFAMOUS2_0"), "PARAM.SFO")));
        /* cellSaveData's dot-named work directories and read-only files do not
         * keep a sandbox from being pruned (below); a read-only checkpoint
         * file arrives writable, or the title's next save would fail */
        {
            const std::string old = join(env("PS3_SAVEDATA_DIR"), ".BCUS98125_INFAMOUS2_0.old");
            mkdirs(old);
            const std::string ro = join(old, "STATE");
            FILE* f = fopen(ro.c_str(), "wb"); fputs("x", f); fclose(f);
            assert(SetFileAttributesA(ro.c_str(), FILE_ATTRIBUTE_READONLY));
            const std::string src = join(join(join(join(lib, "04_tree"), "savedata"), "BCUS98125_INFAMOUS2_0"), "STATE");
            assert(SetFileAttributesA(src.c_str(), FILE_ATTRIBUTE_READONLY));
            assert(make_sandbox("04_tree"));
            const DWORD a = GetFileAttributesA(join(join(env("PS3_SAVEDATA_DIR"), "BCUS98125_INFAMOUS2_0"), "STATE").c_str());
            assert(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_READONLY));
            SetFileAttributesA(src.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        /* pruning keeps the newest CHECKPOINT_KEEP_RUNS */
        _putenv_s("CHECKPOINT_KEEP_RUNS", "2");
        Sleep(20); assert(make_sandbox("@new")); Sleep(20); assert(make_sandbox("@new"));
        assert(list_dir(join(lib, "_runs"), true).size() == 2);
        _putenv_s("CHECKPOINT_KEEP_RUNS", "");
    }

    /* save data: boot load, then autosaves kept / deduplicated, by the exact
     * directory cellSaveData reports */
    {
        const std::string root = join(base, "saves");
        g_tk_root = root;
        g_sandbox.clear();
        put_save(root, "BCUS98125_INFAMOUS2_0", std::string(1000, 'x'), "(Autosave) 3. Forward Momentum");
        put_save(root, "BCUS98125_INFAMOUS2_7", std::string(10, 'n'), "a newer unrelated save");
        wstr(0x200000, "BCUS98125");
        w32(0x210000, 0); w32(0x210004, 0); w32(0x210008, 0x200000);
        ppu_context c{}; c.gpr[5] = 0x210000;
        reset_driver(BOOT_OFF); g_autosnap = 1; g_frames = 1000;
        if2_ListAutoLoad(&c);
        assert(g_bootload.load() == 1 && g_loaded_dir == "BCUS98125_INFAMOUS2_0" && g_ref.valid && g_boot_save.dir == g_loaded_dir);
        wstr(0x220000, "BCUS98125_INFAMOUS2_0");
        ppu_context s{}; s.gpr[4] = 0x220000;
        /* continue save right after the boot load, same title: not kept */
        g_save_payload = std::string(1000, 'x'); g_save_payload[5] = 'y';
        g_frames = 1100; if2_AutoSave2(&s);
        assert(list_dir(lib, true).size() == 3);                       /* 03_flat, 04_tree, _runs */
        /* later, a different save: kept as auto-0001 with meta.json */
        g_save_payload = std::string(1000, 'z');
        g_frames = 5000; g_next_subtitle = "(Autosave) 4. Bad Medicine";
        if2_AutoSave2(&s);
        assert(is_file(join(join(join(join(lib, "auto-0001"), "savedata"), "BCUS98125_INFAMOUS2_0"), "STATE")));
        std::string meta; assert(read_file(join(join(lib, "auto-0001"), "meta.json"), meta));
        assert(meta.find("\"subtitle\": \"(Autosave) 4. Bad Medicine\"") != std::string::npos &&
               meta.find("\"source\": \"autosave\"") != std::string::npos &&
               meta.find("\"dir\": \"BCUS98125_INFAMOUS2_0\"") != std::string::npos);
        /* same title, 3 bytes changed: not kept (dedup 16) */
        g_save_payload[0] = 'a'; g_save_payload[1] = 'b'; g_save_payload[2] = 'c';
        g_frames = 6000; if2_AutoSave2(&s);
        assert(!is_dir(join(lib, "auto-0002")));
        /* PS3_SAVEDATA_READONLY: the save completed, nothing written -> nothing kept */
        g_readonly = 1; g_save_payload = std::string(1000, 'w');
        g_frames = 6500; if2_AutoSave2(&s);
        assert(!is_dir(join(lib, "auto-0002")));
        g_readonly = 0;
        /* a bigger change: kept as auto-0002 */
        g_save_payload = std::string(1000, 'q');
        g_frames = 7000; if2_AutoSave2(&s);
        assert(is_dir(join(lib, "auto-0002")));
        /* a Save Game from the menu: the slot cellSaveData wrote, not the newest-looking one */
        g_frames = 7500; ppu_context m{}; if2_ListSave2(&m);
        assert(is_file(join(join(join(join(lib, "auto-0003"), "savedata"), "BCUS98125_INFAMOUS2_3"), "STATE")));
        /* Load Game from the menu: the new reference -- the same save again is not kept */
        if2_ListLoad2(&m);
        assert(g_ref.dir == "BCUS98125_INFAMOUS2_3");
        g_frames = 7600; if2_ListSave2(&m);
        assert(!is_dir(join(lib, "auto-0004")));
        /* a checkpoint run before READY: not kept */
        reset_driver(BOOT_CONTINUE); g_bootload.store(1); g_save_payload = std::string(1000, 'r');
        g_frames = 9000; if2_AutoSave2(&s);
        assert(!is_dir(join(lib, "auto-0004")));
        /* the boot load with no save, and a failed one */
        reset_driver(BOOT_OFF);
        g_load_dir = "";
        if2_ListAutoLoad(&c); assert(g_bootload.load() == 0 && g_loaded_dir.empty());
        g_load_dir = "BCUS98125_INFAMOUS2_0";
        g_load_rc = (int)0x8002B401; reset_driver(BOOT_OFF);
        if2_ListAutoLoad(&c); assert(g_bootload.load() == 2);
        g_load_rc = 0;
        /* a checkpoint run whose cellSaveData read another directory: 3 (fail), never a READY */
        g_sandbox = join(base, "sandbox-x");
        reset_driver(BOOT_CONTINUE);
        if2_ListAutoLoad(&c); assert(g_bootload.load() == 3);
        g_ready_file = join(base, "ready-root.txt"); g_cp = "03_flat";
        run_frames(1);
        assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["status"] == "fail");
        g_sandbox.clear(); g_ready_file.clear(); g_cp.clear(); g_autosnap = 0; g_tk_root.clear();
    }

    /* driver: signals */
    make_sig_table();
    assert(load_signal_names() && g_nsig == 109);
    S_MENU_ON = sig_id("MenuActivate"); S_MENU_OFF = sig_id("MenuDeactivate");
    S_MOVIE_ON = sig_id("MovieStarted"); S_MOVIE_OFF = sig_id("MovieFinished");
    S_WARP = sig_id("WarpedToCheckpoint"); S_RESET = sig_id("ResetWorldPre");
    S_PACK = sig_id("PackLoaded"); S_MISSION = sig_id("MissionStart");
    g_ready_ids = sig_list("CHECKPOINT_READY_ON", "WarpedToCheckpoint");
    g_sig_on.store(1);
    g_ready_file = join(base, "ready.txt");
    g_cp = "test";

    /* continue: the save loads, packs load, a movie plays, warp, settle, READY */
    reset_driver(BOOT_CONTINUE); g_frames = 0;
    run_frames(50); assert(g_phase.load() == P_BOOT);
    boot_load(1); run_frames(1); assert(g_phase.load() == P_RUN);
    sig("PackLoaded"); sig("MovieStarted"); run_frames(100);
    sig("WarpedToCheckpoint"); run_frames(10); assert(g_phase.load() == P_SETTLE);
    run_frames(200); assert(g_phase.load() == P_SETTLE);               /* the movie is still playing */
    sig("MovieFinished"); run_frames(59); assert(g_phase.load() == P_SETTLE);
    run_frames(2); assert(g_phase.load() == P_DONE && g_ready_frame.load() && g_presses == 0);
    { auto kv = read_kv(g_ready_file); assert(kv["status"] == "ready" && kv["confidence"] == "signal" &&
                                               kv["reason"] == "WarpedToCheckpoint" && kv["save_dir"] == "BCUS98125_INFAMOUS2_0" &&
                                               !kv.count("taskjump")); }

    /* continue, no save in the sandbox: fail at once */
    reset_driver(BOOT_CONTINUE); boot_load(0); run_frames(1);
    assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["status"] == "fail" && g_exit_code.load() == 4);

    /* new game: menu -> CROSS after 45 frames -> deactivate -> a second menu -> CROSS -> warp -> READY */
    reset_driver(BOOT_NEWGAME); g_frames = 0; boot_load(0); run_frames(1);
    sig("PackLoaded"); run_frames(300);
    sig("MenuActivate"); run_frames(44); assert(g_presses == 0);
    run_frames(2); assert(g_presses == 1 && g_press_mask.load() == 0x4000);
    run_frames(100); sig("MenuDeactivate"); run_frames(300); assert(g_presses == 1);
    sig("MenuActivate"); run_frames(50); assert(g_presses == 2);
    sig("MenuDeactivate"); sig("PackLoaded"); run_frames(500); assert(g_presses == 2 && g_phase.load() == P_RUN);
    sig("WarpedToCheckpoint"); run_frames(70); assert(g_phase.load() == P_DONE && g_presses == 2);

    /* new game, the main menu sends no MenuActivate: blind CROSS once loading has been quiet, then quiet READY */
    reset_driver(BOOT_NEWGAME); g_frames = 0; boot_load(0); run_frames(1);
    sig("PackLoaded"); run_frames(1300);
    assert(g_presses == 1 && g_phase.load() == P_RUN);                 /* never READY on the idle menu */
    sig("PackLoaded"); run_frames(1150); assert(g_phase.load() == P_RUN);
    run_frames(100); assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["confidence"] == "quiet");

    /* IF2_TASKJUMP: a READY signal before the jump does not count; the jump
     * resets the count; a warp after it is READY */
    reset_driver(BOOT_NEWGAME); g_frames = 0; g_tj = 1; boot_load(0); run_frames(1);
    sig("PackLoaded"); sig("MenuActivate"); run_frames(50); assert(g_presses == 1);
    sig("MenuDeactivate"); sig("WarpedToCheckpoint"); run_frames(3000);
    assert(g_phase.load() == P_RUN);                                   /* waiting for the jump: no READY of any kind */
    g_tj = 2; run_frames(1); assert(g_tj_frame && g_phase.load() == P_RUN);
    run_frames(100); assert(g_phase.load() == P_RUN);                  /* the warp before the jump no longer counts */
    sig("WarpedToCheckpoint"); run_frames(70);
    { auto kv = read_kv(g_ready_file); assert(g_phase.load() == P_DONE && kv["status"] == "ready" &&
                                               kv["confidence"] == "signal" && kv["taskjump"] == "target step #219 active"); }
    /* ... the jump's warp sends WarpedToCheckpoint before this thread sees
     *     the jump: counted from the moment the jump marked (its main-thread
     *     call before the warp), so it is READY */
    reset_driver(BOOT_NEWGAME); g_frames = 0; g_tj = 1; boot_load(0); run_frames(1);
    sig("PackLoaded"); sig("MenuActivate"); run_frames(50); assert(g_presses == 1);
    sig("MenuDeactivate"); sig("WarpedToCheckpoint"); run_frames(300); assert(g_phase.load() == P_RUN);
    g_frames++; if2_checkpoint_taskjump_mark();
    const unsigned markf = g_frames;
    if2_checkpoint_taskjump_mark();                                    /* only the first mark counts */
    sig("WarpedToCheckpoint"); g_frames++;
    g_tj = 2; run_frames(1); assert(g_tj_frame == markf);
    run_frames(70);
    { auto kv = read_kv(g_ready_file); assert(g_phase.load() == P_DONE && kv["status"] == "ready" &&
                                               kv["confidence"] == "signal"); }
    /* ... no READY signal after the jump: quiet READY once nothing happened for kQuiet frames */
    reset_driver(BOOT_CONTINUE); g_frames = 0; g_tj = 1; boot_load(1); run_frames(1);
    sig("PackLoaded"); run_frames(10); g_tj = 2; run_frames(kQuiet - 5); assert(g_phase.load() == P_RUN);
    run_frames(10); assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["confidence"] == "quiet");
    /* ... the jump gave up: fail */
    reset_driver(BOOT_CONTINUE); g_frames = 0; g_tj = 1; boot_load(1); run_frames(5);
    g_tj = 3; run_frames(1);
    assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["status"] == "fail" && g_exit_code.load() == 4);
    /* ... still waiting at the timeout: says so */
    reset_driver(BOOT_CONTINUE); g_frames = 0; g_tj = 1; boot_load(1); run_frames(2);
    g_timeout = 0.0; run_frames(1);
    assert(read_kv(g_ready_file)["status"] == "timeout" &&
           read_kv(g_ready_file)["reason"].find("IF2_TASKJUMP never reached") == 0);
    g_timeout = 600.0;

    /* no signals at all (hook missing): continue -> READY by frames */
    reset_driver(BOOT_CONTINUE); g_frames = 0; boot_load(1); run_frames(1);
    run_frames(kFallback - 1); assert(g_phase.load() == P_RUN);
    run_frames(3); assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["confidence"] == "frames");
    /* ... newgame -> three blind presses, then READY by frames */
    reset_driver(BOOT_NEWGAME); g_frames = 0; boot_load(0); run_frames(1);
    run_frames(kBlind + 2 * kConfirm + 10); assert(g_presses == 3 && g_phase.load() == P_RUN);
    run_frames(kFallback + 10); assert(g_phase.load() == P_DONE);

    /* continue: a menu idle for kMenuStuck frames with nothing loading gets CROSS */
    reset_driver(BOOT_CONTINUE); g_frames = 0; boot_load(1); run_frames(1);
    sig("MenuActivate"); run_frames(kMenuStuck - 5); assert(g_presses == 0);
    run_frames(10); assert(g_presses == 1);

    /* timeout */
    reset_driver(BOOT_CONTINUE); g_timeout = 0.0; run_frames(1);
    assert(g_phase.load() == P_DONE && read_kv(g_ready_file)["status"] == "timeout" && g_exit_code.load() == 5);
    g_timeout = 600.0;

    /* PNG writer */
    {
        const int w = 64, h = 32; const UINT stride = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> px(stride * h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { px[y * stride + x * 3] = (uint8_t)(x * 4); px[y * stride + x * 3 + 2] = (uint8_t)(y * 8); }
        const std::string png = join(base, "t.png");
        assert(write_png(png, w, h, px.data(), stride));
        std::string b; assert(read_file(png, b) && b.size() > 40 && !memcmp(b.data(), "\x89PNG", 4));
        assert(write_bmp(join(base, "t.bmp"), w, h, px.data(), stride));
        assert(screenshot(join(base, "none.png"), 960).empty());      /* no game window here */
    }

    /* the exits that end a run, in child processes */
    {
        const std::string rf = join(base, "child.ready");
        /* a checkpoint that does not exist: 3, status=fail */
        DWORD code = run_child("nope", {{"CHECKPOINT", "nope"}, {"CHECKPOINT_DIR", lib}, {"CHECKPOINT_READY_FILE", rf}});
        assert(code == 3 && read_kv(rf)["status"] == "fail");
        /* cellSaveData not using the sandbox: 4 before any guest code */
        DeleteFileA(rf.c_str());
        code = run_child("mismatch", {{"CHECKPOINT", "03_flat"}, {"CHECKPOINT_DIR", lib}, {"CHECKPOINT_READY_FILE", rf}});
        assert(code == 4 && read_kv(rf)["status"] == "fail" && read_kv(rf)["reason"].find("not the sandbox") != std::string::npos);
        /* a good run: READY on the warp, the pad script, exit 0 after CHECKPOINT_EXIT_AFTER */
        DeleteFileA(rf.c_str());
        code = run_child("fake-game", {{"CHECKPOINT", "03_flat"}, {"CHECKPOINT_DIR", lib}, {"CHECKPOINT_READY_FILE", rf},
                                       {"CHECKPOINT_EXIT_AFTER", "120"}, {"CHECKPOINT_SCRIPT", "10:0x4000"}});
        auto kv = read_kv(rf);
        assert(code == 0 && kv["status"] == "ready" && kv["confidence"] == "signal" &&
               kv["save_dir"] == "BCUS98125_INFAMOUS2_0" && kv["sandbox"].find("_runs") != std::string::npos);
    }

    remove_tree(base);
    puts("if2_checkpoint host checks passed");
    return 0;
}
