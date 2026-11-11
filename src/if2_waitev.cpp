/*
 * inFamous 2 -- event-driven waits for the hot guest poll loops (WAIT_EVENTS,
 * runtime/syscalls/wait_events.h). M6a of the fps roadmap.
 *
 * Measured at the fairground gate (40 fps, 2026-10-15, PS3_POLLTOP +
 * threadcpu): the main thread spends ~10 ms of each 25 ms frame in
 * func_003374BC's usleep(10) loop and ~4.7 ms in the WWS barrier usleep(1)
 * loops, and the SPU Light Task Kicker spins ~96% of a core in its
 * 900/100/100/500 us polls -- every sub-ms usleep is a SwitchToThread + QPC
 * spin in this runtime. Each loop re-reads a few guest words after every
 * sleep; these sites say which, so the sleep can block until a writer changes
 * them (or the period runs out) and the guest then re-reads them itself.
 *
 *   main-kicker   func_003374BC, sc 0x3374F0 (lr 0x1DB7DC / 0x337554):
 *                 while (*(obj+0x318) != 4) usleep(10); obj in r31. The
 *                 kicker thread stores 4 and then enters lv2
 *                 (sys_semaphore_wait at 0x337484): PPU poke.
 *   main-wws      func_003705D8, sc 0x370630 / 0x370658 (lr 0x370600 /
 *                 0x370640 / 0x370668): until func_00505490(r31 = queue+0x80,
 *                 r29 = row) -- the row's 7 SPU lanes reach head + the run of
 *                 queued list entries. Lanes move by SPU PUTLLC: SPU poke.
 *   main-wws2     func_00371000, sc 0x3710A4 / 0x3710D0 (lr 0x37108C /
 *                 0x3710B8 / 0x3710E4): until func_00504D90(r28, r27, r29) --
 *                 the lanes reach r29.
 *   light-kicker  func_00337408 (SPU Light Task Kicker), sc 0x337458 (lr
 *                 0x337430 / 0x337464): usleep(table[state]) between steps of
 *                 func_00337040 while state (obj+0x318) != 4:
 *                   0 / 1: RSX label *(obj+0x184) / *(obj+0x188) >= the frame's
 *                          expected value (obj+0x190 / +0x194): RSX or
 *                          SPU-DMA poke (the walker / a lighting job)
 *                   2:     both rows of the lighting job queue done (as above)
 *                   3:     u16 at obj+0x100 == 0xFFFF (SPU-written)
 *   edge-kicker   func_00338CC8 (Edge Geom Task Kicker), sc 0x338D04 (lr
 *                 0x338CE8 / 0x338D10): usleep(500) between steps of
 *                 func_00338AC4 while state (obj+0x6A0) != 3; states 1 / 2
 *                 wait for a row of a WWS queue picked by a slot (see e_arm).
 *
 * Each site names the writer classes that can satisfy it (w->src); a PPU
 * syscall evaluates only main-kicker's and the light kicker's tail
 * predicates, so the main thread's own syscalls do not walk the kickers'
 * WWS lists. A change by a class left out is seen at the timeout, which
 * is never shorter than today's poll period.
 *
 * The predicates are wake HINTS. A wrong one cannot change what the guest
 * computes -- it still re-reads and decides itself -- it can only cost a
 * timeout (no wake: one poll period, rounded up to the host timer slice) or
 * a plain poll (ready already: the call sleeps as before). WAIT_EVENTS_STATS=1
 * shows which ([wev] lines: wake signal / timeout / late-ready, plain-ready).
 *
 * Not covered: the Bink movie pacer (func_001D1F48's loop, sc 0x1D1F7C, lr
 * 0x1D1FB8: usleep(10) until BinkWait (func_006DAFB0) says the next frame is
 * due -- a clock wait, nothing writes memory to signal it; movies only) and
 * the boot / menu pollers (lr 0x133640, 0xAAD5C).
 *
 * Every site's code is checked against the disc EBOOT (BCUS98125, APP_VER
 * 01.00) before it is registered. WAIT_EVENTS=0 registers nothing (today's
 * spin).
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "ppu_context.h"
#include "wait_events.h"
#include "if2_version.h"

namespace {

/* ---- WWS job-queue sync rows (func_00504D90 / func_00505490) ------------
 * The word at queue+0x80 points to the queue's sync area; row i is 16 bytes
 * at +0x40 + 16*i: a u16 head, then 7 u16 lanes, one per SPU, which the SPU
 * job manager advances with PUTLLC. The list for row i is at *(+0x30 + 4*i). */
