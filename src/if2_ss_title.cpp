/*
 * inFamous 2 -- what a save-state slot says about its moment (engine design
 * 5.5): the mission text and game mode the window menu's Save / Load items
 * show ("Slot 3  -  today 14:32  -  3. Forward Momentum - 1:02:13"), handed to
 * the slots through ss_slots_host.describe (src/main.cpp). Kept simple:
 *   1. the game's own save text: the SUB_TITLE of the save it last wrote or
 *      loaded ("3. Forward Momentum"; libs/system/cellSaveData.c keeps it, and
 *      a save state carries it, so a loaded state keeps its own text until the
 *      game saves again);
 *   2. in the prologue, which never saves: "Prologue - <step>" from the task
 *      graph (src/if2_taskgraph.cpp);
 *   3. else the game mode from the game's signals (src/if2_checkpoint.cpp:
 *      gameplay, pause menu, title menu, movie, cutscene, loading -- known only
 *      when the signal hook runs: play mode, checkpoint runs, IF2_SIGLOG).
 * Called on the frame-clock thread right after a save's capture, the game
 * running again; everything here only reads.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" uint32_t ps3_savedata_last_text(char* title, size_t title_n, char* subTitle, size_t subTitle_n,
                                           char* detail, size_t detail_n);    /* libs/system/cellSaveData.c */
extern "C" int      if2_taskgraph_prologue_step(char* name, size_t cap);      /* src/if2_taskgraph.cpp */
extern "C" uint32_t if2_checkpoint_game_mode(void);                           /* src/if2_checkpoint.cpp */

namespace {
const char* const kMode[] = { "", "gameplay", "pause menu", "title menu", "movie", "cutscene", "loading" };
}

extern "C" void if2_ss_describe(char* mission, size_t n, uint32_t* game_mode)
{
    const uint32_t mode = if2_checkpoint_game_mode();
    if (game_mode) *game_mode = mode;
    if (!mission || !n) return;
    mission[0] = 0;
    char sub[128] = "";
    if (ps3_savedata_last_text(nullptr, 0, sub, sizeof sub, nullptr, 0) && sub[0]) {
        /* "(Autosave) 3. Forward Momentum": the mission without the save's kind */
        const char* s = sub;
        const char* close = sub[0] == '(' ? strstr(sub, ") ") : nullptr;
        if (close && close[2]) s = close + 2;
        snprintf(mission, n, "%s", s);
        return;
    }
    char step[48] = "";
    const int k = if2_taskgraph_prologue_step(step, sizeof step);
    if (k > 0) {
        if (step[0]) snprintf(mission, n, "Prologue - %s", step);
        else snprintf(mission, n, "Prologue - step %d", k);
        return;
    }
    if (mode < sizeof kMode / sizeof kMode[0]) snprintf(mission, n, "%s", kMode[mode]);
}
