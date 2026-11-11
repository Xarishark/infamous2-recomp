/* Host test for src/if2_taskgraph.cpp: includes the file, stubs the runtime
 * it calls, models the game's SetNodeState (0x37A2E8) and WarpToCheckpoint
 * (0x378EA0), and checks, on a synthetic task graph that lives where the
 * game's does -- in the 0x40000000 heap window -- that the watcher sees it
 * (the run-20261014-180859 regression), lists the mission's steps live, jumps
 * (graph, then warp) only from main-loop polls and only once play has reached
 * IF2_TASKJUMP_AFTER, waits for an idle warp manager, falls back to an earlier
 * step when the warp is refused, follows the warp's outcome (the restart flag
 * consumed by the world, or still pending) and reports its status. Synthetic data only;
 * it needs no game files and no build of the game. From the project folder
 * (Windows):
 *
 *   clang-cl /nologo /EHsc /Od -std:c++20 -MD -Wno-everything
 *     -Iexternal\ps3recomp\include -Iexternal\ps3recomp\runtime\ppu
 *     tests\test_if2_taskgraph.cpp /Fe%TEMP%\test_if2_taskgraph.exe /Fo%TEMP%\
 *     && %TEMP%\test_if2_taskgraph.exe
 *
 * Prints "if2_taskgraph host checks passed". */
#include "../src/if2_taskgraph.cpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <algorithm>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); \
                                  exit(1); } } while (0)

/* ---- guest memory: main memory (the BSS objects) and the heap window ---- */
static std::vector<uint8_t> g_lo(0x01000000), g_hi(0x00100000);
constexpr uint32_t kHeap = 0x40400000u;              /* where the run's heap block was */
extern "C" uint32_t g_sys_mem_bump_ptr = kHeap + 0x00100000u;
static int g_bad_reads = 0;      /* reads by the code under test outside the two regions */
static int g_past_top = 0;       /* ... in the heap window at or past the allocator's top */
static uint8_t* at(uint64_t a)
{
    if (a < g_lo.size()) return &g_lo[a];
    if (a >= kHeap && a < kHeap + g_hi.size()) return &g_hi[a - kHeap];
    return nullptr;
}
extern "C" uint8_t vm_read8(uint64_t a)
{
    if (a >= 0x40000000u && a < 0x50000000u && a >= g_sys_mem_bump_ptr) g_past_top++;
    uint8_t* p = at(a);
    if (!p) { g_bad_reads++; return 0; }
    return *p;
}
extern "C" uint16_t vm_read16(uint64_t a) { return (uint16_t)(vm_read8(a) << 8 | vm_read8(a + 1)); }
extern "C" uint32_t vm_read32(uint64_t a) { return (uint32_t)vm_read16(a) << 16 | vm_read16(a + 2); }
/* the test's own reads (the "game"), not counted */
static uint16_t r16(uint32_t a) { uint8_t* p = at(a); return p ? (uint16_t)(p[0] << 8 | p[1]) : 0; }
static uint32_t r32(uint32_t a) { return (uint32_t)r16(a) << 16 | r16(a + 2); }
static void w8(uint32_t a, uint8_t v) { uint8_t* p = at(a); CHECK(p); *p = v; }
static void w16(uint32_t a, uint16_t v) { w8(a, (uint8_t)(v >> 8)); w8(a + 1, (uint8_t)v); }
static void w32(uint32_t a, uint32_t v) { w16(a, (uint16_t)(v >> 16)); w16(a + 2, (uint16_t)v); }

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx = nullptr;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;
extern "C" int32_t cellSysutilCheckCallback(void) { return 0; }
static void (*g_poll)(ppu_context*) = nullptr;
extern "C" void ps3_hle_register_ctx(uint32_t, const char* name, void (*fn)(ppu_context*))
{
    if (!strcmp(name, "cellSysutilCheckCallback")) g_poll = fn;
}
static int g_seq = 0, g_marks = 0, g_mark_seq = 0;
extern "C" void if2_checkpoint_taskjump_mark(void) { g_marks++; g_mark_seq = ++g_seq; }

/* ---- a synthetic task graph: mission 0x459 with 26 linear steps ---- */
constexpr int kN = 40, kFirstStep = 10, kNSteps = 26;
constexpr uint32_t kArr = kHeap, kNode0 = kHeap + 0x1000, kDesc0 = kHeap + 0x4000, kTab = kHeap + 0x8000,
                   kMsnObj = kHeap + 0xC000;