uint32_t wws_min_lane(uint32_t row)
{
    uint32_t m = wev_rd16(row + 2u);
    for (uint32_t o = 4u; o <= 0xEu; o += 2u) {
        const uint32_t v = wev_rd16(row + o);
        if (v < m) m = v;
    }
    return m;
}

/* func_00505490's target: head + the run of list entries from list[head]
 * whose u16 at +2 is non-zero (bounded: a corrupt list reads as "not yet"). */
uint32_t wws_target(uint32_t row, uint32_t list_ptr_ea)
{
    const uint32_t head = wev_rd16(row);
    const uint32_t e = wev_rd32(list_ptr_ea) + 8u * head;
    uint32_t n = 0;
    if (wev_rd16(e + 2u) != 0u)
        do { n++; } while (n < 4096u && wev_rd16(e + 8u * n + 2u) != 0u);
    return n >= 4096u ? 0xFFFFFFFFu : head + n;
}

/* ---- main-kicker -------------------------------------------------------- */
int k_arm(const ppu_context* ctx, uint64_t, wev_watch* w)
{
    const uint32_t obj = (uint32_t)ctx->gpr[31];
    w->src = WEV_SRC_PPU;                    /* the kicker thread's store, then its sem_wait */
    w->p[0] = obj + 0x318u;
    w->n = 1; w->ea[0] = obj + 0x318u; w->len[0] = 4u;
    return 1;
}
int k_ready(const wev_watch* w) { return wev_rd32(w->p[0]) == 4u; }

/* ---- main-wws ------------------------------------------------------------ */
int b1_arm(const ppu_context* ctx, uint64_t, wev_watch* w)
{
    const uint32_t jqp = (uint32_t)ctx->gpr[31], idx = (uint32_t)ctx->gpr[29];
    const uint32_t base = wev_rd32(jqp);
    const uint32_t row = base + 0x40u + (idx << 4), lst = base + 0x30u + (idx << 2);
    w->src = WEV_SRC_SPU;                    /* the lanes: SPU PUTLLC */
    w->p[0] = row; w->p[1] = lst;
    w->n = 1; w->ea[0] = row; w->len[0] = 16u;
    return 1;
}
int b1_ready(const wev_watch* w) { return wws_min_lane(w->p[0]) >= wws_target(w->p[0], w->p[1]); }

/* ---- main-wws2 ----------------------------------------------------------- */
int b2_arm(const ppu_context* ctx, uint64_t, wev_watch* w)
{
    const uint32_t jqp = (uint32_t)ctx->gpr[28], idx = (uint32_t)ctx->gpr[27];
    const uint32_t row = wev_rd32(jqp) + 0x40u + (idx << 4);
    w->src = WEV_SRC_SPU;
    w->p[0] = row; w->p[1] = (uint32_t)ctx->gpr[29];
    w->n = 1; w->ea[0] = row; w->len[0] = 16u;
    return 1;
}
int b2_ready(const wev_watch* w) { return wws_min_lane(w->p[0]) >= w->p[1]; }

