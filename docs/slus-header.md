# SLUS header facts (A1 step 4)

Parsed with `tools/psx_exe_info.py` from the human-dumped files in
`~/xenogears-port/XenogearsRecomp/game/` (hashes in `~/xenogears-port/docs/hashes.md`).

| file | magic | initial PC | initial GP | load addr | text size | initial SP |
|---|---|---|---|---|---|---|
| ./game/slus_006.64 | OK | 0x80019524 | 0x00000000 | 0x80010000 | 301056 (0x49800) | 0x801FFFF0 |
| ./game/slus_006.69 | OK | 0x80019524 | 0x00000000 | 0x80010000 | 301056 (0x49800) | 0x801FFFF0 |

- file size: 303104 bytes each; text size 301056 = 303104 - 2048 (header) — matches.
- load address 0x80010000 (standard); entry 0x80019524; GP=0 in header (set by startup code).
- disc 1 (SLUS-006.64) and disc 2 (SLUS-006.69) have identical EXE layout — same engine, per master plan 4.A2 note.