static uint16_t g_slot = 0;
static uint32_t node_ea(int i) { return kNode0 + 0x40u * (uint32_t)i; }
static uint32_t step_oid(int j) { return 0x00010000u + 0x13u * (uint32_t)j; }   /* j 1-based */
static int st(int i) { return (int8_t)g_hi[node_ea(i) + 8 - kHeap]; }
static void set_st(int i, int s) { w8(node_ea(i) + 8, (uint8_t)s); }
static int sidx(int j) { return kFirstStep + j - 1; }                            /* node index of step j */
static uint16_t list(std::initializer_list<int> v)
{
    const uint16_t s = g_slot;
    for (int x : v) w16(kTab + 2u * g_slot++, (uint16_t)x);
    return s;
}
static void links(int i, uint32_t off, std::initializer_list<int> v)
{
    w16(node_ea(i) + off, (uint16_t)v.size());
    w16(node_ea(i) + off + 2, v.size() ? list(v) : 0);
}
static void children(int i, int first, int count, int ndesc)
{
    const uint32_t d = kDesc0 + 0x20u * (uint32_t)ndesc;
    w32(node_ea(i) + 0x1C, d);
    w16(d + 0x08, (uint16_t)count);
    w16(d + 0x10, 1);
    w16(d + 0x12, list({first}));
}
static void build_graph(uint32_t cur_msn_oid = 0x459)
{
    std::fill(g_lo.begin(), g_lo.end(), 0);
    std::fill(g_hi.begin(), g_hi.end(), 0);
    g_slot = 0;
    for (int i = 0; i < kN; i++) {
        w32(kArr + 4u * (uint32_t)i, node_ea(i));
        w16(node_ea(i) + 6, 0xFFFF);
        w16(node_ea(i) + 0x0A, (uint16_t)i);
    }
    w32(node_ea(0), 0x459); w8(node_ea(0) + 4, 3); set_st(0, 1);
    w32(node_ea(1), 0x6354); w8(node_ea(1) + 4, 3);
    for (int i = 2; i < kFirstStep; i++) { w32(node_ea(i), 0x3C000u + (uint32_t)i); w8(node_ea(i) + 4, 1); }
    children(0, kFirstStep, kNSteps, 0);
    for (int j = 1; j <= kNSteps; j++) {
        const int i = sidx(j);
        w32(node_ea(i), step_oid(j)); w8(node_ea(i) + 4, 4); w16(node_ea(i) + 6, 0);
        w32(node_ea(i) + 0x20, j % 3 ? 0x4000u : 0x10u);
        if (j < kNSteps) links(i, 0x0C, {i + 1});
        if (j > 1) links(i, 0x10, {i - 1});
    }
    const int cp = kFirstStep + kNSteps;              /* a checkpoint inside step 8 */
    w32(node_ea(cp), 0x00090000u); w8(node_ea(cp) + 4, 5); w16(node_ea(cp) + 6, (uint16_t)sidx(8));
    children(sidx(8), cp, 1, 1);
    set_st(sidx(1), 1);
    w32(kTgNodes, kArr); w16(kTgCount, kN); w32(kTgIndex, kTab);
    w32(kTgMission, kMsnObj); w32(kMsnObj + 0x14, kMsnObj + 0x40); w32(kMsnObj + 0x40 + 0x10, cur_msn_oid);
}

/* the game's propagation: a completed step activates successors whose
 * predecessors are all complete */
static void apply(int i, int s)
{
    set_st(i, s);
    if (s < 2) return;
    const uint32_t n = node_ea(i);
    const int cnt = (int16_t)r16(n + 0x0C), slot = r16(n + 0x0E);
    for (int k = 0; k < cnt; k++) {
        const int sx = (int16_t)r16(kTab + 2u * (uint32_t)(slot + k));
        const int pc = (int16_t)r16(node_ea(sx) + 0x10), ps = r16(node_ea(sx) + 0x12);
        bool ok = true;
        for (int q = 0; q < pc; q++) ok &= st((int16_t)r16(kTab + 2u * (uint32_t)(ps + q))) > 1;
        if (ok && st(sx) == 0) set_st(sx, 1);
    }
}
static int index_of(uint32_t ea) { for (int i = 0; i < kN; i++) if (node_ea(i) == ea) return i; return -1; }

