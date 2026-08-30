# Combatants, Turns, And Targeting

## 1. Overview

Battle's simulation-facing entity model covers readiness timers, turn dispatch,
the player action budget, enemy turns, and target selection.
Damage, status application, reactions, and battle outcomes are documented in
[`04-actions-damage-and-status.md`](04-actions-damage-and-status.md).
The outer phase lifetime is documented in
[`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md), and serialized
encounter inputs are documented in
[`02-loading-resources-and-formations.md`](02-loading-resources-and-formations.md).

## 2. Eleven Runtime Slots

Battle uses eleven stable entity indices:

| Slots | Role | Bitmask |
|---:|---|---:|
| `0..2` | Party combatants | `0x0007` |
| `3..10` | Encounter enemies | `0x07F8` |

The live entity array begins at `0x800CCCE8`. Each entry is `0x170` bytes, so
the record for slot `i` is:

```text
entity(i) = 0x800CCCE8 + i * 0x170
target(i) = 1 << i
```

The low eleven bits of a target mask identify runtime entities. Masks are stored
as `u16`; action and item protocols use the upper five bits.

`isBattleSlotFilled[11]` at `0x800D2DCC` is the primary occupancy array. An
empty slot remains part of every fixed-size loop and receives countdown values
of `0x00FF`; ordinary target lists require an occupied slot. Its ready-state
byte starts at zero. Defeated, removed, and disabled entities receive the
unavailable value `-1`. Party identity is stored separately from slot identity;
`0x7F` denotes an absent party member.

## 3. Entity Record

The first `0xA4` bytes of a Battle entity reproduce the persistent
character-scale combat record. A Gear subrecord begins at `+0xA4`, followed by
Battle-only mode flags and timers. Enemy records use the same `0x170` envelope,
although some party-oriented fields have encounter-specific meanings.

The character-scale prefix exposes these semantically active fields:

| Offset | Type | Meaning |
|---:|---|---|
| `0x00` | `u16` | Primary weapon effect mask |
| `0x02` | `u8` | Primary weapon parameter |
| `0x03` | `u8` | Primary weapon behavior selector |
| `0x04` | `u8` | Primary weapon flat attack component |
| `0x18` | `u16` | Alternate weapon effect mask |
| `0x1A` | `u8` | Alternate weapon parameter |
| `0x1B` | `u8` | Alternate weapon behavior selector |
| `0x1C` | `u8` | Alternate weapon flat attack component |
| `0x28..0x2C` | `u8[5]` | Equipment-derived Attack, Defence, Agility, Ether, and Ether Defence modifiers |
| `0x2D` | `u8` | Additional equipment Defence component |
| `0x2E..0x2F` | `u8[2]` | Equipment-derived Hit and Evade modifiers |
| `0x30..0x31` | `u8[2]` | Equipment HP and MP bonuses in twentieths of each maximum |
| `0x32` | `u16` | Equipment-effect flags |
| `0x34` | `u16` | Character action-state flags, including physical/ether attack immunity and target-state controls |
| `0x36` | `u16` | Secondary combat/control flags |
| `0x38` | `u16` | Elemental affinity and protection flags |
| `0x3A` | `u16` | Prevariance lethal-match counter |
| `0x3C` | `u32` | Cumulative Physical EXP |
| `0x40` | `u32` | Cumulative Ether EXP |
| `0x44` | `u32` | Physical EXP remaining to the next level |
| `0x48` | `u32` | Ether EXP remaining to the next level |
| `0x4C..0x53` | `u16[4]` | Current HP, maximum HP, current MP/EP, and maximum MP/EP |
| `0x54` | `u8` | Hyper Mode Points |
| `0x55` | `u8` | Base deathblow-proficiency gain per qualifying attack |
| `0x56` | `u8` | Character identity/command-loadout type |
| `0x58..0x5C` | `u8[5]` | Attack, Defence, Agility, Ether, and Ether Defence |
| `0x5E..0x5F` | `u8[2]` | Hit and Evade percentages |
| `0x62..0x63` | `u8[2]` | Physical Level and Ether Level |
| `0x6A..0x76` | `u8[13]` | Weapon, loadout, and equipped non-weapon ID banks |
| `0x7A` | `u16` | Enabled-command bitfield |
| `0x7C..0x8F` | `u16[10]` | Five active/protection status pairs |
| `0x90..0x9F` | `u16[8]` | Deathblow-proficiency counters |
| `0xA0` | `u8` | Associated Gear identifier; `0xFF` selects no Gear |
| `0xA1` | `u8` | Equipment-derived deathblow-proficiency gain |

The Gear subrecord begins at entity `+0xA4`. Its active fields are:

| Entity offset | Gear offset | Type | Meaning |
|---:|---:|---|---|
| `0xA6..0xA7` | `0x02..0x03` | `u8[2]` | Frame ID and Engine ID |
| `0xA8..0xB3` | `0x04..0x0F` | `u8[12]` | Special and primary Gear-weapon ID banks, Armor ID, and three Part IDs |
| `0xB4..0xBB` | `0x10..0x17` | 8-byte record | Ordinary Gear-weapon aggregate |
| `0xBC..0xC3` | `0x18..0x1F` | 8-byte record | Special weapon aggregate 1 |
| `0xC4..0xCB` | `0x20..0x27` | 8-byte record | Special weapon aggregate 2 |
| `0xDC..0xDF` | `0x38..0x3B` | `u16[2]` | Current and maximum fuel |
| `0xE0..0xE3` | `0x3C..0x3F` | `u8[4]` | Engine output, secondary engine channel, and two attack-multiplier channels |
| `0xE4..0xE7` | `0x40..0x43` | `u16[2]` | Part-derived physical and Ether Defence |
| `0xE8..0xEB` | `0x44..0x47` | `u16[2]` | Part weight and innate weight component |
| `0xEC` | `0x48` | `u16` | Part-effect flags |
| `0xEE` | `0x4A` | `u8` | Weight-induced Agility penalty |
| `0xF0..0xF3` | `0x4C..0x4F` | `u8[4]` | Part display modifier, Response modifier, Agility adjustment, and Gear-HP action scalar |
| `0xF4..0xF7` | `0x50..0x53` | `u8[4]` | Additive Part secondary counters |
| `0xF8` | `0x54` | `u8` | Equipment EtherAmp modifier |
| `0xFA` | `0x56` | `u8` | Equipment attack-multiplier bonus |
| `0xFB` | `0x57` | `u8` | Charge coefficient |
| `0x100..0x103` | `0x5C..0x5F` | `u8[4]` | Gear-weapon channel bytes |
| `0x104..0x10B` | `0x60..0x67` | `u32[2]` | Current and maximum Gear HP |
| `0x10C` | `0x68` | `u16` | Chassis/base weight |
| `0x112` | `0x6E` | `u16` | Attack-resistance mask |
| `0x114..0x117` | `0x70..0x73` | `u16[2]` | Derived physical and Ether Defence |
| `0x118..0x119` | `0x74..0x75` | `u8[2]` | Attack-multiplier seed and Part-weight capacity tier |
| `0x120..0x12B` | `0x7C..0x87` | `u16[6]` | Three Gear active/protection status pairs |
| `0x12C..0x13B` | `0x88..0x97` | `u8[16]` | Per-attack-mask resistance values |
| `0x13C` | `0x98` | `u8` | Derived Gear Agility |
| `0x13D` | `0x99` | `u8` | Gear Defence-reduction percentage |
| `0x140..0x143` | `0x9C..0x9F` | `u8[4]` | Intrinsic display statistic, Frame display value, EtherAmp, and Response |

Each eight-byte Gear-weapon aggregate begins with its 16-bit effect mask at
record `+0x00`. The flat attack byte is at `+0x02`, the weapon parameter is at
`+0x03`, and the behavior selector is at `+0x04`.

The Battle-only tail completes the shared envelope:

| Offset | Type | Meaning |
|---:|---|---|
| `0x148..0x149` | `u8[2]` | Current Gear Attack Level and maximum ordinary Attack Level |
| `0x14A..0x14B` | 2 bytes | Alignment before the reward fields |
| `0x14C` | `u32` | Enemy EXP reward |
| `0x150..0x151` | `u8[2]` | Primary and secondary drop chances |
| `0x152..0x153` | `u8[2]` | Primary and secondary item IDs |
| `0x154..0x155` | `u8[2]` | Primary and secondary inventory classes |
| `0x156` | `u16` | Enemy gold reward |
| `0x158..0x159` | `u8[2]` | Party Physical-EXP and Ether-EXP distribution weights |
| `0x15A` | `u8` | Battle mode flags; bit `0x80` selects Gear mode and bit `0x01` marks Defend/Charge stance |
| `0x15B` | 1 byte | Timer-bank alignment |
| `0x15C..0x169` | `u8[14]` | Character and Gear timed-status duration bank; `+0x162` also holds Gear Hyper duration |
| `0x16A..0x16F` | `u8[6]` | Serialized tail bytes outside the character and Gear status-duration maps |

Status masks, displayed cues, and duration slots are listed in the next chapter.

## 4. Initialization Boundary

The loader constructs simulation state before the ordinary Battle loop:

| Address | Function | Simulation responsibility |
|---:|---|---|
| `0x801E4048` | `BattleLoaderResetVisualEntityState` | Reset all eleven visual/entity slots |
| `0x801E4160` | `BattleLoaderConfigureEntityLayout` | Map party and enemy identities into formation positions |
| `0x801E4870` | `BattleLoaderLoadEnemyDefinitions` | Copy the selected definition independently into each occupied enemy slot and relocate its script pointers |
| `0x801E4AC0` | `BattleLoaderInitializePartyDisplayState` | Select character AP, Gear fuel, or hidden party state |
| `0x801E4CD0` | `BattleLoaderBuildBattleInventory` | Clamp, filter, and compact usable inventory entries |
| `0x801E4E7C` | `BattleLoaderInitializeTurnOrder` | Randomize tie order and initialize readiness |
| `0x801E5014` | `BattleLoaderInitializeCommandsAndTargets` | Install command layouts and initial facing targets |
| `0x801E5384` | `BattleLoaderLoadPartyResources` | Copy character and Gear statistics into the live records |

`BattleLoaderInitializeCommandsAndTargets` copies each character's persistent
energy-per-turn value into the Battle AP configuration. It also installs one of
sixteen command-ring layouts, obtains an initial target from formation facing,
and applies encounter restrictions that disable Gear entry or escape.

## 5. Readiness State

Readiness uses a descending countdown. The principal arrays are:

| Address | Type | Meaning |
|---:|---|---|
| `0x800D2DD7` | `u8` | Current cursor in the circular randomized tie order |
| `0x800D2DD8` | `u8[11]` | Randomized slot order |
| `0x800D2DE4` | `s8[11]` | Ready state: zero while counting, one when ready, `-1` when unavailable |
| `0x800D2DF0` | `u16[11]` | Saved/reload readiness value |
| `0x800D2E06` | `u16[11]` | Current readiness countdown |
| `0x800D2E1C` | `u16[11]` | Per-slot half-speed toggle |

`BattleInitializeEntityReadinessCounters` at `0x80078508` computes both the
current and saved countdown for every occupied slot. Empty slots receive
`0x00FF` in both arrays, while every ready-state byte is initialized to zero.
`BattleLoaderInitializeTurnOrder` additionally randomizes all eleven indices,
sets both countdowns to one for occupied enemy entities carrying startup flag
`+0x34:0x0200`, and normalizes the initial timers. These flagged entities retain
a zero ready-state byte until the next `BattleTimeProgress` update consumes the
one remaining tick.

The reload helper at `0x80098AF8` derives delay from a speed score and a small
random displacement. Its input arithmetic is:

```text
party character delta = Agility(+0x5A) - signed action modifier(+0x27)
party Gear delta      = Gear speed(+0x13C) - signed action modifier(+0x27)

party speed score = 9 * delta when delta > 0, otherwise 9
enemy speed score = 9 * Agility(+0x5A)
```

Scores above 165 are normalized to 160. The final reload is:

```text
reload = (165 - normalized_speed_score) - (random_mod_8 - 4)
```

## 6. Time Progression

`BattleTimeProgress` at `0x8007171C` visits all eleven slots when Battle time is
enabled. An occupied, non-ready slot normally loses one tick per update. When
the countdown reaches zero, the function clamps it to zero and sets the slot's
ready byte to one.

Several named states alter that operation:

| State | Field and mask | Timing effect |
|---|---:|---|
| Speed UP | `(+0x84 \| +0x86) & 0x8000` | Subtract two from readiness |
| Slow | `+0x7C & 0x1000` | Toggle the per-slot half-speed latch and subtract readiness on alternate updates |
| Stop | `+0x7C & 0x2000` | Subtract from the Stop duration at `+0x15C`; clear Stop when it reaches zero |
| Readiness hold | `+0x7C & 0x0080` | Hold the readiness countdown |
| Sleep | `+0x80 & 0x1000` | Hold the readiness countdown |

Slow halves update frequency. Speed UP doubles the amount subtracted during an
update. Their combination subtracts two on alternate updates.

`BattleSelectLowestReadinessEntity` at `0x80080AE4` scans
`randomTurnOrder[11]` starting at the circular cursor. It chooses the entity
with the smallest timer; strict less-than comparison means circular randomized
order breaks equal-delay ties.

## 7. One Complete Turn

`BattleTickMain` at `0x80071B94` owns the non-presentation turn transaction:

1. Stop readiness progression and convert the selected one-based turn value to
   a zero-based entity slot.
2. Clear per-turn damage, script, reaction, and status-display accumulators.
3. Decrement the acting entity's turn-based status durations through
   `BattleUpdateEntityStatusTimers` at `0x80099890`.
4. Slots `0..2` enter command selection or dispatch the automatic turn selected
   by a disabling status. Slots `3..10` run the enemy turn script.
5. After a party turn, run eligible enemy end-of-turn scripts and queued
   reactions. The enemy-actor branch settles its staged action sequence without
   invoking the enemy end-of-turn programs.
6. Apply deferred readiness effects and rebuild default target choices.
7. Check terminal conditions.
8. If Battle is still active, apply passive HP, MP, and fuel changes.
9. Check terminal conditions again, settle status/defeat animations, reload the
   acting entity's timer, and resume readiness progression.

The first `BattleCheckWinConditions` call gates passive end-of-turn effects. The
second call detects terminal states created by those passive changes.

`BattleUpdateTicksEndOfTurn` at `0x800718BC` clears the actor's ready byte,
computes a fresh reload value, and stores it in both readiness arrays. A slot
marked unavailable with `-1` remains unavailable.

## 8. Party Command Turns And AP

`BattleCommandSelectionLoop` at `0x80080160` begins a character turn by copying
the configured energy-per-turn value into both maximum AP and remaining AP. It
expands the entity's `+0x7A` bitfield into sixteen command-enabled entries and
then dispatches character or Gear command-ring handlers.

Character-scale command paths include Attack, Item, Defend, Gear, Chi, Combo,
alternate Item, and Escape. Gear paths replace the AP-centric attack flow with
Gear attacks, Gear items, Ether, and Gear specials whose costs are normally
paid in fuel. Character abilities check and deduct MP. Inventory actions
consume one compacted Battle inventory entry only after use is committed.

The three ordinary attack inputs cost one, two, or three AP. Each accepted
input transforms an eight-state attack-sequence node through this table:

| Current node | 1 AP | 2 AP | 3 AP |
|---:|---:|---:|---:|
| `0` | `1` | `5` | `7` |
| `1` | `2` | `6` | `7` |
| `2` | `3` | `5` | `7` |
| `3` | `4` | `6` | `7` |
| `4` | `1` | `5` | `7` |
| `5` | `2` | `6` | `7` |
| `6` | `3` | `5` | `7` |
| `7` | `1` | `5` | `7` |

The resulting node selects the attack descriptor used by
`BattlePerformBasicAttack` at `0x80087AF0`. Deathblow recognition is a second
layer: `BattleAppendCharacterAttackInputAndBuildDeathblowHints` at `0x800861D0`
matches the recorded input sequence against deathblow patterns, then tests the
corresponding unlock/proficiency state.

If an attack sequence was committed, remaining AP is added to a persistent Battle
accumulator capped at `0x1C` (28). Cancelling before spending AP returns to the
command ring; cancelling after spending AP ends the sequence.

## 9. Forced And Enemy Turns

A party member with Confusion at `+0x80:0x2000` bypasses the normal command
ring. `BattleExecuteConfusedPartyTurn` at `0x80080C94` builds a list with the
normal validity predicate, removes the acting slot, chooses a random target,
and attempts one of the available basic attack, ability, Gear transformation,
or defend paths.

The actual category and move choice is made by
`BattleSelectAutomatedCharacterAttack` at `0x8009BAC4`, a cascading
multi-roll selector:

- A per-character flag byte can force the whole roll straight to the basic
  attack sub-case below, skipping everything else (data path not fully
  identified — plausibly "no deathblow attempt available this turn").
- Otherwise, a `random_mod_100 < 10` roll first offers a chance at Defend
  (only if a defend option is actually available; if not, or on the 90% side,
  it falls through to a deathblow attempt).
- The deathblow attempt reads a per-character tier byte to select one of
  several learned-move bitmasks and slot counts, then draws a uniform index
  in that range (`rand() % count`) and checks whether the drawn slot is
  actually learned. An unlearned draw, or roughly a 25% roll failing
  separately, falls back to a plain basic attack instead.
- The basic attack case itself rolls two more `random_mod_100` checks (`< 80`,
  then `< 60`) to choose among three attack variants, landing at
  roughly 48% / 32% / 20% overall.

The exact meaning of the tier byte's jump table (which of several
learned-move bitmask/count pairs applies) was not recovered from the
recompiled code alone, since its contents live in a data table this analysis
didn't dump; treat the category split above as the verified shape of the
mechanism, not an exhaustive value table.

Enemy slots execute bytecode through `BattleExecuteMonsterTurnScript` at
`0x800799C8`. The script can select targets, query entity state, request an
attack, alter readiness, or queue a later action. The ordinary stream ends at
`0xFD` or `0xFF`. After party turns,
`BattleExecuteMonsterEndTurnScripts` at `0x80079C24` visits eligible enemy
slots. Enemy turns do not invoke those end-of-turn programs. Encounter
reactions execute inside the fixed eleven-slot scheduler.

Scripts can also call `BattleEnemyScriptForceSelectedEntityNextTurn` at
`0x8007E8E0` or rebuild all readiness values through
`BattleEnemyScriptResetReadinessState` at `0x8007E934`. Scripted scheduling is
therefore an explicit override layered on the ordinary countdown system.

## 10. Target Masks And Validity

`BattleIsTargetValid` at `0x80083FF4` is the ordinary basic-attack predicate. A
candidate qualifies through:

- Occupy a slot.
- Carry an active visual/lifecycle state.
- Have a clear `0xC001` defeated, removed, and physical-effect block mask in the
  character or Gear resource group.
- Satisfy the formation-row reachability table for character-scale attackers.

Gear candidates use occupancy, lifecycle eligibility, and clear blocking state.
Ability targeting uses `BattleIsAbilityTargetCandidate` at `0x80084108`; its
revival mode includes defeated candidates.

`BattleBuildBasicAttackTargetPriority` at `0x800841E0` builds the opposing-side
candidate list. It places entities in the same formation-position slot as the
attacker first, then other valid opponents. When that preferred group is
nonempty, it places the candidate with the lowest unsigned 16-bit character HP
at `+0x4C` first within the group; otherwise it applies the same comparison to
all candidates. This comparison still reads `+0x4C` for Gear-mode candidates,
not their 32-bit Gear HP at `+0x104`. The first entry becomes the default and
directional input can select another candidate.

Interactive directional movement uses
`BattleSelectNearestDirectionalTarget` at `0x80084854`. It filters candidates
to the requested angular sector and chooses the nearest one. Directional input
therefore operates on formation space within the same candidate set.

## 11. Ability Target Modes

`BattleAbilityTargetSelectionInit` at `0x80084DE4` interprets target descriptor
bits and constructs both an ordered list and a `u16` mask:

| Descriptor bits | Operational target mode |
|---:|---|
| `0x1000` clear | Party side, masked to `0x0007` |
| `0x1000` set | Enemy side, masked to `0xFFF8` and occupied low-eleven slots |
| `0x2000` | Combined party and enemy sides in one list |
| `0x4000` | Acting entity as the single target |
| `0x8000` | Defeated candidates |
| Low nibble `0` | One selected target |
| Low nibble `1` | Every candidate in the mask |
| Low nibble `2` | Selected formation row through `BattleGetAbilityTargetsInSelectedRow` |

Enemy scripts have parallel selectors for random party or enemy targets,
character-versus-Gear mode, same or different row, and lowest readiness. These
selectors store the same slot-bit masks used by player actions.
