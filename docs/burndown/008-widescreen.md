# 008 — Widescreen hooks (TOMBA-ALIGNMENT Step 9, 2026-07-18)

## ROOT CAUSE of "16:9 never engages" (found 2026-07-18, user report)

**Symptom:** game stays 4:3 with black bars in both builds; predates today's
changes ("ya venía fallando antes").

`ws_game_mode()` (gpu.c:211) decides gameplay-vs-2D per frame:

```c
if (ws_full_2d_mode()) return 1;
if (ws_gte_game_mode_cfg && frame - ws_last_gte_stamp <= 45) return 1;
return frame - ws_last_tag_stamp <= 2;   // sprite-TAG path only
```

With no `sprite_tag_funcs` configured (Xenogears has no billboards — it's
fully 3D) and no `gte_game_mode`, classification falls to the TAG path, whose
stamp NEVER updates without tags → `game_mode=0` forever →
`gpu_ws_present_native_43()=1` → permanent 4:3 pillarbox. Tomba works because
it's a sprite-tag (2.5D) title; Xenogears needs the GTE-activity detector
(the "Ape" class: ≥3 GTE verts/frame, 45-frame hysteresis).

**Fix (one line):** `[widescreen] gte_game_mode = true`. game.toml is read at
RUNTIME (no rebuild needed; runtime.cmake:283). Verified mechanically:
aspect 16:9 + "HUD squash" + game_started latch all confirmed via startup log
and debug server (`turbo_loads` → game_started:1 with `--disc`); engage runs
in the windowed present path only (headless skips it by design).

Side finding: headless boot needs explicit `--disc` to start the game, and
the CD-boot path then stalls (~0 fps after frame ~2650) — same open question
as A1 journal next-step 4 (headless boot path), unaffected by this fix.

## ROOT CAUSE 3 of "map chunks don't load in the 16:9 reveal" (found 2026-07-19, user report)

**Symptom:** the field map streams in chunks; at 16:9 chunks outside the 4:3
area never load (visible pop-in/out when walking or rotating the camera).
NOT a per-poly cull: it's the chunk-streaming classifier.

**Classifier (Ghidra decompile of the field overlay @0x8006F000, program
field_module2 in XenogearsDisc1.gpr):** `FUN_0001519c` assigns chunk load
priorities with ANGULAR mod-4096 windows around the camera view angle:

| bias site (addiu) | range site (sltiu) | window | executed |
|---|---|---|---|
| `0x80083FB0` (-700) | `0x80083FB4` (2697) | `(angDiff-700)<u 2697` | ✓ gameplay capture |
| `0x80083D1C` (-700) | `0x80083D20` (2697) | 2nd instance | — |
| `0x8007B728` (-128) | `0x8007B72C` (3841) | boundary helper | ✓ |

**Framework gap fixed (2026-07-19):** `[widescreen.cull] range_sites` was
parsed and consumed by the RECOMPILER (native emit) but never reached the
RUNTIME — the interp's SLTIU case had no explicit-site branch, so overlay
titles (all Xenogears cull code is overlay-resident) could not use the Tomba
bias/range mechanism. Fixed: `gpu_ws_set_explicit_cull_sites` now takes the
range list (gpu.h/gpu.c), main.cpp passes it, and dirty_ram_interp's SLTIU
case applies `rs <u (imm + 2*margin)` at explicit range sites (identity at
4:3). ALSO NOTE: the project builds the NESTED psxrecomp copy
(`XenogearsRecomp/psxrecomp`), not the standalone `/home/pc/xenogears-port/
psxrecomp` — framework edits must land in the nested copy (the two were
identical at the time of this fix; both now carry it).

**Fix (config):** bias_sites/range_sites above. Margin units are screen-px
(53 @16:9) applied to angular immediates; if the widen proves insufficient,
`guard_pixels` (margin = 53+guard, ≤256+53) is the tuning knob — sweep live
via the `ws_margin` debug command, then bake.

## ROOT CAUSE 2 of "3D polygons pop in/out at screen edges" (found 2026-07-18, user report)