/* ---- models of the two game functions the jump calls ---- */
static int g_sns = 0, g_bad_ctx = 0, g_sns_before_after = 0, g_after_step = 7;
static bool guest_ctx_ok(ppu_context* c)
{
    return (uint32_t)c->gpr[3] == kTg && c->gpr[2] == kToc && c == g_active_ctx && c->thread_id == 7 &&
           (uint32_t)c->gpr[1] < 0x00F00000u && !(c->gpr[1] & 0xF) && c->gpr[13] == 0x1234;
}
void func_0037A2E8(ppu_context* c)
{
    if (!guest_ctx_ok(c)) g_bad_ctx++;
    g_sns++;
    if (st(sidx(g_after_step)) < 1) g_sns_before_after++;
    const int i = index_of((uint32_t)c->gpr[4]);
    if (i < 0) { g_bad_ctx++; return; }
    apply(i, (int)c->gpr[5]);
}
struct WarpCall { uint32_t msn, oid, flags, b7; uint64_t r8; int seq; };
static std::vector<WarpCall> g_warps;
static uint32_t g_refuse_oid = 0;
void func_00378EA0(ppu_context* c)
{
    if (!guest_ctx_ok(c)) g_bad_ctx++;
    g_warps.push_back({(uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6], (uint32_t)c->gpr[7],
                       c->gpr[8], ++g_seq});
    c->gpr[3] = 0;
    if (r32(kWarpBusy) || (uint32_t)c->gpr[5] == g_refuse_oid) return;
    if (at(kWarpLock) && *at(kWarpLock) && !(c->gpr[6] & 8)) return;   /* 0x36BA08's lock */
    if ((c->gpr[6] & 2) && r32(r32(r32(kTgMission) + 0x14) + 0x10) == (uint32_t)c->gpr[4])
        w8(kRestartFlag, 1);
    c->gpr[3] = 1;
}

/* ---- driving ---- */
static void scenario(const char* jump, const char* mode = nullptr, const char* after = nullptr,
                     const char* tasklog = nullptr, uint32_t cur_msn = 0x459)
{
    _putenv_s("IF2_TASKJUMP", jump ? jump : "");
    _putenv_s("IF2_TASKJUMP_MODE", mode ? mode : "");
    _putenv_s("IF2_TASKJUMP_AFTER", after ? after : "");
    _putenv_s("IF2_TASKLOG", tasklog ? tasklog : "");
    _putenv_s("CHECKPOINT", "test");
    build_graph(cur_msn);
    g_poll = nullptr;
    g_sns = g_bad_ctx = g_sns_before_after = 0; g_after_step = after ? atoi(after) : 7;
    g_warps.clear(); g_refuse_oid = 0; g_marks = g_mark_seq = 0; g_seq = 0; g_bad_reads = g_past_top = 0;
    g_sys_mem_bump_ptr = kHeap + 0x00100000u;
}
static ppu_context mkctx(uint32_t lr)
{
    ppu_context c;
    memset(&c, 0, sizeof c);
    c.thread_id = 7; c.gpr[1] = 0x00F00000u; c.gpr[13] = 0x1234; c.lr = lr;
    return c;
}
/* one frame: the game (play completes the active step every 3rd frame, up to
 * step `play_to`), then the main loop's poll */
static void frame(int f, int play_to, uint32_t lr = kMainLoopRet)
{
    if (f % 3 == 2)
        for (int j = 1; j <= kNSteps; j++)
            if (st(sidx(j)) == 1) { if (j < play_to) apply(sidx(j), 3); break; }
    ppu_context c = mkctx(lr);
    g_active_ctx = &c;
    g_poll(&c);
    CHECK(c.gpr[1] == 0x00F00000u && c.gpr[13] == 0x1234 && c.lr == lr);   /* caller ctx untouched */
    g_active_ctx = nullptr;
}
static int active_count(int* which)
{
    int n = 0;
    for (int j = 1; j <= kNSteps; j++) if (st(sidx(j)) == 1) { n++; *which = j; }
    return n;
}
static std::string status(int* s)
{
    char b[256];
    *s = if2_taskjump_status(b, sizeof b);
    return b;
}