/* ---- light-kicker -------------------------------------------------------- */
enum : uint32_t { L_LABEL = 1, L_WWS = 2, L_TAIL = 3 };
int l_arm(const ppu_context* ctx, uint64_t, wev_watch* w)
{
    const uint32_t obj = (uint32_t)ctx->gpr[31];
    switch (wev_rd32(obj + 0x318u)) {
    case 0: case 1: {
        const uint32_t st = wev_rd32(obj + 0x318u);
        const uint32_t lab = wev_rd32(obj + (st ? 0x188u : 0x184u));
        const uint32_t exp = obj + ((1u - wev_rd32(obj + 0x110u)) << 2) + 0x190u;
        w->src = WEV_SRC_RSX | WEV_SRC_SPU;   /* the walker or a lighting job's DMA */
        w->p[0] = L_LABEL; w->p[1] = lab; w->p[2] = exp;
        w->n = 2; w->ea[0] = lab; w->len[0] = 4u; w->ea[1] = exp; w->len[1] = 4u;
        return 1;
    }
    case 2: {
        /* r30 = the globals block (u32 at TOC-0x7AD4, 1.04: -0x7ACC); the queue is at -0x7FE4 */
        const uint32_t jq = wev_rd32((uint32_t)ctx->gpr[30] - 0x7FE4u);
        const uint32_t base = wev_rd32(jq + 0x80u);
        w->src = WEV_SRC_SPU;
        w->p[0] = L_WWS; w->p[1] = base;
        w->n = 1; w->ea[0] = base + 0x40u; w->len[0] = 32u;
        return 1;
    }
    case 3:
        w->src = WEV_SRC_SPU | WEV_SRC_PPU;   /* whoever writes the tail marker */
        w->p[0] = L_TAIL; w->p[1] = obj + 0x100u;
        w->n = 1; w->ea[0] = obj + 0x100u; w->len[0] = 2u;
        return 1;
    default:
        return 0;
    }
}
int l_ready(const wev_watch* w)
{
    switch (w->p[0]) {
    case L_LABEL: return wev_rd32(w->p[2]) <= wev_rd32(w->p[1]);   /* expected <= label (unsigned) */
    case L_WWS: {
        const uint32_t b = w->p[1];
        return wws_min_lane(b + 0x40u) >= wws_target(b + 0x40u, b + 0x30u) &&
               wws_min_lane(b + 0x50u) >= wws_target(b + 0x50u, b + 0x34u);
    }
    case L_TAIL: return wev_rd16(w->p[1]) == 0xFFFFu;
    }
    return 0;
}

/* ---- edge-kicker ----------------------------------------------------------
 * func_00338AC4's waits, per state (obj+0x6A0):
 *   1: slot obj+0x6A4 (s8 queue index b, u8 id, u16 target): func_003707BC
 *      on queue q = *(*(r2-0x7AD0) - 0x7FE8) + b*0x180 -- the id names row 0
 *      (byte q+0xB8) or row 1 (q+0xD4) of q's sync area *(q+0x80); ready
 *      when that row's lanes reach the target (no row: ready at once).
 *   2: while the ring counters at *(obj+4) (+2 / +4) differ the step works
 *      on every poll (plain sleeps); once equal, slot obj+0x6A8 as in state
 *      1 (func_003710F8), plus, when byte q+0x104 holds the id,
 *      func_00505490 on the OTHER row.
 * b == -1 advances at once: no wait to arm. */
uint32_t edge_queue(const ppu_context* ctx, uint32_t slot)
{
    const int32_t b = (int8_t)wev_rd8(slot);
    if (b == -1) return 0;
    const uint32_t globals = wev_rd32((uint32_t)ctx->gpr[2] - IF2_V(0x7AD0u, 0x7AC8u));   /* 1.04: one TOC slot moved */
    return wev_rd32(globals - 0x7FE8u) + (uint32_t)(b * 0x180);
}
int edge_row(uint32_t q, uint32_t slot)
{
    const uint32_t id = wev_rd8(slot + 1u);
    if (wev_rd8(q + 0xB8u) == id) return 0;
    if (wev_rd8(q + 0xD4u) == id) return 1;
    return -1;
}
enum : uint32_t { E_S1 = 1, E_S2 = 2 };
int e_arm(const ppu_context* ctx, uint64_t, wev_watch* w)
{
    const uint32_t obj = (uint32_t)ctx->gpr[31];
    const uint32_t st = wev_rd32(obj + 0x6A0u);
    uint32_t slot;
    if (st == 1u) slot = obj + 0x6A4u;
    else if (st == 2u) {
        const uint32_t ring = wev_rd32(obj + 0x4u);
        if (wev_rd16(ring + 2u) != wev_rd16(ring + 4u)) return 0;   /* the step has work */
        w->p[3] = ring;
        slot = obj + 0x6A8u;
    } else return 0;
    const uint32_t q = edge_queue(ctx, slot);
    if (!q) return 0;
    const uint32_t base = wev_rd32(q + 0x80u);
    w->src = WEV_SRC_SPU;   /* lanes; new ring work (a PPU store) waits for the timeout = today's poll */
    w->p[0] = st == 1u ? E_S1 : E_S2; w->p[1] = q; w->p[2] = slot;
    w->n = 1; w->ea[0] = base + 0x40u; w->len[0] = 32u;   /* rows 0 and 1 */
    return 1;
}
int e_ready(const wev_watch* w)
{
    const uint32_t q = w->p[1], slot = w->p[2];
    if (w->p[0] == E_S2 && wev_rd16(w->p[3] + 2u) != wev_rd16(w->p[3] + 4u)) return 1;
    const int r = edge_row(q, slot);
    const uint32_t base = wev_rd32(q + 0x80u);
    if (r >= 0 && wws_min_lane(base + 0x40u + ((uint32_t)r << 4)) < wev_rd16(slot + 2u)) return 0;
    if (w->p[0] == E_S1 || wev_rd8(q + 0x104u) != wev_rd8(slot + 1u)) return 1;
    if (r < 0) return 1;                         /* the guest's odd row index: let it look */
    const uint32_t o = (uint32_t)(1 - r);
    return wws_min_lane(base + 0x40u + (o << 4)) >= wws_target(base + 0x40u + (o << 4), base + 0x30u + (o << 2));
}

