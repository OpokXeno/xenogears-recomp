# Field Encounter Tables

## 1. Scope

This chapter is a data table, not a mechanism description: it presents the
complete formation and weight content of every Field random-encounter table
on Disc 1, covering the 635 of 730 fields that carry one. The selection
mechanism that consumes this data is already documented in
[`field/11` §§2-5](../field/11-encounters-transitions-loading-and-persistence.md#2-encounter-resource)
and is only summarized here; the exact byte layout is documented in
[`field/02` §7](../field/02-resource-and-script-formats.md#7-encounter-section).

## 2. Selection Formula

Quoted from `field/11` §5 (`FieldRandomEncounterUpdate`, `0x80079288`):

```text
sample = floor(random15 * (sum(weights) + 1) / 32768)
```

Sample `0` selects no encounter; values `1..sum(weights)` are mapped through
the cumulative formation weights. This differs from World Map's selection in
one important way: **Field reserves one scaled-random bucket for "no
encounter" inside the same weighted draw**
([`field/11` §5](../field/11-encounters-transitions-loading-and-persistence.md#5-weighted-formation-selection)),
where World Map's draw range is `1..sum(weights)` with no such reservation
(the no-encounter case is decided earlier, by a separate movement countdown).
Practically: a Field formation's "chance per check" in this chapter is
`weight / (sum(weights) + 1)`, not `weight / sum(weights)` — the denominator
is always one larger than the raw weight total. The table for each area also
lists the resulting chance of no encounter at all on a given check, since
Field's formula makes that outcome an explicit, quantifiable part of the same
roll rather than a gate that happens elsewhere.

This chapter does not cover *how often* a check happens in the first place —
that is a per-field timer system (interval, active countdown count) also
documented in `field/11` §3, orthogonal to the weight table itself.

## 3. Extraction Method

Each field's own map container is a distinct disc file. The container format
is documented in
[`field/02` §1](../field/02-resource-and-script-formats.md#1-map-container):
nine independent LZSS-compressed sections, with each section's decompressed
size and container-relative offset stored in two parallel 9-entry `u32`
arrays at `+0x10C` and `+0x130`. Section index `6` is the encounter section.

The field-to-file mapping was not previously documented and required tracing
the resident EXE:
`FieldLoadRawBundle` (`0x8001B53C`) is annotated as reading "archive entry
`0xB8` plus the field index," and its caller loads the field index from
`fieldMapNumber` (`0x8004F34C` — the same global the debug overlay's Map
Teleport tool writes, per `debug_overlay/README.md`), masks it to 12 bits,
then **doubles it** before the call. The actual formula is therefore:

```text
file_id = 0xB8 + 2 * field_id
```

directory `4` (found by brute-force testing candidate directories against
field `1`'s known file until one produced a structurally valid container:
increasing section offsets, plausible section sizes, and — decisively — a
528-byte (`0x210`) section 6 exactly matching `field/02` §7's documented
size). `tools/extract_field_encounters.py` implements this addressing,
decompresses section 6 when present, and decodes it with the same Formation
Record layout used throughout this project's RNG chapters.

Field names throughout this chapter come from `debug_overlay/data/fields.xml`
(730 field IDs, already named from earlier project work), not from anything
decoded in this pass.

## 4. Encounter Tables Are Shared Per Area, Not Per Field

Of 730 fields, 635 carry a non-empty encounter section, but those 635 contain
only **55 distinct formation/weight tables** — the same "shared, not
per-instance" pattern already found for World Map's 9 configurations
(`rng/03-worldmap-encounter-tables.md` §4), but organized by story area here
instead of by game progress. One canonical table serves every corridor of
Krelian's Lab, for example, rather than each of Krelian's Lab's ~39 field IDs
authoring its own copy.

This matters for reading the tables correctly:

- Each table below lists **every field ID that references it**, verified
  directly from the disc — not inferred. A field appearing in a table's list
  is a completely accurate statement about the game's data.
- That is not the same as a guarantee that walking around in every one of
  those specific fields can trigger the listed encounters. Field's random
  encounter check only runs while the player is receiving ordinary directional
  input during real walking movement (`field/11` §4) — a field that is a
  cutscene, a vehicle interior, or otherwise never reaches that input state
  will never roll this table regardless of what data it points at. This
  chapter cannot distinguish "this field is a real walkable instance of this
  area" from "this field inherits the area's table but is never in a state to
  use it" from static data alone; that would need either deeper script
  analysis of each field's own control flow or live testing, neither done
  here.
- One table is a confirmed, certain exception: **the single largest bare
  table (102 fields) resolves entirely to enemy set `0`**, which
  [`rng/04-enemy-drop-tables.md`](04-enemy-drop-tables.md) already established
  decodes to non-text garbage in every context it appears — there is no real
  monster data behind it anywhere. That table is marked "confirmed inert"
  below; every other table shown uses real, named enemies and could not have
  been produced from unused data by chance (compare Table 2's roster — Solaris
  Guard, Neo Wels, Phobia, Abandon, Littlefoot — against its exclusively
  Krelian's-Lab-and-Etrenank field list).

## 5. Enemy Name Resolution

Enemy names are resolved exactly as in
[`rng/03-worldmap-encounter-tables.md` §6](03-worldmap-encounter-tables.md#6-enemy-name-resolution):
the enemy-set string bundle's index runs in step with the definition slot. A
slot that could not be resolved to a name falls back to `def<N>`.

## 6. Table Format

Each table below follows the same conventions as
[`rng/03-worldmap-encounter-tables.md` §5](03-worldmap-encounter-tables.md#5-region-table-format):
enemy entries show the resolved monster name (Section 5 above) in place of the
raw definition slot wherever a name was recovered, followed by `(Gear)` for
the Gear-scale flag, `[hidden]` for the hidden flag, `@pos<P>` for position,
and `/f` for the facing flag. An empty formation slot is omitted; a formation
with no enemy entries shows `(none)`.

The weight row header repeats the `03` chapter's matrix format (`Row | 0 | 1 |
... | 15`) even though each Field table has exactly one weight array, not six
— this keeps the two chapters' tables visually and structurally identical.
The added final column, `(no encounter)`, has no equivalent in `03`'s tables:
it is Field-specific, reflecting the reserved bucket described in Section 2.

## 7. Encounter Tables

### Table 0 (used by 251 field(s))

Fields: 0 (Main debug room), 1 (Lahan village), 4 (Introductory text at the start of the game), 5 (Lahan - Beginners hall), 6 (Lahan - Item shop), 7 (Lahan - Rock - Paper - Scissors man), 8 (Lahan - RPS man's basement), 9 (Lahan - Pub), 10 (Lahan - Pub, upstairs), 11 (Lahan - Alice's house), 12 (Lahan - Fei talks to Alice), 13 (Lahan - Fei's house), 14 (Lahan - Looking at Fei's painting), 17 (Citan's House), 18 (Citan's house - Bedroom), ... and 236 more (run --format json for the complete per-field mapping)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 9 | 0xA3 | 18 | 1 | 0,0,0 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)@pos3, Alpha Weltall(Gear)@pos4 |
| 1 | 11 | 0x00 | 28 | 0 | 0,0,0 | Rankar Dragon(Gear)@pos0 |
| 2 | 2 | 0x00 | 17 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot(Gear)@pos0 |
| 3 | 1 | 0x00 | 3 | 0 | 128,128,128 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1, Jackal@pos1, Jackal@pos1 |
| 4 | 15 | 0x00 | 7 | 0 | 0,0,0 | Alkanshel(Gear)@pos0 |
| 5 | 11 | 0x00 | 29 | 0 | 0,0,0 | Rankar Dragon(Gear)@pos0 |
| 6 | 2 | 0x00 | 30 | 0 | 0,0,0 | Gigafoot(Gear)@pos1, Gigafoot(Gear)@pos0 |
| 7 | 9 | 0x00 | 17 | 0 | 0,0,0 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2 |
| 8 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos2, Aveh Corporal@pos2 |
| 9 | 9 | 0x00 | 11 | 0 | 2,2,2 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2 |
| 10 | 3 | 0x00 | 22 | 0 | 0,0,0 | Forest Elf@pos0, Forest Elf@pos0, Forest Elf@pos0, Forest Elf@pos0 |
| 11 | 7 | 0x00 | 2 | 0 | 0,0,0 | Carrier@pos0, Carrier@pos0, Carrier@pos0, Shadey@pos0 |
| 12 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 13 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 14 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 15 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 1 (used by 102 field(s)) — confirmed inert (enemy set 0 is non-text garbage everywhere it appears)

Fields: 49 (Aveh Desert - UFO(Shevat) flies overhead), 50 (Aveh Desert - Bikes ride by, Fei steals one), 56 (Aveh Transport(Fei's dream about Grahf & Kahn)), 59 (Aveh Transport - Fei climbs the crane to Weltall), 60 (Aveh Transport hit by Yggdrasil), 61 (Sand Cruiser Top - Fei in Weltall takes Citan to Desert), 63 (Sand Cruiser - driving with Weltall on top), 178 (Assault on Aveh - Fei spots target Vanderkaum from mountain peak), 179 (Assault on Aveh - Ramsus and Miang's destroyer damages the Yggdrasil), 182 (Assault on Aveh - Fei reaches Vanderkaum's flagship), 183 (Assault on Aveh - Yggdrasil bridge, Grahf's music (hangs)), 184 (Assault on Aveh - Vanderkaum gets in the Dora), 186 (Assault on Aveh - Dora takes out Maitreya's unit), 187 (Assault on Aveh - Bart tries to escape from Ramsus' destroyer), 188 (Assault on Aveh - Id takes out Ramsus' fleet), ... and 87 more (run --format json for the complete per-field mapping)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 1 | 0 | 0x00 | 1 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 2 | 0 | 0x00 | 2 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 3 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 4 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 5 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 6 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 7 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 8 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 9 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 10 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 11 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 12 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 13 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 14 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |
| 15 | 0 | 0x00 | 0 | 0 | 0,0,0 | def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0, def0(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 2 (used by 39 field(s))

Fields: 545 (Etrenank - garbage shredder), 546 (Etrenank - garbage chute ladder), 547 (Etrenank - "food" storage area), 548 (Etrenank - hallway to 547), 551 (Etrenank Soylent entrance), 552 (Etrenank Soylent - Citan explains about the 'M Plan' (Erich, Nikolai)), 555 (Krelian's Lab entrance), 556 (Krelian's Lab - short corridor), 557 (Krelian's Lab - inclined T corridor), 558 (Krelian's Lab - short side corridor), 559 (Krelian's Lab - wide corridor between caged Wels), 560 (Krelian's Lab - inclined corridor, locked door), 561 (Krelian's Lab - upstairs 555, wide corridor after red doors), 562 (Krelian's Lab - wide corridor before Wels surgery), 563 (Krelian's Lab - wide corridor with memory cube), ... and 24 more (run --format json for the complete per-field mapping)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0 |
| 1 | 17 | 0x40 | 59 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 2 | 30 | 0x40 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 3 | 23 | 0x40 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 4 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 5 | 33 | 0x40 | 59 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 7 | 17 | 0xC0 | 59 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 8 | 30 | 0xC0 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 9 | 23 | 0xC0 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 10 | 67 | 0xC0 | 59 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 11 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 12 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 13 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 14 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 15 | 33 | 0xC0 | 48 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 (38.1%) | 0 | 2 (9.5%) | 2 (9.5%) | 4 (19.0%) | 4 (19.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (4.8%) |

### Table 3 (used by 31 field(s))

Fields: 101 (Bledavik map), 130 (Bledavik Underground waterways - near - hotel ladder), 131 (Aveh Castle - tournament sign - up), 133 (Aveh Castle - waterways exit), 134 (Aveh Castle - waterways exit, Shakhan's trap), 135 (Aveh Castle - first floor hall), 136 (Aveh Castle South - 2nd floor west bedroom), 137 (Aveh Castle South - 2nd floor east bedroom), 138 (Aveh Castle East - Cobra Cracka bedroom), 139 (Aveh Castle East - bedroom), 140 (Aveh Castle East - Armory), 141 (Aveh Castle East, 2nd floor - Vanderkaum's room), 142 (Aveh Castle East, 2nd floor - Minister's room, south), 143 (Aveh Castle West - kitchen), 144 (Aveh Castle West - mess hall), ... and 16 more (run --format json for the complete per-field mapping)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 3 | 0xE0 | 8 | 32 | 0,0,0 | Dive Bomber@pos0 |
| 1 | 47 | 0xE0 | 8 | 32 | 0,0,0 | Big Joe@pos0 |
| 2 | 47 | 0xE0 | 8 | 32 | 0,0,0 | Big Joe@pos0 |
| 3 | 8 | 0xE0 | 8 | 9 | 0,0,0 | Dan@pos0 |
| 4 | 8 | 0xE0 | 8 | 16 | 0,0,0 | Dan@pos0 |
| 5 | 4 | 0x40 | 13 | 0 | 0,0,0 | Aveh Soldier@pos0, Aveh Soldier@pos0 |
| 6 | 4 | 0x40 | 13 | 0 | 0,0,0 | Aveh Guard@pos0, Aveh Guard@pos0 |
| 7 | 4 | 0x40 | 13 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 8 | 4 | 0x40 | 13 | 0 | 0,0,0 | Aveh Soldier@pos0, Aveh Soldier@pos0, Aveh Soldier@pos0, Aveh Soldier@pos0 |
| 9 | 4 | 0x40 | 13 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 10 | 68 | 0x20 | 18 | 44 | 0,0,0 | ?(Gear)@pos0, ?(Gear)@pos1, ?(Gear)@pos2, ?(Gear)@pos3, def2@pos1 |
| 11 | 4 | 0x40 | 31 | 0 | 0,0,0 | Aveh Soldier@pos0, Aveh Soldier@pos0 |
| 12 | 4 | 0x40 | 31 | 0 | 0,0,0 | Aveh Guard@pos0, Aveh Guard@pos0 |
| 13 | 45 | 0xE0 | 13 | 28 | 0,0,0 | Margie@pos1, Margie@pos0, Ramsus@pos0, Miang[hidden]@pos1 |
| 14 | 51 | 0xA3 | 18 | 30 | 0,0,0 | Id@pos0, Id(Gear)@pos2, Id(Gear)@pos1, Id(Gear)@pos0 |
| 15 | 45 | 0xE0 | 13 | 29 | 0,0,0 | Margie@pos1, Margie@pos0, Ramsus@pos0, def4@pos2/f |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 4 (26.7%) | 4 (26.7%) | 2 (13.3%) | 2 (13.3%) | 2 (13.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 1 (6.7%) |

### Table 4 (used by 22 field(s))

Fields: 367 (Ethos Dig Site - nanomachine decontamination hallway 1), 368 (Ethos Dig Site - hallway after 367), 369 (Ethos Dig Site - inactive control room 1), 370 (Ethos Dig Site - stairwell 1 top), 371 (Ethos Dig Site - nanomachine decontamination hallway 2), 372 (Ethos Dig Site - hallway after 371), 373 (Ethos Dig Site - inactive control room 2), 374 (Ethos Dig Site - memory cube room), 375 (Ethos Dig Site - stairwell 2 top), 377 (Ethos Dig Site - hall after 376), 379 (Ethos Dig Site - hall to Emeralda's room (Elly's flashback ? )), 508 (Etrenank Soylent - conference room 1), 516 (Solaris - site of Elly's parents' deaths), 591 (Krelian's Lab - interrogation room rescue), 592 (Krelian's Lab - corridor from Krelian's private lab), ... and 7 more (run --format json for the complete per-field mapping)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 6 | 0x40 | 23 | 0 | 0,0,0 | Shellbelle@pos0, Shellbelle@pos0 |
| 1 | 6 | 0x40 | 23 | 0 | 2,2,2 | Shellbelle@pos2, Shellbelle@pos2, Shellbelle@pos2, Shellbelle@pos2 |
| 2 | 6 | 0x40 | 23 | 0 | 1,1,1 | Shellbelle@pos0, Shellbelle@pos1, Shellbelle@pos1, Shellbelle@pos1 |
| 3 | 7 | 0x40 | 23 | 0 | 0,0,0 | Carrier@pos0, Carrier@pos0 |
| 4 | 30 | 0x40 | 23 | 0 | 1,1,1 | Ripper@pos1 |
| 5 | 50 | 0x40 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 6 | 50 | 0x40 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 7 | 50 | 0xC0 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 8 | 50 | 0xC0 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 9 | 50 | 0x40 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 10 | 50 | 0x40 | 23 | 0 | 1,1,1 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 11 | 4 | 0x40 | 23 | 0 | 0,0,0 | Aveh Corporal@pos0 |
| 12 | 4 | 0x40 | 23 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 13 | 4 | 0x40 | 23 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 14 | 57 | 0xC0 | 26 | 0 | 0,0,0 | Kelvina@pos0, Tolone@pos0 |
| 15 | 51 | 0xC0 | 39 | 0 | 0,0,0 | Id@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (11.8%) | 2 (11.8%) | 2 (11.8%) | 6 (35.3%) | 4 (23.5%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (5.9%) |

### Table 5 (used by 15 field(s))

Fields: 198 (Nortune map(after escape)), 273 (Solaris - Gazel plans the Nortune purge), 274 (Solaris - Gazel reprimands Ramsus after Fei reaches Shevat), 495 (Gate Generator 1 - Shakhan battle), 505 (Gate Generator 3 - Crescens battle), 607 (Solaris - Hammer tries to kidnap Elly), 611 (Solaris - Id attacks Yggdrasil, Elly stops him(long scenes follow)), 613 (Taura's House - Ramsus' Omnigear pursues Fei and Elly), 616 (Taura's House - nanoreactor room), 618 (Nortune - Yggdrasil IV versus Ft.Hurricane), 619 (Mahanon - Razael's Tree), 622 (Ignas Soylent System - Sufal Mass battle), 654 (Mahanon - later in 652 cutscene, Deus battle), 655 (Mahanon - Razael's Tree), 728 (Debug room - Saito)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 54 | 0x80 | 7 | 0 | 0,0,0 | Skyghene(Gear)@pos0, Marinebasher(Gear)@pos1 |
| 1 | 53 | 0x80 | 55 | 0 | 0,0,0 | Shakhan(Gear)@pos0 |
| 2 | 31 | 0x80 | 55 | 0 | 0,0,0 | Brigandier(Gear)@pos0 |
| 3 | 56 | 0xC0 | 38 | 0 | 0,0,0 | Grahf@pos0, Grahf@pos0 |
| 4 | 74 | 0xA0 | 53 | 15 | 0,0,0 | Sword Seraph(Gear)@pos0 |
| 5 | 53 | 0xA0 | 70 | 18 | 0,0,0 | Shakhan(Gear)@pos0 |
| 6 | 53 | 0xA0 | 6 | 19 | 2,2,2 | Shakhan(Gear)@pos0 |
| 7 | 57 | 0xC0 | 39 | 0 | 0,0,0 | Dominia@pos0, Dominia@pos0, Tolone@pos0, Seraphita@pos0 |
| 8 | 64 | 0xA0 | 44 | 20 | 0,0,0 | Deus(Gear)@pos0 |
| 9 | 44 | 0xC0 | 52 | 0 | 0,0,0 | Sufal(Gear)@pos0, Sufal Gear@pos0, Sufal Gear@pos0, Sufal Gear@pos0, Sufal Gear@pos0 |
| 10 | 55 | 0x80 | 62 | 0 | 0,0,0 | G Elements(Gear)@pos0 |
| 11 | 55 | 0x80 | 4 | 0 | 0,0,0 | G Elements(Gear)@pos0 |
| 12 | 64 | 0x80 | 51 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 13 | 13 | 0xA0 | 49 | 32 | 0,0,0 | Vierge(Gear)@pos0 |
| 14 | 52 | 0xA0 | 61 | 39 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos1, def2(Gear)@pos2 |
| 15 | 52 | 0xA0 | 61 | 21 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos1, def2(Gear)@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 6 (used by 15 field(s))

Fields: 339 (Ethos HQ entrance), 340 (Ethos HQ - upstairs, Etone room 3), 341 (Ethos HQ - Bishop's room (exit to hidden treasure room hangs)), 342 (Thames - after 360, Gazel(eliminate Ethos), Krelian(Zeboim)), 343 (Ethos HQ - stairwell to infirmary(hangs)), 344 (Ethos HQ Purge - stairwell with bishop(hangs)), 345 (Ethos HQ - stairwell to 341), 346 (Ethos HQ - Billy leads way downstairs to Ethos infirmary), 347 (Ethos HQ - upstairs hallway leading to 348), 348 (Ethos HQ - upstairs hallway to 340), 349 (Ethos HQ Purge - library), 350 (Ethos HQ Purge - dining room), 351 (Ethos HQ Purge - kitchen), 352 (Ethos HQ Purge - infirmary), 354 (Ethos HQ Purge - confessional)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 17 | 0x40 | 50 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos1, Neo Wels@pos1, Neo Wels@pos1 |
| 1 | 10 | 0x40 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 2 | 10 | 0x40 | 50 | 0 | 0,0,0 | Assassin@pos1, Assassin@pos1, Assassin@pos1 |
| 3 | 10 | 0x40 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 4 | 10 | 0x40 | 50 | 0 | 0,0,0 | Assassin@pos1, Assassin@pos1, Assassin@pos1 |
| 5 | 10 | 0x40 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 6 | 17 | 0xC0 | 50 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos1, Neo Wels@pos1, Neo Wels@pos1 |
| 7 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 8 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 67 | 0xC0 | 21 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (14.3%) | 1 (14.3%) | 1 (14.3%) | 1 (14.3%) | 1 (14.3%) | 1 (14.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (14.3%) |

### Table 7 (used by 11 field(s))

Fields: 54 (Aveh Desert - Fei ambushed by 2 gears, Citan brings Weltall), 55 (Aveh Desert - Fei meets Grahf), 62 (Desert - Bart's introduction in Bridgandier), 196 (Nortune, D block - path from A block), 199 (Nortune, D block - 'Baptism' battle), 311 (Thames - Yggdrasil under attack), 323 (Thames - Kelvena and Dominia attack the Yggdrasil), 335 (Ethos Transport - battle with Giant Wels boss), 388 (Ethos Dig Site - Stein and Grahf, Alkanshel battle), 411 (Babel Tower - outer wall(hangs, no Ramsus)), 420 (Babel Tower - top(Seibzehn battle))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 2 | 0x80 | 12 | 0 | 0,0,0 | Gigafoot(Gear)@pos1, Gigafoot(Gear)@pos2 |
| 1 | 39 | 0x80 | 12 | 0 | 0,0,0 | Wyrm(Gear)@pos0 |
| 2 | 31 | 0xA0 | 7 | 5 | 0,0,0 | Brigandier(Gear)@pos0 |
| 3 | 3 | 0xE0 | 2 | 32 | 0,0,0 | Gonzalez@pos0 |
| 4 | 3 | 0xE0 | 2 | 32 | 0,0,0 | Leonardo@pos0 |
| 5 | 3 | 0xE0 | 2 | 32 | 0,0,0 | Heinrich@pos0 |
| 6 | 7 | 0xE0 | 2 | 32 | 0,0,0 | Shadey@pos0 |
| 7 | 28 | 0xE0 | 2 | 27 | 0,0,0 | Rico@pos0 |
| 8 | 18 | 0xA0 | 5 | 33 | 0,0,0 | Miang's Gear(Gear)@pos0 |
| 9 | 54 | 0x80 | 53 | 0 | 0,0,0 | Grandgrowl(Gear)@pos0 |
| 10 | 18 | 0xA0 | 5 | 34 | 0,0,0 | Haishao(Gear)@pos0, Wyvern(Gear)@pos1 |
| 11 | 48 | 0x80 | 53 | 0 | 0,0,0 | Giant Wels(Gear)@pos0 |
| 12 | 15 | 0xA0 | 69 | 35 | 0,0,0 | Alkanshel(Gear)@pos0 |
| 13 | 18 | 0x80 | 36 | 0 | 0,0,0 | Wyvern(Gear)@pos0, Wyvern(Gear)@pos3 |
| 14 | 42 | 0x80 | 46 | 0 | 0,0,0 | Seibzehn(Gear)@pos0 |
| 15 | 15 | 0xA0 | 69 | 31 | 0,0,0 | Schpariel(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (50.0%) |

### Table 8 (used by 10 field(s))

Fields: 260 (Goliath Factory - first "ambush room"), 261 (Goliath Factory - second "ambush room"), 262 (Goliath Factory entrance), 263 (Goliath Factory - conveyor belt switch room), 264 (Goliath Factory - hall to 260), 265 (Goliath Factory - 263 after switching conveyor direction), 266 (Goliath Factory - middle conveyor belt room), 268 (Goliath Factory - Fis - 6 room), 270 (Goliath - Grahf attacks(Hammer:  "...MAD SKILLZ ? !")), 271 (Goliath - Bart missile hits Goliath)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 9 | 0x80 | 11 | 0 | 0,0,0 | def3(Gear)@pos0, def3(Gear)@pos1, def3(Gear)@pos2 |
| 1 | 16 | 0x80 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2 |
| 2 | 16 | 0x80 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos0, HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2 |
| 3 | 16 | 0x80 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 4 | 16 | 0x80 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2 |
| 5 | 16 | 0x80 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2, Hatamoto Mk3@pos1, Hatamoto Mk3@pos1, Hatamoto Mk3@pos2, Hatamoto Mk3@pos2 |
| 6 | 16 | 0x80 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2, Hatamoto Mk3@pos1, Hatamoto Mk3@pos1, Hatamoto Mk3@pos2, Hatamoto Mk3@pos2 |
| 7 | 9 | 0x00 | 11 | 0 | 0,0,0 | def3(Gear)@pos0, def3(Gear)@pos1, def3(Gear)@pos2 |
| 8 | 16 | 0x00 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2 |
| 9 | 16 | 0x00 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 10 | 16 | 0x00 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2 |
| 11 | 16 | 0x00 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 12 | 16 | 0x00 | 11 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos1, HarquebusMk10(Gear)@pos2, Hatamoto Mk3@pos1, Hatamoto Mk3@pos1, Hatamoto Mk3@pos2, Hatamoto Mk3@pos2 |
| 13 | 36 | 0x00 | 11 | 0 | 0,0,0 | Shinobi Mk0(Gear)@pos2 |
| 14 | 38 | 0x80 | 29 | 0 | 0,0,0 | Fis-6(Gear)@pos0, Fis-6[hidden]@pos1, Fis-6[hidden]@pos1, Fis-6[hidden]@pos2, Fis-6[hidden]@pos2 |
| 15 | 56 | 0xA0 | 20 | 12 | 0,0,0 | Grahf@pos0, Executioner(Gear)@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 (17.8%) | 8 (17.8%) | 5 (11.1%) | 8 (17.8%) | 5 (11.1%) | 8 (17.8%) | 2 (4.4%) | 0 | 0 | 1 (2.2%) |

### Table 9 (used by 8 field(s))

Fields: 406 (Babel Tower - entrance ramp), 407 (Babel Tower - relay station), 409 (Babel Tower - handrail room), 412 (Babel Tower Ducts - inside from the mirror ledge), 413 (Babel Tower Ducts - long corridor 1), 414 (Babel Tower Ducts - long corridor 2), 415 (Babel Tower Ducts - short "conduit - stairs" corridor), 416 (Babel Tower Ducts - circular platform room)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 26 | 0x80 | 42 | 0 | 0,0,0 | Traffic Jam(Gear)@pos0, Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 1 | 26 | 0x80 | 67 | 0 | 0,0,0 | Traffic Jam(Gear)@pos0, Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 2 | 26 | 0x00 | 73 | 0 | 0,0,0 | Traffic Jam(Gear)@pos0, Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 3 | 26 | 0x00 | 73 | 0 | 0,0,0 | Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 4 | 37 | 0x00 | 73 | 0 | 0,0,0 | Conjurer(Gear)@pos1, Conjurer(Gear)@pos2 |
| 5 | 37 | 0x00 | 73 | 0 | 2,2,2 | Edin(Gear)@pos0 |
| 6 | 59 | 0x00 | 73 | 0 | 0,0,0 | Breaker(Gear)@pos0 |
| 7 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 8 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 54 | 0x80 | 73 | 0 | 0,0,0 | Bladegash(Gear)@pos1, Skyghene(Gear)@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 3 (37.5%) | 3 (37.5%) | 1 (12.5%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (12.5%) |

### Table 10 (used by 8 field(s))

Fields: 423 (Shevat map), 442 (Shevat Attacked - walking on clouds ? (Achtzehn battle, hangs)), 444 (Shevat Attacked - emergency shaft hallway 1 (start)), 445 (Shevat Attacked - emergency shaft elevator 1), 446 (Shevat Attacked - emergency shaft stairwell 1), 447 (Shevat Attacked - emergency shaft hallway 2), 448 (Shevat Attacked - emergency shaft elevator 2), 456 (Shevat Palace - bottom of Zephyr's tower)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 41 | 0x40 | 34 | 0 | 0,0,0 | Tears@pos0, Tears@pos0 |
| 1 | 41 | 0x40 | 34 | 0 | 0,0,0 | Tears@pos0, Tears@pos0, Tears@pos0 |
| 2 | 24 | 0x40 | 34 | 0 | 0,0,0 | Croaker Tribe[hidden]@pos0, Croaker Tribe[hidden]@pos0, Forbidden@pos0, Forbidden@pos0 |
| 3 | 41 | 0xC0 | 34 | 0 | 0,0,0 | Tears@pos0, Tears@pos0 |
| 4 | 41 | 0xC0 | 34 | 0 | 0,0,0 | Tears@pos0, Tears@pos0, Tears@pos0 |
| 5 | 24 | 0xC0 | 34 | 0 | 0,0,0 | Croaker Tribe[hidden]@pos0, Croaker Tribe[hidden]@pos0, Forbidden@pos0, Forbidden@pos0 |
| 6 | 41 | 0xC0 | 34 | 0 | 0,0,0 | Tears@pos0, Tears@pos0, Tears@pos0 |
| 7 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 8 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 33 | 0x80 | 45 | 0 | 0,0,0 | Littlefoot(Gear)@pos3, Littlefoot(Gear)@pos4, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos3, Littlefoot@pos3, Littlefoot@pos4, Littlefoot@pos4 |
| 11 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 12 | 35 | 0x80 | 45 | 0 | 0,0,0 | Citadel(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 13 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 14 | 42 | 0xA0 | 45 | 17 | 0,0,0 | Achtzehn(Gear)@pos0 |
| 15 | 42 | 0xA0 | 45 | 10 | 0,0,0 | Seibzehn(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (28.6%) | 2 (28.6%) | 2 (28.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (14.3%) |

### Table 11 (used by 8 field(s))

Fields: 474 (Occupied Nisan entrance), 478 (Mausoleum - main hall), 480 (Ft.Jasper - elevator 1 corridor), 483 (Ft.Jasper - atop(in gears)), 485 (Ft.Jasper - elevator 2 corridor), 486 (Ft.Jasper - L - shaped hallway to Omnigear hanger), 491 (Ft.Jasper - Omnigear bay), 494 (Gate Generator 1 - treasure chest area(hangs))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 4 | 0xC0 | 14 | 0 | 0,0,0 | Aveh Guard@pos0, Aveh Guard@pos0, Aveh Guard@pos0, Aveh Guard@pos0 |
| 1 | 25 | 0x40 | 32 | 0 | 0,0,0 | Margie@pos2, Margie@pos0, Margie@pos0, Margie@pos0, Margie@pos0 |
| 2 | 25 | 0x40 | 32 | 0 | 1,1,1 | Margie@pos2, Margie@pos1, Margie@pos1, Margie@pos1, Margie@pos1, GuardMachine@pos0, GuardMachine@pos0 |
| 3 | 25 | 0x40 | 32 | 0 | 0,0,0 | Margie@pos2, Shakhan Monk@pos0, Shakhan Monk@pos0, Shakhan Monk@pos0 |
| 4 | 25 | 0xC0 | 32 | 0 | 1,1,1 | Margie@pos2, Margie@pos1, Margie@pos1, Margie@pos1, Margie@pos1, GuardMachine@pos0, GuardMachine@pos0 |
| 5 | 61 | 0x80 | 74 | 0 | 0,0,0 | Twin Burner(Gear)@pos1, Twin Burner(Gear)@pos2 |
| 6 | 61 | 0x80 | 4 | 0 | 0,0,0 | Neo Etone(Gear)@pos1, Neo Etone(Gear)@pos2 |
| 7 | 61 | 0x80 | 4 | 0 | 0,0,0 | Twin Burner(Gear)@pos0, Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 8 | 61 | 0x80 | 4 | 0 | 0,0,0 | Neo Etone(Gear)@pos0, Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 33 | 0x80 | 45 | 0 | 0,0,0 | Littlefoot(Gear)@pos3, Littlefoot(Gear)@pos4, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos3, Littlefoot@pos3, Littlefoot@pos4, Littlefoot@pos4 |
| 11 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 12 | 35 | 0x80 | 45 | 0 | 0,0,0 | Citadel(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 13 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 14 | 54 | 0x80 | 72 | 0 | 0,0,0 | Bladegash@pos0, Marinebasher@pos1 |
| 15 | 54 | 0x80 | 72 | 0 | 0,0,0 | Bladegash(Gear)@pos0, Marinebasher(Gear)@pos1 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 2 (66.7%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (33.3%) |

### Table 12 (used by 7 field(s))

Fields: 64 (Stalactite Cave), 65 (Stalactite Cave - Aveh Excavation Site), 66 (Stalactite Cave - Outside Bal's Cave), 67 (Stalactite Cave - Passageway), 68 (Stalactite Cave - Passageway2), 70 (Bottom of elevator in Stalactite Cavern(just before Calamity)), 73 (Outside Stalactite Cave)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man(Gear)@pos1 |
| 1 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man(Gear)@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 2 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man(Gear)@pos1, Sand Man(Gear)@pos2 |
| 3 | 27 | 0x00 | 17 | 0 | 0,0,0 | Medusoid(Gear)@pos1, Medusoid(Gear)@pos2 |
| 4 | 21 | 0x00 | 17 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos1 |
| 5 | 27 | 0x00 | 17 | 0 | 0,0,0 | Medusoid(Gear)@pos0 |
| 6 | 27 | 0x00 | 17 | 0 | 0,0,0 | Medusoid(Gear)@pos0, Medusoid(Gear)@pos2 |
| 7 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 8 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 9 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 10 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 11 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 12 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 13 | 21 | 0x00 | 17 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos1 |
| 14 | 19 | 0xA3 | 24 | 6 | 0,0,0 | Calamity(Gear)@pos0 |
| 15 | 19 | 0xA3 | 24 | 6 | 0,0,0 | Calamity(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 4 (22.2%) | 4 (22.2%) | 6 (33.3%) | 2 (11.1%) | 1 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (5.6%) |

### Table 13 (used by 7 field(s))

Fields: 79 (Pirate's Lair - Yggdrasil Dock), 80 (Bart's Hideout - Yggdrasil docking), 82 (Bart's Hideout - Fei fights Vance during Gebler attack), 83 (Bart's Hideout - Fei, Citan and Bart fight the Schpariel boss), 84 (Bart's Hideout - Gear hanger), 245 (Nortune Purge - Fei encounters Elly's unit), 247 (Nortune Purge - Citan helps evacuate Nortune)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 14 | 0xB0 | 15 | 38 | 0,0,0 | Clawknight(Gear)@pos2 |
| 1 | 14 | 0xA0 | 15 | 40 | 0,0,0 | Swordknight(Gear)@pos2 |
| 2 | 14 | 0x80 | 15 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 3 | 14 | 0x80 | 15 | 0 | 0,0,0 | Aegisknight(Gear)@pos0, Aegisknight(Gear)@pos1 |
| 4 | 15 | 0x80 | 15 | 0 | 0,0,0 | Alkanshel(Gear)@pos2 |
| 5 | 14 | 0xA0 | 25 | 41 | 0,0,0 | Aegisknight(Gear)@pos1, Aegisknight(Gear)@pos2 |
| 6 | 14 | 0xA0 | 25 | 42 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 7 | 13 | 0xA0 | 25 | 7 | 0,0,0 | Vierge(Gear)@pos0 |
| 8 | 13 | 0xA0 | 41 | 8 | 0,0,0 | Vierge(Gear)@pos0 |
| 9 | 13 | 0xA0 | 40 | 9 | 0,0,0 | Vierge(Gear)@pos0 |
| 10 | 14 | 0x80 | 68 | 0 | 0,0,0 | Aegisknight R(Gear)@pos2, Aegisknight R(Gear)@pos1 |
| 11 | 14 | 0x80 | 68 | 0 | 0,0,0 | Wandknight(Gear)@pos2 |
| 12 | 14 | 0x80 | 68 | 0 | 0,0,0 | Clawknight R(Gear)@pos2 |
| 13 | 14 | 0x80 | 68 | 0 | 0,0,0 | Swordknight R(Gear)@pos2 |
| 14 | 13 | 0x80 | 40 | 0 | 0,0,0 | Vierge(Gear)@pos0 |
| 15 | 29 | 0xA0 | 40 | 11 | 0,0,0 | Hecht(Gear)@pos0, Hecht(Gear)[hidden]@pos1 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (100.0%) |

### Table 14 (used by 7 field(s))

Fields: 353 (Ethos HQ Purge - infirmary hallway), 355 (Ethos HQ Purge - hallway to 359), 356 (Ethos HQ Purge - prison cell 1), 357 (Ethos HQ Purge - diagonal elevator down at top), 358 (Ethos HQ Purge - diagonal elevator down at bottom), 359 (Ethos HQ Purge - Ethos = Solaris data room(hangs)), 360 (Ethos HQ Purge - Alkanshel Rises(Stone heads for dig site))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 17 | 0x40 | 37 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos1, Neo Wels@pos1, Neo Wels@pos1 |
| 1 | 10 | 0x40 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 2 | 10 | 0x40 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 3 | 10 | 0x40 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 4 | 10 | 0x40 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 5 | 10 | 0x40 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 6 | 17 | 0xC0 | 37 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 7 | 10 | 0xC0 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 8 | 10 | 0xC0 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 9 | 10 | 0xC0 | 37 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 67 | 0xC0 | 21 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 1 (16.7%) | 1 (16.7%) | 1 (16.7%) | 1 (16.7%) | 1 (16.7%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (16.7%) |

### Table 15 (used by 6 field(s))

Fields: 180 (Assault on Aveh - chasing Vanderkaum's flagship mini-game), 181 (Assault on Aveh - chasing Vanderkaum's flagship midpoint), 185 (Assault on Aveh - Dora breaks out of the flagship), 190 (Assault on Aveh - Id trashes Ramsus, attacks Bart), 192 (Assault on Aveh - Miang grabs Ramsus and flees), 193 (Assault on Aveh - same as 184 (? ))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 2 | 0x80 | 7 | 0 | 0,0,0 | Trooper(Gear)@pos0, Trooper(Gear)@pos1, Trooper(Gear)@pos2 |
| 1 | 43 | 0x80 | 7 | 0 | 0,0,0 | Sand Tripper(Gear)@pos1, Sand Tripper(Gear)@pos2 |
| 2 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot(Gear)@pos0, Gigafoot(Gear)[hidden]@pos0 |
| 3 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot(Gear)@pos0, Gigafoot(Gear)[hidden]@pos0, Trooper(Gear)@pos1, Trooper(Gear)@pos2 |
| 4 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot(Gear)@pos0 |
| 5 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot(Gear)@pos0 |
| 6 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot@pos0 |
| 7 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot@pos0 |
| 8 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot@pos0 |
| 9 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot@pos0 |
| 10 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot@pos0 |
| 11 | 2 | 0x80 | 7 | 0 | 0,0,0 | Gigafoot@pos0 |
| 12 | 68 | 0xA0 | 7 | 32 | 0,0,0 | ?(Gear)@pos0 |
| 13 | 68 | 0xA0 | 7 | 22 | 0,0,0 | ?(Gear)@pos1, def3(Gear)@pos2 |
| 14 | 46 | 0x80 | 27 | 0 | 0,0,0 | Main Gun(Gear)@pos0, Main Gun(Gear)@pos1, Main Gun(Gear)@pos2 |
| 15 | 19 | 0x90 | 7 | 0 | 0,0,0 | Calamity(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (100.0%) |

### Table 16 (used by 6 field(s))

Fields: 223 (Nortune, Battling Arena - Fei given Weltall for battling use), 224 (Nortune, Battling Arena - Fei rescues Rico beginning), 252 (Kislev Gear Dock - stockroom), 256 (Kislev Gear Dock - stairwell to hangar), 257 (Kislev Gear Dock - hall between factory and office), 258 (Kislev Gear Dock - other end of 257)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 50 | 0x40 | 10 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0 |
| 1 | 50 | 0x40 | 10 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 2 | 16 | 0x40 | 10 | 0 | 0,0,0 | Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 3 | 50 | 0xC0 | 10 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Swordsman@pos0, Gebler Guard@pos0 |
| 4 | 27 | 0x00 | 4 | 0 | 0,0,0 | Medusoid(Gear)@pos0 |
| 5 | 32 | 0x00 | 4 | 0 | 0,0,0 | Edelweiss(Gear)@pos0 |
| 6 | 15 | 0x80 | 9 | 0 | 2,2,2 | Alkanshel@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 9 | 49 | 0x40 | 19 | 0 | 0,0,0 | Rhino@pos0, Rhino@pos0, Rhino@pos0 |
| 10 | 16 | 0x40 | 10 | 0 | 0,0,0 | Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 11 | 0x80 | 12 | 0 | 0,0,0 | def4(Gear)@pos0 |
| 15 | 8 | 0xE0 | 10 | 37 | 0,0,0 | Wiseman@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (20.0%) | 2 (40.0%) | 1 (20.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (20.0%) |

### Table 17 (used by 6 field(s))

Fields: 363 (Ethos Dig Site - room between diagonal and vertical elevators), 382 (Ethos Dig Site - outside vertical elevator bottom), 383 (Ethos Dig Site - walkway over Zeboim(hangs)), 384 (Ethos Dig Site - other end of 377), 385 (Ethos Dig Site - hallway before 384), 387 (Ethos Dig Site - stairwell 2 bottom)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 6 | 0x40 | 23 | 0 | 0,0,0 | Shellbelle@pos0, Shellbelle@pos0 |
| 1 | 6 | 0x40 | 23 | 0 | 2,2,2 | Shellbelle@pos2, Shellbelle@pos2, Shellbelle@pos2, Shellbelle@pos2 |
| 2 | 6 | 0x40 | 23 | 0 | 1,1,1 | Shellbelle@pos0, Shellbelle@pos1, Shellbelle@pos1, Shellbelle@pos1 |
| 3 | 7 | 0x40 | 23 | 0 | 2,2,2 | Carrier@pos2, Carrier@pos2 |
| 4 | 30 | 0x40 | 23 | 0 | 1,1,1 | Ripper@pos1 |
| 5 | 50 | 0x40 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 6 | 50 | 0x40 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 7 | 50 | 0xC0 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 8 | 50 | 0x40 | 23 | 0 | 2,2,2 | Gebler Guard@pos2, Gebler Guard@pos2, Gebler Guard@pos2, Gebler Guard@pos2 |
| 9 | 50 | 0x40 | 23 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 10 | 50 | 0x40 | 23 | 0 | 1,1,1 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 11 | 4 | 0x40 | 23 | 0 | 0,0,0 | Aveh Corporal@pos0 |
| 12 | 4 | 0x40 | 23 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 13 | 4 | 0x40 | 23 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 14 | 57 | 0xC0 | 26 | 0 | 0,0,0 | Kelvina@pos0, Tolone@pos0 |
| 15 | 51 | 0xC0 | 39 | 0 | 0,0,0 | Id@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (13.3%) | 2 (13.3%) | 2 (13.3%) | 0 | 0 | 4 (26.7%) | 4 (26.7%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (6.7%) |

### Table 18 (used by 6 field(s))

Fields: 627 (Anima Relic 1 - Elements combine and attack), 629 (Anima Relic 1 - fuse room), 630 (Anima Relic 1 - door lock B control room), 632 (Anima Relic 1 - main hallway), 633 (Anima Relic 1 - secondary hallway), 634 (Anima Relic 1 - big round room)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 25 | 0x40 | 32 | 0 | 1,1,1 | Freelancer@pos1, Freelancer@pos1, Freelancer@pos1 |
| 1 | 25 | 0x40 | 32 | 0 | 0,0,0 | Freelancer@pos0, Freelancer@pos0 |
| 2 | 30 | 0x40 | 32 | 0 | 0,0,0 | Dorothy@pos0 |
| 3 | 25 | 0x40 | 32 | 0 | 1,1,1 | Freelancer@pos1, Freelancer@pos1, Freelancer@pos1, Freelancer@pos1 |
| 4 | 67 | 0x40 | 59 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 5 | 33 | 0x40 | 59 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 9 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 10 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 11 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 14 | 57 | 0xC0 | 32 | 0 | 0,0,0 | Dominia@pos0, Dominia@pos0, Tolone@pos0, Seraphita@pos0 |
| 15 | 55 | 0x80 | 62 | 0 | 0,0,0 | G Elements(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 (36.4%) | 4 (36.4%) | 2 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (9.1%) |

### Table 19 (used by 5 field(s))

Fields: 172 (Assault on Aveh - Fei at bottom of inside mountain area), 173 (Assault on Aveh - Fei at entrance to inside mountain area), 174 (Assault on Aveh - Fei attacked by Elly's unit), 175 (Assault on Aveh - Fei at bottom of outside mountain area), 177 (Assault on Aveh - Weltall beats Vierge, 'Drive' wears off)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 27 | 0x00 | 4 | 0 | 0,0,0 | Medusoid(Gear)@pos1, Medusoid(Gear)@pos0 |
| 1 | 27 | 0x00 | 4 | 0 | 0,0,0 | Medusoid(Gear)@pos0 |
| 2 | 32 | 0x00 | 4 | 0 | 0,0,0 | Edelweiss(Gear)@pos0 |
| 3 | 21 | 0x00 | 4 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos0 |
| 4 | 27 | 0x00 | 4 | 0 | 0,0,0 | Medusoid(Gear)@pos0 |
| 5 | 32 | 0x00 | 4 | 0 | 0,0,0 | Edelweiss(Gear)@pos0 |
| 6 | 15 | 0x80 | 9 | 0 | 2,2,2 | Alkanshel@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 9 | 12 | 0x40 | 19 | 0 | 0,0,0 | Armor Grub@pos0 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 12 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 13 | 14 | 0xA0 | 25 | 45 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 14 | 14 | 0xA0 | 25 | 46 | 0,0,0 | Aegisknight(Gear)@pos1, Aegisknight(Gear)@pos2 |
| 15 | 13 | 0xA0 | 25 | 7 | 0,0,0 | Vierge(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 (34.8%) | 8 (34.8%) | 4 (17.4%) | 2 (8.7%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (4.3%) |

### Table 20 (used by 5 field(s))

Fields: 597 (Krelian's Lab - corridor SE from 596), 598 (Krelian's Lab - corridor SW from 596), 599 (Krelian's Lab - useless corridor), 600 (Krelian's Lab - +-shaped corridor), 601 (Krelian's Lab - corridor after first landmark)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0 |
| 1 | 17 | 0x40 | 59 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 2 | 30 | 0x40 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 3 | 23 | 0x40 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 4 | 67 | 0x40 | 59 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 5 | 33 | 0x40 | 59 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 7 | 17 | 0xC0 | 59 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 8 | 30 | 0xC0 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 9 | 23 | 0xC0 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 10 | 67 | 0xC0 | 59 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 11 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 12 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 13 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 14 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 15 | 33 | 0xC0 | 48 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (9.5%) | 4 (19.0%) | 4 (19.0%) | 4 (19.0%) | 2 (9.5%) | 4 (19.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (4.8%) |

### Table 21 (used by 5 field(s))

Fields: 641 (Anima Relic 2 entrance), 642 (Anima Relic 2 - same as 641, except with pillar bridged gap), 644 (Anima Relic 2 - water chasm), 650 (Anima Relic 2 - after 651 (memory cube, Hammer ambush)), 651 (Anima Relic 2 - same as 644 minus water)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 71 | 0x00 | 4 | 0 | 0,0,0 | Dragon(Gear)@pos1, Dragon(Gear)@pos2 |
| 1 | 24 | 0x00 | 4 | 0 | 0,0,0 | Forbidden@pos0, Forbidden@pos0, Forbidden@pos1, Forbidden@pos1, Forbidden@pos2, Forbidden@pos2 |
| 2 | 66 | 0x00 | 4 | 0 | 0,0,0 | Griffon(Gear)@pos0 |
| 3 | 23 | 0x40 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 4 | 67 | 0x40 | 59 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 5 | 33 | 0x40 | 59 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 7 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 8 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 9 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 10 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 11 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 15 | 55 | 0x80 | 4 | 0 | 0,0,0 | G Elements(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 5 (27.8%) | 10 (55.6%) | 2 (11.1%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (5.6%) |

### Table 22 (used by 5 field(s))

Fields: 666 (Point of Sephirot - end of 662 cutscene, Weltall->Xenogears), 710 (Merkava - Ramsus waiting for Fei in Merkava.), 714 (Merkava - just after Elly shoots Fei.), 717 (Merkava - The inner chamber, about to fight Deus.), 719 (Ending - Talking to Krelian before fighting Urobolus.)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 70 | 0xA0 | 63 | 24 | 0,0,0 | Id(Gear)@pos0, Id(Gear)[hidden]@pos3 |
| 1 | 70 | 0xA0 | 63 | 24 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos3 |
| 2 | 5 | 0x00 | 63 | 0 | 0,0,0 | Sand Man(Gear)@pos0 |
| 3 | 13 | 0xA0 | 63 | 26 | 0,0,0 | Alpha Weltall(Gear)@pos0 |
| 4 | 20 | 0x80 | 56 | 0 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 5 | 20 | 0xA0 | 60 | 41 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 6 | 20 | 0xA0 | 60 | 42 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 75 | 0xA0 | 66 | 23 | 0,0,0 | Urobolus(Gear)@pos0 |
| 9 | 62 | 0x80 | 64 | 0 | 0,0,0 | Metatron(Gear)@pos0 |
| 10 | 62 | 0x80 | 64 | 0 | 0,0,0 | Metatron(Gear)@pos0 |
| 11 | 62 | 0x80 | 64 | 0 | 0,0,0 | Sundel(Gear)@pos0 |
| 12 | 62 | 0x80 | 64 | 0 | 0,0,0 | Harlute(Gear)@pos0, Marlute(Gear)[hidden]@pos0 |
| 13 | 63 | 0x88 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 14 | 63 | 0x88 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 15 | 63 | 0x88 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (100.0%) |

### Table 23 (used by 3 field(s))

Fields: 2 (Lahan battle(intro version)), 41 (Lahan - Id in Weltall(before Fei gets in Weltall)), 490 (Title Screen)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 9 | 0xA0 | 18 | 0 | 0,0,0 | Alpha Weltall(Gear)@pos0, Musha Mk100(Gear)@pos1, Musha Mk100(Gear)@pos2, Musha Mk100(Gear)@pos3, Musha Mk100(Gear)@pos4 |
| 1 | 9 | 0xA0 | 18 | 1 | 0,0,0 | Alpha Weltall(Gear)@pos0, Musha Mk100(Gear)@pos1, Musha Mk100(Gear)@pos2, Musha Mk100(Gear)@pos3, Musha Mk100(Gear)@pos4 |
| 2 | 9 | 0xA0 | 18 | 2 | 0,0,0 | Alpha Weltall(Gear)[hidden]@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)[hidden]@pos3, Alpha Weltall(Gear)[hidden]@pos4 |
| 3 | 9 | 0xA3 | 18 | 3 | 0,0,0 | Alpha Weltall(Gear)@pos0, Musha Mk100(Gear)@pos1, Musha Mk100(Gear)@pos2, Musha Mk100(Gear)@pos3, Musha Mk100(Gear)@pos4 |
| 4 | 11 | 0xA3 | 6 | 4 | 0,1,1 | Rankar Dragon(Gear)@pos0, Rankar Dragon(Gear)[hidden]@pos1, def2(Gear)[hidden]@pos1, def3@pos1 |
| 5 | 31 | 0xA3 | 7 | 5 | 0,0,0 | Brigandier(Gear)@pos0 |
| 6 | 19 | 0xA3 | 24 | 6 | 0,0,0 | Calamity(Gear)@pos0 |
| 7 | 13 | 0xA3 | 25 | 7 | 0,0,0 | Vierge(Gear)@pos0 |
| 8 | 13 | 0xA3 | 41 | 8 | 0,0,0 | Vierge(Gear)@pos0 |
| 9 | 8 | 0xE0 | 8 | 9 | 0,0,0 | Dan@pos0 |
| 10 | 42 | 0xA3 | 45 | 10 | 0,0,0 | Seibzehn(Gear)@pos0 |
| 11 | 29 | 0xA3 | 40 | 11 | 0,0,0 | Hecht(Gear)@pos0 |
| 12 | 56 | 0xA0 | 20 | 12 | 0,0,0 | Grahf@pos0, Executioner(Gear)@pos2 |
| 13 | 74 | 0x20 | 41 | 13 | 0,0,0 | Heal Seraph(Gear)@pos0, Heal Seraph(Gear)@pos1, Heal Seraph(Gear)@pos2, Heal Seraph(Gear)@pos3, Heal Seraph(Gear)@pos4, Sword Seraph(Gear)@pos7 |
| 14 | 74 | 0x20 | 41 | 14 | 0,0,0 | Sword Seraph(Gear)@pos7, Fire Seraph(Gear)@pos0, Fire Seraph(Gear)@pos3, Fire Seraph(Gear)@pos4 |
| 15 | 74 | 0xA0 | 53 | 15 | 0,0,0 | Sword Seraph(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (100.0%) |

### Table 24 (used by 3 field(s))

Fields: 47 (Duneman Isle - Bottom of Sandfall Cliffs), 48 (Duneman Desert), 51 (Duneman Isle - Dino Skeleton)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x40 | 7 | 0 | 0,0,0 | Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0 |
| 1 | 5 | 0x40 | 7 | 0 | 0,0,0 | Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0, Dune Man(Gear)@pos0 |
| 2 | 39 | 0x40 | 7 | 0 | 0,0,0 | Death Scythe(Gear)@pos0 |
| 3 | 12 | 0x40 | 7 | 0 | 0,0,0 | Rain Frog@pos0, Rain Frog@pos0 |
| 4 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 5 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 6 | 71 | 0x40 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 7 | 71 | 0x40 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 8 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 9 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 10 | 71 | 0x40 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 11 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 39 | 0xC0 | 7 | 0 | 0,0,0 | Death Scythe(Gear)@pos0 |
| 15 | 5 | 0xC0 | 7 | 0 | 0,0,0 | Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 6 (33.3%) | 2 (11.1%) | 2 (11.1%) | 2 (11.1%) | 1 (5.6%) | 1 (5.6%) | 0 | 1 (5.6%) | 1 (5.6%) | 1 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 1 (5.6%) |

### Table 25 (used by 3 field(s))

Fields: 334 (Ethos Transport - battle with Bloody boss), 336 (Ethos Transport - room after hallway), 338 (Ethos Transport entrance)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 17 | 0x40 | 21 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 1 | 23 | 0x40 | 21 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Slugger@pos0 |
| 2 | 23 | 0x40 | 22 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Abandon@pos0 |
| 3 | 6 | 0x40 | 71 | 0 | 0,0,0 | Shellbelle@pos0 |
| 4 | 17 | 0xC0 | 21 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 5 | 23 | 0xC0 | 21 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Slugger@pos0 |
| 6 | 15 | 0xC0 | 22 | 0 | 2,2,2 | Alkanshel@pos0, Alkanshel@pos0 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 9 | 12 | 0x40 | 19 | 0 | 0,0,0 | Armor Grub@pos0 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 67 | 0xC0 | 21 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (33.3%) |

### Table 26 (used by 3 field(s))

Fields: 496 (Sargasso Point), 497 (Sargasso Point), 498 (Sargasso Point - before first Y)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 27 | 0x00 | 28 | 0 | 0,0,0 | May Fly(Gear)@pos1, May Fly(Gear)@pos2 |
| 1 | 39 | 0x00 | 28 | 0 | 0,0,0 | Wyrm(Gear)@pos1 |
| 2 | 60 | 0x00 | 28 | 0 | 0,0,0 | Aragonite(Gear)@pos1, Aragonite(Gear)@pos2 |
| 3 | 39 | 0x00 | 28 | 0 | 0,0,0 | Wyrm(Gear)@pos0 |
| 4 | 60 | 0x00 | 28 | 0 | 0,0,0 | Aragonite(Gear)@pos0, Aragonite(Gear)@pos1, Aragonite(Gear)@pos2 |
| 5 | 60 | 0x00 | 28 | 0 | 0,0,0 | Aragonite(Gear)@pos0, Aragonite(Gear)@pos1 |
| 6 | 21 | 0x00 | 28 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos2 |
| 7 | 61 | 0x80 | 4 | 0 | 0,0,0 | Twin Burner(Gear)@pos0, Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 8 | 61 | 0x80 | 4 | 0 | 0,0,0 | Neo Etone(Gear)@pos0, Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 33 | 0x80 | 45 | 0 | 0,0,0 | Littlefoot(Gear)@pos3, Littlefoot(Gear)@pos4, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos3, Littlefoot@pos3, Littlefoot@pos4, Littlefoot@pos4 |
| 11 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 12 | 35 | 0x80 | 45 | 0 | 0,0,0 | Citadel(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 13 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 14 | 42 | 0x80 | 45 | 0 | 0,0,0 | Achtzehn(Gear)@pos1, Achtzehn(Gear)@pos2 |
| 15 | 42 | 0x80 | 45 | 0 | 0,0,0 | Seibzehn@pos1, Achtzehn@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 (30.8%) | 0 | 4 (30.8%) | 3 (23.1%) | 0 | 0 | 1 (7.7%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (7.7%) |

### Table 27 (used by 3 field(s))

Fields: 499 (Sargasso Point), 500 (Sargasso Point - at strong current Y), 503 (Sargasso Point - strong current off)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 27 | 0x00 | 28 | 0 | 0,0,0 | May Fly(Gear)@pos1, May Fly(Gear)@pos2 |
| 1 | 39 | 0x00 | 28 | 0 | 0,0,0 | Wyrm(Gear)@pos0 |
| 2 | 60 | 0x00 | 28 | 0 | 0,0,0 | Aragonite(Gear)@pos1, Aragonite(Gear)@pos2 |
| 3 | 39 | 0x00 | 28 | 0 | 0,0,0 | Wyrm(Gear)@pos0 |
| 4 | 60 | 0x00 | 28 | 0 | 0,0,0 | Aragonite(Gear)@pos0, Aragonite(Gear)@pos1, Aragonite(Gear)@pos2 |
| 5 | 60 | 0x00 | 28 | 0 | 0,0,0 | Aragonite(Gear)@pos0, Aragonite(Gear)@pos1 |
| 6 | 21 | 0x00 | 28 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos2 |
| 7 | 61 | 0x80 | 4 | 0 | 0,0,0 | Twin Burner(Gear)@pos0, Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 8 | 61 | 0x80 | 4 | 0 | 0,0,0 | Neo Etone(Gear)@pos0, Etone(Gear)@pos1, Etone(Gear)@pos2 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 33 | 0x80 | 45 | 0 | 0,0,0 | Littlefoot(Gear)@pos3, Littlefoot(Gear)@pos4, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos3, Littlefoot@pos3, Littlefoot@pos4, Littlefoot@pos4 |
| 11 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 12 | 35 | 0x80 | 45 | 0 | 0,0,0 | Citadel(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 13 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 14 | 42 | 0x80 | 45 | 0 | 0,0,0 | Achtzehn(Gear)@pos1, Achtzehn(Gear)@pos2 |
| 15 | 42 | 0x80 | 45 | 0 | 0,0,0 | Seibzehn@pos1, Achtzehn@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 4 (22.2%) | 6 (33.3%) | 6 (33.3%) | 1 (5.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (5.6%) |

### Table 28 (used by 2 field(s))

Fields: 15 (Outside Lahan - Mountain path), 93 (Nisan entrance)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 1 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 2 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos1, Jackal@pos1 |
| 3 | 1 | 0x40 | 3 | 0 | 1,1,1 | Jackal@pos1 |
| 4 | 1 | 0x40 | 3 | 0 | 1,1,1 | Jackal@pos1, Jackal@pos1 |
| 5 | 1 | 0x40 | 3 | 0 | 1,1,1 | Jackal@pos1, Jackal@pos0/f, Jackal@pos0/f |
| 6 | 1 | 0x40 | 3 | 0 | 1,1,1 | Jackal@pos0, Jackal@pos0/f, Jackal@pos0/f |
| 7 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos1, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 8 | 3 | 0x40 | 3 | 0 | 1,1,1 | Gonzalez@pos1 |
| 9 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 10 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 11 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 12 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 13 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 14 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |
| 15 | 1 | 0x40 | 3 | 0 | 0,0,0 | Jackal@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (2.9%) | 4 (11.8%) | 1 (2.9%) | 1 (2.9%) | 4 (11.8%) | 8 (23.5%) | 8 (23.5%) | 6 (17.6%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (2.9%) |

### Table 29 (used by 2 field(s))

Fields: 23 (Blackmoon Forest - Fei & Elly wake up by tree), 24 (Blackmoon Forest - The 'Rankar' opening.T)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x40 | 1 | 0 | 1,1,1 | Armor Grub@pos1, Armor Grub@pos1, Armor Grub@pos1, Armor Grub@pos1 |
| 1 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 2 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 3 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 4 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos1, Jackal@pos1, Jackal@pos1, Jackal@pos1 |
| 5 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 6 | 12 | 0x40 | 1 | 0 | 0,0,0 | Armor Grub@pos0 |
| 7 | 12 | 0x40 | 1 | 0 | 0,0,0 | Armor Grub@pos0, Armor Grub@pos0, Armor Grub@pos0 |
| 8 | 3 | 0x40 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1 |
| 9 | 3 | 0x40 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1 |
| 10 | 3 | 0x40 | 1 | 0 | 0,0,1 | Forest Elf@pos0, Forest Elf@pos0, Forest Elf@pos0, Forest Elf@pos1 |
| 11 | 3 | 0x40 | 1 | 0 | 0,0,0 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1 |
| 12 | 3 | 0x40 | 1 | 0 | 0,0,0 | Forest Elf@pos0, Forest Elf@pos0, Forest Elf@pos1, Forest Elf@pos1 |
| 13 | 3 | 0x40 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos2, Forest Elf@pos2 |
| 14 | 3 | 0xC0 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos2, Forest Elf@pos2 |
| 15 | 11 | 0xE0 | 6 | 4 | 0,0,0 | Rankar Dragon(Gear)@pos0, Rankar Dragon(Gear)[hidden]@pos1, def2(Gear)[hidden]@pos1, def3@pos1 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 (7.5%) | 0 | 0 | 0 | 0 | 0 | 0 | 4 (7.5%) | 8 (15.1%) | 8 (15.1%) | 8 (15.1%) | 8 (15.1%) | 6 (11.3%) | 6 (11.3%) | 0 | 0 | 1 (1.9%) |

### Table 30 (used by 2 field(s))

Fields: 57 (Aveh Transport(Near exit)), 58 (Aveh Transport - Second room)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Soldier[hidden]@pos1 |
| 1 | 4 | 0x80 | 0 | 0 | 2,2,2 | def3@pos2, def3@pos2, def3@pos2, def3@pos2, Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Soldier[hidden]@pos0 |
| 2 | 4 | 0x80 | 0 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Soldier[hidden]@pos1 |
| 3 | 4 | 0x80 | 0 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Soldier[hidden]@pos2 |
| 4 | 4 | 0x80 | 0 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1 |
| 5 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos1, Aveh Corporal@pos1 |
| 6 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1 |
| 7 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 8 | 4 | 0x00 | 0 | 0 | 2,2,2 | Aveh Corporal@pos2, Aveh Corporal@pos2, Aveh Corporal@pos2, Aveh Corporal@pos2 |
| 9 | 4 | 0x00 | 0 | 0 | 2,2,2 | Aveh Corporal@pos2, Aveh Corporal@pos2, Aveh Corporal@pos2, Aveh Corporal@pos2 |
| 10 | 4 | 0x00 | 0 | 0 | 3,3,3 | Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos4, Aveh Corporal@pos4, Aveh Corporal@pos4 |
| 11 | 4 | 0x00 | 0 | 0 | 3,3,3 | Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos4, Aveh Corporal@pos4 |
| 12 | 4 | 0x00 | 0 | 0 | 3,3,3 | Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos4, Aveh Corporal@pos4, Aveh Corporal@pos4 |
| 13 | 4 | 0x00 | 0 | 0 | 3,3,3 | Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos3, Aveh Corporal@pos4, Aveh Corporal@pos4, Aveh Corporal@pos4 |
| 14 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Soldier@pos2, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1 |
| 15 | 4 | 0x00 | 0 | 0 | 1,1,1 | Aveh Soldier@pos2, Aveh Soldier@pos2, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos1 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (50.0%) |

### Table 31 (used by 2 field(s))

Fields: 418 (Babel Tower - floating platform(enemies) area), 419 (Babel Tower - same as 418)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 26 | 0x80 | 67 | 0 | 0,0,0 | Traffic Jam(Gear)@pos0, Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 1 | 26 | 0x80 | 67 | 0 | 0,0,0 | Traffic Jam(Gear)@pos0, Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 2 | 26 | 0x80 | 67 | 0 | 0,0,0 | Traffic Jam(Gear)@pos0, Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 3 | 26 | 0x80 | 67 | 0 | 0,0,0 | Traffic Jam(Gear)@pos1, Traffic Jam(Gear)@pos2 |
| 4 | 37 | 0x80 | 67 | 0 | 0,0,0 | Conjurer(Gear)@pos1, Conjurer(Gear)@pos2 |
| 5 | 37 | 0x80 | 67 | 0 | 2,2,2 | Edin(Gear)@pos0 |
| 6 | 59 | 0x00 | 67 | 0 | 0,0,0 | Breaker(Gear)@pos0 |
| 7 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 8 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 54 | 0x80 | 73 | 0 | 0,0,0 | Bladegash(Gear)@pos1, Skyghene(Gear)@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 2 (13.3%) | 2 (13.3%) | 5 (33.3%) | 5 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (6.7%) |

### Table 32 (used by 2 field(s))

Fields: 449 (Shevat Attacked - emergency shaft elevator 3), 450 (Shevat Attacked - outdoor emergency shaft(stuck near savepoint))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 41 | 0x40 | 35 | 0 | 0,0,0 | Tears@pos0, Tears@pos0 |
| 1 | 41 | 0x40 | 35 | 0 | 0,0,0 | Tears@pos0, Tears@pos0, Tears@pos0 |
| 2 | 24 | 0x40 | 35 | 0 | 0,0,0 | Croaker Tribe[hidden]@pos0, Croaker Tribe[hidden]@pos0, Forbidden@pos0, Forbidden@pos0 |
| 3 | 41 | 0xC0 | 35 | 0 | 0,0,0 | Tears@pos0, Tears@pos0 |
| 4 | 41 | 0xC0 | 35 | 0 | 0,0,0 | Tears@pos0, Tears@pos0, Tears@pos0 |
| 5 | 24 | 0xC0 | 35 | 0 | 0,0,0 | Croaker Tribe[hidden]@pos0, Croaker Tribe[hidden]@pos0, Forbidden@pos0, Forbidden@pos0 |
| 6 | 41 | 0x40 | 35 | 0 | 0,0,0 | Tears@pos0, Tears@pos0, Tears@pos0 |
| 7 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 8 | 10 | 0xC0 | 50 | 0 | 0,0,0 | Assassin@pos0, Assassin@pos0, Assassin@pos0 |
| 9 | 10 | 0xC0 | 50 | 0 | 2,2,2 | Assassin@pos2, Assassin@pos2, Assassin@pos2, Assassin@pos2 |
| 10 | 33 | 0x80 | 45 | 0 | 0,0,0 | Littlefoot(Gear)@pos3, Littlefoot(Gear)@pos4, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos3, Littlefoot@pos3, Littlefoot@pos4, Littlefoot@pos4 |
| 11 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 12 | 35 | 0x80 | 45 | 0 | 0,0,0 | Citadel(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 13 | 35 | 0x80 | 45 | 0 | 0,0,0 | White Knight(Gear)@pos0, White Knight(Gear)@pos1, White Knight(Gear)@pos2 |
| 14 | 42 | 0xA0 | 45 | 17 | 0,0,0 | Achtzehn(Gear)@pos1, Achtzehn(Gear)@pos2 |
| 15 | 42 | 0xA0 | 45 | 10 | 0,0,0 | Seibzehn(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (22.2%) | 2 (22.2%) | 2 (22.2%) | 0 | 0 | 0 | 2 (22.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (11.1%) |

### Table 33 (used by 2 field(s))

Fields: 704 (Merkava - ledge over 1st laser tunnel to 706), 705 (Merkava - 2nd laser tunnel leading to 709)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 9 | 0xA3 | 18 | 1 | 0,0,0 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)@pos3, Alpha Weltall(Gear)@pos4 |
| 1 | 9 | 0x00 | 16 | 0 | 0,0,0 | Alpha Weltall(Gear)@pos1, Musha Mk100(Gear)@pos2 |
| 2 | 8 | 0x00 | 20 | 0 | 0,0,0 | Dan@pos0 |
| 3 | 1 | 0x00 | 3 | 0 | 128,128,128 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 4 | 3 | 0x00 | 9 | 0 | 1,1,1 | Dive Bomber@pos1, Dive Bomber@pos1, Dive Bomber@pos1, Dive Bomber@pos1, Dive Bomber@pos2, Dive Bomber@pos2, Dive Bomber@pos2, Dive Bomber@pos2 |
| 5 | 8 | 0x00 | 14 | 0 | 0,0,0 | Dan@pos0, Dan@pos0, Dan@pos0, Dan@pos0 |
| 6 | 1 | 0x00 | 6 | 0 | 0,0,0 | Hobgob@pos0, Lucre Bug[hidden]@pos0 |
| 7 | 7 | 0x00 | 10 | 0 | 0,0,0 | Carrier@pos0 |
| 8 | 4 | 0x00 | 0 | 0 | 0,0,0 | Aveh Corporal@pos1, Aveh Corporal@pos1, Aveh Corporal@pos0, Aveh Corporal@pos0, Aveh Corporal@pos2, Aveh Corporal@pos2 |
| 9 | 2 | 0x00 | 11 | 0 | 2,2,2 | Gigafoot(Gear)@pos0, Gigafoot(Gear)@pos1, Gigafoot(Gear)@pos2 |
| 10 | 3 | 0x00 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos2, Forest Elf@pos2, Forest Elf@pos2, Forest Elf@pos2 |
| 11 | 7 | 0x00 | 2 | 0 | 0,0,0 | Carrier@pos0, Carrier@pos0, Carrier@pos0, Shadey@pos0 |
| 12 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 13 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 14 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 15 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 34 (used by 2 field(s))

Fields: 707 (Merkava - tunnels north of puzzle room), 708 (Merkava - tunnels west of puzzle room)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 34 | 0x00 | 57 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos1, Eagle Gunner(Gear)@pos2 |
| 1 | 34 | 0x00 | 57 | 0 | 0,0,0 | Eagle Armor(Gear)@pos1, Eagle Armor(Gear)@pos2 |
| 2 | 34 | 0x00 | 57 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos0, Eagle Gunner(Gear)[hidden]@pos0 |
| 3 | 34 | 0x00 | 57 | 0 | 0,0,0 | Eagle Armor(Gear)@pos0, Eagle Gunner(Gear)[hidden]@pos0 |
| 4 | 37 | 0x00 | 57 | 0 | 0,0,0 | Conjurer(Gear)@pos0 |
| 5 | 33 | 0x00 | 57 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 37 | 0x00 | 57 | 0 | 0,0,0 | Conjurer(Gear)@pos1, Conjurer(Gear)@pos2 |
| 7 | 72 | 0x00 | 57 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 8 | 73 | 0x00 | 57 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 9 | 73 | 0x00 | 57 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 10 | 73 | 0x00 | 57 | 0 | 0,0,0 | Sword Seraph(Gear)@pos0 |
| 11 | 73 | 0x00 | 57 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 12 | 73 | 0x00 | 57 | 0 | 0,0,0 | Heal Seraph(Gear)@pos1, Heal Seraph(Gear)@pos2 |
| 13 | 72 | 0x00 | 57 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 72 | 0x00 | 57 | 0 | 0,0,0 | Wind Seraph(Gear)@pos1, Wind Seraph(Gear)@pos2 |
| 15 | 72 | 0x00 | 57 | 0 | 0,0,0 | Earth Seraph(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 (6.2%) | 1 (3.1%) | 8 (25.0%) | 8 (25.0%) | 6 (18.8%) | 6 (18.8%) | 1 (3.1%) |

### Table 35 (used by 2 field(s))

Fields: 713 (Merkava - seconds before fighting Opiomorph.), 718 (Ending - Fei persuing the destructing Deus.)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 70 | 0xA0 | 63 | 24 | 0,0,0 | Id(Gear)@pos0, Id(Gear)[hidden]@pos3 |
| 1 | 70 | 0xA0 | 63 | 24 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos3 |
| 2 | 5 | 0x00 | 63 | 0 | 0,0,0 | Sand Man(Gear)@pos0 |
| 3 | 13 | 0xA0 | 63 | 26 | 0,0,0 | Alpha Weltall(Gear)@pos0 |
| 4 | 20 | 0x80 | 56 | 0 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 5 | 20 | 0xA0 | 60 | 41 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 6 | 20 | 0xA0 | 60 | 42 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 75 | 0x80 | 66 | 0 | 0,0,0 | Urobolus(Gear)@pos0 |
| 9 | 62 | 0x80 | 64 | 0 | 0,0,0 | Metatron(Gear)@pos0 |
| 10 | 62 | 0x80 | 64 | 0 | 0,0,0 | Metatron(Gear)@pos0 |
| 11 | 62 | 0x80 | 64 | 0 | 0,0,0 | Sundel(Gear)@pos0 |
| 12 | 62 | 0x80 | 64 | 0 | 0,0,0 | Harlute(Gear)@pos0, Marlute(Gear)[hidden]@pos0 |
| 13 | 63 | 0x88 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 14 | 63 | 0x88 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 15 | 63 | 0x88 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (100.0%) |

### Table 36 (used by 1 field(s))

Fields: 3 (Outside Lahan - after the battle)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 57 | 0x00 | 32 | 0 | 0,0,0 | Dominia@pos0, Dominia@pos0, Tolone@pos0, Seraphita@pos0 |
| 1 | 17 | 0x40 | 22 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 2 | 17 | 0x40 | 37 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos1, Neo Wels@pos1, Neo Wels@pos1 |
| 3 | 63 | 0x00 | 6 | 0 | 128,128,128 | Deus(Gear)@pos0 |
| 4 | 24 | 0x00 | 47 | 0 | 0,0,0 | Forbidden[hidden]@pos0, Forbidden[hidden]@pos0, Forbidden@pos0, Forbidden@pos0 |
| 5 | 55 | 0x00 | 62 | 0 | 0,0,0 | G Elements(Gear)@pos0 |
| 6 | 17 | 0x40 | 71 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 7 | 59 | 0x00 | 16 | 0 | 0,0,0 | Rapid Fire(Gear)@pos0, Rapid Fire(Gear)@pos1, Rapid Fire(Gear)@pos2 |
| 8 | 16 | 0x00 | 47 | 0 | 0,0,0 | HarquebusMk10(Gear)@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos0, Hatamoto Mk3@pos1, Hatamoto Mk3@pos1, Hatamoto Mk3@pos1 |
| 9 | 3 | 0x40 | 6 | 0 | 1,1,1 | Dwarf@pos1, Dwarf@pos1, Vargas@pos1, Vargas@pos1 |
| 10 | 54 | 0x00 | 72 | 0 | 0,0,0 | Marinebasher(Gear)@pos0 |
| 11 | 23 | 0x00 | 2 | 0 | 0,0,0 | Rotten Sod@pos0, Rotten Sod@pos0 |
| 12 | 17 | 0x00 | 7 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0 |
| 13 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 14 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 15 | 2 | 0x00 | 7 | 0 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 37 (used by 1 field(s))

Fields: 22 (Blackmoon Forest Entrance)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x40 | 1 | 0 | 1,1,1 | Jackal@pos1, Jackal@pos1, Jackal@pos1, Jackal@pos1 |
| 1 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 2 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 3 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 4 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos1, Jackal@pos1, Jackal@pos1, Jackal@pos1 |
| 5 | 1 | 0x40 | 1 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos1, Jackal@pos1 |
| 6 | 12 | 0x40 | 1 | 0 | 0,0,0 | Armor Grub@pos0 |
| 7 | 12 | 0x40 | 1 | 0 | 0,0,0 | Armor Grub@pos0, Armor Grub@pos0 |
| 8 | 3 | 0xC0 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1 |
| 9 | 1 | 0xC0 | 1 | 0 | 1,1,1 | Armor Grub@pos1, Armor Grub@pos1, Armor Grub@pos1, Armor Grub@pos1 |
| 10 | 3 | 0x40 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1 |
| 11 | 3 | 0x40 | 1 | 0 | 0,0,0 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos1 |
| 12 | 3 | 0x40 | 1 | 0 | 0,0,0 | Forest Elf@pos0, Forest Elf@pos0, Forest Elf@pos1, Forest Elf@pos1 |
| 13 | 3 | 0x40 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos2 |
| 14 | 3 | 0x40 | 1 | 0 | 1,1,1 | Forest Elf@pos1, Forest Elf@pos1, Forest Elf@pos2, Forest Elf@pos2 |
| 15 | 11 | 0xE0 | 6 | 4 | 0,0,0 | Rankar Dragon(Gear)@pos0, Rankar Dragon(Gear)[hidden]@pos1, def2(Gear)[hidden]@pos1, def3@pos1 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (9.5%) | 2 (9.5%) | 2 (9.5%) | 2 (9.5%) | 2 (9.5%) | 2 (9.5%) | 4 (19.0%) | 4 (19.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (4.8%) |

### Table 38 (used by 1 field(s))

Fields: 52 (Duneman Isle - Dino Skeleton(Camera zooms around it))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x40 | 7 | 0 | 0,0,0 | Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0 |
| 1 | 5 | 0x40 | 7 | 0 | 0,0,0 | Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0, Dune Man(Gear)@pos0 |
| 2 | 39 | 0x40 | 7 | 0 | 0,0,0 | Death Scythe(Gear)@pos0 |
| 3 | 12 | 0x40 | 7 | 0 | 0,0,0 | Rain Frog@pos0, Rain Frog@pos0 |
| 4 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 5 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 6 | 71 | 0x40 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 7 | 71 | 0x40 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 8 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 9 | 71 | 0x40 | 7 | 0 | 0,0,0 | Dragon(Gear)@pos0 |
| 10 | 71 | 0x40 | 7 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 11 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x00 | 7 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0, Jackal@pos0, Jackal@pos0 |
| 15 | 5 | 0xC0 | 7 | 0 | 0,0,0 | Tin Robo@pos0, Tin Robo@pos0, Tin Robo@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 16 (20.0%) | 16 (20.0%) | 16 (20.0%) | 16 (20.0%) | 1 (1.2%) | 1 (1.2%) | 0 | 1 (1.2%) | 1 (1.2%) | 1 (1.2%) | 10 (12.5%) | 0 | 0 | 0 | 0 | 0 | 1 (1.2%) |

### Table 39 (used by 1 field(s))

Fields: 53 (Aveh Desert Entrance)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x40 | 7 | 0 | 0,0,0 | Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 1 | 5 | 0x40 | 7 | 0 | 0,0,0 | Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 2 | 12 | 0x40 | 7 | 0 | 0,0,0 | Acid Frog@pos0, Acid Frog@pos0 |
| 3 | 4 | 0x40 | 7 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 4 | 4 | 0x40 | 7 | 0 | 0,0,0 | Aveh Corporal@pos0, Aveh Corporal@pos0 |
| 5 | 2 | 0x80 | 16 | 0 | 0,0,0 | Gigafoot(Gear)@pos0 |
| 6 | 15 | 0x80 | 9 | 0 | 2,2,2 | Alkanshel@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 9 | 12 | 0x40 | 19 | 0 | 0,0,0 | Armor Grub@pos0 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Swordknight(Gear)@pos0 |
| 15 | 25 | 0xA3 | 25 | 7 | 0,0,0 | Margie(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 3 (27.3%) | 1 (9.1%) | 4 (36.4%) | 2 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (9.1%) |

### Table 40 (used by 1 field(s))

Fields: 69 (Stalactite Cave - Excavation Site)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man(Gear)@pos1 |
| 1 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man(Gear)@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0, Sand Man@pos0 |
| 2 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man(Gear)@pos1, Sand Man(Gear)@pos2 |
| 3 | 27 | 0x00 | 17 | 0 | 0,0,0 | Medusoid(Gear)@pos1, Medusoid(Gear)@pos2 |
| 4 | 21 | 0x00 | 17 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos1 |
| 5 | 27 | 0x00 | 17 | 0 | 0,0,0 | Medusoid(Gear)@pos0 |
| 6 | 27 | 0x00 | 17 | 0 | 0,0,0 | Medusoid(Gear)@pos0, Medusoid(Gear)@pos2 |
| 7 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 8 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 9 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 10 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 11 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 12 | 5 | 0x00 | 17 | 0 | 0,0,0 | Sand Man@pos0 |
| 13 | 21 | 0x00 | 17 | 0 | 0,0,0 | Nomad Fix Bot(Gear)@pos1 |
| 14 | 19 | 0xA3 | 24 | 6 | 0,0,0 | Calamity(Gear)@pos0 |
| 15 | 19 | 0xA0 | 24 | 6 | 0,0,0 | Calamity(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (6.2%) | 0 | 3 (18.8%) | 2 (12.5%) | 1 (6.2%) | 8 (50.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (6.2%) |

### Table 41 (used by 1 field(s))

Fields: 239 (Kislev sewers entrance)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x40 | 9 | 0 | 1,1,1 | Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos2, Nolucre Bug@pos2 |
| 1 | 1 | 0x40 | 9 | 0 | 1,1,1 | Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos1 |
| 2 | 12 | 0x40 | 9 | 0 | 2,2,2 | Armor Wasp@pos2, Armor Wasp@pos2, Armor Wasp[hidden]@pos2, Armor Wasp[hidden]@pos1, Armor Wasp[hidden]@pos1, Armor Wasp[hidden]@pos1 |
| 3 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0, Armor Grub@pos0 |
| 4 | 1 | 0x40 | 9 | 0 | 0,0,0 | Hobgob@pos0, Lucre Bug[hidden]@pos0 |
| 5 | 23 | 0x40 | 9 | 0 | 0,0,0 | Rotten Sod@pos0 |
| 6 | 23 | 0x40 | 9 | 0 | 2,2,2 | Rotten Sod@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3, Sand Shark@pos3 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 9 | 49 | 0x40 | 9 | 0 | 1,1,1 | Rhino@pos1, Rhino@pos1, Rhino@pos1 |
| 10 | 12 | 0x40 | 9 | 0 | 2,2,2 | Armor Wasp@pos2, Armor Wasp@pos2 |
| 11 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 12 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 13 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 14 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 15 | 67 | 0xC0 | 9 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (14.3%) | 1 (14.3%) | 0 | 2 (28.6%) | 1 (14.3%) | 0 | 0 | 0 | 0 | 0 | 1 (14.3%) | 0 | 0 | 0 | 0 | 0 | 1 (14.3%) |

### Table 42 (used by 1 field(s))

Fields: 240 (Kislev sewers - outside locked door)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x40 | 9 | 0 | 1,1,1 | Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos2, Nolucre Bug@pos2 |
| 1 | 1 | 0x40 | 9 | 0 | 1,1,1 | Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos1 |
| 2 | 12 | 0x40 | 9 | 0 | 2,2,2 | Armor Wasp@pos2, Armor Wasp@pos2, Armor Wasp[hidden]@pos2, Armor Wasp[hidden]@pos1, Armor Wasp[hidden]@pos1, Armor Wasp[hidden]@pos1 |
| 3 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0, Armor Grub@pos0 |
| 4 | 1 | 0x40 | 9 | 0 | 0,0,0 | Hobgob@pos0, Lucre Bug[hidden]@pos0 |
| 5 | 23 | 0x40 | 9 | 0 | 0,0,0 | Rotten Sod@pos0 |
| 6 | 23 | 0x40 | 9 | 0 | 2,2,2 | Rotten Sod@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3, Sand Shark@pos3 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 9 | 49 | 0x40 | 9 | 0 | 1,1,1 | Rhino@pos1, Rhino@pos1, Rhino@pos1 |
| 10 | 12 | 0x40 | 9 | 0 | 2,2,2 | Armor Wasp@pos2, Armor Wasp@pos2 |
| 11 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 12 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 13 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 14 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 15 | 67 | 0xC0 | 9 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 4 (23.5%) | 4 (23.5%) | 2 (11.8%) | 2 (11.8%) | 4 (23.5%) | 0 | 0 | 0 | 0 | 0 | 0 | 1 (5.9%) |

### Table 43 (used by 1 field(s))

Fields: 241 (Kislev sewers - lower level entrance ladder)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 1 | 0x40 | 9 | 0 | 1,1,1 | Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos2, Nolucre Bug@pos2 |
| 1 | 1 | 0x40 | 9 | 0 | 1,1,1 | Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos1, Nolucre Bug@pos1 |
| 2 | 12 | 0x40 | 9 | 0 | 2,2,2 | Armor Wasp@pos2, Armor Wasp@pos2, Armor Wasp[hidden]@pos2, Armor Wasp[hidden]@pos1, Armor Wasp[hidden]@pos1, Armor Wasp[hidden]@pos1 |
| 3 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0, Armor Grub@pos0 |
| 4 | 1 | 0x40 | 9 | 0 | 0,0,0 | Hobgob@pos0, Lucre Bug[hidden]@pos0 |
| 5 | 23 | 0x40 | 9 | 0 | 0,0,0 | Rotten Sod@pos0 |
| 6 | 23 | 0x40 | 9 | 0 | 2,2,2 | Rotten Sod@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3, Sand Shark@pos3 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 9 | 49 | 0x40 | 9 | 0 | 1,1,1 | Rhino@pos1, Rhino@pos1, Rhino@pos1 |
| 10 | 12 | 0x40 | 9 | 0 | 2,2,2 | Armor Wasp@pos2, Armor Wasp@pos2 |
| 11 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 12 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 13 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 14 | 12 | 0x40 | 9 | 0 | 0,0,0 | Sand Shark@pos3/f, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0, Armor Wasp@pos0 |
| 15 | 67 | 0xC0 | 9 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 4 (36.4%) | 0 | 0 | 1 (9.1%) | 1 (9.1%) | 2 (18.2%) | 2 (18.2%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (9.1%) |

### Table 44 (used by 1 field(s))

Fields: 259 (Kislev Gear Dock - ducts)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 50 | 0x40 | 10 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0 |
| 1 | 50 | 0x40 | 10 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Gebler Guard@pos0 |
| 2 | 16 | 0x40 | 10 | 0 | 0,0,0 | Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 3 | 50 | 0xC0 | 10 | 0 | 0,0,0 | Gebler Guard@pos0, Gebler Guard@pos0, Swordsman@pos0, Gebler Guard@pos0 |
| 4 | 27 | 0x00 | 4 | 0 | 0,0,0 | Medusoid(Gear)@pos0 |
| 5 | 32 | 0x00 | 4 | 0 | 0,0,0 | Edelweiss(Gear)@pos0 |
| 6 | 15 | 0x80 | 9 | 0 | 2,2,2 | Alkanshel@pos1 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 9 | 49 | 0x40 | 19 | 0 | 0,0,0 | Rhino@pos0, Rhino@pos0 |
| 10 | 16 | 0x40 | 19 | 0 | 0,0,0 | Hatamoto Mk3@pos0, Hatamoto Mk3@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 13 | 0xA0 | 25 | 7 | 0,0,0 | True Weltall(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 45 (used by 1 field(s))

Fields: 278 (Assault on Aveh - same as 186)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 54 | 0x80 | 7 | 0 | 0,0,0 | Skyghene(Gear)@pos0, Marinebasher(Gear)@pos1 |
| 1 | 53 | 0x80 | 55 | 0 | 0,0,0 | Shakhan(Gear)@pos0 |
| 2 | 31 | 0x80 | 55 | 0 | 0,0,0 | Brigandier(Gear)@pos0 |
| 3 | 56 | 0xC0 | 38 | 0 | 0,0,0 | Grahf@pos0, Grahf@pos0 |
| 4 | 74 | 0xA0 | 53 | 15 | 0,0,0 | Sword Seraph(Gear)@pos0 |
| 5 | 53 | 0xA0 | 70 | 18 | 0,0,0 | Shakhan(Gear)@pos0 |
| 6 | 53 | 0xA0 | 6 | 19 | 2,2,2 | Shakhan(Gear)@pos0 |
| 7 | 57 | 0xC0 | 39 | 0 | 0,0,0 | Dominia@pos0, Dominia@pos0, Tolone@pos0, Seraphita@pos0 |
| 8 | 64 | 0xA0 | 44 | 20 | 0,0,0 | Deus(Gear)@pos0 |
| 9 | 44 | 0xC0 | 52 | 0 | 0,0,0 | Sufal(Gear)@pos0, Sufal Gear@pos0, Sufal Gear@pos0, Sufal Gear@pos0, Sufal Gear@pos0 |
| 10 | 55 | 0x80 | 62 | 0 | 0,0,0 | G Elements(Gear)@pos0 |
| 11 | 55 | 0x80 | 4 | 0 | 0,0,0 | G Elements(Gear)@pos0 |
| 12 | 64 | 0x80 | 51 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 13 | 13 | 0x80 | 49 | 32 | 0,0,0 | Vierge(Gear)@pos0 |
| 14 | 52 | 0xA0 | 61 | 39 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos1, def2(Gear)@pos2 |
| 15 | 52 | 0xA0 | 61 | 21 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos1, def2(Gear)@pos2 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 46 (used by 1 field(s))

Fields: 333 (Ethos Transport - outside, needs mappara or falls off - screen)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 17 | 0x40 | 71 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 1 | 23 | 0x40 | 71 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Slugger@pos0 |
| 2 | 23 | 0x40 | 22 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Abandon@pos0 |
| 3 | 6 | 0x40 | 71 | 0 | 0,0,0 | Shellbelle@pos0 |
| 4 | 17 | 0xC0 | 71 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 5 | 23 | 0xC0 | 21 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Slugger@pos0 |
| 6 | 15 | 0xC0 | 22 | 0 | 2,2,2 | Alkanshel@pos0, Alkanshel@pos0 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 9 | 12 | 0x40 | 19 | 0 | 0,0,0 | Armor Grub@pos0 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 67 | 0xC0 | 21 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (33.3%) | 0 | 0 | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (33.3%) |

### Table 47 (used by 1 field(s))

Fields: 337 (Ethos Transport - freezer, needs mappara or stuck in wall)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 17 | 0x40 | 22 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 1 | 23 | 0x40 | 22 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Slugger@pos0 |
| 2 | 23 | 0x40 | 22 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Rotten Sod@pos0 |
| 3 | 27 | 0x40 | 71 | 0 | 0,0,0 | Medusoid@pos0 |
| 4 | 17 | 0xC0 | 22 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 5 | 23 | 0xC0 | 21 | 0 | 0,0,0 | Slugger@pos0, Slugger@pos0, Slugger@pos0 |
| 6 | 15 | 0xC0 | 22 | 0 | 2,2,2 | Alkanshel@pos0, Alkanshel@pos0 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 9 | 12 | 0x40 | 19 | 0 | 0,0,0 | Armor Grub@pos0 |
| 10 | 1 | 0x40 | 19 | 0 | 0,0,0 | Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0, Nolucre Bug@pos0 |
| 11 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 12 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0 |
| 13 | 13 | 0x80 | 25 | 0 | 0,0,0 | Vierge(Gear)@pos0, Vierge(Gear)@pos1, Vierge(Gear)@pos2 |
| 14 | 14 | 0x80 | 25 | 0 | 0,0,0 | Clawknight(Gear)@pos0, Clawknight(Gear)@pos1, Swordknight(Gear)@pos2 |
| 15 | 67 | 0xC0 | 21 | 0 | 0,0,0 | Redrum@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 (33.3%) | 1 (33.3%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (33.3%) |

### Table 48 (used by 1 field(s))

Fields: 403 (Lighthouse - elevator bottom)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 7 | 0x00 | 30 | 0 | 0,0,0 | Suzarn@pos1, Suzarn@pos1, Suzarn@pos1, Suzarn@pos1 |
| 1 | 71 | 0x00 | 30 | 0 | 0,0,0 | Tusk-Tusk(Gear)@pos0 |
| 2 | 24 | 0x00 | 30 | 0 | 0,0,0 | Forbidden@pos2, Forbidden@pos2, Forbidden@pos0, Forbidden@pos0, Forbidden@pos1, Forbidden@pos1 |
| 3 | 66 | 0x00 | 30 | 0 | 0,0,0 | Griffon(Gear)@pos1, Griffon(Gear)@pos2 |
| 4 | 6 | 0x00 | 30 | 0 | 0,0,0 | Hammerhead@pos0, Hammerhead@pos1, Hammerhead@pos2 |
| 5 | 6 | 0x00 | 30 | 0 | 0,0,0 | Shellbelle F1@pos0, Shellbelle F1@pos0, Shellbelle F1@pos1, Shellbelle F1@pos1, Shellbelle F1@pos2, Shellbelle F1@pos2 |
| 6 | 37 | 0x00 | 56 | 0 | 0,0,0 | Conjurer(Gear)@pos1, Conjurer(Gear)@pos2 |
| 7 | 72 | 0x00 | 56 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 8 | 73 | 0x00 | 56 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 9 | 73 | 0x00 | 56 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 10 | 73 | 0x00 | 56 | 0 | 0,0,0 | Sword Seraph(Gear)@pos0 |
| 11 | 73 | 0x00 | 56 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 12 | 73 | 0x00 | 56 | 0 | 0,0,0 | Heal Seraph(Gear)@pos1, Heal Seraph(Gear)@pos2 |
| 13 | 72 | 0x00 | 56 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 72 | 0x00 | 56 | 0 | 0,0,0 | Wind Seraph(Gear)@pos1, Wind Seraph(Gear)@pos2 |
| 15 | 72 | 0x00 | 56 | 0 | 0,0,0 | Earth Seraph(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 (15.4%) | 2 (15.4%) | 2 (15.4%) | 2 (15.4%) | 2 (15.4%) | 2 (15.4%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (7.7%) |

### Table 49 (used by 1 field(s))

Fields: 489 (Desert FMV - beeps and doesn't play cutscene (disk2?))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 9 | 0xA3 | 18 | 1 | 0,0,0 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)@pos3, Alpha Weltall(Gear)@pos4 |
| 1 | 9 | 0xA3 | 18 | 1 | 0,0,0 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)@pos3, Alpha Weltall(Gear)@pos4 |
| 2 | 9 | 0xA3 | 18 | 2 | 0,0,0 | Alpha Weltall(Gear)[hidden]@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)[hidden]@pos3, Alpha Weltall(Gear)[hidden]@pos4 |
| 3 | 9 | 0xA3 | 18 | 3 | 0,0,0 | Alpha Weltall(Gear)@pos0, Alpha Weltall(Gear)@pos1, Alpha Weltall(Gear)@pos2, Alpha Weltall(Gear)@pos3, Alpha Weltall(Gear)@pos4 |
| 4 | 11 | 0xA3 | 6 | 4 | 0,1,1 | Rankar Dragon(Gear)@pos0, Rankar Dragon(Gear)[hidden]@pos1, def2(Gear)[hidden]@pos1, def3@pos1 |
| 5 | 31 | 0xA3 | 7 | 5 | 0,0,0 | Brigandier(Gear)@pos0 |
| 6 | 19 | 0xA3 | 24 | 6 | 0,0,0 | Calamity(Gear)@pos0 |
| 7 | 25 | 0xA3 | 25 | 7 | 0,0,0 | Margie(Gear)@pos0 |
| 8 | 25 | 0xA3 | 41 | 8 | 0,0,0 | Margie(Gear)@pos0 |
| 9 | 25 | 0xA3 | 40 | 9 | 0,0,0 | Margie(Gear)@pos0 |
| 10 | 3 | 0xA3 | 1 | 10 | 1,1,1 | Forest Elf(Gear)@pos0 |
| 11 | 29 | 0xA3 | 40 | 11 | 0,0,0 | Hecht(Gear)@pos0 |
| 12 | 8 | 0xA3 | 20 | 12 | 0,0,0 | Dan@pos0 |
| 13 | 2 | 0xA3 | 7 | 13 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 14 | 2 | 0xA3 | 7 | 14 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |
| 15 | 2 | 0xA3 | 7 | 15 | 0,0,0 | Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0, Gigafoot@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 1 (25.0%) | 1 (25.0%) | 1 (25.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (25.0%) |

### Table 50 (used by 1 field(s))

Fields: 510 (Etrenank - sewer chase)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0 |
| 1 | 17 | 0xC0 | 59 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 2 | 30 | 0xC0 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 3 | 23 | 0xC0 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 4 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 5 | 33 | 0x40 | 59 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 33 | 0x40 | 59 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 7 | 17 | 0xC0 | 59 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 8 | 30 | 0xC0 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 9 | 23 | 0xC0 | 59 | 0 | 0,0,0 | Abandon@pos0 |
| 10 | 67 | 0xC0 | 59 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 11 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 12 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 13 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 14 | 1 | 0x40 | 48 | 0 | 0,0,0 | Jackal@pos0, Jackal@pos0 |
| 15 | 33 | 0xC0 | 48 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 (38.1%) | 0 | 2 (9.5%) | 2 (9.5%) | 4 (19.0%) | 4 (19.0%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (4.8%) |

### Table 51 (used by 1 field(s))

Fields: 606 (Krelian's Lab - black room (hang))

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 33 | 0x40 | 37 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0, Solaris Guard@pos0 |
| 1 | 17 | 0x40 | 37 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 2 | 30 | 0x40 | 37 | 0 | 0,0,0 | Phobia@pos0 |
| 3 | 23 | 0x40 | 37 | 0 | 0,0,0 | Abandon@pos0 |
| 4 | 67 | 0x40 | 37 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 5 | 33 | 0x40 | 37 | 0 | 0,0,0 | Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0, Littlefoot@pos0 |
| 6 | 33 | 0x40 | 37 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 7 | 17 | 0xC0 | 37 | 0 | 0,0,0 | Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0, Neo Wels@pos0 |
| 8 | 30 | 0xC0 | 59 | 0 | 0,0,0 | Phobia@pos0 |
| 9 | 23 | 0xC0 | 37 | 0 | 0,0,0 | Abandon@pos0 |
| 10 | 67 | 0xC0 | 37 | 0 | 0,0,0 | Bloody@pos0, Bloody@pos0 |
| 11 | 33 | 0x40 | 37 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 12 | 33 | 0x40 | 37 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 13 | 33 | 0x40 | 37 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 14 | 33 | 0x40 | 37 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |
| 15 | 33 | 0xC0 | 48 | 0 | 0,0,0 | Solaris Guard@pos0, Solaris Guard@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 (30.8%) | 1 (7.7%) | 1 (7.7%) | 1 (7.7%) | 1 (7.7%) | 4 (30.8%) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (7.7%) |

### Table 52 (used by 1 field(s))

Fields: 660 (Point of Sephirot - birth of the pre - Xenogears)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 70 | 0xA0 | 63 | 24 | 0,0,0 | Id(Gear)@pos0, Id(Gear)[hidden]@pos3 |
| 1 | 70 | 0xA0 | 63 | 24 | 0,0,0 | Id(Gear)@pos0, Id(Gear)@pos3 |
| 2 | 5 | 0x00 | 63 | 0 | 0,0,0 | Sand Man(Gear)@pos0 |
| 3 | 13 | 0xA0 | 63 | 26 | 0,0,0 | Vierge(Gear)@pos0 |
| 4 | 20 | 0x80 | 56 | 0 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 5 | 20 | 0xA0 | 60 | 41 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 6 | 20 | 0xA0 | 60 | 42 | 0,0,0 | Amphysvena(Gear)@pos0 |
| 7 | 12 | 0x40 | 9 | 0 | 0,0,0 | Armor Grub@pos0 |
| 8 | 75 | 0x80 | 66 | 0 | 0,0,0 | Urobolus(Gear)@pos0 |
| 9 | 62 | 0x80 | 64 | 0 | 0,0,0 | Metatron(Gear)@pos0 |
| 10 | 62 | 0x80 | 64 | 0 | 0,0,0 | Metatron(Gear)@pos0 |
| 11 | 62 | 0x80 | 64 | 0 | 0,0,0 | Sundel(Gear)@pos0 |
| 12 | 62 | 0x80 | 64 | 0 | 0,0,0 | Harlute(Gear)@pos0, Marlute(Gear)[hidden]@pos0 |
| 13 | 63 | 0x80 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 14 | 63 | 0x80 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |
| 15 | 63 | 0x80 | 65 | 0 | 0,0,0 | Deus(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 (100.0%) |

### Table 53 (used by 1 field(s))

Fields: 703 (Merkava - inside entrance)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 34 | 0x00 | 56 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos1, Eagle Gunner(Gear)@pos2 |
| 1 | 34 | 0x00 | 56 | 0 | 0,0,0 | Eagle Armor(Gear)@pos1, Eagle Armor(Gear)@pos2 |
| 2 | 34 | 0x00 | 56 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos0, Eagle Gunner(Gear)[hidden]@pos0 |
| 3 | 34 | 0x00 | 56 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos0, Eagle Armor(Gear)[hidden]@pos0 |
| 4 | 37 | 0x00 | 56 | 0 | 0,0,0 | Conjurer(Gear)@pos0 |
| 5 | 34 | 0x00 | 56 | 0 | 0,0,0 | Eagle Armor(Gear)@pos0, Eagle Gunner(Gear)@pos1, Eagle Gunner(Gear)@pos2 |
| 6 | 37 | 0x00 | 56 | 0 | 0,0,0 | Conjurer(Gear)@pos1, Conjurer(Gear)@pos2 |
| 7 | 72 | 0x00 | 56 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 8 | 73 | 0x00 | 56 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 9 | 73 | 0x00 | 56 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 10 | 73 | 0x00 | 56 | 0 | 0,0,0 | Sword Seraph(Gear)@pos0 |
| 11 | 73 | 0x00 | 56 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 12 | 73 | 0x00 | 56 | 0 | 0,0,0 | Heal Seraph(Gear)@pos1, Heal Seraph(Gear)@pos2 |
| 13 | 72 | 0x00 | 56 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 72 | 0x00 | 56 | 0 | 0,0,0 | Wind Seraph(Gear)@pos1, Wind Seraph(Gear)@pos2 |
| 15 | 72 | 0x00 | 56 | 0 | 0,0,0 | Earth Seraph(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 (19.0%) | 4 (19.0%) | 2 (9.5%) | 2 (9.5%) | 2 (9.5%) | 0 | 0 | 2 (9.5%) | 2 (9.5%) | 2 (9.5%) | 0 | 0 | 0 | 0 | 0 | 0 | 1 (4.8%) |

### Table 54 (used by 1 field(s))

Fields: 706 (Merkava - platform puzzle start)

| # | Enemy set | Policy | Arena | Event | Party | Enemies |
|---:|---:|---:|---:|---:|---|---|
| 0 | 34 | 0x00 | 58 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos1, Eagle Gunner(Gear)@pos2 |
| 1 | 34 | 0x00 | 58 | 0 | 0,0,0 | Eagle Armor(Gear)@pos1, Eagle Armor(Gear)@pos2 |
| 2 | 34 | 0x00 | 58 | 0 | 0,0,0 | Eagle Gunner(Gear)@pos0, Eagle Gunner(Gear)[hidden]@pos0 |
| 3 | 34 | 0x00 | 58 | 0 | 0,0,0 | Eagle Armor(Gear)@pos0, Eagle Gunner(Gear)[hidden]@pos0 |
| 4 | 37 | 0x00 | 58 | 0 | 0,0,0 | Conjurer(Gear)@pos0 |
| 5 | 34 | 0x00 | 58 | 0 | 0,0,0 | Eagle Armor(Gear)@pos0, Eagle Gunner(Gear)@pos1, Eagle Gunner(Gear)@pos2 |
| 6 | 37 | 0x00 | 58 | 0 | 0,0,0 | Conjurer(Gear)@pos1, Conjurer(Gear)@pos2 |
| 7 | 72 | 0x00 | 58 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 8 | 73 | 0x00 | 58 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 9 | 73 | 0x00 | 58 | 0 | 0,0,0 | Fire Seraph(Gear)@pos1, Fire Seraph(Gear)@pos2 |
| 10 | 73 | 0x00 | 58 | 0 | 0,0,0 | Sword Seraph(Gear)@pos0 |
| 11 | 73 | 0x00 | 58 | 0 | 0,0,0 | Sword Seraph(Gear)@pos1, Sword Seraph(Gear)@pos2 |
| 12 | 73 | 0x00 | 58 | 0 | 0,0,0 | Heal Seraph(Gear)@pos1, Heal Seraph(Gear)@pos2 |
| 13 | 72 | 0x00 | 58 | 0 | 0,0,0 | Wind Seraph(Gear)@pos0 |
| 14 | 72 | 0x00 | 58 | 0 | 0,0,0 | Wind Seraph(Gear)@pos1, Wind Seraph(Gear)@pos2 |
| 15 | 72 | 0x00 | 58 | 0 | 0,0,0 | Earth Seraph(Gear)@pos0 |

Weights (see §2 for the selection formula). Each cell shows the raw byte alongside its share of `sum(weights)+1` (rounded to one decimal place); a zero-weight cell shows only `0`:

| Row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | (no encounter) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 | 4 (19.0%) | 4 (19.0%) | 2 (9.5%) | 0 | 0 | 0 | 2 (9.5%) | 0 | 4 (19.0%) | 2 (9.5%) | 0 | 2 (9.5%) | 1 (4.8%) |
