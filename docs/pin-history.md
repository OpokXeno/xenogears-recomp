# Framework pin history — XenogearsRecomp

The `psxrecomp` framework is a **git submodule** pinned to a known-good
commit. Bump it the normal way:

```sh
git -C psxrecomp fetch && git -C psxrecomp checkout <new-sha>
git add psxrecomp && git commit -m "bump psxrecomp to <new-sha>"
```

**Rule (binding):** every pin bump records here, BEFORE committing:
1. validation result (boot_smoke PASS/FAIL + human boot check),
2. whether the **codegen hash changed** — it is embedded in the overlay cache
   path (`build-dbg/cache/SLUS-00664/gcc/linux-x64/cg5_<hash>/`). A hash change
   invalidates ALL cached overlay shards and requires regen + recapture.
3. recompiler/runtime behavior changes relevant to our config knobs.

---

## Pinned 2026-07-17: `678c71f` (initial pin, A1 Step 2)

- Codegen hash: **`0f63e53f`** (cache dir `cg5_0f63e53f/`). 81 native overlay
  shards cached as of 2026-07-18 (regions 0x80000000, 0x8006F000 whole-module;
  0x80199000 interior fragments).
- Split-gen: generated output is `slus_006.64_full_NN.c` shards + `_decls.h`
  (commit `41370a6`). **Broke upstream `tools/compile_overlays.py`** (read only
  the monolithic `_full.c`); no upstream fix on any branch. LOCAL FIX in the
  submodule working tree: `read_generated_c()` shard-aware reconstruction,
  3 read sites patched — game-agnostic, **upstream PR candidate**, uncommitted.
- Known dead config field at this pin: `[recompiler] bios_thunks` is parsed
  (config_loader.cpp:771) and forwarded (config_loader.h:426) but has **no
  consumer** on any branch. Thunk detection is unconditional
  (`function_analysis.cpp` Pass 2.56, `is_bios_dispatch_thunk`). Do NOT add
  `bios_thunks = ...` to game.toml — it would be a no-op. Upstream note
  candidate.
- Runtime knobs available at this pin (all verified in config_schema.md /
  main.cpp / load_accel.c): `turbo_loads`, `idle_skip`, `turbo_audio_sink`,
  `[[runtime.warm_cd_routes]]`, `[video] auto_skip_fmv` + `fmv_skip_*`,
  `[widescreen]` + `[widescreen.cull]`, `[load_accel.vsync_query]`,
  `game_options.toml [[option]]`, annotations CSV auto-load
  (`annotations/<exe_stem>_annotations.csv` — ours: `slus_006.64`).
- Accelerator decision (2026-07-18): `turbo_loads` / `idle_skip` /
  `turbo_audio_sink` deliberately **OFF** — the shelved black-poly bug
  (burndown 006) is a suspected timing-layer desync; timing accelerators
  would contaminate any future repro. Revisit after A4 milestones stabilize.
  `warm_cd_routes` deferred: needs Xenogears LBA evidence (PCSX-Redux trace;
  SETUP Step 8 emulator still open).
- Validation at pin: boot_smoke PASS (6713 frames/25 s headless, 2026-07-18);
  human windowed play through title → intro → Lahan field.