/* ---- this EBOOT's code at each site ------------------------------------- */
struct code_word { uint32_t ea, word; };
struct site_def { wev_site site; code_word code[10]; };

const site_def kSites[] = {
    { { "main-kicker", { IF2_EA(0x003374F4u, 0x00350D18u) }, k_arm, k_ready,
        { IF2_EA(0x001DB7DCu, 0x001EF2A0u), IF2_EA(0x00337554u, 0x00350D78u),
          IF2_EA(0x00337928u, 0x0035114Cu) } },          /* its callers (the loop makes no call) */
      { { IF2_EA(0x003374ECu, 0x00350D10u), 0x3960008Du }, { IF2_EA(0x003374F0u, 0x00350D14u), 0x44000002u },
        { IF2_EA(0x003374F4u, 0x00350D18u), 0x83BF0318u }, { IF2_EA(0x003374F8u, 0x00350D1Cu), 0x2F9D0004u } } },
    { { "main-wws", { IF2_EA(0x00370634u, 0x0038AA70u), IF2_EA(0x0037065Cu, 0x0038AA98u) }, b1_arm, b1_ready,
        { IF2_EA(0x00370600u, 0x0038AA3Cu), IF2_EA(0x00370640u, 0x0038AA7Cu),
          IF2_EA(0x00370668u, 0x0038AAA4u) } },          /* after its func_00505490 calls */
      { { IF2_EA(0x0037062Cu, 0x0038AA68u), 0x3960008Du }, { IF2_EA(0x00370630u, 0x0038AA6Cu), 0x44000002u },
        { IF2_EA(0x00370634u, 0x0038AA70u), 0x7FE3FB78u }, { IF2_EA(0x00370654u, 0x0038AA90u), 0x3960008Du },
        { IF2_EA(0x00370658u, 0x0038AA94u), 0x44000002u }, { IF2_EA(0x0037065Cu, 0x0038AA98u), 0x7FE3FB78u } } },
    { { "main-wws2", { IF2_EA(0x003710A8u, 0x0038B4E4u), IF2_EA(0x003710D4u, 0x0038B510u) }, b2_arm, b2_ready,
        { IF2_EA(0x0037108Cu, 0x0038B4C8u), IF2_EA(0x003710B8u, 0x0038B4F4u),
          IF2_EA(0x003710E4u, 0x0038B520u) } },          /* after its func_00504D90 calls */
      { { IF2_EA(0x003710A0u, 0x0038B4DCu), 0x3960008Du }, { IF2_EA(0x003710A4u, 0x0038B4E0u), 0x44000002u },
        { IF2_EA(0x003710A8u, 0x0038B4E4u), 0x7F83E378u }, { IF2_EA(0x003710CCu, 0x0038B508u), 0x3960008Du },
        { IF2_EA(0x003710D0u, 0x0038B50Cu), 0x44000002u }, { IF2_EA(0x003710D4u, 0x0038B510u), 0x7F83E378u } } },
    { { "light-kicker", { IF2_EA(0x0033745Cu, 0x00350C80u) }, l_arm, l_ready,
        { IF2_EA(0x00337430u, 0x00350C54u), IF2_EA(0x00337464u, 0x00350C88u) } },   /* after its func_00337040 calls */
      { { IF2_EA(0x00337450u, 0x00350C74u), 0x3960008Du }, { IF2_EA(0x00337458u, 0x00350C7Cu), 0x44000002u },
        { IF2_EA(0x0033745Cu, 0x00350C80u), 0x7FE3FB78u }, { IF2_EA(0x00337464u, 0x00350C88u), 0x801F0318u },
        /* the kicker's poll periods per state: 900 / 100 / 100 / 500 us */
        { IF2_EA(0x006F2360u, 0x0074A8B0u), 0x00000384u }, { IF2_EA(0x006F2364u, 0x0074A8B4u), 0x00000064u },
        { IF2_EA(0x006F2368u, 0x0074A8B8u), 0x00000064u }, { IF2_EA(0x006F236Cu, 0x0074A8BCu), 0x000001F4u } } },
    { { "edge-kicker", { IF2_EA(0x00338D08u, 0x0035252Cu) }, e_arm, e_ready,
        { IF2_EA(0x00338CE8u, 0x0035250Cu), IF2_EA(0x00338D10u, 0x00352534u) } },   /* after its func_00338AC4 calls */
      { { IF2_EA(0x00338D00u, 0x00352524u), 0x3960008Du }, { IF2_EA(0x00338D04u, 0x00352528u), 0x44000002u },
        { IF2_EA(0x00338D08u, 0x0035252Cu), 0x7FE3FB78u }, { IF2_EA(0x00338D10u, 0x00352534u), 0x801F06A0u },
        { IF2_EA(0x00338D20u, 0x00352544u), 0x386001F4u },                          /* usleep(500) */
        /* func_00338AC4: globals = *(r2-0x7AD0; 1.04: -0x7AC8), queues at *(globals-0x7FE8), slot 0x6A4 */
        { IF2_EA(0x00338AECu, 0x00352310u), IF2_V(0x83C28530u, 0x83C28538u) },
        { IF2_EA(0x00338B5Cu, 0x00352380u), 0x3BBC06A4u }, { IF2_EA(0x00338B78u, 0x0035239Cu), 0x813E8018u },
        /* func_003707BC / func_003710F8: the row ids at q+0xB8 / q+0xD4 */
        { IF2_EA(0x003707D4u, 0x0038AC10u), 0x880B00B8u }, { IF2_EA(0x00371138u, 0x0038B574u), 0x881F00D4u } } },
};

} /* namespace */

