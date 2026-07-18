# Recompiling the main EXE (A1 step 6)

Exact command (run from the repo root):

```sh
../psxrecomp/recompiler/build/psxrecomp-game --config game.toml
```

Uses the recompiler built in SETUP (same commit as the pinned submodule,
678c71f). Inputs: `game.toml` (+ `seeds/slus_00664_seeds.txt`,
`game_options.toml`, and the exe it points at).

Result (2026-07-18, first run, ~0.25 s):
`generated/slus_006.64_full_00..37.c` (38 shards, 1,339,898 lines),
`slus_006.64_decls.h`, `slus_006.64_dispatch.c` (2212 entries),
`slus_006.64_full.ranges` (code-range manifest).

## Naming note (deviation from plan text)

Files are named `slus_006.64_*`, NOT `SLUS_00664_*` as the A1 plan guessed.
`[recompiler] out_stem` is parsed (config_loader.cpp) but never forwarded by
psxrecomp-game (main_psx.cpp uses the exe filename verbatim: "Use filename()
not stem() because .36 in SCUS_942.36 is part of the serial"). This is the
TombaRecomp ecosystem convention (`SCUS_942.36_full_*.c`), so we keep it and
the plan file was corrected accordingly.

`generated/` is gitignored — rebuild with the command above.
