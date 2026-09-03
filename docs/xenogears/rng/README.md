# Xenogears Random Number Generation

Xenogears drives every random outcome — from Battle damage variance to World
Map encounters — through three independent, deterministic generators. This
folder documents those generators and the data they feed.

## Reading Order

1. [`01-generators-and-determinism.md`](01-generators-and-determinism.md)
   identifies the three generators, their exact algorithms, seed addresses,
   and consumers, and states what determinism means for each.
2. [`02-battle-rng.md`](02-battle-rng.md) indexes every explicit random roll
   in Central Battle, cross-referencing the formula already documented in
   each `battle/` chapter rather than restating it.
3. [`03-worldmap-encounter-tables.md`](03-worldmap-encounter-tables.md) is the
   complete extracted data table of World Map encounter-region formations and
   weight rows that the selection formula in
   [`worldmap/07`](../worldmap/07-encounters-exits-transitions-and-persistence.md)
   consumes, with each formation slot resolved to its real monster name and
   each weight given as a percentage of its row.
4. [`04-enemy-drop-tables.md`](04-enemy-drop-tables.md) is the complete
   extracted drop-chance and item table for all 76 enemy sets, for the roll
   formula indexed in [`02-battle-rng.md` §8](02-battle-rng.md#8-enemy-drop-rolls-in-detail).
5. [`05-field-encounter-tables.md`](05-field-encounter-tables.md) is the
   complete extracted data table of Field random-encounter formations and
   weights for the selection formula in
   [`field/11`](../field/11-encounters-transitions-loading-and-persistence.md),
   covering all 635 fields (of 730) that carry an encounter table, deduplicated
   down to the 55 tables actually authored.
6. [`06-battling-rng.md`](06-battling-rng.md) indexes every explicit random
   roll in Battling, the colosseum minigame, cross-referencing the formula
   already documented in each `battling/` chapter rather than restating it.

## Conventions

`random_mod_N` means "a value in `0..N-1`, uniformly distributed," matching
the notation used throughout [`docs/xenogears/battle/`](../battle/README.md).
Formulas quoted from another chapter are marked as such and are not
re-derived here.

## Scope

This folder covers the mechanism (which generator, seeded how, consumed
where) and the game's own random-weighted data tables (encounter and drop
weights). It does not restate combat formulas already covered in full by
`battle/`, `worldmap/`, or other aspect folders — those chapters remain the
source of truth for the surrounding mechanic; this folder is where the
random-number-specific detail and the exhaustive data tables live.