/* Called from infamous2_install_overrides() (the EBOOT is mapped by then).
 * Installs no HLE override, so it returns 0 for the override count. */
extern "C" int if2_waitev_install(void)
{
    const char* e = getenv("WAIT_EVENTS");
    if (e && *e == '0') return 0;
    int on = 0;
    for (const site_def& d : kSites) {
        bool ok = true;
        for (const code_word& c : d.code)
            if (c.ea && wev_rd32(c.ea) != c.word) {
                fprintf(stderr, "[wev] %s: EBOOT word at 0x%08X is 0x%08X, expected 0x%08X -- not installed\n",
                        d.site.name, c.ea, wev_rd32(c.ea), c.word);
                ok = false;
                break;
            }
        if (ok) on += wev_register(&d.site);
    }
    if (getenv("IF2_VERBOSE")) fprintf(stderr, "[infamous2] WAIT_EVENTS: %d site(s) active\n", on);
    return 0;
}

/* Save states (a load's teardown, engine design 6.2 "wev_reset"; called by
 * the TITL section in infamous2_hooks.cpp with every guest thread gone):
 * no watch may outlive its waiter. A waiter publishes its watch only inside
 * the usleep, which has no park point, so a stop never finds one published
 * and this is normally a no-op; a bit still set would make every guest
 * write take wev_poke's slow path (and poke a slot nobody waits on) until
 * that thread's slot blocked again. The resumed usleeps arm afresh. The
 * slots themselves -- one per guest tid, each with its event and timer --
 * are host objects a load keeps (a tid is the same thread again after it). */
extern "C" void if2_waitev_ss_reset(void)
{
    const uint32_t any = __atomic_exchange_n(&g_wev_any, 0u, __ATOMIC_SEQ_CST);
    for (int k = 0; k < 3; k++) __atomic_store_n(&g_wev_mask[k], 0u, __ATOMIC_SEQ_CST);
    if (any) fprintf(stderr, "[wev] save state: watch(es) 0x%X still published at the load -- cleared\n", any);
}