**Symptom:** with 16:9 engaged, polygons visibly appear/disappear when
walking or moving the camera.

Xenogears' screen-extent trivial-reject funnels live in the **field overlay
module** (0x8006F000, interpreted; cache empty) and use immediates NOT in the
configured `[widescreen.cull]` sets, so `ws_cull_site()` never qualified them
and the original 4:3 windows rejected geometry inside the revealed 16:9 area:

| site | idiom | vanilla window | imms |
|---|---|---|---|
| `0x80075EA8` (post-RTPS point test, sets struct flag 0x200) | `(X+39)<u 0x18F` / `(Y+9)<u 0x143` | X∈[-39,360) | W=0x18F, H=0x143 |
| `0x80095C6C` (point visibility) | `(X-1)<u 0x13F` / `(Y-1)<u 0xDF` | X∈[1,320) | W=0x13F, H=0xDF |

False-positive sweep (main exe + both captured overlays, ±512B interp-window
semantics): only these 2 sites widen; the `slti 0x140/0x141` clamps
(0x80027B74/0x80027CE0/0x80086168/0x80086344) stay vanilla (no H-imm in
window). Not covered: signed `slti 0x135/0xD5` pair @0x8009CA90 (likely UI —
revisit if popping persists in specific scenes).

**Fix (config-only, runtime-read):**
```toml
screen_w_imms = ["0x140", "0x141", "0x13F", "0x18F"]
screen_h_imms = ["0xE0", "0xF0", "0xDF", "0x143"]
```
`psx_ws_cull_sltiu` widens each W compare by ±margin both sides
(e.g. X∈[-39,360) → [-39-m, 360+m)), covering the 16:9 reveal.

```toml
[widescreen]
gte_game_mode = true      # THE fix — GTE-activity gameplay detector
hud_sprt_squash = true

[widescreen.cull]
auto_screen_x = true
screen_w_imms = ["0x140", "0x141", "0x13F", "0x18F"]   # +Xenogears funnels
screen_h_imms = ["0xE0", "0xF0", "0xDF", "0x143"]      # (see ROOT CAUSE 2)
```

## Evidence behind each decision

- **No screen-extent cull signature in the main EXE**: instruction sweep
  (Ghidra MCP, 34528 insns) found ZERO `sltiu/slti` with 0x140/0x141 width or
  0xE0/0xF0 height immediates. Xenogears' main EXE is a thin kernel; ALL
  render funnels live in disc-loaded modules (field 0x80199000, battle…).
  → `auto_screen_x` is inert for the main EXE; it fires on OVERLAY compiles
  (compile_overlays.py always forwards `--ws-config game.toml`).
- **No billboard/sprite-tag evidence**: field and battle actors are 3D
  polygonal models; 2D content is HUD/menus/dialogue text+portraits = SPRT
  prims → covered by `hud_sprt_squash` (runtime-side at GP0 submission,
  works for main-EXE + overlay code). No `sprite_tag_funcs`/anchor shipped.
- **Display height**: debug-server `gpu_state` during BIOS shell showed
  draw_area [0,0,319,239] (240 lines) → `screen_h_imms` covers 0xF0; 0xE0
  kept as the 224-line fallback (detector requires the full paired signature,
  so an unused imm is inert).
- **Per-site bias/range/a1 lists**: deferred — the world-space draw
  classifier lives in the field module (not yet in Ghidra; Track B2).
- **Cache compatibility**: flavor stays 0 (no `--flavor` passed); the 81 A3
  cache shards remain valid — verified post-change: `dispatch_stats`
  static_hits=19,011,093, miss=0 at BIOS boot.

## Human A/B verification (16:9, build-dbg)

1. Play Lahan field as usual (settings.toml aspect 16:9): HUD, dialogue
   text and portraits should keep native proportions (no stretch).
2. Toggle: set `hud_sprt_squash = false` in game.toml, rebuild build-dbg
   (`tools/build-linux.sh build-dbg RelWithDebInfo`), compare.
3. Edge pop-in (`auto_screen_x`) only changes once overlay modules are
   recompiled from captures with the new config — A/B after the next A3
   capture session.
