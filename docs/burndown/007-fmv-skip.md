# 007 — FMV auto-skip address hunt (TOMBA-ALIGNMENT Step 8, 2026-07-18)

## Goal
Find Xenogears equivalents of Tomba's `fmv_skip_total_table` (per-movie u16
frame-total array) + `fmv_skip_movie_id` (current-movie-id byte) so
`[video] auto_skip_fmv` can end movies via the game's own teardown
(runtime/src/main.cpp:2410).

## Method (all via Ghidra MCP bridge, program slus_006.64)

1. Name search: no `mdec|movie|fmv|str|play|video` function names in the DB.
2. Strings: no `.STR` / XA / MDEC path strings in the main EXE. Only
   `CdlPlay` (CD debug string @ 0x80018DC4) and a module-name table @
   0x800182F8 (`Field/Battle/Worldmap/Battling/Menu/Movie`).
3. Byte-pattern sweep: **zero** `lui reg,0x1F80` in the entire main EXE
   (sanity: `li t2,0xA0` pattern finds exactly the 5 known A0 thunks, so the
   search is sound). → The main EXE does **no direct MDEC/MMIO access**;
   all hardware goes through BIOS calls (PsyQ discipline). MDEC work is
   BIOS `DecDCT*` + the movie logic lives elsewhere.
4. Module-name table xref → `FUN_8001A344` = **"XENOGEARS Kernel MENU"** — a
   developer debug menu (Field=1, Battle=2, Worldmap=3, Battling=4, Menu=5,
   **Movie=6**) that calls `FUN_8001996c(module_id)` to switch modules.
   Globals: cursor `DAT_8004f2d8`, pad bits `DAT_800594a4`/`DAT_8005948c`,
   display struct `DAT_800592cc`.

## Conclusion (evidence-based, no guesses shipped)

- The FMV player is NOT in the main EXE — it is the **Movie module (id 6)**,
  almost certainly disc-loaded (overlay domain, not yet imported into Ghidra).
- No static per-movie u16 frame-total table is identifiable in the main EXE.
  STR movies typically derive totals dynamically from stream length, so the
  Tomba-style hook may not exist for Xenogears at all.
- **Shipped config:** `[video] auto_skip_fmv = false` with NO `fmv_skip_*`
  addresses → runtime uses the **generic START-hold fallback** when enabled
  (main.cpp:2456). Requires human verification that Xenogears' movie player
  polls the pad (most Square PSX movies are START-skippable).

## Next leads (Track B)
- Import the Movie overlay module into Ghidra once B2's overlay inventory
  identifies it; look for its MDEC frame counter vs stream-total comparison
  and the movie-id global.
- `FUN_8001996c` (module switch) + the kernel debug menu may be useful for
  testing (boot directly into Field/Battle states?).
