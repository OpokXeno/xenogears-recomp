# Seed promotion policy — XenogearsRecomp

Adapted from TombaRecomp's `audit_notes/v2/discovered_functions_logs.md`
(use policy section). Governs `seeds/slus_00664_seeds.txt`.

## Rules

1. A seed is promoted only with **evidence**: a runtime FAIL-FAST unknown-
   dispatch record, a crash/freeze dump, a dispatch-miss during play, or a
   focused Ghidra audit (project `XenogearsDisc1` via MCP bridge).
2. Before promoting, establish that the address is a **real required entry
   point NOT already covered**: check `generated/slus_00664_dispatch.c` for an
   existing case and the recompiler discovery log. If already discovered, the
   problem is elsewhere (runtime dispatch state, overlay variant, memory
   corruption) — a duplicate seed changes nothing.
3. Every promotion carries an inline comment: `# verified: <evidence>` with
   source (crash file, journal, Ghidra note).
4. Promotions go under the `# --- verified promotions ---` section of
   `seeds/slus_00664_seeds.txt` (single seeds file — schema at pin 678c71f
   accepts only one path).
5. **Batch promotions are bisected** if they affect boot behavior (boot_smoke
   PASS + human windowed check after each batch).
6. Bulk candidate lists (discovery logs, emulator traces) are data-only
   salvage: park them in `analysis/`, never wire them wholesale.

## Evaluation log

### 2026-07-18 — `0x80019524` (A1 Step 8 crash) → REJECTED (already covered)

- Evidence: `psx_crash.txt` FAIL-FAST unknown dispatch `addr=0x80019524`,
  journal `docs/burndown/001.md`.
- Finding: `func_80019524` IS compiled (`full_19.c`), IS in the dispatch table
  (`slus_006.64_dispatch.c`, case `{0x80019524u, ..., func_80019524}`), and IS
  the seeded entry PC. The fail-fast is a **runtime dispatch-state rejection**
  (leading hypothesis 001-H1: BIOS boot-vector re-entry rejected post-boot;
  investigate `psx_game_text_native_ok` re-entry semantics — A2 follow-up,
  NOT a seed gap).
- Freeze dumps `psx_freeze_dump_psx-runtime_*.json` (3×): all point at the BIOS
  boot-caller region (`0x1FC03CF0` / `0xBFC03D04`), no new game-side addresses.
- **Result: zero promotions.** No other unknown-dispatch addresses exist in
  current evidence (burndown 001–006 journals scanned 2026-07-18).
