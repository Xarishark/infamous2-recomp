# Save states

Save the whole game at any moment and go back to it later -- mid-fight, in a cutscene, in a menu --
like an emulator's save states. A save freezes the game for under a tenth of a second and is on disk
about half a second after the key; a load puts the game back about a third of a second after the key,
in the same window, with no restart.

Save states are **not** the game's own saves. The game keeps saving its progress as it always does
(the autosave icon, the pause menu's Save Game); a save state is a snapshot of the running game
beside them.

## Saving and loading

The game window has a menu bar: **File**, **Save**, **Load**, **View**.

| | Menu | Keys |
|---|---|---|
| Quick save / quick load | Save > Quick Save, Load > Quick Load | **F5** / **F9** |
| Slot 1-10 | Save > Slot 1 ... Slot 10, Load > Slot 1 ... Slot 10 | **Ctrl+F1..F10** / **Ctrl+Shift+F1..F10** |
| Back to the moment before the last load | Load > Undo Last Load | **Ctrl+Shift+Z** |
| The folder with the slot files | File > Open Save State Folder | |

- **Saving** happens at the next frame where the whole game can stop -- normally the next one. The
  game freezes for 60-120 ms, keeps running while the file is written (~0.4 s), and a note in the
  corner of the picture says `Saving to slot 3...`, then `Saved slot 3 (14:32)`.
- **Loading** reads the file while the game keeps running (~0.15 s, `Loading slot 3...`), then swaps
  the whole game over in 0.1-0.2 s (`Loaded slot 3`). The window, fullscreen, the mouse and the
  controllers stay as they are; the picture is right from the first frame.
- **The menus label each slot** with when it was saved, the mission (or `Prologue - <step>`) and the
  play time: `Slot 3  -  today 14:32  -  3. Forward Momentum - 1:02:13`.
- **Greyed** (and a key press says why): before the game has shown its first picture, while another
  save or load is still working, and -- for Load -- an empty slot, or one this build cannot load
  (`(another build: layout changed)`, `(damaged)`; see below).
- If the game is in the middle of something it cannot be stopped in (it is writing its own save, a
  thread is busy), a save or load waits up to 3 seconds for a moment that works and then says why:
  `Cannot save to slot 3: the game is saving its progress, try again in a moment`.
- **Overwriting** a slot does not ask: the previous file is kept as `slot03.p3ss.bak`.
- **Quitting** right after a save (Quit game, the close button, Alt+F4) waits for the file to be on
  disk first -- under a second, longer on a slow disk.
- **Fullscreen** (F11 or Alt+Enter) hides the menu bar; the keys keep working. Alt, Alt+letter and
  F10 never open the menu (they are game keys or release the mouse); click the bar, or bind the
  `menu` key in `config\input.ini` [hotkeys].
- The keys are in `config\input.ini` [hotkeys] (`quick_save`, `quick_load`, `save_slots`,
  `load_slots`, `undo_load`); the menu shows whatever is bound.

## Undo last load

Just before every load from the menu or a key, the game as it is right then is saved to `undo.p3ss`
in the slot folder. **Load > Undo Last Load** (**Ctrl+Shift+Z**) loads it: you are back at the moment you
pressed the key, as if the load never happened (`Undid the last load (back to 14:32)`).

- **Undo again** to go forward to the state you had loaded: an undo makes its own undo point first,
  so two undos swap the two moments.
- The menu item shows when the undo point was made; `(nothing to undo)` before the first load.
- **What it costs:** a pause like a quick save's (about 65 ms; about 110 ms for the first save or
  load of a session), and the load lands about 0.3 s later than without it (0.55 s from the key
  instead of 0.28 s), while the undo point is written.
- If the game cannot be saved at that moment (it is saving its own progress, say), the load still
  happens and its notice says `(no undo point: ...)`; Undo Last Load then still goes back to the
  undo point it had.
- A load that is refused (see below) leaves the undo point as it was.
- `backup_before_load = 0` in `config\savestates.ini` turns it off. A launch with `-LoadState` makes no
  undo point.

## States made by another build of the port

Every rebuild of the port is a new build. A save state made by another build of the same game
version **loads** when this build reads its data the same way -- every part of the saved game is laid
out as this build expects and the SPU programs are the same -- with a warning:

- the menu tags the slot: `Slot 3  -  today 14:32  -  3. Forward Momentum - 1:02:13  (older build)`
  (or `(newer build)`);
