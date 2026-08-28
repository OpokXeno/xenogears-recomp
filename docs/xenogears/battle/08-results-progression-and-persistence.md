# Results, Progression, And Persistence

## 1. Result Responsibilities

This chapter specifies how turn-based Battle terminates, enters the Battle
Result phase, reconciles Battle-local state with the resident game-state block,
awards experience and currency, advances character progression, rolls drops,
and selects resident continuation, Field, World Map, or Movie mode.

The preceding simulation and terminal-state rules are documented in
[`Actions, Damage, And Status`](04-actions-damage-and-status.md). Battle Event
return control is documented in
[`Battle Event VM`](07-battle-event-vm.md#17-battle-and-module-lifecycle).

## 2. Result State Boundaries

The Battle Result phase begins after the central Battle loop reaches an exit and
reuses the short-lived memory domain from the earlier Battle phases.

The important state domains are:

| Domain | Address or layout | Lifetime |
|---|---:|---|
| Live Battle entities | `0x800CCCE8 + slot * 0x170`, 11 slots | Battle entry through Result finalization |
| Active party identities | `0x800D2D24`, 3 bytes | Battle entry through Result finalization |
| Persistent game state | base `0x8006D634` | Resident module handoffs and memory-card snapshots |
| Persistent characters | game state `+0x26C`, 11 records of `0xA4` | Battle, Field, World Map, Movie, and menu modes |
| Persistent Gears | game state `+0x978`, 20 records of `0xA4` | Battle, Field, World Map, Movie, and menu modes |
| Ability/AP records | game state `+0x16C0`, 11 records of `0x20` | Battle, Field, World Map, Movie, and menu modes |
| Gold | game state `+0x1924`, absolute `0x8006EF58` | Resident module handoffs and memory-card snapshots |

Result updates the resident game-state block in memory. A later save-menu
operation serializes that updated block to a memory card.

The live entity's first `0xA4` bytes mirror the character record. Result uses
these persistent character fields:

| Offset | Meaning |
|---:|---|
| `+0x32` | Equipment-effect flags, including Hercules Ring and Trader Card |
| `+0x3C`, `+0x40` | Cumulative Physical EXP and Ether EXP |
| `+0x44`, `+0x48` | Physical EXP and Ether EXP remaining to the next level |
| `+0x4C/+0x4E` | Current/maximum HP |
| `+0x50/+0x52` | Current/maximum MP |
| `+0x56` | Character command-loadout type |
| `+0x58/+0x59` | Attack/Defence |
| `+0x5B/+0x5C` | Ether/Ether Defence |
| `+0x5E/+0x5F` | Hit/Evade percentages |
| `+0x62/+0x63` | Physical Level/Ether Level |
| `+0x90` | Eight `u16` proficiency counters; the first seven feed deathblow requirements |
| `+0xA0` | Associated Gear ID |

Physical Level is the level displayed by the ordinary character UI and grows
maximum HP, Attack, Defence, Hit, and Evade. Ether Level is the secondary level
stored in menu cards and save summaries and grows maximum MP, Ether, and Ether
Defence.

## 3. Terminal Outcomes

`BattleCheckWinConditions` at `0x8007252C` rebuilds the living mask over all
eleven slots and writes:

```text
if (living_mask & 0x07F8) == 0: result = 0x01  # victory
if (living_mask & 0x0007) == 0: result = 0x81  # defeat
```

The party test runs second, so simultaneous exhaustion resolves as `0x81`.
Successful character and Gear escape handlers at `0x80082504` and `0x80083580`
write `0x40`. The normal scripted Battle Event success path writes result
`0x01` together with cancellation byte `0x800D2FC4 = 1`. Event-controlled exit
mode `3` also normalizes the result to `0x01`. These paths suppress rewards
through the separate cancellation and exit-mode gates. Result `0x21` remains a
recognized reward-exempt success code.

| Result | Handling |
|---:|---|
| `0x01` | Victory or normal scripted Event success; ordinary rewards run only when the separate eligibility gates pass. |
| `0x40` | Successful escape; immediate combat-resource synchronization and a reward-exempt Result path. |
| `0x81` | Defeat; title Field `0x1EA` becomes the next route. |
| `0x21` | Recognized special success code; scripted success return through a reward-exempt Result path. |

`BattleMain` at `0x80070F40` also normalizes Battle Event termination and
transition state into an exit mode. Exit mode `3` is an Event-controlled
reward-exempt victory route.

## 4. Result Lifecycle

After gameplay terminates, `BattleMain` performs this phase change:

1. Normalize the exit mode and settle action/animation resources.
2. Select logical directory `0x10`, file `0x04`.
3. Start the Result phase.
4. Run transition cleanup at `0x800B853C` while central state remains live.
5. Wait for the remaining asynchronous Battle gate.
6. Shut down the optional Battle Event phase at `0x80070EDC`.
7. Enter `BattleResultFinalize` at `0x801E252C`.

`BattleResultFinalize` then:

1. Calls `BattleResultLoadResources` at `0x801E211C`.
2. Performs mode-dependent audio and resident-state changes.
3. Calls `BattleResultReconcileBattleItemQuantities` at `0x801E24B0` to reconcile
   consumed ordinary Battle items.
4. Runs `BattleResultProcessRewards` at `0x801E2280` for an eligible ordinary
   victory.
5. Releases enemy records, actor records, Battle UI state, Result support,
   `BattleMainGraphicsState`, `BattleGraphicsControlState`, and
   `BattleRuntimeControlState`.
6. Releases allocator domain 2 and calls final Battle runtime cleanup at
   `0x800B8774`.

Item reconciliation and eligible party synchronization read live Battle
records before those records are released.

### Result presentation sequence

When the encounter configuration enables presentation,
`BattleResultRunSequence` at `0x801E1FB8` allocates:

| Allocation | Count | Semantic owner |
|---:|---:|---|
| `0x15FC` | 3 | Per-party-member summary, EXP animation, and progression-panel state |
| `0x3A8C` | 1 | Shared reward panel, unlock notice, drop list, and presentation state |

It then runs:

1. `BattleResultBuildPartyScreen` at `0x801E196C`.
2. `BattleResultRefreshExperienceDisplay` at `0x801E1AA4`, which animates the
   displayed experience values and permits confirmation to finish early.
3. `BattleResultShowPartyProgression` at `0x801E1C10`.
4. `BattleResultShowRewards` at `0x801E1E10`.
5. Frame synchronization, presentation-state release, and restoration of the
   prior central render mode.

The accepted-input state is `0x04` in `0x800D3014`. Detailed comparison panels
appear for active members whose Physical Level delta is nonzero. Ability notices
compare current unlock words with prebattle snapshots and enumerate newly set
bits. The reward panel aggregates and displays drops, then commits them to the
persistent inventory banks.

Two controls select a presentation-free victory:

| Control | Meaning |
|---|---|
| Runtime byte `0x800D2D50` | Silent-Result control for the current Battle. |
| Battle-policy bit `game-state +0x23A9:0x08`, absolute `0x8006F9DD:0x08` | Encounter-configured silent Result. |

EXP, levels, abilities, AP levels, gold, party state, and Gear state are
committed before this gate. Raw drop rolls occur in the progression transaction;
drop delivery occurs in the reward-panel path. The presentation-free route keeps
the reconciled quantities after Battle item use and skips delivery of rolled
drops.

## 5. Reward Eligibility And Totals

Ordinary reward processing runs when all four conditions hold:

- Result bits `0xC0` are clear.
- Result differs from recognized special success code `0x21`.
- The Battle cancellation byte at `0x800D2FC4` is zero.
- The normalized Battle exit mode at `0x800594D0` differs from `3`.

For an eligible result, `BattleResultProcessRewards` scans enemy slots `3..10`.
An enemy contributes when its lane is occupied, its reward-exclusion bytes are
clear, and live status `+0x7C` contains `0x8000`. Each qualifying enemy
contributes:

```text
total_exp  += enemy.exp_reward
total_gold += enemy.gold_reward
defeated_reward_mask |= 1 << enemy_lane
```

The serialized and live reward fields are the 32-bit EXP reward at enemy
`+0x14C` and the 16-bit gold reward at enemy `+0x156`. Enemy script opcodes
`0x39` and `0x3A` can replace them before Result totals the encounter.

Gold is applied immediately:

```text
persistent_gold = min(9_999_999, persistent_gold + total_gold)
```

The integer accumulator visits all eight enemy lanes before applying the cap.
Result stores total EXP at `0x800D2C84` and the qualifying enemy mask at
`0x800D2C9C`.

`BattleResultApplyPostBattleProgression` at `0x801E2794` performs the
progression transaction when at least one active party record has live
`+0x7C:0x8000` clear. With all three records marked, persistent progression,
party/Gear synchronization, and inventory remain at their pre-transaction
values. This transaction gate tests `0x8000`; EXP recipient classification uses
`0xC000`.

## 6. EXP Distribution

`BattleResultDistributeExperience` at `0x801E2ACC` first applies the signed
encounter EXP quarter-step modifier at `0x800D2CAC`:

```text
adjusted_exp = raw_exp - floor(raw_exp / 4) * exp_quarter_step
```

For nonnegative raw EXP, positive steps produce these exact integer results:

```text
step 1: raw_exp - floor(raw_exp / 4) = ceil(3 * raw_exp / 4)
step 2: raw_exp - 2 * floor(raw_exp / 4)
step 3: raw_exp - 3 * floor(raw_exp / 4)
step 4: raw_exp - 4 * floor(raw_exp / 4) = raw_exp mod 4
```

Step `4` therefore leaves the division remainder rather than always producing
zero. The resulting value becomes the encounter total distributed below.

The function then builds a recipient map for all eleven persistent character
records. The three active slots are classified as present survivors, absent
slots, or terminal/removed members using `entity+0x7C & 0xC000`. Immediately
before this pass, Result converts Battle's empty-party sentinel from `0x7F` to
persistent sentinel `0xFF`.

Let:

```text
T = adjusted encounter EXP
N = number of active party slots that are absent or terminal
```

The shares are:

| Recipient | Base share passed to the Physical/Ether splitter |
|---|---:|
| Active survivor | `floor(T / (3 - N))` |
| Benched roster member | `floor(3 * floor(T / 3) / 4)` |
| Active terminal/removed member | `0` |

The division by three precedes the benched multiplication and division. Benched
processing enumerates character IDs `0..10` and excludes IDs present in the
three active-party slots. Active shares divide the complete encounter total
among active survivors; every roster ID not currently active receives the
additional benched share.

### Physical EXP and Ether EXP

For an active survivor, live entity bytes `+0x158/+0x159` provide Physical and
Ether weights. Each raw weight below two becomes one:

```text
physical_weight = max(raw_physical_weight, 1)
ether_weight    = max(raw_ether_weight, 1)

physical_gain = floor(share * physical_weight
                      / (physical_weight + ether_weight))
ether_gain    = floor(share * ether_weight
                      / (physical_weight + ether_weight))
```

Each zero gain becomes one. The standard-progression flag at
`game-state +0x22B6:0x8000` assigns the complete share to both gains before the
one-point floor:

```text
physical_gain = share
ether_gain    = share
```

The halfword test reads byte `game-state +0x22B7:0x80` in the resident
Battle-formation mode area. Its progression consumers give it two coupled
roles: full EXP on both tracks and a stored level cap of 99. Clearing it selects
split-track EXP and extended progression above level 99.

Hercules Ring sets character equipment-effect flag `+0x32:0x2000` and applies:

```text
physical_gain += floor(physical_gain / 2)
ether_gain    += floor(ether_gain / 2)
```

Character equipment-effect flag `+0x32:0x1000` is the Ether-EXP booster and
then applies a second Ether-only increase:

```text
ether_gain += floor(ether_gain / 2)
```

The operations run in that order. A record carrying both flags first receives
Hercules Ring's 50% increase on both tracks and then receives the Ether-EXP
booster's 50% increase on the already modified Ether gain.

Active gains are stored in the per-party-member Result presentation state.
Benched members receive `floor(3 * floor(T / 3) / 4)` on both tracks and use the
benched branch directly.

## 7. Levels And Thresholds

`BattleResultApplyLevelUps` at `0x801E308C` first adds both gains to cumulative
Physical EXP and Ether EXP at `+0x3C/+0x40`. It then subtracts each gain from
the corresponding remaining value at `+0x44/+0x48`.

For each track independently:

```text
remaining -= gain
while remaining < 1:
    level += 1
    threshold = shared_threshold_table[level]
    apply that track's stat growth
    remaining += threshold
```

The loop processes every crossed threshold, including multiple levels and one
randomized growth pass per crossing. The next threshold is a `u32` entry at
Result resource offset `+0xBAC + level * 4`.

Both cumulative fields receive their gains first. A track entering the function
at level 99 adds its gain to the cumulative total and applies zero toward the
remaining value.

When a track starts below 99 and one award crosses the level-99 boundary:

- With `game-state +0x22B6:0x8000` set, each increment from 99 to 100 is rolled
  back to 99 before the threshold lookup and growth call. Threshold entry 99 and
  the below-100 growth targets are used for each remaining crossing.
- With that flag clear, the stored level advances through 100 and higher.
  Threshold lookup uses the resulting level, and growth selects the post-99
  targets and denominators described below.

Result records both level deltas for the three active slots. The detailed
progression panel is selected by the Physical Level delta.

## 8. Stat Growth

Physical Level growth at `0x801E335C` updates:

- maximum HP;
- Attack;
- Defence;
- Hit percentage;
- Evade percentage.

Ether Level growth at `0x801E3500` updates:

- maximum MP;
- Ether;
- Ether Defence.

The character-growth resource uses one `0x110`-byte record per command-loadout
type:

| Record offsets | Growth target |
|---:|---|
| `+0xB8/+0xBA` | Maximum HP below/above level 99, as `u16` |
| `+0xBC/+0xBD` | Attack below/above level 99 |
| `+0xBE/+0xBF` | Defence below/above level 99 |
| `+0xC0/+0xC1` | Hit below/above level 99 |
| `+0xC2/+0xC3` | Evade below/above level 99 |
| `+0xC4/+0xC5` | Ether below/above level 99 |
| `+0xC6/+0xC7` | Ether Defence below/above level 99 |
| `+0xC8/+0xC9` | Maximum MP below/above level 99 |

### One-byte stats

`BattleResultRollStatGrowth` at `0x801E3610` uses a target from the character's
growth table. For `next_level < 100`:

```text
score = trunc(((target - current) * 100) / (100 - next_level))
      - 50
      + random_mod_100

if score >= 50:
    current += 1

current = min(current, 200)
```

For `next_level >= 100`, it selects the second target byte and denominator
`200 - next_level`. Every division is integer division, and each stat samples
the RNG separately.

### Maximum MP

`BattleResultRollMaxMpGrowth` at `0x801E38CC` uses, for
`next_level < 100`:

```text
score = trunc(((target_mp - max_mp) * 100) / (99 - next_level))
      - 50
      + random_mod_100

if score >= 50:
    max_mp += 1

max_mp = min(max_mp, 99)
```

For `next_level >= 100`, it selects the second MP target and denominator
`200 - next_level`.

### Maximum HP

For `next_level < 100`, `BattleResultRollMaxHpGrowth` at `0x801E3700`
computes:

```text
numerator = random_mod_100
          * (hp_target_1 + 99 - next_level - max_hp)
denominator = (100 - next_level) * 100
gain = trunc(numerator / denominator) + 2

if gain < 0:
    gain = 0

max_hp = min(999, max_hp + gain)
```

For `next_level >= 100`, the instruction order is:

```text
q = trunc((hp_target_2 - max_hp) / ((201 - next_level) * 100))
gain = random_mod_100 * q * 2 + 2
max_hp = min(999, max_hp + max(gain, 0))
```

## 9. Deathblows, Abilities, And AP Level

### Deathblow proficiency earned during Battle

During a resolved character attack, `BattleComputeAttackResults` at
`0x800941A4` updates one of the first seven counters at entity `+0x90` when:

- the attacker is party slot `0..2`;
- the selected basic-attack index is `0..6`;
- the selected counter is below `65000`.

The update is:

```text
counter[attack_index] += entity.byte_0x55 + entity.byte_0xA1
```

The first term is the character's learning rate and the second is an active
learning modifier. The pre-addition test is `< 65000`; the proficiency counter
stores the resulting wrapped value. `BattleResultSyncPartyState` copies the first
seven counters to the persistent character record. Successful escape uses the
same copy in `BattlePersistPartyCombatState` at `0x8009BE0C`.

### Physical/deathblow unlocks

`BattleResultUnlockPrimaryAbility` at `0x801E3BE0` evaluates one candidate at a
time against:

- its unlock bit in ability word 0 at game-state
  `+0x16C0 + character * 0x20`;
- a per-candidate Physical Level requirement;
- seven `u16` proficiency requirements compared with character `+0x90`.

It scans seven candidates in the base set and thirteen when advanced
deathblows are enabled by `0x8006F8EA:0x4000`. One call grants the first
eligible locked candidate. `BattleResultUpdateAbilityUnlocks` at `0x801E3A18`
calls it once per eligible active party member.

Within each `0x110`-byte character-growth record, the thirteen candidates use
seven `u16` requirements each beginning at `candidate * 0x0E`; Physical Level
requirements are bytes `+0x100..+0x10C`.

Primary scanning applies to command-loadout types `0..6` and `9..`.

### Ether abilities and derived sets

`BattleResultUnlockSecondaryAbility` at `0x801E3D54` scans up to twelve Ether
Level requirement bytes ending at `0xFF`, sets the first eligible bit in ability
word 1, and returns the one-based ability index for notification. It applies to
active command-loadout types `0..9`.

Two propagation routines derive dependent sets:

| Address | Source and destination |
|---:|---|
| `0x801E3E14` | Map selected physical/deathblow bits from ability word 0 into word 2, up to nine configured entries. |
| `0x801E3F28` | Map selected Ether-ability bits from ability word 1 into word 3, up to thirteen configured entries. |

`BattleResultUpdateType8Unlocks` at `0x801E3EA4` applies Maria's separate
nine-entry Controls threshold table.

`BattleResultUpdateType7Stats` at `0x801E3FB0` refreshes Chu-Chu's persistent
grown-form Gear record after character growth:

| Game-state field | Grown-form field | Assignment |
|---:|---|---:|
| `+0xE30` | Gear 7 engine-output seed | `floor(character Attack / 5) + 1` |
| `+0xE58` | Maximum-HP seed | `character maximum HP * 200` |
| `+0xE64` | Physical-defence seed | `character maximum HP * 10` |
| `+0xE66` | Ether-defence seed | `character maximum HP * 10` |

These fields are Gear record 7 offsets `+0x3C`, `+0x64`, `+0x70`, and `+0x72`.
Result updates those persistent seeds. On Battle entry, Chu-Chu's live
character-ID-7 branch replaces the effective HP, output, physical defence, and
Ether defence with direct character-stat conversions. Result does not update
the Gear attack-multiplier fields; Battle derives the enlarged-form multiplier
from the current Gear-record multiplier seed at relative `+0x74` plus the
equipment multiplier bonus at relative `+0x56`.

`BattleResultUnlockHighLevelAbilities` at `0x801E41B4` visits the three active
party slots and sets word-2 bits `0x0008`, `0x0004`, and `0x0002` at Physical
Levels 50, 60, and 70.

### AP level

The persistent AP-per-turn level is byte `+0x17` of each `0x20` ability record.
`BattleResultUpdateApLevels` at `0x801E403C` visits all eleven roster records,
including benched members, and advances at most one step per Result pass:

| Current AP level | Requirement | New AP level |
|---:|---|---:|
| 3 | Character-specific Physical Level threshold at config `+0xCC` | 4 |
| 4 | Character-specific Physical Level threshold at config `+0xCD` | 5 |
| 5 | Character-specific Physical Level threshold at config `+0xCE` | 6 |
| 6 | Physical Level at least 50 and advanced-deathblow flag `0x8006F8EA:0x4000` set | 7 |

## 10. Drops, Inventory, And Item Reconciliation

### Enemy drop rolls

`BattleResultRollEnemyDrops` at `0x801E42C4` visits enemy lanes `0..7` selected
by the defeated-reward mask. Each enemy provides two ordered entries:

| Live enemy offset | Meaning |
|---:|---|
| `+0x150` | Primary chance, compared with `random_mod_100` |
| `+0x151` | Secondary chance, compared with a fresh `random_mod_100` |
| `+0x152/+0x154` | Primary item ID/inventory class |
| `+0x153/+0x155` | Secondary item ID/inventory class |

The decision is:

```text
if random_mod_100 < primary_chance or trader_card_equipped:
    award primary
else if fresh_random_mod_100 < secondary_chance:
    award secondary
else:
    emit an empty drop entry
```

The secondary probability is conditional on primary failure. Each enemy yields
at most one raw drop. Trader Card sets active-party equipment-effect flag
`+0x32:0x0800`, selecting the primary entry for every defeated enemy.

Raw classes are stored at `0x800CDCF4[8]` and item IDs at `0x800CDCFC[8]`.
`BattleResultAggregateItemDrops` at `0x801E1590` merges equal `(class, item ID)`
pairs into at most eight unique rows. Item ID zero is the empty entry.

### Persistent inventory banks

`BattleResultApplyItemDrops` at `0x801E1444` maps drop classes only to persistent
inventory banks:

| Drop class | Inventory | Identifier base | Quantity base | Capacity |
|---:|---|---:|---:|---:|
| 0 | Character weapons | `0x8006F3D0` | `0x8006F36C` | 100 |
| 1 | Character accessories | `0x8006F4FC` | `0x8006F434` | 200 |
| 2 | Consumable/key items | `0x8006F65A` | `0x8006F5C4` | 150 |
| 3 | Gear parts | `0x8006F754` | `0x8006F6F0` | 100 |
| 4 | Gear weapons | `0x8006F84E` | `0x8006F7B8` | 150 |

The drop-display path at `0x801E1690` independently maps each class to its
resident name accessor before calling the inventory commit. Class `4` is the
Gear Weapon bank. Billy's ammunition path instead uses IDs `0x32..0x48` from
the class-`0` character Weapon bank.

`BattleResultAddInventoryItem` at `0x801E1370` follows these rules:

1. Scan the complete selected bank for a matching ID.
2. Add to a matching stack and clamp its quantity to 99.
3. Otherwise install the ID and quantity in the first empty identifier slot.
4. A full bank discards the award.

An ordinary encounter aggregates at most eight copies of one drop because
there are eight enemy lanes.

### Items consumed during Battle

`BattleLoaderBuildBattleInventory` at `0x801E4CD0` creates a compact usable-item
set. From the 150-entry item bank it retains at most 48 nonzero IDs in usable
range `1..48`, clamps persistent quantities to 99, and records each source ID
beside the compact quantity.

`BattleExecuteItemUse` at `0x8008B908` decrements the compact quantity after a
committed use. Before reward eligibility is evaluated,
`BattleResultReconcileBattleItemQuantities` at `0x801E24B0` matches the 48 source
IDs against all 150 persistent identifiers and writes the remaining quantities.
This reconciliation runs for victory, escape, defeat, and Event victory.

A second compact list accepts IDs `0x32..0x48` from the 100-entry character
weapon bank for Billy's ammunition path. `BattleRegisterBillyAmmoReward` at
`0x8009A854` updates the persistent and equipped ammunition state after use.

## 11. Character, Defeat, And Gear Persistence

`BattleResultSyncPartyState` at `0x801E2888` visits the three occupied party
slots after experience and unlock processing. `BattlePersistPartyCombatState`
at `0x8009BE0C` performs the same resource synchronization immediately after a
successful escape.

For each represented character, the path:

1. Resolves the persistent character by live `+0x56` and the persistent Gear by
   live `+0xA0`.
2. Converts Chu-Chu's grown-form Gear HP back to character HP as
   `max(1, floor((gear_hp + 1) / 50))`.
3. Copies current HP and MP.
4. Clamps HP and MP to their persistent maxima.
5. Copies seven `u16` deathblow proficiency counters from live `+0x90`.
6. Copies the prevariance lethal-match counter at `+0x3A`.
   `BattleApplyAttackOutcome` at `0x800946F4` attempts an increment only when the
   target outcome byte is zero, the initially calculated damage equals the
   target's current HP, and the attacker is party slot `0..2`. This comparison
   precedes target damage variance, reflection, and later postprocessing, so an
   increment does not require the final outcome to defeat the target. The
   routine stores the incremented `u16`, then rolls it back when the resulting
   value exceeds `65000`.
7. Writes persistent character HP as one when live state
   `+0x7C & 0xC000` is nonzero.

The persistent outcome for a defeated or removed character is one HP. Level
growth updates persistent maxima first; synchronization then copies the live
current HP and MP, preserving the current resource values beneath the new
maxima.

Gear synchronization applies to Gear IDs `0..6` and `8..16`. It:

- copies 32-bit current Gear HP;
- copies current fuel;
- clamps both to persistent maxima;
- writes `floor(max_gear_hp / 10)` when live Gear state
  `+0x120:0x8000` marks the Gear destroyed.

A destroyed Gear therefore returns at ten percent of maximum HP. Character and
Gear corrections are independent.

Ordinary victory performs this Result-time synchronization. Escape performs it
at the successful escape command. Event victory and defeat follow their event
or resident reset routes.

## 12. Escape, Defeat, And Event Outcomes

### Escape

`BattleAttemptEscape` at `0x8009A9D0` succeeds when:

```text
random_mod_100 < 50
```

Success synchronizes party and Gear combat state before the command handler
writes result `0x40`. Escape is reward-exempt. Failure consumes the acting turn
and Battle continues. Encounter setup controls character and Gear Escape command
availability.

### Defeat

Result `0x81` selects the resident defeat route. After Battle returns,
`RunBattleAndDispatchOutcome` at `0x8001B6C4`:

1. Calls the resident state-reset helper at `0x8001AC94`.
2. Sets persistent Field ID to `0x1EA`, the title screen.
3. Clears the associated Field-entry and position parameters.
4. Selects the Field module and re-enters the resident dispatcher.

Consumed ordinary-item quantities have already passed through Result
reconciliation. Loading a memory-card save later replaces resident state with
the selected snapshot.

### Battle Event exits

The Battle Event image can direct the return independently of enemy exhaustion:

| Address | Event operation |
|---:|---|
| `0x801E7424` | Set the special return mode. |
| `0x801E746C` | Stop the Event VM and permit the final Event handoff; Battle continues until result or cancellation state ends the central loop. |
| `0x801E74B8` | End or skip combat successfully by setting result `0x01`, cancellation byte `0x800D2FC4`, and the VM stop byte at Event runtime `+0x800`. |
| `0x801E7770` | Set destination Field ID, camera yaw, World Map position index, and World Map mode. |
| `0x801E77E4` | Set Movie type, Movie number, fade parameter, completion value, and Movie return mode. |

Normal scripted Event success carries result `0x01` into the resident success
branch and remains reward-exempt through cancellation byte `0x800D2FC4` or
normalized exit mode `3`. Recognized code `0x21` also selects the resident
success branch and the reward-exempt Result path.

## 13. Resident Return Handoff

When Result's finalizer dispatch is active, `BattleResultFinalize` selects the
next resident module before releasing its remaining allocations:

| Selector | Destination | Selection rule |
|---:|---|---|
| 6 | Movie | Battle Event's Movie-return byte at `0x800D3338` is set. |
| 2 | Resident Battle continuation | Movie-return is clear and continuation-request byte `0x8005947C` is nonzero. |
| 1 | Field | Movie-return and continuation request are clear, and persistent Field ID low eleven bits are below `0x400`. |
| 3 | World Map | Movie-return and continuation request are clear, and persistent Field ID low eleven bits are at least `0x400`. |

After `BattleMain` and Result teardown return,
`RunBattleAndDispatchOutcome` at `0x8001B6C4` reads the final result byte:

- `0x01`, `0x40`, and `0x21` select a successful return route.
- `0x81` selects title Field `0x1EA`.
- Other values preserve the resident route selected by Battle Event policy.

This later successful route maps resident module selectors semantically:

| Selector | Destination | Selection rule |
|---:|---|---|
| 6 | Movie | Battle Event's Movie-return byte at `0x800D3338` is set. |
| 2 | Resident Battle continuation | Movie-return is clear and continuation-request byte `0x8005947C` is nonzero. |
| 1 | Field | Movie-return and continuation request are clear, and persistent Field ID low eleven bits are below `0x400`. |
| 3 | World Map | Movie-return and continuation request are clear, and persistent Field ID low eleven bits are at least `0x400`. |

Field-originated battles return into the Field reentry state captured before the
handoff, allowing reconstruction of the transient actor and script snapshot.
Field opcode `FE 84` can install a post-battle Field destination and script
variable value. Battle Event opcode `0x26` can replace Field, camera, and World
Map return values. The resident dispatcher consumes those persistent values
after Battle releases its overlay allocations.

## 14. Function Index

| Address | Function |
|---:|---|
| `0x8001B6C4` | Run Battle and dispatch the resident outcome |
| `0x80070F40` | Central Battle coordinator |
| `0x8007252C` | Check victory/defeat conditions |
| `0x800941A4` | Resolve attack results and add deathblow proficiency |
| `0x800946F4` | Apply per-target attack outcome and update the prevariance lethal-match counter |
| `0x8009A854` | Update Billy ammunition/equipped state |
| `0x8009A9D0` | Attempt escape and synchronize on success |
| `0x8009BE0C` | Persist party combat resources and proficiency counters |
| `0x801E1370` | Add an item to one persistent inventory bank |
| `0x801E1444` | Apply aggregated drops |
| `0x801E1590` | Aggregate duplicate drops |
| `0x801E1FB8` | Allocate, run, and release Result presentation state |
| `0x801E211C` | Load Result support resources |
| `0x801E2280` | Total and process eligible rewards |
| `0x801E24B0` | Reconcile consumed ordinary Battle items |
| `0x801E252C` | Finalize Result and release Battle resources |
| `0x801E2794` | Coordinate progression, synchronization, and drops |
| `0x801E2888` | Synchronize party and Gear state after ordinary victory |
| `0x801E2ACC` | Distribute EXP across the roster and both tracks |
| `0x801E2EB0` | Compute Physical EXP and Ether EXP gains |
| `0x801E308C` | Apply all crossed level thresholds |
| `0x801E335C` | Apply Physical Level stat growth |
| `0x801E3500` | Apply Ether Level stat growth |
| `0x801E3610` | Roll one-byte stat growth |
| `0x801E3700` | Roll maximum-HP growth |
| `0x801E38CC` | Roll maximum-MP growth |
| `0x801E3A18` | Coordinate active-party ability unlocks |
| `0x801E3BE0` | Unlock one physical/deathblow ability |
| `0x801E3D54` | Unlock one Ether ability |
| `0x801E3E14` | Derive physical dependent unlocks |
| `0x801E3EA4` | Apply Maria's Controls threshold unlocks |
| `0x801E3F28` | Derive Ether dependent unlocks |
| `0x801E3FB0` | Refresh Chu-Chu's grown-form Gear seeds |
| `0x801E403C` | Update roster AP levels |
| `0x801E41B4` | Unlock active-party level-50/60/70 abilities |
| `0x801E42C4` | Roll enemy drop tables |
