# Characters

## 1. Scope

This chapter assembles what's already documented, elsewhere, about the eleven
playable characters into one place: who they are, how their stats are stored
and recalculated, how they gain experience and level up, how they unlock
deathblows and abilities, the three characters with genuinely different rules,
and how a character's state persists across Battle. Every formula here is
quoted from the chapter that actually derives it, not re-derived — this
document exists to connect facts that live in `battle/03`, `battle/05`,
`battle/08`, and `menu/06`/`menu/08`, which were written module-by-module and
never cross-indexed by character before now.

## 2. The Eleven-Character Roster

Character IDs, names, and internal aliases come from
`debug_overlay/data/characters.xml`. ID doubles as the persistent character
record index used throughout Battle and Menu.

| ID | Name | Internal alias | Notes |
|---:|---|---|---|
| 0 | Fei | Fei | |
| 1 | Elly | Elly | |
| 2 | Citan | Shitan | |
| 3 | Bart | Baltho | |
| 4 | Billy | Billy | Command-loadout type 4; see [§8](#8-character-specific-exceptions) |
| 5 | Rico | Lico | |
| 6 | Emeralda | Emerada | |
| 7 | Chu-Chu | Chuchu | Character ID 7; see [§8](#8-character-specific-exceptions) |
| 8 | Maria | Maria | Command-loadout type 8; see [§8](#8-character-specific-exceptions) |
| 9 | Citan (2) | Shitan2 | Distinct persistent record and command-loadout type from ID 2 |
| 10 | Emeralda (2) | Emerada2 | Distinct persistent record and command-loadout type from ID 10 |

The entity field at `+0x56` is documented in `battle/03`/`battle/08` as
"character identity/command-loadout type," and two characters' own sections
confirm the type number equals the character's own roster ID directly (Billy
is ID 4 and is called "command-loadout type 4"; Maria is ID 8 and is called
"command-loadout type 8"). No source chapter states this equivalence as a
general rule, but the two confirmed cases line up exactly with roster ID, and
nothing found contradicts it holding for the other nine — treat it as
strongly supported, not independently re-derived here. "Citan (2)" and
"Emeralda (2)" are separate roster slots (IDs 9 and 10) with their own
persistent records and growth tables — likely used where the story briefly
splits the party and needs an independent stat line for the same named
character, but which specific story moments use them was not investigated.

## 3. Stat Record Layout

Every character occupies the same `0x170`-byte live Battle entity envelope
documented in full in
[`battle/03` §3](../battle/03-combatants-turns-and-targeting.md#3-entity-record).
The character-relevant fields, gathered from that table:

| Offset | Type | Meaning |
|---:|---|---|
| `0x3C` | `u32` | Cumulative Physical EXP |
| `0x40` | `u32` | Cumulative Ether EXP |
| `0x44` | `u32` | Physical EXP remaining to next level |
| `0x48` | `u32` | Ether EXP remaining to next level |
| `0x4C..0x53` | `u16[4]` | Current HP, maximum HP, current MP/EP, maximum MP/EP |
| `0x54` | `u8` | Hyper Mode Points (Gear-side, see [§7](#7-attacks-deathblows-and-attack-level)) |
| `0x55` | `u8` | Base deathblow-proficiency gain per qualifying attack |
| `0x56` | `u8` | Character identity/command-loadout type |
| `0x58..0x5C` | `u8[5]` | Attack, Defence, Agility, Ether, Ether Defence |
| `0x5E..0x5F` | `u8[2]` | Hit and Evade percentages |
| `0x62..0x63` | `u8[2]` | Physical Level and Ether Level |
| `0x6A..0x76` | `u8[13]` | Weapon, loadout, and equipped non-weapon ID banks |
| `0x7A` | `u16` | Enabled-command bitfield |
| `0x90..0x9F` | `u16[8]` | Deathblow-proficiency counters (first seven used, see [§7](#7-attacks-deathblows-and-attack-level)) |
| `0xA0` | `u8` | Associated Gear identifier; `0xFF` selects no Gear |
| `0xA1` | `u8` | Equipment-derived deathblow-proficiency gain |

Equipment layers on top of these base values rather than replacing them —
the modifier fields (`0x28..0x32`, equipment-derived Attack/Defence/Agility/
Ether/Ether Defence/Hit/Evade/HP/MP bonuses and effect flags) are also part
of the same record; see `battle/03` §3 for their exact offsets, and §4 below
for how they combine into what's actually displayed.

## 4. Equipment And Displayed Stats

`menu/06` §14.2 documents the two functions that turn base stats plus
equipped items into what the player actually sees:

- `RecalculateCharacterEquipmentEffects` (General Menu `0x801E36D4`) clears
  accumulated equipment-derived state, then folds three equipped items into
  attribute modifiers, status/elemental masks, weapon power/effect/attribute
  fields, and accuracy/response-style modifiers.
- `CalculateCharacterDisplayedStats` (General Menu `0x801E3A80`) combines base
  character values, weapon values, and the accumulated equipment modifiers
  into seven displayed values, capped at 250 for attack/defence/ether-style
  values and 99 for hit/evade, with a special Agility correction above 20.

These are derived display values — changing equipment re-runs the fold rather
than patching a stored total. Full detail:
[`menu/06` §14.2](../menu/06-general-menu-pages-and-gameplay.md#142-character-recalculation).

## 5. Experience And Leveling

Full derivation: [`battle/08` §5-7](../battle/08-results-progression-and-persistence.md#5-reward-eligibility-and-totals).

Enemy EXP and gold accumulate from qualifying enemy slots after a win, then
`BattleResultDistributeExperience` (`0x801E2ACC`) applies a signed
quarter-step encounter modifier before splitting the total:

| Recipient | Base share |
|---|---:|
| Active survivor | `floor(T / (3 - N))`, `T` = adjusted encounter EXP, `N` = absent/terminal active slots |
| Benched roster member | `floor(3 * floor(T / 3) / 4)` |
| Active terminal/removed member | `0` |

Each recipient's share then splits into Physical and Ether EXP by that
character's own weight bytes (live `+0x158`/`+0x159`, each floored to a
minimum of 1):

```text
physical_gain = floor(share * physical_weight / (physical_weight + ether_weight))
ether_gain    = floor(share * ether_weight    / (physical_weight + ether_weight))
```

A "standard progression" flag gives the full share to both tracks instead
(coupled with a stored level cap of 99). Hercules Ring (+50% both tracks) and
the Ether-EXP booster (+50% Ether only, applied after Hercules Ring) are
equipment-effect flags on top of that. See `battle/08` §6 for the exact flag
bits and ordering.

`BattleResultApplyLevelUps` (`0x801E308C`) then processes each track
independently against a shared per-level threshold table (`u32` at Result
resource `+0xBAC + level * 4`), looping through every level crossed in one
result (including multiple level-ups from one large award) and rolling one
stat-growth pass per crossing. See `battle/08` §7 for the level-99/100
boundary rules the "standard progression" flag also affects.

## 6. Stat Growth

Full derivation: [`battle/08` §8](../battle/08-results-progression-and-persistence.md#8-stat-growth).

Physical Level growth (`0x801E335C`) updates maximum HP, Attack, Defence, Hit,
and Evade. Ether Level growth (`0x801E3500`) updates maximum MP, Ether, and
Ether Defence. Growth targets come from a per-command-loadout-type `0x110`-byte
growth-table record (one target pair, below/above level 99, per stat) — the
record's existence and offsets are documented, but the actual target values
per character have not been extracted into a table here (see
[§10](#10-not-yet-extracted-per-character-growth-targets-ability-costs-and-effects)).

The roll formulas themselves (quoted, not re-derived):

```text
# One-byte stats (Attack, Defence, Hit, Evade, Ether, Ether Defence), next_level < 100
score = trunc(((target - current) * 100) / (100 - next_level)) - 50 + random_mod_100
if score >= 50: current += 1
current = min(current, 200)

# Maximum MP, next_level < 100
score = trunc(((target_mp - max_mp) * 100) / (99 - next_level)) - 50 + random_mod_100
if score >= 50: max_mp += 1
max_mp = min(max_mp, 99)

# Maximum HP, next_level < 100
gain = trunc(random_mod_100 * (hp_target_1 + 99 - next_level - max_hp) / ((100 - next_level) * 100)) + 2
max_hp = min(999, max_hp + max(gain, 0))
```

Levels 100+ use a second target byte and a `200 - next_level`-based
denominator for all three formula families; see `battle/08` §8 for the exact
post-99 variants. Every growth roll draws its own independent `rand()` call
— indexed in [`rng/02-battle-rng.md` §7](../rng/02-battle-rng.md#7-results-progression-and-drops).

## 7. Attacks, Deathblows, And Attack Level

Gear-side Attack Level, the fifteen-slot attack index space, deathblow input
sequences, and Hyper Mode Points are documented in full in
[`battle/05` §5](../battle/05-gear-combat-and-special-commands.md#5-gear-attacks-levels-and-deathblows) —
not repeated here.

Deathblow proficiency and unlocks, from
[`battle/08` §9](../battle/08-results-progression-and-persistence.md#9-deathblows-abilities-and-ap-level):

- During a resolved character attack (party slot `0..2`, basic-attack index
  `0..6`), `BattleComputeAttackResults` (`0x800941A4`) adds
  `entity.byte_0x55 + entity.byte_0xA1` (base learning rate plus an active
  modifier) to one of the first seven `+0x90` proficiency counters, capped
  below `65000`.
- `BattleResultUnlockPrimaryAbility` (`0x801E3BE0`) checks one locked
  candidate at a time against an unlock bit, a Physical Level requirement,
  and seven proficiency-counter requirements — seven candidates ordinarily,
  thirteen when advanced deathblows are enabled globally
  (`0x8006F8EA:0x4000`).
- `BattleResultUnlockSecondaryAbility` (`0x801E3D54`) unlocks Ether abilities
  by Ether Level against up to twelve threshold bytes.
- `BattleResultUnlockHighLevelAbilities` (`0x801E41B4`) sets three additional
  ability bits at Physical Levels 50, 60, and 70 for every active party
  member.
- AP level (the per-turn AP budget tier) advances at most one step per
  Result pass, gated by character-specific Physical Level thresholds stored
  in each character's own `0x20` ability record, up to a shared level-50 +
  advanced-deathblow-flag gate for the final step (AP level 6 -> 7).

## 8. Character-Specific Exceptions

Three characters have genuinely different rules rather than just different
stat numbers or growth targets — fully documented in
[`battle/05` §11](../battle/05-gear-combat-and-special-commands.md#11-character-specific-exceptions):

- **Billy** (command-loadout type 4) can use equipped weapon/ammunition
  components instead of the ordinary Attack byte for both character and Gear
  attacks, with his own attack-power normalization formula.
- **Chu-Chu** (character ID 7) converts character stats directly into her
  grown-form Gear's HP, engine output, and Defence/Ether-Defence when
  transformed, rather than using an ordinary Gear record.
  `BattleResultUpdateType7Stats` (`0x801E3FB0`, see `battle/08` §9) refreshes
  those persistent seeds after every level-up.
- **Maria** (command-loadout type 8) has her own separate nine-entry
  "Controls" ability-threshold table (`BattleResultUpdateType8Unlocks`,
  `0x801E3EA4`), distinct from the ordinary primary/secondary ability unlock
  paths every other character uses.

## 9. Chi / Ether Abilities

Every character has access to elemental/Ether-based special techniques under
its own in-game name (Chi is the internal umbrella term for the command
category, not a Citan-exclusive move set). The mechanism is already
documented, just not gathered under that name:

- The resource is MP/EP, the same `+0x4C..0x53` field documented in §3.
- The damage/healing formula for an Ether-flagged action is the ordinary
  elemental-scaling and core-subtraction pipeline in
  [`battle/04` §4](../battle/04-actions-damage-and-status.md#4-damage-pipelines)
  (`ether = 5 * P - 4 * D`, with elemental weakness/resistance multipliers
  and elemental-absorption-to-healing).
- `battle/05` §9 (Ether Machine) confirms the Gear-side Ether command "enters
  the same ability menu and executor used by the pilot's character
  abilities," i.e. one shared mechanism for both.

The ability-selection menu itself (`BattleRunChiMenu` and its ~20 supporting
UI functions) was deliberately not documented — it's UI-drawing plumbing over
the mechanism above, not a distinct mechanic.

### Ability names

`BattleInitializeChiMenuContents` (`0x80091064`) resolves each ability slot's
name through `GetSystemTable20String` (`0x80033908`), the same resolved-
pointer/offset-array mechanism documented for item names in
[`menu/03` §12](../menu/03-resource-and-string-formats.md#12-item-weapon-and-accessory-name-database) —
member `20` (byte offset `0x50` in the offset array) of that same 53-member
item/name database, previously known to hold "character deathblow/Ether
names" but never cataloged. It decodes to 175 entries in eleven contiguous
16-slot blocks, one per character:

| Character | Ability names |
|---|---|
| Fei | Guided Shot, Inner Healing, Iron Valor, CounterForce, Yang Power, Yin Power, Radiance, Big Bang |
| Elly | Anemo Bolt, Terra Lance, Thermo Cube, Aqua Ice, Anemo Burn, Terra Storm, Thermo Dragon, Aqua Mist, Anemo Wave, Terra Ghost, Thermo Largo, Aqua Lord |
| Citan | Sazanami, Renki, Fuuseii, Chiseii, Kaseii, Suiseii, Ryokusho, Reisho, Koga, Yamiga, Senkei |
| Bart | Wild Smile, Heaven Cent, White Lure, Red Cologne, Blue Cologne, White Cologne, Wind Mode, Earth Mode, Fire Mode, Water Mode |
| Billy | Purity Light, Healing Light, Holy Light, Goddess Call, Goddess Eyes, Wind Shield, Earth Shield, Fire Shield, Water Shield, Goddess Wake |
| Rico | Steel Fist, Steel Body, Steel Spirit, Steel Mettle |
| Emeralda | Anemo Dharm, Terra Feist, Thermo Gord, Aqua Aroum, Anemo Omega, Terra Holz, Thermo Giest, Aqua Dhaum |
| Chu-Chu | Forest Dance, Culen Prayer, Myrm Prayer, Play Dead, Maiden Kiss, Forest Wind, Earth Gnome, Ancient Myth |
| Maria | Robo Beam, Robo Missile, Robo Punch, Robo Kick, Graviton Gun |
| Citan (2) | Identical block to Citan (byte-for-byte) |
| Emeralda (2) | Identical block to Emeralda (byte-for-byte) |

The block-to-character assignment above rests on two independent structural
proofs, not just thematic reading: the Citan/Citan (2) blocks and the
Emeralda/Emeralda (2) blocks are each byte-for-byte identical pairs, exactly
matching the roster's own two duplicate character slots (§2) — a coincidence
this exact would not happen by chance. The other nine blocks' assignment to
a specific character is a strong thematic match (elemental progressions for
Elly, holy/healing names for Billy, mascot-themed names for Chu-Chu, and so
on) rather than something read directly out of the selector code, since that
selector table lives in a runtime-allocated region (see below) that couldn't
be statically resolved. Treat the Fei, Bart, and Maria assignments in
particular as the least independently confirmed of the eleven.

EP costs and effects were not found. `BattleLoadChiMenuData`'s selector bytes
and per-slot cost/element data resolve to addresses roughly 37 KB past the
end of the statically-loaded `battle-overlay` image — a heap/BSS region
allocated at runtime whose base address isn't fixed at link time, so it
can't be read from the static disc/overlay data the way the name table
above could. Getting actual EP-cost numbers would need either a live trace
of that heap allocation or a lucky independent discovery of the same data
elsewhere.

## 10. Not Yet Extracted: Per-Character Growth Targets, Ability Costs And Effects

Two data tables implied by the mechanisms above remain incomplete:

1. **Per-character (per-command-loadout-type) growth targets** — the actual
   below/above-level-99 target values in each `0x110`-byte growth record
   (§6), which would let someone answer "what's Fei's HP at level 99" rather
   than only "here's the formula that computes it." The consumer-side
   arithmetic is fully confirmed at the instruction level: resident pointer
   `0x800D2C08` (itself resolved from a chain rooted at `0x801E44C8` =
   `0x800CCCE8`, the documented live-entity base) holds `table_base`, and
   `BattleResultApplyPhysicalLevelStatGrowth`/`...EtherLevelStatGrowth` read
   `table_base + type*0x110 + level_band + field_offset`. `0x800D2C08` loads
   from an LZSS-compressed member inside the archive selected by
   `ArchiveSetIndex(0x10, 2)`. Extraction stalls one level deeper: the size
   the loader allocates for that read comes from `ArchiveDecodeSize`
   (`0x80028738`), which — on the retail CD path (confirmed; the alternate
   branch only runs under a PsyQ host-PC dev-kit link via `PCopen`/`PCclose`/
   `PClseek`, never in the shipped game) — computes its result from two
   further resident lookup bases (`0x8005FE14`, and `0x8005FDF0` via the
   neighboring size-decoder) that are themselves populated from disc-loaded
   filesystem metadata nobody has reverse-engineered yet. Getting real values
   out would mean reversing that metadata format first — a standalone
   investigation on the scale of the field-ID or item-database addressing
   work already done elsewhere in this project, not a quick follow-up.
   Deliberately not pursued further; the formula above is the verified
   stopping point.
2. **Chi/Ether ability EP costs and effects** — names are done (above); costs
   and effects were traced to a runtime-allocated heap region whose base
   address isn't fixed at link time, so they can't be read from static disc
   data the way the name table could. Would need a live runtime trace, not
   more static analysis.

## 11. Party Formation And Availability

Full derivation: [`menu/08`](../menu/08-member-change-and-party-formation.md).

Three active party slots and eleven playable character IDs (`0..10`) are the
complete formation state. A character's availability comes from a bitmask
test (`menu/08` §5):

```text
available_mask = (game_state+0x1D30 & game_state+0x1D32) & 0x07FF
```

masked to eleven bits, one per character ID. Member Change filters the three
persistent active-party bytes against this mask on entry (an unavailable
character's slot becomes empty, `0xFF`) and rebuilds the bench list by
walking IDs `0..10` in order, admitting any available ID not already active.

## 12. Persistence

Full derivation: [`battle/08` §11](../battle/08-results-progression-and-persistence.md#11-character-defeat-and-gear-persistence).

After Result processing (or a successful escape), each represented
character's live Battle state writes back to its persistent record:

- Current HP and MP copy from live state, clamped to the (possibly just
  grown) persistent maxima.
- The seven deathblow-proficiency counters and the prevariance
  lethal-match counter copy from live state.
- A defeated or removed character persists at exactly one HP, not zero.
- Chu-Chu's grown-form Gear HP converts back to character HP as
  `max(1, floor((gear_hp + 1) / 50))` before the ordinary HP copy.

Associated Gear state persists independently in the same pass (current HP
and fuel, clamped to maxima, with a destroyed Gear returning at ten percent
of maximum HP) — see `battle/08` §11 for the Gear-ID range and exact rules.
