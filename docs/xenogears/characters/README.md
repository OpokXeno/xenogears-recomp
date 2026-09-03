# Xenogears Characters

This folder is a cross-cutting reference: it pulls together everything already
documented about the eleven playable characters — their stat records,
experience and growth, deathblow and ability unlocks, party formation, and
persistence — from wherever it actually lives (mostly `battle/03`, `battle/05`,
`battle/08`, and `menu/06`/`menu/08`). It does not restate those chapters'
full derivations; it indexes and connects them the way
[`docs/xenogears/rng/`](../rng/README.md) indexes RNG rolls without
re-deriving battle formulas.

## Reading Order

1. [`01-characters.md`](01-characters.md) is the only chapter so far: the
   eleven-character roster, the shared stat record, equipment and displayed
   stats, experience and leveling, stat growth, deathblows and ability
   unlocks, the three character-specific exceptions (Billy, Chu-Chu, Maria),
   party formation and availability, and persistence.

## Scope

This folder covers characters specifically — not Gears (see `battle/05` and
`menu/06`/`menu/11` for Gear stats and tuning), not enemies (see `battle/06`
and `rng/04`), and not the Chi/Ether ability *menu* itself (that's UI
plumbing over the same ability system described here; see `battle/05` §9 and
`battle/08` §9 for what exists already — the menu's own drawing code was
deliberately left undocumented as low mod-relevance).
