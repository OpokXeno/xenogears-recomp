# World Map Encounter Tables

## 1. Scope

This chapter is a data table, not a mechanism description: it presents the
complete formation and weight content of every World Map encounter region,
extracted directly from Disc 1. The selection formula that consumes this data
is already documented in
[`worldmap/07` §3-4](../worldmap/07-encounters-exits-transitions-and-persistence.md#3-region-selection)
and is only summarized here. The record layout is documented in
[`worldmap/02` §7](../worldmap/02-resources-formats-and-configuration.md#7-encounter-region-records)
and is not restated in full.

## 2. Selection Formula

Quoted from `worldmap/07` §§3-4 (`WorldMapSelectRandomEncounter`, `0x80075E7C`):

```text
1. Copy the selected 16-byte weight row to a local working array.
2. Sum all weights; a zero total yields no encounter.
3. Draw a value in 1..sum.
4. Walk nonzero weights, subtracting one unit at a time until the draw expires.
```

Unlike Field's formula
([`rng/05-field-encounter-tables.md` §2](05-field-encounter-tables.md#2-selection-formula)),
the no-encounter case here is not part of this weighted draw at all — it is
decided earlier, by a separate movement-countdown check
([`worldmap/07` §1](../worldmap/07-encounters-exits-transitions-and-persistence.md#1-encounter-schedule)).
Once that check has already decided "attempt an encounter," this draw always
produces a formation. Practically: every weight percentage in this chapter is
`weight / row_sum` — there is no reserved "no encounter" share to account for,
and every percentage in a row's table sums to `100%` (rounded to one decimal
place).

## 3. Extraction Method

Sections `10..25` of each configuration's compressed 26-section World archive
(file `+1`) hold sixteen `0x260`-byte encounter region records, one per
terrain encounter-region value `0..15`. Each record is 16 Battle Formation
Records (`0x20` bytes each, the same layout documented for World's own
formation table in
[`worldmap/02` §7](../worldmap/02-resources-formats-and-configuration.md#7-encounter-region-records))
followed by six rows of 16 one-byte formation weights.

`tools/extract_worldmap_encounters.py` reads the disc image directly: it
resolves file `+1` for a given configuration through the same FAT/directory
indexing used by `tools/extract_disc_overlays.py`, LZSS-decompresses the
archive, locates sections 10-25 by their packet offsets, and decodes each
region's 16 formations and 6 weight rows. Formation decoding follows the same
enemy-slot byte layout used by Battle's own formation records: low 7 bits of
the selector byte are the definition slot and bit 7 is the Gear-scale flag,
bit 7 of the flags byte is the hidden flag, and low 7 bits of the position
byte are the position with bit 7 as the facing flag.

## 4. All Nine Configurations Share Identical Encounter Data

[`worldmap/02` §2](../worldmap/02-resources-formats-and-configuration.md#2-progress-selected-configurations)
lists nine progress-selected configurations, each with its own base file ID
and covering a distinct story-progress range:

| Story progress | Configuration | Base file |
|---:|---:|---:|
| `0x0000..0x0017` | 0 | `0x2B` |
| `0x0018..0x0035` | 1 | `0x36` |
| `0x0036..0x0086` | 2 | `0x41` |
| `0x0087..0x0094` | 3 | `0x4C` |
| `0x0095..0x00B9` | 4 | `0x57` |
| `0x00BA..0x00C5` | 5 | `0x62` |
| `0x00C6..0x00CB` | 6 | `0x6D` |
| `0x00CC..0x00EC` | 7 | `0x78` |
| `0x00ED+` | 8 | `0x83` |

Each configuration is a genuinely distinct file on disc: distinct FAT index,
LBA, and compressed size. Extracting and comparing sections 10-25 across all
nine, however, shows their decompressed encounter-region bytes — every
region's formations and every weight row — are byte-identical across every
configuration. Advancing story progress changes World's landmarks, exits,
decorations, and model placements, but it does not change what can be
encountered while walking the map; that is governed entirely by which of the
16 terrain regions the party is standing in and by story progress selecting
one of four weight rows within that region
([`worldmap/07` §3](../worldmap/07-encounters-exits-transitions-and-persistence.md#3-region-selection)),
never by which configuration file is currently loaded.

Because of this, the single table in Section 7 below — extracted from
configuration 0, base file `0x2B` — is the complete encounter-region data set
for ordinary World travel across the entire game.

## 5. Region Table Format

Each region lists its 16 Battle Formation Records, then its 6 stored weight
rows. Only rows `0..3` are reachable through the documented
[story-progress selection formula](../worldmap/07-encounters-exits-transitions-and-persistence.md#3-region-selection);
rows `4` and `5` are serialized as part of the fixed `0x260`-byte record but
are not selected by any known code path. Each weight cell shows the raw byte
alongside its share of that row's total (`weight / row_sum`, rounded to one
decimal place); a zero-weight cell shows only `0`.

Enemy entries show the resolved monster name (Section 6) in place of the raw
definition slot wherever a name was recovered, followed by `(Gear)` for the
Gear-scale flag, `[hidden]` for the hidden flag, `@pos<P>` for position, and
`/f` for the facing flag. A slot that could not be resolved to a name falls
back to `def<N>` — unlike
[`rng/04-enemy-drop-tables.md`](04-enemy-drop-tables.md), every `def<N>` shown
here is confirmed real by construction (it comes from an actual formation
record), so the fallback means only "the original game never authored an
on-screen name for this combatant," not "possibly unused." The one instance
in this table (`def3(Gear)` in Region 4, enemy set 9's Musha Mk100 roster)
decodes to raw non-text bytes for its name entry. An empty formation slot is
omitted; a formation with no enemy entries shows `(none)`.

There are no region/area names in the underlying data. The 16 values used
throughout this chapter are a raw 4-bit terrain classification (bits
`26..29` of the material cell, per
[`worldmap/05` §3](../worldmap/05-terrain-streaming-movement-and-collision.md#3-packed-terrain-samples))
that exists purely to select an encounter table — it is not a named place,
and nothing in the World archive's own resources (checked: its only text
resource is the exit-prompt dialogue bundle, per
[`worldmap/02` §4](../worldmap/02-resources-formats-and-configuration.md#4-primary-section-archive))
associates a label with it. Any real-world geographic name for a region
(e.g. "this is the Aveh desert") would have to be inferred from which part of
the map uses it, not decoded from game data, and is out of scope here.

## 6. Enemy Name Resolution

Every `def<N>` slot in the raw formation data is a per-formation index into
one enemy set's own 8-slot roster, not a global enemy ID — the same slot
number means a different monster in a different enemy set. Recovering the
actual name behind each slot required two independent, previously
undocumented pieces of the format:

1. **The resident bitmap font's single-byte code table.** The font resource
   itself (directory `0x01`, file `6`; SHA-256 confirmed against
   [`graphics/03` §9.1](../graphics/03-resource-formats.md#91-main-bitmap-font))
   was already documented by byte offset and glyph geometry, but nothing
   recorded which character each numeric code actually draws. Rendering every
   narrow single-byte glyph (codes `0x10..0x60`) as a 16x11 monochrome bitmap
   and reading it by eye recovers the full table: digits at `0x16..0x1F`,
   uppercase `A..Z` at `0x20..0x39`, lowercase `a..z` at `0x3D..0x56`, space,
   and basic punctuation. The complete table is now recorded in
   [`graphics/03` §9.1.1](../graphics/03-resource-formats.md#911-single-byte-code-table).
2. **The enemy-set definition file's own name table.** Each enemy-set
   definition file stores a `DialogStringBundle`
   ([`graphics/03` §9.3](../graphics/03-resource-formats.md#93-dialog-string-bundles))
   at `u16(file + 0x30)`, and its string index runs in step with the
   definition-slot index: entry `0` is slot `0`'s own display name, entry `1`
   is slot `1`'s, and so on, before continuing into that enemy set's attack
   names and battle messages. This correspondence and the string-bundle
   location are now recorded in
   [`battle/06` §2](../battle/06-enemy-ai-and-action-scripts.md#2-enemy-set-definition-file).

Decoding both together resolves every `def<N>` slot referenced anywhere in
this chapter to a real, disc-verified monster name (`Hobgob`, `Lucre Bug`,
`Gigafoot`, `Forest Elf`, `Alpha Weltall`, and so on for the full roster used
across all 16 regions). A slot whose decoded string comes back as raw,
non-text bytes (shown here as `{ff}{ff}`-style tokens in the source data) is
a genuinely unused definition slot in that enemy set and is not expected to
appear in any formation; none of the formations below reference one.

## 7. Region Data

### Region 0

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 40 | 0x00 | 44 | 0 | 0,0,0 | Hopper@pos0 |
| 1 | 40 | 0x00 | 44 | 0 | 0,0,0 | Hopper@pos0, Hopper@pos0 |
| 2 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 3 | 49 | 0x00 | 44 | 0 | 0,0,0 | Rhino@pos0 |
| 4 | 40 | 0x00 | 44 | 0 | 0,0,0 | Hopper@pos0, Hopper@pos0 |
| 5 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal Gear(Gear)@pos0 |
| 11 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal Gear(Gear)@pos0 |
| 12 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (33.3%) | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 1 (33.3%) | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 1 (33.3%) | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 1 (33.3%) | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 1 (33.3%) | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 1

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x00 | 7 | 0 | 0,0,0 | Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 1 | 5 | 0x00 | 7 | 0 | 0,0,0 | Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 2 | 12 | 0x00 | 7 | 0 | 0,0,0 | Acid Frog@pos0, Acid Frog@pos0 |
| 3 | 5 | 0x00 | 7 | 0 | 0,0,0 | Dune Man(Gear)@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 4 | 5 | 0x00 | 7 | 0 | 0,0,0 | Dune Man(Gear)@pos1, Dune Man(Gear)@pos2 |
| 5 | 2 | 0x00 | 7 | 0 | 0,0,0 | Trooper(Gear)@pos1, Trooper(Gear)@pos2 |
| 6 | 22 | 0x00 | 7 | 0 | 0,0,0 | Twin Burner(Gear)@pos0 |
| 7 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot(Gear)@pos0, Gigafoot(Gear)[hidden]@pos0 |
| 8 | 22 | 0x00 | 7 | 0 | 0,0,0 | Twin Burner(Gear)@pos0 |
| 9 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal Gear(Gear)@pos0 |
| 11 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal Gear(Gear)@pos0 |
| 12 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 7 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 7 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 15 | 0x00 | 7 | 0 | 0,0,0 | Alkanshel(Gear)@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (40.0%) | 1 (20.0%) | 2 (40.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 0 | 2 (16.7%) | 2 (16.7%) | 3 (25.0%) | 3 (25.0%) | 1 (8.3%) | 1 (8.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 0 | 2 (16.7%) | 2 (16.7%) | 3 (25.0%) | 3 (25.0%) | 1 (8.3%) | 1 (8.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 2 (12.5%) | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 1 (6.2%) | 1 (6.2%) | 0 |
| 4 | 0 | 2 (16.7%) | 2 (16.7%) | 3 (25.0%) | 3 (25.0%) | 1 (8.3%) | 1 (8.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 2

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x00 | 44 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 1 | 49 | 0x00 | 44 | 0 | 0,0,0 | Rhino@pos0 |
| 2 | 12 | 0x00 | 44 | 0 | 0,0,0 | Mullet@pos0, Mullet@pos0, Mullet@pos0, Mullet@pos0 |
| 3 | 69 | 0x00 | 44 | 0 | 0,0,0 | Pecking Duck@pos0, Pecking Duck@pos0 |
| 4 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 5 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal Gear(Gear)@pos0 |
| 11 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal Gear(Gear)@pos0 |
| 12 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 2 (12.5%) | 2 (12.5%) | 2 (12.5%) | 2 (12.5%) | 0 | 0 | 0 | 0 | 0 | 4 (25.0%) | 0 | 0 | 2 (12.5%) | 1 (6.2%) | 1 (6.2%) | 0 |
| 4 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 3

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 3 | 0x40 | 1 | 0 | 0,0,0 | Dwarf@pos0, Dwarf@pos0, Dwarf@pos0, Dwarf@pos0 |
| 1 | 3 | 0x40 | 1 | 0 | 0,0,0 | Vargas@pos0, Vargas@pos0 |
| 2 | 3 | 0x40 | 1 | 0 | 1,1,1 | Dwarf@pos1, Dwarf@pos1, Vargas@pos1, Vargas@pos1 |
| 3 | 69 | 0x40 | 1 | 0 | 0,0,0 | Pecking Duck@pos0, Pecking Duck@pos0, Pecking Duck@pos0 |
| 4 | 11 | 0x40 | 6 | 0 | 0,0,0 | Rankar R (Gear)@pos0 |
| 5 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 10 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 11 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 15 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 16 (33.3%) | 16 (33.3%) | 16 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 16 (33.3%) | 16 (33.3%) | 16 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 16 (33.3%) | 16 (33.3%) | 16 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 16 (33.3%) | 16 (33.3%) | 16 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 16 (32.7%) | 16 (32.7%) | 16 (32.7%) | 1 (2.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 16 (33.3%) | 16 (33.3%) | 16 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

### Region 4

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 43 | 0x00 | 7 | 0 | 0,0,0 | Sand Tripper(Gear)@pos1, Sand Tripper(Gear)@pos2 |
| 1 | 9 | 0x00 | 7 | 0 | 0,0,0 | def3(Gear)@pos0 |
| 2 | 1 | 0x00 | 7 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 3 | 12 | 0x00 | 7 | 0 | 0,0,0 | Mullet@pos0, Mullet@pos0 |
| 4 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 5 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 15 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 16 (72.7%) | 4 (18.2%) | 2 (9.1%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 16 (72.7%) | 4 (18.2%) | 2 (9.1%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 16 (88.9%) | 0 | 2 (11.1%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 2 (10.0%) | 2 (10.0%) | 0 | 0 | 0 | 0 | 0 | 8 (40.0%) | 4 (20.0%) | 4 (20.0%) | 0 | 0 | 0 | 0 |
| 4 | 16 (88.9%) | 0 | 2 (11.1%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (40.0%) | 4 (20.0%) | 4 (20.0%) | 4 (20.0%) | 0 | 0 | 0 |

### Region 5

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 59 | 0x00 | 53 | 0 | 0,0,0 | Rapid Fire(Gear)@pos0, Rapid Fire(Gear)@pos1, Rapid Fire(Gear)@pos2 |
| 1 | 59 | 0x00 | 53 | 0 | 0,0,0 | Rapid Fire(Gear)@pos0, Rapid Fire(Gear)@pos1, Rapid Fire(Gear)@pos2 |
| 2 | 59 | 0x00 | 53 | 0 | 0,0,0 | Rapid Fire(Gear)@pos1, Rapid Fire(Gear)@pos2 |
| 3 | 59 | 0x00 | 53 | 0 | 0,0,0 | Rapid Fire(Gear)@pos1, Rapid Fire(Gear)@pos2 |
| 4 | 65 | 0x00 | 53 | 0 | 0,0,0 | Pedestal(Gear)@pos1, Pedestal(Gear)@pos2 |
| 5 | 65 | 0x00 | 53 | 0 | 0,0,0 | Pedestal(Gear)@pos1, Pedestal(Gear)@pos2 |
| 6 | 66 | 0x00 | 53 | 0 | 0,0,0 | Golem(Gear)@pos0 |
| 7 | 66 | 0x00 | 53 | 0 | 0,0,0 | Golem(Gear)@pos0 |
| 8 | 39 | 0x00 | 53 | 0 | 0,0,0 | Wyrm(Gear)@pos0 |
| 9 | 39 | 0x00 | 53 | 0 | 0,0,0 | Wyrm(Gear)@pos0 |
| 10 | 1 | 0x00 | 53 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 11 | 1 | 0x00 | 53 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x00 | 53 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x00 | 53 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x00 | 53 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 15 | 1 | 0x00 | 53 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 3 (12.0%) | 2 (8.0%) | 3 (12.0%) | 3 (12.0%) | 4 (16.0%) | 2 (8.0%) | 3 (12.0%) | 1 (4.0%) | 3 (12.0%) | 1 (4.0%) | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 3 (12.0%) | 2 (8.0%) | 3 (12.0%) | 3 (12.0%) | 4 (16.0%) | 2 (8.0%) | 3 (12.0%) | 1 (4.0%) | 3 (12.0%) | 1 (4.0%) | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 3 (12.0%) | 2 (8.0%) | 3 (12.0%) | 3 (12.0%) | 4 (16.0%) | 2 (8.0%) | 3 (12.0%) | 1 (4.0%) | 3 (12.0%) | 1 (4.0%) | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 3 (12.0%) | 2 (8.0%) | 3 (12.0%) | 3 (12.0%) | 4 (16.0%) | 2 (8.0%) | 3 (12.0%) | 1 (4.0%) | 3 (12.0%) | 1 (4.0%) | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 3 (12.0%) | 2 (8.0%) | 3 (12.0%) | 3 (12.0%) | 4 (16.0%) | 2 (8.0%) | 3 (12.0%) | 1 (4.0%) | 3 (12.0%) | 1 (4.0%) | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 3 (12.0%) | 2 (8.0%) | 3 (12.0%) | 3 (12.0%) | 4 (16.0%) | 2 (8.0%) | 3 (12.0%) | 1 (4.0%) | 3 (12.0%) | 1 (4.0%) | 0 | 0 | 0 | 0 | 0 | 0 |

### Region 6

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 32 | 0x00 | 44 | 0 | 0,0,0 | Edelweiss@pos0, Edelweiss@pos0, Edelweiss@pos0 |
| 1 | 30 | 0x00 | 44 | 0 | 0,0,0 | Ripper@pos0 |
| 2 | 69 | 0x00 | 44 | 0 | 0,0,0 | Pecking Duck@pos0, Pecking Duck@pos0 |
| 3 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 4 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 5 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 (44.4%) | 6 (33.3%) | 4 (22.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 8 (44.4%) | 6 (33.3%) | 4 (22.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 8 (44.4%) | 6 (33.3%) | 4 (22.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 4 (20.0%) | 0 | 2 (10.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 8 (40.0%) | 0 | 0 | 4 (20.0%) | 1 (5.0%) | 1 (5.0%) | 0 |
| 4 | 8 (44.4%) | 6 (33.3%) | 4 (22.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 7

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 61 | 0x00 | 44 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 1 | 61 | 0x00 | 44 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 2 | 61 | 0x00 | 44 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 3 | 66 | 0x00 | 44 | 0 | 0,0,0 | Golem(Gear)@pos0 |
| 4 | 17 | 0x00 | 44 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0 |
| 5 | 30 | 0x00 | 44 | 0 | 0,0,0 | Ripper@pos0 |
| 6 | 32 | 0x00 | 44 | 0 | 0,0,0 | Edelweiss@pos0, Edelweiss@pos0, Edelweiss@pos0 |
| 7 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (9.1%) | 2 (9.1%) | 2 (9.1%) | 1 (4.5%) | 5 (22.7%) | 2 (9.1%) | 8 (36.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 2 (9.1%) | 2 (9.1%) | 2 (9.1%) | 1 (4.5%) | 5 (22.7%) | 2 (9.1%) | 8 (36.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 2 (9.1%) | 2 (9.1%) | 2 (9.1%) | 1 (4.5%) | 5 (22.7%) | 2 (9.1%) | 8 (36.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 2 (12.5%) | 2 (12.5%) | 0 |
| 4 | 0 | 0 | 0 | 4 (21.1%) | 5 (26.3%) | 2 (10.5%) | 8 (42.1%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 8

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 61 | 0x00 | 47 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 1 | 61 | 0x00 | 47 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 2 | 61 | 0x00 | 47 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 3 | 66 | 0x00 | 47 | 0 | 0,0,0 | Golem(Gear)@pos0 |
| 4 | 17 | 0x00 | 47 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 5 | 30 | 0x00 | 47 | 0 | 0,0,0 | Ripper@pos0 |
| 6 | 7 | 0x00 | 47 | 0 | 0,0,0 | Carrier@pos0 |
| 7 | 36 | 0x00 | 47 | 0 | 0,0,0 | Shinobi Mk0(Gear)@pos0 |
| 8 | 36 | 0x00 | 47 | 0 | 0,0,0 | Shinobi Mk0(Gear)@pos1, Shinobi Mk0(Gear)@pos2 |
| 9 | 44 | 0x00 | 47 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 47 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 47 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 47 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 47 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 47 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 47 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (7.4%) | 2 (7.4%) | 2 (7.4%) | 4 (14.8%) | 8 (29.6%) | 0 | 1 (3.7%) | 4 (14.8%) | 4 (14.8%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 2 (7.4%) | 2 (7.4%) | 2 (7.4%) | 4 (14.8%) | 8 (29.6%) | 0 | 1 (3.7%) | 4 (14.8%) | 4 (14.8%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 0 | 0 | 2 (8.7%) | 4 (17.4%) | 8 (34.8%) | 0 | 1 (4.3%) | 4 (17.4%) | 4 (17.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 4 (18.2%) | 1 (4.5%) | 1 (4.5%) | 0 |
| 4 | 0 | 0 | 0 | 4 (19.0%) | 8 (38.1%) | 0 | 1 (4.8%) | 4 (19.0%) | 4 (19.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 9

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 61 | 0x00 | 44 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 1 | 61 | 0x00 | 44 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 2 | 61 | 0x00 | 44 | 0 | 0,0,0 | Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 3 | 66 | 0x00 | 44 | 0 | 0,0,0 | Golem(Gear)@pos0 |
| 4 | 17 | 0x00 | 44 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0 |
| 5 | 30 | 0x00 | 44 | 0 | 0,0,0 | Ripper@pos0 |
| 6 | 32 | 0x00 | 44 | 0 | 0,0,0 | Edelweiss@pos0, Edelweiss@pos0, Edelweiss@pos0 |
| 7 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (9.1%) | 2 (9.1%) | 2 (9.1%) | 1 (4.5%) | 5 (22.7%) | 2 (9.1%) | 8 (36.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 2 (9.1%) | 2 (9.1%) | 2 (9.1%) | 1 (4.5%) | 5 (22.7%) | 2 (9.1%) | 8 (36.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 2 (9.1%) | 2 (9.1%) | 2 (9.1%) | 1 (4.5%) | 5 (22.7%) | 2 (9.1%) | 8 (36.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 2 (12.5%) | 2 (12.5%) | 0 |
| 4 | 0 | 0 | 0 | 1 (6.2%) | 5 (31.2%) | 2 (12.5%) | 8 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 10

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 32 | 0x40 | 1 | 0 | 0,0,0 | Edelweiss@pos0, Edelweiss@pos0 |
| 1 | 30 | 0x40 | 1 | 0 | 0,0,0 | Ripper@pos0 |
| 2 | 69 | 0x40 | 1 | 0 | 0,0,0 | Pecking Duck@pos0, Pecking Duck@pos0, Pecking Duck@pos0 |
| 3 | 23 | 0x40 | 1 | 0 | 0,0,0 | Rotten Sod@pos0 |
| 4 | 32 | 0x40 | 1 | 0 | 1,1,1 | Edelweiss@pos1, Edelweiss@pos1, Edelweiss@pos1 |
| 5 | 69 | 0x40 | 1 | 0 | 1,1,1 | Pecking Duck@pos1, Pecking Duck@pos1, Pecking Duck@pos1 |
| 6 | 71 | 0x40 | 6 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 7 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x40 | 1 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x40 | 1 | 0 | 0,0,0 | Sufal Gear(Gear)@pos0 |
| 11 | 44 | 0x40 | 1 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal Gear(Gear)@pos0 |
| 12 | 44 | 0x40 | 1 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x40 | 1 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x40 | 1 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 (22.2%) | 2 (5.6%) | 6 (16.7%) | 4 (11.1%) | 8 (22.2%) | 6 (16.7%) | 2 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 8 (22.2%) | 2 (5.6%) | 6 (16.7%) | 4 (11.1%) | 8 (22.2%) | 6 (16.7%) | 2 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 8 (22.2%) | 2 (5.6%) | 6 (16.7%) | 4 (11.1%) | 8 (22.2%) | 6 (16.7%) | 2 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 8 (22.2%) | 2 (5.6%) | 6 (16.7%) | 4 (11.1%) | 8 (22.2%) | 6 (16.7%) | 2 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 8 (22.2%) | 2 (5.6%) | 6 (16.7%) | 4 (11.1%) | 8 (22.2%) | 6 (16.7%) | 2 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (40.0%) | 4 (20.0%) | 4 (20.0%) | 4 (20.0%) | 0 | 0 | 0 |

### Region 11

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x00 | 7 | 0 | 0,0,0 | Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 1 | 5 | 0x00 | 7 | 0 | 0,0,0 | Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man(Gear)@pos0 |
| 2 | 39 | 0x00 | 7 | 0 | 0,0,0 | Death Scythe(Gear)@pos0 |
| 3 | 71 | 0x00 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 4 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 5 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 7 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 7 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 7 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 (25.0%) | 4 (25.0%) | 8 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 4 (25.0%) | 4 (25.0%) | 8 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 4 (25.0%) | 4 (25.0%) | 8 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 2 (12.5%) | 2 (12.5%) | 0 |
| 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 2 (12.5%) | 2 (12.5%) | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 2 (12.5%) | 2 (12.5%) | 0 |

### Region 12

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 71 | 0x00 | 44 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 1 | 71 | 0x00 | 44 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos1, Tusk-Tusk(Gear)@pos2 |
| 2 | 24 | 0x00 | 44 | 0 | 0,0,0 | Croaker Tribe@pos0, Croaker Tribe@pos0, Croaker Tribe@pos1, Croaker Tribe@pos1, Croaker Tribe@pos2, Croaker Tribe@pos2 |
| 3 | 23 | 0x00 | 44 | 0 | 0,0,0 | Rotten Sod@pos0, Rotten Sod@pos0, Rotten Sod@pos0, Rotten Sod@pos0 |
| 4 | 69 | 0x00 | 44 | 0 | 0,0,0 | Pecking Duck@pos0, Pecking Duck@pos0, Pecking Duck@pos0 |
| 5 | 32 | 0x00 | 44 | 0 | 0,0,0 | Edelweiss@pos0, Edelweiss@pos0, Edelweiss@pos1, Edelweiss@pos1, Edelweiss@pos2, Edelweiss@pos2 |
| 6 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 12 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal(Gear)@pos1, Sufal(Gear)@pos2 |
| 13 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (50.0%) | 0 | 0 | 4 (25.0%) | 2 (12.5%) | 2 (12.5%) | 0 |
| 4 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (33.3%) | 4 (16.7%) | 4 (16.7%) | 4 (16.7%) | 2 (8.3%) | 2 (8.3%) | 0 |

### Region 13

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 66 | 0x00 | 43 | 0 | 0,0,0 | Golem(Gear)@pos0 |
| 1 | 66 | 0x00 | 43 | 0 | 0,0,0 | Golem(Gear)@pos1, Golem(Gear)@pos2 |
| 2 | 71 | 0x00 | 43 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 3 | 41 | 0x00 | 43 | 0 | 0,0,0 | Neo Tears@pos0, Neo Tears@pos0, Neo Tears@pos0 |
| 4 | 41 | 0x00 | 43 | 0 | 0,0,0 | Gimmick@pos0, Gimmick@pos0 |
| 5 | 24 | 0x00 | 43 | 0 | 0,0,0 | Forbidden[hidden]@pos0, Forbidden[hidden]@pos0, Forbidden@pos0, Forbidden@pos0 |
| 6 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 10 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 11 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 15 | 1 | 0x00 | 43 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (40.0%) | 2 (40.0%) | 1 (20.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 2 (40.0%) | 2 (40.0%) | 1 (20.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 2 (40.0%) | 2 (40.0%) | 1 (20.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 2 (6.9%) | 2 (6.9%) | 1 (3.4%) | 8 (27.6%) | 8 (27.6%) | 8 (27.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 2 (6.9%) | 2 (6.9%) | 1 (3.4%) | 8 (27.6%) | 8 (27.6%) | 8 (27.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 2 (6.9%) | 2 (6.9%) | 1 (3.4%) | 8 (27.6%) | 8 (27.6%) | 8 (27.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

### Region 14

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 71 | 0x00 | 44 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 1 | 71 | 0x00 | 44 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos1, Tusk-Tusk(Gear)@pos2 |
| 2 | 24 | 0x00 | 44 | 0 | 0,0,0 | Croaker Tribe@pos0, Croaker Tribe@pos0, Croaker Tribe@pos1, Croaker Tribe@pos1, Croaker Tribe@pos2, Croaker Tribe@pos2 |
| 3 | 23 | 0x00 | 44 | 0 | 0,0,0 | Rotten Sod@pos0, Rotten Sod@pos0, Rotten Sod@pos0, Rotten Sod@pos0 |
| 4 | 69 | 0x00 | 44 | 0 | 0,0,0 | Pecking Duck@pos0, Pecking Duck@pos0, Pecking Duck@pos0 |
| 5 | 32 | 0x00 | 44 | 0 | 0,0,0 | Edelweiss@pos0, Edelweiss@pos0, Edelweiss@pos1, Edelweiss@pos1, Edelweiss@pos2, Edelweiss@pos2 |
| 6 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0, Sufal@pos0, Sufal@pos0, Sufal@pos0 |
| 10 | 44 | 0x00 | 44 | 0 | 0,0,0 | Sufal@pos0 |
| 11 | 72 | 0x00 | 44 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 12 | 65 | 0x00 | 44 | 0 | 0,0,0 | Airwalk(Gear)@pos1, Airwalk(Gear)@pos2 |
| 13 | 73 | 0x00 | 44 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 14 | 65 | 0x00 | 44 | 0 | 0,0,0 | Pedestal(Gear)@pos0 |
| 15 | 1 | 0x00 | 44 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 (25.0%) | 2 (25.0%) | 2 (25.0%) | 2 (25.0%) | 0 |
| 4 | 3 (13.6%) | 3 (13.6%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 4 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 (25.0%) | 2 (25.0%) | 2 (25.0%) | 2 (25.0%) | 0 |

### Region 15

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 1 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 2 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 3 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 4 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 5 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 6 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 9 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 10 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 11 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 15 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |

Weight rows (row `0..3` are story-progress-selected; `4..5` are stored but not reachable by the documented selection formula). Percentages are each formation's share of that row's total, rounded to one decimal place:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