- after the load: `Loaded slot 3 -- a state made by another build of the port: something fixed since
  then may behave differently`. A fix made since the state was saved may not have reached what was
  already running in it; if something looks wrong, a fresh save state (or the game's own save) made
  with this build will not have that.

It is **refused** -- greyed in the menu with the reason, and the game keeps running as it was:

| Reason in the menu | What differs |
|---|---|
| `(another game)`, `(another game version)` | another game, disc version or EBOOT |
| `(another build: layout changed)` | a part of the saved game is laid out differently in this build |
| `(another build: data missing)`, `(another build: unknown data)` | a part this build needs is not in the file, or the file has one this build does not know |
| `(another build: SPU code changed)` | the SPU programs differ |
| `(another build: cannot resume it)` | found at the load: a thread in the state stopped somewhere this build has no code to continue from, or in a wait it cannot resume. The load says so (`it was made by another build of the port, which this build cannot resume (...)`) and the menu greys the slot from then on |

None of these touch the running game: the file is checked before anything stops, and the last check
runs before the game is stopped for the load. `build_check = 1` in `config\savestates.ini` loads only
this build's states, as before.

## Files

`savestates\BCUS98125\` in the game's folder (next to `gamedata\`):

| File | What |
|---|---|
| `slot01.p3ss` .. `slot10.p3ss`, `quick.p3ss` | the save states, about 200 MB each (230 MB at RES_SCALE 2) |
| `slot01.png` ... | a 320-wide picture of the moment |
| `slot01.p3ss.bak` ... | the slot's previous save state |
| `undo.p3ss` | the undo point: the game just before the last load (about 240 MB) |
| `undo.p3ss.bak` | the undo point before that, only while a load is running |
| `slot01.p3ss.tmp` ... | a save being written; one a crash left behind becomes the slot's file at the next start if it is whole and the slot has none, else it is removed |

A `.p3ss` file is the game's whole memory, compressed (Windows' own XPRESS Huffman codec), with a 4 KB
header the menus read: the game and its version, the build that made it, when, the mission, the play
time, and a thumbnail. `config\savestates.ini` changes the folder, the number of slots, the
compression, the thumbnails, how long a save or load waits, the undo point and whether other builds'
states load; every line there shows its default.
`SAVESTATES=0` (or `enabled = 0`) turns save states off completely: the menus are greyed and the
game runs exactly the old way.

**Save states are game data.** They hold the game's memory -- its code, its data, your progress --
so they are as personal and as copyrighted as the game itself: never share them, never commit them
(`savestates/` is in `.gitignore`).

## Starting a test in a saved moment

```
tools\run.ps1 -LoadState 3 -ExitAfter 600              # slot 3, or -LoadState q, or a .p3ss file
tools\run.ps1 -Checkpoint 03_forward_momentum_rpcs3 -SaveAt 45 -SaveSlot 2 -ExitAfter 900
```

- `-LoadState` (`SS_LOAD`): the file is read while the game boots and loaded as soon as the game
  shows its first picture -- in gameplay about 0.7 s after the start, against ~27 s for a checkpoint
  boot to the same place. The run's READY (`logs\run-<time>.ready`, `-Script`, `-ExitAfter`) is that
  load; the log says `[ss] loaded <file> at t=... (boot to gameplay N s)`. No title flow is driven; the
  run's save data is an empty sandbox unless `-Checkpoint` names one.
- `-SaveAt <s>[,<s>...]` (`SS_SAVE_AT` + `SS_SAVE_SLOT`): saves at those seconds after the start into
  slot `-SaveSlot` (1) -- how a test makes the state it later starts from.
- `SS_DIR=<folder>` uses another folder for the slot files.

## Not supported yet

- No `Delete slot`, no labels you choose; thumbnails are beside the slot files, not in the menu.
- If a load fails after the game was already stopped (a bug -- it has not happened in testing), the
  game ends with a message instead of restarting itself into the slot. The undo point still holds the
  moment before that load: start the game again and use Load > Undo Last Load.
- Whether another build can resume a state is known for sure only at the load (the menu cannot tell
  in advance that a thread stopped somewhere this build has no code for).
- The game's own saves on disk are not part of a save state: loading an older state leaves newer
  game saves as they are (as RPCS3 does).
- Opening the Save / Load menu does not pause the game.
- A state saved at another picture shape (aspect) keeps its shape until you restart the game.
- Timed waits inside the game restart their full time after a load (the game does not notice).

## How it works

`runtime/savestate/` in the toolkit: `ss_core.c` stops every game thread at a safe point,
`ss_state.c` captures and restores the whole game section by section (memory, kernel objects,
threads, SPUs, files, graphics, audio, input), `ss_file.c` is the `.p3ss` file and its compatibility
checks, `ss_slots.c` the slots behind the menu and the undo point, `ss_ui.c` the menu's side; the
design is the save-state engine design (M4-M7) and `docs/NOTES.md` has the measurements.