int main()
{
    int s, which = 0;

    /* guest_ok: main memory, the heap window up to the allocator's top */
    g_sys_mem_bump_ptr = kHeap + 0x00100000u;
    CHECK(guest_ok(0x00A94E10, 4) && guest_ok(0x0FFFFFFC, 4) && !guest_ok(0x0FFFFFFE, 4));
    CHECK(guest_ok(0x40000000, 4) && guest_ok(kHeap, 0x1000) && guest_ok(g_sys_mem_bump_ptr - 4, 4));
    CHECK(!guest_ok(g_sys_mem_bump_ptr - 2, 4) && !guest_ok(g_sys_mem_bump_ptr, 4) && !guest_ok(0x3FFFFFFC, 4));
    CHECK(!guest_ok(0x10000000, 4) && !guest_ok(0xD0000000, 4) && !guest_ok(0xFFFFFFF0, 0x20) && !guest_ok(0x100, 4));
    g_sys_mem_bump_ptr = 0;
    CHECK(!guest_ok(0x40000000, 4));

    /* 1. The regression: a graph in the heap window is seen (IF2_TASKLOG only). */
    scenario(nullptr, nullptr, nullptr, "1");
    CHECK(if2_taskgraph_install() == 1 && g_poll);
    frame(0, 1);
    CHECK(g_nodes_n == kN && g_nodes_ea == kArr && g_bad_reads == 0 && g_past_top == 0);
    CHECK(if2_taskjump_status(nullptr, 0) == 0);

    /* 2. A node table past the allocator's top is never read. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    g_sys_mem_bump_ptr = kHeap + 0x40;                /* the node table (40 entries) ends past it */
    for (int f = 0; f < 10; f++) frame(f, 1);
    CHECK(g_nodes_n == 0 && g_bad_reads == 0 && g_past_top == 0 && g_sns == 0 && g_warps.empty());
    CHECK(status(&s).find("no task graph yet") != std::string::npos && s == 1);

    /* 3. list: live states, no change to the game; the log says what it sees. */
    scenario("list");
    CHECK(if2_taskgraph_install() == 1);
    {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\if2_tg_test_%lu.txt", getenv("TEMP") ? getenv("TEMP") : ".",
                 (unsigned long)GetCurrentProcessId());
        fflush(stderr);
        const int saved = _dup(2);
        FILE* f = fopen(path, "w");
        CHECK(f && saved >= 0);
        _dup2(_fileno(f), 2);
        for (int fr = 0; fr < 60; fr++) frame(fr, 12);
        fflush(stderr);
        _dup2(saved, 2);
        _close(saved);
        fclose(f);
        std::string log;
        f = fopen(path, "r");
        CHECK(f);
        char line[512];
        while (fgets(line, sizeof line, f)) log += line;
        fclose(f);
        remove(path);
        CHECK(log.find("[if2-task]") != std::string::npos && log.find("task graph: 40 nodes") != std::string::npos);
        CHECK(log.find("first main-loop poll") != std::string::npos);
        CHECK(log.find("mission 00000459 is node #0") != std::string::npos);
        CHECK(log.find("step 16 battle") != std::string::npos);       /* the table, with names */
        CHECK(log.find("[beat]") != std::string::npos);
        CHECK(log.find("step 2 continue_i1_game") != std::string::npos && log.find("-> ACTIVE") != std::string::npos);
    }
    CHECK(g_sns == 0 && g_warps.empty() && if2_taskjump_status(nullptr, 0) == 0);
    CHECK(active_count(&which) == 1 && which == 12);

    /* 4. The jump (warp mode): play reaches step 8 by itself; the jump waits
     *    for step 7, then for 30 idle warp-manager frames, completes 8..15
     *    with SetNodeState and warps to step 16 with ReloadWorld|RestartMsn|Wait. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    CHECK(status(&s).find("first main-loop poll") != std::string::npos && s == 1);
    int f7 = -1, first_sns = -1;
    for (int f = 0; f < 200; f++) {
        frame(f, 8);
        if (f7 < 0 && st(sidx(7)) >= 1) f7 = f;
        if (first_sns < 0 && g_sns) first_sns = f;
    }
    CHECK(f7 >= 0 && first_sns >= f7 + 30);          /* the idle settle */
    CHECK(active_count(&which) == 1 && which == 16);
    for (int j = 1; j < 16; j++) CHECK(st(sidx(j)) >= 2);
    CHECK(g_sns_before_after == 0 && g_bad_ctx == 0 && g_bad_reads == 0);
    CHECK(g_warps.size() == 1);
    CHECK(g_warps[0].msn == 0x459 && g_warps[0].oid == step_oid(16) && g_warps[0].flags == 0x13 &&
          g_warps[0].b7 == 1 && g_warps[0].r8 == ~0ull);
    CHECK(g_marks == 1 && g_mark_seq < g_warps[0].seq);
    CHECK(vm_read8(kRestartFlag) == 1);
    CHECK(status(&s).find("WarpToCheckpoint to step 16") != std::string::npos && s == 2);
    for (int f = 200; f < 260; f++) frame(f, 8);      /* done: nothing more happens */
    CHECK(g_warps.size() == 1 && which == 16);
    /* ... and the warp's outcome is followed: the warp manager runs it, the
     *     world consumes the restart flag; the status says so */
    CHECK(g_watch && g_wbusy == 0 && g_flag_set);
    w32(kWarpBusy, 0x40123400);
    for (int f = 260; f < 270; f++) frame(f, 8);
    CHECK(g_watch && g_wbusy == 1);
    w8(kRestartFlag, 0);
    frame(270, 8);
    CHECK(g_watch && !g_flag_set && status(&s).find("missions restarted") != std::string::npos && s == 2);
    w32(kWarpBusy, 0);
    frame(271, 8);
    CHECK(!g_watch && g_wbusy == 2 && g_warps.size() == 1 && g_sns == 8);   /* steps 8..15 */

    /* 5. A busy warp manager holds the jump (graph untouched) until it is idle. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    w32(kWarpBusy, 0x40123400);
    for (int f = 0; f < 150; f++) frame(f, 8);
    CHECK(g_sns == 0 && g_warps.empty());
    CHECK(status(&s).find("busy") != std::string::npos && s == 1);
    w32(kWarpBusy, 0);
    for (int f = 150; f < 250; f++) frame(f, 8);
    CHECK(active_count(&which) == 1 && which == 16 && g_warps.size() == 1 && status(&s).size() && s == 2);

    /* 6. The target refused: the nearest earlier step is warped to. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    g_refuse_oid = step_oid(16);
    for (int f = 0; f < 200; f++) frame(f, 8);
    CHECK(g_warps.size() == 2 && g_warps[0].oid == step_oid(16) && g_warps[1].oid == step_oid(15));
    CHECK(active_count(&which) == 1 && which == 16);
    CHECK(status(&s).find("the target was refused") != std::string::npos && s == 2);

    /* 7. Graph only: no warp. */
    scenario("16", "graph");
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 120; f++) frame(f, 8);
    CHECK(g_warps.empty() && active_count(&which) == 1 && which == 16 && g_marks == 1);
    CHECK(status(&s).find("graph only") != std::string::npos && s == 2);

    /* 8. Polls that are not the main loop's never act; once the main loop
     *    polls, the others are ignored. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 150; f++) frame(f, 8, 0x00108ED8);
    CHECK(g_nodes_n == kN && g_sns == 0 && g_warps.empty());
    CHECK(status(&s).find("not the main loop") != std::string::npos && s == 1);
    const uint64_t np = g_npoll;
    for (int f = 150; f < 300; f++) frame(f, 8);
    CHECK(active_count(&which) == 1 && which == 16 && g_warps.size() == 1);
    const uint64_t np2 = g_npoll;
    frame(300, 8, 0x00108ED8);
    CHECK(g_npoll == np2 && np2 > np);
    /* ... and if the main loop never shows up, the one polling thread acts
     *     after 1800 polls */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < kOtherActAfter; f++) frame(f, 8, 0x00108ED8);
    CHECK(g_sns == 0 && g_warps.empty());
    for (int f = kOtherActAfter; f < kOtherActAfter + 60; f++) frame(f, 8, 0x00108ED8);
    CHECK(active_count(&which) == 1 && which == 16 && g_warps.size() == 1);

    /* 9. A step by oid. */
    char oid[16];
    snprintf(oid, sizeof oid, "0x%X", step_oid(12));
    scenario(oid);
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 120; f++) frame(f, 8);
    CHECK(active_count(&which) == 1 && which == 12 && g_warps.size() == 1 && g_warps[0].oid == step_oid(12));

    /* 10. No such step: gives up (the boot driver fails the run). */
    scenario("40");
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 60; f++) frame(f, 8);
    CHECK(status(&s).find("no step number") != std::string::npos && s == 3 && g_sns == 0);

    /* 11. Play got there by itself: nothing is done. */
    scenario("8");
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 120; f++) frame(f, 9);
    CHECK(g_sns == 0 && g_warps.empty() && g_marks == 1);
    CHECK(status(&s).find("by play itself") != std::string::npos && s == 2);

    /* 12. IF2_TASKJUMP_AFTER: nothing before that step is reached by play. */
    scenario("16", nullptr, "9");
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 60; f++) frame(f, 8);         /* play stops at 8 */
    CHECK(g_sns == 0 && status(&s).find("waiting for play to reach step 9") != std::string::npos && s == 1);
    for (int f = 60; f < 200; f++) frame(f, 10);
    CHECK(g_sns_before_after == 0 && active_count(&which) == 1 && which == 16 && g_warps.size() == 1);

    /* 13. The current mission object is another mission: the warp is taken
     *     but the restart flag is not set (logged, and in the status). */
    scenario("16", nullptr, nullptr, nullptr, 0x6354);
    CHECK(if2_taskgraph_install() == 1);
    for (int f = 0; f < 200; f++) frame(f, 8);
    CHECK(g_warps.size() == 1 && vm_read8(kRestartFlag) == 0);
    CHECK(status(&s).find("restart flag 0") != std::string::npos && s == 2);

    /* 14. The mission inactive: the jump waits and says so. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    set_st(0, 0);
    for (int f = 0; f < 30; f++) frame(f, 8);
    CHECK(g_sns == 0 && status(&s).find("mission 459 (#0) is inactive") != std::string::npos && s == 1);

    /* 15. Neither variable: nothing registered. */
    scenario(nullptr);
    CHECK(if2_taskgraph_install() == 0 && !g_poll);

    /* 16. The world never consumes the restart flag: after kWatchPolls polls
     *     the log and the status say the mission script was not rebuilt; the
     *     jump itself stays done (status 2), and nothing is called again. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    int fw = -1;
    for (int f = 0; f < 200 && fw < 0; f++) { frame(f, 8); if (!g_warps.empty()) fw = f; }
    CHECK(fw >= 0 && g_watch && g_flag_set);
    for (int f = fw + 1; f < fw + kWatchPolls; f++) frame(f, 8);
    CHECK(g_watch && status(&s).find("pending") == std::string::npos);
    frame(fw + kWatchPolls, 8);
    CHECK(!g_watch && status(&s).find("restart flag still pending") != std::string::npos && s == 2);
    CHECK(g_warps.size() == 1 && g_sns == 8);

    /* 18. Every warp refused (the warp lock is set): kWarpTries attempts, each
     *     from the target down to step 7, then the jump gives up (status 3,
     *     naming the lock) -- the boot driver fails the run. */
    scenario("16");
    CHECK(if2_taskgraph_install() == 1);
    w8(kWarpLock, 1);
    for (int f = 0; f < 800; f++) frame(f, 8);
    CHECK(g_warps.size() == (size_t)kWarpTries * 10 && g_warps.front().oid == step_oid(16) &&
          g_warps[9].oid == step_oid(7));
    CHECK(status(&s).find("warp lock 0x00A32EB0 = 1") != std::string::npos && s == 3 && !g_watch);

    /* 17. The warp flags by the game's names (0x94B89C). */
    char fnb[160];
    CHECK(!strcmp(flag_names(0x12, fnb, sizeof fnb), "RestartMsn|Wait"));
    CHECK(!strcmp(flag_names(0x13, fnb, sizeof fnb), "ReloadWorld|RestartMsn|Wait"));
    CHECK(!strcmp(flag_names(0, fnb, sizeof fnb), "none"));
    CHECK(!strcmp(flag_names(0x80000002u, fnb, sizeof fnb), "RestartMsn|0x80000000"));
    CHECK(!strcmp(flag_names(0x13, fnb, 8), "ReloadW"));

    printf("if2_taskgraph host checks passed\n");
    return 0;
}
